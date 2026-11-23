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
constexpr std::uint32_t kKeyShift = 1u << 13;
constexpr std::uint32_t kKeyCtrl  = 1u << 14;
constexpr std::uint32_t kKeyAlt   = 1u << 15;

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

struct ModSettingDef {
    std::string key;
    std::string label;
    std::string type;  // "bool", "int", "float"
    double default_val = 0.0;
    double min_val = 0.0;
    double max_val = 100.0;
    double step = 1.0;
    double current_val = 0.0;
};

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
    bool enabled = true;
    std::size_t file_count = 0;
    std::size_t texture_count = 0;
    std::size_t video_count = 0;
    std::size_t patch_count = 0;
    std::size_t script_count = 0;
    std::size_t plugin_count = 0;
    std::vector<ModSettingDef> settings;
};

// ---------------- Lightweight Embedded Lua-Subset Runtime (Layer 3) ----------------
namespace detail {

enum class TokKind : std::uint8_t {
    Eof, Ident, Number, String,
    Plus, Minus, Star, Slash, SlashSlash, Percent, DotDot,
    Amp, Pipe, Tilde, Shl, Shr,
    Eq, EqEq, TildeEq, Lt, Le, Gt, Ge,
    LParen, RParen, Comma, Semi,
    KwFunction, KwLocal, KwIf, KwThen, KwElseIf, KwElse, KwEnd,
    KwFor, KwWhile, KwDo, KwReturn, KwBreak, KwAnd, KwOr, KwNot,
    KwTrue, KwFalse, KwNil,
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;
    double num = 0.0;
};

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

inline std::vector<Token> tokenize(const std::string &src) {
    std::vector<Token> out;
    std::size_t i = 0;
    const std::size_t n = src.size();
    while (i < n) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        // Line comment -- ... (or block --[[ ... ]])
        if (c == '-' && i + 1 < n && src[i + 1] == '-') {
            i += 2;
            if (i + 1 < n && src[i] == '[' && src[i + 1] == '[') {
                i += 2;
                const std::size_t end = src.find("]]", i);
                i = (end == std::string::npos) ? n : (end + 2);
            } else {
                while (i < n && src[i] != '\n') ++i;
            }
            continue;
        }
        // String literal '...' or "..."
        if (c == '"' || c == '\'') {
            const char q = c;
            ++i;
            std::string s;
            while (i < n && src[i] != q) {
                if (src[i] == '\\' && i + 1 < n) {
                    ++i;
                    if (src[i] == 'n') s.push_back('\n');
                    else if (src[i] == 't') s.push_back('\t');
                    else s.push_back(src[i]);
                    ++i;
                } else {
                    s.push_back(src[i++]);
                }
            }
            if (i < n) ++i;
            Token tk; tk.kind = TokKind::String; tk.text = std::move(s);
            out.push_back(std::move(tk));
            continue;
        }
        // Number (decimal or 0x hex)
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < n && std::isxdigit(static_cast<unsigned char>(src[i]))) ++i;
                const std::uint64_t uv = std::strtoull(src.substr(start, i - start).c_str(), nullptr, 16);
                Token tk; tk.kind = TokKind::Number; tk.num = static_cast<double>(uv);
                out.push_back(std::move(tk));
            } else {
                while (i < n && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) ++i;
                Token tk; tk.kind = TokKind::Number;
                tk.num = std::strtod(src.substr(start, i - start).c_str(), nullptr);
                out.push_back(std::move(tk));
            }
            continue;
        }
        // Identifier / keyword
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = i;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) ++i;
            std::string id = src.substr(start, i - start);
            TokKind k = TokKind::Ident;
            if (id == "function") k = TokKind::KwFunction;
            else if (id == "local") k = TokKind::KwLocal;
            else if (id == "if") k = TokKind::KwIf;
            else if (id == "then") k = TokKind::KwThen;
            else if (id == "elseif") k = TokKind::KwElseIf;
            else if (id == "else") k = TokKind::KwElse;
            else if (id == "end") k = TokKind::KwEnd;
            else if (id == "for") k = TokKind::KwFor;
            else if (id == "while") k = TokKind::KwWhile;
            else if (id == "do") k = TokKind::KwDo;
            else if (id == "return") k = TokKind::KwReturn;
            else if (id == "break") k = TokKind::KwBreak;
            else if (id == "and") k = TokKind::KwAnd;
            else if (id == "or") k = TokKind::KwOr;
            else if (id == "not") k = TokKind::KwNot;
            else if (id == "true") k = TokKind::KwTrue;
            else if (id == "false") k = TokKind::KwFalse;
            else if (id == "nil") k = TokKind::KwNil;
            Token tk; tk.kind = k; tk.text = std::move(id);
            out.push_back(std::move(tk));
            continue;
        }
        // 2-char operators
        const char nx = (i + 1 < n) ? src[i + 1] : '\0';
        auto emit = [&](TokKind k, std::size_t adv) {
            Token tk; tk.kind = k; out.push_back(std::move(tk)); i += adv;
        };
        if (c == '=' && nx == '=') { emit(TokKind::EqEq, 2); continue; }
        if (c == '~' && nx == '=') { emit(TokKind::TildeEq, 2); continue; }
        if (c == '<' && nx == '=') { emit(TokKind::Le, 2); continue; }
        if (c == '>' && nx == '=') { emit(TokKind::Ge, 2); continue; }
        if (c == '<' && nx == '<') { emit(TokKind::Shl, 2); continue; }
        if (c == '>' && nx == '>') { emit(TokKind::Shr, 2); continue; }
        if (c == '/' && nx == '/') { emit(TokKind::SlashSlash, 2); continue; }
        if (c == '.' && nx == '.') { emit(TokKind::DotDot, 2); continue; }

        switch (c) {
        case '+': emit(TokKind::Plus, 1); break;
        case '-': emit(TokKind::Minus, 1); break;
        case '*': emit(TokKind::Star, 1); break;
        case '/': emit(TokKind::Slash, 1); break;
        case '%': emit(TokKind::Percent, 1); break;
        case '&': emit(TokKind::Amp, 1); break;
        case '|': emit(TokKind::Pipe, 1); break;
        case '~': emit(TokKind::Tilde, 1); break;
        case '=': emit(TokKind::Eq, 1); break;
        case '<': emit(TokKind::Lt, 1); break;
        case '>': emit(TokKind::Gt, 1); break;
        case '(': emit(TokKind::LParen, 1); break;
        case ')': emit(TokKind::RParen, 1); break;
        case ',': emit(TokKind::Comma, 1); break;
        case ';': emit(TokKind::Semi, 1); break;
        default: ++i; break;
        }
    }
    out.push_back(Token{TokKind::Eof, {}, 0.0});
    return out;
}

struct FunctionDef {
    std::vector<std::string> params;
    std::size_t body_start = 0;
    std::size_t body_end = 0;
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
    std::deque<std::string> *log_ring = nullptr;
    std::filesystem::path dev_log_path;
};

struct LuaScript {
    std::string mod_name;
    std::string mod_folder;
    std::filesystem::path mod_dir;
    std::string file_name;
    std::vector<Token> tokens;
    std::unordered_map<std::string, FunctionDef> funcs;
    std::unordered_map<std::string, Val> globals;
    std::unordered_map<std::string, Val> save_store;
    std::unordered_map<std::string, double> settings_store;
    bool top_level_ran = false;

