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

## 2. Unified `.nocturne-mod` Folder Anatomy

Every mod lives in `games/<slug>/mods/<ModFolder>/` and can combine **all 4 layers** in one self-contained package:

```text
MyCustomMod/
├── mod.json                  # Metadata, author, version, optional "settings" schema
├── mod_config.ini            # User-tuned setting overrides (auto-managed)
├── mod_state.ini             # Per-mod persistent save data (via save_get / save_set)
├── files/                    # Layer 1a: Virtual disc file overrides (highest priority wins)
├── textures/                 # Layer 1b: Bundled HD texture PNG overrides
├── video/                    # Layer 1c: Bundled HD MP4 cutscene overrides
├── patches/
│   └── custom.pnach          # Layer 2: Declarative .pnach RAM & code patches
├── scripts/
│   └── main.lua              # Layer 3: Embedded Lua-subset script hooks
└── plugins/                  # Layer 4: Native C++ .dll plugins (opt-in)
    └── my_plugin.dll
```

### `mod.json` Example (with Configurable Settings)

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
      "key": "bounty_mult",
      "label": "Bounty Multiplier",
      "type": "int",
      "default": 1,
      "min": 1,
      "max": 5,
      "step": 1
    }
  ]
}
```

### Load Order & Conflict Resolution
Mods load top-to-bottom according to `active_mods` in `games/<slug>/keys.ini` (e.g. `ModA,ModB,!DisabledMod`).
- **Top-priority mod wins** for overlapping `files/`, `textures/`, and `video/` overrides.
- Use **Tab 6 (Conflicts & Dev Tools)** in Mod Studio or the in-game **`F10` Inspector** to inspect any overlapping files, textures, videos, or `.pnach` addresses.

---

## 3. Layer 3 — Semantic Lua API & Live Event Hooks

Layer 3 scripts require **zero hex addresses** for common RPG mechanics and work across all 4 games.

### Automatic Lifecycle & Gameplay Hooks

| Hook Function | When It Fires |
| --- | --- |
| `on_boot()` | Once when the game boots (or after `F9` / hot-reload). |
| `on_game_ready()` | Once a live save/gameplay state is detected in RAM. |
| `on_frame()` | Every emulated frame (~60 Hz). |
| `on_battle_win(total_wins)` | Immediately when the party wins a battle. |
| `on_ring_perfect(streak)` | On a Perfect Judgment Ring strike (`streak` = consecutive perfects). |
| `on_ring_hit(streak)` | On a non-perfect Judgment Ring hit (`streak` = consecutive hits). |
| `on_ring_miss(prev_streak)` | When a Judgment Ring attempt misses (`prev_streak` = broken streak). |
| `on_hotkey(key)` | Edge-triggered when `F5`–`F8` or `1`–`9` is pressed. |

### High-Level Semantic Built-ins (All 4 Games)

| Category | Functions |
| --- | --- |
| **Game & Economy** | `is_in_game()`, `game_slug()`, `frame()`, `get_cash()`, `add_cash(delta)` |
| **Battle & Ring** | `get_battles_won()`, `get_ring_attempts()`, `get_ring_successes()`, `get_ring_perfects()`, `get_max_level()` |
| **Party Actions** | `heal_party(pct)`, `add_party_exp(amount)`, `sync_bench_exp()`, `unlock_all_party()` |
| **Per-Hero Stats** *(0-based slot)* | `get_hero_count()`, `get_hero_joined(slot)`, `get_hero_level(slot)`, `get_hero_hp(slot)`, `get_hero_max_hp(slot)`, `set_hero_hp(slot, val)`, `get_hero_mp(slot)`, `get_hero_max_mp(slot)`, `set_hero_mp(slot, val)`, `get_hero_exp(slot)`, `set_hero_exp(slot, val)` |
| **Persistent Save Data** | `save_get(key, default)`, `save_set(key, val)`, `save_delete(key)` |
| **Mod Settings** | `get_setting(key, default)` |
| **Input & Hotkeys** | `is_key_down(name)`, `is_key_pressed(name)`, `is_pad_down(btn)`, `is_pad_pressed(btn)` |
| **HUD & Logging** | `toast(msg)`, `hud_set(id, label, value, color_hex)`, `hud_clear(id)`, `hud_clear_all()`, `log(msg)` / `print(...)` |
| **Raw RAM & Math** | `read_u8/u16/u32/s32/f32(addr)`, `write_u8/u16/u32/f32(addr, val)`, `bit_and/or/xor/shl/shr`, `min`, `max`, `clamp`, `floor`, `ceil`, `abs`, `rand()`, `randint(lo, hi)`, `tostring`, `tonumber` |

### Example: Persistent Hotkey + Pinned HUD Counter

```lua
function on_game_ready()
    local uses = save_get("heal_uses", 0)
    hud_set("sanctuary", "SANCTUARY HEALS", tostring(uses), "#7CD992")
    toast("Sanctuary Hotkey Ready — Press F5 to Heal Party")
end

function on_hotkey(key)
    if key == "F5" and is_in_game() then
        local pct = get_setting("heal_pct", 50)
        heal_party(pct)
        local uses = save_get("heal_uses", 0) + 1
        save_set("heal_uses", uses)
        hud_set("sanctuary", "SANCTUARY HEALS", tostring(uses), "#7CD992")
        log("Sanctuary F5 heal #" .. uses .. " (+" .. pct .. "% HP)")
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

Click **Export `.nocturne-mod`** in Nocturne Mod Studio v2 to generate a single `.nocturne-mod` archive containing your `mod.json`, `scripts/`, `patches/`, `files/`, `textures/`, and `video/`. Players can install it in 1 click via **Launcher → Tools → Packs & Mods → Import `.zip / .nocturne-mod`**.
