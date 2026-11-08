#pragma once

#include "core/Correlator.h"
#include "core/EventTimeline.h"
#include "core/FrameRecord.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

#include <optional>
#include <vector>

namespace framescope::core {

// All frames and spikes of one capture, in time order.
// This is the in-memory timeline M2/M3 will attach system events to.
class CaptureSession {
public:
    explicit CaptureSession(SpikeConfig cfg = {});

    std::optional<Spike> append(FrameRecord frame);
    void clear();

    // Replace everything with a saved session (frames + attributed spikes).
    void restore(std::vector<FrameRecord> frames, std::vector<Spike> spikes);

    [[nodiscard]] const std::vector<FrameRecord>& frames() const noexcept { return m_frames; }
    [[nodiscard]] const std::vector<Spike>& spikes() const noexcept { return m_spikes; }
    [[nodiscard]] SessionStats stats() const;

    // Attribute spikes whose frame the trace has fully covered, waiting
    // settleMs for late ETW buffers. Does nothing while no trace has started.
    // Returns how many spikes got a new attribution.
    std::size_t attributePending(const EventTimeline& timeline, double settleMs = 1500.0,
                                 const CorrelatorConfig& cfg = {});

    [[nodiscard]] Diagnosis diagnosis() const { return diagnose(m_spikes); }

private:
    SpikeDetector m_detector;
    std::vector<FrameRecord> m_frames;
    std::vector<Spike> m_spikes;
};

} // namespace framescope::core
