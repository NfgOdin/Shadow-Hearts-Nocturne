#pragma once

#include "psx/cpu.hpp"
#include "psx/gte.hpp"
#include "psx/interpreter.hpp"
#include "psx/memory.hpp"
#include "psx/system.hpp"

#include <cstdint>

namespace psx {

// Inline helpers called by psxrecomp-generated C++20 code.

inline std::uint32_t load_lwl(System &sys, std::uint32_t addr, std::uint32_t cur) {
    const std::uint32_t word = sys.mem.read32(sys, addr & ~3u);
    switch (addr & 3u) {
    case 0u: return (cur & 0x00FFFFFFu) | (word << 24);
    case 1u: return (cur & 0x0000FFFFu) | (word << 16);
    case 2u: return (cur & 0x000000FFu) | (word << 8);
    default: return word;
    }
}

inline std::uint32_t load_lwr(System &sys, std::uint32_t addr, std::uint32_t cur) {
    const std::uint32_t word = sys.mem.read32(sys, addr & ~3u);
    switch (addr & 3u) {
    case 0u: return word;
    case 1u: return (cur & 0xFF000000u) | (word >> 8);
    case 2u: return (cur & 0xFFFF0000u) | (word >> 16);
    default: return (cur & 0xFFFFFF00u) | (word >> 24);
    }
}

inline void store_swl(System &sys, std::uint32_t addr, std::uint32_t val) {
    const std::uint32_t aligned = addr & ~3u;
    const std::uint32_t mem = sys.mem.read32(sys, aligned);
    std::uint32_t res = mem;
    switch (addr & 3u) {
    case 0u: res = (mem & 0xFFFFFF00u) | (val >> 24); break;
    case 1u: res = (mem & 0xFFFF0000u) | (val >> 16); break;
    case 2u: res = (mem & 0xFF000000u) | (val >> 8); break;
    default: res = val; break;
    }
    sys.mem.write32(sys, aligned, res);
}

inline void store_swr(System &sys, std::uint32_t addr, std::uint32_t val) {
    const std::uint32_t aligned = addr & ~3u;
    const std::uint32_t mem = sys.mem.read32(sys, aligned);
    std::uint32_t res = mem;
    switch (addr & 3u) {
    case 0u: res = val; break;
    case 1u: res = (mem & 0x000000FFu) | (val << 8); break;
    case 2u: res = (mem & 0x0000FFFFu) | (val << 16); break;
    default: res = (mem & 0x00FFFFFFu) | (val << 24); break;
    }
    sys.mem.write32(sys, aligned, res);
}

inline void cop0_mtc0(System &sys, std::uint32_t rd, std::uint32_t val) {
    const std::uint32_t reg = rd & 31u;
    if (reg == kCop0Cause) {
        sys.cpu.cop0[kCop0Cause] = (sys.cpu.cop0[kCop0Cause] & ~0x00000300u) | (val & 0x00000300u);
        sys.hw.update_cpu_irq(sys);
    } else if (reg != 15u) {  // COP0_PRID (reg 15) is read-only
        sys.cpu.cop0[reg] = val;
        if (reg == kCop0Sr) {
            sys.hw.update_cpu_irq(sys);
        }
    }
}

inline void cop0_rfe(System &sys) {
    const std::uint32_t sr = sys.cpu.cop0[kCop0Sr];
    sys.cpu.cop0[kCop0Sr] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
}

// Tail jump out of an AOT function (either to a BIOS vector, another AOT function, or interpreter).
inline void tail_jump(System &sys, std::uint32_t target_pc) {
    const std::uint32_t masked = target_pc & 0x1FFFFFFFu;
    if (masked == 0xA0u || masked == 0xB0u || masked == 0xC0u) {
        sys.bios.handle_vector(sys, target_pc);
        return;
    }
    sys.cpu.pc = target_pc;
    sys.cpu.npc = target_pc + 4u;
    if (!sys.should_yield()) {
        dispatch(sys, target_pc);
    }
}

// Call a subroutine from AOT code (direct AOT or dispatch fallback).
inline void call_target(System &sys, std::uint32_t target_pc) {
    dispatch(sys, target_pc);
}

}  // namespace psx
