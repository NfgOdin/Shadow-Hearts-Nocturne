#pragma once

// Shadow Hearts: Nocturne — 4-Layer Gameplay Mod Engine & C-API Plugin Foundation
// - Layer 1: Virtual Disc File Overrides (games/<slug>/mods/<mod>/files/<rel_path>)
// - Layer 2: Declarative .pnach Memory & Code Patches (patch=1,EE,<addr>,extended|word|short|byte,<val>)
// - Layer 3: Embedded Lua-Subset Script Engine (games/<slug>/mods/<mod>/scripts/*.lua:
//            on_boot, on_frame, on_battle_start, on_battle_end, on_turn_begin, on_ring_result, on_area_load;
//            full read/write RAM API, variables, if/elseif/else, for/while loops, math/bitwise, live toast())
// - Layer 4: Stable C-API v1 Hook Table (NocturneModApiV1) for native .dll plugins

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

extern "C" {

// Layer 4 Stable C-API v1 for native .dll plugins (exported entry point: NocturneModInitV1)
struct NocturneModApiV1 {
    std::uint32_t api_version;  // 1
    const char *game_slug;      // "koudelka", "sh1", "sh2-covenant", "sh3-ftnw"
    std::uint8_t *guest_ram;    // Base pointer to guest RAM (2 MB PS1 or 32 MB PS2 EE)
    std::uint32_t guest_ram_size;

    std::uint8_t  (*read_u8)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr);
    std::uint16_t (*read_u16)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr);
    std::uint32_t (*read_u32)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr);
    void          (*write_u8)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr, std::uint8_t val);
    void          (*write_u16)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr, std::uint16_t val);
    void          (*write_u32)(std::uint8_t *ram, std::uint32_t ram_size, std::uint32_t addr, std::uint32_t val);
};

struct NocturneModCallbacksV1 {
    void (*on_boot)(const NocturneModApiV1 *api);
    void (*on_frame)(const NocturneModApiV1 *api, std::uint64_t frame_index);
    void (*on_battle_start)(const NocturneModApiV1 *api, std::uint32_t battle_id);
    void (*on_turn_begin)(const NocturneModApiV1 *api, std::uint32_t actor_slot);
    void (*on_ring_result)(const NocturneModApiV1 *api, std::uint32_t actor_slot, std::uint32_t hits, std::uint32_t perfects);
    void (*on_area_load)(const NocturneModApiV1 *api, const char *area_code);
};

using NocturneModInitFn = bool (*)(const NocturneModApiV1 *api, NocturneModCallbacksV1 *out_callbacks);

}  // extern "C"

namespace mod_engine {

enum class PatchWidth : std::uint8_t {
    Byte = 1,
    Short = 2,
    Word = 4,
};

struct PnachPatch {
    std::uint32_t addr = 0;
    std::uint32_t value = 0;
    PatchWidth width = PatchWidth::Word;
    bool on_boot_only = false;
};

enum class ScriptHookEvent : std::uint8_t {
    OnBoot = 0,
    OnFrame = 1,
    OnBattleStart = 2,
    OnTurnBegin = 3,
    OnRingResult = 4,
    OnAreaLoad = 5,
    OnBattleEnd = 6,
};

// Input bitmasks for Layer 3 hotkey & gamepad hooks
namespace mod_input {
constexpr std::uint32_t kKeyF5    = 1u << 0;
constexpr std::uint32_t kKeyF6    = 1u << 1;
constexpr std::uint32_t kKeyF7    = 1u << 2;
constexpr std::uint32_t kKeyF8    = 1u << 3;
constexpr std::uint32_t kKey1     = 1u << 4;
constexpr std::uint32_t kKey2     = 1u << 5;
constexpr std::uint32_t kKey3     = 1u << 6;
constexpr std::uint32_t kKey4     = 1u << 7;
constexpr std::uint32_t kKey5     = 1u << 8;
constexpr std::uint32_t kKey6     = 1u << 9;
constexpr std::uint32_t kKey7     = 1u << 10;
constexpr std::uint32_t kKey8     = 1u << 11;
constexpr std::uint32_t kKey9     = 1u << 12;
constexpr std::uint32_t kKeyShift    = 1u << 13;
constexpr std::uint32_t kKeyCtrl     = 1u << 14;
constexpr std::uint32_t kKeyAlt      = 1u << 15;
constexpr std::uint32_t kKeyAsterisk = 1u << 16;
constexpr std::uint32_t kKey0        = 1u << 17;

constexpr std::uint32_t kPadCross    = 1u << 0;
constexpr std::uint32_t kPadCircle   = 1u << 1;
constexpr std::uint32_t kPadSquare   = 1u << 2;
constexpr std::uint32_t kPadTriangle = 1u << 3;
constexpr std::uint32_t kPadL1       = 1u << 4;
constexpr std::uint32_t kPadR1       = 1u << 5;
constexpr std::uint32_t kPadL2       = 1u << 6;
constexpr std::uint32_t kPadR2       = 1u << 7;
constexpr std::uint32_t kPadSelect   = 1u << 8;
constexpr std::uint32_t kPadStart    = 1u << 9;
constexpr std::uint32_t kPadUp       = 1u << 10;
constexpr std::uint32_t kPadDown     = 1u << 11;
constexpr std::uint32_t kPadLeft     = 1u << 12;
constexpr std::uint32_t kPadRight    = 1u << 13;

inline std::uint32_t key_mask_from_name(const std::string &name) {
    std::string u;
    u.reserve(name.size());
    for (char c : name) u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (u == "F5") return kKeyF5;
    if (u == "F6") return kKeyF6;
    if (u == "F7") return kKeyF7;
    if (u == "F8") return kKeyF8;
    if (u == "1") return kKey1;
    if (u == "2") return kKey2;
    if (u == "3") return kKey3;
    if (u == "4") return kKey4;
    if (u == "5") return kKey5;
    if (u == "6") return kKey6;
    if (u == "7") return kKey7;
    if (u == "8") return kKey8;
    if (u == "9") return kKey9;
    if (u == "SHIFT") return kKeyShift;
    if (u == "CTRL" || u == "CONTROL") return kKeyCtrl;
    if (u == "ALT") return kKeyAlt;
    if (u == "*" || u == "ASTERISK" || u == "STAR" || u == "KP_MULTIPLY" || u == "NUMPAD*" || u == "MULTIPLY") {
        return kKeyAsterisk;
    }
    return 0u;
}

inline std::uint32_t pad_mask_from_name(const std::string &name) {
    std::string u;
    u.reserve(name.size());
    for (char c : name) u.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (u == "CROSS" || u == "X" || u == "A") return kPadCross;
    if (u == "CIRCLE" || u == "O" || u == "B") return kPadCircle;
    if (u == "SQUARE") return kPadSquare;
    if (u == "TRIANGLE" || u == "Y") return kPadTriangle;
    if (u == "L1" || u == "LB") return kPadL1;
    if (u == "R1" || u == "RB") return kPadR1;
    if (u == "L2" || u == "LT") return kPadL2;
    if (u == "R2" || u == "RT") return kPadR2;
    if (u == "SELECT" || u == "BACK") return kPadSelect;
    if (u == "START") return kPadStart;
    if (u == "UP") return kPadUp;
    if (u == "DOWN") return kPadDown;
    if (u == "LEFT") return kPadLeft;
    if (u == "RIGHT") return kPadRight;
    return 0u;
}
}  // namespace mod_input

struct HudWidget {
    std::string id;
    std::string label;
    std::string value;
    std::uint32_t color_rgb = 0xE6C88Au;  // antique gold default
};

// Immediate-mode 2D custom UI drawing primitives (960x720 virtual coordinates, auto-scaled)
struct CanvasCmd {
    enum class Kind : std::uint8_t { Rect = 0, Bar = 1, Text = 2 };
    Kind kind = Kind::Rect;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float ratio = 1.0f;       // Bar fill ratio 0..1
    int font_size = 14;       // Text size
    std::uint32_t color_rgb = 0xE6C88Au;
    std::uint32_t bg_rgb = 0x121018u;
    int alpha = 220;
    std::string text;
};

struct ModSettingDef {
    std::string key;
    std::string label;
    std::string description;
    std::string type;  // "bool", "int", "float", "choice", "hotkey"
    double default_val = 0.0;
    double min_val = 0.0;
    double max_val = 100.0;
    double step = 1.0;
    double current_val = 0.0;
    std::vector<std::string> options;  // for "choice" or "hotkey"
    std::string default_str;           // for "hotkey" (and label for "choice")
    std::string current_str;           // for "hotkey" (and label for "choice")
};

inline const std::vector<std::string> &mod_hotkey_choices() {
    static const std::vector<std::string> kKeys = {
        "*", "F5", "F7", "F8", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F6"
    };
    return kKeys;
}

inline std::string format_mod_setting_value(const ModSettingDef &sd) {
    if (sd.type == "bool") {
        return (sd.current_val >= 0.5) ? "ON" : "OFF";
    }
    if (sd.type == "hotkey") {
        return sd.current_str.empty() ? "*" : sd.current_str;
    }
    if (sd.type == "choice") {
        const int idx = static_cast<int>(std::lround(sd.current_val));
        if (idx >= 0 && idx < static_cast<int>(sd.options.size())) {
            return sd.options[static_cast<std::size_t>(idx)];
        }
        return sd.current_str.empty() ? std::to_string(idx) : sd.current_str;
    }
    if (sd.type == "float") {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.2f", sd.current_val);
        std::string s(buf);
        while (s.size() > 1 && s.back() == '0' && s.find('.') != std::string::npos) s.pop_back();
        if (!s.empty() && s.back() == '.') s.push_back('0');
        return s;
    }
    return std::to_string(static_cast<long long>(std::lround(sd.current_val)));
}

inline void step_mod_setting(ModSettingDef &sd, int dir) {
    if (dir == 0) dir = 1;
    if (sd.type == "bool") {
        sd.current_val = (sd.current_val >= 0.5) ? 0.0 : 1.0;
        sd.current_str = (sd.current_val >= 0.5) ? "1" : "0";
        return;
    }
    if (sd.type == "choice" || sd.type == "hotkey") {
        const auto &opts = (sd.type == "hotkey" && sd.options.empty()) ? mod_hotkey_choices() : sd.options;
        if (opts.empty()) return;
        const int n = static_cast<int>(opts.size());
        int idx = static_cast<int>(std::lround(sd.current_val));
        if (sd.type == "hotkey") {
            for (int i = 0; i < n; ++i) {
                if (opts[static_cast<std::size_t>(i)] == sd.current_str) { idx = i; break; }
            }
        }
        idx = (idx + (dir > 0 ? 1 : -1) + n) % n;
        sd.current_val = static_cast<double>(idx);
        sd.current_str = opts[static_cast<std::size_t>(idx)];
        return;
    }
    const double step = (sd.step > 0.0) ? sd.step : 1.0;
    double nv = std::clamp(sd.current_val + (dir > 0 ? step : -step), sd.min_val, sd.max_val);
    if (sd.type == "float") {
        nv = std::round(nv * 10000.0) / 10000.0;
    } else {
        nv = std::round(nv);
    }
    sd.current_val = std::clamp(nv, sd.min_val, sd.max_val);
}

inline void reset_mod_settings(std::vector<ModSettingDef> &settings) {
    for (auto &sd : settings) {
        sd.current_val = sd.default_val;
        sd.current_str = sd.default_str;
    }
}

struct ModConflict {
    std::string layer;      // "files", "textures", "video", "patches"
    std::string resource;
    std::string winner_mod;
    std::string shadowed_mod;
};

struct LiveInspectorSnapshot {
    std::string game_slug;
    int detected_game = 0;  // 0=none, 1=SH1, 2=SH2, 3=SH3, 4=Koudelka
    bool in_game = false;
    std::uint32_t cash = 0;
    std::uint32_t battles_won = 0;
    std::uint32_t ring_attempts = 0;
    std::uint32_t ring_successes = 0;
    std::uint32_t ring_perfects = 0;
    int hit_streak = 0;
    int perf_streak = 0;
    int max_level = 0;
    struct HeroSnap {
        int slot = 0;
        bool joined = false;
        int lv = 0;
        int hp = 0;
        int max_hp = 0;
        int mp = 0;
        int max_mp = 0;
        std::uint32_t exp = 0;
    };
    std::vector<HeroSnap> heroes;
};

struct InstalledMod {
    std::string folder;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string category;
    std::string target_game;
    bool enabled = true;
    bool is_shared = false;
    bool coop_safe = false;
    std::filesystem::path mod_dir;
    std::filesystem::path config_path;
    std::filesystem::path state_path;
    std::size_t file_count = 0;
    std::size_t texture_count = 0;
    std::size_t video_count = 0;
    std::size_t model_count = 0;
    std::size_t patch_count = 0;
    std::size_t script_count = 0;
    std::size_t plugin_count = 0;
    std::vector<ModSettingDef> settings;
};

// ---------------- Shared 4-Game Semantic RPG RAM Helpers & Lua 5.4 Runtime (Layer 3) ----------------
namespace detail {

struct Val {
    double num = 0.0;
    std::string str;
    bool is_str = false;
    bool is_nil = false;

    static Val number(double v) { Val r; r.num = v; return r; }
    static Val string(std::string s) { Val r; r.str = std::move(s); r.is_str = true; return r; }
    static Val boolean(bool b) { Val r; r.num = b ? 1.0 : 0.0; return r; }
    static Val nil() { Val r; r.is_nil = true; return r; }

    bool truthy() const noexcept {
        if (is_nil) return false;
        if (is_str) return true;
        return num != 0.0;
    }

