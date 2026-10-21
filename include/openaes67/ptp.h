#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include <cstring>
#include <string>
#include <vector>

namespace openaes67 {

// PTPv2 (IEEE 1588-2008) message types we care about.
enum PtpMessageType : uint8_t {
    kPtpSync      = 0x0,
    kPtpDelayReq  = 0x1,
    kPtpFollowUp  = 0x8,
    kPtpDelayResp = 0x9,
    kPtpAnnounce  = 0xB,
};

constexpr size_t kPtpHeaderSize = 34;
constexpr uint16_t kPtpEventPort   = 319;   // Sync, Delay_Req
constexpr uint16_t kPtpGeneralPort = 320;   // Follow_Up, Delay_Resp, Announce
constexpr const char *kPtpDefaultGroup = "224.0.1.129";

// Frequency estimation uses a sliding TIME window, not a sample count: the
// delay-request rate is configurable, so "last 256 samples" means anything from
// seconds to minutes. A short baseline makes the fitted slope explode (one run
// reported -13478 ppm from four samples).
constexpr int64_t kFreqWindowNs      = 20LL * 1000000000LL;   // 20 s of history
constexpr int64_t kFreqMinBaselineNs =  8LL * 1000000000LL;   // need >= 8 s span

// A PTP timestamp: 48-bit seconds + 32-bit nanoseconds, both big-endian.
struct PtpTimestamp {
    uint64_t seconds = 0;
    uint32_t nanos = 0;

    int64_t toNanos() const {
        return static_cast<int64_t>(seconds) * 1000000000LL + static_cast<int64_t>(nanos);
    }
    static PtpTimestamp fromNanos(int64_t ns) {
        PtpTimestamp t;
        int64_t s = ns / 1000000000LL;
        int64_t n = ns % 1000000000LL;
        if (n < 0) {
            n += 1000000000LL;
            s -= 1;
        }
        t.seconds = static_cast<uint64_t>(s);
        t.nanos = static_cast<uint32_t>(n);
        return t;
    }
};

// Common header fields we actually use.
struct PtpHeader {
    uint8_t  messageType = 0;
    uint8_t  versionPtp = 2;
    uint16_t messageLength = 0;
    uint8_t  domain = 0;
    uint16_t flags = 0;
    int64_t  correctionField = 0;
    uint8_t  clockIdentity[8] = {0};
    uint16_t portNumber = 0;
    uint16_t sequenceId = 0;
    uint8_t  controlField = 0;
    uint8_t  logMessageInterval = 0;
};

bool parsePtpHeader(const uint8_t *data, size_t len, PtpHeader *h);
PtpTimestamp readPtpTimestamp(const uint8_t *p);
void writePtpTimestamp(uint8_t *p, const PtpTimestamp &t);

// Wall clock as a PTP timestamp (Windows: 100 ns resolution).
PtpTimestamp nowAsPtp();

// Builds a Delay_Req message. Returns the number of bytes written, 0 on error.
size_t buildDelayReq(uint8_t *out, size_t cap, uint8_t domain,
                     const uint8_t clockIdentity[8], uint16_t seq);

// ---------------------------------------------------------------------------
// Minimal PTP slave (ordinary clock, E2E delay measurement, two-step Sync).
//
// Handles Sync + Follow_Up to learn the master's send time and Delay_Req /
// Delay_Resp to estimate the one-way path delay:
//
//   offset = (t2 - t1) - pathDelay
//   pathDelay = ((t2 - t1) + (t4 - t3)) / 2
//
// where t1 = master send time, t2 = slave receive time, t3 = slave send time
// of Delay_Req, t4 = master receive time.
// Derives RTP timestamps from a PTP timebase without reading the clock per packet.
//
// Reading it per packet does not work: the clock is published by another thread, so
// several consecutive packets see the same value (step 0) and then jump when it
// updates (measured: step 143 instead of 48). Anchor once and extrapolate by the
// packet count instead; re-anchor only when PTP disagrees with the extrapolation,
// which means the clock stepped or is drifting.
class PtpMediaClock {
public:
    // Half a packet of tolerance before re-anchoring.
    uint32_t update(int64_t masterNs, int sampleRate, long long packetIndex,
                    int framesPerPacket);

    bool anchored() const { return anchored_; }
    int64_t reanchors() const { return reanchors_; }

private:
    bool      anchored_ = false;
    uint32_t  anchorTs_ = 0;
    long long anchorPacket_ = 0;
    int64_t   reanchors_ = 0;
};

// Runs a PtpSlave on its own thread and republishes the grandmaster time.
// Polling the slave from the send loop cost about 3 ms per packet and dropped the
// packet rate from 1000/s to 327/s; the thread keeps that off the audio path.
class PtpFeed {
public:
    PtpFeed();
    ~PtpFeed();