    void load_state_file() {
        save_store.clear();
        if (mod_dir.empty()) return;
        std::ifstream ifs(mod_dir / "mod_state.ini");
        if (!ifs) return;
        std::string line;
        while (std::getline(ifs, line)) {
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
            while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front()))) k.erase(k.begin());
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
            if (k.empty()) continue;
            if (v.rfind("s:", 0) == 0) {
                save_store[k] = Val::string(v.substr(2));
            } else if (v.rfind("n:", 0) == 0) {
                save_store[k] = Val::number(std::strtod(v.c_str() + 2, nullptr));
            } else {
                char *endp = nullptr;
                const double dv = std::strtod(v.c_str(), &endp);
                if (endp && *endp == '\0' && !v.empty()) save_store[k] = Val::number(dv);
                else save_store[k] = Val::string(v);
            }
        }
    }

    void flush_state_file() const {
        if (mod_dir.empty()) return;
        std::ofstream ofs(mod_dir / "mod_state.ini", std::ios::trunc);
        if (!ofs) return;
        for (const auto &kv : save_store) {
            if (kv.second.is_nil) continue;
            if (kv.second.is_str) ofs << kv.first << "=s:" << kv.second.str << "\n";
            else ofs << kv.first << "=n:" << kv.second.to_string() << "\n";
        }
    }

    bool load_source(const std::string &mod, const std::string &fname, const std::string &src,
                     const std::filesystem::path &mdir = {},
                     const std::unordered_map<std::string, double> &cfg = {}) {
        mod_name = mod;
        mod_dir = mdir;
        mod_folder = mdir.empty() ? mod : mdir.filename().string();
        file_name = fname;
        tokens = tokenize(src);
        funcs.clear();
        globals.clear();
        settings_store = cfg;
        load_state_file();
        top_level_ran = false;
        index_functions();
        return !funcs.empty() || tokens.size() > 1;
    }

    bool has_hook(const char *name) const {
        return funcs.find(name) != funcs.end();
    }

    void run_top_level(ScriptHostCtx &ctx) {
        if (top_level_ran) return;
        top_level_ran = true;
        ExecState ex{*this, ctx};
        ex.exec_range(0, tokens.size() - 1, /*skip_funcs=*/true);
    }

    void call_hook(const char *name, ScriptHostCtx &ctx, const std::vector<Val> &args = {}) {
        if (!top_level_ran) run_top_level(ctx);
        const auto it = funcs.find(name);
        if (it == funcs.end()) return;
        ExecState ex{*this, ctx};
        ex.call_user_func(it->second, args);
    }

