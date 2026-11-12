#include "core/Correlator.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <numeric>

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
    case Cause::GpuThrottle:     return "GPU throttling";
    case Cause::CpuThrottle:     return "CPU throttling";
    case Cause::LowMemory:       return "Low memory";
    case Cause::VramFull:        return "VRAM full";
    case Cause::Paging:          return "Hard page faults";
    case Cause::BackgroundLoad:  return "Background load";
    case Cause::CcdScheduling:   return "CCD scheduling";
    case Cause::StorageWait:     return "Storage wait";
    case Cause::GameCpu:         return "Game CPU work";
    case Cause::GameGpu:         return "Game GPU work";
    case Cause::UnexplainedWait: return "Unexplained wait";
    case Cause::ShaderCompile:   return "Shader compilation";
    case Cause::NotInForeground: return "Game not in foreground";
    case Cause::Loading: return "Loading";
    }
    return "?";
}

std::string_view toString(Finding::Level l) noexcept
{
    switch (l) {
    case Finding::Level::Good:    return "Good";
    case Finding::Level::Info:    return "Info";
    case Finding::Level::Warning: return "Warning";
    case Finding::Level::Problem: return "Problem";
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

std::string gb(double mb)
{
    return std::format("{:.1f} GB", mb / 1024.0);
}

std::string gpuReasonText(std::uint64_t r)
{
    std::string out;
    const auto add = [&](std::uint64_t bit, std::string_view name) {
        if (r & bit) {
            if (!out.empty())
                out += ", ";
            out += name;
        }
    };
    add(gpu_reason::HwThermal, "HW thermal");
    add(gpu_reason::SwThermal, "SW thermal");
    add(gpu_reason::HwPowerBrake, "power brake");
    add(gpu_reason::HwSlowdown, "HW slowdown");
    add(gpu_reason::ClockDrop, "clock drop under full load");
    add(gpu_reason::HotspotHigh, "temperature near limit");
    return out;
}

std::string nameOf(const CorrelatorConfig& cfg, std::uint32_t pid)
{
    if (pid == 0)
        return "unknown process";
    if (pid == 4)
        return "Windows (System)"; // kernel threads; can't be opened by name
    if (cfg.processName) {
        auto name = cfg.processName(pid);
        if (!name.empty())
            return name;
    }
    return std::format("PID {}", pid);
}

bool ramLow(const WindowEvidence& ev, const CorrelatorConfig& cfg)
{
    if (!ev.minAvailableRamMB)
        return false;
    if (*ev.minAvailableRamMB < cfg.lowRamMB)
        return true;
    return ev.totalRamMB && *ev.minAvailableRamMB < cfg.lowRamShare * *ev.totalRamMB;
}

} // namespace

WindowEvidence gatherEvidence(const EventTimeline& tl, std::int64_t from, std::int64_t to,
                              std::uint32_t gamePid, const ShaderWindow& shader)
{
    WindowEvidence ev;
    ev.windowUs = tl.toMicroseconds(to - from);
    const auto directTo = std::max(shader.directTo, to);
    const auto nearFrom = shader.nearFrom != 0 ? std::min(shader.nearFrom, from) : from;
    const auto nearTo = std::max(shader.nearTo, directTo);

    for (const SystemEvent* e : tl.overlapping(nearFrom, nearTo)) {
        if (e->kind == EventKind::ShaderCacheWrite) {
            // Disk flushes come from the system's lazy writer; their timing says nothing.
            const bool byGame = e->module != "disk" && (e->processId == gamePid || e->processId == 0) && gamePid != 0;
            if (!byGame)
                continue;
            if (e->startQpc >= from && e->startQpc <= directTo) {
                ++ev.gameShaderWrites;
                ev.gameShaderBytes += e->bytes;
            } else {
                ++ev.nearShaderWrites;
            }
            const double gapMs = tl.toMicroseconds(e->startQpc - to) / 1000.0;
            if (!ev.nearestShaderGapMs || std::fabs(gapMs) < std::fabs(*ev.nearestShaderGapMs))
                ev.nearestShaderGapMs = gapMs;
            continue;
        }
        if (e->startQpc > to || e->endQpc < from)
            continue; // only shader writes use the wider window
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
        case EventKind::ShaderCacheWrite:
            break; // handled above
        }
    }

    for (const SystemSample* s : tl.samplesAround(from, to)) {
        ev.hasSamples = true;
        if (s->gameInForeground && !*s->gameInForeground)
            ev.gameInBackground = true;
        const auto minOpt = [](std::optional<double>& acc, const std::optional<double>& v) {
            if (v)
                acc = acc ? std::min(*acc, *v) : *v;
        };
        const auto maxOpt = [](std::optional<double>& acc, const std::optional<double>& v) {
            if (v)
                acc = acc ? std::max(*acc, *v) : *v;
        };
        minOpt(ev.minAvailableRamMB, s->availableRamMB);
        maxOpt(ev.totalRamMB, s->totalRamMB);
        maxOpt(ev.maxCpuPerfLimitPct, s->cpuPerfLimitPct);
        maxOpt(ev.maxGameVramMB, s->gameVramMB);
        maxOpt(ev.vramTotalMB, s->vramTotalMB);
        if (s->gpu) {
            ev.gpuReasons |= s->gpu->reasons;
            ev.gpuMaxTempC = std::max(ev.gpuMaxTempC, s->gpu->hotspotC.value_or(s->gpu->temperatureC));
            ev.gpuMinClockMHz = ev.gpuMinClockMHz == 0.0 ? s->gpu->coreClockMHz
                                                         : std::min(ev.gpuMinClockMHz, s->gpu->coreClockMHz);
        }
    }

    std::map<std::uint32_t, double> background;
    for (const CpuBucket* b : tl.bucketsIn(from, to)) {
        ev.hasCpuData = true;
        // Scale each bucket by how much of it falls inside the window.
        const auto overlap = std::min(b->endQpc, to) - std::max(b->startQpc, from);
        const double f = b->endQpc > b->startQpc
                             ? std::clamp(static_cast<double>(overlap) / static_cast<double>(b->endQpc - b->startQpc), 0.0, 1.0)
                             : 0.0;
        for (std::size_t i = 0; i < kMaxCcds; ++i)
            ev.gameUsByCcd[i] += f * b->gameUsByCcd[i];
        ev.gameCrossCcdMigrations += b->gameCrossCcdMigrations;
        ev.backgroundUs += f * b->backgroundUs;
        ev.shaderCompileUs += f * b->shaderCompileUs;
        if (b->topBackgroundPid != 0 && b->topBackgroundPid == gamePid) {
            // Before the trace knew the game's PID, its threads counted as "other".
            ev.backgroundUs -= f * b->topBackgroundUs;
        } else if (b->topBackgroundPid != 0) {
            background[b->topBackgroundPid] += f * b->topBackgroundUs;
        }
    }
    for (const auto& [pid, us] : background) {
        if (us > ev.topBackgroundUs) {
            ev.topBackgroundUs = us;
            ev.topBackgroundPid = pid;
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
    const auto ev = gatherEvidence(tl, from, to, frame.processId,
                                   {.directTo = to + tl.fromMilliseconds(cfg.shaderAfterMs),
                                    .nearFrom = *frame.qpcStart - tl.fromMilliseconds(cfg.shaderNearBeforeMs),
                                    .nearTo = to + tl.fromMilliseconds(cfg.shaderNearAfterMs)});
    const double excessUs = spike.excessMs() * 1000.0;
    const bool cpuOrGpuBound = spike.bound == Bound::Cpu || spike.bound == Bound::Gpu;
    const bool cpuSide = spike.bound == Bound::Cpu || spike.bound == Bound::Stall;

    // 0. The game wasn't in front: Windows deprioritises it and many games
    //    throttle themselves. That is alt-tabbing, not stutter.
    if (ev.gameInBackground) {
        return make(Cause::NotInForeground, Side::Unknown, 0.8,
                    "Another window was in front (alt-tab, or stopping the capture); not counted");
    }

    // 1. Driver latency: DPC/ISR time explains a real share of the delay.
    //    A single long DPC is enough on its own only when neither CPU nor GPU
    //    was busy; in a CPU/GPU-bound frame it can't explain the whole spike.
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

    // 2. The game compiled shaders: its threads wrote to a shader cache
    //    during (or right after) the frame. Classic first-run stutter.
    // Direct measurement: the game's threads were sampled inside a shader
    // compiler (driver or D3D) during the frame.
    if (spike.bound != Bound::Gpu && ev.shaderCompileUs >= cfg.compileMinMs * 1000.0 &&
        ev.shaderCompileUs >= cfg.compileShare * excessUs) {
        return make(Cause::ShaderCompile, Side::Game, confidenceFor(ev.shaderCompileUs, excessUs) >= 0.6 ? 0.9 : 0.75,
                    std::format("Shader compiler ran ~{} on the game's threads during this frame; should be gone on "
                                "the next run of this scene",
                                ms(ev.shaderCompileUs)));
    }
    if (ev.gameShaderWrites > 0 && spike.bound != Bound::Gpu) {
        return make(Cause::ShaderCompile, Side::Game, spike.bound == Bound::Cpu ? 0.8 : 0.65,
                    std::format("Game wrote {} shader cache entr{} ({:.0f} KB); should be gone on the next run of this scene",
                                ev.gameShaderWrites, ev.gameShaderWrites == 1 ? "y" : "ies",
                                static_cast<double>(ev.gameShaderBytes) / 1024.0));
    }
    if (ev.nearShaderWrites > 0 && ev.nearestShaderGapMs && spike.bound != Bound::Gpu) {
        const double gapS = *ev.nearestShaderGapMs / 1000.0;
        return make(Cause::ShaderCompile, Side::Game, spike.bound == Bound::Cpu ? 0.55 : 0.45,
                    std::format("Shader cache written {:.1f} s {} this frame (drivers write their cache in batches); "
                                "likely a shader compile",
                                std::fabs(gapS), gapS >= 0 ? "after" : "before"));
    }

    // 3. GPU clocks dropped hard (thermal, power brake, HW slowdown).
    if (ev.gpuReasons & gpu_reason::Harmful) {
        // Inferred clock drops (AMD) are weaker evidence than driver-reported reasons.
        const bool reported = (ev.gpuReasons & gpu_reason::Harmful & ~gpu_reason::ClockDrop) != 0;
        const double conf = (spike.bound == Bound::Gpu ? 0.75 : 0.5) - (reported ? 0.0 : 0.1);
        return make(Cause::GpuThrottle, Side::System, conf,
                    std::format("GPU {}: {:.0f} MHz, {:.0f} °C", gpuReasonText(ev.gpuReasons & ~gpu_reason::HotspotHigh),
                                ev.gpuMinClockMHz, ev.gpuMaxTempC));
    }

    // 3. Hard page faults in the game. RAM data decides who's to blame.
    if (ev.gameHardFaultUs >= cfg.hardFaultShare * excessUs && ev.gameHardFaults > 0) {
        const auto base = std::format("{} hard faults in game, {}", ev.gameHardFaults, ms(ev.gameHardFaultUs));
        const double conf = confidenceFor(ev.gameHardFaultUs, excessUs);
        if (ramLow(ev, cfg))
            return make(Cause::LowMemory, Side::System, conf,
                        base + std::format(", only {} RAM free", gb(*ev.minAvailableRamMB)));
        if (ev.minAvailableRamMB)
            return make(Cause::Paging, Side::Game, conf * 0.8,
                        base + std::format(" with {} RAM free (game loading data)", gb(*ev.minAvailableRamMB)));
        return make(Cause::Paging, Side::Mixed, conf, base);
    }

    // 4. VRAM at the limit: the driver moves resources over PCIe.
    if (ev.maxGameVramMB && ev.vramTotalMB && *ev.vramTotalMB > 0 &&
        *ev.maxGameVramMB >= cfg.vramFullShare * *ev.vramTotalMB && spike.bound != Bound::Cpu) {
        return make(Cause::VramFull, Side::Mixed, 0.6,
                    std::format("Game uses {} of {} VRAM, lower texture settings", gb(*ev.maxGameVramMB),
                                gb(*ev.vramTotalMB)));
    }

    // 5. Slow storage during the frame.
    if (ev.maxGameDiskUs >= cfg.diskShare * excessUs && ev.maxGameDiskUs > 0) {
        return make(Cause::StorageWait, Side::Mixed, confidenceFor(ev.maxGameDiskUs, excessUs),
                    std::format("Game disk I/O took {}", ms(ev.maxGameDiskUs)));
    }
    if (spike.bound == Bound::Stall && ev.maxAnyDiskUs >= cfg.diskShare * excessUs && ev.maxAnyDiskUs > 0) {
        return make(Cause::StorageWait, Side::Mixed, 0.35,
                    std::format("System disk I/O took {}", ms(ev.maxAnyDiskUs)));
    }

    // 6. CPU clocks capped.
    if (cpuSide && ev.maxCpuPerfLimitPct && *ev.maxCpuPerfLimitPct >= cfg.cpuPerfLimitPct) {
        return make(Cause::CpuThrottle, Side::System, 0.5,
                    std::format("CPU performance capped by {:.0f}% (power policy, power or thermal limit)", *ev.maxCpuPerfLimitPct));
    }

    // 7. Another process hogged the CPU.
    if (cpuSide && ev.hasCpuData && ev.windowUs > 0) {
        const double cores = ev.backgroundUs / ev.windowUs;
        if (cores >= cfg.backgroundCores && ev.topBackgroundUs >= cfg.backgroundTopShare * ev.backgroundUs) {
            return make(Cause::BackgroundLoad, Side::System, std::clamp(0.3 + 0.05 * cores, 0.3, 0.7),
                        std::format("{} used ~{:.1f} cores (all others ~{:.1f})", nameOf(cfg, ev.topBackgroundPid),
                                    ev.topBackgroundUs / ev.windowUs, cores));
        }
    }

    // 8. X3D: game threads off the V-Cache CCD or hopping between CCDs.
    const auto& topo = tl.topology();
    if (cpuSide && ev.hasCpuData && topo.hasAsymmetricCache()) {
        const auto vc = static_cast<std::size_t>(topo.vcacheCcd);
        const double total = std::accumulate(ev.gameUsByCcd.begin(), ev.gameUsByCcd.end(), 0.0);
        const double offShare = total > 0.0 ? (total - ev.gameUsByCcd[vc]) / total : 0.0;

        // The session's normal behaviour
        const auto& all = tl.gameUsByCcdTotal();
        const double sessionTotal = std::accumulate(all.begin(), all.end(), 0.0);
        const double sessionOff = sessionTotal > 0.0 ? (sessionTotal - all[vc]) / sessionTotal : 0.0;
        const double sessionRate = tl.bucketCoverageUs() > 0.0
                                       ? static_cast<double>(tl.gameCrossCcdMigrationsTotal()) / tl.bucketCoverageUs()
                                       : 0.0;
        const double windowRate = ev.windowUs > 0.0 ? ev.gameCrossCcdMigrations / ev.windowUs : 0.0;

        // Only a real shift of game work off the V-Cache CCD counts. Bursts of
        // migrations alone happen in every loading hitch (many threads wake up),
        // so they're a symptom, not a cause; they're only reported as detail.
        const bool offCcd = total > 0.0 && offShare >= cfg.ccdOffShare && offShare >= sessionOff + cfg.ccdOffShareAboveNormal;
        if (offCcd) {
            return make(Cause::CcdScheduling, Side::System, 0.45,
                        std::format("{:.0f}% of game CPU time off the V-Cache CCD (normally {:.0f}%), {} cross-CCD "
                                    "migrations ({:.1f}× the usual rate)",
                                    offShare * 100.0, sessionOff * 100.0, ev.gameCrossCcdMigrations,
                                    sessionRate > 0.0 ? windowRate / sessionRate : 0.0));
        }
    }

    // 9. No system cause big enough: judge by which side was busy.
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
        return make(Cause::UnexplainedWait, Side::Unknown, 0.3, "Nothing busy and no system event found" + dpcNote);
    case Bound::Unknown:
        break;
    }
    return make(Cause::UnexplainedWait, Side::Unknown, 0.2, "No CPU/GPU busy data and no system event" + dpcNote);
}

Diagnosis diagnose(std::span<const Spike> spikes)
{
    Diagnosis d;
    std::map<Cause, std::size_t> causes;
    std::map<std::string, std::size_t> modules;

    for (const auto& s : spikes) {
        if (!s.attribution || !countsForVerdict(s.attribution->cause))
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

    const auto pct = [&](std::size_t n) {
        return static_cast<int>(100.0 * static_cast<double>(n) / static_cast<double>(d.attributed) + 0.5);
    };
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

std::vector<Finding> sessionFindings(const EventTimeline& tl, const LatencyStats* latency,
                                     const CorrelatorConfig& cfg)
{
    using L = Finding::Level;
    std::vector<Finding> out;

    // ---- Driver latency
    if (latency && !latency->empty()) {
        const auto worst = latency->worst(1).front();
        const double maxMs = worst.maxUs() / 1000.0;
        if (maxMs < 0.5)
            out.push_back({L::Good, "Driver latency is low", std::format("Worst DPC/ISR {:.2f} ms ({})", maxMs, worst.module)});
        else if (maxMs < 2.0)
            out.push_back({L::Warning, "Some driver latency", std::format("{} reached {:.2f} ms, can cause rare small hitches", worst.module, maxMs)});
        else
            out.push_back({L::Problem, "High driver latency", std::format("{} reached {:.2f} ms ({}× over 1 ms)", worst.module, maxMs, worst.over1ms)});
    }

    // ---- X3D CCD placement
    const auto& topo = tl.topology();
    if (topo.hasAsymmetricCache()) {
        const auto& byCcd = tl.gameUsByCcdTotal();
        const double total = std::accumulate(byCcd.begin(), byCcd.end(), 0.0);
        if (total > 0.0) {
            const double onVcache = byCcd[static_cast<std::size_t>(topo.vcacheCcd)] / total;
            const double perSecond = tl.bucketCoverageUs() > 0.0
                                         ? static_cast<double>(tl.gameCrossCcdMigrationsTotal()) * 1e6 / tl.bucketCoverageUs()
                                         : 0.0;
            const auto detail = std::format("{:.0f}% of game CPU time on the V-Cache CCD (CCD{}, {} MB L3); game threads "
                                            "switch CCD ~{:.0f} times per second",
                                            onVcache * 100.0, topo.vcacheCcd,
                                            topo.l3KBOfCcd[static_cast<std::size_t>(topo.vcacheCcd)] / 1024, perSecond);
            if (onVcache >= 0.8)
                out.push_back({L::Good, "Game stays on the V-Cache CCD", detail});
            else if (onVcache >= 0.6)
                out.push_back({L::Warning, "Game partly runs on the non-V-Cache CCD", detail});
            else
                out.push_back({L::Problem, "Game runs on the wrong CCD",
                               detail + ". Check the AMD 3D V-Cache driver, Game Mode and that Game Bar sees the game"});
        }
    }

    // ---- Shader compilation during play
    // Writes seen at the file API (exact timing) and flushes to disk (delayed,
    // but also catch memory-mapped caches). Use whichever saw more.
    std::size_t apiWrites = 0, diskWrites = 0;
    std::uint64_t apiBytes = 0, diskBytes = 0;
    for (const auto& e : tl.events()) {
        if (e.kind != EventKind::ShaderCacheWrite)
            continue;
        if (e.module == "disk") {
            ++diskWrites;
            diskBytes += e.bytes;
        } else {
            ++apiWrites;
            apiBytes += e.bytes;
        }
    }
    const double compileS = tl.shaderCompileUsTotal() / 1e6;
    if (apiWrites + diskWrites > 0 || compileS >= 0.1) {
        const double mb = static_cast<double>(std::max(apiBytes, diskBytes)) / (1024.0 * 1024.0);
        std::string detail;
        if (compileS >= 0.1)
            detail = std::format("The shader compiler used ~{:.1f} s of the game's CPU time. ", compileS);
        if (apiWrites + diskWrites > 0)
            detail += std::format("{} shader cache writes, {} flushes to disk ({:.1f} MB). ", apiWrites, diskWrites, mb);
        detail += "Spikes from this fade once the cache is warm; compare with a second run of the same scene";
        out.push_back({L::Info, "Shaders compiled during play", detail});
    }

    // ---- Background CPU load (other processes, averaged over the session)
    if (tl.bucketCoverageUs() > 1e6) {
        double backgroundUs = tl.backgroundUsTotal();
        std::uint32_t topPid = 0;
        double topUs = 0.0;
        for (const auto& [pid, us] : tl.topBackgroundTotals()) {
            if (pid == cfg.gamePid && pid != 0) {
                backgroundUs -= us; // the game itself, from before its PID was known
                continue;
            }
            if (us > topUs) {
                topUs = us;
                topPid = pid;
            }
        }
        const double cores = std::max(0.0, backgroundUs) / tl.bucketCoverageUs();
        const auto detail = std::format("Other processes kept ~{:.1f} cores busy on average{}", cores,
                                        topPid != 0 ? std::format(", mostly {} (~{:.1f} cores)", nameOf(cfg, topPid),
                                                                  topUs / tl.bucketCoverageUs())
                                                    : std::string{});
        if (cores < 1.0)
            out.push_back({L::Good, "Low background CPU load", detail});
        else if (cores < 3.0)
            out.push_back({L::Warning, "Some background CPU load", detail});
        else
            out.push_back({L::Problem, "High background CPU load", detail});
    }

    // ---- Samples: GPU, RAM, VRAM, CPU limits
    const auto& samples = tl.samples();
    std::size_t gpuSamples = 0, gpuThrottled = 0, hotspotHigh = 0;
    std::uint64_t reasons = 0;
    double maxTemp = 0.0;
    std::optional<double> maxHotspot;
    bool inferred = false;
    std::optional<double> minRam, totalRam, maxVram, vramTotal, maxLimit;
    for (const auto& s : samples) {
        if (s.gpu) {
            ++gpuSamples;
            maxTemp = std::max(maxTemp, s.gpu->temperatureC);
            if (s.gpu->hotspotC)
                maxHotspot = std::max(maxHotspot.value_or(0.0), *s.gpu->hotspotC);
            if (s.gpu->reasons & gpu_reason::HotspotHigh)
                ++hotspotHigh;
            inferred = inferred || !s.gpu->driverReportsReasons;
            if (s.gpu->reasons & gpu_reason::Harmful) {
                ++gpuThrottled;
                reasons |= s.gpu->reasons;
            }
        }
        if (s.availableRamMB)
            minRam = minRam ? std::min(*minRam, *s.availableRamMB) : *s.availableRamMB;
        if (s.totalRamMB)
            totalRam = s.totalRamMB;
        if (s.gameVramMB)
            maxVram = maxVram ? std::max(*maxVram, *s.gameVramMB) : *s.gameVramMB;
        if (s.vramTotalMB)
            vramTotal = s.vramTotalMB;
        if (s.cpuPerfLimitPct)
            maxLimit = maxLimit ? std::max(*maxLimit, *s.cpuPerfLimitPct) : *s.cpuPerfLimitPct;
    }

    if (!samples.empty()) {
        const auto temps = std::format("max {:.0f} °C{}", maxTemp,
                                       maxHotspot ? std::format(", hotspot {:.0f} °C", *maxHotspot) : std::string{});
        if (gpuSamples == 0) {
            out.push_back({L::Info, "No GPU sensor data", "GPU sensors need NVML (NVIDIA) or ADLX (AMD) from the graphics driver"});
        } else if (gpuThrottled == 0) {
            out.push_back({L::Good, "No GPU throttling",
                           std::format("{}{}", inferred ? "No clock drops under full load, " : "", temps)});
        } else {
            const double share = static_cast<double>(gpuThrottled) / static_cast<double>(gpuSamples);
            const auto how = std::format("{} in {} of {} samples ({}), {}", gpuReasonText(reasons & ~gpu_reason::HotspotHigh),
                                         gpuThrottled, gpuSamples,
                                         share < 0.01 ? std::string{"< 1%"} : std::format("{:.0f}%", share * 100.0), temps);
            if (share < 0.01)
                out.push_back({L::Info, "Rare GPU clock drops", how + "; too rare to matter on their own"});
            else
                out.push_back({share > 0.05 ? L::Problem : L::Warning, "GPU throttling", how});
        }
        if (hotspotHigh > 0) {
            out.push_back({L::Warning, "GPU temperature near its limit",
                           std::format("{} of {} samples close to the point where the GPU slows itself down ({}); check "
                                       "airflow and fan curve",
                                       hotspotHigh, gpuSamples, temps)});
        }

        if (minRam) {
            const bool low = *minRam < cfg.lowRamMB || (totalRam && *minRam < cfg.lowRamShare * *totalRam);
            out.push_back({low ? L::Problem : L::Good, low ? "RAM nearly full" : "Enough free RAM",
                           std::format("At least {} of {} free", gb(*minRam), totalRam ? gb(*totalRam) : std::string{"?"})});
        }

        if (maxVram && vramTotal && *vramTotal > 0) {
            const double share = *maxVram / *vramTotal;
            const auto detail = std::format("Game peaked at {} of {} ({:.0f}%)", gb(*maxVram), gb(*vramTotal), share * 100.0);
            if (share >= cfg.vramFullShare)
                out.push_back({L::Problem, "VRAM full", detail + ", lower texture settings"});
            else if (share >= 0.85)
                out.push_back({L::Warning, "VRAM nearly full", detail});
            else
                out.push_back({L::Good, "Enough VRAM", detail});
        }

        if (maxLimit && *maxLimit >= cfg.cpuPerfLimitPct)
            out.push_back({L::Warning, "CPU clocks were limited", std::format("CPU performance capped by up to {:.0f}% (power policy, power or thermal limit)", *maxLimit)});
    }

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.level > b.level; });
    return out;
}

std::vector<Finding> leftOutFindings(std::span<const Spike> spikes)
{
    std::size_t loading = 0, notInFront = 0;
    double loadingS = 0.0;
    for (const auto& s : spikes) {
        if (!s.attribution)
            continue;
        if (s.attribution->cause == Cause::Loading) {
            ++loading;
            loadingS += s.frameTimeMs / 1000.0;
        } else if (s.attribution->cause == Cause::NotInForeground) {
            ++notInFront;
        }
    }
    if (loading + notInFront == 0)
        return {};
    std::string detail;
    if (loading > 0)
        detail = std::format("{} during loading (~{:.0f} s in which the game all but stopped presenting)", loading,
                             loadingS);
    if (notInFront > 0)
        detail += std::format("{}{} while the game wasn't in front or was being left or closed",
                              detail.empty() ? "" : "; ", notInFront);
    detail += ". These say nothing about stutter during play";
    return {Finding{Finding::Level::Info, std::format("{} spikes left out of the verdict", loading + notInFront),
                    std::move(detail)}};
}

std::vector<Finding> stutterPatternFindings(std::span<const FrameRecord> frames, std::span<const Spike> spikes,
                                            double binSeconds)
{
    using L = Finding::Level;
    std::vector<Finding> out;
    if (frames.empty() || spikes.size() < 10)
        return out;
    const double duration = frames.back().timeSeconds;
    const auto binCount = static_cast<std::size_t>(std::ceil(duration / binSeconds));
    if (duration < 180.0 || binCount < 6)
        return out; // too short to say anything about a pattern

    std::vector<std::size_t> bins(binCount, 0);
    std::size_t counted = 0;
    for (const auto& s : spikes) {
        if (s.attribution && !countsForVerdict(s.attribution->cause) && s.attribution->cause != Cause::NoTraceData)
            continue;
        ++counted;
        ++bins[std::min(binCount - 1, static_cast<std::size_t>(s.timeSeconds / binSeconds))];
    }
    if (counted < 10)
        return out;

    // Fewest bins that hold 60% of the spikes
    std::vector<std::size_t> order(binCount);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return bins[a] > bins[b]; });
    std::size_t covered = 0, used = 0;
    std::vector<std::size_t> busiest;
    while (used < binCount && 10 * covered < 6 * counted) {
        covered += bins[order[used]];
        busiest.push_back(order[used]);
        ++used;
    }
    const double binShare = static_cast<double>(used) / static_cast<double>(binCount);
    const double spikeShare = static_cast<double>(covered) / static_cast<double>(counted);

    const auto clock = [](double sec) {
        const auto t = static_cast<int>(sec + 0.5);
        return std::format("{}:{:02}", t / 60, t % 60);
    };

    if (binShare <= 0.25) {
        // Merge neighbouring bins into ranges for the text.
        std::ranges::sort(busiest);
        std::string ranges;
        for (std::size_t i = 0; i < busiest.size();) {
            std::size_t j = i;
            while (j + 1 < busiest.size() && busiest[j + 1] == busiest[j] + 1)
                ++j;
            if (!ranges.empty())
                ranges += ", ";
            ranges += clock(static_cast<double>(busiest[i]) * binSeconds) + "–" +
                      clock(std::min(duration, static_cast<double>(busiest[j] + 1) * binSeconds));
            i = j + 1;
        }
        out.push_back({L::Info, "Stutter comes in bursts",
                       std::format("{:.0f}% of spikes fall in {:.0f}% of the session ({}). Typical for loading, asset "
                                   "streaming or shader compilation when entering new areas, i.e. the game",
                                   spikeShare * 100.0, binShare * 100.0, ranges)});
    } else if (binShare >= 0.5) {
        out.push_back({L::Info, "Stutter is spread evenly",
                       std::format("Spikes are spread over the whole session ({:.1f} per minute). Points to something "
                                   "constant: a system issue, or ongoing game work",
                                   static_cast<double>(counted) * 60.0 / duration)});
    }
    return out;
}

