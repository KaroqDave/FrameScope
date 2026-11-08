#pragma once

#include "core/Attribution.h"
#include "core/FrameRecord.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace framescope::core {

struct RunSummary {
    SessionStats stats;
    std::size_t cpuSpikes = 0;
    std::size_t gpuSpikes = 0;
    std::size_t stallSpikes = 0;
    std::map<Cause, std::size_t> causes;
};

RunSummary summarizeRun(std::span<const FrameRecord> frames, std::span<const Spike> spikes);

// Compares two runs of the SAME sequence (benchmark, replay, same route from
// the same start), first run vs. second run.
//  - Spikes that vanish on run 2 -> shader compilation (cache warmed up).
//  - Spikes at the same moments in both runs -> game content (streaming).
//  - Same amount at random moments -> system, or nondeterministic game work.
struct RunComparison {
    RunSummary first;
    RunSummary second;
    double offsetSeconds = 0.0;   // best alignment: second + offset ~ first
    std::size_t matchedSpikes = 0;
    double repeatShare = 0.0;     // matched / min(spikes first, spikes second)
    double spikeRateChange = 0.0; // (second - first) / first, per minute
    std::string verdict;
    std::vector<std::string> notes;
};

struct ComparisonConfig {
    double maxOffsetSeconds = 10.0;  // search range for aligning the runs
    double offsetStepSeconds = 0.1;
    double toleranceSeconds = 0.5;   // two spikes this close count as "same moment"
};

RunComparison compareRuns(std::span<const FrameRecord> firstFrames, std::span<const Spike> firstSpikes,
                          std::span<const FrameRecord> secondFrames, std::span<const Spike> secondSpikes,
                          const ComparisonConfig& cfg = {});

} // namespace framescope::core
