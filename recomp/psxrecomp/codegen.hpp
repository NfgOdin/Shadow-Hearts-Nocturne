#pragma once

#include "analysis.hpp"
#include "psx_exe.hpp"

#include <string>
#include <utility>
#include <vector>

namespace psxrecomp {

struct CodegenOptions {
    std::string out_dir;
    std::uint32_t unit_count = 16;
    std::string ns = "psx_gen";
};

using ModulePair = std::pair<PsxBinary, AnalysisResult>;

bool emit_cpp_project(const std::vector<ModulePair> &modules,
                      const CodegenOptions &opts, std::string &err);

// Emit a single function body as C++ source string (also used by unit tests).
std::string emit_function_cpp(const PsxBinary &bin, const Function &fn,
                              const std::unordered_set<std::uint32_t> &known_fns);

}  // namespace psxrecomp