    std::string to_string() const {
        if (is_str) return str;
        if (is_nil) return "nil";
        const double r = std::round(num);
        if (std::fabs(num - r) < 1e-9 && std::fabs(r) < 9e15) {
            return std::to_string(static_cast<std::int64_t>(r));
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4g", num);
        return buf;
    }
};

struct ScriptHostCtx {
    std::uint8_t *ram = nullptr;
    std::size_t ram_size = 0;
    std::uint64_t frame_index = 0;
    std::string game_slug;
    std::vector<std::string> *toast_queue = nullptr;
    std::uint32_t *rng_state = nullptr;
    std::uint32_t keys_down = 0;
    std::uint32_t keys_pressed = 0;
    std::uint32_t pad_down = 0;
    std::uint32_t pad_pressed = 0;
    std::vector<HudWidget> *hud_widgets = nullptr;
    std::vector<CanvasCmd> *canvas_cmds = nullptr;
    std::uint32_t *pad_inject = nullptr;
    std::uint32_t *pad_block = nullptr;
    std::deque<std::string> *log_ring = nullptr;
    float *item_glow_strength = nullptr;
    std::uint32_t *item_glow_rgb = nullptr;
    float *item_glow_pulse_speed = nullptr;
    std::filesystem::path dev_log_path;
};

struct SemanticRam {
    static std::uint8_t rd8(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
        if (!ram || ram_size == 0) return 0u;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        return (off < ram_size) ? ram[off] : 0u;
    }
    static std::uint16_t rd16(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
        if (!ram || ram_size == 0) return 0u;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        std::uint16_t v = 0;
        if (off + 2 <= ram_size) std::memcpy(&v, ram + off, 2);
        return v;
    }
    static std::uint32_t rd32(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr) noexcept {
        if (!ram || ram_size == 0) return 0u;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        std::uint32_t v = 0;
        if (off + 4 <= ram_size) std::memcpy(&v, ram + off, 4);
        return v;
    }
    static void wr8(std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr, std::uint8_t v) noexcept {
        if (!ram || ram_size == 0) return;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        if (off < ram_size) ram[off] = v;
    }
    static void wr16(std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr, std::uint16_t v) noexcept {
        if (!ram || ram_size == 0) return;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        if (off + 2 <= ram_size) std::memcpy(ram + off, &v, 2);
    }
    static void wr32(std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr, std::uint32_t v) noexcept {
        if (!ram || ram_size == 0) return;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        if (off + 4 <= ram_size) std::memcpy(ram + off, &v, 4);
    }

    // Semantic Game Detector (0=none/title, 1=SH1, 2=SH2, 3=SH3, 4=Koudelka PS1)
    static int detect_active_game(const std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (!ram || ram_size == 0) return 0;
        if (ram_size >= 0x00500000u) {
            if (rd8(ram, ram_size, 0x004CAE40u) == 0x43u && rd8(ram, ram_size, 0x004CAE41u) == 0x68u &&
                (rd8(ram, ram_size, 0x00100280u) & 0x40u) != 0u && rd32(ram, ram_size, 0x0010000Cu) >= 1000u) {
                return 1;
            }
            if (rd8(ram, ram_size, 0x00100000u) == 0x53u && rd8(ram, ram_size, 0x00100001u) == 0x53u &&
                rd8(ram, ram_size, 0x00100002u) == 0x48u) {
                const std::uint32_t raw = rd32(ram, ram_size, 0x00102300u);
                float pt = 0.0f;
                std::memcpy(&pt, &raw, 4);
                if (pt >= 1.0f && pt < 1e8f) return 2;
            }
            if (rd8(ram, ram_size, 0x0010001Du) == 0x53u && rd8(ram, ram_size, 0x0010001Eu) == 0x48u &&
                rd8(ram, ram_size, 0x0010001Fu) == 0x33u && rd32(ram, ram_size, 0x00100098u) >= 1000u) {
                return 3;
            }
        } else if (ram_size == 0x00200000u) {
            if (kdl_actor_ptr(ram, ram_size, 0) != 0u) return 4;
        }
        return 0;
    }

    static std::uint32_t kdl_actor_ptr(const std::uint8_t *ram, std::size_t ram_size, int slot) noexcept {
        if (slot < 0 || slot >= 3) return 0u;
        const auto valid_actor = [&](std::uint32_t p) noexcept -> bool {
            if (p < 0x80010000u || p >= 0x801FE000u) return false;
            if (slot == 0) return rd8(ram, ram_size, p + 0x146u) == 'K';
            if (slot == 1) return rd8(ram, ram_size, p + 0x146u) == 'E';
            return rd8(ram, ram_size, p + 0x146u) == 'J';
        };
        // Primary: GameManager (0x800AF470) + 0x0B0 -> party table [+4, +8, +C] (valid in Field, Menu & Battle)
        const std::uint32_t gm = rd32(ram, ram_size, 0x000AF470u);
        if (gm >= 0x80010000u && gm < 0x801FF000u) {
            const std::uint32_t ptbl = rd32(ram, ram_size, gm + 0x0B0u);
            if (ptbl >= 0x80010000u && ptbl < 0x801FF000u) {
                const std::uint32_t p = rd32(ram, ram_size, ptbl + 4u + static_cast<std::uint32_t>(slot) * 4u);
                if (valid_actor(p)) return p;
            }
        }
        // Fallback 1: 0x800B59A0 -> root - 0x44 - slot * 4
        const std::uint32_t root = rd32(ram, ram_size, 0x000B59A0u);
        if (root >= 0x80010000u && root < 0x801FF000u) {
            const std::uint32_t p = rd32(ram, ram_size, root - 0x44u - static_cast<std::uint32_t>(slot) * 4u);
            if (valid_actor(p)) return p;
        }
        // Fallback 2: 0x800B4100 -> root - 0x54 + slot * 4
        const std::uint32_t root2 = rd32(ram, ram_size, 0x000B4100u);
        if (root2 >= 0x80010100u && root2 < 0x801FF000u) {
            const std::uint32_t p = rd32(ram, ram_size, root2 - 0x54u + static_cast<std::uint32_t>(slot) * 4u);
            if (valid_actor(p)) return p;
        }
        return 0u;
    }

    static std::uint32_t get_cash(const std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        if (k == 1) return rd32(ram, ram_size, 0x00100008u);
        if (k == 2) return rd32(ram, ram_size, 0x00102D30u + static_cast<std::uint32_t>(rd8(ram, ram_size, 0x00100132u) & 3u) * 4u);
        if (k == 3) return rd32(ram, ram_size, 0x00100D54u);
        return 0u;
    }

    static std::uint32_t add_cash(std::uint8_t *ram, std::size_t ram_size, int k, std::int64_t delta) noexcept {
        if (k < 1 || k > 3) return 0u;
        const std::int64_t cur = static_cast<std::int64_t>(get_cash(ram, ram_size, k));
        const std::uint32_t nxt = static_cast<std::uint32_t>(std::clamp<std::int64_t>(cur + delta, 0, 9999999));
        if (k == 1) wr32(ram, ram_size, 0x00100008u, nxt);
        else if (k == 2) wr32(ram, ram_size, 0x00102D30u + static_cast<std::uint32_t>(rd8(ram, ram_size, 0x00100132u) & 3u) * 4u, nxt);
        else if (k == 3) wr32(ram, ram_size, 0x00100D54u, nxt);
        return nxt;
    }

    static std::uint32_t get_battles(const std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        if (k == 1) return rd32(ram, ram_size, 0x001014A4u);
        if (k == 2) return rd32(ram, ram_size, 0x001016E4u);
        if (k == 3) return rd32(ram, ram_size, 0x00102734u);
        return 0u;
    }

    static std::uint32_t get_ring(const std::uint8_t *ram, std::size_t ram_size, int k, int which) noexcept {
        if (k == 1) return rd32(ram, ram_size, which == 0 ? 0x001014ACu : (which == 1 ? 0x001014B0u : 0x001014B4u));
        if (k == 2) return rd32(ram, ram_size, which == 0 ? 0x001017C8u : (which == 1 ? 0x001017CCu : 0x001017D0u));
        if (k == 3) {
            const std::uint32_t off = (which == 0) ? 0x0Cu : (which == 1 ? 0x18u : 0x1Cu);
            std::uint32_t sum = 0;
            for (int i = 0; i < 7; ++i) sum += rd32(ram, ram_size, 0x00102778u + static_cast<std::uint32_t>(i) * 0x90u + off);
            return sum;
        }
        return 0u;
    }

    static int hero_count(int k) noexcept {
        return k == 1 ? 6 : (k == 2 ? 8 : (k == 3 ? 7 : (k == 4 ? 3 : 0)));
    }

    static bool hero_joined(const std::uint8_t *ram, std::size_t ram_size, int k, int s) noexcept {
        if (k == 1 && s >= 0 && s < 6) return (rd8(ram, ram_size, 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u) & 0x40u) != 0u;
        if (k == 2 && s >= 0 && s < 8) return rd8(ram, ram_size, 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x16u) != 0u;
        if (k == 3 && s >= 0 && s < 7) return rd8(ram, ram_size, 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x0Au) != 0u;
        if (k == 4 && s >= 0 && s < 3) {
            const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
            return p != 0u && rd8(ram, ram_size, p + 0x4BAu) != 0u;
        }
        return false;
    }

    static std::string hero_name(const std::uint8_t *ram, std::size_t ram_size, int k, int s) {
        static constexpr const char *kSh1[6] = {"Yuri", "Alice", "Zhuzhen", "Margarete", "Keith", "Halley"};
        static constexpr const char *kSh2[8] = {"Yuri", "Karin", "Blanca", "Gepetto", "Joachim", "Lucia", "Anastasia", "Kurando"};
        static constexpr const char *kSh3[7] = {"Johnny", "Shania", "Natan", "Frank", "Mao", "Ricardo", "Hilda"};
        static constexpr const char *kKdl[3] = {"Koudelka", "Edward", "James"};
        if (k == 1 && s >= 0 && s < 6) return kSh1[s];
        if (k == 2 && s >= 0 && s < 8) return kSh2[s];
        if (k == 3 && s >= 0 && s < 7) return kSh3[s];
        if (k == 4 && s >= 0 && s < 3) {
            const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
            if (p != 0u) {
                std::string nm;
                for (std::uint32_t i = 0; i < 16u; ++i) {
                    const std::uint8_t ch = rd8(ram, ram_size, p + 0x146u + i);
                    if (ch == 0u) break;
                    if (ch >= 0x20u && ch < 0x7Fu) nm.push_back(static_cast<char>(ch));
                }
                if (!nm.empty()) return nm;
            }
            return kKdl[s];
        }
        return {};
    }

    static int hero_level(const std::uint8_t *ram, std::size_t ram_size, int k, int s) noexcept {
        if (k == 1 && s >= 0 && s < 6) return rd8(ram, ram_size, 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u + 0x03u);
        if (k == 2 && s >= 0 && s < 8) return rd8(ram, ram_size, 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x0Fu);
        if (k == 3 && s >= 0 && s < 7) return rd8(ram, ram_size, 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x03u);
        if (k == 4 && s >= 0 && s < 3) {
            const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
            if (p != 0u) return rd16(ram, ram_size, p + 0x2D8u);
        }
        return 0;
    }

    static std::uint32_t hero_vital_addr(const std::uint8_t *ram, std::size_t ram_size, int k, int s, bool is_mp, bool is_max) noexcept {
        if (k == 1 && s >= 0 && s < 6) {
            const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u;
            return b + (is_mp ? (is_max ? 0x16u : 0x12u) : (is_max ? 0x14u : 0x10u));
        }
        if (k == 2 && s >= 0 && s < 8) {
            const std::uint32_t b = 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u;
            return b + (is_mp ? (is_max ? 0x2Cu : 0x2Eu) : (is_max ? 0x28u : 0x2Au));
        }
        if (k == 3 && s >= 0 && s < 7) {
            const std::uint32_t b = 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u;
            return b + (is_mp ? (is_max ? 0x22u : 0x20u) : (is_max ? 0x1Eu : 0x1Cu));
        }
        if (k == 4 && s >= 0 && s < 3) {
            const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
            if (p != 0u) return p + (is_mp ? (is_max ? 0x416u : 0x414u) : (is_max ? 0x412u : 0x410u));
        }
        return 0u;
    }

    static int hero_vital(const std::uint8_t *ram, std::size_t ram_size, int k, int s, bool is_mp, bool is_max) noexcept {
        const std::uint32_t a = hero_vital_addr(ram, ram_size, k, s, is_mp, is_max);
        return (a != 0u) ? static_cast<int>(rd16(ram, ram_size, a)) : 0;
    }

    static int set_hero_vital(std::uint8_t *ram, std::size_t ram_size, int k, int s, bool is_mp, int val) noexcept {
        const std::uint32_t a = hero_vital_addr(ram, ram_size, k, s, is_mp, false);
        if (a == 0u) return 0;
        const int v = std::clamp(val, 0, 9999);
        wr16(ram, ram_size, a, static_cast<std::uint16_t>(v));
        return v;
    }

    static std::uint32_t hero_exp_addr(const std::uint8_t *ram, std::size_t ram_size, int k, int s) noexcept {
        if (k == 1 && s >= 0 && s < 6) return 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u + 0x04u;
        if (k == 2 && s >= 0 && s < 8) return 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x10u;
        if (k == 3 && s >= 0 && s < 7) return 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x04u;
        if (k == 4 && s >= 0 && s < 3) {
            const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
            if (p != 0u) return p + 0x2DCu;
        }
        return 0u;
    }

    static std::uint32_t hero_exp(const std::uint8_t *ram, std::size_t ram_size, int k, int s) noexcept {
        const std::uint32_t a = hero_exp_addr(ram, ram_size, k, s);
        return (a != 0u) ? rd32(ram, ram_size, a) : 0u;
    }

    static std::uint32_t set_hero_exp(std::uint8_t *ram, std::size_t ram_size, int k, int s, std::uint32_t v) noexcept {
        const std::uint32_t a = hero_exp_addr(ram, ram_size, k, s);
        if (a == 0u) return 0u;
        wr32(ram, ram_size, a, v);
        return v;
    }

