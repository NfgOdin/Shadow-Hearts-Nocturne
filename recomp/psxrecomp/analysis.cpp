#include "analysis.hpp"
#include "psx/types.hpp"

#include <algorithm>
#include <queue>

namespace psxrecomp {

namespace {

inline std::int32_t simm16(std::uint32_t w) noexcept {
    return static_cast<std::int16_t>(w & 0xFFFFu);
}

inline std::uint32_t jump_target(std::uint32_t pc, std::uint32_t w) noexcept {
    return ((pc + 4u) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
}

inline std::uint32_t branch_target(std::uint32_t pc, std::uint32_t w) noexcept {
    return (pc + 4u) + static_cast<std::uint32_t>(simm16(w) << 2);
}

}  // namespace

bool is_valid_r3000a(std::uint32_t w) noexcept {
    if (w == 0) return true;  // NOP
    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;
    const std::uint32_t sa = (w >> 6) & 31u;
    const std::uint32_t fn = w & 63u;

    switch (op) {
    case 0x00u:
        switch (fn) {
        case 0x00u: case 0x02u: case 0x03u: return rs == 0;
        case 0x04u: case 0x06u: case 0x07u: return sa == 0;
        case 0x08u: return rt == 0 && ((w >> 11) & 31u) == 0 && sa == 0;
        case 0x09u: return rt == 0 && sa == 0;
        case 0x0Cu: case 0x0Du: return true;
        case 0x10u: case 0x12u: return rs == 0 && rt == 0 && sa == 0;
        case 0x11u: case 0x13u: return rt == 0 && ((w >> 11) & 31u) == 0 && sa == 0;
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu: return ((w >> 11) & 31u) == 0 && sa == 0;
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu: return sa == 0;
        default: return false;
        }
    case 0x01u: return (rt == 0u || rt == 1u || rt == 0x10u || rt == 0x11u);
    case 0x02u: case 0x03u: case 0x04u: case 0x05u:
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: return true;
    case 0x06u: case 0x07u: return rt == 0;
    case 0x0Fu: return rs == 0;
    case 0x10u:  // COP0
        return rs == 0u || rs == 4u || (rs == 0x10u && fn == 0x10u);
    case 0x12u:  // COP2 (GTE)
        if (w & (1u << 25)) return true;
        return (rs == 0u || rs == 2u || rs == 4u || rs == 6u) && (w & 0x7FFu) == 0;
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u:
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu: case 0x2Eu:
    case 0x32u: case 0x3Au: return true;
    default: return false;
    }
}

bool is_branch_or_jump(std::uint32_t w) noexcept {
    const std::uint32_t op = w >> 26;
    if (op >= 0x01u && op <= 0x07u) return true;
    if (op == 0x00u) {
        const std::uint32_t fn = w & 63u;
        return fn == 0x08u || fn == 0x09u;
    }
    return false;
}

bool is_load_instruction(std::uint32_t w, std::uint32_t &dst_reg) noexcept {
    const std::uint32_t op = w >> 26;
    const std::uint32_t rs = (w >> 21) & 31u;
    const std::uint32_t rt = (w >> 16) & 31u;
    if (op >= 0x20u && op <= 0x26u) {
        dst_reg = rt;
        return rt != 0;
    }
    if (op == 0x10u && rs == 0u) {
        dst_reg = rt;
        return rt != 0;
    }
    if (op == 0x12u && !(w & (1u << 25)) && (rs == 0u || rs == 2u)) {
        dst_reg = rt;
        return rt != 0;
    }
    dst_reg = 0;
    return false;
}

AnalysisResult analyze_binary(const PsxBinary &bin, const std::string &tag) {
    AnalysisResult res;
    if (bin.text_size < 4) return res;

    const std::uint32_t start_va = bin.load_addr;
    const std::uint32_t end_va = bin.load_addr + bin.text_size;

    std::set<std::uint32_t> candidates;
    if (bin.contains(bin.entry_pc) && is_valid_r3000a(bin.word_at(bin.entry_pc)) && bin.word_at(bin.entry_pc) >= 0x00010000u) {
        candidates.insert(bin.entry_pc);
    }

    // For overlays, check the vtable/dispatch pointers in the first 256 bytes of the header
    if (!tag.empty()) {
        const std::uint32_t hdr_end = std::min(end_va, start_va + 256u);
        for (std::uint32_t va = start_va; va < hdr_end; va += 4u) {
            const std::uint32_t ptr = bin.word_at(va);
            if ((ptr & 3u) == 0 && bin.contains(ptr) && ptr >= start_va + 32u) {
                if (is_valid_r3000a(bin.word_at(ptr)) && bin.word_at(ptr) != 0) {
                    candidates.insert(ptr);
                }
            }
        }
    }

    // Helper: check if `va` sits right after a function terminator (jr/j + delay slot, or zero padding)
    auto is_after_terminator = [&](std::uint32_t va) -> bool {
        if (va == start_va) return true;
        std::uint32_t p = va - 4u;
        while (p > start_va && bin.word_at(p) == 0u) {
            // Check if the zero word is the delay slot of a jr/j at p - 4
            const std::uint32_t prev = bin.word_at(p - 4u);
            const std::uint32_t pop = prev >> 26;
            if (pop == 0x02u || (pop == 0x00u && (prev & 63u) == 0x08u)) return true;
            p -= 4u;
        }
        if (va >= start_va + 8u) {
            const std::uint32_t prev2 = bin.word_at(va - 8u);
            const std::uint32_t pop2 = prev2 >> 26;
            if (pop2 == 0x02u || (pop2 == 0x00u && (prev2 & 63u) == 0x08u)) return true;
        }
        return false;
    };

    // Pass 1: Discover all JAL targets, function pointer table entries, and clean prologues.
    for (std::uint32_t va = start_va; va < end_va; va += 4u) {
        const std::uint32_t w = bin.word_at(va);
        const std::uint32_t op = w >> 26;
        if (op == 0x03u) {  // JAL
            const std::uint32_t tgt = jump_target(va, w);
            if (bin.contains(tgt) && is_valid_r3000a(bin.word_at(tgt))) {
                candidates.insert(tgt);
            }
        }
        // Function pointer in data/ctor/vtable table pointing to valid code right after a terminator
        if ((w & 3u) == 0 && bin.contains(w) && is_after_terminator(w)) {
            const std::uint32_t tw = bin.word_at(w);
            if (tw != 0u && is_valid_r3000a(tw)) {
                candidates.insert(w);
            }
        }
        // Standard stack-frame prologue: addiu $sp, $sp, -N ONLY if directly after a terminator
        // (avoids splitting functions where GCC scheduled 1-4 instructions before addiu $sp, $sp, -N!)
        if ((w & 0xFFFF0000u) == 0x27BD0000u && is_after_terminator(va)) {
            const std::int16_t imm = static_cast<std::int16_t>(w & 0xFFFFu);
            if (imm < 0 && imm >= -0x4000 && ((-imm) & 3) == 0) {
                if (bin.contains(va + 12u) &&
                    is_valid_r3000a(bin.word_at(va + 4u)) &&
                    is_valid_r3000a(bin.word_at(va + 8u))) {
                    candidates.insert(va);
                }
            }
        }
    }

    // Build sorted list of candidate starts
    std::vector<std::uint32_t> starts(candidates.begin(), candidates.end());
    std::sort(starts.begin(), starts.end());

    // Pass 2: Trace reachable blocks for each function up to the next candidate start.
    for (std::size_t idx = 0; idx < starts.size(); ++idx) {
        const std::uint32_t fn_start = starts[idx];
        const std::uint32_t hard_limit = (idx + 1 < starts.size()) ? starts[idx + 1] : end_va;

        std::set<std::uint32_t> visited;
        std::set<std::uint32_t> labels;
        std::map<std::uint32_t, std::vector<std::uint32_t>> switches;
        std::queue<std::uint32_t> work;
        work.push(fn_start);

        std::uint32_t max_pc = fn_start;
        bool valid_fn = true;

        while (!work.empty()) {
            std::uint32_t pc = work.front();
            work.pop();

            while (pc >= fn_start && pc < hard_limit) {
                if (!visited.insert(pc).second) break;
                const std::uint32_t w = bin.word_at(pc);
                if (!is_valid_r3000a(w)) {
                    if (pc == fn_start) valid_fn = false;
                    break;
                }
                if (pc + 4u > max_pc) max_pc = pc + 4u;

                const std::uint32_t op = w >> 26;
                const std::uint32_t fn = w & 63u;

                if (is_branch_or_jump(w)) {
                    // Delay slot must be valid and included in function extent
                    if (pc + 4u < hard_limit) {
                        const std::uint32_t dw = bin.word_at(pc + 4u);
                        if (!is_valid_r3000a(dw)) {
                            valid_fn = false;
                            break;
                        }
                        visited.insert(pc + 4u);
                        if (pc + 8u > max_pc) max_pc = pc + 8u;
                    }

                    if (op == 0x02u) {  // J
                        const std::uint32_t tgt = jump_target(pc, w);
                        if (tgt >= fn_start && tgt < hard_limit) {
                            labels.insert(tgt);
                            work.push(tgt);
                        }
                        break;  // unconditional jump terminates block
                    } else if (op == 0x03u) {  // JAL
                        pc += 8u;
                        continue;
                    } else if (op == 0x00u && fn == 0x08u) {  // JR
                        const std::uint32_t rs = (w >> 21) & 31u;
                        if (rs != 31u) {
                            // Heuristic switch jump-table discovery: look back up to 12 insns for LUI + ADDIU/ORI/LW
                            std::uint32_t table_hi[32] = {};
                            bool has_hi[32] = {};
                            std::uint32_t table_addr = 0;
                            const std::uint32_t scan_lo = (pc >= fn_start + 48u) ? (pc - 48u) : fn_start;
                            for (std::uint32_t s = scan_lo; s < pc; s += 4u) {
                                const std::uint32_t sw = bin.word_at(s);
                                const std::uint32_t sop = sw >> 26;
                                const std::uint32_t srs = (sw >> 21) & 31u;
                                const std::uint32_t srt = (sw >> 16) & 31u;
                                if (sop == 0x0Fu) {  // LUI
                                    table_hi[srt] = (sw & 0xFFFFu) << 16;
                                    has_hi[srt] = true;
                                } else if ((sop == 0x09u || sop == 0x23u) && has_hi[srs]) {
                                    const std::uint32_t cand = table_hi[srs] + static_cast<std::uint32_t>(simm16(sw));
                                    if (bin.contains(cand)) table_addr = cand;
                                } else if (sop == 0x0Du && has_hi[srs]) {
                                    const std::uint32_t cand = table_hi[srs] | (sw & 0xFFFFu);
                                    if (bin.contains(cand)) table_addr = cand;
                                }
                            }
                            if (table_addr != 0) {
                                std::vector<std::uint32_t> targets;
                                std::set<std::uint32_t> seen_t;
                                for (std::uint32_t k = 0; k < 256u; ++k) {
                                    const std::uint32_t entry_va = table_addr + k * 4u;
                                    if (!bin.contains(entry_va)) break;
                                    const std::uint32_t tgt = bin.word_at(entry_va);
                                    if (tgt < fn_start || tgt >= hard_limit || (tgt & 3u) != 0) break;
                                    if (!is_valid_r3000a(bin.word_at(tgt))) break;
                                    if (seen_t.insert(tgt).second) {
                                        targets.push_back(tgt);
                                        labels.insert(tgt);
                                        work.push(tgt);
                                    }
                                }
                                if (targets.size() >= 2u) {
                                    switches[pc] = std::move(targets);
                                }
                            }
                        }
                        break;  // JR terminates block
                    } else if (op == 0x00u && fn == 0x09u) {  // JALR
                        pc += 8u;
                        continue;
                    } else {  // Conditional branch
                        const std::uint32_t tgt = branch_target(pc, w);
                        if (tgt >= fn_start && tgt < hard_limit) {
                            labels.insert(tgt);
                            work.push(tgt);
                        }
                        pc += 8u;
                        continue;
                    }
                }

                pc += 4u;
            }
        }

        if (!valid_fn || max_pc <= fn_start) continue;

        // Verify all words in [fn_start, max_pc) are valid instructions before accepting as AOT function
        bool all_valid = true;
        for (std::uint32_t va = fn_start; va < max_pc; va += 4u) {
            if (!is_valid_r3000a(bin.word_at(va))) {
                all_valid = false;
                break;
            }
        }
        if (!all_valid) continue;

        Function f;
        f.tag = tag;
        f.start = fn_start;
        f.end = max_pc;
        f.labels = std::move(labels);
        f.switch_tables = std::move(switches);
        f.code_hash = psx::fnv1a_words(bin.bytes.data() + (fn_start - bin.load_addr), f.end - f.start);
        res.total_instructions += (f.end - f.start) / 4u;
        res.function_starts.insert(f.start);
        res.functions.push_back(std::move(f));
    }

    return res;
}

}  // namespace psxrecomp
