#include "core/Overhead.h"

#include <format>

namespace framescope::core {

std::vector<Finding> overheadFindings(const Overhead& o)
{
    using L = Finding::Level;
    std::vector<Finding> out;
    if (o.captureSeconds < 10.0)
        return out;

    const double cores = o.cores();
    const double lostShare = o.eventsTotal + o.eventsLost > 0
                                 ? static_cast<double>(o.eventsLost) / static_cast<double>(o.eventsTotal + o.eventsLost)
                                 : 0.0;
    auto detail = std::format("Measuring used ~{:.2f} CPU cores on average (FrameScope {:.2f}, PresentMon {:.2f})", cores,
                              o.frameScopeCpuSeconds / o.captureSeconds, o.presentMonCpuSeconds / o.captureSeconds);
    if (o.offVCacheCores)
        detail += ", kept off the cores the game runs on";
    if (o.eventsLost > 0)
        detail += std::format("; {} trace events were lost ({:.1f}%)", o.eventsLost, lostShare * 100.0);

    const char* hint = o.lightTrace ? "" : ". Tools → Light system trace lowers it";
    if (lostShare >= 0.01) {
        out.push_back({L::Warning, "Trace events were lost",
                       detail + ". Causes for some spikes may be missing" + hint});
    } else if (cores >= 1.0) {
        out.push_back({L::Warning, "FrameScope itself used a lot of CPU", detail + hint});
    } else {
        out.push_back({L::Good, "FrameScope's own load is low", detail});
    }
    return out;
}

std::vector<Finding> frameGenerationLibraryFindings(const std::vector<std::string>& libraries,
                                                    bool generatedFramesReported)
{
    std::vector<Finding> out;
    if (libraries.empty() || generatedFramesReported)
        return out;
    std::string names;
    for (const auto& lib : libraries)
        names += (names.empty() ? "" : ", ") + lib;
    out.push_back({Finding::Level::Info, "Frame generation library loaded",
                   std::format("The game has loaded {}. If it is switched on in the game, its generated frames can't be "
                               "told apart from rendered ones here: FPS and frame times then include them, and a spike "
                               "can look half as long as the game's real hitch",
                               names)});
    return out;
}

} // namespace framescope::core
