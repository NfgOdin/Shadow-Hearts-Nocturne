#pragma once

// Zero-dependency LZ4 block compressor & decompressor for compact PS2 save states.
// Format:
//   [0..7]   "PS2LZ4\0\0"  (8-byte magic)
//   [8..15]  uint64_t raw_size (little-endian uncompressed payload size)
//   [16..N]  standard LZ4 raw block stream

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ps2::state_lz4 {

inline constexpr char kMagic[8] = {'P', 'S', '2', 'L', 'Z', '4', 0, 0};
inline constexpr std::size_t kHeaderSize = 16;

inline bool is_compressed(const std::uint8_t *data, std::size_t size) {
    return data && size >= kHeaderSize && std::memcmp(data, kMagic, sizeof kMagic) == 0;
}

inline std::uint64_t uncompressed_size(const std::uint8_t *data, std::size_t size) {
    if (!is_compressed(data, size)) return 0;
    std::uint64_t raw = 0;
    std::memcpy(&raw, data + 8, sizeof raw);
    return raw;
}

inline std::uint32_t read_u32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline std::vector<std::uint8_t> compress(const std::uint8_t *src, std::size_t src_size) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + src_size / 2 + 1024);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t *>(kMagic), reinterpret_cast<const std::uint8_t *>(kMagic) + sizeof kMagic);
    const std::uint64_t raw64 = static_cast<std::uint64_t>(src_size);
    const auto *raw_bytes = reinterpret_cast<const std::uint8_t *>(&raw64);
    out.insert(out.end(), raw_bytes, raw_bytes + sizeof raw64);
    if (src_size == 0) return out;

    constexpr std::size_t kHashBits = 15;
    constexpr std::size_t kHashSize = 1u << kHashBits;
    constexpr std::size_t kMinMatch = 4;
    constexpr std::size_t kLastLiterals = 5;
    constexpr std::size_t kMfLimit = 12;

    std::vector<std::uint32_t> table(kHashSize, 0);
    const auto hash4 = [](std::uint32_t v) -> std::size_t {
        return static_cast<std::size_t>((v * 2654435761u) >> (32 - kHashBits));
    };

    const auto emit_len = [&](std::size_t extra) {
        while (extra >= 255) {
            out.push_back(255);
            extra -= 255;
        }
        out.push_back(static_cast<std::uint8_t>(extra));
    };

    std::size_t anchor = 0;
    std::size_t pos = 0;
    const std::size_t limit = src_size > kMfLimit ? src_size - kMfLimit : 0;
    const std::size_t match_limit = src_size > kLastLiterals ? src_size - kLastLiterals : 0;

    if (src_size >= kMfLimit) {
        table[hash4(read_u32(src))] = 0;
        pos = 1;
        while (pos < limit) {
            // Find a match (with step acceleration on incompressible runs like random pixels/audio)
            std::size_t ref = 0;
            std::size_t step = 1;
            std::size_t search_miss = 1u << 6;
            bool found = false;
            while (pos < limit) {
                const std::uint32_t seq = read_u32(src + pos);
                const std::size_t h = hash4(seq);
                ref = table[h];
                table[h] = static_cast<std::uint32_t>(pos);
                if (pos > ref && (pos - ref) <= 0xFFFFu && read_u32(src + ref) == seq) {
                    found = true;
                    break;
                }
                pos += step;
                step = (++search_miss) >> 6;
            }
            if (!found) break;

            // Emit literals + match loop
            for (;;) {
                const std::size_t lit_len = pos - anchor;
                const std::size_t token_idx = out.size();
                out.push_back(0);
                if (lit_len >= 15) {
                    out[token_idx] = 0xF0u;
                    emit_len(lit_len - 15);
                } else {
                    out[token_idx] = static_cast<std::uint8_t>(lit_len << 4);
                }
                if (lit_len > 0) out.insert(out.end(), src + anchor, src + pos);

                // Match offset (little-endian 16-bit)
                const std::uint16_t offset = static_cast<std::uint16_t>(pos - ref);
                out.push_back(static_cast<std::uint8_t>(offset & 0xFFu));
                out.push_back(static_cast<std::uint8_t>((offset >> 8) & 0xFFu));

                // Measure match length
                std::size_t m_pos = pos + kMinMatch;
                std::size_t m_ref = ref + kMinMatch;
                while (m_pos + 8 <= match_limit) {
                    std::uint64_t a, b;
                    std::memcpy(&a, src + m_pos, 8);
                    std::memcpy(&b, src + m_ref, 8);
                    if (a != b) break;
                    m_pos += 8;
                    m_ref += 8;
                }
                while (m_pos < match_limit && src[m_pos] == src[m_ref]) {
                    ++m_pos;
                    ++m_ref;
                }
                const std::size_t match_extra = (m_pos - pos) - kMinMatch;
                if (match_extra >= 15) {
                    out[token_idx] |= 0x0Fu;
                    emit_len(match_extra - 15);
                } else {
                    out[token_idx] |= static_cast<std::uint8_t>(match_extra);
                }

                pos = m_pos;
                anchor = pos;
                if (pos >= limit) break;

                // Update hash at pos - 2 and check immediate next match
                table[hash4(read_u32(src + pos - 2))] = static_cast<std::uint32_t>(pos - 2);
                const std::uint32_t next_seq = read_u32(src + pos);
                const std::size_t nh = hash4(next_seq);
                ref = table[nh];
                table[nh] = static_cast<std::uint32_t>(pos);
                if (pos > ref && (pos - ref) <= 0xFFFFu && read_u32(src + ref) == next_seq) continue;
                ++pos;
                break;
            }
        }
    }

    // Emit final trailing literals
    const std::size_t last_lit = src_size - anchor;
    if (last_lit >= 15) {
        out.push_back(0xF0u);
        emit_len(last_lit - 15);
    } else {
        out.push_back(static_cast<std::uint8_t>(last_lit << 4));
    }
    if (last_lit > 0) out.insert(out.end(), src + anchor, src + src_size);
    return out;
}

