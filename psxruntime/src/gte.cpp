#include "psx/gte.hpp"
#include "psx/pgxp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psx {

namespace {

// Official PS1 GTE UNR (Unsigned Newton-Raphson) 257-entry lookup table.
constexpr std::uint8_t kUnrTable[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00,
};

constexpr std::int64_t kMac123Max = (1ll << 43) - 1ll;
constexpr std::int64_t kMac123Min = -(1ll << 43);
constexpr std::int64_t kMac0Max = 0x7FFFFFFFll;
constexpr std::int64_t kMac0Min = -0x80000000ll;

inline std::uint32_t pack_xy(std::int16_t x, std::int16_t y) {
    return static_cast<std::uint16_t>(x) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16);
}

inline std::uint32_t pack_rgba(const std::uint8_t c[4]) {
    return static_cast<std::uint32_t>(c[0]) |
           (static_cast<std::uint32_t>(c[1]) << 8) |
           (static_cast<std::uint32_t>(c[2]) << 16) |
           (static_cast<std::uint32_t>(c[3]) << 24);
}

inline void unpack_rgba(std::uint32_t v, std::uint8_t c[4]) {
    c[0] = static_cast<std::uint8_t>(v & 0xFFu);
    c[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    c[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    c[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

inline std::int32_t count_leading_same_bits(std::int32_t v) {
    std::uint32_t u = static_cast<std::uint32_t>(v);
    if (u & 0x80000000u) u = ~u;
    if (u == 0) return 32;
    return __builtin_clz(u);
}

inline std::int64_t sign_extend44(std::int64_t v) {
    return (v << 20) >> 20;
}

}  // namespace

void Gte::reset() {
    *this = Gte{};
}

std::uint32_t Gte::divide_unr(std::uint16_t h, std::uint16_t sz3, bool &overflow) {
    if (static_cast<std::uint32_t>(h) < static_cast<std::uint32_t>(sz3) * 2u) {
        const unsigned z = (sz3 == 0) ? 16u : static_cast<unsigned>(__builtin_clz(static_cast<std::uint32_t>(sz3)) - 16);
        const std::uint32_t n = static_cast<std::uint32_t>(h) << z;
        std::uint32_t d = static_cast<std::uint32_t>(sz3) << z;
        const std::uint32_t u = static_cast<std::uint32_t>(kUnrTable[(d - 0x7FC0u) >> 7]) + 0x101u;
        d = (0x2000080u - (d * u)) >> 8;
        d = (0x80u + (d * u)) >> 8;
        std::uint32_t res = static_cast<std::uint32_t>((static_cast<std::uint64_t>(n) * d + 0x8000ull) >> 16);
        if (res > 0x1FFFFu) res = 0x1FFFFu;
        overflow = false;
        return res;
    }
    overflow = true;
    return 0x1FFFFu;
}

void Gte::clear_flag() {
    flag = 0;
}

void Gte::finalize_flag() {
    if (flag & 0x7F87E000u) {
        flag |= (1u << 31);
    }
}

std::int64_t Gte::check_mac0(std::int64_t v) {
    if (v > kMac0Max) flag |= (1u << 16);
    if (v < kMac0Min) flag |= (1u << 15);
    return v;
}

std::int64_t Gte::check_mac123(int idx, std::int64_t v) {
    // idx: 1, 2, or 3
    if (v > kMac123Max) flag |= (1u << (31 - idx));  // bit 30, 29, 28
    if (v < kMac123Min) flag |= (1u << (28 - idx));  // bit 27, 26, 25
    return sign_extend44(v);
}

std::int32_t Gte::set_mac123(int idx, std::int64_t v, int sf) {
    check_mac123(idx, v);
    const std::int32_t res = static_cast<std::int32_t>(v >> sf);
    mac[idx] = res;
    return res;
}

std::int16_t Gte::set_ir123(int idx, std::int32_t v, bool lm) {
    const std::int32_t lo = lm ? 0 : -0x8000;
    const std::int32_t hi = 0x7FFF;
    if (v < lo) {
        flag |= (1u << (25 - idx));  // bit 24, 23, 22
        ir[idx] = static_cast<std::int16_t>(lo);
    } else if (v > hi) {
        flag |= (1u << (25 - idx));
        ir[idx] = static_cast<std::int16_t>(hi);
    } else {
        ir[idx] = static_cast<std::int16_t>(v);
    }
    return ir[idx];
}

std::int16_t Gte::set_ir0(std::int64_t v) {
    if (v < 0) {
        flag |= (1u << 12);
        ir[0] = 0;
    } else if (v > 0x1000) {
        flag |= (1u << 12);
        ir[0] = 0x1000;
    } else {
        ir[0] = static_cast<std::int16_t>(v);
    }
    return ir[0];
}

void Gte::push_sxy(std::int32_t x, std::int32_t y) {
    sx[0] = sx[1];
    sx[1] = sx[2];
    sy[0] = sy[1];
    sy[1] = sy[2];

    if (x < -0x400) {
        flag |= (1u << 14);
        sx[2] = -0x400;
    } else if (x > 0x3FF) {
        flag |= (1u << 14);
        sx[2] = 0x3FF;
    } else {
        sx[2] = static_cast<std::int16_t>(x);
    }

    if (y < -0x400) {
        flag |= (1u << 13);
        sy[2] = -0x400;
    } else if (y > 0x3FF) {
        flag |= (1u << 13);
        sy[2] = 0x3FF;
    } else {
        sy[2] = static_cast<std::int16_t>(y);
    }

    sx[3] = sx[2];
    sy[3] = sy[2];
}

void Gte::push_sz(std::int64_t z) {
    sz[0] = sz[1];
    sz[1] = sz[2];
    sz[2] = sz[3];
    if (z < 0) {
        flag |= (1u << 18);
        sz[3] = 0;
    } else if (z > 0xFFFF) {
        flag |= (1u << 18);
        sz[3] = 0xFFFF;
    } else {
        sz[3] = static_cast<std::uint16_t>(z);
    }
}

std::uint8_t Gte::clamp_color(int ch, std::int32_t v) {
    // ch: 0=R (bit 21), 1=G (bit 20), 2=B (bit 19)
    if (v < 0) {
        flag |= (1u << (21 - ch));
        return 0;
    }
    if (v > 0xFF) {
        flag |= (1u << (21 - ch));
        return 0xFF;
    }
    return static_cast<std::uint8_t>(v);
}

void Gte::push_rgb(std::int32_t r, std::int32_t g, std::int32_t b) {
    std::memcpy(rgb[0], rgb[1], 4);
    std::memcpy(rgb[1], rgb[2], 4);
    rgb[2][0] = clamp_color(0, r);
    rgb[2][1] = clamp_color(1, g);
    rgb[2][2] = clamp_color(2, b);
    rgb[2][3] = rgbc[3];
}

std::uint32_t Gte::mfc2(std::uint32_t reg) const {
    switch (reg & 31u) {
    case 0: return pack_xy(vx[0], vy[0]);
    case 1: return static_cast<std::uint32_t>(static_cast<std::int32_t>(vz[0]));
    case 2: return pack_xy(vx[1], vy[1]);
    case 3: return static_cast<std::uint32_t>(static_cast<std::int32_t>(vz[1]));
    case 4: return pack_xy(vx[2], vy[2]);
    case 5: return static_cast<std::uint32_t>(static_cast<std::int32_t>(vz[2]));
    case 6: return pack_rgba(rgbc);
    case 7: return static_cast<std::uint32_t>(otz);
    case 8: return static_cast<std::uint32_t>(static_cast<std::int32_t>(ir[0]));
    case 9: return static_cast<std::uint32_t>(static_cast<std::int32_t>(ir[1]));
    case 10: return static_cast<std::uint32_t>(static_cast<std::int32_t>(ir[2]));
    case 11: return static_cast<std::uint32_t>(static_cast<std::int32_t>(ir[3]));
    case 12: return pack_xy(sx[0], sy[0]);
    case 13: return pack_xy(sx[1], sy[1]);
    case 14: case 15: return pack_xy(sx[2], sy[2]);
    case 16: return static_cast<std::uint32_t>(sz[0]);
    case 17: return static_cast<std::uint32_t>(sz[1]);
    case 18: return static_cast<std::uint32_t>(sz[2]);
    case 19: return static_cast<std::uint32_t>(sz[3]);
    case 20: return pack_rgba(rgb[0]);
    case 21: return pack_rgba(rgb[1]);
    case 22: return pack_rgba(rgb[2]);
    case 23: return res1;
    case 24: return static_cast<std::uint32_t>(mac[0]);
    case 25: return static_cast<std::uint32_t>(mac[1]);
    case 26: return static_cast<std::uint32_t>(mac[2]);
    case 27: return static_cast<std::uint32_t>(mac[3]);
    case 28: case 29: {
        const std::uint32_t r = static_cast<std::uint32_t>(std::clamp<int>(ir[1] >> 7, 0, 31));
        const std::uint32_t g = static_cast<std::uint32_t>(std::clamp<int>(ir[2] >> 7, 0, 31));
        const std::uint32_t b = static_cast<std::uint32_t>(std::clamp<int>(ir[3] >> 7, 0, 31));
        return r | (g << 5) | (b << 10);
    }
    case 30: return static_cast<std::uint32_t>(lzcs);
    case 31: return static_cast<std::uint32_t>(lzcr);
    default: return 0;
    }
}

void Gte::mtc2(std::uint32_t reg, std::uint32_t val) {
    switch (reg & 31u) {
    case 0: vx[0] = static_cast<std::int16_t>(val & 0xFFFFu); vy[0] = static_cast<std::int16_t>(val >> 16); break;
    case 1: vz[0] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 2: vx[1] = static_cast<std::int16_t>(val & 0xFFFFu); vy[1] = static_cast<std::int16_t>(val >> 16); break;
    case 3: vz[1] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 4: vx[2] = static_cast<std::int16_t>(val & 0xFFFFu); vy[2] = static_cast<std::int16_t>(val >> 16); break;
    case 5: vz[2] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 6: unpack_rgba(val, rgbc); break;
    case 7: otz = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 8: ir[0] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 9: ir[1] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 10: ir[2] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 11: ir[3] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 12: sx[0] = static_cast<std::int16_t>(val & 0xFFFFu); sy[0] = static_cast<std::int16_t>(val >> 16); break;
    case 13: sx[1] = static_cast<std::int16_t>(val & 0xFFFFu); sy[1] = static_cast<std::int16_t>(val >> 16); break;
    case 14: sx[2] = static_cast<std::int16_t>(val & 0xFFFFu); sy[2] = static_cast<std::int16_t>(val >> 16); sx[3] = sx[2]; sy[3] = sy[2]; break;
    case 15:
        sx[0] = sx[1]; sy[0] = sy[1];
        sx[1] = sx[2]; sy[1] = sy[2];
        sx[2] = static_cast<std::int16_t>(val & 0xFFFFu);
        sy[2] = static_cast<std::int16_t>(val >> 16);
        sx[3] = sx[2]; sy[3] = sy[2];
        break;
    case 16: sz[0] = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 17: sz[1] = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 18: sz[2] = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 19: sz[3] = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 20: unpack_rgba(val, rgb[0]); break;
    case 21: unpack_rgba(val, rgb[1]); break;
    case 22: unpack_rgba(val, rgb[2]); break;
    case 23: res1 = val; break;
    case 24: mac[0] = static_cast<std::int32_t>(val); break;
    case 25: mac[1] = static_cast<std::int32_t>(val); break;
    case 26: mac[2] = static_cast<std::int32_t>(val); break;
    case 27: mac[3] = static_cast<std::int32_t>(val); break;
    case 28:
        irgb = static_cast<std::uint16_t>(val & 0x7FFFu);
        ir[1] = static_cast<std::int16_t>((val & 0x1Fu) << 7);
        ir[2] = static_cast<std::int16_t>(((val >> 5) & 0x1Fu) << 7);
        ir[3] = static_cast<std::int16_t>(((val >> 10) & 0x1Fu) << 7);
        break;
    case 29: break;  // ORGB is read-only
    case 30:
        lzcs = static_cast<std::int32_t>(val);
        lzcr = count_leading_same_bits(lzcs);
        break;
    case 31: break;  // LZCR is read-only
    default: break;
    }
}

std::uint32_t Gte::cfc2(std::uint32_t reg) const {
    switch (reg & 31u) {
    case 0: return pack_xy(rt[0][0], rt[0][1]);
    case 1: return pack_xy(rt[0][2], rt[1][0]);
    case 2: return pack_xy(rt[1][1], rt[1][2]);
    case 3: return pack_xy(rt[2][0], rt[2][1]);
    case 4: return static_cast<std::uint32_t>(static_cast<std::int32_t>(rt[2][2]));
    case 5: return static_cast<std::uint32_t>(tr[0]);
    case 6: return static_cast<std::uint32_t>(tr[1]);
    case 7: return static_cast<std::uint32_t>(tr[2]);
    case 8: return pack_xy(l[0][0], l[0][1]);
    case 9: return pack_xy(l[0][2], l[1][0]);
    case 10: return pack_xy(l[1][1], l[1][2]);
    case 11: return pack_xy(l[2][0], l[2][1]);
    case 12: return static_cast<std::uint32_t>(static_cast<std::int32_t>(l[2][2]));
    case 13: return static_cast<std::uint32_t>(bk[0]);
    case 14: return static_cast<std::uint32_t>(bk[1]);
    case 15: return static_cast<std::uint32_t>(bk[2]);
    case 16: return pack_xy(lr[0][0], lr[0][1]);
    case 17: return pack_xy(lr[0][2], lr[1][0]);
    case 18: return pack_xy(lr[1][1], lr[1][2]);
    case 19: return pack_xy(lr[2][0], lr[2][1]);
    case 20: return static_cast<std::uint32_t>(static_cast<std::int32_t>(lr[2][2]));
    case 21: return static_cast<std::uint32_t>(fc[0]);
    case 22: return static_cast<std::uint32_t>(fc[1]);
    case 23: return static_cast<std::uint32_t>(fc[2]);
    case 24: return static_cast<std::uint32_t>(ofx);
    case 25: return static_cast<std::uint32_t>(ofy);
    case 26: return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));  // H sign-extends on CFC2!
    case 27: return static_cast<std::uint32_t>(static_cast<std::int32_t>(dqa));
    case 28: return static_cast<std::uint32_t>(dqb);
    case 29: return static_cast<std::uint32_t>(static_cast<std::int32_t>(zsf3));
    case 30: return static_cast<std::uint32_t>(static_cast<std::int32_t>(zsf4));
    case 31: return flag;
    default: return 0;
    }
}

void Gte::ctc2(std::uint32_t reg, std::uint32_t val) {
    switch (reg & 31u) {
    case 0: rt[0][0] = static_cast<std::int16_t>(val & 0xFFFFu); rt[0][1] = static_cast<std::int16_t>(val >> 16); break;
    case 1: rt[0][2] = static_cast<std::int16_t>(val & 0xFFFFu); rt[1][0] = static_cast<std::int16_t>(val >> 16); break;
    case 2: rt[1][1] = static_cast<std::int16_t>(val & 0xFFFFu); rt[1][2] = static_cast<std::int16_t>(val >> 16); break;
    case 3: rt[2][0] = static_cast<std::int16_t>(val & 0xFFFFu); rt[2][1] = static_cast<std::int16_t>(val >> 16); break;
    case 4: rt[2][2] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 5: tr[0] = static_cast<std::int32_t>(val); break;
    case 6: tr[1] = static_cast<std::int32_t>(val); break;
    case 7: tr[2] = static_cast<std::int32_t>(val); break;
    case 8: l[0][0] = static_cast<std::int16_t>(val & 0xFFFFu); l[0][1] = static_cast<std::int16_t>(val >> 16); break;
    case 9: l[0][2] = static_cast<std::int16_t>(val & 0xFFFFu); l[1][0] = static_cast<std::int16_t>(val >> 16); break;
    case 10: l[1][1] = static_cast<std::int16_t>(val & 0xFFFFu); l[1][2] = static_cast<std::int16_t>(val >> 16); break;
    case 11: l[2][0] = static_cast<std::int16_t>(val & 0xFFFFu); l[2][1] = static_cast<std::int16_t>(val >> 16); break;
    case 12: l[2][2] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 13: bk[0] = static_cast<std::int32_t>(val); break;
    case 14: bk[1] = static_cast<std::int32_t>(val); break;
    case 15: bk[2] = static_cast<std::int32_t>(val); break;
    case 16: lr[0][0] = static_cast<std::int16_t>(val & 0xFFFFu); lr[0][1] = static_cast<std::int16_t>(val >> 16); break;
    case 17: lr[0][2] = static_cast<std::int16_t>(val & 0xFFFFu); lr[1][0] = static_cast<std::int16_t>(val >> 16); break;
    case 18: lr[1][1] = static_cast<std::int16_t>(val & 0xFFFFu); lr[1][2] = static_cast<std::int16_t>(val >> 16); break;
    case 19: lr[2][0] = static_cast<std::int16_t>(val & 0xFFFFu); lr[2][1] = static_cast<std::int16_t>(val >> 16); break;
    case 20: lr[2][2] = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 21: fc[0] = static_cast<std::int32_t>(val); break;
    case 22: fc[1] = static_cast<std::int32_t>(val); break;
    case 23: fc[2] = static_cast<std::int32_t>(val); break;
    case 24: ofx = static_cast<std::int32_t>(val); break;
    case 25: ofy = static_cast<std::int32_t>(val); break;
    case 26: h = static_cast<std::uint16_t>(val & 0xFFFFu); break;
    case 27: dqa = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 28: dqb = static_cast<std::int32_t>(val); break;
    case 29: zsf3 = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 30: zsf4 = static_cast<std::int16_t>(val & 0xFFFFu); break;
    case 31:
        flag = val & 0x7FFFF000u;
        if (flag & 0x7F87E000u) flag |= (1u << 31);
        break;
    default: break;
    }
}

std::int64_t Gte::rtps_core(int vidx, int sf, bool lm, bool last) {
    const std::int64_t x = vx[vidx];
    const std::int64_t y = vy[vidx];
    const std::int64_t z = vz[vidx];

    std::int64_t m1 = check_mac123(1, (static_cast<std::int64_t>(tr[0]) << 12) + rt[0][0] * x);
    m1 = check_mac123(1, m1 + rt[0][1] * y);
    m1 = check_mac123(1, m1 + rt[0][2] * z);

    std::int64_t m2 = check_mac123(2, (static_cast<std::int64_t>(tr[1]) << 12) + rt[1][0] * x);
    m2 = check_mac123(2, m2 + rt[1][1] * y);
    m2 = check_mac123(2, m2 + rt[1][2] * z);

    std::int64_t m3 = check_mac123(3, (static_cast<std::int64_t>(tr[2]) << 12) + rt[2][0] * x);
    m3 = check_mac123(3, m3 + rt[2][1] * y);
    m3 = check_mac123(3, m3 + rt[2][2] * z);

    mac[1] = static_cast<std::int32_t>(m1 >> sf);
    mac[2] = static_cast<std::int32_t>(m2 >> sf);
    mac[3] = static_cast<std::int32_t>(m3 >> sf);

    set_ir123(1, mac[1], lm);
    set_ir123(2, mac[2], lm);
    // Hardware quirk: IR3 overflow flag checks m3 >> 12 (regardless of sf!), then clamps mac[3] with lm.
    const std::int32_t z_check = static_cast<std::int32_t>(m3 >> 12);
    if (z_check < -0x8000 || z_check > 0x7FFF) {
        flag |= (1u << 22);
    }
    ir[3] = static_cast<std::int16_t>(std::clamp<std::int32_t>(mac[3], lm ? 0 : -0x8000, 0x7FFF));

    push_sz(m3 >> 12);

    bool div_ovf = false;
    const std::uint32_t div = divide_unr(h, sz[3], div_ovf);
    if (div_ovf) flag |= (1u << 17);

    const std::int64_t sx_raw = check_mac0(static_cast<std::int64_t>(div) * ir[1] + ofx);
    const std::int64_t sy_raw = check_mac0(static_cast<std::int64_t>(div) * ir[2] + ofy);
    push_sxy(static_cast<std::int32_t>(sx_raw >> 16), static_cast<std::int32_t>(sy_raw >> 16));

    if (pgxp::is_enabled()) {
        const float sf_scale = (sf != 0) ? 4096.0f : 1.0f;
        const float vx_f = static_cast<float>(m1) / sf_scale;
        const float vy_f = static_cast<float>(m2) / sf_scale;
        const float vz_f = static_cast<float>(m3) / 4096.0f;
        const float fz = std::max(1.0f, vz_f);
        float fx = static_cast<float>(sx_raw) / 65536.0f;
        float fy = static_cast<float>(sy_raw) / 65536.0f;
        if (!div_ovf && vz_f > static_cast<float>(h) * 0.5f) {
            const float p_fx = (vx_f * static_cast<float>(h) / vz_f) + (static_cast<float>(ofx) / 65536.0f);
            const float p_fy = (vy_f * static_cast<float>(h) / vz_f) + (static_cast<float>(ofy) / 65536.0f);
            if (std::fabs(p_fx - static_cast<float>(sx[2])) <= 1.5f &&
                std::fabs(p_fy - static_cast<float>(sy[2])) <= 1.5f) {
                fx = p_fx;
                fy = p_fy;
            }
        }
        pgxp::record(sx[2], sy[2], fx, fy, fz);
    }

    if (last) {
        const std::int64_t p = check_mac0(static_cast<std::int64_t>(div) * dqa + dqb);
        mac[0] = static_cast<std::int32_t>(p);
        set_ir0(p >> 12);
    }
    return static_cast<std::int64_t>(div);
}

void Gte::ncs_core(int vidx, int sf, bool lm) {
    const std::int64_t x = vx[vidx], y = vy[vidx], z = vz[vidx];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, l[i][0] * x);
        acc = check_mac123(i + 1, acc + l[i][1] * y);
        acc = check_mac123(i + 1, acc + l[i][2] * z);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, (static_cast<std::int64_t>(bk[i]) << 12) + lr[i][0] * i1);
        acc = check_mac123(i + 1, acc + lr[i][1] * i2);
        acc = check_mac123(i + 1, acc + lr[i][2] * i3);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
}

