// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace openaes67 {

// Converts interleaved frames between channel counts: takes the first `to`
// channels when narrowing, and duplicates channel 0 for the extra outputs when
// widening (which makes a mono stream play on both sides of a stereo device).
// `out` must hold frames * to samples. Returns frames converted.
int convertChannels(const int16_t *in, int frames, int from, int to, int16_t *out);

// Pull model: the engine asks the source for interleaved 16-bit frames.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Fills dst with up to frames*channels samples.
    // Returns the number of frames actually produced (may be less).
    virtual int read(int16_t *dst, int frames) = 0;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
    virtual const char *backendName() const { return "unknown"; }
};

// Push model: the engine hands decoded frames to a sink.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Consumes frames*channels samples. Returns frames consumed.
    virtual int write(const int16_t *src, int frames) = 0;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
    virtual const char *backendName() const { return "unknown"; }

    // Frames sitting in the device buffer that have not been played yet.
    // Returns -1 when the backend cannot report it. This is the only way to
    // tell what the device actually played, as opposed to what we handed over:
    // "bytes accepted by write()" is not the same measurement.
    virtual int deviceBufferedFrames() const { return -1; }
};

// Factory declarations; implemented per backend (WASAPI on Windows).
// Returning nullptr means the backend is unavailable.
AudioSource *createWavFileSource(const char *path);   // offline test / file playback
AudioSource *createWasapiLoopbackSource(const wchar_t *deviceIdOrNull);
// Captures a real input endpoint (a microphone or a virtual cable's capture
// side), i.e. what an application records from. Loopback capture records what a
// render endpoint plays; this records what a capture endpoint produces.
AudioSource *createWasapiCaptureSource(const wchar_t *deviceIdOrNull);
// sampleRate/channels: requested stream format. Shared mode converts to it
// (AUTOCONVERTPCM), so the rate used for RTP framing and for playback stays the
// same. Pass 0 to accept whatever the device mixes at.
AudioSink   *createWasapiRenderSink(const wchar_t *deviceIdOrNull,
                                    int sampleRate = 0, int channels = 0);
// Tries WASAPI exclusive mode (bypasses the audio engine and every APO).
// Falls back to shared mode when the device rejects the format.
AudioSink   *createWasapiRenderSinkExclusive(const wchar_t *deviceIdOrNull);

// ASIO render sink. Only available when the build has the ASIO backend; call
// asioBackendAvailable() (audio_devices.h) first. `driverName` is the registry
// key under HKLM\SOFTWARE\ASIO, e.g. "Madis UT44".
AudioSink   *createAsioRenderSink(const char *driverName, int channels, int sampleRate,
                                  std::string *err);

} // namespace openaes67
