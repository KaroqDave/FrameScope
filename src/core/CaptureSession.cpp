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

SessionStats CaptureSession::stats() const
{
    return computeStats(m_frames, m_spikes.size());
}

} // namespace framescope::core
