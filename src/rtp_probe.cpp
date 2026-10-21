#include "openaes67/rtp_probe.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "openaes67/net.h"
#include "openaes67/rtp.h"

namespace openaes67 {

namespace {

// RTP timestamps advance at the sample rate, so two packets give the rate:
//   rate = (ts2 - ts1) * 1000 / dtMs
// That is exact for a well-formed sender and needs no SDP.
int rateFromTimestamps(uint32_t ts0, uint32_t ts1, double dtMs) {
    if (dtMs <= 0.0) {
        return 0;
    }
    const uint32_t delta = ts1 - ts0;   // wraps safely
    const double rate = static_cast<double>(delta) * 1000.0 / dtMs;
    // Snap to the rates AES67 actually uses; anything else is noise.
    static const int kRates[] = {8000, 16000, 32000, 44100, 48000, 88200, 96000, 192000};
    for (const int candidate : kRates) {
        if (std::fabs(rate - candidate) < candidate * 0.02) {
            return candidate;
        }
    }
    return static_cast<int>(std::lround(rate));
}

// Payload bytes = framesPerPacket * channels * bytesPerSample. With the frame
// count derived from ptime and rate, the remaining unknown is the encoding; try
// both and keep the combination that yields a legal channel count.
// The measured ptime always has a little jitter (a 1 ms stream may measure 0.98 ms),
// so the frame count is snapped to the packet sizes AES67 actually uses before the
// payload is divided by it. Without the snap, 48 frames becomes 47 and nothing
// divides evenly.
void deriveFormat(int payloadBytes, int sampleRate, double ptimeMs, uint8_t payloadType, RtpProbeResult *out) {
    if (payloadBytes <= 0 || sampleRate <= 0 || ptimeMs <= 0.0) {
        return;
    }
    static const int kFrames[] = {6,   8,   12,  16,  24,  32,  48,   64,
                                  96,  120, 128, 144, 160, 192, 240,  256,
                                  288, 320, 384, 480, 512, 576, 720,  960};
    const double exact = sampleRate * ptimeMs / 1000.0;
    int best = 0;
    double bestDistance = 1e9;
    for (const int frames : kFrames) {
        const double distance = std::fabs(frames - exact);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = frames;
        }
    }
    // Only trust a snap that is close to the measurement.
    const int frames = (best > 0 && bestDistance <= std::max(2.0, exact * 0.15)) ? best
                                                                                 : static_cast<int>(std::lround(exact));
    if (frames <= 0) {
        return;
    }
    // L16 6 ch and L24 4 ch are the same number of bytes, so the size alone cannot
    // settle it. The payload type can: 96 is L16 and 97 is L24 by the convention
    // every AES67 implementation follows; anything else is reported as ambiguous.
    Codec order[2] = {Codec::L16, Codec::L24};
    if (payloadType == 97) {
        order[0] = Codec::L24;
        order[1] = Codec::L16;
    }
    for (const Codec codec : order) {
        const int bytesPerSample = codec == Codec::L24 ? 3 : 2;
        if (payloadBytes % (frames * bytesPerSample) != 0) {
            continue;
        }
        const int channels = payloadBytes / (frames * bytesPerSample);
        if (channels >= 1 && channels <= 64) {
            out->channels = channels;
            out->codec = codec;
            // Both encodings fit and the payload type does not decide: say so
            // instead of silently picking one.
            const int otherBytes = codec == Codec::L16 ? 3 : 2;
            const Codec other = codec == Codec::L16 ? Codec::L24 : Codec::L16;
            const int otherChannels = (payloadType == 96 || payloadType == 97)
                                          ? -1
                                          : payloadBytes / (frames * otherBytes);
            if (payloadType != 96 && payloadType != 97 && otherChannels >= 1 &&
                otherChannels <= 64 && payloadBytes % (frames * otherBytes) == 0) {
                out->error = "ambiguous: the payload fits L16 " + std::to_string(channels) +
                             " ch and L24 " + std::to_string(otherChannels) +
                             " ch (payload type " + std::to_string(payloadType) + ")";
                (void)other;
            }
            return;
        }
    }
}

} // namespace

RtpProbeResult probeRtpStream(const std::string &ifaceIp, const std::string &group,
                              uint16_t port, double seconds) {
    RtpProbeResult result;
    result.group = group;
    result.port = port;

    UdpSocket socket;
    std::string err;
    if (!socket.openReceiver(group, port, ifaceIp, &err)) {
        result.error = err.empty() ? "cannot open the multicast socket" : err;
        return result;
    }

    std::vector<uint8_t> buffer(65536);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
    std::chrono::steady_clock::time_point first{};
    std::chrono::steady_clock::time_point last{};
    uint32_t firstTs = 0;
    uint32_t lastTs = 0;
    int payloadBytes = -1;
    bool haveFirst = false;

    while (std::chrono::steady_clock::now() < deadline) {
        const int n = socket.recvFrom(buffer.data(), buffer.size(), 200);
        if (n <= 0) {
            continue;
        }
        RtpHeader header;
        if (n < 12 || !readRtpHeader(buffer.data(), static_cast<size_t>(n), &header)) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        ++result.packets;
        if (!haveFirst) {
            first = now;
            firstTs = header.timestamp;
            haveFirst = true;
            result.payloadType = header.payloadType;
            result.ssrc = header.ssrc;
            payloadBytes = n - 12;
        }
        last = now;
        lastTs = header.timestamp;
        // A 125 us stream packs 8 packets per millisecond, so a 50-packet window is
        // only ~6 ms long and the timestamp rate measured over it is useless. Keep
        // going until the window is long enough to divide by.
        const double spannedMs = std::chrono::duration<double, std::milli>(last - first).count();
        if (result.packets >= 10 && spannedMs >= 300.0) {
            break;
        }
    }
    socket.close();

    if (!haveFirst || result.packets < 2) {
        result.error = "no RTP packets on that address";
        return result;
    }

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(last - first).count();
    const int gaps = result.packets - 1;
    result.ptimeMs = gaps > 0 ? elapsedMs / gaps : 0.0;
    result.sampleRate = rateFromTimestamps(firstTs, lastTs, elapsedMs);

    deriveFormat(payloadBytes, result.sampleRate, result.ptimeMs, result.payloadType, &result);
    result.ok = result.channels > 0 && result.sampleRate > 0 && result.error.empty();
    if (!result.ok && result.error.empty()) {
        result.error = "packets arrived but the format could not be derived";
    }
    return result;
}

} // namespace openaes67
