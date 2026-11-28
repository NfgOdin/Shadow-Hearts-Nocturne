#pragma once

// Shadow Hearts: Nocturne — Sandboxed Lua 5.4.7 VM for Layer 3 Mods
// Included by host/mod_engine.hpp when NOCTURNE_MOD_ENGINE_WITH_LUA54 is defined.
// Provides full standard Lua 5.4 (tables, closures, pairs/ipairs, string/math/table/utf8,
// mod-jailed require()) with strict sandboxing (no os/io/debug/loadlib) and deterministic
// per-hook instruction-cap termination so a runaway mod can never hang emulation.

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

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

namespace mod_engine::detail {

struct Lua54Script {
    std::string mod_name;
    std::string mod_folder;
    std::filesystem::path mod_dir;
    std::filesystem::path state_path;
    std::string file_name;
    bool coop_safe = false;

    lua_State *L = nullptr;
    ScriptHostCtx *active_ctx = nullptr;
    int scan_budget = 0;

    std::unordered_map<std::string, Val> save_store;
    std::unordered_map<std::string, double> settings_store;
    std::unordered_map<std::string, std::string> string_settings_store;
    std::unordered_map<std::string, bool> hook_cache;
    bool top_level_ran = false;

    static constexpr int kInstructionCap = 50000;

    Lua54Script() = default;
    ~Lua54Script() { close_vm(); }

    Lua54Script(const Lua54Script &) = delete;
    Lua54Script &operator=(const Lua54Script &) = delete;

    Lua54Script(Lua54Script &&o) noexcept { *this = std::move(o); }
    Lua54Script &operator=(Lua54Script &&o) noexcept {
        if (this == &o) return *this;
        close_vm();
        mod_name = std::move(o.mod_name);
        mod_folder = std::move(o.mod_folder);
        mod_dir = std::move(o.mod_dir);
        state_path = std::move(o.state_path);
        file_name = std::move(o.file_name);
        coop_safe = o.coop_safe;
        L = o.L;
        o.L = nullptr;
        active_ctx = nullptr;
        save_store = std::move(o.save_store);
        settings_store = std::move(o.settings_store);
        string_settings_store = std::move(o.string_settings_store);
        hook_cache = std::move(o.hook_cache);
        top_level_ran = o.top_level_ran;
        if (L) {
            lua_pushlightuserdata(L, this);
            lua_setfield(L, LUA_REGISTRYINDEX, "__nocturne_self");
        }
        return *this;
    }

    void close_vm() noexcept {
        if (L) {
            lua_close(L);
            L = nullptr;
        }
        hook_cache.clear();
        top_level_ran = false;
    }

    std::filesystem::path effective_state_path() const {
        if (!state_path.empty()) return state_path;
        if (!mod_dir.empty()) return mod_dir / "mod_state.ini";
        return {};
    }

    void load_state_file() {
        save_store.clear();
        const auto sp = effective_state_path();
        if (sp.empty()) return;
        std::ifstream ifs(sp);
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
        const auto sp = effective_state_path();
        if (sp.empty()) return;
        std::error_code ec;
        if (sp.has_parent_path()) std::filesystem::create_directories(sp.parent_path(), ec);
        std::ofstream ofs(sp, std::ios::trunc);
        if (!ofs) return;
        for (const auto &kv : save_store) {
            if (kv.second.is_nil) continue;
            if (kv.second.is_str) ofs << kv.first << "=s:" << kv.second.str << "\n";
            else ofs << kv.first << "=n:" << kv.second.to_string() << "\n";
        }
    }

    static Lua54Script *self_from(lua_State *ls) noexcept {
        lua_getfield(ls, LUA_REGISTRYINDEX, "__nocturne_self");
        auto *p = static_cast<Lua54Script *>(lua_touserdata(ls, -1));
        lua_pop(ls, 1);
        return p;
    }

    static void instruction_hook(lua_State *ls, lua_Debug *) {
        luaL_error(ls, "script instruction budget exceeded (%d ops)", kInstructionCap);
    }

    void emit_log(const std::string &msg) {
        const std::string line = "[" + mod_name + "] " + msg;
        std::fprintf(stderr, "[mod:%s] %s\n", mod_name.c_str(), msg.c_str());
        if (active_ctx) {
            if (active_ctx->log_ring) {
                active_ctx->log_ring->push_back(line);
                if (active_ctx->log_ring->size() > 80) active_ctx->log_ring->pop_front();
            }
            if (!active_ctx->dev_log_path.empty()) {
                std::ofstream lfs(active_ctx->dev_log_path, std::ios::app);
                if (lfs) lfs << "[f" << active_ctx->frame_index << "] " << line << "\n";
            }
        }
    }

