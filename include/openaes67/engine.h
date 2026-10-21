#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "openaes67/config.h"
#include "openaes67/jitter.h"
#include "openaes67/net.h"
#include "openaes67/rtp.h"
#include "openaes67/rtcp.h"

namespace openaes67 {

// Send-side counters. A failed send used to stop the stream silently; now the
// caller can report how often it happened and keep going.
struct TxStats {
    uint64_t packets = 0;
    uint32_t errors = 0;
};

struct RxStats {
    uint32_t packets = 0;
    uint32_t frames = 0;
    uint32_t lost = 0;
    uint32_t duplicates = 0;
    // Packets from another source on the same multicast group. Without this the
    // receiver happily interleaves two senders into one garbled stream.
    uint32_t wrongSsrc = 0;
    // Sender restarts (new SSRC) that were adopted after a run of packets.
    uint32_t ssrcChanges = 0;
    uint32_t truncated = 0;
    // Packets dropped because the payload type is not the one this stream uses.
    // Kept apart from `truncated`: "another stream on this group / wrong codec"
    // and "malformed packet" need different fixes.
    uint32_t wrongPayloadType = 0;
};

class RtpSender {
public:
    RtpSender(const StreamConfig &cfg, uint32_t ssrc);
    bool start(std::string *err);
    bool sendFrames(const int16_t *interleaved, int frames);

    // Sends with an explicitly derived RTP timestamp (PTP-locked mode). The value
    // must already be in RTP units: (ptpNs * sampleRate / 1e9) mod 2^32.
    bool sendFramesAt(uint32_t timestamp, const int16_t *interleaved, int frames);
    void stop();
    const TxStats &stats() const { return txStats_; }
    // SSRC actually used on the wire (a random one when 0 was requested).
    uint32_t ssrc() const { return pkt_.ssrc(); }

    // Sends one RTCP Sender Report to the RTCP port. Call every few seconds
    // when RTCP is enabled; returns false when the datagram could not be sent.
    bool sendRtcpSenderReport(const std::string &cname);
    uint64_t packetsSent() const { return txPackets_; }
    uint64_t octetsSent() const { return txOctets_; }

    // RTCP reception on the sender side: peer Receiver Reports.
    bool rrActive() const { return rrStarted_; }
    bool hasReceiverReport() const { return haveRr_; }
    const RtcpReceptionStats &receiverReport() const { return lastRr_; }
    const std::string &rrCname() const { return rrCname_; }
    uint32_t rrPackets() const { return rrPackets_; }
    const std::string &rrError() const { return rrError_; }
    // Drains pending RR datagrams. Called from the send loop.
    void pollReceiverReports();

private:
    StreamConfig cfg_;
    UdpSocket    sock_;
    RtpPacketizer pkt_;
    bool          inTalkspurt_ = false;
    std::vector<uint8_t> buf_;
    bool started_ = false;
    TxStats       txStats_;
    uint64_t      txPackets_ = 0;
    uint64_t      txOctets_ = 0;
    uint32_t      lastRtpTimestamp_ = 0;
    UdpSocket     rrSock_;
    bool          rrStarted_ = false;
    bool          haveRr_ = false;
    RtcpReceptionStats lastRr_;
    std::string   rrCname_;
    uint32_t      rrPackets_ = 0;
    std::string   rrError_;
    std::vector<uint8_t> rrBuf_;
};

class RtpReceiver {
public:
    explicit RtpReceiver(const StreamConfig &cfg);
    bool start(std::string *err);

    // Pulls up to maxFrames of interleaved audio. Returns frames produced.
    int pull(int16_t *dst, int maxFrames, int timeoutMs);

    void stop();

    const RxStats &stats() const { return stats_; }
    const JitterBuffer &jitter() const { return jitter_; }

    // RTCP reception (only when cfg.rtcp is set).
    bool rtcpActive() const { return rtcpStarted_; }
    bool hasSenderReport() const { return haveSr_; }
    const RtcpSenderReport &senderReport() const { return lastSr_; }
    const std::string &rtcpCname() const { return cname_; }
    uint32_t rtcpPackets() const { return rtcpPackets_; }
    const std::string &rtcpError() const { return rtcpError_; }

    // Sends one RTCP Receiver Report describing how this stream is arriving.
    // Call every few seconds when RTCP is enabled and a sender report has been
    // seen (the report needs the sender's SSRC and its last SR time).
    bool sendReceiverReport(const std::string &cname);
    // Interarrival jitter in RTP timestamp units, RFC 3550 section 6.4.1.
    uint32_t interarrivalJitter() const { return static_cast<uint32_t>(jitterEstimate_); }

private:
    StreamConfig cfg_;
    UdpSocket    sock_;
    JitterBuffer jitter_;
    UdpSocket    rtcpSock_;
    bool         rtcpStarted_ = false;
    bool         haveSr_ = false;
    RtcpSenderReport lastSr_;
    std::string  cname_;
    uint32_t     rtcpPackets_ = 0;
    std::string  rtcpError_;
    double       jitterEstimate_ = 0.0;   // RFC 3550: scaled by 16
    bool         haveArrival_ = false;
    uint32_t     prevRtpTimestamp_ = 0;
    std::chrono::steady_clock::time_point prevArrival_{};
    uint32_t     lastSrNtpMiddle_ = 0;
    std::chrono::steady_clock::time_point lastSrAt_{};
    std::vector<uint8_t> rtcpBuf_;
    std::vector<uint8_t> pktBuf_;
    RxStats      stats_;
    // SSRC the receiver is locked to, and how many consecutive packets a
    // different source must send before it takes over (sender restart).
    uint32_t lockedSsrc_ = 0;
    uint32_t otherSsrc_ = 0;
    int      otherRun_ = 0;
    // Adoption of a new SSRC also requires the current source to have gone quiet:
    // two senders on one group must not make the receiver flap between them.
    std::chrono::steady_clock::time_point lastLocked_{};
    bool         started_ = false;
};

} // namespace openaes67
