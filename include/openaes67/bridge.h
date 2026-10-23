// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <functional>
#include <string>

#include "openaes67/config.h"

namespace openaes67 {

// Duplex bridge: AES67 downlink -> shared ring -> ASIO, and ASIO -> shared ring
// -> AES67 uplink. This is the "sound card mode" and it lives in one function so
// the CLI (`openaes67 bridge`) and the GUI run exactly the same code.
struct BridgeOptions {
    StreamConfig downCfg;        // downlink: group, port, channels, rate, codec
    StreamConfig upCfg;          // uplink: usually downCfg with group/port swapped
    std::wstring shmName;        // shared section the ASIO driver attaches to
    double       seconds = 0.0;  // 0 = run until shouldStop() returns false
    double       reportSeconds = 5.0;
    bool         usePtp = false;
    // Announce the uplink over SAP. Without this the return path is invisible in
    // Dante Controller: a Dante receiver can only subscribe to a flow it can see,
    // and Dante Controller lists third-party AES67 *transmit* flows only.
    bool         announceUp = false;
    int          sapInterval = 30;
    int          ptpDomain = 0;
    // Optional. Returning false stops the bridge.
    std::function<bool()> shouldStop;
    // Optional log sink; when null the messages go to stdout.
    std::function<void(const std::string &)> onLog;
};

struct BridgeStats {
    long long uplinkPackets = 0;
    long long uplinkSilent = 0;   // packets sent as silence (ASIO host idle)
    long long ringDropped = 0;    // downlink frames the ring refused
};

// Runs until `seconds` elapse or shouldStop() says stop. Returns false and fills
// `err` when the section or the uplink socket cannot be created.
bool runBridge(const BridgeOptions &options, BridgeStats *stats, std::string *err);

} // namespace openaes67
