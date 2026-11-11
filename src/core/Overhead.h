#pragma once

#include "core/Correlator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace framescope::core {

// What measuring cost: a stutter tool must not cause stutter itself.
struct Overhead {
    double captureSeconds = 0.0;
    double frameScopeCpuSeconds = 0.0;  // FrameScope's own process
    double presentMonCpuSeconds = 0.0;  // the PresentMon it started
    std::uint64_t eventsLost = 0;       // trace events Windows had to drop
    std::uint64_t eventsTotal = 0;
    bool lightTrace = false;            // already running without sampling / file tracking
    bool offVCacheCores = false;        // FrameScope was kept off the game's CCD

    [[nodiscard]] double cores() const noexcept
    {
        return captureSeconds > 0.0 ? (frameScopeCpuSeconds + presentMonCpuSeconds) / captureSeconds : 0.0;
    }
};

// Empty until there is enough capture time to say something (10 s).
std::vector<Finding> overheadFindings(const Overhead& overhead);

// For frame generation PresentMon can't see: the game has the library loaded.
// Empty if `libraries` is empty or generated frames are already reported.
std::vector<Finding> frameGenerationLibraryFindings(const std::vector<std::string>& libraries,
                                                    bool generatedFramesReported);

} // namespace framescope::core
