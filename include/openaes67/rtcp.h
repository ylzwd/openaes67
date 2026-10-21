#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace openaes67 {

// RTCP (RFC 3550 section 6) — just the parts an AES67 endpoint needs to be a
// well-behaved participant:
//
//   Sender Report   (PT 200): wallclock + RTP timestamp mapping, packet and octet
//                             counts; tells a receiver what the sender's clock is.
//   Receiver Report (PT 201): per-source loss, highest sequence, jitter; tells a
//                             sender how its stream is arriving.
//   SDES CNAME      (PT 202): required canonical name, without which some stacks
//                             discard the compound packet.
//
// Packets are always compound: SR or RR first, then SDES with CNAME (RFC 3550
// section 6.1). Everything here is pure byte handling so it can be unit tested.

constexpr uint8_t kRtcpSenderReport = 200;
constexpr uint8_t kRtcpReceiverReport = 201;
constexpr uint8_t kRtcpSdes = 202;

// A 64-bit NTP timestamp: seconds since 1900 in the high word, fraction in the low.
uint64_t rtcpNtpNow();

struct RtcpReportBlock {
    uint32_t ssrc = 0;               // source this block describes
    uint8_t  fractionLost = 0;       // 0..255 = 0..100 %
    int32_t  cumulativeLost = 0;     // signed 24-bit on the wire
    uint32_t highestSequence = 0;    // extended sequence number
    uint32_t jitter = 0;             // RTP timestamp units
    uint32_t lastSenderReport = 0;   // middle 32 bits of the sender's NTP time
    uint32_t delaySinceLastSr = 0;   // units of 1/65536 s
};

struct RtcpSenderReport {
    uint32_t ssrc = 0;
    uint64_t ntp = 0;
    uint32_t rtpTimestamp = 0;
    uint32_t packetCount = 0;
    uint32_t octetCount = 0;
    RtcpReportBlock blocks[1];       // zero or one block from this endpoint
    int blockCount = 0;
};

// What a receiver reports about one source.
struct RtcpReceptionStats {
    uint32_t ssrc = 0;
    uint8_t  fractionLost = 0;
    int32_t  cumulativeLost = 0;
    uint32_t highestSequence = 0;
    uint32_t jitter = 0;
};

// Builds an SR + SDES(CNAME) compound packet. `cname` must be non-empty (the
// user@host form is customary).
std::string buildRtcpSenderReport(const RtcpSenderReport &sr, const std::string &cname);

// Builds an RR + SDES(CNAME) compound packet with zero or one report block.
std::string buildRtcpReceiverReport(uint32_t reporterSsrc,
                                    const RtcpReceptionStats *stats,
                                    const std::string &cname);

// Parses the first packet of a compound RTCP datagram. Returns false when the
// datagram is too short, not version 2, or not an SR. Fields are only written on
// success.
bool parseRtcpSenderReport(const uint8_t *data, size_t len, RtcpSenderReport *out);

// Parses the first packet of a compound RTCP datagram when it is an RR. The first
// report block is returned when present.
bool parseRtcpReceiverReport(const uint8_t *data, size_t len,
                             RtcpReceptionStats *out, uint32_t *reporterSsrc);

// Extracts the CNAME from the SDES packet that follows an SR/RR. Returns false when
// there is no SDES chunk or no CNAME item.
bool parseRtcpCname(const uint8_t *data, size_t len, std::string *cname);

} // namespace openaes67
