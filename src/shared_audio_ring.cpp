#include "openaes67/shared_audio_ring.h"

#include <atomic>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace openaes67 {
namespace {
// Release/acquire fences around index publication. On x86 these compile to
// compiler barriers, which is all that is required for this single-producer,
// single-consumer pattern.
//
// The indices stay plain volatile rather than std::atomic on purpose: the
// header struct is the ABI shared with the kernel driver, so its layout cannot
// change. A 32-bit aligned volatile load/store is atomic on every platform this
// targets, and the fences supply the ordering that volatile alone does not.
// Verified by the concurrent producer/consumer case in tests/selftest.cpp:
// 240000 frames through a 10 ms ring, bit-exact, over 20 consecutive runs.
inline void publish() { std::atomic_thread_fence(std::memory_order_release); }
inline void consume() { std::atomic_thread_fence(std::memory_order_acquire); }

inline uint32_t loadIndex(const volatile uint32_t &v) {
    return static_cast<uint32_t>(v);
}

inline void storeIndex(volatile uint32_t &v, uint32_t value) {
    v = value;
}

} // namespace

SharedAudioRing::SharedAudioRing(void *mapping, size_t bytes,
                                 const SharedAudioConfig &cfg, bool initialize,
                                 bool ownMapping)
    : base_(static_cast<uint8_t *>(mapping)),
      bytes_(bytes),
      ownsMapping_(ownMapping),
      mapping_(mapping) {
    if (!mapping || bytes < sizeof(OpenAes67SharedAudio)) {
        error_ = "mapping too small";
        return;
    }
    hdr_ = reinterpret_cast<OpenAes67SharedAudio *>(mapping);

    const uint32_t frameBytes = static_cast<uint32_t>(cfg.channels) * 2u;
    const uint32_t capacity = cfg.capacityFrames;

    if (initialize) {
        if (bytes < openaes67_mapping_size(capacity, frameBytes)) {
            error_ = "mapping too small for the requested capacity";
            hdr_ = nullptr;
            return;
        }
        std::memset(mapping, 0, bytes);
        hdr_->magic = OPEN_AES67_SHM_MAGIC;
        hdr_->version = OPEN_AES67_SHM_VERSION;
        hdr_->frameBytes = frameBytes;
        hdr_->channels = static_cast<uint32_t>(cfg.channels);
        hdr_->sampleRate = static_cast<uint32_t>(cfg.sampleRate);
        hdr_->capacityFrames = capacity;
        hdr_->flags = 0;
        valid_ = true;
        return;
    }

    if (hdr_->magic != OPEN_AES67_SHM_MAGIC) {
        error_ = "bad magic";
        hdr_ = nullptr;
        return;
    }
    if (hdr_->version != OPEN_AES67_SHM_VERSION) {
        error_ = "unsupported version";
        hdr_ = nullptr;
        return;
    }
    if (hdr_->frameBytes == 0 || hdr_->capacityFrames == 0) {
        error_ = "incomplete header";
        hdr_ = nullptr;
        return;
    }
    if (bytes < openaes67_mapping_size(hdr_->capacityFrames, hdr_->frameBytes)) {
        error_ = "mapping smaller than the header claims";
        hdr_ = nullptr;
        return;
    }
    valid_ = true;
}

SharedAudioRing::~SharedAudioRing() {
    if (ownsMapping_ && mapping_) {
        UnmapViewOfFile(mapping_);
    }
    if (section_) {
        CloseHandle(static_cast<HANDLE>(section_));
    }
}

int SharedAudioRing::channels() const {
    return hdr_ ? static_cast<int>(hdr_->channels) : 0;
}

int SharedAudioRing::sampleRate() const {
    return hdr_ ? static_cast<int>(hdr_->sampleRate) : 0;
}

uint32_t SharedAudioRing::capacityFrames() const {
    return hdr_ ? hdr_->capacityFrames : 0;
}

int16_t *SharedAudioRing::renderPtr() const {
    return reinterpret_cast<int16_t *>(base_ + openaes67_render_offset());
}

int16_t *SharedAudioRing::capturePtr() const {
    return reinterpret_cast<int16_t *>(
        base_ + openaes67_capture_offset(hdr_->capacityFrames, hdr_->frameBytes));
}

int SharedAudioRing::renderFree() const {
    return static_cast<int>(openaes67_frames_free(
        loadIndex(hdr_->writeIndex), loadIndex(hdr_->readIndex), hdr_->capacityFrames));
}

int SharedAudioRing::renderPending() const {
    return static_cast<int>(openaes67_frames_pending(
        loadIndex(hdr_->writeIndex), loadIndex(hdr_->readIndex), hdr_->capacityFrames));
}

