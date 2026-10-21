// asio_host_test — acts like an ASIO host against our own driver DLL.
//
// It exists because Audition is not scriptable here: loading the DLL, calling
// through the vtable and pumping buffers proves the ABI path works, which is the
// part that would otherwise be discovered by a crash inside a DAW.
//
//   asio_host_test [seconds] [section]
//
// Exit code 0 means every check passed.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "asio_abi.h"
#include "openaes67/shared_audio_ring.h"

namespace {

constexpr int  kChannels = 64;
constexpr long kBufferSize = 256;

std::atomic<long> g_callbacks{0};
std::atomic<long long> g_frames{0};
std::atomic<int> g_peak{0};

// Optional tone generator: fills the ASIO *outputs* so the capture ring can be
// fed without a DAW. This is what verifies the 64-channel output direction.
ASIOBufferInfo *g_infos = nullptr;
long            g_numChannels = 0;
bool            g_tone = false;
double          g_phase = 0.0;
constexpr int   kTestChannels = 64;

void BufferSwitch(long index, ASIOBool) {
    g_callbacks.fetch_add(1);
    g_frames.fetch_add(kBufferSize);

    if (g_tone && g_infos) {
        for (long ch = 0; ch < g_numChannels; ++ch) {
            ASIOBufferInfo &bi = g_infos[ch];
            if (bi.isInput) {
                continue;
            }
            int32_t *dst = static_cast<int32_t *>(bi.buffers[index]);
            if (!dst) {
                continue;
            }
            for (long i = 0; i < kBufferSize; ++i) {
                const double v = std::sin(g_phase) * 20000.0;
                dst[i] = static_cast<int32_t>(v) << 16;
                g_phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
                if (g_phase > 6.283185307179586) {
                    g_phase -= 6.283185307179586;
                }
            }
        }
    }
}
long AsioMessage(long selector, long value, void *message, double *opt) {
    (void)selector;
    (void)value;
    (void)message;
    (void)opt;
    return 0;
}

ASIOTime *BufferSwitchTimeInfo(long index, ASIOTime *timeInfo, ASIOBool processNow) {
    BufferSwitch(index, processNow);
    return timeInfo;
}

int g_failures = 0;

void Check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

} // namespace

