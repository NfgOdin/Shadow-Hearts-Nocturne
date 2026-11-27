# Shadow Hearts: Nocturne — Modder's Handbook

Welcome to the official **4-Layer Mod Engine & SDK Guide** for **Shadow Hearts: Nocturne**, covering:
- **Koudelka** (`koudelka` — PS1, 4 discs)
- **Shadow Hearts** (`sh1` — PS2)
- **Shadow Hearts: Covenant** (`sh2-covenant` — PS2)
- **Shadow Hearts: From the New World** (`sh3-ftnw` — PS2)

---

## 1. Quickstart for Modders

Regular players install `.nocturne-mod` packages from the Launcher and toggle them in-game via `F1 → MODS`.
Developers and modders who clone/download this repository get the **Nocturne Mod Studio v2** standalone desktop workbench:

1. Double-click **`Nocturne Mod Studio.cmd`** in the repository root.
2. Pick your target game (`sh1`, `sh2-covenant`, `sh3-ftnw`, or `koudelka`) and load a **Starter Preset** or start blank.
3. Press **`Ctrl+S`** (or click **Save & Install**) to write your mod directly into `games/<slug>/mods/<YourMod>/`.
4. Click **Launch Game (`--mod-dev`)** to test with live developer tools enabled:
   - **Auto Hot-Reload:** Saving any `.lua`, `.pnach`, `mod.json`, or `mod_config.ini` reloads automatically within ~0.5 seconds.
   - **`F9`:** Instant manual reload of all active mods.
   - **`F10`:** Toggle the **Live Modder Inspector & Lua Log Console** (live Cash, Battle Wins, Ring stats/streaks, Party HP/MP/Lv RAM snapshot, Conflicts, and `log()` output + `games/<slug>/mods.log`).

> **Note:** Live dev tools (`--mod-dev` / `mod_dev_mode = 1`) and Layer 4 native `.dll` plugins (`allow_native_plugins = 1`) are **off by default** so regular players never encounter developer overlays or untrusted native code.

---

## 2. Unified `.nocturne-mod` Folder Anatomy & Shared Mods

Mods can be installed either per-game (`games/<slug>/mods/<ModFolder>/`) or **once globally across games** in `games/shared/mods/<ModFolder>/`:
- **Shared Mods (`games/shared/mods/`):** Filtered automatically by `mod.json`'s `"game"` field (`"all"` / `"shared"` for all 4 games, `"all_ps2"` for SH1/SH2/SH3, or a comma-separated list). Each game keeps its own enable/disable toggle in `games/<slug>/keys.ini` and **independent per-game settings** in `games/<slug>/mods_config/<ModFolder>.ini` (plus per-game persistent state in `<ModFolder>.state.ini`).
- **Co-op Safety (`"coop_safe"`):** Pure client-side visual/HUD mods (with zero `files/`, `patches/`, or `plugins/`) can declare `"coop_safe": true` in `mod.json` so they stay active during Online/LAN Co-op even if your partner doesn't have them installed. Gameplay-mutating mods (`"coop_safe": false`, default) are automatically hash-verified between Host & Guest (`PacketType::ModManifest`) and safely paused on mismatch to prevent lockstep desyncs.
- **In-Launcher GitHub Mod Catalog:** Official and community mods listed in `assets/mods/catalog.json` can be browsed and 1-click installed/updated directly inside **Launcher → Tools → HD Packs & Mods → Catalog** (with automatic offline fallback).

