# Shadow Hearts: Covenant — Native Windows PC Port

A native Windows static-recompilation PC port of **Shadow Hearts: Covenant** (PS2 NTSC-U: Disc 1 `SLUS-21041` / Disc 2 `SLUS-21044`, CRC `F3BDB2E6`), built on a fork of `ps2-native-recomp` with **seamless automatic Disc 1 ↔ Disc 2 switching**.

> **Legal / Bring-Your-Own-ISO:** This repository contains zero Sony BIOS files, game binaries, overlays, textures, audio, or disc images. You must supply your own legally dumped NTSC-U Disc 1 and Disc 2 `.iso` images.

---

## Quick Play (Already Built)

Double-click [`Play Shadow Hearts Covenant.cmd`](Play%20Shadow%20Hearts%20Covenant.cmd).

### Controls & Shortcuts
- **Quick Menu Hub:** `F1` (or hold `Select` + press `Start` / `Guide` on a controller)
  - Compact Main Hub: `Resume`, `Quick save (Slot N)`, `Quick load (Slot N)`, `Save States (0-9) >`, `Take screenshot`, `Achievements (XX / 32)`, `Settings >`, and `Quit the game`.
  - **Save States Manager (`F1 → Save States`) & 0–9 Slots:**
    - 10 independent slots (`Slot 0`–`Slot 9`) stored as compact LZ4-compressed 1×-VRAM saves (`~10 MB` at any resolution scale — **~10× smaller** than 104 MB 4× uncompressed states, while staying 100% backward-compatible with legacy `.state` files).
    - **Instant Mode (Default):** `F5` saves to the active slot, `F8` loads the active slot, and `F6` / `F7` cycles slots `0–9` with a quick on-screen HUD indicator.
    - **Slot Manager & Optional Picker:** Browse all 10 slots with live `160×112` thumbnail previews, timestamps, area/map, party level, playtime, disc #, and Save (`S`/`X`), Load (`Enter`/`L`), or Delete (`Del`/`Y`). Toggle `F5/F8 opens slot picker` in `Settings > Controls` to pop open the Slot 0–9 Picker directly on `F5`/`F8`.
  - **Settings > Graphics Tab** (`Q`/`E` or `L1`/`R1` or click tabs):
    - **Resolution (2^n):** Cycles powers of 2 (`1x`, `2x`, `4x`, `8x`, ..., up to GPU-safe maximum based on `maxStorageBufferBindingSize`) with live pixel resolution and estimated VRAM (`MB`/`GB`).
    - **Custom scale (`1`–`gpu_max_scale`):** Press Enter to type any whole integer scale from `1` up to your GPU's safe limit with live resolution & VRAM preview.
    - **Single-Pass GPU Tile Binning (`on` by default at 2×+) + Auto `16×16` Tiles at 4×+:** Offloads primitive-to-tile filtering into on-chip GPU shared memory (`raster_gpu`), cutting 4× CPU render overhead by **2.5×** (`590 ms/s → 236 ms/s`) with **100% bit-exact PS2 accuracy** and **~3× faster 8× scaling**.
    - **FPS Limit & High-FPS Speed Sync:** `30`, `60`, `120`, `144`, `240`, `Match Display (<Hz> Hz)`, or `Unlimited`, paired with `High-FPS speed sync` (ON by default) to keep IOP/SPU2 audio at exact 48 kHz real-time and auto-hold 60 Hz during FMVs.
    - **Depth-Aware Lighting & Post-Processing (Live in `blit.wgsl`):** Resolves the active PS2 GS Z-buffer on the GPU (zero CPU readback) to power live **SSAO (`0–100%`)**, **Bloom / HDR Glow (`0–100%`)**, **SSGI Light Bounce (`0–100%`)**, **Filmic Tone Map (`on`/`off`)**, and **Gothic Vignette (`0–100%`)** — all off by default for authentic PS2 visuals.
    - **Live Image & Display Controls:** VSync / Present Mode (`Mailbox` / `Fifo` / `Immediate`), Picture look (`F2`), Text and menus (`soft` / `sharp` / `sharpest`), Aspect Ratio (`4:3 Classic` / `16:9 Widescreen` / `Stretch to Fill`), Brightness (`-50..+50%`), Contrast (`50..150%`), Saturation (`0..200%`), Gamma (`0.50..2.00`), CRT Scanlines (`0..100%`), Deinterlacing (`on` / `off`), Achievement Toast Gradient Theme (4 selectable Shadow Hearts palettes, default **Dynamic Tier-Tinted**), Graphics chip, Graphics API (`Direct3D 12` / `Vulkan`), Async GPU submit (`on` / `off`), GPU tile binning (`on` / `off`), Full Screen (`F11`), and Reset Image Defaults.
  - **Settings > Audio Tab:** Master Volume (`0–100%`), Mute (`on` / `off`), Output Device switcher, Audio Time-Stretch (`on` / `off`), Achievement & UI Chime Volume (`0–100%`), Music & Voices (`SPU2 Core 0`), and Sound Effects (`SPU2 Core 1`).
  - **Settings > Controls Tab:** `F5/F8 opens slot picker` toggle, Show key card, and live in-menu key rebinding (`Change keys >`).
