#include "openaes67/engine.h"
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <random>

namespace openaes67 {


// RFC 3550 section 5.1: an SSRC must be globally unique per session. Two copies of
// this sender used to ship the same fixed value, which makes them indistinguishable
// to receivers and to any tool that tracks sources.
static uint32_t makeRandomSsrc() {
    std::random_device rd;
    uint32_t v = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
    return v != 0 ? v : 0x5EED0001u;
}

RtpSender::RtpSender(const StreamConfig &cfg, uint32_t ssrc)
    : cfg_(cfg), pkt_(ssrc != 0 ? ssrc : makeRandomSsrc(), cfg.payloadType) {}

bool RtpSender::start(std::string *err) {
    if (!sock_.openSender(cfg_.ifaceIp, cfg_.ttl, err)) {
        return false;
    }
    buf_.resize(kRtpHeaderSize + static_cast<size_t>(cfg_.framesPerPacket()) *
                                    cfg_.bytesPerSample() * cfg_.channels);
    started_ = true;
    if (cfg_.rtcp) {
        // Listen for peer Receiver Reports on the RTCP port. A failure here must
        // not stop the stream: the reports are diagnostic, the audio is the product.
        rrBuf_.assign(2048, 0);
        std::string rrErr;
        if (rrSock_.openReceiver(cfg_.group, cfg_.rtcpPort(), cfg_.ifaceIp, &rrErr)) {
            rrStarted_ = true;
        } else {
            rrError_ = rrErr;
        }
    }
    return true;
}

bool RtpSender::sendRtcpSenderReport(const std::string &cname) {
    if (!started_) {
        return false;
    }
    RtcpSenderReport sr;
    sr.ssrc = pkt_.ssrc();
    sr.ntp = rtcpNtpNow();
    sr.rtpTimestamp = lastRtpTimestamp_;
    sr.packetCount = static_cast<uint32_t>(txPackets_);
    sr.octetCount = static_cast<uint32_t>(txOctets_);
    const std::string pkt = buildRtcpSenderReport(sr, cname);
    return sock_.sendTo(pkt.data(), pkt.size(), cfg_.group, cfg_.rtcpPort());
}

bool RtpSender::sendFramesAt(uint32_t timestamp, const int16_t *interleaved, int frames) {
    pkt_.setNextTimestamp(timestamp);
    return sendFrames(interleaved, frames);
}
bool RtpSender::sendFrames(const int16_t *interleaved, int frames) {
    if (!started_ || !interleaved || frames <= 0) {
        return false;
    }
    // RFC 3550: the marker bit marks the start of a talkspurt for audio.
    // Playing audio after a silent stretch is exactly that transition.
    bool hasAudio = false;
    for (int i = 0, n = frames * cfg_.channels; i < n; ++i) {
        if (interleaved[i] != 0) { hasAudio = true; break; }
    }
    if (hasAudio && !inTalkspurt_) {
        pkt_.setMarker(true);
    }
    inTalkspurt_ = hasAudio;

    const size_t hdr = pkt_.writeNextHeader(buf_.data(), buf_.size(), frames);
    if (hdr == 0) {
        return false;
    }
    // Linear PCM, big endian: 2 bytes per sample (L16) or 3 (L24).
    const int samples = frames * cfg_.channels;
    const size_t payload =
        packPayload(cfg_.codec, interleaved, static_cast<size_t>(samples),
                    buf_.data() + hdr, buf_.size() - hdr);
    if (payload == 0) {
        return false;
    }
    const bool sent = sock_.sendTo(buf_.data(), hdr + payload, cfg_.group, cfg_.port);
    if (sent) {
        ++txPackets_;
        txOctets_ += payload;
        lastRtpTimestamp_ = (static_cast<uint32_t>(buf_[4]) << 24) |
                            (static_cast<uint32_t>(buf_[5]) << 16) |
                            (static_cast<uint32_t>(buf_[6]) << 8) |
                            static_cast<uint32_t>(buf_[7]);
    }
    return sent;
}

void RtpSender::stop() {
    sock_.close();
    started_ = false;
}

RtpReceiver::RtpReceiver(const StreamConfig &cfg)
    : cfg_(cfg), jitter_(cfg.framesPerPacket(), cfg.channels, 4) {}

bool RtpReceiver::start(std::string *err) {
    if (!sock_.openReceiver(cfg_.group, cfg_.port, cfg_.ifaceIp, err)) {
        return false;
    }
    // Size for the largest packet this stream can produce (a 64-channel,
    // 1 ms AES67 packet is ~6 KB). A fixed 2048 truncated jumbo packets and the
    // frame count was then computed from the truncated payload.
    const size_t want =
        static_cast<size_t>(cfg_.framesPerPacket()) * cfg_.channels *
            static_cast<size_t>(cfg_.bytesPerSample()) +
        kRtpHeaderSize + 64;
    pktBuf_.resize(want < 2048 ? 2048 : want);
    if (cfg_.rtcp) {
        // RTCP rides port+1. Failure to join must not stop the audio stream:
        // the reports are diagnostic, the audio is the product.
        std::string rtcpErr;
        if (rtcpSock_.openReceiver(cfg_.group, cfg_.rtcpPort(), cfg_.ifaceIp, &rtcpErr)) {
            rtcpStarted_ = true;
            rtcpBuf_.assign(2048, 0);
        }
    }
    started_ = true;
    return true;
}

int RtpReceiver::pull(int16_t *dst, int maxFrames, int timeoutMs) {
    if (!started_) {
        return 0;
    }
    // Drain what is already buffered before blocking on the socket.
    int n = jitter_.pop(dst, maxFrames);
    if (n > 0) {
        stats_.frames += static_cast<uint32_t>(n);
        return n;
    }

    const int bytes = sock_.recvFrom(pktBuf_.data(), pktBuf_.size(), timeoutMs);
    if (bytes <= 0) {
        return 0;
    }

    RtpHeader h;
    if (!readRtpHeader(pktBuf_.data(), static_cast<size_t>(bytes), &h)) {
        return 0;
    }
    if (h.payloadType != cfg_.payloadType) {
        // Counted, not silently dropped: "another stream on this group" and "the
        // sender is using a different codec" both look like silence otherwise.
        ++stats_.wrongPayloadType;
        return 0;
    }

    // Latch onto one source. Another SSRC on the same group is either a second
    // stream or a sender restart. It is adopted only when it has sent a sustained
    // run AND the current source has gone quiet -- otherwise two live senders make
    // the receiver flap between them (276 adoptions in 7 s was measured).
    const auto now = std::chrono::steady_clock::now();
    if (lockedSsrc_ == 0) {
        lockedSsrc_ = h.ssrc;
        lastLocked_ = now;
    } else if (h.ssrc == lockedSsrc_) {
        lastLocked_ = now;
        otherRun_ = 0;
    } else {
        ++stats_.wrongSsrc;
        const bool quiet = lastLocked_.time_since_epoch().count() == 0 ||
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - lastLocked_).count() > 200;
        if (quiet) {
            if (h.ssrc == otherSsrc_) {
                ++otherRun_;
            } else {
                otherSsrc_ = h.ssrc;
                otherRun_ = 1;
            }
            if (otherRun_ >= 25) {
                lockedSsrc_ = otherSsrc_;
                otherRun_ = 0;
                lastLocked_ = now;
                ++stats_.ssrcChanges;
                jitter_.flush();
            }
        } else {
            otherRun_ = 0;
        }
        return 0;
    }

