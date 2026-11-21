#include "codegen.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace psxrecomp {

namespace {

inline std::int32_t simm16(std::uint32_t w) noexcept {
    return static_cast<std::int16_t>(w & 0xFFFFu);
}

inline std::uint32_t uimm16(std::uint32_t w) noexcept {
    return w & 0xFFFFu;
}

inline std::uint32_t jump_target(std::uint32_t pc, std::uint32_t w) noexcept {
    return ((pc + 4u) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
}

inline std::uint32_t branch_target(std::uint32_t pc, std::uint32_t w) noexcept {
    return (pc + 4u) + static_cast<std::uint32_t>(simm16(w) << 2);
}

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

std::string fn_name(const std::string &tag, std::uint32_t va) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "fn_%s%08X", tag.c_str(), va);
    return buf;
}

std::string lbl_name(std::uint32_t va) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "L_%08X", va);
    return buf;
}

// Check if instruction `w` reads register `reg` (ignoring consecutive LWL/LWR to same reg, which forwards on R3000A).
bool reads_reg(std::uint32_t w, std::uint32_t reg) noexcept {
    if (reg == 0) return false;
    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;
    const std::uint32_t fn = w & 63u;

    switch (op) {
    case 0x00u:
        switch (fn) {
        case 0x00u: case 0x02u: case 0x03u: return rt == reg;
        case 0x04u: case 0x06u: case 0x07u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu: return rs == reg || rt == reg;
        case 0x08u: case 0x09u: case 0x11u: case 0x13u: return rs == reg;
        default: return false;
        }
    case 0x01u: case 0x06u: case 0x07u: return rs == reg;
    case 0x04u: case 0x05u: return rs == reg || rt == reg;
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: return rs == reg;
    case 0x10u: return rs == 4u && rt == reg;  // MTC0
    case 0x12u: return !(w & (1u << 25)) && (rs == 4u || rs == 6u) && rt == reg;  // MTC2/CTC2
    case 0x20u: case 0x21u: case 0x23u: case 0x24u: case 0x25u: case 0x32u: return rs == reg;
    case 0x22u: case 0x26u: return rs == reg;  // Note: LWL/LWR rt forwards pending load value on R3000A!
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu: case 0x2Eu: return rs == reg || rt == reg;
    case 0x3Au: return rs == reg;
    default: return false;
    }
}

