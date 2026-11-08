# FrameScope

Tells you whether game stutter comes from **your PC** or from **the game**.

FrameScope records every frame with [PresentMon](https://github.com/GameTechDev/PresentMon),
detects stutter spikes, and (from M2 on) matches each spike against what the system was doing
at that moment: DPC latency, paging, throttling, background load, CCD placement.

No injection, no overlay hooks. Everything comes from ETW, so it's safe with anti-cheat.

## Status: M3

| Milestone | Scope | State |
|---|---|---|
| **M1** | PresentMon capture, spike detection, frame-time graph | ✅ done |
| **M2** | ETW kernel trace: DPC/ISR per driver, hard faults, disk I/O; cause per spike | ✅ done, needs testing on real hardware |
| **M3** | GPU/CPU throttling, RAM/VRAM pressure, background load, X3D CCD check, findings | ✅ done, needs testing on real hardware |
| M4 | Baseline tests, repeat-run comparison, verdict report | |

## What M1 does

- Live capture: starts PresentMon for a given `.exe` and streams its CSV output.
- Offline: opens any PresentMon CSV (2.x or 1.x column names).
- **Spike detection:** a frame is a spike if it's **> 2× the rolling median** (120 frames)
  **and ≥ 8 ms above it**.
- **Bound hint** per spike:
  - `CPU` / `GPU`: which side was busy.
  - `Stall`: neither was busy for most of the frame, so something was waiting
    (I/O, paging, driver, sync). That's the main system-side suspect M2 will dig into.
- Stats: average FPS, 1% low, worst frame, spikes per minute.

## What M2 adds

With **System trace** on, FrameScope runs a real-time ETW kernel session next to PresentMon.
Both use raw QPC timestamps (`--qpc_time`), so every spike can be lined up with what the
system did during that exact frame.

Each spike gets a **side**, a **cause**, a **confidence** and the **evidence**:

| Cause | Side | When |
|---|---|---|
| Driver latency | System | DPC/ISR time explains ≥ 15% of the delay, or one DPC/ISR ≥ 2 ms in a frame where neither CPU nor GPU was busy |
| Hard page faults | Mixed | The game's threads waited on hard faults for ≥ 20% of the delay |
| Storage wait | Mixed | A disk I/O during the frame took ≥ 30% of the delay |
| Game CPU work | Game | CPU-bound frame, no big system cause (shader compile, game logic) |
| Game GPU work | Game | GPU-bound frame, no big system cause |
| Unexplained wait | Unknown | Nothing busy, no system event found (sync? M3 adds more signals) |

"Mixed" means it could be either side (e.g. slow storage, or VRAM full because of high settings).

Spikes get their cause about 2 s after they happen, once the trace has delivered that window.
On the graph: red = system, violet = game, amber = mixed, white = pending or unknown.
A verdict line sums up the session ("Mostly your system: 70% of spikes. Top cause: Driver
latency (nvlddmkm.sys)").

The ETW session is a private system logger (Windows 8+), so it doesn't collide with LatencyMon
or xperf. If that fails it falls back to the classic NT Kernel Logger.

## What M3 adds

More signals, sampled 4× per second or built from context switches:

| Source | What | Used for |
|---|---|---|
| ETW context switches | Game CPU time per CCD, thread migrations, other processes' CPU time (10 ms buckets) | CCD scheduling, background load |
| `GetLogicalProcessorInformationEx` | L3 cache groups → which CCD has 3D V-Cache | CCD scheduling |
| PDH (English counter names, works on any Windows language) | Available RAM, page-ins/s, CPU `% Performance Limit`, the game's dedicated VRAM | Low memory, CPU throttling, VRAM full |
| DXGI | Total VRAM | VRAM full |
| NVML (from the NVIDIA driver, loaded at runtime) | GPU clock, temperature, power, throttle reasons | GPU throttling |

New causes per spike, checked in this order after driver latency:

| Cause | Side | When |
|---|---|---|
| GPU throttling | System | NVML reports HW/SW thermal, power brake or HW slowdown (the normal power cap doesn't count) |
| Low memory | System | Game hard faults while free RAM was under 1.5 GB or 5% |
| Hard page faults | Game | Same faults with plenty of free RAM: the game is loading data |
| VRAM full | Mixed | Game uses ≥ 95% of VRAM: lower texture settings |
| CPU throttling | System | `% Performance Limit` ≥ 10% in a CPU-side spike |
| Background load | System | Other processes used ≥ 4 cores on average during the frame, mostly one process |
| CCD scheduling | System | ≥ 40% of the game's CPU time off the V-Cache CCD, or ≥ 3 cross-CCD migrations in the frame |

The **Findings** tab checks the whole session: worst driver latency, how much of the game's CPU
time ran on the V-Cache CCD, GPU throttling, minimum free RAM, peak VRAM, CPU limits.

Spikes now get their cause about 4 s after they happen. Context-switch data arrives ~2 s late
because each CPU's ETW buffer is delivered separately.

AMD GPUs (ADLX) are not covered yet; RAM, VRAM and CPU checks work on any system.

### framescope-latency

A LatencyMon-style system check without a game:

```sh
framescope-latency --seconds 60          # as administrator
framescope-latency --nt-kernel-logger    # force the classic session
```

It prints the worst drivers by single DPC/ISR plus a verdict (under 0.5 ms good, under 2 ms OK,
over 2 ms a problem). Run it idle and again with the game running. It also shows the CPU
topology, which sensors work, and whether context-switch data arrives (~100 buckets/s), which
makes it the quickest way to check M2/M3 on a new machine.

## Build

Requirements: CMake ≥ 3.25, a C++23 compiler (GCC 13 / MSVC 17.8+), Qt ≥ 6.4 (Widgets).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64
cmake --build build
ctest --test-dir build
```

Options: `FRAMESCOPE_BUILD_GUI`, `FRAMESCOPE_BUILD_CLI`, `FRAMESCOPE_BUILD_TESTS` (all `ON`).
The core library and tests are pure C++ with no Qt, so they also build on Linux.
Without Qt (`-DFRAMESCOPE_BUILD_GUI=OFF`) the Windows targets also cross-compile with MinGW.

## Run

1. Download the PresentMon console app (`PresentMon-2.x.x-x64.exe`) from its GitHub releases.
2. Start `framescope.exe`. It asks for admin rights (PresentMon needs them).
3. Point it at PresentMon, enter the game's executable name, then **Start capture**.

Graph controls: mouse wheel zooms, drag pans, double-click returns to live. Click a spike in
the table to jump to it.

> The Windows build requests elevation through its manifest. To debug from Qt Creator,
> run Qt Creator as admin.

### CLI

```sh
framescope-cli capture.csv --top 10
```

## Notes

- **The ETW event layouts are untested on real hardware so far.** They're parsed by hand from
  the documented kernel MOF classes (PerfInfo DPC/ISR, PageFault_HardFault, DiskIo, Thread,
  Image, CSwitch). If `framescope-latency` shows only `unknown` drivers, no events or no CPU
  buckets, that's the place to look (`src/etw/KernelTrace.cpp`).

- PresentMon flags used: `--process_name`, `--output_stdout`, `--session_name`,
  `--stop_existing_session`, `--terminate_existing_session` (in
  `src/capture/PresentMonCapture.cpp`). Check them against `PresentMon --help` if you use a
  different version.
- The parser locks onto the **first swap chain** it sees in the process. Launchers or overlays
  that present first could get picked by mistake; a smarter pick is planned.
- Generated frames (`FrameType` other than `Application`) are ignored.

## Layout

```
src/core/      pure C++: CSV parser, spike detector, stats, event timeline, correlator
src/etw/       ETW kernel trace (Windows; stub elsewhere)
src/win/       system sampler (PDH, DXGI, NVML), CPU topology, process names
src/capture/   PresentMon process wrapper (Qt)
src/ui/        main window, frame-time graph
src/cli/       offline analyser, latency check
tests/         dependency-free unit tests (ctest)
```
