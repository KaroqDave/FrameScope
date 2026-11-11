#include "win/SystemSampler.h"

#ifdef _WIN32

#ifdef FRAMESCOPE_HAVE_ADLX
#include "win/AdlxGpu.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace framescope::win {
namespace {

std::int64_t qpcNow()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// ---------------------------------------------------------------------------
// NVML, loaded at runtime so there's no SDK or hard dependency.
// ---------------------------------------------------------------------------
class Nvml {
public:
    Nvml()
    {
        m_lib = LoadLibraryW(L"nvml.dll");
        if (!m_lib) {
            wchar_t path[MAX_PATH];
            if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, MAX_PATH))
                m_lib = LoadLibraryW(path);
        }
        if (!m_lib)
            return;

        m_init = load<Init>("nvmlInit_v2");
        m_shutdown = load<Shutdown>("nvmlShutdown");
        m_handle = load<HandleByIndex>("nvmlDeviceGetHandleByIndex_v2");
        m_name = load<Name>("nvmlDeviceGetName");
        m_clock = load<Clock>("nvmlDeviceGetClockInfo");
        m_temp = load<Temp>("nvmlDeviceGetTemperature");
        m_power = load<Power>("nvmlDeviceGetPowerUsage");
        m_reasons = load<Reasons>("nvmlDeviceGetCurrentClocksEventReasons");
        if (!m_reasons)
            m_reasons = load<Reasons>("nvmlDeviceGetCurrentClocksThrottleReasons");
        m_utilization = load<Utilization>("nvmlDeviceGetUtilizationRates");
        m_threshold = load<Threshold>("nvmlDeviceGetTemperatureThreshold");

        if (!m_init || !m_handle || !m_clock || m_init() != 0)
            return;
        m_initialised = true;
        if (m_handle(0, &m_device) != 0)
            return;
        m_ok = true;
        if (m_name) {
            char buf[96] = {};
            if (m_name(m_device, buf, sizeof(buf)) == 0)
                m_deviceName = buf;
        }
        // Temperature at which the driver starts slowing the GPU down.
        unsigned int limit = 0;
        if (m_threshold && m_threshold(m_device, 1 /*NVML_TEMPERATURE_THRESHOLD_SLOWDOWN*/, &limit) == 0 &&
            limit >= 60 && limit <= 130)
            m_slowdownC = limit;
    }

    ~Nvml()
    {
        if (m_initialised && m_shutdown)
            m_shutdown();
        if (m_lib)
            FreeLibrary(m_lib);
    }

    [[nodiscard]] bool ok() const { return m_ok; }
    [[nodiscard]] const std::string& deviceName() const { return m_deviceName; }

    std::optional<core::GpuSample> sample()
    {
        if (!m_ok)
            return std::nullopt;
        core::GpuSample s;
        unsigned int v = 0;
        if (m_clock(m_device, 0 /*NVML_CLOCK_GRAPHICS*/, &v) == 0)
            s.coreClockMHz = v;
        if (m_temp && m_temp(m_device, 0 /*NVML_TEMPERATURE_GPU*/, &v) == 0)
            s.temperatureC = v;
        if (m_power && m_power(m_device, &v) == 0)
            s.powerW = v / 1000.0;
        struct { unsigned int gpu; unsigned int memory; } util{};
        if (m_utilization && m_utilization(m_device, &util) == 0)
            s.usagePct = util.gpu;

        // Throttle reasons straight from the driver. Older drivers without them
        // get the same clock-drop inference as AMD cards.
        unsigned long long r = 0;
        if (m_reasons && m_reasons(m_device, &r) == 0) {
            s.reasons = r;
        } else {
            s.driverReportsReasons = false;
            m_inference.apply(s);
        }
        // Same "close to the limit" warning AMD cards get from their hotspot sensor.
        if (m_slowdownC > 0) {
            s.limitC = m_slowdownC;
            if (s.temperatureC >= m_slowdownC - 3.0)
                s.reasons |= core::gpu_reason::HotspotHigh;
        }
        return s;
    }

