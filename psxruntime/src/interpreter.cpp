#include "psx/interpreter.hpp"
#include "psx/pgxp.hpp"
#include "psx/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psx {

namespace {

constexpr std::uint32_t kCallbackReturnSentinel = 0xBFC7FFF0u;

inline std::int32_t simm16(std::uint32_t w) noexcept {
    return static_cast<std::int16_t>(w & 0xFFFFu);
}

inline std::uint32_t uimm16(std::uint32_t w) noexcept {
    return w & 0xFFFFu;
}

inline std::uint32_t fast_fetch32(const System &sys, std::uint32_t vaddr) noexcept {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (__builtin_expect(phys < 0x00800000u, 1)) {
        std::uint32_t v = 0;
        std::memcpy(&v, sys.mem.ram + (phys & kRamMask), sizeof(v));
        return v;
    }
    return sys.mem.fetch32(vaddr);
}

inline std::uint8_t fast_read8(System &sys, std::uint32_t vaddr) {
    const std::uint32_t phys = virt_to_phys(vaddr);
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated(), 1)) {
        return sys.mem.ram[phys & kRamMask];
    }
    return sys.mem.read8(sys, vaddr);
}

inline std::uint16_t fast_read16(System &sys, std::uint32_t vaddr) {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~1u;
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated(), 1)) {
        std::uint16_t v = 0;
        std::memcpy(&v, sys.mem.ram + (phys & kRamMask), sizeof(v));
        return v;
    }
    return sys.mem.read16(sys, vaddr);
}

inline std::uint32_t fast_read32(System &sys, std::uint32_t vaddr) {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated(), 1)) {
        std::uint32_t v = 0;
        std::memcpy(&v, sys.mem.ram + (phys & kRamMask), sizeof(v));
        pgxp::on_mem_read32(phys, v);
        return v;
    }
    return sys.mem.read32(sys, vaddr);
}

inline void fast_write8(System &sys, std::uint32_t vaddr, std::uint8_t val) {
    const std::uint32_t phys = virt_to_phys(vaddr);
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated(), 1)) {
        const std::uint32_t off = phys & kRamMask;
        sys.mem.ram[off] = val;
        pgxp::invalidate_mem(phys);
        ++sys.mem.page_gen[off >> kCodePageShift];
        ++sys.mem.global_code_gen;
        return;
    }
    sys.mem.write8(sys, vaddr, val);
}

inline void fast_write16(System &sys, std::uint32_t vaddr, std::uint16_t val) {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~1u;
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated(), 1)) {
        const std::uint32_t off = phys & kRamMask;
        std::memcpy(sys.mem.ram + off, &val, sizeof(val));
        pgxp::invalidate_mem(phys);
        ++sys.mem.page_gen[off >> kCodePageShift];
        ++sys.mem.global_code_gen;
        return;
    }
    sys.mem.write16(sys, vaddr, val);
}

inline void fast_write32(System &sys, std::uint32_t vaddr, std::uint32_t val) {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (__builtin_expect(phys < 0x00800000u && !sys.cpu.cache_isolated() && vaddr != 0xFFFE0130u, 1)) {
        const std::uint32_t off = phys & kRamMask;
        std::memcpy(sys.mem.ram + off, &val, sizeof(val));
        pgxp::on_mem_write32(phys, val);
        ++sys.mem.page_gen[off >> kCodePageShift];
        ++sys.mem.global_code_gen;
        return;
    }
    sys.mem.write32(sys, vaddr, val);
}

