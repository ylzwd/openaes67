#pragma once
#include <cstdint>
#include <string>

#include "openaes67/config.h"

namespace openaes67 {

// What a short passive listen on one multicast address can tell us about a stream.
//
// Why this exists: Dante AES67 flows are static multicast and are not always
// advertised over SAP, so the operator copies an address out of Dante Controller.
// The address says where, not how -- but the packets themselves carry enough to
// derive the format, which saves the operator from guessing channels, ptime and
// codec (and from getting silence when they guess wrong).
struct RtpProbeResult {
    bool        ok = false;
    std::string error;      // why it failed, when ok is false
    int         packets = 0;
    int         channels = 0;          // 0 = could not be derived
    int         sampleRate = 0;        // 0 = could not be derived (RTP has no rate)
    double      ptimeMs = 0.0;         // from the packet spacing
    Codec       codec = Codec::L16;    // best fit for the payload size
    uint8_t     payloadType = 0;
    uint32_t    ssrc = 0;
    std::string group;
    uint16_t    port = 0;
};

// Listens on <group>:<port> for up to `seconds` and derives the format.
// A stream that carries no packets is reported as an error, not as silence.
RtpProbeResult probeRtpStream(const std::string &ifaceIp, const std::string &group,
                              uint16_t port, double seconds);

} // namespace openaes67
