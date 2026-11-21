#include "psx/interpreter.hpp"
#include "psx/system.hpp"

#include <cstdio>

namespace psx {

namespace {

constexpr std::uint32_t kCallbackReturnSentinel = 0xBFC7FFF0u;

inline std::int32_t simm16(std::uint32_t w) noexcept {
    return static_cast<std::int16_t>(w & 0xFFFFu);
}

inline std::uint32_t uimm16(std::uint32_t w) noexcept {
    return w & 0xFFFFu;
}

}  // namespace

void step_instruction(System &sys) {
    Cpu &c = sys.cpu;
    const std::uint32_t pc = c.pc;

    // Intercept BIOS HLE vectors (0xA0 / 0xB0 / 0xC0)
    const std::uint32_t masked_pc = pc & 0x1FFFFFFFu;
    if (masked_pc == 0xA0u || masked_pc == 0xB0u || masked_pc == 0xC0u) {
        if (sys.bios.handle_vector(sys, pc)) {
            ++c.cycles;
            --c.budget;
            return;
        }
    }

    const std::uint32_t w = sys.mem.fetch32(pc);
    const std::uint32_t next_pc = c.npc;
    c.pc = next_pc;
    c.npc = next_pc + 4u;
    c.in_delay_slot = c.next_in_delay_slot;
    c.next_in_delay_slot = false;

    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;
    const std::uint32_t rd = (w >> 11) & 31u;
    const std::uint32_t sa = (w >> 6) & 31u;
    const std::uint32_t fn = w & 63u;

    // Snapshot register reads before pending load-delay commits at end of instruction.
    const std::uint32_t r_rs = c.r[rs];
    const std::uint32_t r_rt = c.r[rt];

    // Helper: write a non-load result immediately (cancels any pending load to same reg).
    auto write_reg = [&](std::uint32_t reg, std::uint32_t val) {
        if (reg != 0) {
            c.cancel_load_delay(reg);
            c.r[reg] = val;
        }
    };

    // Helper: schedule a load-delay result for the next instruction.
    auto load_reg_delayed = [&](std::uint32_t reg, std::uint32_t val) {
        if (reg != 0) {
            c.cancel_load_delay(reg);  // consecutive load to same reg supersedes earlier one
            c.next_load_reg = static_cast<std::uint8_t>(reg);
            c.next_load_val = val;
        }
    };

    // Current value for LWL/LWR: forwards pending load-delay value if targeting same register!
    auto cur_for_unaligned = [&](std::uint32_t reg) -> std::uint32_t {
        if (reg != 0 && c.load_reg == reg) return c.load_val;
        return c.r[reg];
    };

    auto branch = [&](bool cond) {
        c.next_in_delay_slot = true;
        if (cond) {
            c.npc = next_pc + static_cast<std::uint32_t>(simm16(w) << 2);
        }
    };

    switch (op) {
    case 0x00u:  // SPECIAL
        switch (fn) {
        case 0x00u: write_reg(rd, r_rt << sa); break;  // SLL
        case 0x02u: write_reg(rd, r_rt >> sa); break;  // SRL
        case 0x03u: write_reg(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r_rt) >> sa)); break;  // SRA
        case 0x04u: write_reg(rd, r_rt << (r_rs & 31u)); break;  // SLLV
        case 0x06u: write_reg(rd, r_rt >> (r_rs & 31u)); break;  // SRLV
        case 0x07u: write_reg(rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(r_rt) >> (r_rs & 31u))); break;  // SRAV
        case 0x08u:  // JR
            c.next_in_delay_slot = true;
            c.npc = r_rs;
            break;
        case 0x09u:  // JALR
            c.next_in_delay_slot = true;
            write_reg(rd, pc + 8u);
            c.npc = r_rs;
            break;
        case 0x0Cu:  // SYSCALL
            c.commit_load_delay();
            sys.bios.handle_syscall(sys);
            ++c.cycles;
            --c.budget;
            return;
        case 0x0Du:  // BREAK (no-op in HLE release)
            break;
        case 0x10u: write_reg(rd, c.hi); break;  // MFHI
        case 0x11u: c.hi = r_rs; break;          // MTHI
        case 0x12u: write_reg(rd, c.lo); break;  // MFLO
        case 0x13u: c.lo = r_rs; break;          // MTLO
        case 0x18u: r3000a_mult(c, r_rs, r_rt); break;   // MULT
        case 0x19u: r3000a_multu(c, r_rs, r_rt); break;  // MULTU
        case 0x1Au: r3000a_div(c, r_rs, r_rt); break;    // DIV
        case 0x1Bu: r3000a_divu(c, r_rs, r_rt); break;   // DIVU
        case 0x20u:  // ADD
        case 0x21u: write_reg(rd, r_rs + r_rt); break;  // ADDU
        case 0x22u:  // SUB
        case 0x23u: write_reg(rd, r_rs - r_rt); break;  // SUBU
        case 0x24u: write_reg(rd, r_rs & r_rt); break;  // AND
        case 0x25u: write_reg(rd, r_rs | r_rt); break;  // OR
        case 0x26u: write_reg(rd, r_rs ^ r_rt); break;  // XOR
        case 0x27u: write_reg(rd, ~(r_rs | r_rt)); break;  // NOR
        case 0x2Au: write_reg(rd, static_cast<std::int32_t>(r_rs) < static_cast<std::int32_t>(r_rt) ? 1u : 0u); break;  // SLT
        case 0x2Bu: write_reg(rd, r_rs < r_rt ? 1u : 0u); break;  // SLTU
        default: break;
        }
        break;

    case 0x01u: {  // REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL
        const bool is_bgez = (rt & 1u) != 0;
        const bool link = (rt & 0x1Eu) == 0x10u;
        const bool cond = is_bgez ? (static_cast<std::int32_t>(r_rs) >= 0) : (static_cast<std::int32_t>(r_rs) < 0);
        if (link) write_reg(31u, pc + 8u);
        branch(cond);
        break;
    }

    case 0x02u:  // J
        c.next_in_delay_slot = true;
        c.npc = (next_pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        break;

    case 0x03u:  // JAL
        c.next_in_delay_slot = true;
        write_reg(31u, pc + 8u);
        c.npc = (next_pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        break;

    case 0x04u: branch(r_rs == r_rt); break;                                  // BEQ
    case 0x05u: branch(r_rs != r_rt); break;                                  // BNE
    case 0x06u: branch(static_cast<std::int32_t>(r_rs) <= 0); break;          // BLEZ
    case 0x07u: branch(static_cast<std::int32_t>(r_rs) > 0); break;           // BGTZ

    case 0x08u:  // ADDI
    case 0x09u: write_reg(rt, r_rs + static_cast<std::uint32_t>(simm16(w))); break;  // ADDIU
    case 0x0Au: write_reg(rt, static_cast<std::int32_t>(r_rs) < simm16(w) ? 1u : 0u); break;  // SLTI
    case 0x0Bu: write_reg(rt, r_rs < static_cast<std::uint32_t>(simm16(w)) ? 1u : 0u); break;  // SLTIU
    case 0x0Cu: write_reg(rt, r_rs & uimm16(w)); break;  // ANDI
    case 0x0Du: write_reg(rt, r_rs | uimm16(w)); break;  // ORI
    case 0x0Eu: write_reg(rt, r_rs ^ uimm16(w)); break;  // XORI
    case 0x0Fu: write_reg(rt, uimm16(w) << 16); break;   // LUI

    case 0x10u:  // COP0
        if (rs == 0x00u) {  // MFC0
            load_reg_delayed(rt, c.cop0[rd]);
        } else if (rs == 0x04u) {  // MTC0
            c.cop0[rd] = r_rt;
            if (rd == kCop0Sr || rd == kCop0Cause) {
                sys.hw.update_cpu_irq(sys);
            }
        } else if (rs == 0x10u && fn == 0x10u) {  // RFE
            const std::uint32_t sr = c.cop0[kCop0Sr];
            c.cop0[kCop0Sr] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
        }
        break;

    case 0x12u:  // COP2 (GTE)
        if (w & (1u << 25)) {
            sys.gte.execute(w & 0x01FFFFFFu);
        } else {
            switch (rs) {
            case 0x00u: load_reg_delayed(rt, sys.gte.mfc2(rd)); break;  // MFC2
            case 0x02u: load_reg_delayed(rt, sys.gte.cfc2(rd)); break;  // CFC2
            case 0x04u: sys.gte.mtc2(rd, r_rt); break;                  // MTC2
            case 0x06u: sys.gte.ctc2(rd, r_rt); break;                  // CTC2
            default: break;
            }
        }
        break;

    case 0x20u: {  // LB
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, static_cast<std::uint32_t>(static_cast<std::int8_t>(sys.mem.read8(sys, addr))));
        break;
    }
    case 0x21u: {  // LH
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, static_cast<std::uint32_t>(static_cast<std::int16_t>(sys.mem.read16(sys, addr))));
        break;
    }
    case 0x22u: {  // LWL
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t cur = cur_for_unaligned(rt);
        const std::uint32_t word = sys.mem.read32(sys, addr & ~3u);
        std::uint32_t res = cur;
        switch (addr & 3u) {
        case 0u: res = (cur & 0x00FFFFFFu) | (word << 24); break;
        case 1u: res = (cur & 0x0000FFFFu) | (word << 16); break;
        case 2u: res = (cur & 0x000000FFu) | (word << 8); break;
        case 3u: res = word; break;
        }
        load_reg_delayed(rt, res);
        break;
    }
    case 0x23u: {  // LW
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, sys.mem.read32(sys, addr));
        break;
    }
    case 0x24u: {  // LBU
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, sys.mem.read8(sys, addr));
        break;
    }
    case 0x25u: {  // LHU
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, sys.mem.read16(sys, addr));
        break;
    }
    case 0x26u: {  // LWR
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t cur = cur_for_unaligned(rt);
        const std::uint32_t word = sys.mem.read32(sys, addr & ~3u);
        std::uint32_t res = cur;
        switch (addr & 3u) {
        case 0u: res = word; break;
        case 1u: res = (cur & 0xFF000000u) | (word >> 8); break;
        case 2u: res = (cur & 0xFFFF0000u) | (word >> 16); break;
        case 3u: res = (cur & 0xFFFFFF00u) | (word >> 24); break;
        }
        load_reg_delayed(rt, res);
        break;
    }
    case 0x28u:  // SB
        sys.mem.write8(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), static_cast<std::uint8_t>(r_rt));
        break;
    case 0x29u:  // SH
        sys.mem.write16(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), static_cast<std::uint16_t>(r_rt));
        break;
    case 0x2Au: {  // SWL
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t mem = sys.mem.read32(sys, addr & ~3u);
        std::uint32_t val = mem;
        switch (addr & 3u) {
        case 0u: val = (mem & 0xFFFFFF00u) | (r_rt >> 24); break;
        case 1u: val = (mem & 0xFFFF0000u) | (r_rt >> 16); break;
        case 2u: val = (mem & 0xFF000000u) | (r_rt >> 8); break;
        case 3u: val = r_rt; break;
        }
        sys.mem.write32(sys, addr & ~3u, val);
        break;
    }
    case 0x2Bu:  // SW
        sys.mem.write32(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), r_rt);
        break;
    case 0x2Eu: {  // SWR
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t mem = sys.mem.read32(sys, addr & ~3u);
        std::uint32_t val = mem;
        switch (addr & 3u) {
        case 0u: val = r_rt; break;
        case 1u: val = (mem & 0x000000FFu) | (r_rt << 8); break;
        case 2u: val = (mem & 0x0000FFFFu) | (r_rt << 16); break;
        case 3u: val = (mem & 0x00FFFFFFu) | (r_rt << 24); break;
        }
        sys.mem.write32(sys, addr & ~3u, val);
        break;
    }
    case 0x32u: {  // LWC2 (load word directly to GTE data reg rt)
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        sys.gte.mtc2(rt, sys.mem.read32(sys, addr));
        break;
    }
    case 0x3Au: {  // SWC2 (store GTE data reg rt to memory)
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        sys.mem.write32(sys, addr, sys.gte.mfc2(rt));
        break;
    }
    default:
        break;
    }

    c.commit_load_delay();
    ++c.cycles;
    --c.budget;
}

