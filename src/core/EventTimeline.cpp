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
    m_samples.clear();
    m_buckets.clear();
    m_gameUsByCcd.fill(0.0);
    m_gameMigrations = 0;
    m_gameCrossCcd = 0;
    // topology is a property of the machine, keep it
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

void EventTimeline::appendSamples(std::span<const SystemSample> samples)
{
    const auto byQpc = [](const SystemSample& a, const SystemSample& b) { return a.qpc < b.qpc; };
    const auto oldSize = static_cast<std::ptrdiff_t>(m_samples.size());
    m_samples.insert(m_samples.end(), samples.begin(), samples.end());
    const auto mid = m_samples.begin() + oldSize;
    std::sort(mid, m_samples.end(), byQpc);
    if (oldSize > 0 && mid != m_samples.end() && byQpc(*mid, *(mid - 1)))
        std::inplace_merge(m_samples.begin(), mid, m_samples.end(), byQpc);
}

std::vector<const SystemSample*> EventTimeline::samplesAround(std::int64_t from, std::int64_t to) const
{
    std::vector<const SystemSample*> out;
    if (m_samples.empty())
        return out;
    auto first = std::lower_bound(m_samples.begin(), m_samples.end(), from,
                                  [](const SystemSample& s, std::int64_t t) { return s.qpc < t; });
    if (first != m_samples.begin())
        --first; // the state just before the window
    for (auto it = first; it != m_samples.end(); ++it) {
        out.push_back(&*it);
        if (it->qpc > to)
            break; // include one sample after the window, then stop
    }
    return out;
}

void EventTimeline::appendCpuBuckets(std::span<const CpuBucket> buckets)
{
    for (const auto& b : buckets) {
        for (std::size_t i = 0; i < kMaxCcds; ++i)
            m_gameUsByCcd[i] += b.gameUsByCcd[i];
        m_gameMigrations += b.gameMigrations;
        m_gameCrossCcd += b.gameCrossCcdMigrations;
        m_buckets.push_back(b);
    }
    std::sort(m_buckets.begin(), m_buckets.end(),
              [](const CpuBucket& a, const CpuBucket& b) { return a.startQpc < b.startQpc; });
}

std::vector<const CpuBucket*> EventTimeline::bucketsIn(std::int64_t from, std::int64_t to) const
{
    std::vector<const CpuBucket*> out;
    auto it = std::lower_bound(m_buckets.begin(), m_buckets.end(), from,
                               [](const CpuBucket& b, std::int64_t t) { return b.endQpc <= t; });
    for (; it != m_buckets.end() && it->startQpc < to; ++it)
        out.push_back(&*it);
    return out;
}

void EventTimeline::pruneBucketsBefore(std::int64_t qpc)
{
    while (!m_buckets.empty() && m_buckets.front().endQpc < qpc)
        m_buckets.pop_front();
}

} // namespace framescope::core
