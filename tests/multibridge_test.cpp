// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/multibridge.h"
#include "openaes67/engine.h"
#include "openaes67/shared_audio_ring.h"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>

using namespace openaes67;
static int failures = 0, checks = 0;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static const char *preset =
    "rx 239.69.71.1 25704 8 1 L24 97\n"
    "rx 239.69.71.2 25706 8 9 L16 96\n"
    "tx 239.69.72.1 25710 8 1 L24 97\n"
    "tx 239.69.72.2 25712 8 9 L16 96\n";

static void unitTests() {
    std::string err;
    std::vector<AudioRoute> routes;
    check(parseAudioRoutes(std::string("\xef\xbb\xbf# preset\r\n") + preset, &routes, &err), "UTF-8 BOM, comments and preset");
    check(routes.size() == 4 && routes[1].asioFirst == 8 && !routes[2].receive, "persisted channel numbers are one based");
    std::string utf16("\xff\xfe", 2);
    for (const char *p = preset; *p; ++p) { utf16.push_back(*p); utf16.push_back(0); }
    check(parseAudioRoutes(utf16, &routes, &err), "PowerShell UTF-16 preset accepted");
    const char *invalid[] = {
        "", "rx 239.1.1.1 5004 8 60 L24 97", "rx 239.1.1.1 -1 8 1 L24 97",
        "rx 127.0.0.1 5004 8 1 L24 97", "rx 239.1.1.1 65536 8 1 L24 97",
        "rx 239.1.1.1 5004 8 1 L32 97", "rx 239.1.1.1 5004 8 1 L24 128",
        "rx 239.1.1.1 5004 8 1 L24 97 extra", "send 239.1.1.1 5004 8 1 L24 97",
        "rx 239.1.1.1 5004 8 1 L24 97\nrx 239.1.1.2 5006 8 8 L24 97",
        "rx 239.1.1.1 5004 8 1 L24 97\ntx 239.1.1.2 5004 8 1 L24 97"
    };
    for (const char *text : invalid) {
        check(!parseAudioRoutes(text, &routes, &err), "invalid preset rejected");
        check(routes.size() == 4, "invalid preset does not replace previous routes");
    }
    RoutedPlayout play(routes);
    std::array<int16_t, 48 * 8> a{}, b{};
    std::array<int16_t, 48 * 64> out{};
    for (int f = 0; f < 48; ++f) for (int c = 0; c < 8; ++c) {
        a[f * 8 + c] = static_cast<int16_t>(100 + c);
        b[f * 8 + c] = static_cast<int16_t>(200 + c);
    }
    // Begin close to wrap, with the second route arriving out of order.
    const uint32_t t = 0xfffffff0u;
    check(play.push(0, t, a.data()) && play.push(1, t + 48, b.data()) &&
          play.push(1, t, b.data()), "timestamp wrap and reorder accepted");
    auto duplicate = a; duplicate.fill(999);
    check(play.push(0, t, duplicate.data()), "duplicate accepted idempotently");
    check(play.pop(out.data()) == 0, "both flows present at shared epoch");
    bool mapped = true;
    for (int f = 0; f < 48; ++f) for (int c = 0; c < 64; ++c) {
        const int expected = c < 8 ? 100 + c : (c < 16 ? 200 + c - 8 : 0);
        mapped &= out[f * 64 + c] == expected;
    }
    check(mapped, "two 8-channel flows map exactly to ASIO 1-16, unused channels zero");
    check(!play.push(0, t, a.data()), "late packet rejected");
    check(play.pop(out.data()) == 48 * 8 && out[0] == 0 && out[8] == 200, "one missing flow does not shift the other");
    check(!play.push(0, t + 20000, a.data()), "incompatible RTP epoch rejected");
    check(!play.push(2, t, a.data()) && !play.push(99, t, a.data()), "cannot write tx or absent routes");
    play.reset();
    check(!play.anchored() && play.push(0, 1000, a.data()), "restart can anchor a new epoch");
    play.clearRoute(0);
    check(play.pop(out.data()) == 48 * 16, "clearing a source removes stale samples");
    // Exercise circular reuse, not just the first 48 frames.
    play.reset(); bool wraps = true;
    for (uint32_t n = 0; n < 300; ++n) {
        wraps &= play.push(0, t + n * 48, a.data());
        play.pop(out.data()); wraps &= out[0] == 100 && out[7] == 107;
    }
    check(wraps, "multiple buffer wraps preserve channels");

    const std::wstring name = L"OpenAES67RouteFactoryTest-" + std::to_wstring(GetCurrentProcessId());
    SharedAudioConfig cfg; cfg.channels = 64; cfg.capacityFrames = 48000;
    auto original = createSharedAudio(name.c_str(), cfg, &err);
    check(bool(original), "create named shared ring");
    if (original) {
        out.fill(123);
        original->writeRender(out.data(), 48);
        auto attached = createSharedAudio(name.c_str(), cfg, &err);
        check(attached && attached->renderPending() == 48, "reattach does not reset running indices");
        if (attached) { attached->readRender(out.data(), 48); check(out[0] == 123, "reattach preserves samples"); }
        cfg.channels = 32;
        check(!createSharedAudio(name.c_str(), cfg, &err), "mismatched geometry rejected");
        check(original->channels() == 64, "failed attach preserves original geometry");
    }
}

