#include "core/FrameGenerationFilter.h"

#include <algorithm>
#include <array>
#include <format>

namespace framescope::core {
namespace {

constexpr std::size_t kMinGroups = 8;      // groups needed in the window to call it a pattern
constexpr double kShortShare = 0.3;        // "near zero" = below this share of the window's average gap
constexpr double kShortMaxMs = 4.0;        // ... and never longer than this
constexpr double kRegularity = 0.8;        // share of groups that must have the same size
constexpr double kShownShare = 0.6;        // generated frames reach the screen; duplicates that get dropped don't
constexpr int kMaxGenerated = 5;           // DLSS Multi Frame Generation goes up to 6x
constexpr std::size_t kAnalyseEvery = 8;

// A present without any CPU or GPU work of its own: PresentMon books the
// whole group's work on one row and leaves the other rows at exactly zero.
bool noWorkOfItsOwn(const FrameRecord& f)
{
    return f.cpuBusyMs && f.gpuBusyMs && *f.cpuBusyMs == 0.0 && *f.gpuBusyMs == 0.0;
}

std::optional<double> maxOf(const std::optional<double>& a, const std::optional<double>& b)
{
    if (!a)
        return b;
    return b ? std::max(*a, *b) : a;
}

} // namespace

void FrameGenerationFilter::reset()
{
    *this = FrameGenerationFilter{};
}

std::string FrameGenerationFilter::description() const
{
    if (m_merged == 0 || m_lastFactor == 0)
        return {};
    return std::format("{}x, detected from the frame pattern", m_lastFactor + 1);
}

bool FrameGenerationFilter::isGenerated(const Pattern& p, const FrameRecord& f)
{
    return p.byWork ? noWorkOfItsOwn(f) : f.frameTimeMs < p.shortBelowMs;
}

FrameGenerationFilter::Pattern FrameGenerationFilter::analyse() const
{
    if (m_buffer.size() < kWindow / 2)
        return {};

    double total = 0.0;
    for (const auto& f : m_buffer)
        total += f.frameTimeMs;

    // 1. Generated frames presented right behind the rendered one: near-zero gaps.
    Pattern byGap;
    byGap.shortBelowMs = std::min(kShortMaxMs, kShortShare * total / static_cast<double>(m_buffer.size()));
    if (analyseAs(byGap))
        return byGap;

    // 2. Generated frames paced evenly between the rendered ones (seen with
    //    FSR-FG): the gaps all look alike, but only every other present
    //    carries CPU/GPU work, and that work spans the whole group.
    Pattern byWork;
    byWork.byWork = true;
    if (analyseAs(byWork))
        return byWork;
    return {};
}

bool FrameGenerationFilter::analyseAs(Pattern& p) const
{
    // Sizes of the runs of generated frames between two rendered ones.
    std::array<std::size_t, kMaxGenerated + 2> runs{}; // index = run length, last = longer
    std::size_t groups = 0, shorts = 0, shortsShown = 0, shortsTracked = 0;
    std::size_t spanning = 0;  // rendered frames whose CPU work is longer than their own gap
    double pendingMs = 0.0;
    int run = -1; // -1 until the first rendered frame, so a cut-off group at the start isn't counted
    for (const auto& f : m_buffer) {
        if (isGenerated(p, f)) {
            ++shorts;
            if (f.displayTracked) {
                ++shortsTracked;
                shortsShown += f.displayedMs.has_value();
            }
            if (run >= 0)
                ++run;
            pendingMs += f.frameTimeMs;
        } else {
            if (run >= 0) {
                ++runs[std::min<std::size_t>(static_cast<std::size_t>(run), runs.size() - 1)];
                ++groups;
                if (run > 0 && f.cpuBusyMs && *f.cpuBusyMs > f.frameTimeMs + 0.5 * pendingMs)
                    ++spanning;
            }
            run = 0;
            pendingMs = 0.0;
        }
    }
    if (groups < kMinGroups || shorts == 0)
        return false;

    const auto best = std::max_element(runs.begin() + 1, runs.end() - 1);
    const auto generated = static_cast<int>(best - runs.begin());
    if (static_cast<double>(*best) < kRegularity * static_cast<double>(groups))
        return false;
    if (shortsTracked > 0 && static_cast<double>(shortsShown) < kShownShare * static_cast<double>(shortsTracked))
        return false;
    // Without the near-zero gaps the work has to prove it: it must reach
    // back over the generated frames in nearly every group.
    if (p.byWork && static_cast<double>(spanning) < kRegularity * static_cast<double>(groups))
        return false;

    p.active = true;
    p.generated = generated;
    return true;
}

void FrameGenerationFilter::emitFront(std::vector<FrameRecord>& out, bool flushing)
{
    if (m_sinceAnalysis == 0 && !flushing) {
        // The window looks ahead, so it stops showing the pattern a little
        // before the last groups have passed. Keep the old pattern until the
        // groups really stop (two rendered frames in a row without generated ones).
        if (const auto next = analyse(); next.active || !m_pattern.active || m_longsInARow >= 2)
            m_pattern = next;
    }
    m_sinceAnalysis = (m_sinceAnalysis + 1) % kAnalyseEvery;

    FrameRecord f = std::move(m_buffer.front());
    m_buffer.pop_front();
    if (m_pattern.active)
        m_longsInARow = isGenerated(m_pattern, f) ? 0 : m_longsInARow + 1;

    if (m_pattern.active && isGenerated(m_pattern, f) && m_pendingCount < kMaxGenerated) {
        // A generated frame: hold it for the rendered frame that follows.
        if (m_pendingCount == 0)
            m_pendingQpcStart = f.qpcStart;
        ++m_pendingCount;
        m_pendingMs += f.frameTimeMs;
        m_pendingDisplayedMs += f.displayedMs.value_or(0.0);
        m_pendingCpuBusyMs = maxOf(m_pendingCpuBusyMs, f.cpuBusyMs);
        m_pendingGpuBusyMs = maxOf(m_pendingGpuBusyMs, f.gpuBusyMs);
        return;
    }

    if (m_pendingCount > 0) {
        // The rendered frame of the group: it took the whole group's time.
        f.frameTimeMs += m_pendingMs;
        f.generatedFrames = static_cast<std::uint16_t>(f.generatedFrames + m_pendingCount);
        f.generatedDisplayedMs += m_pendingDisplayedMs;
        if (m_pendingQpcStart && f.qpcStart)
            f.qpcStart = std::min(*f.qpcStart, *m_pendingQpcStart);
        f.cpuBusyMs = maxOf(f.cpuBusyMs, m_pendingCpuBusyMs);
        f.gpuBusyMs = maxOf(f.gpuBusyMs, m_pendingGpuBusyMs);
        // Busy times were measured against a single row; never more than the group took.
        if (f.cpuBusyMs)
            f.cpuBusyMs = std::min(*f.cpuBusyMs, f.frameTimeMs);
        if (f.gpuBusyMs)
            f.gpuBusyMs = std::min(*f.gpuBusyMs, f.frameTimeMs);
        m_pendingCpuBusyMs.reset();
        m_pendingGpuBusyMs.reset();
        m_merged += m_pendingCount;
        m_lastFactor = m_pattern.active ? m_pattern.generated : m_lastFactor;
        m_pendingCount = 0;
        m_pendingMs = 0.0;
        m_pendingDisplayedMs = 0.0;
        m_pendingQpcStart.reset();
    }
    out.push_back(std::move(f));
}

std::vector<FrameRecord> FrameGenerationFilter::push(FrameRecord frame)
{
    std::vector<FrameRecord> out;
    m_buffer.push_back(std::move(frame));
    while (m_buffer.size() > kWindow)
        emitFront(out, false);
    return out;
}

std::vector<FrameRecord> FrameGenerationFilter::flush()
{
    std::vector<FrameRecord> out;
    // The last pattern still describes these frames.
    while (!m_buffer.empty())
        emitFront(out, true);
    return out;
}

} // namespace framescope::core
