#pragma once

#include "core/FrameRecord.h"

#include <cstddef>
#include <span>

namespace framescope::core {

struct SessionStats {
    std::size_t frames = 0;
    double durationSeconds = 0.0;
    double averageFps = 0.0;
    double onePercentLowFps = 0.0; // 1000 / 99th-percentile frame time
    double worstFrameMs = 0.0;
    std::size_t spikes = 0;
    double spikesPerMinute = 0.0;
};

SessionStats computeStats(std::span<const FrameRecord> frames, std::size_t spikeCount);

} // namespace framescope::core
