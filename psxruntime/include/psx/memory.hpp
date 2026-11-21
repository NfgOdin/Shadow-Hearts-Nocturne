#pragma once

#include "psx/types.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace psx {

class System;

// PlayStation 1 Memory Bus:
// - 2 MiB Main RAM (mirrored across 0x00000000..0x007FFFFF)
// - 1 KiB Scratchpad (0x1F800000..0x1F8003FF)
// - Hardware I/O registers (0x1F801000..0x1F802FFF)
// - 512 KiB BIOS ROM region (0x1FC00000..0x1FC7FFFF)
// - Cache Control register (0xFFFE0130)
class Memory {
public:
    std::unique_ptr<std::uint8_t[]> ram_storage;
    std::unique_ptr<std::uint8_t[]> bios_storage;
    std::uint8_t *ram = nullptr;
    std::uint8_t *bios = nullptr;
    alignas(64) std::uint8_t scratchpad[kScratchpadSize] = {};

    Memory();

    // Per-4KB RAM page write generation counter (detects overlay loads & self-modifying code).
    std::array<std::uint32_t, kCodePageCount> page_gen = {};
    std::uint32_t global_code_gen = 1;
    std::uint32_t cache_ctrl = 0;

    void reset();

    // Direct RAM helpers (bypass I/O and cache isolation, bump dirty generation on write).
    inline std::uint8_t ram8(std::uint32_t offset) const noexcept {
        return ram[offset & kRamMask];
    }
    inline std::uint16_t ram16(std::uint32_t offset) const noexcept {
        std::uint16_t v = 0;
        std::memcpy(&v, ram + (offset & kRamMask & ~1u), sizeof(v));
        return v;
    }
    inline std::uint32_t ram32(std::uint32_t offset) const noexcept {
        std::uint32_t v = 0;
        std::memcpy(&v, ram + (offset & kRamMask & ~3u), sizeof(v));
        return v;
    }

    inline void mark_code_dirty(std::uint32_t phys_addr, std::uint32_t byte_len) noexcept {
        if (byte_len == 0) return;
        const std::uint32_t start_p = (phys_addr & kRamMask) >> kCodePageShift;
        const std::uint32_t end_p = ((phys_addr + byte_len - 1u) & kRamMask) >> kCodePageShift;
        ++global_code_gen;
        if (start_p <= end_p) {
            for (std::uint32_t p = start_p; p <= end_p; ++p) ++page_gen[p];
        } else {
            for (std::uint32_t p = start_p; p < kCodePageCount; ++p) ++page_gen[p];
            for (std::uint32_t p = 0; p <= end_p; ++p) ++page_gen[p];
        }
    }

    inline void write_ram_bytes(std::uint32_t phys_addr, const void *src, std::uint32_t len) noexcept {
        const auto *s = static_cast<const std::uint8_t *>(src);
        std::uint32_t off = phys_addr & kRamMask;
        std::uint32_t rem = len;
        while (rem > 0) {
            const std::uint32_t chunk = (off + rem <= kRamSize) ? rem : (kRamSize - off);
            std::memcpy(ram + off, s, chunk);
            s += chunk;
            off = (off + chunk) & kRamMask;
            rem -= chunk;
        }
        mark_code_dirty(phys_addr, len);
    }

    // Full virtual-address bus reads/writes (respects SR.IsC cache isolation & routes I/O).
    std::uint8_t read8(System &sys, std::uint32_t vaddr) const;
    std::uint16_t read16(System &sys, std::uint32_t vaddr) const;
    std::uint32_t read32(System &sys, std::uint32_t vaddr) const;
    std::uint32_t fetch32(std::uint32_t vaddr) const noexcept;

    void write8(System &sys, std::uint32_t vaddr, std::uint8_t val);
    void write16(System &sys, std::uint32_t vaddr, std::uint16_t val);
    void write32(System &sys, std::uint32_t vaddr, std::uint32_t val);
};

}  // namespace psx
