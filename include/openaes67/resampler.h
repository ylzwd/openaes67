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

// Keeps a receive buffer at a target depth by nudging the playback ratio.
//
// The PI loop corrects the natural drift between the network clock (PTP) and
// the audio device clock. Correction is expressed in ppm and clamped, so a bad
// measurement can never make playback run away.
//
// Two details matter in practice:
//
//   * The error is averaged over a short window. Real receive loops drain the
//     network in bursts and then wait, so the instantaneous buffer depth is a
//     sawtooth; feeding that straight into the controller makes the integral
//     wind up and drives the buffer to empty.
//   * The integral leaks slightly, so a one-sided perturbation cannot leave a
//     permanent offset behind.
//
// Resampling happens directly on the internal FIFO with linear interpolation and
// a persistent fractional position. When the correction is exactly 0 the read
// position stays integral, so output is a bit-exact copy of the input.
class RateController {
public:
    RateController(int channels, int sampleRate, double targetMs = 30.0);

    void push(const int16_t *frames, int frameCount);

    // Produces frames for the device. Returns frames produced (may be 0).
    int pull(int16_t *dst, int maxFrames);

    int    bufferedFrames() const;
    double targetFrames() const { return targetFrames_; }
    double correctionPpm() const { return corrPpm_; }
    double peakErrorFrames() const { return peakErrorFrames_; }
    int    minBuffered() const { return minBuffered_; }
    int    maxBuffered() const { return maxBuffered_; }
    double meanBuffered() const {
        return samples_ > 0 ? static_cast<double>(bufferedSum_) / samples_ : 0.0;
    }
    // Lifetime statistics that are never reset by the periodic reporter.
    int    minBufferedLifetime() const { return minLife_; }
    int    maxBufferedLifetime() const { return maxLife_; }
    double meanBufferedLifetime() const {
        return lifeCount_ > 0 ? static_cast<double>(lifeSum_) / lifeCount_ : 0.0;
    }
    void   resetPeakError() { peakErrorFrames_ = 0.0; }
    // Clears the windowed buffer statistics (used for periodic reporting).
    void   resetWindowStats() {
        minBuffered_ = 1 << 30;
        maxBuffered_ = 0;
        bufferedSum_ = 0;
        samples_ = 0;
    }

    // Effective rate ratio measured from real consumption, not from the
    // controller: positive means playback consumed fewer inputs per output,
    // i.e. the device is running fast relative to the network.
    double effectivePpm() const {
        return totalProduced_ > 0
                   ? (1.0 - static_cast<double>(totalConsumed_) /
                                static_cast<double>(totalProduced_)) * 1e6
                   : 0.0;
    }

    // Test hook: pretend the device clock runs this many ppm fast (+) or slow (-).
    void setDevicePpm(double ppm) { devicePpm_ = ppm; }

    // Feed-forward rate error (usually from PTP). It is added straight to the
    // correction so the PI loop only has to clean up the residual instead of
    // discovering the drift from scratch.
    void setFeedforwardPpm(double ppm) { feedforwardPpm_ = ppm; }
    double feedforwardPpm() const { return feedforwardPpm_; }

private:
    void updateControl();

    static constexpr int kWindowSamples = 128;

    int    channels_;
    int    sampleRate_;
    double targetFrames_;
    double devicePpm_ = 0.0;
    double feedforwardPpm_ = 0.0;

    std::vector<int16_t> fifo_;
    double pos_ = 0.0;              // fractional read position, in frames



    double integral_ = 0.0;
    double corrPpm_ = 0.0;
    double peakErrorFrames_ = 0.0;
    long long totalProduced_ = 0;
    long long totalConsumed_ = 0;
    int    minBuffered_ = 1 << 30;
    int    maxBuffered_ = 0;
    long long bufferedSum_ = 0;
    long long samples_ = 0;
    long long ctrlSum_ = 0;
    long long ctrlCount_ = 0;
    int    minLife_ = 1 << 30;
    int    maxLife_ = 0;
    long long lifeSum_ = 0;
    long long lifeCount_ = 0;
};

} // namespace openaes67
