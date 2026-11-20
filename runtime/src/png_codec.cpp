#include "ps2/png_codec.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace ps2::png {
namespace {

// Canonical Huffman table construction (Huff::build, decode_huff) and RFC 1951 block
// decoding in inflate_rfc1951 are derived from Mark Adler's puff.c / puff.h
// (zlib contrib/puff, version 2.3). This is an altered version, rewritten in C++.
//
// Copyright (C) 2002-2013 Mark Adler, all rights reserved
// version 2.3, 21 Jan 2013
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the author be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//
// Mark Adler    madler@alumni.caltech.edu
bool inflate_rfc1951(const std::uint8_t *src, std::size_t src_len, std::vector<std::uint8_t> &out) {
    std::size_t bit_pos = 0;
    const std::size_t total_bits = src_len * 8;
    const auto read_bits = [&](int n) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (bit_pos >= total_bits) return 0;
            v |= static_cast<std::uint32_t>((src[bit_pos >> 3] >> (bit_pos & 7)) & 1u) << i;
            ++bit_pos;
        }
        return v;
    };
    struct Huff {
        std::uint16_t counts[16] = {};
        std::uint16_t symbols[320] = {};
        void build(const std::uint8_t *lengths, int n) {
            std::memset(counts, 0, sizeof counts);
            for (int i = 0; i < n; ++i)
                if (lengths[i] < 16) ++counts[lengths[i]];
            counts[0] = 0;
            std::uint16_t offs[16] = {};
            for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + counts[i];
            for (int i = 0; i < n; ++i)
                if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
        }
    };
    const auto decode_huff = [&](const Huff &h) -> int {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(read_bits(1));
            const int count = h.counts[len];
            if (code - count < first) return h.symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    };
    static constexpr int kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                          33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                          1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static constexpr int kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    while (bit_pos < total_bits) {
        const std::uint32_t bfinal = read_bits(1);
        const std::uint32_t btype = read_bits(2);
        if (btype == 0) {
            bit_pos = (bit_pos + 7) & ~static_cast<std::size_t>(7);
            if (bit_pos + 32 > total_bits) return false;
            const std::size_t byte_pos = bit_pos >> 3;
            const std::uint16_t len = static_cast<std::uint16_t>(src[byte_pos] | (src[byte_pos + 1] << 8));
            bit_pos += 32;
            if ((bit_pos >> 3) + len > src_len) return false;
            out.insert(out.end(), src + (bit_pos >> 3), src + (bit_pos >> 3) + len);
            bit_pos += static_cast<std::size_t>(len) * 8;
        } else if (btype == 1 || btype == 2) {
            Huff lit_huff, dist_huff;
            if (btype == 1) {
                std::uint8_t lit_lens[288], dist_lens[32];
                for (int i = 0; i < 144; ++i) lit_lens[i] = 8;
                for (int i = 144; i < 256; ++i) lit_lens[i] = 9;
                for (int i = 256; i < 280; ++i) lit_lens[i] = 7;
                for (int i = 280; i < 288; ++i) lit_lens[i] = 8;
                for (int i = 0; i < 32; ++i) dist_lens[i] = 5;
                lit_huff.build(lit_lens, 288);
                dist_huff.build(dist_lens, 32);
            } else {
                const int hlit = static_cast<int>(read_bits(5)) + 257;
                const int hdist = static_cast<int>(read_bits(5)) + 1;
                const int hclen = static_cast<int>(read_bits(4)) + 4;
                if (hlit > 286 || hdist > 30) return false;
                std::uint8_t cl_lens[19] = {};
                for (int i = 0; i < hclen; ++i) cl_lens[kClOrder[i]] = static_cast<std::uint8_t>(read_bits(3));
                Huff cl_huff;
                cl_huff.build(cl_lens, 19);
                std::uint8_t lens[320] = {};
                int idx = 0;
                while (idx < hlit + hdist) {
                    const int sym = decode_huff(cl_huff);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lens[idx++] = static_cast<std::uint8_t>(sym);
                    } else if (sym == 16) {
                        if (idx == 0) return false;
                        const int rep = static_cast<int>(read_bits(2)) + 3;
                        const std::uint8_t prev = lens[idx - 1];
                        for (int r = 0; r < rep && idx < hlit + hdist; ++r) lens[idx++] = prev;
                    } else if (sym == 17) {
                        const int rep = static_cast<int>(read_bits(3)) + 3;
                        for (int r = 0; r < rep && idx < hlit + hdist; ++r) lens[idx++] = 0;
                    } else {
                        const int rep = static_cast<int>(read_bits(7)) + 11;
                        for (int r = 0; r < rep && idx < hlit + hdist; ++r) lens[idx++] = 0;
                    }
                }
                lit_huff.build(lens, hlit);
                dist_huff.build(lens + hlit, hdist);
            }
            for (;;) {
                const int sym = decode_huff(lit_huff);
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back(static_cast<std::uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li >= 29) return false;
                    const int length = kLenBase[li] + static_cast<int>(read_bits(kLenExtra[li]));
                    const int di = decode_huff(dist_huff);
                    if (di < 0 || di >= 30) return false;
                    const std::size_t dist = static_cast<std::size_t>(kDistBase[di] + read_bits(kDistExtra[di]));
                    if (dist == 0 || dist > out.size()) return false;
                    for (int k = 0; k < length; ++k) out.push_back(out[out.size() - dist]);
                }
            }
        } else {
            return false;
        }
        if (bfinal) break;
    }
    return true;
}

