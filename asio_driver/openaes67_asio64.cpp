// OpenAES67 ASIO driver: one device, 64 inputs and 64 outputs, 48 kHz.
//
// Direction (see asio_driver/README.md):
//   ASIO inputs  <- shared ring, render buffer   (openaes67 recv writes it)
//   ASIO outputs -> shared ring, capture buffer  (openaes67 send will read it)
//
// The ring carries interleaved int16; ASIO hosts expect 32-bit LSB by default,
// so samples are widened with a shift and narrowed again on the way back. That
// keeps the existing ring ABI instead of widening it for one backend.
//
// If the shared section is missing the device still works and produces silence:
// a host that starts before the network pump must not fail to open the device.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "asio_abi.h"
#include "openaes67/shared_audio_ring.h"

namespace {

constexpr int   kChannels = 64;
constexpr int   kSampleRate = 48000;
constexpr long  kDefaultBufferSize = 256;      // ~5.3 ms at 48 kHz
constexpr GUID  kClassId =
    {0x7c1f2e64, 0x9a3b, 0x4c5d, {0x8e, 0x7f, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f}};

std::string toUtf8(const std::wstring &w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n,
                        nullptr, nullptr);
    return out;
}

// The section name the pump creates. Overridable so two instances can coexist.
std::wstring ringName() {
    wchar_t buf[256] = {0};
    const DWORD n = GetEnvironmentVariableW(L"OPENAES67_SHM", buf, 256);
    if (n > 0 && n < 256) {
        return std::wstring(buf);
    }
    return L"OpenAES67Audio";
}

class CAsioDriver : public IASIO {
public:
    CAsioDriver() : m_Ref(1) {}
    ~CAsioDriver() {
        stop();
        disposeBuffers();
    }

