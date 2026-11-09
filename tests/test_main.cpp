// Minimal dependency-free tests for the core library. Run via `ctest`.

#include "core/CaptureSession.h"
#include "core/Comparison.h"
#include "core/Report.h"
#include "core/SessionFile.h"
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

// ---------------------------------------------------------------------------
// M3: samples, CPU buckets, topology, new rules, findings
// ---------------------------------------------------------------------------
GpuSample gpu(double clockMHz, double tempC, std::uint64_t reasons)
{
    GpuSample g;
    g.coreClockMHz = clockMHz;
    g.temperatureC = tempC;
    g.reasons = reasons;
    return g;
}

SystemSample sample(double atMs)
{
    SystemSample s;
    s.qpc = ms2t(atMs);
    return s;
}

void timelineSamplesAndBuckets()
{
    EventTimeline tl(kFreq);
    std::vector<SystemSample> ss{sample(0), sample(250), sample(500), sample(750)};
    tl.appendSamples(ss);
    auto around = tl.samplesAround(ms2t(300), ms2t(340));
    CHECK(around.size() == 2); // 250 before, 500 after
    if (around.size() == 2)
        CHECK(around[0]->qpc == ms2t(250) && around[1]->qpc == ms2t(500));

    std::vector<CpuBucket> bs;
    for (int i = 0; i < 10; ++i) {
        CpuBucket b;
        b.startQpc = ms2t(i * 10.0);
        b.endQpc = ms2t(i * 10.0 + 10.0);
        b.gameUsByCcd[0] = 5000;
        b.gameUsByCcd[1] = 1000;
        b.gameCrossCcdMigrations = 1;
        bs.push_back(b);
    }
    tl.appendCpuBuckets(bs);
    CHECK(tl.bucketsIn(ms2t(25), ms2t(45)).size() == 3); // buckets 20, 30, 40
    CHECK(near(tl.gameUsByCcdTotal()[0], 50000.0));
    CHECK(tl.gameCrossCcdMigrationsTotal() == 10);
    tl.pruneBucketsBefore(ms2t(50));
    CHECK(tl.bucketsIn(0, ms2t(100)).size() == 6); // bucket ending exactly at 50 ms stays
    CHECK(near(tl.gameUsByCcdTotal()[0], 50000.0)); // totals survive pruning
}

void topology()
{
    auto t = makeTopology({0, 0, 1, 1}, {98304, 32768}); // 9950X3D-style
    CHECK(t.hasAsymmetricCache() && t.vcacheCcd == 0 && t.ccdCount() == 2);
    CHECK(!makeTopology({0, 0}, {98304}).hasAsymmetricCache());          // single CCD
    CHECK(!makeTopology({0, 1}, {32768, 32768}).hasAsymmetricCache());   // plain dual CCD
}

void correlateGpuThrottle()
{
    Fixture f(Bound::Gpu);
    auto s = sample(1010);
    s.gpu = gpu(1200, 88, gpu_reason::HwThermal | gpu_reason::SwPowerCap);
    f.tl.appendSamples(std::span{&s, 1});
    auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause == Cause::GpuThrottle && a.side == Side::System);
    CHECK(a.detail.find("HW thermal") != std::string::npos);

    // Power cap alone is normal boost behaviour, not a cause.
    Fixture g(Bound::Gpu);
    s.gpu->reasons = gpu_reason::SwPowerCap;
    g.tl.appendSamples(std::span{&s, 1});
    CHECK(attribute(g.spike, g.frame, g.tl).cause == Cause::GameGpu);
}

