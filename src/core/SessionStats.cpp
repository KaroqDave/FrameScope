#include "core/SessionStats.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace framescope::core {

SessionStats computeStats(std::span<const FrameRecord> frames, std::size_t spikeCount)
{
    SessionStats s;
    s.frames = frames.size();
    s.spikes = spikeCount;
    if (frames.empty())
        return s;

    std::vector<double> times;
    times.reserve(frames.size());
    double totalMs = 0.0;
    for (const auto& f : frames) {
        times.push_back(f.frameTimeMs);
        totalMs += f.frameTimeMs;
    }

    s.durationSeconds = totalMs / 1000.0;
    s.averageFps = 1000.0 * static_cast<double>(frames.size()) / totalMs;
    s.worstFrameMs = *std::ranges::max_element(times);

    const auto idx = static_cast<std::size_t>(
        std::ceil(0.99 * static_cast<double>(times.size()))) - 1;
    const auto nth = times.begin() + static_cast<std::ptrdiff_t>(idx);
    std::nth_element(times.begin(), nth, times.end());
    s.onePercentLowFps = 1000.0 / *nth;

    if (s.durationSeconds > 0.0)
        s.spikesPerMinute = static_cast<double>(spikeCount) * 60.0 / s.durationSeconds;
    return s;
}

} // namespace framescope::core
