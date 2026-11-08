#pragma once

#include "core/LatencyStats.h"
#include "core/SystemEvent.h"

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
};

// Real-time ETW kernel trace: DPC/ISR (resolved to driver names), hard page
// faults and disk I/O (resolved to processes). Timestamps are raw QPC, the
// same clock PresentMon uses with --qpc_time.
//
// Needs admin rights. On non-Windows builds isSupported() is false and
// start() always fails.
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

    [[nodiscard]] std::int64_t startQpc() const noexcept;
    [[nodiscard]] std::int64_t lastEventQpc() const noexcept; // trace coverage
    [[nodiscard]] std::uint64_t dpcIsrCount() const noexcept;  // sanity check: should grow fast
    [[nodiscard]] core::LatencyStats latencySnapshot() const;
    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] std::string sessionDescription() const;      // which session mode is in use

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace framescope::etw
