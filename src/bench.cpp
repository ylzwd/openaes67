#include "openaes67/bench.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "openaes67/audio_io.h"
#include "openaes67/config.h"
#include "openaes67/frame_queue.h"
#include "openaes67/rtp.h"
#include "openaes67/sdp.h"
#include "openaes67/shared_audio_ring.h"

namespace openaes67 {

namespace {

using Clock = std::chrono::steady_clock;

// The repository path contains non-ASCII characters, so a narrow fopen on the
// baseline path fails with ENOENT. Convert to UTF-16 and use the wide call.
std::FILE *openUtf8(const char *path, const char *mode) {
    const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wideLen <= 0) {
        return nullptr;
    }
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path, -1, &wide[0], wideLen);
    const int modeLen = ::MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    std::wstring wideMode(static_cast<size_t>(modeLen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, mode, -1, &wideMode[0], modeLen);
    return ::_wfopen(wide.c_str(), wideMode.c_str());
}

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// The ring is exercised the way the two threads use it: one side writes a packet's
// worth of frames, the other reads the same amount back. Anything that adds work to
// the copy path (an extra pass, a per-frame modulo, a lock) shows up here.
void benchRing(std::vector<BenchCase> *out, int channels) {
    const int capacity = 4096;
    const size_t bytes = static_cast<size_t>(sizeof(OpenAes67SharedAudio)) +
                         2u * static_cast<size_t>(capacity) * channels * sizeof(int16_t);
    std::vector<uint8_t> mapping(bytes, 0);
    SharedAudioConfig cfg;
    cfg.channels = static_cast<uint32_t>(channels);
    cfg.sampleRate = 48000;
    cfg.capacityFrames = static_cast<uint32_t>(capacity);
    SharedAudioRing ring(mapping.data(), mapping.size(), cfg, /*initialize=*/true);
    if (!ring.valid()) {
        out->push_back(BenchCase{"ring " + std::to_string(channels) + "ch", 0.0,
                                 "Mframes/s", true});
        return;
    }

    const int frames = 240;
    std::vector<int16_t> in(static_cast<size_t>(frames) * channels, 1);
    std::vector<int16_t> back(static_cast<size_t>(frames) * channels, 0);
    const int iterations = 20000;

    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        ring.writeRender(in.data(), frames);
        ring.readRender(back.data(), frames);
    }
    const double elapsed = secondsSince(start);
    const double framesMoved = 2.0 * static_cast<double>(iterations) * frames;
    out->push_back(BenchCase{"ring " + std::to_string(channels) + "ch",
                             elapsed > 0.0 ? framesMoved / elapsed / 1e6 : 0.0,
                             "Mframes/s", true});
}

// Channel count conversion runs on every delivered block when the stream does not
// match the device, which is the normal case (64-channel stream, stereo device).
void benchConvert(std::vector<BenchCase> *out, int from, int to) {
    const int frames = 240;
    std::vector<int16_t> src(static_cast<size_t>(frames) * from, 7);
    std::vector<int16_t> dst(static_cast<size_t>(frames) * to, 0);
    const int iterations = 200000;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        convertChannels(src.data(), frames, from, to, dst.data());
    }
    const double elapsed = secondsSince(start);
    const double framesDone = static_cast<double>(iterations) * frames;
    out->push_back(BenchCase{"convert " + std::to_string(from) + "->" + std::to_string(to),
                             elapsed > 0.0 ? framesDone / elapsed / 1e6 : 0.0,
                             "Mframes/s", true});
}

// The jitter-buffer consume path: one packet appended, then handed to the device
// in small pieces, which is how the receive loop actually drains it.
void benchQueue(std::vector<BenchCase> *out, int channels) {
    FrameQueue queue;
    queue.setChannels(channels);
    const int packetFrames = 48;
    const int deliveryFrames = 12;
    std::vector<int16_t> packet(static_cast<size_t>(packetFrames) * channels, 5);
    // 200 000 iterations finished in ~50 ms, which made the number swing with
    // whatever else the machine was doing. 1 000 000 is still fast but long enough
    // that scheduling noise averages out.
    const int iterations = 1000000;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        queue.append(packet.data(), packetFrames);
        // Four deliveries, one of which lands exactly at the end of the packet.
        for (int d = 0; d < 4; ++d) {
            (void)queue.data();
            queue.consume(deliveryFrames);
        }
    }
    const double elapsed = secondsSince(start);
    const double framesDone = static_cast<double>(iterations) * packetFrames;
    out->push_back(BenchCase{"queue " + std::to_string(channels) + "ch",
                             elapsed > 0.0 ? framesDone / elapsed / 1e6 : 0.0,
                             "Mframes/s", true});
}