private:
    using Device = void*;
    using Init = int (*)();
    using Shutdown = int (*)();
    using HandleByIndex = int (*)(unsigned int, Device*);
    using Name = int (*)(Device, char*, unsigned int);
    using Clock = int (*)(Device, int, unsigned int*);
    using Temp = int (*)(Device, int, unsigned int*);
    using Power = int (*)(Device, unsigned int*);
    using Reasons = int (*)(Device, unsigned long long*);
    using Utilization = int (*)(Device, void*);
    using Threshold = int (*)(Device, int, unsigned int*);

    template <typename F>
    F load(const char* name)
    {
        return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m_lib, name)));
    }

    HMODULE m_lib = nullptr;
    Device m_device = nullptr;
    bool m_initialised = false;
    bool m_ok = false;
    std::string m_deviceName;
    double m_slowdownC = 0.0;
    core::GpuThrottleInference m_inference;
    Utilization m_utilization = nullptr;
    Threshold m_threshold = nullptr;
    Init m_init = nullptr;
    Shutdown m_shutdown = nullptr;
    HandleByIndex m_handle = nullptr;
    Name m_name = nullptr;
    Clock m_clock = nullptr;
    Temp m_temp = nullptr;
    Power m_power = nullptr;
    Reasons m_reasons = nullptr;
};

// Biggest dedicated VRAM of any hardware adapter, in MB.
std::optional<double> queryVramTotalMB()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return std::nullopt;
    SIZE_T best = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            best = std::max(best, desc.DedicatedVideoMemory);
        adapter->Release();
    }
    factory->Release();
    if (best == 0)
        return std::nullopt;
    return static_cast<double>(best) / (1024.0 * 1024.0);
}

} // namespace

struct SystemSampler::Impl {
    int intervalMs = 250;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<std::uint32_t> targetPid{0};

    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopRequested = false;
    std::vector<core::SystemSample> pending;
    std::string status;

