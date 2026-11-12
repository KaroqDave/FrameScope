#include "core/SessionStats.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace framescope::core {

std::vector<bool> leftOutFrames(std::span<const FrameRecord> frames, std::span<const Spike> spikes, double maxGapS)
{
    std::vector<bool> out(frames.size(), false);
    const Spike* previous = nullptr; // the last left-out spike, if no counted spike came after it
    for (const auto& s : spikes) {
        const bool leftOut = s.attribution && s.frameIndex < frames.size()
                             && (s.attribution->cause == Cause::Loading
                                 || s.attribution->cause == Cause::SettingsChange
                                 || s.attribution->cause == Cause::NotInForeground);
        if (!leftOut) {
            previous = nullptr;
            continue;
        }
        out[s.frameIndex] = true;
        if (previous && previous->frameIndex < s.frameIndex
            && (s.timeSeconds - s.frameTimeMs / 1000.0) - previous->timeSeconds <= maxGapS) {
            for (std::size_t i = previous->frameIndex + 1; i < s.frameIndex; ++i)
                out[i] = true;
        }
        previous = &s;
    }
    return out;
}

SessionStats computeStats(std::span<const FrameRecord> frames, std::span<const Spike> spikes)
{
    const std::size_t spikeCount = spikes.size();
    SessionStats s;
    s.frames = frames.size();
    s.spikes = spikeCount;
    if (frames.empty())
        return s;

    const auto leftOut = leftOutFrames(frames, spikes);
    std::vector<double> times;
    times.reserve(frames.size());
    double totalMs = 0.0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        totalMs += frames[i].frameTimeMs;
        if (!leftOut[i])
            times.push_back(frames[i].frameTimeMs);
    }
    s.leftOutFrames = frames.size() - times.size();

    s.durationSeconds = totalMs / 1000.0;
    s.averageFps = 1000.0 * static_cast<double>(frames.size()) / totalMs;

    if (!times.empty()) {
        s.worstFrameMs = *std::ranges::max_element(times);
        const auto idx = static_cast<std::size_t>(
            std::ceil(0.99 * static_cast<double>(times.size()))) - 1;
        const auto nth = times.begin() + static_cast<std::ptrdiff_t>(idx);
        std::nth_element(times.begin(), nth, times.end());
        s.onePercentLowFps = 1000.0 / *nth;
    }

    if (s.durationSeconds > 0.0)
        s.spikesPerMinute = static_cast<double>(spikeCount) * 60.0 / s.durationSeconds;
    return s;
}

} // namespace framescope::core
