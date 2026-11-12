# Status and next steps

Last updated: 12 November 2026, with **v0.1.0-beta.17**.

FrameScope is in beta. Everything below "confirmed" has been seen working on real captures;
everything else is built from documentation and simulated data.

## What has been confirmed

All real captures so far come from one PC: Ryzen 9 9950X3D, Radeon RX 9070 XT,
CPPC set to "Driver". Games: Helldivers 2 and Control Resonant.

| Area | State | Seen on real captures |
|---|---|---|
| Frames and spikes from PresentMon | Confirmed | 8 sessions, 2 to 36 minutes long |
| Driver latency (DPC/ISR per driver) | Confirmed | `amdkmdag.sys`, `storport.sys` just under 1 ms |
| AMD GPU sensors (ADLX), throttle inference | Confirmed | Clock, temperature, hotspot, load |
| V-Cache CCD check | Confirmed | 11% on the V-Cache CCD without Game Bar's "Remember this is a game", 97% with it |
| Shader compilation (compiler DLL sampling) | Confirmed on AMD | 80–120 s in `amdxc64.dll` with a cold cache, 8–23 s warm |
| FSR Frame Generation from the present pattern | Confirmed | Both pacings (generated frame right behind the rendered one, and evenly spaced), switched on and off mid-capture |
| Loading screens left out | Confirmed | 1.4 s to 9.4 s stalls in Control Resonant; no false hits in Helldivers 2 |
| Leaving or closing the game left out | Confirmed | 26 and 34 spikes at the end of two captures |
| FrameScope's own load | Confirmed | 0.02 to 0.04 CPU cores, kept off the game's CCD |
| 1% low and worst frame without loading | Confirmed | Control Resonant sessions (beta.16) |
| Frame generation refiltered in older sessions | Confirmed | A session saved before beta.14 (beta.16) |
| Settings changes left out | Seen once | One Control Resonant capture: both visits to the settings menu (31 and 64 spikes) left out. Thresholds come from that one capture |
| Capture hotkey | Confirmed | Starts and stops a capture from inside the game (beta.16) |
| NVIDIA sensors (NVML), `nvgpucomp64.dll` sampling | **Not tested** | No NVIDIA hardware available |
| DLSS Frame Generation / Multi Frame Generation | **Not tested** | Same detector as FSR-FG, only simulated for 3x and 4x |
| AFMF, XeSS-FG (labelled by PresentMon) | **Not tested** | Parser tested with sample rows only |
| Intel CPUs, single-CCD and non-X3D Ryzen | **Not tested** | |
| Low memory, VRAM full, storage wait, CPU throttling | **Not tested** | Never occurred on the test PC |

## Recent changes

| Version | What changed |
|---|---|
| beta.17 | New cause **Settings or scene change**: a burst of spikes after which the frame rate stays at a different level (changing graphics settings, a menu) is left out of the verdict. The spike count and spikes per minute show the spikes in play; those not counted are shown next to them |
| beta.16 | **1% low** and **Worst frame** leave out loading screens and frames while the game wasn't in front. Opening a session saved before beta.14 runs the frame generation filter again, so its false spikes are gone. Global hotkey (Ctrl+Shift+F10, changeable) starts and stops a capture from inside the game |
| beta.15 | New cause **Loading**: loading screens and level changes are left out of the verdict. New finding that sums up what was left out and why |
| beta.14 | Frame generation with evenly spaced frames is recognised (no more flood of false spikes when FSR-FG changes its pacing). The slowdown right before alt-tabbing, closing the game or stopping the capture is left out |
| beta.13 | Spikes while the game isn't in the foreground are left out. FrameScope moves off the CCD the game really runs on |
| Earlier betas | Frame generation from the present pattern, "what to do" advice per cause and finding, user guide, MIT license |

Older versions: see the [Releases](https://github.com/KaroqDave/FrameScope/releases) page.

## To do

### Next

| # | Task | Why |
|---|---|---|
| 1 | Check why PresentMon's CPU load reads 0.00 | Either it really is that low or its process time isn't measured |
| 2 | Replace the demo screenshots with real captures | README and report screenshots still show generated data |

### Before stable v0.1.0

| # | Task | Blocked by |
|---|---|---|
| 4 | NVIDIA validation: NVML sensors, throttle reasons, shader compiler sampling, DLSS-FG pattern | A tester with an NVIDIA card |
| 5 | Threshold review with more captures: other games, an Intel CPU, a PC with a real system-side problem | More captures |
| 6 | Confirm the labelled frame generation path (AFMF, XeSS-FG) | A capture with AFMF on |
| 7 | Tag v0.1.0 and mark it as the latest release | 4 and 5 |
| 8 | Copyright line in `LICENSE` | Decision: "KaroqDave" or real name |

### Later

| Task | Note |
|---|---|
| Intel GPU sensors | Arc cards currently get PDH and DXGI only |
| Download PresentMon from inside the app | Today it is a separate manual download |
| Linux support (MangoHud logs) | The analysis core already builds on Linux; there is no capture side |
| Translations | The UI is English only |

## How to help

Captures are what move this forward, most of all from NVIDIA cards, Intel CPUs and PCs that
really have a system-side problem. Save the session (`.fscope`) and **Tools → Save
diagnostics**, and attach both to a [GitHub issue](https://github.com/KaroqDave/FrameScope/issues).
