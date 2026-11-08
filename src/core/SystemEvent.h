#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace framescope::core {

enum class EventKind : std::uint8_t {
    Dpc,       // deferred procedure call (driver work at raised IRQL)
    Isr,       // interrupt service routine
    HardFault, // page fault that had to read from disk
    DiskIo,    // completed disk read/write
    ShaderCacheWrite, // write to a GPU driver / D3D shader cache file (= a shader was just compiled)
};

std::string_view toString(EventKind k) noexcept;

// One system event on the QPC timeline, already resolved by the trace
// (driver module name, owning process). Platform-independent so the
// correlation logic is testable anywhere.
struct SystemEvent {
    EventKind kind = EventKind::Dpc;
    std::int64_t startQpc = 0;
    std::int64_t endQpc = 0;
    std::uint32_t processId = 0; // HardFault/DiskIo: owning process (0 if unknown)
    std::uint32_t bytes = 0;     // DiskIo / HardFault / ShaderCacheWrite size
    std::string module;          // Dpc/Isr: driver file name, e.g. "nvlddmkm.sys"

    [[nodiscard]] std::int64_t durationQpc() const noexcept { return endQpc - startQpc; }
};

} // namespace framescope::core