    static int get_max_level(const std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        int ml = 0;
        const int n = hero_count(k);
        for (int i = 0; i < n; ++i) {
            if (hero_joined(ram, ram_size, k, i)) ml = std::max(ml, hero_level(ram, ram_size, k, i));
        }
        return ml;
    }

    static int heal_party(std::uint8_t *ram, std::size_t ram_size, int k, int pct) noexcept {
        if (pct <= 0) return 0;
        pct = std::clamp(pct, 1, 100);
        int healed = 0;
        const int n = hero_count(k);
        for (int i = 0; i < n; ++i) {
            if (!hero_joined(ram, ram_size, k, i)) continue;
            const int hc = hero_vital(ram, ram_size, k, i, false, false);
            const int hm = hero_vital(ram, ram_size, k, i, false, true);
            if (hm >= 10 && hc > 0 && hc < hm) {
                const int add = std::max(1, hm * pct / 100);
                set_hero_vital(ram, ram_size, k, i, false, std::min(hm, hc + add));
                ++healed;
            }
        }
        return healed;
    }

    static int restore_party_mp(std::uint8_t *ram, std::size_t ram_size, int k, int pct) noexcept {
        if (pct <= 0) return 0;
        pct = std::clamp(pct, 1, 100);
        int restored = 0;
        const int n = hero_count(k);
        for (int i = 0; i < n; ++i) {
            if (!hero_joined(ram, ram_size, k, i)) continue;
            const int mc = hero_vital(ram, ram_size, k, i, true, false);
            const int mm = hero_vital(ram, ram_size, k, i, true, true);
            if (mm >= 1 && mc < mm) {
                const int add = std::max(1, mm * pct / 100);
                set_hero_vital(ram, ram_size, k, i, true, std::min(mm, mc + add));
                ++restored;
            }
        }
        return restored;
    }

    static int add_party_exp(std::uint8_t *ram, std::size_t ram_size, int k, std::uint32_t amount) noexcept {
        if (amount == 0u) return 0;
        int boosted = 0;
        const int n = hero_count(k);
        for (int i = 0; i < n; ++i) {
            if (!hero_joined(ram, ram_size, k, i)) continue;
            const std::uint32_t cur = hero_exp(ram, ram_size, k, i);
            set_hero_exp(ram, ram_size, k, i, cur + amount);
            ++boosted;
        }
        return boosted;
    }

    static int sync_bench_exp(std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        const int n = hero_count(k);
        if (n <= 0) return 0;
        std::uint32_t max_exp = 0u;
        int max_lv = 1;
        for (int i = 0; i < n; ++i) {
            if (!hero_joined(ram, ram_size, k, i)) continue;
            max_exp = std::max(max_exp, hero_exp(ram, ram_size, k, i));
            const int lv = hero_level(ram, ram_size, k, i);
            if (lv >= 1 && lv <= 99) max_lv = std::max(max_lv, lv);
        }
        if (max_exp == 0u) return 0;
        int synced = 0;
        for (int i = 0; i < n; ++i) {
            if (!hero_joined(ram, ram_size, k, i)) continue;
            if (hero_exp(ram, ram_size, k, i) < max_exp) {
                set_hero_exp(ram, ram_size, k, i, max_exp);
                ++synced;
            }
            if (k == 1) {
                const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                if (rd8(ram, ram_size, b + 0x03u) < static_cast<std::uint8_t>(max_lv)) {
                    wr8(ram, ram_size, b + 0x03u, static_cast<std::uint8_t>(max_lv));
                }
            }
        }
        return synced;
    }

    static int unlock_all_party(std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        if (k != 1) return 0;
        const std::uint8_t ylv = rd8(ram, ram_size, 0x00100280u + 0x03u);
        const std::uint32_t yexp = rd32(ram, ram_size, 0x00100280u + 0x04u);
        if (ylv < 1u || ylv > 99u) return 0;
        int unlocked = 0;
        for (int i = 1; i < 6; ++i) {
            const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
            const std::uint8_t fl = rd8(ram, ram_size, b);
            if ((fl & 0x40u) == 0u) {
                wr8(ram, ram_size, b, static_cast<std::uint8_t>(fl | 0x40u));
                if (rd8(ram, ram_size, b + 0x03u) < ylv) wr8(ram, ram_size, b + 0x03u, ylv);
                if (rd32(ram, ram_size, b + 0x04u) < yexp) wr32(ram, ram_size, b + 0x04u, yexp);
                ++unlocked;
            }
        }
        return unlocked;
    }

    // Koudelka equipped-weapon durability (verified: actor+0x498 -> wpn ptr, +0x7A cur u16, +0x7C max u16).
    static int hero_weapon_durability(const std::uint8_t *ram, std::size_t ram_size, int k, int s, bool is_max) noexcept {
        if (k != 4 || s < 0 || s >= 3) return -1;
        const std::uint32_t p = kdl_actor_ptr(ram, ram_size, s);
        if (p == 0u || rd8(ram, ram_size, p + 0x4BAu) == 0u) return -1;
        const std::uint32_t wpn = rd32(ram, ram_size, p + 0x498u);
        if (wpn < 0x80010000u || wpn >= 0x801FE000u) return -1;
        const int cur = static_cast<int>(rd16(ram, ram_size, wpn + 0x7Au));
        const int maxd = static_cast<int>(rd16(ram, ram_size, wpn + 0x7Cu));
        if (maxd < 1 || maxd > 999 || cur > maxd) return -1;
        return is_max ? maxd : cur;
    }

    struct SelectedWeaponInfo {
        bool valid = false;
        int cur = -1;
        int maxd = -1;
        std::string name;
    };

    // Decode Koudelka's 2-byte-per-char nibble-encoded menu label (skipping 2-byte icon prefix at +0..+1).
    static std::string kdl_decode_menu_label(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t va) noexcept {
        if (va < 0x80010000u || va >= 0x801FFFC0u) return {};
        std::string out;
        for (std::uint32_t i = 2u; i + 1u < 64u; i += 2u) {
            const std::uint8_t c0 = rd8(ram, ram_size, va + i);
            const std::uint8_t c1 = rd8(ram, ram_size, va + i + 1u);
            if (c0 == 0u || c1 == 0u) break;
            if (c0 < 0x40u || c0 > 0x4Fu || c1 < 0x40u || c1 > 0x4Fu) break;
            const char ch = static_cast<char>(((c1 - 0x40u) << 4) | ((c0 - 0x40u) & 0x0Fu));
            if (ch == '\0') break;
            out.push_back(ch == '_' ? ' ' : ch);
        }
        return out;
    }

    static std::string kdl_read_weapon_name(const std::uint8_t *ram, std::size_t ram_size, std::uint32_t wpn_va) noexcept {
        if (wpn_va < 0x80010000u || wpn_va >= 0x801FFE00u) return {};
        std::string out;
        for (std::uint32_t i = 0u; i < 32u; ++i) {
            const std::uint8_t ch = rd8(ram, ram_size, wpn_va + 0x0Au + i);
            if (ch == 0u) break;
            out.push_back(ch == '_' ? ' ' : static_cast<char>(ch));
        }
        return out;
    }

    // Koudelka menu-selected weapon durability (supports Field Equip->Weapon, Field Equip->Slots,
    // Field Items->Weapon, and Battle->Action->Weapon).
    static SelectedWeaponInfo selected_weapon_info(const std::uint8_t *ram, std::size_t ram_size, int k) noexcept {
        SelectedWeaponInfo out{};
        if (k != 4 || !ram || ram_size != 0x00200000u) return out;
        const std::uint32_t inv = rd32(ram, ram_size, 0x000B4DD0u);
        if (inv < 0x80010000u || inv >= 0x801FD000u) return out;
        const std::uint32_t wpn_count = rd32(ram, ram_size, inv + 0x12Cu);
        if (wpn_count == 0u || wpn_count > 64u) return out;

        const std::uint32_t ovl = rd32(ram, ram_size, 0x000B6DF0u);
        std::uint32_t ctrl = 0u;
        bool is_slot_overview = false;
        if (ovl == 4u) {
            const std::uint32_t eqw_wgt = rd32(ram, ram_size, 0x8017E2BCu + 0x3Cu);
            const std::uint32_t eqs_wgt = rd32(ram, ram_size, 0x8017DC88u + 0x3Cu);
            const std::uint32_t itw_wgt = rd32(ram, ram_size, 0x8017A9A0u + 0x3Cu);
            if (eqw_wgt >= 0x80010000u && eqw_wgt < 0x801FF000u &&
                rd32(ram, ram_size, 0x8017E2BCu + 0x30u) == 0x31u &&
                rd32(ram, ram_size, 0x8017DC88u + 0x20u) == 0u) {
                ctrl = 0x8017E2BCu;  // Field -> Equip -> Weapon Picker
            } else if (itw_wgt >= 0x80010000u && itw_wgt < 0x801FF000u &&
                       rd32(ram, ram_size, 0x8017A9A0u + 0x30u) == 0x31u &&
                       rd8(ram, ram_size, 0x8017A8E8u) == 0u) {
                ctrl = 0x8017A9A0u;  // Field -> Items -> Weapon List
            } else if (eqs_wgt >= 0x80010000u && eqs_wgt < 0x801FF000u &&
                       eqw_wgt == 0u &&
                       rd32(ram, ram_size, 0x8017DC88u + 0x30u) == 0x10u &&
                       rd32(ram, ram_size, 0x8017DC88u + 0x20u) == 0u) {
                ctrl = 0x8017DC88u;  // Field -> Equip -> Slots Overview (Weapon slot highlighted)
                is_slot_overview = true;
            }
        } else if (ovl == 5u) {
            const std::uint32_t btl_wgt = rd32(ram, ram_size, 0x8016010Cu + 0x3Cu);
            if (btl_wgt >= 0x80010000u && btl_wgt < 0x801FF000u &&
                rd32(ram, ram_size, 0x8016010Cu + 0x30u) == 0x31u) {
                ctrl = 0x8016010Cu;  // Battle -> Action -> Weapon
            }
        }
        if (ctrl == 0u) return out;

        const std::uint32_t idx = rd32(ram, ram_size, ctrl + 0x20u);
        const std::uint32_t list_ptr = rd32(ram, ram_size, ctrl + 0x28u);
        if (idx >= 64u || list_ptr < 0x80010000u || list_ptr >= 0x801FF000u) return out;

        std::string label;
        const std::uint32_t entry = rd32(ram, ram_size, list_ptr + idx * 4u);
        if (entry >= 0x80010000u && entry < 0x801FF000u) {
            const std::uint32_t str_ptr = rd32(ram, ram_size, entry + 0x10u);
            label = kdl_decode_menu_label(ram, ram_size, str_ptr);
        }
        if (label == "Bare-Handed" || label == "None") return out;

        const auto read_slot = [&](std::uint32_t slot_idx) noexcept -> SelectedWeaponInfo {
            SelectedWeaponInfo w{};
            const std::uint32_t wpn = inv + 0x238u + slot_idx * 0x94u;
            if (wpn < 0x80010000u || wpn + 0x94u >= 0x801FFFF0u) return w;
            const int cur = static_cast<int>(rd16(ram, ram_size, wpn + 0x7Au));
            const int maxd = static_cast<int>(rd16(ram, ram_size, wpn + 0x7Cu));
            if (maxd < 1 || maxd > 999 || cur > maxd) return w;
            w.name = kdl_read_weapon_name(ram, ram_size, wpn);
            if (w.name.empty()) return w;
            w.valid = true;
            w.cur = cur;
            w.maxd = maxd;
            return w;
        };

        if (!is_slot_overview && idx < wpn_count) {
            SelectedWeaponInfo direct = read_slot(idx);
            if (direct.valid && (label.empty() || direct.name == label)) return direct;
        }
        if (!label.empty()) {
            for (std::uint32_t i = 0u; i < wpn_count; ++i) {
                SelectedWeaponInfo cand = read_slot(i);
                if (cand.valid && cand.name == label) return cand;
            }
        }
        return out;
    }
};

}  // namespace detail

#ifdef NOCTURNE_MOD_ENGINE_WITH_LUA54
}  // namespace mod_engine
#include "mod_lua54.hpp"
namespace mod_engine {
namespace detail {
using LuaScript = Lua54Script;
}  // namespace detail
#else
namespace detail {
// Metadata-only stub for targets that do not execute Lua scripts (e.g. sh2launcher).
struct LuaScript {
    std::string mod_name;
    std::string mod_folder;
    std::filesystem::path mod_dir;
    std::filesystem::path state_path;
    std::string file_name;
    bool coop_safe = false;
    std::unordered_map<std::string, double> settings_store;
    std::unordered_map<std::string, std::string> string_settings_store;

    bool load_source(const std::string &, const std::string &, const std::string &,
                     const std::filesystem::path & = {},
                     const std::unordered_map<std::string, double> & = {},
                     const std::unordered_map<std::string, std::string> & = {},
                     const std::filesystem::path & = {}, bool = false) { return false; }
    bool has_hook(const char *) const { return false; }
    std::size_t hook_count() const noexcept { return 0; }
    void call_hook(const char *, ScriptHostCtx &, const std::vector<Val> & = {}) {}
};
}  // namespace detail
#endif


class Engine {
public:
    Engine() = default;
    ~Engine() { unload_native_plugins(); }
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    void clear() {
        unload_native_plugins();
        file_overrides_.clear();
        file_override_owners_.clear();
        texture_overrides_.clear();
        texture_override_owners_.clear();
        texture_override_dirs_.clear();
        video_overrides_.clear();
        video_override_owners_.clear();
        video_override_dirs_.clear();
        model_overrides_.clear();
        model_override_owners_.clear();
        patches_.clear();
        patch_owners_.clear();
        conflicts_.clear();
        scripts_.clear();
        active_mod_names_.clear();
        toast_queue_.clear();
        hud_widgets_.clear();
        canvas_cmds_.clear();
        pad_inject_ = 0;
        pad_block_ = 0;
        ev_initialized_ = false;
        ev_in_battle_ = false;
        ev_battle_char_ = 0xFFu;
        ev_area_id_ = 0xFFFFFFFFu;
        ev_lvlup_active_ = false;
        ev_lvlup_char_ = 0xFFu;
        watch_files_.clear();
        has_on_frame_hook_ = false;
        has_on_hotkey_hook_ = false;
        has_semantic_hooks_ = false;
        frame_counter_ = 0;
        ev_initialized_ = false;
        ev_game_kind_ = 0;
        ev_prev_battles_ = 0;
        ev_prev_att_ = ev_prev_suc_ = ev_prev_prf_ = 0;
        ev_hit_streak_ = ev_perf_streak_ = 0;
        item_glow_strength_ = 0.0f;
        item_glow_rgb_ = 0xFFCC44u;
        item_glow_pulse_speed_ = 1.0f;
        coop_gameplay_hash_ = 0u;
        coop_gameplay_summary_.clear();
        coop_gameplay_paused_ = false;
        mod_config_paths_.clear();
        mod_dirs_.clear();
    }