    void run()
    {
        // GPU sensors: NVIDIA via NVML, otherwise AMD via ADLX.
        Nvml nvml;
#ifdef FRAMESCOPE_HAVE_ADLX
        std::unique_ptr<AdlxGpu> adlx;
        if (!nvml.ok())
            adlx = std::make_unique<AdlxGpu>();
        const bool amdOk = adlx && adlx->ok();
        const std::string amdName = amdOk ? adlx->deviceName() : std::string{};
#else
        const bool amdOk = false;
        const std::string amdName;
#endif
        const auto gpuSample = [&]() -> std::optional<core::GpuSample> {
            if (nvml.ok())
                return nvml.sample();
#ifdef FRAMESCOPE_HAVE_ADLX
            if (amdOk)
                return adlx->sample();
#endif
            return std::nullopt;
        };
        const auto vramTotal = queryVramTotalMB();

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        const std::optional<double> totalRam =
            GlobalMemoryStatusEx(&mem) ? std::optional<double>(static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0))
                                       : std::nullopt;

        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER cAvail = nullptr, cPageIn = nullptr, cLimit = nullptr, cVram = nullptr;
        std::uint32_t vramPid = 0;
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            // English names: work on German (and every other) Windows.
            PdhAddEnglishCounterW(query, L"\\Memory\\Available MBytes", 0, &cAvail);
            PdhAddEnglishCounterW(query, L"\\Memory\\Pages Input/sec", 0, &cPageIn);
            PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Performance Limit", 0, &cLimit);
        }

        {
            std::string s = query ? "PDH" : "PDH unavailable";
            if (vramTotal)
                s += std::string{", DXGI ("} + std::to_string(static_cast<int>(*vramTotal / 1024.0 + 0.5)) + " GB VRAM)";
            if (nvml.ok())
                s += ", NVML (" + nvml.deviceName() + ")";
            else if (amdOk)
                s += ", ADLX (" + amdName + ")";
            else
                s += ", no GPU sensors (needs NVML or ADLX)";
            std::lock_guard lock(mutex);
            status = s;
        }

        const auto read = [](PDH_HCOUNTER c) -> std::optional<double> {
            if (!c)
                return std::nullopt;
            PDH_FMT_COUNTERVALUE v{};
            if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS ||
                (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA))
                return std::nullopt;
            return v.doubleValue;
        };

        // Sum over all instances (a process can have several per adapter).
        const auto readSum = [](PDH_HCOUNTER c) -> std::optional<double> {
            if (!c)
                return std::nullopt;
            DWORD bytes = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA))
                return std::nullopt;
            std::vector<BYTE> buf(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
                return std::nullopt;
            double sum = 0.0;
            for (DWORD i = 0; i < count; ++i)
                sum += items[i].FmtValue.doubleValue;
            return sum;
        };

        if (query)
            PdhCollectQueryData(query); // rate counters need two collections

        std::unique_lock lock(mutex);
        while (!stopRequested) {
            wake.wait_for(lock, std::chrono::milliseconds(intervalMs), [this] { return stopRequested; });
            if (stopRequested)
                break;
            lock.unlock();

            // (Re)attach the VRAM counter when the game's PID becomes known.
            const auto pid = targetPid.load();
            if (query && pid != vramPid) {
                if (cVram)
                    PdhRemoveCounter(cVram);
                cVram = nullptr;
                wchar_t path[128];
                std::swprintf(path, 128, L"\\GPU Process Memory(pid_%u_*)\\Dedicated Usage", pid);
                if (pid != 0)
                    PdhAddEnglishCounterW(query, path, 0, &cVram);
                vramPid = pid;
                PdhCollectQueryData(query);
            }

            core::SystemSample s;
            if (query && PdhCollectQueryData(query) == ERROR_SUCCESS) {
                s.availableRamMB = read(cAvail);
                s.pageInsPerSec = read(cPageIn);
                if (const auto v = read(cLimit))
                    s.cpuPerfLimitPct = core::perfLimitFromCounter(*v);
                if (const auto bytes = readSum(cVram))
                    s.gameVramMB = *bytes / (1024.0 * 1024.0);
            }
            s.totalRamMB = totalRam;
            s.vramTotalMB = vramTotal;
            s.gpu = gpuSample();
            s.qpc = qpcNow();

            lock.lock();
            pending.push_back(s);
        }
        lock.unlock();
        if (query)
            PdhCloseQuery(query);
    }
};

SystemSampler::SystemSampler() : m(std::make_unique<Impl>()) {}
SystemSampler::~SystemSampler() { stop(); }
bool SystemSampler::isSupported() noexcept { return true; }

bool SystemSampler::start(int intervalMs)
{
    if (m->worker.joinable())
        return true;
    m->intervalMs = std::max(50, intervalMs);
    {
        std::lock_guard lock(m->mutex);
        m->stopRequested = false;
        m->pending.clear();
    }
    m->running = true;
    m->worker = std::thread([impl = m.get()] {
        impl->run();
        impl->running = false;
    });
    return true;
}

void SystemSampler::stop()
{
    if (!m->worker.joinable())
        return;
    {
        std::lock_guard lock(m->mutex);
        m->stopRequested = true;
    }
    m->wake.notify_all();
    m->worker.join();
}

bool SystemSampler::isRunning() const noexcept { return m->running; }
void SystemSampler::setTargetProcess(std::uint32_t pid) { m->targetPid = pid; }

std::vector<core::SystemSample> SystemSampler::drain()
{
    std::lock_guard lock(m->mutex);
    return std::exchange(m->pending, {});
}

std::string SystemSampler::status() const
{
    std::lock_guard lock(m->mutex);
    return m->status;
}

