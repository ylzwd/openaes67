// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "openaes67/shared_audio.h"

namespace openaes67 {

struct SharedAudioConfig {
    int      channels = 2;
    int      sampleRate = 48000;
    uint32_t capacityFrames = 48000 / 2;   // 500 ms at 48 kHz
};

// One side of the shared-memory ring defined by shared_audio.h.
//
// The class works on a raw mapping, so it can be unit-tested without any OS
// objects, and the same code runs on top of a real section created by the
// driver. Both directions are single-producer/single-consumer:
//
//   render : user mode writes, the driver reads
//   capture: the driver writes, user mode reads
class SharedAudioRing {
public:
    // `mapping` must point at a buffer of `bytes` bytes laid out as described
    // in shared_audio.h. When `initialize` is true the header is written;
    // otherwise the existing header is validated.
    // `ownMapping` makes the destructor unmap the view; the factories pass true.
    SharedAudioRing(void *mapping, size_t bytes, const SharedAudioConfig &cfg,
                    bool initialize, bool ownMapping = false);
    ~SharedAudioRing();
    SharedAudioRing(const SharedAudioRing &) = delete;
    SharedAudioRing &operator=(const SharedAudioRing &) = delete;

    bool valid() const { return valid_; }
    const std::string &error() const { return error_; }

    int      channels() const;
    int      sampleRate() const;
    uint32_t capacityFrames() const;

    // ---- render direction (this side produces) ----
    int  writeRender(const int16_t *src, int frames);   // returns frames accepted
    int  renderFree() const;
    int  renderPending() const;

    // ---- render direction (driver side, used by tests) ----
    int  readRender(int16_t *dst, int frames);

    // ---- capture direction (driver side, used by tests) ----
    int  writeCapture(const int16_t *src, int frames);
    int  readCapture(int16_t *dst, int frames);
    int  capturePending() const;

    // Driver-visible counters.
    void noteCaptureOverrun();
    uint32_t renderUnderruns() const;
    uint32_t captureOverruns() const;

    bool running() const;
    void setRunning(bool on);

    // Exposed for tests that need the raw layout.
    OpenAes67SharedAudio *header() const { return hdr_; }

private:
    // The factories own the OS handles and hand them to the ring.
    friend std::unique_ptr<SharedAudioRing> createSharedAudio(const wchar_t *,
                                                              const SharedAudioConfig &,
                                                              std::string *);
    friend std::unique_ptr<SharedAudioRing> openSharedAudio(const wchar_t *,
                                                            std::string *);

    int16_t *renderPtr() const;
    int16_t *capturePtr() const;

    OpenAes67SharedAudio *hdr_ = nullptr;
    uint8_t              *base_ = nullptr;
    size_t                bytes_ = 0;
    bool                  valid_ = false;
    bool                  ownsMapping_ = false;
    void                 *mapping_ = nullptr;
    // The section handle must stay open: a section object is destroyed when its
    // last handle closes, even if views still exist, and the name then stops
    // resolving for OpenFileMapping. Closing it early made the second attach in
    // the self-test fail.
    void                 *section_ = nullptr;
    std::string           error_;
};

// Creates a named section (user mode acts as owner) and initialises its header.
// Returns nullptr and fills `err` on failure.
std::unique_ptr<SharedAudioRing> createSharedAudio(const wchar_t *name,
                                                   const SharedAudioConfig &cfg,
                                                   std::string *err);

// Attaches to an existing section created by someone else (normally the driver).
std::unique_ptr<SharedAudioRing> openSharedAudio(const wchar_t *name,
                                                 std::string *err);

} // namespace openaes67
