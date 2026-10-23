// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <string>

namespace openaes67 {

// Normalises a text file's bytes to UTF-8.
//
// Why this exists: on Windows, `openaes67 sdp ... > stream.sdp` from PowerShell
// 5.1 writes UTF-16LE, and Notepad may add either BOM. A parser that assumes
// UTF-8 then sees NUL bytes between every character and reports nonsense -- the
// first symptom here was "group is not a multicast address" for a perfectly good
// SDP. Accept UTF-8 (with or without BOM), UTF-16LE and UTF-16BE, and return the
// content as UTF-8 with no BOM.
//
// Bytes that are not valid in the detected encoding are replaced, not rejected:
// the caller's parser gives a better error than "invalid byte here".
std::string decodeTextToUtf8(const std::string &bytes);

} // namespace openaes67