    void set_game_slug(std::string slug) { game_slug_ = std::move(slug); }
    void set_dev_mode(bool on) noexcept { dev_mode_ = on; }
    bool dev_mode() const noexcept { return dev_mode_; }
    void set_allow_native_plugins(bool allow) noexcept { allow_native_plugins_ = allow; }
    bool allow_native_plugins() const noexcept { return allow_native_plugins_; }
    void set_coop_gameplay_paused(bool paused) noexcept { coop_gameplay_paused_ = paused; }
    bool coop_gameplay_paused() const noexcept { return coop_gameplay_paused_; }
    std::uint64_t coop_gameplay_hash() const noexcept { return coop_gameplay_hash_; }
    const std::string &coop_gameplay_summary() const noexcept { return coop_gameplay_summary_; }
    bool wants_input() const noexcept { return has_on_hotkey_hook_ || has_on_frame_hook_; }

    static bool mod_matches_game(const std::string &target_game, const std::string &game_slug) {
        const std::string tg = to_lower_str(trim_str(target_game));
        const std::string gs = to_lower_str(trim_str(game_slug));
        if (tg.empty() || tg == "all" || tg == "shared" || tg == "quadrilogy" || tg == "*" || gs.empty()) return true;
        auto norm_slug = [](const std::string &s) -> std::string {
            if (s == "sh2" || s == "covenant") return "sh2-covenant";
            if (s == "sh3" || s == "ftnw") return "sh3-ftnw";
            return s;
        };
        const std::string ngs = norm_slug(gs);
        std::size_t pos = 0;
        while (pos <= tg.size()) {
            const std::size_t c = tg.find(',', pos);
            const std::string tok = trim_str((c == std::string::npos) ? tg.substr(pos) : tg.substr(pos, c - pos));
            if (tok == "all" || tok == "shared" || tok == "quadrilogy" || tok == "*") return true;
            if (tok == "all_ps2" || tok == "ps2" || tok == "trilogy") {
                if (ngs == "sh1" || ngs == "sh2-covenant" || ngs == "sh3-ftnw") return true;
            }
            if (norm_slug(tok) == ngs) return true;
            if (c == std::string::npos) break;
            pos = c + 1;
        }
        return false;
    }

    void set_input_state(std::uint32_t keys_down, std::uint32_t pad_down) noexcept {
        keys_pressed_ = keys_down & ~keys_prev_;
        pad_pressed_ = pad_down & ~pad_prev_;
        keys_down_ = keys_down;
        pad_down_ = pad_down;
        keys_prev_ = keys_down;
        pad_prev_ = pad_down;
    }

    static std::string json_field(const std::string &text, const std::string &key) {
        const std::string pat = "\"" + key + "\"";
        const std::size_t p = text.find(pat);
        if (p == std::string::npos) return {};
        const std::size_t colon = text.find(':', p + pat.size());
        if (colon == std::string::npos) return {};
        std::size_t i = colon + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size() || text[i] != '"') return {};
        const std::size_t q1 = i;
        std::size_t q2 = q1 + 1;
        while (q2 < text.size()) {
            if (text[q2] == '\\' && q2 + 1 < text.size()) { q2 += 2; continue; }
            if (text[q2] == '"') break;
            ++q2;
        }
        if (q2 >= text.size()) return {};
        return text.substr(q1 + 1, q2 - q1 - 1);
    }

    static double json_num_field(const std::string &text, const std::string &key, double def_val = 0.0) {
        const std::string pat = "\"" + key + "\"";
        const std::size_t p = text.find(pat);
        if (p == std::string::npos) return def_val;
        const std::size_t colon = text.find(':', p + pat.size());
        if (colon == std::string::npos) return def_val;
        std::size_t i = colon + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) return def_val;
        if (text.compare(i, 4, "true") == 0) return 1.0;
        if (text.compare(i, 5, "false") == 0) return 0.0;
        char *endp = nullptr;
        const double v = std::strtod(text.c_str() + i, &endp);
        return (endp && endp != text.c_str() + i) ? v : def_val;
    }

    static std::vector<std::string> json_string_array(const std::string &obj, const char *key) {
        std::vector<std::string> out;
        const std::string needle = std::string("\"") + key + "\"";
        const std::size_t kp = obj.find(needle);
        if (kp == std::string::npos) return out;
        const std::size_t lb = obj.find('[', kp + needle.size());
        if (lb == std::string::npos) return out;
        std::size_t i = lb + 1;
        while (i < obj.size()) {
            while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t' || obj[i] == '\r' || obj[i] == '\n' || obj[i] == ',')) ++i;
            if (i >= obj.size() || obj[i] == ']') break;
            if (obj[i] == '"') {
                ++i;
                std::string s;
                while (i < obj.size() && obj[i] != '"') {
                    if (obj[i] == '\\' && i + 1 < obj.size()) { s.push_back(obj[i + 1]); i += 2; }
                    else { s.push_back(obj[i++]); }
                }
                if (i < obj.size() && obj[i] == '"') ++i;
                out.push_back(std::move(s));
            } else {
                ++i;
            }
        }
        return out;
    }

    static std::vector<ModSettingDef> parse_mod_settings(const std::string &manifest_txt,
                                                         const std::filesystem::path &mdir,
                                                         const std::filesystem::path &config_path_override = {}) {
        std::vector<ModSettingDef> out;
        const std::size_t sp = manifest_txt.find("\"settings\"");
        if (sp == std::string::npos) return out;
        const std::size_t lb = manifest_txt.find('[', sp);
        if (lb == std::string::npos) return out;
        int depth = 0;
        std::size_t obj_start = std::string::npos;
        for (std::size_t i = lb + 1; i < manifest_txt.size(); ++i) {
            const char c = manifest_txt[i];
            if (c == ']' && depth == 0) break;
            if (c == '{') {
                if (depth == 0) obj_start = i;
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0 && obj_start != std::string::npos) {
                    const std::string obj = manifest_txt.substr(obj_start, i - obj_start + 1);
                    ModSettingDef sd{};
                    sd.key = json_field(obj, "key");
                    sd.label = json_field(obj, "label");
                    if (sd.label.empty()) sd.label = sd.key;
                    sd.description = json_field(obj, "description");
                    sd.type = to_lower_str(json_field(obj, "type"));
                    if (sd.type.empty()) sd.type = "int";
                    if (sd.type == "enum" || sd.type == "select") sd.type = "choice";
                    if (sd.type == "key") sd.type = "hotkey";

                    if (sd.type == "choice") {
                        sd.options = json_string_array(obj, "options");
                        sd.min_val = 0.0;
                        sd.max_val = sd.options.empty() ? 0.0 : static_cast<double>(sd.options.size() - 1);
                        sd.step = 1.0;
                        const std::string def_s = json_field(obj, "default");
                        int def_idx = static_cast<int>(std::lround(json_num_field(obj, "default", 0.0)));
                        if (!def_s.empty()) {
                            for (int k = 0; k < static_cast<int>(sd.options.size()); ++k) {
                                if (to_lower_str(sd.options[static_cast<std::size_t>(k)]) == to_lower_str(def_s)) {
                                    def_idx = k;
                                    break;
                                }
                            }
                        }
                        if (!sd.options.empty()) {
                            def_idx = std::clamp(def_idx, 0, static_cast<int>(sd.options.size()) - 1);
                            sd.default_val = static_cast<double>(def_idx);
                            sd.default_str = sd.options[static_cast<std::size_t>(def_idx)];
                        } else {
                            sd.default_val = 0.0;
                            sd.default_str.clear();
                        }
                    } else if (sd.type == "hotkey") {
                        sd.options = json_string_array(obj, "options");
                        if (sd.options.empty()) sd.options = mod_hotkey_choices();
                        sd.min_val = 0.0;
                        sd.max_val = static_cast<double>(sd.options.size() - 1);
                        sd.step = 1.0;
                        std::string def_k = json_field(obj, "default");
                        if (def_k.empty()) def_k = sd.options.front();
                        int def_idx = 0;
                        bool found = false;
                        for (int k = 0; k < static_cast<int>(sd.options.size()); ++k) {
                            if (to_lower_str(sd.options[static_cast<std::size_t>(k)]) == to_lower_str(def_k)) {
                                def_idx = k;
                                def_k = sd.options[static_cast<std::size_t>(k)];
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            sd.options.insert(sd.options.begin(), def_k);
                            sd.max_val = static_cast<double>(sd.options.size() - 1);
                            def_idx = 0;
                        }
                        sd.default_val = static_cast<double>(def_idx);
                        sd.default_str = def_k;
                    } else {
                        sd.default_val = json_num_field(obj, "default", 0.0);
                        sd.min_val = json_num_field(obj, "min", 0.0);
                        sd.max_val = json_num_field(obj, "max", sd.type == "bool" ? 1.0 : 100.0);
                        sd.step = json_num_field(obj, "step", 1.0);
                        if (sd.step <= 0.0) sd.step = 1.0;
                        sd.default_val = std::clamp(sd.default_val, sd.min_val, sd.max_val);
                        if (sd.type == "bool") sd.default_str = (sd.default_val >= 0.5) ? "1" : "0";
                    }
                    sd.current_val = sd.default_val;
                    sd.current_str = sd.default_str;
                    if (!sd.key.empty()) out.push_back(std::move(sd));
                    obj_start = std::string::npos;
                }
            }
        }
        // Apply user overrides from per-game config override (for shared mods) or mod_config.ini
        std::filesystem::path cfg_file = (!config_path_override.empty() && std::filesystem::is_regular_file(config_path_override))
            ? config_path_override
            : (mdir / "mod_config.ini");
        std::ifstream ifs(cfg_file);
        if (ifs) {
            std::string line;
            while (std::getline(ifs, line)) {
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = trim_str(line.substr(0, eq));
                const std::string v = trim_str(line.substr(eq + 1));
                for (auto &sd : out) {
                    if (sd.key != k) continue;
                    if (sd.type == "hotkey") {
                        for (int idx = 0; idx < static_cast<int>(sd.options.size()); ++idx) {
                            if (to_lower_str(sd.options[static_cast<std::size_t>(idx)]) == to_lower_str(v)) {
                                sd.current_val = static_cast<double>(idx);
                                sd.current_str = sd.options[static_cast<std::size_t>(idx)];
                                break;
                            }
                        }
                    } else if (sd.type == "choice") {
                        bool matched_label = false;
                        for (int idx = 0; idx < static_cast<int>(sd.options.size()); ++idx) {
                            if (to_lower_str(sd.options[static_cast<std::size_t>(idx)]) == to_lower_str(v)) {
                                sd.current_val = static_cast<double>(idx);
                                sd.current_str = sd.options[static_cast<std::size_t>(idx)];
                                matched_label = true;
                                break;
                            }
                        }
                        if (!matched_label && !sd.options.empty()) {
                            const int idx = std::clamp(static_cast<int>(std::lround(std::strtod(v.c_str(), nullptr))),
                                                       0, static_cast<int>(sd.options.size()) - 1);
                            sd.current_val = static_cast<double>(idx);
                            sd.current_str = sd.options[static_cast<std::size_t>(idx)];
                        }
                    } else {
                        sd.current_val = std::clamp(std::strtod(v.c_str(), nullptr), sd.min_val, sd.max_val);
                        if (sd.type == "bool") sd.current_str = (sd.current_val >= 0.5) ? "1" : "0";
                    }
                }
            }
        }
        return out;
    }

    static bool save_mod_settings(const std::filesystem::path &mdir,
                                  const std::vector<ModSettingDef> &settings,
                                  const std::filesystem::path &config_path_override = {}) {
        const std::filesystem::path target = config_path_override.empty() ? (mdir / "mod_config.ini") : config_path_override;
        std::error_code ec;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream ofs(target, std::ios::trunc);
        if (!ofs) return false;
        for (const auto &sd : settings) {
            if (sd.type == "hotkey") {
                ofs << sd.key << " = " << (sd.current_str.empty() ? "*" : sd.current_str) << "\n";
            } else {
                ofs << sd.key << " = " << sd.current_val << "\n";
            }
        }
        return true;
    }

