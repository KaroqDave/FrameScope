#include "core/CaptureSession.h"

#include <string>
#include <string_view>

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
    m_frameGeneration.clear();
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
    if (done > 0)
        done += markLeadUpToFocusLoss(m_spikes);
    return done;
}

namespace {
constexpr std::string_view kLeadUpDetail = "Part of a slowdown that ran straight into the game losing focus "
                                           "(pause menu and alt-tab, or stopping the capture); not counted";
constexpr std::string_view kCaptureEndDetail = "Part of a slowdown right before the capture was stopped "
                                               "(closing the game, pausing, switching to FrameScope); not counted";

// Marks the burst that ends in spikes[last + 1] (or in the capture's end at anchorS).
std::size_t walkBack(std::span<Spike> spikes, std::size_t count, double anchorS, std::string_view detail,
                     double maxGapS, double maxReachS)
{
    std::size_t changed = 0;
    double next = anchorS;
    for (std::size_t j = count; j-- > 0;) {
        auto& s = spikes[j];
        if (next - s.timeSeconds > maxGapS || anchorS - s.timeSeconds > maxReachS)
            break;
        next = s.timeSeconds;
        if (!s.attribution || s.attribution->cause == Cause::NotInForeground)
            continue;
        if (s.attribution->side == Side::System)
            break; // a system-side cause was found for it: that stands
        s.attribution = Attribution{Cause::NotInForeground, Side::Unknown, 0.6, std::string{detail}, {}};
        ++changed;
    }
    return changed;
}
}

std::size_t markLeadUpToFocusLoss(std::span<Spike> spikes, double maxGapS, double maxReachS)
{
    std::size_t changed = 0;
    for (std::size_t i = spikes.size(); i-- > 1;) {
        const auto& anchor = spikes[i];
        if (!anchor.attribution || anchor.attribution->cause != Cause::NotInForeground
            || anchor.attribution->detail == kLeadUpDetail || anchor.attribution->detail == kCaptureEndDetail)
            continue; // only a real focus loss starts a walk, not a spike marked here earlier
        changed += walkBack(spikes, i, anchor.timeSeconds, kLeadUpDetail, maxGapS, maxReachS);
    }
    return changed;
}

std::size_t markSlowdownAtCaptureEnd(std::span<Spike> spikes, double captureEndS, double maxGapS, double maxReachS)
{
    return walkBack(spikes, spikes.size(), captureEndS, kCaptureEndDetail, maxGapS, maxReachS);
}

std::size_t CaptureSession::closeOut()
{
    if (m_frames.empty())
        return 0;
    return markLeadUpToFocusLoss(m_spikes) + markSlowdownAtCaptureEnd(m_spikes, m_frames.back().timeSeconds);
}

void CaptureSession::restore(std::vector<FrameRecord> frames, std::vector<Spike> spikes)
{
    m_detector.reset();
    m_frames = std::move(frames);
    m_spikes = std::move(spikes);
    closeOut(); // sessions saved before these rules existed
}

SessionStats CaptureSession::stats() const
{
    return computeStats(m_frames, m_spikes.size());
}

} // namespace framescope::core