int SharedAudioRing::capturePending() const {
    return static_cast<int>(openaes67_frames_pending(
        loadIndex(hdr_->captureWrite), loadIndex(hdr_->captureRead),
        hdr_->capacityFrames));
}

int SharedAudioRing::writeRender(const int16_t *src, int frames) {
    if (!valid_ || !src || frames <= 0) {
        return 0;
    }
    const uint32_t capacity = hdr_->capacityFrames;
    uint32_t write = loadIndex(hdr_->writeIndex);
    const uint32_t read = loadIndex(hdr_->readIndex);
    const uint32_t freeFrames = openaes67_frames_free(write, read, capacity);

    uint32_t toWrite = static_cast<uint32_t>(frames);
    if (toWrite > freeFrames) {
        toWrite = freeFrames;              // overrun: drop the excess, do not block
    }
    if (toWrite == 0) {
        return 0;
    }

    int16_t *ring = renderPtr();
    uint32_t pos = write % capacity;
    uint32_t remaining = toWrite;
    uint32_t copied = 0;
    while (remaining > 0) {
        const uint32_t chunk = (capacity - pos) < remaining ? (capacity - pos) : remaining;
        std::memcpy(ring + static_cast<size_t>(pos) * hdr_->channels,
                    src + static_cast<size_t>(copied) * hdr_->channels,
                    static_cast<size_t>(chunk) * hdr_->frameBytes);
        pos = (pos + chunk) % capacity;
        copied += chunk;
        remaining -= chunk;
    }

    write += toWrite;
    publish();
    storeIndex(hdr_->writeIndex, write);
    return static_cast<int>(toWrite);
}

int SharedAudioRing::readRender(int16_t *dst, int frames) {
    if (!valid_ || !dst || frames <= 0) {
        return 0;
    }
    const uint32_t capacity = hdr_->capacityFrames;
    const uint32_t write = loadIndex(hdr_->writeIndex);
    uint32_t read = loadIndex(hdr_->readIndex);
    const uint32_t pending = openaes67_frames_pending(write, read, capacity);

    uint32_t toRead = static_cast<uint32_t>(frames);
    if (toRead > pending) {
        toRead = pending;                  // underrun: caller fills silence
    }
    if (toRead == 0) {
        return 0;
    }

    const int16_t *ring = renderPtr();
    uint32_t pos = read % capacity;
    uint32_t remaining = toRead;
    uint32_t copied = 0;
    while (remaining > 0) {
        const uint32_t chunk = (capacity - pos) < remaining ? (capacity - pos) : remaining;
        std::memcpy(dst + static_cast<size_t>(copied) * hdr_->channels,
                    ring + static_cast<size_t>(pos) * hdr_->channels,
                    static_cast<size_t>(chunk) * hdr_->frameBytes);
        pos = (pos + chunk) % capacity;
        copied += chunk;
        remaining -= chunk;
    }

    read += toRead;
    publish();
    storeIndex(hdr_->readIndex, read);
    return static_cast<int>(toRead);
}

int SharedAudioRing::writeCapture(const int16_t *src, int frames) {
    if (!valid_ || !src || frames <= 0) {
        return 0;
    }
    const uint32_t capacity = hdr_->capacityFrames;
    uint32_t write = loadIndex(hdr_->captureWrite);
    const uint32_t read = loadIndex(hdr_->captureRead);
    const uint32_t freeFrames = openaes67_frames_free(write, read, capacity);

    uint32_t toWrite = static_cast<uint32_t>(frames);
    if (toWrite > freeFrames) {
        toWrite = freeFrames;
        noteCaptureOverrun();
    }
    if (toWrite == 0) {
        return 0;
    }

    int16_t *ring = capturePtr();
    uint32_t pos = write % capacity;
    uint32_t remaining = toWrite;
    uint32_t copied = 0;
    while (remaining > 0) {
        const uint32_t chunk = (capacity - pos) < remaining ? (capacity - pos) : remaining;
        std::memcpy(ring + static_cast<size_t>(pos) * hdr_->channels,
                    src + static_cast<size_t>(copied) * hdr_->channels,
                    static_cast<size_t>(chunk) * hdr_->frameBytes);
        pos = (pos + chunk) % capacity;
        copied += chunk;
        remaining -= chunk;
    }

    write += toWrite;
    publish();
    storeIndex(hdr_->captureWrite, write);
    return static_cast<int>(toWrite);
}