void push_be32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

constexpr std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

std::uint32_t crc32_bytes(const std::uint8_t *data, std::size_t len) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = kCrc32Table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32_bytes(const std::uint8_t *data, std::size_t len) {
    std::uint32_t s1 = 1, s2 = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::size_t chunk = std::min<std::size_t>(len - i, 5552);
        for (std::size_t j = 0; j < chunk; ++j) {
            s1 += data[i + j];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        i += chunk;
    }
    return (s2 << 16) | s1;
}

struct BitWriter {
    std::vector<std::uint8_t> &out;
    std::uint64_t bits = 0;
    int count = 0;

    void put(std::uint32_t val, int nbits) {
        bits |= static_cast<std::uint64_t>(val) << count;
        count += nbits;
        while (count >= 8) {
            out.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
            bits >>= 8;
            count -= 8;
        }
    }

    void flush() {
        if (count > 0) {
            out.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
            bits = 0;
            count = 0;
        }
    }
};

constexpr std::uint16_t rev_bits(std::uint16_t v, int n) {
    std::uint16_t r = 0;
    for (int i = 0; i < n; ++i) {
        r = static_cast<std::uint16_t>((r << 1) | (v & 1u));
        v >>= 1;
    }
    return r;
}

struct FixedTables {
    std::array<std::uint16_t, 288> lit_code{};
    std::array<std::uint8_t, 288> lit_len{};
    std::array<std::uint8_t, 30> dist_code{};

    constexpr FixedTables() {
        for (int i = 0; i <= 143; ++i) {
            lit_code[i] = rev_bits(static_cast<std::uint16_t>(0x30 + i), 8);
            lit_len[i] = 8;
        }
        for (int i = 144; i <= 255; ++i) {
            lit_code[i] = rev_bits(static_cast<std::uint16_t>(0x190 + (i - 144)), 9);
            lit_len[i] = 9;
        }
        for (int i = 256; i <= 279; ++i) {
            lit_code[i] = rev_bits(static_cast<std::uint16_t>(i - 256), 7);
            lit_len[i] = 7;
        }
        for (int i = 280; i <= 287; ++i) {
            lit_code[i] = rev_bits(static_cast<std::uint16_t>(0xC0 + (i - 280)), 8);
            lit_len[i] = 8;
        }
        for (int i = 0; i < 30; ++i) dist_code[i] = static_cast<std::uint8_t>(rev_bits(static_cast<std::uint16_t>(i), 5));
    }
};

constexpr FixedTables kFixed{};

void emit_length(BitWriter &bw, std::uint32_t len) {
    static constexpr std::uint16_t kBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::uint8_t kExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int idx = 28;
    if (len < 258) {
        idx = 0;
        while (idx < 27 && len >= kBase[idx + 1]) ++idx;
    }
    const int sym = 257 + idx;
    bw.put(kFixed.lit_code[sym], kFixed.lit_len[sym]);
    if (kExtra[idx]) bw.put(len - kBase[idx], kExtra[idx]);
}

