#include "core/CaptureSession.h"

#include "core/FrameGenerationFilter.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    m_endedInPlay = false;
    m_refilteredFrames = 0;
    m_spikesBeforeRefilter = 0;
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
        done += markLeadUpToFocusLoss(m_spikes) + markLoading(m_spikes) + markSettingsChange(m_spikes, m_frames, false);
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

std::size_t markLoading(std::span<Spike> spikes, double minStalledS, double maxGapS, double slowFrameMs)
{
    const auto start = [](const Spike& s) { return s.timeSeconds - s.frameTimeMs / 1000.0; };
    const auto slow = [&](const Spike& s) { return s.frameTimeMs >= slowFrameMs; };
    std::size_t changed = 0;
    for (std::size_t first = 0; first < spikes.size(); ++first) {
        if (!slow(spikes[first]))
            continue;
        // A stretch: slow frames (nearly) back to back. Smaller spikes in between don't matter.
        std::size_t last = first;
        double stalledS = spikes[first].frameTimeMs / 1000.0;
        for (std::size_t i = first + 1; i < spikes.size(); ++i) {
            if (start(spikes[i]) - spikes[last].timeSeconds > maxGapS)
                break;
            if (slow(spikes[i])) {
                last = i;
                stalledS += spikes[i].frameTimeMs / 1000.0;
            }
        }
        if (stalledS >= minStalledS) {
            // The stretch, plus the small spikes right around it (the game settling).
            const double from = start(spikes[first]) - maxGapS, to = spikes[last].timeSeconds + maxGapS;
            for (auto& s : spikes) {
                if (s.timeSeconds < from || start(s) > to)
                    continue;
                if (!s.attribution || !countsForVerdict(s.attribution->cause) || s.attribution->side == Side::System)
                    continue; // not attributed yet, already left out, or a system-side cause that stands
                s.attribution = Attribution{Cause::Loading, Side::Unknown, 0.6,
                                            std::format("The game all but stopped presenting for {:.1f} s (loading "
                                                        "screen or level change); not counted", stalledS),
                                            {}};
                ++changed;
            }
        }
        first = last;
    }
    return changed;
}

namespace {

// Median frame time of the frames in [fromS, toS] that aren't spikes; empty
// if they cover less than minS.
std::optional<double> levelMs(std::span<const FrameRecord> frames, std::span<const Spike> spikes, double fromS,
                              double toS, double minS)
{
    const auto byTime = [](const FrameRecord& f, double t) { return f.timeSeconds < t; };
    const auto first = std::lower_bound(frames.begin(), frames.end(), fromS, byTime);
    const auto last = std::lower_bound(first, frames.end(), toS, byTime);
    const auto firstIndex = static_cast<std::size_t>(first - frames.begin());
    const auto lastIndex = static_cast<std::size_t>(last - frames.begin());
    auto spike = std::lower_bound(spikes.begin(), spikes.end(), firstIndex,
                                  [](const Spike& s, std::size_t i) { return s.frameIndex < i; });
    std::vector<double> times;
    double coveredMs = 0.0;
    for (std::size_t i = firstIndex; i < lastIndex; ++i) {
        coveredMs += frames[i].frameTimeMs;
        while (spike != spikes.end() && spike->frameIndex < i)
            ++spike;
        if (spike != spikes.end() && spike->frameIndex == i)
            continue;
        times.push_back(frames[i].frameTimeMs);
    }
    if (times.empty() || coveredMs < minS * 1000.0)
        return std::nullopt;
    const auto mid = times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2);
    std::nth_element(times.begin(), mid, times.end());
    return *mid;
}

} // namespace

