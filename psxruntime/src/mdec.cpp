#include "psx/mdec.hpp"

#include <algorithm>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace psx {

namespace {

// Standard PS1 zig-zag inverse scan table (zagzig[scan_pos] = matrix_index).
constexpr std::uint8_t kZagZig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

// Default PS1 IDCT cosine matrix (Q16 fixed-point, matching Sony PsyQ DecDCTReset Command 3).
constexpr std::int16_t kDefaultScaleTable[64] = {
     23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170,
     32138,  27245,  18204,   6392,  -6392, -18204, -27245, -32138,
     30273,  12539, -12539, -30273, -30273, -12539,  12539,  30273,
     27245,  -6392, -32138, -18204,  18204,  32138,   6392, -27245,
     23170, -23170, -23170,  23170,  23170, -23170, -23170,  23170,
     18204, -32138,   6392,  27245, -27245,  -6392,  32138, -18204,
     12539, -30273,  30273, -12539, -12539,  30273, -30273,  12539,
      6392, -18204,  27245, -32138,  32138, -27245,  18204,  -6392,
};

inline std::int32_t sign_extend10(std::uint16_t v) noexcept {
    return (static_cast<std::int32_t>(v << 22)) >> 22;
}

inline std::uint8_t clamp_rgb(int v, bool signed_out) noexcept {
    int c = std::clamp(v, -128, 127);
    if (!signed_out) c += 128;
    return static_cast<std::uint8_t>(c);
}

}  // namespace

Mdec::Mdec() {
    reset();
}

void Mdec::reset() {
    iq_y.fill(2);
    iq_uv.fill(2);
    std::memcpy(scale_table.data(), kDefaultScaleTable, sizeof(kDefaultScaleTable));
    cmd_header = 0;
    words_remaining = 0;
    active_cmd = 0;
    in_halfwords.clear();
    out_fifo.clear();
}

void Mdec::write_ctrl(std::uint32_t word) {
    if (word & (1u << 31)) {
        cmd_header = 0;
        words_remaining = 0;
        active_cmd = 0;
        in_halfwords.clear();
        out_fifo.clear();
    }
}

std::uint32_t Mdec::read_data() {
    if (out_fifo.empty()) return 0u;
    const std::uint32_t w = out_fifo.front();
    out_fifo.pop_front();
    return w;
}

std::uint32_t Mdec::read_stat() const {
    std::uint32_t st = 0;
    if (out_fifo.empty()) st |= (1u << 31);          // Data-out FIFO empty
    if (words_remaining > 0) st |= (1u << 29);       // Command busy
    st |= (1u << 28);                                // Data-in request
    if (!out_fifo.empty()) st |= (1u << 27);         // Data-out request
    st |= ((cmd_header >> 25) & 0xFu) << 23;         // Depth / signed / bit15
    st |= (4u << 16);                                // Current block = Y4 (ready)
    st |= static_cast<std::uint32_t>((words_remaining - 1u) & 0xFFFFu);
    return st;
}

void Mdec::write_cmd(std::uint32_t word) {
    if (active_cmd == 0) {
        cmd_header = word;
        const std::uint32_t op = (word >> 29) & 7u;
        if (op == 1u) {
            active_cmd = 1;
            words_remaining = word & 0xFFFFu;
            if (words_remaining == 0) words_remaining = 0x10000u;
        } else if (op == 2u) {
            active_cmd = 2;
            words_remaining = (word & 1u) ? 32u : 16u;
        } else if (op == 3u) {
            active_cmd = 3;
            words_remaining = 32u;
        } else {
            active_cmd = 0;
            words_remaining = 0;
        }
        in_halfwords.clear();
        in_halfwords.reserve(words_remaining * 2u);
        return;
    }

    in_halfwords.push_back(static_cast<std::uint16_t>(word & 0xFFFFu));
    in_halfwords.push_back(static_cast<std::uint16_t>(word >> 16));
    if (--words_remaining == 0) {
        run_command();
        active_cmd = 0;
        in_halfwords.clear();
    }
}

