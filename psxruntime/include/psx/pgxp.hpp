#pragma once

#include <cmath>
#include <cstdint>

namespace psx::pgxp {

struct Entry {
    std::uint32_t packed_xy = 0xFFFFFFFFu;
    float fx = 0.0f;
    float fy = 0.0f;
    float fz = 1.0f;
    bool valid = false;
};

inline constexpr std::size_t kCacheSize = 4096;  // Power of two

struct State {
    bool enabled = true;
    Entry table[kCacheSize]{};
};

inline State &state() noexcept {
    static State s;
    return s;
}

inline void set_enabled(bool en) noexcept {
    state().enabled = en;
}

inline bool is_enabled() noexcept {
    return state().enabled;
}

inline void clear() noexcept {
    State &s = state();
    for (auto &e : s.table) {
        e.valid = false;
    }
}

inline std::uint32_t pack_sxy(std::int16_t sx, std::int16_t sy) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(sx)) |
           (static_cast<std::uint32_t>(static_cast<std::uint16_t>(sy)) << 16);
}

inline std::size_t hash_xy(std::uint32_t packed_xy) noexcept {
    return static_cast<std::size_t>(((packed_xy * 2654435761u) >> 20) & (kCacheSize - 1));
}

inline void record(std::int16_t sx_int, std::int16_t sy_int, float fx, float fy, float fz) noexcept {
    State &s = state();
    if (!s.enabled) return;

    const std::uint32_t key = pack_sxy(sx_int, sy_int);
    // Validate subpixel offset is within reasonable proximity of integer GTE output
    if (std::fabs(fx - static_cast<float>(sx_int)) > 1.75f ||
        std::fabs(fy - static_cast<float>(sy_int)) > 1.75f) {
        return;
    }

    const std::size_t idx = hash_xy(key);
    Entry &e = s.table[idx];
    e.packed_xy = key;
    e.fx = fx;
    e.fy = fy;
    e.fz = (fz > 1.0f) ? fz : 1.0f;
    e.valid = true;

    // Also populate a secondary linear-probed slot so hash collisions inside the same triangle/quad don't evict
    const std::size_t idx2 = (idx + 1) & (kCacheSize - 1);
    Entry &e2 = s.table[idx2];
    if (!e2.valid || e2.packed_xy == key) {
        e2 = e;
    }
}

inline bool lookup(std::uint32_t raw_xy_word, float &out_fx, float &out_fy, float &out_fz) noexcept {
    const State &s = state();
    if (!s.enabled) return false;

    const std::size_t idx = hash_xy(raw_xy_word);
    const Entry &e = s.table[idx];
    if (e.valid && e.packed_xy == raw_xy_word) {
        out_fx = e.fx;
        out_fy = e.fy;
        out_fz = e.fz;
        return true;
    }
    const std::size_t idx2 = (idx + 1) & (kCacheSize - 1);
    const Entry &e2 = s.table[idx2];
    if (e2.valid && e2.packed_xy == raw_xy_word) {
        out_fx = e2.fx;
        out_fy = e2.fy;
        out_fz = e2.fz;
        return true;
    }
    return false;
}

}  // namespace psx::pgxp
