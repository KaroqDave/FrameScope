#pragma once

#include "core/Attribution.h"
#include "core/FrameRecord.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace framescope::core {

struct SpikeConfig {
    std::size_t windowFrames = 120; // frames in the rolling median
    std::size_t warmupFrames = 30;  // no spikes until this many frames seen
    double ratio = 2.0;             // frame must be > ratio * median ...
    double minExcessMs = 8.0;       // ... AND at least this many ms above it
};

// Which side was busy during the spike frame. A first hint only;
// M2/M3 add the real system-side signals.
enum class Bound {
    Unknown, // no CPU/GPU busy data
    Cpu,     // CPU work dominated (game thread, shader compile, CPU contention)
    Gpu,     // GPU work dominated
    Stall,   // neither was busy for most of the frame: waiting on something
             // (I/O, paging, DPC, sync). Prime suspect for system issues.
};

std::string_view toString(Bound b) noexcept;

struct Spike {
    std::size_t frameIndex = 0;
    double timeSeconds = 0.0;
    double frameTimeMs = 0.0;
    double medianMs = 0.0;
    Bound bound = Bound::Unknown;

    // Filled in later by the correlator, once the trace has covered the frame.
    std::optional<Attribution> attribution;

    [[nodiscard]] double excessMs() const noexcept { return frameTimeMs - medianMs; }
};

class SpikeDetector {
public:
    explicit SpikeDetector(SpikeConfig cfg = {});

    // Feed frames in order. Returns a spike if this frame is one.
    std::optional<Spike> push(const FrameRecord& frame);

    void reset();

    [[nodiscard]] const SpikeConfig& config() const noexcept { return m_cfg; }
    [[nodiscard]] double currentMedian() const;

    static Bound classify(const FrameRecord& frame) noexcept;

private:
    SpikeConfig m_cfg;
    std::vector<double> m_window; // ring buffer
    std::size_t m_head = 0;
    std::size_t m_index = 0;
    mutable std::vector<double> m_scratch;
};

} // namespace framescope::core
