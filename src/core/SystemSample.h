#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace framescope::core {

// NVML clock-event ("throttle") reason bits.
namespace gpu_reason {
constexpr std::uint64_t Idle = 0x1;
constexpr std::uint64_t AppClocks = 0x2;
constexpr std::uint64_t SwPowerCap = 0x4;          // normal boost limit under load
constexpr std::uint64_t HwSlowdown = 0x8;
constexpr std::uint64_t SyncBoost = 0x10;
constexpr std::uint64_t SwThermal = 0x20;
constexpr std::uint64_t HwThermal = 0x40;
constexpr std::uint64_t HwPowerBrake = 0x80;
constexpr std::uint64_t DisplayClock = 0x100;

// Inferred by FrameScope when the driver doesn't report reasons (AMD/ADLX).
constexpr std::uint64_t ClockDrop = 0x10000;   // clock fell well below its usual level under full load
constexpr std::uint64_t HotspotHigh = 0x20000; // hotspot near the throttle point (informational)

// Reasons that mean real, sudden clock drops worth blaming.
constexpr std::uint64_t Harmful = HwSlowdown | SwThermal | HwThermal | HwPowerBrake | ClockDrop;
} // namespace gpu_reason

struct GpuSample {
    double coreClockMHz = 0.0;
    double temperatureC = 0.0;
    double powerW = 0.0;
    std::uint64_t reasons = 0;           // gpu_reason bits
    std::optional<double> hotspotC;      // junction temperature (AMD)
    std::optional<double> usagePct;      // GPU load
    bool driverReportsReasons = true;    // false: reasons are inferred (ClockDrop/HotspotHigh)
};

// For GPUs without throttle reasons: flags a clock drop under full load and a
// hotspot near the limit, from the samples alone.
class GpuThrottleInference {
public:
    struct Config {
        double busyUsagePct = 90.0;  // "full load"
        double dropRatio = 0.75;     // clock below this share of the usual full-load clock
        int warmupSamples = 8;       // full-load samples needed before judging
        double hotspotHighC = 105.0;
    };

    GpuThrottleInference() = default;
    explicit GpuThrottleInference(Config cfg) : m_cfg(cfg) {}

    // Adds ClockDrop / HotspotHigh to sample.reasons where they apply.
    void apply(GpuSample& sample);

private:
    Config m_cfg{};
    double m_typicalBusyClock = 0.0; // slow-moving average of full-load clocks
    int m_busySamples = 0;
};

// Periodic system snapshot (a few per second). Every field is optional:
// what's missing depends on hardware and permissions.
struct SystemSample {
    std::int64_t qpc = 0;
    std::optional<double> availableRamMB;
    std::optional<double> totalRamMB;
    std::optional<double> pageInsPerSec;
    std::optional<double> cpuPerfLimitPct; // "% Performance Limit": >0 = clocks capped
    std::optional<double> gameVramMB;      // dedicated VRAM used by the game
    std::optional<double> vramTotalMB;
    std::optional<GpuSample> gpu;          // NVIDIA (NVML) only for now
};

constexpr std::size_t kMaxCcds = 8;

// CPU time in one short time slice (10 ms), built from ETW context switches.
struct CpuBucket {
    std::int64_t startQpc = 0;
    std::int64_t endQpc = 0;
    std::array<float, kMaxCcds> gameUsByCcd{};   // game threads' CPU time per CCD
    std::uint16_t gameMigrations = 0;            // game thread resumed on another core
    std::uint16_t gameCrossCcdMigrations = 0;    // ... on another CCD (cold cache)
    float backgroundUs = 0.0f;                   // all other non-idle processes, all cores
    std::uint32_t topBackgroundPid = 0;
    float topBackgroundUs = 0.0f;
};

// Which logical CPU belongs to which CCD (= L3 cache group).
struct CpuTopology {
    std::vector<std::uint8_t> ccdOfCpu;  // index: logical CPU number
    std::vector<std::uint32_t> l3KBOfCcd;
    int vcacheCcd = -1;                  // CCD with the bigger L3; -1 if all equal or single CCD

    [[nodiscard]] std::size_t ccdCount() const noexcept { return l3KBOfCcd.size(); }
    [[nodiscard]] bool hasAsymmetricCache() const noexcept { return vcacheCcd >= 0; }
};

// One-line human-readable summary, for logs and diagnostics.
std::string describe(const SystemSample& sample);
std::string describe(const CpuTopology& topology);

// Builds a topology from (cpu -> L3 group, L3 size) pairs.
CpuTopology makeTopology(const std::vector<std::uint8_t>& ccdOfCpu, const std::vector<std::uint32_t>& l3KBOfCcd);

} // namespace framescope::core
