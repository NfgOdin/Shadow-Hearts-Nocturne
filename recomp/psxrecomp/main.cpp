#include "analysis.hpp"
#include "codegen.hpp"
#include "psx_exe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: psxrecomp <PS-X EXE> --out <dir> [--units N] [--ns name] [--overlay <path>:<hex_addr> ...]\n");
        return 1;
    }

    std::string input_path = argv[1];
    psxrecomp::CodegenOptions opts;
    opts.out_dir = "generated/psx";
    opts.unit_count = 16;
    std::vector<std::pair<std::string, std::uint32_t>> overlays;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            opts.out_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--units") == 0 && i + 1 < argc) {
            opts.unit_count = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--ns") == 0 && i + 1 < argc) {
            opts.ns = argv[++i];
        } else if (std::strcmp(argv[i], "--overlay") == 0 && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t colon = spec.find_last_of(':');
            if (colon == std::string::npos || colon == 0) {
                std::fprintf(stderr, "[psxrecomp] invalid --overlay '%s' (expected <path>:<hex_addr>)\n", spec.c_str());
                return 1;
            }
            const std::string ovl_path = spec.substr(0, colon);
            const std::uint32_t base = static_cast<std::uint32_t>(std::strtoul(spec.substr(colon + 1).c_str(), nullptr, 16));
            overlays.emplace_back(ovl_path, base);
        }
    }

    std::vector<psxrecomp::ModulePair> modules;
    std::string err;

    psxrecomp::PsxBinary main_bin;
    if (!psxrecomp::load_psx_exe(input_path, main_bin, err)) {
        std::fprintf(stderr, "[psxrecomp] error: %s\n", err.c_str());
        return 1;
    }
    auto main_analysis = psxrecomp::analyze_binary(main_bin, "");
    std::size_t total_fns = main_analysis.functions.size();
    std::size_t total_insns = main_analysis.total_instructions;
    std::printf("[psxrecomp] %s: load=0x%08X size=0x%X entry=0x%08X -> %zu functions (%zu instructions)\n",
                main_bin.name.c_str(), main_bin.load_addr, main_bin.text_size, main_bin.entry_pc,
                main_analysis.functions.size(), main_analysis.total_instructions);
    modules.emplace_back(std::move(main_bin), std::move(main_analysis));

    for (std::size_t idx = 0; idx < overlays.size(); ++idx) {
        psxrecomp::PsxBinary ovl_bin;
        if (!psxrecomp::load_raw_overlay(overlays[idx].first, overlays[idx].second, ovl_bin, err)) {
            std::fprintf(stderr, "[psxrecomp] overlay error: %s\n", err.c_str());
            return 1;
        }
        const std::string tag = "ovl" + std::to_string(idx) + "_";
        auto ovl_analysis = psxrecomp::analyze_binary(ovl_bin, tag);
        total_fns += ovl_analysis.functions.size();
        total_insns += ovl_analysis.total_instructions;
        std::printf("[psxrecomp]   + overlay %s @ 0x%08X (size=0x%X) -> %zu functions (%zu instructions)\n",
                    ovl_bin.name.c_str(), ovl_bin.load_addr, ovl_bin.text_size,
                    ovl_analysis.functions.size(), ovl_analysis.total_instructions);
        modules.emplace_back(std::move(ovl_bin), std::move(ovl_analysis));
    }

    if (!psxrecomp::emit_cpp_project(modules, opts, err)) {
        std::fprintf(stderr, "[psxrecomp] codegen error: %s\n", err.c_str());
        return 1;
    }

    std::printf("[psxrecomp] emitted %u C++20 units + registry.cpp (%zu total functions, %zu instructions) to %s\n",
                opts.unit_count, total_fns, total_insns, opts.out_dir.c_str());
    return 0;
}
