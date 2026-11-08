// framescope-cli <capture.csv> [--top N]
// Offline analysis of a PresentMon CSV: stats plus the worst spikes.

#include "core/CaptureSession.h"
#include "core/PresentMonCsvParser.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        out(stderr, "usage: framescope-cli <presentmon.csv> [--top N]");
        return 2;
    }

    std::size_t top = 10;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--top")
            top = static_cast<std::size_t>(std::strtoul(argv[i + 1], nullptr, 10));
    }

    try {
        CaptureSession session;
        for (auto& f : loadPresentMonCsv(argv[1]))
            session.append(std::move(f));

        const auto s = session.stats();
        out(stdout, "Frames        {:>10}", s.frames);
        out(stdout, "Duration      {:>10.1f} s", s.durationSeconds);
        out(stdout, "Average FPS   {:>10.1f}", s.averageFps);
        out(stdout, "1% low FPS    {:>10.1f}", s.onePercentLowFps);
        out(stdout, "Worst frame   {:>10.1f} ms", s.worstFrameMs);
        out(stdout, "Spikes        {:>10}  ({:.1f}/min)", s.spikes, s.spikesPerMinute);

        auto spikes = session.spikes();
        std::ranges::sort(spikes, std::greater{}, &Spike::excessMs);
        if (spikes.size() > top)
            spikes.resize(top);

        if (!spikes.empty()) {
            out(stdout, "\n{:>9}  {:>9}  {:>9}  {:>9}  {}", "Time s", "Frame ms", "Median", "Excess", "Bound");
            for (const auto& sp : spikes) {
                out(stdout, "{:>9.2f}  {:>9.1f}  {:>9.1f}  {:>9.1f}  {}",
                    sp.timeSeconds, sp.frameTimeMs, sp.medianMs, sp.excessMs(),
                    toString(sp.bound));
            }
        }
    } catch (const std::exception& e) {
        out(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
