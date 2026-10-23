// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include <random>

#include "openaes67/sap.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "openaes67/net.h"

namespace openaes67 {
namespace {

// RFC 2974 defines 224.2.127.254 as the SAPv1 "all sessions" address; AES67 gear
// in the field most often listens on 239.255.255.255. Announce on both: they cost
// one extra packet and a receiver that only joined one of them still sees us.
constexpr const char *kSapGroups[] = {"239.255.255.255", "224.2.127.254"};
constexpr size_t kSapGroupCount = sizeof(kSapGroups) / sizeof(kSapGroups[0]);
constexpr uint16_t    kSapPort = 9875;
constexpr size_t      kSapHeaderSize = 8;

// RFC 2974: the message id hash identifies one announcement stream so receivers
// can treat repeats as updates rather than new sessions.
uint32_t fnv1a(const std::string &text) {
    uint32_t hash = 2166136261u;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash;
}

} // namespace

SapAnnouncer::~SapAnnouncer() { stop(); }

bool SapAnnouncer::start(const std::string &ifaceIp, std::string sdp, std::string *err) {
    if (sdp.empty()) {
        if (err) {
            *err = "SAP: empty SDP";
        }
        return false;
    }
    sdp_ = std::move(sdp);
    msgIdHash_ = fnv1a(sdp_);
    // RFC 2974 section 6: for application/sdp the deletion payload is a single
    // SDP line containing the origin field. Without it a listener cannot know
    // which session to remove, which is why Dante Controller kept showing the
    // stream after the sender was gone.
    const size_t oPos = sdp_.find("o=");
    if (oPos != std::string::npos) {
        size_t oEnd = sdp_.find_first_of("\r\n", oPos);
        originLine_ = sdp_.substr(oPos, oEnd == std::string::npos ? std::string::npos
                                                : oEnd - oPos);
    }
    origin_ = ifaceIp;
    ifaceIp_ = ifaceIp;

    socket_ = static_cast<uintptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket_ == kInvalid) {
        if (err) {
            *err = "SAP: socket() failed";
        }
        return false;
    }

    const bool debug = ::getenv("OPENAES67_SAP_DEBUG") != nullptr;
    int ttl = 255;   // SAP is normally sent with a large TTL
    {
        const int rc = ::setsockopt(static_cast<SOCKET>(socket_), IPPROTO_IP, IP_MULTICAST_TTL,
                                    reinterpret_cast<const char *>(&ttl), sizeof(ttl));
        if (debug) {
            (void)rc;   // best effort: a failed TTL set must not stop announcements
        }
    }

    // Loopback must be explicit: without it a listener on the same machine never
    // sees the announcement, which is exactly how the first version failed.
    int loop = 1;
    {
        const int rc = ::setsockopt(static_cast<SOCKET>(socket_), IPPROTO_IP, IP_MULTICAST_LOOP,
                                    reinterpret_cast<const char *>(&loop), sizeof(loop));
        if (debug) {
            std::fprintf(stderr, "[sap] setsockopt LOOP rc=%d wsa=%d\n", rc, ::WSAGetLastError());
        }
    }

    if (!ifaceIp.empty()) {
        in_addr iface;
        if (::InetPtonA(AF_INET, ifaceIp.c_str(), &iface) == 1) {
            const int rc = ::setsockopt(static_cast<SOCKET>(socket_), IPPROTO_IP, IP_MULTICAST_IF,
                                        reinterpret_cast<const char *>(&iface), sizeof(iface));
            if (debug) {
                std::fprintf(stderr, "[sap] setsockopt IF(%s) rc=%d wsa=%d\n",
                             ifaceIp.c_str(), rc, ::WSAGetLastError());
            }
        }
    }
    return true;
}