```text
MyCustomMod/
├── mod.json                  # Metadata, author, version, game, coop_safe, category, optional "settings"
├── mod_config.ini            # User-tuned setting overrides (or games/<slug>/mods_config/<Mod>.ini for shared mods)
├── mod_state.ini             # Per-mod persistent save data (via save_get / save_set)
├── files/                    # Layer 1a: Virtual disc file overrides (PS2 & Koudelka; highest priority wins)
├── textures/                 # Layer 1b: Bundled HD texture PNG overrides (PS2; highest priority wins)
├── video/                    # Layer 1c: Bundled HD MP4 cutscene overrides (PS2; highest priority wins)
├── models/                   # Layer 1d: High-poly glTF 2.0 (.glb) skinned 3D model swaps (Koudelka D3D11)
├── patches/
│   └── custom.pnach          # Layer 2: Declarative .pnach RAM & code patches (top-priority mod wins)
├── scripts/
│   └── main.lua              # Layer 3: Full Sandboxed Lua 5.4.7 scripts (tables, string/math/table/utf8, mod-jailed require)
└── plugins/                  # Layer 4: Native C++ .dll plugins (opt-in; all V1 callbacks dispatched)
    └── my_plugin.dll
```

### `mod.json` Example (with Configurable Settings)

Players can edit any mod's `"settings"` both in the **Launcher (`Tools -> HD Packs & Mods -> Config`)** and **live in-game (`F1 -> MODS -> Right / Settings`)** across all 4 games. Supported types are `"bool"`, `"int"`, `"float"`, `"choice"` (with `"options": [...]`), and `"hotkey"` (top-row number keys `"1"`–`"9"`, `"0"`, or `"*"`).

```json
{
  "id": "crimson_bounty",
  "name": "Crimson Ring Bounty",
  "version": "1.0.0",
  "author": "YourName",
  "game": "sh2-covenant",
  "description": "Escalating Cash & pinned streak HUD for Perfect Judgment Rings.",
  "settings": [
    {
      "key": "enabled_bonus",
      "label": "Enable Cash Bonus",
      "description": "Award bonus Cash on Perfect Judgment Ring strikes",
      "type": "bool",
      "default": 1
    },
    {
      "key": "bounty_mult",
      "label": "Bounty Multiplier",
      "description": "Cash multiplier per streak level",
      "type": "int",
      "default": 1,
      "min": 1,
      "max": 5,
      "step": 1
    },
    {
      "key": "glow_color",
      "label": "Glow Color",
      "description": "Accent palette for HUD / lantern effects",
      "type": "choice",
      "options": ["Gold", "Cyan", "Crimson"],
      "default": "Gold"
    },
    {
      "key": "toggle_key",
      "label": "Toggle Hotkey",
      "description": "Keyboard key to toggle this mod in-game",
      "type": "hotkey",
      "default": "*"
    }
  ]
}
```

### Load Order & Conflict Resolution
Mods load top-to-bottom according to `active_mods` in `games/<slug>/keys.ini` (e.g. `ModA,ModB,!DisabledMod`).
- **Top-priority mod wins** for overlapping `files/`, `textures/`, and `video/` overrides.
- Use **Tab 6 (Conflicts & Dev Tools)** in Mod Studio or the in-game **`F10` Inspector** to inspect any overlapping files, textures, videos, or `.pnach` addresses.

---

## 3. Layer 3 — Sandboxed Lua 5.4.7 Runtime & Semantic RPG API

Layer 3 runs an official, sandboxed **Lua 5.4.7** VM per script (`tables`, `pairs`/`ipairs`, closures, `string.*`, `math.*`, `table.*`, `utf8.*`, plus a mod-folder-jailed `require()`). Unsafe OS, filesystem, coroutine, and dynamic-bytecode APIs (`os`, `io`, `debug`, `coroutine`, `dofile`, `loadfile`, `load`) are stripped and every hook runs under a deterministic 50,000-instruction budget designed to prevent runaway scripts from freezing emulation.

Common RPG mechanics require **zero hex addresses** and work across all 4 games.

### Automatic Lifecycle & Gameplay Hooks

