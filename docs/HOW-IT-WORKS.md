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
| – | Loading | not counted | Applied after the rules below: a stretch of slow frames (50 ms or longer) nearly back to back (at most 0.25 s between one ending and the next starting) that together took at least 1.5 s. Small spikes within 0.25 s around it go with it; spikes with a system-side cause keep it |
| 0 | Game not in foreground | not counted | A sensor sample around the frame saw another window in front. Also: the burst of spikes leading straight into such a spike, and a burst that runs up to the end of the capture (spikes at most 1 s apart, up to 5 s back; spikes with a system-side cause keep it) |
| 1 | Driver latency | System | A DPC/ISR ≥ 0.5 ms, and either DPC/ISR time ≥ 15% of the excess, or a single one ≥ 2 ms in a stall |
| 2 | Shader compilation | Game | CPU-side spike with ≥ 5 ms and ≥ 15% of the excess sampled inside a shader compiler DLL on the game's threads; or the game wrote to a shader cache during the frame (up to 250 ms after); or near it (2 s before to 5 s after, lower confidence) |
| 3 | GPU throttling | System | Throttle reason from the driver (thermal, power brake, hardware slowdown), or a clock below 75% of its usual full-load value at ≥ 90% load |
| – | Settings or scene change | not counted | Applied after the rules below: a burst of 3 or more spikes, at most 3.5 s apart and 30 s long, after which the frame rate stays at another level. The level is the median frame time of the frames that aren't spikes, up to 5 s before and after the burst (at least 1 s on each side; during a capture the full 5 s after); it must differ by 1.5x or more. Spikes with a system-side cause keep it |
| 4 | Low memory / Hard page faults | System / Game | The game's threads waited on hard faults for ≥ 20% of the excess. With < 1.5 GB or < 5% RAM free it is low memory, otherwise the game loading data |
| 5 | VRAM full | Mixed | The game uses ≥ 95% of VRAM (not for CPU-bound spikes) |
| 6 | Storage wait | Mixed | A disk I/O of the game took ≥ 30% of the excess |
| 7 | CPU throttling | System | CPU performance capped by ≥ 10% (CPU-side spikes) |
| 8 | Background load | System | Other processes used ≥ 4 cores on average during the frame, half of it one process |
| 9 | CCD scheduling | System | ≥ 40% of the game's CPU time off the V-Cache CCD, and ≥ 25 points above the session's normal |
| 10 | Game CPU / GPU work | Game | Nothing above applied; decided by the bound |
| 11 | Unexplained wait | Unknown | A stall with no system event |

Confidence grows with the share of the excess that the evidence explains.

"Not counted" spikes stay in the spike table but are left out of the verdict, the percentages
and the stutter pattern. **1% low** and **worst frame** leave out their frames too, along with
the frames between two such spikes at most 0.25 s apart (the same loading screen). Duration,
average FPS and the spike count still cover the whole capture.

A capture stopped with the hotkey while the game was in front ended in play: the rule about
the slowdown right before the capture's end is skipped, and the session file notes it
(`ended	inplay`) so reopening the session doesn't apply the rule either.

Opening a saved session runs the frame generation filter again. If it folds generated frames
(a session saved before that pattern was recognised), the spikes are detected anew; each keeps
the cause of the saved spike that ended within its frame. Saving the session keeps the result.

## Session checks (Findings)

Driver latency (good below 0.5 ms, warning below 2 ms, problem above), V-Cache CCD share (good
from 80%), background load (good below 1 core, problem from 3), GPU throttling, temperature
near its limit, free RAM, VRAM, CPU limits, shader compilation, stutter pattern (bursts vs.
evenly spread, in 30 s bins), display pacing, frame generation, FrameScope's own load, and how
many spikes were left out of the verdict (loading, game not in front).

Each check is linked to the spikes with the matching cause. A problem linked to no spike is
shown as a warning; a warning linked to at least 10% of the spikes (and at least 3) becomes a
problem.

## Frame generation

| Technique | How it is recognised |
|---|---|
| AMD AFMF, AMD FSR-FG, Intel XeSS-FG | PresentMon labels the generated frames (`--track_frame_type`, passed when the PresentMon version supports it) |
| NVIDIA DLSS-FG / Multi Frame Generation, AMD FSR-FG when the game doesn't report it, anything unlabelled | Present pattern: groups of one long gap and 1–5 gaps near zero (below 30% of the average gap and below 4 ms), at least 8 groups in a window of 96 presents, 80% of them the same size, and the near-zero frames must reach the screen. If the generated frames are spaced evenly instead (seen with FSR-FG): presents with exactly 0 CPU and GPU work alternating with presents whose CPU work spans the whole group, same group rules |
| Neither found | If the game has a frame generation library loaded, a finding says so |

Both patterns were confirmed on real FSR Frame Generation captures. Near-zero gaps: ~19 ms and
~0.1 ms alternating, each frame shown for ~9.7 ms; 6,900 false spikes without the detector, 69
with it. Even spacing: ~9 ms gaps throughout, every other present with 0 ms of work and the next
one with ~18 ms. FSR-FG switched between the two within one capture.

The filter looks 96 presents ahead, so frames reach the analysis with that delay (about a
second).

Generated frames are folded into the rendered frame they belong to. PresentMon reports the
group's CPU and GPU work on either row, so the merged frame takes the larger values. Spikes, average FPS and 1%
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
docs/          this file, STATUS.md (what is confirmed, to-do list), screenshots
packaging/     release notes, third-party notices, AMD EULA
```

## Releases

Push a tag `vX.Y.Z` (or `vX.Y.Z-beta.N`), or a branch `release/vX.Y.Z-beta.N`. CI builds both
packages and publishes the release, as a prerelease when the version has a suffix; with the
branch variant it also creates the tag.

The ADLX SDK is downloaded by CMake at configure time instead of living in this repository,
because its license only allows distributing compiled code.
