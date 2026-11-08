#include "core/Correlator.h"

#include <algorithm>
#include <format>
#include <map>
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
    return out;
}

std::string nameOf(const CorrelatorConfig& cfg, std::uint32_t pid)
{
    if (pid == 0)
        return "unknown process";
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
                              std::uint32_t gamePid)
{
    WindowEvidence ev;
    ev.windowUs = tl.toMicroseconds(to - from);

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

    for (const SystemSample* s : tl.samplesAround(from, to)) {
        ev.hasSamples = true;
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
            ev.gpuMaxTempC = std::max(ev.gpuMaxTempC, s->gpu->temperatureC);
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
        if (b->topBackgroundPid != 0)
            background[b->topBackgroundPid] += f * b->topBackgroundUs;
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
    const auto ev = gatherEvidence(tl, from, to, frame.processId);
    const double excessUs = spike.excessMs() * 1000.0;
    const bool cpuOrGpuBound = spike.bound == Bound::Cpu || spike.bound == Bound::Gpu;
    const bool cpuSide = spike.bound == Bound::Cpu || spike.bound == Bound::Stall;

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

    // 2. GPU clocks dropped hard (thermal, power brake, HW slowdown).
    if (ev.gpuReasons & gpu_reason::Harmful) {
        return make(Cause::GpuThrottle, Side::System, spike.bound == Bound::Gpu ? 0.75 : 0.5,
                    std::format("GPU {}: {:.0f} MHz, {:.0f} °C", gpuReasonText(ev.gpuReasons),
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
                    std::format("CPU performance limited by {:.0f}%", *ev.maxCpuPerfLimitPct));
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
        const double total = std::accumulate(ev.gameUsByCcd.begin(), ev.gameUsByCcd.end(), 0.0);
        const double off = total - ev.gameUsByCcd[static_cast<std::size_t>(topo.vcacheCcd)];
        const double offShare = total > 0.0 ? off / total : 0.0;
        if ((total > 0.0 && offShare >= cfg.ccdOffShare) || ev.gameCrossCcdMigrations >= cfg.ccdMigrations) {
            return make(Cause::CcdScheduling, Side::System, 0.45,
                        std::format("{:.0f}% of game CPU time off the V-Cache CCD, {} cross-CCD migrations",
                                    offShare * 100.0, ev.gameCrossCcdMigrations));
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
            const auto detail = std::format("{:.0f}% of game CPU time on the V-Cache CCD (CCD{}, {} MB L3), {} cross-CCD migrations",
                                            onVcache * 100.0, topo.vcacheCcd,
                                            topo.l3KBOfCcd[static_cast<std::size_t>(topo.vcacheCcd)] / 1024,
                                            tl.gameCrossCcdMigrationsTotal());
            if (onVcache >= 0.8)
                out.push_back({L::Good, "Game stays on the V-Cache CCD", detail});
            else if (onVcache >= 0.6)
                out.push_back({L::Warning, "Game partly runs on the non-V-Cache CCD", detail});
            else
                out.push_back({L::Problem, "Game runs on the wrong CCD",
                               detail + ". Check the AMD 3D V-Cache driver, Game Mode and that Game Bar sees the game"});
        }
    }

    // ---- Background CPU load (other processes, averaged over the session)
    if (tl.bucketCoverageUs() > 1e6) {
        const double cores = tl.backgroundUsTotal() / tl.bucketCoverageUs();
        std::uint32_t topPid = 0;
        double topUs = 0.0;
        for (const auto& [pid, us] : tl.topBackgroundTotals()) {
            if (us > topUs) {
                topUs = us;
                topPid = pid;
            }
        }
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
    std::size_t gpuSamples = 0, gpuThrottled = 0;
    std::uint64_t reasons = 0;
    double maxTemp = 0.0;
    std::optional<double> minRam, totalRam, maxVram, vramTotal, maxLimit;
    for (const auto& s : samples) {
        if (s.gpu) {
            ++gpuSamples;
            maxTemp = std::max(maxTemp, s.gpu->temperatureC);
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
        if (gpuSamples == 0) {
            out.push_back({L::Info, "No GPU sensor data", "Clock/thermal checks need an NVIDIA GPU for now (AMD via ADLX is planned)"});
        } else if (gpuThrottled == 0) {
            out.push_back({L::Good, "No GPU throttling", std::format("Max GPU temperature {:.0f} °C", maxTemp)});
        } else {
            const double share = static_cast<double>(gpuThrottled) / static_cast<double>(gpuSamples);
            out.push_back({share > 0.05 ? L::Problem : L::Warning, "GPU throttling",
                           std::format("{} in {:.0f}% of samples, max {:.0f} °C", gpuReasonText(reasons), share * 100.0, maxTemp)});
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
            out.push_back({L::Warning, "CPU clocks were limited", std::format("Performance limit up to {:.0f}%", *maxLimit)});
    }

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.level > b.level; });
    return out;
}

} // namespace framescope::core
