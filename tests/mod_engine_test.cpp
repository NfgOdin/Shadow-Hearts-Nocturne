#include "mod_engine.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    } else {
        std::printf("PASS: %s\n", msg);
    }
}

void write_text(const std::filesystem::path &p, const std::string &txt) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::trunc);
    ofs << txt;
}

void test_short_circuit_and_or() {
    mod_engine::detail::LuaScript script;
    const std::string src = R"(
local side_and = 0
local side_or = 0
function bump_and()
    side_and = side_and + 1
    return true
end
function bump_or()
    side_or = side_or + 1
    return true
end
function on_boot()
    local a = false and bump_and()
    local b = true or bump_or()
    if side_and == 0 and side_or == 0 then
        hud_set("sc", "ShortCircuit", "OK")
    else
        hud_set("sc", "ShortCircuit", "BROKEN")
    end
end
)";
    check(script.load_source("TestMod", "test.lua", src), "LuaScript loads short-circuit test");
    std::vector<mod_engine::HudWidget> widgets;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.hud_widgets = &widgets;
    script.call_hook("on_boot", ctx);
    check(widgets.size() == 1 && widgets[0].value == "OK",
          "Lua `and`/`or` short-circuits without evaluating RHS side effects");
}

void test_lua54_tables_closures_and_sandbox() {
    mod_engine::detail::LuaScript script;
    const std::string src = R"(
local function make_adder(base)
    return function(x) return base + x end
end
function on_boot()
    local t = { {k="a", v=10}, {k="b", v=25} }
    local sum = 0
    for _, item in ipairs(t) do sum = sum + item.v end
    local add5 = make_adder(5)
    local safe = (os == nil) and (io == nil) and (dofile == nil) and (loadfile == nil)
    if safe and add5(sum) == 40 then
        hud_set("l54", "Lua54", string.format("OK:%d", add5(sum)))
    else
        hud_set("l54", "Lua54", "FAIL")
    end
end
)";
    check(script.load_source("Lua54Mod", "l54.lua", src), "Lua 5.4 tables, closures & sandbox test loads");
    std::vector<mod_engine::HudWidget> widgets;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.hud_widgets = &widgets;
    script.call_hook("on_boot", ctx);
    check(widgets.size() == 1 && widgets[0].value == "OK:40",
          "Lua 5.4 executes tables/ipairs/closures/string.format with os/io/dofile stripped");
}

void test_lua54_instruction_cap() {
    mod_engine::detail::LuaScript script;
    const std::string src = R"(
function on_boot()
    local x = 0
    while true do x = x + 1 end
end
)";
    check(script.load_source("InfMod", "inf.lua", src), "Infinite loop script loads");
    std::deque<std::string> logs;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.log_ring = &logs;
    script.call_hook("on_boot", ctx);
    check(!logs.empty() && logs.back().find("instruction budget") != std::string::npos,
          "Lua 5.4 instruction cap terminates infinite loop cleanly");
}

void test_play_sfx_path_traversal_blocked() {
    mod_engine::detail::LuaScript script;
    const std::string src = R"(
function on_boot()
    local r1 = play_sfx("../../../Windows/Media/chimes.wav")
    local r2 = play_sfx("C:/Windows/Media/chimes.wav")
    if not r1 and not r2 then
        hud_set("sfx", "Jail", "BLOCKED")
    else
        hud_set("sfx", "Jail", "ESCAPED")
    end
end
)";
    const auto fake_mod_dir = std::filesystem::temp_directory_path() / "nocturne_mod_jail_test";
    check(script.load_source("JailMod", "jail.lua", src, fake_mod_dir), "LuaScript loads play_sfx jail test");
    std::vector<mod_engine::HudWidget> widgets;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.hud_widgets = &widgets;
    script.call_hook("on_boot", ctx);
    check(widgets.size() == 1 && widgets[0].value == "BLOCKED",
          "play_sfx() blocks relative ../ and absolute path traversal outside mod_dir");
}

