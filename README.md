# Shadow Hearts: Nocturne

A native Windows static-recompilation PC port of **Shadow Hearts: Covenant** (PS2 NTSC-U: Disc 1 `SLUS-21041` / Disc 2 `SLUS-21044`, CRC `F3BDB2E6`), built on a heavily customized fork of [`ps2-native-recomp`](https://github.com/exoteric33/ps2-native-recomp).

> **Legal / Bring-Your-Own-Disc:** This repository contains **zero** copyrighted game code, Sony BIOS files, disc images, extracted binaries, overlays, textures, audio, saves, or prebuilt executables. You must supply your own legally dumped NTSC-U Disc 1 and Disc 2 images (`.iso`, `.chd`, `.cso`, or `.bin`/`.cue`) to build the game locally.

---

## Highlights & Features

### Native Port, ISO-Free Folders & Multi-Disc Engine
- **ISO-Free Unpacked Virtual Disc Folders:** Unpacks Disc 1 & Disc 2 (`.iso`, `.chd`, `.cso`, or `.bin`/`.cue`) into standard folders (`games\sh2-covenant\disc1_files\` and `disc2_files\`) with cross-disc NTFS hardlink deduplication and full byte-for-byte verification against your source images. Once verified, no `.iso` files are needed to play (and legacy `disc.iso`/`disc2.iso` copies are automatically upgraded and cleaned up).
- **Fast SSD Disc Loading:** ON by default (`~16×` faster CDVD/CDVDFSV read & seek timing than a PS2 DVD drive), with a live toggle in `F1 → Settings → Controls` (`fast_disc = on/off` in `keys.ini`) if you ever want authentic PS2 DVD timing.
- **Direct Double-Click Executable (`Shadow Hearts Nocturne.exe`):** Staged directly in the project folder alongside required DLLs; double-clicking auto-detects `disc1_files`/`disc2_files`, attaches `memcard.ps2`, and opens the game window with zero scripts required.
- **Full Ahead-of-Time Static Recompilation:** Translates `SLUS_210.41` and all three `MWo3` runtime overlays (`OPENING.BIN`, `FIELD.BIN`, `BATTLE.BIN`) ahead of time at `-O2`, eliminating mid-game live-recompilation hitches.
- **Seamless Automatic Disc 1 ↔ Disc 2 Switching:** Mounts both discs at launch (`disc1_files` + `disc2_files`, with fallback to legacy `disc.iso`/`disc2.iso`) and intercepts `SSHSys::DiskChange` automatically so disc swaps at the end of Disc 1 or when loading a Disc 2 save happen seamlessly without manual intervention.
- **Shadow Hearts: Covenant Runtime Fixes:** Includes dedicated IOP hardware root-counter reset-on-compare fixes (`SDRV_IOP.IRX` audio/FMV deadlock fix), SIF command handler mirroring (`IOPUTY.IRX`), and async `sceCdSeek` completion (`FIO.IRX`).

### Modern PC Graphics & Performance
- **Fast DXC + Multi-Threaded Shader Startup:** Automatically fetches Microsoft DXC (`dxcompiler.dll` + `dxil.dll`) on Windows and compiles startup compute pipelines in parallel across CPU threads for dramatically faster Direct3D 12 launch times.
- **High-Resolution Scaling (`1×` – `64×`):** Cycle powers of 2 (`1×`, `2×`, `4×`, `8×`, ..., up to your GPU's safe limit) or type any custom integer scale (`1`–`gpu_max_scale`) in `F1 → Settings → Graphics` with live pixel resolution and estimated VRAM preview.
- **Single-Pass GPU Tile Binning (`raster_gpu`) + Auto `16×16` Tiles at `4×+`:** Offloads primitive-to-tile filtering into on-chip GPU shared memory, cutting `4×` CPU render overhead by **2.5×** with **100% bit-exact PS2 accuracy** and **~3× faster `8×` scaling**.
- **Unlocked & Monitor-Hz Frame Pacing:** Choose `30`, `60`, `120`, `144`, `240`, `Match Display (<Hz> Hz)`, or `Unlimited`, paired with **High-FPS Speed Sync** (on by default) to keep IOP/SPU2 audio at exact 48 kHz real-time and auto-hold 60 Hz during FMVs.
- **Depth-Aware Post-Processing (Live in `blit.wgsl`):** Resolves the PS2 GS Z-buffer directly on the GPU (zero CPU readback) to drive real-time **SSAO**, **HDR Bloom**, **SSGI Light Bounce**, **Filmic Tone Mapping**, and **Gothic Vignette** (all off by default for authentic PS2 visuals).
- **Live Display & Color Grading:** Direct3D 12 / Vulkan selector, GPU adapter selector, Async GPU submit, VSync / Present Mode (`Mailbox` / `Fifo` / `Immediate`), Aspect Ratio (`4:3` / `16:9` / `Stretch`), Brightness, Contrast, Saturation, Gamma, CRT Scanlines, and Deinterlacing.

### Refactored `F1` Quick Menu, Save States, Screenshots & Achievements
- **Organized `F1` Quick Menu Hub & Tabbed Settings (`Graphics` | `Audio` | `Controls`):** Clean main hub plus dedicated settings tabs (navigate with mouse, keyboard `Q`/`E`, or controller `L1`/`R1`).
- **0–9 Compact LZ4 Save State Slots (`F5`/`F6`/`F7`/`F8` & `F1 → Save States`):**
  - 10 independent slots (`Slot 0`–`Slot 9`) with live `160×112` thumbnails, timestamps, area/map, party level, playtime, and active disc metadata.
  - Compact LZ4-compressed 1×-VRAM save format (`~10 MB` at any resolution scale — **~10× smaller** than 104 MB uncompressed 4× states) while remaining 100% backward-compatible with legacy `.state` files.
  - Optional `F5/F8 opens slot picker` toggle in `Settings → Controls`.
- **Custom 32-Achievement System (`F1 → Achievements`):**
  - 32 Shadow Hearts: Covenant achievements across 7 tiers: **Mark (6)**, **Sigil (5)**, **Crest (5)**, **Sovereign (5)**, **The Covenant Capstone (1)**, **Follies (5)**, and **Curiosities (5, hidden as Secret until unlocked)**, tracked live from NTSC-U `SSH_SAVE` EE RAM and persisted in `games\sh2-covenant\achievements.json`.
  - Unlocks trigger a harmonic chime, an animated **Dynamic Tier-Tinted Gradient** toast (with 4 selectable themes), and an automatic composited PNG screenshot saved to `%USERPROFILE%\Pictures\Game Screenshots\Achievement_<Name>_<YYYY-MM-DD>.png`.
- **Clean Game-Only Screenshots (`F12` / `F1 → Take screenshot`):** Captures only the rendered game frame (never the `F1` menu) as a lossless PNG in `%USERPROFILE%\Pictures\Game Screenshots\ShadowHeartsCovenant_YYYY-MM-DD_HH-MM-SS.png`.
- **Granular Audio Controls (`Settings → Audio`):** Master Volume, Mute, Output Device switcher, Audio Time-Stretch, UI/Achievement Chime Volume, Music & Voices (`SPU2 Core 0`), and Sound Effects (`SPU2 Core 1`).

---

## Getting Started on Windows

### 1. One-Click Build from Your Own Disc Images (Fresh Clone or Upgrade)
No Visual Studio installation or manual setup is required. Just double-click [`Build Shadow Hearts Covenant.cmd`](Build%20Shadow%20Hearts%20Covenant.cmd):
- Automatically downloads the portable build toolchain (`llvm-mingw`, `CMake`, `Ninja`, `Python`, `MinGit`), `SDL2`, `wgpu-native`, and Microsoft `DXC` on a fresh clone.
- Prompts you to paste/drag-and-drop your NTSC-U Disc 1 & Disc 2 images (`.iso`, `.chd`, `.cso`, or `.bin`/`.cue`), unpacks them into `games\sh2-covenant\disc1_files\` and `disc2_files\`, deduplicates identical cross-disc files via NTFS hardlinks, verifies every byte against your source images, and optionally offers to delete your original source images.
- Automatically upgrades and cleans up legacy `games\sh2-covenant\disc.iso` / `disc2.iso` copies if present.
- Translates `SLUS_210.41`, all 3 overlays (`OPENING.BIN`, `FIELD.BIN`, `BATTLE.BIN`), and any recorded VU1/VU0 vector programs with a live progress bar, then stages **`Shadow Hearts Nocturne.exe`** + required DLLs in the project folder.
- Automatically preserves your memory card (`games\sh2-covenant\memcard.ps2`) and keybindings (`games\sh2-covenant\keys.ini`).

### 2. Play
Double-click **`Shadow Hearts Nocturne.exe`** (or [`Play Shadow Hearts Covenant.cmd`](Play%20Shadow%20Hearts%20Covenant.cmd)).

---

## Controls & Shortcuts

| Shortcut | Action |
|---|---|
| **`F1`** *(or `Select+Start` / `Guide`)* | Open/close the **Quick Menu Hub** (`Settings`, `Save States`, `Achievements`, `Take screenshot`) |
| **`F5`** | Quick Save to active slot (`0–9`), or open Slot Picker if enabled |
| **`F8`** | Quick Load from active slot (`0–9`), or open Slot Picker if enabled |
| **`F6` / `F7`** | Cycle active Save State slot (`0–9`) |
| **`F12`** | Take clean game-only PNG screenshot (`%USERPROFILE%\Pictures\Game Screenshots\`) |
| **`F2`** | Cycle picture styles / filters |
| **`F11`** | Toggle Full Screen |
| **`Q` / `E`** *(or `L1` / `R1`)* | Switch Settings tabs (`Graphics` / `Audio` / `Controls`) or Achievement Tier filters |

- Custom bindings & settings are saved automatically in `games\sh2-covenant\keys.ini` (or rebind live inside `F1 → Settings → Controls → Change keys`).
- In-game memory card saves are stored in `games\sh2-covenant\memcard.ps2` with automatic rotating backups.

---

## Technical Documentation

For deep technical notes on the Shadow Hearts: Covenant engine fixes (`SDRV_IOP`, `IOPUTY`, `FIO`, `MWo3` AOT overlay translation, GPU upload cache / sprite filter optimizations, and `SSHSys::DiskChange`), see [`SHADOW_HEARTS_COVENANT.md`](SHADOW_HEARTS_COVENANT.md).

---

## Credits & License

- Built on [`ps2-native-recomp`](https://github.com/exoteric33/ps2-native-recomp) and [`ps2-web-recomp`](https://github.com/snuri00/ps2-web-recomp) by Samir Nuri.
- Released under the **MIT License** — see [`LICENSE`](LICENSE). MPEG VLC tables adapted from [Play!](https://github.com/jpd002/Play-) (BSD 2-Clause, see `runtime/src/ipu/NOTICE`).
- *Shadow Hearts: Covenant* is a trademark of its respective copyright holders; PlayStation and PS2 are trademarks of Sony Interactive Entertainment. This project is an independent fan-made recompiler port and is not affiliated with or endorsed by Sony or any game publisher.
