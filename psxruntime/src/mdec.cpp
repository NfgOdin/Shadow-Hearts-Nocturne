#include "psx/mdec.hpp"

#include <algorithm>
#include <cstring>

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

// Default PS1 IDCT cosine matrix (used even before game issues MDEC Command 3).
constexpr std::int16_t kDefaultScaleTable[64] = {
     5792,  5792,  5792,  5792,  5792,  5792,  5792,  5792,
     8034,  6811,  4551,  1599, -1599, -4551, -6811, -8034,
     7568,  3134, -3134, -7568, -7568, -3134,  3134,  7568,
     6811, -1599, -8034, -4551,  4551,  8034,  1599, -6811,
     5792, -5792, -5792,  5792,  5792, -5792, -5792,  5792,
     4551, -8034,  1599,  6811, -6811, -1599,  8034, -4551,
     3134, -7568,  7568, -3134, -3134,  7568, -7568,  3134,
     1599, -4551,  6811, -8034,  8034, -6811,  4551, -1599,
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
    std::int32_t tmp[64] = {};
    for (int x = 0; x < 8; ++x) {
        for (int y = 0; y < 8; ++y) {
            std::int32_t sum = 0;
            for (int u = 0; u < 8; ++u) {
                sum += static_cast<std::int32_t>(blk[u * 8 + x]) * scale_table[u * 8 + y];
            }
            tmp[y * 8 + x] = (sum + 0x800) >> 12;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            std::int32_t sum = 0;
            for (int u = 0; u < 8; ++u) {
                sum += tmp[y * 8 + u] * scale_table[u * 8 + x];
            }
            const std::int32_t val = (sum + 0x20000) >> 18;
            blk[y * 8 + x] = static_cast<std::int16_t>(std::clamp(val, -128, 127));
        }
    }
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
            val = (ac_val * static_cast<std::int32_t>(qt[k]) * static_cast<std::int32_t>(q_scale) + 4) / 8;
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
        std::uint8_t rgb24[16 * 16 * 3] = {};
        std::uint16_t rgb15[16 * 16] = {};

        for (int py = 0; py < 16; ++py) {
            for (int px = 0; px < 16; ++px) {
                const int qidx = ((py >> 3) << 1) | (px >> 3);
                const int lum = y_quads[qidx][(py & 7) * 8 + (px & 7)];
                const int b_ch = cb[(py >> 1) * 8 + (px >> 1)];
                const int r_ch = cr[(py >> 1) * 8 + (px >> 1)];

                const int dr = (359 * r_ch + 128) >> 8;
                const int dg = (-88 * b_ch - 183 * r_ch + 128) >> 8;
                const int db = (454 * b_ch + 128) >> 8;

                const std::uint8_t r8 = clamp_rgb(lum + dr, signed_out);
                const std::uint8_t g8 = clamp_rgb(lum + dg, signed_out);
                const std::uint8_t b8 = clamp_rgb(lum + db, signed_out);

                const int idx = py * 16 + px;
                rgb24[idx * 3 + 0] = r8;
                rgb24[idx * 3 + 1] = g8;
                rgb24[idx * 3 + 2] = b8;
                rgb15[idx] = static_cast<std::uint16_t>((r8 >> 3) | ((g8 >> 3) << 5) | ((b8 >> 3) << 10) | mask_bit);
            }
        }

        if (depth == 3u) {
            // 15-bit output: 128 words (256 pixels)
            for (int i = 0; i < 256; i += 2) {
                out_fifo.push_back(static_cast<std::uint32_t>(rgb15[i]) |
                                   (static_cast<std::uint32_t>(rgb15[i + 1]) << 16));
            }
        } else {
            // 24-bit output: 192 words (768 bytes)
            for (int i = 0; i < 768; i += 4) {
                std::uint32_t w = 0;
                std::memcpy(&w, rgb24 + i, 4);
                out_fifo.push_back(w);
            }
        }
    }
}

}  // namespace psx
