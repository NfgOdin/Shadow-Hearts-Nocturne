#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace psx::pgxp {

struct VertexData {
    std::uint32_t raw_xy = 0u;
    float fx = 0.0f;
    float fy = 0.0f;
    float fz = 0.0f;  // > 0.0f indicates valid PGXP subpixel + depth data
};

inline constexpr std::size_t kRamWords = (2u * 1024u * 1024u) / 4u;  // 2 MiB PS1 RAM / 4
inline constexpr std::size_t kSpadWords = 1024u / 4u;                // 1 KiB Scratchpad / 4
inline constexpr std::size_t kRecentSize = 16u;                      // Power of two
inline constexpr std::uint32_t kNoDmaAddr = 0xFFFFFFFFu;
inline constexpr std::uint32_t kRecentTtlOps = 64u;

struct State {
    bool enabled = true;
    VertexData gte_sxy[3]{};
    VertexData recent[kRecentSize]{};
    std::uint32_t recent_head = 0u;
    std::uint32_t recent_count = 0u;
    std::uint32_t recent_ttl = 0u;
    std::uint64_t recent_bloom = 0u;
    std::uint32_t dma_word_addr = kNoDmaAddr;
    std::array<VertexData, kSpadWords> spad_shadow{};
    std::unique_ptr<VertexData[]> ram_shadow;
};

inline State &state() noexcept {
    static State s;
    return s;
}

inline std::uint32_t pack_sxy(std::int16_t sx, std::int16_t sy) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(sx)) |
           (static_cast<std::uint32_t>(static_cast<std::uint16_t>(sy)) << 16);
}

inline std::uint64_t bloom_bit(std::uint32_t word) noexcept {
    const std::uint32_t h = (word * 2654435761u) >> 26;
    return 1ull << (h & 63u);
}

inline VertexData *ensure_ram_shadow(State &s) noexcept {
    if (!s.ram_shadow) {
        s.ram_shadow = std::make_unique<VertexData[]>(kRamWords);
    }
    return s.ram_shadow.get();
}

inline void clear_recent() noexcept {
    State &s = state();
    s.recent_head = 0u;
    s.recent_count = 0u;
    s.recent_ttl = 0u;
    s.recent_bloom = 0u;
}

inline void clear_gte() noexcept {
    State &s = state();
    for (auto &v : s.gte_sxy) v.fz = 0.0f;
    clear_recent();
}

inline void clear() noexcept {
    State &s = state();
    for (auto &v : s.gte_sxy) v.fz = 0.0f;
    clear_recent();
    s.dma_word_addr = kNoDmaAddr;
    std::memset(s.spad_shadow.data(), 0, sizeof(s.spad_shadow));
    if (s.ram_shadow) {
        std::memset(s.ram_shadow.get(), 0, kRamWords * sizeof(VertexData));
    }
}

inline void set_enabled(bool en) noexcept {
    State &s = state();
    if (s.enabled != en) {
        s.enabled = en;
        clear();
    }
}

inline bool is_enabled() noexcept {
    return state().enabled;
}

inline void push_recent(State &s, const VertexData &v) noexcept {
    if (v.fz <= 0.0f) return;
    const std::size_t slot = static_cast<std::size_t>(s.recent_head & (kRecentSize - 1u));
    const bool overwriting = (s.recent_count == kRecentSize);
    s.recent[slot] = v;
    ++s.recent_head;
    if (!overwriting) {
        ++s.recent_count;
        s.recent_bloom |= bloom_bit(v.raw_xy);
    } else {
        std::uint64_t b = 0u;
        for (std::size_t i = 0; i < kRecentSize; ++i) {
            if (s.recent[i].fz > 0.0f) b |= bloom_bit(s.recent[i].raw_xy);
        }
        s.recent_bloom = b;
    }
    s.recent_ttl = kRecentTtlOps;
}

inline bool find_recent(const State &s, std::uint32_t word, VertexData &out) noexcept {
    if (s.recent_count == 0u || (s.recent_bloom & bloom_bit(word)) == 0u) {
        return false;
    }
    for (std::uint32_t i = 0; i < s.recent_count; ++i) {
        const std::size_t idx = static_cast<std::size_t>((s.recent_head - 1u - i) & (kRecentSize - 1u));
        const VertexData &e = s.recent[idx];
        if (e.fz > 0.0f && e.raw_xy == word) {
            out = e;
            return true;
        }
    }
    return false;
}

inline void push_gte_sxy(std::int16_t sx_int, std::int16_t sy_int,
                         float fx, float fy, float fz, bool valid) noexcept {
    State &s = state();
    s.gte_sxy[0] = s.gte_sxy[1];
    s.gte_sxy[1] = s.gte_sxy[2];
    if (s.enabled && valid &&
        std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz) && fz >= 1.0f) {
        s.gte_sxy[2].raw_xy = pack_sxy(sx_int, sy_int);
        s.gte_sxy[2].fx = fx;
        s.gte_sxy[2].fy = fy;
        s.gte_sxy[2].fz = fz;
    } else {
        s.gte_sxy[2].fz = 0.0f;
    }
}