    const int payloadBytes = bytes - static_cast<int>(kRtpHeaderSize);
    const int frameBytes = cfg_.bytesPerSample() * cfg_.channels;
    // A payload that is not a whole number of frames is malformed: dividing and
    // rounding down would shift every following sample by a byte.
    if (frameBytes <= 0 || payloadBytes % frameBytes != 0) {
        ++stats_.truncated;
        return 0;
    }
    const int frames = payloadBytes / frameBytes;
    const int expectedFrames = cfg_.framesPerPacket();
    if (frames > expectedFrames * 2) {
        ++stats_.truncated;
        return 0;
    }
    if (frames <= 0) {
        return 0;
    }

    // Convert big-endian linear PCM (L16 or L24) to host order.
    static thread_local std::vector<int16_t> tmp;
    tmp.resize(static_cast<size_t>(frames) * cfg_.channels);
    const size_t samples = unpackPayload(cfg_.codec, pktBuf_.data() + kRtpHeaderSize,
                                        static_cast<size_t>(payloadBytes), tmp.data(),
                                        tmp.size());
    if (samples == 0) {
        ++stats_.truncated;
        return 0;
    }
    jitter_.push(h.sequence, tmp.data(), frames);

    // Interarrival jitter (RFC 3550 6.4.1): compare the spacing of arrivals with
    // the spacing of RTP timestamps, then smooth with the 1/16 gain.
    const auto arrival = std::chrono::steady_clock::now();
    if (haveArrival_) {
        const double arrivalDelta =
            std::chrono::duration<double>(arrival - prevArrival_).count() * cfg_.sampleRate;
        const int32_t tsDelta = static_cast<int32_t>(h.timestamp - prevRtpTimestamp_);
        const double d = arrivalDelta - static_cast<double>(tsDelta);
        jitterEstimate_ += (std::fabs(d) - jitterEstimate_) / 16.0;
    }
    prevArrival_ = arrival;
    prevRtpTimestamp_ = h.timestamp;
    haveArrival_ = true;

