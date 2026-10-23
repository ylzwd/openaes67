// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "openaes67/engine.h"
#include "openaes67/net.h"
#include "openaes67/receive_loop.h"
#include "openaes67/sap.h"
#include "openaes67/sdp.h"
#include "openaes67/shared_audio_ring.h"

namespace openaes67 {

namespace {

void logLine(const BridgeOptions &opt, const std::string &line) {
    if (opt.onLog) {
        opt.onLog(line);
        return;
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

bool stopped(const BridgeOptions &opt, std::atomic<bool> *running) {
    if (opt.shouldStop && !opt.shouldStop()) {
        running->store(false);
    }
    return !running->load();
}

} // namespace

bool runBridge(const BridgeOptions &options, BridgeStats *stats, std::string *err) {
    if (options.shmName.empty()) {
        if (err) {
            *err = "bridge needs a shared section name";
        }
        return false;
    }
    if (options.downCfg.channels <= 0 || options.downCfg.sampleRate <= 0) {
        if (err) {
            *err = "bridge needs a valid channel count and sample rate";
        }
        return false;
    }

    BridgeStats local;
    std::atomic<bool> running{true};

    // Downlink: the regular receive loop, writing into the shared ring.
    ReceiveLoopOptions rx;
    rx.cfg = options.downCfg;
    rx.shmName = options.shmName;
    rx.backend = "wasapi";
    rx.seconds = options.seconds;
    rx.usePtp = options.usePtp;
    rx.ptpDomain = options.ptpDomain;
    rx.shouldStop = [&running] { return running.load(); };
    rx.onLog = [&options](const std::string &s) { logLine(options, "[rx] " + s); };

    std::string rxErr;
    std::thread rxThread([&] {
        if (!runReceiveLoop(rx, nullptr, &rxErr)) {
            logLine(options, "[rx] failed: " + rxErr);
            running.store(false);
        }
    });

    // The receive side owns the section; wait for it to appear.
    std::unique_ptr<SharedAudioRing> ring;
    for (int i = 0; i < 100 && !ring; ++i) {
        std::string e;
        ring = openSharedAudio(options.shmName.c_str(), &e);
        if (!ring) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    if (!ring) {
        running.store(false);
        if (rxThread.joinable()) {
            rxThread.join();
        }
        if (err) {
            *err = "shared section never appeared";
        }
        return false;
    }

    RtpSender sender(options.upCfg, 0);
    std::string sendErr;
    if (!sender.start(&sendErr)) {
        running.store(false);
        if (rxThread.joinable()) {
            rxThread.join();
        }
        if (err) {
            *err = "uplink start failed: " + sendErr;
        }
        return false;
    }

    // The uplink is announced so a Dante receiver can see and subscribe to it.
    // Dante Controller shows third-party AES67 transmit flows (blue, at the right
    // of the routing matrix) and nothing for a non-Dante receiver, so announcing
    // this direction is the only part of "both directions visible" we control.
    SapAnnouncer sap;
    if (options.announceUp) {
        SdpClockRef ref;
        ref.kind = SdpClockRef::Kind::LocalMac;
        if (!localMacForIp(options.upCfg.ifaceIp, ref.mac)) {
            logLine(options, "[sap] cannot read the interface MAC; SDP will carry zeros");
        }
        std::string sapErr;
        if (!sap.start(options.upCfg.ifaceIp, buildSdp(options.upCfg, 0, ref), &sapErr) ||
            !sap.announce(&sapErr)) {
            logLine(options, "[sap] announce failed: " + sapErr);
        } else {
            char line[192];
            std::snprintf(line, sizeof(line), "[sap] announcing uplink %s:%u",
                          options.upCfg.group.c_str(), options.upCfg.port);
            logLine(options, line);
        }
        if (options.sapInterval > 0) {
            sap.setIntervalSeconds(options.sapInterval);
        }
    }
    // SAP announcements repeat: a receiver that was not listening at start-up
    // (or joined the network later) only learns about the flow from a repeat.
    auto nextAnnounce = std::chrono::steady_clock::now() +
                        std::chrono::seconds(options.sapInterval > 0 ? options.sapInterval : 30);
    const int fpp = options.upCfg.framesPerPacket();
    const long long ptimeUs =
        static_cast<long long>(1000000.0 * fpp / options.upCfg.sampleRate + 0.5);
    const long long reportEvery =
        std::max(1LL, static_cast<long long>(options.reportSeconds * 1000000.0 / ptimeUs));
    std::vector<int16_t> chunk(static_cast<size_t>(fpp) * options.upCfg.channels, 0);
    const auto t0 = std::chrono::steady_clock::now();

    for (long long n = 0; !stopped(options, &running); ++n) {
        if (options.seconds > 0.0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (elapsed >= options.seconds) {
                break;
            }
        }
        if (n > 0 && n % reportEvery == 0) {
            char line[192];
            std::snprintf(line, sizeof(line),
                          "[tx] %lld packets, %lld with no ASIO audio to send",
                          local.uplinkPackets, local.uplinkSilent);
            logLine(options, line);
        }
        // Non-blocking read: the ASIO driver is the producer, so a short read means
        // the host is not playing yet (or stopped). Silence keeps the RTP timeline
        // continuous, which is what a receiver expects from a live device.
        const int got = ring->readCapture(chunk.data(), fpp);
        if (got < fpp) {
            std::fill(chunk.begin(), chunk.end(), static_cast<int16_t>(0));
            ++local.uplinkSilent;
        }
        if (sender.sendFrames(chunk.data(), fpp)) {
            ++local.uplinkPackets;
        }
        if (options.announceUp && std::chrono::steady_clock::now() >= nextAnnounce) {
            std::string sapErr;
            if (!sap.announce(&sapErr)) {
                logLine(options, "[sap] re-announce failed: " + sapErr);
            }
            nextAnnounce = std::chrono::steady_clock::now() +
                           std::chrono::seconds(options.sapInterval > 0 ? options.sapInterval
                                                                        : 30);
        }
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(ptimeUs * (n + 1)));
    }

    if (options.announceUp) {
        sap.withdraw();
        sap.stop();
    }
    running.store(false);
    if (rxThread.joinable()) {
        rxThread.join();
    }
    sender.stop();
    local.ringDropped = ring->renderUnderruns();
    if (stats) {
        *stats = local;
    }
    return true;
}

} // namespace openaes67
