#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace openaes67 {

// fopen() takes an ANSI path on Windows, so a UTF-8 path with non-ASCII characters
// silently fails to open. This converts to UTF-16 and uses _wfopen instead.
std::FILE *openFileUtf8(const char *path, const char *mode);

struct WavData {
    int channels = 0;
    int sampleRate = 0;
    int sourceBits = 0;             // bit depth found in the file (16 or 24)
    std::vector<int16_t> samples;   // interleaved, always int16 internally

    int frames() const {
        return channels > 0 ? static_cast<int>(samples.size() / channels) : 0;
    }
};

// Reads a WAV file with PCM 16-bit or 24-bit samples. 24-bit input keeps the top
// 16 bits (the internal audio path is int16); `sourceBits` records what the file
// actually contained so the caller can report the conversion instead of hiding it.
// Anything else (float, ADPCM, 8-bit) fails with a readable error.
bool readWavPcm(const std::string &path, WavData *out, std::string *err);

// Kept for callers that only deal with 16-bit files.
bool readWavPcm16(const std::string &path, WavData *out, std::string *err);

// Writes a PCM16 WAV file.
bool writeWavPcm16(const std::string &path, const int16_t *data, size_t sampleCount,
                   int channels, int sampleRate);

// Writes a PCM24 WAV file: each int16 sample goes into the upper two bytes and the
// low byte is zero. Useful when a downstream tool insists on 24-bit input; it does
// not add resolution (the source is int16), which the caller should say out loud.
bool writeWavPcm24(const std::string &path, const int16_t *data, size_t sampleCount,
                   int channels, int sampleRate);

} // namespace openaes67
