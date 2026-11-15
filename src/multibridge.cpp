// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/multibridge.h"
#include "openaes67/engine.h"
#include "openaes67/shared_audio_ring.h"
#include "openaes67/text_encoding.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

namespace openaes67 {
namespace {
bool fail(std::string *err, const std::string &message) {
    if (err) *err = message;
    return false;
}
bool number(const std::string &s, int lo, int hi, int *out) {
    if (s.empty() || s.size() > 5) return false;
    int value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + c - '0';
    }
    if (value < lo || value > hi) return false;
    *out = value;
    return true;
}
// RFC 3550 timestamps are modulo 2^32; avoid implementation-defined signed casts.
int64_t delta(uint32_t a, uint32_t b) {
    const uint32_t d = a - b;
    return d < 0x80000000u ? int64_t(d) : int64_t(d) - 0x100000000LL;
}
bool validate(const std::vector<AudioRoute> &routes, std::string *err) {
    if (routes.empty()) return fail(err, "route table is empty");
    std::array<bool, 64> occupied{};
    std::set<std::pair<std::string, uint16_t>> endpoints;
    // Windows multicast sockets bound to the same port can receive one another's
    // groups. Give every route its own port so packets cannot cross routes.
    std::set<uint16_t> ports;
    int rx = 0, tx = 0;
    for (const auto &r : routes) {
        if (!isMulticastIPv4(r.group) || r.port == 0 || r.channels < 1 ||
            r.channels > 8 || r.asioFirst < 0 || r.asioFirst > 64 - r.channels ||
            (r.codec != Codec::L16 && r.codec != Codec::L24) || r.payloadType < 96 ||
            r.payloadType > 127)
            return fail(err, "invalid multicast endpoint, channel range, codec or payload type");
        if (!endpoints.insert({r.group, r.port}).second || !ports.insert(r.port).second)
            return fail(err, "each route must use a distinct UDP port (including tx routes)");
        if (r.receive) {
            if (++rx > 8) return fail(err, "at most 8 rx routes are supported");
            for (int c = r.asioFirst; c < r.asioFirst + r.channels; ++c) {
                if (occupied[c]) return fail(err, "overlapping rx ASIO channels");
                occupied[c] = true;
            }
        } else if (++tx > 8) return fail(err, "at most 8 tx routes are supported");
    }
    return true;
}
} // namespace

bool parseAudioRoutes(const std::string &text, std::vector<AudioRoute> *routes,
                      std::string *err) {
    if (!routes) return fail(err, "null route output");
    std::vector<AudioRoute> parsed;
    std::istringstream input(decodeTextToUtf8(text));
    std::string line;
    int lineNo = 0;
    while (std::getline(input, line)) {
        ++lineNo;
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        std::istringstream row(line);
        std::string direction, port, channels, first, codec, pt, extra;
        AudioRoute route;
        if (!(row >> direction)) continue;
        int p = 0, n = 0, start = 0, payload = 0;
        if (!(row >> route.group >> port >> channels >> first >> codec >> pt) ||
            (row >> extra) || (direction != "rx" && direction != "tx") ||
            !number(port, 1, 65535, &p) || !number(channels, 1, 8, &n) ||
            !number(first, 1, 64, &start) || !number(pt, 96, 127, &payload) ||
            (codec != "L16" && codec != "L24"))
            return fail(err, "line " + std::to_string(lineNo) +
                        ": expected rx|tx GROUP PORT CHANNELS ASIO_FIRST L16|L24 PT");
        route.receive = direction == "rx";
        route.port = static_cast<uint16_t>(p);
        route.channels = n;
        route.asioFirst = start - 1;
        route.codec = codec == "L24" ? Codec::L24 : Codec::L16;
        route.payloadType = static_cast<uint8_t>(payload);
        parsed.push_back(route);
        if (!validate(parsed, err)) {
            if (err) *err = "line " + std::to_string(lineNo) + ": " + *err;
            return false;
        }
    }
    if (!validate(parsed, err)) return false;
    *routes = std::move(parsed); // leave caller's previous preset intact on error
    return true;
}

bool loadAudioRoutes(const std::string &path, std::vector<AudioRoute> *routes,
                     std::string *err) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) return fail(err, "cannot open route file");
    file.seekg(0, std::ios::end);
    if (file.tellg() < 0 || file.tellg() > 64 * 1024)
        return fail(err, "route file exceeds 64 KiB or cannot be read");
    file.seekg(0);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.size() > 64 * 1024) return fail(err, "route file exceeds 64 KiB");
    return parseAudioRoutes(text, routes, err);
}