void Mdec::idct_8x8(std::int16_t blk[64]) const {
    const std::int16_t *const st = scale_table.data();
#if defined(__SSE2__)
    // Pure SSE2 Q16 8x8 IDCT — 100% bit-exact with the scalar 64-bit reference below.
    // Pass 1: all 8 columns (x=0..7) in parallel. |blk| <= 1024, |st| <= 32768 => |sum| <= 2^28 (fits int32).
    const __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 0));
    const __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 8));
    const __m128i r2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 16));
    const __m128i r3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 24));
    const __m128i r4 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 32));
    const __m128i r5 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 40));
    const __m128i r6 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 48));
    const __m128i r7 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(blk + 56));

    const __m128i lo01 = _mm_unpacklo_epi16(r0, r1), hi01 = _mm_unpackhi_epi16(r0, r1);
    const __m128i lo23 = _mm_unpacklo_epi16(r2, r3), hi23 = _mm_unpackhi_epi16(r2, r3);
    const __m128i lo45 = _mm_unpacklo_epi16(r4, r5), hi45 = _mm_unpackhi_epi16(r4, r5);
    const __m128i lo67 = _mm_unpacklo_epi16(r6, r7), hi67 = _mm_unpackhi_epi16(r6, r7);

    const auto pair16 = [](std::int16_t a, std::int16_t b) noexcept -> __m128i {
        const std::uint32_t w = static_cast<std::uint16_t>(a) |
                                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(b)) << 16);
        return _mm_set1_epi32(static_cast<int>(w));
    };
    const __m128i kRound14 = _mm_set1_epi32(0x2000);

    __m128i tmp_rows[8];
    for (int y = 0; y < 8; ++y) {
        const __m128i c01 = pair16(st[0 * 8 + y], st[1 * 8 + y]);
        const __m128i c23 = pair16(st[2 * 8 + y], st[3 * 8 + y]);
        const __m128i c45 = pair16(st[4 * 8 + y], st[5 * 8 + y]);
        const __m128i c67 = pair16(st[6 * 8 + y], st[7 * 8 + y]);

        const __m128i sum_lo = _mm_add_epi32(
            _mm_add_epi32(_mm_madd_epi16(lo01, c01), _mm_madd_epi16(lo23, c23)),
            _mm_add_epi32(_mm_madd_epi16(lo45, c45), _mm_madd_epi16(lo67, c67)));
        const __m128i sum_hi = _mm_add_epi32(
            _mm_add_epi32(_mm_madd_epi16(hi01, c01), _mm_madd_epi16(hi23, c23)),
            _mm_add_epi32(_mm_madd_epi16(hi45, c45), _mm_madd_epi16(hi67, c67)));

        const __m128i q_lo = _mm_srai_epi32(_mm_add_epi32(sum_lo, kRound14), 14);
        const __m128i q_hi = _mm_srai_epi32(_mm_add_epi32(sum_hi, kRound14), 14);
        // |q| <= 16384, so packing to int16_t is strictly lossless.
        tmp_rows[y] = _mm_packs_epi32(q_lo, q_hi);
    }

    // Pass 2: all 8 outputs (x=0..7) per row in parallel, accumulating 4 pair products in exact 64-bit
    // so worst-case custom scale tables never overflow int32_t.
    const __m128i sr0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 0));
    const __m128i sr1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 8));
    const __m128i sr2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 16));
    const __m128i sr3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 24));
    const __m128i sr4 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 32));
    const __m128i sr5 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 40));
    const __m128i sr6 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 48));
    const __m128i sr7 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(st + 56));

    const __m128i slo01 = _mm_unpacklo_epi16(sr0, sr1), shi01 = _mm_unpackhi_epi16(sr0, sr1);
    const __m128i slo23 = _mm_unpacklo_epi16(sr2, sr3), shi23 = _mm_unpackhi_epi16(sr2, sr3);
    const __m128i slo45 = _mm_unpacklo_epi16(sr4, sr5), shi45 = _mm_unpackhi_epi16(sr4, sr5);
    const __m128i slo67 = _mm_unpacklo_epi16(sr6, sr7), shi67 = _mm_unpackhi_epi16(sr6, sr7);

    const __m128i kBias = _mm_set1_epi64x((1LL << 40) + 0x20000LL);
    const __m128i kUnbias = _mm_set1_epi32(1 << 22);
    const __m128i kMin128 = _mm_set1_epi16(-128);
    const __m128i kMax127 = _mm_set1_epi16(127);

    const auto sum4_shift18 = [&](__m128i a, __m128i b, __m128i c, __m128i d) noexcept -> __m128i {
        const __m128i sa = _mm_srai_epi32(a, 31), sb = _mm_srai_epi32(b, 31);
        const __m128i sc = _mm_srai_epi32(c, 31), sd = _mm_srai_epi32(d, 31);
        const __m128i lo64 = _mm_add_epi64(
            _mm_add_epi64(_mm_add_epi64(_mm_unpacklo_epi32(a, sa), _mm_unpacklo_epi32(b, sb)),
                          _mm_add_epi64(_mm_unpacklo_epi32(c, sc), _mm_unpacklo_epi32(d, sd))),
            kBias);
        const __m128i hi64 = _mm_add_epi64(
            _mm_add_epi64(_mm_add_epi64(_mm_unpackhi_epi32(a, sa), _mm_unpackhi_epi32(b, sb)),
                          _mm_add_epi64(_mm_unpackhi_epi32(c, sc), _mm_unpackhi_epi32(d, sd))),
            kBias);
        const __m128i slo = _mm_srli_epi64(lo64, 18);
        const __m128i shi = _mm_srli_epi64(hi64, 18);
        const __m128i packed = _mm_castps_si128(
            _mm_shuffle_ps(_mm_castsi128_ps(slo), _mm_castsi128_ps(shi), _MM_SHUFFLE(2, 0, 2, 0)));
        return _mm_sub_epi32(packed, kUnbias);
    };

    for (int y = 0; y < 8; ++y) {
        const __m128i tr = tmp_rows[y];
        const __m128i t01 = _mm_shuffle_epi32(tr, _MM_SHUFFLE(0, 0, 0, 0));
        const __m128i t23 = _mm_shuffle_epi32(tr, _MM_SHUFFLE(1, 1, 1, 1));
        const __m128i t45 = _mm_shuffle_epi32(tr, _MM_SHUFFLE(2, 2, 2, 2));
        const __m128i t67 = _mm_shuffle_epi32(tr, _MM_SHUFFLE(3, 3, 3, 3));

        const __m128i out_lo = sum4_shift18(
            _mm_madd_epi16(slo01, t01), _mm_madd_epi16(slo23, t23),
            _mm_madd_epi16(slo45, t45), _mm_madd_epi16(slo67, t67));
        const __m128i out_hi = sum4_shift18(
            _mm_madd_epi16(shi01, t01), _mm_madd_epi16(shi23, t23),
            _mm_madd_epi16(shi45, t45), _mm_madd_epi16(shi67, t67));

        __m128i out16 = _mm_packs_epi32(out_lo, out_hi);
        out16 = _mm_min_epi16(_mm_max_epi16(out16, kMin128), kMax127);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(blk + y * 8), out16);
    }