    static std::vector<InstalledMod> scan_installed_mods(const std::filesystem::path &game_dir,
                                                         const std::string &active_mods_spec,
                                                         const std::string &game_slug_hint = {}) {
        std::vector<InstalledMod> out;
        std::error_code ec;
        const std::filesystem::path mods_root = game_dir / "mods";
        const std::filesystem::path shared_root = game_dir.parent_path() / "shared" / "mods";
        const std::string effective_slug = game_slug_hint.empty() ? game_dir.filename().string() : game_slug_hint;
        const bool has_game_mods = std::filesystem::is_directory(mods_root, ec);
        const bool has_shared_mods = std::filesystem::is_directory(shared_root, ec);
        if (!has_game_mods && !has_shared_mods) return out;

        std::vector<std::string> spec_order;
        std::unordered_map<std::string, bool> spec_enabled;
        {
            std::size_t pos = 0;
            while (pos < active_mods_spec.size()) {
                const std::size_t c = active_mods_spec.find(',', pos);
                std::string tok = trim_str((c == std::string::npos) ? active_mods_spec.substr(pos)
                                                                    : active_mods_spec.substr(pos, c - pos));
                if (!tok.empty()) {
                    bool en = true;
                    if (tok[0] == '!') { en = false; tok = trim_str(tok.substr(1)); }
                    if (!tok.empty()) {
                        spec_order.push_back(tok);
                        spec_enabled[to_lower_str(tok)] = en;
                    }
                }
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        }

        std::unordered_map<std::string, InstalledMod> by_key;
        std::vector<std::string> discovered_keys;
        auto scan_dir = [&](const std::filesystem::path &root_dir, bool is_shared_root) {
            if (!std::filesystem::is_directory(root_dir, ec)) return;
            for (const auto &entry : std::filesystem::directory_iterator(root_dir, ec)) {
                if (!entry.is_directory(ec)) continue;
                const std::filesystem::path mdir = entry.path();
                const std::string folder = mdir.filename().string();
                const std::string key = to_lower_str(folder);
                if (by_key.find(key) != by_key.end()) continue;  // game-specific copy overrides shared copy

                InstalledMod m{};
                m.folder = folder;
                m.name = m.folder;
                m.version = "1.0";
                m.author = "Community";
                m.is_shared = is_shared_root;
                m.mod_dir = mdir;
                if (is_shared_root) {
                    m.config_path = game_dir / "mods_config" / (m.folder + ".ini");
                    m.state_path = game_dir / "mods_config" / (m.folder + ".state.ini");
                } else {
                    m.config_path = mdir / "mod_config.ini";
                    m.state_path = mdir / "mod_state.ini";
                }
                const auto sit = spec_enabled.find(key);
                bool default_enabled = true;
                bool declared_coop_safe = false;
                const std::filesystem::path manifest = mdir / "mod.json";
                if (std::filesystem::is_regular_file(manifest, ec)) {
                    std::ifstream ifs(manifest);
                    if (ifs) {
                        std::ostringstream oss; oss << ifs.rdbuf();
                        const std::string txt = oss.str();
                        if (const std::string v = json_field(txt, "name"); !v.empty()) m.name = v;
                        if (const std::string v = json_field(txt, "version"); !v.empty()) m.version = v;
                        if (const std::string v = json_field(txt, "author"); !v.empty()) m.author = v;
                        if (const std::string v = json_field(txt, "description"); !v.empty()) m.description = v;
                        if (const std::string v = json_field(txt, "category"); !v.empty()) m.category = v;
                        if (const std::string v = json_field(txt, "game"); !v.empty()) m.target_game = v;
                        declared_coop_safe = (json_num_field(txt, "coop_safe", 0.0) >= 0.5);
                        default_enabled = (json_num_field(txt, "default_enabled", 1.0) >= 0.5);
                        if (is_shared_root && !mod_matches_game(m.target_game, effective_slug)) {
                            continue;  // Filter shared mods by compatible game
                        }
                        m.settings = parse_mod_settings(txt, mdir, m.config_path);
                    }
                }
                m.enabled = (sit == spec_enabled.end()) ? default_enabled : sit->second;
                const std::filesystem::path files_dir = mdir / "files";
                if (std::filesystem::is_directory(files_dir, ec)) {
                    for (const auto &fe : std::filesystem::recursive_directory_iterator(files_dir, ec)) {
                        if (fe.is_regular_file(ec)) ++m.file_count;
                    }
                }
                const std::filesystem::path tex_dir = mdir / "textures";
                if (std::filesystem::is_directory(tex_dir, ec)) {
                    for (const auto &te : std::filesystem::recursive_directory_iterator(tex_dir, ec)) {
                        if (te.is_regular_file(ec)) ++m.texture_count;
                    }
                }
                const std::filesystem::path vid_dir = mdir / "video";
                if (std::filesystem::is_directory(vid_dir, ec)) {
                    for (const auto &ve : std::filesystem::directory_iterator(vid_dir, ec)) {
                        if (ve.is_regular_file(ec)) ++m.video_count;
                    }
                }
                const std::filesystem::path mdl_dir = mdir / "models";
                if (std::filesystem::is_directory(mdl_dir, ec)) {
                    for (const auto &me : std::filesystem::directory_iterator(mdl_dir, ec)) {
                        const std::string ext = to_lower_str(me.path().extension().string());
                        if (me.is_regular_file(ec) && (ext == ".glb" || ext == ".gltf")) ++m.model_count;
                    }
                }
                const std::filesystem::path patch_dir = mdir / "patches";
                if (std::filesystem::is_directory(patch_dir, ec)) {
                    for (const auto &pe : std::filesystem::directory_iterator(patch_dir, ec)) {
                        if (pe.is_regular_file(ec) && to_lower_str(pe.path().extension().string()) == ".pnach") ++m.patch_count;
                    }
                }
                const std::filesystem::path script_dir = mdir / "scripts";
                if (std::filesystem::is_directory(script_dir, ec)) {
                    for (const auto &se : std::filesystem::directory_iterator(script_dir, ec)) {
                        if (se.is_regular_file(ec) && to_lower_str(se.path().extension().string()) == ".lua") ++m.script_count;
                    }
                }
                const std::filesystem::path plug_dir = mdir / "plugins";
                if (std::filesystem::is_directory(plug_dir, ec)) {
                    for (const auto &de : std::filesystem::directory_iterator(plug_dir, ec)) {
                        if (de.is_regular_file(ec) && to_lower_str(de.path().extension().string()) == ".dll") ++m.plugin_count;
                    }
                }
                // Only pure client-side visual/HUD/script mods with zero disc/patch/DLL overrides can be coop_safe
                m.coop_safe = declared_coop_safe && m.file_count == 0 && m.patch_count == 0 && m.plugin_count == 0;
                discovered_keys.push_back(key);
                by_key[key] = std::move(m);
            }
        };
        scan_dir(mods_root, false);
        scan_dir(shared_root, true);
        std::sort(discovered_keys.begin(), discovered_keys.end());

        std::unordered_map<std::string, bool> emitted;
        for (const auto &tok : spec_order) {
            const std::string k = to_lower_str(tok);
            const auto it = by_key.find(k);
            if (it != by_key.end() && !emitted[k]) {
                out.push_back(it->second);
                emitted[k] = true;
            }
        }
        for (const auto &k : discovered_keys) {
            if (!emitted[k]) {
                out.push_back(by_key[k]);
                emitted[k] = true;
            }
        }
        return out;
    }

    static std::string serialize_active_mods(const std::vector<InstalledMod> &mods) {
        std::string out;
        for (std::size_t i = 0; i < mods.size(); ++i) {
            if (i > 0) out.push_back(',');
            if (!mods[i].enabled) out.push_back('!');
            out += mods[i].folder;
        }
        return out;
    }

    // Load all enabled mods from games/<slug>/mods/ according to active_mods spec ("ModA,ModB,!DisabledMod")
    void load_from_game_dir(const std::filesystem::path &game_dir, const std::string &active_mods_spec, bool enabled = true) {
        clear();
        loaded_game_dir_ = game_dir;
        loaded_mods_spec_ = active_mods_spec;
        loaded_enabled_ = enabled;
        if (!enabled) return;
        if (game_slug_.empty()) {
            game_slug_ = game_dir.filename().string();
        }

        std::error_code ec;
        const std::filesystem::path mods_root = game_dir / "mods";
        const std::filesystem::path shared_root = game_dir.parent_path() / "shared" / "mods";
        if (!std::filesystem::is_directory(mods_root, ec) && !std::filesystem::is_directory(shared_root, ec)) return;

        auto track_watch = [&](const std::filesystem::path &p) {
            std::error_code wec;
            if (std::filesystem::is_regular_file(p, wec)) {
                watch_files_.push_back({p, std::filesystem::last_write_time(p, wec)});
            }
        };

        auto fnv_mix = [](std::uint64_t h, const void *data, std::size_t len) noexcept -> std::uint64_t {
            const auto *p = static_cast<const std::uint8_t *>(data);
            for (std::size_t i = 0; i < len; ++i) {
                h ^= static_cast<std::uint64_t>(p[i]);
                h *= 1099511628211ull;
            }
            return h;
        };
        auto fnv_str = [&](std::uint64_t h, const std::string &s) noexcept -> std::uint64_t {
            h = fnv_mix(h, s.data(), s.size());
            const std::uint8_t sep = 0x1Fu;
            return fnv_mix(h, &sep, 1);
        };

        const auto installed = scan_installed_mods(game_dir, active_mods_spec, game_slug_);
        std::uint64_t gp_hash = 14695981039346656037ull;
        bool any_gp_mod = false;
        std::string gp_summary;

        for (const auto &m : installed) {
            if (!m.enabled) continue;
            const std::filesystem::path mdir = m.mod_dir.empty() ? (mods_root / m.folder) : m.mod_dir;
            active_mod_names_.push_back(m.folder);
            mod_dirs_[m.folder] = mdir;
            mod_config_paths_[m.folder] = m.config_path.empty() ? (mdir / "mod_config.ini") : m.config_path;
            track_watch(mdir / "mod.json");
            track_watch(mod_config_paths_[m.folder]);

            if (!m.coop_safe) {
                any_gp_mod = true;
                if (!gp_summary.empty()) gp_summary += ", ";
                gp_summary += m.folder + "@" + m.version;
                gp_hash = fnv_str(gp_hash, m.folder);
                gp_hash = fnv_str(gp_hash, m.version);
                for (const auto &sd : m.settings) {
                    gp_hash = fnv_str(gp_hash, sd.key);
                    gp_hash = fnv_str(gp_hash, format_mod_setting_value(sd));
                }
            }

            std::unordered_map<std::string, double> cfg_map;
            std::unordered_map<std::string, std::string> str_cfg_map;
            for (const auto &sd : m.settings) {
                cfg_map[sd.key] = sd.current_val;
                if (!sd.current_str.empty()) str_cfg_map[sd.key] = sd.current_str;
            }

            // Layer 1a: Virtual Disc File Overrides (top priority mod wins)
            const std::filesystem::path files_dir = mdir / "files";
            if (std::filesystem::is_directory(files_dir, ec)) {
                for (const auto &fe : std::filesystem::recursive_directory_iterator(files_dir, ec)) {
                    if (!fe.is_regular_file(ec)) continue;
                    std::string rel = std::filesystem::relative(fe.path(), files_dir, ec).generic_string();
                    std::string key = to_lower_str(rel);
                    const auto oit = file_override_owners_.find(key);
                    if (oit == file_override_owners_.end()) {
                        file_overrides_.emplace(key, fe.path());
                        file_override_owners_.emplace(std::move(key), m.folder);
                    } else {
                        conflicts_.push_back(ModConflict{"files", rel, oit->second, m.folder});
                    }
                }
            }

            // Layer 1b: Bundled HD Texture Overrides (mods/<mod>/textures/)
            const std::filesystem::path tex_dir = mdir / "textures";
            if (std::filesystem::is_directory(tex_dir, ec)) {
                texture_override_dirs_.push_back(tex_dir);
                for (const auto &te : std::filesystem::recursive_directory_iterator(tex_dir, ec)) {
                    if (!te.is_regular_file(ec)) continue;
                    std::string fname = te.path().filename().string();
                    std::string key = to_lower_str(fname);
                    const auto oit = texture_override_owners_.find(key);
                    if (oit == texture_override_owners_.end()) {
                        texture_overrides_.emplace(key, te.path());
                        texture_override_owners_.emplace(std::move(key), m.folder);
                    } else {
                        conflicts_.push_back(ModConflict{"textures", fname, oit->second, m.folder});
                    }
                }
            }

            // Layer 1c: Bundled HD Video / Audio Overrides (mods/<mod>/video/)
            const std::filesystem::path vid_dir = mdir / "video";
            if (std::filesystem::is_directory(vid_dir, ec)) {
                video_override_dirs_.push_back(vid_dir);
                for (const auto &ve : std::filesystem::directory_iterator(vid_dir, ec)) {
                    if (!ve.is_regular_file(ec)) continue;
                    std::string fname = ve.path().filename().string();
                    std::string key = to_lower_str(fname);
                    const auto oit = video_override_owners_.find(key);
                    if (oit == video_override_owners_.end()) {
                        video_overrides_.emplace(key, ve.path());
                        video_override_owners_.emplace(std::move(key), m.folder);
                    } else {
                        conflicts_.push_back(ModConflict{"video", fname, oit->second, m.folder});
                    }
                }
            }

            // Layer 1d: High-Poly 3D Model Overrides (mods/<mod>/models/*.glb)
            const std::filesystem::path mdl_dir = mdir / "models";
            if (std::filesystem::is_directory(mdl_dir, ec)) {
                for (const auto &me : std::filesystem::directory_iterator(mdl_dir, ec)) {
                    if (!me.is_regular_file(ec)) continue;
                    const std::string ext = to_lower_str(me.path().extension().string());
                    if (ext != ".glb" && ext != ".gltf") continue;
                    track_watch(me.path());
                    std::string stem = to_lower_str(me.path().stem().string());
                    const auto oit = model_override_owners_.find(stem);
                    if (oit == model_override_owners_.end()) {
                        model_overrides_.emplace(stem, me.path());
                        model_override_owners_.emplace(std::move(stem), m.folder);
                    } else {
                        conflicts_.push_back(ModConflict{"models", me.path().filename().string(), oit->second, m.folder});
                    }
                }
            }

            // Layer 2: Declarative .pnach Memory & Code Patches
            const std::filesystem::path patch_dir = mdir / "patches";
            if (std::filesystem::is_directory(patch_dir, ec)) {
                std::vector<std::filesystem::path> pfiles;
                for (const auto &pe : std::filesystem::directory_iterator(patch_dir, ec)) {
                    if (pe.is_regular_file(ec) && to_lower_str(pe.path().extension().string()) == ".pnach") {
                        pfiles.push_back(pe.path());
                    }
                }
                std::sort(pfiles.begin(), pfiles.end());
                for (const auto &pf : pfiles) {
                    track_watch(pf);
                    parse_pnach_file(pf, m.folder);
                }
            }

            // Layer 3: Embedded Lua-Subset Script Hooks
            const std::filesystem::path script_dir = mdir / "scripts";
            if (std::filesystem::is_directory(script_dir, ec)) {
                std::vector<std::filesystem::path> sfiles;
                for (const auto &se : std::filesystem::directory_iterator(script_dir, ec)) {
                    if (se.is_regular_file(ec) && to_lower_str(se.path().extension().string()) == ".lua") {
                        sfiles.push_back(se.path());
                    }
                }
                std::sort(sfiles.begin(), sfiles.end());
                for (const auto &sf : sfiles) {
                    track_watch(sf);
                    std::ifstream ifs(sf);
                    if (!ifs) continue;
                    std::ostringstream oss; oss << ifs.rdbuf();
                    detail::LuaScript sc;
                    if (sc.load_source(m.name, sf.filename().string(), oss.str(), mdir, cfg_map, str_cfg_map, m.state_path, m.coop_safe)) {
                        if (sc.has_hook("on_frame")) has_on_frame_hook_ = true;
                        if (sc.has_hook("on_hotkey")) has_on_hotkey_hook_ = true;
                        if (sc.has_hook("on_game_ready") || sc.has_hook("on_battle_win") ||
                            sc.has_hook("on_ring_perfect") || sc.has_hook("on_ring_hit") ||
                            sc.has_hook("on_ring_miss") || sc.has_hook("on_battle_start") ||
                            sc.has_hook("on_battle_end") || sc.has_hook("on_turn_begin") ||
                            sc.has_hook("on_area_load") || sc.has_hook("on_level_up") ||
                            sc.has_hook("on_ring_result")) {
                            has_semantic_hooks_ = true;
                        }
                        if (!m.coop_safe) {
                            gp_hash = fnv_str(gp_hash, sf.filename().string());
                            gp_hash = fnv_str(gp_hash, oss.str());
                        }
                        scripts_.push_back(std::move(sc));
                    }
                }
            }

            // Layer 4: Native C-API v1 .dll Plugins (strictly opt-in via allow_native_plugins = 1)
            const std::filesystem::path plug_dir = mdir / "plugins";
            if (std::filesystem::is_directory(plug_dir, ec)) {
                std::vector<std::filesystem::path> dfiles;
                for (const auto &de : std::filesystem::directory_iterator(plug_dir, ec)) {
                    if (de.is_regular_file(ec) && to_lower_str(de.path().extension().string()) == ".dll") {
                        dfiles.push_back(de.path());
                    }
                }
                std::sort(dfiles.begin(), dfiles.end());
                for (const auto &df : dfiles) {
                    if (!allow_native_plugins_) {
                        push_log("[" + m.name + "] Skipped native plugin " + df.filename().string() +
                                 " (set allow_native_plugins = 1 in keys.ini to enable)");
                    } else {
                        load_native_plugin(m.name, df);
                    }
                }
            }
        }
        coop_gameplay_hash_ = any_gp_mod ? gp_hash : 0u;
        coop_gameplay_summary_ = any_gp_mod ? std::move(gp_summary) : std::string("none");
    }

    // Layer 1 lookups
    std::filesystem::path resolve_file_override(const std::string &rel_disc_path) const {
        const auto it = file_overrides_.find(to_lower_str(rel_disc_path));
        return (it != file_overrides_.end()) ? it->second : std::filesystem::path{};
    }
    std::filesystem::path resolve_texture_override(const std::string &filename) const {
        const auto it = texture_overrides_.find(to_lower_str(filename));
        return (it != texture_overrides_.end()) ? it->second : std::filesystem::path{};
    }
    std::filesystem::path resolve_video_override(const std::string &filename) const {
        const auto it = video_overrides_.find(to_lower_str(filename));
        return (it != video_overrides_.end()) ? it->second : std::filesystem::path{};
    }
    std::filesystem::path resolve_model_override(const std::string &actor_stem) const {
        const auto it = model_overrides_.find(to_lower_str(actor_stem));
        return (it != model_overrides_.end()) ? it->second : std::filesystem::path{};
    }
    const std::unordered_map<std::string, std::filesystem::path> &model_overrides() const noexcept { return model_overrides_; }
    std::size_t model_override_count() const noexcept { return model_overrides_.size(); }
    const std::vector<std::filesystem::path> &texture_override_dirs() const noexcept { return texture_override_dirs_; }
    const std::vector<std::filesystem::path> &video_override_dirs() const noexcept { return video_override_dirs_; }
    const std::vector<ModConflict> &conflicts() const noexcept { return conflicts_; }
    const std::vector<HudWidget> &hud_widgets() const noexcept { return hud_widgets_; }
    const std::vector<CanvasCmd> &canvas_cmds() const noexcept { return canvas_cmds_; }
    std::uint32_t pad_inject_mask() const noexcept { return pad_inject_; }
    std::uint32_t pad_block_mask() const noexcept { return pad_block_; }
    float item_glow_strength() const noexcept { return item_glow_strength_; }
    std::uint32_t item_glow_rgb() const noexcept { return item_glow_rgb_; }
    float item_glow_pulse_speed() const noexcept { return item_glow_pulse_speed_; }
    const std::deque<std::string> &recent_logs() const noexcept { return log_ring_; }
    const std::vector<std::string> &active_mod_names() const noexcept { return active_mod_names_; }
    std::size_t loaded_plugin_count() const noexcept { return native_plugins_.size(); }
    std::size_t texture_override_count() const noexcept { return texture_overrides_.size(); }
    std::size_t video_override_count() const noexcept { return video_overrides_.size(); }

    void on_boot(std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (!coop_gameplay_paused_) apply_patches(ram, ram_size, /*boot_pass=*/true);
        run_script_event(ScriptHookEvent::OnBoot, ram, ram_size);
        if (!coop_gameplay_paused_) dispatch_native_boot(ram, ram_size);
    }

    bool poll_hot_reload(std::uint8_t *ram, std::size_t ram_size) {
        if (!dev_mode_ || loaded_game_dir_.empty() || watch_files_.empty()) return false;
        std::error_code ec;
        bool changed = false;
        for (const auto &wf : watch_files_) {
            if (!std::filesystem::is_regular_file(wf.path, ec)) { changed = true; break; }
            if (std::filesystem::last_write_time(wf.path, ec) != wf.mtime) { changed = true; break; }
        }
        if (!changed) return false;
        return manual_reload(ram, ram_size, /*is_auto=*/true);
    }

    // Live in-place settings update: updates Lua settings_store & string_settings_store without
    // resetting runtime globals, and invokes optional on_setting_changed(key, value) hooks.
    bool apply_mod_settings_live(const std::string &mod_folder,
                                 const std::vector<ModSettingDef> &settings,
                                 std::uint8_t *ram, std::size_t ram_size) {
        if (loaded_game_dir_.empty() || mod_folder.empty()) return false;
        const auto dit = mod_dirs_.find(mod_folder);
        const std::filesystem::path mdir = (dit != mod_dirs_.end()) ? dit->second : (loaded_game_dir_ / "mods" / mod_folder);
        const auto cit = mod_config_paths_.find(mod_folder);
        const std::filesystem::path cfg_path = (cit != mod_config_paths_.end()) ? cit->second : (mdir / "mod_config.ini");
        save_mod_settings(mdir, settings, cfg_path);

        // Keep dev-mode file watcher in sync so our own save doesn't trigger a redundant full reload
        std::error_code ec;
        if (std::filesystem::is_regular_file(cfg_path, ec)) {
            const auto mt = std::filesystem::last_write_time(cfg_path, ec);
            if (!ec) {
                for (auto &wf : watch_files_) {
                    if (wf.path == cfg_path) wf.mtime = mt;
                }
            }
        }

        detail::ScriptHostCtx ctx = make_script_ctx(ram, ram_size);
        bool updated_any = false;
        for (auto &sc : scripts_) {
            if (sc.mod_folder != mod_folder) continue;
            updated_any = true;
            for (const auto &sd : settings) {
                const auto prev_num_it = sc.settings_store.find(sd.key);
                const auto prev_str_it = sc.string_settings_store.find(sd.key);
                const bool changed = (sd.type == "hotkey")
                    ? (prev_str_it == sc.string_settings_store.end() || prev_str_it->second != sd.current_str)
                    : (prev_num_it == sc.settings_store.end() || std::abs(prev_num_it->second - sd.current_val) > 1e-9);
                sc.settings_store[sd.key] = sd.current_val;
                if (!sd.current_str.empty()) {
                    sc.string_settings_store[sd.key] = sd.current_str;
                } else {
                    sc.string_settings_store.erase(sd.key);
                }
                if (changed && sc.has_hook("on_setting_changed") && (!coop_gameplay_paused_ || sc.coop_safe)) {
                    const detail::Val v = (sd.type == "hotkey")
                        ? detail::Val::string(sd.current_str)
                        : detail::Val::number(sd.current_val);
                    sc.call_hook("on_setting_changed", ctx, {detail::Val::string(sd.key), v});
                }
            }
        }
        return updated_any;
    }

    bool manual_reload(std::uint8_t *ram, std::size_t ram_size, bool is_auto = false) {
        if (loaded_game_dir_.empty()) return false;
        const std::filesystem::path gdir = loaded_game_dir_;
        const std::string spec = loaded_mods_spec_;
        const bool en = loaded_enabled_;
        load_from_game_dir(gdir, spec, en);
        if (ram && ram_size > 0) on_boot(ram, ram_size);
        const std::string msg = std::string(is_auto ? "[MOD DEV] Auto hot-reloaded " : "[MOD DEV] Reloaded ") +
                                std::to_string(active_mod_names_.size()) + " mod(s)";
        push_log(msg);
        if (toast_queue_.size() < 8) toast_queue_.push_back(msg);
        return true;
    }

    void on_frame(std::uint8_t *ram, std::size_t ram_size) noexcept {
        ++frame_counter_;
        canvas_cmds_.clear();
        pad_inject_ = 0;
        pad_block_ = 0;
        if (dev_mode_ && (frame_counter_ % 30u) == 0u) {
            poll_hot_reload(ram, ram_size);
        }
        if (!coop_gameplay_paused_ && !patches_.empty()) apply_patches(ram, ram_size, /*boot_pass=*/false);
        if (has_on_hotkey_hook_ && (keys_pressed_ != 0u || pad_pressed_ != 0u)) {
            dispatch_hotkeys(ram, ram_size);
        }
        if (has_on_frame_hook_) {
            run_script_event(ScriptHookEvent::OnFrame, ram, ram_size);
        }
        if (!coop_gameplay_paused_ && !native_plugins_.empty()) dispatch_native_frame(ram, ram_size);
        if ((has_semantic_hooks_ || !native_plugins_.empty()) && ram && ram_size > 0 && (frame_counter_ % 6u) == 0u) {
            poll_semantic_events(ram, ram_size);
        }
        keys_pressed_ = 0;
        pad_pressed_ = 0;
    }

    LiveInspectorSnapshot capture_inspector_snapshot(const std::uint8_t *ram, std::size_t ram_size) const {
        LiveInspectorSnapshot snap{};
        snap.game_slug = game_slug_;
        snap.hit_streak = ev_hit_streak_;
        snap.perf_streak = ev_perf_streak_;
        if (!ram || ram_size == 0) return snap;
        auto rd8 = [&](std::uint32_t a) -> std::uint8_t {
            const std::size_t o = static_cast<std::size_t>(a) & (ram_size - 1u);
            return (o < ram_size) ? ram[o] : 0u;
        };
        auto rd16 = [&](std::uint32_t a) -> std::uint16_t {
            const std::size_t o = static_cast<std::size_t>(a) & (ram_size - 1u);
            std::uint16_t v = 0;
            if (o + 2 <= ram_size) std::memcpy(&v, ram + o, 2);
            return v;
        };
        auto rd32 = [&](std::uint32_t a) -> std::uint32_t {
            const std::size_t o = static_cast<std::size_t>(a) & (ram_size - 1u);
            std::uint32_t v = 0;
            if (o + 4 <= ram_size) std::memcpy(&v, ram + o, 4);
            return v;
        };
        int kind = 0;
        if (ram_size >= 0x00500000u) {
            if (rd8(0x004CAE40u) == 0x43u && rd8(0x004CAE41u) == 0x68u &&
                (rd8(0x00100280u) & 0x40u) != 0u && rd32(0x0010000Cu) >= 1000u) kind = 1;
            else if (rd8(0x00100000u) == 0x53u && rd8(0x00100001u) == 0x53u && rd8(0x00100002u) == 0x48u) {
                const std::uint32_t raw = rd32(0x00102300u);
                float pt = 0.0f; std::memcpy(&pt, &raw, 4);
                if (pt >= 1.0f && pt < 1e8f) kind = 2;
            } else if (rd8(0x0010001Du) == 0x53u && rd8(0x0010001Eu) == 0x48u && rd8(0x0010001Fu) == 0x33u &&
                       rd32(0x00100098u) >= 1000u) kind = 3;
        } else if (ram_size == 0x00200000u) {
            kind = detail::SemanticRam::detect_active_game(ram, ram_size);
        }
        snap.detected_game = kind;
        snap.in_game = (kind != 0);
        if (kind == 1) {
            snap.cash = rd32(0x00100008u);
            snap.battles_won = rd32(0x001014A4u);
            snap.ring_attempts = rd32(0x001014ACu); snap.ring_successes = rd32(0x001014B0u); snap.ring_perfects = rd32(0x001014B4u);
            for (int i = 0; i < 6; ++i) {
                const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                const bool j = (rd8(b) & 0x40u) != 0u;
                const int lv = rd8(b + 0x03u);
                if (j && lv > snap.max_level) snap.max_level = lv;
                snap.heroes.push_back({i, j, lv, rd16(b + 0x10u), rd16(b + 0x14u), rd16(b + 0x12u), rd16(b + 0x16u), rd32(b + 0x04u)});
            }
        } else if (kind == 2) {
            snap.cash = rd32(0x00102D30u + (rd8(0x00100132u) & 3u) * 4u);
            snap.battles_won = rd32(0x001016E4u);
            snap.ring_attempts = rd32(0x001017C8u); snap.ring_successes = rd32(0x001017CCu); snap.ring_perfects = rd32(0x001017D0u);
            for (int i = 0; i < 8; ++i) {
                const std::uint32_t b = 0x00100250u + static_cast<std::uint32_t>(i) * 0x90u;
                const bool j = rd8(b + 0x16u) != 0u;
                const int lv = rd8(b + 0x0Fu);
                if (j && lv > snap.max_level) snap.max_level = lv;
                snap.heroes.push_back({i, j, lv, rd16(b + 0x2Au), rd16(b + 0x28u), rd16(b + 0x2Eu), rd16(b + 0x2Cu), rd32(b + 0x10u)});
            }
        } else if (kind == 3) {
            snap.cash = rd32(0x00100D54u);
            snap.battles_won = rd32(0x00102734u);
            for (int i = 0; i < 7; ++i) {
                const std::uint32_t rb = 0x00102778u + static_cast<std::uint32_t>(i) * 0x90u;
                snap.ring_attempts += rd32(rb + 0x0Cu); snap.ring_successes += rd32(rb + 0x18u); snap.ring_perfects += rd32(rb + 0x1Cu);
                const std::uint32_t b = 0x00100D70u + static_cast<std::uint32_t>(i) * 0x84u;
                const bool j = rd8(b + 0x0Au) != 0u;
                const int lv = rd8(b + 0x03u);
                if (j && lv > snap.max_level) snap.max_level = lv;
                snap.heroes.push_back({i, j, lv, rd16(b + 0x1Cu), rd16(b + 0x1Eu), rd16(b + 0x20u), rd16(b + 0x22u), rd32(b + 0x04u)});
            }
        } else if (kind == 4) {
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t p = detail::SemanticRam::kdl_actor_ptr(ram, ram_size, i);
                if (p < 0x80010000u || p >= 0x801FE000u) continue;
                const bool j = rd8(p + 0x4BAu) != 0u;
                const int lv = rd16(p + 0x2D8u);
                if (j && lv > snap.max_level) snap.max_level = lv;
                snap.heroes.push_back({i, j, lv, rd16(p + 0x410u), rd16(p + 0x412u), rd16(p + 0x414u), rd16(p + 0x416u), rd32(p + 0x2DCu)});
            }
        }
        return snap;
    }

