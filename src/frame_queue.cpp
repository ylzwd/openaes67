// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/frame_queue.h"

#include <algorithm>
#include <cstring>

namespace openaes67 {

void FrameQueue::setChannels(int channels) {
    channels_ = channels > 0 ? channels : 1;
    clear();
}

void FrameQueue::append(const int16_t *src, int frames) {
    if (!src || frames <= 0) {
        return;
    }
    const size_t samples = static_cast<size_t>(frames) * static_cast<size_t>(channels_);
    // Reserve before inserting: the queue is drained in small pieces, so without
    // this every packet after the first would grow the vector by a few frames.
    const size_t needed = buffer_.size() + samples;
    if (needed > buffer_.capacity()) {
        buffer_.reserve(std::max(needed, buffer_.capacity() * 2));
    }
    buffer_.insert(buffer_.end(), src, src + samples);
    frames_ += frames;
}

int FrameQueue::consume(int frames) {
    if (frames <= 0 || frames_ == 0) {
        return 0;
    }
    const int taken = std::min(frames, frames_);
    head_ += static_cast<size_t>(taken) * static_cast<size_t>(channels_);
    frames_ -= taken;
    if (frames_ == 0) {
        // Fully drained: keep the storage, drop the contents.
        buffer_.clear();
        head_ = 0;
    } else if (head_ > buffer_.size() / 2) {
        // Compact only once the dead prefix is at least half the buffer, so the
        // copies amortise to one per buffer-full instead of one per delivery.
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(head_));
        head_ = 0;
    }
    return taken;
}

int FrameQueue::frames() const {
    return frames_;
}

void FrameQueue::clear() {
    buffer_.clear();
    head_ = 0;
    frames_ = 0;
}

} // namespace openaes67
