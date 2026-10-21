#include "openaes67/wav.h"
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace openaes67 {

static uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool readWavPcm(const std::string &path, WavData *out, std::string *err) {
    if (!out) {
        return false;
    }
    FILE *f = openFileUtf8(path.c_str(), "rb");
    if (!f) {
        if (err) { *err = "cannot open " + path; }
        return false;
    }

    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        if (err) { *err = "not a RIFF/WAVE file"; }
        return false;
    }

    bool haveFmt = false, haveData = false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    std::vector<uint8_t> dataBytes;

    for (;;) {
        uint8_t ch[8];
        if (std::fread(ch, 1, 8, f) != 8) {
            break;
        }
        const uint32_t size = rd32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            if (size < 16) { break; }
            std::vector<uint8_t> fmt(size);
            if (std::fread(fmt.data(), 1, size, f) != size) { break; }
            format = rd16(&fmt[0]);
            channels = rd16(&fmt[2]);
            rate = rd32(&fmt[4]);
            bits = rd16(&fmt[14]);
            haveFmt = true;
            if (size % 2) { std::fseek(f, 1, SEEK_CUR); }
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataBytes.resize(size);
            const size_t got = std::fread(dataBytes.data(), 1, size, f);
            dataBytes.resize(got);
            haveData = true;
            if (size % 2) { std::fseek(f, 1, SEEK_CUR); }
        } else {
            std::fseek(f, static_cast<long>(size) + (size % 2), SEEK_CUR);
        }
        if (haveFmt && haveData) {
            break;
        }
    }
    std::fclose(f);

    if (!haveFmt || !haveData) {
        if (err) { *err = "missing fmt or data chunk"; }
        return false;
    }
    if (format != 1) {
        if (err) { *err = "unsupported WAVE format (need PCM), got " + std::to_string(format); }
        return false;
    }
    if (bits != 16 && bits != 24) {
        if (err) {
            *err = "unsupported bit depth, need 16 or 24, got " + std::to_string(bits);
        }
        return false;
    }
    // Bounded, not merely non-zero. A file is untrusted input: a 44-byte header can
    // claim 2000 channels at 1 Hz, and accepting it produced a WavData that nothing
    // downstream could carry. These are the same limits the stream enforces
    // (StreamConfig::validate), so "the reader accepted it" means "this build can
    // send it".
    if (channels < 1 || channels > 64) {
        if (err) {
            *err = "channel count " + std::to_string(channels) + " (expected 1..64)";
        }
        return false;
    }
    if (rate < 8000 || rate > 384000) {
        if (err) {
            *err = "sample rate " + std::to_string(rate) + " (expected 8000..384000)";
        }
        return false;
    }

    out->channels = static_cast<int>(channels);
    out->sampleRate = static_cast<int>(rate);
    out->sourceBits = static_cast<int>(bits);
    const size_t bytesPerSample = bits / 8;
    const size_t n = dataBytes.size() / bytesPerSample;
    out->samples.resize(n);
    if (bits == 16) {
        for (size_t i = 0; i < n; ++i) {
            out->samples[i] = static_cast<int16_t>(rd16(&dataBytes[i * 2]));
        }
    } else {
        // WAV is little-endian: LSB, middle, MSB. Keep the top 16 bits.
        for (size_t i = 0; i < n; ++i) {
            const uint8_t *p = &dataBytes[i * 3];
            out->samples[i] = static_cast<int16_t>((static_cast<uint16_t>(p[2]) << 8) | p[1]);
        }
    }
    return true;
}

bool readWavPcm16(const std::string &path, WavData *out, std::string *err) {
    if (!readWavPcm(path, out, err)) {
        return false;
    }
    if (out->sourceBits != 16) {
        if (err) {
            *err = "expected a 16-bit WAV, got " + std::to_string(out->sourceBits);
        }
        return false;
    }
    return true;
}

bool writeWavPcm16(const std::string &path, const int16_t *data, size_t sampleCount,
                   int channels, int sampleRate) {
    FILE *f = openFileUtf8(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * 2);
    const uint16_t bits = 16;
    const uint16_t ch = static_cast<uint16_t>(channels);
    const uint32_t rate = static_cast<uint32_t>(sampleRate);
    const uint32_t byteRate = rate * ch * (bits / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(ch * (bits / 8));

    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(ch);
    w32(rate);
    w32(byteRate);
    w16(blockAlign);
    w16(bits);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    if (dataBytes && data) {
        std::fwrite(data, 1, dataBytes, f);
    }
    std::fclose(f);
    return true;
}

bool writeWavPcm24(const std::string &path, const int16_t *data, size_t sampleCount,
                   int channels, int sampleRate) {
    FILE *f = openFileUtf8(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * 3);
    const uint16_t bits = 24;
    const uint16_t ch = static_cast<uint16_t>(channels);
    const uint32_t rate = static_cast<uint32_t>(sampleRate);
    const uint32_t byteRate = rate * ch * (bits / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(ch * (bits / 8));

    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(ch);
    w32(rate);
    w32(byteRate);
    w16(blockAlign);
    w16(bits);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    if (dataBytes && data) {
        // Little-endian 24-bit: low = 0, then low byte of the sample, then high.
        std::vector<uint8_t> buf(sampleCount * 3);
        for (size_t i = 0; i < sampleCount; ++i) {
            const uint16_t v = static_cast<uint16_t>(data[i]);
            buf[i * 3 + 0] = 0;
            buf[i * 3 + 1] = static_cast<uint8_t>(v & 0xFF);
            buf[i * 3 + 2] = static_cast<uint8_t>(v >> 8);
        }
        std::fwrite(buf.data(), 1, buf.size(), f);
    }
    std::fclose(f);
    return true;
}

} // namespace openaes67

namespace openaes67 {

std::FILE *openFileUtf8(const char *path, const char *mode) {
    if (!path || !mode) {
        return nullptr;
    }
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        return std::fopen(path, mode);
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
    const int mlen = ::MultiByteToWideChar(CP_UTF8, 0, mode, -1, nullptr, 0);
    std::wstring wmode(static_cast<size_t>(mlen > 0 ? mlen : 1), L'\0');
    if (mlen > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, mode, -1, &wmode[0], mlen);
    }
    return ::_wfopen(wpath.c_str(), wmode.c_str());
}

} // namespace openaes67
