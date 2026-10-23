// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.


// WASAPI backends for OpenAES67.
//
// WasapiCapture : loopback capture of the default render device (WASAPI shared mode)
// WasapiRender  : playback to the default render device
//
// Scope for M3: shared mode only, the device mix format is used as-is.
// Sample-rate conversion (device 44.1 kHz -> network 48 kHz) is M4 work; both
// classes expose the device rate so the caller can adopt it.
#include "openaes67/audio_io.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace openaes67 {

namespace {

bool isFloatFormat(const WAVEFORMATEX *fmt) {
    if (!fmt) {
        return false;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return false;
}

inline int16_t floatToInt16(float v) {
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}

inline float int16ToFloat(int16_t v) {
    return static_cast<float>(v) / 32768.0f;
}

// Converts one interleaved buffer of fmt to interleaved int16.
void convertToInt16(const BYTE *src, UINT32 frames, int channels, bool float32,
                    int16_t *dst) {
    const int n = static_cast<int>(frames) * channels;
    if (float32) {
        const float *f = reinterpret_cast<const float *>(src);
        for (int i = 0; i < n; ++i) {
            dst[i] = floatToInt16(f[i]);
        }
    } else {
        std::memcpy(dst, src, static_cast<size_t>(n) * 2);
    }
}

void convertFromInt16(const int16_t *src, UINT32 frames, int channels, bool float32,
                      BYTE *dst) {
    const int n = static_cast<int>(frames) * channels;
    if (float32) {
        float *f = reinterpret_cast<float *>(dst);
        for (int i = 0; i < n; ++i) {
            f[i] = int16ToFloat(src[i]);
        }
    } else {
        std::memcpy(dst, src, static_cast<size_t>(n) * 2);
    }
}

// ------------------------------------------------------------------ capture
// Opens the requested endpoint, or the system default when none was given.
// Until this existed every backend silently used the default device, so a
// machine with an onboard codec plus a USB interface had no way to choose.
HRESULT openEndpoint(IMMDeviceEnumerator *enumerator, const std::wstring &deviceId,
                     IMMDevice **device, bool captureEndpoint = false) {
    if (!deviceId.empty()) {
        return enumerator->GetDevice(deviceId.c_str(), device);
    }
    // Capture endpoints are what applications record from; render endpoints are
    // what loopback capture listens to. Choosing the wrong one is how a
    // "record from the virtual cable" request ends up opening a speaker.
    return enumerator->GetDefaultAudioEndpoint(captureEndpoint ? eCapture : eRender,
                                               eConsole, device);
}
class WasapiCapture : public AudioSource {
public:
    ~WasapiCapture() override { shutdown(); }

    void setDeviceId(const wchar_t *id) { deviceId_ = (id != nullptr) ? id : L""; }

    // False (default): loopback-capture a render endpoint, i.e. record what a
    // speaker is playing. True: capture a real input endpoint, which is what an
    // application like a DAW or Audition records from.
    void setCaptureEndpoint(bool on) { captureEndpoint_ = on; }

    bool init(std::string *err) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator_));
        if (FAILED(hr)) {
            if (err) { *err = "CoCreateInstance(MMDeviceEnumerator) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = openEndpoint(enumerator_, deviceId_, &device_, captureEndpoint_);
    if (SUCCEEDED(hr)) {
        // Print the endpoint that was actually opened: the previous code
        // reported the caller's label, which hid the fact that the id was
        // being ignored and the default device was used instead.
        IPropertyStore *props = nullptr;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR) {
                char utf8[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, utf8, sizeof(utf8),
                                    nullptr, nullptr);
                std::fprintf(stderr, "[wasapi] opened: %s\n", utf8);
            }
            PropVariantClear(&name);
            props->Release();
        }
    }
        if (FAILED(hr)) {
            if (err) { *err = "no audio endpoint (capture=" +
                              std::string(captureEndpoint_ ? "yes" : "no") +
                              "), hr=0x" + hex(hr); }
            return false;
        }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&client_));
        if (FAILED(hr)) {
            if (err) { *err = "Activate(IAudioClient) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetMixFormat(&mixFormat_);
        if (FAILED(hr)) {
            if (err) { *err = "GetMixFormat failed, hr=0x" + hex(hr); }
            return false;
        }
        float32_ = isFloatFormat(mixFormat_);
        channels_ = mixFormat_->nChannels;
        rate_ = static_cast<int>(mixFormat_->nSamplesPerSec);

        // Loopback captures what a render device is playing; a real input endpoint
        // is captured directly. Passing the loopback flag to an input endpoint
        // fails with E_INVALIDARG, which is exactly how this was discovered.
        const DWORD flags = captureEndpoint_ ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mixFormat_, nullptr);
        if (FAILED(hr)) {
            if (err) { *err = std::string("IAudioClient::Initialize(") +
                              (captureEndpoint_ ? "capture" : "loopback") +
                              ") failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr)) {
            if (err) { *err = "GetBufferSize failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void **>(&capture_));
        if (FAILED(hr)) {
            if (err) { *err = "GetService(IAudioCaptureClient) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->Start();
        if (FAILED(hr)) {
            if (err) { *err = "IAudioClient::Start failed, hr=0x" + hex(hr); }
            return false;
        }
        return true;
    }

    int read(int16_t *dst, int frames) override {
        if (!capture_ || !dst || frames <= 0) {
            return 0;
        }
        int produced = 0;

        while (produced < frames) {
            UINT32 packetFrames = 0;
            if (FAILED(capture_->GetNextPacketSize(&packetFrames))) {
                break;
            }
            if (packetFrames == 0) {
                // Non-blocking: return immediately when nothing is ready.
                //
                // This used to spin for up to 20 ms (measured with GetTickCount,
                // whose 15.6 ms granularity made every wait ~31 ms). The sender
                // calls read() once per 10 ms tick, so it was starved: 317
                // packets/s instead of 1000. Pacing belongs to the caller.
                break;
            }
            BYTE *data = nullptr;
            UINT32 got = 0;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &got, &flags, nullptr, nullptr))) {
                break;
            }
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            int want = static_cast<int>(got);
            if (produced + want > frames) {
                want = frames - produced;
            }
            if (silent) {
                std::memset(dst + static_cast<size_t>(produced) * channels_, 0,
                            static_cast<size_t>(want) * channels_ * 2);
            } else {
                convertToInt16(data, static_cast<UINT32>(want), channels_, float32_,
                               dst + static_cast<size_t>(produced) * channels_);
            }
            capture_->ReleaseBuffer(got);
            produced += want;

            if (want < static_cast<int>(got)) {
                break;   // caller buffer is full
            }
        }
        return produced;
    }

    int channels() const override { return channels_; }
    int sampleRate() const override { return rate_; }
    const char *backendName() const override { return "wasapi-shared"; }

private:
    static std::string hex(HRESULT hr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
        return std::string(buf);
    }

    void shutdown() {
        if (client_) {
            client_->Stop();
        }
        if (capture_)  { capture_->Release();   capture_ = nullptr; }
        if (client_)   { client_->Release();    client_ = nullptr; }
        if (mixFormat_) { CoTaskMemFree(mixFormat_); mixFormat_ = nullptr; }
        if (device_)   { device_->Release();    device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    }

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice           *device_ = nullptr;
    std::wstring         deviceId_;
    IAudioClient        *client_ = nullptr;
    IAudioCaptureClient *capture_ = nullptr;
    WAVEFORMATEX        *mixFormat_ = nullptr;
    UINT32               bufferFrames_ = 0;
    int                  channels_ = 0;
    int                  rate_ = 0;
    bool                 float32_ = false;
    bool                 captureEndpoint_ = false;
};

// ------------------------------------------------------------------- render
class WasapiRender : public AudioSink {
public:
    ~WasapiRender() override { shutdown(); }

    void setDeviceId(const wchar_t *id) { deviceId_ = (id != nullptr) ? id : L""; }
    // Requested stream format. In shared mode WASAPI can convert for us
    // (AUTOCONVERTPCM), which keeps the RTP rate and the playback rate identical --
    // the receive path used to adopt the device rate instead and then framed
    // packets at the wrong rate.
    void setFormat(int sampleRate, int channels) {
        wantRate_ = sampleRate;
        wantChannels_ = channels;
    }

    bool init(std::string *err) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator_));
        if (FAILED(hr)) {
            if (err) { *err = "CoCreateInstance(MMDeviceEnumerator) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = openEndpoint(enumerator_, deviceId_, &device_);
    if (SUCCEEDED(hr)) {
        // Print the endpoint that was actually opened: the previous code
        // reported the caller's label, which hid the fact that the id was
        // being ignored and the default device was used instead.
        IPropertyStore *props = nullptr;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR) {
                char utf8[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, utf8, sizeof(utf8),
                                    nullptr, nullptr);
                std::fprintf(stderr, "[wasapi] opened: %s\n", utf8);
            }
            PropVariantClear(&name);
            props->Release();
        }
    }
        if (FAILED(hr)) {
            if (err) { *err = "no default render endpoint, hr=0x" + hex(hr); }
            return false;
        }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&client_));
        if (FAILED(hr)) {
            if (err) { *err = "Activate(IAudioClient) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetMixFormat(&mixFormat_);
        if (FAILED(hr)) {
            if (err) { *err = "GetMixFormat failed, hr=0x" + hex(hr); }
            return false;
        }
        float32_ = isFloatFormat(mixFormat_);
        channels_ = mixFormat_->nChannels;
        rate_ = static_cast<int>(mixFormat_->nSamplesPerSec);

        // Use the requested stream format when it differs from the device mix
        // format: shared mode converts it for us, so the RTP rate is preserved.
        // Exclusive mode cannot convert, which is why the caller gets the device
        // format back from sampleRate() there.
        unsigned int flags = 0;
        if (wantRate_ > 0 && static_cast<int>(mixFormat_->nSamplesPerSec) != wantRate_) {
            WAVEFORMATEXTENSIBLE *want =
                static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
            if (want) {
                ZeroMemory(want, sizeof(*want));
                want->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                want->Format.nChannels = static_cast<WORD>(wantChannels_ > 0 ? wantChannels_ : 2);
                want->Format.nSamplesPerSec = static_cast<DWORD>(wantRate_);
                want->Format.wBitsPerSample = 16;
                want->Format.nBlockAlign = static_cast<WORD>(want->Format.nChannels * 2);
                want->Format.nAvgBytesPerSec =
                    want->Format.nSamplesPerSec * want->Format.nBlockAlign;
                want->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                want->Samples.wValidBitsPerSample = 16;
                want->dwChannelMask = want->Format.nChannels == 2 ? 3u : 0u;
                want->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                CoTaskMemFree(mixFormat_);
                mixFormat_ = reinterpret_cast<WAVEFORMATEX *>(want);
                flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            }
        }
        float32_ = isFloatFormat(mixFormat_);
        channels_ = mixFormat_->nChannels;
        rate_ = static_cast<int>(mixFormat_->nSamplesPerSec);

        // 200 ms shared-mode buffer.
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, mixFormat_, nullptr);
        if (FAILED(hr)) {
            if (err) { *err = "IAudioClient::Initialize failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr)) {
            if (err) { *err = "GetBufferSize failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void **>(&render_));
        if (FAILED(hr)) {
            if (err) { *err = "GetService(IAudioRenderClient) failed, hr=0x" + hex(hr); }
            return false;
        }
        hr = client_->Start();
        if (FAILED(hr)) {
            if (err) { *err = "IAudioClient::Start failed, hr=0x" + hex(hr); }
            return false;
        }
        return true;
    }

    int write(const int16_t *src, int frames) override {
        if (!render_ || !src || frames <= 0) {
            return 0;
        }
        int written = 0;
        const DWORD spinStart = GetTickCount();

        while (written < frames) {
            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) {
                break;
            }
            UINT32 avail = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
            if (avail == 0) {
                if (GetTickCount() - spinStart > 50) {
                    break;
                }
                Sleep(1);
                continue;
            }
            UINT32 want = static_cast<UINT32>(frames - written);
            if (want > avail) {
                want = avail;
            }
            BYTE *dst = nullptr;
            if (FAILED(render_->GetBuffer(want, &dst))) {
                break;
            }
            convertFromInt16(src + static_cast<size_t>(written) * channels_, want,
                             channels_, float32_, dst);
            render_->ReleaseBuffer(want, 0);
            written += static_cast<int>(want);
        }
        return written;
    }

    int channels() const override { return channels_; }
    int sampleRate() const override { return rate_; }
    const char *backendName() const override { return "wasapi-shared"; }

    int deviceBufferedFrames() const override {
        if (!client_) {
            return -1;
        }
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) {
            return -1;
        }
        return static_cast<int>(padding);
    }

private:
    static std::string hex(HRESULT hr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
        return std::string(buf);
    }

    void shutdown() {
        if (client_) {
            client_->Stop();
        }
        if (render_)   { render_->Release();    render_ = nullptr; }
        if (client_)   { client_->Release();    client_ = nullptr; }
        if (mixFormat_) { CoTaskMemFree(mixFormat_); mixFormat_ = nullptr; }
        if (device_)   { device_->Release();    device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    }

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice           *device_ = nullptr;
    std::wstring         deviceId_;
    int                  wantRate_ = 0;
    int                  wantChannels_ = 0;
    IAudioClient        *client_ = nullptr;
    IAudioRenderClient  *render_ = nullptr;
    WAVEFORMATEX        *mixFormat_ = nullptr;
    UINT32               bufferFrames_ = 0;
    int                  channels_ = 0;
    int                  rate_ = 0;
    bool                 float32_ = false;
};

} // namespace