    bool start(const std::string &ifaceIp, uint8_t domain, std::string *err);
    void stop();

    bool    valid() const;
    int64_t masterNs() const;
    double  ppm() const;
    // Grandmaster EUI-64 once one has been seen; false before that.
    bool    masterIdentity(uint8_t out[8]) const;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

class PtpSlave {
public:
    struct Stats {
        uint32_t syncCount = 0;
        uint32_t followUpCount = 0;
        uint32_t delayRespCount = 0;
        uint32_t truncatedCount = 0;  // messages shorter than their body
        int64_t  lastOffsetNs = 0;
        int64_t  minOffsetNs = 0;
        int64_t  maxOffsetNs = 0;
        int64_t  meanOffsetNs = 0;
        int64_t  pathDelayNs = 0;
        bool     haveFirst = false;

        // Frequency servo results.
        double   freqPpm = 0.0;          // estimated master rate vs local clock
        bool     freqValid = false;
        int64_t  offsetFilteredNs = 0;   // offset predicted at the last sample time
    };

    PtpSlave();
    ~PtpSlave();

    bool start(const std::string &ifaceIp, uint8_t domain, std::string *err);
    void stop();

    // Processes one incoming message per call. timeoutMs is the socket timeout
    // for the event socket. Returns true when an offset sample was produced.
    bool poll(int timeoutMs);

    // Sends a Delay_Req (call this periodically, e.g. once per second).
    bool sendDelayReq();

    // Processes one already-received datagram. Public so the packet handling can
    // be driven directly by tests (and by callers that do their own socket I/O).
    // `recvNanos` is the local receive timestamp.
    void handleMessage(const uint8_t *data, size_t len, uint64_t recvNanos);

    const Stats &stats() const { return stats_; }

    // Current grandmaster time in nanoseconds: the local PTP-style clock plus the
    // filtered offset. This is what a media sender needs to derive RTP timestamps
    // from the PTP timebase (AES67 a=mediaclk:direct=0). Returns false until at
    // least one offset sample exists.
    bool masterTimeNs(int64_t *ns) const;

    // The local PTP-style clock in nanoseconds (same timebase the offsets are
    // measured against). A media sender adds the offset to this to get master time.
    bool hasMaster() const { return haveMaster_; }
    const uint8_t *masterIdentity() const { return masterId_; }
private:
    void addSample(int64_t offsetNs);

    uintptr_t eventSock_ = ~static_cast<uintptr_t>(0);
    uintptr_t generalSock_ = ~static_cast<uintptr_t>(0);
    uint8_t  domain_ = 0;
    uint8_t  clockId_[8] = {0};
    uint16_t seq_ = 0;

    bool     waitingFollowUp_ = false;
    uint16_t pendingSyncSeq_ = 0;
    uint64_t pendingT2_ = 0;

    bool     waitingDelayResp_ = false;
    uint16_t pendingDelaySeq_ = 0;
    uint64_t pendingT3_ = 0;

    // E2E delay needs BOTH halves of the exchange:
    //   delta1 = t2 - t1  (slave clock minus master clock, includes offset + delay)
    //   delta2 = t4 - t3
    //   pathDelay = (delta1 + delta2) / 2
    //   offset    = delta1 - pathDelay = (delta1 - delta2) / 2
    bool     haveDelta1_ = false;
    int64_t  lastDelta1Ns_ = 0;

    bool     haveMaster_ = false;
    uint8_t  masterId_[8] = {0};

    // Least-squares regression over recent (localTime, offset) pairs.
    void updateFrequencyEstimate();

    struct OffsetSample {
        int64_t localNs;
        int64_t offsetNs;
    };

    Stats    stats_;
    int64_t  offsetSum_ = 0;
    uint32_t offsetCount_ = 0;
    std::vector<OffsetSample> history_;

    // Per-second binning. Raw offset samples carry ~0.2 ms of timestamp jitter;
    // fitting the raw series lets that jitter dominate the slope, and two runs of
    // the same code disagreed (standalone 310 ppm, in-loop 175 ppm, both for a
    // 300 ppm master). Averaging each second first cuts the noise by sqrt(n).
    int64_t binSecond_ = -1;
    double  binSumNs_ = 0.0;
    int     binCount_ = 0;
};

} // namespace openaes67
