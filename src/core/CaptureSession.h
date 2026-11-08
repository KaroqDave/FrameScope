#pragma once

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

    [[nodiscard]] const std::vector<FrameRecord>& frames() const noexcept { return m_frames; }
    [[nodiscard]] const std::vector<Spike>& spikes() const noexcept { return m_spikes; }
    [[nodiscard]] SessionStats stats() const;

private:
    SpikeDetector m_detector;
    std::vector<FrameRecord> m_frames;
    std::vector<Spike> m_spikes;
};

} // namespace framescope::core