RoutedPlayout::RoutedPlayout(const std::vector<AudioRoute> &routes) : routes_(routes) {
    std::string err;
    if (!validate(routes_, &err)) { routes_.clear(); return; }
    slots_.resize(routes_.size());
    for (size_t i = 0; i < routes_.size(); ++i)
        if (routes_[i].receive) slots_[i].resize(kWindow);
}
void RoutedPlayout::reset() {
    for (size_t i = 0; i < slots_.size(); ++i) clearRoute(i);
    anchored_ = false;
    next_ = 0;
    latestEnd_ = 0;
    fraction_ = 0;
}
void RoutedPlayout::clearRoute(size_t route) {
    if (route < slots_.size()) for (auto &s : slots_[route]) s.valid = false;
}
bool RoutedPlayout::push(size_t route, uint32_t timestamp, const int16_t *samples) {
    if (route >= routes_.size() || !routes_[route].receive || !samples) return false;
    if (!anchored_) { next_ = timestamp; latestEnd_ = timestamp; anchored_ = true; }
    const int64_t offset = delta(timestamp, next_);
    if (offset < 0 || offset + 48 > kWindow) return false;
    if (delta(timestamp + 48, latestEnd_) > 0) latestEnd_ = timestamp + 48;
    const int channels = routes_[route].channels;
    // A power-of-two window remains collision-free across the 2^32 wrap.
    for (uint32_t f = 0; f < 48; ++f) {
        const uint32_t tag = timestamp + f;
        auto &slot = slots_[route][tag & (kWindow - 1)];
        if (slot.valid && slot.tag == tag) continue; // first duplicate wins
        slot.tag = tag;
        slot.valid = true;
        std::copy_n(samples + f * channels, channels, slot.samples.begin());
    }
    return true;
}
uint32_t RoutedPlayout::pop(int16_t *out) {
    return popFrames(out, 48, 0);
}
int RoutedPlayout::bufferedFrames() const {
    return anchored_ ? static_cast<int>(std::max(int64_t(0), delta(latestEnd_, next_))) : 0;
}
uint32_t RoutedPlayout::popFrames(int16_t *out, int frames, double ppm) {
    if (!out || frames < 0 || frames > 2400) return 0;
    std::fill_n(out, frames * 64, int16_t(0));
    const double ratio = 1.0 + (std::isfinite(ppm) ? std::clamp(ppm, -2000.0, 2000.0) : 0.0) * 1e-6;
    uint32_t missing = 0;
    for (int f = 0; f < frames; ++f) {
        for (size_t i = 0; i < routes_.size(); ++i) {
            const auto &r = routes_[i];
            if (!r.receive) continue;
            const auto &a = slots_[i][next_ & (kWindow - 1)];
            const auto &b = slots_[i][(next_ + 1) & (kWindow - 1)];
            if (!anchored_ || !a.valid || a.tag != next_ ||
                (fraction_ != 0 && (!b.valid || b.tag != next_ + 1))) {
                missing += r.channels;
                continue;
            }
            for (int c = 0; c < r.channels; ++c) {
                const double value = a.samples[c] + (b.samples[c] - a.samples[c]) * fraction_;
                out[f * 64 + r.asioFirst + c] = static_cast<int16_t>(std::lround(value));
            }
        }
        if (!anchored_) continue;
        fraction_ += ratio;
        const int consume = static_cast<int>(fraction_);
        fraction_ -= consume;
        for (int n = 0; n < consume; ++n) {
            for (auto &routeSlots : slots_) {
                if (!routeSlots.empty()) routeSlots[next_ & (kWindow - 1)].valid = false;
            }
            ++next_;
        }
    }
    return missing;
}

void PlayoutRateServo::reset() {
    depthSeconds_ = seconds_ = integral_ = correction_ = 0;
}
void PlayoutRateServo::observe(int depth, double dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1 || depth < 0 || depth > 8192) {
        reset(); return;
    }
    depthSeconds_ += depth * dt;
    seconds_ += dt;
    if (seconds_ < 0.128) return;
    const double error = depthSeconds_ / seconds_ - 1440.0; // 30 ms at 48 kHz
    // Integrate only the excess outside a half-packet deadband. Freeze at the
    // correction bound rather than winding up during an unrecoverable outage.
    const double excess = error > 24 ? error - 24 : (error < -24 ? error + 24 : 0);
    const double candidate = std::clamp(integral_ + 0.4 * excess * seconds_, -2000.0, 2000.0);
    const double correction = 4.0 * excess + candidate;
    if (std::fabs(correction) <= 2000 || correction * excess < 0) integral_ = candidate;
    correction_ = std::clamp(4.0 * excess + integral_, -2000.0, 2000.0);
    depthSeconds_ = seconds_ = 0;
}

