#include "etw/KernelTrace.h"

#include <format>

namespace framescope::etw {

std::string describe(const TraceDiagnostics& d)
{
    const double unresolvedPct = d.dpcIsr ? 100.0 * static_cast<double>(d.dpcIsrUnresolved) / static_cast<double>(d.dpcIsr) : 0.0;
    std::string out;
    out += std::format("Events total        {}\n", d.eventsTotal);
    out += std::format("DPC/ISR             {} ({:.1f}% not resolved to a driver)\n", d.dpcIsr, unresolvedPct);
    out += std::format("Known drivers       {}\n", d.knownDrivers);
    out += std::format("Hard faults         {}\n", d.hardFaults);
    out += std::format("Disk I/O            {}\n", d.diskIo);
    out += std::format("Context switches    {}\n", d.contextSwitches);
    out += std::format("CPU buckets         {}\n", d.cpuBucketsFinished);
    out += std::format("Thread events       {} ({} threads known)\n", d.threadEvents, d.knownThreads);
    out += std::format("Image events        {}\n", d.imageEvents);
    out += std::format("File creates        {}\n", d.fileCreates);
    out += std::format("Shader cache writes {}\n", d.shaderCacheWrites);
    out += std::format("Malformed events    {}\n", d.malformed);
    out += std::format("Events lost         {} (real-time buffers lost {})\n", d.eventsLost, d.realtimeBuffersLost);
    return out;
}

} // namespace framescope::etw

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace framescope::etw {
namespace {

// Classic kernel event classes (MOF). In a kernel session these arrive as
// EventHeader.ProviderId, with the event type in EventDescriptor.Opcode.
constexpr GUID kPerfInfoGuid  = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
constexpr GUID kPageFaultGuid = {0x3d6fa8d3, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kDiskIoGuid    = {0x3d6fa8d4, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kThreadGuid    = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kImageGuid     = {0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};

// Session GUIDs: our own for the private system logger,
// SystemTraceControlGuid for the classic NT Kernel Logger.
constexpr GUID kSessionGuid           = {0x6f1c3a52, 0x9d4e, 0x4b8a, {0xa1, 0x7e, 0x2c, 0x55, 0x90, 0x3b, 0x1d, 0x64}};
constexpr GUID kSystemTraceControlGuid = {0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};

constexpr wchar_t kPrivateSessionName[] = L"FrameScope-Kernel";
constexpr wchar_t kNtKernelLoggerName[] = L"NT Kernel Logger";

// Opcodes
constexpr UCHAR kOpThreadDpc = 66, kOpIsr = 67, kOpDpc = 68, kOpTimerDpc = 69;
constexpr UCHAR kOpHardFault = 32;
constexpr UCHAR kOpDiskRead = 10, kOpDiskWrite = 11;
constexpr UCHAR kOpStart = 1, kOpEnd = 2, kOpDcStart = 3, kOpDcEnd = 4;
constexpr UCHAR kOpImageLoad = 10, kOpImageUnload = 2;
constexpr UCHAR kOpCSwitch = 36;

constexpr std::int64_t kBucketsPerSecond = 100;   // 10 ms buckets
constexpr std::int64_t kBucketFinalizeLag = 200;  // keep buckets open ~2 s for late CPUs

template <typename T>
T read(const BYTE* data, std::size_t offset)
{
    T v{};
    std::memcpy(&v, data + offset, sizeof(T));
    return v;
}

std::uint64_t readPtr(const BYTE* data, std::size_t offset, std::size_t ptrSize)
{
    return ptrSize == 8 ? read<std::uint64_t>(data, offset) : read<std::uint32_t>(data, offset);
}

std::string narrow(const wchar_t* s, std::size_t maxChars)
{
    const auto len = static_cast<int>(wcsnlen(s, maxChars));
    if (len == 0)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string fileNameOf(const std::string& path)
{
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string win32Error(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = buf ? narrow(buf, 1024) : std::string{};
    if (buf)
        LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == '.'))
        msg.pop_back();
    return msg + " (" + std::to_string(code) + ")";
}

// EVENT_TRACE_PROPERTIES followed by room for the session name.
struct Properties {
    std::vector<BYTE> buffer;

    Properties(const wchar_t* name, const GUID& guid, ULONG logFileMode, ULONG enableFlags)
        : buffer(sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(name) + 1) * sizeof(wchar_t) + 1024)
    {
        auto* p = get();
        p->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1; // QPC timestamps
        p->Wnode.Guid = guid;
        p->LogFileMode = logFileMode;
        p->EnableFlags = enableFlags;
        p->BufferSize = 1024; // KB
        p->MinimumBuffers = 32;
        p->MaximumBuffers = 256;
        p->FlushTimer = 1; // seconds
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    }

    EVENT_TRACE_PROPERTIES* get() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data()); }
};

struct Module {
    std::uint64_t end = 0;
    std::string name;
};

} // namespace