void emit_distance(BitWriter &bw, std::uint32_t dist) {
    static constexpr std::uint16_t kBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::uint8_t kExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int idx = 0;
    while (idx < 29 && dist >= kBase[idx + 1]) ++idx;
    bw.put(kFixed.dist_code[idx], 5);
    if (kExtra[idx]) bw.put(dist - kBase[idx], kExtra[idx]);
}

std::vector<std::uint8_t> build_zlib_stream(const std::uint8_t *raw, std::size_t raw_size) {
    const std::uint32_t adler = adler32_bytes(raw, raw_size);
    std::vector<std::uint8_t> z;
    z.reserve(raw_size / 2 + 64);
    z.push_back(0x78);
    z.push_back(0x01);

    BitWriter bw{z};
    bw.put(0x03u, 3);  // BFINAL=1, BTYPE=01 (fixed Huffman)

    constexpr std::uint32_t kHashBits = 15;
    constexpr std::uint32_t kHashSize = 1u << kHashBits;
    constexpr std::uint32_t kHashMask = kHashSize - 1u;
    std::vector<std::int32_t> head(kHashSize, -1);

    const auto hash3 = [](const std::uint8_t *p) -> std::uint32_t {
        const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    };

    std::size_t pos = 0;
    while (pos < raw_size) {
        std::size_t best_len = 0;
        std::uint32_t best_dist = 0;
        if (pos + 3 <= raw_size) {
            const std::uint32_t h = hash3(raw + pos) & kHashMask;
            const std::int32_t cand = head[h];
            head[h] = static_cast<std::int32_t>(pos);
            if (cand >= 0) {
                const std::size_t dist = pos - static_cast<std::size_t>(cand);
                if (dist > 0 && dist <= 32768u) {
                    const std::size_t max_len = std::min<std::size_t>(258u, raw_size - pos);
                    std::size_t m = 0;
                    const std::uint8_t *a = raw + cand;
                    const std::uint8_t *b = raw + pos;
                    while (m < max_len && a[m] == b[m]) ++m;
                    if (m >= 3) {
                        best_len = m;
                        best_dist = static_cast<std::uint32_t>(dist);
                    }
                }
            }
        }
        if (best_len >= 3) {
            emit_length(bw, static_cast<std::uint32_t>(best_len));
            emit_distance(bw, best_dist);
            const std::size_t end = pos + best_len;
            for (std::size_t p = pos + 1; p < end && p + 3 <= raw_size; ++p)
                head[hash3(raw + p) & kHashMask] = static_cast<std::int32_t>(p);
            pos = end;
        } else {
            const std::uint8_t b = raw[pos++];
            bw.put(kFixed.lit_code[b], kFixed.lit_len[b]);
        }
    }
    bw.put(kFixed.lit_code[256], kFixed.lit_len[256]);
    bw.flush();

    constexpr std::size_t kMaxBlock = 65535;
    const std::size_t stored_est = 2 + raw_size + ((raw_size + kMaxBlock - 1) / kMaxBlock) * 5;
    if (z.size() > stored_est) {
        z.clear();
        z.reserve(stored_est + 4);
        z.push_back(0x78);
        z.push_back(0x01);
        std::size_t off = 0;
        while (off < raw_size) {
            const std::size_t chunk = std::min(kMaxBlock, raw_size - off);
            const bool last = (off + chunk == raw_size);
            z.push_back(last ? 0x01 : 0x00);
            const std::uint16_t len16 = static_cast<std::uint16_t>(chunk);
            const std::uint16_t nlen16 = static_cast<std::uint16_t>(~len16);
            z.push_back(static_cast<std::uint8_t>(len16 & 0xFFu));
            z.push_back(static_cast<std::uint8_t>((len16 >> 8) & 0xFFu));
            z.push_back(static_cast<std::uint8_t>(nlen16 & 0xFFu));
            z.push_back(static_cast<std::uint8_t>((nlen16 >> 8) & 0xFFu));
            z.insert(z.end(), raw + off, raw + off + chunk);
            off += chunk;
        }
    }
    push_be32(z, adler);
    return z;
}

