#pragma once

#include "core/SystemSample.h"

#include <memory>
#include <optional>
#include <string>

namespace framescope::win {

// AMD GPU sensors through ADLX (ships with the AMD Adrenalin driver as
// amdadlx64.dll, loaded at runtime). Picks the discrete GPU with the most VRAM.
//
// ADLX has no throttle reasons, so ClockDrop/HotspotHigh are inferred from
// clock, load and hotspot temperature (core::GpuThrottleInference).
//
// Only built with FRAMESCOPE_WITH_ADLX (the SDK is fetched at configure time,
// its license doesn't allow keeping it in this repository).
class AdlxGpu {
public:
    AdlxGpu();
    ~AdlxGpu();
    AdlxGpu(const AdlxGpu&) = delete;
    AdlxGpu& operator=(const AdlxGpu&) = delete;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const std::string& deviceName() const noexcept;
    std::optional<core::GpuSample> sample();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace framescope::win
