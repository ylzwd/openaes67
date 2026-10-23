// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstdint>
#include <string>

#include "openaes67/config.h"

namespace openaes67 {

// Clock reference advertised in a=ts-refclk.
//
// This line matters more than it looks. AES67 requires the sender to name the
// clock its RTP timestamps are derived from, and a Dante device in AES67 mode
// will not lock to a stream that claims an unsynchronised local clock. The
// original code emitted "localmac=00-00-00-00-00-00" unconditionally: a bogus
// MAC and no PTP reference at all.
struct SdpClockRef {
    enum class Kind {
        Ptp,        // a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>
        LocalMac    // a=ts-refclk:localmac=<mac>  (sender is its own clock)
    };

    Kind    kind = Kind::LocalMac;
    uint8_t gmid[8] = {0};      // grandmaster EUI-64, used when kind == Ptp
    uint8_t domain = 0;         // PTP domain, used when kind == Ptp
    uint8_t mac[6] = {0};       // sender MAC, used when kind == LocalMac
};

// Builds a minimal AES67 / ST2110-30 SDP body for the configured codec
// (L16 or L24). Reference: AES67-2018 section 8 (SDP) and RFC 4566.
//
// `sessionVersion` becomes the version field of the o= line. A receiver only
// treats an announcement as an update when this number grows, so a sender that
// changes its parameters (or re-locks to another grandmaster) must bump it --
// re-announcing a changed body with the same version is silently ignored.
// Direction of the session. A stream we transmit must say sendonly: writing
// recvonly on it is a lie a strict receiver can act on, and it made our own
// discovery list label our outgoing stream as something to consume.
enum class SdpDirection { SendOnly, RecvOnly };

std::string buildSdp(const StreamConfig &cfg, uint32_t ssrc,
                     const SdpClockRef &clockRef = SdpClockRef{},
                     uint32_t sessionVersion = 1,
                     SdpDirection direction = SdpDirection::SendOnly);

// Extracts a=rtpmap channel count, sample rate and codec from an SDP body.
// `codecOut` may be null; it is only written when the line parses.
// Returns false when the rtpmap line is missing, malformed, or not L16/L24.
bool parseSdpAudioFormat(const std::string &sdp, int *channels, int *sampleRate,
                         Codec *codecOut = nullptr);

// Everything a receiver needs to tune to a stream discovered over SAP.
// Built from the four lines that matter: s= (name), c= (group), m= (port) and
// a=rtpmap (codec/channels/rate). A missing or unusable field leaves that member
// untouched and is reported through `usable`.
struct SapStreamInfo {
    std::string name;         // s= line (may be empty)
    // true when the session says a=recvonly: the announcer is *receiving* this
    // stream, so it is not a source to subscribe to.
    bool        recvOnly = false;
    std::string group;        // c=IN IP4 <group>[/ttl]; the group only
    uint16_t    port = 0;     // from m=audio <port>
    int         channels = 0;
    int         sampleRate = 0;
    Codec       codec = Codec::L16;
    std::string origin;       // RFC 2974 originating source, filled by the caller
    bool        usable = false;   // group is multicast and port/rate/channels are sane
};

// Parses one SDP body into stream information. Returns false when the body has no
// usable audio description (then `out->usable` stays false as well).
bool parseSdpStreamInfo(const std::string &sdp, SapStreamInfo *out);

// Formats an 8-byte EUI-64 as 00-1D-C1-FF-FE-12-34-56 (AES67 / IEEE 1588 style).
std::string formatGmid(const uint8_t gmid[8]);

// Parses "00-1D-C1-FF-FE-12-34-56" or "001DC1FFFE123456" into eight bytes.
bool parseGmid(const std::string &text, uint8_t gmid[8]);

} // namespace openaes67
