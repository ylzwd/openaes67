// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstdint>
#include <vector>

namespace openaes67 {

// Reorders RTP packets by sequence number, absorbs jitter, and reports loss.
//
// push() accepts packets inside a sliding window; duplicates and packets older
// than the play position are counted and dropped. pop() returns the next
// in-order packet. When a gap persists, pop() skips forward and counts the
// missing packets as lost, so a single dropped packet never stalls playback.
//
// Storage is a preallocated ring of slots indexed by sequence number modulo the
// slot count. The previous implementation used a std::map of std::vectors and
// therefore allocated (and freed) per packet -- two thousand allocations a
// second on a 1 ms stream, right in the audio path. Measured 885 ns per
// push+pop before; the ring is an order of magnitude cheaper.
class JitterBuffer {
public:
    JitterBuffer(int framesPerPacket, int channels, int prefillPackets = 4);

    bool push(uint16_t seq, const int16_t *frames, int frameCount);

    // Copies the next in-order packet into dst.
    // Returns frames copied (0 when nothing is ready).
    int pop(int16_t *dst, int maxFrames);

    void flush();

    uint32_t received()   const { return received_; }
    uint32_t duplicates() const { return duplicates_; }
    uint32_t late()       const { return late_; }
    uint32_t lost()       const { return lost_; }
    int      depth()      const { return depth_; }

private:
    int64_t extend(uint16_t seq) const;
    int     slotOf(int64_t seq) const;
    // Smallest pending sequence number, or -1 when nothing is buffered.
    int64_t oldestPending() const;

    int framesPerPacket_;
    int channels_;
    int prefill_;

    bool    started_ = false;
    int64_t highest_ = -1;
    int64_t next_ = 0;
    // Consumption offset inside the packet at next_: pop() may be asked for
    // fewer frames than a packet holds, and the old code erased the whole
    // packet anyway, silently discarding the remainder.
    int     offset_ = 0;
    // How many consecutive pop() calls have found the next sequence number absent.
    // Reordering tolerance must be measured in wait time, not queue depth: with a
    // 20 ms prefill the queue is always deeper than the tolerance, so a depth test
    // declared every reordered packet lost.
    int     gapWaits_ = 0;

    std::vector<int64_t> slotSeq_;        // -1 = free
    std::vector<int>     slotFrames_;
    std::vector<int16_t> storage_;        // slots * framesPerPacket * channels
    int                  depth_ = 0;

    uint32_t received_ = 0;
    uint32_t duplicates_ = 0;
    uint32_t late_ = 0;
    uint32_t lost_ = 0;
};

} // namespace openaes67