#else
    std::int32_t tmp[64] = {};
    for (int x = 0; x < 8; ++x) {
        // Most DCT columns are all-zero (especially high-frequency AC); skip them bit-exactly.
        std::int32_t col_or = 0;
        for (int u = 0; u < 8; ++u) col_or |= blk[u * 8 + x];
        if (col_or == 0) continue;
        for (int y = 0; y < 8; ++y) {
            std::int64_t sum = 0;
            for (int u = 0; u < 8; ++u) {
                sum += static_cast<std::int64_t>(blk[u * 8 + x]) * st[u * 8 + y];
            }
            tmp[y * 8 + x] = static_cast<std::int32_t>((sum + 0x2000) >> 14);
        }
    }
    for (int y = 0; y < 8; ++y) {
        const std::int32_t *const trow = tmp + y * 8;
        std::int16_t *const brow = blk + y * 8;
        for (int x = 0; x < 8; ++x) {
            std::int64_t sum = 0;
            for (int u = 0; u < 8; ++u) {
                sum += static_cast<std::int64_t>(trow[u]) * st[u * 8 + x];
            }
            const std::int32_t val = static_cast<std::int32_t>((sum + 0x20000) >> 18);
            brow[x] = static_cast<std::int16_t>(std::clamp(val, -128, 127));
        }
    }
