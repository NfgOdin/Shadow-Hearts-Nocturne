// Entry point and runtime forwarders compiled into each PS2 game's game.dll.
// Compiled with -DPS2_LIVE_EE=1 alongside the generated EE/VU units.

#define PS2_LIVE_EE 1
#define PS2_LIVE_EE_DEFINE 1

#include "ps2/game_dll_abi.hpp"

void register_generated_code();
__attribute__((weak)) void register_generated_vu() {}

namespace ps2 {

static const Ps2GameHost *g_ps2_game_host = nullptr;

void register_units(const UnitInfo *units, std::uint32_t count) {
    g_ps2_game_host->register_units(units, count);
}

void register_modules(const ModuleInfo *modules, std::uint32_t count) {
    g_ps2_game_host->register_modules(modules, count);
}

namespace vu {

void register_programs(const Program *programs, std::uint32_t count) {
    g_ps2_game_host->vu_register_programs(programs, count);
}

void kick(int unit, std::uint32_t address) {
    g_ps2_game_host->vu_kick(unit, address);
}

std::uint32_t top() {
    return g_ps2_game_host->vu_top();
}

std::uint32_t itop() {
    return g_ps2_game_host->vu_itop0();
}

std::uint32_t itop(int unit) {
    return g_ps2_game_host->vu_itop1(unit);
}

std::uint32_t entry() {
    return g_ps2_game_host->vu_entry();
}

void interpret(int unit, std::uint32_t pc) {
    g_ps2_game_host->vu_interpret(unit, pc);
}

bool stopped() {
    return g_ps2_game_host->vu_stopped();
}

std::uint64_t vu0_key(std::uint32_t e) {
    return g_ps2_game_host->vu_vu0_key(e);
}

void vu0_restart(std::uint32_t e) {
    g_ps2_game_host->vu_vu0_restart(e);
}

}  // namespace vu
}  // namespace ps2

#if defined(_WIN32)
#define NOCTURNE_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define NOCTURNE_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

NOCTURNE_GAME_EXPORT std::uint32_t nocturne_ps2_game_abi() {
    return ps2::kPs2GameAbiVersion;
}

NOCTURNE_GAME_EXPORT std::uint64_t nocturne_ps2_game_layout() {
    return ps2::ps2_game_dll_layout();
}

NOCTURNE_GAME_EXPORT bool nocturne_ps2_game_init(const ps2::Ps2GameHost *host) {
    if (!host) return false;
    if (host->abi_version != ps2::kPs2GameAbiVersion) return false;
    if (host->layout != ps2::ps2_game_dll_layout()) return false;
    ps2::g_ps2_game_host = host;
    ps2::g_ee_host = &host->ee;
    ps2::g_live_ram = host->ee.ram;
    ps2::g_live_spr = host->ee.spr;
    ps2::g_watch_on = host->ee.watch_on;
    ps2::g_watch_base = host->ee.watch_base;
    ps2::g_watch_end = host->ee.watch_end;
    register_generated_code();
    register_generated_vu();
    return true;
}