std::size_t markSettingsChange(std::span<Spike> spikes, std::span<const FrameRecord> frames, bool captureEnded,
                               const SettingsChangeConfig& cfg)
{
    const double minAfterS = captureEnded ? cfg.minWindowS : std::max(cfg.minWindowS, cfg.windowS - 0.5);
    const auto start = [](const Spike& s) { return s.timeSeconds - s.frameTimeMs / 1000.0; };
    std::size_t changed = 0;
    for (std::size_t first = 0; first < spikes.size();) {
        std::size_t last = first;
        while (last + 1 < spikes.size() && start(spikes[last + 1]) - spikes[last].timeSeconds <= cfg.maxGapS)
            ++last;
        const double from = start(spikes[first]), to = spikes[last].timeSeconds;
        if (last - first + 1 >= cfg.minSpikes && to - from <= cfg.maxBurstS) {
            const auto before = levelMs(frames, spikes, from - cfg.windowS, from, cfg.minWindowS);
            const auto after = levelMs(frames, spikes, to + 1e-9, to + cfg.windowS, minAfterS);
            if (before && after && *before > 0.0 && *after > 0.0
                && std::max(*before, *after) >= cfg.minRatio * std::min(*before, *after)) {
                for (std::size_t i = first; i <= last; ++i) {
                    auto& s = spikes[i];
                    if (!s.attribution || !countsForVerdict(s.attribution->cause) || s.attribution->side == Side::System)
                        continue; // not attributed yet, already left out, or a system-side cause that stands
                    s.attribution = Attribution{Cause::SettingsChange, Side::Unknown, 0.5,
                                                std::format("The frame rate went from {:.0f} to {:.0f} fps and stayed "
                                                            "there (graphics settings changed, or a menu); not counted",
                                                            1000.0 / *before, 1000.0 / *after),
                                                {}};
                    ++changed;
                }
            }
        }
        first = last + 1;
    }
    return changed;
}

std::size_t CaptureSession::closeOut()
{
    if (m_frames.empty())
        return 0;
    const auto atEnd = m_endedInPlay ? 0 : markSlowdownAtCaptureEnd(m_spikes, m_frames.back().timeSeconds);
    return markLeadUpToFocusLoss(m_spikes) + atEnd + markLoading(m_spikes) + markSettingsChange(m_spikes, m_frames);
}

void CaptureSession::restore(std::vector<FrameRecord> frames, std::vector<Spike> spikes, std::string frameGeneration,
                             bool endedInPlay)
{
    m_detector.reset();
    m_endedInPlay = endedInPlay;
    m_refilteredFrames = 0;
    m_spikesBeforeRefilter = spikes.size();

    FrameGenerationFilter filter;
    std::vector<FrameRecord> folded;
    folded.reserve(frames.size());
    for (const auto& f : frames) {
        for (auto& r : filter.push(f))
            folded.push_back(std::move(r));
    }
    for (auto& r : filter.flush())
        folded.push_back(std::move(r));

    if (filter.mergedFrames() > 0) {
        // The frames changed, so the spikes have to be found again. A spike
        // keeps what was known about it: the cause of the saved spike that
        // ended within its frame, preferably the one ending with it.
        constexpr double kEps = 1e-6;
        std::vector<Spike> found;
        SpikeDetector detector{m_detector.config()};
        std::size_t old = 0;
        for (const auto& f : folded) {
            auto spike = detector.push(f);
            if (!spike)
                continue;
            const double from = f.timeSeconds - f.frameTimeMs / 1000.0;
            while (old < spikes.size() && spikes[old].timeSeconds <= from + kEps)
                ++old;
            for (; old < spikes.size() && spikes[old].timeSeconds <= f.timeSeconds + kEps; ++old) {
                if (spikes[old].attribution)
                    spike->attribution = std::move(spikes[old].attribution);
            }
            found.push_back(std::move(*spike));
        }
        m_refilteredFrames = filter.mergedFrames();
        frames = std::move(folded);
        spikes = std::move(found);
        if (frameGeneration.empty())
            frameGeneration = filter.description();
    }

    m_frames = std::move(frames);
    m_spikes = std::move(spikes);
    m_frameGeneration = std::move(frameGeneration);
    closeOut(); // sessions saved before these rules existed
}

SessionStats CaptureSession::stats() const
{
    return computeStats(m_frames, m_spikes);
}

} // namespace framescope::core
