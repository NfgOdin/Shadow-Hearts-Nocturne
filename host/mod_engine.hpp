#pragma once

// Shadow Hearts: Nocturne — 4-Layer Gameplay Mod Engine & C-API Plugin Foundation
// - Layer 1: Virtual Disc File Overrides (games/<slug>/mods/<mod>/files/<rel_path>)
// - Layer 2: Declarative .pnach Memory & Code Patches (patch=1,EE,<addr>,extended|word|short|byte,<val>)
// - Layer 3: Embedded Script Hooks (games/<slug>/mods/<mod>/scripts/*.lua: on_boot, on_frame, on_battle_start, on_turn_begin, on_ring_result, on_area_load)
// - Layer 4: Stable C-API v1 Hook Table (NocturneModApiV1) for native .dll plugins

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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
};

struct ScriptWriteOp {
    ScriptHookEvent event = ScriptHookEvent::OnFrame;
    std::uint32_t cond_addr = 0;      // 0 = unconditional
    std::uint32_t cond_eq_val = 0;
    PatchWidth cond_width = PatchWidth::Word;
    std::uint32_t write_addr = 0;
    std::uint32_t write_val = 0;
    PatchWidth write_width = PatchWidth::Word;
};

class Engine {
public:
    void clear() {
        file_overrides_.clear();
        patches_.clear();
        script_ops_.clear();
        active_mod_names_.clear();
    }