#endif
}

bool Mdec::decode_block(std::size_t &hw_pos, std::int16_t blk[64], const std::uint8_t qt[64]) {
    std::memset(blk, 0, sizeof(std::int16_t) * 64);
    while (hw_pos < in_halfwords.size() && in_halfwords[hw_pos] == 0xFE00u) {
        ++hw_pos;
    }
    if (hw_pos >= in_halfwords.size()) return false;

    const std::uint16_t dc_hw = in_halfwords[hw_pos++];
    const std::uint32_t q_scale = (dc_hw >> 10) & 0x3Fu;
    const std::int32_t dc_val = sign_extend10(dc_hw);

    if (q_scale == 0) {
        blk[0] = static_cast<std::int16_t>(std::clamp(dc_val * 2, -1024, 1023));
    } else {
        blk[0] = static_cast<std::int16_t>(std::clamp(dc_val * static_cast<std::int32_t>(qt[0]), -1024, 1023));
    }

    int k = 0;
    while (hw_pos < in_halfwords.size()) {
        const std::uint16_t ac_hw = in_halfwords[hw_pos++];
        if (ac_hw == 0xFE00u) break;
        k += ((ac_hw >> 10) & 0x3Fu) + 1;
        if (k >= 64) continue;
        const std::int32_t ac_val = sign_extend10(ac_hw);
        std::int32_t val = 0;
        if (q_scale == 0) {
            val = ac_val * 2;
            blk[k & 63] = static_cast<std::int16_t>(std::clamp(val, -1024, 1023));
        } else {
            val = (ac_val * static_cast<std::int32_t>(qt[k]) * static_cast<std::int32_t>(q_scale) + 4) >> 3;
            blk[kZagZig[k]] = static_cast<std::int16_t>(std::clamp(val, -1024, 1023));
        }
    }

    idct_8x8(blk);
    ++blocks_decoded;
    return true;
}

