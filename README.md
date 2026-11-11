# FrameScope

Tells you whether game stutter comes from **your PC** or from **the game**.

FrameScope records every frame with [PresentMon](https://github.com/GameTechDev/PresentMon),
detects stutter spikes, and (from M2 on) matches each spike against what the system was doing
at that moment: DPC latency, paging, throttling, background load, CCD placement.

No injection, no overlay hooks. Everything comes from ETW, so it's safe with anti-cheat.

## Status: M4

| Milestone | Scope | State |
|---|---|---|
| **M1** | PresentMon capture, spike detection, frame-time graph | ✅ done |
| **M2** | ETW kernel trace: DPC/ISR per driver, hard faults, disk I/O; cause per spike | ✅ done, needs testing on real hardware |
| **M3** | GPU/CPU throttling, RAM/VRAM pressure, background load, X3D CCD check, findings | ✅ done, needs testing on real hardware |
| **M4** | Idle baseline, run comparison, session files, HTML report | ✅ done |

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
| ADLX (from the AMD driver, loaded at runtime) | GPU clock, temperature, hotspot, power, load | GPU throttling (inferred) |

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

**Shader compilation** is detected directly: the trace watches file writes to GPU driver and
D3D shader caches (`DXCache`, `DxcCache`, `D3DSCache`, `shadercache`, engine PSO caches …).
A CPU-side spike where the game writes a cache entry during the frame (or up to 250 ms after)
gets **Shader compilation** (game side) with high confidence. It's checked right after driver
latency. Games that keep their own shader cache somewhere else aren't seen; for those the
run comparison still works.

Each check is **linked to the spikes it explains** ("linked to 12 of 107 spikes"), and rated
by that: a problem behind no spike at all is shown as a warning, a warning behind at least 10% of
the spikes (and at least 3) becomes a problem. Causes with several spikes but no check of their
own (storage waits, the game loading data, the game's own work) get one.

The **Findings** tab checks the whole session: worst driver latency, how much of the game's CPU
time ran on the V-Cache CCD, GPU throttling, minimum free RAM, peak VRAM, CPU limits.

Spikes now get their cause about 4 s after they happen. Context-switch data arrives ~2 s late
because each CPU's ETW buffer is delivered separately.

### AMD GPUs

On AMD cards the GPU sensors come from **ADLX**, which ships with the Adrenalin driver
(`amdadlx64.dll`): clock, temperature, hotspot, board power and load.

ADLX doesn't report *why* clocks drop, so FrameScope infers it:

| Flag | When | Counts as a cause |
|---|---|---|
| Clock drop under full load | GPU load ≥ 90% and clock < 75% of its usual full-load clock | Yes (GPU throttling, slightly lower confidence than NVIDIA's driver-reported reasons) |
| Hotspot near limit | Hotspot ≥ 105 °C | No, shown as a finding (check airflow and fan curve) |

On single-CCD CPUs (e.g. 9800X3D, 9850X3D) the CCD check is skipped: there's only one cache.

**Build notes:** the ADLX SDK is downloaded by CMake at configure time (pinned commit) instead of
living in this repository, because its license only allows distributing compiled code. It's on
by default for MSVC builds (what CI uses) and opt-in for MinGW (`-DFRAMESCOPE_WITH_ADLX=ON`),
since AMD only supports MSVC. `-DFRAMESCOPE_WITH_ADLX=OFF` builds without it. If you publish
binaries that include ADLX, its license asks for an end-user license agreement; the
AMD release package ships with `packaging/EULA-AMD.txt` for that reason.

## What M4 adds

| Feature | Where | What it does |
|---|---|---|
| Idle baseline | **Idle baseline** button / Tools menu | 30 s system trace without the game: driver latency, background CPU load, RAM, GPU state. Shows whether the PC has problems before a game even runs. |
| Sessions | File → Save / Open session | `.fscope` file with frames, spikes, causes, findings and driver stats. Plain tab-separated text. |
| Run comparison | Tools → Compare with earlier run | Compares the current run (run 2) with an earlier one (run 1, `.fscope` or PresentMon CSV). |
| Report | File → Export report | One self-contained HTML file: verdict, stats, frame-time chart, causes, system checks, baseline, comparison, worst spikes, driver latency. |

**How the comparison decides** (both runs must play the same sequence from the same start,
e.g. a benchmark or replay):

| Result | Meaning |
|---|---|
| Run 2 has ≥ 60% fewer spikes | Shader compilation: it fades once the shader cache is warm (game) |
| Same rate, ≥ 60% of spikes at the same moments | Game content such as asset streaming or level traversal (game) |
| Same rate, < 30% at the same moments | Random stutter: the system, or unpredictable game work |
| Anything else | Inconclusive |

The two runs are lined up automatically: the tool tries offsets up to ±10 s and keeps the one
where the most spikes coincide (within 0.5 s).

The CLI can do the same offline:

```sh
framescope-cli run1.fscope --compare run2.csv --report report.html
```

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

## Download

Prebuilt Windows builds are on the [Releases](https://github.com/KaroqDave/FrameScope/releases) page:

| Package | AMD GPU sensors | License |
|---|---|---|
| `FrameScope-…-windows-x64.zip` | No | Third-party notices only |
| `FrameScope-AMD-…-windows-x64.zip` | Yes (ADLX) | `EULA-AMD.txt`: required by AMD's ADLX license, use on AMD-based systems only |

### Making a release

Push a tag `vX.Y.Z` (or `vX.Y.Z-beta.N`), or a branch `release/vX.Y.Z-beta.N`. CI builds both
packages and publishes the release (as a prerelease when the version has a suffix); with the
branch variant it also creates the tag.

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
framescope-cli session.fscope --report report.html
framescope-cli run1.fscope --compare run2.fscope
```

## Frame generation and what reaches the screen

FrameScope judges stutter on the frames the **game renders**. On top of that it looks at what
was actually **shown**:

| Finding | Meaning |
|---|---|
| Frame generation is active | Share of generated frames (AMD AFMF, AMD FSR-FG, Intel XeSS-FG), FPS on screen vs. FPS rendered |
| Rendered frames that never reached the screen | The game renders faster than the display refreshes |
| Uneven frame pacing on screen | Hitches on screen that the game's frame times don't show: the frames were rendered evenly but shown unevenly (frame generation, V-Sync/VRR, frame limiter) |
| Frame pacing on screen matches the game | How many of the spikes were visible on screen |

A screen hitch uses the same rule as a spike, applied to how long each frame stayed on screen.

Generated frames are only reported when PresentMon knows about them: FrameScope passes
`--track_frame_type` if the PresentMon version supports it (it reads `PresentMon --help` once,
because PresentMon refuses to start on unknown options), and the driver or game SDK has to
report its generated frames to PresentMon. Without that, generated frames look like ordinary
frames. Generated frames never count as frames of the game: average FPS, 1% lows and spikes
always refer to rendered frames.

## Files it writes

FrameScope is portable. Its only file of its own is **`framescope.ini` next to the exe**
(PresentMon path, game name, System trace on/off, last folder). If that folder isn't writable,
e.g. under Program Files, the ini goes to the user's config folder instead
(`%LOCALAPPDATA%\FrameScope\FrameScope\framescope.ini`).

Everything else is only written when you save it (sessions, reports, diagnostics). Traces are
streamed into memory; no `.etl` or CSV files are created. While capturing, Windows runs the
trace sessions `FrameScope-Kernel` and `FrameScope`; they end when the capture stops.

Betas 1–5 stored their settings in the registry (`HKCU\Software\FrameScope`); the first start
of a newer version moves them into the ini and deletes that key.

## Reporting problems

**Tools → Save diagnostics…** writes one text file with everything needed to debug a capture:
build and OS, admin rights, CPU topology, sensor sources and recent samples, ETW event counters
(per event type, unresolved drivers, malformed and lost events), the PresentMon command and CSV
header, causes per spike, findings and the log. `framescope-latency` prints the same trace
counters at the end.

## Notes

- **The ETW event layouts are untested on real hardware so far.** They're parsed by hand from
  the documented kernel MOF classes (PerfInfo DPC/ISR, PageFault_HardFault, DiskIo, Thread,
  Image, CSwitch). If `framescope-latency` shows only `unknown` drivers, no events or no CPU
  buckets, that's the place to look (`src/etw/KernelTrace.cpp`).

- PresentMon flags used: `--process_name`, `--output_stdout`, `--qpc_time`, `--session_name`,
  `--stop_existing_session`, `--terminate_existing_session`, plus `--track_frame_type` when
  the version lists it in `--help` (in `src/capture/PresentMonCapture.cpp`).
- The CSV parser reads the columns of all PresentMon versions (2.0–2.3.0, the current default
  and `--v1_metrics`), matching names without regard to case.
- The parser locks onto the **first swap chain** it sees in the process. Launchers or overlays
  that present first could get picked by mistake; a smarter pick is planned.
- Generated frames (`FrameType` other than `Application`) are ignored.

## Layout

```
src/core/      pure C++: CSV parser, spike detector, stats, event timeline, correlator,
               session files, run comparison, HTML report
src/etw/       ETW kernel trace (Windows; stub elsewhere)
src/win/       system sampler (PDH, DXGI, NVML, ADLX), CPU topology, process names
src/capture/   PresentMon process wrapper (Qt)
src/ui/        main window, frame-time graph
src/cli/       offline analyser, latency check
tests/         dependency-free unit tests (ctest)
```