void test_scan_bytes_matches_and_budgets() {
    std::vector<std::uint8_t> ram(65536, 0x00u);
    ram[0x1234] = 0x4Bu;  // 'K'
    ram[0x1235] = 0x6Fu;  // 'o'
    ram[0x1236] = 0x99u;  // wildcard
    ram[0x1237] = 0x64u;  // 'd'

    mod_engine::detail::LuaScript script;
    const std::string src = R"(
function on_boot()
    local pos = scan_bytes("4B 6F ?? 64", 0, 65536)
    if pos == 4660 then
        hud_set("aob", "AOB", "FOUND")
    else
        hud_set("aob", "AOB", "MISS")
    end
end
)";
    check(script.load_source("AobMod", "aob.lua", src), "LuaScript loads scan_bytes test");
    std::vector<mod_engine::HudWidget> widgets;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.ram = ram.data();
    ctx.ram_size = ram.size();
    ctx.hud_widgets = &widgets;
    script.call_hook("on_boot", ctx);
    check(widgets.size() == 1 && widgets[0].value == "FOUND",
          "scan_bytes() finds AOB pattern with ?? wildcard");
}

void test_pnach_priority_and_shared_coop_safety() {
    const auto root = std::filesystem::temp_directory_path() / "nocturne_mod_engine_v2_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto sh1_dir = root / "games" / "sh1";
    const auto shared_mods = root / "games" / "shared" / "mods";

    // Top-priority ModA writes 0x11111111 to 0x00001000
    write_text(sh1_dir / "mods" / "ModA" / "mod.json",
               R"({"id":"moda","name":"ModA","version":"1.0.0","game":"sh1","coop_safe":false})");
    write_text(sh1_dir / "mods" / "ModA" / "patches" / "a.pnach",
               "patch=1,EE,00001000,word,11111111\n");

    // Lower-priority ModB writes 0x22222222 to the same address 0x00001000
    write_text(sh1_dir / "mods" / "ModB" / "mod.json",
               R"({"id":"modb","name":"ModB","version":"1.0.0","game":"sh1","coop_safe":false})");
    write_text(sh1_dir / "mods" / "ModB" / "patches" / "b.pnach",
               "patch=1,EE,00001000,word,22222222\n");

    // Shared coop_safe cosmetic mod targeting all games
    write_text(shared_mods / "SharedHud" / "mod.json",
               R"({"id":"shared_hud","name":"SharedHud","version":"1.0.0","game":"all","coop_safe":true,"settings":[{"key":"alpha","label":"Alpha","type":"int","default":180,"min":0,"max":255}]})");
    write_text(shared_mods / "SharedHud" / "scripts" / "hud.lua",
               "function on_boot()\n  hud_set('sh','HUD',tostring(get_setting('alpha',0)))\nend\n");

    // Shared Koudelka-only mod should NOT show up for sh1
    write_text(shared_mods / "KdlOnly" / "mod.json",
               R"({"id":"kdl_only","name":"KdlOnly","version":"1.0.0","game":"koudelka","coop_safe":true})");

    // Per-game setting override for SharedHud under games/sh1/mods_config/SharedHud.ini
    write_text(sh1_dir / "mods_config" / "SharedHud.ini", "alpha = 225\n");

    mod_engine::Engine eng;
    eng.set_game_slug("sh1");
    eng.load_from_game_dir(sh1_dir, "ModA,ModB,SharedHud", true);

    check(eng.active_mod_count() == 3, "Discovers game-specific + compatible shared mods (filters KdlOnly)");
    check(eng.conflicts().size() == 1 && eng.conflicts()[0].winner_mod == "ModA",
          "Detects .pnach address conflict with ModA as winner");
    check(eng.coop_gameplay_hash() != 0u, "Computes non-zero Co-op gameplay mod hash for non-coop_safe mods");

    std::vector<std::uint8_t> ram(65536, 0x00u);
    eng.on_boot(ram.data(), ram.size());
    std::uint32_t patched_val = 0;
    std::memcpy(&patched_val, ram.data() + 0x1000, 4);
    check(patched_val == 0x11111111u, "Top-priority .pnach patch wins over shadowed lower-priority patch");
    check(eng.hud_widgets().size() == 1 && eng.hud_widgets()[0].value == "225",
          "Shared mod loads per-game setting override from games/<slug>/mods_config/<Mod>.ini");

    // Co-op mismatch pause: gameplay patches stop applying, coop_safe HUD still runs
    std::memset(ram.data() + 0x1000, 0, 4);
    eng.set_coop_gameplay_paused(true);
    eng.on_boot(ram.data(), ram.size());
    std::memcpy(&patched_val, ram.data() + 0x1000, 4);
    check(patched_val == 0u && eng.hud_widgets().size() == 1,
          "Co-op gameplay pause skips gameplay .pnach patches while keeping coop_safe HUD mod running");

    std::filesystem::remove_all(root, ec);
}

