#pragma once

#include "core/Attribution.h"
#include "core/EventTimeline.h"
#include "core/FrameRecord.h"
#include "core/LatencyStats.h"
#include "core/SpikeDetector.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace framescope::core {

struct CorrelatorConfig {
    double preWindowMs = 1.0;        // also look this far before the frame interval

    double dpcMinMs = 0.5;           // ignore DPC/ISR shorter than this as a cause
    double dpcStrongMs = 2.0;        // a single DPC/ISR this long is a cause in a stall
    double dpcShare = 0.15;          // ... or DPC/ISR time >= this share of the excess

    double hardFaultShare = 0.20;    // game hard-fault time >= share of excess
    double diskShare = 0.30;         // longest disk I/O >= share of excess

    double lowRamMB = 1500.0;        // "RAM nearly full" below this ...
    double lowRamShare = 0.05;       // ... or below this share of total RAM
    double vramFullShare = 0.95;     // game VRAM use >= share of the card's VRAM
    double cpuPerfLimitPct = 10.0;   // "% Performance Limit" above this = throttled

    double backgroundCores = 4.0;    // other processes used >= this many cores on average
    double backgroundTopShare = 0.5; // ... and one process was >= this share of it

    // CCD scheduling counts only when the frame is clearly worse than the
    // session's own normal: some CPUs hop thousands of times per second anyway.
    double ccdOffShare = 0.40;       // >= this share of game CPU time off the V-Cache CCD ...
    double ccdOffShareAboveNormal = 0.25; // ... and this much above the session's average share


    // Shader cache writes: right at the frame = strong evidence. Drivers (AMD)
    // also write their cache in batches, seconds after the compile, so writes
    // near the frame still count, with lower confidence.
    double compileMinMs = 5.0;       // sampled shader compiler time on game threads in the frame ...
    double compileShare = 0.15;      // ... and at least this share of the excess
    double shaderAfterMs = 250.0;
    double shaderNearBeforeMs = 2000.0;
    double shaderNearAfterMs = 5000.0;

    // Turns a PID into a readable name for the evidence text.
    std::function<std::string(std::uint32_t)> processName;
};

// Everything the trace saw during one spike window.
struct WindowEvidence {
    double windowUs = 0.0;

    double maxDpcIsrUs = 0.0;
    EventKind maxDpcIsrKind = EventKind::Dpc;
    std::string maxDpcIsrModule;
    double sumDpcIsrUs = 0.0;         // clipped to the window

    std::size_t gameHardFaults = 0;
    double gameHardFaultUs = 0.0;     // clipped to the window
    std::size_t allHardFaults = 0;

    double maxGameDiskUs = 0.0;
    double maxAnyDiskUs = 0.0;

    std::size_t gameShaderWrites = 0;     // in [frame start, frame end + shaderAfterMs]
    std::uint64_t gameShaderBytes = 0;
    double shaderCompileUs = 0.0;         // sampled compiler time on game threads (all cores)
    std::size_t nearShaderWrites = 0;     // in the wider "near" window
    std::optional<double> nearestShaderGapMs; // write time - frame end, closest one

    // From periodic samples around the window
    bool hasSamples = false;
    std::uint64_t gpuReasons = 0;     // OR of all samples
    double gpuMinClockMHz = 0.0;
    double gpuMaxTempC = 0.0;
    std::optional<double> minAvailableRamMB;
    std::optional<double> totalRamMB;
    std::optional<double> maxCpuPerfLimitPct;
    std::optional<double> maxGameVramMB;
    std::optional<double> vramTotalMB;

    // From CPU buckets
    bool hasCpuData = false;
    std::array<double, kMaxCcds> gameUsByCcd{};
    int gameCrossCcdMigrations = 0;
    double backgroundUs = 0.0;
    std::uint32_t topBackgroundPid = 0;
    double topBackgroundUs = 0.0;
};

struct ShaderWindow {
    std::int64_t directTo = 0; // end of the "right at the frame" window
    std::int64_t nearFrom = 0; // wider window
    std::int64_t nearTo = 0;
};

WindowEvidence gatherEvidence(const EventTimeline& timeline, std::int64_t from, std::int64_t to,
                              std::uint32_t gamePid, const ShaderWindow& shader = {});

// Decide the most likely cause of one spike.
Attribution attribute(const Spike& spike, const FrameRecord& frame, const EventTimeline& timeline,
                      const CorrelatorConfig& cfg = {});

// Session-level summary across all attributed spikes.
struct Diagnosis {
    std::size_t attributed = 0;
    std::size_t system = 0;
    std::size_t game = 0;
    std::size_t mixed = 0;
    std::size_t unknown = 0;
    Cause topCause = Cause::NoTraceData;
    std::size_t topCauseCount = 0;
    std::string topModule;          // driver most often behind DriverLatency spikes
    std::string verdict;            // one line for the UI
};

Diagnosis diagnose(std::span<const Spike> spikes);

// Whole-session health checks, independent of single spikes.
struct Finding {
    enum class Level { Good, Info, Warning, Problem };
    Level level = Level::Info;
    std::string title;
    std::string detail;
};

std::string_view toString(Finding::Level l) noexcept;

std::vector<Finding> sessionFindings(const EventTimeline& timeline, const LatencyStats* latency,
                                     const CorrelatorConfig& cfg = {});

// When does the stutter happen? Bursts (loading, streaming, shader compiles
// when entering new areas) vs. evenly spread (something constant). Works
// without a system trace, e.g. on an opened CSV.
std::vector<Finding> stutterPatternFindings(std::span<const FrameRecord> frames, std::span<const Spike> spikes,
                                            double binSeconds = 30.0);

} // namespace framescope::core