std::vector<uint8_t> buildSapDatagram(bool deletion, const std::string &sdp,
                                      const std::string &originIp,
                                      const std::string &originLine,
                                      uint16_t msgIdHash) {
    // RFC 2974 section 6: byte 0 carries the flags (V=1, A=0, T for deletion),
    // byte 1 is the authentication length in 32-bit words (always 0 here), then
    // the 16-bit message id hash and the 4-byte originating source.
    std::vector<uint8_t> packet(kSapHeaderSize, 0);
    packet[0] = static_cast<uint8_t>(deletion ? 0x24 : 0x20);
    packet[1] = 0x00;
    packet[2] = static_cast<uint8_t>((msgIdHash >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(msgIdHash & 0xFF);
    in_addr origin{};
    if (::InetPtonA(AF_INET, originIp.c_str(), &origin) == 1) {
        std::memcpy(packet.data() + 4, &origin.s_addr, 4);
    }
    static const char kMime[] = "application/sdp";
    packet.insert(packet.end(), kMime, kMime + sizeof(kMime));   // includes NUL
    if (deletion) {
        packet.insert(packet.end(), originLine.begin(), originLine.end());
    } else {
        packet.insert(packet.end(), sdp.begin(), sdp.end());
    }
    return packet;
}

bool SapAnnouncer::sendPacket(bool deletion, std::string *err) {

    if (socket_ == kInvalid) {
        if (err) { *err = "SAP: not started"; }
        return false;
    }
    if (deletion && originLine_.empty()) {
        return false;
    }
    std::vector<uint8_t> packet =
        buildSapDatagram(deletion, sdp_, origin_, originLine_,
                          static_cast<uint16_t>(msgIdHash_ & 0xFFFF));

    bool anySent = false;
    for (size_t gi = 0; gi < kSapGroupCount; ++gi) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kSapPort);
    ::InetPtonA(AF_INET, kSapGroups[gi], &destination.sin_addr);

    char dstText[32] = {0};
    ::InetNtopA(AF_INET, &destination.sin_addr, dstText, sizeof(dstText));
    if (::getenv("OPENAES67_SAP_DEBUG") != nullptr) {
        std::fprintf(stderr, "[sap] dst=%s:%u len=%zu del=%d\n", dstText,
                     static_cast<unsigned>(ntohs(destination.sin_port)),
                     packet.size(), deletion ? 1 : 0);
    }

    const int sent = ::sendto(static_cast<SOCKET>(socket_),
                              reinterpret_cast<const char *>(packet.data()),
                              static_cast<int>(packet.size()), 0,
                              reinterpret_cast<const sockaddr *>(&destination),
                              sizeof(destination));
    if (sent > 0) {
        anySent = true;
        if (::getenv("OPENAES67_SAP_DEBUG") != nullptr) {
            std::fprintf(stderr, "[sap] sent %d bytes to %s\n", sent, dstText);
        }
    } else if (::getenv("OPENAES67_SAP_DEBUG") != nullptr) {
        std::fprintf(stderr, "[sap] sendto %s failed, wsa=%d\n", dstText, ::WSAGetLastError());
    }
    }   // for each SAP group

    // Compatibility copy, sent as unicast to our own interface address.
    //
    // Rationale: a socket bound to 0.0.0.0:9875 receives unicast without joining
    // any multicast group, so this reaches a listener whose multicast membership
    // landed on a different adapter. This machine has six adapters and Dante
    // Controller's SAP listener did not react to the multicast announcements, so
    // the copy exists to tell the two failure modes apart.
    if (!ifaceIp_.empty()) {
        sockaddr_in direct{};
        direct.sin_family = AF_INET;
        direct.sin_port = htons(kSapPort);
        if (::InetPtonA(AF_INET, ifaceIp_.c_str(), &direct.sin_addr) == 1) {
            const int sentDirect = ::sendto(
                static_cast<SOCKET>(socket_),
                reinterpret_cast<const char *>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<const sockaddr *>(&direct), sizeof(direct));
            if (sentDirect > 0) {
                anySent = true;
            }
            if (::getenv("OPENAES67_SAP_DEBUG") != nullptr) {
                std::fprintf(stderr, "[sap] unicast copy to %s -> %d\n",
                             ifaceIp_.c_str(), sentDirect);
            }
        }
    }

    if (!anySent) {
        if (err) {
            *err = "SAP: every sendto failed";
        }
        return false;
    }
    return true;
}

bool SapAnnouncer::announce(std::string *err) { return sendPacket(false, err); }

double SapAnnouncer::nextIntervalSeconds() {
    // RFC 2974 section 3.1: offset = rand(interval * 2/3) - interval/3, i.e. a
    // uniform +-1/3 around the base. Doing this in whole seconds collapsed the
    // offset for small intervals (base 2 s produced 2 or 3 s, never 1.33..2.67),
    // so the value is computed in floating point and returned in seconds.
    static thread_local std::mt19937 rng{std::random_device{}()};
    const double base = intervalSec_ > 0 ? static_cast<double>(intervalSec_) : 30.0;
    std::uniform_real_distribution<double> dist(-base / 3.0, base / 3.0);
    const double next = base + dist(rng);
    return next < 0.5 ? 0.5 : next;
}
void SapAnnouncer::setSdp(const std::string &sdp) {
    sdp_ = sdp;
}