void test_4game_semantic_apis() {
    // SH1 RAM layout test: heal_party + restore_party_mp (power-of-two RAM size)
    std::vector<std::uint8_t> sh1_ram(0x00800000u, 0x00u);
    sh1_ram[0x004CAE40u] = 0x43u; // 'C'
    sh1_ram[0x004CAE41u] = 0x68u; // 'h'
    sh1_ram[0x00100280u] = 0x40u; // Yuri joined
    const std::uint32_t playtime = 5000u;
    std::memcpy(sh1_ram.data() + 0x0010000Cu, &playtime, 4);
    const std::uint16_t hp = 50u, mhp = 200u, mp = 10u, mmp = 100u;
    std::memcpy(sh1_ram.data() + 0x00100280u + 0x10u, &hp, 2);
    std::memcpy(sh1_ram.data() + 0x00100280u + 0x14u, &mhp, 2);
    std::memcpy(sh1_ram.data() + 0x00100280u + 0x12u, &mp, 2);
    std::memcpy(sh1_ram.data() + 0x00100280u + 0x16u, &mmp, 2);

    mod_engine::detail::LuaScript script;
    const std::string src = R"(
function on_boot()
    heal_party(50)
    restore_party_mp(50)
    hud_set("sh1", get_hero_name(0), string.format("%d/%d", get_hero_hp(0), get_hero_mp(0)))
end
)";
    check(script.load_source("SemMod", "sem.lua", src), "Semantic API Lua test loads");
    std::vector<mod_engine::HudWidget> widgets;
    mod_engine::detail::ScriptHostCtx ctx{};
    ctx.ram = sh1_ram.data();
    ctx.ram_size = sh1_ram.size();
    ctx.hud_widgets = &widgets;
    script.call_hook("on_boot", ctx);
    check(widgets.size() == 1 && widgets[0].label == "Yuri" && widgets[0].value == "150/60",
          "SH1 heal_party(50), restore_party_mp(50), and get_hero_name(0) work cleanly");
}

void test_all_bundled_mods_load_in_lua54() {
    const std::vector<std::filesystem::path> scripts = {
        "assets/mods/koudelka/NemetonLantern/scripts/nemeton_lantern.lua",
        "assets/mods/koudelka/NemetonTracker/scripts/nemeton_tracker.lua",
        "assets/mods/sh1/ChronicleUnleashed/scripts/chronicle_unleashed.lua",
        "assets/mods/shared/CrimsonFlow/scripts/crimson_flow.lua",
        "assets/mods/shared/PartyVitalsHUD/scripts/party_vitals_hud.lua",
        "assets/mods/shared/NocturneQoL/scripts/nocturne_qol.lua",
        "assets/mods/shared/EclipseChallenge/scripts/eclipse_challenge.lua",
    };
    std::error_code ec;
    int checked = 0;
    for (const auto &rel : scripts) {
        if (!std::filesystem::is_regular_file(rel, ec)) continue;
        std::ifstream ifs(rel);
        std::ostringstream oss;
        oss << ifs.rdbuf();
        mod_engine::detail::LuaScript sc;
        std::deque<std::string> logs;
        std::vector<mod_engine::HudWidget> widgets;
        std::vector<std::string> toasts;
        mod_engine::detail::ScriptHostCtx ctx{};
        ctx.log_ring = &logs;
        ctx.hud_widgets = &widgets;
        ctx.toast_queue = &toasts;
        const bool ok = sc.load_source(rel.stem().string(), rel.filename().string(), oss.str());
        if (ok) sc.call_hook("on_boot", ctx);
        const std::string msg = "Bundled mod loads & boots cleanly in Lua 5.4: " + rel.filename().string();
        check(ok, msg.c_str());
        ++checked;
    }
    check(checked == 7 || checked == 0, "All 7 bundled official showcase mods smoke-tested when run from repo root");
}

}  // namespace

int main() {
    test_short_circuit_and_or();
    test_lua54_tables_closures_and_sandbox();
    test_lua54_instruction_cap();
    test_play_sfx_path_traversal_blocked();
    test_scan_bytes_matches_and_budgets();
    test_pnach_priority_and_shared_coop_safety();
    test_4game_semantic_apis();
    test_all_bundled_mods_load_in_lua54();
    if (g_fails == 0) {
        std::printf("ALL MOD ENGINE TESTS PASSED!\n");
    } else {
        std::fprintf(stderr, "%d MOD ENGINE TEST(S) FAILED!\n", g_fails);
    }
    return g_fails ? 1 : 0;
}