inline void on_mtc2_sxy(std::uint32_t reg, std::uint32_t val) noexcept {
    State &s = state();
    const std::uint32_t r = reg & 31u;
    if (r < 12u || r > 15u) return;
    VertexData v{};
    if (s.enabled) {
        find_recent(s, val, v);
    }
    if (r == 15u) {
        s.gte_sxy[0] = s.gte_sxy[1];
        s.gte_sxy[1] = s.gte_sxy[2];
        s.gte_sxy[2] = v;
    } else {
        s.gte_sxy[r - 12u] = v;
    }
}

inline void on_mfc2_sxy(std::uint32_t reg, std::uint32_t packed_xy) noexcept {
    State &s = state();
    if (!s.enabled) return;
    const std::uint32_t r = reg & 31u;
    if (r < 12u || r > 15u) return;
    const std::size_t slot = (r == 15u) ? 2u : static_cast<std::size_t>(r - 12u);
    const VertexData &v = s.gte_sxy[slot];
    if (v.fz > 0.0f && v.raw_xy == packed_xy) {
        push_recent(s, v);
    }
}

inline void on_mem_read32(std::uint32_t phys, std::uint32_t val) noexcept {
    State &s = state();
    if (!s.enabled) return;
    if (phys < 0x00800000u) {
        if (!s.ram_shadow) return;
        const VertexData &e = s.ram_shadow[(phys & 0x001FFFFCu) >> 2];
        if (e.fz > 0.0f && e.raw_xy == val) [[unlikely]] {
            push_recent(s, e);
        }
        return;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        const VertexData &e = s.spad_shadow[(phys & 0x3FCu) >> 2];
        if (e.fz > 0.0f && e.raw_xy == val) [[unlikely]] {
            push_recent(s, e);
        }
    }
}

inline void on_mem_write32(std::uint32_t phys, std::uint32_t val) noexcept {
    State &s = state();
    if (!s.enabled) return;

    const bool is_ram = (phys < 0x00800000u);
    const bool is_spad = (!is_ram && phys >= 0x1F800000u && phys < 0x1F800400u);
    if (!is_ram && !is_spad) return;

    if (s.recent_count == 0u) {
        if (is_ram) {
            if (s.ram_shadow) {
                VertexData &dst = s.ram_shadow[(phys & 0x001FFFFCu) >> 2];
                if (dst.fz > 0.0f) dst.fz = 0.0f;
            }
        } else {
            VertexData &dst = s.spad_shadow[(phys & 0x3FCu) >> 2];
            if (dst.fz > 0.0f) dst.fz = 0.0f;
        }
        return;
    }

    VertexData matched{};
    const bool hit = find_recent(s, val, matched);
    if (s.recent_ttl > 0u) {
        if (--s.recent_ttl == 0u) {
            clear_recent();
        }
    }

    if (hit) {
        if (is_ram) {
            ensure_ram_shadow(s)[(phys & 0x001FFFFCu) >> 2] = matched;
        } else {
            s.spad_shadow[(phys & 0x3FCu) >> 2] = matched;
        }
    } else {
        if (is_ram) {
            if (s.ram_shadow) s.ram_shadow[(phys & 0x001FFFFCu) >> 2].fz = 0.0f;
        } else {
            s.spad_shadow[(phys & 0x3FCu) >> 2].fz = 0.0f;
        }
    }
}

inline void invalidate_mem(std::uint32_t phys) noexcept {
    State &s = state();
    if (!s.enabled) return;
    if (phys < 0x00800000u) {
        if (s.ram_shadow) s.ram_shadow[(phys & 0x001FFFFCu) >> 2].fz = 0.0f;
    } else if (phys >= 0x1F800000u && phys < 0x1F800400u) {
        s.spad_shadow[(phys & 0x3FCu) >> 2].fz = 0.0f;
    }
}

inline void invalidate_ram_range(std::uint32_t start_phys, std::uint32_t words, std::int32_t step) noexcept {
    State &s = state();
    if (!s.enabled || !s.ram_shadow || words == 0u) return;
    std::uint32_t addr = start_phys & 0x001FFFFCu;
    VertexData *ram = s.ram_shadow.get();
    for (std::uint32_t i = 0; i < words; ++i) {
        ram[addr >> 2].fz = 0.0f;
        addr = static_cast<std::uint32_t>(static_cast<std::int32_t>(addr) + step) & 0x001FFFFCu;
    }
}

inline void set_dma_word_addr(std::uint32_t phys_addr) noexcept {
    state().dma_word_addr = phys_addr & 0x001FFFFCu;
}

inline void clear_dma_word_addr() noexcept {
    state().dma_word_addr = kNoDmaAddr;
}

inline bool lookup_gp0_word(std::uint32_t word, VertexData &out) noexcept {
    const State &s = state();
    if (!s.enabled) return false;
    if (s.dma_word_addr != kNoDmaAddr) {
        if (!s.ram_shadow) return false;
        const VertexData &e = s.ram_shadow[(s.dma_word_addr & 0x001FFFFCu) >> 2];
        if (e.fz > 0.0f && e.raw_xy == word) {
            out = e;
            return true;
        }
        return false;
    }
    return find_recent(s, word, out);
}

}  // namespace psx::pgxp