struct KernelTrace::Impl {
    KernelTraceOptions opts;
    std::int64_t freq = KernelTrace::qpcFrequency();

    std::wstring sessionName;
    GUID sessionGuid{};
    ULONG logFileMode = 0;
    ULONG enableFlags = 0;
    TRACEHANDLE session = 0;
    TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker;
    std::atomic<bool> running{false};

    std::atomic<std::int64_t> startQpc{0};
    std::atomic<std::int64_t> lastQpc{0};
    std::atomic<std::uint64_t> dpcIsrCount{0};

    // Diagnostics counters (written on the callback thread, read anywhere)
    struct Counters {
        std::atomic<std::uint64_t> total{0}, unresolved{0}, hardFaults{0}, diskIo{0}, cswitch{0}, thread{0},
            image{0}, fileCreates{0}, shaderWrites{0}, malformed{0}, buckets{0}, threads{0}, drivers{0},
            eventsLost{0}, buffersLost{0};
        void reset()
        {
            for (auto* c : {&total, &unresolved, &hardFaults, &diskIo, &cswitch, &thread, &image, &fileCreates,
                            &shaderWrites, &malformed, &buckets, &threads, &drivers, &eventsLost, &buffersLost})
                c->store(0);
        }
    } counters;
    static void bump(std::atomic<std::uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

    mutable std::mutex mutex; // guards everything below
    std::vector<core::SystemEvent> pending;
    core::LatencyStats stats;
    std::string error;
    std::string description;

    // Only touched on the ETW callback thread.
    std::unordered_map<std::uint32_t, std::uint32_t> threadToProcess;
    std::map<std::uint64_t, Module> modules; // kernel images by base address
    std::uint64_t lastRoutine = 0;
    std::string lastRoutineModule;

    // ---- CPU scheduling (callback thread only, except finishedBuckets)
    struct CpuState {
        std::uint32_t tid = 0;
        std::uint32_t pid = 0;
        std::int64_t since = 0;
        bool valid = false;
    };
    struct OpenBucket {
        core::CpuBucket bucket;
        std::unordered_map<std::uint32_t, double> background;
    };
    std::atomic<std::uint32_t> targetPid{0};
    std::int64_t bucketTicks = 1;
    std::vector<CpuState> cpus;
    std::map<std::int64_t, OpenBucket> openBuckets;
    std::unordered_map<std::uint32_t, std::uint16_t> lastCpuOfThread;
    std::int64_t maxBucketSeen = 0;
    std::vector<core::CpuBucket> finishedBuckets; // guarded by mutex

    std::uint8_t ccdOf(std::size_t cpu) const
    {
        if (cpu >= opts.ccdOfCpu.size())
            return 0;
        return std::min<std::uint8_t>(opts.ccdOfCpu[cpu], core::kMaxCcds - 1);
    }

    OpenBucket& bucketAt(std::int64_t idx)
    {
        auto [it, inserted] = openBuckets.try_emplace(idx);
        if (inserted) {
            it->second.bucket.startQpc = idx * bucketTicks;
            it->second.bucket.endQpc = (idx + 1) * bucketTicks;
        }
        maxBucketSeen = std::max(maxBucketSeen, idx);
        return it->second;
    }

    void account(const CpuState& st, std::size_t cpu, std::int64_t end)
    {
        if (st.tid == 0 || end <= st.since || end - st.since > 10 * freq)
            return; // idle thread, or a nonsense interval
        const auto target = targetPid.load(std::memory_order_relaxed);
        const bool game = target != 0 && st.pid == target;
        const auto ccd = ccdOf(cpu);

        for (auto idx = st.since / bucketTicks; idx * bucketTicks < end; ++idx) {
            const auto a = std::max(st.since, idx * bucketTicks);
            const auto b = std::min(end, (idx + 1) * bucketTicks);
            const auto us = static_cast<float>(static_cast<double>(b - a) * 1e6 / static_cast<double>(freq));
            auto& ob = bucketAt(idx);
            if (game) {
                ob.bucket.gameUsByCcd[ccd] += us;
            } else {
                ob.bucket.backgroundUs += us;
                ob.background[st.pid] += us;
            }
        }
    }

    void finalize(std::map<std::int64_t, OpenBucket>::iterator it)
    {
        auto& ob = it->second;
        for (const auto& [pid, us] : ob.background) {
            if (us > ob.bucket.topBackgroundUs) {
                ob.bucket.topBackgroundUs = static_cast<float>(us);
                ob.bucket.topBackgroundPid = pid;
            }
        }
        bump(counters.buckets);
        std::lock_guard lock(mutex);
        finishedBuckets.push_back(ob.bucket);
    }

    void finalizeOldBuckets(bool all)
    {
        while (!openBuckets.empty() &&
               (all || openBuckets.begin()->first < maxBucketSeen - kBucketFinalizeLag)) {
            finalize(openBuckets.begin());
            openBuckets.erase(openBuckets.begin());
        }
    }

    void onContextSwitch(std::size_t cpu, std::int64_t ts, std::uint32_t newTid)
    {
        if (cpu >= cpus.size())
            cpus.resize(cpu + 1);
        auto& st = cpus[cpu];
        if (st.valid)
            account(st, cpu, ts);

        const std::uint32_t newPid = newTid == 0 ? 0 : processOf(newTid);
        const auto target = targetPid.load(std::memory_order_relaxed);
        if (target != 0 && newPid == target) {
            const auto c = static_cast<std::uint16_t>(cpu);
            const auto it = lastCpuOfThread.find(newTid);
            if (it != lastCpuOfThread.end() && it->second != c) {
                auto& ob = bucketAt(ts / bucketTicks);
                ++ob.bucket.gameMigrations;
                if (ccdOf(it->second) != ccdOf(cpu))
                    ++ob.bucket.gameCrossCcdMigrations;
            }
            lastCpuOfThread[newTid] = c;
        }
        st = CpuState{newTid, newPid, ts, true};
        finalizeOldBuckets(false);
    }

    static void WINAPI callback(PEVENT_RECORD r)
    {
        static_cast<Impl*>(r->UserContext)->onEvent(*r);
    }

    std::string resolve(std::uint64_t address)
    {
        // DPCs from the same driver come in bursts; cache the last lookup.
        if (address == lastRoutine)
            return lastRoutineModule;
        std::string name = "unknown";
        auto it = modules.upper_bound(address);
        if (it != modules.begin()) {
            --it;
            if (address < it->second.end)
                name = it->second.name;
        }
        lastRoutine = address;
        lastRoutineModule = name;
        return name;
    }

    std::uint32_t processOf(std::uint32_t tid) const
    {
        const auto it = threadToProcess.find(tid);
        return it == threadToProcess.end() ? 0 : it->second;
    }

    void push(core::SystemEvent e)
    {
        std::lock_guard lock(mutex);
        pending.push_back(std::move(e));
    }

    void onEvent(const EVENT_RECORD& r)
    {
        const auto& h = r.EventHeader;
        const auto op = h.EventDescriptor.Opcode;
        const auto* data = static_cast<const BYTE*>(r.UserData);
        const std::size_t len = r.UserDataLength;
        const std::size_t ptr = (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
        const std::int64_t ts = h.TimeStamp.QuadPart;

        if (ts > lastQpc.load(std::memory_order_relaxed))
            lastQpc.store(ts, std::memory_order_relaxed);
        bump(counters.total);
        if (!data)
            return;

        const auto& g = h.ProviderId;

        if (IsEqualGUID(g, kPerfInfoGuid)) {
            // DPC / ISR: InitialTime (8), Routine (ptr), ... ; logged at completion.
            core::EventKind kind;
            if (op == kOpDpc || op == kOpThreadDpc || op == kOpTimerDpc)
                kind = core::EventKind::Dpc;
            else if (op == kOpIsr)
                kind = core::EventKind::Isr;
            else
                return;
            if (len < 8 + ptr)
                return bump(counters.malformed);

            const auto initial = read<std::int64_t>(data, 0);
            const auto duration = ts - initial;
            if (duration < 0 || duration > freq) // > 1 s: corrupt or clock mismatch
                return bump(counters.malformed);

            dpcIsrCount.fetch_add(1, std::memory_order_relaxed);
            auto module = resolve(readPtr(data, 8, ptr));
            if (module == "unknown")
                bump(counters.unresolved);
            const double us = static_cast<double>(duration) * 1e6 / static_cast<double>(freq);

            std::lock_guard lock(mutex);
            stats.add(kind, module, us);
            if (us >= opts.keepDpcIsrOverUs)
                pending.push_back({kind, initial, ts, 0, 0, std::move(module)});
            return;
        }

        if (IsEqualGUID(g, kPageFaultGuid) && op == kOpHardFault) {
            // InitialTime (8), ReadOffset (8), VirtualAddress (ptr), FileObject (ptr),
            // TThreadId (4), ByteCount (4)
            if (len < 16 + 2 * ptr + 8)
                return bump(counters.malformed);
            const auto initial = read<std::int64_t>(data, 0);
            if (initial > ts || ts - initial > 10 * freq)
                return bump(counters.malformed);
            bump(counters.hardFaults);
            const auto tid = read<std::uint32_t>(data, 16 + 2 * ptr);
            const auto bytes = read<std::uint32_t>(data, 16 + 2 * ptr + 4);
            push({core::EventKind::HardFault, initial, ts, processOf(tid), bytes, {}});
            return;
        }

        if (IsEqualGUID(g, kDiskIoGuid) && (op == kOpDiskRead || op == kOpDiskWrite)) {
            // DiskNumber (4), IrpFlags (4), TransferSize (4), Reserved (4), ByteOffset (8),
            // FileObject (ptr), Irp (ptr), HighResResponseTime (8), IssuingThreadId (4)
            const std::size_t respOff = 24 + 2 * ptr;
            if (len < respOff + 8)
                return bump(counters.malformed);
            const auto bytes = read<std::uint32_t>(data, 8);
            const auto response = static_cast<std::int64_t>(read<std::uint64_t>(data, respOff));
            if (response < 0 || response > 60 * freq)
                return bump(counters.malformed);
            bump(counters.diskIo);
            const std::uint32_t tid = len >= respOff + 12 ? read<std::uint32_t>(data, respOff + 8) : 0;
            push({core::EventKind::DiskIo, ts - response, ts, processOf(tid), bytes, {}});
            return;
        }

        if (IsEqualGUID(g, kThreadGuid)) {
            if (op == kOpCSwitch) {
                // NewThreadId (4), OldThreadId (4), priorities/states ...
                if (len < 8)
                    return bump(counters.malformed);
                bump(counters.cswitch);
                onContextSwitch(GetEventProcessorIndex(&r), ts, read<std::uint32_t>(data, 0));
                return;
            }
            // ProcessId (4), TThreadId (4), ...
            if (len < 8)
                return bump(counters.malformed);
            bump(counters.thread);
            const auto pid = read<std::uint32_t>(data, 0);
            const auto tid = read<std::uint32_t>(data, 4);
            if (op == kOpStart || op == kOpDcStart)
                threadToProcess[tid] = pid;
            else if (op == kOpEnd || op == kOpDcEnd) {
                threadToProcess.erase(tid);
                lastCpuOfThread.erase(tid);
            }
            counters.threads.store(threadToProcess.size(), std::memory_order_relaxed);
            return;
        }

        if (IsEqualGUID(g, kImageGuid)) {
            // ImageBase (ptr), ImageSize (ptr), ProcessId (4), ImageCheckSum (4),
            // TimeDateStamp (4), Reserved0 (4), DefaultBase (ptr), Reserved1..4 (16),
            // FileName (wchar_t[])
            const std::size_t nameOff = 2 * ptr + 16 + ptr + 16;
            if (len < nameOff)
                return bump(counters.malformed);
            bump(counters.image);
            const auto base = readPtr(data, 0, ptr);
            const bool kernelSpace = ptr == 8 ? (base >> 63) != 0 : (base >> 31) != 0;
            if (!kernelSpace)
                return; // user-mode DLLs don't run DPCs

            if (op == kOpImageLoad || op == kOpDcStart) {
                const auto size = readPtr(data, ptr, ptr);
                const auto* name = reinterpret_cast<const wchar_t*>(data + nameOff);
                modules[base] = Module{base + size, fileNameOf(narrow(name, (len - nameOff) / sizeof(wchar_t)))};
                lastRoutine = 0;
            } else if (op == kOpImageUnload) {
                modules.erase(base);
                lastRoutine = 0;
            }
            counters.drivers.store(modules.size(), std::memory_order_relaxed);
        }
    }

    bool startSession(const wchar_t* name, const GUID& guid, ULONG mode)
    {
        // A crashed previous run may have left the session alive.
        Properties stopProps(name, guid, mode, 0);
        ControlTraceW(0, name, stopProps.get(), EVENT_TRACE_CONTROL_STOP);

        Properties props(name, guid, mode, enableFlags);
        const ULONG status = StartTraceW(&session, name, props.get());
        if (status != ERROR_SUCCESS) {
            std::lock_guard lock(mutex);
            error = status == ERROR_ACCESS_DENIED
                        ? "Access denied: run FrameScope as administrator"
                        : "StartTrace failed: " + win32Error(status);
            return false;
        }
        sessionName = name;
        sessionGuid = guid;
        logFileMode = mode;
        return true;
    }
};

KernelTrace::KernelTrace()
    : m(std::make_unique<Impl>())
{
}

KernelTrace::~KernelTrace()
{
    stop();
}

bool KernelTrace::isSupported() noexcept { return true; }

std::int64_t KernelTrace::qpcFrequency() noexcept
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

std::int64_t KernelTrace::qpcNow() noexcept
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

bool KernelTrace::start(const KernelTraceOptions& options)
{
    if (isRunning())
        return true;

    m->opts = options;
    m->enableFlags = EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT | EVENT_TRACE_FLAG_THREAD |
                     EVENT_TRACE_FLAG_IMAGE_LOAD;
    if (options.hardFaults)
        m->enableFlags |= EVENT_TRACE_FLAG_MEMORY_HARD_FAULTS;
    if (options.diskIo)
        m->enableFlags |= EVENT_TRACE_FLAG_DISK_IO;
    if (options.cpuScheduling)
        m->enableFlags |= EVENT_TRACE_FLAG_CSWITCH;

    {
        std::lock_guard lock(m->mutex);
        m->pending.clear();
        m->stats.clear();
        m->error.clear();
        m->finishedBuckets.clear();
    }
    m->bucketTicks = std::max<std::int64_t>(1, m->freq / kBucketsPerSecond);
    m->cpus.clear();
    m->openBuckets.clear();
    m->lastCpuOfThread.clear();
    m->maxBucketSeen = 0;
    m->threadToProcess.clear();
    m->modules.clear();
    m->lastRoutine = 0;
    m->dpcIsrCount = 0;
    m->counters.reset();

    // Prefer a private system logger session (Windows 8+), so we don't fight
    // LatencyMon & co. over the single NT Kernel Logger.
    bool ok = false;
    if (!options.forceNtKernelLogger) {
        ok = m->startSession(kPrivateSessionName, kSessionGuid,
                             EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE);
        if (ok)
            m->description = "private system logger session";
    }
    if (!ok && lastError().rfind("Access denied", 0) != 0) {
        ok = m->startSession(kNtKernelLoggerName, kSystemTraceControlGuid, EVENT_TRACE_REAL_TIME_MODE);
        if (ok)
            m->description = "NT Kernel Logger";
    }
    if (!ok)
        return false;

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<wchar_t*>(m->sessionName.c_str());
    logfile.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = &Impl::callback;
    logfile.Context = m.get();

    m->consumer = OpenTraceW(&logfile);
    if (m->consumer == INVALID_PROCESSTRACE_HANDLE) {
        const auto err = GetLastError();
        {
            std::lock_guard lock(m->mutex);
            m->error = "OpenTrace failed: " + win32Error(err);
        }
        Properties props(m->sessionName.c_str(), m->sessionGuid, m->logFileMode, 0);
        ControlTraceW(m->session, nullptr, props.get(), EVENT_TRACE_CONTROL_STOP);
        return false;
    }

    m->startQpc = qpcNow();
    m->lastQpc = m->startQpc.load();
    m->running = true;
    m->worker = std::thread([impl = m.get()] {
        ProcessTrace(&impl->consumer, 1, nullptr, nullptr); // blocks until the session stops
        impl->finalizeOldBuckets(true);
        impl->running = false;
    });
    return true;
}

void KernelTrace::stop()
{
    if (!m->worker.joinable())
        return;

    Properties props(m->sessionName.c_str(), m->sessionGuid, m->logFileMode, 0);
    if (ControlTraceW(m->session, nullptr, props.get(), EVENT_TRACE_CONTROL_STOP) == ERROR_SUCCESS) {
        m->counters.eventsLost = props.get()->EventsLost;
        m->counters.buffersLost = props.get()->RealTimeBuffersLost;
    }
    m->worker.join();
    CloseTrace(m->consumer);
    m->consumer = INVALID_PROCESSTRACE_HANDLE;
    m->running = false;
}

bool KernelTrace::isRunning() const noexcept { return m->running; }

std::vector<core::SystemEvent> KernelTrace::drainEvents()
{
    std::lock_guard lock(m->mutex);
    return std::exchange(m->pending, {});
}

std::vector<core::CpuBucket> KernelTrace::drainCpuBuckets()
{
    std::lock_guard lock(m->mutex);
    return std::exchange(m->finishedBuckets, {});
}

void KernelTrace::setTargetProcess(std::uint32_t pid) noexcept
{
    m->targetPid = pid;
}

std::int64_t KernelTrace::startQpc() const noexcept { return m->startQpc; }
std::int64_t KernelTrace::lastEventQpc() const noexcept { return m->lastQpc; }
std::uint64_t KernelTrace::dpcIsrCount() const noexcept { return m->dpcIsrCount; }

core::LatencyStats KernelTrace::latencySnapshot() const
{
    std::lock_guard lock(m->mutex);
    return m->stats;
}

std::string KernelTrace::lastError() const
{
    std::lock_guard lock(m->mutex);
    return m->error;
}

TraceDiagnostics KernelTrace::diagnostics() const
{
    // While running, ask ETW for the current loss counters.
    if (isRunning()) {
        Properties props(m->sessionName.c_str(), m->sessionGuid, m->logFileMode, 0);
        if (ControlTraceW(m->session, nullptr, props.get(), EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS) {
            m->counters.eventsLost = props.get()->EventsLost;
            m->counters.buffersLost = props.get()->RealTimeBuffersLost;
        }
    }
    const auto& c = m->counters;
    TraceDiagnostics d;
    d.eventsTotal = c.total;
    d.dpcIsr = m->dpcIsrCount;
    d.dpcIsrUnresolved = c.unresolved;
    d.hardFaults = c.hardFaults;
    d.diskIo = c.diskIo;
    d.contextSwitches = c.cswitch;
    d.threadEvents = c.thread;
    d.imageEvents = c.image;
    d.fileCreates = c.fileCreates;
    d.shaderCacheWrites = c.shaderWrites;
    d.malformed = c.malformed;
    d.cpuBucketsFinished = c.buckets;
    d.knownThreads = c.threads;
    d.knownDrivers = c.drivers;
    d.eventsLost = c.eventsLost;
    d.realtimeBuffersLost = c.buffersLost;
    return d;
}

std::string KernelTrace::sessionDescription() const
{
    std::lock_guard lock(m->mutex);
    return m->description;
}

} // namespace framescope::etw

#else // ---------------------------------------------------------------------
// Non-Windows stub: keeps the GUI and tests building on Linux.

#include <chrono>

namespace framescope::etw {

struct KernelTrace::Impl {};

KernelTrace::KernelTrace() : m(std::make_unique<Impl>()) {}
KernelTrace::~KernelTrace() = default;
bool KernelTrace::isSupported() noexcept { return false; }
std::int64_t KernelTrace::qpcFrequency() noexcept { return 1'000'000'000; }
std::int64_t KernelTrace::qpcNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool KernelTrace::start(const KernelTraceOptions&) { return false; }
void KernelTrace::stop() {}
bool KernelTrace::isRunning() const noexcept { return false; }
std::vector<core::SystemEvent> KernelTrace::drainEvents() { return {}; }
std::vector<core::CpuBucket> KernelTrace::drainCpuBuckets() { return {}; }
void KernelTrace::setTargetProcess(std::uint32_t) noexcept {}
std::int64_t KernelTrace::startQpc() const noexcept { return 0; }
std::int64_t KernelTrace::lastEventQpc() const noexcept { return 0; }
std::uint64_t KernelTrace::dpcIsrCount() const noexcept { return 0; }
core::LatencyStats KernelTrace::latencySnapshot() const { return {}; }
std::string KernelTrace::lastError() const { return "Kernel tracing is only available on Windows"; }
std::string KernelTrace::sessionDescription() const { return {}; }
TraceDiagnostics KernelTrace::diagnostics() const { return {}; }

} // namespace framescope::etw

#endif