| Hook Function | When It Fires |
| --- | --- |
| `on_boot()` | Once when the game boots (or after `F9` / hot-reload). |
| `on_game_ready()` | Once a live save/gameplay state is detected in RAM. |
| `on_frame()` | Every emulated frame (~60 Hz). |
| `on_area_load(room_id)` | When entering a new room/area (e.g. Koudelka room transitions). |
| `on_battle_start()` | When a battle encounter begins. |
| `on_battle_end()` | When a battle finishes and returns to field/menus. |
| `on_turn_begin(char_slot)` | When a party member's turn begins in battle (`0=Koudelka, 1=Edward, 2=James`). |
| `on_level_up(char_slot)` | When a hero enters the Level-Up screen. |
| `on_battle_win(total_wins)` | Immediately when the party wins a battle. |
| `on_ring_result(outcome, streak)` | On any Judgment Ring outcome (`"perfect"`, `"hit"`, or `"miss"`) with current/broken `streak`. |
| `on_ring_perfect(streak)` | On a Perfect Judgment Ring strike (`streak` = consecutive perfects). |
| `on_ring_hit(streak)` | On a non-perfect Judgment Ring hit (`streak` = consecutive hits). |
| `on_ring_miss(prev_streak)` | When a Judgment Ring attempt misses (`prev_streak` = broken streak). |
| `on_hotkey(key)` | Edge-triggered when top-row number keys `1`–`9`, `0`, or `*` are pressed (engine/launcher actions use `F1`–`F12`, and save states use Numpad `0`–`9` / `Shift+Numpad 0`–`9`). |
| `on_setting_changed(key, val)` | Immediately when a player adjusts a setting live in `F1 -> MODS` (without resetting Lua globals). |

### High-Level Semantic Built-ins (All 4 Games)

| Category | Functions |
| --- | --- |
| **Game & Economy** | `is_in_game()`, `game_slug()`, `frame()`, `get_cash()`, `add_cash(delta)` |
| **Battle & Ring** | `get_battles_won()`, `get_ring_attempts()`, `get_ring_successes()`, `get_ring_perfects()`, `get_max_level()` |
| **Party Actions** | `heal_party(pct)`, `restore_party_mp(pct)`, `add_party_exp(amount)`, `sync_bench_exp()`, `unlock_all_party()` *(all 4 games)* |
| **Per-Hero Stats** *(0-based slot)* | `get_hero_count()`, `get_hero_joined(slot)`, `get_hero_name(slot)`, `get_hero_level(slot)`, `get_hero_hp(slot)`, `get_hero_max_hp(slot)`, `set_hero_hp(slot, val)`, `get_hero_mp(slot)`, `get_hero_max_mp(slot)`, `set_hero_mp(slot, val)`, `get_hero_exp(slot)`, `set_hero_exp(slot, val)`, `get_hero_weapon_durability(slot)`, `get_hero_weapon_max_durability(slot)` |
| **Persistent Save Data** | `save_get(key, default)`, `save_set(key, val)`, `save_delete(key)` |
| **Mod Settings** | `get_setting(key, default)`, `get_setting_str(key, default)` |
| **Input & Hotkeys** | `is_key_down(name)`, `is_key_pressed(name)`, `is_pad_down(btn)`, `is_pad_pressed(btn)`, `pad_press(btn)`, `pad_block(btn)` |
| **Custom 2D UI Canvas** | `draw_rect(x, y, w, h, hex, alpha)`, `draw_bar(x, y, w, h, ratio, fg_hex, bg_hex, label)`, `draw_text(x, y, text, size, hex)`, `draw_clear()` *(960×720 virtual coords)* |
| **Custom Audio** | `play_sfx(rel_wav_path)` *(plays `.wav` from `mods/<mod>/audio/`)* |
| **HUD & Logging** | `toast(msg)`, `hud_set(id, label, value, color_hex)`, `hud_clear(id)`, `hud_clear_all()`, `log(msg)` / `print(...)` |
| **Raw RAM & AOB Tools** | `read_u8/u16/u32/s32/f32(addr)`, `write_u8/u16/u32/f32(addr, val)`, `read_str(addr, max)`, `write_str(addr, str)`, `scan_bytes("4B 6F ?? 64", start, end)`, `bit_and/or/xor/shl/shr`, `min`, `max`, `clamp`, `floor`, `ceil`, `abs`, `rand()`, `randint(lo, hi)`, `tostring`, `tonumber` |

