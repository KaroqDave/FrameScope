**Beta:** this is the first public build. Everything is tested in CI, but the system trace
(ETW), the sensors and the cause detection have not been verified on many machines yet.
Bug reports with **Tools → Save diagnostics** attached are very welcome.

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
4. Enter the game's `.exe` name, keep **System trace** on, and press **Start capture**.

Quick system check without the GUI: `framescope-latency.exe --seconds 30` (as admin).

Requirements: Windows 10/11 x64.