void push_chunk(std::vector<std::uint8_t> &out, const char type[4], const std::uint8_t *payload, std::size_t len) {
    push_be32(out, static_cast<std::uint32_t>(len));
    const std::size_t start = out.size();
    out.insert(out.end(), reinterpret_cast<const std::uint8_t *>(type), reinterpret_cast<const std::uint8_t *>(type) + 4);
    if (len > 0) out.insert(out.end(), payload, payload + len);
    const std::uint32_t crc = crc32_bytes(out.data() + start, 4 + len);
    push_be32(out, crc);
}

}  // namespace

bool decode_rgba_bytes(const std::uint8_t *data, std::size_t len, std::uint32_t &out_w, std::uint32_t &out_h,
                       std::vector<std::uint32_t> &out_words, bool ps2_alpha) {
    if (!data || len < 33 || len > 64u * 1024u * 1024u) return false;
    static constexpr std::uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(data, kSig, 8) != 0) return false;

    const auto be32 = [](const std::uint8_t *p) -> std::uint32_t {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    };
    std::uint32_t w = 0, h = 0;
    std::uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<std::uint8_t> idat, plte, trns;
    std::size_t pos = 8;
    while (pos + 12 <= len) {
        const std::uint32_t clen = be32(data + pos);
        const char *ctype = reinterpret_cast<const char *>(data + pos + 4);
        const std::uint8_t *cdata = data + pos + 8;
        if (pos + 12 + clen > len) break;
        if (std::memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            w = be32(cdata);
            h = be32(cdata + 4);
            bit_depth = cdata[8];
            color_type = cdata[9];
            interlace = cdata[12];
        } else if (std::memcmp(ctype, "PLTE", 4) == 0) {
            plte.assign(cdata, cdata + clen);
        } else if (std::memcmp(ctype, "tRNS", 4) == 0) {
            trns.assign(cdata, cdata + clen);
        } else if (std::memcmp(ctype, "IDAT", 4) == 0) {
            idat.insert(idat.end(), cdata, cdata + clen);
        } else if (std::memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (!w || !h || w > 16384 || h > 16384 || bit_depth != 8 || interlace != 0 || idat.size() < 6) return false;
    const std::size_t bpp = (color_type == 6) ? 4 : (color_type == 2) ? 3 : (color_type == 4) ? 2 : (color_type == 0 || color_type == 3) ? 1 : 0;
    if (!bpp) return false;
    const std::size_t z_start = (idat[0] & 0x0Fu) == 8 && (idat[1] & 0x20u) ? 6 : 2;
    if (idat.size() <= z_start + 4) return false;

    std::vector<std::uint8_t> raw;
    const std::size_t stride = static_cast<std::size_t>(w) * bpp;
    raw.reserve(static_cast<std::size_t>(h) * (1 + stride));
    if (!inflate_rfc1951(idat.data() + z_start, idat.size() - z_start - 4, raw)) return false;
    if (raw.size() < static_cast<std::size_t>(h) * (1 + stride)) return false;

    std::vector<std::uint8_t> recon(static_cast<std::size_t>(h) * stride, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t ftype = raw[static_cast<std::size_t>(y) * (1 + stride)];
        const std::uint8_t *src_row = raw.data() + static_cast<std::size_t>(y) * (1 + stride) + 1;
        std::uint8_t *dst_row = recon.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t *up_row = y > 0 ? (dst_row - stride) : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? dst_row[i - bpp] : 0;
            const int b = up_row ? up_row[i] : 0;
            const int c = (up_row && i >= bpp) ? up_row[i - bpp] : 0;
            int pred = 0;
            if (ftype == 1) pred = a;
            else if (ftype == 2) pred = b;
            else if (ftype == 3) pred = (a + b) >> 1;
            else if (ftype == 4) {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
            }
            dst_row[i] = static_cast<std::uint8_t>((src_row[i] + pred) & 0xFF);
        }
    }
    out_w = w;
    out_h = h;
    out_words.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        std::uint8_t r = 0, g = 0, b = 0, a = 255;
        if (color_type == 6) {
            r = recon[i * 4]; g = recon[i * 4 + 1]; b = recon[i * 4 + 2]; a = recon[i * 4 + 3];
        } else if (color_type == 2) {
            r = recon[i * 3]; g = recon[i * 3 + 1]; b = recon[i * 3 + 2];
        } else if (color_type == 3) {
            const std::size_t idx = recon[i];
            if (idx * 3 + 2 < plte.size()) {
                r = plte[idx * 3]; g = plte[idx * 3 + 1]; b = plte[idx * 3 + 2];
            }
            if (idx < trns.size()) a = trns[idx];
        } else if (color_type == 4) {
            r = g = b = recon[i * 2]; a = recon[i * 2 + 1];
        } else {
            r = g = b = recon[i];
        }
        const std::uint32_t out_a = ps2_alpha ? ((static_cast<std::uint32_t>(a) * 128u + 127u) / 255u) : static_cast<std::uint32_t>(a);
        out_words[i] = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                       (static_cast<std::uint32_t>(b) << 16) | (out_a << 24);
    }
    return true;
}

