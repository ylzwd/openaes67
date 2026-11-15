// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "openaes67/config.h"

namespace openaes67 {

// Experimental fixed profile: 48 kHz, 48 frames/packet, 64-channel int16 ring.
// asioFirst is zero based internally; the persisted route format is one based.
struct AudioRoute {
    bool receive = true;
    std::string group;
    uint16_t port = 0;
    int channels = 0;
    int asioFirst = 0;
    Codec codec = Codec::L24;
    uint8_t payloadType = 97;
};
bool parseAudioRoutes(const std::string &text, std::vector<AudioRoute> *routes,
                      std::string *err);
bool loadAudioRoutes(const std::string &path, std::vector<AudioRoute> *routes,
                     std::string *err);

// A bounded timestamp-addressed jitter buffer. All sources must use the SAME
// RTP media epoch; arrival order never moves samples to a different time slot.
// Ownership is confined to the pump thread. push/pop perform no allocation.
class RoutedPlayout {
public:
    explicit RoutedPlayout(const std::vector<AudioRoute> &routes);
    bool push(size_t route, uint32_t timestamp, const int16_t *samples);
    uint32_t pop(int16_t *interleaved64); // missing channel-frames, out of 48*64
    void reset();
    void clearRoute(size_t route);
    bool anchored() const { return anchored_; }
private:
    static constexpr uint32_t kWindow = 4096;
    struct Slot {
        uint32_t tag = 0;
        bool valid = false;
        std::array<int16_t, 8> samples{};
    };
    std::vector<AudioRoute> routes_;
    std::vector<std::vector<Slot>> slots_;
    bool anchored_ = false;
    uint32_t next_ = 0;
};

struct MultiBridgeOptions {
    std::vector<AudioRoute> routes;
    std::string ifaceIp = "0.0.0.0";
    std::wstring shmName = L"OpenAES67Audio";
    double seconds = 0; // zero runs until shouldStop
    std::function<bool()> shouldStop;
};
struct MultiBridgeStats {
    uint64_t rxPackets = 0, rejectedPackets = 0, timestampRejects = 0;
    uint64_t txPackets = 0, txErrors = 0, missingChannelFrames = 0;
    uint64_t renderDroppedFrames = 0, captureMissingFrames = 0;
    uint64_t restarts = 0, scheduleResets = 0;
};
bool runMultiBridge(const MultiBridgeOptions &options, MultiBridgeStats *stats,
                    std::string *err);

} // namespace openaes67