// Emit a single non-branch instruction. If `subst_reg != 0`, reads of `subst_reg` use `subst_expr`.
void emit_single(std::ostringstream &out, std::uint32_t w,
                 std::uint32_t subst_reg = 0, const char *subst_expr = nullptr) {
    if (w == 0) return;  // NOP
    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;
    const std::uint32_t rd = (w >> 11) & 31u;
    const std::uint32_t sa = (w >> 6) & 31u;
    const std::uint32_t fn = w & 63u;

    auto R = [&](std::uint32_t reg) -> std::string {
        if (reg == 0) return "0u";
        if (subst_reg != 0 && reg == subst_reg && subst_expr) return subst_expr;
        return "c.r[" + std::to_string(reg) + "]";
    };

    auto addr_expr = [&]() -> std::string {
        const std::int32_t imm = simm16(w);
        if (imm == 0) return R(rs);
        return "(" + R(rs) + " + " + hex32(static_cast<std::uint32_t>(imm)) + ")";
    };

    switch (op) {
    case 0x00u:
        switch (fn) {
        case 0x00u: if (rd) out << "    c.r[" << rd << "] = " << R(rt) << " << " << sa << ";\n"; break;
        case 0x02u: if (rd) out << "    c.r[" << rd << "] = " << R(rt) << " >> " << sa << ";\n"; break;
        case 0x03u: if (rd) out << "    c.r[" << rd << "] = std::uint32_t(std::int32_t(" << R(rt) << ") >> " << sa << ");\n"; break;
        case 0x04u: if (rd) out << "    c.r[" << rd << "] = " << R(rt) << " << (" << R(rs) << " & 31u);\n"; break;
        case 0x06u: if (rd) out << "    c.r[" << rd << "] = " << R(rt) << " >> (" << R(rs) << " & 31u);\n"; break;
        case 0x07u: if (rd) out << "    c.r[" << rd << "] = std::uint32_t(std::int32_t(" << R(rt) << ") >> (" << R(rs) << " & 31u));\n"; break;
        case 0x0Cu: out << "    sys.bios.handle_syscall(sys);\n"; break;
        case 0x0Du: break;  // BREAK
        case 0x10u: if (rd) out << "    c.r[" << rd << "] = c.hi;\n"; break;
        case 0x11u: out << "    c.hi = " << R(rs) << ";\n"; break;
        case 0x12u: if (rd) out << "    c.r[" << rd << "] = c.lo;\n"; break;
        case 0x13u: out << "    c.lo = " << R(rs) << ";\n"; break;
        case 0x18u: out << "    psx::r3000a_mult(c, " << R(rs) << ", " << R(rt) << ");\n"; break;
        case 0x19u: out << "    psx::r3000a_multu(c, " << R(rs) << ", " << R(rt) << ");\n"; break;
        case 0x1Au: out << "    psx::r3000a_div(c, " << R(rs) << ", " << R(rt) << ");\n"; break;
        case 0x1Bu: out << "    psx::r3000a_divu(c, " << R(rs) << ", " << R(rt) << ");\n"; break;
        case 0x20u:
        case 0x21u: if (rd) out << "    c.r[" << rd << "] = " << R(rs) << " + " << R(rt) << ";\n"; break;
        case 0x22u:
        case 0x23u: if (rd) out << "    c.r[" << rd << "] = " << R(rs) << " - " << R(rt) << ";\n"; break;
        case 0x24u: if (rd) out << "    c.r[" << rd << "] = " << R(rs) << " & " << R(rt) << ";\n"; break;
        case 0x25u: if (rd) out << "    c.r[" << rd << "] = " << R(rs) << " | " << R(rt) << ";\n"; break;
        case 0x26u: if (rd) out << "    c.r[" << rd << "] = " << R(rs) << " ^ " << R(rt) << ";\n"; break;
        case 0x27u: if (rd) out << "    c.r[" << rd << "] = ~(" << R(rs) << " | " << R(rt) << ");\n"; break;
        case 0x2Au: if (rd) out << "    c.r[" << rd << "] = (std::int32_t(" << R(rs) << ") < std::int32_t(" << R(rt) << ")) ? 1u : 0u;\n"; break;
        case 0x2Bu: if (rd) out << "    c.r[" << rd << "] = (" << R(rs) << " < " << R(rt) << ") ? 1u : 0u;\n"; break;
        default: break;
        }
        break;

    case 0x08u:
    case 0x09u: if (rt) out << "    c.r[" << rt << "] = " << R(rs) << " + " << hex32(static_cast<std::uint32_t>(simm16(w))) << ";\n"; break;
    case 0x0Au: if (rt) out << "    c.r[" << rt << "] = (std::int32_t(" << R(rs) << ") < " << simm16(w) << ") ? 1u : 0u;\n"; break;
    case 0x0Bu: if (rt) out << "    c.r[" << rt << "] = (" << R(rs) << " < " << hex32(static_cast<std::uint32_t>(simm16(w))) << ") ? 1u : 0u;\n"; break;
    case 0x0Cu: if (rt) out << "    c.r[" << rt << "] = " << R(rs) << " & " << hex32(uimm16(w)) << ";\n"; break;
    case 0x0Du: if (rt) out << "    c.r[" << rt << "] = " << R(rs) << " | " << hex32(uimm16(w)) << ";\n"; break;
    case 0x0Eu: if (rt) out << "    c.r[" << rt << "] = " << R(rs) << " ^ " << hex32(uimm16(w)) << ";\n"; break;
    case 0x0Fu: if (rt) out << "    c.r[" << rt << "] = " << hex32(uimm16(w) << 16) << ";\n"; break;

    case 0x10u:
        if (rs == 0u && rt) out << "    c.r[" << rt << "] = c.cop0[" << rd << "];\n";
        else if (rs == 4u) out << "    psx::cop0_mtc0(sys, " << rd << "u, " << R(rt) << ");\n";
        else if (rs == 0x10u && fn == 0x10u) out << "    psx::cop0_rfe(sys);\n";
        break;

    case 0x12u:
        if (w & (1u << 25)) {
            out << "    sys.gte.execute(" << hex32(w & 0x01FFFFFFu) << ");\n";
        } else if (rs == 0u && rt) {
            out << "    c.r[" << rt << "] = sys.gte.mfc2(" << rd << "u);\n";
        } else if (rs == 2u && rt) {
            out << "    c.r[" << rt << "] = sys.gte.cfc2(" << rd << "u);\n";
        } else if (rs == 4u) {
            out << "    sys.gte.mtc2(" << rd << "u, " << R(rt) << ");\n";
        } else if (rs == 6u) {
            out << "    sys.gte.ctc2(" << rd << "u, " << R(rt) << ");\n";
        }
        break;

    case 0x20u:
        if (rt) out << "    c.r[" << rt << "] = std::uint32_t(std::int8_t(sys.mem.read8(sys, " << addr_expr() << ")));\n";
        else out << "    (void)sys.mem.read8(sys, " << addr_expr() << ");\n";
        break;
    case 0x21u:
        if (rt) out << "    c.r[" << rt << "] = std::uint32_t(std::int16_t(sys.mem.read16(sys, " << addr_expr() << ")));\n";
        else out << "    (void)sys.mem.read16(sys, " << addr_expr() << ");\n";
        break;
    case 0x22u:
        if (rt) out << "    c.r[" << rt << "] = psx::load_lwl(sys, " << addr_expr() << ", c.r[" << rt << "]);\n";
        break;
    case 0x23u:
        if (rt) out << "    c.r[" << rt << "] = sys.mem.read32(sys, " << addr_expr() << ");\n";
        else out << "    (void)sys.mem.read32(sys, " << addr_expr() << ");\n";
        break;
    case 0x24u:
        if (rt) out << "    c.r[" << rt << "] = sys.mem.read8(sys, " << addr_expr() << ");\n";
        else out << "    (void)sys.mem.read8(sys, " << addr_expr() << ");\n";
        break;
    case 0x25u:
        if (rt) out << "    c.r[" << rt << "] = sys.mem.read16(sys, " << addr_expr() << ");\n";
        else out << "    (void)sys.mem.read16(sys, " << addr_expr() << ");\n";
        break;
    case 0x26u:
        if (rt) out << "    c.r[" << rt << "] = psx::load_lwr(sys, " << addr_expr() << ", c.r[" << rt << "]);\n";
        break;
    case 0x28u: out << "    sys.mem.write8(sys, " << addr_expr() << ", std::uint8_t(" << R(rt) << "));\n"; break;
    case 0x29u: out << "    sys.mem.write16(sys, " << addr_expr() << ", std::uint16_t(" << R(rt) << "));\n"; break;
    case 0x2Au: out << "    psx::store_swl(sys, " << addr_expr() << ", " << R(rt) << ");\n"; break;
    case 0x2Bu: out << "    sys.mem.write32(sys, " << addr_expr() << ", " << R(rt) << ");\n"; break;
    case 0x2Eu: out << "    psx::store_swr(sys, " << addr_expr() << ", " << R(rt) << ");\n"; break;
    case 0x32u: out << "    sys.gte.mtc2(" << rt << "u, sys.mem.read32(sys, " << addr_expr() << "));\n"; break;
    case 0x3Au: out << "    sys.mem.write32(sys, " << addr_expr() << ", sys.gte.mfc2(" << rt << "u));\n"; break;
    default: break;
    }
}

