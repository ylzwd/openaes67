// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

// WAV file backed AudioSource. Used for offline, bit-exact verification of the
// whole RTP path without any audio hardware.
#include "openaes67/audio_io.h"
#include "openaes67/wav.h"

namespace openaes67 {

class WavFileSource : public AudioSource {
public:
    explicit WavFileSource(const WavData &d) : data_(d) {}

    int read(int16_t *dst, int frames) override {
        if (!dst || frames <= 0 || data_.channels <= 0) {
            return 0;
        }
        const int totalFrames = data_.frames();
        int n = frames;
        if (pos_ + n > totalFrames) {
            n = totalFrames - pos_;
        }
        if (n <= 0) {
            return 0;
        }
        const size_t off = static_cast<size_t>(pos_) * data_.channels;
        for (int i = 0; i < n * data_.channels; ++i) {
            dst[i] = data_.samples[off + static_cast<size_t>(i)];
        }
        pos_ += n;
        return n;
    }

    int channels() const override { return data_.channels; }
    int sampleRate() const override { return data_.sampleRate; }

    int framesRemaining() const { return data_.frames() - pos_; }

private:
    WavData data_;
    int pos_ = 0;
};

AudioSource *createWavFileSource(const char *path) {
    if (!path) {
        return nullptr;
    }
    WavData d;
    std::string err;
    if (!readWavPcm16(path, &d, &err)) {
        return nullptr;
    }
    return new WavFileSource(d);
}

} // namespace openaes67