    static std::string lua_val_to_string(lua_State *ls, int idx) {
        const int t = lua_type(ls, idx);
        if (t == LUA_TSTRING) {
            std::size_t len = 0;
            const char *s = lua_tolstring(ls, idx, &len);
            return s ? std::string(s, len) : std::string();
        }
        if (t == LUA_TBOOLEAN) return lua_toboolean(ls, idx) ? "true" : "false";
        if (t == LUA_TNIL || t == LUA_TNONE) return "nil";
        if (t == LUA_TNUMBER) {
            if (lua_isinteger(ls, idx)) return std::to_string(static_cast<long long>(lua_tointeger(ls, idx)));
            const double num = lua_tonumber(ls, idx);
            const double r = std::round(num);
            if (std::fabs(num - r) < 1e-9 && std::fabs(r) < 9e15) {
                return std::to_string(static_cast<std::int64_t>(r));
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4g", num);
            return buf;
        }
        return luaL_typename(ls, idx);
    }

    static std::uint32_t parse_hex_rgb(lua_State *ls, int idx, std::uint32_t def_rgb) {
        if (lua_gettop(ls) < idx || lua_isnil(ls, idx)) return def_rgb;
        if (lua_type(ls, idx) == LUA_TSTRING) {
            std::string hs = lua_tostring(ls, idx);
            if (!hs.empty() && hs[0] == '#') hs.erase(0, 1);
            if (hs.rfind("0x", 0) == 0 || hs.rfind("0X", 0) == 0) hs.erase(0, 2);
            return static_cast<std::uint32_t>(std::strtoul(hs.c_str(), nullptr, 16)) & 0xFFFFFFu;
        }
        return static_cast<std::uint32_t>(luaL_optinteger(ls, idx, static_cast<lua_Integer>(def_rgb))) & 0xFFFFFFu;
    }

    // ---- Sandboxed mod-jailed require() ----
    static int l_require(lua_State *ls) {
        auto *self = self_from(ls);
        const char *name = luaL_checkstring(ls, 1);
        if (!self || self->mod_dir.empty() || !name || !*name) {
            return luaL_error(ls, "require() unavailable for module '%s'", name ? name : "");
        }
        // Check module cache first
        lua_getfield(ls, LUA_REGISTRYINDEX, "__nocturne_loaded");
        if (lua_istable(ls, -1)) {
            lua_getfield(ls, -1, name);
            if (!lua_isnil(ls, -1)) return 1;
            lua_pop(ls, 1);
        }
        lua_pop(ls, 1);

        std::string rel(name);
        if (rel.find("..") != std::string::npos || rel.find(':') != std::string::npos ||
            rel[0] == '/' || rel[0] == '\\') {
            return luaL_error(ls, "require('%s'): path escape blocked by sandbox", name);
        }
        // Strip optional trailing ".lua" before mapping module dots to path separators
        if (rel.size() >= 4 && rel.compare(rel.size() - 4, 4, ".lua") == 0) {
            rel.resize(rel.size() - 4);
        }
        for (char &c : rel) {
            if (c == '.') c = '/';
        }
        rel += ".lua";

        std::error_code ec;
        const auto root = std::filesystem::weakly_canonical(self->mod_dir, ec);
        auto resolve_inside = [&](const std::filesystem::path &cand) -> std::filesystem::path {
            if (!std::filesystem::is_regular_file(cand, ec)) return {};
            const auto canon = std::filesystem::weakly_canonical(cand, ec);
            if (ec || root.empty()) return {};
            const auto rs = root.wstring();
            const auto cs = canon.wstring();
            if (cs.size() > rs.size() && cs.compare(0, rs.size(), rs) == 0 &&
                (cs[rs.size()] == L'\\' || cs[rs.size()] == L'/')) {
                return canon;
            }
            return {};
        };
        auto target = resolve_inside(self->mod_dir / "scripts" / rel);
        if (target.empty()) target = resolve_inside(self->mod_dir / rel);
        if (target.empty()) {
            return luaL_error(ls, "require('%s'): module not found inside mod folder", name);
        }
        std::ifstream ifs(target);
        if (!ifs) return luaL_error(ls, "require('%s'): failed to open file", name);
        std::ostringstream oss;
        oss << ifs.rdbuf();
        const std::string src = oss.str();
        const std::string chunk_name = "@" + target.filename().string();
        if (luaL_loadbufferx(ls, src.data(), src.size(), chunk_name.c_str(), "t") != LUA_OK) {
            return lua_error(ls);
        }
        lua_call(ls, 0, 1);
        if (lua_isnil(ls, -1)) {
            lua_pop(ls, 1);
            lua_pushboolean(ls, 1);
        }
        lua_getfield(ls, LUA_REGISTRYINDEX, "__nocturne_loaded");
        if (lua_istable(ls, -1)) {
            lua_pushvalue(ls, -2);
            lua_setfield(ls, -2, name);
        }
        lua_pop(ls, 1);
        return 1;
    }

    // ---- Semantic RPG & Host API bindings ----
    static int l_is_in_game(lua_State *ls) {
        auto *s = self_from(ls);
        lua_pushboolean(ls, (s && s->active_ctx && SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size) != 0) ? 1 : 0);
        return 1;
    }
    static int l_get_cash(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::get_cash(s->active_ctx->ram, s->active_ctx->ram_size, g)));
        return 1;
    }
    static int l_add_cash(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const auto delta = static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0));
        lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::add_cash(s->active_ctx->ram, s->active_ctx->ram_size, g, delta)));
        return 1;
    }
    static int l_get_battles_won(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::get_battles(s->active_ctx->ram, s->active_ctx->ram_size, g)));
        return 1;
    }
    static int l_get_ring(lua_State *ls) {
        auto *s = self_from(ls);
        const int which = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::get_ring(s->active_ctx->ram, s->active_ctx->ram_size, g, which)));
        return 1;
    }
    static int l_get_max_level(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, SemanticRam::get_max_level(s->active_ctx->ram, s->active_ctx->ram_size, g));
        return 1;
    }
    static int l_heal_party(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int pct = static_cast<int>(luaL_optnumber(ls, 1, 100.0));
        lua_pushinteger(ls, SemanticRam::heal_party(s->active_ctx->ram, s->active_ctx->ram_size, g, pct));
        return 1;
    }
    static int l_restore_party_mp(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int pct = static_cast<int>(luaL_optnumber(ls, 1, 100.0));
        lua_pushinteger(ls, SemanticRam::restore_party_mp(s->active_ctx->ram, s->active_ctx->ram_size, g, pct));
        return 1;
    }
    static int l_add_party_exp(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const auto amt = static_cast<std::uint32_t>(std::max<double>(0.0, luaL_optnumber(ls, 1, 0.0)));
        lua_pushinteger(ls, SemanticRam::add_party_exp(s->active_ctx->ram, s->active_ctx->ram_size, g, amt));
        return 1;
    }
    static int l_sync_bench_exp(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, SemanticRam::sync_bench_exp(s->active_ctx->ram, s->active_ctx->ram_size, g));
        return 1;
    }
    static int l_unlock_all_party(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, SemanticRam::unlock_all_party(s->active_ctx->ram, s->active_ctx->ram_size, g));
        return 1;
    }
    static int l_get_hero_count(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        lua_pushinteger(ls, SemanticRam::hero_count(g));
        return 1;
    }
    static int l_get_hero_joined(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushboolean(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int slot = static_cast<int>(luaL_optnumber(ls, 1, 0.0));
        lua_pushboolean(ls, SemanticRam::hero_joined(s->active_ctx->ram, s->active_ctx->ram_size, g, slot) ? 1 : 0);
        return 1;
    }
    static int l_get_hero_name(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushstring(ls, ""); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int slot = static_cast<int>(luaL_optnumber(ls, 1, 0.0));
        const std::string nm = SemanticRam::hero_name(s->active_ctx->ram, s->active_ctx->ram_size, g, slot);
        lua_pushlstring(ls, nm.data(), nm.size());
        return 1;
    }
    static int l_get_hero_level(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int slot = static_cast<int>(luaL_optnumber(ls, 1, 0.0));
        lua_pushinteger(ls, SemanticRam::hero_level(s->active_ctx->ram, s->active_ctx->ram_size, g, slot));
        return 1;
    }
    static int l_hero_stat(lua_State *ls) {
        // upvalue 1: 0=get_hp, 1=get_max_hp, 2=set_hp, 3=get_mp, 4=get_max_mp, 5=set_mp, 6=get_exp, 7=set_exp
        auto *s = self_from(ls);
        const int op = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx) { lua_pushinteger(ls, 0); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int slot = static_cast<int>(luaL_optnumber(ls, 1, 0.0));
        if (op == 6) {
            lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::hero_exp(s->active_ctx->ram, s->active_ctx->ram_size, g, slot)));
            return 1;
        }
        if (op == 7) {
            const auto v = static_cast<std::uint32_t>(std::max<double>(0.0, luaL_optnumber(ls, 2, 0.0)));
            lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::set_hero_exp(s->active_ctx->ram, s->active_ctx->ram_size, g, slot, v)));
            return 1;
        }
        const bool is_mp = (op >= 3);
        const bool is_max = (op == 1 || op == 4);
        const bool is_set = (op == 2 || op == 5);
        if (is_set) {
            const int v = static_cast<int>(luaL_optnumber(ls, 2, 0.0));
            lua_pushinteger(ls, SemanticRam::set_hero_vital(s->active_ctx->ram, s->active_ctx->ram_size, g, slot, is_mp, v));
        } else {
            lua_pushinteger(ls, SemanticRam::hero_vital(s->active_ctx->ram, s->active_ctx->ram_size, g, slot, is_mp, is_max));
        }
        return 1;
    }
    static int l_get_hero_weapon_durability(lua_State *ls) {
        auto *s = self_from(ls);
        const bool is_max = (lua_tointeger(ls, lua_upvalueindex(1)) != 0);
        if (!s || !s->active_ctx) { lua_pushinteger(ls, -1); return 1; }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const int slot = static_cast<int>(luaL_optnumber(ls, 1, 0.0));
        lua_pushinteger(ls, SemanticRam::hero_weapon_durability(s->active_ctx->ram, s->active_ctx->ram_size, g, slot, is_max));
        return 1;
    }
    static int l_get_selected_weapon(lua_State *ls) {
        auto *s = self_from(ls);
        const int op = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx) {
            if (op == 2) lua_pushliteral(ls, "");
            else lua_pushinteger(ls, -1);
            return 1;
        }
        const int g = SemanticRam::detect_active_game(s->active_ctx->ram, s->active_ctx->ram_size);
        const auto info = SemanticRam::selected_weapon_info(s->active_ctx->ram, s->active_ctx->ram_size, g);
        if (op == 2) {
            if (info.valid) lua_pushlstring(ls, info.name.data(), info.name.size());
            else lua_pushliteral(ls, "");
        } else if (op == 1) {
            lua_pushinteger(ls, info.valid ? info.maxd : -1);
        } else {
            lua_pushinteger(ls, info.valid ? info.cur : -1);
        }
        return 1;
    }

    static void push_smart_number(lua_State *ls, double v) {
        const double r = std::round(v);
        if (std::fabs(v - r) < 1e-9 && std::fabs(r) < 9e15) {
            lua_pushinteger(ls, static_cast<lua_Integer>(r));
        } else {
            lua_pushnumber(ls, v);
        }
    }

    // ---- Persistent Per-Mod Save Data ----
    static int l_save_get(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || lua_gettop(ls) < 1) { lua_pushnil(ls); return 1; }
        const std::string k = lua_val_to_string(ls, 1);
        const auto it = s->save_store.find(k);
        if (it != s->save_store.end()) {
            if (it->second.is_nil) lua_pushnil(ls);
            else if (it->second.is_str) lua_pushlstring(ls, it->second.str.data(), it->second.str.size());
            else push_smart_number(ls, it->second.num);
            return 1;
        }
        if (lua_gettop(ls) >= 2) lua_pushvalue(ls, 2);
        else lua_pushnil(ls);
        return 1;
    }
    static int l_save_set(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && lua_gettop(ls) >= 2) {
            const std::string k = lua_val_to_string(ls, 1);
            const int t = lua_type(ls, 2);
            if (t == LUA_TNIL) s->save_store.erase(k);
            else if (t == LUA_TSTRING) s->save_store[k] = Val::string(lua_val_to_string(ls, 2));
            else if (t == LUA_TBOOLEAN) s->save_store[k] = Val::number(lua_toboolean(ls, 2) ? 1.0 : 0.0);
            else s->save_store[k] = Val::number(lua_tonumber(ls, 2));
            s->flush_state_file();
        }
        return 0;
    }
    static int l_save_delete(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && lua_gettop(ls) >= 1) {
            s->save_store.erase(lua_val_to_string(ls, 1));
            s->flush_state_file();
        }
        return 0;
    }

    // ---- Configurable Mod Settings ----
    static int l_get_setting(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || lua_gettop(ls) < 1) { lua_pushinteger(ls, 0); return 1; }
        const std::string k = lua_val_to_string(ls, 1);
        const auto it = s->settings_store.find(k);
        if (it != s->settings_store.end()) { push_smart_number(ls, it->second); return 1; }
        if (lua_gettop(ls) >= 2) lua_pushvalue(ls, 2);
        else lua_pushinteger(ls, 0);
        return 1;
    }
    static int l_get_setting_str(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || lua_gettop(ls) < 1) { lua_pushstring(ls, ""); return 1; }
        const std::string k = lua_val_to_string(ls, 1);
        const auto sit = s->string_settings_store.find(k);
        if (sit != s->string_settings_store.end()) {
            lua_pushlstring(ls, sit->second.data(), sit->second.size());
            return 1;
        }
        const auto nit = s->settings_store.find(k);
        if (nit != s->settings_store.end()) {
            const std::string vs = Val::number(nit->second).to_string();
            lua_pushlstring(ls, vs.data(), vs.size());
            return 1;
        }
        if (lua_gettop(ls) >= 2) {
            const std::string ds = lua_val_to_string(ls, 2);
            lua_pushlstring(ls, ds.data(), ds.size());
        } else {
            lua_pushstring(ls, "");
        }
        return 1;
    }

    // ---- Input Queries & Injection ----
    static int l_input_query(lua_State *ls) {
        auto *s = self_from(ls);
        const int mode = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx || lua_gettop(ls) < 1) { lua_pushboolean(ls, 0); return 1; }
        const std::string nm = lua_val_to_string(ls, 1);
        bool hit = false;
        if (mode == 0) hit = (s->active_ctx->keys_down & mod_input::key_mask_from_name(nm)) != 0u;
        else if (mode == 1) hit = (s->active_ctx->keys_pressed & mod_input::key_mask_from_name(nm)) != 0u;
        else if (mode == 2) hit = (s->active_ctx->pad_down & mod_input::pad_mask_from_name(nm)) != 0u;
        else if (mode == 3) hit = (s->active_ctx->pad_pressed & mod_input::pad_mask_from_name(nm)) != 0u;
        lua_pushboolean(ls, hit ? 1 : 0);
        return 1;
    }
    static int l_pad_mut(lua_State *ls) {
        auto *s = self_from(ls);
        const bool is_block = (lua_tointeger(ls, lua_upvalueindex(1)) != 0);
        if (s && s->active_ctx && lua_gettop(ls) >= 1) {
            const std::uint32_t m = mod_input::pad_mask_from_name(lua_val_to_string(ls, 1));
            if (is_block && s->active_ctx->pad_block) *s->active_ctx->pad_block |= m;
            if (!is_block && s->active_ctx->pad_inject) *s->active_ctx->pad_inject |= m;
        }
        return 0;
    }

    // ---- Gothic HUD Pills & 2D Canvas ----
    static int l_hud_set(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->hud_widgets && lua_gettop(ls) >= 3) {
            const std::string id = s->mod_folder + ":" + lua_val_to_string(ls, 1);
            const std::string lbl = lua_val_to_string(ls, 2);
            const std::string val = lua_val_to_string(ls, 3);
            const std::uint32_t col = parse_hex_rgb(ls, 4, 0xE6C88Au);
            bool updated = false;
            for (auto &w : *s->active_ctx->hud_widgets) {
                if (w.id == id) { w.label = lbl; w.value = val; w.color_rgb = col; updated = true; break; }
            }
            if (!updated && s->active_ctx->hud_widgets->size() < 8) {
                s->active_ctx->hud_widgets->push_back(HudWidget{id, lbl, val, col});
            }
        }
        return 0;
    }
    static int l_hud_clear(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->hud_widgets && lua_gettop(ls) >= 1) {
            const std::string id = s->mod_folder + ":" + lua_val_to_string(ls, 1);
            auto &v = *s->active_ctx->hud_widgets;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const HudWidget &w) { return w.id == id; }), v.end());
        }
        return 0;
    }
    static int l_hud_clear_all(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->hud_widgets) {
            const std::string pfx = s->mod_folder + ":";
            auto &v = *s->active_ctx->hud_widgets;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const HudWidget &w) { return w.id.rfind(pfx, 0) == 0; }), v.end());
        }
        return 0;
    }
    static int l_set_item_glow(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx) {
            if (s->active_ctx->item_glow_strength && lua_gettop(ls) >= 1) {
                *s->active_ctx->item_glow_strength = std::clamp(static_cast<float>(luaL_optnumber(ls, 1, 0.0)), 0.0f, 1.5f);
            }
            if (s->active_ctx->item_glow_rgb && lua_gettop(ls) >= 2) {
                *s->active_ctx->item_glow_rgb = parse_hex_rgb(ls, 2, 0xFFCC44u);
            }
            if (s->active_ctx->item_glow_pulse_speed && lua_gettop(ls) >= 3) {
                *s->active_ctx->item_glow_pulse_speed = std::clamp(static_cast<float>(luaL_optnumber(ls, 3, 1.0)), 0.0f, 3.0f);
            }
        }
        return 0;
    }
    static int l_draw_clear(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->canvas_cmds) s->active_ctx->canvas_cmds->clear();
        return 0;
    }
    static int l_draw_rect(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->canvas_cmds && s->active_ctx->canvas_cmds->size() < 96 && lua_gettop(ls) >= 4) {
            CanvasCmd c{};
            c.kind = CanvasCmd::Kind::Rect;
            c.x = static_cast<float>(luaL_optnumber(ls, 1, 0.0));
            c.y = static_cast<float>(luaL_optnumber(ls, 2, 0.0));
            c.w = static_cast<float>(luaL_optnumber(ls, 3, 0.0));
            c.h = static_cast<float>(luaL_optnumber(ls, 4, 0.0));
            c.color_rgb = parse_hex_rgb(ls, 5, 0x14101Cu);
            c.alpha = (lua_gettop(ls) >= 6) ? std::clamp(static_cast<int>(luaL_optnumber(ls, 6, 210.0)), 0, 255) : 210;
            s->active_ctx->canvas_cmds->push_back(std::move(c));
        }
        return 0;
    }
    static int l_draw_bar(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->canvas_cmds && s->active_ctx->canvas_cmds->size() < 96 && lua_gettop(ls) >= 5) {
            CanvasCmd c{};
            c.kind = CanvasCmd::Kind::Bar;
            c.x = static_cast<float>(luaL_optnumber(ls, 1, 0.0));
            c.y = static_cast<float>(luaL_optnumber(ls, 2, 0.0));
            c.w = static_cast<float>(luaL_optnumber(ls, 3, 0.0));
            c.h = static_cast<float>(luaL_optnumber(ls, 4, 0.0));
            c.ratio = std::clamp(static_cast<float>(luaL_optnumber(ls, 5, 0.0)), 0.0f, 1.0f);
            c.color_rgb = parse_hex_rgb(ls, 6, 0xD84848u);
            c.bg_rgb = parse_hex_rgb(ls, 7, 0x16121Cu);
            if (lua_gettop(ls) >= 8 && !lua_isnil(ls, 8)) c.text = lua_val_to_string(ls, 8);
            c.alpha = 235;
            s->active_ctx->canvas_cmds->push_back(std::move(c));
        }
        return 0;
    }
    static int l_draw_text(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->canvas_cmds && s->active_ctx->canvas_cmds->size() < 96 && lua_gettop(ls) >= 3) {
            CanvasCmd c{};
            c.kind = CanvasCmd::Kind::Text;
            c.x = static_cast<float>(luaL_optnumber(ls, 1, 0.0));
            c.y = static_cast<float>(luaL_optnumber(ls, 2, 0.0));
            c.text = lua_val_to_string(ls, 3);
            c.font_size = (lua_gettop(ls) >= 4) ? std::clamp(static_cast<int>(luaL_optnumber(ls, 4, 14.0)), 8, 48) : 14;
            c.color_rgb = parse_hex_rgb(ls, 5, 0xF0E6D2u);
            c.alpha = (lua_gettop(ls) >= 6) ? std::clamp(static_cast<int>(luaL_optnumber(ls, 6, 255.0)), 0, 255) : 255;
            s->active_ctx->canvas_cmds->push_back(std::move(c));
        }
        return 0;
    }

    // ---- Custom WAV SFX (path-jailed) ----
    static int l_play_sfx(lua_State *ls) {
#ifdef _WIN32
        auto *s = self_from(ls);
        if (s && lua_gettop(ls) >= 1 && !s->mod_dir.empty()) {
            const std::string rel = lua_val_to_string(ls, 1);
            const std::filesystem::path rel_p(rel);
            if (!rel.empty() && !rel_p.is_absolute() && rel.find("..") == std::string::npos) {
                std::error_code ec;
                const auto root = std::filesystem::weakly_canonical(s->mod_dir, ec);
                auto resolve_inside = [&](const std::filesystem::path &cand) -> std::filesystem::path {
                    if (!std::filesystem::is_regular_file(cand, ec)) return {};
                    const auto canon = std::filesystem::weakly_canonical(cand, ec);
                    if (ec || root.empty()) return {};
                    const auto rs = root.wstring();
                    const auto cs = canon.wstring();
                    if (cs.size() > rs.size() && cs.compare(0, rs.size(), rs) == 0 &&
                        (cs[rs.size()] == L'\\' || cs[rs.size()] == L'/')) {
                        return canon;
                    }
                    return {};
                };
                auto p = resolve_inside(s->mod_dir / "audio" / rel_p);
                if (p.empty()) p = resolve_inside(s->mod_dir / rel_p);
                if (!p.empty()) {
                    PlaySoundW(p.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
                    lua_pushboolean(ls, 1);
                    return 1;
                }
            }
        }
#endif
        lua_pushboolean(ls, 0);
        return 1;
    }

    // ---- Raw RAM Read/Write & AOB Scanner ----
    static int l_ram_read(lua_State *ls) {
        auto *s = self_from(ls);
        const int kind = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx || !s->active_ctx->ram || s->active_ctx->ram_size == 0) {
            lua_pushnumber(ls, 0.0);
            return 1;
        }
        const auto addr = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0)));
        const auto *r = s->active_ctx->ram;
        const auto sz = s->active_ctx->ram_size;
        if (kind == 0) lua_pushinteger(ls, SemanticRam::rd8(r, sz, addr));
        else if (kind == 1) lua_pushinteger(ls, SemanticRam::rd16(r, sz, addr));
        else if (kind == 2) lua_pushinteger(ls, static_cast<lua_Integer>(SemanticRam::rd32(r, sz, addr)));
        else if (kind == 3) lua_pushinteger(ls, static_cast<std::int32_t>(SemanticRam::rd32(r, sz, addr)));
        else {
            const std::uint32_t raw = SemanticRam::rd32(r, sz, addr);
            float f = 0.0f;
            std::memcpy(&f, &raw, 4);
            lua_pushnumber(ls, static_cast<double>(f));
        }
        return 1;
    }
    static int l_ram_write(lua_State *ls) {
        auto *s = self_from(ls);
        const int kind = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        if (!s || !s->active_ctx || !s->active_ctx->ram || s->active_ctx->ram_size == 0) return 0;
        const auto addr = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0)));
        auto *r = s->active_ctx->ram;
        const auto sz = s->active_ctx->ram_size;
        if (kind == 0) SemanticRam::wr8(r, sz, addr, static_cast<std::uint8_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0)) & 0xFF));
        else if (kind == 1) SemanticRam::wr16(r, sz, addr, static_cast<std::uint16_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0)) & 0xFFFF));
        else if (kind == 2) SemanticRam::wr32(r, sz, addr, static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0))));
        else {
            const float f = static_cast<float>(luaL_optnumber(ls, 2, 0.0));
            std::uint32_t raw = 0;
            std::memcpy(&raw, &f, 4);
            SemanticRam::wr32(r, sz, addr, raw);
        }
        return 0;
    }
    static int l_read_str(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx || !s->active_ctx->ram || s->active_ctx->ram_size == 0 || lua_gettop(ls) < 1) {
            lua_pushstring(ls, "");
            return 1;
        }
        const auto base = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0)));
        const std::size_t max_len = std::clamp<std::size_t>(static_cast<std::size_t>(luaL_optnumber(ls, 2, 128.0)), 1u, 1024u);
        std::string out;
        out.reserve(std::min<std::size_t>(max_len, 64u));
        for (std::size_t i = 0; i < max_len; ++i) {
            const std::uint8_t ch = SemanticRam::rd8(s->active_ctx->ram, s->active_ctx->ram_size, base + static_cast<std::uint32_t>(i));
            if (ch == 0u) break;
            out.push_back(static_cast<char>(ch));
        }
        lua_pushlstring(ls, out.data(), out.size());
        return 1;
    }
    static int l_write_str(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->ram && s->active_ctx->ram_size > 0 && lua_gettop(ls) >= 2) {
            const auto base = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0)));
            const std::string txt = lua_val_to_string(ls, 2);
            const std::size_t max_len = (lua_gettop(ls) >= 3)
                ? std::clamp<std::size_t>(static_cast<std::size_t>(luaL_optnumber(ls, 3, 0.0)), 1u, 1024u)
                : (txt.size() + 1u);
            const std::size_t n = std::min(txt.size(), max_len > 0 ? max_len - 1u : 0u);
            for (std::size_t i = 0; i < n; ++i) {
                SemanticRam::wr8(s->active_ctx->ram, s->active_ctx->ram_size, base + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(txt[i]));
            }
            if (max_len > 0) SemanticRam::wr8(s->active_ctx->ram, s->active_ctx->ram_size, base + static_cast<std::uint32_t>(n), 0u);
        }
        return 0;
    }
    static int l_scan_bytes(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s || !s->active_ctx || !s->active_ctx->ram || s->active_ctx->ram_size == 0 || lua_gettop(ls) < 1) {
            lua_pushinteger(ls, -1);
            return 1;
        }
        std::vector<int> pat;
        std::istringstream iss(lua_val_to_string(ls, 1));
        std::string tok;
        while (iss >> tok) {
            if (tok == "??" || tok == "?" || tok == "*") pat.push_back(-1);
            else pat.push_back(static_cast<int>(std::strtoul(tok.c_str(), nullptr, 16) & 0xFFu));
        }
        const std::size_t rsz = s->active_ctx->ram_size;
        if (pat.empty() || pat.size() > rsz) { lua_pushinteger(ls, -1); return 1; }
        std::size_t start_off = (lua_gettop(ls) >= 2) ? (static_cast<std::size_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0))) & (rsz - 1u)) : 0u;
        std::size_t end_off = (lua_gettop(ls) >= 3) ? (static_cast<std::size_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 3, static_cast<double>(rsz)))) & (rsz - 1u)) : rsz;
        if (end_off == 0 || end_off > rsz) end_off = rsz;
        if (start_off + pat.size() > end_off) { lua_pushinteger(ls, -1); return 1; }
        constexpr std::size_t kMaxScanWindow = 4u << 20;
        if (end_off - start_off > kMaxScanWindow) end_off = start_off + kMaxScanWindow;
        const std::size_t limit = end_off - pat.size();
        const auto *ram = s->active_ctx->ram;
        for (std::size_t pos = start_off; pos <= limit; ++pos) {
            if ((pos & 0xFFFu) == 0u && ++s->scan_budget > 2048) { lua_pushinteger(ls, -1); return 1; }
            bool match = true;
            for (std::size_t j = 0; j < pat.size(); ++j) {
                if (pat[j] >= 0 && ram[pos + j] != static_cast<std::uint8_t>(pat[j])) { match = false; break; }
            }
            if (match) { lua_pushinteger(ls, static_cast<lua_Integer>(pos)); return 1; }
        }
        lua_pushinteger(ls, -1);
        return 1;
    }

    // ---- Bitwise, Math, RNG, Misc ----
    static int l_bit_op(lua_State *ls) {
        const int op = static_cast<int>(lua_tointeger(ls, lua_upvalueindex(1)));
        const auto a = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0)));
        const auto b = static_cast<std::uint32_t>(static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0)));
        std::uint32_t r = 0u;
        if (op == 0) r = a & b;
        else if (op == 1) r = a | b;
        else if (op == 2) r = a ^ b;
        else if (op == 3) r = a << (b & 31u);
        else r = a >> (b & 31u);
        lua_pushinteger(ls, static_cast<lua_Integer>(r));
        return 1;
    }
    static int l_min(lua_State *ls) { lua_pushnumber(ls, std::min(luaL_optnumber(ls, 1, 0.0), luaL_optnumber(ls, 2, 0.0))); return 1; }
    static int l_max(lua_State *ls) { lua_pushnumber(ls, std::max(luaL_optnumber(ls, 1, 0.0), luaL_optnumber(ls, 2, 0.0))); return 1; }
    static int l_clamp(lua_State *ls) {
        lua_pushnumber(ls, std::clamp(luaL_optnumber(ls, 1, 0.0), luaL_optnumber(ls, 2, 0.0), luaL_optnumber(ls, 3, 1.0)));
        return 1;
    }
    static int fn_floor(lua_State *ls) { lua_pushnumber(ls, std::floor(luaL_optnumber(ls, 1, 0.0))); return 1; }
    static int fn_ceil(lua_State *ls)  { lua_pushnumber(ls, std::ceil(luaL_optnumber(ls, 1, 0.0))); return 1; }
    static int fn_abs(lua_State *ls)   { lua_pushnumber(ls, std::fabs(luaL_optnumber(ls, 1, 0.0))); return 1; }
    static std::uint32_t next_rng(Lua54Script *s) noexcept {
        if (!s || !s->active_ctx || !s->active_ctx->rng_state) return 1u;
        std::uint32_t x = *s->active_ctx->rng_state;
        if (x == 0u) x = 0x6D2B79F5u;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        *s->active_ctx->rng_state = x;
        return x;
    }
    static int l_rand(lua_State *ls) {
        lua_pushnumber(ls, static_cast<double>(next_rng(self_from(ls))) / 4294967296.0);
        return 1;
    }
    static int l_randint(lua_State *ls) {
        const auto lo = static_cast<std::int64_t>(luaL_optnumber(ls, 1, 0.0));
        const auto hi = static_cast<std::int64_t>(luaL_optnumber(ls, 2, 0.0));
        if (hi <= lo) { lua_pushinteger(ls, lo); return 1; }
        const auto span = static_cast<std::uint64_t>(hi - lo + 1);
        lua_pushinteger(ls, lo + static_cast<std::int64_t>(next_rng(self_from(ls)) % span));
        return 1;
    }
    static int l_frame(lua_State *ls) {
        auto *s = self_from(ls);
        lua_pushinteger(ls, (s && s->active_ctx) ? static_cast<lua_Integer>(s->active_ctx->frame_index) : 0);
        return 1;
    }
    static int l_game_slug(lua_State *ls) {
        auto *s = self_from(ls);
        const std::string slug = (s && s->active_ctx) ? s->active_ctx->game_slug : std::string();
        lua_pushlstring(ls, slug.data(), slug.size());
        return 1;
    }
    static int l_toast(lua_State *ls) {
        auto *s = self_from(ls);
        if (s && s->active_ctx && s->active_ctx->toast_queue && lua_gettop(ls) >= 1 && s->active_ctx->toast_queue->size() < 8) {
            s->active_ctx->toast_queue->push_back(lua_val_to_string(ls, 1));
        }
        return 0;
    }
    static int l_log(lua_State *ls) {
        auto *s = self_from(ls);
        if (!s) return 0;
        const int n = lua_gettop(ls);
        if (n <= 0) return 0;
        std::string msg;
        for (int i = 1; i <= n; ++i) {
            if (i > 1) msg.push_back(' ');
            msg += lua_val_to_string(ls, i);
        }
        s->emit_log(msg);
        return 0;
    }

    void register_sandbox_and_api() {
        // Open only safe standard libraries (os, io, debug, package/loadlib, coroutine excluded so
        // scripts cannot escape sandbox or bypass instruction-count hook limits).
        luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);

        // Strip file/system/bytecode-load escape primitives from base library
        for (const char *blocked : {"dofile", "loadfile", "load", "collectgarbage"}) {
            lua_pushnil(L);
            lua_setglobal(L, blocked);
        }

        // Store self pointer & module cache table in registry
        lua_pushlightuserdata(L, this);
        lua_setfield(L, LUA_REGISTRYINDEX, "__nocturne_self");
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "__nocturne_loaded");

        auto reg = [&](const char *name, lua_CFunction fn) {
            lua_pushcfunction(L, fn);
            lua_setglobal(L, name);
        };
        auto reg_u1 = [&](const char *name, lua_CFunction fn, int tag) {
            lua_pushinteger(L, tag);
            lua_pushcclosure(L, fn, 1);
            lua_setglobal(L, name);
        };

        reg("require", l_require);
        reg("is_in_game", l_is_in_game);
        reg("get_cash", l_get_cash);
        reg("add_cash", l_add_cash);
        reg("get_battles_won", l_get_battles_won);
        reg_u1("get_ring_attempts", l_get_ring, 0);
        reg_u1("get_ring_successes", l_get_ring, 1);
        reg_u1("get_ring_perfects", l_get_ring, 2);
        reg("get_max_level", l_get_max_level);
        reg("heal_party", l_heal_party);
        reg("restore_party_mp", l_restore_party_mp);
        reg("add_party_exp", l_add_party_exp);
        reg("sync_bench_exp", l_sync_bench_exp);
        reg("unlock_all_party", l_unlock_all_party);

        reg("get_hero_count", l_get_hero_count);
        reg("get_hero_joined", l_get_hero_joined);
        reg("get_hero_name", l_get_hero_name);
        reg("get_hero_level", l_get_hero_level);
        reg_u1("get_hero_hp", l_hero_stat, 0);
        reg_u1("get_hero_max_hp", l_hero_stat, 1);
        reg_u1("set_hero_hp", l_hero_stat, 2);
        reg_u1("get_hero_mp", l_hero_stat, 3);
        reg_u1("get_hero_max_mp", l_hero_stat, 4);
        reg_u1("set_hero_mp", l_hero_stat, 5);
        reg_u1("get_hero_exp", l_hero_stat, 6);
        reg_u1("set_hero_exp", l_hero_stat, 7);
        reg_u1("get_hero_weapon_durability", l_get_hero_weapon_durability, 0);
        reg_u1("get_hero_weapon_max_durability", l_get_hero_weapon_durability, 1);
        reg_u1("get_selected_weapon_durability", l_get_selected_weapon, 0);
        reg_u1("get_selected_weapon_max_durability", l_get_selected_weapon, 1);
        reg_u1("get_selected_weapon_name", l_get_selected_weapon, 2);

        reg("save_get", l_save_get);
        reg("save_set", l_save_set);
        reg("save_delete", l_save_delete);
        reg("get_setting", l_get_setting);
        reg("get_setting_str", l_get_setting_str);

        reg_u1("is_key_down", l_input_query, 0);
        reg_u1("is_key_pressed", l_input_query, 1);
        reg_u1("is_pad_down", l_input_query, 2);
        reg_u1("is_pad_pressed", l_input_query, 3);
        reg_u1("pad_press", l_pad_mut, 0);
        reg_u1("pad_block", l_pad_mut, 1);

        reg("hud_set", l_hud_set);
        reg("hud_clear", l_hud_clear);
        reg("hud_clear_all", l_hud_clear_all);
        reg("set_item_glow", l_set_item_glow);
        reg("draw_clear", l_draw_clear);
        reg("draw_rect", l_draw_rect);
        reg("draw_bar", l_draw_bar);
        reg("draw_text", l_draw_text);
        reg("play_sfx", l_play_sfx);

        reg_u1("read_u8", l_ram_read, 0);
        reg_u1("read_u16", l_ram_read, 1);
        reg_u1("read_u32", l_ram_read, 2);
        reg_u1("read_s32", l_ram_read, 3);
        reg_u1("read_f32", l_ram_read, 4);
        reg_u1("write_u8", l_ram_write, 0);
        reg_u1("write_u16", l_ram_write, 1);
        reg_u1("write_u32", l_ram_write, 2);
        reg_u1("write_f32", l_ram_write, 3);
        reg("read_str", l_read_str);
        reg("write_str", l_write_str);
        reg("scan_bytes", l_scan_bytes);

        reg_u1("bit_and", l_bit_op, 0);
        reg_u1("bit_or", l_bit_op, 1);
        reg_u1("bit_xor", l_bit_op, 2);
        reg_u1("bit_shl", l_bit_op, 3);
        reg_u1("bit_shr", l_bit_op, 4);
        reg("min", l_min);
        reg("max", l_max);
        reg("clamp", l_clamp);
        reg("floor", fn_floor);
        reg("ceil", fn_ceil);
        reg("abs", fn_abs);
        reg("rand", l_rand);
        reg("randint", l_randint);
        reg("frame", l_frame);
        reg("game_slug", l_game_slug);
        reg("toast", l_toast);
        reg("notify", l_toast);
        reg("log", l_log);
        reg("print", l_log);
    }

    void refresh_hook_cache() {
        hook_cache.clear();
        if (!L) return;
        static constexpr const char *kHooks[] = {
            "on_boot", "on_game_ready", "on_frame", "on_hotkey", "on_setting_changed",
            "on_battle_start", "on_battle_end", "on_battle_win", "on_turn_begin",
            "on_area_load", "on_level_up", "on_ring_result",
            "on_ring_perfect", "on_ring_hit", "on_ring_miss",
        };
        for (const char *h : kHooks) {
            lua_getglobal(L, h);
            if (lua_isfunction(L, -1)) hook_cache[h] = true;
            lua_pop(L, 1);
        }
    }

    bool load_source(const std::string &mod, const std::string &fname, const std::string &src,
                     const std::filesystem::path &mdir = {},
                     const std::unordered_map<std::string, double> &cfg = {},
                     const std::unordered_map<std::string, std::string> &str_cfg = {},
                     const std::filesystem::path &custom_state_path = {},
                     bool is_coop_safe = false) {
        close_vm();
        mod_name = mod;
        mod_dir = mdir;
        state_path = custom_state_path;
        mod_folder = mdir.empty() ? mod : mdir.filename().string();
        file_name = fname;
        coop_safe = is_coop_safe;
        settings_store = cfg;
        string_settings_store = str_cfg;
        load_state_file();

        L = luaL_newstate();
        if (!L) return false;
        register_sandbox_and_api();

        const std::string chunk_name = "@" + fname;
        if (luaL_loadbufferx(L, src.data(), src.size(), chunk_name.c_str(), "t") != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            emit_log(std::string("Lua syntax error: ") + (err ? err : "unknown"));
            lua_pop(L, 1);
            close_vm();
            return false;
        }
        // Execute top-level definitions (functions & initializers) under instruction cap
        lua_sethook(L, instruction_hook, LUA_MASKCOUNT, kInstructionCap);
        const int st = lua_pcall(L, 0, 0, 0);
        lua_sethook(L, nullptr, 0, 0);
        if (st != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            emit_log(std::string("Lua load error: ") + (err ? err : "unknown"));
            lua_pop(L, 1);
            close_vm();
            return false;
        }
        top_level_ran = true;
        refresh_hook_cache();
        return true;
    }

    bool has_hook(const char *name) const {
        return hook_cache.find(name) != hook_cache.end();
    }

    std::size_t hook_count() const noexcept {
        return hook_cache.size();
    }

    void call_hook(const char *name, ScriptHostCtx &ctx, const std::vector<Val> &args = {}) {
        if (!L || !has_hook(name)) return;
        active_ctx = &ctx;
        scan_budget = 0;
        lua_getglobal(L, name);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            active_ctx = nullptr;
            return;
        }
        for (const auto &a : args) {
            if (a.is_nil) lua_pushnil(L);
            else if (a.is_str) lua_pushlstring(L, a.str.data(), a.str.size());
            else push_smart_number(L, a.num);
        }
        lua_sethook(L, instruction_hook, LUA_MASKCOUNT, kInstructionCap);
        const int st = lua_pcall(L, static_cast<int>(args.size()), 0, 0);
        lua_sethook(L, nullptr, 0, 0);
        if (st != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            emit_log(std::string("Lua error in ") + name + "(): " + (err ? err : "unknown"));
            lua_pop(L, 1);
        }
        active_ctx = nullptr;
    }
};

}  // namespace mod_engine::detail
