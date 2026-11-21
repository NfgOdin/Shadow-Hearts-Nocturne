#pragma once

#include <cstdint>

namespace psx {

// PlayStation 1 Geometry Transformation Engine (COP2).
// Implements bit-exact fixed-point arithmetic, UNR division, color/screen FIFOs,
// and FLAG overflow/saturation semantics matching PS1 hardware.
struct Gte {
    // Data registers (0..31)
    std::int16_t vx[3] = {};
    std::int16_t vy[3] = {};
    std::int16_t vz[3] = {};
    std::uint8_t rgbc[4] = {};      // r, g, b, code (reg 6)
    std::uint16_t otz = 0;          // reg 7
    std::int16_t ir[4] = {};        // ir0..ir3 (regs 8..11)
    std::int16_t sx[4] = {};        // screen X FIFO: 0..2, 3=mirror of 2 (regs 12..15)
    std::int16_t sy[4] = {};        // screen Y FIFO
    std::uint16_t sz[4] = {};       // screen Z FIFO: sz0..sz3 (regs 16..19)
    std::uint8_t rgb[3][4] = {};    // color FIFO: rgb0..rgb2 (regs 20..22)
    std::uint32_t res1 = 0;         // prohibited reg 23 (preserved verbatim)
    std::int32_t mac[4] = {};       // mac0..mac3 (regs 24..27)
    std::uint16_t irgb = 0;         // reg 28/29
    std::int32_t lzcs = 0;          // reg 30
    std::int32_t lzcr = 32;         // reg 31

    // Control registers (0..31)
    std::int16_t rt[3][3] = {};     // rotation matrix (regs 0..4)
    std::int32_t tr[3] = {};        // translation vector (regs 5..7)
    std::int16_t l[3][3] = {};      // light source matrix (regs 8..12)
    std::int32_t bk[3] = {};        // background color (regs 13..15)
    std::int16_t lr[3][3] = {};     // light color matrix (regs 16..20)
    std::int32_t fc[3] = {};        // far color (regs 21..23)
    std::int32_t ofx = 0;           // screen offset X (reg 24)
    std::int32_t ofy = 0;           // screen offset Y (reg 25)
    std::uint16_t h = 0;            // projection plane distance (reg 26)
    std::int16_t dqa = 0;           // depth cueing param A (reg 27)
    std::int32_t dqb = 0;           // depth cueing param B (reg 28)
    std::int16_t zsf3 = 0;          // Z scale factor 3 (reg 29)
    std::int16_t zsf4 = 0;          // Z scale factor 4 (reg 30)
    std::uint32_t flag = 0;         // calculation flags (reg 31)

    void reset();

    std::uint32_t mfc2(std::uint32_t reg) const;
    void mtc2(std::uint32_t reg, std::uint32_t val);
    std::uint32_t cfc2(std::uint32_t reg) const;
    void ctc2(std::uint32_t reg, std::uint32_t val);

    // Execute a 25-bit COP2 command word (bits 0..24 of instruction).
    void execute(std::uint32_t cmd);

    // Hardware-exact UNR division (H / SZ3) returning 0..0x1FFFF.
    static std::uint32_t divide_unr(std::uint16_t h, std::uint16_t sz3, bool &overflow);

private:
    void clear_flag();
    void finalize_flag();

    std::int64_t check_mac0(std::int64_t v);
    std::int64_t check_mac123(int idx, std::int64_t v);
    std::int32_t set_mac123(int idx, std::int64_t v, int sf);
    std::int16_t set_ir123(int idx, std::int32_t v, bool lm);
    std::int16_t set_ir0(std::int64_t v);
    void push_sxy(std::int32_t x, std::int32_t y);
    void push_sz(std::int64_t z);
    void push_rgb(std::int32_t r, std::int32_t g, std::int32_t b);
    std::uint8_t clamp_color(int ch, std::int32_t v);

    std::int64_t rtps_core(int vidx, int sf, bool lm, bool last);
    void ncs_core(int vidx, int sf, bool lm);
    void ncds_core(int vidx, int sf, bool lm);
    void nccs_core(int vidx, int sf, bool lm);
    void dpcs_core(bool use_fifo, int sf, bool lm);
    void mvmva(std::uint32_t cmd);
};

}  // namespace psx