void correlateMemory()
{
    const auto faults = [](Fixture& f) {
        for (int i = 0; i < 5; ++i)
            f.add(ev(EventKind::HardFault, 1005 + i * 3, 2.0, 42));
    };
    Fixture low;
    faults(low);
    auto s = sample(1000);
    s.availableRamMB = 900;
    s.totalRamMB = 32768;
    low.tl.appendSamples(std::span{&s, 1});
    CHECK(attribute(low.spike, low.frame, low.tl).cause == Cause::LowMemory);

    Fixture plenty;
    faults(plenty);
    s.availableRamMB = 20000;
    plenty.tl.appendSamples(std::span{&s, 1});
    auto a = attribute(plenty.spike, plenty.frame, plenty.tl);
    CHECK(a.cause == Cause::Paging && a.side == Side::Game);

    Fixture vram(Bound::Gpu);
    auto v = sample(1000);
    v.gameVramMB = 31500;
    v.vramTotalMB = 32607;
    vram.tl.appendSamples(std::span{&v, 1});
    CHECK(attribute(vram.spike, vram.frame, vram.tl).cause == Cause::VramFull);
}

CpuBucket bucket(double startMs, float ccd0, float ccd1, float background, std::uint32_t topPid, float topUs,
                 std::uint16_t cross = 0)
{
    CpuBucket b;
    b.startQpc = ms2t(startMs);
    b.endQpc = ms2t(startMs + 10.0);
    b.gameUsByCcd[0] = ccd0;
    b.gameUsByCcd[1] = ccd1;
    b.backgroundUs = background;
    b.topBackgroundPid = topPid;
    b.topBackgroundUs = topUs;
    b.gameCrossCcdMigrations = cross;
    return b;
}

void correlateCpuScheduling()
{
    CorrelatorConfig cfg;
    cfg.processName = [](std::uint32_t pid) { return pid == 777 ? std::string{"MsMpEng.exe"} : std::string{}; };

    // Background: ~6 cores busy with other work, mostly one process.
    Fixture bg(Bound::Cpu);
    std::vector<CpuBucket> bs;
    for (double t = 990; t < 1040; t += 10)
        bs.push_back(bucket(t, 8000, 0, 60000, 777, 50000));
    bg.tl.appendCpuBuckets(bs);
    auto a = attribute(bg.spike, bg.frame, bg.tl, cfg);
    CHECK(a.cause == Cause::BackgroundLoad);
    CHECK(a.detail.starts_with("MsMpEng.exe"));

    // CCD: topology with V-Cache on CCD0, game mostly on CCD1.
    Fixture ccd(Bound::Cpu);
    ccd.tl.setTopology(makeTopology({0, 0, 1, 1}, {98304, 32768}));
    bs.clear();
    for (int i = 0; i < 1000; ++i) // normally the game stays on the V-Cache CCD
        bs.push_back(bucket(-10000.0 + i * 10.0, 8000, 200, 1000, 0, 0));
    ccd.tl.appendCpuBuckets(bs);
    bs.clear();
    for (double t = 990; t < 1040; t += 10)
        bs.push_back(bucket(t, 2000, 6000, 1000, 0, 0));
    ccd.tl.appendCpuBuckets(bs);
    a = attribute(ccd.spike, ccd.frame, ccd.tl, cfg);
    CHECK(a.cause == Cause::CcdScheduling && a.side == Side::System);

    // Same pattern without asymmetric cache: not a CCD problem.
    Fixture sym(Bound::Cpu);
    sym.tl.setTopology(makeTopology({0, 0, 1, 1}, {32768, 32768}));
    sym.tl.appendCpuBuckets(bs);
    CHECK(attribute(sym.spike, sym.frame, sym.tl, cfg).cause == Cause::GameCpu);

    // CPU perf limit
    Fixture lim(Bound::Cpu);
    auto s = sample(1000);
    s.cpuPerfLimitPct = 25;
    lim.tl.appendSamples(std::span{&s, 1});
    CHECK(attribute(lim.spike, lim.frame, lim.tl).cause == Cause::CpuThrottle);
}