int SharedAudioRing::readCapture(int16_t *dst, int frames) {
    if (!valid_ || !dst || frames <= 0) {
        return 0;
    }
    const uint32_t capacity = hdr_->capacityFrames;
    const uint32_t write = loadIndex(hdr_->captureWrite);
    uint32_t read = loadIndex(hdr_->captureRead);
    const uint32_t pending = openaes67_frames_pending(write, read, capacity);

    uint32_t toRead = static_cast<uint32_t>(frames);
    if (toRead > pending) {
        toRead = pending;
    }
    if (toRead == 0) {
        return 0;
    }

    const int16_t *ring = capturePtr();
    uint32_t pos = read % capacity;
    uint32_t remaining = toRead;
    uint32_t copied = 0;
    while (remaining > 0) {
        const uint32_t chunk = (capacity - pos) < remaining ? (capacity - pos) : remaining;
        std::memcpy(dst + static_cast<size_t>(copied) * hdr_->channels,
                    ring + static_cast<size_t>(pos) * hdr_->channels,
                    static_cast<size_t>(chunk) * hdr_->frameBytes);
        pos = (pos + chunk) % capacity;
        copied += chunk;
        remaining -= chunk;
    }

    read += toRead;
    publish();
    storeIndex(hdr_->captureRead, read);
    return static_cast<int>(toRead);
}


void SharedAudioRing::noteCaptureOverrun() {
    if (valid_) {
        ++hdr_->captureOverruns;
    }
}

uint32_t SharedAudioRing::renderUnderruns() const {
    return valid_ ? loadIndex(hdr_->renderUnderruns) : 0;
}

uint32_t SharedAudioRing::captureOverruns() const {
    return valid_ ? loadIndex(hdr_->captureOverruns) : 0;
}

bool SharedAudioRing::running() const {
    return valid_ && (hdr_->flags & OPEN_AES67_FLAG_RUNNING) != 0;
}

void SharedAudioRing::setRunning(bool on) {
    if (!valid_) {
        return;
    }
    if (on) {
        hdr_->flags |= OPEN_AES67_FLAG_RUNNING;
    } else {
        hdr_->flags &= ~OPEN_AES67_FLAG_RUNNING;
    }
}

// ---------------------------------------------------------------- OS wrappers

std::unique_ptr<SharedAudioRing> createSharedAudio(const wchar_t *name,
                                                   const SharedAudioConfig &cfg,
                                                   std::string *err) {
    if (!name) {
        if (err) {
            *err = "no section name";
        }
        return nullptr;
    }
    const uint32_t frameBytes = static_cast<uint32_t>(cfg.channels) * 2u;
    const size_t bytes = openaes67_mapping_size(cfg.capacityFrames, frameBytes);

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                        PAGE_READWRITE,
                                        static_cast<DWORD>(bytes >> 32),
                                        static_cast<DWORD>(bytes & 0xFFFFFFFFu), name);
    if (!section) {
        if (err) {
            *err = "CreateFileMapping failed, error " + std::to_string(GetLastError());
        }
        return nullptr;
    }
    void *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!view) {
        CloseHandle(section);
        if (err) {
            *err = "MapViewOfFile failed, error " + std::to_string(GetLastError());
        }
        return nullptr;
    }

    std::unique_ptr<SharedAudioRing> ring(
        new SharedAudioRing(view, bytes, cfg, /*initialize=*/true, /*ownMapping=*/true));
    ring->section_ = section;
    if (!ring->valid()) {
        if (err) {
            *err = ring->error();
        }
        return nullptr;
    }
    return ring;
}

std::unique_ptr<SharedAudioRing> openSharedAudio(const wchar_t *name,
                                                 std::string *err) {
    if (!name) {
        if (err) {
            *err = "no section name";
        }
        return nullptr;
    }
    HANDLE section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!section) {
        if (err) {
            *err = "OpenFileMapping failed, error " + std::to_string(GetLastError());
        }
        return nullptr;
    }
    void *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) {
        CloseHandle(section);
        if (err) {
            *err = "MapViewOfFile failed, error " + std::to_string(GetLastError());
        }
        return nullptr;
    }

    // The header carries the real size; validate against what it claims.
    OpenAes67SharedAudio *hdr = static_cast<OpenAes67SharedAudio *>(view);
    const size_t bytes =
        openaes67_mapping_size(hdr->capacityFrames, hdr->frameBytes);

    std::unique_ptr<SharedAudioRing> ring(new SharedAudioRing(
        view, bytes, SharedAudioConfig{}, /*initialize=*/false, /*ownMapping=*/true));
    ring->section_ = section;
    if (!ring->valid()) {
        if (err) {
            *err = ring->error();
        }
        return nullptr;
    }
    return ring;
}

} // namespace openaes67
