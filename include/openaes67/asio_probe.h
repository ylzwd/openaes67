// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#pragma once
#include <string>

namespace openaes67 {

// Whether this machine has the OpenAES67 ASIO driver registered, which is what a
// DAW needs before it can list the device. Registration lives in
// HKLM\SOFTWARE\ASIO\<name> (plus the WOW6432Node copy for 32-bit hosts), and the
// in-process server path is in the class's InprocServer32.
struct AsioRegistration {
    bool        registered = false;   // a key exists in either registry view
    bool        dllPresent = false;   // the DLL the CLSID points at still exists
    std::string name;                 // the ASIO device name, e.g. "OpenAES67 ASIO 64x64"
    std::string clsid;
    std::string dllPath;
    // "registered", "key present but DLL missing", "not registered"
    std::string summary;
};

AsioRegistration probeAsioRegistration();

} // namespace openaes67
