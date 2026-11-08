// Minimal dependency-free tests for the core library. Run via `ctest`.

#include "core/CaptureSession.h"
#include "core/PresentMonCsvParser.h"
#include "core/SessionStats.h"
#include "core/SpikeDetector.h"

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
    };

    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