    void on_event(ScriptHookEvent ev, std::uint8_t *ram, std::size_t ram_size) noexcept {
        run_script_event(ev, ram, ram_size);
    }

    std::vector<std::string> take_toasts() {
        std::vector<std::string> out;
        out.swap(toast_queue_);
        return out;
    }

    std::size_t active_mod_count() const noexcept { return active_mod_names_.size(); }
    std::size_t file_override_count() const noexcept { return file_overrides_.size(); }
    std::size_t patch_count() const noexcept { return patches_.size(); }
    std::size_t script_op_count() const noexcept {
        std::size_t total = 0;
        for (const auto &sc : scripts_) total += sc.hook_count();
        return total;
    }

    static NocturneModApiV1 make_c_api(const char *slug, std::uint8_t *ram, std::uint32_t ram_size) noexcept {
        NocturneModApiV1 api{};
        api.api_version = 1;
        api.game_slug = slug;
        api.guest_ram = ram;
        api.guest_ram_size = ram_size;
        api.read_u8 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint8_t {
            if (!r || sz == 0u) return 0u;
            const std::uint32_t off = a & (sz - 1u);
            return (off < sz) ? r[off] : 0u;
        };
        api.read_u16 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint16_t {
            if (!r || sz < 2u) return 0u;
            const std::uint32_t off = a & (sz - 1u);
            std::uint16_t v = 0;
            if (off <= sz - 2u) std::memcpy(&v, r + off, 2);
            return v;
        };
        api.read_u32 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint32_t {
            if (!r || sz < 4u) return 0u;
            const std::uint32_t off = a & (sz - 1u);
            std::uint32_t v = 0;
            if (off <= sz - 4u) std::memcpy(&v, r + off, 4);
            return v;
        };
        api.write_u8 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint8_t v) {
            if (!r || sz == 0u) return;
            const std::uint32_t off = a & (sz - 1u);
            if (off < sz) r[off] = v;
        };
        api.write_u16 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint16_t v) {
            if (!r || sz < 2u) return;
            const std::uint32_t off = a & (sz - 1u);
            if (off <= sz - 2u) std::memcpy(r + off, &v, 2);
        };
        api.write_u32 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint32_t v) {
            if (!r || sz < 4u) return;
            const std::uint32_t off = a & (sz - 1u);
            if (off <= sz - 4u) std::memcpy(r + off, &v, 4);
        };
        return api;
    }