void Mdec::run_command() {
    if (active_cmd == 2) {
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(in_halfwords.data());
        std::memcpy(iq_y.data(), bytes, 64);
        if (in_halfwords.size() >= 64) {
            std::memcpy(iq_uv.data(), bytes + 64, 64);
        }
        return;
    }

    if (active_cmd == 3) {
        if (in_halfwords.size() >= 64) {
            std::memcpy(scale_table.data(), in_halfwords.data(), 64 * sizeof(std::int16_t));
        }
        return;
    }

    if (active_cmd != 1) return;

    const std::uint32_t depth = (cmd_header >> 27) & 3u;
    const bool signed_out = (cmd_header & (1u << 26)) != 0;
    const std::uint16_t mask_bit = (cmd_header & (1u << 25)) ? 0x8000u : 0u;

    out_fifo.clear();
    std::size_t pos = 0;

    if (depth < 2u) {
        // Monochrome 8x8 blocks (4-bit or 8-bit)
        std::int16_t yblk[64];
        while (decode_block(pos, yblk, iq_y.data())) {
            for (int i = 0; i < 64; i += 4) {
                const std::uint32_t b0 = clamp_rgb(yblk[i + 0], signed_out);
                const std::uint32_t b1 = clamp_rgb(yblk[i + 1], signed_out);
                const std::uint32_t b2 = clamp_rgb(yblk[i + 2], signed_out);
                const std::uint32_t b3 = clamp_rgb(yblk[i + 3], signed_out);
                out_fifo.push_back(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
            }
        }
        return;
    }

    // Color 16x16 macroblocks: Cr, Cb, Y1, Y2, Y3, Y4
    std::int16_t cr[64], cb[64], y1[64], y2[64], y3[64], y4[64];
    while (pos < in_halfwords.size()) {
        if (!decode_block(pos, cr, iq_uv.data())) break;
        if (!decode_block(pos, cb, iq_uv.data())) break;
        if (!decode_block(pos, y1, iq_y.data())) break;
        if (!decode_block(pos, y2, iq_y.data())) break;
        if (!decode_block(pos, y3, iq_y.data())) break;
        if (!decode_block(pos, y4, iq_y.data())) break;

        const std::int16_t *y_quads[4] = {y1, y2, y3, y4};
        if (depth == 3u) {
            // 15-bit output: 128 words (256 pixels)
            std::uint16_t rgb15[16 * 16];
            for (int py = 0; py < 16; ++py) {
                const int cy = (py >> 1) * 8;
                const int yrow = (py & 7) * 8;
                const int qrow = (py >> 3) << 1;
                for (int px = 0; px < 16; ++px) {
                    const int lum = y_quads[qrow | (px >> 3)][yrow + (px & 7)];
                    const int cidx = cy + (px >> 1);
                    const int b_ch = cb[cidx];
                    const int r_ch = cr[cidx];
                    const int dr = (359 * r_ch + 128) >> 8;
                    const int dg = (-88 * b_ch - 183 * r_ch + 128) >> 8;
                    const int db = (454 * b_ch + 128) >> 8;
                    const std::uint8_t r8 = clamp_rgb(lum + dr, signed_out);
                    const std::uint8_t g8 = clamp_rgb(lum + dg, signed_out);
                    const std::uint8_t b8 = clamp_rgb(lum + db, signed_out);
                    rgb15[py * 16 + px] = static_cast<std::uint16_t>(
                        (r8 >> 3) | ((g8 >> 3) << 5) | ((b8 >> 3) << 10) | mask_bit);
                }
            }
            for (int i = 0; i < 256; i += 2) {
                out_fifo.push_back(static_cast<std::uint32_t>(rgb15[i]) |
                                   (static_cast<std::uint32_t>(rgb15[i + 1]) << 16));
            }
        } else {
            // 24-bit output: 192 words (768 bytes)
            std::uint8_t rgb24[16 * 16 * 3];
            for (int py = 0; py < 16; ++py) {
                const int cy = (py >> 1) * 8;
                const int yrow = (py & 7) * 8;
                const int qrow = (py >> 3) << 1;
                for (int px = 0; px < 16; ++px) {
                    const int lum = y_quads[qrow | (px >> 3)][yrow + (px & 7)];
                    const int cidx = cy + (px >> 1);
                    const int b_ch = cb[cidx];
                    const int r_ch = cr[cidx];
                    const int dr = (359 * r_ch + 128) >> 8;
                    const int dg = (-88 * b_ch - 183 * r_ch + 128) >> 8;
                    const int db = (454 * b_ch + 128) >> 8;
                    const int idx = (py * 16 + px) * 3;
                    rgb24[idx + 0] = clamp_rgb(lum + dr, signed_out);
                    rgb24[idx + 1] = clamp_rgb(lum + dg, signed_out);
                    rgb24[idx + 2] = clamp_rgb(lum + db, signed_out);
                }
            }
            for (int i = 0; i < 768; i += 4) {
                std::uint32_t w = 0;
                std::memcpy(&w, rgb24 + i, 4);
                out_fifo.push_back(w);
            }
        }
    }
}

}  // namespace psx
