#pragma once

#include "core/FrameRecord.h"
#include "core/SpikeDetector.h"

#include <cstddef>
#include <span>
#include <vector>

namespace framescope::core {

struct SessionStats {
    std::size_t frames = 0;
    double durationSeconds = 0.0;
    double averageFps = 0.0;
    // 1% low and worst frame describe play: frames left out of the verdict
    // (loading, game not in the foreground) don't go into them.
    double onePercentLowFps = 0.0; // 1000 / 99th-percentile frame time
    double worstFrameMs = 0.0;
    std::size_t leftOutFrames = 0; // frames not counted for the two above
    std::size_t spikes = 0;
    double spikesPerMinute = 0.0;
};

// Which frames belong to a spike that is left out of the verdict because the
// game was loading or not in the foreground, plus the frames between two such
// spikes at most maxGapS apart (the same loading screen). One flag per frame.
std::vector<bool> leftOutFrames(std::span<const FrameRecord> frames, std::span<const Spike> spikes,
                                double maxGapS = 0.25);

SessionStats computeStats(std::span<const FrameRecord> frames, std::span<const Spike> spikes);

} // namespace framescope::core