private:
    struct WatchEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime{};
    };
    struct LoadedPlugin {
        std::string mod_name;
        std::string file_name;
#ifdef _WIN32
        HMODULE handle = nullptr;
#else
        void *handle = nullptr;
#endif
        NocturneModCallbacksV1 callbacks{};
    };

    std::string game_slug_;
    std::filesystem::path loaded_game_dir_;
    std::string loaded_mods_spec_;
    bool loaded_enabled_ = false;
    bool dev_mode_ = false;
    bool allow_native_plugins_ = false;
    bool has_on_frame_hook_ = false;
    bool has_on_hotkey_hook_ = false;
    bool has_semantic_hooks_ = false;
    std::vector<std::string> active_mod_names_;
    std::unordered_map<std::string, std::filesystem::path> file_overrides_;
    std::unordered_map<std::string, std::string> file_override_owners_;
    std::unordered_map<std::string, std::filesystem::path> texture_overrides_;
    std::unordered_map<std::string, std::string> texture_override_owners_;
    std::vector<std::filesystem::path> texture_override_dirs_;
    std::unordered_map<std::string, std::filesystem::path> video_overrides_;
    std::unordered_map<std::string, std::string> video_override_owners_;
    std::vector<std::filesystem::path> video_override_dirs_;
    std::unordered_map<std::string, std::filesystem::path> model_overrides_;
    std::unordered_map<std::string, std::string> model_override_owners_;
    std::vector<PnachPatch> patches_;
    std::unordered_map<std::uint32_t, std::string> patch_owners_;
    std::vector<ModConflict> conflicts_;
    std::vector<detail::LuaScript> scripts_;
    std::vector<LoadedPlugin> native_plugins_;
    std::vector<std::string> toast_queue_;
    std::vector<HudWidget> hud_widgets_;
    std::vector<CanvasCmd> canvas_cmds_;
    std::uint32_t pad_inject_ = 0;
    std::uint32_t pad_block_ = 0;
    std::deque<std::string> log_ring_;
    std::vector<WatchEntry> watch_files_;
    std::uint64_t frame_counter_ = 0;
    std::uint32_t rng_state_ = 0x5EED1913u;
    std::uint32_t keys_down_ = 0;
    std::uint32_t keys_pressed_ = 0;
    std::uint32_t keys_prev_ = 0;
    std::uint32_t pad_down_ = 0;
    std::uint32_t pad_pressed_ = 0;
    std::uint32_t pad_prev_ = 0;
    bool ev_initialized_ = false;
    int ev_game_kind_ = 0;
    std::uint32_t ev_prev_battles_ = 0;
    std::uint32_t ev_prev_att_ = 0;
    std::uint32_t ev_prev_suc_ = 0;
    std::uint32_t ev_prev_prf_ = 0;
    int ev_hit_streak_ = 0;
    int ev_perf_streak_ = 0;
    bool ev_in_battle_ = false;
    std::uint32_t ev_battle_char_ = 0xFFu;
    std::uint32_t ev_area_id_ = 0xFFFFFFFFu;
    bool ev_lvlup_active_ = false;
    std::uint32_t ev_lvlup_char_ = 0xFFu;
    float item_glow_strength_ = 0.0f;
    std::uint32_t item_glow_rgb_ = 0xFFCC44u;
    float item_glow_pulse_speed_ = 1.0f;
    std::uint64_t coop_gameplay_hash_ = 0u;
    std::string coop_gameplay_summary_;
    bool coop_gameplay_paused_ = false;
    std::unordered_map<std::string, std::filesystem::path> mod_config_paths_;
    std::unordered_map<std::string, std::filesystem::path> mod_dirs_;

    void push_log(const std::string &line) {
        log_ring_.push_back(line);
        if (log_ring_.size() > 80) log_ring_.pop_front();
        if (dev_mode_ && !loaded_game_dir_.empty()) {
            std::ofstream lfs(loaded_game_dir_ / "mods.log", std::ios::app);
            if (lfs) lfs << "[f" << frame_counter_ << "] " << line << "\n";
        }
    }

    detail::ScriptHostCtx make_script_ctx(std::uint8_t *ram, std::size_t ram_size) noexcept {
        detail::ScriptHostCtx ctx{};
        ctx.ram = ram;
        ctx.ram_size = ram_size;
        ctx.frame_index = frame_counter_;
        ctx.game_slug = game_slug_;
        ctx.toast_queue = &toast_queue_;
        ctx.rng_state = &rng_state_;
        ctx.keys_down = keys_down_;
        ctx.keys_pressed = keys_pressed_;
        ctx.pad_down = pad_down_;
        ctx.pad_pressed = pad_pressed_;
        ctx.hud_widgets = &hud_widgets_;
        ctx.canvas_cmds = &canvas_cmds_;
        ctx.pad_inject = &pad_inject_;
        ctx.pad_block = &pad_block_;
        ctx.log_ring = &log_ring_;
        ctx.item_glow_strength = &item_glow_strength_;
        ctx.item_glow_rgb = &item_glow_rgb_;
        ctx.item_glow_pulse_speed = &item_glow_pulse_speed_;
        if (dev_mode_ && !loaded_game_dir_.empty()) {
            ctx.dev_log_path = loaded_game_dir_ / "mods.log";
        }
        return ctx;
    }

    void call_named_hook(const char *hname, std::uint8_t *ram, std::size_t ram_size,
                         const std::vector<detail::Val> &args = {}) noexcept {
        if (scripts_.empty()) return;
        detail::ScriptHostCtx ctx = make_script_ctx(ram, ram_size);
        for (auto &sc : scripts_) {
            if (coop_gameplay_paused_ && !sc.coop_safe) continue;
            if (sc.has_hook(hname)) sc.call_hook(hname, ctx, args);
        }
    }

    void dispatch_hotkeys(std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (scripts_.empty() || (keys_pressed_ == 0u && pad_pressed_ == 0u)) return;
        struct KeyNameBit { std::uint32_t bit; const char *name; };
        static constexpr KeyNameBit kKeyMap[] = {
            {mod_input::kKeyF5, "F5"}, {mod_input::kKeyF6, "F6"},
            {mod_input::kKeyF7, "F7"}, {mod_input::kKeyF8, "F8"},
            {mod_input::kKey1, "1"}, {mod_input::kKey2, "2"}, {mod_input::kKey3, "3"},
            {mod_input::kKey4, "4"}, {mod_input::kKey5, "5"}, {mod_input::kKey6, "6"},
            {mod_input::kKey7, "7"}, {mod_input::kKey8, "8"}, {mod_input::kKey9, "9"},
            {mod_input::kKey0, "0"}, {mod_input::kKeyAsterisk, "*"},
        };
        for (const auto &kn : kKeyMap) {
            if ((keys_pressed_ & kn.bit) != 0u) {
                call_named_hook("on_hotkey", ram, ram_size, {detail::Val::string(kn.name)});
            }
        }
    }

    void unload_native_plugins() noexcept {
#ifdef _WIN32
        for (auto &pl : native_plugins_) {
            if (pl.handle) FreeLibrary(pl.handle);
        }
#endif
        native_plugins_.clear();
    }

    void load_native_plugin(const std::string &mod_name, const std::filesystem::path &dll_path) {
#ifdef _WIN32
        HMODULE h = LoadLibraryW(dll_path.c_str());
        if (!h) {
            push_log("[" + mod_name + "] Failed to load native plugin " + dll_path.filename().string());
            return;
        }
        auto init_fn = reinterpret_cast<NocturneModInitFn>(GetProcAddress(h, "NocturneModInitV1"));
        if (!init_fn) {
            FreeLibrary(h);
            push_log("[" + mod_name + "] Missing NocturneModInitV1 export in " + dll_path.filename().string());
            return;
        }
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), nullptr, 0);
        NocturneModCallbacksV1 cbs{};
        if (!init_fn(&api, &cbs)) {
            FreeLibrary(h);
            push_log("[" + mod_name + "] NocturneModInitV1 returned false for " + dll_path.filename().string());
            return;
        }
        native_plugins_.push_back(LoadedPlugin{mod_name, dll_path.filename().string(), h, cbs});
        push_log("[" + mod_name + "] Loaded native plugin " + dll_path.filename().string());
#else
        (void)mod_name; (void)dll_path;
