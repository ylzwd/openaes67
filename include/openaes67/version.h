// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

// Single source of truth for the version string.
//
// It lives in a header rather than on the compiler command line because the
// command line is set in two places (build.bat and CMakeLists.txt) and one of
// them silently won: the binary printed "0.0.0-dev" while the tag said 0.1.0.
// verify-claims.ps1 now also checks that the CMake project version matches this
// string, so the two cannot drift apart again.
#pragma once

#ifndef OPENAES67_VERSION
#define OPENAES67_VERSION "0.1.1"
#endif
