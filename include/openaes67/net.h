// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace openaes67 {

// One-time Winsock initialisation. Construct one per process before sockets.
class Winsock {
public:
    Winsock();
    ~Winsock();
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

// Applies a receive timeout to a socket handle.
//
// Winsock treats SO_RCVTIMEO == 0 as "block forever", but every caller in this
// project passes 0 meaning "do not block". This helper floors the value at 1 ms
// so the trap is handled in exactly one place.
bool setRecvTimeoutMs(uintptr_t socketHandle, int timeoutMs);

// DSCP marking, in the units the standards use: 46 = EF (audio), 48 = CS6 (PTP).
// AES67 §12 and ST 2110-21 expect both, and without marking a congested link
// queues audio behind whatever else shares the port.
bool setSocketDscp(uintptr_t socketHandle, int dscp);
int  socketDscp(uintptr_t socketHandle);   // -1 when the OS will not report it

// Opens a throwaway socket, marks it, reads the value back, and reports what the
// platform actually did. Used by the self-test: a setsockopt that silently fails
// is indistinguishable from "QoS is handled" until a congested switch says
// otherwise.
bool verifyDscpSupport(std::string *detail);

// MAC address of the adapter that owns `ipv4` (dotted quad). Used for the SDP
// a=ts-refclk:localmac line: a sender that is its own clock must still name a
// real interface, and the code used to emit 00-00-00-00-00-00.
// True for 224.0.0.0/4. AES67 transmits on multicast groups; a unicast address
// here silently sends the stream to one host and advertises an unusable c= line.
bool isMulticastIPv4(const std::string &ipv4);

bool localMacForIp(const std::string &ipv4, uint8_t mac[6]);

// UDP endpoint for multicast send / receive.
// The native SOCKET is kept opaque so this header stays platform-neutral.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    // Sender: bind to ifaceIp (or 0.0.0.0) and set multicast interface + TTL.
    bool openSender(const std::string &ifaceIp, int ttl, std::string *err);

    // Receiver: bind the port, then join group on ifaceIp.
    bool openReceiver(const std::string &group, uint16_t port,
                      const std::string &ifaceIp, std::string *err);

    bool sendTo(const void *data, size_t len,
                const std::string &group, uint16_t port);

    // Returns bytes read, 0 on timeout, -1 on error.
    int recvFrom(void *buf, size_t cap, int timeoutMs);

    void close();
    bool isOpen() const { return handle_ != kInvalid; }

private:
    static const uintptr_t kInvalid = ~static_cast<uintptr_t>(0);
    uintptr_t handle_ = kInvalid;
};

} // namespace openaes67
