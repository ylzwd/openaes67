#include "openaes67/net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#include <cstring>
#include <cstring>

namespace openaes67 {

Winsock::Winsock() {
    WSADATA d;
    ok_ = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
}

Winsock::~Winsock() {
    if (ok_) {
        WSACleanup();
    }
}

bool isMulticastIPv4(const std::string &ipv4) {
    IN_ADDR a{};
    if (InetPtonA(AF_INET, ipv4.c_str(), &a) != 1) {
        return false;
    }
    const uint32_t host = ntohl(a.s_addr);
    return host >= 0xE0000000u && host <= 0xEFFFFFFFu;   // 224.0.0.0 - 239.255.255.255
}
bool localMacForIp(const std::string &ipv4, uint8_t mac[6]) {
    if (mac == nullptr || ipv4.empty()) {
        return false;
    }

    in_addr wanted;
    if (InetPtonA(AF_INET, ipv4.c_str(), &wanted) != 1) {
        return false;
    }

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto *addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size) != NO_ERROR) {
        return false;
    }

    for (auto *adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->PhysicalAddressLength < 6) {
            continue;
        }
        for (auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto *sin =
                reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
            if (sin->sin_addr.s_addr == wanted.s_addr) {
                std::memcpy(mac, adapter->PhysicalAddress, 6);
                return true;
            }
        }
    }
    return false;
}
bool setRecvTimeoutMs(uintptr_t socketHandle, int timeoutMs) {
    if (socketHandle == ~static_cast<uintptr_t>(0)) {
        return false;
    }
    // 0 would mean "block forever" in Winsock; callers use it for "do not wait".
    const DWORD tv = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 1);
    return setsockopt(static_cast<SOCKET>(socketHandle), SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char *>(&tv), sizeof(tv)) == 0;
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (handle_ != kInvalid) {
        closesocket(static_cast<SOCKET>(handle_));
        handle_ = kInvalid;
    }
}

static bool setSockOptInt(SOCKET s, int level, int name, int value, std::string *err) {
    if (setsockopt(s, level, name, reinterpret_cast<const char *>(&value),
                   static_cast<int>(sizeof(value))) == SOCKET_ERROR) {
        if (err) {
            *err = "setsockopt failed, code=" + std::to_string(WSAGetLastError());
        }
        return false;
    }
    return true;
}

bool UdpSocket::openSender(const std::string &ifaceIp, int ttl, std::string *err) {
    close();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (err) { *err = "socket() failed, code=" + std::to_string(WSAGetLastError()); }
        return false;
    }

    if (ifaceIp != "0.0.0.0" && !ifaceIp.empty()) {
        IN_ADDR a{};
        if (InetPtonA(AF_INET, ifaceIp.c_str(), &a) != 1) {
            if (err) { *err = "invalid iface ip: " + ifaceIp; }
            closesocket(s);
            return false;
        }
        char buf[sizeof(IN_ADDR)];
        std::memcpy(buf, &a, sizeof(a));
        if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, buf, sizeof(a)) == SOCKET_ERROR) {
            if (err) {
                *err = "IP_MULTICAST_IF failed, code=" + std::to_string(WSAGetLastError());
            }
            closesocket(s);
            return false;
        }
    }

    // 1 ms ptime with many channels produces bursts; the default 8 KB send buffer
    // is easily overrun and the resulting WSAENOBUFS was reported as a fatal send
    // failure by the caller.
    setSockOptInt(s, SOL_SOCKET, SO_SNDBUF, 1024 * 1024, nullptr);

    if (!setSockOptInt(s, IPPROTO_IP, IP_MULTICAST_TTL, ttl, err)) {
        closesocket(s);
        return false;
    }
    // Let a receiver on the same host see the packets during testing.
    setSockOptInt(s, IPPROTO_IP, IP_MULTICAST_LOOP, 1, nullptr);

    handle_ = static_cast<uintptr_t>(s);
    return true;
}

bool UdpSocket::openReceiver(const std::string &group, uint16_t port,
                             const std::string &ifaceIp, std::string *err) {
    close();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (err) { *err = "socket() failed, code=" + std::to_string(WSAGetLastError()); }
        return false;
    }

    // Allow several processes / reruns to share the port on one host.
    setSockOptInt(s, SOL_SOCKET, SO_REUSEADDR, 1, nullptr);
    setSockOptInt(s, SOL_SOCKET, SO_RCVBUF, 4 * 1024 * 1024, nullptr);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
        if (err) { *err = "bind failed on port " + std::to_string(port) +
                           ", code=" + std::to_string(WSAGetLastError()); }
        closesocket(s);
        return false;
    }

    ip_mreq mreq{};
    if (InetPtonA(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
        if (err) { *err = "invalid group: " + group; }
        closesocket(s);
        return false;
    }
    if (!ifaceIp.empty() && ifaceIp != "0.0.0.0") {
        if (InetPtonA(AF_INET, ifaceIp.c_str(), &mreq.imr_interface) != 1) {
            if (err) { *err = "invalid iface ip: " + ifaceIp; }
            closesocket(s);
            return false;
        }
    }
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char *>(&mreq), sizeof(mreq)) == SOCKET_ERROR) {
        if (err) {
            *err = "IP_ADD_MEMBERSHIP failed, code=" + std::to_string(WSAGetLastError());
        }
        closesocket(s);
        return false;
    }

    handle_ = static_cast<uintptr_t>(s);
    return true;
}

bool UdpSocket::sendTo(const void *data, size_t len,
                       const std::string &group, uint16_t port) {
    if (handle_ == kInvalid) {
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (InetPtonA(AF_INET, group.c_str(), &to.sin_addr) != 1) {
        return false;
    }
    // A UDP send can fail transiently: the socket buffer is full because the
    // moment-scheduled burst outran the NIC, or a signal interrupted the call.
    // Treating that as fatal stopped the whole stream on a single hiccup, so
    // retry briefly and let the caller decide what a persistent failure means.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int n = sendto(static_cast<SOCKET>(handle_),
                             static_cast<const char *>(data), static_cast<int>(len), 0,
                             reinterpret_cast<sockaddr *>(&to), sizeof(to));
        if (n == static_cast<int>(len)) {
            return true;
        }
        const int e = WSAGetLastError();
        const bool transient = (e == WSAENOBUFS || e == WSAEWOULDBLOCK ||
                                e == WSAEINTR || e == WSAETIMEDOUT);
        if (!transient) {
            return false;
        }
        Sleep(1);
    }
    return false;
}

int UdpSocket::recvFrom(void *buf, size_t cap, int timeoutMs) {
    if (handle_ == kInvalid) {
        return -1;
    }
    setRecvTimeoutMs(handle_, timeoutMs);

    const int n = recvfrom(static_cast<SOCKET>(handle_),
                           static_cast<char *>(buf), static_cast<int>(cap), 0, nullptr, nullptr);
    if (n == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) {
            return 0;
        }
        return -1;
    }
    return n;
}

} // namespace openaes67