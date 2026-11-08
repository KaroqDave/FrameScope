#include "core/SystemSample.h"

#include <algorithm>

namespace framescope::core {

CpuTopology makeTopology(const std::vector<std::uint8_t>& ccdOfCpu, const std::vector<std::uint32_t>& l3KBOfCcd)
{
    CpuTopology t;
    t.ccdOfCpu = ccdOfCpu;
    t.l3KBOfCcd = l3KBOfCcd;
    if (l3KBOfCcd.size() > 1) {
        const auto [mn, mx] = std::ranges::minmax_element(l3KBOfCcd);
        if (*mx > *mn)
            t.vcacheCcd = static_cast<int>(mx - l3KBOfCcd.begin());
    }
    return t;
}

void GpuThrottleInference::apply(GpuSample& s)
{
    if (s.hotspotC && *s.hotspotC >= m_cfg.hotspotHighC)
        s.reasons |= gpu_reason::HotspotHigh;

    if (!s.usagePct || *s.usagePct < m_cfg.busyUsagePct || s.coreClockMHz <= 0.0)
        return;

    const bool judged = m_busySamples >= m_cfg.warmupSamples;
    const bool dropped = judged && s.coreClockMHz < m_cfg.dropRatio * m_typicalBusyClock;
    if (dropped) {
        s.reasons |= gpu_reason::ClockDrop;
        return; // don't let throttled clocks drag the baseline down
    }
    // Average of full-load clocks; quick at first, then slow.
    ++m_busySamples;
    const double alpha = m_busySamples < 20 ? 1.0 / m_busySamples : 0.05;
    m_typicalBusyClock += alpha * (s.coreClockMHz - m_typicalBusyClock);
}

} // namespace framescope::core
