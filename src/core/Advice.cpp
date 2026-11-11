#include "core/Advice.h"

namespace framescope::core {

std::string_view adviceFor(Cause cause) noexcept
{
    switch (cause) {
    case Cause::DriverLatency:
        return "Update or reinstall the driver named in the evidence (GPU, network, audio, storage, chipset). "
               "Unplug USB devices one at a time to find a misbehaving one.";
    case Cause::GpuThrottle:
        return "Check GPU temperatures and fan curve, clean dust, improve case airflow. If it's a power limit, "
               "check the power cables and the power supply.";
    case Cause::CpuThrottle:
        return "Use the Balanced or High performance power plan, check CPU temperatures and cooler mounting, and "
               "look for power limits in the BIOS.";
    case Cause::LowMemory:
        return "Close memory-hungry programs (browser, launchers) or add RAM. Keep the page file enabled.";
    case Cause::VramFull:
        return "Lower texture quality or resolution, or turn off ray tracing, until VRAM use stays below the limit.";
    case Cause::Paging:
        return "The game is reading data from disk. Installing it on a fast SSD helps; otherwise this is up to the game.";
    case Cause::BackgroundLoad:
        return "Close the program named in the evidence while playing, or stop it from scanning or updating then.";
    case Cause::CcdScheduling:
        return "Install the AMD chipset driver, keep Xbox Game Bar and Game Mode on, and make sure Game Bar "
               "recognises the game (Win+G, 'Remember this is a game').";
    case Cause::StorageWait:
        return "Install the game on a fast SSD and check the drive's health and free space. If it stays, the game "
               "is streaming data at that moment.";
    case Cause::ShaderCompile:
        return "Nothing wrong with the PC. It fades as the shader cache fills; let the game finish any shader "
               "pre-compilation step, and expect it again after driver updates.";
    case Cause::GameCpu:
        return "The game's own work. Lowering CPU-heavy settings (crowd or view distance, simulation quality) or "
               "capping the frame rate can soften it.";
    case Cause::GameGpu:
        return "The game's own work. Lowering GPU-heavy settings or using an upscaler gives more headroom.";
    case Cause::UnexplainedWait:
        return "No cause found. Capture again with the full system trace on; if it stays unexplained, try with "
               "V-Sync and frame limiters off to rule those out.";
    case Cause::NoTraceData:
        return "Capture live with System trace switched on to find the cause.";
    }
    return {};
}

std::string adviceFor(const Finding& f)
{
    struct Entry {
        std::string_view title;
        std::string_view advice;
    };
    static constexpr Entry table[] = {
        {"GPU temperature near its limit", "Check the fan curve, clean dust and improve case airflow before it starts throttling."},
        {"Uneven frame pacing on screen",
         "Try a frame limiter a few fps below the refresh rate, check that VRR (FreeSync/G-Sync) is active, and "
         "compare with frame generation switched off."},
        {"Rendered frames that never reached the screen",
         "Optional: a frame limiter at or just below the refresh rate saves power and heat."},
        {"Trace events were lost", "Use Tools → Light system trace, and close other tracing tools (LatencyMon, xperf, overlays)."},
        {"FrameScope itself used a lot of CPU", "Use Tools → Light system trace and keep the window minimised while playing."},
        {"Frame generation library loaded",
         "If stutter looks odd here, capture once with frame generation switched off in the game to compare."},
        {"Stutter comes in bursts",
         "Compare with a second run of the same scene (Tools → Compare with earlier run): if the bursts are gone, it was "
         "shader compilation."},
    };
    static constexpr std::pair<std::string_view, Cause> byCause[] = {
        {"Some driver latency", Cause::DriverLatency},
        {"High driver latency", Cause::DriverLatency},
        {"GPU throttling", Cause::GpuThrottle},
        {"RAM nearly full", Cause::LowMemory},
        {"VRAM full", Cause::VramFull},
        {"VRAM nearly full", Cause::VramFull},
        {"CPU clocks were limited", Cause::CpuThrottle},
        {"Some background CPU load", Cause::BackgroundLoad},
        {"High background CPU load", Cause::BackgroundLoad},
        {"Game partly runs on the non-V-Cache CCD", Cause::CcdScheduling},
        {"Game runs on the wrong CCD", Cause::CcdScheduling},
        {"Spikes waiting on storage", Cause::StorageWait},
    };
    // Nothing to do about a check that is fine.
    if (f.level == Finding::Level::Good)
        return {};
    for (const auto& [title, cause] : byCause) {
        if (title == f.title)
            return std::string{adviceFor(cause)};
    }
    for (const auto& e : table) {
        if (e.title == f.title)
            return std::string{e.advice};
    }
    return {};
}

} // namespace framescope::core