private:
    void index_functions() {
        const std::size_t n = tokens.size();
        for (std::size_t i = 0; i + 3 < n; ++i) {
            if (tokens[i].kind != TokKind::KwFunction || tokens[i + 1].kind != TokKind::Ident ||
                tokens[i + 2].kind != TokKind::LParen) {
                continue;
            }
            const std::string fn_name = tokens[i + 1].text;
            std::size_t p = i + 3;
            FunctionDef def{};
            while (p < n && tokens[p].kind != TokKind::RParen && tokens[p].kind != TokKind::Eof) {
                if (tokens[p].kind == TokKind::Ident) def.params.push_back(tokens[p].text);
                ++p;
            }
            if (p >= n || tokens[p].kind != TokKind::RParen) continue;
            ++p;  // past ')'
            def.body_start = p;
            int depth = 1;
            while (p < n && tokens[p].kind != TokKind::Eof) {
                const TokKind k = tokens[p].kind;
                if (k == TokKind::KwFunction || k == TokKind::KwIf || k == TokKind::KwFor || k == TokKind::KwWhile) {
                    ++depth;
                } else if (k == TokKind::KwEnd) {
                    if (--depth == 0) break;
                }
                ++p;
            }
            def.body_end = p;
            funcs[fn_name] = std::move(def);
            i = p;
        }
    }

    struct ExecState {
        LuaScript &script;
        ScriptHostCtx &ctx;
        ExecState(LuaScript &s, ScriptHostCtx &c) : script(s), ctx(c) {}
        std::size_t pos = 0;
        std::size_t end_pos = 0;
        std::vector<std::unordered_map<std::string, Val>> scopes;
        int steps = 0;
        static constexpr int kMaxSteps = 12000;
        bool returned = false;
        bool broke = false;
        Val ret_val = Val::nil();

        const Token &peek(std::size_t off = 0) const noexcept {
            const std::size_t idx = pos + off;
            return (idx < end_pos && idx < script.tokens.size()) ? script.tokens[idx] : script.tokens.back();
        }
        const Token &advance() noexcept {
            const Token &t = peek();
            if (pos < end_pos) ++pos;
            return t;
        }
        bool match(TokKind k) noexcept {
            if (peek().kind == k) { advance(); return true; }
            return false;
        }

        Val get_var(const std::string &name) const {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                const auto f = it->find(name);
                if (f != it->end()) return f->second;
            }
            const auto g = script.globals.find(name);
            if (g != script.globals.end()) return g->second;
            return Val::nil();
        }

        void set_var(const std::string &name, const Val &v, bool force_local) {
            if (force_local && !scopes.empty()) {
                scopes.back()[name] = v;
                return;
            }
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                const auto f = it->find(name);
                if (f != it->end()) { f->second = v; return; }
            }
            script.globals[name] = v;
        }

        std::uint32_t read_u32_mem(std::uint32_t addr) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return 0u;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            std::uint32_t v = 0;
            if (off + 4 <= ctx.ram_size) std::memcpy(&v, ctx.ram + off, 4);
            return v;
        }
        std::uint16_t read_u16_mem(std::uint32_t addr) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return 0u;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            std::uint16_t v = 0;
            if (off + 2 <= ctx.ram_size) std::memcpy(&v, ctx.ram + off, 2);
            return v;
        }
        std::uint8_t read_u8_mem(std::uint32_t addr) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return 0u;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            return (off < ctx.ram_size) ? ctx.ram[off] : 0u;
        }
        void write_u32_mem(std::uint32_t addr, std::uint32_t v) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            if (off + 4 <= ctx.ram_size) std::memcpy(ctx.ram + off, &v, 4);
        }
        void write_u16_mem(std::uint32_t addr, std::uint16_t v) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            if (off + 2 <= ctx.ram_size) std::memcpy(ctx.ram + off, &v, 2);
        }
        void write_u8_mem(std::uint32_t addr, std::uint8_t v) const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return;
            const std::size_t off = static_cast<std::size_t>(addr) & (ctx.ram_size - 1u);
            if (off < ctx.ram_size) ctx.ram[off] = v;
        }

        std::uint32_t next_rng() noexcept {
            if (!ctx.rng_state) return 1u;
            std::uint32_t x = *ctx.rng_state;
            if (x == 0u) x = 0x6D2B79F5u;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            *ctx.rng_state = x;
            return x;
        }

        // Semantic Game Detector (0=none/title, 1=SH1, 2=SH2, 3=SH3, 4=Koudelka PS1)
        int detect_active_game() const noexcept {
            if (!ctx.ram || ctx.ram_size == 0) return 0;
            if (ctx.ram_size >= 0x00500000u) {
                // SH1 (SLUS-20347): "Ch" at 0x004CAE40, Yuri joined & playtime >= 1s
                if (read_u8_mem(0x004CAE40u) == 0x43u && read_u8_mem(0x004CAE41u) == 0x68u &&
                    (read_u8_mem(0x00100280u) & 0x40u) != 0u && read_u32_mem(0x0010000Cu) >= 1000u) {
                    return 1;
                }
                // SH2: Covenant (SLUS-21041): "SSH_" at 0x00100000, playtime float >= 1.0
                if (read_u8_mem(0x00100000u) == 0x53u && read_u8_mem(0x00100001u) == 0x53u &&
                    read_u8_mem(0x00100002u) == 0x48u) {
                    const std::uint32_t raw = read_u32_mem(0x00102300u);
                    float pt = 0.0f; std::memcpy(&pt, &raw, 4);
                    if (pt >= 1.0f && pt < 1e8f) return 2;
                }
                // SH3: From the New World (SLUS-21326): "SH3C" at 0x0010001D, playtime >= 1s
                if (read_u8_mem(0x0010001Du) == 0x53u && read_u8_mem(0x0010001Eu) == 0x48u &&
                    read_u8_mem(0x0010001Fu) == 0x33u && read_u32_mem(0x00100098u) >= 1000u) {
                    return 3;
                }
            } else if (ctx.ram_size == 0x00200000u) {
                // Koudelka (PS1): party root pointer at 0x800B59A0 (offset 0x0B59A0)
                const std::uint32_t root = read_u32_mem(0x000B59A0u);
                if (root >= 0x80010000u && root < 0x801FF000u) {
                    const std::uint32_t p_kdl = read_u32_mem(root - 0x44u);
                    if (p_kdl >= 0x80010000u && p_kdl < 0x801FE000u && read_u8_mem(p_kdl + 0x146u) == 'K') {
                        return 4;
                    }
                }
            }
            return 0;
        }

        std::uint32_t sem_get_cash(int k) const noexcept {
            if (k == 1) return read_u32_mem(0x00100008u);
            if (k == 2) return read_u32_mem(0x00102D30u + static_cast<std::uint32_t>(read_u8_mem(0x00100132u) & 3u) * 4u);
            if (k == 3) return read_u32_mem(0x00100D54u);
            return 0u;
        }
        std::uint32_t sem_add_cash(int k, std::int64_t delta) const noexcept {
            if (k < 1 || k > 3) return 0u;
            const std::int64_t cur = static_cast<std::int64_t>(sem_get_cash(k));
            const std::uint32_t nxt = static_cast<std::uint32_t>(std::clamp<std::int64_t>(cur + delta, 0, 9999999));
            if (k == 1) write_u32_mem(0x00100008u, nxt);
            else if (k == 2) write_u32_mem(0x00102D30u + static_cast<std::uint32_t>(read_u8_mem(0x00100132u) & 3u) * 4u, nxt);
            else if (k == 3) write_u32_mem(0x00100D54u, nxt);
            return nxt;
        }
        std::uint32_t sem_get_battles(int k) const noexcept {
            if (k == 1) return read_u32_mem(0x001014A4u);
            if (k == 2) return read_u32_mem(0x001016E4u);
            if (k == 3) return read_u32_mem(0x00102734u);
            return 0u;
        }
        std::uint32_t sem_get_ring(int k, int which) const noexcept {
            // which: 0=attempts, 1=successes, 2=perfects
            if (k == 1) return read_u32_mem(which == 0 ? 0x001014ACu : (which == 1 ? 0x001014B0u : 0x001014B4u));
            if (k == 2) return read_u32_mem(which == 0 ? 0x001017C8u : (which == 1 ? 0x001017CCu : 0x001017D0u));
            if (k == 3) {
                const std::uint32_t off = (which == 0) ? 0x0Cu : (which == 1 ? 0x18u : 0x1Cu);
                std::uint32_t sum = 0;
                for (int i = 0; i < 7; ++i) sum += read_u32_mem(0x00102778u + static_cast<std::uint32_t>(i) * 0x90u + off);
                return sum;
            }
            return 0u;
        }
        int sem_get_max_level(int k) const noexcept {
            int ml = 0;
            if (k == 1) {
                for (int i = 0; i < 6; ++i) {
                    const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                    if ((read_u8_mem(b) & 0x40u) != 0u) ml = std::max(ml, static_cast<int>(read_u8_mem(b + 0x03u)));
                }
            } else if (k == 2) {
                for (int i = 0; i < 14; ++i) {
                    const std::uint32_t b = 0x00100250u + static_cast<std::uint32_t>(i) * 0x90u;
                    if (read_u8_mem(b + 0x16u) != 0u) ml = std::max(ml, static_cast<int>(read_u8_mem(b + 0x0Fu)));
                }
            } else if (k == 3) {
                for (int i = 0; i < 7; ++i) {
                    const std::uint32_t b = 0x00100D70u + static_cast<std::uint32_t>(i) * 0x84u;
                    if (read_u8_mem(b + 0x0Au) != 0u) ml = std::max(ml, static_cast<int>(read_u8_mem(b + 0x03u)));
                }
            } else if (k == 4) {
                const std::uint32_t root = read_u32_mem(0x000B59A0u);
                const std::uint32_t offs[3] = {0x44u, 0x48u, 0x4Cu};
                for (int i = 0; i < 3; ++i) {
                    const std::uint32_t p = read_u32_mem(root - offs[i]);
                    if (p >= 0x80010000u && p < 0x801FE000u && read_u8_mem(p + 0x4BAu) != 0u) {
                        ml = std::max(ml, static_cast<int>(read_u32_mem(p + 0x2D8u)));
                    }
                }
            }
            return ml;
        }
        int sem_heal_party(int k, int pct) const noexcept {
            if (pct <= 0) return 0;
            pct = std::clamp(pct, 1, 100);
            int healed = 0;
            if (k == 2) {
                for (int i = 0; i < 14; ++i) {
                    const std::uint32_t b = 0x00100250u + static_cast<std::uint32_t>(i) * 0x90u;
                    const std::uint16_t hm = read_u16_mem(b + 0x28u);
                    const std::uint16_t hc = read_u16_mem(b + 0x2Au);
                    if (read_u8_mem(b + 0x16u) != 0u && hm >= 10u && hc > 0u && hc < hm) {
                        const std::uint16_t add = static_cast<std::uint16_t>(std::max(1, static_cast<int>(hm) * pct / 100));
                        write_u16_mem(b + 0x2Au, static_cast<std::uint16_t>(std::min<int>(hm, hc + add)));
                        ++healed;
                    }
                }
            } else if (k == 3) {
                for (int i = 0; i < 7; ++i) {
                    const std::uint32_t b = 0x00100D70u + static_cast<std::uint32_t>(i) * 0x84u;
                    const std::uint16_t hc = read_u16_mem(b + 0x1Cu);
                    const std::uint16_t hm = read_u16_mem(b + 0x1Eu);
                    if (read_u8_mem(b + 0x0Au) != 0u && hm >= 10u && hc > 0u && hc < hm) {
                        const std::uint16_t add = static_cast<std::uint16_t>(std::max(1, static_cast<int>(hm) * pct / 100));
                        write_u16_mem(b + 0x1Cu, static_cast<std::uint16_t>(std::min<int>(hm, hc + add)));
                        ++healed;
                    }
                }
            } else if (k == 4) {
                const std::uint32_t root = read_u32_mem(0x000B59A0u);
                const std::uint32_t offs[3] = {0x44u, 0x48u, 0x4Cu};
                for (int i = 0; i < 3; ++i) {
                    const std::uint32_t p = read_u32_mem(root - offs[i]);
                    if (p >= 0x80010000u && p < 0x801FE000u && read_u8_mem(p + 0x4BAu) != 0u) {
                        const std::uint16_t hc = read_u16_mem(p + 0x410u);
                        const std::uint16_t hm = read_u16_mem(p + 0x412u);
                        if (hm >= 10u && hc > 0u && hc < hm) {
                            const std::uint16_t add = static_cast<std::uint16_t>(std::max(1, static_cast<int>(hm) * pct / 100));
                            write_u16_mem(p + 0x410u, static_cast<std::uint16_t>(std::min<int>(hm, hc + add)));
                            ++healed;
                        }
                    }
                }
            }
            return healed;
        }
        int sem_add_party_exp(int k, std::uint32_t amount) const noexcept {
            if (amount == 0u) return 0;
            int boosted = 0;
            if (k == 1) {
                for (int i = 0; i < 6; ++i) {
                    const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                    if ((read_u8_mem(b) & 0x40u) != 0u) {
                        write_u32_mem(b + 0x04u, read_u32_mem(b + 0x04u) + amount);
                        ++boosted;
                    }
                }
            } else if (k == 4) {
                const std::uint32_t root = read_u32_mem(0x000B59A0u);
                const std::uint32_t offs[3] = {0x44u, 0x48u, 0x4Cu};
                for (int i = 0; i < 3; ++i) {
                    const std::uint32_t p = read_u32_mem(root - offs[i]);
                    if (p >= 0x80010000u && p < 0x801FE000u && read_u8_mem(p + 0x4BAu) != 0u) {
                        write_u32_mem(p + 0x2DCu, read_u32_mem(p + 0x2DCu) + amount);
                        ++boosted;
                    }
                }
            }
            return boosted;
        }
        int sem_sync_bench_exp(int k) const noexcept {
            if (k != 1) return 0;
            std::uint32_t max_exp = 0u;
            std::uint8_t max_lv = 1u;
            for (int i = 0; i < 6; ++i) {
                const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                if ((read_u8_mem(b) & 0x40u) != 0u) {
                    max_exp = std::max(max_exp, read_u32_mem(b + 0x04u));
                    const std::uint8_t lv = read_u8_mem(b + 0x03u);
                    if (lv >= 1u && lv <= 99u) max_lv = std::max(max_lv, lv);
                }
            }
            int synced = 0;
            for (int i = 0; i < 6; ++i) {
                const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                if ((read_u8_mem(b) & 0x40u) != 0u) {
                    if (read_u32_mem(b + 0x04u) < max_exp) { write_u32_mem(b + 0x04u, max_exp); ++synced; }
                    if (read_u8_mem(b + 0x03u) < max_lv) write_u8_mem(b + 0x03u, max_lv);
                }
            }
            return synced;
        }
        int sem_unlock_all_party(int k) const noexcept {
            if (k != 1) return 0;
            const std::uint8_t ylv = read_u8_mem(0x00100280u + 0x03u);
            const std::uint32_t yexp = read_u32_mem(0x00100280u + 0x04u);
            if (ylv < 1u || ylv > 99u) return 0;
            int unlocked = 0;
            for (int i = 1; i < 6; ++i) {
                const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
                const std::uint8_t fl = read_u8_mem(b);
                if ((fl & 0x40u) == 0u) {
                    write_u8_mem(b, static_cast<std::uint8_t>(fl | 0x40u));
                    if (read_u8_mem(b + 0x03u) < ylv) write_u8_mem(b + 0x03u, ylv);
                    if (read_u32_mem(b + 0x04u) < yexp) write_u32_mem(b + 0x04u, yexp);
                    ++unlocked;
                }
            }
            return unlocked;
        }

        Val call_builtin_or_user(const std::string &name, const std::vector<Val> &args) {
            auto arg_u32 = [&](std::size_t i) -> std::uint32_t {
                return (i < args.size()) ? static_cast<std::uint32_t>(static_cast<std::int64_t>(args[i].num)) : 0u;
            };
            auto arg_num = [&](std::size_t i) -> double {
                return (i < args.size()) ? args[i].num : 0.0;
            };
            // High-Level Semantic Mod API (zero hex addresses required!)
            if (name == "is_in_game")         return Val::boolean(detect_active_game() != 0);
            if (name == "get_cash")           return Val::number(static_cast<double>(sem_get_cash(detect_active_game())));
            if (name == "add_cash")           return Val::number(static_cast<double>(sem_add_cash(detect_active_game(), static_cast<std::int64_t>(arg_num(0)))));
            if (name == "get_battles_won")    return Val::number(static_cast<double>(sem_get_battles(detect_active_game())));
            if (name == "get_ring_attempts")  return Val::number(static_cast<double>(sem_get_ring(detect_active_game(), 0)));
            if (name == "get_ring_successes") return Val::number(static_cast<double>(sem_get_ring(detect_active_game(), 1)));
            if (name == "get_ring_perfects")  return Val::number(static_cast<double>(sem_get_ring(detect_active_game(), 2)));
            if (name == "get_max_level")      return Val::number(static_cast<double>(sem_get_max_level(detect_active_game())));
            if (name == "heal_party")         return Val::number(static_cast<double>(sem_heal_party(detect_active_game(), static_cast<int>(arg_num(0)))));
            if (name == "add_party_exp")      return Val::number(static_cast<double>(sem_add_party_exp(detect_active_game(), arg_u32(0))));
            if (name == "sync_bench_exp")     return Val::number(static_cast<double>(sem_sync_bench_exp(detect_active_game())));
            if (name == "unlock_all_party")   return Val::number(static_cast<double>(sem_unlock_all_party(detect_active_game())));

            // Expanded Per-Hero Semantic Stat API (slot is 0-based)
            if (name == "get_hero_count") {
                const int g = detect_active_game();
                return Val::number(g == 1 ? 6.0 : (g == 2 ? 8.0 : (g == 3 ? 7.0 : (g == 4 ? 3.0 : 0.0))));
            }
            if (name == "get_hero_joined") {
                const int g = detect_active_game(), s = static_cast<int>(arg_num(0));
                if (g == 1 && s >= 0 && s < 6) return Val::boolean((read_u8_mem(0x00100280u + static_cast<std::uint32_t>(s) * 0x50u) & 0x40u) != 0u);
                if (g == 2 && s >= 0 && s < 8) return Val::boolean(read_u8_mem(0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x16u) != 0u);
                if (g == 3 && s >= 0 && s < 7) return Val::boolean(read_u8_mem(0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x0Au) != 0u);
                if (g == 4 && s >= 0 && s < 3) {
                    const std::uint32_t root = read_u32_mem(0x000B59A0u);
                    const std::uint32_t p = read_u32_mem(root - 0x44u - static_cast<std::uint32_t>(s) * 4u);
                    return Val::boolean(p >= 0x80010000u && p < 0x801FE000u && read_u8_mem(p + 0x4BAu) != 0u);
                }
                return Val::boolean(false);
            }
            if (name == "get_hero_level") {
                const int g = detect_active_game(), s = static_cast<int>(arg_num(0));
                if (g == 1 && s >= 0 && s < 6) return Val::number(read_u8_mem(0x00100280u + static_cast<std::uint32_t>(s) * 0x50u + 0x03u));
                if (g == 2 && s >= 0 && s < 8) return Val::number(read_u8_mem(0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x0Fu));
                if (g == 3 && s >= 0 && s < 7) return Val::number(read_u8_mem(0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x03u));
                if (g == 4 && s >= 0 && s < 3) {
                    const std::uint32_t p = read_u32_mem(read_u32_mem(0x000B59A0u) - 0x44u - static_cast<std::uint32_t>(s) * 4u);
                    if (p >= 0x80010000u && p < 0x801FE000u) return Val::number(read_u16_mem(p + 0x2D8u));
                }
                return Val::number(0.0);
            }
            if (name == "get_hero_hp" || name == "get_hero_max_hp" || name == "set_hero_hp" ||
                name == "get_hero_mp" || name == "get_hero_max_mp" || name == "set_hero_mp") {
                const int g = detect_active_game(), s = static_cast<int>(arg_num(0));
                const bool is_mp = (name.find("mp") != std::string::npos);
                const bool is_max = (name.find("max") != std::string::npos);
                const bool is_set = (name.rfind("set_", 0) == 0);
                std::uint32_t addr = 0u;
                if (g == 1 && s >= 0 && s < 6) {
                    const std::uint32_t b = 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u;
                    addr = b + (is_mp ? (is_max ? 0x16u : 0x12u) : (is_max ? 0x14u : 0x10u));
                } else if (g == 2 && s >= 0 && s < 8) {
                    const std::uint32_t b = 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u;
                    addr = b + (is_mp ? (is_max ? 0x2Cu : 0x2Eu) : (is_max ? 0x28u : 0x2Au));
                } else if (g == 3 && s >= 0 && s < 7) {
                    const std::uint32_t b = 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u;
                    addr = b + (is_mp ? (is_max ? 0x22u : 0x20u) : (is_max ? 0x1Eu : 0x1Cu));
                } else if (g == 4 && s >= 0 && s < 3) {
                    const std::uint32_t p = read_u32_mem(read_u32_mem(0x000B59A0u) - 0x44u - static_cast<std::uint32_t>(s) * 4u);
                    if (p >= 0x80010000u && p < 0x801FE000u) {
                        addr = p + (is_mp ? (is_max ? 0x416u : 0x414u) : (is_max ? 0x412u : 0x410u));
                    }
                }
                if (addr == 0u) return Val::number(0.0);
                if (is_set) {
                    const int v = std::clamp(static_cast<int>(arg_num(1)), 0, 9999);
                    write_u16_mem(addr, static_cast<std::uint16_t>(v));
                    return Val::number(static_cast<double>(v));
                }
                return Val::number(static_cast<double>(read_u16_mem(addr)));
            }
            if (name == "get_hero_exp" || name == "set_hero_exp") {
                const int g = detect_active_game(), s = static_cast<int>(arg_num(0));
                std::uint32_t addr = 0u;
                if (g == 1 && s >= 0 && s < 6) addr = 0x00100280u + static_cast<std::uint32_t>(s) * 0x50u + 0x04u;
                else if (g == 2 && s >= 0 && s < 8) addr = 0x00100250u + static_cast<std::uint32_t>(s) * 0x90u + 0x10u;
                else if (g == 3 && s >= 0 && s < 7) addr = 0x00100D70u + static_cast<std::uint32_t>(s) * 0x84u + 0x04u;
                else if (g == 4 && s >= 0 && s < 3) {
                    const std::uint32_t p = read_u32_mem(read_u32_mem(0x000B59A0u) - 0x44u - static_cast<std::uint32_t>(s) * 4u);
                    if (p >= 0x80010000u && p < 0x801FE000u) addr = p + 0x2DCu;
                }
                if (addr == 0u) return Val::number(0.0);
                if (name == "set_hero_exp") {
                    const std::uint32_t v = arg_u32(1);
                    write_u32_mem(addr, v);
                    return Val::number(static_cast<double>(v));
                }
                return Val::number(static_cast<double>(read_u32_mem(addr)));
            }

            // Per-Mod Persistent Save Data (mod_state.ini)
            if (name == "save_get") {
                if (args.empty()) return Val::nil();
                const std::string k = args[0].to_string();
                const auto it = script.save_store.find(k);
                if (it != script.save_store.end()) return it->second;
                return (args.size() > 1) ? args[1] : Val::nil();
            }
            if (name == "save_set") {
                if (args.size() >= 2) {
                    script.save_store[args[0].to_string()] = args[1];
                    script.flush_state_file();
                }
                return Val::nil();
            }
            if (name == "save_delete") {
                if (!args.empty()) {
                    script.save_store.erase(args[0].to_string());
                    script.flush_state_file();
                }
                return Val::nil();
            }

            // Configurable Mod Settings (mod.json "settings" + mod_config.ini)
            if (name == "get_setting") {
                if (args.empty()) return Val::number(0.0);
                const std::string k = args[0].to_string();
                const auto it = script.settings_store.find(k);
                if (it != script.settings_store.end()) return Val::number(it->second);
                return (args.size() > 1) ? args[1] : Val::number(0.0);
            }

            // Hotkey & Gamepad Input Queries
            if (name == "is_key_down")    return Val::boolean(!args.empty() && (ctx.keys_down & mod_input::key_mask_from_name(args[0].to_string())) != 0u);
            if (name == "is_key_pressed") return Val::boolean(!args.empty() && (ctx.keys_pressed & mod_input::key_mask_from_name(args[0].to_string())) != 0u);
            if (name == "is_pad_down")    return Val::boolean(!args.empty() && (ctx.pad_down & mod_input::pad_mask_from_name(args[0].to_string())) != 0u);
            if (name == "is_pad_pressed") return Val::boolean(!args.empty() && (ctx.pad_pressed & mod_input::pad_mask_from_name(args[0].to_string())) != 0u);

            // Custom Pinned Gothic HUD Widgets
            if (name == "hud_set") {
                if (ctx.hud_widgets && args.size() >= 3) {
                    const std::string id = script.mod_folder + ":" + args[0].to_string();
                    const std::string lbl = args[1].to_string();
                    const std::string val = args[2].to_string();
                    std::uint32_t col = 0xE6C88Au;
                    if (args.size() >= 4) {
                        if (args[3].is_str) {
                            std::string hs = args[3].str;
                            if (!hs.empty() && hs[0] == '#') hs.erase(0, 1);
                            col = static_cast<std::uint32_t>(std::strtoul(hs.c_str(), nullptr, 16)) & 0xFFFFFFu;
                        } else {
                            col = arg_u32(3) & 0xFFFFFFu;
                        }
                    }
                    bool updated = false;
                    for (auto &w : *ctx.hud_widgets) {
                        if (w.id == id) { w.label = lbl; w.value = val; w.color_rgb = col; updated = true; break; }
                    }
                    if (!updated && ctx.hud_widgets->size() < 6) {
                        ctx.hud_widgets->push_back(HudWidget{id, lbl, val, col});
                    }
                }
                return Val::nil();
            }
            if (name == "hud_clear") {
                if (ctx.hud_widgets && !args.empty()) {
                    const std::string id = script.mod_folder + ":" + args[0].to_string();
                    ctx.hud_widgets->erase(
                        std::remove_if(ctx.hud_widgets->begin(), ctx.hud_widgets->end(),
                                       [&](const HudWidget &w) { return w.id == id; }),
                        ctx.hud_widgets->end());
                }
                return Val::nil();
            }
            if (name == "hud_clear_all") {
                if (ctx.hud_widgets) {
                    const std::string pfx = script.mod_folder + ":";
                    ctx.hud_widgets->erase(
                        std::remove_if(ctx.hud_widgets->begin(), ctx.hud_widgets->end(),
                                       [&](const HudWidget &w) { return w.id.rfind(pfx, 0) == 0; }),
                        ctx.hud_widgets->end());
                }
                return Val::nil();
            }

            if (name == "read_u8")  return Val::number(static_cast<double>(read_u8_mem(arg_u32(0))));
            if (name == "read_u16") return Val::number(static_cast<double>(read_u16_mem(arg_u32(0))));
            if (name == "read_u32") return Val::number(static_cast<double>(read_u32_mem(arg_u32(0))));
            if (name == "read_s32") return Val::number(static_cast<double>(static_cast<std::int32_t>(read_u32_mem(arg_u32(0)))));
            if (name == "read_f32") {
                const std::uint32_t raw = read_u32_mem(arg_u32(0));
                float f = 0.0f; std::memcpy(&f, &raw, 4);
                return Val::number(static_cast<double>(f));
            }
            if (name == "write_u8")  { write_u8_mem(arg_u32(0), static_cast<std::uint8_t>(arg_u32(1) & 0xFFu)); return Val::nil(); }
            if (name == "write_u16") { write_u16_mem(arg_u32(0), static_cast<std::uint16_t>(arg_u32(1) & 0xFFFFu)); return Val::nil(); }
            if (name == "write_u32") { write_u32_mem(arg_u32(0), arg_u32(1)); return Val::nil(); }
            if (name == "write_f32") {
                const float f = static_cast<float>(arg_num(1));
                std::uint32_t raw = 0; std::memcpy(&raw, &f, 4);
                write_u32_mem(arg_u32(0), raw);
                return Val::nil();
            }
            if (name == "bit_and") return Val::number(static_cast<double>(arg_u32(0) & arg_u32(1)));
            if (name == "bit_or")  return Val::number(static_cast<double>(arg_u32(0) | arg_u32(1)));
            if (name == "bit_xor") return Val::number(static_cast<double>(arg_u32(0) ^ arg_u32(1)));
            if (name == "bit_shl") return Val::number(static_cast<double>(arg_u32(0) << (arg_u32(1) & 31u)));
            if (name == "bit_shr") return Val::number(static_cast<double>(arg_u32(0) >> (arg_u32(1) & 31u)));
            if (name == "min")     return Val::number(std::min(arg_num(0), arg_num(1)));
            if (name == "max")     return Val::number(std::max(arg_num(0), arg_num(1)));
            if (name == "clamp")   return Val::number(std::clamp(arg_num(0), arg_num(1), arg_num(2)));
            if (name == "floor")   return Val::number(std::floor(arg_num(0)));
            if (name == "ceil")    return Val::number(std::ceil(arg_num(0)));
            if (name == "abs")     return Val::number(std::fabs(arg_num(0)));
            if (name == "rand")    return Val::number(static_cast<double>(next_rng()) / 4294967296.0);
            if (name == "randint") {
                const std::int64_t lo = static_cast<std::int64_t>(arg_num(0));
                const std::int64_t hi = static_cast<std::int64_t>(arg_num(1));
                if (hi <= lo) return Val::number(static_cast<double>(lo));
                const std::uint64_t span = static_cast<std::uint64_t>(hi - lo + 1);
                return Val::number(static_cast<double>(lo + static_cast<std::int64_t>(next_rng() % span)));
            }
            if (name == "frame")     return Val::number(static_cast<double>(ctx.frame_index));
            if (name == "game_slug") return Val::string(ctx.game_slug);
            if (name == "tostring")  return Val::string(args.empty() ? std::string() : args[0].to_string());
            if (name == "tonumber")  return Val::number(args.empty() ? 0.0 : (args[0].is_str ? std::strtod(args[0].str.c_str(), nullptr) : args[0].num));
            if (name == "toast" || name == "notify") {
                if (ctx.toast_queue && !args.empty() && ctx.toast_queue->size() < 8) {
                    ctx.toast_queue->push_back(args[0].to_string());
                }
                return Val::nil();
            }
            if (name == "log" || name == "print") {
                if (!args.empty()) {
                    std::string msg;
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        if (i > 0) msg.push_back(' ');
                        msg += args[i].to_string();
                    }
                    const std::string line = "[" + script.mod_name + "] " + msg;
                    std::fprintf(stderr, "[mod:%s] %s\n", script.mod_name.c_str(), msg.c_str());
                    if (ctx.log_ring) {
                        ctx.log_ring->push_back(line);
                        if (ctx.log_ring->size() > 80) ctx.log_ring->pop_front();
                    }
                    if (!ctx.dev_log_path.empty()) {
                        std::ofstream lfs(ctx.dev_log_path, std::ios::app);
                        if (lfs) lfs << "[f" << ctx.frame_index << "] " << line << "\n";
                    }
                }
                return Val::nil();
            }
            const auto it = script.funcs.find(name);
            if (it != script.funcs.end() && scopes.size() < 16) {
                return call_user_func(it->second, args);
            }
            return Val::nil();
        }

        Val call_user_func(const FunctionDef &def, const std::vector<Val> &args) {
            const std::size_t saved_pos = pos;
            const std::size_t saved_end = end_pos;
            const bool saved_ret = returned;
            const bool saved_brk = broke;
            const Val saved_rv = ret_val;

            scopes.emplace_back();
            for (std::size_t i = 0; i < def.params.size(); ++i) {
                scopes.back()[def.params[i]] = (i < args.size()) ? args[i] : Val::nil();
            }
            returned = false;
            broke = false;
            ret_val = Val::nil();
            exec_range(def.body_start, def.body_end, /*skip_funcs=*/true);
            Val result = ret_val;
            scopes.pop_back();

            pos = saved_pos;
            end_pos = saved_end;
            returned = saved_ret;
            broke = saved_brk;
            ret_val = saved_rv;
            return result;
        }

        void skip_block() {
            int depth = 1;
            while (pos < end_pos && peek().kind != TokKind::Eof) {
                const TokKind k = advance().kind;
                if (k == TokKind::KwFunction || k == TokKind::KwIf || k == TokKind::KwFor || k == TokKind::KwWhile) ++depth;
                else if (k == TokKind::KwEnd && --depth == 0) break;
            }
        }

        void skip_until_branch_end() {
            int depth = 0;
            while (pos < end_pos && peek().kind != TokKind::Eof) {
                const TokKind k = peek().kind;
                if (k == TokKind::KwFunction || k == TokKind::KwIf || k == TokKind::KwFor || k == TokKind::KwWhile) {
                    ++depth; advance();
                } else if (k == TokKind::KwEnd) {
                    if (depth == 0) break;
                    --depth; advance();
                } else if (depth == 0 && (k == TokKind::KwElseIf || k == TokKind::KwElse)) {
                    break;
                } else {
                    advance();
                }
            }
        }

        Val parse_expr() { return parse_or(); }
        Val parse_or() {
            Val l = parse_and();
            while (match(TokKind::KwOr)) {
                Val r = parse_and();
                l = l.truthy() ? l : r;
            }
            return l;
        }
        Val parse_and() {
            Val l = parse_cmp();
            while (match(TokKind::KwAnd)) {
                Val r = parse_cmp();
                l = l.truthy() ? r : l;
            }
            return l;
        }
        Val parse_cmp() {
            Val l = parse_bit_or();
            while (true) {
                const TokKind k = peek().kind;
                if (k != TokKind::EqEq && k != TokKind::TildeEq && k != TokKind::Lt &&
                    k != TokKind::Le && k != TokKind::Gt && k != TokKind::Ge) break;
                advance();
                Val r = parse_bit_or();
                bool res = false;
                if (l.is_str && r.is_str) {
                    if (k == TokKind::EqEq) res = (l.str == r.str);
                    else if (k == TokKind::TildeEq) res = (l.str != r.str);
                    else if (k == TokKind::Lt) res = (l.str < r.str);
                    else if (k == TokKind::Le) res = (l.str <= r.str);
                    else if (k == TokKind::Gt) res = (l.str > r.str);
                    else if (k == TokKind::Ge) res = (l.str >= r.str);
                } else if (l.is_nil || r.is_nil) {
                    if (k == TokKind::EqEq) res = (l.is_nil && r.is_nil);
                    else if (k == TokKind::TildeEq) res = !(l.is_nil && r.is_nil);
                } else {
                    if (k == TokKind::EqEq) res = (l.num == r.num);
                    else if (k == TokKind::TildeEq) res = (l.num != r.num);
                    else if (k == TokKind::Lt) res = (l.num < r.num);
                    else if (k == TokKind::Le) res = (l.num <= r.num);
                    else if (k == TokKind::Gt) res = (l.num > r.num);
                    else if (k == TokKind::Ge) res = (l.num >= r.num);
                }
                l = Val::boolean(res);
            }
            return l;
        }
        Val parse_bit_or() {
            Val l = parse_bit_xor();
            while (match(TokKind::Pipe)) {
                Val r = parse_bit_xor();
                l = Val::number(static_cast<double>(static_cast<std::uint32_t>(static_cast<std::int64_t>(l.num)) |
                                                    static_cast<std::uint32_t>(static_cast<std::int64_t>(r.num))));
            }
            return l;
        }
        Val parse_bit_xor() {
            Val l = parse_bit_and();
            while (match(TokKind::Tilde)) {
                Val r = parse_bit_and();
                l = Val::number(static_cast<double>(static_cast<std::uint32_t>(static_cast<std::int64_t>(l.num)) ^
                                                    static_cast<std::uint32_t>(static_cast<std::int64_t>(r.num))));
            }
            return l;
        }
        Val parse_bit_and() {
            Val l = parse_shift();
            while (match(TokKind::Amp)) {
                Val r = parse_shift();
                l = Val::number(static_cast<double>(static_cast<std::uint32_t>(static_cast<std::int64_t>(l.num)) &
                                                    static_cast<std::uint32_t>(static_cast<std::int64_t>(r.num))));
            }
            return l;
        }
        Val parse_shift() {
            Val l = parse_concat();
            while (peek().kind == TokKind::Shl || peek().kind == TokKind::Shr) {
                const TokKind k = advance().kind;
                Val r = parse_concat();
                const std::uint32_t a = static_cast<std::uint32_t>(static_cast<std::int64_t>(l.num));
                const std::uint32_t b = static_cast<std::uint32_t>(static_cast<std::int64_t>(r.num)) & 31u;
                l = Val::number(static_cast<double>(k == TokKind::Shl ? (a << b) : (a >> b)));
            }
            return l;
        }
        Val parse_concat() {
            Val l = parse_add();
            while (match(TokKind::DotDot)) {
                Val r = parse_add();
                l = Val::string(l.to_string() + r.to_string());
            }
            return l;
        }
        Val parse_add() {
            Val l = parse_mul();
            while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
                const TokKind k = advance().kind;
                Val r = parse_mul();
                l = Val::number(k == TokKind::Plus ? (l.num + r.num) : (l.num - r.num));
            }
            return l;
        }
        Val parse_mul() {
            Val l = parse_unary();
            while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash ||
                   peek().kind == TokKind::SlashSlash || peek().kind == TokKind::Percent) {
                const TokKind k = advance().kind;
                Val r = parse_unary();
                if (k == TokKind::Star) l = Val::number(l.num * r.num);
                else if (k == TokKind::Slash) l = Val::number(r.num != 0.0 ? (l.num / r.num) : 0.0);
                else if (k == TokKind::SlashSlash) l = Val::number(r.num != 0.0 ? std::floor(l.num / r.num) : 0.0);
                else l = Val::number(r.num != 0.0 ? std::fmod(l.num, r.num) : 0.0);
            }
            return l;
        }
        Val parse_unary() {
            if (match(TokKind::Minus)) return Val::number(-parse_unary().num);
            if (match(TokKind::KwNot)) return Val::boolean(!parse_unary().truthy());
            return parse_primary();
        }
        Val parse_primary() {
            const Token &t = peek();
            if (t.kind == TokKind::Number) { advance(); return Val::number(t.num); }
            if (t.kind == TokKind::String) { advance(); return Val::string(t.text); }
            if (t.kind == TokKind::KwTrue) { advance(); return Val::boolean(true); }
            if (t.kind == TokKind::KwFalse) { advance(); return Val::boolean(false); }
            if (t.kind == TokKind::KwNil) { advance(); return Val::nil(); }
            if (match(TokKind::LParen)) {
                Val v = parse_expr();
                match(TokKind::RParen);
                return v;
            }
            if (t.kind == TokKind::Ident) {
                const std::string name = advance().text;
                if (match(TokKind::LParen)) {
                    std::vector<Val> args;
                    if (peek().kind != TokKind::RParen) {
                        do {
                            args.push_back(parse_expr());
                        } while (match(TokKind::Comma));
                    }
                    match(TokKind::RParen);
                    return call_builtin_or_user(name, args);
                }
                return get_var(name);
            }
            advance();
            return Val::nil();
        }

        void exec_stmt(bool skip_funcs) {
            if (++steps > kMaxSteps || returned || broke) return;
            const Token &t = peek();
            if (t.kind == TokKind::Semi) { advance(); return; }
            if (t.kind == TokKind::KwFunction && skip_funcs) {
                advance();
                skip_block();
                return;
            }
            if (match(TokKind::KwLocal)) {
                if (peek().kind == TokKind::Ident) {
                    const std::string name = advance().text;
                    Val v = Val::nil();
                    if (match(TokKind::Eq)) v = parse_expr();
                    set_var(name, v, /*force_local=*/!scopes.empty());
                }
                return;
            }
            if (match(TokKind::KwReturn)) {
                if (peek().kind != TokKind::KwEnd && peek().kind != TokKind::KwElse &&
                    peek().kind != TokKind::KwElseIf && peek().kind != TokKind::Eof && peek().kind != TokKind::Semi) {
                    ret_val = parse_expr();
                }
                returned = true;
                return;
            }
            if (match(TokKind::KwBreak)) {
                broke = true;
                return;
            }
            if (match(TokKind::KwIf)) {
                bool taken = false;
                while (pos < end_pos) {
                    const Val cond = parse_expr();
                    match(TokKind::KwThen);
                    if (!taken && cond.truthy()) {
                        taken = true;
                        while (pos < end_pos && !returned && !broke &&
                               peek().kind != TokKind::KwElseIf && peek().kind != TokKind::KwElse &&
                               peek().kind != TokKind::KwEnd && peek().kind != TokKind::Eof) {
                            exec_stmt(skip_funcs);
                        }
                    } else {
                        skip_until_branch_end();
                    }
                    if (match(TokKind::KwElseIf)) continue;
                    if (match(TokKind::KwElse)) {
                        if (!taken && !returned && !broke) {
                            while (pos < end_pos && !returned && !broke &&
                                   peek().kind != TokKind::KwEnd && peek().kind != TokKind::Eof) {
                                exec_stmt(skip_funcs);
                            }
                        } else {
                            skip_until_branch_end();
                        }
                    }
                    match(TokKind::KwEnd);
                    break;
                }
                return;
            }
            if (match(TokKind::KwFor)) {
                if (peek().kind != TokKind::Ident) { skip_block(); return; }
                const std::string var = advance().text;
                match(TokKind::Eq);
                const double start_v = parse_expr().num;
                match(TokKind::Comma);
                const double stop_v = parse_expr().num;
                double step_v = 1.0;
                if (match(TokKind::Comma)) step_v = parse_expr().num;
                match(TokKind::KwDo);
                const std::size_t body_start = pos;
                skip_block();
                const std::size_t after_end = pos;
                const std::size_t body_end = (after_end > body_start) ? (after_end - 1) : body_start;
                if (step_v != 0.0) {
                    scopes.emplace_back();
                    for (double cur = start_v;
                         (step_v > 0.0) ? (cur <= stop_v) : (cur >= stop_v);
                         cur += step_v) {
                        if (++steps > kMaxSteps || returned) break;
                        scopes.back()[var] = Val::number(cur);
                        broke = false;
                        exec_range(body_start, body_end, skip_funcs);
                        if (broke) { broke = false; break; }
                    }
                    scopes.pop_back();
                }
                pos = after_end;
                return;
            }
            if (match(TokKind::KwWhile)) {
                const std::size_t cond_start = pos;
                while (pos < end_pos && peek().kind != TokKind::KwDo && peek().kind != TokKind::Eof) advance();
                match(TokKind::KwDo);
                const std::size_t body_start = pos;
                skip_block();
                const std::size_t after_end = pos;
                const std::size_t body_end = (after_end > body_start) ? (after_end - 1) : body_start;
                while (++steps <= kMaxSteps && !returned) {
                    pos = cond_start;
                    const Val c = parse_expr();
                    if (!c.truthy()) break;
                    broke = false;
                    exec_range(body_start, body_end, skip_funcs);
                    if (broke) { broke = false; break; }
                }
                pos = after_end;
                return;
            }
            if (t.kind == TokKind::Ident && peek(1).kind == TokKind::Eq) {
                const std::string name = advance().text;
                advance();  // '='
                const Val v = parse_expr();
                set_var(name, v, /*force_local=*/false);
                return;
            }
            // Expression statement (e.g., function call)
            (void)parse_expr();
        }

        void exec_range(std::size_t start, std::size_t end, bool skip_funcs) {
            const std::size_t prev_end = end_pos;
            pos = start;
            end_pos = end;
            while (pos < end_pos && !returned && !broke && steps <= kMaxSteps && peek().kind != TokKind::Eof) {
                exec_stmt(skip_funcs);
            }
            end_pos = prev_end;
        }
    };
};

}  // namespace detail

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
        patches_.clear();
        patch_owners_.clear();
        conflicts_.clear();
        scripts_.clear();
        active_mod_names_.clear();
        toast_queue_.clear();
        hud_widgets_.clear();
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
    }

    void set_game_slug(std::string slug) { game_slug_ = std::move(slug); }
    void set_dev_mode(bool on) noexcept { dev_mode_ = on; }
    bool dev_mode() const noexcept { return dev_mode_; }
    void set_allow_native_plugins(bool allow) noexcept { allow_native_plugins_ = allow; }
    bool allow_native_plugins() const noexcept { return allow_native_plugins_; }
    bool wants_input() const noexcept { return has_on_hotkey_hook_ || has_on_frame_hook_; }

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
        const std::size_t q1 = text.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        const std::size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
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

    static std::vector<ModSettingDef> parse_mod_settings(const std::string &manifest_txt, const std::filesystem::path &mdir) {
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
                    sd.type = to_lower_str(json_field(obj, "type"));
                    if (sd.type.empty()) sd.type = "int";
                    sd.default_val = json_num_field(obj, "default", 0.0);
                    sd.min_val = json_num_field(obj, "min", 0.0);
                    sd.max_val = json_num_field(obj, "max", sd.type == "bool" ? 1.0 : 100.0);
                    sd.step = json_num_field(obj, "step", 1.0);
                    if (sd.step <= 0.0) sd.step = 1.0;
                    sd.current_val = sd.default_val;
                    if (!sd.key.empty()) out.push_back(std::move(sd));
                    obj_start = std::string::npos;
                }
            }
        }
        // Apply user overrides from mod_config.ini
        std::ifstream ifs(mdir / "mod_config.ini");
        if (ifs) {
            std::string line;
            while (std::getline(ifs, line)) {
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = trim_str(line.substr(0, eq));
                const std::string v = trim_str(line.substr(eq + 1));
                for (auto &sd : out) {
                    if (sd.key == k) {
                        sd.current_val = std::clamp(std::strtod(v.c_str(), nullptr), sd.min_val, sd.max_val);
                    }
                }
            }
        }
        return out;
    }

    static bool save_mod_settings(const std::filesystem::path &mdir, const std::vector<ModSettingDef> &settings) {
        std::ofstream ofs(mdir / "mod_config.ini", std::ios::trunc);
        if (!ofs) return false;
        for (const auto &sd : settings) {
            ofs << sd.key << " = " << sd.current_val << "\n";
        }
        return true;
    }

    static std::vector<InstalledMod> scan_installed_mods(const std::filesystem::path &game_dir,
                                                         const std::string &active_mods_spec) {
        std::vector<InstalledMod> out;
        std::error_code ec;
        const std::filesystem::path mods_root = game_dir / "mods";
        if (!std::filesystem::is_directory(mods_root, ec)) return out;

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
        for (const auto &entry : std::filesystem::directory_iterator(mods_root, ec)) {
            if (!entry.is_directory(ec)) continue;
            const std::filesystem::path mdir = entry.path();
            InstalledMod m{};
            m.folder = mdir.filename().string();
            m.name = m.folder;
            m.version = "1.0";
            m.author = "Community";
            const std::string key = to_lower_str(m.folder);
            const auto sit = spec_enabled.find(key);
            m.enabled = (sit == spec_enabled.end()) ? true : sit->second;

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
                    m.settings = parse_mod_settings(txt, mdir);
                }
            }
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
            discovered_keys.push_back(key);
            by_key[key] = std::move(m);
        }
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
        if (!std::filesystem::is_directory(mods_root, ec)) return;

        auto track_watch = [&](const std::filesystem::path &p) {
            std::error_code wec;
            if (std::filesystem::is_regular_file(p, wec)) {
                watch_files_.push_back({p, std::filesystem::last_write_time(p, wec)});
            }
        };

        const auto installed = scan_installed_mods(game_dir, active_mods_spec);
        for (const auto &m : installed) {
            if (!m.enabled) continue;
            const std::filesystem::path mdir = mods_root / m.folder;
            active_mod_names_.push_back(m.folder);
            track_watch(mdir / "mod.json");
            track_watch(mdir / "mod_config.ini");

            std::unordered_map<std::string, double> cfg_map;
            for (const auto &sd : m.settings) cfg_map[sd.key] = sd.current_val;

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
                    if (sc.load_source(m.name, sf.filename().string(), oss.str(), mdir, cfg_map)) {
                        if (sc.has_hook("on_frame")) has_on_frame_hook_ = true;
                        if (sc.has_hook("on_hotkey")) has_on_hotkey_hook_ = true;
                        if (sc.has_hook("on_game_ready") || sc.has_hook("on_battle_win") ||
                            sc.has_hook("on_ring_perfect") || sc.has_hook("on_ring_hit") ||
                            sc.has_hook("on_ring_miss")) {
                            has_semantic_hooks_ = true;
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
    const std::vector<std::filesystem::path> &texture_override_dirs() const noexcept { return texture_override_dirs_; }
    const std::vector<std::filesystem::path> &video_override_dirs() const noexcept { return video_override_dirs_; }
    const std::vector<ModConflict> &conflicts() const noexcept { return conflicts_; }
    const std::vector<HudWidget> &hud_widgets() const noexcept { return hud_widgets_; }
    const std::deque<std::string> &recent_logs() const noexcept { return log_ring_; }
    const std::vector<std::string> &active_mod_names() const noexcept { return active_mod_names_; }
    std::size_t loaded_plugin_count() const noexcept { return native_plugins_.size(); }
    std::size_t texture_override_count() const noexcept { return texture_overrides_.size(); }
    std::size_t video_override_count() const noexcept { return video_overrides_.size(); }

    void on_boot(std::uint8_t *ram, std::size_t ram_size) noexcept {
        apply_patches(ram, ram_size, /*boot_pass=*/true);
        run_script_event(ScriptHookEvent::OnBoot, ram, ram_size);
        dispatch_native_boot(ram, ram_size);
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
        if (dev_mode_ && (frame_counter_ % 30u) == 0u) {
            poll_hot_reload(ram, ram_size);
        }
        if (!patches_.empty()) apply_patches(ram, ram_size, /*boot_pass=*/false);
        if (has_on_hotkey_hook_ && (keys_pressed_ != 0u || pad_pressed_ != 0u)) {
            dispatch_hotkeys(ram, ram_size);
        }
        if (has_on_frame_hook_) {
            run_script_event(ScriptHookEvent::OnFrame, ram, ram_size);
        }
        if (!native_plugins_.empty()) dispatch_native_frame(ram, ram_size);
        if (has_semantic_hooks_ && ram && ram_size > 0 && (frame_counter_ % 6u) == 0u) {
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
            const std::uint32_t root = rd32(0x000B59A0u);
            if (root >= 0x80010000u && root < 0x801FF000u) {
                const std::uint32_t p = rd32(root - 0x44u);
                if (p >= 0x80010000u && p < 0x801FE000u && rd8(p + 0x146u) == 'K') kind = 4;
            }
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
            const std::uint32_t root = rd32(0x000B59A0u);
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t p = rd32(root - 0x44u - static_cast<std::uint32_t>(i) * 4u);
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
        for (const auto &sc : scripts_) total += sc.funcs.size();
        return total;
    }

    static NocturneModApiV1 make_c_api(const char *slug, std::uint8_t *ram, std::uint32_t ram_size) noexcept {
        NocturneModApiV1 api{};
        api.api_version = 1;
        api.game_slug = slug;
        api.guest_ram = ram;
        api.guest_ram_size = ram_size;
        api.read_u8 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint8_t {
            const std::uint32_t off = a & (sz - 1u);
            return (r && off < sz) ? r[off] : 0u;
        };
        api.read_u16 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint16_t {
            const std::uint32_t off = a & (sz - 1u);
            std::uint16_t v = 0;
            if (r && off + 2 <= sz) std::memcpy(&v, r + off, 2);
            return v;
        };
        api.read_u32 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a) -> std::uint32_t {
            const std::uint32_t off = a & (sz - 1u);
            std::uint32_t v = 0;
            if (r && off + 4 <= sz) std::memcpy(&v, r + off, 4);
            return v;
        };
        api.write_u8 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint8_t v) {
            const std::uint32_t off = a & (sz - 1u);
            if (r && off < sz) r[off] = v;
        };
        api.write_u16 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint16_t v) {
            const std::uint32_t off = a & (sz - 1u);
            if (r && off + 2 <= sz) std::memcpy(r + off, &v, 2);
        };
        api.write_u32 = [](std::uint8_t *r, std::uint32_t sz, std::uint32_t a, std::uint32_t v) {
            const std::uint32_t off = a & (sz - 1u);
            if (r && off + 4 <= sz) std::memcpy(r + off, &v, 4);
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
    std::vector<PnachPatch> patches_;
    std::unordered_map<std::uint32_t, std::string> patch_owners_;
    std::vector<ModConflict> conflicts_;
    std::vector<detail::LuaScript> scripts_;
    std::vector<LoadedPlugin> native_plugins_;
    std::vector<std::string> toast_queue_;
    std::vector<HudWidget> hud_widgets_;
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
        ctx.log_ring = &log_ring_;
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
            const std::uint32_t root = rd32(0x000B59A0u);
            if (root >= 0x80010000u && root < 0x801FF000u) {
                const std::uint32_t p = rd32(root - 0x44u);
                if (p >= 0x80010000u && p < 0x801FE000u && rd8(p + 0x146u) == 'K') kind = 4;
            }
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
            } else {
                ev_hit_streak_ += static_cast<int>(ds);
                if (dp >= da) {
                    ev_perf_streak_ += static_cast<int>(dp);
                    call_named_hook("on_ring_perfect", ram, ram_size, {detail::Val::number(static_cast<double>(ev_perf_streak_))});
                } else {
                    ev_perf_streak_ = 0;
                    call_named_hook("on_ring_hit", ram, ram_size, {detail::Val::number(static_cast<double>(ev_hit_streak_))});
                }
            }
        }

        if (battles > ev_prev_battles_) {
            ev_prev_battles_ = battles;
            call_named_hook("on_battle_win", ram, ram_size, {detail::Val::number(static_cast<double>(battles))});
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
            sc.call_hook(hname, ctx);
        }
    }
};

}  // namespace mod_engine
