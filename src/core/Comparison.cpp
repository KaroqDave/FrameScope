#include "core/Comparison.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace framescope::core {
namespace {

struct MatchResult {
    std::size_t matched = 0;
    double error = 0.0; // sum of |time difference| over matched pairs
};

// Greedy two-pointer matching of sorted spike times; each spike used once.
MatchResult countMatches(const std::vector<double>& a, const std::vector<double>& b, double offset, double tol)
{
    std::size_t i = 0, j = 0;
    MatchResult r;
    while (i < a.size() && j < b.size()) {
        const double tb = b[j] + offset;
        if (std::fabs(a[i] - tb) <= tol) {
            ++r.matched;
            r.error += std::fabs(a[i] - tb);
            ++i;
            ++j;
        } else if (a[i] < tb) {
            ++i;
        } else {
            ++j;
        }
    }
    return r;
}

std::vector<double> times(std::span<const Spike> spikes)
{
    std::vector<double> t;
    t.reserve(spikes.size());
    for (const auto& s : spikes)
        t.push_back(s.timeSeconds);
    std::ranges::sort(t);
    return t;
}

} // namespace

RunSummary summarizeRun(std::span<const FrameRecord> frames, std::span<const Spike> spikes)
{
    RunSummary r;
    r.stats = computeStats(frames, spikes.size());
    for (const auto& s : spikes) {
        switch (s.bound) {
        case Bound::Cpu:   ++r.cpuSpikes; break;
        case Bound::Gpu:   ++r.gpuSpikes; break;
        case Bound::Stall: ++r.stallSpikes; break;
        case Bound::Unknown: break;
        }
        if (s.attribution && s.attribution->cause != Cause::NoTraceData)
            ++r.causes[s.attribution->cause];
    }
    return r;
}

RunComparison compareRuns(std::span<const FrameRecord> firstFrames, std::span<const Spike> firstSpikes,
                          std::span<const FrameRecord> secondFrames, std::span<const Spike> secondSpikes,
                          const ComparisonConfig& cfg)
{
    RunComparison c;
    c.first = summarizeRun(firstFrames, firstSpikes);
    c.second = summarizeRun(secondFrames, secondSpikes);

    const auto a = times(firstSpikes);
    const auto b = times(secondSpikes);

    // Find the offset that lines up the most spikes; ties go to the tightest fit.
    const int steps = static_cast<int>(std::lround(cfg.maxOffsetSeconds / cfg.offsetStepSeconds));
    double bestError = 0.0;
    for (int k = -steps; k <= steps; ++k) {
        const double offset = k * cfg.offsetStepSeconds;
        const auto m = countMatches(a, b, offset, cfg.toleranceSeconds);
        if (m.matched > c.matchedSpikes || (m.matched == c.matchedSpikes && m.matched > 0 && m.error < bestError - 1e-9)) {
            c.matchedSpikes = m.matched;
            c.offsetSeconds = offset;
            bestError = m.error;
        }
    }
    const auto fewer = std::min(a.size(), b.size());
    c.repeatShare = fewer > 0 ? static_cast<double>(c.matchedSpikes) / static_cast<double>(fewer) : 0.0;

    const double rate1 = c.first.stats.spikesPerMinute;
    const double rate2 = c.second.stats.spikesPerMinute;
    c.spikeRateChange = rate1 > 0.0 ? (rate2 - rate1) / rate1 : 0.0;

    const double minutes = std::min(c.first.stats.durationSeconds, c.second.stats.durationSeconds) / 60.0;
    if (std::fabs(c.first.stats.durationSeconds - c.second.stats.durationSeconds) >
        0.25 * std::max(c.first.stats.durationSeconds, c.second.stats.durationSeconds)) {
        c.notes.push_back("The runs differ a lot in length; make sure both cover the same sequence.");
    }

    // ---- Verdict
    if (a.size() <= 2 && b.size() <= 2) {
        c.verdict = "Both runs are smooth; nothing to compare.";
        return c;
    }

    const bool dropped = rate1 >= 2.0 && c.spikeRateChange <= -0.6;
    const std::size_t cpuDrop = c.first.cpuSpikes > c.second.cpuSpikes ? c.first.cpuSpikes - c.second.cpuSpikes : 0;
    const std::size_t totalDrop = a.size() > b.size() ? a.size() - b.size() : 0;

    if (dropped) {
        c.verdict = std::format(
            "Likely shader compilation: run 2 has {:.0f}% fewer spikes{}. The game should smooth out once its shader "
            "cache is warm (game side).",
            -c.spikeRateChange * 100.0, totalDrop > 0 && 2 * cpuDrop >= totalDrop ? ", mostly CPU-bound ones" : "");
    } else if (c.repeatShare >= 0.6 && std::fabs(c.spikeRateChange) <= 0.4) {
        c.verdict = std::format(
            "Stutter repeats at the same moments in both runs ({:.0f}% of spikes line up): likely game content such "
            "as asset streaming or level traversal (game side).",
            c.repeatShare * 100.0);
    } else if (c.repeatShare < 0.3 && std::fabs(c.spikeRateChange) <= 0.4) {
        c.verdict = std::format(
            "Similar amount of stutter, but at random moments ({:.0f}% line up): points to the system or to "
            "unpredictable game work. Check the per-spike causes.",
            c.repeatShare * 100.0);
    } else {
        c.verdict = std::format("Inconclusive: spike rate changed by {:+.0f}%, {:.0f}% of spikes line up.",
                                c.spikeRateChange * 100.0, c.repeatShare * 100.0);
    }

    const auto shaderIn = [](const RunSummary& r) {
        const auto it = r.causes.find(Cause::ShaderCompile);
        return it == r.causes.end() ? std::size_t{0} : it->second;
    };
    if (shaderIn(c.first) + shaderIn(c.second) > 0) {
        c.notes.push_back(std::format("Spikes with shader cache writes: run 1 {}, run 2 {}.", shaderIn(c.first),
                                      shaderIn(c.second)));
    }
    if (minutes < 1.0)
        c.notes.push_back("Runs shorter than a minute give weak results.");
    c.notes.push_back("Only meaningful if both runs played the same sequence from the same starting point.");
    return c;
}

} // namespace framescope::core
