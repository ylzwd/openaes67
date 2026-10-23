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

// A FIFO of interleaved frames that is consumed from the front far more often
// than it is appended to.
//
// Why not std::vector::erase: the receive loop hands the device whatever it will
// take, which is usually less than a whole packet. Erasing from the front after
// every partial write moves everything that is left, and at 64 channels that is
// ~180 KB per call. A read offset makes consumption O(1); the buffer is only
// compacted when the offset has grown past half of it, so the copies amortise to
// one per buffer-full instead of one per delivery.
class FrameQueue {
public:
    void setChannels(int channels);
    int  channels() const { return channels_; }

    // Appends `frames` frames from `src` (interleaved, channels() channels).
    void append(const int16_t *src, int frames);
    // Removes up to `frames` frames from the front; returns how many were available.
    int  consume(int frames);

    // Front of the queue, valid until the next append/consume/clear.
    const int16_t *data() const { return buffer_.data() + head_; }
    int  frames() const;
    bool empty() const { return frames() == 0; }
    void clear();

    // Bytes currently allocated; used by the benchmark to show that a change did
    // not quietly start reallocating per packet.
    size_t capacitySamples() const { return buffer_.capacity(); }

private:
    std::vector<int16_t> buffer_;
    size_t head_ = 0;      // samples already consumed
    int    frames_ = 0;    // frames still queued (kept explicitly: dividing by
                           // channels on every call showed up in the benchmark)
    int    channels_ = 1;
};

} // namespace openaes67
