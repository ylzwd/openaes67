#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "openaes67/config.h"
#include "openaes67/audio_io.h"

namespace openaes67 {

// One periodic snapshot of the receive path.
struct RxSnapshot {
    double   seconds = 0.0;
    int      buffered = 0;        // average depth over the reporting window
    int      target = 0;          // control target depth
    double   correctionPpm = 0.0; // rate controller correction
    double   effectivePpm = 0.0;  // measured from consumption/production
    double   ptpPpm = 0.0;        // feed-forward estimate (0 when unused)
    uint32_t packets = 0;
    uint32_t lost = 0;
    uint32_t late = 0;
    uint32_t wrongSsrc = 0;
    uint32_t wrongPayloadType = 0;  // packets from another source on the group
    uint32_t ssrcChanges = 0; // sender restarts adopted
    int      underruns = 0;
    int64_t  framesWritten = 0;
    double   outRateHz = 0.0;     // framesWritten / seconds, to expose clock error
};

struct ReceiveLoopOptions {
    StreamConfig cfg;
    bool   exclusive = false;
    bool   usePtp = false;
    // PTP v2 domain number (0-127). AES67 mandates 0, but networks that also
    // carry ST 2110 or vendor gear often run another domain, and a slave on the
    // wrong domain never sees a grandmaster. Configurable so a mismatch can be
    // fixed rather than guessed at.
    int    ptpDomain = 0;
    // When non-empty, audio is written into this named shared-memory section
    // instead of a WASAPI device. That is the path the kernel driver consumes
    // (docs/M6-DESIGN.md); no audio device is opened at all in this mode.
    std::wstring shmName;
    // Audio output selection. Empty deviceId means the system default endpoint.
    std::wstring deviceId;
    std::string  backend = "wasapi";   // "wasapi" | "asio"
    std::string  asioDriver;           // registry key, required when backend=="asio"
    std::string  deviceLabel;          // what to print in the log ("" = system default)
    double seconds = 0.0;         // 0 = run until shouldStop() returns false
    double reportIntervalSeconds = 2.0;

    // Called periodically with a snapshot. May be null.
    std::function<void(const RxSnapshot &)> onStats;

    // Called when the stream stops delivering packets and when it resumes.
    // A receiver that keeps feeding silence looks healthy in the stats; only this
    // distinguishes "playing" from "the sender vanished".
    std::function<void(bool stalled)> onStall;

    // How long without a single packet before the stream is reported as stalled.
    double stallSeconds = 1.0;
    // Human readable log lines. May be null.
    std::function<void(const std::string &)> onLog;
    // Polled every loop iteration; returning false ends the loop. May be null.
    std::function<bool()> shouldStop;
};

// Runs the complete receive path: opens the render device, starts the RTP
// receiver, prebuffers to the control target, then feeds the device until the
// time window elapses or shouldStop() returns false.
//
// This exists so the CLI and the GUI cannot diverge: both call this function.
// Returns true when the loop ran to completion.
bool runReceiveLoop(const ReceiveLoopOptions &opt, RxSnapshot *finalSnapshot,
                    std::string *err);

} // namespace openaes67