AudioSource *createWasapiLoopbackSource(const wchar_t *deviceIdOrNull) {
    auto *c = new WasapiCapture();
    // The id used to be dropped here, so this factory always opened the default
    // endpoint no matter which device the user picked.
    c->setDeviceId(deviceIdOrNull);
    std::string err;
    if (!c->init(&err)) {
        delete c;
        return nullptr;
    }
    return c;
}

AudioSource *createWasapiCaptureSource(const wchar_t *deviceIdOrNull) {
    auto *c = new WasapiCapture();
    c->setDeviceId(deviceIdOrNull);
    // A real input endpoint, not loopback: this is the side an application
    // records from, so it is what proves a virtual cable actually carries audio.
    c->setCaptureEndpoint(true);
    std::string err;
    if (!c->init(&err)) {
        delete c;
        return nullptr;
    }
    return c;
}

AudioSink *createWasapiRenderSink(const wchar_t *deviceIdOrNull, int sampleRate, int channels) {
    auto *r = new WasapiRender();
    r->setFormat(sampleRate, channels);
    r->setDeviceId(deviceIdOrNull);
    std::string err;
    if (!r->init(&err)) {
        delete r;
        return nullptr;
    }
    return r;
}

} // namespace openaes67
namespace openaes67 {

int convertChannels(const int16_t *in, int frames, int from, int to, int16_t *out) {
    if (!in || !out || frames <= 0 || from <= 0 || to <= 0) {
        return 0;
    }
    if (from == to) {
        std::memcpy(out, in, static_cast<size_t>(frames) * to * sizeof(int16_t));
        return frames;
    }
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < to; ++c) {
            const int src = c < from ? c : 0;
            out[static_cast<size_t>(i) * to + c] = in[static_cast<size_t>(i) * from + src];
        }
    }
    return frames;
}

} // namespace openaes67