void findings()
{
    EventTimeline tl(kFreq);
    tl.setTopology(makeTopology({0, 0, 1, 1}, {98304, 32768}));
    std::vector<CpuBucket> bs{bucket(0, 3000, 7000, 0, 0, 0, 4)};
    tl.appendCpuBuckets(bs);

    std::vector<SystemSample> ss;
    for (int i = 0; i < 20; ++i) {
        auto s = sample(i * 250.0);
        s.availableRamMB = 12000;
        s.totalRamMB = 65536;
        s.gameVramMB = 20000;
        s.vramTotalMB = 32607;
        s.gpu = gpu(2700, 70, i == 3 ? gpu_reason::HwThermal : gpu_reason::SwPowerCap);
        ss.push_back(s);
    }
    tl.appendSamples(ss);

    LatencyStats lat;
    lat.add(EventKind::Dpc, "nvlddmkm.sys", 2500);

    const auto f = sessionFindings(tl, &lat);
    const auto find = [&](std::string_view title) {
        return std::ranges::find_if(f, [&](const Finding& x) { return x.title == title; });
    };
    CHECK(find("Game runs on the wrong CCD") != f.end());   // 30% on V-Cache CCD
    CHECK(find("High driver latency") != f.end());
    CHECK(find("GPU throttling") != f.end());               // 1 of 20 samples = 5% -> Warning
    CHECK(find("GPU throttling")->detail.find("1 of 20 samples (5%)") != std::string::npos);
    if (auto it = find("GPU throttling"); it != f.end())
        CHECK(it->level == Finding::Level::Warning);
    CHECK(find("Enough free RAM") != f.end());
    CHECK(find("Enough VRAM") != f.end());
    CHECK(f.front().level == Finding::Level::Problem);      // sorted worst first
}

// ---------------------------------------------------------------------------
// M4: session files, run comparison, report, background finding
// ---------------------------------------------------------------------------
// A run of `seconds` at 7 ms frames with spikes (45 ms) at the given times.
std::pair<std::vector<FrameRecord>, std::vector<Spike>> makeRun(double seconds, const std::vector<double>& spikeTimes,
                                                                Bound bound = Bound::Cpu)
{
    CaptureSession cs;
    std::size_t next = 0;
    double t = 0.0;
    while (t < seconds) {
        double ms = 7.0;
        if (next < spikeTimes.size() && t >= spikeTimes[next]) {
            ms = 45.0;
            ++next;
        }
        auto f = bound == Bound::Cpu ? frame(ms, ms * 0.9, 3.0) : frame(ms, 3.0, ms * 0.9);
        t += ms / 1000.0;
        f.timeSeconds = t;
        cs.append(f);
    }
    return {cs.frames(), cs.spikes()};
}

