// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openaes67 {

// SAP (Session Announcement Protocol, RFC 2974) announcer.
//
// Why this exists: AES67 defines how audio is carried (RTP + SDP) but not how a
// receiver finds a stream. The standard answer is SAP: the sender multicasts an
// SDP body to 239.255.255.255:9875 every few seconds, and AES67 receivers -- and
// the AES67 views in some controllers -- pick it up from there.
//
// This is deliberately NOT Dante discovery. Dante Controller finds Dante devices
// over mDNS (_netaudio-arc._udp and friends) and then speaks Audinate's private
// control protocol. Announcing over SAP does not, and cannot, make OpenAES67 look
// like a Dante device; it makes the stream discoverable to AES67 receivers.
// Builds one SAP datagram without touching a socket, so the wire format can be
// tested directly. `originLine` must be the SDP o= line and is only used for
// deletion packets (RFC 2974 section 6).
std::vector<uint8_t> buildSapDatagram(bool deletion, const std::string &sdp,
                                      const std::string &originIp,
                                      const std::string &originLine,
                                      uint16_t msgIdHash);

class SapAnnouncer {
public:
    SapAnnouncer() = default;
    ~SapAnnouncer();
    SapAnnouncer(const SapAnnouncer &) = delete;
    SapAnnouncer &operator=(const SapAnnouncer &) = delete;

    // `ifaceIp` selects the outgoing interface, `sdp` must be a complete SDP body.
    bool start(const std::string &ifaceIp, std::string sdp, std::string *err);

    // Sends one announcement. Call every 5-30 s; RFC 2974 suggests repeat until
    // the stream ends, then a deletion announcement.
    // RFC 2974 section 3.1: interval = max(300, ...) with a random offset of
    // +-1/3. 300 s is unusable interactively, so the default here is 30 s
    // (the value ST 2110 and NMOS deployments use) and callers can raise it.
    void setIntervalSeconds(int seconds) { intervalSec_ = seconds > 0 ? seconds : 30; }
    // Base interval plus the RFC offset term, so a network full of AES67
    // devices does not synchronise its announcements into bursts.
    double nextIntervalSeconds();
    bool announce(std::string *err);

    // Announces deletion (T=1) so receivers drop the entry promptly.
    void withdraw();
    // Replaces the announced SDP body. The caller must bump the o= session
    // version first: a receiver only notices a change when the version moves.
    void setSdp(const std::string &sdp);

    void stop();
    bool started() const { return socket_ != kInvalid; }

private:
    bool sendPacket(bool deletion, std::string *err);

    static constexpr uintptr_t kInvalid = ~static_cast<uintptr_t>(0);
    uintptr_t   socket_ = kInvalid;
    std::string sdp_;
    std::string originLine_;  // the SDP o= line, required in a deletion
    uint32_t    msgIdHash_ = 0;
    int         intervalSec_ = 30;   // base SAP announcement interval
    uint16_t    sequence_ = 0;
    std::string origin_;      // dotted IPv4 of the announcing interface
    std::string ifaceIp_;     // for the unicast compatibility copy
};

struct SapAnnouncement {
    // Address carried in the RFC 2974 originating-source field: who created the
    // announcement. Distinct from `from`, the UDP peer that delivered the datagram;
    // they differ for a multi-homed announcer or through a relay.
    std::string origin;
    std::string from;
    bool        deletion = false;
    std::string sdp;
};

// Parses one received SAP datagram into `out` (origin, deletion flag, SDP body).
// `from` is left untouched: the caller knows the UDP peer, this function does not.
// Returns false for anything that is not an application/sdp announcement.
bool parseSapAnnouncement(const uint8_t *data, size_t len, SapAnnouncement *out);

// Listens for SAP announcements for `seconds` and reports each one. Used by
// `sap-listen` to prove the announcer works without a hardware receiver.
// `ifaceIp` selects which interface joins the group. On a machine with several
// adapters (VPNs, virtual cables, Bluetooth) joining on INADDR_ANY can land on
// the wrong one and receive nothing, which is exactly what happened on the first
// attempt here.
int listenSap(int seconds, const std::string &ifaceIp,
              const std::function<void(const SapAnnouncement &)> &onAnnounce,
              std::string *err);

// Expiry rule for a discovered source, kept as a pure function so it can be
// tested without a GUI or a network. RFC 2974 relies on a deletion packet, but
// that packet can be lost, so a timeout is required: ten announce periods, with
// a 20 s floor so a source seen once is not dropped immediately.
struct SapSourceAge {
    int64_t lastSeenSeconds = 0;   // 0 = never seen
    int64_t periodSeconds = 0;     // 0 = unknown yet
};

inline int64_t sapExpiryLimitSeconds(const SapSourceAge &age) {
    const int64_t tenPeriods = age.periodSeconds > 0 ? age.periodSeconds * 10 : 0;
    return tenPeriods > 20 ? tenPeriods : 20;
}

inline bool sapSourceExpired(const SapSourceAge &age, int64_t nowSeconds) {
    if (age.lastSeenSeconds <= 0 || nowSeconds <= age.lastSeenSeconds) {
        return false;
    }
    return (nowSeconds - age.lastSeenSeconds) > sapExpiryLimitSeconds(age);
}

} // namespace openaes67
