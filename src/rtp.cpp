#include <random>

#include "openaes67/rtp.h"

namespace openaes67 {

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

static inline uint16_t get16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static inline uint32_t get32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

size_t packPayload(Codec codec, const int16_t *samples, size_t sampleCount,
                   uint8_t *out, size_t outSize) {
    if (!samples || !out) {
        return 0;
    }
    if (codec == Codec::L24) {
        if (outSize < sampleCount * 3) {
            return 0;
        }
        for (size_t i = 0; i < sampleCount; ++i) {
            // int16 value in the top two bytes, low byte zero.
            const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(samples[i]));
            out[i * 3 + 0] = static_cast<uint8_t>(v >> 8);
            out[i * 3 + 1] = static_cast<uint8_t>(v & 0xFF);
            out[i * 3 + 2] = 0;
        }
        return sampleCount * 3;
    }
    if (outSize < sampleCount * 2) {
        return 0;
    }
    for (size_t i = 0; i < sampleCount; ++i) {
        put16(out + i * 2, static_cast<uint16_t>(samples[i]));
    }
    return sampleCount * 2;
}

size_t unpackPayload(Codec codec, const uint8_t *in, size_t inSize,
                     int16_t *out, size_t outCount) {
    if (!in || !out) {
        return 0;
    }
    if (codec == Codec::L24) {
        const size_t samples = inSize / 3;
        if (samples == 0 || inSize % 3 != 0 || samples > outCount) {
            return 0;
        }
        for (size_t i = 0; i < samples; ++i) {
            // Keep the top 16 bits; the low byte is discarded.
            out[i] = static_cast<int16_t>(get16(in + i * 3));
        }
        return samples;
    }
    const size_t samples = inSize / 2;
    if (samples == 0 || inSize % 2 != 0 || samples > outCount) {
        return 0;
    }
    for (size_t i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(get16(in + i * 2));
    }
    return samples;
}

size_t writeRtpHeader(uint8_t *out, size_t outSize, const RtpHeader &h) {
    if (!out || outSize < kRtpHeaderSize) {
        return 0;
    }
    // V=2, P=0, X=0, CC=0  |  M, PT
    out[0] = static_cast<uint8_t>((h.version & 0x03) << 6);
    out[1] = static_cast<uint8_t>(((h.marker ? 1u : 0u) << 7) | (h.payloadType & 0x7F));
    put16(out + 2, h.sequence);
    put32(out + 4, h.timestamp);
    put32(out + 8, h.ssrc);
    return kRtpHeaderSize;
}

bool readRtpHeader(const uint8_t *in, size_t inSize, RtpHeader *h) {
    if (!in || !h || inSize < kRtpHeaderSize) {
        return false;
    }
    const uint8_t version = static_cast<uint8_t>((in[0] >> 6) & 0x03);
    if (version != 2) {
        return false;
    }
    h->version     = version;
    h->marker      = (in[1] & 0x80) != 0;
    h->payloadType = static_cast<uint8_t>(in[1] & 0x7F);
    h->sequence    = get16(in + 2);
    h->timestamp   = get32(in + 4);
    h->ssrc        = get32(in + 8);
    return true;
}

RtpPacketizer::RtpPacketizer(uint32_t ssrc, uint8_t payloadType)
    : ssrc_(ssrc), payloadType_(payloadType) {
    // RFC 3550 section 5.1: initial sequence and timestamp must be unpredictable.
    std::random_device rd;
    sequence_  = static_cast<uint16_t>(rd() & 0xFFFF);
    timestamp_ = rd();
}

size_t RtpPacketizer::writeNextHeader(uint8_t *out, size_t outSize, int framesInPacket) {
    RtpHeader h;
    h.version     = 2;
    h.marker      = markerNext_;
    markerNext_   = false;
    h.payloadType = payloadType_;
    h.sequence    = sequence_;
    h.timestamp   = timestamp_;
    h.ssrc        = ssrc_;

    const size_t n = writeRtpHeader(out, outSize, h);
    if (n == 0) {
        return 0;
    }

    sequence_  = static_cast<uint16_t>(sequence_ + 1);
    timestamp_ = static_cast<uint32_t>(timestamp_ + static_cast<uint32_t>(framesInPacket));
    return n;
}

} // namespace openaes67