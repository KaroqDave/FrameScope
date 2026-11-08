#include "core/SystemSample.h"

#include <algorithm>
#include <format>

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

double perfLimitFromCounter(double counterValue) noexcept
{
    // Turbo/boost can push the counter above 100; that's no limit either.
    return std::clamp(100.0 - counterValue, 0.0, 100.0);
}

std::string describe(const SystemSample& s)
{
    std::string out;
    const auto add = [&out](const std::string& part) {
        if (!out.empty())
            out += ", ";
        out += part;
    };
    if (s.availableRamMB)
        add(std::format("RAM {:.1f}{} GB free", *s.availableRamMB / 1024.0,
                        s.totalRamMB ? std::format("/{:.1f}", *s.totalRamMB / 1024.0) : std::string{}));
    if (s.pageInsPerSec)
        add(std::format("page-ins {:.0f}/s", *s.pageInsPerSec));
    if (s.cpuPerfLimitPct)
        add(std::format("CPU capped by {:.0f}%", *s.cpuPerfLimitPct));
    if (s.gameVramMB || s.vramTotalMB)
        add(std::format("game VRAM {}/{} GB", s.gameVramMB ? std::format("{:.1f}", *s.gameVramMB / 1024.0) : std::string{"-"},
                        s.vramTotalMB ? std::format("{:.1f}", *s.vramTotalMB / 1024.0) : std::string{"-"}));
    if (s.gpu) {
        std::string g = std::format("GPU {:.0f} MHz {:.0f} °C", s.gpu->coreClockMHz, s.gpu->temperatureC);
        if (s.gpu->hotspotC)
            g += std::format(" (hotspot {:.0f} °C)", *s.gpu->hotspotC);
        g += std::format(" {:.0f} W", s.gpu->powerW);
        if (s.gpu->usagePct)
            g += std::format(" {:.0f}% load", *s.gpu->usagePct);
        g += std::format(" reasons 0x{:x}{}", s.gpu->reasons, s.gpu->driverReportsReasons ? "" : " (inferred)");
        add(g);
    }
    return out.empty() ? std::string{"(empty sample)"} : out;
}

std::string describe(const CpuTopology& t)
{
    if (t.ccdCount() == 0)
        return "unknown";
    std::string l3;
    for (const auto kb : t.l3KBOfCcd)
        l3 += std::format("{}{} MB", l3.empty() ? "" : " / ", kb / 1024);
    return std::format("{} logical CPUs, {} L3 group(s): {}{}", t.ccdOfCpu.size(), t.ccdCount(), l3,
                       t.hasAsymmetricCache() ? std::format(", 3D V-Cache on CCD{}", t.vcacheCcd) : std::string{});
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
