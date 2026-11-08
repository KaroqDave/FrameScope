// Minimal dependency-free tests for the core library. Run via `ctest`.

#include "core/CaptureSession.h"
#include "core/Correlator.h"
#include "core/EventTimeline.h"
#include "core/LatencyStats.h"
#include "core/PresentMonCsvParser.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace framescope::core;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                           \
    } while (0)

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

FrameRecord frame(double ms, std::optional<double> cpu = {}, std::optional<double> gpu = {})
{
    FrameRecord f;
    f.frameTimeMs = ms;
    f.cpuBusyMs = cpu;
    f.gpuBusyMs = gpu;
    return f;
}

// ---------------------------------------------------------------------------
void parserV2()
{
    PresentMonCsvParser p;
    CHECK(!p.feedLine("PresentMon 2.3.0 starting...")); // status text before header
    CHECK(!p.hasHeader());

    CHECK(!p.feedLine("Application,ProcessID,SwapChainAddress,PresentRuntime,FrameType,"
                      "CPUStartTime,FrameTime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy\r"));
    CHECK(p.hasHeader());

    auto a = p.feedLine("cod.exe,1234,0x1A,DXGI,Application,1.0,16.5,10.2,6.3,1.0,14.0,13.9\r");
    CHECK(a.has_value());
    if (a) {
        CHECK(near(a->frameTimeMs, 16.5));
        CHECK(near(*a->cpuBusyMs, 10.2));
        CHECK(near(*a->gpuBusyMs, 13.9));
        CHECK(a->processId == 1234);
        CHECK(a->application == "cod.exe");
        CHECK(near(a->timeSeconds, 0.0165));
    }

    // NA values become nullopt, frame still counts.
    auto b = p.feedLine("cod.exe,1234,0x1A,DXGI,Application,1.1,20.0,NA,NA,NA,NA,NA");
    CHECK(b.has_value());
    if (b) {
        CHECK(!b->cpuBusyMs.has_value());
        CHECK(near(b->timeSeconds, 0.0365));
    }

    // Generated frames and other swap chains are skipped.
    CHECK(!p.feedLine("cod.exe,1234,0x1A,DXGI,AMD_AFMF,1.2,8.0,1,1,1,1,1"));
    CHECK(!p.feedLine("cod.exe,1234,0x2B,DXGI,Application,1.2,8.0,1,1,1,1,1"));
    // Truncated / garbage rows are skipped.
    CHECK(!p.feedLine("cod.exe,1234"));
    CHECK(!p.feedLine("cod.exe,1234,0x1A,DXGI,Application,1.3,NA,1,1,1,1,1"));
    CHECK(p.skippedRows() == 4);
    CHECK(p.lockedSwapChain() == "1234:0x1A");
}

void parserV1()
{
    PresentMonCsvParser p;
    p.feedLine("Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,"
               "AllowsTearing,PresentMode,TimeInSeconds,MsInPresentAPI,MsBetweenPresents,"
               "MsCPUBusy,MsGPUBusy");
    auto f = p.feedLine("\"game.exe\",42,0x0000ABCD,DXGI,0,0,1,Hardware: Independent Flip,"
                        "3.2,0.1,6.9,5.0,6.5");
    CHECK(f.has_value());
    if (f) {
        CHECK(f->application == "game.exe");
        CHECK(near(f->frameTimeMs, 6.9));
        CHECK(near(*f->cpuBusyMs, 5.0));
        CHECK(near(*f->gpuBusyMs, 6.5));
    }
}

void parserRejectsNonPresentMon()
{
    PresentMonCsvParser p;
    p.feedLine("a,b,c");
    CHECK(!p.hasHeader());
    CHECK(!p.feedLine("1,2,3"));
}