bool decode_rgba(const std::filesystem::path &path, std::uint32_t &out_w, std::uint32_t &out_h,
                 std::vector<std::uint32_t> &out_words, bool ps2_alpha) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 33 || sz > 64 * 1024 * 1024) return false;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(data.data()), sz)) return false;
    return decode_rgba_bytes(data.data(), data.size(), out_w, out_h, out_words, ps2_alpha);
}

bool encode_rgba_bytes(std::uint32_t width, std::uint32_t height, const std::uint32_t *rgba,
                       std::vector<std::uint8_t> &out_png, bool from_ps2_alpha) {
    if (!rgba || width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(height) * (1 + stride));

    // Sub-filter (type 1) gives significantly better compression on sprites & textures while staying very fast.
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t *row = raw.data() + static_cast<std::size_t>(y) * (1 + stride);
        row[0] = 1;  // Sub filter
        std::uint8_t prev[4] = {0, 0, 0, 0};
        const std::uint32_t *src = rgba + static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t p = src[x];
            const std::uint8_t r = static_cast<std::uint8_t>(p & 0xFFu);
            const std::uint8_t g = static_cast<std::uint8_t>((p >> 8) & 0xFFu);
            const std::uint8_t b = static_cast<std::uint8_t>((p >> 16) & 0xFFu);
            std::uint8_t a = static_cast<std::uint8_t>((p >> 24) & 0xFFu);
            if (from_ps2_alpha) {
                const std::uint32_t scaled = (static_cast<std::uint32_t>(a) * 255u + 64u) / 128u;
                a = static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, scaled));
            }
            std::uint8_t *dst = row + 1 + static_cast<std::size_t>(x) * 4;
            dst[0] = static_cast<std::uint8_t>(r - prev[0]);
            dst[1] = static_cast<std::uint8_t>(g - prev[1]);
            dst[2] = static_cast<std::uint8_t>(b - prev[2]);
            dst[3] = static_cast<std::uint8_t>(a - prev[3]);
            prev[0] = r; prev[1] = g; prev[2] = b; prev[3] = a;
        }
    }

    const std::vector<std::uint8_t> z = build_zlib_stream(raw.data(), raw.size());
    out_png.clear();
    out_png.reserve(64 + z.size());
    static constexpr std::uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out_png.insert(out_png.end(), kSig, kSig + 8);

    std::uint8_t ihdr[13] = {};
    ihdr[0] = static_cast<std::uint8_t>((width >> 24) & 0xFFu);
    ihdr[1] = static_cast<std::uint8_t>((width >> 16) & 0xFFu);
    ihdr[2] = static_cast<std::uint8_t>((width >> 8) & 0xFFu);
    ihdr[3] = static_cast<std::uint8_t>(width & 0xFFu);
    ihdr[4] = static_cast<std::uint8_t>((height >> 24) & 0xFFu);
    ihdr[5] = static_cast<std::uint8_t>((height >> 16) & 0xFFu);
    ihdr[6] = static_cast<std::uint8_t>((height >> 8) & 0xFFu);
    ihdr[7] = static_cast<std::uint8_t>(height & 0xFFu);
    ihdr[8] = 8;  // 8-bit
    ihdr[9] = 6;  // RGBA
    push_chunk(out_png, "IHDR", ihdr, sizeof ihdr);
    push_chunk(out_png, "IDAT", z.data(), z.size());
    push_chunk(out_png, "IEND", nullptr, 0);
    return true;
}

bool encode_rgba(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                 const std::uint32_t *rgba, bool from_ps2_alpha) {
    std::vector<std::uint8_t> png;
    if (!encode_rgba_bytes(width, height, rgba, png, from_ps2_alpha)) return false;
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!f.good()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

}  // namespace ps2::png
