#include "core/DisplayAnalysis.h"

#include <algorithm>
#include <format>

namespace framescope::core {
namespace {

// A spike reaches the screen a little later than it happens (display latency),
// and with frame generation it is spread over neighbouring frames.
constexpr double kBeforeS = 0.10;
constexpr double kAfterS = 0.30;

bool anyWithin(const std::vector<double>& sorted, double from, double to)
{
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), from);
    return it != sorted.end() && *it <= to;
}

} // namespace

DisplayAnalysis analyzeDisplay(std::span<const FrameRecord> frames, std::span<const Spike> spikes)
{
    DisplayAnalysis a;
    a.renderedFrames = frames.size();
    a.spikes = spikes.size();
    if (frames.empty() || !std::ranges::any_of(frames, [](const FrameRecord& f) { return f.displayTracked; }))
        return a;
    a.available = true;

    // Walk through everything that was shown, in order, and look for frames
    // that stayed on screen too long.
    SpikeDetector detector;
    double renderedMs = 0.0, displayedMs = 0.0;
    const auto show = [&](double timeSeconds, double ms) {
        ++a.displayedFrames;
        displayedMs += ms;
        FrameRecord shown;
        shown.timeSeconds = timeSeconds;
        shown.frameTimeMs = ms;
        if (detector.push(shown)) {
            ++a.screenHitches;
            a.screenHitchTimes.push_back(timeSeconds);
        }
    };

    for (const auto& f : frames) {
        renderedMs += f.frameTimeMs;
        if (!f.displayTracked)
            continue;
        if (f.generatedFrames > 0) {
            a.generatedFrames += f.generatedFrames;
            const double each = f.generatedDisplayedMs / f.generatedFrames;
            for (unsigned i = 0; i < f.generatedFrames; ++i)
                show(f.timeSeconds, each);
        }
        if (f.displayedMs)
            show(f.timeSeconds, *f.displayedMs);
        else
            ++a.droppedFrames;
    }

    a.renderedFps = renderedMs > 0.0 ? 1000.0 * static_cast<double>(a.renderedFrames) / renderedMs : 0.0;
    a.displayedFps = displayedMs > 0.0 ? 1000.0 * static_cast<double>(a.displayedFrames) / displayedMs : 0.0;

    std::vector<double> spikeTimes;
    spikeTimes.reserve(spikes.size());
    for (const auto& s : spikes)
        spikeTimes.push_back(s.timeSeconds);
    std::ranges::sort(spikeTimes);
    std::ranges::sort(a.screenHitchTimes);

    for (const double t : spikeTimes)
        a.spikesSeenOnScreen += anyWithin(a.screenHitchTimes, t - kBeforeS, t + kAfterS);
    for (const double t : a.screenHitchTimes)
        a.screenHitchesWithoutSpike += !anyWithin(spikeTimes, t - kAfterS, t + kBeforeS);
    return a;
}

std::vector<Finding> displayFindings(const DisplayAnalysis& a, const std::string& frameGenerationName)
{
    using L = Finding::Level;
    std::vector<Finding> out;
    if (!a.available || a.displayedFrames == 0)
        return out;

    if (a.generatedShare() >= 0.05) {
        out.push_back({L::Info, "Frame generation is active",
                       std::format("{:.0f}% of the frames on screen are generated{}: {:.0f} fps shown, {:.0f} fps rendered "
                                   "by the game. Stutter is judged on the rendered frames",
                                   a.generatedShare() * 100.0,
                                   frameGenerationName.empty() ? std::string{} : " (" + frameGenerationName + ")",
                                   a.displayedFps, a.renderedFps)});
    }

    if (a.droppedShare() >= 0.02) {
        out.push_back({L::Info, "Rendered frames that never reached the screen",
                       std::format("{:.0f}% of the game's frames were not shown ({} of {}). Normal when the game renders "
                                   "faster than the display refreshes; a frame limiter avoids the wasted work",
                                   a.droppedShare() * 100.0, a.droppedFrames, a.renderedFrames)});
    }

    // Hitches on screen that the game's own frame times don't show: the frames
    // were rendered evenly but shown unevenly.
    const std::size_t significant = std::max<std::size_t>(5, a.screenHitches / 4);
    const auto seen = a.spikes > 0 ? std::format("{} of {} spikes were visible on screen", a.spikesSeenOnScreen, a.spikes)
                                   : std::string{"no spikes in the game's frame times"};
    if (a.screenHitchesWithoutSpike >= significant) {
        out.push_back({L::Warning, "Uneven frame pacing on screen",
                       std::format("{} hitches on screen without a spike in the game's frame times ({}). The frames were "
                                   "rendered evenly but shown unevenly: check frame generation, V-Sync/VRR and frame "
                                   "limiter settings",
                                   a.screenHitchesWithoutSpike, seen)});
    } else if (a.spikes > 0 || a.screenHitches > 0) {
        out.push_back({L::Good, "Frame pacing on screen matches the game",
                       std::format("{} hitches on screen, {}", a.screenHitches, seen)});
    }
    return out;
}

void refreshFrameFindings(std::vector<Finding>& findings, std::span<const FrameRecord> frames,
                          std::span<const Spike> spikes, const std::string& frameGenerationName,
                          const DisplayAnalysis* cachedAnalysis)
{
    linkFindingsToSpikes(findings, spikes);

    constexpr std::string_view recomputed[] = {
        "Stutter comes in bursts",
        "Stutter is spread evenly",
        "Frame generation is active",
        "Rendered frames that never reached the screen",
        "Uneven frame pacing on screen",
        "Frame pacing on screen matches the game",
    };
    std::erase_if(findings, [&](const Finding& f) {
        return std::ranges::find(recomputed, f.title) != std::end(recomputed)
               || f.title.ends_with("spikes left out of the verdict");
    });
    const auto leftOut = leftOutFindings(spikes);
    findings.insert(findings.end(), leftOut.begin(), leftOut.end());

    const auto display = displayFindings(cachedAnalysis ? *cachedAnalysis : analyzeDisplay(frames, spikes), frameGenerationName);
    findings.insert(findings.end(), display.begin(), display.end());
    const auto pattern = stutterPatternFindings(frames, spikes);
    findings.insert(findings.end(), pattern.begin(), pattern.end());
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) { return a.level > b.level; });
}

} // namespace framescope::core
