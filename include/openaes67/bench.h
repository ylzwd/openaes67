#pragma once
#include <cstddef>
#include <vector>
#include <string>

namespace openaes67 {

// A repeatable measurement of the paths that decide whether a 64x64 sound card
// keeps up. Numbers, not opinions: every optimisation in this project is judged
// against this output, and `--check` turns it into a regression gate.
//
// The benchmarks are deliberately micro: they run the same functions the audio
// threads run, in a loop, with buffers already allocated. What they measure is
// the cost of the operation itself, so a change in the hot path moves the number.
struct BenchCase {
    std::string name;
    double      value = 0.0;
    std::string unit;        // "Mframes/s", "Mops/s", "us"
    bool        higher_is_better = true;
};

// Runs every case and returns the results in a stable order.
std::vector<BenchCase> runBenchmarks();

// Baseline file: one "name value" pair per line. Missing names are not compared
// (a new case must not fail the gate on its first run), and a case that got more
// than `tolerancePercent` worse fails.
bool writeBenchBaseline(const std::string &path, const std::vector<BenchCase> &cases,
                        std::string *error);
// Returns false when a case regressed; `report` receives one line per case.
bool checkBenchBaseline(const std::string &path, const std::vector<BenchCase> &cases,
                        double tolerancePercent, std::string *report);

} // namespace openaes67