void Gte::ncds_core(int vidx, int sf, bool lm) {
    const std::int64_t x = vx[vidx], y = vy[vidx], z = vz[vidx];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, l[i][0] * x);
        acc = check_mac123(i + 1, acc + l[i][1] * y);
        acc = check_mac123(i + 1, acc + l[i][2] * z);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, (static_cast<std::int64_t>(bk[i]) << 12) + lr[i][0] * i1);
        acc = check_mac123(i + 1, acc + lr[i][1] * i2);
        acc = check_mac123(i + 1, acc + lr[i][2] * i3);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    for (int i = 0; i < 3; ++i) {
        const std::int64_t c_val = (static_cast<std::int64_t>(rgbc[i]) * ir[i + 1]) << 4;
        const std::int64_t diff = check_mac123(i + 1, (static_cast<std::int64_t>(fc[i]) << 12) - c_val);
        const std::int16_t tmp_ir = set_ir123(i + 1, static_cast<std::int32_t>(diff >> sf), false);
        const std::int64_t res = check_mac123(i + 1, c_val + static_cast<std::int64_t>(ir[0]) * tmp_ir);
        mac[i + 1] = static_cast<std::int32_t>(res >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
}

void Gte::nccs_core(int vidx, int sf, bool lm) {
    const std::int64_t x = vx[vidx], y = vy[vidx], z = vz[vidx];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, l[i][0] * x);
        acc = check_mac123(i + 1, acc + l[i][1] * y);
        acc = check_mac123(i + 1, acc + l[i][2] * z);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
    for (int i = 0; i < 3; ++i) {
        std::int64_t acc = check_mac123(i + 1, (static_cast<std::int64_t>(bk[i]) << 12) + lr[i][0] * i1);
        acc = check_mac123(i + 1, acc + lr[i][1] * i2);
        acc = check_mac123(i + 1, acc + lr[i][2] * i3);
        mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    for (int i = 0; i < 3; ++i) {
        const std::int64_t prod = (static_cast<std::int64_t>(rgbc[i]) * ir[i + 1]) << 4;
        set_mac123(i + 1, prod, sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
}

void Gte::dpcs_core(bool use_fifo, int sf, bool lm) {
    const std::uint8_t *src = use_fifo ? rgb[0] : rgbc;
    for (int i = 0; i < 3; ++i) {
        const std::int64_t c_val = static_cast<std::int64_t>(src[i]) << 16;
        const std::int64_t diff = check_mac123(i + 1, (static_cast<std::int64_t>(fc[i]) << 12) - c_val);
        const std::int16_t tmp_ir = set_ir123(i + 1, static_cast<std::int32_t>(diff >> sf), false);
        const std::int64_t res = check_mac123(i + 1, c_val + static_cast<std::int64_t>(ir[0]) * tmp_ir);
        mac[i + 1] = static_cast<std::int32_t>(res >> sf);
        set_ir123(i + 1, mac[i + 1], lm);
    }
    push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
}

void Gte::mvmva(std::uint32_t cmd) {
    const int sf = (cmd & (1u << 19)) ? 12 : 0;
    const std::uint32_t mx = (cmd >> 17) & 3u;
    const std::uint32_t vx_sel = (cmd >> 15) & 3u;
    const std::uint32_t tx_sel = (cmd >> 13) & 3u;
    const bool lm = (cmd & (1u << 10)) != 0;

    std::int16_t m[3][3] = {};
    if (mx == 0) {
        std::memcpy(m, rt, sizeof(m));
    } else if (mx == 1) {
        std::memcpy(m, l, sizeof(m));
    } else if (mx == 2) {
        std::memcpy(m, lr, sizeof(m));
    } else {
        // Hardware bug matrix (mx == 3)
        m[0][0] = static_cast<std::int16_t>(-(static_cast<std::int16_t>(rgbc[0]) << 4));
        m[0][1] = static_cast<std::int16_t>(static_cast<std::int16_t>(rgbc[0]) << 4);
        m[0][2] = ir[0];
        m[1][0] = rt[0][2]; m[1][1] = rt[0][2]; m[1][2] = rt[0][2];
        m[2][0] = rt[1][1]; m[2][1] = rt[1][1]; m[2][2] = rt[1][1];
    }

    std::int64_t v[3] = {};
    if (vx_sel < 3u) {
        v[0] = vx[vx_sel];
        v[1] = vy[vx_sel];
        v[2] = vz[vx_sel];
    } else {
        v[0] = ir[1];
        v[1] = ir[2];
        v[2] = ir[3];
    }

    std::int64_t t[3] = {};
    if (tx_sel == 0) {
        t[0] = tr[0]; t[1] = tr[1]; t[2] = tr[2];
    } else if (tx_sel == 1) {
        t[0] = bk[0]; t[1] = bk[1]; t[2] = bk[2];
    } else if (tx_sel == 2) {
        t[0] = fc[0]; t[1] = fc[1]; t[2] = fc[2];
    }

    if (tx_sel == 2) {
        // Known hardware quirk for MVMVA with tx == FC: first term overflows IR, final uses terms 1 & 2.
        for (int i = 0; i < 3; ++i) {
            const std::int64_t first = check_mac123(i + 1, (t[i] << 12) + m[i][0] * v[0]);
            set_ir123(i + 1, static_cast<std::int32_t>(first >> sf), false);
            std::int64_t rest = check_mac123(i + 1, m[i][1] * v[1]);
            rest = check_mac123(i + 1, rest + m[i][2] * v[2]);
            mac[i + 1] = static_cast<std::int32_t>(rest >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            std::int64_t acc = check_mac123(i + 1, (t[i] << 12) + m[i][0] * v[0]);
            acc = check_mac123(i + 1, acc + m[i][1] * v[1]);
            acc = check_mac123(i + 1, acc + m[i][2] * v[2]);
            mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
    }
}

void Gte::execute(std::uint32_t cmd) {
    clear_flag();
    const std::uint32_t op = cmd & 0x3Fu;
    const int sf = (cmd & (1u << 19)) ? 12 : 0;
    const bool lm = (cmd & (1u << 10)) != 0;

    switch (op) {
    case 0x01:  // RTPS
        rtps_core(0, sf, lm, true);
        break;
    case 0x06: {  // NCLIP
        const std::int64_t p =
            static_cast<std::int64_t>(sx[0]) * sy[1] +
            static_cast<std::int64_t>(sx[1]) * sy[2] +
            static_cast<std::int64_t>(sx[2]) * sy[0] -
            static_cast<std::int64_t>(sx[0]) * sy[2] -
            static_cast<std::int64_t>(sx[1]) * sy[0] -
            static_cast<std::int64_t>(sx[2]) * sy[1];
        check_mac0(p);
        mac[0] = static_cast<std::int32_t>(p);
        break;
    }
    case 0x0C: {  // OP(sf) — outer product of diagonal RT and IR1..3
        const std::int64_t d1 = rt[0][0], d2 = rt[1][1], d3 = rt[2][2];
        const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
        set_mac123(1, i3 * d2 - i2 * d3, sf);
        set_mac123(2, i1 * d3 - i3 * d1, sf);
        set_mac123(3, i2 * d1 - i1 * d2, sf);
        set_ir123(1, mac[1], lm);
        set_ir123(2, mac[2], lm);
        set_ir123(3, mac[3], lm);
        break;
    }
    case 0x10:  // DPCS
        dpcs_core(false, sf, lm);
        break;
    case 0x11: {  // INTPL
        for (int i = 0; i < 3; ++i) {
            const std::int64_t cur = static_cast<std::int64_t>(ir[i + 1]) << 12;
            const std::int64_t diff = check_mac123(i + 1, (static_cast<std::int64_t>(fc[i]) << 12) - cur);
            const std::int16_t tmp_ir = set_ir123(i + 1, static_cast<std::int32_t>(diff >> sf), false);
            const std::int64_t res = check_mac123(i + 1, cur + static_cast<std::int64_t>(ir[0]) * tmp_ir);
            mac[i + 1] = static_cast<std::int32_t>(res >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x12:  // MVMVA
        mvmva(cmd);
        break;
    case 0x13:  // NCDS
        ncds_core(0, sf, lm);
        break;
    case 0x14: {  // CDP
        const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
        for (int i = 0; i < 3; ++i) {
            std::int64_t acc = check_mac123(i + 1, (static_cast<std::int64_t>(bk[i]) << 12) + lr[i][0] * i1);
            acc = check_mac123(i + 1, acc + lr[i][1] * i2);
            acc = check_mac123(i + 1, acc + lr[i][2] * i3);
            mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        for (int i = 0; i < 3; ++i) {
            const std::int64_t c_val = (static_cast<std::int64_t>(rgbc[i]) * ir[i + 1]) << 4;
            const std::int64_t diff = check_mac123(i + 1, (static_cast<std::int64_t>(fc[i]) << 12) - c_val);
            const std::int16_t tmp_ir = set_ir123(i + 1, static_cast<std::int32_t>(diff >> sf), false);
            const std::int64_t res = check_mac123(i + 1, c_val + static_cast<std::int64_t>(ir[0]) * tmp_ir);
            mac[i + 1] = static_cast<std::int32_t>(res >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x16:  // NCDT
        ncds_core(0, sf, lm);
        ncds_core(1, sf, lm);
        ncds_core(2, sf, lm);
        break;
    case 0x1B:  // NCCS
        nccs_core(0, sf, lm);
        break;
    case 0x1C: {  // CC
        const std::int64_t i1 = ir[1], i2 = ir[2], i3 = ir[3];
        for (int i = 0; i < 3; ++i) {
            std::int64_t acc = check_mac123(i + 1, (static_cast<std::int64_t>(bk[i]) << 12) + lr[i][0] * i1);
            acc = check_mac123(i + 1, acc + lr[i][1] * i2);
            acc = check_mac123(i + 1, acc + lr[i][2] * i3);
            mac[i + 1] = static_cast<std::int32_t>(acc >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        for (int i = 0; i < 3; ++i) {
            set_mac123(i + 1, (static_cast<std::int64_t>(rgbc[i]) * ir[i + 1]) << 4, sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x1E:  // NCS
        ncs_core(0, sf, lm);
        break;
    case 0x20:  // NCT
        ncs_core(0, sf, lm);
        ncs_core(1, sf, lm);
        ncs_core(2, sf, lm);
        break;
    case 0x28: {  // SQR(sf)
        set_mac123(1, static_cast<std::int64_t>(ir[1]) * ir[1], sf);
        set_mac123(2, static_cast<std::int64_t>(ir[2]) * ir[2], sf);
        set_mac123(3, static_cast<std::int64_t>(ir[3]) * ir[3], sf);
        set_ir123(1, mac[1], lm);
        set_ir123(2, mac[2], lm);
        set_ir123(3, mac[3], lm);
        break;
    }
    case 0x29: {  // DCPL
        for (int i = 0; i < 3; ++i) {
            const std::int64_t c_val = (static_cast<std::int64_t>(rgbc[i]) * ir[i + 1]) << 4;
            const std::int64_t diff = check_mac123(i + 1, (static_cast<std::int64_t>(fc[i]) << 12) - c_val);
            const std::int16_t tmp_ir = set_ir123(i + 1, static_cast<std::int32_t>(diff >> sf), false);
            const std::int64_t res = check_mac123(i + 1, c_val + static_cast<std::int64_t>(ir[0]) * tmp_ir);
            mac[i + 1] = static_cast<std::int32_t>(res >> sf);
            set_ir123(i + 1, mac[i + 1], lm);
        }
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x2A:  // DPCT
        dpcs_core(true, sf, lm);
        dpcs_core(true, sf, lm);
        dpcs_core(true, sf, lm);
        break;
    case 0x2D: {  // AVSZ3
        const std::int64_t p = static_cast<std::int64_t>(zsf3) * (sz[1] + sz[2] + sz[3]);
        check_mac0(p);
        mac[0] = static_cast<std::int32_t>(p);
        const std::int64_t z = p >> 12;
        if (z < 0) { flag |= (1u << 18); otz = 0; }
        else if (z > 0xFFFF) { flag |= (1u << 18); otz = 0xFFFF; }
        else { otz = static_cast<std::uint16_t>(z); }
        break;
    }
    case 0x2E: {  // AVSZ4
        const std::int64_t p = static_cast<std::int64_t>(zsf4) * (sz[0] + sz[1] + sz[2] + sz[3]);
        check_mac0(p);
        mac[0] = static_cast<std::int32_t>(p);
        const std::int64_t z = p >> 12;
        if (z < 0) { flag |= (1u << 18); otz = 0; }
        else if (z > 0xFFFF) { flag |= (1u << 18); otz = 0xFFFF; }
        else { otz = static_cast<std::uint16_t>(z); }
        break;
    }
    case 0x30:  // RTPT
        rtps_core(0, sf, lm, false);
        rtps_core(1, sf, lm, false);
        rtps_core(2, sf, lm, true);
        break;
    case 0x3D: {  // GPF(sf)
        set_mac123(1, static_cast<std::int64_t>(ir[0]) * ir[1], sf);
        set_mac123(2, static_cast<std::int64_t>(ir[0]) * ir[2], sf);
        set_mac123(3, static_cast<std::int64_t>(ir[0]) * ir[3], sf);
        set_ir123(1, mac[1], lm);
        set_ir123(2, mac[2], lm);
        set_ir123(3, mac[3], lm);
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x3E: {  // GPL(sf)
        set_mac123(1, (static_cast<std::int64_t>(mac[1]) << sf) + static_cast<std::int64_t>(ir[0]) * ir[1], sf);
        set_mac123(2, (static_cast<std::int64_t>(mac[2]) << sf) + static_cast<std::int64_t>(ir[0]) * ir[2], sf);
        set_mac123(3, (static_cast<std::int64_t>(mac[3]) << sf) + static_cast<std::int64_t>(ir[0]) * ir[3], sf);
        set_ir123(1, mac[1], lm);
        set_ir123(2, mac[2], lm);
        set_ir123(3, mac[3], lm);
        push_rgb(mac[1] >> 4, mac[2] >> 4, mac[3] >> 4);
        break;
    }
    case 0x3F:  // NCCT
        nccs_core(0, sf, lm);
        nccs_core(1, sf, lm);
        nccs_core(2, sf, lm);
        break;
    default:
        break;
    }

    finalize_flag();
}

}  // namespace psx
