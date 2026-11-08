#include "etw/KernelTrace.h"

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
                return;

            const auto initial = read<std::int64_t>(data, 0);
            const auto duration = ts - initial;
            if (duration < 0 || duration > freq) // > 1 s: corrupt or clock mismatch
                return;

            dpcIsrCount.fetch_add(1, std::memory_order_relaxed);
            auto module = resolve(readPtr(data, 8, ptr));
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
                return;
            const auto initial = read<std::int64_t>(data, 0);
            if (initial > ts || ts - initial > 10 * freq)
                return;
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
                return;
            const auto bytes = read<std::uint32_t>(data, 8);
            const auto response = static_cast<std::int64_t>(read<std::uint64_t>(data, respOff));
            if (response < 0 || response > 60 * freq)
                return;
            const std::uint32_t tid = len >= respOff + 12 ? read<std::uint32_t>(data, respOff + 8) : 0;
            push({core::EventKind::DiskIo, ts - response, ts, processOf(tid), bytes, {}});
            return;
        }

        if (IsEqualGUID(g, kThreadGuid)) {
            // ProcessId (4), TThreadId (4), ...
            if (len < 8)
                return;
            const auto pid = read<std::uint32_t>(data, 0);
            const auto tid = read<std::uint32_t>(data, 4);
            if (op == kOpStart || op == kOpDcStart)
                threadToProcess[tid] = pid;
            else if (op == kOpEnd || op == kOpDcEnd)
                threadToProcess.erase(tid);
            return;
        }

        if (IsEqualGUID(g, kImageGuid)) {
            // ImageBase (ptr), ImageSize (ptr), ProcessId (4), ImageCheckSum (4),
            // TimeDateStamp (4), Reserved0 (4), DefaultBase (ptr), Reserved1..4 (16),
            // FileName (wchar_t[])
            const std::size_t nameOff = 2 * ptr + 16 + ptr + 16;
            if (len < nameOff)
                return;
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

    {
        std::lock_guard lock(m->mutex);
        m->pending.clear();
        m->stats.clear();
        m->error.clear();
    }
    m->threadToProcess.clear();
    m->modules.clear();
    m->lastRoutine = 0;
    m->dpcIsrCount = 0;

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
        impl->running = false;
    });
    return true;
}

void KernelTrace::stop()
{
    if (!m->worker.joinable())
        return;

    Properties props(m->sessionName.c_str(), m->sessionGuid, m->logFileMode, 0);
    ControlTraceW(m->session, nullptr, props.get(), EVENT_TRACE_CONTROL_STOP);
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
std::int64_t KernelTrace::startQpc() const noexcept { return 0; }
std::int64_t KernelTrace::lastEventQpc() const noexcept { return 0; }
std::uint64_t KernelTrace::dpcIsrCount() const noexcept { return 0; }
core::LatencyStats KernelTrace::latencySnapshot() const { return {}; }
std::string KernelTrace::lastError() const { return "Kernel tracing is only available on Windows"; }
std::string KernelTrace::sessionDescription() const { return {}; }

} // namespace framescope::etw

#endif
