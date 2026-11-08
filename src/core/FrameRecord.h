#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace framescope::core {

// One presented frame of the tracked swap chain.
struct FrameRecord {
    // Seconds since capture start, at the end of this frame.
    double timeSeconds = 0.0;

    // Time between this present and the previous one (what the player feels).
    double frameTimeMs = 0.0;

    // How long the CPU / GPU were actually busy on this frame.
    // Missing when PresentMon reports "NA" or the column doesn't exist.
    std::optional<double> cpuBusyMs;
    std::optional<double> gpuBusyMs;

    std::uint32_t processId = 0;
    std::string application;
};

} // namespace framescope::core
