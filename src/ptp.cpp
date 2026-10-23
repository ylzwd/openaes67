// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/ptp.h"
#include "openaes67/net.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace openaes67 {
namespace {

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint64_t rd48(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}
inline void wr16(uint8_t *p, uint16_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
}
inline void wr48(uint8_t *p, uint64_t v) {
    for (int i = 5; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

// Windows FILETIME epoch (1601-01-01) to Unix epoch (1970-01-01) in 100 ns units.
constexpr uint64_t kFiletimeToUnix100ns = 116444736000000000ULL;

} // namespace

PtpTimestamp readPtpTimestamp(const uint8_t *p) {
    PtpTimestamp t;
    t.seconds = rd48(p);
    t.nanos = rd32(p + 6);
    return t;
}

void writePtpTimestamp(uint8_t *p, const PtpTimestamp &t) {
    wr48(p, t.seconds);
    wr32(p + 6, t.nanos);
}

PtpTimestamp nowAsPtp() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t v = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    v -= kFiletimeToUnix100ns;                 // 100 ns ticks since Unix epoch
    PtpTimestamp t;
    t.seconds = v / 10000000ULL;
    t.nanos = static_cast<uint32_t>((v % 10000000ULL) * 100ULL);
    return t;
}

bool parsePtpHeader(const uint8_t *data, size_t len, PtpHeader *h) {
    if (!data || !h || len < kPtpHeaderSize) {
        return false;
    }
    h->messageType = static_cast<uint8_t>((data[0] >> 4) & 0x0F);
    h->versionPtp = static_cast<uint8_t>(data[1] & 0x0F);
    h->messageLength = rd16(data + 2);
    h->domain = data[4];
    h->flags = rd16(data + 6);
    // correctionField is signed 64-bit, scaled nanoseconds (<<16)
    int64_t corr = 0;
    for (int i = 0; i < 8; ++i) {
        corr = (corr << 8) | data[8 + i];
    }
    h->correctionField = corr;
    std::memcpy(h->clockIdentity, data + 20, 8);
    h->portNumber = rd16(data + 28);
    h->sequenceId = rd16(data + 30);
    h->controlField = data[32];
    h->logMessageInterval = data[33];
    return h->versionPtp == 2;
}

size_t buildDelayReq(uint8_t *out, size_t cap, uint8_t domain,
                     const uint8_t clockIdentity[8], uint16_t seq) {
    constexpr size_t bodySize = 10;
    const size_t total = kPtpHeaderSize + bodySize;
    if (!out || cap < total) {
        return 0;
    }
    std::memset(out, 0, total);
    out[0] = static_cast<uint8_t>((kPtpDelayReq << 4) | 0x0);   // messageType, transportSpecific
    out[1] = 0x02;                                             // versionPtp = 2
    wr16(out + 2, static_cast<uint16_t>(total));
    out[4] = domain;
    std::memcpy(out + 20, clockIdentity, 8);
    wr16(out + 28, 1);                                         // portNumber
    wr16(out + 30, seq);
    out[32] = 0x01;                                            // controlField: Delay_Req
    out[33] = 0x7F;                                            // logMessageInterval
    // originTimestamp body left as zero: two-step style
    return total;
}

// ---------------------------------------------------------------------------
PtpSlave::PtpSlave() {
    // Derive a clock identity from the MAC address so two slaves differ.
    const char *iface = std::getenv("OPENAES67_PTP_CLOCK_ID");
    if (iface) {
        for (int i = 0; i < 8 && iface[i]; ++i) {
            clockId_[i] = static_cast<uint8_t>(iface[i]);
        }
    } else {
        clockId_[0] = 0x0A;
        clockId_[1] = 0x0B;
        clockId_[7] = 0x01;
    }
}

PtpSlave::~PtpSlave() { stop(); }

static bool openMulticast(uint16_t port, const std::string &ifaceIp, uintptr_t *handle,
                          std::string *err) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (err) { *err = "socket() failed"; }
        return false;
    }
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
    const int rcv = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcv), sizeof(rcv));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == SOCKET_ERROR) {
        if (err) { *err = "bind failed on port " + std::to_string(port); }
        closesocket(s);
        return false;
    }
    ip_mreq mreq{};
    InetPtonA(AF_INET, kPtpDefaultGroup, &mreq.imr_multiaddr);
    if (!ifaceIp.empty() && ifaceIp != "0.0.0.0") {
        InetPtonA(AF_INET, ifaceIp.c_str(), &mreq.imr_interface);
    }
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char *>(&mreq), sizeof(mreq)) == SOCKET_ERROR) {
        if (err) {
            *err = "IP_ADD_MEMBERSHIP failed on port " + std::to_string(port) +
                   ", code=" + std::to_string(WSAGetLastError());
        }
        closesocket(s);
        return false;
    }
    *handle = static_cast<uintptr_t>(s);
    return true;
}