// Real UDP multicast + shared memory; the audio host is simulated, not hardware.
static void integrationTest(int hostBlock) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    Winsock winsock;
    std::string err;
    MultiBridgeOptions opt;
    int reports = 0, reportedPeak = 0;
    opt.report = [&](const MultiBridgeStats &s) {
        ++reports; reportedPeak = std::max(reportedPeak, s.renderPendingFrames);
    };
    parseAudioRoutes(preset, &opt.routes, &err);
    opt.ifaceIp = "127.0.0.1";
    opt.renderQueueMs = hostBlock > 480 ? 50 : 10;
    opt.shmName = L"OpenAES67MultiIntegration-" + std::to_wstring(GetCurrentProcessId());
    std::atomic<bool> stop{false};
    opt.shouldStop = [&] { return stop.load(); };
    SharedAudioConfig cfg; cfg.channels = 64; cfg.capacityFrames = 48000;
    auto host = createSharedAudio(opt.shmName.c_str(), cfg, &err);
    check(bool(host), "integration shared-memory host");
    if (!host) return;
    UdpSocket upA, upB;
    check(upA.openReceiver(opt.routes[2].group, opt.routes[2].port, opt.ifaceIp, &err) &&
          upB.openReceiver(opt.routes[3].group, opt.routes[3].port, opt.ifaceIp, &err), "integration tx receivers");
    if (!upA.isOpen() || !upB.isOpen()) return;
    StreamConfig stream;
    stream.ifaceIp = opt.ifaceIp; stream.channels = 8;
    stream.group = opt.routes[0].group; stream.port = opt.routes[0].port;
    stream.codec = Codec::L24; stream.payloadType = 97;
    RtpSender downA(stream, 100);
    stream.group = opt.routes[1].group; stream.port = opt.routes[1].port;
    stream.codec = Codec::L16; stream.payloadType = 96;
    RtpSender downB(stream, 200);
    check(downA.start(&err) && downB.start(&err), "integration rx senders");
    std::array<int16_t, 384> a{}, b{};
    std::vector<int16_t> capture(2048 * 64), render(2048 * 64);
    for (int f = 0; f < 2048; ++f) for (int c = 0; c < 64; ++c) {
        capture[f * 64 + c] = static_cast<int16_t>(1000 + c);
        if (f < 48 && c < 8) { a[f * 8 + c] = static_cast<int16_t>(100 + c); b[f * 8 + c] = static_cast<int16_t>(200 + c); }
    }
    MultiBridgeStats stats;
    bool bridgeOk = false;
    std::string bridgeErr;
    std::thread pump([&] { bridgeOk = runMultiBridge(opt, &stats, &bridgeErr); });
    const auto start = Clock::now();
    auto next = start;
    bool both = false, gap = false, resumed = false, txA = false, txB = false;
    bool crosstalk = false, hostRecovered = false;
    int peakRender = 0;
    int hostDue = 0;
    int shortHostReads = 0;
    int matchedEpochs = 0;
    std::map<uint32_t, unsigned> epochs;
    uint32_t stamp = 100000;
    std::array<uint8_t, 2048> packet{};
    std::array<int16_t, 384> decoded{};
    UdpSocket rogue;
    check(rogue.openSender(opt.ifaceIp, 1, &err), "integration malformed-packet injector");
    bool injected = false, duplicateOwnerRejected = false;
    while (Clock::now() - start < 4200ms) {
        const auto elapsed = Clock::now() - start;
        if (!injected && elapsed > 250ms && both) {
            RtpHeader h; h.ssrc = 999; h.timestamp = stamp; h.payloadType = 97;
            writeRtpHeader(packet.data(), packet.size(), h);
            const size_t bytes = 12 + packPayload(Codec::L24, a.data(), a.size(), packet.data() + 12, packet.size() - 12);
            rogue.sendTo(packet.data(), bytes, opt.routes[0].group, opt.routes[0].port);
            packet[0] |= 0x10; // unsupported extension
            rogue.sendTo(packet.data(), bytes, opt.routes[0].group, opt.routes[0].port);
            packet[0] = 0x80; packet[1] = 96; // unexpected payload type
            rogue.sendTo(packet.data(), bytes, opt.routes[0].group, opt.routes[0].port);
            rogue.sendTo(packet.data(), 8, opt.routes[0].group, opt.routes[0].port);
            MultiBridgeOptions second = opt; second.seconds = 0.01;
            std::string ownerErr;
            duplicateOwnerRejected = !runMultiBridge(second, nullptr, &ownerErr) &&
                ownerErr.find("another multibridge") != std::string::npos;
            injected = true;
        }
        // One-flow gap, then an all-flow outage > 1 s and a new media epoch.
        const bool outage = elapsed > 1100ms && elapsed < 2350ms;
        const bool onlyA = elapsed > 500ms && elapsed < 850ms;
        const bool hostPaused = elapsed > 3000ms && elapsed < 3400ms;
        if (elapsed >= 3400ms)
            for (int f = 0; f < 48; ++f) for (int c = 0; c < 8; ++c)
                a[f * 8 + c] = static_cast<int16_t>(300 + c);
        if (!outage) {
            const uint32_t ts = elapsed >= 2350ms ? stamp + 1000000 : stamp;
            downA.sendFramesAt(ts, a.data(), 48);
            if (!onlyA) downB.sendFramesAt(ts, b.data(), 48);
        }
        stamp += 48;
        hostDue = hostPaused ? 0 : hostDue + 48;
        const bool hostTick = !hostPaused && hostDue >= hostBlock;
        if (hostTick) { hostDue -= hostBlock; host->writeCapture(capture.data(), hostBlock); }
        peakRender = std::max(peakRender, host->renderPending());
        int frames = 0;
        // Fixed host blocks run at a 48 kHz clock; do not drain unlimited frames.
        if (hostTick) {
            frames = host->readRender(render.data(), hostBlock);
            if (elapsed > 200ms && frames != hostBlock) ++shortHostReads;
        }
        if (frames > 0) {
            for (int f = 0; f < frames; ++f) {
                const auto *v = render.data() + f * 64;
                if (v[0] == 100 && v[8] == 200) {
                    both = true; if (elapsed > 2500ms) resumed = true;
                }
                if (elapsed > 600ms && elapsed < 850ms && v[0] == 100 && v[8] == 0) gap = true;
                if (elapsed > 3500ms && v[0] == 300 && v[8] == 200) hostRecovered = true;
                for (int c = 0; c < 64; ++c) {
                    const int expected = c < 8 ? (elapsed > 3560ms ? 300 : 100) + c : (c < 16 ? 200 + c - 8 : 0);
                    if (c < 8 && elapsed >= 3400ms && elapsed <= 3560ms && v[c] == 300 + c) continue;
                    crosstalk |= v[c] != 0 && v[c] != expected;
                }
            }
        }
        for (int side = 0; side < 2; ++side) {
            UdpSocket &sock = side ? upB : upA;
            for (int n = 0; n < 64; ++n) {
                const int bytes = sock.recvAvailable(packet.data(), packet.size());
                if (bytes <= 0) break;
                RtpHeader h;
                if (!readRtpHeader(packet.data(), size_t(bytes), &h)) continue;
                const auto codec = side ? Codec::L16 : Codec::L24;
                const size_t got = unpackPayload(codec, packet.data() + 12, size_t(bytes) - 12, decoded.data(), decoded.size());
                bool valid = got == 384;
                for (size_t j = 0; j < got; ++j)
                    valid &= decoded[j] == 0 || decoded[j] == 1000 + int(j % 8) + side * 8;
                crosstalk |= !valid;
                if (decoded[0] == 1000 + side * 8) { if (side) txB = true; else txA = true; }
                auto &mask = epochs[h.timestamp];
                mask |= side ? 2u : 1u;
                if (mask == 3) { ++matchedEpochs; epochs.erase(h.timestamp); }
            }
        }
        next += 1ms;
        if (Clock::now() - next > 5ms) next = Clock::now();
        std::this_thread::sleep_until(next);
    }
    stop.store(true); pump.join();
    if (!bridgeOk) std::fprintf(stderr, "bridge: %s\n", bridgeErr.c_str());
    check(bridgeOk, "UDP bridge exits cleanly");
    check(both && !crosstalk, "two real UDP rx routes reach shared ASIO channels without crosstalk");
    check(gap, "one-flow outage produces silence while other flow continues");
    check(resumed && stats.restarts >= 1, "all-flow outage recovers with new RTP epoch");
    check(txA && txB && matchedEpochs > 100 && stats.txErrors == 0, "ASIO capture reaches both tx routes at common timestamps");
    check(stats.rxPackets > 100 && stats.txPackets > 100, "integration exchanged real packets");
    check(injected && stats.rejectedPackets >= 4, "active SSRC conflict, extension, wrong PT and short packet rejected");
    check(duplicateOwnerRejected, "second pump cannot become a concurrent shared-ring writer");
    check(hostRecovered && stats.hostPauses >= 1 && stats.hostResumes >= 2, "host pause reanchors fresh media on resume");
    check(peakRender <= 48 * opt.renderQueueMs && stats.renderDroppedFrames == 0, "render backlog stays within configured target while host stops");
    check(reports >= 3 && reportedPeak <= 48 * opt.renderQueueMs, "live reports expose bounded buffer depth");
    check(shortHostReads == 0, "configured queue supplies complete host blocks after startup");
    std::printf("UDP simulation: host_block=%d rx=%llu tx=%llu restarts=%llu schedule_resets=%llu matched_tx=%d\n",
        hostBlock, stats.rxPackets, stats.txPackets, stats.restarts, stats.scheduleResets, matchedEpochs);
}