void run_interpreter(System &sys, std::uint32_t stop_pc) {
    Cpu &c = sys.cpu;
    std::uint32_t hw_batch = 0;

    while (!c.halted && c.budget > 0) {
        if (stop_pc != 0 && c.pc == stop_pc) break;

        // Take hardware IRQ only when not inside a branch delay slot
        if (!c.next_in_delay_slot && c.irq_enabled()) {
            if (hw_batch > 0) { sys.step_hardware(hw_batch); hw_batch = 0; }
            sys.bios.handle_irq_exception(sys);
            continue;
        }

        // Try verified AOT function if not inside a delay slot and no pending load delay
        if (sys.aot_enabled && !c.next_in_delay_slot && c.load_reg == 0 && c.next_load_reg == 0) {
            if (AotFn fn = sys.lookup_aot(c.pc)) {
                if (hw_batch > 0) { sys.step_hardware(hw_batch); hw_batch = 0; }
                ++sys.aot_hits;
                fn(sys);
                continue;
            }
        }

        step_instruction(sys);
        ++hw_batch;
        if (hw_batch >= 64u) {
            sys.step_hardware(hw_batch);
            hw_batch = 0;
        }
    }

    if (hw_batch > 0) {
        sys.step_hardware(hw_batch);
    }
}

void dispatch(System &sys, std::uint32_t target_pc) {
    sys.cpu.flush_load_delay();
    sys.cpu.pc = target_pc;
    sys.cpu.npc = target_pc + 4u;

    const std::uint32_t masked = target_pc & 0x1FFFFFFFu;
    if (masked == 0xA0u || masked == 0xB0u || masked == 0xC0u) {
        sys.bios.handle_vector(sys, target_pc);
        return;
    }

    if (sys.aot_enabled) {
        if (AotFn fn = sys.lookup_aot(target_pc)) {
            ++sys.aot_hits;
            fn(sys);
            return;
        }
    }

    // Fallback: execute in interpreter until returning to caller's $ra or budget expires
    ++sys.interp_fallbacks;
    const std::uint32_t ret_pc = sys.cpu.r[31];
    run_interpreter(sys, ret_pc);
}

void call_guest_function(System &sys, std::uint32_t func_pc, std::uint32_t max_cycles) {
    const std::int32_t prev_budget = sys.cpu.budget;
    const std::uint32_t prev_sr = sys.cpu.cop0[kCop0Sr];
    sys.cpu.cop0[kCop0Sr] &= ~1u;  // Prevent recursive IRQ dispatch inside guest callbacks
    sys.cpu.r[31] = kCallbackReturnSentinel;
    sys.cpu.pc = func_pc;
    sys.cpu.npc = func_pc + 4u;
    sys.cpu.budget = static_cast<std::int32_t>(max_cycles);
    run_interpreter(sys, kCallbackReturnSentinel);
    sys.cpu.cop0[kCop0Sr] = prev_sr;
    sys.cpu.budget = prev_budget;
}

}  // namespace psx
