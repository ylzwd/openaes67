// Config-file support for the CLI.
//
// A sound-card install is started by a script or a service, and repeating fifteen
// command-line flags there is fragile. `--config FILE` reads "key = value" lines;
// command-line arguments are parsed afterwards, so a flag always wins.
//
// Unknown keys are an error, not a no-op: a typo that silently does nothing is
// worse than a startup failure.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "openaes67/config.h"

namespace openaes67 {

// Every option that can come from a config file. Grouped in one struct so the
// parser signature does not grow a new parameter per key.
struct CliOptions {
    StreamConfig cfg;
    double       seconds = 6.0;
    int          ptpDomain = 0;
    bool         usePtp = false;
    bool         ptpLock = false;
    bool         useSap = false;
    int          sapInterval = 30;
    std::wstring shmName;
    std::string  sendGroup;
    uint16_t     sendPort = 0;
    bool         discover = false;
    std::string  discoverName;
    double       discoverSeconds = 4.0;
    std::string  refclk;
    std::string  deviceName;
    std::string  inDeviceName;
    std::string  backend;
    std::string  asioDriver;
    std::string  outPath = "rx.wav";
    int          outBits = 16;
    std::string  sdpOutPath;
    std::string  sdpInPath;
};

// Returns false when the file cannot be read or contains an unknown key.
// `openFileUtf8` is the caller's UTF-8 aware fopen (Windows paths are UTF-8 in
// this program), so it is passed in rather than duplicated here.
// `report` false suppresses the diagnostic text. The self-test needs that: it
// deliberately feeds an unknown key, and the claims gate treats any stderr text
// from selftest.exe as a failure.
bool applyConfigFile(const char *path, CliOptions *options,
                     std::FILE *(*openFileUtf8)(const char *, const char *),
                     bool report = true);

} // namespace openaes67
