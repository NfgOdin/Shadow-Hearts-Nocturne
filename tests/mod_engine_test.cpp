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

    // Koudelka PS1 weapon durability test (actor+0x498 -> wpn, +0x7A cur, +0x7C max)
    std::vector<std::uint8_t> kdl_ram(0x00200000u, 0x00u);
    const std::uint32_t root = 0x800B6000u;
    const std::uint32_t actor = 0x801D0FB8u;
    const std::uint32_t wpn = 0x801D9598u;
    std::memcpy(kdl_ram.data() + 0x000B59A0u, &root, 4);
    std::memcpy(kdl_ram.data() + ((root - 0x44u) & 0x1FFFFFu), &actor, 4);
    kdl_ram[(actor + 0x146u) & 0x1FFFFFu] = 'K';
    kdl_ram[(actor + 0x4BAu) & 0x1FFFFFu] = 1u;
    std::memcpy(kdl_ram.data() + ((actor + 0x498u) & 0x1FFFFFu), &wpn, 4);
    const std::uint16_t cur_d = 18u, max_d = 79u;
    std::memcpy(kdl_ram.data() + ((wpn + 0x7Au) & 0x1FFFFFu), &cur_d, 2);
    std::memcpy(kdl_ram.data() + ((wpn + 0x7Cu) & 0x1FFFFFu), &max_d, 2);

    mod_engine::detail::LuaScript kscript;
    const std::string ksrc = R"(
function on_boot()
    hud_set("dur", "SA_Pistol", string.format("%d/%d", get_hero_weapon_durability(0), get_hero_weapon_max_durability(0)))
end
)";
    check(kscript.load_source("KdlDur", "dur.lua", ksrc), "Koudelka durability Lua test loads");
    std::vector<mod_engine::HudWidget> kwidgets;
    mod_engine::detail::ScriptHostCtx kctx{};
    kctx.ram = kdl_ram.data();
    kctx.ram_size = kdl_ram.size();
    kctx.hud_widgets = &kwidgets;
    kscript.call_hook("on_boot", kctx);
    check(kwidgets.size() == 1 && kwidgets[0].value == "18/79",
          "Koudelka get_hero_weapon_durability(0) & max_durability(0) read actor+0x498 -> +0x7A/+0x7C");

    // Verify menu-selected weapon durability (Field -> Equip -> Weapon Picker)
    const std::uint32_t inv = 0x801D92CCu;
    const std::uint32_t wpn0 = inv + 0x238u;           // Normal Knife (25/25)
    const std::uint32_t wpn1 = inv + 0x238u + 0x94u;   // SA_Pistol (18/79)
    const std::uint32_t wpn_cnt = 2u;
    const std::uint32_t ovl4 = 4u;
    std::memcpy(kdl_ram.data() + 0x000B4DD0u, &inv, 4);
    std::memcpy(kdl_ram.data() + 0x000B6DF0u, &ovl4, 4);
    std::memcpy(kdl_ram.data() + ((inv + 0x12Cu) & 0x1FFFFFu), &wpn_cnt, 4);
    std::memcpy(kdl_ram.data() + ((wpn0 + 0x0Au) & 0x1FFFFFu), "Normal Knife", 13);
    const std::uint16_t k_cur = 21u, k_max = 25u;
    std::memcpy(kdl_ram.data() + ((wpn0 + 0x7Au) & 0x1FFFFFu), &k_cur, 2);
    std::memcpy(kdl_ram.data() + ((wpn0 + 0x7Cu) & 0x1FFFFFu), &k_max, 2);
    std::memcpy(kdl_ram.data() + ((wpn1 + 0x0Au) & 0x1FFFFFu), "SA_Pistol", 10);
    std::memcpy(kdl_ram.data() + ((wpn1 + 0x7Au) & 0x1FFFFFu), &cur_d, 2);
    std::memcpy(kdl_ram.data() + ((wpn1 + 0x7Cu) & 0x1FFFFFu), &max_d, 2);

    // Place Equip->Weapon controller at a dynamically relocated heap address (not Room-1 fixed offset)
    const std::uint32_t eqw_ctrl = 0x80181240u;
    const std::uint32_t active1 = 1u, self_list = eqw_ctrl + 0x28u;
    const std::uint32_t tag31 = 0x31u, wgt = 0x80166B9Cu, list_ptr = 0x801AED78u;
    const std::uint32_t e0 = 0x8017E358u, e1 = 0x8017E3C4u, e2 = 0x8017E430u;
    const std::uint32_t s0 = 0x8016B978u, s1 = 0x8016B95Cu, s2 = 0x8016BAA4u;
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x08u) & 0x1FFFFFu), &active1, 4);
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x1Cu) & 0x1FFFFFu), &self_list, 4);
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x24u) & 0x1FFFFFu), &self_list, 4);
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x30u) & 0x1FFFFFu), &tag31, 4);
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x3Cu) & 0x1FFFFFu), &wgt, 4);
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x28u) & 0x1FFFFFu), &list_ptr, 4);
    std::memcpy(kdl_ram.data() + ((list_ptr + 0u) & 0x1FFFFFu), &e0, 4);
    std::memcpy(kdl_ram.data() + ((list_ptr + 4u) & 0x1FFFFFu), &e1, 4);
    std::memcpy(kdl_ram.data() + ((list_ptr + 8u) & 0x1FFFFFu), &e2, 4);
    std::memcpy(kdl_ram.data() + ((e0 + 0x10u) & 0x1FFFFFu), &s0, 4);
    std::memcpy(kdl_ram.data() + ((e1 + 0x10u) & 0x1FFFFFu), &s1, 4);
    std::memcpy(kdl_ram.data() + ((e2 + 0x10u) & 0x1FFFFFu), &s2, 4);
    std::memcpy(kdl_ram.data() + (s0 & 0x1FFFFFu), "beNDOFBGMFAFLF@BKDNFIFFFEF", 27);
    std::memcpy(kdl_ram.data() + (s1 & 0x1FFFFFu), "heCEADOE@EIFCGDGOFLF", 21);
    std::memcpy(kdl_ram.data() + (s2 & 0x1FFFFFu), "`eBDAFBGEFMBHDAFNFDFEFDF", 25);

    mod_engine::detail::LuaScript sel_script;
    const std::string sel_src = R"(
