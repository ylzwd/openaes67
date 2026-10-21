#pragma once
#include <string>
#include <vector>

namespace openaes67 {

// One line of environment diagnosis. The levels map to what the CLI prints and
// to the colour a GUI would use.
struct DoctorLine {
    enum Level { Ok, Warn, Fail, Info };
    Level       level = Info;
    std::string text;
};

struct DoctorOptions {
    std::string ifaceIp;
    std::string group;
    uint16_t    port = 0;
    int         ptpDomain = 0;
    // The SAP and PTP checks block for these long; a GUI may want them shorter.
    int sapSeconds = 3;
    int ptpSeconds = 3;
    bool checkSap = true;
    bool checkPtp = true;
};

struct DoctorResult {
    std::vector<DoctorLine> lines;
    int failures = 0;   // number of Fail lines
    bool ready() const { return failures == 0; }
};

// Runs the checks the two common failures need: "no audio" (interface, multicast
// group, SAP receive, PTP lock) and "the DAW does not list the device" (ASIO
// registration). Shared by the CLI `doctor` command and the GUI button.
DoctorResult runDoctor(const DoctorOptions &options);

} // namespace openaes67
