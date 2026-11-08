#pragma once

#include "core/LatencyStats.h"
#include "core/SystemEvent.h"
#include "core/SystemSample.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace framescope::etw {

struct KernelTraceOptions {
    // DPC/ISR shorter than this only go into LatencyStats, not onto the
    // timeline. Keeps memory flat: there are thousands of DPCs per second.
    double keepDpcIsrOverUs = 100.0;
    bool hardFaults = true;
    bool diskIo = true;

    // Use the classic global "NT Kernel Logger" instead of a private system
    // logger session. Only one tool at a time can own it (LatencyMon, xperf ...),
    // so it's the fallback, not the default.
    bool forceNtKernelLogger = false;

    // Context switches -> per-10 ms CPU buckets (game time per CCD,
    // migrations, background load). Highest-volume event; can be turned off.
    bool cpuScheduling = true;
    std::vector<std::uint8_t> ccdOfCpu; // from CpuTopology; empty = single CCD

    // File I/O init events -> writes to GPU driver / D3D shader caches
    // (NVIDIA DXCache, AMD DxCache, D3DSCache ...): direct evidence of shader
    // compilation. Moderate volume (every file open/read/write on the system).
    bool shaderCache = true;
};

// Real-time ETW kernel trace: DPC/ISR (resolved to driver names), hard page
// faults and disk I/O (resolved to processes). Timestamps are raw QPC, the
// same clock PresentMon uses with --qpc_time.
//
// Needs admin rights. On non-Windows builds isSupported() is false and
// start() always fails.
// Raw counters, for checking that event decoding works on a given machine.
struct TraceDiagnostics {
    std::uint64_t eventsTotal = 0;
    std::uint64_t dpcIsr = 0;
    std::uint64_t dpcIsrUnresolved = 0;  // routine address not inside a known driver
    std::uint64_t hardFaults = 0;
    std::uint64_t diskIo = 0;
    std::uint64_t contextSwitches = 0;
    std::uint64_t threadEvents = 0;
    std::uint64_t imageEvents = 0;
    std::uint64_t fileCreates = 0;
    std::uint64_t shaderCacheWrites = 0;
    std::uint64_t malformed = 0;         // known event, but too short / implausible
    std::uint64_t cpuBucketsFinished = 0;
    std::uint64_t knownThreads = 0;
    std::uint64_t knownDrivers = 0;
    std::uint64_t eventsLost = 0;        // ETW: buffers full
    std::uint64_t realtimeBuffersLost = 0;
};

std::string describe(const TraceDiagnostics& d);

class KernelTrace {
public:
    KernelTrace();
    ~KernelTrace();
    KernelTrace(const KernelTrace&) = delete;
    KernelTrace& operator=(const KernelTrace&) = delete;

    static bool isSupported() noexcept;
    static std::int64_t qpcFrequency() noexcept;
    static std::int64_t qpcNow() noexcept;

    bool start(const KernelTraceOptions& options = {});
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;

    // Thread-safe. Moves out events collected since the last call.
    std::vector<core::SystemEvent> drainEvents();

    // Thread-safe. Finished 10 ms CPU buckets. They lag ~2 s behind real time,
    // because per-CPU ETW buffers arrive out of order.
    std::vector<core::CpuBucket> drainCpuBuckets();

    // The game's PID, so its threads can be told apart. Set it once known.
    void setTargetProcess(std::uint32_t pid) noexcept;

    [[nodiscard]] std::int64_t startQpc() const noexcept;
    [[nodiscard]] std::int64_t lastEventQpc() const noexcept; // trace coverage
    [[nodiscard]] std::uint64_t dpcIsrCount() const noexcept;  // sanity check: should grow fast
    [[nodiscard]] core::LatencyStats latencySnapshot() const;
    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] std::string sessionDescription() const;      // which session mode is in use
    [[nodiscard]] TraceDiagnostics diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace framescope::etw
