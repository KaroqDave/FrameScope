#pragma once

#include "core/SystemEvent.h"
#include "core/SystemSample.h"

#include <array>
#include <deque>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace framescope::core {

// System events sorted by start time, with fast overlap queries.
// ETW delivers events per CPU buffer, so batches can arrive slightly out of
// order; append() keeps the vector sorted anyway.
class EventTimeline {
public:
    explicit EventTimeline(std::int64_t qpcFrequency = 10'000'000);

    void clear();

    // When the trace started; frames before this can't be judged.
    void setTraceStart(std::int64_t qpc) noexcept { m_traceStart = qpc; }
    [[nodiscard]] std::optional<std::int64_t> traceStart() const noexcept { return m_traceStart; }

    // Newest point in time the trace is known to have covered.
    void noteCoverage(std::int64_t qpc) noexcept;
    [[nodiscard]] std::int64_t coveredUntil() const noexcept { return m_coveredUntil; }

    void append(std::span<const SystemEvent> events);

    // Events whose [start, end] overlaps [from, to].
    [[nodiscard]] std::vector<const SystemEvent*> overlapping(std::int64_t from, std::int64_t to) const;

    // ---- Periodic system samples (RAM, VRAM, GPU clocks/throttling ...)
    void appendSamples(std::span<const SystemSample> samples);
    [[nodiscard]] const std::vector<SystemSample>& samples() const noexcept { return m_samples; }
    // Samples inside [from, to] plus the nearest one on each side, so even a
    // short frame between two samples gets the surrounding state.
    [[nodiscard]] std::vector<const SystemSample*> samplesAround(std::int64_t from, std::int64_t to) const;

    // ---- CPU scheduling (from context switches)
    void setTopology(CpuTopology topology) { m_topology = std::move(topology); }
    [[nodiscard]] const CpuTopology& topology() const noexcept { return m_topology; }
    void appendCpuBuckets(std::span<const CpuBucket> buckets);
    [[nodiscard]] std::vector<const CpuBucket*> bucketsIn(std::int64_t from, std::int64_t to) const;
    // Buckets are only needed until their spikes are attributed; totals stay.
    void pruneBucketsBefore(std::int64_t qpc);
    [[nodiscard]] const std::array<double, kMaxCcds>& gameUsByCcdTotal() const noexcept { return m_gameUsByCcd; }
    [[nodiscard]] std::uint64_t gameMigrationsTotal() const noexcept { return m_gameMigrations; }
    [[nodiscard]] std::uint64_t gameCrossCcdMigrationsTotal() const noexcept { return m_gameCrossCcd; }

    [[nodiscard]] std::int64_t qpcFrequency() const noexcept { return m_freq; }
    [[nodiscard]] std::size_t size() const noexcept { return m_events.size(); }
    [[nodiscard]] const std::vector<SystemEvent>& events() const noexcept { return m_events; }

    [[nodiscard]] double toMicroseconds(std::int64_t ticks) const noexcept
    {
        return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(m_freq);
    }
    [[nodiscard]] std::int64_t fromMilliseconds(double ms) const noexcept
    {
        return static_cast<std::int64_t>(ms * static_cast<double>(m_freq) / 1000.0);
    }

private:
    std::int64_t m_freq;
    std::vector<SystemEvent> m_events;
    std::int64_t m_maxDuration = 0;
    std::optional<std::int64_t> m_traceStart;
    std::int64_t m_coveredUntil = 0;

    std::vector<SystemSample> m_samples;
    CpuTopology m_topology;
    std::deque<CpuBucket> m_buckets;
    std::array<double, kMaxCcds> m_gameUsByCcd{};
    std::uint64_t m_gameMigrations = 0;
    std::uint64_t m_gameCrossCcd = 0;
};

} // namespace framescope::core