    // ------------------------------------------------------------ IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        // ASIO hosts query for IUnknown or for the interface GUID their SDK
        // defines. Both resolve to the same object, so any request succeeds:
        // refusing an unknown riid is what breaks third-party hosts that use a
        // GUID this build cannot see.
        (void)riid;
        *ppv = static_cast<IASIO *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_Ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&m_Ref);
        if (n == 0) {
            delete this;
        }
        return n;
    }

    // -------------------------------------------------------------- IASIO
    ASIOBool init(void *sysHandle) override {
        (void)sysHandle;
        attachRing();
        m_Initialised = true;
        return ASIOTrue;
    }

    void getDriverName(char *name) override {
        if (name) {
            std::snprintf(name, 32, "%s", "OpenAES67 ASIO 64x64");
        }
    }

    long getDriverVersion() override { return 1; }

    void getErrorMessage(char *string) override {
        if (string) {
            std::snprintf(string, 124, "%s", m_Error.empty() ? "no error" : m_Error.c_str());
        }
    }

    ASIOError start() override {
        if (!m_Initialised || m_BufferSize <= 0 || !m_Callbacks) {
            return ASE_InvalidMode;
        }
        if (m_Running.exchange(true)) {
            return ASE_OK;
        }
        // 1 ms timer resolution: a 256-frame buffer is 5.3 ms and the default
        // 15.6 ms granularity would make every second callback late.
        timeBeginPeriod(1);
        m_Thread = std::thread([this] { pump(); });
        return ASE_OK;
    }

    ASIOError stop() override {
        if (!m_Running.exchange(false)) {
            return ASE_OK;
        }
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
        timeEndPeriod(1);
        return ASE_OK;
    }

    ASIOError getChannels(long *numInputChannels, long *numOutputChannels) override {
        if (!numInputChannels || !numOutputChannels) {
            return ASE_InvalidParameter;
        }
        *numInputChannels = kChannels;
        *numOutputChannels = kChannels;
        return ASE_OK;
    }

    ASIOError getLatencies(long *inputLatency, long *outputLatency) override {
        if (!inputLatency || !outputLatency) {
            return ASE_InvalidParameter;
        }
        *inputLatency = m_BufferSize;
        *outputLatency = m_BufferSize;
        return ASE_OK;
    }

    ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize,
                            long *granularity) override {
        if (!minSize || !maxSize || !preferredSize || !granularity) {
            return ASE_InvalidParameter;
        }
        *minSize = 64;
        *maxSize = 2048;
        *preferredSize = kDefaultBufferSize;
        *granularity = -1;
        return ASE_OK;
    }

    ASIOError canSampleRate(ASIOSampleRate sampleRate) override {
        return std::fabs(sampleRate - kSampleRate) < 0.5 ? ASE_OK : ASE_NoClock;
    }

    ASIOError getSampleRate(ASIOSampleRate *sampleRate) override {
        if (!sampleRate) {
            return ASE_InvalidParameter;
        }
        *sampleRate = kSampleRate;
        return ASE_OK;
    }

    ASIOError setSampleRate(ASIOSampleRate sampleRate) override {
        return canSampleRate(sampleRate);
    }

    ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) override {
        if (!numSources) {
            return ASE_InvalidParameter;
        }
        if (clocks) {
            clocks[0].index = 0;
            clocks[0].associatedChannel = -1;
            clocks[0].associatedGroup = -1;
            clocks[0].isCurrentSource = ASIOTrue;
            std::snprintf(clocks[0].name, sizeof(clocks[0].name), "%s", "AES67 network");
        }
        *numSources = 1;
        return ASE_OK;
    }

    ASIOError setClockSource(long reference) override {
        return reference == 0 ? ASE_OK : ASE_InvalidParameter;
    }

    ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override {
        if (!sPos || !tStamp) {
            return ASE_InvalidParameter;
        }
        *sPos = m_Samples.load();
        *tStamp = m_Samples.load() * 10000000LL / kSampleRate;
        return ASE_OK;
    }

    ASIOError getChannelInfo(ASIOChannelInfo *info) override {
        if (!info) {
            return ASE_InvalidParameter;
        }
        if (info->channel < 0 || info->channel >= kChannels) {
            return ASE_InvalidParameter;
        }
        info->isActive = ASIOTrue;
        info->channelGroup = 0;
        info->type = ASIOSTInt32LSB;
        std::snprintf(info->name, sizeof(info->name), "%s %ld",
                      info->isInput ? "Input" : "Output", info->channel + 1);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels,
                            long bufferSize, ASIOCallbacks *callbacks) override {
        if (!bufferInfos || !callbacks || numChannels <= 0) {
            return ASE_InvalidParameter;
        }
        if (bufferSize < 64 || bufferSize > 2048) {
            return ASE_InvalidParameter;
        }
        disposeBuffers();

        m_BufferSize = bufferSize;
        m_Callbacks = callbacks;
        // The host keeps its own ASIOBufferInfo array and reads the buffer
        // pointers from it, so they must be written into the caller's array --
        // writing into a private copy leaves the host with null buffers.
        m_Infos = bufferInfos;
        m_NumChannels = numChannels;
        m_Storage.assign(static_cast<size_t>(numChannels) * 2 *
                             static_cast<size_t>(bufferSize), 0);

        // The host passes one entry per channel per direction, and the order is
        // not guaranteed: entries 0..63 may be the inputs and 64..127 the outputs,
        // or interleaved. Index by direction and channel number instead of by
        // array position -- using the position read the input buffers when
        // producing capture data, which sent silence to the network.
        m_In.assign(kChannels, nullptr);
        m_Out.assign(kChannels, nullptr);
        for (long i = 0; i < numChannels; ++i) {
            int32_t *base = m_Storage.data() +
                            static_cast<size_t>(i) * 2 * static_cast<size_t>(bufferSize);
            m_Infos[i].buffers[0] = base;
            m_Infos[i].buffers[1] = base + bufferSize;

            const long ch = m_Infos[i].channelNum;
            if (ch < 0 || ch >= kChannels) {
                continue;
            }
            if (m_Infos[i].isInput) {
                m_In[static_cast<size_t>(ch)] = &m_Infos[i];
            } else {
                m_Out[static_cast<size_t>(ch)] = &m_Infos[i];
            }
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        stop();
        m_Infos = nullptr;
        m_NumChannels = 0;
        m_In.clear();
        m_Out.clear();
        m_Storage.clear();
        m_Callbacks = nullptr;
        m_BufferSize = 0;
        return ASE_OK;
    }

    ASIOError controlPanel() override { return ASE_NotPresent; }

    ASIOError future(long selector, void *opt) override {
        (void)opt;
        switch (selector) {
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        default:
            return ASE_NotPresent;
        }
    }

    ASIOError outputReady() override { return ASE_OK; }

private:
    void attachRing() {
        std::string err;
        m_Ring = openaes67::openSharedAudio(ringName().c_str(), &err);
        if (!m_Ring || !m_Ring->valid()) {
            // The host may start before the network pump. Creating the section here
            // means the device still opens; the pump attaches when it starts.
            openaes67::SharedAudioConfig cfg;
            cfg.channels = kChannels;
            cfg.sampleRate = kSampleRate;
            cfg.capacityFrames = 48000;
            m_Ring = openaes67::createSharedAudio(ringName().c_str(), cfg, &err);
        }
        if (!m_Ring || !m_Ring->valid()) {
            m_Ring.reset();
            const std::string name = toUtf8(ringName());
            m_Error = "shared section '" + name + "' not found; routing silence (" + err + ")";
            m_RingChannels = 0;
        } else {
            m_RingChannels = m_Ring->channels();
            m_Error.clear();
        }
    }

    // One host buffer per tick: network -> inputs, outputs -> network, then hand
    // the host its callback.
    void pump() {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(
            1000000000LL * m_BufferSize / kSampleRate);
        auto next = clock::now();
        const int n = m_BufferSize;
        const int ringCh = m_RingChannels > 0 ? m_RingChannels : kChannels;
        std::vector<int16_t> ringBuf(static_cast<size_t>(n) * ringCh, 0);

        while (m_Running.load()) {
            const long buf = m_Index;
            m_Index ^= 1;

            // ---- network -> host inputs
            int got = 0;
            if (m_Ring) {
                got = m_Ring->readRender(ringBuf.data(), n);
            }
            for (long ch = 0; ch < kChannels; ++ch) {
                int32_t *dst =
                    m_In[static_cast<size_t>(ch)] && m_In[static_cast<size_t>(ch)]->buffers[buf]
                        ? static_cast<int32_t *>(
                              m_In[static_cast<size_t>(ch)]->buffers[buf])
                        : nullptr;
                if (!dst) {
                    continue;
                }
                if (m_Ring && ch < ringCh) {
                    for (int f = 0; f < got; ++f) {
                        dst[f] = static_cast<int32_t>(
                                     ringBuf[static_cast<size_t>(f) * ringCh + ch])
                                 << 16;
                    }
                }
                // Channels with no network source, and the tail of a short read,
                // are silent rather than stale.
                for (int f = (m_Ring && ch < ringCh) ? got : 0; f < n; ++f) {
                    dst[f] = 0;
                }
            }

            // ---- host outputs -> network
            if (m_Ring) {
                // Clearing the whole interleaved buffer is only needed when the
                // writes below cannot cover every slot: extra ring channels, or a
                // host that exposed fewer buffers than the ring has. Otherwise it
                // is 30 KB of memset per tick (5.6 MB/s at 5.3 ms) discarded again
                // by the very next loop.
                bool writesEverySlot = ringCh >= kChannels;
                for (long ch = 0; writesEverySlot && ch < kChannels; ++ch) {
                    if (!m_Out[static_cast<size_t>(ch)] ||
                        !m_Out[static_cast<size_t>(ch)]->buffers[buf]) {
                        writesEverySlot = false;
                    }
                }
                if (!writesEverySlot) {
                    std::memset(ringBuf.data(), 0, ringBuf.size() * sizeof(int16_t));
                }
                for (long ch = 0; ch < kChannels && ch < ringCh; ++ch) {
                    const int32_t *src =
                        m_Out[static_cast<size_t>(ch)] &&
                                m_Out[static_cast<size_t>(ch)]->buffers[buf]
                            ? static_cast<const int32_t *>(
                                  m_Out[static_cast<size_t>(ch)]->buffers[buf])
                            : nullptr;
                    if (!src) {
                        continue;
                    }
                    for (int f = 0; f < n; ++f) {
                        ringBuf[static_cast<size_t>(f) * ringCh + ch] =
                            static_cast<int16_t>(src[f] >> 16);
                    }
                }
                m_Ring->writeCapture(ringBuf.data(), n);
            }

            if (m_Callbacks && m_Callbacks->bufferSwitch) {
                m_Callbacks->bufferSwitch(buf, ASIOTrue);
            }
            m_Samples.fetch_add(n);

            next += period;
            std::this_thread::sleep_until(next);
            const auto now = clock::now();
            if (now - next > period * 4) {
                next = now;   // stalled (debugger, sleep): resync, do not burst
            }
        }
    }

    volatile LONG     m_Ref;
    bool              m_Initialised = false;
    std::atomic<bool> m_Running{false};
    std::thread       m_Thread;
    ASIOCallbacks    *m_Callbacks = nullptr;
    ASIOBufferInfo  *m_Infos = nullptr;   // the host's array, not a copy
    std::vector<ASIOBufferInfo *> m_In;   // by channel number
    std::vector<ASIOBufferInfo *> m_Out;
    long             m_NumChannels = 0;
    std::vector<int32_t>        m_Storage;
    long              m_BufferSize = 0;
    long              m_Index = 0;
    std::atomic<long long> m_Samples{0};
    std::string       m_Error;
    std::unique_ptr<openaes67::SharedAudioRing> m_Ring;
    int               m_RingChannels = 0;
};

// ---------------------------------------------------------------- COM plumbing
volatile LONG g_LockCount = 0;

class CClassFactory : public IClassFactory {
public:
    CClassFactory() : m_Ref(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_Ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&m_Ref);
        if (n == 0) {
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID riid,
                                             void **ppv) override {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        auto *obj = new (std::nothrow) CAsioDriver();
        if (!obj) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = obj->QueryInterface(riid, ppv);
        if (FAILED(hr)) {
            delete obj;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_LockCount);
        } else {
            InterlockedDecrement(&g_LockCount);
        }
        return S_OK;
    }

private:
    volatile LONG m_Ref;
};

} // namespace

// ------------------------------------------------------------------- exports
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (!IsEqualCLSID(rclsid, kClassId)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto *factory = new (std::nothrow) CClassFactory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    if (FAILED(hr)) {
        delete factory;
    }
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return g_LockCount == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