    // Drain any RTCP datagrams that arrived: an SR tells us the sender's clock
    // and packet counts, which is the only way to see its side of the stream.
    if (rtcpStarted_ && !rtcpBuf_.empty()) {
        for (;;) {
            const int rtcpBytes =
                rtcpSock_.recvFrom(rtcpBuf_.data(), rtcpBuf_.size(), 0);
            if (rtcpBytes <= 0) {
                break;
            }
            RtcpSenderReport sr;
            if (parseRtcpSenderReport(rtcpBuf_.data(),
                                      static_cast<size_t>(rtcpBytes), &sr)) {
                lastSr_ = sr;
                lastSrNtpMiddle_ = static_cast<uint32_t>((sr.ntp >> 16) & 0xFFFFFFFF);
                lastSrAt_ = std::chrono::steady_clock::now();
                haveSr_ = true;
                ++rtcpPackets_;
                parseRtcpCname(rtcpBuf_.data(), static_cast<size_t>(rtcpBytes),
                               &cname_);
            }
        }
    }
    ++stats_.packets;

    n = jitter_.pop(dst, maxFrames);
    if (n > 0) {
        stats_.frames += static_cast<uint32_t>(n);
    }
    return n;
}

bool RtpReceiver::sendReceiverReport(const std::string &cname) {
    if (!rtcpStarted_) {
        return false;
    }
    // Without a sender report there is no SSRC to describe and no LSR/DLSR to
    // report; an RR block would be meaningless.
    if (!haveSr_) {
        return false;
    }
    RtcpReceptionStats stats;
    stats.ssrc = lastSr_.ssrc;
    stats.cumulativeLost = static_cast<int32_t>(stats_.lost);
    const double received = static_cast<double>(stats_.packets);
    const double expected = received + static_cast<double>(stats_.lost);
    stats.fractionLost = expected > 0.0
                             ? static_cast<uint8_t>(std::min(255.0,
                                   static_cast<double>(stats_.lost) * 256.0 / expected))
                             : 0;
    stats.highestSequence = static_cast<uint32_t>(lastSr_.packetCount) +
                            static_cast<uint32_t>(stats_.lost);
    stats.jitter = interarrivalJitter();

    const std::string pkt = buildRtcpReceiverReport(lockedSsrc_, &stats, cname);
    return rtcpSock_.sendTo(pkt.data(), pkt.size(), cfg_.group, cfg_.rtcpPort());
}

void RtpReceiver::stop() {
    sock_.close();
    started_ = false;
    stats_.lost = jitter_.lost();
    stats_.duplicates = jitter_.duplicates();
}


void RtpSender::pollReceiverReports() {
    if (!rrStarted_ || rrBuf_.empty()) {
        return;
    }
    for (;;) {
        const int bytes = rrSock_.recvFrom(rrBuf_.data(), rrBuf_.size(), 0);
        if (bytes <= 0) {
            break;
        }
        RtcpReceptionStats rr;
        uint32_t reporter = 0;
        if (parseRtcpReceiverReport(rrBuf_.data(), static_cast<size_t>(bytes), &rr,
                                    &reporter)) {
            lastRr_ = rr;
            haveRr_ = true;
            ++rrPackets_;
            parseRtcpCname(rrBuf_.data(), static_cast<size_t>(bytes), &rrCname_);
        }
    }
}

} // namespace openaes67
