#include "core/LatencyStats.h"

#include <algorithm>

namespace framescope::core {

void LatencyStats::add(EventKind kind, const std::string& module, double us)
{
    auto it = m_modules.find(module);
    if (it == m_modules.end())
        it = m_modules.emplace(module, ModuleLatency{.module = module}).first;

    auto& m = it->second;
    if (kind == EventKind::Isr) {
        ++m.isrCount;
        m.maxIsrUs = std::max(m.maxIsrUs, us);
    } else {
        ++m.dpcCount;
        m.maxDpcUs = std::max(m.maxDpcUs, us);
    }
    m.totalUs += us;
    if (us >= 500.0)
        ++m.over500us;
    if (us >= 1000.0)
        ++m.over1ms;
}

std::vector<ModuleLatency> LatencyStats::worst(std::size_t n) const
{
    std::vector<ModuleLatency> out;
    out.reserve(m_modules.size());
    for (const auto& [_, m] : m_modules)
        out.push_back(m);
    std::ranges::sort(out, std::greater{}, &ModuleLatency::maxUs);
    if (out.size() > n)
        out.resize(n);
    return out;
}

} // namespace framescope::core