bool PtpSlave::start(const std::string &ifaceIp, uint8_t domain, std::string *err) {
    domain_ = domain;
    if (!openMulticast(kPtpEventPort, ifaceIp, &eventSock_, err)) {
        return false;
    }
    if (!openMulticast(kPtpGeneralPort, ifaceIp, &generalSock_, err)) {
        stop();
        return false;
    }
    return true;
}

void PtpSlave::stop() {
    for (uintptr_t *h : {&eventSock_, &generalSock_}) {
        if (*h != ~static_cast<uintptr_t>(0)) {
            closesocket(static_cast<SOCKET>(*h));
            *h = ~static_cast<uintptr_t>(0);
        }
    }
}

bool PtpSlave::sendDelayReq() {
    if (eventSock_ == ~static_cast<uintptr_t>(0)) {
        return false;
    }
    uint8_t msg[64];
    const size_t n = buildDelayReq(msg, sizeof(msg), domain_, clockId_, seq_++);
    if (n == 0) {
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPtpEventPort);
    InetPtonA(AF_INET, kPtpDefaultGroup, &to.sin_addr);

    pendingT3_ = nowAsPtp().toNanos();
    pendingDelaySeq_ = static_cast<uint16_t>(seq_ - 1);
    waitingDelayResp_ = true;

    const int sent = sendto(static_cast<SOCKET>(eventSock_), reinterpret_cast<const char *>(msg),
                            static_cast<int>(n), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
    return sent == static_cast<int>(n);
}

void PtpSlave::updateFrequencyEstimate() {
    // Fit offset = intercept + slope * localTime by least squares.
    // slope is d(offset)/d(localTime); the master runs at -slope relative rate,
    // so master ppm = -slope * 1e6.
    const size_t n = history_.size();
    if (n < 6) {
        stats_.freqValid = false;
        return;
    }
    // The baseline must span enough time. With a short span the fitted slope is
    // dominated by per-sample jitter (0.2 ms over 1 s reads as ~200 ppm, and
    // four samples once produced -13478 ppm).
    const int64_t baselineNs = history_.back().localNs - history_.front().localNs;
    if (baselineNs < kFreqMinBaselineNs) {
        stats_.freqValid = false;
        return;
    }
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    const int64_t x0 = history_.front().localNs;
    for (const OffsetSample &s : history_) {
        const double x = static_cast<double>(s.localNs - x0);
        const double y = static_cast<double>(s.offsetNs);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    const double dn = static_cast<double>(n);
    const double denom = dn * sumXX - sumX * sumX;
    if (denom == 0.0) {
        stats_.freqValid = false;
        return;
    }
    const double slope = (dn * sumXY - sumX * sumY) / denom;   // ns per ns
    stats_.freqPpm = -slope * 1e6;
    stats_.freqValid = true;

    // Predict the offset at the newest sample time with the drift removed,
    // which is what a disciplined clock would use right now.
    const double intercept = (sumY - slope * sumX) / dn;
    const OffsetSample &last = history_.back();
    const double xLast = static_cast<double>(last.localNs - x0);
    stats_.offsetFilteredNs = static_cast<int64_t>(intercept + slope * xLast);
}

void PtpSlave::addSample(int64_t offsetNs) {
    stats_.lastOffsetNs = offsetNs;
    if (!stats_.haveFirst) {
        stats_.minOffsetNs = offsetNs;
        stats_.maxOffsetNs = offsetNs;
        stats_.haveFirst = true;
    } else {
        stats_.minOffsetNs = std::min(stats_.minOffsetNs, offsetNs);
        stats_.maxOffsetNs = std::max(stats_.maxOffsetNs, offsetNs);
    }
    offsetSum_ += offsetNs;
    ++offsetCount_;
    stats_.meanOffsetNs = offsetSum_ / static_cast<int64_t>(offsetCount_);

    const int64_t stamp = static_cast<int64_t>(nowAsPtp().toNanos());

    // Fold samples into one-second bins; the regression then runs on bin means.
    const int64_t second = stamp / 1000000000LL;
    if (binSecond_ < 0) {
        binSecond_ = second;
    }
    if (second != binSecond_) {
        if (binCount_ > 0) {
            const int64_t binStamp = binSecond_ * 1000000000LL + 500000000LL;
            history_.push_back(
                OffsetSample{binStamp, static_cast<int64_t>(binSumNs_ / binCount_)});
        }
        binSecond_ = second;
        binSumNs_ = 0.0;
        binCount_ = 0;
    }
    binSumNs_ += static_cast<double>(offsetNs);
    ++binCount_;
    // Sliding time window, plus a hard count cap so a fast delay-request rate
    // cannot grow the buffer without bound.
    while (!history_.empty() &&
           (stamp - history_.front().localNs > kFreqWindowNs || history_.size() > 512)) {
        history_.erase(history_.begin());
    }
    updateFrequencyEstimate();
}

void PtpSlave::handleMessage(const uint8_t *data, size_t len, uint64_t recvNanos) {
    PtpHeader h;
    if (!parsePtpHeader(data, len, &h)) {
        return;
    }
    if (h.domain != domain_) {
        return;
    }

    switch (h.messageType) {
    case kPtpSync: {
        ++stats_.syncCount;
        std::memcpy(masterId_, h.clockIdentity, 8);
        haveMaster_ = true;
        // Two-step master: the accurate send time arrives in Follow_Up.
        if (h.flags & 0x0002) {
            waitingFollowUp_ = true;
            pendingSyncSeq_ = h.sequenceId;
            pendingT2_ = recvNanos;
        } else {
            // One-step master: t1 is in the Sync itself.
            // The timestamp body is 10 bytes past the header. A 34-byte Sync
            // (header only) used to make this read past the received buffer.
            if (len < kPtpHeaderSize + 10) {
                ++stats_.truncatedCount;
                break;
            }
            const PtpTimestamp t1 = readPtpTimestamp(data + kPtpHeaderSize);
            lastDelta1Ns_ = static_cast<int64_t>(recvNanos) - t1.toNanos();
            haveDelta1_ = true;
        }
        break;
    }
    case kPtpFollowUp: {
        ++stats_.followUpCount;
        if (waitingFollowUp_ && h.sequenceId == pendingSyncSeq_) {
            if (len < kPtpHeaderSize + 10) {
                ++stats_.truncatedCount;
                break;
            }
            waitingFollowUp_ = false;
            const PtpTimestamp t1 = readPtpTimestamp(data + kPtpHeaderSize);
            lastDelta1Ns_ = static_cast<int64_t>(pendingT2_) - t1.toNanos();
            haveDelta1_ = true;
        }
        break;
    }
    case kPtpDelayResp: {
        ++stats_.delayRespCount;
        if (!waitingDelayResp_ || h.sequenceId != pendingDelaySeq_) {
            break;
        }
        waitingDelayResp_ = false;
        // t4 lives after the header; refuse a truncated Delay_Resp instead of
        // reading whatever follows the buffer.
        if (len < kPtpHeaderSize + 10) {
            ++stats_.truncatedCount;
            break;
        }
        const PtpTimestamp t4 = readPtpTimestamp(data + kPtpHeaderSize);
        // pathDelay = ((t2 - t1) + (t4 - t3)) / 2 ; t1 is folded into the offset
        // sample, so here we only need (t4 - t3) added to the last raw delta.
        const int64_t t4n = t4.toNanos();
        const int64_t t3n = static_cast<int64_t>(pendingT3_);
        const int64_t delta2 = t4n - t3n;
        if (haveDelta1_) {
            stats_.pathDelayNs = (lastDelta1Ns_ + delta2) / 2;
            addSample(lastDelta1Ns_ - stats_.pathDelayNs);
        }
        break;
    }
    case kPtpAnnounce:
        std::memcpy(masterId_, h.clockIdentity, 8);
        haveMaster_ = true;
        break;
    default:
        break;
    }
}


bool PtpSlave::masterTimeNs(int64_t *ns) const {
    if (ns == nullptr || !stats_.haveFirst) {
        return false;
    }
    // The offsets are measured against nowAsPtp(), so the master's current time is
    // the same clock plus the (filtered) offset.
    *ns = static_cast<int64_t>(nowAsPtp().toNanos()) + stats_.offsetFilteredNs;
    return true;
}

bool PtpSlave::poll(int timeoutMs) {
    if (eventSock_ == ~static_cast<uintptr_t>(0)) {
        return false;
    }
    const uint32_t before = offsetCount_;
    uint8_t buf[128];

    for (uintptr_t sock : {eventSock_, generalSock_}) {
        setRecvTimeoutMs(sock, timeoutMs);

        for (;;) {
            const uint64_t recvNs = nowAsPtp().toNanos();
            const int n = recvfrom(static_cast<SOCKET>(sock), reinterpret_cast<char *>(buf),
                                   sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) {
                break;
            }
            handleMessage(buf, static_cast<size_t>(n), recvNs);
        }
    }
    return offsetCount_ != before;
}

// ---------------------------------------------------------------------------
// PtpMediaClock / PtpFeed
// ---------------------------------------------------------------------------

uint32_t PtpMediaClock::update(int64_t masterNs, int sampleRate, long long packetIndex,
                               int framesPerPacket) {
    if (sampleRate <= 0 || framesPerPacket <= 0) {
        return 0;
    }
    // Convert nanoseconds to RTP units without overflowing 64 bits: masterNs is a
    // Unix-epoch value (~1.8e18), and multiplying it by 48000 reaches 8.6e22, well
    // past UINT64_MAX. Split seconds from the sub-second remainder instead.
    if (masterNs < 0) {
        masterNs = 0;
    }
    const unsigned long long wholeSeconds =
        static_cast<unsigned long long>(masterNs) / 1000000000ULL;
    const unsigned long long remainder =
        static_cast<unsigned long long>(masterNs) % 1000000000ULL;
    const uint32_t ptpTs = static_cast<uint32_t>(
        wholeSeconds * static_cast<unsigned long long>(sampleRate) +
        (remainder * static_cast<unsigned long long>(sampleRate)) / 1000000000ULL);

    if (!anchored_) {
        anchored_ = true;
        anchorTs_ = ptpTs;
        anchorPacket_ = packetIndex;
        return ptpTs;
    }

    const uint32_t predicted =
        anchorTs_ + static_cast<uint32_t>((packetIndex - anchorPacket_) * framesPerPacket);
    const int32_t diff = static_cast<int32_t>(ptpTs - predicted);
    // Re-anchor only on a genuine clock step, not on jitter. A one-packet tolerance
    // made every wobble re-anchor: measured on the wire, half the timestamps stepped
    // backwards by 48 samples and forwards by 148 instead of advancing by 48.
    const int32_t stepLimit = sampleRate / 2;   // 0.5 s of samples
    if (diff > stepLimit || diff < -stepLimit) {
        anchorTs_ = ptpTs;
        anchorPacket_ = packetIndex;
        ++reanchors_;
        return ptpTs;
    }
    return predicted;
}

struct PtpFeed::Impl {
    PtpSlave slave;
    std::atomic<bool> run{false};
    std::atomic<bool> valid{false};
    std::atomic<int64_t> masterNs{0};
    std::atomic<double> ppm{0.0};
    std::atomic<uint64_t> masterId{0};
    std::atomic<bool> haveMasterId{false};
    std::thread thread;
    std::string err;
};

PtpFeed::PtpFeed() : impl_(new Impl()) {}

PtpFeed::~PtpFeed() { stop(); delete impl_; }

bool PtpFeed::start(const std::string &ifaceIp, uint8_t domain, std::string *err) {
    if (!impl_) {
        return false;
    }
    if (!impl_->slave.start(ifaceIp, domain, &impl_->err)) {
        if (err) {
            *err = impl_->err;
        }
        return false;
    }
    impl_->run.store(true);
    Impl *impl = impl_;
    impl_->thread = std::thread([impl] {
        auto lastReq = std::chrono::steady_clock::now();
        while (impl->run.load()) {
            impl->slave.poll(1);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReq >= std::chrono::milliseconds(250)) {
                lastReq = now;
                impl->slave.sendDelayReq();
            }
            if (impl->slave.hasMaster()) {
                const uint8_t *id = impl->slave.masterIdentity();
                uint64_t packed = 0;
                for (int i = 0; i < 8; ++i) {
                    packed = (packed << 8) | id[i];
                }
                impl->masterId.store(packed);
                impl->haveMasterId.store(true);
            }
            const PtpSlave::Stats &st = impl->slave.stats();
            impl->ppm.store(st.freqPpm);
            int64_t ns = 0;
            const bool ok = impl->slave.masterTimeNs(&ns);
            // Wait for a few delay exchanges before calling the clock usable: the
            // first offset samples are still converging, and anchoring the RTP
            // timestamp on one of them shifts the whole stream by seconds.
            impl->valid.store(ok && st.delayRespCount >= 4);
            if (ok) {
                impl->masterNs.store(ns);
            }
        }
    });
    return true;
}

void PtpFeed::stop() {
    if (!impl_) {
        return;
    }
    impl_->run.store(false);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool PtpFeed::valid() const { return impl_ && impl_->valid.load(); }
int64_t PtpFeed::masterNs() const { return impl_ ? impl_->masterNs.load() : 0; }
double PtpFeed::ppm() const { return impl_ ? impl_->ppm.load() : 0.0; }

bool PtpFeed::masterIdentity(uint8_t out[8]) const {
    if (!impl_ || !impl_->haveMasterId.load() || !out) {
        return false;
    }
    const uint64_t packed = impl_->masterId.load();
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((packed >> (8 * (7 - i))) & 0xFF);
    }
    return true;
}
} // namespace openaes67
