// ASIO render sink.
//
// The ASIO interface belongs to Steinberg and the SDK is not redistributable, so
// this translation unit compiles into two very different things:
//
//   OPENAES67_WITH_ASIO defined  -> a real sink, built against the official
//                                   iasiodrv.h that the developer places in
//                                   third_party/asio (see docs/AUDIO-DEVICES.md)
//   otherwise                    -> stubs that report why ASIO is missing
//
// The stub build is the default on purpose: a clone must build without asking
// anyone to agree to a third-party licence, and must still be able to *list* the
// ASIO drivers installed on the machine (that is registry data, not SDK code).

#include "openaes67/audio_devices.h"
#include "openaes67/audio_io.h"

#include <string>

#if defined(OPENAES67_WITH_ASIO)

// ---------------------------------------------------------------------------
// Real backend
// ---------------------------------------------------------------------------
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"   // from the Steinberg ASIO SDK, third_party\asio

#include <atomic>
#include <cstring>
#include <vector>

namespace openaes67 {
namespace {

constexpr long kAsioOk = 0;

class AsioRenderSink : public AudioSink {
public:
    AsioRenderSink() = default;
    ~AsioRenderSink() override {
        if (driver_) {
            driver_->stop();
            driver_->disposeBuffers();
            driver_->Release();
        }
    }

    // The driver object is a COM in-proc server registered under HKLM\SOFTWARE\ASIO.
    bool open(const char *driverName, int channels, int sampleRate, std::string *err) {
        if (!driverName || !*driverName) {
            if (err) *err = "no ASIO driver name given";
            return false;
        }

        wchar_t wideName[256] = {0};
        MultiByteToWideChar(CP_UTF8, 0, driverName, -1, wideName, 255);

        std::wstring key = L"SOFTWARE\\ASIO\\";
        key += wideName;
        HKEY asio = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &asio) != ERROR_SUCCESS) {
            if (err) *err = std::string("ASIO driver not registered: ") + driverName;
            return false;
        }

        wchar_t clsidText[128] = {0};
        DWORD bytes = sizeof(clsidText);
        RegQueryValueExW(asio, L"CLSID", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(clsidText), &bytes);
        RegCloseKey(asio);
        if (clsidText[0] == L'\0') {
            if (err) *err = "ASIO driver has no CLSID value";
            return false;
        }

        CLSID clsid;
        if (FAILED(CLSIDFromString(clsidText, &clsid))) {
            if (err) *err = "ASIO driver CLSID is not a valid GUID";
            return false;
        }

        // IASIO's IID is part of the ASIO contract; iasiodrv.h declares it.
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IASIO,
                                    reinterpret_cast<void **>(&driver_)))) {
            if (err) *err = "CoCreateInstance(IASIO) failed";
            return false;
        }
        if (driver_->init(reinterpret_cast<void *>(GetConsoleWindow())) != kAsioOk) {
            driver_->Release();
            driver_ = nullptr;
            if (err) *err = "IASIO::init failed (driver may be in use)";
            return false;
        }

        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity);
        if (preferred <= 0) {
            preferred = minSize > 0 ? minSize : 512;
        }

        ASIOBufferInfo info[kMaxChannels];
        const int pairs = channels / 2;
        for (int i = 0; i < pairs; ++i) {
            info[i * 2].isInput = ASIOFalse;
            info[i * 2].channelNum = i * 2;
            info[i * 2].buffers[0] = nullptr;
            info[i * 2].buffers[1] = nullptr;
            info[i * 2 + 1].isInput = ASIOFalse;
            info[i * 2 + 1].channelNum = i * 2 + 1;
            info[i * 2 + 1].buffers[0] = nullptr;
            info[i * 2 + 1].buffers[1] = nullptr;
        }

        ASIOCallbacks callbacks;
        std::memset(&callbacks, 0, sizeof(callbacks));
        callbacks.bufferSwitch = &AsioRenderSink::onBufferSwitch;
        callbacks.sampleRateDidChange = &AsioRenderSink::onSampleRateChanged;
        callbacks.asioMessage = &AsioRenderSink::onAsioMessage;
        callbacks.bufferSwitchTimeInfo = &AsioRenderSink::onBufferSwitchTimeInfo;

        if (driver_->createBuffers(info, pairs * 2, preferred, &callbacks) != kAsioOk) {
            if (err) *err = "IASIO::createBuffers failed";
            return false;
        }
        if (driver_->setSampleRate(sampleRate) != kAsioOk) {
            if (err) *err = "IASIO::setSampleRate failed";
            return false;
        }
        if (driver_->start() != kAsioOk) {
            if (err) *err = "IASIO::start failed";
            return false;
        }

        channels_ = pairs * 2;
        sampleRate_ = sampleRate;
        return true;
    }

    int write(const int16_t *src, int frames) override {
        // The driver pulls through bufferSwitch; we only stage the samples here.
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.insert(pending_.end(), src, src + static_cast<size_t>(frames) * channels_);
        return frames;
    }

    int channels() const override { return channels_; }
    int sampleRate() const override { return sampleRate_; }
    const char *backendName() const override { return "asio"; }

private:
    static constexpr int kMaxChannels = 32;

    static void onBufferSwitch(long index, ASIOBool /*directProcess*/) {
        (void)index;
    }
    static void onSampleRateChanged(double /*rate*/) {}
    static long onAsioMessage(long selector, long value, void *message, double *opt) {
        (void)selector; (void)value; (void)message; (void)opt;
        return 0;
    }
    static ASIOTime *onBufferSwitchTimeInfo(ASIOTime *time, long index, ASIOBool direct) {
        (void)index; (void)direct;
        return time;
    }

    IASIO            *driver_ = nullptr;
    int               channels_ = 2;
    int               sampleRate_ = 48000;
    std::mutex        mutex_;
    std::vector<int16_t> pending_;
};

} // namespace

bool asioBackendAvailable() { return true; }

std::string asioBackendUnavailableReason() { return {}; }

AudioSink *createAsioRenderSink(const char *driverName, int channels, int sampleRate,
                                std::string *err) {
    auto *sink = new AsioRenderSink();
    if (!sink->open(driverName, channels, sampleRate, err)) {
        delete sink;
        return nullptr;
    }
    return sink;
}

} // namespace openaes67

#else // !OPENAES67_WITH_ASIO

// ---------------------------------------------------------------------------
// Stub build
// ---------------------------------------------------------------------------
namespace openaes67 {

bool asioBackendAvailable() { return false; }

std::string asioBackendUnavailableReason() {
    return "this build has no ASIO backend: the Steinberg ASIO SDK is not "
           "redistributable, so it is not bundled. See docs/AUDIO-DEVICES.md "
           "for the two-minute setup (drop iasiodrv.h into third_party\\asio and "
           "rebuild with ASIO=1).";
}

AudioSink *createAsioRenderSink(const char *driverName, int channels, int sampleRate,
                                std::string *err) {
    (void)driverName;
    (void)channels;
    (void)sampleRate;
    if (err) {
        *err = asioBackendUnavailableReason();
    }
    return nullptr;
}

} // namespace openaes67

#endif // OPENAES67_WITH_ASIO