void loadFile()
{
    const auto path = std::filesystem::temp_directory_path() / "framescope_test.csv";
    {
        std::ofstream out(path);
        out << "Application,ProcessID,SwapChainAddress,FrameTime,CPUBusy,GPUBusy\n";
        for (int i = 0; i < 10; ++i)
            out << "x.exe,1,0x1," << 10.0 << ",5,9\n";
    }
    const auto frames = loadPresentMonCsv(path);
    CHECK(frames.size() == 10);
    if (frames.size() == 10)
        CHECK(near(frames.back().timeSeconds, 0.1));
    std::filesystem::remove(path);

    bool threw = false;
    try {
        loadPresentMonCsv(path); // removed above
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
void detectorFindsSingleSpike()
{
    SpikeDetector d;
    std::vector<Spike> spikes;
    for (int i = 0; i < 200; ++i) {
        const double ms = (i == 150) ? 60.0 : 16.67;
        if (auto s = d.push(frame(ms)))
            spikes.push_back(*s);
    }
    CHECK(spikes.size() == 1);
    if (spikes.size() == 1) {
        CHECK(spikes[0].frameIndex == 150);
        CHECK(near(spikes[0].medianMs, 16.67));
        CHECK(near(spikes[0].excessMs(), 60.0 - 16.67));
    }
}

void detectorRespectsWarmup()
{
    SpikeDetector d;
    int count = 0;
    for (int i = 0; i < 29; ++i)
        count += d.push(frame(i == 10 ? 100.0 : 16.67)).has_value();
    CHECK(count == 0);
}

void detectorIgnoresSmallVariance()
{
    SpikeDetector d;
    int count = 0;
    for (int i = 0; i < 200; ++i) {
        // 25 ms at a 16.7 ms median: only 1.5x -> not a spike.
        count += d.push(frame(i % 20 == 0 ? 25.0 : 16.67)).has_value();
    }
    CHECK(count == 0);

    // High fps: 2x ratio met but only 5 ms excess -> not a spike.
    SpikeDetector hi;
    count = 0;
    for (int i = 0; i < 200; ++i)
        count += hi.push(frame(i == 100 ? 10.0 : 4.17)).has_value();
    CHECK(count == 0);
}

void detectorAdaptsToNewBaseline()
{
    // Dropping from 120 to 40 fps: only the first frames after the change
    // may be flagged, then the median catches up.
    SpikeDetector d;
    int late = 0;
    for (int i = 0; i < 600; ++i) {
        const bool spike = d.push(frame(i < 200 ? 8.33 : 25.0)).has_value();
        if (spike && i > 270)
            ++late;
    }
    CHECK(late == 0);
}

void classification()
{
    CHECK(SpikeDetector::classify(frame(50.0)) == Bound::Unknown);
    CHECK(SpikeDetector::classify(frame(50.0, 45.0, 10.0)) == Bound::Cpu);
    CHECK(SpikeDetector::classify(frame(50.0, 12.0, 44.0)) == Bound::Gpu);
    CHECK(SpikeDetector::classify(frame(50.0, 8.0, 7.0)) == Bound::Stall);
}

void medianEven()
{
    SpikeDetector d({.windowFrames = 4, .warmupFrames = 1});
    for (double v : {1.0, 2.0, 3.0, 10.0})
        d.push(frame(v));
    CHECK(near(d.currentMedian(), 2.5));
    d.push(frame(4.0)); // ring overwrites 1.0 -> {4,2,3,10}
    CHECK(near(d.currentMedian(), 3.5));
}

// ---------------------------------------------------------------------------
void stats()
{
    std::vector<FrameRecord> frames(1000, frame(16.0));
    for (int i = 0; i < 11; ++i)
        frames[i * 90].frameTimeMs = 50.0; // just over 1% slow frames

    const auto s = computeStats(frames, 11);
    CHECK(s.frames == 1000);
    CHECK(near(s.durationSeconds, (989 * 16.0 + 11 * 50.0) / 1000.0));
    CHECK(near(s.worstFrameMs, 50.0));
    CHECK(near(s.onePercentLowFps, 20.0)); // p99 lands on a 50 ms frame
    CHECK(s.spikesPerMinute > 0.0);

    const auto empty = computeStats({}, 0);
    CHECK(empty.frames == 0 && empty.averageFps == 0.0);
}

void session()
{
    CaptureSession cs;
    for (int i = 0; i < 100; ++i) {
        auto f = frame(i == 60 ? 70.0 : 16.0, 4.0, 4.0);
        f.timeSeconds = i * 0.016;
        cs.append(f);
    }
    CHECK(cs.frames().size() == 100);
    CHECK(cs.spikes().size() == 1);
    if (!cs.spikes().empty())
        CHECK(cs.spikes()[0].bound == Bound::Stall);
    cs.clear();
    CHECK(cs.frames().empty() && cs.spikes().empty());
}

// ---------------------------------------------------------------------------
// M2: QPC times, event timeline, correlation
// ---------------------------------------------------------------------------
constexpr std::int64_t kFreq = 10'000'000; // 10 MHz -> 10'000 ticks per ms
constexpr std::int64_t ms2t(double ms) { return static_cast<std::int64_t>(ms * 10'000.0); }

void parserQpcColumns()
{
    {   // PresentMon 2.x --qpc_time: CPUStartQPC = frame start
        PresentMonCsvParser p;
        p.feedLine("Application,ProcessID,SwapChainAddress,CPUStartQPC,FrameTime,CPUBusy,GPUBusy");
        auto f = p.feedLine("g.exe,7,0x1,123450000000,10.0,5,9");
        CHECK(p.hasQpcTimes());
        CHECK(f && f->qpcStart && *f->qpcStart == 123450000000);
        CHECK(f && f->qpcEnd && *f->qpcEnd == 123450000000 + ms2t(10.0));
    }
    {   // 1.x: QPCTime = present time = frame end
        PresentMonCsvParser p;
        p.feedLine("Application,ProcessID,SwapChainAddress,QPCTime,MsBetweenPresents");
        auto f = p.feedLine("g.exe,7,0x1,500000000,20.0");
        CHECK(f && f->qpcEnd && *f->qpcEnd == 500000000);
        CHECK(f && f->qpcStart && *f->qpcStart == 500000000 - ms2t(20.0));
    }
    {   // CPUStartTime in seconds: no QPC
        PresentMonCsvParser p;
        p.feedLine("Application,ProcessID,SwapChainAddress,CPUStartTime,FrameTime");
        auto f = p.feedLine("g.exe,7,0x1,12.345,16.0");
        CHECK(f && !f->qpcStart);
        // ... but raw ticks under the same name are recognised.
        auto g = p.feedLine("g.exe,7,0x1,98765432100,16.0");
        CHECK(g && g->qpcStart && *g->qpcStart == 98765432100);
    }
    {   // custom frequency survives reset()
        PresentMonCsvParser p;
        p.setQpcFrequency(1'000'000);
        p.reset();
        p.feedLine("Application,ProcessID,SwapChainAddress,CPUStartQPC,FrameTime");
        auto f = p.feedLine("g.exe,7,0x1,1000000,10.0");
        CHECK(f && *f->qpcEnd == 1000000 + 10000);
    }
}

SystemEvent ev(EventKind k, double startMs, double durMs, std::uint32_t pid = 0, std::string module = {})
{
    return SystemEvent{.kind = k, .startQpc = ms2t(startMs), .endQpc = ms2t(startMs + durMs),
                       .processId = pid, .bytes = 0, .module = std::move(module)};
}

void timelineOverlap()
{
    EventTimeline tl(kFreq);
    // Out-of-order batches, like per-CPU ETW buffers.
    std::vector<SystemEvent> a{ev(EventKind::Dpc, 100, 1), ev(EventKind::Dpc, 300, 1)};
    std::vector<SystemEvent> b{ev(EventKind::DiskIo, 50, 200), ev(EventKind::Isr, 200, 0.1)};
    tl.append(a);
    tl.append(b);
    CHECK(tl.size() == 4);
    CHECK(std::is_sorted(tl.events().begin(), tl.events().end(),
                         [](const auto& x, const auto& y) { return x.startQpc < y.startQpc; }));
    CHECK(tl.coveredUntil() == ms2t(301));

    // Window 190..210 ms: the long disk I/O (50..250) and the ISR at 200.
    const auto hits = tl.overlapping(ms2t(190), ms2t(210));
    CHECK(hits.size() == 2);
    CHECK(tl.overlapping(ms2t(260), ms2t(290)).empty());
}

struct Fixture {
    EventTimeline tl{kFreq};
    FrameRecord frame;
    Spike spike;

    Fixture(Bound bound = Bound::Stall)
    {
        tl.setTraceStart(0);
        frame.processId = 42;
        frame.frameTimeMs = 40.0;
        frame.qpcStart = ms2t(1000);
        frame.qpcEnd = ms2t(1040);
        spike.frameTimeMs = 40.0;
        spike.medianMs = 7.0; // excess 33 ms
        spike.bound = bound;
    }
    void add(SystemEvent e) { tl.append(std::span{&e, 1}); }
};

void correlateDriverLatency()
{
    Fixture f;
    f.add(ev(EventKind::Dpc, 1010, 3.0, 0, "nvlddmkm.sys"));
    f.add(ev(EventKind::Dpc, 1020, 0.1, 0, "ndis.sys"));
    const auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause == Cause::DriverLatency);
    CHECK(a.side == Side::System);
    CHECK(a.module == "nvlddmkm.sys");
    CHECK(a.detail.find("3.0 ms") != std::string::npos);

    // CPU-bound 33 ms excess: a single 3 ms DPC can't explain it -> game, with a note.
    Fixture cpu(Bound::Cpu);
    cpu.add(ev(EventKind::Dpc, 1010, 3.0, 0, "nvlddmkm.sys"));
    auto c = attribute(cpu.spike, cpu.frame, cpu.tl);
    CHECK(c.cause == Cause::GameCpu);
    CHECK(c.detail.find("nvlddmkm.sys") != std::string::npos);
    // ... but a DPC storm covering a real share of it is a system cause.
    for (int i = 0; i < 4; ++i)
        cpu.add(ev(EventKind::Dpc, 1015 + i * 3, 1.5, 0, "ndis.sys"));
    CHECK(attribute(cpu.spike, cpu.frame, cpu.tl).cause == Cause::DriverLatency);

    // Short DPCs are noise, not a cause.
    Fixture g(Bound::Cpu);
    g.add(ev(EventKind::Dpc, 1010, 0.2, 0, "ndis.sys"));
    CHECK(attribute(g.spike, g.frame, g.tl).cause == Cause::GameCpu);
}

void correlatePagingAndDisk()
{
    Fixture f;
    for (int i = 0; i < 5; ++i)
        f.add(ev(EventKind::HardFault, 1005 + i * 3, 2.0, 42)); // 10 ms of game faults
    f.add(ev(EventKind::HardFault, 1005, 20.0, 99));            // other process: ignored
    auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause == Cause::Paging);
    CHECK(a.detail.starts_with("5 hard faults"));

    Fixture d;
    d.add(ev(EventKind::DiskIo, 990, 25.0, 42));
    CHECK(attribute(d.spike, d.frame, d.tl).cause == Cause::StorageWait);

    // Other process's slow I/O only counts for stalls, with low confidence.
    Fixture o;
    o.add(ev(EventKind::DiskIo, 990, 25.0, 4));
    a = attribute(o.spike, o.frame, o.tl);
    CHECK(a.cause == Cause::StorageWait && a.confidence < 0.5);
    Fixture oc(Bound::Cpu);
    oc.add(ev(EventKind::DiskIo, 990, 25.0, 4));
    CHECK(attribute(oc.spike, oc.frame, oc.tl).cause == Cause::GameCpu);
}

void correlateNoData()
{
    Fixture f;
    f.frame.qpcStart.reset();
    f.frame.qpcEnd.reset();
    CHECK(attribute(f.spike, f.frame, f.tl).cause == Cause::NoTraceData);

    Fixture late;
    late.tl.setTraceStart(ms2t(2000)); // trace started after the frame
    CHECK(attribute(late.spike, late.frame, late.tl).cause == Cause::NoTraceData);

    Fixture clean(Bound::Gpu);
    CHECK(attribute(clean.spike, clean.frame, clean.tl).cause == Cause::GameGpu);
    Fixture stall(Bound::Stall);
    CHECK(attribute(stall.spike, stall.frame, stall.tl).cause == Cause::UnexplainedWait);
}

void sessionAttributesAfterSettle()
{
    CaptureSession cs;
    EventTimeline tl(kFreq);
    for (int i = 0; i < 100; ++i) {
        auto f = frame(i == 60 ? 50.0 : 10.0, 2.0, 2.0); // stall spike
        f.processId = 42;
        f.qpcStart = ms2t(i * 10.0);
        f.qpcEnd = ms2t(i * 10.0 + f.frameTimeMs);
        cs.append(f);
    }
    CHECK(cs.spikes().size() == 1);

    CHECK(cs.attributePending(tl) == 0); // no trace yet
    tl.setTraceStart(0);
    auto dpc = ev(EventKind::Dpc, 610, 4.0, 0, "rtwlane.sys");
    tl.append(std::span{&dpc, 1});
    CHECK(cs.attributePending(tl) == 0); // not settled: coverage only ~614 ms

    tl.noteCoverage(ms2t(3000));
    CHECK(cs.attributePending(tl) == 1);
    CHECK(cs.spikes()[0].attribution->cause == Cause::DriverLatency);
    CHECK(cs.attributePending(tl) == 0); // idempotent

    const auto d = cs.diagnosis();
    CHECK(d.attributed == 1 && d.system == 1);
    CHECK(d.topModule == "rtwlane.sys");
    CHECK(d.verdict.starts_with("Mostly your system"));
}

void diagnosisMix()
{
    std::vector<Spike> spikes(4);
    spikes[0].attribution = Attribution{Cause::GameCpu, Side::Game, 0.6, "", ""};
    spikes[1].attribution = Attribution{Cause::GameCpu, Side::Game, 0.6, "", ""};
    spikes[2].attribution = Attribution{Cause::GameGpu, Side::Game, 0.5, "", ""};
    spikes[3].attribution = Attribution{Cause::NoTraceData, Side::Unknown, 0, "", ""};
    auto d = diagnose(spikes);
    CHECK(d.attributed == 3 && d.game == 3);
    CHECK(d.topCause == Cause::GameCpu);
    CHECK(d.verdict.starts_with("Mostly the game: 100%"));

    CHECK(diagnose(std::vector<Spike>(2)).verdict == "Not enough data yet");
}

void latencyStats()
{
    LatencyStats s;
    s.add(EventKind::Dpc, "a.sys", 100);
    s.add(EventKind::Dpc, "a.sys", 1200);
    s.add(EventKind::Isr, "b.sys", 600);
    const auto w = s.worst(5);
    CHECK(w.size() == 2);
    CHECK(w[0].module == "a.sys" && w[0].dpcCount == 2 && w[0].over1ms == 1);
    CHECK(w[1].module == "b.sys" && w[1].isrCount == 1 && w[1].over500us == 1);
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"parserV2", parserV2},
        {"parserV1", parserV1},
        {"parserRejectsNonPresentMon", parserRejectsNonPresentMon},
        {"loadFile", loadFile},
        {"detectorFindsSingleSpike", detectorFindsSingleSpike},
        {"detectorRespectsWarmup", detectorRespectsWarmup},
        {"detectorIgnoresSmallVariance", detectorIgnoresSmallVariance},
        {"detectorAdaptsToNewBaseline", detectorAdaptsToNewBaseline},
        {"classification", classification},
        {"medianEven", medianEven},
        {"stats", stats},
        {"session", session},
        {"parserQpcColumns", parserQpcColumns},
        {"timelineOverlap", timelineOverlap},
        {"correlateDriverLatency", correlateDriverLatency},
        {"correlatePagingAndDisk", correlatePagingAndDisk},
        {"correlateNoData", correlateNoData},
        {"sessionAttributesAfterSettle", sessionAttributesAfterSettle},
        {"diagnosisMix", diagnosisMix},
        {"latencyStats", latencyStats},
    };

    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