namespace {

std::optional<Cause> causeOfFinding(std::string_view title)
{
    struct Entry {
        std::string_view title;
        Cause cause;
    };
    static constexpr Entry table[] = {
        {"Driver latency is low", Cause::DriverLatency},
        {"Some driver latency", Cause::DriverLatency},
        {"High driver latency", Cause::DriverLatency},
        {"Game stays on the V-Cache CCD", Cause::CcdScheduling},
        {"Game partly runs on the non-V-Cache CCD", Cause::CcdScheduling},
        {"Game runs on the wrong CCD", Cause::CcdScheduling},
        {"Shaders compiled during play", Cause::ShaderCompile},
        {"Low background CPU load", Cause::BackgroundLoad},
        {"Some background CPU load", Cause::BackgroundLoad},
        {"High background CPU load", Cause::BackgroundLoad},
        {"No GPU throttling", Cause::GpuThrottle},
        {"Rare GPU clock drops", Cause::GpuThrottle},
        {"GPU throttling", Cause::GpuThrottle},
        {"Enough free RAM", Cause::LowMemory},
        {"RAM nearly full", Cause::LowMemory},
        {"Enough VRAM", Cause::VramFull},
        {"VRAM nearly full", Cause::VramFull},
        {"VRAM full", Cause::VramFull},
        {"CPU clocks were limited", Cause::CpuThrottle},
        // Added by linkFindingsToSpikes itself
        {"Spikes waiting on storage", Cause::StorageWait},
        {"Game loading data from disk", Cause::Paging},
    };
    for (const auto& e : table) {
        if (e.title == title)
            return e.cause;
    }
    return std::nullopt;
}

constexpr std::string_view kGameWorkTitle = "The game's own work";

} // namespace