void sessionFileRoundTrip()
{
    auto [frames, spikes] = makeRun(10, {2, 5, 8});
    CHECK(spikes.size() == 3);
    spikes[0].attribution = Attribution{Cause::DriverLatency, Side::System, 0.8,
                                        "DPC 2.0 ms in nvlddmkm.sys\twith tab", "nvlddmkm.sys"};
    SavedSession s;
    s.application = "cod.exe";
    s.createdAt = "2026-11-08T20:00:00";
    s.sensors = "PDH, NVML (RTX 5090)";
    s.frames = frames;
    s.spikes = spikes;
    s.findings = {{Finding::Level::Warning, "Some driver latency", "nvlddmkm.sys reached 1.2 ms"}};
    ModuleLatency m;
    m.module = "nvlddmkm.sys";
    m.dpcCount = 1000;
    m.maxDpcUs = 1234.5;
    m.over1ms = 2;
    s.drivers = {m};

    const auto path = std::filesystem::temp_directory_path() / "framescope_test.fscope";
    saveSession(path, s);
    CHECK(isSessionFile(path));
    const auto r = loadSession(path);
    std::filesystem::remove(path);

    CHECK(r.application == "cod.exe" && r.sensors == s.sensors && r.createdAt == s.createdAt);
    CHECK(r.frames.size() == frames.size());
    CHECK(near(r.frames.back().timeSeconds, frames.back().timeSeconds, 1e-9));
    CHECK(near(*r.frames[5].cpuBusyMs, *frames[5].cpuBusyMs, 1e-9));
    CHECK(r.spikes.size() == 3);
    CHECK(r.spikes[0].attribution && r.spikes[0].attribution->cause == Cause::DriverLatency);
    CHECK(r.spikes[0].attribution->detail == "DPC 2.0 ms in nvlddmkm.sys with tab"); // tab cleaned
    CHECK(r.spikes[0].attribution->module == "nvlddmkm.sys");
    CHECK(!r.spikes[1].attribution);
    CHECK(near(r.spikes[2].frameTimeMs, 45.0) && r.spikes[2].bound == Bound::Cpu);
    CHECK(r.findings.size() == 1 && r.findings[0].level == Finding::Level::Warning);
    CHECK(r.drivers.size() == 1 && near(r.drivers[0].maxDpcUs, 1234.5) && r.drivers[0].over1ms == 2);

    CaptureSession cs;
    cs.restore(r.frames, r.spikes);
    CHECK(cs.spikes().size() == 3 && cs.diagnosis().system == 1);

    // Not a session file
    const auto bad = std::filesystem::temp_directory_path() / "framescope_bad.fscope";
    { std::ofstream(bad) << "hello\n"; }
    CHECK(!isSessionFile(bad));
    bool threw = false;
    try { loadSession(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    std::filesystem::remove(bad);
}

void compareShaderCompile()
{
    // Run 1: lots of CPU spikes; run 2: almost none.
    std::vector<double> many;
    for (double t = 2; t < 58; t += 3)
        many.push_back(t);
    auto [f1, s1] = makeRun(60, many);
    auto [f2, s2] = makeRun(60, {20, 40});
    const auto c = compareRuns(f1, s1, f2, s2);
    CHECK(c.verdict.starts_with("Likely shader compilation"));
    CHECK(c.verdict.find("mostly CPU-bound") != std::string::npos);
    CHECK(c.spikeRateChange < -0.8);
}

void compareRepeatable()
{
    std::vector<double> times{5, 12, 20, 31, 44, 52};
    std::vector<double> shifted;
    for (double t : times)
        shifted.push_back(t + 1.3); // run 2 started 1.3 s later in the sequence
    auto [f1, s1] = makeRun(60, times);
    auto [f2, s2] = makeRun(62, shifted);
    const auto c = compareRuns(f1, s1, f2, s2);
    CHECK(c.verdict.starts_with("Stutter repeats at the same moments"));
    CHECK(c.matchedSpikes == 6);
    CHECK(near(c.offsetSeconds, -1.3, 0.15));
}

void compareRandom()
{
    auto [f1, s1] = makeRun(60, {5, 12, 20, 31, 44, 52});
    auto [f2, s2] = makeRun(60, {8.5, 16, 25, 37, 48, 57});
    const auto c = compareRuns(f1, s1, f2, s2, {.maxOffsetSeconds = 1.0});
    CHECK(c.verdict.starts_with("Similar amount of stutter, but at random moments"));

    auto [f3, s3] = makeRun(60, {});
    auto [f4, s4] = makeRun(60, {30});
    CHECK(compareRuns(f3, s3, f4, s4).verdict.starts_with("Both runs are smooth"));
}

void htmlReport()
{
    auto [frames, spikes] = makeRun(30, {5, 15, 25});
    spikes[0].attribution = Attribution{Cause::DriverLatency, Side::System, 0.8, "DPC <2 ms> & more", "a.sys"};
    spikes[1].attribution = Attribution{Cause::DriverLatency, Side::System, 0.7, "DPC", "a.sys"};
    spikes[2].attribution = Attribution{Cause::GameCpu, Side::Game, 0.6, "CPU busy", ""};

    ReportData d;
    d.application = "game<1>.exe";
    d.frames = frames;
    d.spikes = spikes;
    d.findings = {{Finding::Level::Problem, "High driver latency", "a.sys reached 2.5 ms"}};
    auto [f2, s2] = makeRun(30, {5, 15, 25});
    d.comparison = compareRuns(frames, spikes, f2, s2);
    const auto html = renderHtmlReport(d);

    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.find("game&lt;1&gt;.exe") != std::string::npos);        // escaped
    CHECK(html.find("DPC &lt;2 ms&gt; &amp; more") != std::string::npos);
    CHECK(html.find("<svg class=\"chart\"") != std::string::npos);
    CHECK(html.find("Mostly your system: 67%") != std::string::npos);
    CHECK(html.find("High driver latency") != std::string::npos);
    CHECK(html.find("Run comparison") != std::string::npos);
    CHECK(html.find("<script") == std::string::npos);                    // static file
    CHECK(htmlEscape("a\"b'c") == "a&quot;b&#39;c");
}

void backgroundFinding()
{
    EventTimeline tl(kFreq);
    std::vector<CpuBucket> bs;
    for (int i = 0; i < 300; ++i) // 3 s, 3.5 cores of background, mostly PID 777
        bs.push_back(bucket(i * 10.0, 0, 0, 35000, 777, 30000));
    tl.appendCpuBuckets(bs);
    CHECK(near(tl.bucketCoverageUs(), 3e6, 1.0));

    CorrelatorConfig cfg;
    cfg.processName = [](std::uint32_t) { return std::string{"obs64.exe"}; };
    const auto f = sessionFindings(tl, nullptr, cfg);
    const auto it = std::ranges::find_if(f, [](const Finding& x) { return x.title == "High background CPU load"; });
    CHECK(it != f.end());
    if (it != f.end())
        CHECK(it->detail.find("obs64.exe") != std::string::npos);
}

// ---------------------------------------------------------------------------
// AMD: inferred GPU throttling
// ---------------------------------------------------------------------------
GpuSample amd(double clock, double usage, double hotspot = 80.0)
{
    GpuSample g;
    g.coreClockMHz = clock;
    g.usagePct = usage;
    g.temperatureC = 65.0;
    g.hotspotC = hotspot;
    g.driverReportsReasons = false;
    return g;
}

void gpuThrottleInference()
{
    GpuThrottleInference inf;
    for (int i = 0; i < 10; ++i) {
        auto g = amd(2900 + (i % 3) * 20, 98);
        inf.apply(g);
        CHECK(g.reasons == 0);
    }
    auto idle = amd(500, 20); // low load: low clocks are fine
    inf.apply(idle);
    CHECK(idle.reasons == 0);

    auto drop = amd(1900, 99); // full load, ~35% below usual
    inf.apply(drop);
    CHECK(drop.reasons & gpu_reason::ClockDrop);

    auto normal = amd(2850, 97, 107); // normal clock, hot spot high
    inf.apply(normal);
    CHECK(!(normal.reasons & gpu_reason::ClockDrop));
    CHECK(normal.reasons & gpu_reason::HotspotHigh);

    // Not judged before warm-up
    GpuThrottleInference fresh;
    auto early = amd(1000, 99);
    fresh.apply(early);
    CHECK(early.reasons == 0);
}

void correlateAmdThrottle()
{
    Fixture f(Bound::Gpu);
    auto s = sample(1010);
    auto g = amd(1900, 99, 108);
    g.reasons = gpu_reason::ClockDrop | gpu_reason::HotspotHigh;
    s.gpu = g;
    f.tl.appendSamples(std::span{&s, 1});
    const auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause == Cause::GpuThrottle);
    CHECK(near(a.confidence, 0.65)); // inferred: a bit less sure than driver-reported
    CHECK(a.detail.find("clock drop under full load") != std::string::npos);
    CHECK(a.detail.find("108") != std::string::npos); // hotspot shown

    // Hotspot alone is not a cause
    Fixture h(Bound::Gpu);
    g.reasons = gpu_reason::HotspotHigh;
    s.gpu = g;
    h.tl.appendSamples(std::span{&s, 1});
    CHECK(attribute(h.spike, h.frame, h.tl).cause == Cause::GameGpu);

    // Findings mention hotspot
    const auto fs = sessionFindings(h.tl, nullptr);
    CHECK(std::ranges::any_of(fs, [](const Finding& x) { return x.title == "GPU hotspot near its limit"; }));
    CHECK(std::ranges::any_of(fs, [](const Finding& x) { return x.title == "No GPU throttling"; }));
}

