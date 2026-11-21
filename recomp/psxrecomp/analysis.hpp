#pragma once

#include "psx_exe.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

namespace psxrecomp {

struct Function {
    std::string tag;        // empty for main EXE, "ovl0_", "ovl1_", etc. for overlays
    std::uint32_t start = 0;
    std::uint32_t end = 0;  // exclusive
    std::uint64_t code_hash = 0;
    std::set<std::uint32_t> labels;  // intra-function branch/jump/switch targets
    std::map<std::uint32_t, std::vector<std::uint32_t>> switch_tables;  // jr_pc -> target PCs
};

struct AnalysisResult {
    std::vector<Function> functions;
    std::unordered_set<std::uint32_t> function_starts;
    std::size_t total_instructions = 0;
};

bool is_valid_r3000a(std::uint32_t w) noexcept;
bool is_branch_or_jump(std::uint32_t w) noexcept;
bool is_load_instruction(std::uint32_t w, std::uint32_t &dst_reg) noexcept;

AnalysisResult analyze_binary(const PsxBinary &bin, const std::string &tag = "");

}  // namespace psxrecomp
