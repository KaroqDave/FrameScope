#include "core/CaptureSession.h"

namespace framescope::core {

CaptureSession::CaptureSession(SpikeConfig cfg)
    : m_detector(cfg)
{
}

std::optional<Spike> CaptureSession::append(FrameRecord frame)
{
    auto spike = m_detector.push(frame);
    m_frames.push_back(std::move(frame));
    if (spike)
        m_spikes.push_back(*spike);
    return spike;
}

void CaptureSession::clear()
{
    m_detector.reset();
    m_frames.clear();
    m_spikes.clear();
}

std::size_t CaptureSession::attributePending(const EventTimeline& timeline, double settleMs,
                                             const CorrelatorConfig& cfg)
{
    if (!timeline.traceStart())
        return 0;

    const auto settled = timeline.coveredUntil() - timeline.fromMilliseconds(settleMs);
    std::size_t done = 0;
    for (auto& spike : m_spikes) {
        if (spike.attribution)
            continue;
        const auto& frame = m_frames[spike.frameIndex];
        if (frame.qpcEnd && *frame.qpcEnd > settled)
            continue; // trace may not have delivered this window yet
        spike.attribution = attribute(spike, frame, timeline, cfg);
        ++done;
    }
    return done;
}

void CaptureSession::restore(std::vector<FrameRecord> frames, std::vector<Spike> spikes)
{
    m_detector.reset();
    m_frames = std::move(frames);
    m_spikes = std::move(spikes);
}

SessionStats CaptureSession::stats() const
{
    return computeStats(m_frames, m_spikes.size());
}

} // namespace framescope::core
