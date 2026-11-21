# Shadow Hearts: Nocturne

A native Windows static-recompilation PC port of the **Shadow Hearts PlayStation 2 Trilogy**, unified inside a single Gothic Direct2D Command Hub (`Shadow Hearts Nocturne.exe`) and built on a heavily customized fork of [`ps2-native-recomp`](https://github.com/exoteric33/ps2-native-recomp).

> **Legal / Bring-Your-Own-Disc:** This repository contains **zero** copyrighted game code, Sony BIOS files, disc images, extracted binaries, overlays, textures, audio, saves, or prebuilt executables. You must supply your own legally dumped NTSC-U disc images (`.iso`, `.chd`, `.cso`, or `.bin`/`.cue`) to build each game locally.

---

## Supported Trilogy Titles

| Chronicle | Region / ID | Executable | Setup Menu Option | Key Features |
|---|---|---|---|---|
| **I. Shadow Hearts** *(1913)* | NTSC-U `SLUS-20347` (1 Disc) | `games\sh1\SH1.exe` | `[1]` in [`Shadow Hearts Nocturne Setup.cmd`](Shadow%20Hearts%20Nocturne%20Setup.cmd) | Full AOT static recomp, True 16:9 Hor+ (3D battles/cutscenes with 1:1 2D field alignment), 75-Trophy live system & Gallery, Area Autosave + rich Save-Slot metadata, Offline `.PKB` Texture & FMV Extractor (890 PNGs + 19 FMVs), 1-Click VU1 AOT Baker |
| **II. Shadow Hearts: Covenant** *(1915)* | NTSC-U `SLUS-21041` / `SLUS-21044` (2 Discs) | `games\sh2-covenant\SH2C.exe` | `[2]` in [`Shadow Hearts Nocturne Setup.cmd`](Shadow%20Hearts%20Nocturne%20Setup.cmd) | Full AOT static recomp (`SLUS_210.41` + `OPENING`/`FIELD`/`BATTLE` `MWo3` overlays), Seamless Disc 1 ↔ Disc 2 auto-swap, True 16:9 Hor+ + frustum render-fix, 110-Trophy live system & Gallery, Area Autosave, Offline `.PKB` Texture & FMV Extractor (~5,650 PNGs + 40 FMVs), 1-Click VU1 AOT Baker |
| **III. Shadow Hearts: From the New World** *(1929)* | NTSC-U `SLUS-21326` (1 Disc) | `games\sh3-ftnw\SH3.exe` | `[3]` in [`Shadow Hearts Nocturne Setup.cmd`](Shadow%20Hearts%20Nocturne%20Setup.cmd) | Playable native PC port (`SLUS_213.26` + live `.CPX` overlay recompilation), Fast SSD Disc Loading, High-Res 1×–64× scaling, LZ4 Save States, Depth Post-FX, F1 Quick Menu |

---

## Highlights & Features

### Nocturne Trilogy Command Hub (`Shadow Hearts Nocturne.exe`)
- **3-Panel Gothic Triptych Landing Screen:** Launch directly into a full-screen 3-panel Trilogy selector (with full Mouse, Keyboard, and XInput Gamepad navigation), each panel styled with its game's signature palette (1913 Blood-Moon & Ashen Silver for *Shadow Hearts*, 1915 Obsidian/Crimson/Gold for *Covenant*, 1929 Art-Deco Midnight-Indigo & Brass for *From the New World*), animated Judgment Ring crests, live trophy/save progress, and optional custom box art (`games\<slug>\cover.png` or `cover.jpg`).
- **Clean Self-Contained Per-Game Layout:** The project root stays clutter-free (only `Shadow Hearts Nocturne.exe`, `launcher.ini`, and shared DLLs). Each title owns its own isolated folder under `games\<slug>\` containing its executable (`SH1.exe`, `SH2C.exe`, `SH3.exe`), unpacked disc folders (`disc_files\` for 1-disc games; `disc1_files\` + `disc2_files\` for Covenant), `memcard.ps2`, `keys.ini`, `states\`, `achievements.json`, `mods\`, `dumps\`, and `backups\`.
- **ISO-Free Unpacked Virtual Disc Folders & Fast SSD Loading:** Unpacks your `.iso`, `.chd`, `.cso`, or `.bin`/`.cue` images with byte-for-byte verification (and NTFS hardlink deduplication across Covenant's 2 discs). Fast SSD loading (`~16×` faster CDVD timing) is ON by default with a live toggle in `F1 → Settings → Controls`.
- **One-Click Tools Built Into the Launcher:** Manage HD Texture Packs (drag/reorder/toggle), dump all disc textures & FMVs offline, run 1-click AI 4× Texture / 2×–4× FMV upscaling (`Real-ESRGAN` + `ffmpeg`), bake captured VU1/VU0 vector microprograms permanently into the game EXE (`~3–8s`), back up/restore memory cards, and open per-game screenshot folders (`%USERPROFILE%\Pictures\Shadow Hearts Nocturne\<Game>\`).

### Modern PC Graphics, Widescreen & Performance
- **Live-Toggleable True 16:9 Hor+ Widescreen:** True 3D camera projection (`ax *= 0.75`) and horizontal clip-frustum fixes for both *Shadow Hearts: Covenant* and *Shadow Hearts* (with automatic 1:1 protection on *Shadow Hearts 1*'s pre-rendered 2D field rooms so 3D characters never misalign with 2D backgrounds).
- **High-Resolution Scaling (`1×` – `64×`) + Adaptive Resolution Scaling:** Cycle powers of 2 or type any custom integer scale up to your GPU's safe VRAM limit, or enable **Adaptive Scale** to hold silky-smooth frame pacing automatically.
- **Fast DXC + Multi-Threaded Shader Startup & Single-Pass GPU Tile Binning (`raster_gpu`):** Offloads primitive-to-tile binning into GPU shared memory with automatic `16×16` tiles at `4×+`, cutting `4×` CPU render overhead by **2.5×** with **100% bit-exact PS2 accuracy**.
- **Unlocked & Monitor-Hz Frame Pacing:** Choose `30`, `60`, `120`, `144`, `240`, `Match Display (<Hz> Hz)`, or `Unlimited`, paired with **High-FPS Speed Sync** (keeps IOP/SPU2 audio at exact 48 kHz real-time and auto-holds 60 Hz during FMVs).
- **Depth-Aware Post-Processing (Live in `blit.wgsl`):** Resolves the PS2 GS Z-buffer directly on the GPU for real-time **SSAO**, **HDR Bloom**, **SSGI Light Bounce**, **Filmic Tone Mapping**, and **Gothic Vignette** (all off by default for authentic PS2 visuals), alongside live Color Grading, CRT Scanlines, and Deinterlacing.
- **HD Texture & HD FMV Replacement:** Drop-in PNG texture packs (`games\<slug>\mods\textures\`) and hardware-decoded Windows Media Foundation MP4 cutscenes (`games\<slug>\mods\fmv\`) with offline full-disc `.PKB` extractors for both *Shadow Hearts 1* and *Covenant*.

### `F1` Quick Menu, Save States, Area Autosave & 185 Achievements
- **Refactored `F1` Quick Menu Hub & Tabbed Settings (`Graphics` | `Audio` | `Controls`):** Navigate with mouse, keyboard (`Q`/`E`), or controller (`L1`/`R1` / `Select+Start` / `Guide`).
- **Area-Transition Autosave + 0–9 Compact LZ4 Save State Slots (`F5`–`F8`):**
  - Automatic area-transition `autosave.state` plus 10 manual slots (`Slot 0`–`Slot 9`) with live `160×112` thumbnails, timestamps, area name, max party level, playtime, and active disc metadata.
  - Compact LZ4-compressed 1×-VRAM save format (`~10 MB` at any resolution scale — **~10× smaller** than uncompressed 4× states) with full backward compatibility for legacy `.state` files.
- **Custom Achievement Systems & High-Res Trophy Galleries (185 Total Trophies):**
  - **Shadow Hearts (1913) — 75 Achievements:** Mark (16), Sigil (14), Crest (11), Sovereign (8), Follies (10), secret Curiosities (15 — all iconic 1913 Asia/Europe locations & optional endgame dungeons), and 1 Capstone (*Harmonixer of Destiny*).
  - **Shadow Hearts: Covenant (1915) — 110 Achievements:** Mark (24), Sigil (19), Crest (15), Sovereign (11), Follies (15), secret Curiosities (25 — story milestones, optional dungeons, and tagged `(Missable)` entries), and 1 Capstone (*The Covenant Sealed*).
  - Live EE RAM polling (`< 0.01 ms/frame`), animated TrueType + medallion gradient toasts (4 selectable themes + smart burst coalescing), harmonic unlock chime, and automatic composited PNG trophy screenshots.
- **Clean Game-Only Screenshots (`F12`):** Lossless PNG capture of the rendered game frame (never the `F1` menu) organized per game in `%USERPROFILE%\Pictures\Shadow Hearts Nocturne\<Game>\`.
- **Granular Audio Controls (`Settings → Audio`):** Master Volume, Mute, Output Device switcher, Audio Time-Stretch, UI/Achievement Chime Volume, Music & Voices (`SPU2 Core 0`), and Sound Effects (`SPU2 Core 1`).

---

## Getting Started on Windows

No Visual Studio installation or manual toolchain setup is required. Every build script automatically bootstraps the bundled portable toolchain (`llvm-mingw` Clang, `CMake`, `Ninja`, `Python`, `SDL2`, `wgpu-native`, and Microsoft `DXC`) on a fresh clone.

### 1. Build One or All Three Games from Your Own Disc Images
Double-click **[`Shadow Hearts Nocturne Setup.cmd`](Shadow%20Hearts%20Nocturne%20Setup.cmd)** in the main folder and pick from the interactive menu:

- **`[1]` Shadow Hearts (1913):** Prompts for your NTSC-U `SLUS-20347` `.iso`/`.chd`/`.cso`/`.cue`
- **`[2]` Shadow Hearts: Covenant (1915):** Prompts for your NTSC-U Disc 1 `SLUS-21041` & Disc 2 `SLUS-21044` images
- **`[3]` Shadow Hearts: From the New World (1929):** Prompts for your NTSC-U `SLUS-21326` image
- **`[4]` Build / Update ALL Three Games in Sequence**
- Also includes direct launch options (`[5]`–`[8]`), VU1 AOT baking (`[9]`), offline texture/FMV extraction (`[A]`), and toolchain repair (`[T]`).

> **Upgrading an existing installation?** Simply `git pull`, open `Shadow Hearts Nocturne Setup.cmd`, and choose the game(s) you want to update. Your existing `memcard.ps2`, `keys.ini`, `states\`, `achievements.json`, `mods\`, and `dumps\` are preserved automatically (and any legacy root-folder `mods\`/`dumps\`/`Shadow Hearts Covenant.exe` layout is migrated cleanly into `games\sh2-covenant\`).

### 2. Play
Double-click **`Shadow Hearts Nocturne.exe`** to open the 3-panel Gothic Trilogy Launcher, choose your Chronicle, and click **Play**! (You can also launch `games\sh1\SH1.exe`, `games\sh2-covenant\SH2C.exe`, or `games\sh3-ftnw\SH3.exe` directly.)

---

## Controls & Shortcuts

| Shortcut | Action |
|---|---|
| **`F1`** *(or `Select+Start` / `Guide`)* | Open/close the **Quick Menu Hub** (`Settings`, `Save States`, `Achievements`, `Take screenshot`) |
| **`F5`** | Quick Save to active slot (`0–9`), or open Slot Picker if enabled |
| **`F8`** | Quick Load from active slot (`0–9`), or open Slot Picker if enabled |
| **`F6` / `F7`** | Cycle active Save State slot (`0–9`) |
| **`F12`** | Take clean game-only PNG screenshot (`%USERPROFILE%\Pictures\Shadow Hearts Nocturne\<Game>\`) |
| **`F2`** | Cycle picture styles / filters |
| **`F11`** | Toggle Full Screen |
| **`Q` / `E`** *(or `L1` / `R1`)* | Switch Settings tabs (`Graphics` / `Audio` / `Controls`) or Trophy Gallery tier filters |

- Custom bindings & settings are saved per game in `games\<slug>\keys.ini` (or rebind live inside `F1 → Settings → Controls → Change keys`).
- In-game memory card saves are stored per game in `games\<slug>\memcard.ps2` with automatic rotating backups.

---

## Technical Documentation

For deep technical notes on the Shadow Hearts engine fixes (`SDRV_IOP`, `IOPUTY`, `FIO`, `MWo3` AOT overlay translation, GPU upload cache / sprite filter optimizations, and `SSHSys::DiskChange`), see [`SHADOW_HEARTS_COVENANT.md`](SHADOW_HEARTS_COVENANT.md).

---

## Credits & License

- Built on [`ps2-native-recomp`](https://github.com/exoteric33/ps2-native-recomp) and [`ps2-web-recomp`](https://github.com/snuri00/ps2-web-recomp) by Samir Nuri.
- Released under the **MIT License** — see [`LICENSE`](LICENSE). MPEG VLC tables adapted from [Play!](https://github.com/jpd002/Play-) (BSD 2-Clause, see `runtime/src/ipu/NOTICE`).
- *Shadow Hearts*, *Shadow Hearts: Covenant*, and *Shadow Hearts: From the New World* are trademarks of their respective copyright holders; PlayStation and PS2 are trademarks of Sony Interactive Entertainment. This project is an independent fan-made recompiler port and is not affiliated with or endorsed by Sony or any game publisher.
