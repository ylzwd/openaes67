// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

#include "openaes67/asio_probe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace openaes67 {

namespace {

std::string wideToUtf8(const std::wstring &w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n,
                          nullptr, nullptr);
    return out;
}

// Reads one string value from an open key. Returns false when absent.
bool readString(HKEY key, const wchar_t *name, std::wstring *out) {
    wchar_t buf[1024] = {0};
    DWORD type = 0;
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    const LONG rc = ::RegQueryValueExW(key, name, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buf), &bytes);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    *out = buf;
    return true;
}

// Enumerates HKLM\SOFTWARE\ASIO (and the 32-bit view) looking for a key whose
// name starts with the OpenAES67 prefix -- the driver may be registered under a
// slightly different suffix, and the DLL it points at matters more than the name.
bool findOwnAsioKey(const wchar_t *subkeyPath, std::wstring *name, std::wstring *clsid) {
    HKEY asio = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkeyPath, 0, KEY_READ, &asio) !=
        ERROR_SUCCESS) {
        return false;
    }
    bool found = false;
    for (DWORD index = 0;; ++index) {
        wchar_t subName[256] = {0};
        DWORD length = 256;
        const LONG rc = ::RegEnumKeyExW(asio, index, subName, &length, nullptr, nullptr,
                                        nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            break;
        }
        if (std::wstring(subName).find(L"OpenAES67") == std::wstring::npos) {
            continue;
        }
        HKEY device = nullptr;
        if (::RegOpenKeyExW(asio, subName, 0, KEY_READ, &device) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring value;
        if (readString(device, L"CLSID", &value)) {
            *name = subName;
            *clsid = value;
            found = true;
        }
        ::RegCloseKey(device);
        if (found) {
            break;
        }
    }
    ::RegCloseKey(asio);
    return found;
}

std::wstring dllForClsid(const std::wstring &clsid) {
    // The registration script writes the class under HKLM\SOFTWARE\Classes\CLSID,
    // which is the 64-bit view; a 32-bit host would read the WOW6432Node copy.
    const std::wstring path = L"SOFTWARE\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return std::wstring();
    }
    std::wstring dll;
    const bool ok = readString(key, nullptr, &dll);
    ::RegCloseKey(key);
    return ok ? dll : std::wstring();
}

} // namespace

AsioRegistration probeAsioRegistration() {
    AsioRegistration result;

    // Both registry views: a 64-bit host reads SOFTWARE\ASIO, a 32-bit host the
    // redirected copy. The registration script writes both, but a hand-made
    // registration may only have one, and the message must still be accurate.
    std::wstring name;
    std::wstring clsid;
    const bool found = findOwnAsioKey(L"SOFTWARE\\ASIO", &name, &clsid) ||
                       findOwnAsioKey(L"SOFTWARE\\WOW6432Node\\ASIO", &name, &clsid);

    if (!found) {
        result.summary = "not registered";
        return result;
    }

    result.registered = true;
    result.name = wideToUtf8(name);
    result.clsid = wideToUtf8(clsid);

    const std::wstring dll = dllForClsid(clsid);
    if (!dll.empty()) {
        result.dllPath = wideToUtf8(dll);
        const DWORD attributes = ::GetFileAttributesW(dll.c_str());
        result.dllPresent = attributes != INVALID_FILE_ATTRIBUTES &&
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    if (result.dllPresent) {
        result.summary = "registered";
    } else if (!result.dllPath.empty()) {
        result.summary = "registered but the DLL is missing";
    } else {
        result.summary = "registered without an InprocServer32 path";
    }
    return result;
}

} // namespace openaes67