- **Clean Game-Only Screenshot:** `F12` or `F1` → `Take screenshot`
  - Captures **only the game picture** (never the `F1` menu) as a lossless PNG in `%USERPROFILE%\Pictures\Game Screenshots\ShadowHeartsCovenant_YYYY-MM-DD_HH-MM-SS.png`.
- **Shadow Hearts: Covenant Achievements (`F1` → `Achievements`):**
  - 32 custom achievements across 7 tiers: **Mark (6)**, **Sigil (5)**, **Crest (5)**, **Sovereign (5)**, **The Covenant Capstone (1)**, **Follies (5)**, and **Curiosities (5, hidden as Secret until unlocked)**, tracked live from NTSC-U `SSH_SAVE` EE RAM and persisted in `games\sh2-covenant\achievements.json`.
  - Unlocks trigger a harmonic chime, a top-right animated gradient toast, and an automatic composited PNG screenshot saved to `%USERPROFILE%\Pictures\Game Screenshots\Achievement_<Name>_<YYYY-MM-DD>.png`.
  - Use `Q`/`E` (or `L1`/`R1`) to filter by Tier, `Left`/`Right` to filter `All / Unlocked / Locked`, and `Tab` (or `Y`) to reveal/hide Secret achievements.
- **Visual Presets / Filters:** `F2`
- **Quick Save / Quick Load:** `F5` / `F8` (save states also remember which disc is active)
- **Custom Keys & Settings:** Edit `games\sh2-covenant\keys.ini`
- **Memory Card Saves:** Saved automatically to `games\sh2-covenant\memcard.ps2`
- **Title Bar Stats:** Displays FPS, Task Manager-normalized total CPU % + main emulation thread single-core % (`CPU X% (main Y%)`), GPU %, RAM, and audio queue ms.

### Seamless 2-Disc Support
Both discs are mounted at launch (`--disc disc.iso --disc2 disc2.iso`, or automatically detected when `disc2.iso` sits next to `disc.iso`).

Because `SLUS_210.41`/`SLUS_210.44` and all three code overlays (`OPENING.BIN`, `FIELD.BIN`, `BATTLE.BIN`) are 100% byte-identical between Disc 1 and Disc 2, when the game invokes `SSHSys::DiskChange` (at the end of Disc 1 or when loading a Disc 2 save from Disc 1), the runtime detects the target disc index before the ISO9660 Primary Volume Descriptor (sector 16) read completes and seamlessly mounts the target disc, allowing the game to reload `PKFINFO.TBL` and `LSD.PKB` automatically without manual disc swapping.

---

## Key Engine & Runtime Fixes for Shadow Hearts: Covenant

1. **IOP Hardware Timer Reset-on-Compare (`runtime/src/iop/libs.cpp`):**
   - `SDRV_IOP.IRX` schedules a 1 kHz root-counter callback via `timrman::SetTimerHandler` and calls `GetTimerCounter` before returning to guard against overruns.
   - Hardware root counters reset to `0` on compare match; previously `g_hard_timer_start` was never reset on tick, causing `SDRV_IOP` to see an ever-growing counter and exponentially double its audio timer interval (`1ms → 2ms → 4ms → ... → 17s`), stalling audio and deadlocking the opening credits movie at ~29.6 seconds.