inline void step_instruction_inline(System &sys) {
    Cpu &c = sys.cpu;
    const std::uint32_t pc = c.pc;

    // Intercept BIOS HLE vectors (0xA0 / 0xB0 / 0xC0)
    const std::uint32_t masked_pc = pc & 0x1FFFFFFFu;
    if (__builtin_expect(masked_pc <= 0xC0u && (masked_pc == 0xA0u || masked_pc == 0xB0u || masked_pc == 0xC0u), 0)) {
        if (sys.bios.handle_vector(sys, pc)) {
            ++c.cycles;
            --c.budget;
            return;
        }
    }

    const std::uint32_t w = fast_fetch32(sys, pc);
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
            if (rd == kCop0Cause) {
                // Only software interrupt bits IP0..IP1 (bits 8..9) are writable on R3000A.
                c.cop0[kCop0Cause] = (c.cop0[kCop0Cause] & ~0x00000300u) | (r_rt & 0x00000300u);
                sys.hw.update_cpu_irq(sys);
            } else if (rd != 15u) {  // COP0_PRID (reg 15) is read-only
                c.cop0[rd] = r_rt;
                if (rd == kCop0Sr) {
                    sys.hw.update_cpu_irq(sys);
                }
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
        load_reg_delayed(rt, static_cast<std::uint32_t>(static_cast<std::int8_t>(fast_read8(sys, addr))));
        break;
    }
    case 0x21u: {  // LH
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, static_cast<std::uint32_t>(static_cast<std::int16_t>(fast_read16(sys, addr))));
        break;
    }
    case 0x22u: {  // LWL
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t cur = cur_for_unaligned(rt);
        const std::uint32_t word = fast_read32(sys, addr & ~3u);
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
        load_reg_delayed(rt, fast_read32(sys, addr));
        break;
    }
    case 0x24u: {  // LBU
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, fast_read8(sys, addr));
        break;
    }
    case 0x25u: {  // LHU
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        load_reg_delayed(rt, fast_read16(sys, addr));
        break;
    }
    case 0x26u: {  // LWR
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t cur = cur_for_unaligned(rt);
        const std::uint32_t word = fast_read32(sys, addr & ~3u);
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
        fast_write8(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), static_cast<std::uint8_t>(r_rt));
        break;
    case 0x29u:  // SH
        fast_write16(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), static_cast<std::uint16_t>(r_rt));
        break;
    case 0x2Au: {  // SWL
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t mem = fast_read32(sys, addr & ~3u);
        std::uint32_t val = mem;
        switch (addr & 3u) {
        case 0u: val = (mem & 0xFFFFFF00u) | (r_rt >> 24); break;
        case 1u: val = (mem & 0xFFFF0000u) | (r_rt >> 16); break;
        case 2u: val = (mem & 0xFF000000u) | (r_rt >> 8); break;
        case 3u: val = r_rt; break;
        }
        fast_write32(sys, addr & ~3u, val);
        break;
    }
    case 0x2Bu:  // SW
        fast_write32(sys, r_rs + static_cast<std::uint32_t>(simm16(w)), r_rt);
        break;
    case 0x2Eu: {  // SWR
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        const std::uint32_t mem = fast_read32(sys, addr & ~3u);
        std::uint32_t val = mem;
        switch (addr & 3u) {
        case 0u: val = r_rt; break;
        case 1u: val = (mem & 0x000000FFu) | (r_rt << 8); break;
        case 2u: val = (mem & 0x0000FFFFu) | (r_rt << 16); break;
        case 3u: val = (mem & 0x00FFFFFFu) | (r_rt << 24); break;
        }
        fast_write32(sys, addr & ~3u, val);
        break;
    }
    case 0x32u: {  // LWC2 (load word directly to GTE data reg rt)
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        sys.gte.mtc2(rt, fast_read32(sys, addr));
        break;
    }
    case 0x3Au: {  // SWC2 (store GTE data reg rt to memory)
        const std::uint32_t addr = r_rs + static_cast<std::uint32_t>(simm16(w));
        fast_write32(sys, addr, sys.gte.mfc2(rt));
        break;
    }
    default:
        break;
    }

    c.commit_load_delay();
    ++c.cycles;
    --c.budget;
}

inline bool is_pure_idle_body_insn(std::uint32_t w) noexcept {
    const std::uint32_t op = w >> 26;
    if (op == 0x00u) {
        const std::uint32_t fn = w & 63u;
        return fn == 0x00u || fn == 0x02u || fn == 0x03u || fn == 0x04u ||
               fn == 0x06u || fn == 0x07u || fn == 0x10u || fn == 0x12u ||
               (fn >= 0x20u && fn <= 0x27u) || fn == 0x2Au || fn == 0x2Bu;
    }
    if (op >= 0x08u && op <= 0x0Fu) return true;
    return op == 0x20u || op == 0x21u || op == 0x23u || op == 0x24u || op == 0x25u;
}

inline bool is_pure_cond_branch_to(std::uint32_t w, std::uint32_t branch_pc, std::uint32_t target_pc) noexcept {
    const std::uint32_t op = w >> 26;
    const bool ok_op = (op >= 0x04u && op <= 0x07u) ||
                       (op == 0x01u && (((w >> 16) & 0x1Eu) == 0u));
    if (!ok_op) return false;
    const std::uint32_t dest = (branch_pc + 4u) + static_cast<std::uint32_t>(simm16(w) << 2);
    return dest == target_pc;
}

inline bool is_pure_load_op(std::uint32_t op) noexcept {
    return op == 0x20u || op == 0x21u || op == 0x23u || op == 0x24u || op == 0x25u;
}

}  // namespace

void step_instruction(System &sys) {
    step_instruction_inline(sys);
}

