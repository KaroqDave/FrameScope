#include "core/EventTimeline.h"

#include <algorithm>

namespace framescope::core {

std::string_view toString(EventKind k) noexcept
{
    switch (k) {
    case EventKind::Dpc:       return "DPC";
    case EventKind::Isr:       return "ISR";
    case EventKind::HardFault: return "Hard fault";
    case EventKind::DiskIo:    return "Disk I/O";
    }
    return "?";
}

EventTimeline::EventTimeline(std::int64_t qpcFrequency)
    : m_freq(qpcFrequency > 0 ? qpcFrequency : 10'000'000)
{
}

void EventTimeline::clear()
{
    m_events.clear();
    m_maxDuration = 0;
    m_traceStart.reset();
    m_coveredUntil = 0;
}

void EventTimeline::noteCoverage(std::int64_t qpc) noexcept
{
    m_coveredUntil = std::max(m_coveredUntil, qpc);
}

void EventTimeline::append(std::span<const SystemEvent> events)
{
    if (events.empty())
        return;

    const auto byStart = [](const SystemEvent& a, const SystemEvent& b) { return a.startQpc < b.startQpc; };
    const auto oldSize = static_cast<std::ptrdiff_t>(m_events.size());

    m_events.insert(m_events.end(), events.begin(), events.end());
    for (const auto& e : events) {
        m_maxDuration = std::max(m_maxDuration, e.durationQpc());
        noteCoverage(e.endQpc);
    }

    const auto mid = m_events.begin() + oldSize;
    std::sort(mid, m_events.end(), byStart);
    if (oldSize > 0 && byStart(*mid, *(mid - 1)))
        std::inplace_merge(m_events.begin(), mid, m_events.end(), byStart);
}

std::vector<const SystemEvent*> EventTimeline::overlapping(std::int64_t from, std::int64_t to) const
{
    std::vector<const SystemEvent*> out;
    // Any overlapping event must start after (from - longest event).
    const auto first = std::lower_bound(m_events.begin(), m_events.end(), from - m_maxDuration,
                                        [](const SystemEvent& e, std::int64_t t) { return e.startQpc < t; });
    for (auto it = first; it != m_events.end() && it->startQpc <= to; ++it) {
        if (it->endQpc >= from)
            out.push_back(&*it);
    }
    return out;
}

} // namespace framescope::core