void describeHelpers()
{
    auto s = sample(0);
    s.availableRamMB = 42291;
    s.totalRamMB = 64614;
    s.gpu = amd(2900, 98, 81);
    const auto text = describe(s);
    CHECK(text.find("RAM 41.3/63.1 GB free") != std::string::npos);
    CHECK(text.find("hotspot 81") != std::string::npos);
    CHECK(text.find("(inferred)") != std::string::npos);
    CHECK(describe(SystemSample{}) == "(empty sample)");

    CHECK(describe(makeTopology({0, 0, 1, 1}, {98304, 32768})) == "4 logical CPUs, 2 L3 group(s): 96 MB / 32 MB, 3D V-Cache on CCD0");
    CHECK(describe(CpuTopology{}) == "unknown");
}

void correlateShaderCompile()
{
    // Cache write 100 ms after the frame ended: still inside the extended window.
    Fixture f(Bound::Cpu);
    auto w = ev(EventKind::ShaderCacheWrite, 1140, 0, 42);
    w.bytes = 48 * 1024;
    f.add(w);
    auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause == Cause::ShaderCompile && a.side == Side::Game);
    CHECK(near(a.confidence, 0.8));
    CHECK(a.detail.starts_with("Game wrote 1 shader cache entry (48 KB)"));

    // A batched write 2 s later: still likely a compile, lower confidence.
    Fixture near_(Bound::Cpu);
    near_.add(ev(EventKind::ShaderCacheWrite, 3040, 0, 42));
    const auto n = attribute(near_.spike, near_.frame, near_.tl);
    CHECK(n.cause == Cause::ShaderCompile && near(n.confidence, 0.55));
    CHECK(n.detail.starts_with("Shader cache written 2.0 s after this frame"));

    // Too late (frame ended at 1040 ms, write 6 s later) or another process: no.
    Fixture late(Bound::Cpu);
    late.add(ev(EventKind::ShaderCacheWrite, 7100, 0, 42));
    CHECK(attribute(late.spike, late.frame, late.tl).cause == Cause::GameCpu);
    Fixture other(Bound::Cpu);
    other.add(ev(EventKind::ShaderCacheWrite, 1010, 0, 7));
    CHECK(attribute(other.spike, other.frame, other.tl).cause == Cause::GameCpu);

    // Shader writes don't count as disk or DPC evidence.
    Fixture stall;
    stall.add(ev(EventKind::ShaderCacheWrite, 1010, 0, 42));
    CHECK(attribute(stall.spike, stall.frame, stall.tl).cause == Cause::ShaderCompile);

    // Finding
    const auto fs = sessionFindings(f.tl, nullptr);
    CHECK(std::ranges::any_of(fs, [](const Finding& x) { return x.title == "Shaders compiled during play"; }));

    // Saved sessions keep the new cause and the shader writes
    auto [frames, spikes] = makeRun(5, {2});
    spikes[0].attribution = a;
    SavedSession s;
    s.frames = frames;
    s.spikes = spikes;
    s.gamePid = 42;
    s.shaderWrites = {{1.25, 42, 4096, false}, {3.5, 0, 65536, true}};
    const auto path = std::filesystem::temp_directory_path() / "framescope_shader.fscope";
    saveSession(path, s);
    const auto r = loadSession(path);
    std::filesystem::remove(path);
    CHECK(r.spikes[0].attribution && r.spikes[0].attribution->cause == Cause::ShaderCompile);
    CHECK(r.gamePid == 42 && r.shaderWrites.size() == 2);
    if (r.shaderWrites.size() == 2)
        CHECK(near(r.shaderWrites[0].timeSeconds, 1.25) && !r.shaderWrites[0].disk && r.shaderWrites[1].disk);

    // Mapping timeline events onto session time
    EventTimeline tl(kFreq);
    std::vector<FrameRecord> fr(3, frame(10.0));
    for (int i = 0; i < 3; ++i) {
        fr[i].timeSeconds = 0.010 * (i + 1);
        fr[i].qpcStart = ms2t(5000 + i * 10.0);
        fr[i].qpcEnd = ms2t(5010 + i * 10.0);
    }
    auto sw = ev(EventKind::ShaderCacheWrite, 5500, 0, 42);
    tl.append(std::span{&sw, 1});
    const auto mapped = shaderWritesForSession(tl, fr);
    CHECK(mapped.size() == 1 && near(mapped[0].timeSeconds, 0.5, 1e-9));
}

