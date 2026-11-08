#include "core/SpikeDetector.h"

#include <algorithm>

namespace framescope::core {

std::string_view toString(Bound b) noexcept
{
    switch (b) {
    case Bound::Cpu:   return "CPU";
    case Bound::Gpu:   return "GPU";
    case Bound::Stall: return "Stall";
    case Bound::Unknown: break;
    }
    return "Unknown";
}

SpikeDetector::SpikeDetector(SpikeConfig cfg)
    : m_cfg(cfg)
{
    m_cfg.windowFrames = std::max<std::size_t>(m_cfg.windowFrames, 1);
    m_cfg.warmupFrames = std::clamp<std::size_t>(m_cfg.warmupFrames, 1, m_cfg.windowFrames);
    m_window.reserve(m_cfg.windowFrames);
}

void SpikeDetector::reset()
{
    m_window.clear();
    m_head = 0;
    m_index = 0;
}

double SpikeDetector::currentMedian() const
{
    if (m_window.empty())
        return 0.0;

    m_scratch = m_window;
    const auto n = m_scratch.size();
    const auto mid = m_scratch.begin() + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(m_scratch.begin(), mid, m_scratch.end());
    if (n % 2 == 1)
        return *mid;
    const double lower = *std::max_element(m_scratch.begin(), mid);
    return (lower + *mid) / 2.0;
}

Bound SpikeDetector::classify(const FrameRecord& f) noexcept
{
    if (!f.cpuBusyMs || !f.gpuBusyMs)
        return Bound::Unknown;

    const double cpu = *f.cpuBusyMs;
    const double gpu = *f.gpuBusyMs;
    if (std::max(cpu, gpu) < 0.5 * f.frameTimeMs)
        return Bound::Stall;
    return cpu >= gpu ? Bound::Cpu : Bound::Gpu;
}

std::optional<Spike> SpikeDetector::push(const FrameRecord& frame)
{
    std::optional<Spike> result;

    if (m_window.size() >= m_cfg.warmupFrames) {
        const double median = currentMedian();
        const double ft = frame.frameTimeMs;
        if (ft > median * m_cfg.ratio && ft - median >= m_cfg.minExcessMs) {
            result = Spike{
                .frameIndex = m_index,
                .timeSeconds = frame.timeSeconds,
                .frameTimeMs = ft,
                .medianMs = median,
                .bound = classify(frame),
                .attribution = std::nullopt,
            };
        }
    }

    // Spikes go into the window too; the median is robust against them.
    if (m_window.size() < m_cfg.windowFrames) {
        m_window.push_back(frame.frameTimeMs);
    } else {
        m_window[m_head] = frame.frameTimeMs;
        m_head = (m_head + 1) % m_cfg.windowFrames;
    }

    ++m_index;
    return result;
}

} // namespace framescope::core