#endif
    }

    void dispatch_native_boot(std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_boot) pl.callbacks.on_boot(&api);
        }
    }

    void dispatch_native_frame(std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_frame) pl.callbacks.on_frame(&api, frame_counter_);
        }
    }

    void dispatch_native_battle_start(std::uint8_t *ram, std::size_t ram_size, std::uint32_t battle_id) noexcept {
        if (coop_gameplay_paused_ || native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_battle_start) pl.callbacks.on_battle_start(&api, battle_id);
        }
    }

    void dispatch_native_turn_begin(std::uint8_t *ram, std::size_t ram_size, std::uint32_t actor_slot) noexcept {
        if (coop_gameplay_paused_ || native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_turn_begin) pl.callbacks.on_turn_begin(&api, actor_slot);
        }
    }

    void dispatch_native_ring_result(std::uint8_t *ram, std::size_t ram_size,
                                     std::uint32_t actor_slot, std::uint32_t hits, std::uint32_t perfects) noexcept {
        if (coop_gameplay_paused_ || native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_ring_result) pl.callbacks.on_ring_result(&api, actor_slot, hits, perfects);
        }
    }

    void dispatch_native_area_load(std::uint8_t *ram, std::size_t ram_size, const char *area_code) noexcept {
        if (coop_gameplay_paused_ || native_plugins_.empty()) return;
        NocturneModApiV1 api = make_c_api(game_slug_.c_str(), ram, static_cast<std::uint32_t>(ram_size));
        for (const auto &pl : native_plugins_) {
            if (pl.callbacks.on_area_load) pl.callbacks.on_area_load(&api, area_code);
        }
    }

    void poll_semantic_events(std::uint8_t *ram, std::size_t ram_size) noexcept {
        auto rd8 = [&](std::uint32_t a) -> std::uint8_t {
            const std::size_t o = static_cast<std::size_t>(a) & (ram_size - 1u);
            return (o < ram_size) ? ram[o] : 0u;
        };
        auto rd32 = [&](std::uint32_t a) -> std::uint32_t {
            const std::size_t o = static_cast<std::size_t>(a) & (ram_size - 1u);
            std::uint32_t v = 0;
            if (o + 4 <= ram_size) std::memcpy(&v, ram + o, 4);
            return v;
        };
        int kind = 0;
        if (ram_size >= 0x00500000u) {
            if (rd8(0x004CAE40u) == 0x43u && rd8(0x004CAE41u) == 0x68u &&
                (rd8(0x00100280u) & 0x40u) != 0u && rd32(0x0010000Cu) >= 1000u) kind = 1;
            else if (rd8(0x00100000u) == 0x53u && rd8(0x00100001u) == 0x53u && rd8(0x00100002u) == 0x48u) {
                const std::uint32_t raw = rd32(0x00102300u);
                float pt = 0.0f; std::memcpy(&pt, &raw, 4);
                if (pt >= 1.0f && pt < 1e8f) kind = 2;
            } else if (rd8(0x0010001Du) == 0x53u && rd8(0x0010001Eu) == 0x48u && rd8(0x0010001Fu) == 0x33u &&
                       rd32(0x00100098u) >= 1000u) kind = 3;
        } else if (ram_size == 0x00200000u) {
            kind = detail::SemanticRam::detect_active_game(ram, ram_size);
        }
        if (kind == 0) return;

        std::uint32_t battles = 0, att = 0, suc = 0, prf = 0;
        if (kind == 1) {
            battles = rd32(0x001014A4u); att = rd32(0x001014ACu); suc = rd32(0x001014B0u); prf = rd32(0x001014B4u);
        } else if (kind == 2) {
            battles = rd32(0x001016E4u); att = rd32(0x001017C8u); suc = rd32(0x001017CCu); prf = rd32(0x001017D0u);
        } else if (kind == 3) {
            battles = rd32(0x00102734u);
            for (int i = 0; i < 7; ++i) {
                const std::uint32_t b = 0x00102778u + static_cast<std::uint32_t>(i) * 0x90u;
                att += rd32(b + 0x0Cu); suc += rd32(b + 0x18u); prf += rd32(b + 0x1Cu);
            }
        }

        if (!ev_initialized_ || kind != ev_game_kind_ || att < ev_prev_att_ || battles < ev_prev_battles_) {
            ev_initialized_ = true;
            ev_game_kind_ = kind;
            ev_prev_battles_ = battles;
            ev_prev_att_ = att;
            ev_prev_suc_ = suc;
            ev_prev_prf_ = prf;
            ev_hit_streak_ = 0;
            ev_perf_streak_ = 0;
            if (kind == 4) {
                const std::uint32_t init_room = rd32(0x000B6B08u);
                if (init_room != 0u && init_room < 0x400u) ev_area_id_ = init_room;
            }
            call_named_hook("on_game_ready", ram, ram_size);
            return;
        }

        if (att > ev_prev_att_) {
            const std::uint32_t da = att - ev_prev_att_;
            const std::uint32_t ds = (suc >= ev_prev_suc_) ? (suc - ev_prev_suc_) : 0u;
            const std::uint32_t dp = (prf >= ev_prev_prf_) ? (prf - ev_prev_prf_) : 0u;
            ev_prev_att_ = att; ev_prev_suc_ = suc; ev_prev_prf_ = prf;
            if (ds < da) {
                const int prev_s = ev_hit_streak_;
                ev_hit_streak_ = 0;
                ev_perf_streak_ = 0;
                call_named_hook("on_ring_miss", ram, ram_size, {detail::Val::number(static_cast<double>(prev_s))});
                call_named_hook("on_ring_result", ram, ram_size, {detail::Val::string("miss"), detail::Val::number(0.0)});
                dispatch_native_ring_result(ram, ram_size, 0u, 0u, 0u);
            } else {
                ev_hit_streak_ += static_cast<int>(ds);
                if (dp >= da) {
                    ev_perf_streak_ += static_cast<int>(dp);
                    call_named_hook("on_ring_perfect", ram, ram_size, {detail::Val::number(static_cast<double>(ev_perf_streak_))});
                    call_named_hook("on_ring_result", ram, ram_size, {detail::Val::string("perfect"), detail::Val::number(static_cast<double>(ev_perf_streak_))});
                } else {
                    ev_perf_streak_ = 0;
                    call_named_hook("on_ring_hit", ram, ram_size, {detail::Val::number(static_cast<double>(ev_hit_streak_))});
                    call_named_hook("on_ring_result", ram, ram_size, {detail::Val::string("hit"), detail::Val::number(static_cast<double>(ev_hit_streak_))});
                }
                dispatch_native_ring_result(ram, ram_size, 0u, ds, dp);
            }
        }

        if (battles > ev_prev_battles_) {
            ev_prev_battles_ = battles;
            call_named_hook("on_battle_win", ram, ram_size, {detail::Val::number(static_cast<double>(battles))});
            if (kind != 4) {
                call_named_hook("on_battle_end", ram, ram_size);
            }
        }

        // Koudelka (PS1) live lifecycle hooks: on_area_load, on_battle_start/end, on_turn_begin, on_level_up
        if (kind == 4) {
            const std::uint32_t room_id = rd32(0x000B6B08u);
            if (room_id != 0u && room_id < 0x400u) {
                if (ev_area_id_ == 0xFFFFFFFFu) {
                    ev_area_id_ = room_id;
                } else if (room_id != ev_area_id_) {
                    ev_area_id_ = room_id;
                    call_named_hook("on_area_load", ram, ram_size, {detail::Val::number(static_cast<double>(room_id))});
                    char rbuf[24];
                    std::snprintf(rbuf, sizeof(rbuf), "ROOM_%03u", room_id);
                    dispatch_native_area_load(ram, ram_size, rbuf);
                }
            }
            const std::uint32_t gm = rd32(0x000AF470u);
            if (gm >= 0x80010000u && gm < 0x801FF000u) {
                const std::uint32_t gm_off = gm - 0x80000000u;
                const bool in_b = (rd32(gm_off + 0x36Cu) == 1u);
                if (in_b && !ev_in_battle_) {
                    ev_in_battle_ = true;
                    ev_battle_char_ = 0xFFu;
                    call_named_hook("on_battle_start", ram, ram_size);
                    dispatch_native_battle_start(ram, ram_size, ev_area_id_);
                } else if (!in_b && ev_in_battle_) {
                    ev_in_battle_ = false;
                    ev_battle_char_ = 0xFFu;
                    call_named_hook("on_battle_end", ram, ram_size);
                }
                if (in_b) {
                    std::uint32_t cid = rd32(0x000B4D5Cu);
                    const std::uint32_t scene = rd32(gm_off + 0x32Cu);
                    if (scene >= 0x80010000u && scene < 0x801FF000u) {
                        const std::uint32_t actor = rd32((scene - 0x80000000u) + 0xA0u);
                        if (actor >= 0x80010000u && actor < 0x801FF000u) {
                            const std::uint32_t ac = rd8((actor - 0x80000000u) + 0x4BCu);
                            if (ac < 3u) cid = ac;
                        }
                    }
                    if (cid < 3u && cid != ev_battle_char_) {
                        ev_battle_char_ = cid;
                        call_named_hook("on_turn_begin", ram, ram_size, {detail::Val::number(static_cast<double>(cid))});
                        dispatch_native_turn_begin(ram, ram_size, cid);
                    }
                }
                const std::uint32_t lvlup = rd32(gm_off + 0x358u);
                const bool in_lu = (lvlup >= 0x80010000u && lvlup < 0x801FF000u);
                if (in_lu) {
                    const std::uint32_t cid = rd32(0x000B4D5Cu);
                    if (!ev_lvlup_active_ || (cid < 3u && cid != ev_lvlup_char_)) {
                        ev_lvlup_active_ = true;
                        ev_lvlup_char_ = cid;
                        call_named_hook("on_level_up", ram, ram_size, {detail::Val::number(static_cast<double>(cid < 3u ? cid : 0u))});
                    }
                } else {
                    ev_lvlup_active_ = false;
                    ev_lvlup_char_ = 0xFFu;
                }
            }
        }
    }

    static std::string trim_str(const std::string &s) {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    static std::string to_lower_str(std::string s) {
        for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static const char *hook_name(ScriptHookEvent ev) noexcept {
        switch (ev) {
        case ScriptHookEvent::OnBoot:        return "on_boot";
        case ScriptHookEvent::OnFrame:       return "on_frame";
        case ScriptHookEvent::OnBattleStart: return "on_battle_start";
        case ScriptHookEvent::OnBattleEnd:   return "on_battle_end";
        case ScriptHookEvent::OnTurnBegin:   return "on_turn_begin";
        case ScriptHookEvent::OnRingResult:  return "on_ring_result";
        case ScriptHookEvent::OnAreaLoad:    return "on_area_load";
        }
        return "on_frame";
    }

    void parse_pnach_file(const std::filesystem::path &p, const std::string &mod_folder = {}) {
        std::ifstream ifs(p);
        if (!ifs) return;
        std::string line;
        while (std::getline(ifs, line)) {
            const std::size_t comment = line.find("//");
            if (comment != std::string::npos) line.resize(comment);
            const std::size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const std::string t = trim_str(line);
            if (t.rfind("patch=", 0) != 0 && t.rfind("patch =", 0) != 0) continue;
            const std::size_t eq = t.find('=');
            if (eq == std::string::npos) continue;

            std::vector<std::string> fields;
            std::size_t pos = eq + 1;
            while (pos <= t.size()) {
                const std::size_t c = t.find(',', pos);
                fields.push_back(trim_str((c == std::string::npos) ? t.substr(pos) : t.substr(pos, c - pos)));
                if (c == std::string::npos) break;
                pos = c + 1;
            }
            if (fields.size() < 5) continue;

            PnachPatch pp{};
            pp.on_boot_only = (fields[0] == "0");
            pp.addr = static_cast<std::uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 16));
            const std::string wtype = to_lower_str(fields[3]);
            pp.value = static_cast<std::uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 16));
            if (wtype == "byte") pp.width = PatchWidth::Byte;
            else if (wtype == "short") pp.width = PatchWidth::Short;
            else if (wtype == "extended") {
                const std::uint8_t hi = static_cast<std::uint8_t>((pp.addr >> 24) & 0xFFu);
                if (hi == 0x00u) pp.width = PatchWidth::Byte;
                else if (hi == 0x01u) pp.width = PatchWidth::Short;
                else pp.width = PatchWidth::Word;
                pp.addr &= 0x01FFFFFFu;
            } else {
                pp.width = PatchWidth::Word;
            }
            if (!mod_folder.empty()) {
                const auto pit = patch_owners_.find(pp.addr);
                if (pit == patch_owners_.end()) {
                    patch_owners_.emplace(pp.addr, mod_folder);
                } else if (pit->second != mod_folder) {
                    char abuf[24];
                    std::snprintf(abuf, sizeof(abuf), "0x%08X", pp.addr);
                    conflicts_.push_back(ModConflict{"patches", abuf, pit->second, mod_folder});
                    continue;  // Top-priority mod wins: skip shadowed lower-priority patch
                }
            }
            patches_.push_back(pp);
        }
    }

    static void write_guest(std::uint8_t *ram, std::size_t ram_size, std::uint32_t addr, std::uint32_t val, PatchWidth w) noexcept {
        if (!ram || ram_size == 0) return;
        const std::size_t off = static_cast<std::size_t>(addr) & (ram_size - 1u);
        if (w == PatchWidth::Byte && off < ram_size) {
            ram[off] = static_cast<std::uint8_t>(val & 0xFFu);
        } else if (w == PatchWidth::Short && off + 2 <= ram_size) {
            const std::uint16_t v16 = static_cast<std::uint16_t>(val & 0xFFFFu);
            std::memcpy(ram + off, &v16, 2);
        } else if (w == PatchWidth::Word && off + 4 <= ram_size) {
            std::memcpy(ram + off, &val, 4);
        }
    }

    void apply_patches(std::uint8_t *ram, std::size_t ram_size, bool boot_pass) const noexcept {
        for (const auto &p : patches_) {
            if (p.on_boot_only && !boot_pass) continue;
            write_guest(ram, ram_size, p.addr, p.value, p.width);
        }
    }

    void run_script_event(ScriptHookEvent ev, std::uint8_t *ram, std::size_t ram_size) noexcept {
        if (scripts_.empty() || !ram || ram_size == 0) return;
        detail::ScriptHostCtx ctx = make_script_ctx(ram, ram_size);
        const char *hname = hook_name(ev);
        for (auto &sc : scripts_) {
            if (coop_gameplay_paused_ && !sc.coop_safe) continue;
            sc.call_hook(hname, ctx);
        }
    }
};

}  // namespace mod_engine
