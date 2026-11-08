// framescope-latency [--seconds N] [--nt-kernel-logger]
//
// Idle/baseline system check, like LatencyMon: records DPC/ISR latency per
// driver plus hard faults and disk I/O for N seconds, then rates the system.
// Run it as administrator while the PC is idle, and again while gaming.

#include "etw/KernelTrace.h"
#include "win/SystemSampler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace framescope;

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI onCtrl(DWORD)
{
    g_stop = true;
    return TRUE;
}
#endif

template <typename... Args>
void out(std::format_string<Args...> fmt, Args&&... args)
{
    std::fputs(std::format(fmt, std::forward<Args>(args)...).c_str(), stdout);
    std::fputc('\n', stdout);
}

} // namespace

int main(int argc, char** argv)
{
    int seconds = 30;
    etw::KernelTraceOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--seconds" && i + 1 < argc)
            seconds = std::max(1, std::atoi(argv[++i]));
        else if (a == "--nt-kernel-logger")
            opts.forceNtKernelLogger = true;
        else {
            out("usage: framescope-latency [--seconds N] [--nt-kernel-logger]");
            return 2;
        }
    }

    if (!etw::KernelTrace::isSupported()) {
        out("Kernel tracing is only available on Windows.");
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(onCtrl, TRUE);
#endif

    const auto topo = win::queryCpuTopology();
    if (topo.ccdCount() > 0) {
        std::string l3;
        for (const auto kb : topo.l3KBOfCcd)
            l3 += std::format("{}{} MB", l3.empty() ? "" : " / ", kb / 1024);
        out("CPU: {} logical CPUs, {} L3 group(s): {}{}", topo.ccdOfCpu.size(), topo.ccdCount(), l3,
            topo.hasAsymmetricCache() ? std::format(", 3D V-Cache on CCD{}", topo.vcacheCcd) : std::string{});
    }
    opts.ccdOfCpu = topo.ccdOfCpu;

    win::SystemSampler sampler;
    sampler.start(500);

    etw::KernelTrace trace;
    if (!trace.start(opts)) {
        out("Could not start kernel trace: {}", trace.lastError());
        return 1;
    }
    out("Tracing for {} s using {} (Ctrl+C to stop early) ...", seconds, trace.sessionDescription());

    std::size_t hardFaults = 0, diskIos = 0, longEvents = 0, cpuBuckets = 0;
    for (int s = 0; s < seconds && !g_stop; ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (const auto& e : trace.drainEvents()) {
            switch (e.kind) {
            case core::EventKind::HardFault: ++hardFaults; break;
            case core::EventKind::DiskIo:    ++diskIos; break;
            default:                         ++longEvents; break;
            }
        }
        cpuBuckets += trace.drainCpuBuckets().size();
        std::fprintf(stdout, "\r  %3d s  %10llu DPC/ISR", s + 1,
                     static_cast<unsigned long long>(trace.dpcIsrCount()));
        std::fflush(stdout);
    }
    std::fputc('\n', stdout);

    const auto stats = trace.latencySnapshot();
    trace.stop();
    cpuBuckets += trace.drainCpuBuckets().size();
    const auto samples = sampler.drain();
    out("Sensors: {}", sampler.status());
    sampler.stop();
    if (!samples.empty()) {
        const auto& s = samples.back();
        std::string line = "Last sample:";
        if (s.availableRamMB && s.totalRamMB)
            line += std::format(" RAM {:.1f}/{:.1f} GB free", *s.availableRamMB / 1024, *s.totalRamMB / 1024);
        if (s.cpuPerfLimitPct)
            line += std::format(", CPU perf limit {:.0f}%", *s.cpuPerfLimitPct);
        if (s.gpu)
            line += std::format(", GPU {:.0f} MHz {:.0f} °C {:.0f} W reasons 0x{:x}", s.gpu->coreClockMHz,
                                s.gpu->temperatureC, s.gpu->powerW, s.gpu->reasons);
        out("{}", line);
    }
    out("CPU scheduling buckets: {} (expect ~100 per second)", cpuBuckets);

    if (trace.dpcIsrCount() == 0) {
        out("No DPC/ISR events received. The session started but delivered nothing;");
        out("try again with --nt-kernel-logger.");
        return 1;
    }

    const auto worst = stats.worst(15);
    out("\n{:<28} {:>9} {:>9} {:>10} {:>10} {:>7} {:>6}", "Driver", "DPCs", "ISRs", "Max DPC", "Max ISR", ">0.5ms", ">1ms");
    for (const auto& m : worst) {
        out("{:<28} {:>9} {:>9} {:>7.3f} ms {:>7.3f} ms {:>7} {:>6}", m.module.substr(0, 28), m.dpcCount,
            m.isrCount, m.maxDpcUs / 1000.0, m.maxIsrUs / 1000.0, m.over500us, m.over1ms);
    }
    out("\nLong DPC/ISR kept: {}   Hard faults: {}   Disk I/Os: {}", longEvents, hardFaults, diskIos);

    const double maxMs = worst.empty() ? 0.0 : worst.front().maxUs() / 1000.0;
    const auto& culprit = worst.empty() ? std::string{} : worst.front().module;
    if (maxMs < 0.5)
        out("Verdict: GOOD. Worst DPC/ISR {:.2f} ms, drivers won't cause stutter.", maxMs);
    else if (maxMs < 2.0)
        out("Verdict: OK. Worst {:.2f} ms ({}); can cause rare small hitches.", maxMs, culprit);
    else
        out("Verdict: PROBLEM. {} hit {:.2f} ms; that's long enough to cause visible stutter.", culprit, maxMs);
    return 0;
}