// Accelerated virtual time: independent network and host clocks for two minutes.
// Constant inter-channel offsets make any split resampling phase observable.
static void driftTests() {
    std::vector<AudioRoute> routes;
    std::string err;
    parseAudioRoutes(preset, &routes, &err);
    const double scenarios[][2] = {{100, 0}, {-100, 0}, {0, 500}, {0, -500}};
    for (const auto &scenario : scenarios) {
        const double networkPpm = scenario[0], hostPpm = scenario[1];
        RoutedPlayout play(routes);
        PlayoutRateServo servo;
        std::array<int16_t, 384> a{}, b{};
        std::array<int16_t, 256 * 64> out{};
        uint32_t stamp = 0xffffffe0u;
        bool accepted = true, aligned = true;
        auto send = [&] {
            for (uint32_t f = 0; f < 48; ++f) for (int c = 0; c < 8; ++c) {
                a[f * 8 + c] = static_cast<int16_t>(1000 + ((stamp + f) & 1023u) + c * 16);
                b[f * 8 + c] = static_cast<int16_t>(a[f * 8 + c] + 8000);
            }
            accepted &= play.push(0, stamp, a.data()) && play.push(1, stamp, b.data());
            stamp += 48;
        };
        for (int n = 0; n < 30; ++n) send();
        double networkDue = 0, hostDue = 0, ppmSum = 0, depthSum = 0;
        uint64_t missing = 0;
        int minDepth = 4096, maxDepth = 0;
        for (int tick = 0; tick < 120000; ++tick) {
            networkDue += 48 * (1 + networkPpm * 1e-6);
            while (networkDue >= 48) { send(); networkDue -= 48; }
            const int depth = play.bufferedFrames();
            servo.observe(depth, 0.001);
            hostDue += 48 * (1 + hostPpm * 1e-6);
            if (hostDue >= 256) {
                hostDue -= 256;
                missing += play.popFrames(out.data(), 256, servo.correctionPpm());
                for (int f = 0; f < 256; ++f) {
                    for (int c = 0; c < 8; ++c)
                        aligned &= std::abs(int(out[f * 64 + c + 8]) - out[f * 64 + c] - 8000) <= 1;
                    aligned &= out[f * 64 + 16] == 0 && out[f * 64 + 63] == 0;
                }
            }
            if (tick > 20000) { minDepth = std::min(minDepth, depth); maxDepth = std::max(maxDepth, depth); }
            if (tick >= 100000) { ppmSum += servo.correctionPpm(); depthSum += depth; }
        }
        const double expected = ((1 + networkPpm * 1e-6) / (1 + hostPpm * 1e-6) - 1) * 1e6;
        const double meanPpm = ppmSum / 20000;
        std::printf("Drift simulation: network=%+.0f host=%+.0f ppm, correction=%+.1f, depth=%d..%d, mean=%.1f, missing=%llu\n",
            networkPpm, hostPpm, meanPpm, minDepth, maxDepth, depthSum / 20000, missing);
        check(accepted && missing == 0, "120-second drift simulation has no late packets or missing samples");
        check(aligned, "all routes retain common interpolation phase and unused channels stay silent");
        check(minDepth > 900 && maxDepth < 2000, "drift servo keeps media buffer bounded near 30 ms");
        check(std::fabs(meanPpm - expected) < 50, "rate correction converges to measured virtual clock difference");
    }
    PlayoutRateServo servo;
    for (int n = 0; n < 5000; ++n) servo.observe(4096, 0.001);
    check(servo.correctionPpm() == 2000, "positive correction bounded");
    servo.reset();
    for (int n = 0; n < 5000; ++n) servo.observe(0, 0.001);
    check(servo.correctionPpm() == -2000, "negative correction bounded");
    servo.observe(1440, 1);
    check(servo.correctionPpm() == 0, "scheduler stall resets control history");
}
int main() {
    timeBeginPeriod(1);
    unitTests();
    driftTests();
    integrationTest(48);
    integrationTest(256);
    integrationTest(2048);
    timeEndPeriod(1);
    std::printf("MULTIBRIDGE %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
