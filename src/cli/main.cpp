// framescope-cli <capture.csv|session.fscope> [--top N] [--report out.html] [--compare second-run]
//
// Offline analysis: stats, worst spikes, optional HTML report and comparison
// with a second run of the same sequence (input = run 1, --compare = run 2).

#include "core/CaptureSession.h"
#include "core/Comparison.h"
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
        l.session.restore(l.meta.frames, l.meta.spikes);
    } else {
        for (auto& f : loadPresentMonCsv(path))
            l.session.append(std::move(f));
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
        linkFindingsToSpikes(findings, run.session.spikes());
        std::erase_if(findings, [](const Finding& f) { return f.title.starts_with("Stutter "); });
        for (auto& f : stutterPatternFindings(run.session.frames(), run.session.spikes()))
            findings.push_back(std::move(f));
        if (!findings.empty()) {
            out(stdout, "\nFindings:");
            for (const auto& f : findings)
                out(stdout, "  [{}] {}: {}{}", toString(f.level), f.title, f.detail,
                    impactText(f).empty() ? std::string{} : " -> " + impactText(f));
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
