**Beta.** The analysis has been checked on real captures from one AMD system (Ryzen 9950X3D,
Radeon RX 9070 XT), including shader compilation and FSR Frame Generation. The NVIDIA side
(sensors, DLSS Frame Generation) still needs confirmation on real hardware. Bug reports with
**Tools → Save diagnostics** attached are very welcome.

What is confirmed, what changed and what comes next:
https://github.com/KaroqDave/FrameScope/blob/main/docs/STATUS.md

### Which download?

| Download | For | AMD GPU sensors |
|---|---|---|
| `FrameScope-…-windows-x64.zip` | Everyone | No (NVIDIA GPU sensors work) |
| `FrameScope-AMD-…-windows-x64.zip` | PCs with an AMD CPU or GPU | Yes; covered by `EULA-AMD.txt`, which AMD's license requires |

### Getting started

1. Unzip anywhere and download the PresentMon console app (`PresentMon-2.x-x64.exe`) from
   https://github.com/GameTechDev/PresentMon/releases
2. Start `framescope.exe` (it asks for admin rights: needed for the system trace).
3. Optional: run **Idle baseline** once to check the PC without a game.
4. Enter the game's `.exe` name, keep **System trace** on, press **Start capture** and play.
5. Press **Stop** and read the verdict and the **Findings** tab.

Full guide: https://github.com/KaroqDave/FrameScope#readme

Requirements: Windows 10/11 x64. FrameScope is MIT licensed; see `LICENSE` in the package.
