#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace psxrecomp {

struct PsxBinary {
    std::string name;
    std::uint32_t entry_pc = 0;
    std::uint32_t init_gp = 0;
    std::uint32_t load_addr = 0;
    std::uint32_t text_size = 0;
    std::vector<std::uint8_t> bytes;  // raw code/data bytes loaded at load_addr

    bool contains(std::uint32_t va) const noexcept {
        return va >= load_addr && va + 4u <= load_addr + text_size;
    }

    std::uint32_t word_at(std::uint32_t va) const noexcept {
        if (!contains(va)) return 0;
        const std::uint32_t off = va - load_addr;
        return static_cast<std::uint32_t>(bytes[off]) |
               (static_cast<std::uint32_t>(bytes[off + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[off + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
    }
};

bool load_psx_exe(const std::string &path, PsxBinary &out, std::string &err);
bool load_raw_overlay(const std::string &path, std::uint32_t base_addr, PsxBinary &out, std::string &err);

}  // namespace psxrecomp
