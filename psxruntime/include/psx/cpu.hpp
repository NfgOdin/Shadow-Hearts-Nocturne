#pragma once

#include "psx/types.hpp"

#include <cstdint>

namespace psx {

// COP0 register indices on R3000A (PS1)
enum Cop0Reg : std::uint32_t {
    kCop0Bpc = 3,
    kCop0Bda = 5,
    kCop0JumpDest = 6,
    kCop0Dcic = 7,
    kCop0BadVAddr = 8,
    kCop0Bdam = 9,
    kCop0Bpcm = 11,
    kCop0Sr = 12,
    kCop0Cause = 13,
    kCop0Epc = 14,
    kCop0Prid = 15,
};

struct Cpu {
    std::uint32_t r[32] = {};
    std::uint32_t hi = 0;
    std::uint32_t lo = 0;
    std::uint32_t pc = 0xBFC00000u;
    std::uint32_t npc = 0xBFC00004u;
    std::uint32_t cop0[32] = {};

    // Pending load-delay slot state (used by interpreter for exact R3000A hazard fidelity).
    std::uint8_t load_reg = 0;
    std::uint32_t load_val = 0;
    std::uint8_t next_load_reg = 0;
    std::uint32_t next_load_val = 0;

    bool in_delay_slot = false;
    bool next_in_delay_slot = false;
    bool halted = false;

    std::uint64_t cycles = 0;
    std::int32_t budget = 0;

    void reset() {
        for (auto &v : r) v = 0;
        for (auto &v : cop0) v = 0;
        hi = 0;
        lo = 0;
        pc = 0xBFC00000u;
        npc = 0xBFC00004u;
        cop0[kCop0Sr] = 0x10900000u;    // CU0/CU2 enabled, BEV=1
        cop0[kCop0Prid] = 0x00000002u;  // R3000A rev 2
        load_reg = 0;
        load_val = 0;
        next_load_reg = 0;
        next_load_val = 0;
        in_delay_slot = false;
        next_in_delay_slot = false;
        halted = false;
        cycles = 0;
        budget = 0;
    }

    inline bool cache_isolated() const noexcept {
        return (cop0[kCop0Sr] & (1u << 16)) != 0;
    }

    inline bool irq_enabled() const noexcept {
        const std::uint32_t sr = cop0[kCop0Sr];
        const std::uint32_t cause = cop0[kCop0Cause];
        return (sr & 1u) != 0 && ((sr & cause & 0xFF00u) != 0);
    }

    inline void commit_load_delay() noexcept {
        if (load_reg != 0) {
            r[load_reg] = load_val;
        }
        load_reg = next_load_reg;
        load_val = next_load_val;
        next_load_reg = 0;
        next_load_val = 0;
        r[0] = 0;
    }

    inline void flush_load_delay() noexcept {
        if (load_reg != 0) {
            r[load_reg] = load_val;
            load_reg = 0;
        }
        if (next_load_reg != 0) {
            r[next_load_reg] = next_load_val;
            next_load_reg = 0;
        }
        r[0] = 0;
    }

    inline void cancel_load_delay(std::uint32_t reg) noexcept {
        if (load_reg == reg) load_reg = 0;
    }
};

// Exact R3000A signed/unsigned division semantics (no UB on div-by-zero or INT32_MIN/-1).
inline void r3000a_div(Cpu &c, std::uint32_t rs_val, std::uint32_t rt_val) noexcept {
    const std::int32_t n = static_cast<std::int32_t>(rs_val);
    const std::int32_t d = static_cast<std::int32_t>(rt_val);
    if (d == 0) {
        c.lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
        c.hi = static_cast<std::uint32_t>(n);
    } else if (n == static_cast<std::int32_t>(0x80000000u) && d == -1) {
        c.lo = 0x80000000u;
        c.hi = 0u;
    } else {
        c.lo = static_cast<std::uint32_t>(n / d);
        c.hi = static_cast<std::uint32_t>(n % d);
    }
}

inline void r3000a_divu(Cpu &c, std::uint32_t rs_val, std::uint32_t rt_val) noexcept {
    if (rt_val == 0) {
        c.lo = 0xFFFFFFFFu;
        c.hi = rs_val;
    } else {
        c.lo = rs_val / rt_val;
        c.hi = rs_val % rt_val;
    }
}

inline void r3000a_mult(Cpu &c, std::uint32_t rs_val, std::uint32_t rt_val) noexcept {
    const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(rs_val)) *
                           static_cast<std::int64_t>(static_cast<std::int32_t>(rt_val));
    c.lo = static_cast<std::uint32_t>(p);
    c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
}

inline void r3000a_multu(Cpu &c, std::uint32_t rs_val, std::uint32_t rt_val) noexcept {
    const std::uint64_t p = static_cast<std::uint64_t>(rs_val) * static_cast<std::uint64_t>(rt_val);
    c.lo = static_cast<std::uint32_t>(p);
    c.hi = static_cast<std::uint32_t>(p >> 32);
}

}  // namespace psx