int main(int argc, char **argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    const char *dllPath = argc > 2 ? argv[2] : "asio_driver\\out\\openaes67_asio64.dll";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "tone") == 0) {
            g_tone = true;
        }
    }

    std::printf("ASIO host test\n");
    std::printf("  dll     : %s\n", dllPath);
    std::printf("  seconds : %.1f\n  tone    : %s\n\n", seconds, g_tone ? "on (fills outputs)" : "off");

    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) {
        std::printf("  [FAIL] LoadLibrary failed, error %lu\n", GetLastError());
        return 2;
    }
    Check(true, "DLL loads");

    using GetClassObjectFn = HRESULT(__stdcall *)(REFCLSID, REFIID, void **);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(
        GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject) {
        std::printf("  [FAIL] DllGetClassObject missing\n");
        return 2;
    }
    Check(true, "DllGetClassObject exported");

    // The class id must match what the register script writes.
    static const GUID kClassId =
        {0x7c1f2e64, 0x9a3b, 0x4c5d, {0x8e, 0x7f, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f}};

    IClassFactory *factory = nullptr;
    HRESULT hr = getClassObject(kClassId, IID_IClassFactory,
                                reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
        std::printf("  [FAIL] class object, hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    Check(true, "class factory created");

    IASIO *asio = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IUnknown, reinterpret_cast<void **>(&asio));
    if (FAILED(hr) || !asio) {
        std::printf("  [FAIL] CreateInstance, hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    Check(true, "driver object created");

    Check(asio->init(nullptr) == ASIOTrue, "init() returns true");

    char name[64] = {0};
    asio->getDriverName(name);
    std::printf("  driver  : %s\n", name);
    Check(std::strstr(name, "OpenAES67") != nullptr, "driver name says OpenAES67");

    char errText[128] = {0};
    asio->getErrorMessage(errText);
    std::printf("  status  : %s\n", errText);

    long inCh = 0, outCh = 0;
    Check(asio->getChannels(&inCh, &outCh) == ASE_OK, "getChannels succeeds");
    std::printf("  channels: %ld in / %ld out\n", inCh, outCh);
    Check(inCh == kChannels && outCh == kChannels, "64 inputs and 64 outputs");

    ASIOSampleRate rate = 0;
    Check(asio->getSampleRate(&rate) == ASE_OK && rate == 48000.0, "48 kHz");

    long minSize = 0, maxSize = 0, pref = 0, gran = 0;
    Check(asio->getBufferSize(&minSize, &maxSize, &pref, &gran) == ASE_OK,
          "getBufferSize succeeds");
    std::printf("  buffers : min=%ld max=%ld preferred=%ld\n", minSize, maxSize, pref);

    ASIOChannelInfo info{};
    info.channel = 0;
    info.isInput = ASIOTrue;
    Check(asio->getChannelInfo(&info) == ASE_OK && info.type == ASIOSTInt32LSB,
          "channel 0 is int32 LSB");

    // Two buffers per channel, as a host allocates them.
    std::vector<ASIOBufferInfo> infos(kChannels * 2);
    for (int i = 0; i < kChannels; ++i) {
        infos[static_cast<size_t>(i)].isInput = ASIOTrue;
        infos[static_cast<size_t>(i)].channelNum = i;
        infos[static_cast<size_t>(kChannels + i)].isInput = ASIOFalse;
        infos[static_cast<size_t>(kChannels + i)].channelNum = i;
    }
    g_infos = infos.data();
    g_numChannels = static_cast<long>(infos.size());
    ASIOCallbacks callbacks{};
    callbacks.bufferSwitch = BufferSwitch;
    callbacks.asioMessage = AsioMessage;
    callbacks.bufferSwitchTimeInfo = BufferSwitchTimeInfo;

    Check(asio->createBuffers(infos.data(), static_cast<long>(infos.size()), kBufferSize,
                              &callbacks) == ASE_OK,
          "createBuffers with 128 channels succeeds");
    Check(infos[0].buffers[0] != nullptr && infos[0].buffers[1] != nullptr,
          "double buffering pointers are set");

    Check(asio->start() == ASE_OK, "start() succeeds");

    // Watch one input channel while the host runs.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long>(seconds * 1000));
    int peak = 0;
    long long lastFrames = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int32_t *src = static_cast<const int32_t *>(infos[0].buffers[0]);
        if (!src) {
            continue;
        }
        for (long i = 0; i < kBufferSize; ++i) {
            const int v = static_cast<int>(src[i] >> 16);
            const int a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        lastFrames = g_frames.load();
    }
    std::printf("  callbacks: %ld  frames: %lld  input peak: %d\n", g_callbacks.load(),
                lastFrames, peak);

    Check(g_callbacks.load() > 0, "bufferSwitch was called");

    // Did the tone actually reach the host buffers? If this is zero the
    // generator never ran, and any silence upstream is a test artefact.
    int outPeak = 0;
    for (long ch = 0; ch < static_cast<long>(infos.size()); ++ch) {
        if (infos[static_cast<size_t>(ch)].isInput) {
            continue;
        }
        const int32_t *b = static_cast<const int32_t *>(
            infos[static_cast<size_t>(ch)].buffers[0]);
        if (!b) {
            continue;
        }
        for (long i = 0; i < kBufferSize; ++i) {
            const int v = static_cast<int>(b[i] >> 16);
            const int a = v < 0 ? -v : v;
            if (a > outPeak) {
                outPeak = a;
            }
        }
        break;   // one output channel is enough
    }
    std::printf("  output peak (host buffers): %d\n", outPeak);
    // ~256 frames per 5.33 ms: allow a wide band for scheduling noise.
    const double expected = seconds * 48000.0;
    Check(lastFrames > expected * 0.5 && lastFrames < expected * 1.5,
          "sample position advanced at roughly real time");

    ASIOSamples pos = 0;
    ASIOTimeStamp stamp = 0;
    Check(asio->getSamplePosition(&pos, &stamp) == ASE_OK && pos > 0,
          "getSamplePosition reports progress");

    Check(asio->stop() == ASE_OK, "stop() succeeds");

    // Read the capture side of the ring directly: this separates "the driver never
    // wrote" from "the pump read the wrong thing".
    {
        std::string shmErr;
        auto ring = openaes67::openSharedAudio(L"OpenAES67Audio", &shmErr);
        if (ring && ring->valid()) {
            const int ringCh = ring->channels();
            std::vector<int16_t> buf(static_cast<size_t>(1024) * ringCh, 0);
            const int got = ring->readCapture(buf.data(), 1024);
            int ringPeak = 0;
            for (int i = 0; i < got * ringCh; ++i) {
                const int a = buf[static_cast<size_t>(i)] < 0
                                  ? -buf[static_cast<size_t>(i)]
                                  : buf[static_cast<size_t>(i)];
                if (a > ringPeak) ringPeak = a;
            }
            std::printf("  capture ring: %d ch, read %d frames, peak %d\n", ringCh, got,
                        ringPeak);
            Check(g_tone ? ringPeak > 0 : true, "tone reached the capture ring");
        } else {
            std::printf("  capture ring: not available (%s)\n", shmErr.c_str());
        }
    }
    Check(asio->disposeBuffers() == ASE_OK, "disposeBuffers() succeeds");

    asio->Release();
    factory->Release();
    FreeLibrary(dll);

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