inline bool decompress(const std::uint8_t *src, std::size_t src_size, std::vector<std::uint8_t> &out) {
    if (!is_compressed(src, src_size)) return false;
    const std::uint64_t raw64 = uncompressed_size(src, src_size);
    // Sanity bound: max 1 GiB uncompressed save state
    if (raw64 > (1024ull << 20)) return false;
    const std::size_t raw_size = static_cast<std::size_t>(raw64);
    out.resize(raw_size);
    if (raw_size == 0) return true;

    const std::uint8_t *ip = src + kHeaderSize;
    const std::uint8_t *const iend = src + src_size;
    std::uint8_t *op = out.data();
    std::uint8_t *const oend = op + raw_size;

    while (ip < iend) {
        const std::uint8_t token = *ip++;
        std::size_t lit_len = token >> 4;
        if (lit_len == 15) {
            std::uint8_t b = 255;
            while (b == 255) {
                if (ip >= iend) return false;
                b = *ip++;
                lit_len += b;
            }
        }
        if (ip + lit_len > iend || op + lit_len > oend) return false;
        if (lit_len > 0) {
            std::memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }
        if (ip >= iend) break;

        if (ip + 2 > iend) return false;
        const std::uint16_t offset = static_cast<std::uint16_t>(ip[0]) | (static_cast<std::uint16_t>(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || static_cast<std::size_t>(op - out.data()) < offset) return false;

        std::size_t match_len = (token & 0x0Fu) + 4;
        if ((token & 0x0Fu) == 15) {
            std::uint8_t b = 255;
            while (b == 255) {
                if (ip >= iend) return false;
                b = *ip++;
                match_len += b;
            }
        }
        if (op + match_len > oend) return false;
        const std::uint8_t *ref = op - offset;
        if (offset >= match_len) {
            std::memcpy(op, ref, match_len);
            op += match_len;
        } else {
            // Overlapping RLE-style copy (standard LZ4 semantics)
            for (std::size_t k = 0; k < match_len; ++k) op[k] = ref[k];
            op += match_len;
        }
    }
    return op == oend;
}

}  // namespace ps2::state_lz4
