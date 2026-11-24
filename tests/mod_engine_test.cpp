#include "mod_engine.hpp"

#include <cstdio>
#include <filesystem>
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

}  // namespace

int main() {
    test_short_circuit_and_or();
    test_play_sfx_path_traversal_blocked();
    test_scan_bytes_matches_and_budgets();
    if (g_fails == 0) {
        std::printf("ALL MOD ENGINE TESTS PASSED!\n");
    } else {
        std::fprintf(stderr, "%d MOD ENGINE TEST(S) FAILED!\n", g_fails);
    }
    return g_fails ? 1 : 0;
}