void perfLimitCounter()
{
    CHECK(near(perfLimitFromCounter(100.0), 0.0));  // no limit
    CHECK(near(perfLimitFromCounter(115.0), 0.0));  // boosting
    CHECK(near(perfLimitFromCounter(70.0), 30.0));  // capped by 30%
    CHECK(near(perfLimitFromCounter(-5.0), 100.0));
}

void ccdRelativeToSession()
{
    // Session: 4% off the V-Cache CCD and ~5 cross-CCD migrations per ms, like a real 9950X3D.
    const auto setup = [](Fixture& f, float ccd0, float ccd1, std::uint16_t crossPerBucket) {
        f.tl.setTopology(makeTopology({0, 0, 1, 1}, {98304, 32768}));
        std::vector<CpuBucket> normal;
        for (int i = 0; i < 2000; ++i) // 20 s of normal operation, before the spike
            normal.push_back(bucket(-20000.0 + i * 10.0, 9600, 400, 1000, 0, 0, 50));
        f.tl.appendCpuBuckets(normal);
        std::vector<CpuBucket> window;
        for (double t = 990; t < 1040; t += 10)
            window.push_back(bucket(t, ccd0, ccd1, 1000, 0, 0, crossPerBucket));
        f.tl.appendCpuBuckets(window);
    };

    Fixture usual(Bound::Cpu);
    setup(usual, 9600, 400, 50); // the spike frame looks like every other frame
    CHECK(attribute(usual.spike, usual.frame, usual.tl).cause == Cause::GameCpu);

    Fixture off(Bound::Cpu);
    setup(off, 3000, 7000, 50); // 70% off the V-Cache CCD vs 4% normally
    const auto a = attribute(off.spike, off.frame, off.tl);
    CHECK(a.cause == Cause::CcdScheduling);
    CHECK(a.detail.find("normally 4%") != std::string::npos);

    // A burst of migrations alone (any loading hitch) is not blamed on the CCDs.
    Fixture hop(Bound::Cpu);
    setup(hop, 9600, 400, 400); // 8x the usual migration rate
    CHECK(attribute(hop.spike, hop.frame, hop.tl).cause == Cause::GameCpu);
}

