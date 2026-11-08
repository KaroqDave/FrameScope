#include "core/Correlator.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace framescope::core {

std::string_view toString(Side s) noexcept
{
    switch (s) {
    case Side::System:  return "System";
    case Side::Game:    return "Game";
    case Side::Mixed:   return "Mixed";
    case Side::Unknown: break;
    }
    return "Unknown";
}

std::string_view toString(Cause c) noexcept
{
    switch (c) {
    case Cause::NoTraceData:     return "No trace data";
    case Cause::DriverLatency:   return "Driver latency";
    case Cause::Paging:          return "Hard page faults";
    case Cause::StorageWait:     return "Storage wait";
    case Cause::GameCpu:         return "Game CPU work";
    case Cause::GameGpu:         return "Game GPU work";
    case Cause::UnexplainedWait: return "Unexplained wait";
    }
    return "?";
}

namespace {

double clippedUs(const EventTimeline& tl, const SystemEvent& e, std::int64_t from, std::int64_t to)
{
    const auto s = std::max(e.startQpc, from);
    const auto t = std::min(e.endQpc, to);
    return t > s ? tl.toMicroseconds(t - s) : 0.0;
}

// Evidence share of the excess -> 0.3..0.95
double confidenceFor(double evidenceUs, double excessUs)
{
    const double ratio = excessUs > 0.0 ? evidenceUs / excessUs : 1.0;
    return std::clamp(0.3 + 0.6 * ratio, 0.3, 0.95);
}

Attribution make(Cause c, Side s, double confidence, std::string detail, std::string module = {})
{
    return Attribution{c, s, confidence, std::move(detail), std::move(module)};
}

std::string ms(double us)
{
    return std::format("{:.1f} ms", us / 1000.0);
}

} // namespace

WindowEvidence gatherEvidence(const EventTimeline& tl, std::int64_t from, std::int64_t to,
                              std::uint32_t gamePid)
{
    WindowEvidence ev;
    for (const SystemEvent* e : tl.overlapping(from, to)) {
        const double fullUs = tl.toMicroseconds(e->durationQpc());
        const double inWindowUs = clippedUs(tl, *e, from, to);

        switch (e->kind) {
        case EventKind::Dpc:
        case EventKind::Isr:
            ev.sumDpcIsrUs += inWindowUs;
            if (fullUs > ev.maxDpcIsrUs) {
                ev.maxDpcIsrUs = fullUs;
                ev.maxDpcIsrKind = e->kind;
                ev.maxDpcIsrModule = e->module;
            }
            break;
        case EventKind::HardFault:
            ++ev.allHardFaults;
            if (gamePid != 0 && e->processId == gamePid) {
                ++ev.gameHardFaults;
                ev.gameHardFaultUs += inWindowUs;
            }
            break;
        case EventKind::DiskIo:
            ev.maxAnyDiskUs = std::max(ev.maxAnyDiskUs, fullUs);
            if (gamePid != 0 && e->processId == gamePid)
                ev.maxGameDiskUs = std::max(ev.maxGameDiskUs, fullUs);
            break;
        }
    }
    return ev;
}

