#pragma once

#include <string>
#include <string_view>

namespace framescope::core {

// Who is to blame for a spike.
enum class Side {
    Unknown,
    System, // your PC: drivers, hardware, OS, background load
    Game,   // the game's own work
    Mixed,  // could be either (e.g. streaming from disk)
};

enum class Cause {
    NoTraceData,     // spike happened while no system trace was running
    DriverLatency,   // long DPC/ISR from a driver
    GpuThrottle,     // GPU clocks dropped (thermal / power brake / HW slowdown)
    CpuThrottle,     // CPU clocks capped by the OS/firmware
    LowMemory,       // hard faults while RAM was nearly full
    VramFull,        // game at the VRAM limit, the driver is swapping
    Paging,          // the game's threads waited on hard page faults (RAM was fine)
    BackgroundLoad,  // another process took a lot of CPU during the frame
    CcdScheduling,   // game threads on / moving to the CCD without 3D V-Cache
    StorageWait,     // slow disk I/O during the frame
    GameCpu,         // CPU-heavy frame without system interference
    GameGpu,         // GPU-heavy frame without system interference
    UnexplainedWait, // nobody busy, no system event found (sync? M3 signals)
    ShaderCompile,   // the game wrote to a shader cache during the frame (appended: saved files store the number)
};

std::string_view toString(Side s) noexcept;
std::string_view toString(Cause c) noexcept;

struct Attribution {
    Cause cause = Cause::NoTraceData;
    Side side = Side::Unknown;
    double confidence = 0.0; // 0..1
    std::string detail;      // human-readable evidence, e.g. "DPC 2.4 ms in nvlddmkm.sys"
    std::string module;      // DriverLatency: the driver responsible
};

} // namespace framescope::core