void stutterPattern()
{
    // 15 min, 60 spikes: 40 in the first 2 minutes + 20 spread out.
    std::vector<double> times;
    for (int i = 0; i < 40; ++i)
        times.push_back(1.0 + i * 3.0);
    for (int i = 0; i < 20; ++i)
        times.push_back(150.0 + i * 37.0);
    auto [f1, s1] = makeRun(900, times);
    auto pf = stutterPatternFindings(f1, s1);
    CHECK(pf.size() == 1 && pf[0].title == "Stutter comes in bursts");
    if (!pf.empty())
        CHECK(pf[0].detail.find("0:00–2:00") != std::string::npos);

    std::vector<double> even;
    for (int i = 0; i < 30; ++i)
        even.push_back(10.0 + i * 29.0);
    auto [f2, s2] = makeRun(900, even);
    pf = stutterPatternFindings(f2, s2);
    CHECK(pf.size() == 1 && pf[0].title == "Stutter is spread evenly");

    auto [f3, s3] = makeRun(60, times); // too short
    CHECK(stutterPatternFindings(f3, s3).empty());
}

void rareGpuDropsAndDiskFlushes()
{
    EventTimeline tl(kFreq);
    std::vector<SystemSample> ss;
    for (int i = 0; i < 400; ++i) {
        auto s = sample(i * 250.0);
        s.gpu = amd(2900, 98);
        if (i == 7)
            s.gpu->reasons = gpu_reason::ClockDrop;
        ss.push_back(s);
    }
    tl.appendSamples(ss);
    // Shader cache: one game write and two disk flushes (PID 0, "disk")
    auto w = ev(EventKind::ShaderCacheWrite, 100, 0, 42);
    w.bytes = 4096;
    tl.append(std::span{&w, 1});
    for (int i = 0; i < 2; ++i) {
        auto d = ev(EventKind::ShaderCacheWrite, 2000 + i, 0, 0, "disk");
        d.bytes = 1 << 20;
        tl.append(std::span{&d, 1});
    }
    const auto f = sessionFindings(tl, nullptr);
    const auto find = [&](std::string_view title) {
        return std::ranges::find_if(f, [&](const Finding& x) { return x.title == title; });
    };
    CHECK(find("GPU throttling") == f.end());
    CHECK(find("Rare GPU clock drops") != f.end());
    if (auto it = find("Rare GPU clock drops"); it != f.end())
        CHECK(it->level == Finding::Level::Info && it->detail.find("1 of 400 samples (< 1%)") != std::string::npos);
    const auto sh = find("Shaders compiled during play");
    CHECK(sh != f.end());
    if (sh != f.end())
        CHECK(sh->detail.starts_with("1 shader cache writes, 2 flushes to disk (2.0 MB)"));

    // Disk flushes have no game PID: they never decide a single spike.
    Fixture fx(Bound::Cpu);
    auto d = ev(EventKind::ShaderCacheWrite, 1010, 0, 0, "disk");
    fx.add(d);
    CHECK(attribute(fx.spike, fx.frame, fx.tl).cause == Cause::GameCpu);
}

