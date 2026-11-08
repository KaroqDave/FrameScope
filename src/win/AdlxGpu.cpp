#include "win/AdlxGpu.h"

#include "ADLXHelper/Windows/Cpp/ADLXHelper.h"
#include "Include/IPerformanceMonitoring.h"
#include "Include/ISystem.h"

namespace framescope::win {

using namespace adlx;

struct AdlxGpu::Impl {
    ADLXHelper helper;
    bool initialised = false;
    bool ok = false;
    std::string name;

    IADLXPerformanceMonitoringServicesPtr perf;
    IADLXGPUPtr gpu;
    IADLXGPUMetricsSupportPtr support;
    core::GpuThrottleInference inference;

    bool supported(ADLX_RESULT (ADLX_STD_CALL IADLXGPUMetricsSupport::*check)(adlx_bool*)) const
    {
        adlx_bool yes = false;
        return support && ADLX_SUCCEEDED(((*support).*check)(&yes)) && yes;
    }
};

AdlxGpu::AdlxGpu()
    : m(std::make_unique<Impl>())
{
    if (!ADLX_SUCCEEDED(m->helper.Initialize()))
        return; // no AMD driver / no amdadlx64.dll
    m->initialised = true;

    IADLXSystem* system = m->helper.GetSystemServices();
    if (!system)
        return;

    IADLXGPUListPtr gpus;
    if (!ADLX_SUCCEEDED(system->GetGPUs(&gpus)) || !gpus)
        return;

    // Prefer a discrete GPU, then the one with the most VRAM.
    std::uint64_t bestScore = 0;
    for (adlx_uint i = gpus->Begin(); i != gpus->End(); ++i) {
        IADLXGPUPtr candidate;
        if (!ADLX_SUCCEEDED(gpus->At(i, &candidate)) || !candidate)
            continue;
        ADLX_GPU_TYPE type = GPUTYPE_UNDEFINED;
        candidate->Type(&type);
        adlx_uint vramMB = 0;
        candidate->TotalVRAM(&vramMB);
        const std::uint64_t score = vramMB + (type == GPUTYPE_DISCRETE ? (std::uint64_t{1} << 32) : 0);
        if (!m->gpu || score > bestScore) {
            m->gpu = candidate;
            bestScore = score;
        }
    }
    if (!m->gpu)
        return;

    if (!ADLX_SUCCEEDED(system->GetPerformanceMonitoringServices(&m->perf)) || !m->perf)
        return;
    m->perf->GetSupportedGPUMetrics(m->gpu, &m->support);

    const char* name = nullptr;
    if (ADLX_SUCCEEDED(m->gpu->Name(&name)) && name)
        m->name = name;
    m->ok = true;
}

AdlxGpu::~AdlxGpu()
{
    // All interfaces must be released before ADLX is terminated.
    m->support.Release();
    m->gpu.Release();
    m->perf.Release();
    if (m->initialised)
        m->helper.Terminate();
}

bool AdlxGpu::ok() const noexcept { return m->ok; }
const std::string& AdlxGpu::deviceName() const noexcept { return m->name; }

std::optional<core::GpuSample> AdlxGpu::sample()
{
    if (!m->ok)
        return std::nullopt;

    IADLXGPUMetricsPtr metrics;
    if (!ADLX_SUCCEEDED(m->perf->GetCurrentGPUMetrics(m->gpu, &metrics)) || !metrics)
        return std::nullopt;

    core::GpuSample s;
    s.driverReportsReasons = false;

    adlx_int clock = 0;
    if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUClockSpeed) && ADLX_SUCCEEDED(metrics->GPUClockSpeed(&clock)))
        s.coreClockMHz = clock;

    adlx_double v = 0.0;
    if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUTemperature) && ADLX_SUCCEEDED(metrics->GPUTemperature(&v)))
        s.temperatureC = v;
    if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUHotspotTemperature) &&
        ADLX_SUCCEEDED(metrics->GPUHotspotTemperature(&v)))
        s.hotspotC = v;
    if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUTotalBoardPower) &&
        ADLX_SUCCEEDED(metrics->GPUTotalBoardPower(&v)))
        s.powerW = v;
    else if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUPower) && ADLX_SUCCEEDED(metrics->GPUPower(&v)))
        s.powerW = v;
    if (m->supported(&IADLXGPUMetricsSupport::IsSupportedGPUUsage) && ADLX_SUCCEEDED(metrics->GPUUsage(&v)))
        s.usagePct = v;

    m->inference.apply(s);
    return s;
}

} // namespace framescope::win