Attribution attribute(const Spike& spike, const FrameRecord& frame, const EventTimeline& tl,
                      const CorrelatorConfig& cfg)
{
    const auto traceStart = tl.traceStart();
    if (!frame.qpcStart || !frame.qpcEnd || !traceStart || *frame.qpcStart < *traceStart) {
        return make(Cause::NoTraceData, Side::Unknown, 0.0,
                frame.qpcStart ? "Trace not running" : "No QPC times (PresentMon --qpc_time)");
    }

    const auto from = *frame.qpcStart - tl.fromMilliseconds(cfg.preWindowMs);
    const auto to = *frame.qpcEnd;
    const auto ev = gatherEvidence(tl, from, to, frame.processId);
    const double excessUs = spike.excessMs() * 1000.0;

    // 1. Driver latency: DPC/ISR time explains a real share of the delay.
    //    A single long DPC is enough on its own only when neither CPU nor GPU
    //    was busy; in a CPU/GPU-bound frame it can't explain the whole spike.
    const bool cpuOrGpuBound = spike.bound == Bound::Cpu || spike.bound == Bound::Gpu;
    const bool strongSingle = ev.maxDpcIsrUs >= cfg.dpcStrongMs * 1000.0 && !cpuOrGpuBound;
    if (ev.maxDpcIsrUs >= cfg.dpcMinMs * 1000.0 &&
        (strongSingle || ev.sumDpcIsrUs >= cfg.dpcShare * excessUs)) {
        const auto module = ev.maxDpcIsrModule.empty() ? std::string{"unknown driver"} : ev.maxDpcIsrModule;
        return make(Cause::DriverLatency, Side::System,
                confidenceFor(std::max(ev.sumDpcIsrUs, ev.maxDpcIsrUs), excessUs),
                std::format("{} {} in {} ({} total)", toString(ev.maxDpcIsrKind), ms(ev.maxDpcIsrUs),
                            module, ms(ev.sumDpcIsrUs)),
                module);
    }

    // 2. The game itself waited on hard page faults. Without RAM data (M3)
    //    this could be memory pressure or the game reading mapped files.
    if (ev.gameHardFaultUs >= cfg.hardFaultShare * excessUs) {
        return make(Cause::Paging, Side::Mixed, confidenceFor(ev.gameHardFaultUs, excessUs),
                std::format("{} hard faults in game, {}", ev.gameHardFaults, ms(ev.gameHardFaultUs)));
    }

    // 3. Slow storage during the frame.
    if (ev.maxGameDiskUs >= cfg.diskShare * excessUs) {
        return make(Cause::StorageWait, Side::Mixed, confidenceFor(ev.maxGameDiskUs, excessUs),
                std::format("Game disk I/O took {}", ms(ev.maxGameDiskUs)));
    }
    if (spike.bound == Bound::Stall && ev.maxAnyDiskUs >= cfg.diskShare * excessUs) {
        return make(Cause::StorageWait, Side::Mixed, 0.35,
                std::format("System disk I/O took {}", ms(ev.maxAnyDiskUs)));
    }

    // 4. No system cause big enough: judge by which side was busy.
    //    Mention a notable DPC anyway, it's still worth knowing about.
    const auto dpcNote = ev.maxDpcIsrUs >= cfg.dpcMinMs * 1000.0
                             ? std::format(" (also {} {} in {})", toString(ev.maxDpcIsrKind), ms(ev.maxDpcIsrUs),
                                           ev.maxDpcIsrModule.empty() ? "unknown driver" : ev.maxDpcIsrModule)
                             : std::string{};
    switch (spike.bound) {
    case Bound::Cpu:
        return make(Cause::GameCpu, Side::Game, dpcNote.empty() ? 0.6 : 0.5,
                    "CPU busy, no major system interference (shader compile? game logic?)" + dpcNote);
    case Bound::Gpu:
        return make(Cause::GameGpu, Side::Game, dpcNote.empty() ? 0.5 : 0.4,
                    "GPU busy, no major system interference" + dpcNote);
    case Bound::Stall:
        return make(Cause::UnexplainedWait, Side::Unknown, 0.3, "Nothing busy and no system event found");
    case Bound::Unknown:
        break;
    }
    return make(Cause::UnexplainedWait, Side::Unknown, 0.2, "No CPU/GPU busy data and no system event");
}

Diagnosis diagnose(std::span<const Spike> spikes)
{
    Diagnosis d;
    std::map<Cause, std::size_t> causes;
    std::map<std::string, std::size_t> modules;

    for (const auto& s : spikes) {
        if (!s.attribution || s.attribution->cause == Cause::NoTraceData)
            continue;
        const auto& a = *s.attribution;
        ++d.attributed;
        ++causes[a.cause];
        switch (a.side) {
        case Side::System:  ++d.system; break;
        case Side::Game:    ++d.game; break;
        case Side::Mixed:   ++d.mixed; break;
        case Side::Unknown: ++d.unknown; break;
        }
        if (a.cause == Cause::DriverLatency && !a.module.empty())
            ++modules[a.module];
    }

    if (d.attributed == 0) {
        d.verdict = "Not enough data yet";
        return d;
    }

    for (const auto& [cause, n] : causes) {
        if (n > d.topCauseCount) {
            d.topCause = cause;
            d.topCauseCount = n;
        }
    }
    std::size_t best = 0;
    for (const auto& [module, n] : modules) {
        if (n > best) {
            best = n;
            d.topModule = module;
        }
    }

    const auto pct = [&](std::size_t n) { return static_cast<int>(100.0 * static_cast<double>(n) / static_cast<double>(d.attributed) + 0.5); };
    const auto top = std::format("{}{}", toString(d.topCause),
                                 d.topCause == Cause::DriverLatency && !d.topModule.empty()
                                     ? std::format(" ({})", d.topModule) : std::string{});

    if (2 * d.system >= d.attributed)
        d.verdict = std::format("Mostly your system: {}% of spikes. Top cause: {}", pct(d.system), top);
    else if (2 * d.game >= d.attributed)
        d.verdict = std::format("Mostly the game: {}% of spikes. Top cause: {}", pct(d.game), top);
    else
        d.verdict = std::format("Mixed: system {}%, game {}%, unclear {}%. Top cause: {}",
                                pct(d.system), pct(d.game), pct(d.mixed + d.unknown), top);
    return d;
}

} // namespace framescope::core