void linkFindingsToSpikes(std::vector<Finding>& findings, std::span<const Spike> spikes)
{
    using L = Finding::Level;
    std::map<Cause, std::size_t> counts;
    std::size_t total = 0;
    for (const auto& s : spikes) {
        if (s.attribution && countsForVerdict(s.attribution->cause)) {
            ++counts[s.attribution->cause];
            ++total;
        }
    }
    if (total == 0)
        return;

    // Findings this function adds are rebuilt every time (saved sessions contain them).
    std::erase_if(findings, [](const Finding& f) {
        return f.title == kGameWorkTitle || f.title == "Spikes waiting on storage" || f.title == "Game loading data from disk";
    });

    const auto count = [&](Cause c) {
        const auto it = counts.find(c);
        return it == counts.end() ? std::size_t{0} : it->second;
    };
    const std::size_t significant = std::max<std::size_t>(3, (total + 9) / 10); // >= 3 and >= 10%

    std::set<Cause> covered;
    for (auto& f : findings) {
        const auto cause = causeOfFinding(f.title);
        if (!cause)
            continue;
        covered.insert(*cause);
        const auto n = count(*cause);
        f.spikes = n;
        f.spikesTotal = total;
        if (f.level == L::Problem && n == 0)
            f.level = L::Warning;
        else if (f.level == L::Warning && n >= significant)
            f.level = L::Problem;
    }

    // Causes behind several spikes that no check covers yet.
    const auto add = [&](Cause c, L level, std::string title, std::string detail) {
        const auto n = count(c);
        if (n < 3 || covered.contains(c))
            return;
        findings.push_back({level, std::move(title), std::move(detail), n, total});
    };
    add(Cause::StorageWait, count(Cause::StorageWait) >= significant ? L::Warning : L::Info, "Spikes waiting on storage",
        "Frames waited on slow disk I/O; usually the game streaming data, sometimes a busy or slow drive");
    add(Cause::Paging, L::Info, "Game loading data from disk",
        "The game's threads waited on hard page faults with enough free RAM: it was reading data in");

    const auto cpu = count(Cause::GameCpu), gpu = count(Cause::GameGpu), shader = count(Cause::ShaderCompile);
    if (cpu + gpu + shader > 0) {
        findings.push_back({L::Info, std::string{kGameWorkTitle},
                            std::format("CPU-heavy frames {}, GPU-heavy frames {}, shader compilation {}; nothing on the "
                                        "system side explained these",
                                        cpu, gpu, shader),
                            cpu + gpu + shader, total});
    }

    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) { return a.level > b.level; });
}

std::string impactText(const Finding& f)
{
    if (!f.spikes || !f.spikesTotal)
        return {};
    if (*f.spikes == 0)
        return std::format("linked to 0 of {} spikes", *f.spikesTotal);
    return std::format("linked to {} of {} spikes ({:.0f}%)", *f.spikes, *f.spikesTotal,
                       100.0 * static_cast<double>(*f.spikes) / static_cast<double>(*f.spikesTotal));
}

} // namespace framescope::core
