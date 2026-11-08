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
    Paging,          // the game's threads waited on hard page faults
    StorageWait,     // slow disk I/O during the frame
    GameCpu,         // CPU-heavy frame without system interference
    GameGpu,         // GPU-heavy frame without system interference
    UnexplainedWait, // nobody busy, no system event found (sync? M3 signals)
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
