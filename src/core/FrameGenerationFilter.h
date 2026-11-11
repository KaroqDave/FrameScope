#pragma once

#include "core/FrameRecord.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace framescope::core {

// Finds frame generation that PresentMon doesn't label (NVIDIA DLSS Frame
// Generation and Multi Frame Generation, or any SDK that presents its
// generated frames itself) from the rhythm of the presents, and folds the
// generated frames into the rendered frame they belong to.
//
// The pattern: the generated frames are presented almost at the same moment
// as a rendered frame, so the time between presents alternates between one
// long gap (the game rendering) and 1-5 gaps near zero, group after group,
// while the frames still reach the screen evenly.
//
// Some frame generation (seen with FSR-FG) instead spaces its frames evenly.
// Then the gaps say nothing, but the generated presents carry no CPU or GPU
// work of their own while the rendered one's work spans the whole group.
//
// Frames pass through with a delay of `kWindow` frames, because whether a
// frame is part of such a group can only be told from the frames around it.
class FrameGenerationFilter {
public:
    static constexpr std::size_t kWindow = 96;

    // Feed frames in order; returns the frames that are ready (possibly none).
    std::vector<FrameRecord> push(FrameRecord frame);
    // End of the capture: returns everything still held back.
    std::vector<FrameRecord> flush();
    void reset();

    // Generated frames per rendered frame in the pattern last seen (0 = none).
    [[nodiscard]] int generatedPerFrame() const noexcept { return m_lastFactor; }
    [[nodiscard]] std::size_t mergedFrames() const noexcept { return m_merged; }
    // "2x, detected from the frame pattern" ...; empty if never detected.
    [[nodiscard]] std::string description() const;

private:
    struct Pattern {
        bool active = false;
        int generated = 0;
        bool byWork = false;       // told apart by CPU/GPU work instead of by the gap
        double shortBelowMs = 0.0;
    };
    [[nodiscard]] static bool isGenerated(const Pattern& p, const FrameRecord& f);
    [[nodiscard]] Pattern analyse() const;
    [[nodiscard]] bool analyseAs(Pattern& p) const;
    void emitFront(std::vector<FrameRecord>& out, bool flushing);

    std::deque<FrameRecord> m_buffer;
    Pattern m_pattern;
    std::size_t m_sinceAnalysis = 0;
    int m_longsInARow = 0;

    // Generated frames waiting for the rendered frame that closes their group.
    std::uint16_t m_pendingCount = 0;
    double m_pendingMs = 0.0;
    double m_pendingDisplayedMs = 0.0;
    std::optional<std::int64_t> m_pendingQpcStart;
    std::optional<double> m_pendingCpuBusyMs; // PresentMon puts the group's CPU/GPU work on
    std::optional<double> m_pendingGpuBusyMs; // either row (seen with FSR-FG: on the near-zero one)

    int m_lastFactor = 0;
    std::size_t m_merged = 0;
};

} // namespace framescope::core