std::string branch_cond_expr(std::uint32_t w, std::uint32_t subst_reg, const char *subst_expr) {
    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;

    auto R = [&](std::uint32_t reg) -> std::string {
        if (reg == 0) return "0u";
        if (subst_reg != 0 && reg == subst_reg && subst_expr) return subst_expr;
        return "c.r[" + std::to_string(reg) + "]";
    };

    switch (op) {
    case 0x01u: return (rt & 1u) ? ("(std::int32_t(" + R(rs) + ") >= 0)") : ("(std::int32_t(" + R(rs) + ") < 0)");
    case 0x04u: return "(" + R(rs) + " == " + R(rt) + ")";
    case 0x05u: return "(" + R(rs) + " != " + R(rt) + ")";
    case 0x06u: return "(std::int32_t(" + R(rs) + ") <= 0)";
    case 0x07u: return "(std::int32_t(" + R(rs) + ") > 0)";
    default: return "false";
    }
}

}  // namespace

std::string emit_function_cpp(const PsxBinary &bin, const Function &fn,
                              const std::unordered_set<std::uint32_t> &known_fns) {
    std::ostringstream out;
    out << "void " << fn_name(fn.tag, fn.start) << "(psx::System &sys) {\n";
    out << "    psx::Cpu &c = sys.cpu;\n";
    std::uint32_t pending_cycles = 0;

    auto flush_cycles = [&]() {
        if (pending_cycles > 0) {
            out << "    sys.consume_cycles(" << pending_cycles << "u);\n";
            pending_cycles = 0;
        }
    };

    std::uint32_t pc = fn.start;
    while (pc < fn.end) {
        if (fn.labels.count(pc)) {
            flush_cycles();
            out << lbl_name(pc) << ":\n";
        }

        const std::uint32_t w = bin.word_at(pc);
        const std::uint32_t op = w >> 26;
        const std::uint32_t rs = (w >> 21) & 31u;
        const std::uint32_t rt = (w >> 16) & 31u;
        const std::uint32_t rd = (w >> 11) & 31u;
        const std::uint32_t fn_code = w & 63u;

        // Check for R3000A load-delay hazard with the immediately following instruction!
        std::uint32_t load_dst = 0;
        if (!is_branch_or_jump(w) && is_load_instruction(w, load_dst) &&
            pc + 4u < fn.end && !fn.labels.count(pc + 4u)) {
            const std::uint32_t next_w = bin.word_at(pc + 4u);
            if (reads_reg(next_w, load_dst)) {
                // Save old value of load_dst before executing the load, then feed it into next_w!
                out << "    { const std::uint32_t _ld = c.r[" << load_dst << "];\n";
                emit_single(out, w);
                ++pending_cycles;
                if (!is_branch_or_jump(next_w)) {
                    emit_single(out, next_w, load_dst, "_ld");
                    out << "    }\n";
                    ++pending_cycles;
                    pc += 8u;
                    continue;
                } else {
                    // Next instruction is a branch/jump reading _ld! Handle branch with _ld right here.
                    const std::uint32_t bpc = pc + 4u;
                    const std::uint32_t bop = next_w >> 26;
                    const std::uint32_t brs = (next_w >> 21) & 31u;
                    const std::uint32_t brt = (next_w >> 16) & 31u;
                    const std::uint32_t brd = (next_w >> 11) & 31u;
                    const std::uint32_t bfn = next_w & 63u;
                    const std::uint32_t dw = (bpc + 4u < fn.end) ? bin.word_at(bpc + 4u) : 0u;
                    pending_cycles += 2u;

                    if (bop == 0x00u && bfn == 0x08u) {  // JR reading _ld
                        out << "    const std::uint32_t _tgt = " << ((brs == load_dst) ? "_ld" : ("c.r[" + std::to_string(brs) + "]")) << ";\n";
                        emit_single(out, dw);
                        flush_cycles();
                        if (brs == 31u) {
                            out << "    c.pc = _tgt; c.npc = _tgt + 4u; return;\n    }\n";
                        } else {
                            out << "    psx::tail_jump(sys, _tgt); return;\n    }\n";
                        }
                    } else if (bop == 0x00u && bfn == 0x09u) {  // JALR reading _ld
                        out << "    const std::uint32_t _tgt = " << ((brs == load_dst) ? "_ld" : ("c.r[" + std::to_string(brs) + "]")) << ";\n";
                        if (brd) out << "    c.r[" << brd << "] = " << hex32(bpc + 8u) << ";\n";
                        emit_single(out, dw);
                        flush_cycles();
                        out << "    psx::call_target(sys, _tgt);\n";
                        out << "    if (sys.should_yield()) { c.pc = " << hex32(bpc + 8u) << "; c.npc = " << hex32(bpc + 12u) << "; return; }\n    }\n";
                    } else {  // Conditional branch reading _ld
                        const std::uint32_t tgt = branch_target(bpc, next_w);
                        out << "    const bool _br = " << branch_cond_expr(next_w, load_dst, "_ld") << ";\n";
                        if (bop == 0x01u && (brt & 0x1Eu) == 0x10u) {
                            out << "    c.r[31] = " << hex32(bpc + 8u) << ";\n";
                        }
                        emit_single(out, dw);
                        flush_cycles();
                        out << "    if (_br) {\n";
                        if (tgt <= bpc) {
                            out << "        if (sys.should_yield()) { c.pc = " << hex32(tgt) << "; c.npc = " << hex32(tgt + 4u) << "; return; }\n";
                        }
                        if (tgt >= fn.start && tgt < fn.end && fn.labels.count(tgt)) {
                            out << "        goto " << lbl_name(tgt) << ";\n";
                        } else {
                            out << "        psx::tail_jump(sys, " << hex32(tgt) << "); return;\n";
                        }
                        out << "    }\n    }\n";
                    }
                    pc += 12u;
                    continue;
                }
            }
        }

        if (is_branch_or_jump(w)) {
            const std::uint32_t dw = (pc + 4u < fn.end) ? bin.word_at(pc + 4u) : 0u;
            pending_cycles += 2u;

            if (op == 0x02u) {  // J
                const std::uint32_t tgt = jump_target(pc, w);
                emit_single(out, dw);
                flush_cycles();
                if (tgt <= pc) {
                    out << "    if (sys.should_yield()) { c.pc = " << hex32(tgt) << "; c.npc = " << hex32(tgt + 4u) << "; return; }\n";
                }
                if (tgt >= fn.start && tgt < fn.end && fn.labels.count(tgt)) {
                    out << "    goto " << lbl_name(tgt) << ";\n";
                } else {
                    out << "    psx::tail_jump(sys, " << hex32(tgt) << "); return;\n";
                }
            } else if (op == 0x03u) {  // JAL
                const std::uint32_t tgt = jump_target(pc, w);
                out << "    c.r[31] = " << hex32(pc + 8u) << ";\n";
                emit_single(out, dw);
                flush_cycles();
                if (known_fns.count(tgt)) {
                    out << "    " << fn_name(fn.tag, tgt) << "(sys);\n";
                } else {
                    out << "    psx::call_target(sys, " << hex32(tgt) << ");\n";
                }
                out << "    if (sys.should_yield()) { c.pc = " << hex32(pc + 8u) << "; c.npc = " << hex32(pc + 12u) << "; return; }\n";
            } else if (op == 0x00u && fn_code == 0x08u) {  // JR
                out << "    { const std::uint32_t _tgt = " << (rs ? ("c.r[" + std::to_string(rs) + "]") : "0u") << ";\n";
                emit_single(out, dw);
                flush_cycles();
                if (rs == 31u) {
                    out << "    c.pc = _tgt; c.npc = _tgt + 4u; return; }\n";
                } else {
                    auto sw_it = fn.switch_tables.find(pc);
                    if (sw_it != fn.switch_tables.end() && !sw_it->second.empty()) {
                        out << "    switch (_tgt) {\n";
                        for (std::uint32_t tgt : sw_it->second) {
                            out << "    case " << hex32(tgt) << ": goto " << lbl_name(tgt) << ";\n";
                        }
                        out << "    default: psx::tail_jump(sys, _tgt); return;\n    } }\n";
                    } else {
                        out << "    psx::tail_jump(sys, _tgt); return; }\n";
                    }
                }
            } else if (op == 0x00u && fn_code == 0x09u) {  // JALR
                out << "    { const std::uint32_t _tgt = " << (rs ? ("c.r[" + std::to_string(rs) + "]") : "0u") << ";\n";
                if (rd) out << "    c.r[" << rd << "] = " << hex32(pc + 8u) << ";\n";
                emit_single(out, dw);
                flush_cycles();
                out << "    psx::call_target(sys, _tgt);\n";
                out << "    if (sys.should_yield()) { c.pc = " << hex32(pc + 8u) << "; c.npc = " << hex32(pc + 12u) << "; return; } }\n";
            } else {  // Conditional branch
                const std::uint32_t tgt = branch_target(pc, w);
                out << "    { const bool _br = " << branch_cond_expr(w, 0, nullptr) << ";\n";
                if (op == 0x01u && (rt & 0x1Eu) == 0x10u) {
                    out << "    c.r[31] = " << hex32(pc + 8u) << ";\n";
                }
                emit_single(out, dw);
                flush_cycles();
                out << "    if (_br) {\n";
                if (tgt <= pc) {
                    out << "        if (sys.should_yield()) { c.pc = " << hex32(tgt) << "; c.npc = " << hex32(tgt + 4u) << "; return; }\n";
                }
                if (tgt >= fn.start && tgt < fn.end && fn.labels.count(tgt)) {
                    out << "        goto " << lbl_name(tgt) << ";\n";
                } else {
                    out << "        psx::tail_jump(sys, " << hex32(tgt) << "); return;\n";
                }
                out << "    } }\n";
            }
            pc += 8u;
            continue;
        }

        emit_single(out, w);
        ++pending_cycles;
        pc += 4u;
    }

    flush_cycles();
    out << "    c.pc = " << hex32(fn.end) << "; c.npc = " << hex32(fn.end + 4u) << ";\n";
    out << "}\n";
    return out.str();
}

