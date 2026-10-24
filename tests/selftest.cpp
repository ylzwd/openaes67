// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

// Protocol self-test: no audio hardware, no network.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <windows.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "openaes67/rtp.h"
#include "openaes67/sap.h"
#include "openaes67/asio_probe.h"
#include "openaes67/text_encoding.h"
#include "openaes67/cli_config.h"
#include "openaes67/frame_queue.h"
#include "openaes67/sdp.h"
#include "openaes67/jitter.h"
#include "openaes67/ptp.h"
#include "openaes67/audio_io.h"
#include "openaes67/wav.h"
#include "openaes67/rtcp.h"
#include "openaes67/net.h"
#include "openaes67/config.h"
#include "openaes67/resampler.h"
#include "openaes67/shared_audio_ring.h"

// The test blocks deliberately reuse short names (out, cfg, h, chunk) inside
// their own scopes; that is readable here and shadowing is harmless. Production
// sources are warning-clean at /W4, which is where the warnings matter.
#ifdef _MSC_VER
#pragma warning(disable : 4456)   // declaration hides previous local
#endif


using namespace openaes67;

static int failures = 0;
static void check(const char *name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static std::vector<int16_t> makePacket(int frames, int channels, int16_t value) {
    return std::vector<int16_t>(static_cast<size_t>(frames) * channels, value);
}

// Tests must not depend on a directory that exists only on the developer's
// machine: a hard-coded path both leaks local layout and fails on a clean
// checkout. Everything that needs a file goes under the process temp directory.
static std::string tempTestPath(const char *name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = std::filesystem::current_path(ec);
    }
    dir /= "openaes67-selftest";
    std::filesystem::create_directories(dir, ec);
    // u8string(), not string(): the temp path contains the user name, and every
    // file helper in this project treats narrow paths as UTF-8. string() returns
    // the ANSI code page, which is not UTF-8 when the name is non-ASCII.
    return (dir / name).u8string();
}
int main() {
    std::printf("OpenAES67 protocol self-test\n");

    // ---------------- RTP header ----------------
    RtpHeader h;
    h.sequence = 0x1234;
    h.timestamp = 0xDEADBEEF;
    h.ssrc = 0x5EED0001;
    h.payloadType = 96;

    uint8_t buf[64] = {0};
    const size_t n = writeRtpHeader(buf, sizeof(buf), h);
    check("header size is 12", n == kRtpHeaderSize);
    check("first byte is 0x80 (V=2)", buf[0] == 0x80);
    check("second byte is 96", buf[1] == 96);
    check("sequence big-endian", buf[2] == 0x12 && buf[3] == 0x34);
    check("timestamp big-endian", buf[4] == 0xDE && buf[5] == 0xAD && buf[6] == 0xBE && buf[7] == 0xEF);
    check("ssrc big-endian", buf[8] == 0x5E && buf[9] == 0xED && buf[10] == 0x00 && buf[11] == 0x01);

    RtpHeader r;
    check("parse succeeds", readRtpHeader(buf, n, &r));
    check("parsed sequence", r.sequence == 0x1234);
    check("parsed timestamp", r.timestamp == 0xDEADBEEF);
    check("parsed ssrc", r.ssrc == 0x5EED0001);
    check("parsed payload type", r.payloadType == 96);
    check("short buffer rejected", readRtpHeader(buf, 8, &r) == false);

    RtpPacketizer pkt(0x5EED0001);
    uint8_t b1[12], b2[12];
    pkt.writeNextHeader(b1, sizeof(b1), 48);
    pkt.writeNextHeader(b2, sizeof(b2), 48);
    RtpHeader h1, h2;
    readRtpHeader(b1, sizeof(b1), &h1);
    readRtpHeader(b2, sizeof(b2), &h2);
    check("seq increments", h2.sequence == h1.sequence + 1);
    check("timestamp advances by 48", h2.timestamp == h1.timestamp + 48);

    // ---------------- SDP ----------------
    StreamConfig cfg;
    cfg.ifaceIp = "192.168.1.10";
    cfg.group = "239.69.1.1";
    cfg.port = 5004;
    cfg.channels = 8;
    cfg.sampleRate = 48000;
    cfg.ptimeMs = 1.0;

    const std::string sdp = buildSdp(cfg, 0x5EED0001);
    check("sdp has rtpmap", sdp.find("a=rtpmap:96 L16/48000/8") != std::string::npos);
    check("sdp has ptime", sdp.find("a=ptime:1") != std::string::npos);
    check("sdp has multicast", sdp.find("c=IN IP4 239.69.1.1/32") != std::string::npos);

    int ch = 0, sr = 0;
    check("sdp parses back", parseSdpAudioFormat(sdp, &ch, &sr));
    check("parsed channels = 8", ch == 8);
    check("parsed rate = 48000", sr == 48000);
    check("48 frames per 1ms at 48k", cfg.framesPerPacket() == 48);

    // ---------------- jitter buffer ----------------
    const int fpp = 4, chans = 2;
    int16_t out[64];

    {   // in-order delivery after prefill
        JitterBuffer jb(fpp, chans, 4);
        for (int i = 0; i < 4; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(100 + i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        int got = 0, packets = 0;
        while ((got = jb.pop(out, fpp)) > 0) { ++packets; }
        check("jitter: prefill then play in order", packets == 4);
    }

    {   // out-of-order arrival is reordered
        JitterBuffer jb(fpp, chans, 4);
        const int order[4] = {0, 2, 1, 3};
        for (int k = 0; k < 4; ++k) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(order[k]));
            jb.push(static_cast<uint16_t>(order[k]), p.data(), fpp);
        }
        check("jitter: pop 1st", jb.pop(out, fpp) == fpp && out[0] == 0);
        check("jitter: pop 2nd", jb.pop(out, fpp) == fpp && out[0] == 1);
        check("jitter: pop 3rd", jb.pop(out, fpp) == fpp && out[0] == 2);
        check("jitter: pop 4th", jb.pop(out, fpp) == fpp && out[0] == 3);
    }

    {   // a packet that arrives late but still within the window is reordered,
        // not counted as loss (the old code declared loss immediately)
        JitterBuffer jb(fpp, chans, 4);
        auto p0 = makePacket(fpp, chans, 0);
        auto p1 = makePacket(fpp, chans, 1);
        auto p2 = makePacket(fpp, chans, 2);
        auto p3 = makePacket(fpp, chans, 3);
        jb.push(0, p0.data(), fpp);
        jb.push(1, p1.data(), fpp);
        jb.push(3, p3.data(), fpp);   // 2 arrives late
        jb.push(2, p2.data(), fpp);
        int a0 = jb.pop(out, fpp);
        int a1 = jb.pop(out, fpp);
        int a2 = jb.pop(out, fpp);
        int a3 = jb.pop(out, fpp);
        check("jitter: late packet still reordered",
              a0 == fpp && a1 == fpp && a2 == fpp && a3 == fpp);
        check("jitter: reordering is not loss", jb.lost() == 0);
    }

    {   // the buffer must not grow without bound when nothing is consumed
        JitterBuffer jb(fpp, chans, 4);
        auto p = makePacket(fpp, chans, 5);
        for (int i = 0; i < 400; ++i) {
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        check("jitter: bounded depth", jb.depth() <= 200);
        check("jitter: overflow counted as loss", jb.lost() > 0);
    }

    {   // a partial read must not discard the rest of the packet
        JitterBuffer jb(fpp, chans, 1);
        auto p = makePacket(fpp, chans, 9);
        jb.push(0, p.data(), fpp);
        int16_t half[64] = {0};
        const int first = jb.pop(half, fpp / 2);
        const int second = jb.pop(out, fpp);
        check("jitter: partial read returns rest",
              first == fpp / 2 && second == fpp - fpp / 2);
    }

    {   // duplicates are counted and dropped
        JitterBuffer jb(fpp, chans, 2);
        auto p = makePacket(fpp, chans, 7);
        jb.push(10, p.data(), fpp);
        jb.push(10, p.data(), fpp);
        check("jitter: duplicate counted", jb.duplicates() == 1);
    }

    {   // a lost packet is skipped and counted once the reorder window is
        // exhausted; playback does not stall forever
        JitterBuffer jb(fpp, chans, 4);
        for (int i = 0; i < 4; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        for (int i = 0; i < 4; ++i) { jb.pop(out, fpp); }
        // seq 5..9 queued, seq 4 never arrives: enough depth to conclude loss
        for (int i = 5; i <= 9; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        const uint32_t lostBefore = jb.lost();
        // The hole gets a few pops to fill before it is declared lost. (It used to
        // be declared lost immediately whenever the queue was deeper than the
        // reorder tolerance, which is always true after prebuffering.)
        int waits = 0;
        int got = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            got = jb.pop(out, fpp);
            if (got > 0) {
                break;
            }
            ++waits;
        }
        check("jitter: gives the hole a few ticks before giving up",
              waits >= 1 && waits <= 4);
        check("jitter: then skips the gap and plays next", got == fpp && out[0] == 5);
        check("jitter: gap still counted as lost", jb.lost() == lostBefore + 1);
    }

    {   // A hole must be waited on even when the queue is deep: with a 20 ms
        // prefill, depth is always >= kReorderTolerance, so keying the decision on
        // depth declared every single reordered packet as loss.
        JitterBuffer jb(fpp, chans, 4);
        for (int i = 0; i < 4; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        for (int i = 0; i < 4; ++i) { jb.pop(out, fpp); }
        // Queue holds 5..12 (eight packets) while sequence 4 is still missing.
        for (int i = 5; i <= 12; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        const uint32_t lostBefore = jb.lost();
        check("jitter: deep queue waits for the hole", jb.pop(out, fpp) == 0);
        check("jitter: deep waiting is not loss", jb.lost() == lostBefore);
        // The late packet arrives and must be played, not dropped as late.
        auto late = makePacket(fpp, chans, 4);
        check("jitter: late packet accepted after the wait",
              jb.push(4, late.data(), fpp));
        check("jitter: late packet plays in order",
              jb.pop(out, fpp) == fpp && out[0] == 4);
        check("jitter: recovered packet is not loss", jb.lost() == lostBefore);
    }


    {   // with only a shallow queue a hole is waited on, not declared lost
        JitterBuffer jb(fpp, chans, 4);
        for (int i = 0; i < 4; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
        }
        for (int i = 0; i < 4; ++i) { jb.pop(out, fpp); }
        auto p6 = makePacket(fpp, chans, 6);
        jb.push(6, p6.data(), fpp);   // seq 5 missing, depth 1
        check("jitter: shallow hole waits", jb.pop(out, fpp) == 0);
        check("jitter: waiting is not loss", jb.lost() == 0);
    }

    {   // 16-bit sequence wrap
        JitterBuffer jb(fpp, chans, 4);
        const uint16_t seqs[4] = {65534, 65535, 0, 1};
        for (int i = 0; i < 4; ++i) {
            auto p = makePacket(fpp, chans, static_cast<int16_t>(i));
            jb.push(seqs[i], p.data(), fpp);
        }
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const int got = jb.pop(out, fpp);
            if (got != fpp || out[0] != i) { ok = false; }
        }
        check("jitter: handles sequence wrap", ok);
    }

    // ---------------- rate controller (clock recovery) ----------------
    {
        // Locked clocks: no resampling should happen at all.
        RateController rc(2, 48000, 30.0);
        std::vector<int16_t> frame(96);
        std::vector<int16_t> reference;
        std::vector<int16_t> produced;
        for (int i = 0; i < 30; ++i) {
            for (int k = 0; k < 96; ++k) {
                frame[k] = static_cast<int16_t>((i * 131 + k * 7) % 30000 - 15000);
            }
            rc.push(frame.data(), 48);
            reference.insert(reference.end(), frame.begin(), frame.end());
        }
        for (int step = 0; step < 300; ++step) {
            for (int k = 0; k < 96; ++k) {
                frame[k] = static_cast<int16_t>((step * 37 + k * 11) % 30000 - 15000);
            }
            rc.push(frame.data(), 48);
            reference.insert(reference.end(), frame.begin(), frame.end());
            int16_t out[96];
            const int n = rc.pull(out, 48);
            produced.insert(produced.end(), out, out + n * 2);
        }
        // The FIFO starts full of prefill, so the first pull returns prefill frames.
        const size_t prefillSamples = 0;
        int maxDiff = 0;
        size_t cmp = 0;
        if (reference.size() > prefillSamples) {
            cmp = std::min(produced.size(), reference.size() - prefillSamples);
        }
        for (size_t i = 0; i < cmp; ++i) {
            const int d = std::abs(static_cast<int>(produced[i]) -
                                   static_cast<int>(reference[prefillSamples + i]));
            if (d > maxDiff) {
                maxDiff = d;
            }
        }
        std::printf("       [locked] eff=%.3f ppm maxDiff=%d cmp=%zu\n",
                    rc.effectivePpm(), maxDiff, cmp);
        check("rate: locked clock does not resample", std::fabs(rc.effectivePpm()) < 1.0);
        check("rate: locked output matches input (<=2 LSB)", cmp > 0 && maxDiff <= 2);
    }

    {
        // Device clock 300 ppm fast. The buffer must stay bounded and the
        // measured rate ratio must reflect the fast device (positive).
        RateController rc(2, 48000, 30.0);
        std::vector<int16_t> zeros(96, 0);
        for (int i = 0; i < 30; ++i) {
            rc.push(zeros.data(), 48);
        }
        const double ppm = 300.0;
        const double framesPerStep = 48.0 * (1.0 + ppm * 1e-6);
        long long wantCum = 0;
        int16_t out[128];
        for (int step = 1; step <= 20000; ++step) {
            rc.push(zeros.data(), 48);
            const long long target = static_cast<long long>(framesPerStep * step);
            const int want = static_cast<int>(target - wantCum);
            wantCum = target;
            rc.pull(out, want > 120 ? 120 : want);
        }
        const double errMs = (rc.bufferedFrames() - rc.targetFrames()) / 48.0;
        std::printf("       [+300ppm] err=%.2f ms eff=%.1f ppm peak=%.2f ms buffered=%d\n",
                    errMs, rc.effectivePpm(), rc.peakErrorFrames() / 48.0, rc.bufferedFrames());
        check("rate: holds buffer with +300ppm device",
              std::fabs(errMs) < 5.0 && rc.bufferedFrames() > 100);
        check("rate: rate ratio reflects +300ppm device",
              rc.effectivePpm() > 100.0 && rc.effectivePpm() < 450.0);
        check("rate: peak error stays bounded", rc.peakErrorFrames() / 48.0 < 20.0);
    }

    {
        // Device clock 300 ppm slow: symmetric case.
        RateController rc(2, 48000, 30.0);
        std::vector<int16_t> zeros(96, 0);
        for (int i = 0; i < 30; ++i) {
            rc.push(zeros.data(), 48);
        }
        const double ppm = -300.0;
        const double framesPerStep = 48.0 * (1.0 + ppm * 1e-6);
        long long wantCum = 0;
        int16_t out[128];
        for (int step = 1; step <= 20000; ++step) {
            rc.push(zeros.data(), 48);
            const long long target = static_cast<long long>(framesPerStep * step);
            int want = static_cast<int>(target - wantCum);
            wantCum = target;
            if (want < 0) want = 0;
            rc.pull(out, want > 120 ? 120 : want);
        }
        const double errMs = (rc.bufferedFrames() - rc.targetFrames()) / 48.0;
        std::printf("       [-300ppm] err=%.2f ms eff=%.1f ppm peak=%.2f ms buffered=%d\n",
                    errMs, rc.effectivePpm(), rc.peakErrorFrames() / 48.0, rc.bufferedFrames());
        check("rate: holds buffer with -300ppm device", std::fabs(errMs) < 5.0);
        check("rate: rate ratio reflects -300ppm device",
              rc.effectivePpm() < -100.0 && rc.effectivePpm() > -450.0);
    }
    // ---------------- shared audio ring (M6 shared-memory ABI) ----------------
    {
        SharedAudioConfig cfg;
        cfg.channels = 2;
        cfg.sampleRate = 48000;
        cfg.capacityFrames = 64;

        const uint32_t frameBytes = static_cast<uint32_t>(cfg.channels) * 2u;
        const size_t bytes = openaes67_mapping_size(cfg.capacityFrames, frameBytes);
        std::vector<uint8_t> mem(bytes, 0);

        SharedAudioRing ring(mem.data(), bytes, cfg, true);
        check("ring: header validates", ring.valid());
        check("ring: reports configured geometry",
              ring.channels() == 2 && ring.sampleRate() == 48000 &&
                  ring.capacityFrames() == 64);

        std::vector<int16_t> src(static_cast<size_t>(cfg.capacityFrames) * cfg.channels);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<int16_t>(static_cast<int>(i * 37) % 20000 - 10000);
        }
        std::vector<int16_t> dst(static_cast<size_t>(cfg.capacityFrames) * cfg.channels, 0);

        // Fill 40, drain 24, fill 40 again: the second write crosses the end of
        // the ring, so this exercises the wrap path in both directions.
        const int w1 = ring.writeRender(src.data(), 40);
        const int r1 = ring.readRender(dst.data(), 24);
        const int w2 = ring.writeRender(src.data(), 40);
        const int r2 = ring.readRender(dst.data() + 24 * cfg.channels, 40);
        check("ring: write/read accept expected counts",
              w1 == 40 && r1 == 24 && w2 == 40 && r2 == 40);

        // After filling 40 and draining 24, the FIFO holds frames 24..39.
        // Filling 40 more queues frames 0..39 behind them, so the second read
        // must return 24..39 followed by 0..23: that is the wrap being verified.
        bool same = true;
        for (int i = 0; i < 24 * cfg.channels; ++i) {
            if (dst[i] != src[i]) same = false;
        }
        for (int i = 0; i < 16 * cfg.channels; ++i) {
            if (dst[24 * cfg.channels + i] != src[24 * cfg.channels + i]) same = false;
        }
        for (int i = 0; i < 24 * cfg.channels; ++i) {
            if (dst[(24 + 16) * cfg.channels + i] != src[i]) same = false;
        }
        check("ring: data is bit-exact across the wrap", same);

        // Overrun: a full ring must accept nothing more, and must not corrupt.
        ring.writeRender(src.data(), static_cast<int>(cfg.capacityFrames));
        const int overrun = ring.writeRender(src.data(), 10);
        check("ring: full ring refuses more data", overrun == 0 && ring.renderFree() == 0);

        // Underrun: an empty ring returns zero frames rather than blocking.
        ring.readRender(dst.data(), static_cast<int>(cfg.capacityFrames));
        const int drained = ring.readRender(dst.data(), 10);
        check("ring: empty ring returns zero frames", drained == 0);

        // Capture direction round trip.
        std::vector<int16_t> cap(static_cast<size_t>(cfg.capacityFrames) * cfg.channels);
        for (size_t i = 0; i < cap.size(); ++i) {
            cap[i] = static_cast<int16_t>(static_cast<int>(i * 11) % 12000 - 6000);
        }
        const int cw = ring.writeCapture(cap.data(), 30);
        std::vector<int16_t> back(static_cast<size_t>(cfg.capacityFrames) * cfg.channels, 0);
        const int cr = ring.readCapture(back.data(), 30);
        bool capSame = cw == 30 && cr == 30;
        for (int i = 0; i < 30 * cfg.channels && capSame; ++i) {
            if (back[i] != cap[i]) capSame = false;
        }
        check("ring: capture direction round-trips", capSame);

        ring.setRunning(true);
        check("ring: running flag round-trips", ring.running());
    }

    {
        // Real named section: create in this process, attach a second view, and
        // prove data crosses between the two views bit-exactly.
        SharedAudioConfig cfg;
        cfg.channels = 2;
        cfg.sampleRate = 48000;
        cfg.capacityFrames = 480;          // 10 ms
        const wchar_t *name = L"OpenAES67-Selftest-Section";

        std::string err;
        auto writer = createSharedAudio(name, cfg, &err);
        check("ring: named section created", writer != nullptr);
        if (writer) {
            auto reader = openSharedAudio(name, &err);
            check("ring: named section attached twice", reader != nullptr);
            if (reader) {
                std::vector<int16_t> src(160 * cfg.channels);
                for (size_t i = 0; i < src.size(); ++i) {
                    src[i] = static_cast<int16_t>(static_cast<int>(i * 53) % 24000 - 12000);
                }
                const int w = writer->writeRender(src.data(), 160);
                std::vector<int16_t> dst(160 * cfg.channels, 0);
                const int r = reader->readRender(dst.data(), 160);
                bool ok = (w == 160 && r == 160);
                for (size_t i = 0; i < src.size() && ok; ++i) {
                    if (dst[i] != src[i]) ok = false;
                }
                check("ring: data crosses two views bit-exactly", ok);
            }

    // ---------------- parser fuzz (deterministic) ----------------
    {
        // Feed both parsers random and truncated inputs. The point is not to prove
        // correctness but to prove they never read out of bounds or crash: a receiver
        // sees whatever the network delivers.
        uint32_t seed = 0x12345678u;
        auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        bool ok = true;
        std::string junk;
        for (int round = 0; round < 4000 && ok; ++round) {
            junk.assign(static_cast<size_t>(next() % 200), char(0));
            for (auto &c : junk) {
                c = static_cast<char>(next() & 0xFF);
            }
            SapAnnouncement parsed;
            (void)parseSapAnnouncement(reinterpret_cast<const uint8_t *>(junk.data()),
                                       junk.size(), &parsed);
            int ch = 0, sr = 0;
            (void)parseSdpAudioFormat(junk, &ch, &sr);
            // Whatever comes back must still be a usable pair or untouched defaults.
            if (ch < 0 || sr < 0) {
                ok = false;
            }
        }
        check("fuzz: random input does not crash the parsers", ok);

        // Truncating a valid packet at every length must be safe.
        StreamConfig fuzzCfg;
        fuzzCfg.group = "239.69.1.1";
        const std::string sdp = buildSdp(fuzzCfg, 0x11223344);
        const auto packet = buildSapDatagram(false, sdp, "10.0.0.1",
                                             "o=- 1 1 IN IP4 10.0.0.1", 0x5151);
        bool safe = true;
        for (size_t len = 0; len <= packet.size(); ++len) {
            SapAnnouncement parsed;
            (void)parseSapAnnouncement(packet.data(), len, &parsed);
        }
        check("fuzz: every truncation of a valid SAP packet is safe", safe);
    }


    // ---------------- SDP parse robustness ----------------
    {
        int ch = 0, sr = 0;
        const std::string base =
            "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=x\r\nc=IN IP4 239.69.1.1/32\r\n"
            "t=0 0\r\nm=audio 5004 RTP/AVP 96\r\n";

        // LF-only line endings must parse: not every tool emits CRLF.
        check("sdp: accepts LF-only text",
              parseSdpAudioFormat("m=audio 5004 RTP/AVP 96\na=rtpmap:96 L16/48000/2\n", &ch,
                                  &sr) && ch == 2 && sr == 48000);

        // RFC 4566 makes the channels field optional (default 1).
        ch = 0; sr = 0;
        check("sdp: omitted channels default to mono",
              parseSdpAudioFormat(base + "a=rtpmap:96 L16/48000\r\n", &ch, &sr) && ch == 1 &&
                  sr == 48000);

        // A video rtpmap before the audio one must not stop the scan.
        ch = 0; sr = 0;
        check("sdp: skips a non-L16 rtpmap",
              parseSdpAudioFormat(base + "a=rtpmap:97 H264/90000\r\n" +
                                      "a=rtpmap:96 L16/48000/8\r\n",
                                  &ch, &sr) && ch == 8 && sr == 48000);

        // Extra whitespace after the payload type is tolerated.
        ch = 0; sr = 0;
        check("sdp: tolerates extra spaces",
              parseSdpAudioFormat(base + "a=rtpmap:96   L16/48000/2\r\n", &ch, &sr) &&
                  ch == 2 && sr == 48000);

        // Malformed values are refused rather than turned into 0-channel streams.
        check("sdp: rejects a non-numeric rate",
              !parseSdpAudioFormat(base + "a=rtpmap:96 L16/abc/2\r\n", &ch, &sr));
        check("sdp: rejects a trailing slash",
              !parseSdpAudioFormat(base + "a=rtpmap:96 L16/48000/\r\n", &ch, &sr));
        check("sdp: rejects empty input", !parseSdpAudioFormat("", &ch, &sr));
        check("sdp: rejects an SDP with no rtpmap", !parseSdpAudioFormat(base, &ch, &sr));

        // A long body (200 lines) must still find the audio rtpmap.
        std::string longSdp = base;
        for (int i = 0; i < 200; ++i) {
            longSdp += "a=fmtp:96 profile-level-id=" + std::to_string(i) + "\r\n";
        }
        longSdp += "a=rtpmap:96 L16/48000/16\r\n";
        ch = 0; sr = 0;
        check("sdp: parses a long body",
              parseSdpAudioFormat(longSdp, &ch, &sr) && ch == 16 && sr == 48000);
    }


    // ---------------- L16 / L24 payload packing ----------------
    {
        const int16_t src[3] = {0x1234, -2, 0x7FFF};

        uint8_t l16[6] = {0};
        check("payload: L16 packs two bytes per sample",
              packPayload(Codec::L16, src, 3, l16, sizeof(l16)) == 6);
        check("payload: L16 is big endian",
              l16[0] == 0x12 && l16[1] == 0x34 && l16[4] == 0x7F && l16[5] == 0xFF);

        uint8_t l24[9] = {0};
        check("payload: L24 packs three bytes per sample",
              packPayload(Codec::L24, src, 3, l24, sizeof(l24)) == 9);
        check("payload: L24 keeps the sample in the top two bytes",
              l24[0] == 0x12 && l24[1] == 0x34 && l24[2] == 0x00);
        check("payload: L24 low byte is zero",
              l24[2] == 0 && l24[5] == 0 && l24[8] == 0);

        // Round trip through both encodings.
        int16_t back16[3] = {0};
        check("payload: L16 round trip",
              unpackPayload(Codec::L16, l16, sizeof(l16), back16, 3) == 3 &&
                  back16[0] == 0x1234 && back16[1] == -2 && back16[2] == 0x7FFF);
        int16_t back24[3] = {0};
        check("payload: L24 round trip",
              unpackPayload(Codec::L24, l24, sizeof(l24), back24, 3) == 3 &&
                  back24[0] == 0x1234 && back24[1] == -2 && back24[2] == 0x7FFF);

        // Malformed payloads are refused, not rounded down into garbage.
        check("payload: L24 rejects a partial sample",
              unpackPayload(Codec::L24, l24, 5, back24, 3) == 0);
        check("payload: L16 rejects a partial sample",
              unpackPayload(Codec::L16, l16, 3, back16, 3) == 0);
        check("payload: pack refuses a short buffer",
              packPayload(Codec::L24, src, 3, l24, 8) == 0);
        check("payload: rejects null buffers",
              packPayload(Codec::L16, nullptr, 3, l16, sizeof(l16)) == 0 &&
                  unpackPayload(Codec::L16, nullptr, 6, back16, 3) == 0);

        // Codec names as they appear in SDP.
        Codec parsed = Codec::L16;
        check("codec: parses L24", parseCodecName("L24", &parsed) && parsed == Codec::L24);
        check("codec: is case insensitive",
              parseCodecName("l24", &parsed) && parsed == Codec::L24);
        check("codec: rejects OPUS", !parseCodecName("OPUS", &parsed));
        check("codec: names round trip", std::string(codecName(Codec::L24)) == "L24");
    }

    {
        // SDP advertises the codec that is actually on the wire.
        StreamConfig l24Cfg;
        l24Cfg.group = "239.69.1.1";
        l24Cfg.codec = Codec::L24;
        l24Cfg.payloadType = 97;
        const std::string sdp24 = buildSdp(l24Cfg, 0x11112222);
        check("sdp: L24 appears in rtpmap",
              sdp24.find("a=rtpmap:97 L24/48000/2") != std::string::npos);

        // The five attributes the reference Ravenna endpoint emits and we used
        // to omit; a parser that does not know them ignores them.
        {
            StreamConfig sc;
            sc.ifaceIp = "192.168.1.10";
            sc.group = "239.69.1.1";
            sc.port = 5004;
            sc.channels = 2;
            const std::string body = buildSdp(sc, 0x01020304);
            check("sdp: carries a source-filter (RFC 4570)",
                  body.find("a=source-filter: incl IN IP4 239.69.1.1 192.168.1.10") !=
                      std::string::npos);
            check("sdp: carries sync-time",
                  body.find("a=sync-time:0") != std::string::npos);
            check("sdp: framecount matches the packet size",
                  body.find("a=framecount:48") != std::string::npos);
            check("sdp: carries a media id",
                  body.find("a=mid:1") != std::string::npos);
            SdpClockRef ptp;
            ptp.kind = SdpClockRef::Kind::Ptp;
            ptp.domain = 0;
            const std::string ptpBody = buildSdp(sc, 1, ptp);
            check("sdp: PTP reference adds clock-domain",
                  ptpBody.find("a=clock-domain:PTPv2 0") != std::string::npos);
        }
        int ch = 0, sr = 0;
        Codec codec = Codec::L16;
        check("sdp: parses L24 back",
              parseSdpAudioFormat(sdp24, &ch, &sr, &codec) && codec == Codec::L24 &&
                  ch == 2 && sr == 48000);

        StreamConfig l16Cfg;
        l16Cfg.group = "239.69.1.1";
        const std::string sdp16 = buildSdp(l16Cfg, 0x11112222);
        codec = Codec::L24;
        check("sdp: L16 still parses as L16",
              parseSdpAudioFormat(sdp16, &ch, &sr, &codec) && codec == Codec::L16);
        check("sdp: payload size follows the codec",
              l24Cfg.payloadBytes() == l24Cfg.framesPerPacket() * l24Cfg.channels * 3 &&
                  l16Cfg.payloadBytes() == l16Cfg.framesPerPacket() * l16Cfg.channels * 2);
    }


    // ---------------- stream parameter validation ----------------
    {
        StreamConfig good;
        good.channels = 2;
        good.sampleRate = 48000;
        good.ptimeMs = 1.0;
        good.port = 5004;
        // The defaults are part of the CLI contract: the help text promises
        // "multicast group (default 239.69.1.1)" and the quick start relies on it.
        // The group field was empty once, so every command without --group failed
        // with "group is not a multicast address" while the help said otherwise.
        {
            const StreamConfig defaults;
            check("config: default group is the Dante media group",
                  defaults.group == "239.69.1.1");
            check("config: default port is 5004", defaults.port == 5004);
            check("config: default is 2 ch at 48 kHz",
                  defaults.channels == 2 && defaults.sampleRate == 48000);
            check("config: default ptime is 1 ms", defaults.ptimeMs == 1.0);
            check("config: default codec is L16", defaults.codec == Codec::L16);
            StreamConfig withIface;
            withIface.ifaceIp = "192.0.2.10";
            check("config: defaults validate once an interface is set",
                  withIface.validate(nullptr).empty());
        }
        check("config: accepts a normal stream", good.validate(nullptr).empty());

        StreamConfig c;
        c = good; c.channels = 0;
        check("config: rejects 0 channels", !c.validate(nullptr).empty());
        c = good; c.channels = 100;
        check("config: rejects 100 channels", !c.validate(nullptr).empty());
        c = good; c.channels = 64;
        check("config: accepts 64 channels", c.validate(nullptr).empty());
        c = good; c.sampleRate = 1000;
        check("config: rejects 1 kHz", !c.validate(nullptr).empty());
        c = good; c.ptimeMs = 0.0;
        check("config: rejects 0 ms ptime", !c.validate(nullptr).empty());
        c = good; c.ptimeMs = 1000.0;
        check("config: rejects a 1 s ptime", !c.validate(nullptr).empty());
        c = good; c.port = 0;
        check("config: rejects port 0", !c.validate(nullptr).empty());

        std::string why;
        c = good; c.channels = 0;
        (void)c.validate(&why);
        check("config: explains the rejection", why.find("channel") != std::string::npos);
    }


    // ---------------- SAP receive parsing ----------------
    {
        StreamConfig sdpCfg;
        sdpCfg.group = "239.69.1.1";
        const std::string sdp = buildSdp(sdpCfg, 0x22334455);
        const std::string oline = "o=- 1 1 IN IP4 10.1.2.3";
        const auto ann = buildSapDatagram(false, sdp, "10.1.2.3", oline, 0xBEEF);

        SapAnnouncement parsed;
        check("sap rx: parses an announcement",
              parseSapAnnouncement(ann.data(), ann.size(), &parsed));
        check("sap rx: origin comes from the header", parsed.origin == "10.1.2.3");
        check("sap rx: not a deletion", !parsed.deletion);
        check("sap rx: body round trips", parsed.sdp == sdp);

        const auto del = buildSapDatagram(true, sdp, "10.1.2.3", oline, 0xBEEF);
        SapAnnouncement parsedDel;
        check("sap rx: parses a deletion",
              parseSapAnnouncement(del.data(), del.size(), &parsedDel));
        check("sap rx: deletion flag set", parsedDel.deletion);
        check("sap rx: deletion body is the origin line", parsedDel.sdp == oline);

        check("sap rx: rejects a short datagram",
              !parseSapAnnouncement(ann.data(), 8, &parsed));
        check("sap rx: rejects a null buffer", !parseSapAnnouncement(nullptr, 100, &parsed));

        // A datagram whose MIME type is not application/sdp is not an announcement.
        std::vector<uint8_t> wrong = ann;
        std::memcpy(wrong.data() + 8, "text/plain        ", 18);
        check("sap rx: rejects a non-SDP payload",
              !parseSapAnnouncement(wrong.data(), wrong.size(), &parsed));
    }


    // ---------------- RTCP (RFC 3550) ----------------
    {
        RtcpSenderReport sr;
        sr.ssrc = 0x11223344;
        sr.ntp = 0xE001020304050607ULL;
        sr.rtpTimestamp = 0x55667788;
        sr.packetCount = 1234;
        sr.octetCount = 567890;
        const std::string pkt = buildRtcpSenderReport(sr, "user@host");

        check("rtcp: SR is a whole number of 32-bit words", pkt.size() % 4 == 0);
        check("rtcp: SR first byte is V=2, RC=0",
              static_cast<uint8_t>(pkt[0]) == 0x80);
        check("rtcp: SR payload type is 200", static_cast<uint8_t>(pkt[1]) == 200);

        RtcpSenderReport back;
        check("rtcp: SR parses",
              parseRtcpSenderReport(reinterpret_cast<const uint8_t *>(pkt.data()),
                                    pkt.size(), &back));
        check("rtcp: SR keeps the SSRC", back.ssrc == sr.ssrc);
        check("rtcp: SR keeps the NTP timestamp", back.ntp == sr.ntp);
        check("rtcp: SR keeps the RTP timestamp", back.rtpTimestamp == sr.rtpTimestamp);
        check("rtcp: SR keeps the packet count", back.packetCount == sr.packetCount);
        check("rtcp: SR keeps the octet count", back.octetCount == sr.octetCount);

        std::string cname;
        check("rtcp: CNAME follows the SR",
              parseRtcpCname(reinterpret_cast<const uint8_t *>(pkt.data()), pkt.size(),
                             &cname) && cname == "user@host");

        // A receiver report carries loss and jitter for one source.
        RtcpReceptionStats rs;
        rs.ssrc = 0xAABBCCDD;
        rs.fractionLost = 51;          // 20 %
        rs.cumulativeLost = 42;
        rs.highestSequence = 70000;
        rs.jitter = 96;
        const std::string rr = buildRtcpReceiverReport(0x01020304, &rs, "recv@host");
        RtcpReceptionStats rrBack;
        uint32_t reporter = 0;
        check("rtcp: RR parses",
              parseRtcpReceiverReport(reinterpret_cast<const uint8_t *>(rr.data()),
                                      rr.size(), &rrBack, &reporter));
        check("rtcp: RR keeps the reporter SSRC", reporter == 0x01020304);
        check("rtcp: RR keeps fraction lost", rrBack.fractionLost == 51);
        check("rtcp: RR keeps cumulative lost", rrBack.cumulativeLost == 42);
        check("rtcp: RR keeps highest sequence", rrBack.highestSequence == 70000);
        check("rtcp: RR keeps jitter", rrBack.jitter == 96);

        // An RR without a block is legal but carries no per-source statistics.
        const std::string empty = buildRtcpReceiverReport(0x01020304, nullptr, "recv@host");
        RtcpReceptionStats none;
        check("rtcp: empty RR is rejected by the block parser",
              !parseRtcpReceiverReport(reinterpret_cast<const uint8_t *>(empty.data()),
                                       empty.size(), &none, nullptr));
        check("rtcp: empty RR still carries a CNAME",
              parseRtcpCname(reinterpret_cast<const uint8_t *>(empty.data()), empty.size(),
                             &cname) && cname == "recv@host");

        // Malformed input must be refused, not read out of bounds.
        check("rtcp: rejects an SR shorter than 28 bytes",
              !parseRtcpSenderReport(reinterpret_cast<const uint8_t *>(pkt.data()), 20, &back));
        check("rtcp: rejects a null buffer", !parseRtcpSenderReport(nullptr, 100, &back));
        std::string wrongVersion = pkt;
        wrongVersion[0] = static_cast<char>(0x40);   // version 1
        check("rtcp: rejects version 1",
              !parseRtcpSenderReport(reinterpret_cast<const uint8_t *>(wrongVersion.data()),
                                     wrongVersion.size(), &back));
        std::string notSdes = pkt;
        // The SDES packet starts right after the 28-byte SR: relabel it so the
        // CNAME walk has no SDES left to find.
        notSdes[28 + 1] = static_cast<char>(200);
        check("rtcp: CNAME parser ignores a non-SDES packet",
              !parseRtcpCname(reinterpret_cast<const uint8_t *>(notSdes.data()),
                              notSdes.size(), &cname));

        // Cumulative loss is signed on the wire: a sender that receives more
        // than it expected reports a negative count (duplicates).
        RtcpReceptionStats neg;
        neg.ssrc = 0x1234;
        neg.cumulativeLost = -7;
        const std::string negPkt = buildRtcpReceiverReport(1, &neg, "x@y");
        RtcpReceptionStats negBack;
        check("rtcp: negative cumulative loss survives the wire format",
              parseRtcpReceiverReport(reinterpret_cast<const uint8_t *>(negPkt.data()),
                                      negPkt.size(), &negBack, nullptr) &&
                  negBack.cumulativeLost == -7);
    }


    // ---------------- SAP stream discovery ----------------
    {
        StreamConfig cfg;
        cfg.ifaceIp = "192.168.1.10";
        cfg.group = "239.69.4.4";
        cfg.port = 5010;
        cfg.channels = 8;
        cfg.sampleRate = 48000;
        cfg.codec = Codec::L24;
        const std::string sdp = buildSdp(cfg, 0x0A0B0C0D);

        SapStreamInfo info;
        check("discovery: parses our own SDP", parseSdpStreamInfo(sdp, &info));
        check("discovery: name comes from s=", info.name == "OpenAES67 stream");
        check("discovery: group comes from c=", info.group == "239.69.4.4");
        check("discovery: port comes from m=", info.port == 5010);
        check("discovery: channels come from rtpmap", info.channels == 8);
        check("discovery: codec comes from rtpmap", info.codec == Codec::L24);
        check("discovery: marked usable", info.usable);

        // c= without /ttl, LF-only line endings, extra spaces.
        const std::string sparse =
            "v=0\no=- 1 1 IN IP4 10.0.0.9\ns=Third Party\nc=IN IP4 239.69.8.8\n"
            "m=audio 5030 RTP/AVP 96\na=rtpmap:96 L16/44100/2\n";
        SapStreamInfo sparseInfo;
        check("discovery: accepts c= without a TTL suffix",
              parseSdpStreamInfo(sparse, &sparseInfo));
        check("discovery: reads a bare group", sparseInfo.group == "239.69.8.8");
        check("discovery: reads 44.1 kHz stereo",
              sparseInfo.sampleRate == 44100 && sparseInfo.channels == 2);
        check("discovery: defaults to L16", sparseInfo.codec == Codec::L16);

        // Unusable descriptions are refused instead of shown as tunable.
        SapStreamInfo bad;
        check("discovery: rejects an SDP without c=",
              !parseSdpStreamInfo("s=x\r\nm=audio 5004 RTP/AVP 96\r\n"
                                  "a=rtpmap:96 L16/48000/2\r\n", &bad));
        check("discovery: rejects an SDP without rtpmap",
              !parseSdpStreamInfo("s=x\r\nc=IN IP4 239.1.1.1\r\nm=audio 5004 RTP/AVP 96\r\n",
                                  &bad));
        check("discovery: rejects an empty body", !parseSdpStreamInfo("", &bad));
        check("discovery: rejects a null output", !parseSdpStreamInfo(sdp, nullptr));

        // A real Dante/AES67 announcement carries attributes we do not generate
        // ourselves (group:DUP, source-filter, sync-time, clock-domain, mid). The
        // parser must ignore the ones it does not use and still get group, port,
        // channels, rate and codec right -- otherwise discovery works only against
        // our own sender.
        {
            const std::string danteStyle =
                "v=0\r\n"
                "o=- 2831159553 317021570 IN IP4 192.168.1.10\r\n"
                "s=Daemon a8c01101 ALSA Source 0\r\n"
                "t=0 0\r\n"
                "a=group:DUP 1 2\r\n"
                "m=audio 5004 RTP/AVP 98\r\n"
                "c=IN IP4 239.1.0.1/15\r\n"
                "a=source-filter: incl IN IP4 239.1.0.1 192.168.1.10\r\n"
                "a=rtpmap:98 L24/48000/8\r\n"
                "a=sync-time:0\r\n"
                "a=framecount:48\r\n"
                "a=ptime:1\r\n"
                "a=mediaclk:direct=0\r\n"
                "a=clock-domain:PTPv2 0\r\n"
                "a=ts-refclk:ptp=IEEE1588-2008:00-1D-C1-FF-FE-50-36-33:0\r\n"
                "a=recvonly\r\n"
                "a=mid:1\r\n";
            SapStreamInfo dante;
            check("discovery: parses a Dante-style SDP",
                  parseSdpStreamInfo(danteStyle, &dante));
            check("discovery: Dante group and port",
                  dante.group == "239.1.0.1" && dante.port == 5004);
            check("discovery: Dante channels, rate and codec",
                  dante.channels == 8 && dante.sampleRate == 48000 &&
                      dante.codec == Codec::L24);
            check("discovery: Dante stream name",
                  dante.name == "Daemon a8c01101 ALSA Source 0");

        // Direction decides whether a discovered session is something to subscribe
        // to. Our own sender used to announce recvonly, which mislabeled the
        // outgoing stream in this very list.
        {
            StreamConfig cfg;
            cfg.ifaceIp = "192.168.1.10";
            cfg.group = "239.69.1.2";
            cfg.port = 5006;
            cfg.channels = 4;
            const std::string sent = buildSdp(cfg, 1);
            check("sdp: a sender announces sendonly",
                  sent.find("a=sendonly") != std::string::npos &&
                      sent.find("a=recvonly") == std::string::npos);
            SapStreamInfo parsedSent;
            parseSdpStreamInfo(sent, &parsedSent);
            check("sdp: sendonly parses as a source", !parsedSent.recvOnly);

            const std::string received =
                buildSdp(cfg, 1, SdpClockRef{}, 1, SdpDirection::RecvOnly);
            check("sdp: a receiver announces recvonly",
                  received.find("a=recvonly") != std::string::npos);
            SapStreamInfo parsedReceived;
            parseSdpStreamInfo(received, &parsedReceived);
            check("sdp: recvonly parses as a subscription", parsedReceived.recvOnly);

        // SDP is a CRLF protocol. Writing it with printf on Windows turns every
        // "\n" into "\r\n", so a body that already used CRLF came out as "\r\r\n"
        // and a redirected file was rejected by strict parsers. Pin the bytes.
        {
            StreamConfig cfg;
            cfg.ifaceIp = "192.168.1.10";
            cfg.group = "239.69.1.2";
            cfg.port = 5006;
            cfg.channels = 4;
            const std::string sdp = buildSdp(cfg, 1);
            check("sdp: no doubled carriage return",
                  sdp.find("\r\r") == std::string::npos);
            check("sdp: no blank lines", sdp.find("\r\n\r\n") == std::string::npos);
            check("sdp: ends with exactly one CRLF",
                  sdp.size() >= 2 && sdp.substr(sdp.size() - 2) == "\r\n" &&
                      (sdp.size() < 4 || sdp.substr(sdp.size() - 4) != "\r\n\r\n"));
            bool everyLineTyped = true;
            std::size_t pos = 0;
            while (pos < sdp.size()) {
                const std::size_t eol = sdp.find("\r\n", pos);
                const std::string line =
                    sdp.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
                if (line.size() < 2 || line[1] != 61) {
                    everyLineTyped = false;
                }
                if (eol == std::string::npos) {
                    break;
                }
                pos = eol + 2;
            }
            check("sdp: every line is <type>=<value>", everyLineTyped);
        }

        }


        // Text files written by Windows tools are not always UTF-8: PowerShell
        // redirection produces UTF-16LE, and that made a valid SDP unreadable.
        {
            const std::string plain = "v=0\r\ns=test\r\n";
            check("decode: UTF-8 passes through",
                  decodeTextToUtf8(plain) == plain);
            const std::string utf8Bom = std::string("\xEF\xBB\xBF") + plain;
            check("decode: UTF-8 BOM is stripped", decodeTextToUtf8(utf8Bom) == plain);
            std::string utf16;
            utf16.push_back(static_cast<char>(0xFF));
            utf16.push_back(static_cast<char>(0xFE));
            for (char ch : plain) {
                utf16.push_back(ch);
                utf16.push_back(0);
            }
            check("decode: UTF-16LE is converted", decodeTextToUtf8(utf16) == plain);
            std::string utf16be;
            utf16be.push_back(static_cast<char>(0xFE));
            utf16be.push_back(static_cast<char>(0xFF));
            for (char ch : plain) {
                utf16be.push_back(0);
                utf16be.push_back(ch);
            }
            check("decode: UTF-16BE is converted", decodeTextToUtf8(utf16be) == plain);
        }

        }

    }


    // ---------------- SAP stream info: the usable flag means what it says ----------
    {
        const std::string good =
            "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=Test\r\n"
            "c=IN IP4 239.69.1.1/32\r\nt=0 0\r\n"
            "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L16/48000/8\r\n";
        SapStreamInfo info;
        check("sap-info: a well formed announcement is usable",
              parseSdpStreamInfo(good, &info) && info.usable && info.group == "239.69.1.1" &&
                  info.port == 5004 && info.channels == 8 && info.sampleRate == 48000);

        const std::string unicast =
            "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=Test\r\n"
            "c=IN IP4 10.0.0.1/32\r\nt=0 0\r\n"
            "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L16/48000/8\r\n";
        SapStreamInfo u;
        check("sap-info: a unicast address is not usable",
              !parseSdpStreamInfo(unicast, &u) && !u.usable);

        const std::string insane =
            "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=Test\r\n"
            "c=IN IP4 239.69.1.1/32\r\nt=0 0\r\n"
            "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L16/48000/99999\r\n";
        SapStreamInfo bad;
        check("sap-info: an impossible channel count is not usable",
              !parseSdpStreamInfo(insane, &bad) && !bad.usable);

        const std::string recvOnly =
            "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=Test\r\n"
            "c=IN IP4 239.69.1.1/32\r\nt=0 0\r\n"
            "m=audio 5004 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 L16/48000/2\r\n";
        SapStreamInfo r;
        check("sap-info: recvonly is flagged", parseSdpStreamInfo(recvOnly, &r) && r.recvOnly);
    }
    // ---------------- DSCP marking ----------------
    // AES67 §12 expects audio at EF (46) and PTP at CS6 (48). A setsockopt that
    // silently does nothing looks exactly like working QoS until a congested switch
    // disagrees, so the platform is asked to hand the value back.
    {
        std::string detail;
        const bool ok = verifyDscpSupport(&detail);
        check("net: dscp marking survives a round trip", ok);
        std::printf("    [INFO] dscp: %s\n", detail.c_str());
    }
    // ---------------- malformed RTCP and PTP ----------------
    // Both are network input. The parsers already bounds-check (there are comments in
    // ptp.cpp about a past read past the end), so what these assertions add is
    // regression pressure: the invariant that "accepted" implies "the required bytes
    // were present" must survive future edits.
    {
        auto nextRandom = [](uint32_t &state) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };

        // --- RTCP sender report ---
        {
            RtcpSenderReport sr;
            sr.ssrc = 0xC0FFEE;
            sr.ntp = 0x1234567890ABCDEFull;
            sr.rtpTimestamp = 0x11223344;
            sr.packetCount = 999;
            sr.octetCount = 123456;
            const std::string good = buildRtcpSenderReport(sr, "openaes67@192.0.2.10");
            check("rtcp-fuzz: built a sender report", good.size() >= 28);

            uint32_t state = 0x1234567u;
            int accepted = 0, tooShort = 0;
            for (int i = 0; i < 3000; ++i) {
                std::vector<uint8_t> mutated(good.begin(), good.end());
                const int edits = 1 + static_cast<int>(nextRandom(state) % 4);
                for (int e = 0; e < edits; ++e) {
                    mutated[nextRandom(state) % mutated.size()] =
                        static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t len = nextRandom(state) % (mutated.size() + 3);
                RtcpSenderReport out;
                if (parseRtcpSenderReport(mutated.data(), len, &out)) {
                    ++accepted;
                    // A parsed SR is 28 bytes plus one 24-byte block at most.
                    const size_t blocks = static_cast<size_t>(out.blockCount);
                    if (len < 28 + blocks * 24) { ++tooShort; }
                }
            }
            check("rtcp-fuzz: a sender report is never parsed below its length", tooShort == 0);
            check("rtcp-fuzz: senders reports still parse sometimes", accepted > 0);
            std::printf("    [INFO] rtcp SR mutations: %d accepted, %d too short\n", accepted,
                        tooShort);
        }

        // --- RTCP receiver report ---
        {
            RtcpReceptionStats stats;
            stats.ssrc = 0xC0FFEE;
            stats.fractionLost = 12;
            stats.cumulativeLost = -3;
            stats.highestSequence = 4000;
            stats.jitter = 37;
            const std::string good = buildRtcpReceiverReport(0xABCDEF, &stats, "r@192.0.2.10");
            uint32_t state = 0x7654321u;
            int accepted = 0, tooShort = 0;
            for (int i = 0; i < 3000; ++i) {
                std::vector<uint8_t> mutated(good.begin(), good.end());
                const int edits = 1 + static_cast<int>(nextRandom(state) % 4);
                for (int e = 0; e < edits; ++e) {
                    mutated[nextRandom(state) % mutated.size()] =
                        static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t len = nextRandom(state) % (mutated.size() + 3);
                RtcpReceptionStats out;
                uint32_t reporter = 0;
                if (parseRtcpReceiverReport(mutated.data(), len, &out, &reporter)) {
                    ++accepted;
                    if (len < 8) { ++tooShort; }
                }
            }
            check("rtcp-fuzz: a receiver report is never parsed below 8 bytes", tooShort == 0);
            check("rtcp-fuzz: receiver reports still parse sometimes", accepted > 0);
            std::printf("    [INFO] rtcp RR mutations: %d accepted, %d too short\n", accepted,
                        tooShort);
        }

        // --- RTCP SDES CNAME ---
        {
            RtcpSenderReport sr;
            sr.ssrc = 1;
            const std::string good = buildRtcpSenderReport(sr, "openaes67@192.0.2.10");
            uint32_t state = 0x2468aceu;
            int accepted = 0, impossible = 0;
            for (int i = 0; i < 3000; ++i) {
                std::vector<uint8_t> mutated(good.begin(), good.end());
                const int edits = 1 + static_cast<int>(nextRandom(state) % 5);
                for (int e = 0; e < edits; ++e) {
                    mutated[nextRandom(state) % mutated.size()] =
                        static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t len = nextRandom(state) % (mutated.size() + 3);
                std::string cname;
                if (parseRtcpCname(mutated.data(), len, &cname)) {
                    ++accepted;
                    if (cname.size() > len) { ++impossible; }
                }
            }
            check("rtcp-fuzz: cname is never longer than the datagram", impossible == 0);
            std::printf("    [INFO] rtcp SDES mutations: %d accepted, %d impossible\n", accepted,
                        impossible);
        }

        // --- PTP ---
        {
            uint8_t clockId[8] = {0x00, 0x0E, 0xC6, 0xFF, 0xFE, 0x37, 0x38, 0x07};
            uint8_t packet[64] = {0};
            const size_t goodLen = buildDelayReq(packet, sizeof(packet), 0, clockId, 7);
            check("ptp-fuzz: built a delay request", goodLen >= kPtpHeaderSize);

            uint32_t state = 0xfeedfaceu;
            int accepted = 0, tooShort = 0, wrongVersion = 0;
            for (int i = 0; i < 4000; ++i) {
                uint8_t mutated[64];
                std::memcpy(mutated, packet, goodLen);
                const int edits = 1 + static_cast<int>(nextRandom(state) % 4);
                for (int e = 0; e < edits; ++e) {
                    mutated[nextRandom(state) % goodLen] =
                        static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t len = nextRandom(state) % (goodLen + 3);
                PtpHeader header;
                if (parsePtpHeader(mutated, len, &header)) {
                    ++accepted;
                    if (len < kPtpHeaderSize) { ++tooShort; }
                    if (header.versionPtp != 2) { ++wrongVersion; }
                }
            }
            check("ptp-fuzz: a header is never parsed below 34 bytes", tooShort == 0);
            check("ptp-fuzz: version 2 is required", wrongVersion == 0);
            check("ptp-fuzz: delay requests still parse sometimes", accepted > 0);
            std::printf("    [INFO] ptp mutations: %d accepted, %d too short, %d wrong version\n",
                        accepted, tooShort, wrongVersion);
        }
    }

    // ---------------- malformed WAV input ----------------
    // --wav accepts a file the user chose, so the reader is exposed to arbitrary
    // bytes with a RIFF header. Whatever it accepts must stay bounded by the file:
    // a declared size larger than the file, or a channel count of 99999, must not
    // become an allocation.
    {
        const std::string goodPath = tempTestPath("selftest-wavsrc.wav");
        std::vector<int16_t> tone(4800);
        for (size_t i = 0; i < tone.size(); ++i) {
            tone[i] = static_cast<int16_t>((i * 37) % 2000 - 1000);
        }
        check("wav-fuzz: prepared a source file",
              writeWavPcm16(goodPath.c_str(), tone.data(), tone.size(), 2, 48000));

        std::vector<uint8_t> bytes;
        {
            std::FILE *f = openFileUtf8(goodPath.c_str(), "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                bytes.resize(static_cast<size_t>(size));
                if (size > 0) {
                    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
                    bytes.resize(got);
                }
                std::fclose(f);
            }
        }
        check("wav-fuzz: read the source bytes back", bytes.size() > 64);

        auto nextRandom = [](uint32_t &state) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };

        const std::string scratchPath = tempTestPath("selftest-wavmut.wav");
        uint32_t state = 0x9e3779b9u;
        int accepted = 0, unbounded = 0;
        for (int i = 0; i < 400; ++i) {
            std::vector<uint8_t> mutated = bytes;
            const int edits = 1 + static_cast<int>(nextRandom(state) % 8);
            for (int e = 0; e < edits; ++e) {
                // Bias the mutations into the 44-byte header: that is where the
                // sizes and counts live, and where a hostile file does its damage.
                const size_t at = (nextRandom(state) % 3 == 0)
                                      ? nextRandom(state) % mutated.size()
                                      : nextRandom(state) % 44;
                mutated[at] = static_cast<uint8_t>(nextRandom(state) & 0xFF);
            }
            {
                std::FILE *f = openFileUtf8(scratchPath.c_str(), "wb");
                if (!f) { continue; }
                std::fwrite(mutated.data(), 1, mutated.size(), f);
                std::fclose(f);
            }
            WavData out;
            std::string err;
            if (readWavPcm(scratchPath, &out, &err)) {
                ++accepted;
                const bool sane = out.channels >= 1 && out.channels <= 64 &&
                                  out.sampleRate >= 8000 && out.sampleRate <= 384000 &&
                                  (out.sourceBits == 16 || out.sourceBits == 24) &&
                                  out.samples.size() <= mutated.size();
                if (!sane) { ++unbounded; }
            }
        }
        check("wav-fuzz: accepted files stay bounded by the file size", unbounded == 0);
        std::printf("    [INFO] wav mutations: %d accepted of 400, %d unbounded\n",
                    accepted, unbounded);

        // Truncation is the common real case: a download that stopped early.
        int truncations = 0, truncationOverflow = 0;
        for (size_t cut = 0; cut < bytes.size(); cut += 7) {
            std::vector<uint8_t> prefix(bytes.begin(), bytes.begin() + cut);
            std::FILE *f = openFileUtf8(scratchPath.c_str(), "wb");
            if (!f) { continue; }
            std::fwrite(prefix.data(), 1, prefix.size(), f);
            std::fclose(f);
            WavData out;
            std::string err;
            ++truncations;
            if (readWavPcm(scratchPath, &out, &err)) {
                if (out.samples.size() > prefix.size()) { ++truncationOverflow; }
            }
        }
        check("wav-fuzz: a truncated file never yields more samples than it holds",
              truncationOverflow == 0);
        std::printf("    [INFO] wav truncations: %d tried, %d overflow\n", truncations,
                    truncationOverflow);

        std::remove(goodPath.c_str());
        std::remove(scratchPath.c_str());
    }
    // ---------------- 24-bit WAV round trip ----------------
    {
        const std::string path = tempTestPath("selftest-24.wav");
        const int16_t src[4] = {0x1234, -2, 0x7FFF, -32768};
        check("wav24: writes a file", writeWavPcm24(path.c_str(), src, 4, 2, 48000));

        WavData back;
        std::string err;
        check("wav24: reads back", readWavPcm(path, &back, &err));
        check("wav24: reports 24-bit source", back.sourceBits == 24);
        check("wav24: keeps channels and rate",
              back.channels == 2 && back.sampleRate == 48000);
        check("wav24: samples survive the round trip",
              back.samples.size() == 4 && back.samples[0] == 0x1234 &&
                  back.samples[1] == -2 && back.samples[2] == 0x7FFF &&
                  back.samples[3] == -32768);

        // The 16-bit reader must refuse a 24-bit file rather than misread it.
        WavData strict;
        check("wav24: strict 16-bit reader refuses 24-bit",
              !readWavPcm16(path, &strict, &err));
        std::remove(path.c_str());
    }


    // ---------------- non-ASCII file paths ----------------
    {
        // fopen() takes an ANSI path; openFileUtf8() must handle a Chinese name.
        const std::string path = tempTestPath("selftest-测试.wav");
        std::FILE *w = openFileUtf8(path.c_str(), "wb");
        check("file: opens a UTF-8 path for writing", w != nullptr);
        if (w) {
            const char payload[] = "ok";
            std::fwrite(payload, 1, 2, w);
            std::fclose(w);
        }
        std::FILE *r = openFileUtf8(path.c_str(), "rb");
        check("file: reopens a UTF-8 path for reading", r != nullptr);
        if (r) {
            char buf[4] = {0};
            const size_t got = std::fread(buf, 1, 2, r);
            std::fclose(r);
            check("file: UTF-8 path round trip", got == 2 && buf[0] == 'o' && buf[1] == 'k');
        }
        std::remove(path.c_str());
    }


    // ---------------- channel conversion (stream -> device) ----------------
    {
        // 8 channels -> 2: keep the first two, drop the rest.
        const int16_t eight[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        int16_t two[4] = {0};
        check("convert: 8->2 converts frames",
              convertChannels(eight, 2, 8, 2, two) == 2);
        check("convert: 8->2 keeps the first two channels",
              two[0] == 1 && two[1] == 2 && two[2] == 9 && two[3] == 10);

        // mono -> 2: duplicate across the device channels.
        const int16_t mono[2] = {100, 200};
        int16_t stereo[4] = {0};
        check("convert: 1->2 converts frames",
              convertChannels(mono, 2, 1, 2, stereo) == 2);
        check("convert: 1->2 duplicates the mono channel",
              stereo[0] == 100 && stereo[1] == 100 && stereo[2] == 200 && stereo[3] == 200);

        // 2 -> 1: first channel only.
        int16_t down[2] = {0};
        convertChannels(stereo, 2, 2, 1, down);
        check("convert: 2->1 takes the first channel", down[0] == 100 && down[1] == 200);

        // Same count: straight copy.
        int16_t copy[4] = {0};
        convertChannels(stereo, 2, 2, 2, copy);
        check("convert: equal counts copy verbatim",
              copy[0] == 100 && copy[1] == 100 && copy[3] == 200);

        // Degenerate inputs are refused rather than crashing.
        check("convert: rejects zero frames",
              convertChannels(eight, 0, 8, 2, two) == 0);
        check("convert: rejects a null buffer",
              convertChannels(nullptr, 2, 8, 2, two) == 0);
    }


    // ---------------- PTP-derived RTP timestamps ----------------
    {
        PtpMediaClock clock;
        const int rate = 48000;
        const int fpp = 48;
        // First packet anchors to the grandmaster.
        const uint32_t t0 = clock.update(1000000000LL, rate, 0, fpp);
        check("ptp clock: anchors on the first packet", t0 == 48000);
        check("ptp clock: reports anchored", clock.anchored());

        // Clock advances exactly one packet: extrapolation matches, no re-anchor.
        const uint32_t t1 = clock.update(1001000000LL, rate, 1, fpp);
        check("ptp clock: extrapolates one packet", t1 == 48048);
        check("ptp clock: no re-anchor when in step", clock.reanchors() == 0);

        // Clock frozen (published value not updated): extrapolate, do not follow.
        const uint32_t t2 = clock.update(1001000000LL, rate, 2, fpp);
        check("ptp clock: ignores a stale clock value", t2 == 48096);
        check("ptp clock: stale value is not a re-anchor", clock.reanchors() == 0);

        // Clock steps by a second: re-anchor to it.
        const uint32_t t3 = clock.update(1001000000LL + 1000000000LL, rate, 3, fpp);
        const uint32_t expect = static_cast<uint32_t>(
            ((1001000000ULL + 1000000000ULL) * 48000ULL) / 1000000000ULL);
        check("ptp clock: re-anchors after a clock step", t3 == expect);
        check("ptp clock: re-anchor counted", clock.reanchors() == 1);

        // Realistic Unix-epoch input: 1.792e18 ns overflows a naive ns*rate/1e9,
        // which is exactly how the RTP timestamp ended up ~10.7 hours off.
        {
            PtpMediaClock epochClock;
            const int64_t unixNs = 1792348921LL * 1000000000LL + 484132000LL;
            const uint32_t got = epochClock.update(unixNs, 48000, 0, 48);
            const uint64_t want = (static_cast<uint64_t>(1792348921LL) * 48000ULL +
                                   (484132000ULL * 48000ULL) / 1000000000ULL) &
                                  0xFFFFFFFFULL;
            check("ptp clock: Unix-epoch input does not overflow",
                  got == static_cast<uint32_t>(want));
        }
        // Sequence stays continuous across the re-anchor.
        const uint32_t t4 = clock.update(1001000000LL + 1000000000LL, rate, 4, fpp);
        check("ptp clock: continues after re-anchor",
              static_cast<uint32_t>(t4 - t3) == static_cast<uint32_t>(fpp));
    }


    // ---------------- multicast address validation ----------------
    {
        check("net: 239.69.1.1 is multicast", isMulticastIPv4("239.69.1.1"));
        check("net: 224.0.0.1 is multicast", isMulticastIPv4("224.0.0.1"));
        check("net: 239.255.255.255 is multicast", isMulticastIPv4("239.255.255.255"));
        check("net: 223.255.255.255 is not multicast", !isMulticastIPv4("223.255.255.255"));
        check("net: 240.0.0.1 is not multicast", !isMulticastIPv4("240.0.0.1"));
        check("net: 192.0.2.50 is not multicast", !isMulticastIPv4("192.0.2.50"));
        check("net: garbage is not multicast", !isMulticastIPv4("not-an-ip"));
        check("net: empty is not multicast", !isMulticastIPv4(""));
    }

        }
    // ---------------- PTP header parsing and truncated packets ----------------
    {
        // Header fields, laid out per IEEE 1588-2008 figure 6.
        uint8_t pkt[44] = {0};
        pkt[0] = 0x00;                       // Sync, transportSpecific 0
        pkt[1] = 0x02;                       // versionPtp = 2
        pkt[2] = 0x00; pkt[3] = 44;          // messageLength
        pkt[4] = 7;                          // domain
        pkt[6] = 0x00; pkt[7] = 0x02;        // flags: twoStep (big-endian)
        for (int i = 0; i < 8; ++i) { pkt[8 + i] = 0xFF; }   // correctionField = -1
        const uint8_t id[8] = {0x00, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
        std::memcpy(pkt + 20, id, 8);
        pkt[28] = 0x00; pkt[29] = 0x01;      // portNumber
        pkt[30] = 0x12; pkt[31] = 0x34;      // sequenceId
        pkt[32] = 0x00;                      // controlField
        pkt[33] = 0x7F;                      // logMessageInterval

        PtpHeader h;
        check("ptp: header parses", parsePtpHeader(pkt, sizeof(pkt), &h));
        check("ptp: message type is Sync", h.messageType == 0x00);
        check("ptp: version is 2", h.versionPtp == 2);
        check("ptp: message length", h.messageLength == 44);
        check("ptp: domain", h.domain == 7);
        check("ptp: two-step flag", (h.flags & 0x0002) != 0);
        check("ptp: correctionField is signed", h.correctionField == -1);
        check("ptp: clock identity", std::memcmp(h.clockIdentity, id, 8) == 0);
        check("ptp: port number", h.portNumber == 1);
        check("ptp: sequence id", h.sequenceId == 0x1234);
        check("ptp: log interval", h.logMessageInterval == 0x7F);

        check("ptp: rejects short header", !parsePtpHeader(pkt, 33, &h));
        uint8_t badVersion[44] = {0};
        badVersion[1] = 0x01;               // versionPtp = 1
        check("ptp: rejects version 1", !parsePtpHeader(badVersion, sizeof(badVersion), &h));

        // A 34-byte Sync has the header but no timestamp body. The handler used to
        // read 10 bytes past the buffer; it must now refuse the message and say so.
        PtpSlave slave;
        uint8_t shortSync[34];
        std::memcpy(shortSync, pkt, 34);
        shortSync[4] = 0;                   // domain 0 (the default)
        shortSync[6] = 0; shortSync[7] = 0; // one-step: timestamp body expected
        shortSync[0] = 0x00;                // Sync
        slave.handleMessage(shortSync, 34, 1000000);
        check("ptp: truncated Sync refused", slave.stats().truncatedCount == 1);
        check("ptp: truncated Sync still counted as Sync", slave.stats().syncCount == 1);

        uint8_t fullSync[44];
        std::memcpy(fullSync, shortSync, 34);
        std::memset(fullSync + 34, 0, 10);  // t1 = 0
        check("ptp: full Sync accepted", parsePtpHeader(fullSync, 44, &h));
        slave.handleMessage(fullSync, 44, 1000000);
        check("ptp: full Sync does not count as truncated",
              slave.stats().truncatedCount == 1);

        // A Follow_Up is only examined when a two-step Sync is pending, so set that
        // up first: otherwise the handler ignores it before any length check.
        uint8_t twoStepSync[34];
        std::memcpy(twoStepSync, shortSync, 34);
        twoStepSync[6] = 0x00;
        twoStepSync[7] = 0x02;              // flags: twoStep (big-endian)
        twoStepSync[0] = 0x00;              // Sync
        slave.handleMessage(twoStepSync, 34, 1000000);

        uint8_t shortFollowUp[34];
        std::memcpy(shortFollowUp, twoStepSync, 34);
        shortFollowUp[0] = 0x80;            // Follow_Up
        slave.handleMessage(shortFollowUp, 34, 1000000);
        check("ptp: truncated Follow_Up refused", slave.stats().truncatedCount == 2);
        // PTP timestamp wire format (48-bit seconds + 32-bit nanoseconds).
        {
            uint8_t raw[10] = {0};
            PtpTimestamp src;
            src.seconds = 0x0000123456789ABCULL;
            src.nanos = 999999999;
            writePtpTimestamp(raw, src);
            const PtpTimestamp back = readPtpTimestamp(raw);
            check("ptp: timestamp wire round trip",
                  back.seconds == src.seconds && back.nanos == src.nanos);
        }

        // Timestamp helpers.
        PtpTimestamp ts;
        ts.seconds = 3;
        ts.nanos = 250000000;
        check("ptp: timestamp to nanos", ts.toNanos() == 3250000000LL);
        const PtpTimestamp back = PtpTimestamp::fromNanos(3250000000LL);
        check("ptp: timestamp round trip", back.seconds == 3 && back.nanos == 250000000);
    }

    }
    // ---------------- shared audio ring: concurrent producer/consumer -------
    // The single-threaded cases above prove the arithmetic; only a real pair of
    // threads proves the release/acquire fences. This is the pattern the kernel
    // driver uses, so a mistake here shows up as clicks in the field.
    {
        SharedAudioConfig scfg;
        scfg.channels = 2;
        scfg.sampleRate = 48000;
        scfg.capacityFrames = 480;          // 10 ms, small enough to force contention
        const size_t bytes = openaes67_mapping_size(scfg.capacityFrames,
                                                    scfg.channels * 2);
        std::vector<uint8_t> mem(bytes, 0);
        SharedAudioRing ring(mem.data(), bytes, scfg, true);
        check("ring: concurrent setup valid", ring.valid());

        const int totalFrames = 240000;     // 5 s at 48 kHz
        std::atomic<int> written{0};
        std::atomic<int> readBack{0};
        std::atomic<bool> mismatch{false};
        std::atomic<bool> done{false};

        std::thread producer([&] {
            std::vector<int16_t> buf(static_cast<size_t>(scfg.channels) * 32);
            int seq = 0;
            while (seq < totalFrames) {
                const int want = std::min(32, totalFrames - seq);
                for (int i = 0; i < want; ++i) {
                    const int16_t v = static_cast<int16_t>((seq + i) & 0x7FFF);
                    for (int c = 0; c < scfg.channels; ++c) {
                        buf[static_cast<size_t>(i) * scfg.channels + c] = v;
                    }
                }
                const int n = ring.writeRender(buf.data(), want);
                if (n > 0) {
                    seq += n;
                } else {
                    std::this_thread::yield();
                }
            }
            written.store(seq);
            done.store(true);
        });

        std::thread consumer([&] {
            std::vector<int16_t> buf(static_cast<size_t>(scfg.channels) * 32);
            int expect = 0;
            int spins = 0;
            while (expect < totalFrames) {
                const int n = ring.readRender(buf.data(), 32);
                if (n == 0) {
                    if (done.load() && ring.renderPending() == 0) {
                        break;
                    }
                    if (++spins > 2000000) {
                        break;   // never wedge the suite on a logic error
                    }
                    std::this_thread::yield();
                    continue;
                }
                spins = 0;
                for (int i = 0; i < n; ++i) {
                    const int16_t want = static_cast<int16_t>((expect + i) & 0x7FFF);
                    for (int c = 0; c < scfg.channels; ++c) {
                        if (buf[static_cast<size_t>(i) * scfg.channels + c] != want) {
                            mismatch.store(true);
                        }
                    }
                }
                expect += n;
            }
            readBack.store(expect);
        });

        producer.join();
        consumer.join();

        check("ring: concurrent producer finished", written.load() == totalFrames);
        check("ring: concurrent consumer received everything",
              readBack.load() == totalFrames);
        check("ring: concurrent data is bit-exact (no torn frames)",
              !mismatch.load());
        std::printf("    [INFO] ring concurrent: %d frames, no corruption\n",
                    readBack.load());
    }


    // ---------------- SAP wire format (RFC 2974) ----------------
    {
        const std::string body = buildSdp(cfg, 0x11223344);
        const std::string oline = "o=- 287454020 1 IN IP4 192.168.1.10";
        const auto ann = buildSapDatagram(false, body, "192.168.1.10", oline, 0xA23E);
        check("sap: announcement V=1 A=0 T=0", ann.size() > 8 && ann[0] == 0x20);
        check("sap: auth length is zero", ann.size() > 8 && ann[1] == 0x00);
        check("sap: msg id hash non-zero", ((ann[2] << 8) | ann[3]) == 0xA23E);
        check("sap: originating source is the interface",
              ann[4] == 192 && ann[5] == 168 && ann[6] == 1 && ann[7] == 10);
        const std::string payload(reinterpret_cast<const char *>(ann.data() + 8),
                                  ann.size() - 8);
        check("sap: payload type is application/sdp",
              payload.rfind("application/sdp", 0) == 0);
        check("sap: payload carries the SDP", payload.find("v=0") != std::string::npos);

        const auto del = buildSapDatagram(true, body, "192.168.1.10", oline, 0xA23E);
        check("sap: deletion sets T bit", del.size() > 8 && del[0] == 0x24);
        check("sap: deletion keeps the hash", ((del[2] << 8) | del[3]) == 0xA23E);
        const std::string dpay(reinterpret_cast<const char *>(del.data() + 8),
                               del.size() - 8);
        check("sap: deletion payload is the origin line",
              dpay == std::string("application/sdp\0", 16) + oline);
        check("sap: deletion is shorter than an announcement", del.size() < ann.size());
    }

    {
        // Expiry rule, kept pure so it can be tested without a GUI.
        SapSourceAge fresh;
        fresh.lastSeenSeconds = 1000;
        check("sap: a fresh source is kept", !sapSourceExpired(fresh, 1000 + 19));
        check("sap: 20 s of silence expires an unknown-period source",
              sapSourceExpired(fresh, 1000 + 21));
        SapSourceAge slow;
        slow.lastSeenSeconds = 1000;
        slow.periodSeconds = 30;   // announced every 30 s
        check("sap: a slow announcer is not expired early",
              !sapSourceExpired(slow, 1000 + 60));
        check("sap: a slow announcer expires after ten periods",
              sapSourceExpired(slow, 1000 + 301));
        SapSourceAge never;
        check("sap: an entry never seen cannot expire",
              !sapSourceExpired(never, 100000));
    }

    {
        // The o= version is the only signal a receiver has that an announcement
        // is an update rather than a repeat of the same stream.
        StreamConfig sc;
        sc.group = "239.69.1.1";
        const std::string v1 = buildSdp(sc, 0x01020304, SdpClockRef{}, 1);
        const std::string v7 = buildSdp(sc, 0x01020304, SdpClockRef{}, 7);
        check("sdp: o= carries the session version",
              v1.find("o=- 16909060 1 IN IP4") != std::string::npos);
        check("sdp: a bumped version appears in o=",
              v7.find("o=- 16909060 7 IN IP4") != std::string::npos);
        check("sdp: version 0 is normalised to 1",
              buildSdp(sc, 1, SdpClockRef{}, 0).find("o=- 1 1 IN IP4") !=
                  std::string::npos);
    }

    // ---------------- ASIO registration probe ----------------
    {
        const AsioRegistration asio = probeAsioRegistration();
        // The machine either has the driver registered or it does not; both are
        // valid, but the fields must stay consistent with the verdict so a caller
        // can print an actionable message.
        check("asio probe: reports one of the known verdicts",
              asio.summary == "registered" ||
                  asio.summary == "registered but the DLL is missing" ||
                  asio.summary == "registered without an InprocServer32 path" ||
                  asio.summary == "not registered");
        check("asio probe: a registered driver has a name and CLSID",
              !asio.registered || (!asio.name.empty() && !asio.clsid.empty()));
    }

    // ---------------- frame queue ----------------
    // The receive loop drains this in pieces smaller than a packet. The invariants
    // that matter: order is preserved, a partial consume keeps the remainder, and a
    // drained queue reuses its storage instead of growing forever.
    {
        FrameQueue queue;
        queue.setChannels(2);
        check("queue: starts empty", queue.empty() && queue.frames() == 0);

        const int16_t a[4] = {1, 2, 3, 4};   // two frames
        queue.append(a, 2);
        check("queue: append counts frames", queue.frames() == 2);
        check("queue: data points at the first sample",
              queue.data()[0] == 1 && queue.data()[3] == 4);

        check("queue: partial consume returns what it took", queue.consume(1) == 1);
        check("queue: remainder is preserved",
              queue.frames() == 1 && queue.data()[0] == 3 && queue.data()[1] == 4);

        const int16_t b[2] = {5, 6};
        queue.append(b, 1);
        check("queue: order is preserved across appends",
              queue.frames() == 2 && queue.data()[0] == 3 && queue.data()[2] == 5);

        check("queue: consume clamps to what is there", queue.consume(10) == 2);
        check("queue: draining resets the counters",
              queue.empty() && queue.frames() == 0 && queue.consume(1) == 0);

        // Interleave append/partial-consume the way the loop does; the queue must
        // keep its storage instead of reallocating on every packet.
        // Steady state: one packet in, the same amount out.
        queue.clear();
        for (int i = 0; i < 5000; ++i) {
            queue.append(a, 2);
            queue.consume(2);
        }
        check("queue: storage stays bounded in steady state",
              queue.empty() && queue.capacitySamples() <= 64);
        for (int i = 0; i < 2000; ++i) {
            queue.append(a, 2);
            queue.consume(1);
        }
        check("queue: a slow drain keeps the frames it owes", queue.frames() == 2000);
        // std::vector doubles, so capacity can sit just under twice the live size;
        // the point is that it is not quadratic (which would be millions of samples
        // by this point).
        check("queue: growth is linear, not quadratic",
              queue.capacitySamples() <= static_cast<size_t>(queue.frames()) * 8);
    }

    // ---------------- config file parser ----------------
    {
        const char *path = "oa67_selftest.conf";
        {
            std::FILE *f = std::fopen(path, "wb");
            check("config: temp file created", f != nullptr);
            if (f) {
                // A BOM plus comments and blank lines: the two things an editor adds.
                std::fputs("\xEF\xBB\xBF# heading\n", f);
                std::fputs("\n", f);
                std::fputs("iface = 192.168.1.10   # trailing comment\n", f);
                std::fputs("group = 239.69.4.4\n", f);
                std::fputs("port = 5040\n", f);
                std::fputs("channels = 8\n", f);
                std::fputs("codec = l24\n", f);
                std::fputs("shm = OpenAES67Audio\n", f);
                std::fclose(f);
            }
        }
        CliOptions parsed;
        const bool ok = applyConfigFile(path, &parsed,
                                        [](const char *p, const char *m) {
                                            return std::fopen(p, m);
                                        });
        check("config: parses with a BOM and comments", ok);
        check("config: iface", parsed.cfg.ifaceIp == "192.168.1.10");
        check("config: group and port",
              parsed.cfg.group == "239.69.4.4" && parsed.cfg.port == 5040);
        check("config: channels", parsed.cfg.channels == 8);
        check("config: codec", parsed.cfg.codec == Codec::L24);
        check("config: shared section", parsed.shmName == L"OpenAES67Audio");

        {
            std::FILE *f = std::fopen(path, "wb");
            if (f) {
                std::fputs("iface = 1.2.3.4\n", f);
                std::fputs("bogus = 1\n", f);
                std::fclose(f);
            }
        }
        CliOptions bad;
        const bool badOk = applyConfigFile(path, &bad,
                                           [](const char *p, const char *m) {
                                               return std::fopen(p, m);
                                           },
                                           /*report=*/false);
        check("config: an unknown key is rejected", !badOk);
        std::remove(path);
    }

    // ---------------- SDP field order and clock reference ----------------
    {
        SdpClockRef ref;
        ref.kind = SdpClockRef::Kind::Ptp;
        const uint8_t gmid[8] = {0x00, 0x0E, 0xC6, 0xFF, 0xFE, 0x37, 0x38, 0x07};
        std::memcpy(ref.gmid, gmid, 8);
        ref.domain = 7;
        const std::string s = buildSdp(cfg, 0x5EED0001, ref);
        const size_t mPos = s.find("m=audio");
        const size_t cPos = s.find("c=IN IP4");
        const size_t rPos = s.find("a=rtpmap");
        const size_t tPos = s.find("a=ts-refclk");
        check("sdp: media line present", mPos != std::string::npos);
        check("sdp: connection line after media", cPos > mPos && cPos != std::string::npos);
        check("sdp: rtpmap after media", rPos > mPos && rPos != std::string::npos);
        check("sdp: ts-refclk after rtpmap", tPos > rPos && tPos != std::string::npos);
        check("sdp: ts-refclk names grandmaster and domain",
              s.find("a=ts-refclk:ptp=IEEE1588-2008:00-0E-C6-FF-FE-37-38-07:7") !=
                  std::string::npos);
        check("sdp: mediaclk present", s.find("a=mediaclk") != std::string::npos);

        SdpClockRef local;
        local.kind = SdpClockRef::Kind::LocalMac;
        const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
        std::memcpy(local.mac, mac, 6);
        const std::string s2 = buildSdp(cfg, 0x5EED0001, local);
        check("sdp: localmac form",
              s2.find("a=ts-refclk:localmac=AA-BB-CC-11-22-33") != std::string::npos);

        uint8_t parsed[8] = {0};
        check("sdp: gmid round trip",
              parseGmid("00-0E-C6-FF-FE-37-38-07", parsed) && parsed[3] == 0xFF &&
                  parsed[7] == 0x07);
        check("sdp: gmid rejects short input", !parseGmid("00-0E-C6", parsed));
        check("sdp: gmid rejects junk", !parseGmid("zz-0E-C6-FF-FE-37-38-07", parsed));
        check("sdp: gmid formats back",
              formatGmid(gmid) == std::string("00-0E-C6-FF-FE-37-38-07"));
    }

    // ---------------- SDP negative cases ----------------
    {
        int ch = 0, sr = 0;
        check("sdp: rejects empty", !parseSdpAudioFormat("", &ch, &sr));
        check("sdp: rejects missing rtpmap",
              !parseSdpAudioFormat("v=0\no=- 1 1 IN IP4 1.2.3.4\nm=audio 5004 RTP/AVP 96\n",
                                   &ch, &sr));
        check("sdp: rejects non-L16 codec",
              !parseSdpAudioFormat("m=audio 5004 RTP/AVP 96\na=rtpmap:96 OPUS/48000/2\n",
                                   &ch, &sr));
    }


    {   // Throughput of the jitter buffer push/pop path. This is the hot path of a
        // 1 ms stream (1000 packets/s), so it must not allocate per packet.
        JitterBuffer jb(fpp, chans, 4);
        auto p = makePacket(fpp, chans, 11);
        const int iterations = 200000;
        const auto t0 = std::chrono::steady_clock::now();
        int checksum = 0;
        for (int i = 0; i < iterations; ++i) {
            jb.push(static_cast<uint16_t>(i), p.data(), fpp);
            if (i % 2 == 1) { checksum += jb.pop(out, fpp); }
        }
        const auto dt = std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count();
        check("jitter: throughput sanity", checksum > 0);
        std::printf("    [INFO] jitter push+pop: %.1f ns per iteration (%d iterations)\n",
                    dt / iterations, iterations);
    }

    // ---------------- malformed network input ----------------
    // These parsers run on bytes an unauthenticated peer chose: AES67 has no
    // transport security, so a malformed datagram is the expected input, not an
    // exotic one. The mutations are deterministic (xorshift32 and fixed seeds), so
    // a failure reproduces on any standard library.
    {
        auto nextRandom = [](uint32_t &state) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };

        // --- SDP: whatever parses must be a stream this build can open ---
        {
            const std::string base =
                "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=OpenAES67\r\n"
                "c=IN IP4 239.69.1.1/32\r\nt=0 0\r\n"
                "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L16/48000/8\r\n";
            uint32_t state = 0x5eed1234u;
            int parsed = 0, impossible = 0;
            for (int i = 0; i < 4000; ++i) {
                std::string mutated = base;
                const int edits = 1 + static_cast<int>(nextRandom(state) % 4);
                for (int e = 0; e < edits; ++e) {
                    const size_t at = nextRandom(state) % mutated.size();
                    mutated[at] = static_cast<char>(nextRandom(state) & 0xFF);
                }
                int ch = 0, sr = 0;
                if (parseSdpAudioFormat(mutated, &ch, &sr)) {
                    ++parsed;
                    if (ch < 1 || ch > 64 || sr < 8000 || sr > 384000) {
                        ++impossible;
                    }
                }
            }
            check("fuzz: sdp never yields an impossible format", impossible == 0);
            check("fuzz: sdp still parses some mutations", parsed > 0);
            std::printf("    [INFO] sdp mutations: %d parsed, %d impossible\n", parsed, impossible);
        }

        // --- SDP: absurd values must be refused outright ---
        {
            int ch = 0, sr = 0;
            check("fuzz: sdp rejects 99999 channels",
                  !parseSdpAudioFormat("a=rtpmap:96 L16/48000/99999\r\n", &ch, &sr));
            check("fuzz: sdp rejects channel 0",
                  !parseSdpAudioFormat("a=rtpmap:96 L16/48000/0\r\n", &ch, &sr));
            check("fuzz: sdp rejects 1 Hz",
                  !parseSdpAudioFormat("a=rtpmap:96 L16/1/2\r\n", &ch, &sr));
            check("fuzz: sdp rejects negative channels",
                  !parseSdpAudioFormat("a=rtpmap:96 L16/48000/-8\r\n", &ch, &sr));
            check("fuzz: sdp accepts 64 channels at 96 kHz",
                  parseSdpAudioFormat("a=rtpmap:96 L24/96000/64\r\n", &ch, &sr) && ch == 64 &&
                      sr == 96000);
        }

        // --- SAP: the parsed body must stay inside the datagram ---
        {
            const std::string sdp = "v=0\r\no=- 1 1 IN IP4 192.0.2.10\r\ns=t\r\n";
            const std::vector<uint8_t> good =
                buildSapDatagram(false, sdp, "192.0.2.10", "o=- 1 1 IN IP4 192.0.2.10", 0x1234);
            std::vector<uint8_t> mutated;
            uint32_t state = 0xc0ffeeu;
            int parsed = 0, overflow = 0;
            for (int i = 0; i < 3000; ++i) {
                mutated = good;
                const int edits = 1 + static_cast<int>(nextRandom(state) % 5);
                for (int e = 0; e < edits; ++e) {
                    const size_t at = nextRandom(state) % mutated.size();
                    mutated[at] = static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                // Also exercise truncation: a datagram cut short by a partial read.
                const size_t len = nextRandom(state) % (mutated.size() + 1);
                SapAnnouncement announcement;
                if (parseSapAnnouncement(mutated.data(), len, &announcement)) {
                    ++parsed;
                    if (announcement.sdp.size() > len) {
                        ++overflow;
                    }
                }
            }
            check("fuzz: sap body never exceeds the datagram", overflow == 0);
            check("fuzz: sap still parses some mutations", parsed > 0);
            std::printf("    [INFO] sap mutations: %d parsed of 3000, %d overflow\n", parsed,
                        overflow);
        }

        // --- RTP: a header is either rejected or fits the buffer ---
        {
            uint8_t packet[kRtpHeaderSize + 4] = {0};
            RtpHeader original;
            original.sequence = 0x1234;
            original.timestamp = 0xDEADBEEF;
            original.ssrc = 0xC0FFEE;
            writeRtpHeader(packet, sizeof(packet), original);
            uint32_t state = 0xabad1deau;
            int parsed = 0, bad = 0;
            for (int i = 0; i < 4000; ++i) {
                uint8_t mutated[kRtpHeaderSize] = {0};
                std::memcpy(mutated, packet, kRtpHeaderSize);
                const int edits = 1 + static_cast<int>(nextRandom(state) % 3);
                for (int e = 0; e < edits; ++e) {
                    mutated[nextRandom(state) % kRtpHeaderSize] =
                        static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t len = nextRandom(state) % (kRtpHeaderSize + 3);
                RtpHeader header;
                if (readRtpHeader(mutated, len, &header)) {
                    ++parsed;
                    if (len < kRtpHeaderSize || header.version != 2) {
                        ++bad;
                    }
                }
            }
            check("fuzz: rtp header only parses inside the buffer", bad == 0);
            check("fuzz: rtp still parses some mutations", parsed > 0);
            RtpHeader tooShort;
            check("fuzz: rtp rejects an 11-byte buffer",
                  !readRtpHeader(packet, kRtpHeaderSize - 1, &tooShort));
            std::printf("    [INFO] rtp mutations: %d parsed, %d out of bounds\n", parsed, bad);
        }

        // --- payload decoding: never write past the caller's capacity ---
        {
            uint32_t state = 0x0ddba11u;
            int overflow = 0;
            for (int i = 0; i < 2000; ++i) {
                uint8_t payload[256];
                for (size_t b = 0; b < sizeof(payload); ++b) {
                    payload[b] = static_cast<uint8_t>(nextRandom(state) & 0xFF);
                }
                const size_t inSize = nextRandom(state) % (sizeof(payload) + 1);
                const Codec codec = (nextRandom(state) & 1) ? Codec::L16 : Codec::L24;
                const size_t capacity = nextRandom(state) % 128;
                int16_t out[128] = {0};
                const size_t produced = unpackPayload(codec, payload, inSize, out, capacity);
                if (produced > capacity || produced > inSize) {
                    ++overflow;
                }
            }
            check("fuzz: unpack never exceeds the destination", overflow == 0);
        }
    }

    std::printf("%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
