#pragma once

// ABI between the shared PS2 engine executable (engine/nocturne_ps2.exe) and a
// locally-compiled per-game DLL (games/<slug>/game.dll).
//
// game.dll contains ONLY the statically recompiled EE & VU units for one game
// (built once from the user's own disc). It has zero static linkage to
// ps2runtime, SDL2, or wgpu-native — every runtime call forwards through
// Ps2GameHost. This lets engine/launcher/UI/GPU/audio updates ship as a 2-second
// binary swap without ever recompiling any game unless kPs2GameAbiVersion or
// ps2_game_dll_layout() changes.

#include "ee_live_abi.hpp"
#include "generated.hpp"
#include "vu_ops.hpp"

#include <cstdint>

namespace ps2 {

inline constexpr std::uint32_t kPs2GameAbiVersion = 2u;

struct Ps2GameHost {
    std::uint32_t abi_version;
    std::uint32_t reserved;
    std::uint64_t layout;  // ps2_game_dll_layout()
    EeLiveHost ee;

    void (*register_units)(const UnitInfo *units, std::uint32_t count);
    void (*register_modules)(const ModuleInfo *modules, std::uint32_t count);

    void (*vu_register_programs)(const vu::Program *programs, std::uint32_t count);
    void (*vu_kick)(int unit, std::uint32_t address);
    std::uint32_t (*vu_top)();
    std::uint32_t (*vu_itop0)();
    std::uint32_t (*vu_itop1)(int unit);
    std::uint32_t (*vu_entry)();
    void (*vu_interpret)(int unit, std::uint32_t pc);
    bool (*vu_stopped)();
    std::uint64_t (*vu_vu0_key)(std::uint32_t entry);
    void (*vu_vu0_restart)(std::uint32_t entry);
};

inline constexpr std::uint64_t ps2_game_dll_layout() noexcept {
    return (static_cast<std::uint64_t>(sizeof(Cpu)) & 0xFFFFull) |
           ((static_cast<std::uint64_t>(sizeof(EeLiveHost)) & 0xFFFFull) << 16) |
           ((static_cast<std::uint64_t>(sizeof(Ps2GameHost)) & 0xFFFFull) << 32) |
           ((static_cast<std::uint64_t>(sizeof(Vu0Regs)) & 0xFFFFull) << 48);
}

using Ps2GameAbiFn = std::uint32_t (*)();
using Ps2GameLayoutFn = std::uint64_t (*)();
using Ps2GameInitFn = bool (*)(const Ps2GameHost *host);

}  // namespace ps2