bool emit_cpp_project(const std::vector<ModulePair> &modules,
                      const CodegenOptions &opts, std::string &err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(opts.out_dir, ec);
    if (ec) {
        err = "Cannot create output directory: " + opts.out_dir;
        return false;
    }

    // 1. Emit shared header psx_units.hpp
    {
        std::ofstream hdr(fs::path(opts.out_dir) / "psx_units.hpp", std::ios::trunc);
        if (!hdr.is_open()) { err = "Cannot write psx_units.hpp"; return false; }
        hdr << "#pragma once\n\n";
        hdr << "#include \"psx/recomp_helpers.hpp\"\n\n";
        hdr << "namespace " << opts.ns << " {\n\n";
        for (const auto &[bin, analysis] : modules) {
            (void)bin;
            for (const auto &f : analysis.functions) {
                hdr << "void " << fn_name(f.tag, f.start) << "(psx::System &sys);\n";
            }
        }
        hdr << "\nvoid register_all(psx::System &sys);\n";
        hdr << "\n}  // namespace " << opts.ns << "\n";
    }

    // 2. Partition functions across N translation units for parallel low-RAM Clang builds
    struct Item {
        const PsxBinary *bin;
        const AnalysisResult *analysis;
        const Function *fn;
    };
    const std::uint32_t n_units = std::max<std::uint32_t>(1u, opts.unit_count);
    std::vector<std::vector<Item>> buckets(n_units);
    std::size_t global_idx = 0;
    for (const auto &[bin, analysis] : modules) {
        for (const auto &f : analysis.functions) {
            buckets[global_idx % n_units].push_back(Item{&bin, &analysis, &f});
            ++global_idx;
        }
    }

    for (std::uint32_t u = 0; u < n_units; ++u) {
        char uname[32];
        std::snprintf(uname, sizeof(uname), "unit_%03u.cpp", u);
        std::ofstream ofs(fs::path(opts.out_dir) / uname, std::ios::trunc);
        if (!ofs.is_open()) { err = std::string("Cannot write ") + uname; return false; }
        ofs << "#include \"psx_units.hpp\"\n\n";
        ofs << "namespace " << opts.ns << " {\n\n";
        for (const Item &item : buckets[u]) {
            ofs << emit_function_cpp(*item.bin, *item.fn, item.analysis->function_starts) << "\n";
        }
        ofs << "}  // namespace " << opts.ns << "\n";
    }

    // 3. Emit registry.cpp registering every AOT entry with its FNV-1a RAM hash
    {
        std::ofstream reg(fs::path(opts.out_dir) / "registry.cpp", std::ios::trunc);
        if (!reg.is_open()) { err = "Cannot write registry.cpp"; return false; }
        reg << "#include \"psx_units.hpp\"\n\n";
        reg << "namespace " << opts.ns << " {\n\n";
        reg << "void register_all(psx::System &sys) {\n";
        reg << "    static const psx::AotEntry kEntries[] = {\n";
        for (const auto &[bin, analysis] : modules) {
            (void)bin;
            for (const auto &f : analysis.functions) {
                char line[160];
                std::snprintf(line, sizeof(line),
                              "        { 0x%08Xu, 0x%Xu, 0x%016llXull, &%s, 0u, false },\n",
                              f.start, f.end - f.start,
                              static_cast<unsigned long long>(f.code_hash),
                              fn_name(f.tag, f.start).c_str());
                reg << line;
            }
        }
        reg << "    };\n";
        reg << "    sys.register_aot(kEntries, sizeof(kEntries) / sizeof(kEntries[0]));\n";
        reg << "}\n\n";
        reg << "}  // namespace " << opts.ns << "\n";
    }

    return true;
}

}  // namespace psxrecomp