### Example: Persistent Hotkey + Pinned HUD Counter

```lua
function on_game_ready()
    local uses = save_get("heal_uses", 0)
    hud_set("sanctuary", "SANCTUARY HEALS", tostring(uses), "#7CD992")
    toast("Sanctuary Hotkey Ready — Press 5 to Heal Party")
end

function on_hotkey(key)
    if key == "5" and is_in_game() then
        local pct = get_setting("heal_pct", 50)
        heal_party(pct)
        local uses = save_get("heal_uses", 0) + 1
        save_set("heal_uses", uses)
        hud_set("sanctuary", "SANCTUARY HEALS", tostring(uses), "#7CD992")
        log("Sanctuary [5] heal #" .. uses .. " (+" .. pct .. "% HP)")
        toast("Sanctuary Heal! +" .. pct .. "% HP")
    end
end
```

---

## 4. Layer 2 — `.pnach` Memory & Code Patches

Drop any `.pnach` file into `patches/` (or use **Tab 3** in Mod Studio):

```text
// Format: patch=<0=boot-only | 1=every-frame>,EE,<hex_addr>,<byte|short|word|extended>,<hex_value>
patch=0,EE,00102D30,word,00002710  // Set SH2 Wallet 0 to 10,000 Cash on boot
```

---

## 5. Layer 4 — Native C++ `.dll` Plugin SDK

For native x64 speed or complex custom systems, include [`sdk/nocturne_mod_api.h`](../sdk/nocturne_mod_api.h) and export `NocturneModInitV1`:

```cpp
#include "nocturne_mod_api.h"

static void on_frame(const NocturneModApiV1 *api, uint64_t frame_index) {
    (void)frame_index;
    // Direct safe guest RAM access via api->read_u32 / api->write_u32
}

extern "C" NOCTURNE_MOD_EXPORT bool NocturneModInitV1(
    const NocturneModApiV1 *api, NocturneModCallbacksV1 *out_cbs) {
    if (!api || !out_cbs || api->api_version != NOCTURNE_MOD_API_VERSION) return false;
    out_cbs->on_frame = on_frame;
    return true;
}
```

Build with the portable `llvm-mingw` toolchain (or use **Tab 5 → Generate Layer 4 C++ Plugin Project** in Mod Studio):

```powershell
tools\portable\llvm-mingw\bin\clang++.exe -shared -O2 -std=c++20 -I sdk my_plugin.cpp -o my_plugin.dll
```

Drop `my_plugin.dll` in `games/<slug>/mods/<YourMod>/plugins/` and set `allow_native_plugins = 1` in `games/<slug>/keys.ini`.

---

## 6. Verified RAM Quick Reference

| Game | Cash | Battles Won | Ring Stats | Party Array |
| --- | --- | --- | --- | --- |
| **SH1** | `0x00100008` (`u32`) | `0x001014A4` (`u32`) | `0x001014AC/B0/B4` | `0x00100280 + i*0x50` (6 slots) |
| **SH2: Covenant** | `0x00102D30 + (u8[0x00100132]&3)*4` | `0x001016E4` (`u32`) | `0x001017C8/CC/D0` | `0x00100250 + i*0x90` (8 slots) |
| **SH3: FTNW** | `0x00100D54` (`u32`) | `0x00102734` (`u32`) | `0x00102778 + i*0x90 + {0x0C,0x18,0x1C}` | `0x00100D70 + i*0x84` (7 slots) |
| **Koudelka (PS1)** | — | — | — | Root ptr `0x000B59A0`, actors at `root - 0x44/48/4C` |

---

## 7. Packaging & Sharing Mods

Click **Export `.nocturne-mod`** in Nocturne Mod Studio v2 to generate a single `.nocturne-mod` archive containing your `mod.json`, `scripts/`, `patches/`, `files/`, `textures/`, `video/`, and `models/`. Players can install it in 1 click via **Launcher → Tools → HD Packs & Mods → Import `.zip / .nocturne-mod`**.