void SapAnnouncer::withdraw() {
    // RFC 2974 section 4: a deletion may be lost like any other datagram, and
    // a receiver that misses it keeps the stream listed forever. The reference
    // Linux daemon sends three; so do we.
    std::string ignored;
    for (int i = 0; i < 3; ++i) {
        sendPacket(true, &ignored);
    }
}

void SapAnnouncer::stop() {
    if (socket_ != kInvalid) {
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalid;
    }
}

bool parseSapAnnouncement(const uint8_t *data, size_t len, SapAnnouncement *out) {
    if (!data || !out || len <= kSapHeaderSize) {
        return false;
    }
    out->deletion = (data[0] & 0x04) != 0;   // T bit; byte 1 is the auth length
    const size_t authBytes = static_cast<size_t>(data[1]) * 4;
    const size_t headerBytes = kSapHeaderSize + authBytes;
    if (len <= headerBytes) {
        return false;
    }

    // RFC 2974 puts the creator's address in header bytes 4..7.
    in_addr announced{};
    std::memcpy(&announced, data + 4, 4);
    char announcedText[32] = {0};
    ::InetNtopA(AF_INET, &announced, announcedText, sizeof(announcedText));
    out->origin = announced.s_addr != 0 ? announcedText : std::string();

    const char *payload = reinterpret_cast<const char *>(data) + headerBytes;
    const size_t payloadLength = len - headerBytes;
    const size_t mimeLength = ::strnlen(payload, payloadLength);
    // Only application/sdp is an announcement; skipping the rest keeps a stray
    // packet from being reported as an empty SDP.
    if (mimeLength >= payloadLength ||
        ::strncmp(payload, "application/sdp", mimeLength) != 0) {
        return false;
    }
    out->sdp.assign(payload + mimeLength + 1, payloadLength - mimeLength - 1);
    return true;
}

int listenSap(int seconds, const std::string &ifaceIp,
              const std::function<void(const SapAnnouncement &)> &onAnnounce,
              std::string *err) {
    const uintptr_t handle = static_cast<uintptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (handle == ~static_cast<uintptr_t>(0)) {
        if (err) {
            *err = "SAP listen: socket() failed";
        }
        return 0;
    }

    BOOL reuse = TRUE;
    ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kSapPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(static_cast<SOCKET>(handle), reinterpret_cast<const sockaddr *>(&local),
               sizeof(local)) != 0) {
        if (err) {
            *err = "SAP listen: bind 9875 failed (another SAP listener running?)";
        }
        ::closesocket(static_cast<SOCKET>(handle));
        return 0;
    }

    for (size_t gi = 0; gi < kSapGroupCount; ++gi) {
    ip_mreq request{};
    ::InetPtonA(AF_INET, kSapGroups[gi], &request.imr_multiaddr);
    request.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!ifaceIp.empty()) {
        in_addr iface;
        if (::InetPtonA(AF_INET, ifaceIp.c_str(), &iface) == 1) {
            request.imr_interface = iface;
        }
    }
    ::setsockopt(static_cast<SOCKET>(handle), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 reinterpret_cast<const char *>(&request), sizeof(request));
    }

    const int timeoutMs = 500;
    ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

    int received = 0;
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ULL;
    std::vector<char> buffer(64 * 1024);
    while (::GetTickCount64() < deadline) {
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int bytes = ::recvfrom(static_cast<SOCKET>(handle), buffer.data(),
                                     static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (bytes <= static_cast<int>(kSapHeaderSize)) {
            continue;
        }

        SapAnnouncement announcement;
        if (!parseSapAnnouncement(reinterpret_cast<const uint8_t *>(buffer.data()),
                                  static_cast<size_t>(bytes), &announcement)) {
            continue;
        }
        // The header carries the creator's address; report the UDP peer separately
        // because they differ for a multi-homed announcer.
        char originText[32] = {0};
        ::InetNtopA(AF_INET, &from.sin_addr, originText, sizeof(originText));
        announcement.from = originText;
        if (announcement.origin.empty()) {
            announcement.origin = originText;
        }
        if (onAnnounce) {
            onAnnounce(announcement);
        }
        ++received;
    }

    ::closesocket(static_cast<SOCKET>(handle));
    return received;
}

} // namespace openaes67
