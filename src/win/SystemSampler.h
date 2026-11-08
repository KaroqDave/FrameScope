#pragma once

#include "core/SystemSample.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace framescope::win {

// Samples system state a few times per second on its own thread:
//  - PDH: available RAM, page-ins/s, CPU "% Performance Limit",
//         the game's dedicated VRAM use
//  - DXGI: total VRAM of the biggest GPU
//  - NVML (loaded at runtime from the NVIDIA driver): GPU clock, temperature,
//         power and throttle reasons
// Every source is optional; whatever fails to initialise is just left out.
class SystemSampler {
public:
    SystemSampler();
    ~SystemSampler();
    SystemSampler(const SystemSampler&) = delete;
    SystemSampler& operator=(const SystemSampler&) = delete;

    static bool isSupported() noexcept;

    bool start(int intervalMs = 250);
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;

    void setTargetProcess(std::uint32_t pid);

    // Thread-safe. Samples collected since the last call.
    std::vector<core::SystemSample> drain();

    // Which sources are active, e.g. "PDH, DXGI (32 GB VRAM), NVML (NVIDIA GeForce RTX 5090)".
    [[nodiscard]] std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// L3 cache groups (= CCDs on Ryzen) of the logical CPUs.
core::CpuTopology queryCpuTopology();

// Executable name of a running process ("" if it can't be opened). Cached.
std::string processName(std::uint32_t pid);

// True if this process runs with admin rights (needed for ETW and PresentMon).
bool isElevated();

} // namespace framescope::win
