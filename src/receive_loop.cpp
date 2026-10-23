// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/receive_loop.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>

#include "openaes67/engine.h"
#include "openaes67/frame_queue.h"
#include "openaes67/net.h"
#include "openaes67/ptp.h"
#include "openaes67/resampler.h"
#include "openaes67/shared_audio_ring.h"

namespace openaes67 {
namespace {

void logTo(const ReceiveLoopOptions &opt, const std::string &line) {
    if (opt.onLog) {
        opt.onLog(line);
    }
}

} // namespace

bool runReceiveLoop(const ReceiveLoopOptions &opt, RxSnapshot *finalSnapshot,
                    std::string *err) {
    const StreamConfig cfgIn = opt.cfg;

    // WASAPI is COM based. The CLI happens to work without this, but a worker
    // thread (the GUI runs the loop on a QThread) must initialize its own
    // apartment, otherwise the client can behave oddly -- observed as the
    // device consuming ~2000 ppm faster than the network delivered.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comInit);
    struct ComGuard {
        bool owned;
        ~ComGuard() { if (owned) { CoUninitialize(); } }
    } comGuard{comOwned};

    Winsock ws;
    if (!ws.ok()) {
        if (err) { *err = "Winsock init failed"; }
        return false;
    }

    // Shared-memory mode replaces the audio device entirely: the kernel driver
    // is the consumer, so no WASAPI endpoint is opened.
    StreamConfig cfg = cfgIn;
    const int streamChannels = cfg.channels > 0 ? cfg.channels : 2;

    // Join the group before touching any audio device: a wrong interface should
    // fail fast instead of opening a sound card and closing it again immediately.
    RtpReceiver receiver(cfg);
    if (!receiver.start(err)) {
        return false;
    }

    std::unique_ptr<SharedAudioRing> shm;
    std::unique_ptr<AudioSink> sink;
    if (!opt.shmName.empty()) {
        SharedAudioConfig shmCfg;
        shmCfg.channels = cfgIn.channels > 0 ? cfgIn.channels : 2;
        shmCfg.sampleRate = cfgIn.sampleRate > 0 ? cfgIn.sampleRate : 48000;
        shmCfg.capacityFrames = static_cast<uint32_t>(shmCfg.sampleRate);   // 1 s
        shm = createSharedAudio(opt.shmName.c_str(), shmCfg, err);
        if (!shm) {
            // The ASIO driver may have created it first (host started before the
            // pump). Attaching is the right answer there, not failing.
            shm = openSharedAudio(opt.shmName.c_str(), err);
        }
        if (!shm) {
            return false;
        }
        logTo(opt, "shm: section ready");
    } else if (opt.backend == "asio") {
        std::string asioError;
        sink.reset(createAsioRenderSink(opt.asioDriver.c_str(),
                                        opt.cfg.channels, opt.cfg.sampleRate, &asioError));
        if (!sink) {
            if (err) { *err = asioError.empty() ? "cannot open ASIO driver" : asioError; }
            return false;
        }
    } else {
        const wchar_t *wanted = opt.deviceId.empty() ? nullptr : opt.deviceId.c_str();
        // Ask for the stream format. Shared mode converts it, so the RTP rate and
        // the playback rate stay identical; exclusive mode has no converter and
        // will report the device rate back, which the block below then adopts.
        sink.reset(opt.exclusive ? createWasapiRenderSinkExclusive(wanted)
                                 : createWasapiRenderSink(wanted, cfgIn.sampleRate,
                                                          cfgIn.channels));
        if (!sink) {
            if (err) { *err = "cannot open WASAPI render device"; }
            return false;
        }
    }

    int sinkChannels = sink ? sink->channels() : streamChannels;
    if (sinkChannels <= 0) {
        sinkChannels = streamChannels;
    }
    if (sink && (sinkChannels != streamChannels || sink->sampleRate() != cfg.sampleRate)) {
        logTo(opt, "note: stream " + std::to_string(streamChannels) + "ch@" +
                       std::to_string(cfg.sampleRate) + "Hz, device " +
                       std::to_string(sinkChannels) + "ch@" +
                       std::to_string(sink->sampleRate()) + "Hz -> converting");
    }
    const int channels = streamChannels;
    const int fpp = cfg.framesPerPacket();
    if (sink) {
        char info[512];
        // Report the sink's real format, not the stream's: with conversion active
        // they differ, and printing the stream count claimed 8ch on a 2ch device.
        std::snprintf(info, sizeof(info), "sink: %s, %d ch @ %d Hz, packet=%d frames",
                      sink->backendName(), sink->channels(), sink->sampleRate(), fpp);
        logTo(opt, info);
        if (!opt.deviceLabel.empty()) {
            logTo(opt, "device: " + opt.deviceLabel);
        } else {
            logTo(opt, "device: system default output");
        }
    }

    // Prime the device with silence: an exclusive stream that starts empty can
    // stop advancing, after which writes block forever.
    {
        if (sink) {
            std::vector<int16_t> silence(static_cast<size_t>(fpp) * channels, 0);
            const int primeTarget = cfg.sampleRate / 10;      // 100 ms
            for (int done = 0; done < primeTarget; done += fpp) {
                if (sink->write(silence.data(), fpp) <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<int16_t> chunk(static_cast<size_t>(fpp) * channels);
    // Silence used to keep the device fed when the network starves.
    std::vector<int16_t> silenceBuf(static_cast<size_t>(fpp) * channels, 0);
    std::vector<int16_t> pullBuf(static_cast<size_t>(fpp) * channels);
    // Frames the device refused last time. Consumed from the front with an offset
    // (FrameQueue): erasing after every partial write moved the whole remainder,
    // which at 64 channels is ~180 KB per delivery.
    FrameQueue pending;
    pending.setChannels(channels);
    RateController rateCtl(channels, cfg.sampleRate, 30.0);

    // Optional PTP closed loop.
    //
    // The slave runs in its own thread. Timestamps must be taken when the packet
    // is processed, and the audio loop can block for up to ~50 ms inside
    // sink->write(); polling PTP from there made t2 late by that much, which
    // destroyed the frequency estimate (standalone: 307 ppm for a 300 ppm
    // master, in-loop: anywhere from 150 to 500).
    PtpSlave ptp;
    bool ptpUp = false;
    std::atomic<bool> ptpRun{false};
    std::atomic<double> ptpPpm{0.0};
    std::atomic<bool> ptpValid{false};
    std::atomic<uint32_t> ptpSamples{0};
    std::thread ptpThread;
    if (opt.usePtp) {
        std::string ptpErr;
        if (ptp.start(cfg.ifaceIp, static_cast<uint8_t>(opt.ptpDomain), &ptpErr)) {
            ptpUp = true;
            ptpRun.store(true);
            ptpThread = std::thread([&ptp, &ptpRun, &ptpPpm, &ptpValid, &ptpSamples, &opt] {
                auto lastReq = std::chrono::steady_clock::now();
                while (ptpRun.load()) {
                    ptp.poll(1);
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastReq >= std::chrono::milliseconds(250)) {
                        lastReq = now;
                        ptp.sendDelayReq();
                    }
                    const PtpSlave::Stats &st = ptp.stats();
                    ptpPpm.store(st.freqPpm);
                    ptpValid.store(st.freqValid);
                    ptpSamples.store(st.delayRespCount);
                    // Announce the first lock: without this the operator cannot
                    // tell a wrong PTP domain from a network with no clock at all.
                    static bool announced = false;
                    if (!announced && st.freqValid) {
                        announced = true;
                        const uint8_t *gm = ptp.masterIdentity();
                        char msg[192];
                        std::snprintf(msg, sizeof(msg),
                                      "ptp: locked to grandmaster %02X-%02X-%02X-%02X-%02X"
                                      "-%02X-%02X-%02X, master %+.1f ppm",
                                      gm[0], gm[1], gm[2], gm[3], gm[4], gm[5], gm[6], gm[7],
                                      st.freqPpm);
                        logTo(opt, msg);
                    }
                }
            });
            logTo(opt, "ptp: slave on domain " + std::to_string(opt.ptpDomain) +
                           ", waiting for a grandmaster");
        } else {
            logTo(opt, "ptp: disabled (" + ptpErr + ")");
        }
    }

    if (cfg.rtcp) {
        if (receiver.rtcpActive()) {
            logTo(opt, "rtcp: listening on port " + std::to_string(cfg.rtcpPort()));
        } else {
            logTo(opt, "rtcp: disabled (" + receiver.rtcpError() + ")");
        }
    }

    // Prebuffer to the control target before playing anything.
    {
        const int target = static_cast<int>(rateCtl.targetFrames());
        const auto preDeadline = t0 + std::chrono::milliseconds(3000);
        std::vector<int16_t> silence(static_cast<size_t>(fpp) * channels, 0);
        while (rateCtl.bufferedFrames() < target &&
               std::chrono::steady_clock::now() < preDeadline) {
            const int n = receiver.pull(chunk.data(), fpp, 20);
            if (n > 0) {
                rateCtl.push(chunk.data(), n);
            } else if (sink) {
                // Keep the device alive while waiting for the first packets. In
                // shared-memory mode there is no device to keep alive: writing
                // here dereferenced a null sink and killed the process before it
                // produced a single report line.
                sink->write(silence.data(), fpp);
            }
        }
        logTo(opt, "prebuffer: reached " + std::to_string(rateCtl.bufferedFrames()) +
                       " of " + std::to_string(target) + " frames");
    }

    const auto window = std::chrono::milliseconds(
        static_cast<long long>(opt.reportIntervalSeconds * 1000.0));
    auto deadline = t0 + std::chrono::milliseconds(
                              static_cast<long long>(opt.seconds * 1000.0));
    auto nextReport = t0 + window;

    long long written = 0;
    auto      lastFeed = std::chrono::steady_clock::now();
    double    feedRemainder = 0.0;   // sub-frame remainder for real-time silence
    long long writtenAtLastReport = 0;
    long long partialWrites = 0;
    int       padAtLastReport = sink ? sink->deviceBufferedFrames() : -1;
    long long zeroWrites = 0;
    double    secondsAtLastReport = 0.0;
    size_t underruns = 0;
    int    consecutiveZeroWrites = 0;
    double fbPpm = 0.0;
    bool fbInit = false;

    // Stall detection: a receiver that keeps writing silence looks perfectly
    // healthy in every other counter. Track the packet counter and report the
    // transition once in each direction instead of spamming the log.
    uint32_t packetsSeen = 0;
    auto lastPacketAt = std::chrono::steady_clock::now();
    bool stalled = false;
    uint32_t rtcpSeen = 0;      // SRs already reported
    auto lastRrSent = std::chrono::steady_clock::now();
    const std::string rrCname = "openaes67@" + cfg.ifaceIp;
    const auto stallAfter = std::chrono::milliseconds(
        static_cast<long long>((opt.stallSeconds > 0.0 ? opt.stallSeconds : 1.0) * 1000.0));

    // One delivery path for both modes: the shared ring (kernel driver is the
    // consumer) or a WASAPI device. Keeping a single call site is what stops the
    // two modes from drifting apart.
    // With a WASAPI device the device itself paces the loop: write() blocks when
    // its buffer is full. A shared ring never blocks while the reader keeps up,
    // so without pacing the pump ran at 48969 frames/s for a 48 kHz stream and
    // the rate controller, seeing a permanent deficit, pinned at its -20000 ppm
    // clamp (mean depth 365 of 1440). Pace to the nominal rate here.
    // Pace ALL backends to the nominal rate. A real device blocks inside write()
    // when its buffer is full, but a virtual device (ToDesk, VB-Cable, Dante Via)
    // has no hardware clock and accepts data as fast as it is offered. The loop then
    // outran production, the FIFO never filled, and the rate controller pinned at its
    // -20000 ppm clamp: measured written=49230 frames/s for a 48 kHz stream.
    const auto paceStart = std::chrono::steady_clock::now();
    long long pacedFrames = 0;
    long long ringDropped = 0;   // frames the ring refused (no consumer)
    // The stream may carry more (or fewer) channels than the output device: take the
    // first N channels of each frame, duplicating channel 0 when the device has more.
    std::vector<int16_t> convertBuf;
    auto deliver = [&](const int16_t *frames, int count) -> int {
        const int16_t *out = frames;
        if (!shm && sinkChannels != streamChannels && count > 0) {
            convertBuf.resize(static_cast<size_t>(count) * sinkChannels);
            convertChannels(frames, count, streamChannels, sinkChannels,
                            convertBuf.data());
            out = convertBuf.data();
        }
        if (shm) {
            // A sound card drops when its consumer is late; it does not queue.
            // Returning the accepted count made the caller keep the remainder in
            // its pending buffer, so an absent reader (ASIO host not started, or
            // already stopped) grew the jitter buffer without bound: measured
            // 102920 frames against a 1440 target.
            const int accepted = shm->writeRender(out, count);
            if (accepted < count) {
                ringDropped += count - accepted;
            }
            pacedFrames += count;
            const int rate = cfg.sampleRate > 0 ? cfg.sampleRate : 48000;
            const auto due =
                paceStart + std::chrono::microseconds(pacedFrames * 1000000LL / rate);
            std::this_thread::sleep_until(due);
            return count;
        }
        const int accepted = sink->write(out, count);
        if (accepted > 0) {
            pacedFrames += accepted;
            const int rate = cfg.sampleRate > 0 ? cfg.sampleRate : 48000;
            const auto due = paceStart +
                             std::chrono::microseconds(pacedFrames * 1000000LL / rate);
            // sleep_until returns immediately when the device is already behind, so
            // real hardware keeps its own pacing.
            std::this_thread::sleep_until(due);
        }
        return accepted;
    };

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (opt.seconds > 0.0 && now >= deadline) {
            break;
        }
        if (opt.shouldStop && !opt.shouldStop()) {
            break;
        }

        // Did any packet arrive since the last pass? A stream can vanish without
        // any error: the socket simply stops delivering. Compare the receiver's own
        // counter so buffered audio draining does not look like traffic.
        {
            const uint32_t seen = receiver.stats().packets;
            if (seen != packetsSeen) {
                packetsSeen = seen;
                lastPacketAt = now;
                if (stalled) {
                    stalled = false;
                    logTo(opt, "stream resumed");
                    if (opt.onStall) {
                        opt.onStall(false);
                    }
                }
            } else if (!stalled && now - lastPacketAt >= stallAfter) {
                stalled = true;
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "no packets for %.1f s (sender stopped?)",
                              std::chrono::duration<double>(now - lastPacketAt).count());
                logTo(opt, msg);
                if (opt.onStall) {
                    opt.onStall(true);
                }

            }
        }
        // A sender report is the only view of the far end: it carries the
        // sender clock and its own packet counters.
        if (receiver.rtcpActive() && receiver.rtcpPackets() != rtcpSeen) {
            rtcpSeen = receiver.rtcpPackets();
            const RtcpSenderReport &sr = receiver.senderReport();
            char line[256];
            std::snprintf(line, sizeof(line),
                          "rtcp: SR from %s ntp=%08X.%08X rtp=%u packets=%u octets=%u",
                          receiver.rtcpCname().empty() ? "(no cname)"
                                                       : receiver.rtcpCname().c_str(),
                          static_cast<uint32_t>(sr.ntp >> 32),
                          static_cast<uint32_t>(sr.ntp & 0xFFFFFFFF),
                          sr.rtpTimestamp, sr.packetCount, sr.octetCount);
            logTo(opt, line);
        }

        // Report back every 5 s once a sender report has arrived: this is what
        // the sending end needs to show loss and jitter for its own stream.
        if (receiver.rtcpActive() && receiver.hasSenderReport() &&
            std::chrono::steady_clock::now() - lastRrSent >=
                std::chrono::milliseconds(5000)) {
            receiver.sendReceiverReport(rrCname);
            lastRrSent = std::chrono::steady_clock::now();
        }

        if (ptpUp) {
            // The regression needs a decent baseline; the first samples can
            // produce absurd slopes that would slam the controller into a clamp.
            if (ptpValid.load() && ptpSamples.load() >= 12) {
                const double raw = ptpPpm.load();
                fbPpm = fbInit ? (fbPpm * 0.9 + raw * 0.1) : raw;
                fbInit = true;
                if (fbPpm > 500.0)  fbPpm = 500.0;
                if (fbPpm < -500.0) fbPpm = -500.0;
                rateCtl.setFeedforwardPpm(fbPpm);
            }
        }

        bool gotAny = false;
        for (int k = 0; k < 64; ++k) {
            // 1 ms, not 20: when the network starves the loop must still run at
            // roughly packet cadence, otherwise the silence it feeds arrives far too
            // slowly to keep the device alive (measured: 2154 Hz written instead of
            // 48000, and the device underran anyway).
            const int n = receiver.pull(chunk.data(), fpp, (k == 0 && !gotAny) ? 1 : 0);
            if (n <= 0) {
                break;
            }
            rateCtl.push(chunk.data(), n);
            gotAny = true;
        }

        // Feed the device until it refuses more, but stop once it already holds
        // a comfortable amount. Without this cap the split between our FIFO and
        // the device buffer depends on how eagerly the loop runs -- the GUI
        // parked ~1950 frames in the device and 1067 in the FIFO, the CLI the
        // other way round, for the same total latency.
        for (int guard = 0; guard < 64; ++guard) {
            if (!pending.empty()) {
                const int w = deliver(pending.data(), pending.frames());
                if (w <= 0) {
                    // The device is not draining. If it never resumes, holding
                    // the frames forever deadlocks the loop (observed with a
                    // too-small exclusive buffer: devpad stuck, written stayed
                    // 0 for the whole run). Drop them and count it instead.
                    if (++consecutiveZeroWrites > 200) {
                        pending.clear();
                        consecutiveZeroWrites = 0;
                        ++underruns;
                    }
                    break;
                }
                consecutiveZeroWrites = 0;
                written += w;
                pending.consume(w);
                continue;
            }
            const int p = rateCtl.pull(pullBuf.data(), fpp);
            if (p <= 0) {
                break;
            }
            if (sink && sink->deviceBufferedFrames() == 0) {
                consecutiveZeroWrites = 0;
            }
            const int w = deliver(pullBuf.data(), p);
            if (w == 0) {
                ++zeroWrites;
            } else if (w < p) {
                ++partialWrites;
            }
            if (w > 0) {
                written += w;
            }
            if (w < p) {
                pending.append(pullBuf.data() + static_cast<size_t>(w) * channels, p - w);
            }
            if (w <= 0) {
                break;
            }
        }

        if (!gotAny && rateCtl.bufferedFrames() == 0) {
            // Nothing to play. Sleeping alone lets the device underrun, which is the
            // click heard at the start of a dropout. Feed silence paced by wall clock:
            // the frames due since the last feed are known exactly, so this keeps the
            // device time-continuous no matter how slowly this loop is turning.
            ++underruns;
            const auto feedNow = std::chrono::steady_clock::now();
            const double elapsedSec =
                std::chrono::duration<double>(feedNow - lastFeed).count();
            lastFeed = feedNow;
            feedRemainder += elapsedSec * (cfg.sampleRate > 0 ? cfg.sampleRate : 48000);
            int due = static_cast<int>(feedRemainder);
            feedRemainder -= due;
            while (due > 0) {
                const int wantFrames = due < fpp ? due : fpp;
                int w = 0;
                if (sink) {
                    w = sink->write(silenceBuf.data(), wantFrames);
                } else if (shm) {
                    w = deliver(silenceBuf.data(), wantFrames);
                }
                if (w <= 0) {
                    break;
                }
                written += w;
                due -= w;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            lastFeed = std::chrono::steady_clock::now();
            feedRemainder = 0.0;
        }

        if (now >= nextReport) {
            nextReport = now + window;
            {
                RxSnapshot s;
                s.seconds = std::chrono::duration<double>(now - t0).count();
                s.buffered = static_cast<int>(rateCtl.meanBuffered());
                s.target = static_cast<int>(rateCtl.targetFrames());
                s.correctionPpm = rateCtl.correctionPpm();
                s.effectivePpm = rateCtl.effectivePpm();
                s.ptpPpm = fbInit ? fbPpm : 0.0;
                s.packets = receiver.stats().packets;
                s.lost = receiver.jitter().lost();
                s.late = receiver.jitter().late();
    s.wrongSsrc = receiver.stats().wrongSsrc;
    s.ssrcChanges = receiver.stats().ssrcChanges;
                s.underruns = static_cast<int>(underruns);
                s.framesWritten = written;
                // Rate over this reporting window only. framesWritten / total
                // seconds is useless because the total includes prebuffer and
                // teardown idle time (it read 43566 Hz for a healthy 48000 Hz
                // stream).
                const double windowSeconds = s.seconds - secondsAtLastReport;
                s.outRateHz = windowSeconds > 0.01
                                  ? static_cast<double>(written - writtenAtLastReport) /
                                        windowSeconds
                                  : 0.0;
                // What the device really played in this window: frames handed
                // over, adjusted by the change in its own buffer occupancy.
                // "bytes accepted by write()" alone cannot distinguish "played"
                // from "still queued".
                const int padNow = sink ? sink->deviceBufferedFrames() : -1;
                double playedRate = 0.0;
                if (windowSeconds > 0.01 && padNow >= 0 && padAtLastReport >= 0) {
                    const long long played =
                        (written - writtenAtLastReport) + (padAtLastReport - padNow);
                    playedRate = static_cast<double>(played) / windowSeconds;
                }
                padAtLastReport = padNow;

                writtenAtLastReport = written;
                secondsAtLastReport = s.seconds;
                if (opt.onStats) {
                    opt.onStats(s);
                }
                char line[256];
                std::snprintf(line, sizeof(line),
                              "[%5.1f s] mean=%6d corr=%+7.1f ptp=%+7.1f written=%.0f Hz "
                              "played=%.0f Hz devpad=%d",
                              s.seconds, s.buffered, s.correctionPpm, s.ptpPpm, s.outRateHz,
                              playedRate, padNow);
                logTo(opt, line);
            }
            rateCtl.resetWindowStats();
        }
    }

    ptpRun.store(false);
    if (ptpThread.joinable()) {
        ptpThread.join();
    }
    receiver.stop();
    sink.reset();

    RxSnapshot finalSnap;
    if (ringDropped > 0) {
        logTo(opt, "ring: dropped " + std::to_string(ringDropped) +
                       " frames because nothing was reading it");
    }

    finalSnap.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    finalSnap.buffered = static_cast<int>(rateCtl.meanBufferedLifetime());
    finalSnap.target = static_cast<int>(rateCtl.targetFrames());
    finalSnap.correctionPpm = rateCtl.correctionPpm();
    finalSnap.effectivePpm = rateCtl.effectivePpm();
    finalSnap.ptpPpm = fbInit ? fbPpm : 0.0;
    finalSnap.packets = receiver.stats().packets;
    finalSnap.lost = receiver.jitter().lost();
    finalSnap.late = receiver.jitter().late();
    finalSnap.wrongSsrc = receiver.stats().wrongSsrc;
    finalSnap.ssrcChanges = receiver.stats().ssrcChanges;
    finalSnap.underruns = static_cast<int>(underruns);
    finalSnap.framesWritten = written;
    finalSnap.outRateHz = finalSnap.seconds > 0.0
                              ? static_cast<double>(written) / finalSnap.seconds
                              : 0.0;
    if (finalSnapshot) {
        *finalSnapshot = finalSnap;
    }
    return true;
}

} // namespace openaes67
