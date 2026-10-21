#pragma once
#include <cstddef>
#include <cstdint>

#include "openaes67/config.h"

namespace openaes67 {

constexpr size_t kRtpHeaderSize = 12;

// RTP payload <-> internal samples, shared by the sender and the receiver so both
// sides agree on the byte layout and the tests can target one implementation.
//
//   L16: 2 bytes per sample, big endian.
//   L24: 3 bytes per sample, big endian, value in the upper 16 bits (low byte 0)
//        because the internal audio path is int16.
//
// packPayload() returns the number of payload bytes written, or 0 when the output
// buffer is too small. unpackPayload() returns the number of samples written, or 0
// when the input length is not a whole number of samples or the output is too small.
size_t packPayload(Codec codec, const int16_t *samples, size_t sampleCount,
                   uint8_t *out, size_t outSize);
size_t unpackPayload(Codec codec, const uint8_t *in, size_t inSize,
                     int16_t *out, size_t outCount);

// RFC 3550 fixed header, no CSRC / extension / padding.
struct RtpHeader {
    uint8_t  version     = 2;
    bool     marker      = false;
    uint8_t  payloadType = 96;
    uint16_t sequence    = 0;
    uint32_t timestamp   = 0;
    uint32_t ssrc        = 0;
};

// Writes the 12-byte big-endian header into out.
// Returns kRtpHeaderSize, or 0 when the buffer is too small.
size_t writeRtpHeader(uint8_t *out, size_t outSize, const RtpHeader &h);

// Parses a 12-byte big-endian header.
// Returns false when the buffer is too small or the version is not 2.
bool readRtpHeader(const uint8_t *in, size_t inSize, RtpHeader *h);

// Keeps sequence and timestamp in step with the audio frame count.
class RtpPacketizer {
public:
    RtpPacketizer(uint32_t ssrc, uint8_t payloadType = 96);

    // Builds the next packet header (without payload) into out.
    size_t writeNextHeader(uint8_t *out, size_t outSize, int framesInPacket);

    uint32_t ssrc() const { return ssrc_; }

    // Overrides the timestamp of the NEXT packet. Used by a PTP-locked sender:
    // the timestamp is derived from the grandmaster clock instead of incremented.
    void setNextTimestamp(uint32_t timestamp) { timestamp_ = timestamp; }
    // RFC 3550 section 5.1 wants unpredictable initial sequence and timestamp
    // values; both used to start at 0, which makes every session trivially
    // recognisable and allows two senders to look identical on the wire.
    void setMarker(bool on) { markerNext_ = on; }

private:
    uint32_t ssrc_;
    uint8_t  payloadType_;
    uint16_t sequence_ = 0;
    uint32_t timestamp_ = 0;
    bool     markerNext_ = false;
};

} // namespace openaes67