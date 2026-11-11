# How FrameScope works

The numbers below are the current defaults. They were set by reasoning and a small number of
real captures and may still change.

## Data sources

| Source | What | Used for |
|---|---|---|
| PresentMon (child process, CSV over stdout, `--qpc_time`) | Every present: frame time, CPU busy, GPU busy, time on screen, frame type | Frames, spikes, display pacing, frame generation |
| ETW kernel session (private system logger; NT Kernel Logger as fallback) | DPC/ISR per driver, hard page faults, disk I/O, context switches, image loads, file I/O, sampled CPU profile | Causes per spike |
| PDH counters (English names, any Windows language) | Free RAM, page-ins, CPU performance limit, the game's VRAM | Low memory, CPU throttling, VRAM full |
| DXGI | Total VRAM | VRAM full |
| NVML / ADLX (loaded from the GPU driver at runtime) | GPU clock, temperature, power, load, throttle reasons | GPU throttling |
| `GetLogicalProcessorInformationEx` | L3 cache groups | Which CCD has the 3D V-Cache |

PresentMon and the kernel trace both use raw QPC timestamps, so a spike can be lined up with
what the system did during that exact frame.

## Spikes

A frame is a spike if it is **more than 2× the rolling median** of the last 120 frames **and
at least 8 ms above it**. The first 30 frames are warm-up.

Each spike frame is classified by who was busy:

| Bound | Meaning |
|---|---|
| CPU | CPU work dominated |
| GPU | GPU work dominated |
| Stall | Neither was busy for most of the frame: something was waiting |

## Causes, in the order they are checked

The "excess" is the spike's frame time minus the median. The window is the frame's interval
plus 1 ms before it.

| # | Cause | Side | Rule |
|---|---|---|---|
| 1 | Driver latency | System | A DPC/ISR ≥ 0.5 ms, and either DPC/ISR time ≥ 15% of the excess, or a single one ≥ 2 ms in a stall |
| 2 | Shader compilation | Game | CPU-side spike with ≥ 5 ms and ≥ 15% of the excess sampled inside a shader compiler DLL on the game's threads; or the game wrote to a shader cache during the frame (up to 250 ms after); or near it (2 s before to 5 s after, lower confidence) |
| 3 | GPU throttling | System | Throttle reason from the driver (thermal, power brake, hardware slowdown), or a clock below 75% of its usual full-load value at ≥ 90% load |
| 4 | Low memory / Hard page faults | System / Game | The game's threads waited on hard faults for ≥ 20% of the excess. With < 1.5 GB or < 5% RAM free it is low memory, otherwise the game loading data |
| 5 | VRAM full | Mixed | The game uses ≥ 95% of VRAM (not for CPU-bound spikes) |
| 6 | Storage wait | Mixed | A disk I/O of the game took ≥ 30% of the excess |
| 7 | CPU throttling | System | CPU performance capped by ≥ 10% (CPU-side spikes) |
| 8 | Background load | System | Other processes used ≥ 4 cores on average during the frame, half of it one process |
| 9 | CCD scheduling | System | ≥ 40% of the game's CPU time off the V-Cache CCD, and ≥ 25 points above the session's normal |
| 10 | Game CPU / GPU work | Game | Nothing above applied; decided by the bound |
| 11 | Unexplained wait | Unknown | A stall with no system event |

Confidence grows with the share of the excess that the evidence explains.

## Session checks (Findings)

Driver latency (good below 0.5 ms, warning below 2 ms, problem above), V-Cache CCD share (good
from 80%), background load (good below 1 core, problem from 3), GPU throttling, temperature
near its limit, free RAM, VRAM, CPU limits, shader compilation, stutter pattern (bursts vs.
evenly spread, in 30 s bins), display pacing, frame generation, FrameScope's own load.

Each check is linked to the spikes with the matching cause. A problem linked to no spike is
shown as a warning; a warning linked to at least 10% of the spikes (and at least 3) becomes a
problem.

## Frame generation

| Technique | How it is recognised |
|---|---|
| AMD AFMF, AMD FSR-FG, Intel XeSS-FG | PresentMon labels the generated frames (`--track_frame_type`, passed when the PresentMon version supports it) |
| NVIDIA DLSS-FG / Multi Frame Generation, anything unlabelled | Present pattern: groups of one long gap and 1–5 gaps near zero (below 30% of the average gap and below 4 ms), at least 8 groups in a window of 96 presents, 80% of them the same size, and the near-zero frames must reach the screen |
| Neither found | If the game has a frame generation library loaded, a finding says so |

Generated frames are folded into the rendered frame they belong to. Spikes, average FPS and 1%
lows always describe rendered frames.

A **screen hitch** is the spike rule applied to how long each frame stayed on screen. Hitches
on screen without a spike in the game's frame times are reported as uneven pacing.

## Run comparison

Two runs of the same sequence are lined up by trying offsets of ±10 s and keeping the one where
the most spikes coincide (within 0.5 s).

| Result | Meaning |
|---|---|
| Run 2 has ≥ 60% fewer spikes | Shader compilation |
| Similar rate, ≥ 60% of spikes at the same moments | Game content (streaming, traversal) |
| Similar rate, < 30% at the same moments | Random: the system, or unpredictable game work |

## Code layout

```
src/core/      plain C++: parser, spike detection, correlation, findings, report, session files
src/etw/       ETW kernel trace (Windows; stub elsewhere)
src/win/       sensors (PDH, DXGI, NVML, ADLX), CPU topology, process helpers
src/capture/   PresentMon process wrapper (Qt)
src/ui/        main window, frame-time graph, settings
src/cli/       framescope-cli, framescope-latency
tests/         unit tests without dependencies (ctest)
packaging/     release notes, third-party notices, AMD EULA
```

## Releases

Push a tag `vX.Y.Z` (or `vX.Y.Z-beta.N`), or a branch `release/vX.Y.Z-beta.N`. CI builds both
packages and publishes the release, as a prerelease when the version has a suffix; with the
branch variant it also creates the tag.

The ADLX SDK is downloaded by CMake at configure time instead of living in this repository,
because its license only allows distributing compiled code.
