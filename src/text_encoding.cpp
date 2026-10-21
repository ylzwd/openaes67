#include "openaes67/text_encoding.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace openaes67 {

namespace {

bool hasPrefix(const std::string &bytes, const unsigned char *prefix, size_t length) {
    if (bytes.size() < length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(bytes[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

std::string utf16ToUtf8(const std::string &bytes, bool littleEndian) {
    // Drop the BOM, then read 16-bit units honouring the byte order.
    const size_t start = 2;
    std::wstring wide;
    wide.reserve((bytes.size() - start) / 2);
    for (size_t i = start; i + 1 < bytes.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(bytes[i]);
        const auto hi = static_cast<unsigned char>(bytes[i + 1]);
        const wchar_t ch = littleEndian ? static_cast<wchar_t>(lo | (hi << 8))
                                        : static_cast<wchar_t>((lo << 8) | hi);
        wide.push_back(ch);
    }
    if (wide.empty()) {
        return std::string();
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                             static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0],
                          needed, nullptr, nullptr);
    return out;
}

} // namespace

std::string decodeTextToUtf8(const std::string &bytes) {
    static const unsigned char kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
    static const unsigned char kUtf16LeBom[2] = {0xFF, 0xFE};
    static const unsigned char kUtf16BeBom[2] = {0xFE, 0xFF};

    if (hasPrefix(bytes, kUtf16LeBom, 2)) {
        return utf16ToUtf8(bytes, true);
    }
    if (hasPrefix(bytes, kUtf16BeBom, 2)) {
        return utf16ToUtf8(bytes, false);
    }
    if (hasPrefix(bytes, kUtf8Bom, 3)) {
        return bytes.substr(3);
    }
    return bytes;
}

} // namespace openaes67
