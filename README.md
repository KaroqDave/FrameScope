# FrameScope

Tells you whether game stutter comes from **your PC** or from **the game**.

FrameScope records every frame with [PresentMon](https://github.com/GameTechDev/PresentMon),
detects stutter spikes, and (from M2 on) matches each spike against what the system was doing
at that moment: DPC latency, paging, throttling, background load, CCD placement.

No injection, no overlay hooks. Everything comes from ETW, so it's safe with anti-cheat.

## Status: M1

| Milestone | Scope | State |
|---|---|---|
| **M1** | PresentMon capture, spike detection, frame-time graph | ✅ done |
| M2 | ETW kernel trace (krabsetw): DPC/ISR, hard faults, disk I/O; match events to spikes | next |
| M3 | Throttling (NVML/ADLX, PDH), VRAM, background processes, X3D CCD check | |
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

## Build

Requirements: CMake ≥ 3.25, a C++23 compiler (GCC 13 / MSVC 17.8+), Qt ≥ 6.4 (Widgets).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64
cmake --build build
ctest --test-dir build
```

Options: `FRAMESCOPE_BUILD_GUI`, `FRAMESCOPE_BUILD_CLI`, `FRAMESCOPE_BUILD_TESTS` (all `ON`).
The core library and tests are pure C++ with no Qt, so they also build on Linux.

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

- PresentMon flags used: `--process_name`, `--output_stdout`, `--session_name`,
  `--stop_existing_session`, `--terminate_existing_session` (in
  `src/capture/PresentMonCapture.cpp`). Check them against `PresentMon --help` if you use a
  different version.
- The parser locks onto the **first swap chain** it sees in the process. Launchers or overlays
  that present first could get picked by mistake; a smarter pick is planned.
- Generated frames (`FrameType` other than `Application`) are ignored.

## Layout

```
src/core/      pure C++: CSV parser, spike detector, stats, session timeline
src/capture/   PresentMon process wrapper (Qt)
src/ui/        main window, frame-time graph
src/cli/       offline analyser
tests/         dependency-free unit tests (ctest)
```
