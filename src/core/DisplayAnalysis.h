#pragma once

#include "core/Correlator.h"
#include "core/FrameRecord.h"
#include "core/SpikeDetector.h"

#include <span>
#include <string>
#include <vector>

namespace framescope::core {

// What actually reached the screen, as opposed to what the game rendered.
// Needs a capture with display tracking; generated frames additionally need
// PresentMon --track_frame_type and an instrumented driver/SDK.
struct DisplayAnalysis {
    bool available = false;            // the capture has display times

    std::size_t renderedFrames = 0;    // application frames
    std::size_t droppedFrames = 0;     // ... that never reached the screen
    std::size_t generatedFrames = 0;   // frames inserted by frame generation
    std::size_t displayedFrames = 0;   // shown application frames + generated frames

    double renderedFps = 0.0;
    double displayedFps = 0.0;

    // Frames that stayed on screen much longer than the ones around them
    // (same rule as spikes: > 2x the rolling median and >= 8 ms above it).
    std::size_t screenHitches = 0;
    std::vector<double> screenHitchTimes;      // seconds on the session time axis

    std::size_t spikes = 0;                    // spikes in the game's frame times
    std::size_t spikesSeenOnScreen = 0;        // ... with a screen hitch at the same moment
    std::size_t screenHitchesWithoutSpike = 0; // hitches the game's frame times don't show

    [[nodiscard]] double generatedShare() const noexcept
    {
        return displayedFrames ? static_cast<double>(generatedFrames) / static_cast<double>(displayedFrames) : 0.0;
    }
    [[nodiscard]] double droppedShare() const noexcept
    {
        return renderedFrames ? static_cast<double>(droppedFrames) / static_cast<double>(renderedFrames) : 0.0;
    }
};

DisplayAnalysis analyzeDisplay(std::span<const FrameRecord> frames, std::span<const Spike> spikes);

// Findings about frame generation, dropped frames and pacing on screen.
// frameGenerationName: e.g. "AMD AFMF"; may be empty.
std::vector<Finding> displayFindings(const DisplayAnalysis& analysis, const std::string& frameGenerationName = {});

// Brings the findings that are derived from frames and spikes up to date:
// links checks to the spikes they explain, and (re)computes the stutter
// pattern and the display findings. Safe to call on findings loaded from a
// saved session. Pass a cached analysis to skip recomputing it.
void refreshFrameFindings(std::vector<Finding>& findings, std::span<const FrameRecord> frames,
                          std::span<const Spike> spikes, const std::string& frameGenerationName = {},
                          const DisplayAnalysis* cachedAnalysis = nullptr);

} // namespace framescope::core
