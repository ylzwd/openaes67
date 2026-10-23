// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/config.h"

#include <cctype>
#include <string>

namespace openaes67 {

const char *codecName(Codec codec) {
    return codec == Codec::L24 ? "L24" : "L16";
}

bool parseCodecName(const std::string &name, Codec *codec) {
    std::string upper;
    upper.reserve(name.size());
    for (const char c : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "L16") {
        if (codec) {
            *codec = Codec::L16;
        }
        return true;
    }
    if (upper == "L24") {
        if (codec) {
            *codec = Codec::L24;
        }
        return true;
    }
    return false;
}

std::string StreamConfig::validate(std::string *detail) const {
    auto fail = [&](const std::string &why) {
        if (detail) {
            *detail = why;
        }
        return why;
    };
    if (channels <= 0 || channels > 64) {
        return fail("channel count " + std::to_string(channels) + " (expected 1..64)");
    }
    if (sampleRate < 8000 || sampleRate > 384000) {
        return fail("sample rate " + std::to_string(sampleRate) + " (expected 8000..384000)");
    }
    if (ptimeMs <= 0.0 || ptimeMs > 100.0) {
        return fail("ptime (expected 0.01..100 ms)");
    }
    if (port == 0) {
        return fail("port 0 (UDP ports start at 1)");
    }
    if (detail) {
        detail->clear();
    }
    return std::string();
}

} // namespace openaes67
