// WASAPI exclusive-mode render backend.
//
// Exclusive mode bypasses the Windows audio engine, every APO and the system
// mixer, so the samples reach the driver unchanged. That is what an AES67
// endpoint needs on the output side.
//
// Important limitation: WASAPI loopback capture (AUDCLNT_STREAMFLAGS_LOOPBACK)
// is only valid in SHARED mode. An exclusive-mode stream also bypasses the
// shared mix, so a loopback capture sees nothing while it plays. Exclusive mode
// therefore fixes the OUTPUT path only; see docs/FINDINGS.md.
#include "openaes67/audio_io.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstring>
#include <string>

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

void fillFormat(WAVEFORMATEXTENSIBLE *wf, int channels, int rate, int bits, bool f32) {
    std::memset(wf, 0, sizeof(*wf));
    wf->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf->Format.nChannels = static_cast<WORD>(channels);
    wf->Format.nSamplesPerSec = static_cast<DWORD>(rate);
    wf->Format.wBitsPerSample = static_cast<WORD>(bits);
    wf->Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    wf->Format.nAvgBytesPerSec = wf->Format.nSamplesPerSec * wf->Format.nBlockAlign;
    wf->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf->Samples.wValidBitsPerSample = static_cast<WORD>(bits);
    wf->dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    wf->SubFormat = f32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

std::string hexHr(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

inline int16_t floatToInt16(float v) {
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}
inline float int16ToFloat(int16_t v) { return static_cast<float>(v) / 32768.0f; }

// Opens the requested endpoint, or the system default when none was given.
// Until this existed every backend silently used the default device, so a
// machine with an onboard codec plus a USB interface had no way to choose.
HRESULT openEndpoint(IMMDeviceEnumerator *enumerator, const std::wstring &deviceId,
                     IMMDevice **device) {
    if (!deviceId.empty()) {
        return enumerator->GetDevice(deviceId.c_str(), device);
    }
    return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device);
}
class WasapiRenderExclusive : public AudioSink {
public:
    ~WasapiRenderExclusive() override { shutdown(); }

    void setDeviceId(const wchar_t *id) { deviceId_ = (id != nullptr) ? id : L""; }

    bool init(std::string *err) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator_));
        if (FAILED(hr)) {
            if (err) { *err = "CoCreateInstance failed, hr=0x" + hexHr(hr); }
            return false;
        }
        hr = openEndpoint(enumerator_, deviceId_, &device_);
        if (FAILED(hr)) {
            if (err) { *err = "no default render endpoint, hr=0x" + hexHr(hr); }
            return false;
        }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&client_));
        if (FAILED(hr)) {
            if (err) { *err = "Activate failed, hr=0x" + hexHr(hr); }
            return false;
        }

        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        client_->GetDevicePeriod(&defPeriod, &minPeriod);

        // Try exclusive first: 16-bit PCM at the device rate, then the same in
        // float, then 48 kHz variants. Only S_OK is accepted (S_FALSE means the
        // format is not usable as-is).
        struct Cand { int rate; int bits; bool f32; };
        const Cand cands[] = {
            { 48000, 16, false },
            { 48000, 32, true  },
        };

        for (const Cand &c : cands) {
            WAVEFORMATEXTENSIBLE wf;
            fillFormat(&wf, 2, c.rate, c.bits, c.f32);
            WAVEFORMATEX *closest = nullptr;
            hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wf.Format, &closest);
            if (closest) {
                CoTaskMemFree(closest);
                closest = nullptr;
            }
            if (hr != S_OK) {
                continue;
            }
            // A single default period leaves no headroom: a starved exclusive
            // stream can stop advancing. Ask for 4 periods (>= 20 ms).
                // Four periods. Reducing this to one or two lowered the device buffering to
    // ~20 ms as intended, but the exclusive stream then stalled permanently with
    // its buffer full (devpad stuck, writes returning 0 forever). Lowering it
    // safely needs proper underrun recovery in the sink first.
    REFERENCE_TIME dur = defPeriod > 0 ? defPeriod * 4 : 1000000;
            if (dur < 200000) {
                dur = 200000;
            }
            hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur,
                                     0, &wf.Format, nullptr);
            if (SUCCEEDED(hr)) {
                channels_ = 2;
                rate_ = c.rate;
                float32_ = c.f32;
                exclusive_ = true;
                break;
            }
        }

        if (!exclusive_) {
            // Fall back to shared mode with the device mix format.
            hr = client_->GetMixFormat(&mixFormat_);
            if (FAILED(hr)) {
                if (err) { *err = "GetMixFormat failed, hr=0x" + hexHr(hr); }
                return false;
            }
            float32_ = isFloatFormat(mixFormat_);
            channels_ = mixFormat_->nChannels;
            rate_ = static_cast<int>(mixFormat_->nSamplesPerSec);
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0,
                                     mixFormat_, nullptr);
            if (FAILED(hr)) {
                if (err) { *err = "shared Initialize failed, hr=0x" + hexHr(hr); }
                return false;
            }
        }

        hr = client_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr)) {
            if (err) { *err = "GetBufferSize failed, hr=0x" + hexHr(hr); }
            return false;
        }
        hr = client_->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void **>(&render_));
        if (FAILED(hr)) {
            if (err) { *err = "GetService failed, hr=0x" + hexHr(hr); }
            return false;
        }
        hr = client_->Start();
        if (FAILED(hr)) {
            if (err) { *err = "Start failed, hr=0x" + hexHr(hr); }
            return false;
        }
        return true;
    }

    int write(const int16_t *src, int frames) override {
        if (!render_ || !src || frames <= 0) {
            return 0;
        }
        int written = 0;
        const DWORD spin = GetTickCount();
        while (written < frames) {
            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) {
                break;
            }
            const UINT32 avail = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
            if (avail == 0) {
                if (GetTickCount() - spin > 50) {
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
            const int n = static_cast<int>(want) * channels_;
            if (float32_) {
                float *f = reinterpret_cast<float *>(dst);
                for (int i = 0; i < n; ++i) {
                    f[i] = int16ToFloat(src[static_cast<size_t>(written) * channels_ + i]);
                }
            } else {
                std::memcpy(dst, src + static_cast<size_t>(written) * channels_,
                            static_cast<size_t>(n) * 2);
            }
            render_->ReleaseBuffer(want, 0);
            written += static_cast<int>(want);
        }
        return written;
    }

    int channels() const override { return channels_; }
    int sampleRate() const override { return rate_; }
    const char *backendName() const override {
        return exclusive_ ? "wasapi-exclusive" : "wasapi-shared-fallback";
    }

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
    void shutdown() {
        if (client_) { client_->Stop(); }
        if (render_)    { render_->Release();    render_ = nullptr; }
        if (client_)    { client_->Release();    client_ = nullptr; }
        if (mixFormat_) { CoTaskMemFree(mixFormat_); mixFormat_ = nullptr; }
        if (device_)    { device_->Release();    device_ = nullptr; }
        if (enumerator_){ enumerator_->Release();enumerator_ = nullptr; }
    }

    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice           *device_ = nullptr;
    std::wstring         deviceId_;
    IAudioClient        *client_ = nullptr;
    IAudioRenderClient  *render_ = nullptr;
    WAVEFORMATEX        *mixFormat_ = nullptr;
    UINT32               bufferFrames_ = 0;
    int                  channels_ = 0;
    int                  rate_ = 0;
    bool                 float32_ = false;
    bool                 exclusive_ = false;
};

} // namespace

AudioSink *createWasapiRenderSinkExclusive(const wchar_t * /*deviceIdOrNull*/) {
    auto *r = new WasapiRenderExclusive();
    std::string err;
    if (!r->init(&err)) {
        delete r;
        return nullptr;
    }
    return r;
}

} // namespace openaes67
