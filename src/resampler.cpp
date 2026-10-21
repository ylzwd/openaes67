#include "openaes67/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace openaes67 {

RateController::RateController(int channels, int sampleRate, double targetMs)
    : channels_(channels > 0 ? channels : 2),
      sampleRate_(sampleRate > 0 ? sampleRate : 48000) {
    targetFrames_ = static_cast<double>(sampleRate_) * targetMs / 1000.0;
}

int RateController::bufferedFrames() const {
    return static_cast<int>(fifo_.size() / static_cast<size_t>(channels_));
}

void RateController::push(const int16_t *frames, int frameCount) {
    if (!frames || frameCount <= 0) {
        return;
    }
    fifo_.insert(fifo_.end(), frames,
                 frames + static_cast<size_t>(frameCount) * channels_);
}

void RateController::updateControl() {
    const int depth = bufferedFrames();
    if (depth < minBuffered_) {
        minBuffered_ = depth;
    }
    if (depth > maxBuffered_) {
        maxBuffered_ = depth;
    }
    bufferedSum_ += depth;
    ++samples_;



    const double instantaneous = static_cast<double>(depth) - targetFrames_;
    peakErrorFrames_ = std::max(peakErrorFrames_, std::abs(instantaneous));

    // The receive loop drains the FIFO in bursts and then waits, so the raw
    // depth swings over a couple of thousand frames. A controller fed with that
    // sawtooth random-walks its integral and saturates (observed: corr climbing
    // to the +1891 ppm clamp and sticking there). Servo on the window average
    // instead, and only once per window.
    // Control window (separate from the reporting window, which the caller
    // resets on its own schedule).
    ctrlSum_ += depth;
    ++ctrlCount_;
    if (ctrlCount_ < kWindowSamples) {
        return;
    }
    const double meanDepth = static_cast<double>(ctrlSum_) / static_cast<double>(ctrlCount_);
    ctrlSum_ = 0;
    ctrlCount_ = 0;

    const double error = meanDepth - targetFrames_;

    // Deadband of 1 ms on the mean: absorbs the push-before-pull bias (about
    // half a packet) so a locked clock still reads zero correction.
    const double framesPerMs = static_cast<double>(sampleRate_) / 1000.0;
    const double deadband = framesPerMs * 0.5;
    double excess = 0.0;
    if (error > deadband) {
        excess = error - deadband;
    } else if (error < -deadband) {
        excess = error + deadband;
    }

    // Slow servo: roughly one correction per window, so the loop cannot chase
    // individual bursts.
    // Two regimes:
    //   * steady state: gentle gains, +/-2000 ppm, inaudible correction
    //   * recovery: a large buffer error (e.g. after a startup transient) is
    //     corrected hard, because 2000 ppm only moves 96 frames per second --
    //     refilling 1000 frames would take ten seconds, and until then the
    //     controller sits pinned at its clamp (observed on the GUI path).
    const bool recovering = std::fabs(excess) > framesPerMs * 10.0;
    const double kp = recovering ? 20.0 : 4.0;   // ppm per frame of excess
    const double ki = 0.01;                      // ppm per frame of excess per window
    const double limit = recovering ? 20000.0 : 2000.0;

    integral_ = integral_ * 0.99 + excess;      // leaky, avoids permanent offset
    const double maxIntegral = 2000.0 / std::max(ki, 1e-9);
    if (integral_ > maxIntegral) {
        integral_ = maxIntegral;
    } else if (integral_ < -maxIntegral) {
        integral_ = -maxIntegral;
    }

    double corr = kp * excess + ki * integral_ + devicePpm_ + feedforwardPpm_;
    if (corr > limit) {
        corr = limit;
    } else if (corr < -limit) {
        corr = -limit;
    }
    if (std::fabs(corr) < 0.5) {
        corr = 0.0;                             // keep a locked clock bit-exact
    }
    corrPpm_ = corr;
}
int RateController::pull(int16_t *dst, int maxFrames) {
    if (!dst || maxFrames <= 0) {
        return 0;
    }

    const int buffered = bufferedFrames();
    if (buffered < 2) {
        updateControl();
        return 0;                       // need two frames to interpolate
    }

    const double ratio = 1.0 + corrPpm_ * 1e-6;
    int produced = 0;

    while (produced < maxFrames) {
        const int i0 = static_cast<int>(pos_);
        const double frac = pos_ - static_cast<double>(i0);
        if (i0 + 1 >= buffered) {
            break;
        }
        const int16_t *a = &fifo_[static_cast<size_t>(i0) * channels_];
        const int16_t *b = &fifo_[static_cast<size_t>(i0 + 1) * channels_];
        for (int c = 0; c < channels_; ++c) {
            double v;
            if (frac == 0.0) {
                v = static_cast<double>(a[c]);          // exact passthrough
            } else {
                v = static_cast<double>(a[c]) +
                    (static_cast<double>(b[c]) - static_cast<double>(a[c])) * frac;
            }
            int iv = static_cast<int>(std::lround(v));
            if (iv > 32767) {
                iv = 32767;
            } else if (iv < -32768) {
                iv = -32768;
            }
            dst[static_cast<size_t>(produced) * channels_ + c] = static_cast<int16_t>(iv);
        }
        ++produced;
        pos_ += ratio;
    }

    const int consume = static_cast<int>(pos_);
    if (consume > 0) {
        fifo_.erase(fifo_.begin(),
                    fifo_.begin() + static_cast<size_t>(consume) * channels_);
        pos_ -= static_cast<double>(consume);
    }
    totalProduced_ += produced;
    totalConsumed_ += consume;

    // Lifetime statistics sample only when we actually produced audio.
    // Sampling on every pull let thousands of starvation calls (depth 0-2,
    // during prebuffer and idle moments) drag the average far below the real
    // buffer depth: the GUI showed 330 frames where the CLI showed 1469.
    if (produced > 0) {
        const int nowDepth = bufferedFrames();
        if (nowDepth < minLife_) {
            minLife_ = nowDepth;
        }
        if (nowDepth > maxLife_) {
            maxLife_ = nowDepth;
        }
        lifeSum_ += nowDepth;
        ++lifeCount_;
    }

    updateControl();
    return produced;
}

} // namespace openaes67
