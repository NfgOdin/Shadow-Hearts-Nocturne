#pragma once

// ABI between the shared PS1 engine executable (engine/nocturne_psx.exe) and a
// locally-compiled PS1 game DLL (games/koudelka/game.dll).
//
// game.dll contains ONLY the statically recompiled R3000A + GTE units for
// Koudelka (built once from the user's own discs). It has zero static linkage
// to psxruntime or SDL2 — every runtime call forwards through PsxGameHost.

#include "psx/system.hpp"

#include <cstddef>
#include <cstdint>

namespace psx {

inline constexpr std::uint32_t kPsxGameAbiVersion = 2u;

struct PsxGameHost {
    std::uint32_t abi_version;
    std::uint32_t reserved;
    std::uint64_t layout;  // psx_game_dll_layout()

    std::uint8_t (*mem_read8)(const Memory *mem, System &sys, std::uint32_t vaddr);
    std::uint16_t (*mem_read16)(const Memory *mem, System &sys, std::uint32_t vaddr);
    std::uint32_t (*mem_read32)(const Memory *mem, System &sys, std::uint32_t vaddr);
    void (*mem_write8)(Memory *mem, System &sys, std::uint32_t vaddr, std::uint8_t val);
    void (*mem_write16)(Memory *mem, System &sys, std::uint32_t vaddr, std::uint16_t val);
    void (*mem_write32)(Memory *mem, System &sys, std::uint32_t vaddr, std::uint32_t val);

    std::uint32_t (*gte_mfc2)(const Gte *gte, std::uint32_t reg);
    void (*gte_mtc2)(Gte *gte, std::uint32_t reg, std::uint32_t val);
    std::uint32_t (*gte_cfc2)(const Gte *gte, std::uint32_t reg);
    void (*gte_ctc2)(Gte *gte, std::uint32_t reg, std::uint32_t val);
    void (*gte_execute)(Gte *gte, std::uint32_t cmd);

    void (*hw_update_cpu_irq)(Hardware *hw, System &sys);
    bool (*bios_handle_vector)(Bios *bios, System &sys, std::uint32_t vector_addr);
    void (*bios_handle_syscall)(Bios *bios, System &sys);

    void (*sys_step_hardware)(System *sys, std::uint32_t cycles);
    void (*sys_register_aot)(System *sys, const AotEntry *entries, std::size_t count);
    void (*dispatch_fn)(System &sys, std::uint32_t target_pc);
};

inline constexpr std::uint64_t psx_game_dll_layout() noexcept {
    // Mix sizes of all System sub-objects touched by AOT code into a 64-bit fingerprint.
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 0x100000001B3ull;
    };
    mix(sizeof(System));
    mix(sizeof(Cpu));
    mix(sizeof(Gte));
    mix(sizeof(Memory));
    mix(sizeof(Hardware));
    mix(sizeof(Cdrom));
    mix(sizeof(Bios));
    mix(sizeof(AotEntry));
    mix(sizeof(PsxGameHost));
    return h;
}

using PsxGameAbiFn = std::uint32_t (*)();
using PsxGameLayoutFn = std::uint64_t (*)();
using PsxGameInitFn = bool (*)(const PsxGameHost *host, System *sys);

}  // namespace psx