void run_interpreter(System &sys, std::uint32_t stop_pc) {
    Cpu &c = sys.cpu;
    std::uint32_t hw_batch = 0;
    const std::uint64_t *const aot_bits = sys.aot_present_bitset();
    std::uint32_t last_delay_pc = 0;

    while (!c.halted && c.budget > 0) {
        if (stop_pc != 0 && c.pc == stop_pc) break;

        // Take hardware IRQ only when not inside a branch delay slot
        if (__builtin_expect(!c.next_in_delay_slot && c.irq_enabled(), 0)) {
            if (hw_batch > 0) { sys.step_hardware(hw_batch); hw_batch = 0; }
            sys.bios.handle_irq_exception(sys);
            last_delay_pc = 0;
            continue;
        }

        // Try verified AOT function if not inside a delay slot and no pending load delay.
        // O(1) L1-resident bitset test skips lookup_aot() completely on non-AOT overlay/FMV PCs.
        if (aot_bits && !c.next_in_delay_slot && c.load_reg == 0 && c.next_load_reg == 0) {
            const std::uint32_t phys = virt_to_phys(c.pc);
            if (phys < kRamSize && ((aot_bits[phys >> 8] >> ((phys >> 2) & 63u)) & 1ull)) {
                if (AotFn fn = sys.lookup_aot(c.pc)) {
                    if (hw_batch > 0) { sys.step_hardware(hw_batch); hw_batch = 0; }
                    ++sys.aot_hits;
                    fn(sys);
                    last_delay_pc = 0;
                    continue;
                }
            }
        }

        // Generic state-invariant RAM-polling idle-loop fast-forward:
        // If a tight backward conditional branch (<= 9 insns, pure ALU/RAM-loads, zero stores/I/O)
        // completes one full iteration with bit-for-bit identical CPU registers, fast-forward
        // remaining scanline budget while stepping hardware at the exact same cadence until IRQ/budget.
        if (__builtin_expect(last_delay_pc != 0u && c.pc < last_delay_pc &&
                             !c.next_in_delay_slot && c.load_reg == 0 && c.next_load_reg == 0 &&
                             (c.cop0[kCop0Sr] & 1u) != 0u && !c.cache_isolated(), 0)) {
            const std::uint32_t target_pc = c.pc;
            const std::uint32_t branch_pc = last_delay_pc - 4u;
            const std::uint32_t span = branch_pc - target_pc;
            const std::uint32_t loop_len = (span >> 2) + 2u;
            last_delay_pc = 0;

            if (target_pc < branch_pc && span <= 28u && (span & 3u) == 0u &&
                c.budget >= static_cast<std::int32_t>(loop_len * 2u) &&
                (stop_pc == 0u || stop_pc < target_pc || stop_pc > branch_pc + 4u)) {
                const std::uint32_t phys_start = virt_to_phys(target_pc);
                bool static_ok = (phys_start + loop_len * 4u <= kRamSize);
                if (static_ok) {
                    static_ok = is_pure_cond_branch_to(fast_fetch32(sys, branch_pc), branch_pc, target_pc) &&
                                is_pure_idle_body_insn(fast_fetch32(sys, branch_pc + 4u));
                    for (std::uint32_t a = target_pc; static_ok && a < branch_pc; a += 4u) {
                        static_ok = is_pure_idle_body_insn(fast_fetch32(sys, a));
                    }
                }
                if (static_ok) {
                    std::uint32_t saved_r[32];
                    std::memcpy(saved_r, c.r, sizeof(saved_r));
                    const std::uint32_t saved_hi = c.hi;
                    const std::uint32_t saved_lo = c.lo;
                    bool dyn_ok = true;

                    for (std::uint32_t s = 0; s < loop_len; ++s) {
                        const std::uint32_t cur_w = fast_fetch32(sys, c.pc);
                        const std::uint32_t op = cur_w >> 26;
                        if (is_pure_load_op(op)) {
                            const std::uint32_t rs = (cur_w >> 21) & 31u;
                            const std::uint32_t eff = c.r[rs] + static_cast<std::uint32_t>(simm16(cur_w));
                            const std::uint32_t p = virt_to_phys(eff);
                            if (!(p < 0x00800000u || (p >= 0x1F800000u && p < 0x1F800400u))) {
                                dyn_ok = false;
                            }
                        }
                        step_instruction_inline(sys);
                        ++hw_batch;
                        if (hw_batch >= 64u) {
                            sys.step_hardware(hw_batch);
                            hw_batch = 0;
                        }
                        if (c.halted || (!c.next_in_delay_slot && c.irq_enabled())) {
                            dyn_ok = false;
                            break;
                        }
                    }

                    if (dyn_ok && c.pc == target_pc && !c.next_in_delay_slot &&
                        c.load_reg == 0 && c.next_load_reg == 0 &&
                        c.hi == saved_hi && c.lo == saved_lo &&
                        std::memcmp(saved_r, c.r, sizeof(saved_r)) == 0) {
                        if (hw_batch > 0) {
                            sys.step_hardware(hw_batch);
                            hw_batch = 0;
                        }
                        const std::uint32_t max_iters = std::max<std::uint32_t>(1u, 64u / loop_len);
                        while (!c.halted && c.budget >= static_cast<std::int32_t>(loop_len) && !c.irq_enabled()) {
                            const std::uint32_t avail_iters = static_cast<std::uint32_t>(c.budget) / loop_len;
                            const std::uint32_t iters = std::min<std::uint32_t>(avail_iters, max_iters);
                            const std::uint32_t step = iters * loop_len;
                            c.cycles += step;
                            c.budget -= static_cast<std::int32_t>(step);
                            sys.step_hardware(step);
                        }
                    }
                    continue;
                }
            }
        } else {
            last_delay_pc = 0;
        }

        const bool was_delay = c.next_in_delay_slot;
        const std::uint32_t step_pc = c.pc;
        step_instruction_inline(sys);
        if (was_delay) {
            last_delay_pc = step_pc;
        }
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
