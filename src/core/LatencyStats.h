#pragma once

#include "core/SystemEvent.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace framescope::core {

// Per-driver DPC/ISR statistics over the whole trace, like LatencyMon.
// Counts every DPC/ISR, not only the long ones kept on the timeline.
struct ModuleLatency {
    std::string module;
    std::uint64_t dpcCount = 0;
    std::uint64_t isrCount = 0;
    double maxDpcUs = 0.0;
    double maxIsrUs = 0.0;
    double totalUs = 0.0;
    std::uint64_t over500us = 0;
    std::uint64_t over1ms = 0;

    [[nodiscard]] double maxUs() const noexcept { return maxDpcUs > maxIsrUs ? maxDpcUs : maxIsrUs; }
};

class LatencyStats {
public:
    void add(EventKind kind, const std::string& module, double durationUs);
    void clear() { m_modules.clear(); }
    // Replaces everything with per-driver totals collected elsewhere.
    void assign(const std::vector<ModuleLatency>& modules);

    // Sorted by worst single DPC/ISR, longest first.
    [[nodiscard]] std::vector<ModuleLatency> worst(std::size_t n) const;
    [[nodiscard]] bool empty() const noexcept { return m_modules.empty(); }

private:
    std::map<std::string, ModuleLatency, std::less<>> m_modules;
};

} // namespace framescope::core