core::CpuTopology queryCpuTopology()
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (len == 0)
        return {};
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationCache,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
        return {};

    std::vector<std::uint8_t> ccdOfCpu;
    std::vector<std::uint32_t> l3;
    for (DWORD off = 0; off < len;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Relationship == RelationCache && info->Cache.Level == 3 &&
            info->Cache.GroupMask.Group == 0 && l3.size() < core::kMaxCcds) {
            const auto ccd = static_cast<std::uint8_t>(l3.size());
            l3.push_back(info->Cache.CacheSize / 1024);
            const KAFFINITY mask = info->Cache.GroupMask.Mask;
            for (std::size_t cpu = 0; cpu < sizeof(KAFFINITY) * 8; ++cpu) {
                if (mask & (KAFFINITY{1} << cpu)) {
                    if (ccdOfCpu.size() <= cpu)
                        ccdOfCpu.resize(cpu + 1, 0);
                    ccdOfCpu[cpu] = ccd;
                }
            }
        }
        off += info->Size;
    }
    return core::makeTopology(ccdOfCpu, l3);
}

std::string processName(std::uint32_t pid)
{
    static std::mutex mutex;
    static std::map<std::uint32_t, std::string> cache;
    {
        std::lock_guard lock(mutex);
        if (const auto it = cache.find(pid); it != cache.end())
            return it->second;
    }

    std::string name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &size)) {
            const wchar_t* file = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\')
                    file = p + 1;
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                name.resize(static_cast<std::size_t>(bytes - 1));
                WideCharToMultiByte(CP_UTF8, 0, file, -1, name.data(), bytes, nullptr, nullptr);
            }
        }
        CloseHandle(h);
    }
    if (!name.empty()) { // don't cache failures: the process may be reachable later
        std::lock_guard lock(mutex);
        cache[pid] = name;
    }
    return name;
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

double processCpuSeconds(std::uint32_t pid)
{
    HANDLE h = pid == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return -1.0;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(h, &created, &exited, &kernel, &user);
    if (pid != 0)
        CloseHandle(h);
    if (!ok)
        return -1.0;
    const auto ticks = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
}

std::uint32_t currentProcessId()
{
    return GetCurrentProcessId();
}

bool keepOffVCacheCores(const core::CpuTopology& topology)
{
    if (!topology.hasAsymmetricCache())
        return false;
    DWORD_PTR mask = 0;
    for (std::size_t cpu = 0; cpu < topology.ccdOfCpu.size() && cpu < sizeof(DWORD_PTR) * 8; ++cpu) {
        if (topology.ccdOfCpu[cpu] != topology.vcacheCcd)
            mask |= DWORD_PTR{1} << cpu;
    }
    // Child processes started afterwards (PresentMon) inherit the mask.
    return mask != 0 && SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
}

void tieToThisProcess(std::uint32_t pid)
{
    // One job for all children: when FrameScope goes away for any reason
    // (crash, task manager), Windows ends them too.
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return j;
    }();
    if (!job)
        return;
    if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid)) {
        AssignProcessToJobObject(job, h);
        CloseHandle(h);
    }
}

void deleteLegacyRegistrySettings()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\FrameScope");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\FrameScope");
}

} // namespace framescope::win

#else // ---------------------------------------------------------------------

namespace framescope::win {

struct SystemSampler::Impl {};
SystemSampler::SystemSampler() : m(std::make_unique<Impl>()) {}
SystemSampler::~SystemSampler() = default;
bool SystemSampler::isSupported() noexcept { return false; }
bool SystemSampler::start(int) { return false; }
void SystemSampler::stop() {}
bool SystemSampler::isRunning() const noexcept { return false; }
void SystemSampler::setTargetProcess(std::uint32_t) {}
std::vector<core::SystemSample> SystemSampler::drain() { return {}; }
std::string SystemSampler::status() const { return "System sampling is only available on Windows"; }
core::CpuTopology queryCpuTopology() { return {}; }
std::string processName(std::uint32_t) { return {}; }
bool isElevated() { return false; }
void deleteLegacyRegistrySettings() {}
double processCpuSeconds(std::uint32_t) { return -1.0; }
std::uint32_t currentProcessId() { return 0; }
bool keepOffVCacheCores(const core::CpuTopology&) { return false; }
void tieToThisProcess(std::uint32_t) {}

} // namespace framescope::win

#endif
