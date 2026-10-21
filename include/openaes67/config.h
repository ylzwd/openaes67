#pragma once
#include <cstdint>
#include <string>

namespace openaes67 {

// Sample encoding on the wire. AES67-2018 section 7 requires both:
//   L16 = 16-bit linear PCM, 2 bytes per sample, big endian
//   L24 = 24-bit linear PCM, 3 bytes per sample, big endian
//
// The internal audio path of this project is int16 (WASAPI, the shared ring and
// the jitter buffer all use int16_t), so L24 carries those samples in the upper
// 16 bits and leaves the low 8 bits zero. That is honest interoperability, not
// 24-bit resolution: an L24 stream from this sender sounds identical to L16, but
// it can be received by, and accepted from, equipment that only speaks L24.
enum class Codec {
    L16,
    L24
};

// Name as it appears in a=rtpmap (L16 / L24).
const char *codecName(Codec codec);
// Parses "L16" / "L24" (case-insensitive); returns false for anything else.
bool parseCodecName(const std::string &name, Codec *codec);

// One AES67 audio stream (one direction).
struct StreamConfig {
    std::string ifaceIp;      // local IPv4 of the NIC used for multicast
    std::string group;        // e.g. "239.69.1.1"
    uint16_t    port = 5004;
    int         channels = 2;
    int         sampleRate = 48000;
    double      ptimeMs = 1.0;   // packet time
    Codec       codec = Codec::L16;
    uint8_t     payloadType = 96; // dynamic payload type; 97 is used for L24
    int         ttl = 32;
    // RTCP (RFC 3550 section 6). Off by default: AES67 streams do not require it
    // and unsolicited reports to a device that never asked can be noise.
    bool        rtcp = false;

    int bytesPerSample() const { return codec == Codec::L24 ? 3 : 2; }
    // RTCP conventionally rides the next port up from RTP.
    uint16_t rtcpPort() const {
        return port == 65535 ? port : static_cast<uint16_t>(port + 1);
    }
    // Payload size of one packet, header excluded.
    int payloadBytes() const { return framesPerPacket() * channels * bytesPerSample(); }

    // Returns an empty string when the parameters are usable, otherwise a short
    // reason. Kept here (not in the CLI) so the GUI and the tests share one rule;
    // each check guards a failure that is silent rather than loud (port wrap,
    // division by zero, packets no network can carry).
    std::string validate(std::string *detail) const;

    int framesPerPacket() const {
        return static_cast<int>(sampleRate * ptimeMs / 1000.0 + 0.5);
    }
};

} // namespace openaes67