void backgroundExcludesGame()
{
    // Before the trace knew the game's PID, the game itself was the top "background" process.
    Fixture f(Bound::Cpu);
    std::vector<CpuBucket> bs;
    for (double t = 990; t < 1040; t += 10)
        bs.push_back(bucket(t, 0, 0, 43000, 42, 21000)); // PID 42 = the game
    f.tl.appendCpuBuckets(bs);
    const auto a = attribute(f.spike, f.frame, f.tl);
    CHECK(a.cause != Cause::BackgroundLoad);
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
        {"timelineSamplesAndBuckets", timelineSamplesAndBuckets},
        {"topology", topology},
        {"correlateGpuThrottle", correlateGpuThrottle},
        {"correlateMemory", correlateMemory},
        {"correlateCpuScheduling", correlateCpuScheduling},
        {"findings", findings},
        {"sessionFileRoundTrip", sessionFileRoundTrip},
        {"compareShaderCompile", compareShaderCompile},
        {"compareRepeatable", compareRepeatable},
        {"compareRandom", compareRandom},
        {"htmlReport", htmlReport},
        {"backgroundFinding", backgroundFinding},
        {"gpuThrottleInference", gpuThrottleInference},
        {"correlateAmdThrottle", correlateAmdThrottle},
        {"describeHelpers", describeHelpers},
        {"correlateShaderCompile", correlateShaderCompile},
        {"perfLimitCounter", perfLimitCounter},
        {"ccdRelativeToSession", ccdRelativeToSession},
        {"stutterPattern", stutterPattern},
        {"rareGpuDropsAndDiskFlushes", rareGpuDropsAndDiskFlushes},
        {"backgroundExcludesGame", backgroundExcludesGame},
    };

    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