function on_boot()
    hud_set("sel", get_selected_weapon_name(), string.format("%d/%d", get_selected_weapon_durability(), get_selected_weapon_max_durability()))
end
)";
    check(sel_script.load_source("KdlSel", "sel.lua", sel_src), "Koudelka selected weapon Lua test loads");
    // Slot 0 -> Normal Knife (21/25)
    std::uint32_t sel_idx = 0u;
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x20u) & 0x1FFFFFu), &sel_idx, 4);
    kwidgets.clear();
    sel_script.call_hook("on_boot", kctx);
    check(kwidgets.size() == 1 && kwidgets[0].label == "Normal Knife" && kwidgets[0].value == "21/25",
          "Koudelka selected weapon slot 0 resolves Normal Knife 21/25");
    // Slot 1 -> SA Pistol (18/79)
    sel_idx = 1u;
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x20u) & 0x1FFFFFu), &sel_idx, 4);
    kwidgets.clear();
    sel_script.call_hook("on_boot", kctx);
    check(kwidgets.size() == 1 && kwidgets[0].label == "SA Pistol" && kwidgets[0].value == "18/79",
          "Koudelka selected weapon slot 1 resolves SA Pistol 18/79");
    // Slot 2 -> Bare-Handed (-1/-1)
    sel_idx = 2u;
    std::memcpy(kdl_ram.data() + ((eqw_ctrl + 0x20u) & 0x1FFFFFu), &sel_idx, 4);
    kwidgets.clear();
    sel_script.call_hook("on_boot", kctx);
    check(kwidgets.size() == 1 && kwidgets[0].label.empty() && kwidgets[0].value == "-1/-1",
          "Koudelka selected weapon slot 2 (Bare-Handed) returns -1/-1");
}

void test_all_bundled_mods_load_in_lua54() {
    const std::vector<std::filesystem::path> scripts = {
        "assets/mods/koudelka/NemetonLantern/scripts/nemeton_lantern.lua",
        "assets/mods/koudelka/NocturneDurabilityMod/scripts/nocturne_durability.lua",
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
    check(checked == 2 || checked == 0, "All bundled catalog Lua mods smoke-tested when run from repo root");
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