bool runMultiBridge(const MultiBridgeOptions &opt, MultiBridgeStats *result,
                    std::string *err) {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    if (!validate(opt.routes, err)) return false;
    if (!std::isfinite(opt.seconds) || opt.seconds < 0 || opt.shmName.empty() ||
        opt.renderQueueMs < 5 || opt.renderQueueMs > 50)
        return fail(err, "invalid duration, shared-memory name or render queue (5..50 ms)");
    Winsock winsock;
    if (!winsock.ok()) return fail(err, "Winsock initialization failed");
    // A second multi-stream pump must never become another SPSC ring writer.
    const std::wstring ownerName = opt.shmName + L".MultiBridgeOwner";
    HANDLE owner = CreateMutexW(nullptr, FALSE, ownerName.c_str());
    if (!owner) return fail(err, "cannot create bridge ownership mutex");
    struct OwnerGuard {
        HANDLE handle;
        bool held = false;
        ~OwnerGuard() { if (held) ReleaseMutex(handle); CloseHandle(handle); }
    } guard{owner};
    const DWORD wait = WaitForSingleObject(owner, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        return fail(err, "another multibridge owns this shared section");
    guard.held = true;
    auto ring = openSharedAudio(opt.shmName.c_str(), err);
    if (!ring) {
        SharedAudioConfig cfg;
        cfg.channels = 64; cfg.sampleRate = 48000; cfg.capacityFrames = 48000;
        ring = createSharedAudio(opt.shmName.c_str(), cfg, err);
    }
    if (!ring) return false;
    if (ring->channels() != 64 || ring->sampleRate() != 48000 ||
        ring->header()->frameBytes != 128)
        return fail(err, "multibridge requires a 64-channel 48000 Hz int16 shared section");
    if (ring->capacityFrames() < static_cast<uint32_t>(48 * opt.renderQueueMs))
        return fail(err, "shared section is smaller than the requested render queue");
    struct Flow {
        std::unique_ptr<UdpSocket> rx;
        std::unique_ptr<RtpSender> tx;
        bool locked = false;
        uint32_t ssrc = 0;
        Clock::time_point last{};
    };
    std::vector<Flow> flows(opt.routes.size());
    bool hasRx = false, hasTx = false;
    for (size_t i = 0; i < flows.size(); ++i) {
        const auto &r = opt.routes[i];
        auto &flow = flows[i];
        if (r.receive) {
            hasRx = true;
            flow.rx = std::make_unique<UdpSocket>();
            if (!flow.rx->openReceiver(r.group, r.port, opt.ifaceIp, err)) return false;
        } else {
            hasTx = true;
            StreamConfig cfg;
            cfg.group = r.group; cfg.port = r.port; cfg.channels = r.channels;
            cfg.sampleRate = 48000; cfg.ptimeMs = 1; cfg.codec = r.codec;
            cfg.payloadType = r.payloadType; cfg.ifaceIp = opt.ifaceIp;
            flow.tx = std::make_unique<RtpSender>(cfg, 0);
            if (!flow.tx->start(err)) return false;
        }
    }
    MultiBridgeStats stats;
    RoutedPlayout playout(opt.routes);
    PlayoutRateServo servo;
    std::array<uint8_t, 65536> packet{};
    std::array<int16_t, 48 * 8> samples{};
    std::vector<int16_t> render(2400 * 64); // allocated once, never in the pump loop
    std::array<int16_t, 48 * 64> capture{};
    const auto start = Clock::now();
    auto deadline = start, lastAccepted = start, readyAt = start;
    auto lastHostRead = start, lastControl = start, nextReport = start + 1s;
    const auto readPosition = [&] {
        return static_cast<uint32_t>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&ring->header()->readIndex), 0, 0));
    };
    uint32_t previousRead = readPosition();
    // Bound the render queue, rather than filling a one-second ring
    // while the host is stopped. Only the driver owns its read index.
    const int renderTarget = 48 * opt.renderQueueMs;
    // All tx routes share one epoch; this is a local clock, not a PTP claim.
    uint32_t txTimestamp = static_cast<uint32_t>(start.time_since_epoch().count());
    while ((!opt.shouldStop || !opt.shouldStop()) &&
           (opt.seconds == 0 || std::chrono::duration<double>(Clock::now() - start).count() < opt.seconds)) {
        const auto now = Clock::now();
        if (hasRx) {
            const uint32_t read = readPosition();
            if (read != previousRead) {
                if (!stats.hostActive) {
                    playout.reset(); servo.reset();
                    for (auto &f : flows) f.locked = false;
                    ++stats.hostResumes;
                }
                stats.hostActive = true;
                previousRead = read; lastHostRead = now;
            } else if (stats.hostActive && now - lastHostRead >= 250ms) {
                stats.hostActive = false;
                playout.reset(); servo.reset();
                ++stats.hostPauses;
            }
        }
        if (playout.anchored() && now - lastAccepted >= 1s) {
            playout.reset();
            servo.reset();
            for (auto &f : flows) f.locked = false;
            ++stats.restarts;
        }
        for (size_t i = 0; i < flows.size(); ++i) {
            auto &flow = flows[i];
            if (!flow.rx) continue;
            const auto &r = opt.routes[i];
            const size_t payloadBytes = size_t(48 * r.channels) * (r.codec == Codec::L24 ? 3 : 2);
            // Bound work per tick, including malformed traffic, to protect other routes.
            for (int n = 0; n < 64; ++n) {
                const int bytes = flow.rx->recvAvailable(packet.data(), packet.size());
                if (bytes < 0) { if (result) *result = stats; return fail(err, "multibridge receive failed"); }
                if (bytes == 0) break;
                RtpHeader h;
                if (size_t(bytes) != kRtpHeaderSize + payloadBytes || (packet[0] & 0x3fu) ||
                    !readRtpHeader(packet.data(), size_t(bytes), &h) || h.payloadType != r.payloadType) {
                    ++stats.rejectedPackets; continue;
                }
                if (!stats.hostActive) { ++stats.waitingHostPackets; continue; }
                if (flow.locked && h.ssrc != flow.ssrc) {
                    if (now - flow.last < 1s) { ++stats.rejectedPackets; continue; }
                    playout.clearRoute(i);
                    flow.locked = false;
                }
                unpackPayload(r.codec, packet.data() + kRtpHeaderSize, payloadBytes,
                              samples.data(), samples.size());
                const bool wasAnchored = playout.anchored();
                if (!playout.push(i, h.timestamp, samples.data())) {
                    ++stats.timestampRejects; continue;
                }
                if (!wasAnchored) readyAt = now + std::chrono::milliseconds(std::max(30, opt.renderQueueMs));
                flow.locked = true; flow.ssrc = h.ssrc; flow.last = now;
                lastAccepted = now;
                ++stats.rxPackets;
            }
        }
        if (hasRx) {
            const int wanted = std::clamp(renderTarget - ring->renderPending(), 0, renderTarget);
            const bool playing = stats.hostActive && playout.anchored() && now >= readyAt;
            if (playing) {
                if (now - lastAccepted < 100ms)
                    servo.observe(playout.bufferedFrames(), std::chrono::duration<double>(now - lastControl).count());
                else servo.reset(); // a source outage is not clock drift
                stats.missingChannelFrames += playout.popFrames(render.data(), wanted, servo.correctionPpm());
            } else {
                std::fill_n(render.data(), wanted * 64, int16_t(0));
            }
            stats.renderDroppedFrames += wanted - ring->writeRender(render.data(), wanted);
            stats.renderPendingFrames = ring->renderPending();
            stats.rxBufferedFrames = playout.bufferedFrames();
            stats.rxCorrectionPpm = servo.correctionPpm();
        }
        lastControl = now;
        const int got = hasTx ? ring->readCapture(capture.data(), 48) : 48;
        std::fill(capture.begin() + got * 64, capture.end(), int16_t(0));
        stats.captureMissingFrames += 48 - got;
        for (size_t i = 0; i < flows.size(); ++i) {
            if (!flows[i].tx) continue;
            const auto &r = opt.routes[i];
            for (int f = 0; f < 48; ++f)
                std::copy_n(capture.data() + f * 64 + r.asioFirst, r.channels,
                            samples.data() + f * r.channels);
            if (flows[i].tx->sendFramesAt(txTimestamp, samples.data(), 48)) ++stats.txPackets;
            else ++stats.txErrors;
        }
        txTimestamp += 48;
        deadline += 1ms;
        const auto after = Clock::now();
        if (after - deadline > 5ms) {
            // Drop the missed wall-clock intervals instead of sending an unbounded burst.
            const auto missed = std::chrono::duration_cast<std::chrono::milliseconds>(after - deadline).count();
            txTimestamp += static_cast<uint32_t>(missed * 48);
            deadline += std::chrono::milliseconds(missed);
            playout.reset(); servo.reset(); ++stats.scheduleResets;
        }
        if (opt.report && after >= nextReport) {
            opt.report(stats);
            nextReport = after + 1s;
        }
        std::this_thread::sleep_until(deadline);
    }
    if (result) *result = stats;
    return true;
}
} // namespace openaes67
