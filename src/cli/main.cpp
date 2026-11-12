// framescope-cli <capture.csv|session.fscope> [--top N] [--report out.html] [--compare second-run]
//
// Offline analysis: stats, worst spikes, optional HTML report and comparison
// with a second run of the same sequence (input = run 1, --compare = run 2).

#include "core/Advice.h"
#include "core/CaptureSession.h"
#include "core/Comparison.h"
#include "core/DisplayAnalysis.h"
#include "core/PresentMonCsvParser.h"
#include "core/Report.h"
#include "core/SessionFile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace framescope::core;

namespace {

// std::print needs GCC 14; Qt's MinGW kit ships GCC 13.
template <typename... Args>
void out(std::FILE* f, std::format_string<Args...> fmt, Args&&... args)
{
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), f);
    std::fputc('\n', f);
}

struct Loaded {
    CaptureSession session;
    SavedSession meta; // findings/drivers when loaded from .fscope
};

Loaded load(const std::filesystem::path& path)
{
    Loaded l;
    if (isSessionFile(path)) {
        l.meta = loadSession(path);
        l.session.restore(l.meta.frames, l.meta.spikes, l.meta.frameGeneration, l.meta.endedInPlay);
        if (const auto n = l.session.refilteredFrames(); n > 0) {
            out(stdout, "{}: older session, {} generated frames folded into their rendered frames ({} -> {} spikes)",
                path.filename().string(), n, l.session.spikesBeforeRefilter(), l.session.spikes().size());
        }
    } else {
        std::string frameGeneration;
        for (auto& f : loadPresentMonCsv(path, &frameGeneration))
            l.session.append(std::move(f));
        l.session.setFrameGeneration(frameGeneration);
        if (!l.session.frames().empty())
            l.meta.application = l.session.frames().front().application;
    }
    return l;
}

void printStats(const CaptureSession& session)
{
    const auto s = session.stats();
    out(stdout, "Frames        {:>10}", s.frames);
    out(stdout, "Duration      {:>10.1f} s", s.durationSeconds);
    out(stdout, "Average FPS   {:>10.1f}", s.averageFps);
    out(stdout, "1% low FPS    {:>10.1f}", s.onePercentLowFps);
    out(stdout, "Worst frame   {:>10.1f} ms", s.worstFrameMs);
    if (s.leftOutFrames > 0)
        out(stdout, "              {:>10} frames left out of 1% low and worst frame (loading, game not in front)",
            s.leftOutFrames);
    out(stdout, "Spikes        {:>10}  ({:.1f}/min)", s.spikes, s.spikesPerMinute);
    const auto d = session.diagnosis();
    if (d.attributed > 0)
        out(stdout, "Verdict       {}", d.verdict);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        out(stderr, "usage: framescope-cli <capture.csv|session.fscope> [--top N] [--report out.html] [--compare second-run]");
        return 2;
    }

    std::size_t top = 10;
    std::string reportPath, comparePath;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--top" && i + 1 < argc)
            top = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--report" && i + 1 < argc)
            reportPath = argv[++i];
        else if (a == "--compare" && i + 1 < argc)
            comparePath = argv[++i];
        else {
            out(stderr, "unknown argument: {}", a);
            return 2;
        }
    }

    try {
        const auto run = load(argv[1]);
        printStats(run.session);

        auto spikes = run.session.spikes();
        std::ranges::sort(spikes, std::greater{}, &Spike::excessMs);
        if (spikes.size() > top)
            spikes.resize(top);
        if (!spikes.empty()) {
            out(stdout, "\n{:>9}  {:>9}  {:>9}  {:>9}  {:<6} {}", "Time s", "Frame ms", "Median", "Excess", "Bound", "Cause");
            for (const auto& sp : spikes) {
                out(stdout, "{:>9.2f}  {:>9.1f}  {:>9.1f}  {:>9.1f}  {:<6} {}", sp.timeSeconds, sp.frameTimeMs,
                    sp.medianMs, sp.excessMs(), toString(sp.bound),
                    sp.attribution ? std::format("{} ({})", toString(sp.attribution->cause), sp.attribution->detail)
                                   : std::string{"-"});
            }
        }

        // Findings: saved ones (from .fscope) plus the stutter pattern from the frames.
        auto findings = run.meta.findings;
        const auto display = analyzeDisplay(run.session.frames(), run.session.spikes());
        refreshFrameFindings(findings, run.session.frames(), run.session.spikes(), run.session.frameGeneration(), &display);
        if (display.available) {
            out(stdout, "On screen     {:>10.1f} fps  ({:.0f}% generated, {:.0f}% of rendered frames not shown, {} hitches)",
                display.displayedFps, display.generatedShare() * 100.0, display.droppedShare() * 100.0,
                display.screenHitches);
        }
        if (!findings.empty()) {
            out(stdout, "\nFindings:");
            for (const auto& f : findings)
                out(stdout, "  [{}] {}: {}{}", toString(f.level), f.title, f.detail,
                    impactText(f).empty() ? std::string{} : " -> " + impactText(f));
            for (const auto& f : findings) {
                if (const auto advice = adviceFor(f); !advice.empty())
                    out(stdout, "  What to do about \"{}\": {}", f.title, advice);
            }
        }

        if (!run.meta.shaderWrites.empty()) {
            std::size_t disk = 0;
            for (const auto& w : run.meta.shaderWrites)
                disk += w.disk;
            out(stdout, "\nShader cache: {} writes, {} disk flushes", run.meta.shaderWrites.size() - disk, disk);
        }

        std::optional<RunComparison> comparison;
        if (!comparePath.empty()) {
            const auto second = load(comparePath);
            comparison = compareRuns(run.session.frames(), run.session.spikes(), second.session.frames(),
                                     second.session.spikes());
            out(stdout, "\nComparison (run 1 = {}, run 2 = {}):", std::filesystem::path(argv[1]).filename().string(),
                std::filesystem::path(comparePath).filename().string());
            out(stdout, "  Spikes/min   {:.1f} -> {:.1f} ({:+.0f}%)", comparison->first.stats.spikesPerMinute,
                comparison->second.stats.spikesPerMinute, comparison->spikeRateChange * 100.0);
            out(stdout, "  Same moments {} of {} ({:.0f}%), offset {:+.1f} s", comparison->matchedSpikes,
                std::min(comparison->first.stats.spikes, comparison->second.stats.spikes),
                comparison->repeatShare * 100.0, comparison->offsetSeconds);
            out(stdout, "  {}", comparison->verdict);
            for (const auto& n : comparison->notes)
                out(stdout, "  Note: {}", n);
        }

        if (!reportPath.empty()) {
            ReportData d;
            d.application = run.meta.application;
            d.title = d.application.empty() ? "FrameScope report" : "FrameScope report: " + d.application;
            d.createdAt = run.meta.createdAt;
            d.sensors = run.meta.sensors;
            d.frames = run.session.frames();
            d.spikes = run.session.spikes();
            d.findings = findings;
            d.drivers = run.meta.drivers;
            d.comparison = comparison;
            std::ofstream f(reportPath, std::ios::binary);
            f << renderHtmlReport(d);
            if (!f)
                throw std::runtime_error("Cannot write " + reportPath);
            out(stdout, "\nReport written to {}", reportPath);
        }
    } catch (const std::exception& e) {
        out(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
