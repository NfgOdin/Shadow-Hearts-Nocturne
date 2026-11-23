#include "psx/memory.hpp"
#include "psx/pgxp.hpp"
#include "psx/system.hpp"

#include <cstring>

namespace psx {

Memory::Memory()
    : ram_storage(std::make_unique<std::uint8_t[]>(kRamSize)),
      bios_storage(std::make_unique<std::uint8_t[]>(kBiosSize)),
      ram(ram_storage.get()),
      bios(bios_storage.get()) {
    reset();
}

void Memory::reset() {
    std::memset(ram, 0, kRamSize);
    std::memset(scratchpad, 0, sizeof(scratchpad));
    std::memset(bios, 0, kBiosSize);
    page_gen.fill(1);
    global_code_gen = 1;
    cache_ctrl = 0;
    pgxp::clear();
}

std::uint32_t Memory::fetch32(std::uint32_t vaddr) const noexcept {
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (phys < 0x00800000u) {
        return ram32(phys);
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        std::uint32_t v = 0;
        std::memcpy(&v, bios + (phys & kBiosMask), sizeof(v));
        return v;
    }
    return 0;
}

std::uint8_t Memory::read8(System &sys, std::uint32_t vaddr) const {
    if (sys.cpu.cache_isolated()) return 0;
    const std::uint32_t phys = virt_to_phys(vaddr);
    if (phys < 0x00800000u) {
        return ram[phys & kRamMask];
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        return scratchpad[phys & kScratchpadMask];
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        return sys.hw.read_io8(sys, phys);
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        return bios[phys & kBiosMask];
    }
    if (phys >= 0x1F000000u && phys < 0x1F800000u) {
        return 0xFFu;  // Expansion 1 (unpopulated)
    }
    return 0xFFu;
}

std::uint16_t Memory::read16(System &sys, std::uint32_t vaddr) const {
    if (sys.cpu.cache_isolated()) [[unlikely]] return 0;
    const std::uint32_t phys = virt_to_phys(vaddr) & ~1u;
    if (phys < 0x00800000u) [[likely]] {
        std::uint16_t v = 0;
        std::memcpy(&v, ram + (phys & kRamMask), sizeof(v));
        return v;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        std::uint16_t v = 0;
        std::memcpy(&v, scratchpad + (phys & kScratchpadMask), sizeof(v));
        return v;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        return sys.hw.read_io16(sys, phys);
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        std::uint16_t v = 0;
        std::memcpy(&v, bios + (phys & kBiosMask), sizeof(v));
        return v;
    }
    return 0xFFFFu;
}

std::uint32_t Memory::read32(System &sys, std::uint32_t vaddr) const {
    if (sys.cpu.cache_isolated()) [[unlikely]] return 0;
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (phys < 0x00800000u) [[likely]] {
        std::uint32_t v = 0;
        std::memcpy(&v, ram + (phys & kRamMask), sizeof(v));
        pgxp::on_mem_read32(phys, v);
        return v;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        std::uint32_t v = 0;
        std::memcpy(&v, scratchpad + (phys & kScratchpadMask), sizeof(v));
        pgxp::on_mem_read32(phys, v);
        return v;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        return sys.hw.read_io32(sys, phys);
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) {
        std::uint32_t v = 0;
        std::memcpy(&v, bios + (phys & kBiosMask), sizeof(v));
        return v;
    }
    if (vaddr == 0xFFFE0130u) {
        return cache_ctrl;
    }
    return 0xFFFFFFFFu;
}

void Memory::write8(System &sys, std::uint32_t vaddr, std::uint8_t val) {
    if (sys.cpu.cache_isolated()) return;
    const std::uint32_t phys = virt_to_phys(vaddr);
    if (phys < 0x00800000u) {
        const std::uint32_t off = phys & kRamMask;
        ram[off] = val;
        pgxp::invalidate_mem(phys);
        ++page_gen[off >> kCodePageShift];
        ++global_code_gen;
        return;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        scratchpad[phys & kScratchpadMask] = val;
        pgxp::invalidate_mem(phys);
        return;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        sys.hw.write_io8(sys, phys, val);
    }
}

void Memory::write16(System &sys, std::uint32_t vaddr, std::uint16_t val) {
    if (sys.cpu.cache_isolated()) return;
    const std::uint32_t phys = virt_to_phys(vaddr) & ~1u;
    if (phys < 0x00800000u) {
        const std::uint32_t off = phys & kRamMask;
        std::memcpy(ram + off, &val, sizeof(val));
        pgxp::invalidate_mem(phys);
        ++page_gen[off >> kCodePageShift];
        ++global_code_gen;
        return;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        std::memcpy(scratchpad + (phys & kScratchpadMask), &val, sizeof(val));
        pgxp::invalidate_mem(phys);
        return;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        sys.hw.write_io16(sys, phys, val);
    }
}

void Memory::write32(System &sys, std::uint32_t vaddr, std::uint32_t val) {
    if (sys.cpu.cache_isolated()) return;
    if (vaddr == 0xFFFE0130u) {
        cache_ctrl = val;
        return;
    }
    const std::uint32_t phys = virt_to_phys(vaddr) & ~3u;
    if (phys < 0x00800000u) {
        const std::uint32_t off = phys & kRamMask;
        std::memcpy(ram + off, &val, sizeof(val));
        pgxp::on_mem_write32(phys, val);
        ++page_gen[off >> kCodePageShift];
        ++global_code_gen;
        return;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        std::memcpy(scratchpad + (phys & kScratchpadMask), &val, sizeof(val));
        pgxp::on_mem_write32(phys, val);
        return;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        sys.hw.write_io32(sys, phys, val);
    }
}

}  // namespace psx
