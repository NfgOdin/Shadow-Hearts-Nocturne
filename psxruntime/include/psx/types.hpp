#pragma once

#include <cstddef>
#include <cstdint>

namespace psx {

constexpr std::uint32_t kRamSize = 2u * 1024u * 1024u;          // 2 MiB main RAM
constexpr std::uint32_t kRamMask = kRamSize - 1u;
constexpr std::uint32_t kScratchpadSize = 1024u;                // 1 KiB scratchpad @ 0x1F800000
constexpr std::uint32_t kScratchpadMask = kScratchpadSize - 1u;
constexpr std::uint32_t kBiosSize = 512u * 1024u;               // 512 KiB BIOS @ 0x1FC00000
constexpr std::uint32_t kBiosMask = kBiosSize - 1u;

constexpr std::uint64_t kCpuClockHz = 33868800ull;              // 33.8688 MHz R3000A
constexpr std::uint32_t kCyclesPerFrameNtsc = 564480u;          // ~60.0 Hz (33868800 / 60)
constexpr std::uint32_t kScanlinesNtsc = 263u;
constexpr std::uint32_t kCyclesPerScanline = kCyclesPerFrameNtsc / kScanlinesNtsc;

constexpr std::uint32_t kCodePageShift = 12u;                   // 4 KiB dirty-tracking pages
constexpr std::uint32_t kCodePageCount = kRamSize >> kCodePageShift;

// Hardware IRQ bit numbers (I_STAT / I_MASK at 0x1F801070 / 0x1F801074)
enum IrqBit : std::uint32_t {
    kIrqVBlank = 0,
    kIrqGpu = 1,
    kIrqCdrom = 2,
    kIrqDma = 3,
    kIrqTimer0 = 4,
    kIrqTimer1 = 5,
    kIrqTimer2 = 6,
    kIrqSio0 = 7,
    kIrqSio1 = 8,
    kIrqSpu = 9,
    kIrqPio = 10,
};

inline constexpr std::uint32_t kRegionMask[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0x7FFFFFFFu, 0x1FFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};

// Mask virtual MIPS address (KUSEG / KSEG0 / KSEG1) to physical 29-bit address.
inline constexpr std::uint32_t virt_to_phys(std::uint32_t vaddr) noexcept {
    // 0x00000000..0x7FFFFFFF -> unchanged
    // 0x80000000..0x9FFFFFFF -> vaddr & 0x1FFFFFFF
    // 0xA0000000..0xBFFFFFFF -> vaddr & 0x1FFFFFFF
    // 0xC0000000..0xFFFFFFFF -> unchanged (KSEG2, e.g. 0xFFFE0130 cache control)
    return vaddr & kRegionMask[vaddr >> 29];
}

// Fast 64-bit FNV-1a hash used for validating AOT code pages against RAM.
inline std::uint64_t fnv1a_words(const std::uint8_t *base, std::uint32_t byte_len) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (std::uint32_t i = 0; i < byte_len; ++i) {
        h ^= base[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace psx