    // Load all enabled mods from games/<slug>/mods/ according to active_mods spec ("ModA,ModB,!DisabledMod")
    void load_from_game_dir(const std::filesystem::path &game_dir, const std::string &active_mods_spec, bool enabled = true) {
        clear();
        if (!enabled) return;

        std::error_code ec;
        const std::filesystem::path mods_root = game_dir / "mods";
        if (!std::filesystem::is_directory(mods_root, ec)) return;

        std::vector<std::string> ordered;
        std::vector<std::string> disabled;
        {
            std::size_t pos = 0;
            while (pos < active_mods_spec.size()) {
                const std::size_t c = active_mods_spec.find(',', pos);
                std::string tok = trim_str((c == std::string::npos) ? active_mods_spec.substr(pos) : active_mods_spec.substr(pos, c - pos));
                if (!tok.empty()) {
                    if (tok[0] == '!' && tok.size() > 1) disabled.push_back(tok.substr(1));
                    else ordered.push_back(tok);
                }
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        }

        std::vector<std::string> discovered;
        for (const auto &ent : std::filesystem::directory_iterator(mods_root, ec)) {
            if (!ent.is_directory(ec)) continue;
            const std::string name = ent.path().filename().string();
            const std::string low = to_lower_str(name);
            if (low == "textures" || low == "fmv") continue;
            discovered.push_back(name);
        }
        std::sort(discovered.begin(), discovered.end());
        for (const auto &d : discovered) {
            if (std::find(ordered.begin(), ordered.end(), d) == ordered.end() &&
                std::find(disabled.begin(), disabled.end(), d) == disabled.end()) {
                ordered.push_back(d);
            }
        }

        for (const auto &mod_name : ordered) {
            if (std::find(disabled.begin(), disabled.end(), mod_name) != disabled.end()) continue;
            const std::filesystem::path mod_dir = mods_root / mod_name;
            if (!std::filesystem::is_directory(mod_dir, ec)) continue;
            active_mod_names_.push_back(mod_name);

            // Layer 1: Virtual disc file overrides
            const std::filesystem::path files_dir = mod_dir / "files";
            if (std::filesystem::is_directory(files_dir, ec)) {
                for (const auto &fit : std::filesystem::recursive_directory_iterator(files_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                    if (!fit.is_regular_file(ec)) continue;
                    std::string rel = to_lower_str(std::filesystem::relative(fit.path(), files_dir, ec).generic_string());
                    if (file_overrides_.find(rel) == file_overrides_.end()) {
                        file_overrides_.emplace(std::move(rel), fit.path());
                    }
                }
            }

            // Layer 2 & Layer 3: .pnach patches and .lua hook scripts
            for (const auto &it : std::filesystem::recursive_directory_iterator(mod_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (!it.is_regular_file(ec)) continue;
                const std::string ext = to_lower_str(it.path().extension().string());
                if (ext == ".pnach") {
                    parse_pnach_file(it.path());
                } else if (ext == ".lua") {
                    parse_lua_script(it.path());
                }
            }
        }
    }

    // Layer 1: Resolve a virtual disc file path against active mod overrides
    std::filesystem::path resolve_file_override(const std::string &rel_disc_path) const {
        std::string norm = to_lower_str(rel_disc_path);
        for (char &c : norm) if (c == '\\') c = '/';
        while (!norm.empty() && norm.front() == '/') norm.erase(norm.begin());
        auto it = file_overrides_.find(norm);
        return (it != file_overrides_.end()) ? it->second : std::filesystem::path{};
    }

    // Layer 2 & 3: Apply boot/per-frame patches and event hooks to guest RAM
    void on_boot(std::uint8_t *ram, std::size_t ram_size) const noexcept {
        apply_patches(ram, ram_size, /*boot_pass=*/true);
        run_script_event(ScriptHookEvent::OnBoot, ram, ram_size);
    }

    void on_frame(std::uint8_t *ram, std::size_t ram_size) const noexcept {
        apply_patches(ram, ram_size, /*boot_pass=*/false);
        run_script_event(ScriptHookEvent::OnFrame, ram, ram_size);
    }

    void on_event(ScriptHookEvent ev, std::uint8_t *ram, std::size_t ram_size) const noexcept {
        run_script_event(ev, ram, ram_size);
    }

    std::size_t active_mod_count() const noexcept { return active_mod_names_.size(); }
    std::size_t file_override_count() const noexcept { return file_overrides_.size(); }
    std::size_t patch_count() const noexcept { return patches_.size(); }
    std::size_t script_op_count() const noexcept { return script_ops_.size(); }

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
    std::vector<std::string> active_mod_names_;
    std::unordered_map<std::string, std::filesystem::path> file_overrides_;
    std::vector<PnachPatch> patches_;
    std::vector<ScriptWriteOp> script_ops_;

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

    void parse_pnach_file(const std::filesystem::path &p) {
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
            patches_.push_back(pp);
        }
    }

    void parse_lua_script(const std::filesystem::path &p) {
        std::ifstream ifs(p);
        if (!ifs) return;
        ScriptHookEvent cur_ev = ScriptHookEvent::OnFrame;
        std::string line;
        while (std::getline(ifs, line)) {
            const std::size_t cmt = line.find("--");
            if (cmt != std::string::npos) line.resize(cmt);
            const std::string t = trim_str(line);
            if (t.empty()) continue;

            if (t.find("function") != std::string::npos) {
                if (t.find("on_boot") != std::string::npos) cur_ev = ScriptHookEvent::OnBoot;
                else if (t.find("on_frame") != std::string::npos) cur_ev = ScriptHookEvent::OnFrame;
                else if (t.find("on_battle_start") != std::string::npos) cur_ev = ScriptHookEvent::OnBattleStart;
                else if (t.find("on_turn_begin") != std::string::npos) cur_ev = ScriptHookEvent::OnTurnBegin;
                else if (t.find("on_ring_result") != std::string::npos) cur_ev = ScriptHookEvent::OnRingResult;
                else if (t.find("on_area_load") != std::string::npos) cur_ev = ScriptHookEvent::OnAreaLoad;
                continue;
            }

            // Support write_u8/write_u16/write_u32(addr, val) inside hook functions
            PatchWidth w = PatchWidth::Word;
            std::size_t wpos = std::string::npos;
            if ((wpos = t.find("write_u32(")) != std::string::npos) { w = PatchWidth::Word; wpos += 10; }
            else if ((wpos = t.find("write_u16(")) != std::string::npos) { w = PatchWidth::Short; wpos += 10; }
            else if ((wpos = t.find("write_u8(")) != std::string::npos) { w = PatchWidth::Byte; wpos += 9; }

            if (wpos != std::string::npos) {
                const std::size_t comma = t.find(',', wpos);
                const std::size_t rp = (comma != std::string::npos) ? t.find(')', comma + 1) : std::string::npos;
                if (comma != std::string::npos && rp != std::string::npos) {
                    ScriptWriteOp op{};
                    op.event = cur_ev;
                    op.write_width = w;
                    op.write_addr = static_cast<std::uint32_t>(std::strtoul(trim_str(t.substr(wpos, comma - wpos)).c_str(), nullptr, 0));
                    op.write_val = static_cast<std::uint32_t>(std::strtoul(trim_str(t.substr(comma + 1, rp - comma - 1)).c_str(), nullptr, 0));
                    script_ops_.push_back(op);
                }
            }
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

    void run_script_event(ScriptHookEvent ev, std::uint8_t *ram, std::size_t ram_size) const noexcept {
        for (const auto &op : script_ops_) {
            if (op.event != ev) continue;
            write_guest(ram, ram_size, op.write_addr, op.write_val, op.write_width);
        }
    }
};

}  // namespace mod_engine