2. **SIF Command Handler Mirroring (`runtime/src/iop/sif.cpp`):**
   - Mirrored `sceSifSetCmdBuffer` / `sceSifAddCmdHandler` / `sceSifRemoveCmdHandler` entries into IOP guest RAM so Nautilus's `IOPUTY.IRX` registers its command handler cleanly without hanging during boot.
3. **Async `sceCdSeek` Completion (`runtime/src/iop/cdvd.cpp`):**
   - Fired `SCECdFuncSeek` (`finish_cd(4)`) asynchronously so Nautilus's `FIO.IRX` completes disc seeks cleanly.
4. **Full Ahead-of-Time `MWo3` Overlay Translation (`recomp/main.cpp`, `recomp/eerecomp_gen.cpp`, `scripts/sh2_prepare_overlays.py`):**
   - Raised the code-window capture cap to 3 MiB (`0x300000`) and deduplicated overlapping `PT_LOAD` overlay segments.
   - Added `MWo3` `.text` bounds detection and whole-image + leaf + ELF-pointer scanning in `eerecomp` so 100% of `OPENING.BIN`, `FIELD.BIN`, and `BATTLE.BIN` (including internal vtables, switch jump tables, and static constructors) are compiled ahead-of-time at `-O2` with check words strictly on immutable `.text` instructions (eliminating mid-game `ee-live` translation freezes).
5. **Audio & Background Live-Compiler Smoothness (`host/stretch.hpp`, `host/stretch.cpp`, `host/window.cpp`, `runtime/src/ee_live.cpp`, `runtime/src/vu_live.cpp`):**
   - Added clean audio pause/fade during the `F1` quick menu (so menu pauses never drain the audio queue or trigger underruns), tuned the audio safety buffer (`target_ms = 85 ms`), and throttled fallback live-compile threads on 4-core CPUs so background compilation never starves gameplay or audio.
6. **Multi-Disc Virtual Drive & Auto-Swap (`runtime/src/disc.cpp`, `runtime/src/iop/cdvdfsv.cpp`, `host/main.cpp`):**
   - Added `--disc2` / `PS2_DISC2` / sibling `disc2.iso` support, save-state active-disc persistence, and `SSHSys::DiskChange` automatic disc switching.
7. **4x Resolution 60 FPS Battle Performance & GPU Controls (`runtime/src/gs/gpu/gpu.cpp`, `runtime/src/gs/gpu/gs.wgsl`, `runtime/src/dmac.cpp`, `host/window.cpp`, `host/quickmenu.cpp`, `host/keymap.cpp`):**
   - Fixed a circular upload cache overlap bug in `cache_insert()` that previously shattered battle frames into 79 WebGPU submits per frame (now 1 submit/frame) and expanded the upload cache to 64 MB with inter-frame headroom rotation.
   - Fixed a full-screen 1:1 sprite copy precision check in `gs.wgsl` that triggered a 49-tap (196 atomic load) tent filter at 4x scale (`10.93 ms` -> `<1 ms` for that pass), cutting total 4x battle GPU frame time from `20.23 ms` to `7.64 ms` (`2.65x` faster).
   - Enabled the async GPU submit thread by default on Windows (`PS2_GPU_SUBMIT_THREAD=1`), batched `VIF1`/`GIF` DMA chain ring posts, added GPU adapter / Graphics API / live Async Submit Thread controls to the `F1` Quick Menu & `keys.ini`, and normalized the title bar CPU % readout to match Windows Task Manager alongside main-thread % (`CPU X% (main Y%)`).

---

## Building from Your Own ISOs (Windows)

Double-click [`Build Shadow Hearts Covenant.cmd`](Build%20Shadow%20Hearts%20Covenant.cmd):
- Auto-detects `games\sh2-covenant\disc.iso` and `disc2.iso` (or prompts you to paste/drag-and-drop your NTSC-U Disc 1 & Disc 2 ISO paths on a fresh install).
- Preserves your saves (`memcard.ps2`) and keybindings (`keys.ini`).
- Extracts and translates `SLUS_210.41`, all 3 overlays (`OPENING.BIN`, `FIELD.BIN`, `BATTLE.BIN`), and any recorded VU1/VU0 vector programs with a live progress bar, then clears stale live-compile caches.

Once built, launch with [`Play Shadow Hearts Covenant.cmd`](Play%20Shadow%20Hearts%20Covenant.cmd).