// The ASIO driver's interleave/de-interleave, measured both ways: it runs on every
// audio tick, 64 channels, once per 256-frame buffer.
void benchTranspose(std::vector<BenchCase> *out, int channels, bool blocked) {
    const int frames = 256;
    std::vector<int16_t> ring(static_cast<size_t>(frames) * channels, 7);
    std::vector<int32_t> planar(static_cast<size_t>(frames) * channels, 0);
    const int iterations = 20000;
    constexpr int kBlock = 32;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (blocked) {
            for (int base = 0; base < frames; base += kBlock) {
                const int count = (frames - base) < kBlock ? (frames - base) : kBlock;
                for (int ch = 0; ch < channels; ++ch) {
                    const int16_t *src = ring.data() + static_cast<size_t>(base) * channels + ch;
                    int32_t *dst = planar.data() + static_cast<size_t>(ch) * frames;
                    for (int f = 0; f < count; ++f) {
                        dst[base + f] =
                            static_cast<int32_t>(src[static_cast<size_t>(f) * channels]) << 16;
                    }
                }
            }
        } else {
            for (int ch = 0; ch < channels; ++ch) {
                const int16_t *src = ring.data() + ch;
                int32_t *dst = planar.data() + static_cast<size_t>(ch) * frames;
                for (int f = 0; f < frames; ++f) {
                    dst[f] = static_cast<int32_t>(src[static_cast<size_t>(f) * channels]) << 16;
                }
            }
        }
    }
    const double elapsed = secondsSince(start);
    const double framesDone = static_cast<double>(iterations) * frames;
    out->push_back(BenchCase{
        std::string(blocked ? "transpose blocked " : "transpose strided ") +
            std::to_string(channels) + "ch",
        elapsed > 0.0 ? framesDone / elapsed / 1e6 : 0.0, "Mframes/s", true});
}

// RTP header write + parse, once per packet in each direction.
void benchRtp(std::vector<BenchCase> *out) {
    RtpPacketizer packetizer(0x11223344u, 96);
    std::vector<uint8_t> buffer(2048, 0);
    std::vector<int16_t> audio(480, 3);
    RtpHeader parsed;
    const int iterations = 200000;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        // What the sender does per packet: header, then payload, then the
        // receiver's header parse.
        const size_t header = packetizer.writeNextHeader(buffer.data(), buffer.size(), 48);
        const size_t payload = packPayload(Codec::L16, audio.data(), 48,
                                           buffer.data() + header, buffer.size() - header);
        readRtpHeader(buffer.data(), header + payload, &parsed);
    }
    const double elapsed = secondsSince(start);
    out->push_back(BenchCase{"rtp pack+parse",
                             elapsed > 0.0 ? static_cast<double>(iterations) / elapsed / 1e6
                                           : 0.0,
                             "Mops/s", true});
}

// SDP is rebuilt on every announcement and on every grandmaster change.
void benchSdp(std::vector<BenchCase> *out) {
    StreamConfig cfg;
    cfg.ifaceIp = "192.168.1.10";
    cfg.group = "239.69.1.2";
    cfg.port = 5006;
    cfg.channels = 64;
    SdpClockRef ref;
    ref.kind = SdpClockRef::Kind::LocalMac;
    const int iterations = 20000;
    size_t total = 0;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        total += buildSdp(cfg, 1, ref).size();
    }
    const double elapsed = secondsSince(start);
    out->push_back(BenchCase{"sdp build",
                             elapsed > 0.0
                                 ? elapsed / static_cast<double>(iterations) * 1e6
                                 : 0.0,
                             "us", false});
    (void)total;
}

} // namespace

std::vector<BenchCase> runBenchmarks() {
    std::vector<BenchCase> results;
    benchRing(&results, 2);
    benchRing(&results, 64);
    benchQueue(&results, 2);
    benchQueue(&results, 64);
    benchConvert(&results, 64, 2);
    benchConvert(&results, 64, 64);
    // Only the algorithm the driver actually uses; the blocked variant was
    // measured 23% slower and reverted (see FINDINGS 122).
    benchRtp(&results);
    benchSdp(&results);
    return results;
}

bool writeBenchBaseline(const std::string &path, const std::vector<BenchCase> &cases,
                        std::string *error) {
    std::FILE *f = openUtf8(path.c_str(), "wb");
    if (!f) {
        if (error) {
            *error = "cannot write " + path;
        }
        return false;
    }
    std::fprintf(f, "# OpenAES67 benchmark baseline: name value unit\n");
    for (const BenchCase &c : cases) {
        std::fprintf(f, "%s %.6f %s\n", c.name.c_str(), c.value, c.unit.c_str());
    }
    std::fclose(f);
    return true;
}

bool checkBenchBaseline(const std::string &path, const std::vector<BenchCase> &cases,
                        double tolerancePercent, std::string *report) {
    std::FILE *f = openUtf8(path.c_str(), "rb");
    if (!f) {
        if (report) {
            *report = "no baseline at " + path;
        }
        return false;
    }
    std::map<std::string, double> baseline;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            continue;
        }
        char name[256] = {0};
        double value = 0.0;
        if (std::sscanf(line, "%255[^0-9-] %lf", name, &value) == 2) {
            std::string key(name);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.pop_back();
            }
            baseline[key] = value;
        }
    }
    std::fclose(f);

    bool ok = true;
    for (const BenchCase &c : cases) {
        const auto it = baseline.find(c.name);
        if (it == baseline.end() || it->second <= 0.0) {
            continue;   // a new case has nothing to compare against yet
        }
        const double before = it->second;
        // Higher is better for throughput; lower is better for latency.
        const double change = c.higher_is_better ? (c.value - before) / before * 100.0
                                                 : (before - c.value) / before * 100.0;
        const bool pass = change >= -tolerancePercent;
        ok = ok && pass;
        if (report) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%-22s %10.3f %-12s was %10.3f  %+6.1f%%  %s",
                          c.name.c_str(), c.value, c.unit.c_str(), before, change,
                          pass ? "ok" : "REGRESSED");
            *report += std::string(buf) + "\n";
        }
    }
    return ok;
}

} // namespace openaes67
