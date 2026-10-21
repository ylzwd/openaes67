#include "openaes67/jitter.h"

#include <algorithm>
#include <cstring>

namespace openaes67 {

namespace {
// A jitter buffer that is never allowed to shrink is a memory leak with extra
// steps: one stalled audio device and the buffer grows until the process dies.
// 200 packets is 200 ms at the 1 ms ptime this project targets, far beyond the
// 4-30 ms a receiver should ever need.
constexpr int kMaxPackets = 200;
// How many queued packets to hold before deciding a gap is a real loss. A packet
// that arrives two or three positions late is normal reordering, not loss; the
// original code declared loss the instant the next sequence number was absent.
constexpr int kReorderTolerance = 4;
// Beyond this a gap is treated as a discontinuity (sender restart, long outage)
// and the buffer resynchronises instead of waiting forever.
constexpr int64_t kMaxGap = 64;
} // namespace

JitterBuffer::JitterBuffer(int framesPerPacket, int channels, int prefillPackets)
    : framesPerPacket_(framesPerPacket > 0 ? framesPerPacket : 1),
      channels_(channels > 0 ? channels : 1),
      prefill_(prefillPackets) {
    slotSeq_.assign(kMaxPackets, -1);
    slotFrames_.assign(kMaxPackets, 0);
    storage_.assign(static_cast<size_t>(kMaxPackets) * framesPerPacket_ * channels_, 0);
}

int64_t JitterBuffer::extend(uint16_t seq) const {
    if (highest_ < 0) {
        return static_cast<int64_t>(seq);
    }
    const int64_t base = highest_ & ~static_cast<int64_t>(0xFFFF);
    int64_t cand = base | static_cast<int64_t>(seq);
    if (cand + 32768 < highest_) {
        cand += 65536;
    } else if (cand - 32768 > highest_) {
        cand -= 65536;
    }
    return cand;
}

int JitterBuffer::slotOf(int64_t seq) const {
    // Sequence numbers are signed here (extended across wraps), so fold negatives
    // back into the positive range before taking the modulus.
    const int64_t m = static_cast<int64_t>(kMaxPackets);
    int64_t idx = seq % m;
    if (idx < 0) {
        idx += m;
    }
    return static_cast<int>(idx);
}

int64_t JitterBuffer::oldestPending() const {
    int64_t best = -1;
    for (int i = 0; i < kMaxPackets; ++i) {
        const int64_t s = slotSeq_[static_cast<size_t>(i)];
        if (s >= 0 && (best < 0 || s < best)) {
            best = s;
        }
    }
    return best;
}

bool JitterBuffer::push(uint16_t seq, const int16_t *frames, int frameCount) {
    if (!frames || frameCount <= 0) {
        return false;
    }
    if (frameCount > framesPerPacket_) {
        // A packet larger than a slot cannot be stored without reallocating in the
        // audio path. The receiver already rejects oversized payloads.
        return false;
    }
    const int64_t ext = extend(seq);

    if (ext > highest_) {
        highest_ = ext;
    }

    if (started_ && ext < next_) {
        ++late_;
        return false;
    }

    const int slot = slotOf(ext);
    const int64_t held = slotSeq_[static_cast<size_t>(slot)];
    if (held == ext) {
        ++duplicates_;
        return false;
    }
    if (held >= 0) {
        // The slot is occupied by a different sequence number: the ring wrapped
        // before that packet was played. It is lost, not duplicated.
        ++lost_;
        if (depth_ > 0) {
            --depth_;
        }
    }

    int16_t *dst = storage_.data() + static_cast<size_t>(slot) * framesPerPacket_ * channels_;
    std::memcpy(dst, frames, static_cast<size_t>(frameCount) * channels_ * sizeof(int16_t));
    slotSeq_[static_cast<size_t>(slot)] = ext;
    slotFrames_[static_cast<size_t>(slot)] = frameCount;
    ++depth_;
    ++received_;
    return true;
}

int JitterBuffer::pop(int16_t *dst, int maxFrames) {
    if (!dst || maxFrames <= 0) {
        return 0;
    }
    if (!started_) {
        if (depth_ < prefill_) {
            return 0;
        }
        const int64_t first = oldestPending();
        if (first < 0) {
            return 0;
        }
        started_ = true;
        next_ = first;
        offset_ = 0;
    }
    if (depth_ <= 0) {
        return 0;
    }

    int slot = slotOf(next_);
    if (slotSeq_[static_cast<size_t>(slot)] != next_) {
        const int64_t first = oldestPending();
        if (first < 0) {
            return 0;
        }
        if (first < next_) {
            // Buffered data older than the play position: resynchronise.
            next_ = first;
            offset_ = 0;
            slot = slotOf(next_);
        } else {
            const int64_t gap = first - next_;
            // Give a late packet a few pops to arrive. The window is counted in
            // pop() calls (one audio tick each), not in queue depth: a deep prefill
            // made the old depth test true for every gap, so reordered packets were
            // always declared lost and then dropped as "late".
            if (gap > kMaxGap) {
                lost_ += static_cast<uint32_t>(gap);
                next_ = first;
                offset_ = 0;
                gapWaits_ = 0;
                slot = slotOf(next_);
            } else if (++gapWaits_ > kReorderTolerance) {
                lost_ += static_cast<uint32_t>(gap);
                next_ = first;
                offset_ = 0;
                gapWaits_ = 0;
                slot = slotOf(next_);
            } else {
                return 0;
            }
        }
    }

    const int total = slotFrames_[static_cast<size_t>(slot)];
    const int available = total - offset_;
    if (available <= 0) {
        slotSeq_[static_cast<size_t>(slot)] = -1;
        slotFrames_[static_cast<size_t>(slot)] = 0;
        --depth_;
        ++next_;
        offset_ = 0;
        return 0;
    }
    const int n = std::min(available, maxFrames);
    const int16_t *src = storage_.data() +
                         static_cast<size_t>(slot) * framesPerPacket_ * channels_ +
                         static_cast<size_t>(offset_) * channels_;
    std::memcpy(dst, src, static_cast<size_t>(n) * channels_ * sizeof(int16_t));

    gapWaits_ = 0;      // a packet was delivered: the hole (if any) is behind us
    offset_ += n;
    if (offset_ >= total) {
        slotSeq_[static_cast<size_t>(slot)] = -1;
        slotFrames_[static_cast<size_t>(slot)] = 0;
        --depth_;
        ++next_;
        offset_ = 0;
    }
    return n;
}

void JitterBuffer::flush() {
    std::fill(slotSeq_.begin(), slotSeq_.end(), static_cast<int64_t>(-1));
    std::fill(slotFrames_.begin(), slotFrames_.end(), 0);
    depth_ = 0;
    started_ = false;
    next_ = 0;
    highest_ = -1;
    offset_ = 0;
}

} // namespace openaes67
