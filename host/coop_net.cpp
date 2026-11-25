#include "coop_net.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSock = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kInvalidSock = -1;
#endif

namespace coop {

namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr std::uint8_t kFlagRelayed = 0x01;

#ifdef _WIN32
void ensure_wsa() {
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)ok;
}
void close_native(NativeSocket s) {
    if (s != kInvalidSock) closesocket(s);
}
void set_nonblocking(NativeSocket s) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}
#else
void ensure_wsa() {}
void close_native(NativeSocket s) {
    if (s != kInvalidSock) ::close(s);
}
void set_nonblocking(NativeSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
#endif

inline void put_u16(std::vector<std::uint8_t> &b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
inline void put_u32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}
inline void put_u64(std::vector<std::uint8_t> &b, std::uint64_t v) {
    put_u32(b, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    put_u32(b, static_cast<std::uint32_t>(v >> 32));
}
inline void put_str(std::vector<std::uint8_t> &b, const std::string &s, std::size_t max_len = 64) {
    const std::size_t n = std::min<std::size_t>(s.size(), std::min<std::size_t>(max_len, 255));
    b.push_back(static_cast<std::uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

inline std::uint16_t get_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
inline std::uint32_t get_u32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t get_u64(const std::uint8_t *p) {
    return static_cast<std::uint64_t>(get_u32(p)) | (static_cast<std::uint64_t>(get_u32(p + 4)) << 32);
}
inline bool get_str(const std::uint8_t *p, std::size_t len, std::size_t &off, std::string &out) {
    if (off >= len) return false;
    const std::size_t n = p[off++];
    if (off + n > len) return false;
    out.assign(reinterpret_cast<const char *>(p + off), n);
    off += n;
    return true;
}

void serialize_lobby_settings(std::vector<std::uint8_t> &b, const LobbySettings &cfg) {
    b.push_back(static_cast<std::uint8_t>(cfg.game));
    b.push_back(cfg.max_players);
    for (int i = 0; i < 4; ++i) b.push_back(cfg.slot_owner[i]);
    std::uint8_t bits = 0;
    if (cfg.auto_battle_turn) bits |= 0x01;
    if (cfg.relay_combos)     bits |= 0x02;
    if (cfg.pass_the_stick)   bits |= 0x04;
    if (cfg.show_hud)         bits |= 0x08;
    if (cfg.allow_pings)      bits |= 0x10;
    if (cfg.share_trophies)   bits |= 0x20;
    if (cfg.mirror_memcard)   bits |= 0x40;
    b.push_back(bits);
    put_str(b, cfg.campaign_name, 48);
}

bool deserialize_lobby_settings(const std::uint8_t *p, std::size_t len, std::size_t &off, LobbySettings &cfg) {
    if (off + 7 > len) return false;
    cfg.game = static_cast<GameKind>(p[off++] & 3u);
    cfg.max_players = std::clamp<std::uint8_t>(p[off++], 2, 4);
    for (int i = 0; i < 4; ++i) cfg.slot_owner[i] = p[off++] & 3u;
    const std::uint8_t bits = p[off++];
    cfg.auto_battle_turn = (bits & 0x01) != 0;
    cfg.relay_combos     = (bits & 0x02) != 0;
    cfg.pass_the_stick   = (bits & 0x04) != 0;
    cfg.show_hud         = (bits & 0x08) != 0;
    cfg.allow_pings      = (bits & 0x10) != 0;
    cfg.share_trophies   = (bits & 0x20) != 0;
    cfg.mirror_memcard   = (bits & 0x40) != 0;
    return get_str(p, len, off, cfg.campaign_name);
}

sockaddr_in to_sockaddr(const Endpoint &ep) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ep.ipv4);
    sa.sin_port = htons(ep.port);
    return sa;
}

Endpoint from_sockaddr(const sockaddr_in &sa) {
    Endpoint ep;
    ep.ipv4 = ntohl(sa.sin_addr.s_addr);
    ep.port = ntohs(sa.sin_port);
    return ep;
}

} // namespace

// ---- Endpoint ----

std::string Endpoint::to_string() const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                  (ipv4 >> 24) & 0xFFu, (ipv4 >> 16) & 0xFFu,
                  (ipv4 >> 8) & 0xFFu, ipv4 & 0xFFu, port);
    return buf;
}

Endpoint Endpoint::parse(const std::string &host_port, std::uint16_t default_port) {
    Endpoint ep;
    if (host_port.empty()) return ep;
    ensure_wsa();

    std::string host = host_port;
    std::uint16_t port = default_port;
    const auto colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        const int p = std::atoi(host_port.substr(colon + 1).c_str());
        if (p > 0 && p <= 65535) port = static_cast<std::uint16_t>(p);
    }
    if (host.empty()) host = "127.0.0.1";

    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) == 1) {
        ep.ipv4 = ntohl(addr.s_addr);
        ep.port = port;
        return ep;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        auto *sin = reinterpret_cast<sockaddr_in *>(res->ai_addr);
        ep.ipv4 = ntohl(sin->sin_addr.s_addr);
        ep.port = port;
        freeaddrinfo(res);
    }
    return ep;
}

// ---- Utilities ----

std::uint32_t crc32(const std::uint8_t *data, std::size_t len) {
    static const std::array<std::uint32_t, 256> tbl = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = tbl[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t fnv1a_32(const std::uint8_t *data, std::size_t len) {
    std::uint32_t h = 0x811C9DC5u;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

std::uint64_t fnv1a_64(const std::uint8_t *data, std::size_t len) {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::uint64_t steady_now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string normalize_room_code(const std::string &input) {
    std::string out;
    out.reserve(6);
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            if (out.size() == 6) break;
        }
    }
    return out;
}

std::string format_room_code_pretty(const std::string &code) {
    const std::string n = normalize_room_code(code);
    if (n.size() == 6) return n.substr(0, 3) + "-" + n.substr(3, 3);
    return n;
}

// ---- Fast 64-Bit XOR + Delta-LZ0 Compression (Zero-Run RLE + LZ77 Back-References) ----
// Format:
//   [u32 orig_size][u32 orig_crc][u8 mode_flags]
//     mode_flags bit 0 (0x01): XOR delta against baseline
//     mode_flags bit 1 (0x02): Delta-LZ0 v2 stream (0 = legacy zero-run RLE)
//   Delta-LZ0 v2 tokens:
//     0x00..0x3F: literal run of (tok + 1) bytes (1..64)
//     0x40..0x7E: short zero run of ((tok - 0x40) + 1) bytes (1..63)
//     0x7F [u32 count]: long zero run of `count` bytes
//     0x80..0xFE [u16 dist_m1]: LZ match of ((tok - 0x80) + 4) bytes (4..130) at distance (dist_m1 + 1)
//     0xFF [u16 dist_m1][u16 len_m4]: long LZ match of (len_m4 + 4) bytes at distance (dist_m1 + 1)

std::vector<std::uint8_t> compress_delta(const std::vector<std::uint8_t> &current,
                                         const std::vector<std::uint8_t> &baseline) {
    const std::size_t n = current.size();
    const bool use_base = (!baseline.empty() && baseline.size() == n);

    std::vector<std::uint8_t> delta_buf;
    const std::uint8_t *src = current.data();
    if (use_base && n > 0) {
        delta_buf.resize(n);
        const std::uint8_t *cur_p = current.data();
        const std::uint8_t *base_p = baseline.data();
        std::uint8_t *dst_p = delta_buf.data();
        std::size_t k = 0;
        for (; k + 8 <= n; k += 8) {
            std::uint64_t a, b;
            std::memcpy(&a, cur_p + k, 8);
            std::memcpy(&b, base_p + k, 8);
            const std::uint64_t x = a ^ b;
            std::memcpy(dst_p + k, &x, 8);
        }
        for (; k < n; ++k) dst_p[k] = static_cast<std::uint8_t>(cur_p[k] ^ base_p[k]);
        src = delta_buf.data();
    }

    std::vector<std::uint8_t> out;
    out.reserve(std::min<std::size_t>(n / 4 + 64, n + 64));
    put_u32(out, static_cast<std::uint32_t>(n));
    put_u32(out, crc32(current.data(), n));
    out.push_back(static_cast<std::uint8_t>((use_base ? 0x01u : 0x00u) | 0x02u));

    if (n == 0) return out;

    constexpr std::size_t kHashBits = 13;
    constexpr std::size_t kHashSize = 1u << kHashBits;
    std::uint32_t htab[kHashSize];
    std::memset(htab, 0xFF, sizeof(htab));

    auto load_u32 = [](const std::uint8_t *p) -> std::uint32_t {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    };
    auto hash4 = [](std::uint32_t v) -> std::size_t {
        return static_cast<std::size_t>((v * 2654435761u) >> (32 - kHashBits));
    };
    auto flush_literals = [&](std::size_t start, std::size_t end) {
        while (start < end) {
            const std::size_t chunk = std::min<std::size_t>(64u, end - start);
            out.push_back(static_cast<std::uint8_t>(chunk - 1u));
            out.insert(out.end(), src + start, src + start + chunk);
            start += chunk;
        }
    };

    std::size_t i = 0;
    std::size_t lit_start = 0;
    while (i < n) {
        // 1. Fast 64-bit zero-run scan (optimal for unchanged RAM pages in deltas)
        if (src[i] == 0) {
            std::size_t run = 1;
            while (i + run + 8 <= n) {
                std::uint64_t w;
                std::memcpy(&w, src + i + run, 8);
                if (w != 0) break;
                run += 8;
            }
            while (i + run < n && src[i + run] == 0) ++run;
            if (run >= 3 || lit_start == i) {
                flush_literals(lit_start, i);
                if (run <= 63u) {
                    out.push_back(static_cast<std::uint8_t>(0x40u + (run - 1u)));
                } else {
                    out.push_back(0x7Fu);
                    put_u32(out, static_cast<std::uint32_t>(run));
                }
                i += run;
                lit_start = i;
                continue;
            }
        }

        // 2. Fast LZ77 4-byte hash match (compresses textures, VRAM, SPU RAM & structs)
        if (i + 4 <= n) {
            const std::uint32_t v4 = load_u32(src + i);
            const std::size_t h = hash4(v4);
            const std::uint32_t ref = htab[h];
            htab[h] = static_cast<std::uint32_t>(i);
            if (ref != 0xFFFFFFFFu && i > ref && (i - ref) <= 65536u && load_u32(src + ref) == v4) {
                const std::size_t max_len = std::min<std::size_t>(65539u, n - i);
                std::size_t mlen = 4;
                while (mlen + 8 <= max_len) {
                    std::uint64_t a, b;
                    std::memcpy(&a, src + ref + mlen, 8);
                    std::memcpy(&b, src + i + mlen, 8);
                    if (a != b) break;
                    mlen += 8;
                }
                while (mlen < max_len && src[ref + mlen] == src[i + mlen]) ++mlen;

                flush_literals(lit_start, i);
                const std::uint16_t dist_m1 = static_cast<std::uint16_t>((i - ref) - 1u);
                if (mlen <= 130u) {
                    out.push_back(static_cast<std::uint8_t>(0x80u + (mlen - 4u)));
                    out.push_back(static_cast<std::uint8_t>(dist_m1 & 0xFFu));
                    out.push_back(static_cast<std::uint8_t>(dist_m1 >> 8));
                } else {
                    const std::uint16_t len_m4 = static_cast<std::uint16_t>(mlen - 4u);
                    out.push_back(0xFFu);
                    out.push_back(static_cast<std::uint8_t>(dist_m1 & 0xFFu));
                    out.push_back(static_cast<std::uint8_t>(dist_m1 >> 8));
                    out.push_back(static_cast<std::uint8_t>(len_m4 & 0xFFu));
                    out.push_back(static_cast<std::uint8_t>(len_m4 >> 8));
                }
                i += mlen;
                lit_start = i;
                continue;
            }
        }

        ++i;
        if (i - lit_start == 64u) {
            flush_literals(lit_start, i);
            lit_start = i;
        }
    }
    flush_literals(lit_start, n);
    return out;
}

bool decompress_delta(const std::vector<std::uint8_t> &compressed,
                      const std::vector<std::uint8_t> &baseline,
                      std::vector<std::uint8_t> &out) {
    if (compressed.size() < 9) return false;
    const std::uint32_t orig_size = get_u32(compressed.data());
    const std::uint32_t orig_crc = get_u32(compressed.data() + 4);
    const std::uint8_t mode = compressed[8];
    const bool use_base = (mode & 0x01u) != 0;
    const bool use_lz0 = (mode & 0x02u) != 0;
    if (orig_size > 128u * 1024u * 1024u) return false;
    if (use_base && baseline.size() != orig_size) return false;

    out.assign(orig_size, 0);
    std::size_t pos = 9;
    std::size_t dst = 0;

    if (!use_lz0) {
        // Legacy zero-run RLE decoder (backward-compatible)
        while (pos < compressed.size() && dst < orig_size) {
            const std::uint8_t tok = compressed[pos++];
            if (tok < 0x80u) {
                const std::size_t lit = static_cast<std::size_t>(tok) + 1;
                if (pos + lit > compressed.size() || dst + lit > orig_size) return false;
                std::memcpy(out.data() + dst, compressed.data() + pos, lit);
                pos += lit;
                dst += lit;
            } else if (tok < 0xFFu) {
                const std::size_t zrun = static_cast<std::size_t>(tok - 0x80u) + 1;
                if (dst + zrun > orig_size) return false;
                dst += zrun; // already zero-initialized
            } else {
                if (pos + 4 > compressed.size()) return false;
                const std::size_t zrun = get_u32(compressed.data() + pos);
                pos += 4;
                if (dst + zrun > orig_size) return false;
                dst += zrun;
            }
        }
    } else {
        // Delta-LZ0 v2 decoder
        while (pos < compressed.size() && dst < orig_size) {
            const std::uint8_t tok = compressed[pos++];
            if (tok < 0x40u) {
                const std::size_t lit = static_cast<std::size_t>(tok) + 1u;
                if (pos + lit > compressed.size() || dst + lit > orig_size) return false;
                std::memcpy(out.data() + dst, compressed.data() + pos, lit);
                pos += lit;
                dst += lit;
            } else if (tok < 0x7Fu) {
                const std::size_t zrun = static_cast<std::size_t>(tok - 0x40u) + 1u;
                if (dst + zrun > orig_size) return false;
                dst += zrun; // already zero-initialized
            } else if (tok == 0x7Fu) {
                if (pos + 4 > compressed.size()) return false;
                const std::size_t zrun = get_u32(compressed.data() + pos);
                pos += 4;
                if (dst + zrun > orig_size) return false;
                dst += zrun;
            } else {
                std::size_t dist = 0;
                std::size_t mlen = 0;
                if (tok < 0xFFu) {
                    if (pos + 2 > compressed.size()) return false;
                    dist = static_cast<std::size_t>(compressed[pos] | (static_cast<std::uint16_t>(compressed[pos + 1]) << 8)) + 1u;
                    pos += 2;
                    mlen = static_cast<std::size_t>(tok - 0x80u) + 4u;
                } else {
                    if (pos + 4 > compressed.size()) return false;
                    dist = static_cast<std::size_t>(compressed[pos] | (static_cast<std::uint16_t>(compressed[pos + 1]) << 8)) + 1u;
                    mlen = static_cast<std::size_t>(compressed[pos + 2] | (static_cast<std::uint16_t>(compressed[pos + 3]) << 8)) + 4u;
                    pos += 4;
                }
                if (dist == 0 || dist > dst || dst + mlen > orig_size) return false;
                std::uint8_t *d = out.data() + dst;
                const std::uint8_t *s = d - dist;
                if (dist >= mlen) {
                    std::memcpy(d, s, mlen);
                } else {
                    for (std::size_t k = 0; k < mlen; ++k) d[k] = s[k];
                }
                dst += mlen;
            }
        }
    }
    if (dst != orig_size) return false;
    if (use_base) {
        std::uint8_t *dst_p = out.data();
        const std::uint8_t *base_p = baseline.data();
        std::size_t k = 0;
        for (; k + 8 <= orig_size; k += 8) {
            std::uint64_t a, b;
            std::memcpy(&a, dst_p + k, 8);
            std::memcpy(&b, base_p + k, 8);
            const std::uint64_t x = a ^ b;
            std::memcpy(dst_p + k, &x, 8);
        }
        for (; k < orig_size; ++k) dst_p[k] ^= base_p[k];
    }
    return crc32(out.data(), out.size()) == orig_crc;
}

// ---- InputJitterBuffer ----

void InputJitterBuffer::reset(std::uint32_t start_frame) {
    frames_.clear();
    last_returned_ = PadSample{};
    last_returned_.frame = start_frame;
    has_last_ = false;
    latest_frame_ = start_frame;
    last_played_frame_ = start_frame;
}

void InputJitterBuffer::push(const PadSample &sample) {
    frames_[sample.frame] = sample;
    if (sample.frame > latest_frame_) latest_frame_ = sample.frame;
    // Keep at most 180 frames (~3 seconds) buffered
    while (frames_.size() > 180) frames_.erase(frames_.begin());
}

bool InputJitterBuffer::pop_for_frame(std::uint32_t target_frame, PadSample &out, bool active_low_buttons) {
    auto it = frames_.find(target_frame);
    if (it == frames_.end() && !frames_.empty()) {
        // Fall back to the newest buffered sample <= target_frame (or earliest available)
        auto ub = frames_.upper_bound(target_frame);
        if (ub != frames_.begin()) {
            it = std::prev(ub);
        } else {
            it = frames_.begin();
        }
    }
    if (it != frames_.end()) {
        out = it->second;
        const std::uint32_t chosen_frame = it->first;
        // Coalesce button presses across any skipped frames between last_played_frame_ and chosen_frame
        // so quick taps (skipping movies, confirming battle commands) are NEVER dropped on slower clients.
        if (has_last_ && chosen_frame > last_played_frame_ + 1 && chosen_frame - last_played_frame_ <= 12) {
            std::uint32_t merged = out.buttons;
            for (auto sit = frames_.upper_bound(last_played_frame_); sit != frames_.end() && sit->first <= chosen_frame; ++sit) {
                if (active_low_buttons) {
                    merged &= sit->second.buttons;
                } else {
                    merged |= sit->second.buttons;
                }
            }
            out.buttons = merged;
        }
        last_played_frame_ = chosen_frame;
        last_returned_ = out;
        has_last_ = true;
        // Erase frames strictly older than chosen_frame - 8
        while (!frames_.empty() && frames_.begin()->first + 8 < chosen_frame) {
            frames_.erase(frames_.begin());
        }
        return true;
    }
    // Packet drop concealment: hold last known pad state if available
    if (has_last_) {
        out = last_returned_;
        out.frame = target_frame;
        return true;
    }
    return false;
}

bool InputJitterBuffer::pop_next(PadSample &out, bool active_low_buttons, bool hold_on_stall) {
    if (!has_last_) {
        if (frames_.empty()) return false;
        // Play from the oldest buffered frame unless a large backlog (>6 frames) accumulated before startup
        auto it = frames_.begin();
        if (frames_.size() > 6) {
            it = std::prev(frames_.end(), 3);
        }
        out = it->second;
        last_played_frame_ = it->first;
        last_returned_ = out;
        has_last_ = true;
        frames_.erase(frames_.begin(), std::next(it));
        return true;
    }

    // If we fell >8 frames behind the authority (e.g. momentary hitch), catch up smoothly to latest - 2
    // while coalescing any button presses across the skipped frames so taps are never dropped.
    if (latest_frame_ > last_played_frame_ + 8 && !frames_.empty()) {
        ++catchups_;
        const std::uint32_t catchup_target = latest_frame_ - 2;
        auto it = frames_.lower_bound(catchup_target);
        if (it == frames_.end()) it = std::prev(frames_.end());
        out = it->second;
        std::uint32_t merged = out.buttons;
        for (auto sit = frames_.upper_bound(last_played_frame_); sit != frames_.end() && sit->first <= it->first; ++sit) {
            if (active_low_buttons) merged &= sit->second.buttons;
            else merged |= sit->second.buttons;
        }
        out.buttons = merged;
        last_played_frame_ = it->first;
        last_returned_ = out;
        frames_.erase(frames_.begin(), std::next(it));
        return true;
    }

    // Normal 1-by-1 monotonic advance: consume exact next frame (or earliest newer frame if 1 packet dropped)
    auto it = frames_.upper_bound(last_played_frame_);
    if (it != frames_.end()) {
        out = it->second;
        last_played_frame_ = it->first;
        last_returned_ = out;
        frames_.erase(frames_.begin(), std::next(it));
        return true;
    }

    // Brief 1-frame UDP stall: hold digital state on PS2 (smooth movement/Judgment Ring),
    // or release digital buttons on Koudelka so a 1-frame stall never extends a 1-tile tap into 2 tiles.
    ++stalls_;
    out = last_returned_;
    if (!hold_on_stall) {
        out.buttons = active_low_buttons ? 0xFFFFu : 0x0000u;
    }
    return true;
}

// ---- BlobTransferIn / BlobTransferOut ----

bool BlobTransferIn::begin(std::uint32_t id, std::uint32_t total, std::uint32_t crc,
                           std::uint8_t fl, std::uint32_t max_bytes) {
    if (total == 0 || total > max_bytes) return false;
    blob_id = id;
    total_bytes = total;
    expected_crc = crc;
    flags = fl;
    total_chunks = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(total) + kChunkPayloadBytes - 1u) / kChunkPayloadBytes);
    data.assign(total, 0);
    received.assign(total_chunks, false);
    received_count = 0;
    last_rx_ms = steady_now_ms();
    return true;
}

bool BlobTransferIn::add_chunk(std::uint32_t chunk_idx, const std::uint8_t *bytes, std::size_t len) {
    if (chunk_idx >= total_chunks) return false;
    const std::size_t offset = static_cast<std::size_t>(chunk_idx) * kChunkPayloadBytes;
    if (offset + len > total_bytes) return false;
    last_rx_ms = steady_now_ms();
    if (!received[chunk_idx]) {
        std::memcpy(data.data() + offset, bytes, len);
        received[chunk_idx] = true;
        ++received_count;
    }
    return true;
}

void BlobTransferOut::begin(std::uint32_t id, std::vector<std::uint8_t> payload, std::uint8_t fl,
                            std::uint8_t target, std::uint8_t mask) {
    blob_id = id;
    flags = fl;
    target_slot = target;
    target_mask = mask;
    expected_crc = crc32(payload.data(), payload.size());
    data = std::move(payload);
    total_chunks = static_cast<std::uint32_t>((data.size() + kChunkPayloadBytes - 1u) / kChunkPayloadBytes);
    ack_mask.assign(total_chunks, 0u);
    last_sent_ms.assign(total_chunks, 0u);
    acked_count = 0;
    started_ms = steady_now_ms();
    last_send_ms = 0;
    next_cursor = 0;
}

void BlobTransferOut::recount_acked() {
    if (target_mask == 0 || total_chunks == 0) {
        acked_count = total_chunks;
        return;
    }
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < total_chunks; ++i) {
        if ((ack_mask[i] & target_mask) == target_mask) ++count;
    }
    acked_count = count;
}

// ---- CoopTransport ----

CoopTransport::CoopTransport() {
    for (int i = 0; i < kMaxPlayers; ++i) peers_[i].slot = static_cast<std::uint8_t>(i);
}

CoopTransport::~CoopTransport() {
    close();
}

bool CoopTransport::is_open() const {
    return static_cast<NativeSocket>(sock_) != kInvalidSock;
}

bool CoopTransport::open(std::uint16_t local_port, bool listen_lan_beacons) {
    close();
    ensure_wsa();

    NativeSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSock) return false;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    int bcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&bcast), sizeof(bcast));
    int rcvbuf = 2 * 1024 * 1024;
    int sndbuf = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&rcvbuf), sizeof(rcvbuf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&sndbuf), sizeof(sndbuf));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(local_port);
    if (bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
        close_native(s);
        return false;
    }

    socklen_t slen = sizeof(sa);
    if (getsockname(s, reinterpret_cast<sockaddr *>(&sa), &slen) == 0) {
        local_port_ = ntohs(sa.sin_port);
    } else {
        local_port_ = local_port;
    }

    set_nonblocking(s);
    sock_ = static_cast<std::uintptr_t>(s);

    if (listen_lan_beacons) {
        NativeSocket bs = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (bs != kInvalidSock) {
            setsockopt(bs, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
            sockaddr_in bsa{};
            bsa.sin_family = AF_INET;
            bsa.sin_addr.s_addr = htonl(INADDR_ANY);
            bsa.sin_port = htons(kLanBeaconPort);
            if (bind(bs, reinterpret_cast<sockaddr *>(&bsa), sizeof(bsa)) == 0) {
                set_nonblocking(bs);
                beacon_sock_ = static_cast<std::uintptr_t>(bs);
            } else {
                close_native(bs);
            }
        }
    }
    return true;
}

void CoopTransport::close() {
    if (is_open()) {
        // Send Leave twice BEFORE RelayLeave so peers receive it immediately over Direct UDP and Relay
        const auto leave_pkt = make_packet(PacketType::Leave, nullptr, 0);
        broadcast_to_peers(leave_pkt);
        broadcast_to_peers(leave_pkt);
        if (using_relay_ && relay_ep_.valid() && room_id_ != 0) {
            send_raw(make_packet(PacketType::RelayLeave, nullptr, 0), relay_ep_);
        }
        close_native(static_cast<NativeSocket>(sock_));
        sock_ = static_cast<std::uintptr_t>(kInvalidSock);
    }
    if (static_cast<NativeSocket>(beacon_sock_) != kInvalidSock) {
        close_native(static_cast<NativeSocket>(beacon_sock_));
        beacon_sock_ = static_cast<std::uintptr_t>(kInvalidSock);
    }
    local_port_ = 0;
    is_host_ = false;
    using_relay_ = false;
    pending_connect_ = PendingConnect::None;
    got_welcome_ = false;
    last_connect_retry_ms_ = 0;
    last_completed_state_blob_id_ = 0;
    last_completed_mc_blob_id_ = 0;
    state_out_ = BlobTransferOut{};
    state_in_ = BlobTransferIn{};
    mc_out_ = BlobTransferOut{};
    mc_in_ = BlobTransferIn{};
    room_id_ = 0;
    room_code_.clear();
    for (int i = 0; i < kMaxPlayers; ++i) {
        peers_[i] = PeerInfo{};
        peers_[i].slot = static_cast<std::uint8_t>(i);
    }
}

std::vector<std::uint8_t> CoopTransport::make_packet(PacketType type, const std::uint8_t *body,
                                                     std::size_t body_len, std::uint8_t flags) {
    std::vector<std::uint8_t> pkt;
    pkt.reserve(kHeaderSize + body_len);
    pkt.push_back('N');
    pkt.push_back('O');
    pkt.push_back('C');
    pkt.push_back('T');
    pkt.push_back(kProtoVersion);
    pkt.push_back(static_cast<std::uint8_t>(type));
    pkt.push_back(local_slot_);
    pkt.push_back(flags);
    put_u32(pkt, room_id_);
    put_u32(pkt, seq_++);
    if (body && body_len > 0) pkt.insert(pkt.end(), body, body + body_len);
    return pkt;
}

void CoopTransport::send_raw(const std::vector<std::uint8_t> &pkt, const Endpoint &ep) {
    if (!is_open() || !ep.valid()) return;
    sockaddr_in sa = to_sockaddr(ep);
    const int n = sendto(static_cast<NativeSocket>(sock_),
                         reinterpret_cast<const char *>(pkt.data()),
                         static_cast<int>(pkt.size()),
                         0,
                         reinterpret_cast<const sockaddr *>(&sa),
                         sizeof(sa));
    if (n > 0) {
        ++stats_.pkts_tx;
        stats_.bytes_tx += static_cast<std::uint64_t>(n);
    }
}

void CoopTransport::send_to_peer(std::uint8_t slot, const std::vector<std::uint8_t> &pkt) {
    if (slot >= kMaxPlayers || slot == local_slot_) return;
    const PeerInfo &p = peers_[slot];
    if (!p.connected && !p.direct_ep.valid()) return;

    if (p.direct_ep.valid() && (p.direct_ok || !using_relay_)) {
        send_raw(pkt, p.direct_ep);
        return;
    }
    // Only send direct punch for small control/heartbeat packets; avoid doubling large state chunks
    if (p.direct_ep.valid() && pkt.size() <= 128) send_raw(pkt, p.direct_ep);
    if (using_relay_ && relay_ep_.valid()) {
        std::vector<std::uint8_t> fwd_body;
        fwd_body.reserve(1 + pkt.size());
        fwd_body.push_back(slot);
        fwd_body.insert(fwd_body.end(), pkt.begin(), pkt.end());
        send_raw(make_packet(PacketType::RelayForward, fwd_body.data(), fwd_body.size()), relay_ep_);
    }
}

void CoopTransport::broadcast_to_peers(const std::vector<std::uint8_t> &pkt) {
    bool any_needs_relay = false;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i == local_slot_ || !peers_[i].connected) continue;
        if (peers_[i].direct_ep.valid()) send_raw(pkt, peers_[i].direct_ep);
        if (!peers_[i].direct_ok && using_relay_) any_needs_relay = true;
    }
    if (any_needs_relay && relay_ep_.valid()) {
        std::vector<std::uint8_t> fwd_body;
        fwd_body.reserve(1 + pkt.size());
        fwd_body.push_back(0xFFu); // relay broadcast to all other peers
        fwd_body.insert(fwd_body.end(), pkt.begin(), pkt.end());
        send_raw(make_packet(PacketType::RelayForward, fwd_body.data(), fwd_body.size()), relay_ep_);
    }
}

bool CoopTransport::host_room(const std::string &player_name, const LobbySettings &cfg,
                              const std::string &relay_host_port) {
    if (!is_open() && !open(kDefaultGamePort, true)) return false;
    is_host_ = true;
    local_slot_ = 0;
    player_name_ = player_name.empty() ? "Host" : player_name;
    lobby_cfg_ = cfg;
    for (int i = 0; i < kMaxPlayers; ++i) {
        peers_[i] = PeerInfo{};
        peers_[i].slot = static_cast<std::uint8_t>(i);
    }
    peers_[0].connected = true;
    peers_[0].ready = true;
    peers_[0].name = player_name_;
    peers_[0].last_seen_ms = steady_now_ms();

    if (!relay_host_port.empty()) {
        relay_ep_ = Endpoint::parse(relay_host_port, kDefaultRelayPort);
        if (relay_ep_.valid()) {
            using_relay_ = true;
            pending_connect_ = PendingConnect::RelayHost;
            last_connect_retry_ms_ = steady_now_ms();
            std::vector<std::uint8_t> body;
            body.push_back(static_cast<std::uint8_t>(cfg.game));
            body.push_back(cfg.max_players);
            put_str(body, player_name_, 24);
            send_raw(make_packet(PacketType::RelayCreateRoom, body.data(), body.size()), relay_ep_);
            return true;
        }
    }

    // LAN / Direct IP room (deterministic local code from local port + timestamp)
    using_relay_ = false;
    static constexpr char kAlpha[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    std::uint64_t seed = steady_now_ms() ^ (static_cast<std::uint64_t>(local_port_) << 16);
    room_code_.clear();
    for (int i = 0; i < 6; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        room_code_.push_back(kAlpha[(seed >> 33) % 31]);
    }
    room_id_ = fnv1a_32(reinterpret_cast<const std::uint8_t *>(room_code_.data()), room_code_.size());
    if (cb_.on_room_created) cb_.on_room_created(room_code_, room_id_);
    send_lan_beacon();
    return true;
}

bool CoopTransport::join_by_code(const std::string &player_name, const std::string &room_code,
                                 const std::string &relay_host_port) {
    if (!is_open() && !open(0, false)) return false;
    const std::string norm = normalize_room_code(room_code);
    if (norm.size() != 6) {
        if (cb_.on_error) cb_.on_error("Room code must be 6 characters.");
        return false;
    }
    relay_ep_ = Endpoint::parse(relay_host_port, kDefaultRelayPort);
    if (!relay_ep_.valid()) {
        if (cb_.on_error) cb_.on_error("Invalid Relay Server address.");
        return false;
    }
    is_host_ = false;
    using_relay_ = true;
    pending_connect_ = PendingConnect::RelayJoin;
    got_welcome_ = false;
    last_connect_retry_ms_ = steady_now_ms();
    local_slot_ = 0xFF;
    room_code_ = norm;
    player_name_ = player_name.empty() ? "Guest" : player_name;

    std::vector<std::uint8_t> body;
    body.insert(body.end(), norm.begin(), norm.end());
    put_str(body, player_name_, 24);
    send_raw(make_packet(PacketType::RelayJoinRoom, body.data(), body.size()), relay_ep_);
    return true;
}

bool CoopTransport::join_direct(const std::string &player_name, const std::string &host_ip_port) {
    if (!is_open() && !open(0, false)) return false;
    Endpoint host_ep = Endpoint::parse(host_ip_port, kDefaultGamePort);
    if (!host_ep.valid()) {
        if (cb_.on_error) cb_.on_error("Invalid Host IP:Port.");
        return false;
    }
    is_host_ = false;
    using_relay_ = false;
    pending_connect_ = PendingConnect::DirectJoin;
    got_welcome_ = false;
    last_connect_retry_ms_ = steady_now_ms();
    local_slot_ = 0xFF;
    player_name_ = player_name.empty() ? "Guest" : player_name;

    peers_[0] = PeerInfo{};
    peers_[0].slot = 0;
    peers_[0].direct_ep = host_ep;

    std::vector<std::uint8_t> body;
    put_str(body, player_name_, 24);
    send_raw(make_packet(PacketType::Hello, body.data(), body.size()), host_ep);
    return true;
}

int CoopTransport::connected_peer_count() const {
    int c = 0;
    for (const auto &p : peers_) {
        if (p.connected) ++c;
    }
    return c;
}

void CoopTransport::broadcast_lobby_config(const LobbySettings &cfg) {
    lobby_cfg_ = cfg;
    std::vector<std::uint8_t> body;
    serialize_lobby_settings(body, cfg);
    // Append roster summary so guests see all peer names/ready states
    for (int i = 0; i < kMaxPlayers; ++i) {
        body.push_back(peers_[i].connected ? 1u : 0u);
        body.push_back(peers_[i].ready ? 1u : 0u);
        put_u16(body, static_cast<std::uint16_t>(std::min<std::uint32_t>(peers_[i].rtt_ms, 9999u)));
        put_str(body, peers_[i].name, 24);
    }
    broadcast_to_peers(make_packet(PacketType::LobbyConfig, body.data(), body.size()));
}

void CoopTransport::set_ready(bool ready) {
    if (local_slot_ < kMaxPlayers) peers_[local_slot_].ready = ready;
    std::uint8_t r = ready ? 1u : 0u;
    broadcast_to_peers(make_packet(PacketType::ReadyToggle, &r, 1));
}

void CoopTransport::send_launch_together(const LobbySettings &cfg, std::uint16_t game_port,
                                         bool use_relay, const std::string &game_room_code) {
    lobby_cfg_ = cfg;
    std::vector<std::uint8_t> body;
    put_u16(body, game_port);
    serialize_lobby_settings(body, cfg);
    body.push_back(use_relay ? 1u : 0u);
    put_str(body, normalize_room_code(game_room_code), 8);
    broadcast_to_peers(make_packet(PacketType::LaunchTogether, body.data(), body.size()));
}

void CoopTransport::send_input_frame(const PadSample &sample, std::uint16_t epoch) {
    // Shift redundant history (last 3 frames) so single/double UDP drops heal immediately
    recent_inputs_[2] = recent_inputs_[1];
    recent_inputs_[1] = recent_inputs_[0];
    recent_inputs_[0] = sample;
    if (recent_input_count_ < 3) ++recent_input_count_;

    std::vector<std::uint8_t> body;
    put_u16(body, epoch);
    body.push_back(static_cast<std::uint8_t>(recent_input_count_));
    for (int i = 0; i < recent_input_count_; ++i) {
        const auto &s = recent_inputs_[i];
        put_u32(body, s.frame);
        put_u32(body, s.buttons);
        body.push_back(s.lx);
        body.push_back(s.ly);
        body.push_back(s.rx);
        body.push_back(s.ry);
    }
    broadcast_to_peers(make_packet(PacketType::InputFrame, body.data(), body.size()));
}

void CoopTransport::send_authority_grant(std::uint8_t new_owner, std::uint8_t party_slot,
                                         AuthorityReason reason, std::uint16_t epoch) {
    // Clear stale input history from the previous turn so old frames never leak into the new epoch
    recent_input_count_ = 0;
    std::vector<std::uint8_t> body;
    body.push_back(new_owner);
    body.push_back(party_slot);
    body.push_back(static_cast<std::uint8_t>(reason));
    put_u16(body, epoch);
    const auto pkt = make_packet(PacketType::AuthorityGrant, body.data(), body.size());
    broadcast_to_peers(pkt);

    // Track unacked peers for reliable AuthorityAck resend (every 100ms up to 5 retries)
    std::uint8_t mask = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i != local_slot_ && peers_[i].connected) mask |= static_cast<std::uint8_t>(1u << i);
    }
    pending_grant_pkt_ = pkt;
    pending_grant_epoch_ = epoch;
    pending_grant_mask_ = mask;
    pending_grant_last_ms_ = steady_now_ms();
    pending_grant_retries_ = 40;
}

void CoopTransport::send_authority_request(std::uint8_t desired_owner, AuthorityReason reason) {
    std::uint8_t body[2] = {desired_owner, static_cast<std::uint8_t>(reason)};
    broadcast_to_peers(make_packet(PacketType::AuthorityRequest, body, 2));
}

void CoopTransport::send_state_blob(const std::vector<std::uint8_t> &compressed_blob,
                                    std::uint8_t flags, std::uint8_t target_slot) {
    if (compressed_blob.empty()) return;
    std::uint8_t mask = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i == local_slot_ || !peers_[i].connected) continue;
        if (target_slot == 0xFF || target_slot == static_cast<std::uint8_t>(i)) mask |= static_cast<std::uint8_t>(1u << i);
    }
    if (mask == 0) return;
    state_out_.begin(next_blob_id_++, compressed_blob, flags, target_slot, mask);
    pump_blob_transfers(steady_now_ms());
}

void CoopTransport::send_sync_hash(std::uint32_t frame, std::uint64_t hash) {
    std::vector<std::uint8_t> body;
    put_u32(body, frame);
    put_u64(body, hash);
    broadcast_to_peers(make_packet(PacketType::SyncHash, body.data(), body.size()));
}

void CoopTransport::send_resync_request(bool force_full) {
    const std::uint8_t flag = force_full ? 1u : 0u;
    broadcast_to_peers(make_packet(PacketType::ResyncRequest, &flag, 1));
}

void CoopTransport::send_memcard_mirror(const std::vector<std::uint8_t> &compressed_mc) {
    if (compressed_mc.empty()) return;
    std::uint8_t mask = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i != local_slot_ && peers_[i].connected) mask |= static_cast<std::uint8_t>(1u << i);
    }
    if (mask == 0) return;
    mc_out_.begin(next_blob_id_++, compressed_mc, 0, 0xFF, mask);
    pump_blob_transfers(steady_now_ms());
}

void CoopTransport::send_ring_callout(RingOutcome outcome, std::uint8_t combo_count, const std::string &char_name) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(outcome));
    body.push_back(combo_count);
    put_str(body, char_name, 32);
    broadcast_to_peers(make_packet(PacketType::RingCallout, body.data(), body.size()));
}

void CoopTransport::send_trophy_share(const std::string &trophy_id, const std::string &trophy_name) {
    std::vector<std::uint8_t> body;
    put_str(body, trophy_id, 48);
    put_str(body, trophy_name, 64);
    broadcast_to_peers(make_packet(PacketType::TrophyShare, body.data(), body.size()));
}

void CoopTransport::send_emote_ping(EmoteKind emote) {
    std::uint8_t e = static_cast<std::uint8_t>(emote);
    broadcast_to_peers(make_packet(PacketType::EmotePing, &e, 1));
}

void CoopTransport::send_lan_beacon() {
    if (!is_host_ || !is_open()) return;
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(lobby_cfg_.game));
    body.push_back(static_cast<std::uint8_t>(connected_peer_count()));
    body.push_back(lobby_cfg_.max_players);
    put_u16(body, local_port_);
    put_str(body, room_code_, 8);
    put_str(body, player_name_, 24);
    put_str(body, lobby_cfg_.campaign_name, 48);

    Endpoint bcast{0xFFFFFFFFu, kLanBeaconPort};
    send_raw(make_packet(PacketType::LanBeacon, body.data(), body.size()), bcast);
}

void CoopTransport::pump_blob_transfers(std::uint64_t now_ms) {
    // Expire stalled incoming transfers (>15s with no chunk) so state_transfer_active() never wedges
    if (state_in_.total_chunks > 0 && !state_in_.complete() &&
        state_in_.last_rx_ms != 0 && now_ms - state_in_.last_rx_ms > 15000) {
        state_in_ = BlobTransferIn{};
    }
    if (mc_in_.total_chunks > 0 && !mc_in_.complete() &&
        mc_in_.last_rx_ms != 0 && now_ms - mc_in_.last_rx_ms > 15000) {
        mc_in_ = BlobTransferIn{};
    }

    // Reliable AuthorityGrant resend until every connected peer replies with AuthorityAck
    const std::uint64_t grant_interval = (pending_grant_retries_ > 30) ? 100u : 250u;
    if (pending_grant_mask_ != 0 && pending_grant_retries_ > 0 && !pending_grant_pkt_.empty() &&
        now_ms - pending_grant_last_ms_ >= grant_interval) {
        pending_grant_last_ms_ = now_ms;
        --pending_grant_retries_;
        for (int p = 0; p < kMaxPlayers; ++p) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << p);
            if (!(pending_grant_mask_ & bit)) continue;
            if (!peers_[p].connected) {
                pending_grant_mask_ &= static_cast<std::uint8_t>(~bit);
                continue;
            }
            send_to_peer(static_cast<std::uint8_t>(p), pending_grant_pkt_);
        }
    }

    auto pump_one = [&](BlobTransferOut &out, PacketType chunk_type) {
        if (out.done()) return;
        if (out.started_ms != 0 && now_ms - out.started_ms > 15000) {
            out = BlobTransferOut{};
            return;
        }
        // Check if any recipient still needs the Relay and compute RTT-aware retransmit interval
        bool any_relayed = false;
        std::uint64_t max_rtt = 40;
        for (int p = 0; p < kMaxPlayers; ++p) {
            if (out.target_mask & (1u << p)) {
                if (using_relay_ && !peers_[p].direct_ok) any_relayed = true;
                if (peers_[p].rtt_ms > max_rtt) max_rtt = peers_[p].rtt_ms;
            }
        }
        const std::uint64_t interval_ms = any_relayed ? 14 : 6;
        const int max_burst = any_relayed ? 12 : 28;
        const std::uint64_t retransmit_ms = std::clamp<std::uint64_t>((max_rtt * 3) / 2 + 20, 60, 260);
        if (out.last_send_ms != 0 && now_ms - out.last_send_ms < interval_ms) return;
        out.last_send_ms = now_ms;
        int sent = 0;
        for (std::uint32_t tried = 0; tried < out.total_chunks && sent < max_burst; ++tried) {
            const std::uint32_t idx = (out.next_cursor + tried) % out.total_chunks;
            const std::uint8_t missing = static_cast<std::uint8_t>(out.target_mask & ~out.ack_mask[idx]);
            if (missing == 0) continue;
            // Skip chunks still in-flight within 1.5x RTT so we never flood the Relay with duplicate retransmits
            if (idx < out.last_sent_ms.size() && out.last_sent_ms[idx] != 0 &&
                now_ms - out.last_sent_ms[idx] < retransmit_ms) {
                continue;
            }
            const std::size_t off = static_cast<std::size_t>(idx) * kChunkPayloadBytes;
            const std::size_t len = std::min<std::size_t>(kChunkPayloadBytes, out.data.size() - off);

            std::vector<std::uint8_t> body;
            body.reserve(17 + len);
            put_u32(body, out.blob_id);
            put_u32(body, static_cast<std::uint32_t>(out.data.size()));
            put_u32(body, out.expected_crc);
            put_u32(body, idx);
            body.push_back(out.flags);
            body.insert(body.end(), out.data.begin() + static_cast<std::ptrdiff_t>(off),
                        out.data.begin() + static_cast<std::ptrdiff_t>(off + len));

            const bool is_retransmit = (idx < out.last_sent_ms.size() && out.last_sent_ms[idx] != 0);
            auto pkt = make_packet(chunk_type, body.data(), body.size());
            for (int p = 0; p < kMaxPlayers; ++p) {
                if (missing & (1u << p)) send_to_peer(static_cast<std::uint8_t>(p), pkt);
            }
            ++stats_.chunks_sent;
            if (is_retransmit) ++stats_.chunks_retransmitted;
            if (idx < out.last_sent_ms.size()) out.last_sent_ms[idx] = now_ms;
            out.next_cursor = (idx + 1) % out.total_chunks;
            ++sent;
        }
    };
    pump_one(state_out_, PacketType::StateChunk);
    pump_one(mc_out_, PacketType::MemcardChunk);
}

void CoopTransport::poll() {
    if (!is_open()) return;
    std::uint8_t buf[65536];

    auto drain_socket = [&](NativeSocket s) {
        if (s == kInvalidSock) return;
        for (int iter = 0; iter < 128; ++iter) {
            sockaddr_in from_sa{};
            socklen_t from_len = sizeof(from_sa);
            const int n = recvfrom(s, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr *>(&from_sa), &from_len);
            if (n <= 0) break;
            handle_packet(buf, static_cast<std::size_t>(n), from_sockaddr(from_sa));
        }
    };

    drain_socket(static_cast<NativeSocket>(sock_));
    drain_socket(static_cast<NativeSocket>(beacon_sock_));

    const std::uint64_t now = steady_now_ms();
    pump_blob_transfers(now);

    // Automatic handshake retry every 1s until connected (eliminates launch-order & UDP drop races)
    if (now - last_connect_retry_ms_ >= 1000) {
        if (pending_connect_ == PendingConnect::RelayHost && room_id_ == 0 && relay_ep_.valid()) {
            last_connect_retry_ms_ = now;
            std::vector<std::uint8_t> body;
            body.push_back(static_cast<std::uint8_t>(lobby_cfg_.game));
            body.push_back(lobby_cfg_.max_players);
            put_str(body, player_name_, 24);
            send_raw(make_packet(PacketType::RelayCreateRoom, body.data(), body.size()), relay_ep_);
        } else if (pending_connect_ == PendingConnect::RelayJoin && local_slot_ == 0xFF &&
                   relay_ep_.valid() && room_code_.size() == 6) {
            last_connect_retry_ms_ = now;
            std::vector<std::uint8_t> body;
            body.insert(body.end(), room_code_.begin(), room_code_.end());
            put_str(body, player_name_, 24);
            send_raw(make_packet(PacketType::RelayJoinRoom, body.data(), body.size()), relay_ep_);
        } else if (pending_connect_ == PendingConnect::DirectJoin && local_slot_ == 0xFF &&
                   peers_[0].direct_ep.valid()) {
            last_connect_retry_ms_ = now;
            std::vector<std::uint8_t> body;
            put_str(body, player_name_, 24);
            send_raw(make_packet(PacketType::Hello, body.data(), body.size()), peers_[0].direct_ep);
        } else if (!is_host_ && using_relay_ && local_slot_ != 0xFF && !got_welcome_) {
            // Joined relay room, keep nudging Host with Hello until Welcome arrives
            last_connect_retry_ms_ = now;
            std::vector<std::uint8_t> hbody;
            put_str(hbody, player_name_, 24);
            send_to_peer(0, make_packet(PacketType::Hello, hbody.data(), hbody.size()));
        }
    }

    // Heartbeat every 1s for RTT measurement, NAT keepalive, direct-link health & peer disconnect timeout
    if (now - last_heartbeat_ms_ >= 1000) {
        last_heartbeat_ms_ = now;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (i == local_slot_ || !peers_[i].connected) continue;
            if (using_relay_ && peers_[i].direct_ok &&
                peers_[i].last_direct_rx_ms != 0 && now - peers_[i].last_direct_rx_ms > 3500) {
                // Direct UDP went silent (NAT timeout or asymmetric path) — fall back to Relay
                peers_[i].direct_ok = false;
                ++stats_.direct_fallbacks;
            }
            if (peers_[i].last_seen_ms != 0 && now - peers_[i].last_seen_ms > 12000) {
                // Peer timed out (crash or network drop without Leave packet)
                peers_[i].connected = false;
                peers_[i].ready = false;
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << i);
                state_out_.target_mask &= static_cast<std::uint8_t>(~bit);
                state_out_.recount_acked();
                mc_out_.target_mask &= static_cast<std::uint8_t>(~bit);
                mc_out_.recount_acked();
                if (is_host_) broadcast_lobby_config(lobby_cfg_);
                if (cb_.on_peer_left) cb_.on_peer_left(static_cast<std::uint8_t>(i));
            }
        }
        std::vector<std::uint8_t> hb;
        put_u64(hb, now);
        broadcast_to_peers(make_packet(PacketType::Heartbeat, hb.data(), hb.size()));
    }

    // Relay keepalive every 8s
    if (using_relay_ && relay_ep_.valid() && room_id_ != 0 && now - last_keepalive_ms_ >= 8000) {
        last_keepalive_ms_ = now;
        send_raw(make_packet(PacketType::RelayKeepalive, nullptr, 0), relay_ep_);
    }

    // LAN discovery beacon every 2s while hosting
    if (is_host_ && now - last_beacon_ms_ >= 2000) {
        last_beacon_ms_ = now;
        send_lan_beacon();
    }
}

void CoopTransport::handle_packet(const std::uint8_t *data, std::size_t len, const Endpoint &from) {
    if (len < kHeaderSize) return;
    if (data[0] != 'N' || data[1] != 'O' || data[2] != 'C' || data[3] != 'T') return;
    if (data[4] != kProtoVersion) return;
    ++stats_.pkts_rx;
    stats_.bytes_rx += static_cast<std::uint64_t>(len);

    const auto type = static_cast<PacketType>(data[5]);
    const std::uint8_t sender = data[6];
    const std::uint8_t flags = data[7];
    const std::uint32_t pkt_room_id = get_u32(data + 8);
    const std::uint8_t *body = data + kHeaderSize;
    const std::size_t body_len = len - kHeaderSize;
    const std::uint64_t now = steady_now_ms();

    // Authenticate room_id on all session packets once joined/hosted to block off-room UDP spoofing.
    const bool room_exempt =
        type == PacketType::LanBeacon ||
        type == PacketType::RelayRoomCreated ||
        type == PacketType::RelayJoinOk ||
        type == PacketType::RelayJoinErr ||
        (type == PacketType::Hello && is_host_ && pkt_room_id == 0) ||
        (type == PacketType::Welcome && !is_host_ && !got_welcome_);
    if (!room_exempt && room_id_ != 0 && pkt_room_id != room_id_) {
        return;
    }

    // Update peer direct endpoint / liveness if this is a peer packet.
    // When using a Relay, only promote direct_ok = true on a direct HeartbeatAck (proving
    // bidirectional direct UDP works) so asymmetric NATs never blackhole Host -> Guest packets!
    if (sender < kMaxPlayers && sender != local_slot_ && type != PacketType::LanBeacon) {
        if ((flags & kFlagRelayed) == 0 && from.valid()) {
            peers_[sender].direct_ep = from;
            peers_[sender].last_direct_rx_ms = now;
            if (!using_relay_ || type == PacketType::HeartbeatAck) {
                peers_[sender].direct_ok = true;
            }
        }
        peers_[sender].last_seen_ms = now;
    }

    switch (type) {
    case PacketType::RelayRoomCreated: {
        if (body_len < 16) return;
        room_code_.assign(reinterpret_cast<const char *>(body), 6);
        room_id_ = get_u32(body + 6);
        pending_connect_ = PendingConnect::None;
        if (cb_.on_room_created) cb_.on_room_created(room_code_, room_id_);
        break;
    }
    case PacketType::RelayJoinOk: {
        if (body_len < 12) return;
        room_id_ = get_u32(body);
        local_slot_ = body[4];
        pending_connect_ = PendingConnect::None;
        last_connect_retry_ms_ = now;
        const std::uint32_t host_ip = get_u32(body + 5);
        const std::uint16_t host_port = get_u16(body + 9);
        const std::uint8_t count = body[11];
        if (local_slot_ < kMaxPlayers) {
            peers_[local_slot_].connected = true;
            peers_[local_slot_].name = player_name_;
            peers_[local_slot_].last_seen_ms = now;
        }
        peers_[0].direct_ep = Endpoint{host_ip, host_port};
        peers_[0].connected = true;
        peers_[0].last_seen_ms = now;

        std::size_t off = 12;
        for (int i = 0; i < count && off + 7 <= body_len; ++i) {
            const std::uint8_t pslot = body[off++];
            const std::uint32_t pip = get_u32(body + off); off += 4;
            const std::uint16_t pport = get_u16(body + off); off += 2;
            if (pslot < kMaxPlayers && pslot != local_slot_) {
                peers_[pslot].direct_ep = Endpoint{pip, pport};
                peers_[pslot].connected = true;
            }
        }
        // Immediately send Hello to host (via direct punch + relay fallback)
        std::vector<std::uint8_t> hbody;
        put_str(hbody, player_name_, 24);
        send_to_peer(0, make_packet(PacketType::Hello, hbody.data(), hbody.size()));
        if (cb_.on_joined_room) cb_.on_joined_room(local_slot_, room_id_);
        break;
    }
    case PacketType::RelayPeerEndpoint: {
        if (body_len < 7) return;
        const std::uint8_t pslot = body[0];
        const std::uint32_t pip = get_u32(body + 1);
        const std::uint16_t pport = get_u16(body + 5);
        if (pslot < kMaxPlayers && pslot != local_slot_) {
            peers_[pslot].direct_ep = Endpoint{pip, pport};
            // Send punch packet immediately so NAT opens for this peer
            std::vector<std::uint8_t> hb;
            put_u64(hb, now);
            send_raw(make_packet(PacketType::Heartbeat, hb.data(), hb.size()), peers_[pslot].direct_ep);
        }
        break;
    }
    case PacketType::RelayJoinErr: {
        const std::uint8_t err = body_len > 0 ? body[0] : 0;
        if (cb_.on_error) {
            cb_.on_error(err == 2 ? "Co-op room is full." : "Co-op room code not found.");
        }
        break;
    }
    case PacketType::Hello: {
        if (!is_host_) return;
        std::size_t off = 0;
        std::string pname;
        if (!get_str(body, body_len, off, pname)) pname = "Guest";

        // Find existing or free slot
        std::uint8_t assigned = 0xFF;
        if (sender < kMaxPlayers && sender != 0) {
            assigned = sender;
        } else {
            for (int i = 1; i < lobby_cfg_.max_players; ++i) {
                if (!peers_[i].connected || peers_[i].direct_ep == from) {
                    assigned = static_cast<std::uint8_t>(i);
                    break;
                }
            }
        }
        if (assigned == 0xFF) return;

        peers_[assigned].connected = true;
        peers_[assigned].ready = false;
        peers_[assigned].name = pname;
        if ((flags & kFlagRelayed) == 0) {
            peers_[assigned].direct_ep = from;
            peers_[assigned].last_direct_rx_ms = now;
            if (!using_relay_) peers_[assigned].direct_ok = true;
        }
        peers_[assigned].last_seen_ms = now;

        // Reply Welcome with assigned slot + room_id + room_code
        std::vector<std::uint8_t> wbody;
        wbody.push_back(assigned);
        put_u32(wbody, room_id_);
        put_str(wbody, room_code_, 8);
        send_to_peer(assigned, make_packet(PacketType::Welcome, wbody.data(), wbody.size()));

        broadcast_lobby_config(lobby_cfg_);
        if (cb_.on_peer_joined) cb_.on_peer_joined(assigned, peers_[assigned]);
        break;
    }
    case PacketType::Welcome: {
        if (is_host_ || body_len < 6) return;
        const bool first_welcome = !got_welcome_;
        got_welcome_ = true;
        pending_connect_ = PendingConnect::None;
        local_slot_ = body[0];
        room_id_ = get_u32(body + 1);
        std::size_t off = 5;
        get_str(body, body_len, off, room_code_);
        if (local_slot_ < kMaxPlayers) {
            peers_[local_slot_].connected = true;
            peers_[local_slot_].name = player_name_;
            peers_[local_slot_].last_seen_ms = now;
        }
        peers_[0].connected = true;
        peers_[0].last_seen_ms = now;
        if ((flags & kFlagRelayed) == 0) {
            peers_[0].direct_ep = from;
            peers_[0].last_direct_rx_ms = now;
            if (!using_relay_) peers_[0].direct_ok = true;
        }
        if (first_welcome && cb_.on_joined_room) cb_.on_joined_room(local_slot_, room_id_);
        break;
    }
    case PacketType::Heartbeat: {
        if (body_len < 8 || sender >= kMaxPlayers) return;
        send_to_peer(sender, make_packet(PacketType::HeartbeatAck, body, 8));
        break;
    }
    case PacketType::HeartbeatAck: {
        if (body_len < 8 || sender >= kMaxPlayers) return;
        const std::uint64_t sent_ms = get_u64(body);
        if (now >= sent_ms) peers_[sender].rtt_ms = static_cast<std::uint32_t>(now - sent_ms);
        break;
    }
    case PacketType::LobbyConfig: {
        std::size_t off = 0;
        LobbySettings cfg;
        if (!deserialize_lobby_settings(body, body_len, off, cfg)) return;
        lobby_cfg_ = cfg;
        for (int i = 0; i < kMaxPlayers && off + 4 < body_len; ++i) {
            const bool conn = body[off++] != 0;
            const bool rdy = body[off++] != 0;
            const std::uint16_t rtt = get_u16(body + off); off += 2;
            std::string nm;
            if (!get_str(body, body_len, off, nm)) break;
            if (i != local_slot_) {
                peers_[i].connected = conn;
                peers_[i].ready = rdy;
                if (rtt > 0) peers_[i].rtt_ms = rtt;
                peers_[i].name = nm;
            }
        }
        if (cb_.on_lobby_config) cb_.on_lobby_config(lobby_cfg_);
        break;
    }
    case PacketType::ReadyToggle: {
        if (body_len < 1 || sender >= kMaxPlayers) return;
        peers_[sender].ready = body[0] != 0;
        if (is_host_) broadcast_lobby_config(lobby_cfg_);
        break;
    }
    case PacketType::LaunchTogether: {
        if (body_len < 9) return;
        const std::uint16_t game_port = get_u16(body);
        std::size_t off = 2;
        LobbySettings cfg;
        if (!deserialize_lobby_settings(body, body_len, off, cfg)) return;
        bool use_relay = using_relay_;
        std::string game_code;
        if (off < body_len) {
            use_relay = body[off++] != 0;
            get_str(body, body_len, off, game_code);
        }
        lobby_cfg_ = cfg;
        if (cb_.on_launch_together) cb_.on_launch_together(cfg, game_port, use_relay, game_code);
        break;
    }
    case PacketType::Leave: {
        if (sender < kMaxPlayers && sender != local_slot_) {
            peers_[sender].connected = false;
            peers_[sender].ready = false;
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << sender);
            state_out_.target_mask &= static_cast<std::uint8_t>(~bit);
            state_out_.recount_acked();
            mc_out_.target_mask &= static_cast<std::uint8_t>(~bit);
            mc_out_.recount_acked();
            if (is_host_) broadcast_lobby_config(lobby_cfg_);
            if (cb_.on_peer_left) cb_.on_peer_left(sender);
        }
        break;
    }
    case PacketType::InputFrame: {
        if (body_len < 3) return;
        const std::uint16_t epoch = get_u16(body);
        const std::uint8_t count = body[2];
        std::size_t off = 3;
        for (int i = 0; i < count && off + 12 <= body_len; ++i) {
            PadSample s;
            s.frame = get_u32(body + off); off += 4;
            s.buttons = get_u32(body + off); off += 4;
            s.lx = body[off++];
            s.ly = body[off++];
            s.rx = body[off++];
            s.ry = body[off++];
            if (cb_.on_input_frame) cb_.on_input_frame(sender, s, epoch);
        }
        break;
    }
    case PacketType::AuthorityGrant: {
        if (body_len < 5) return;
        const std::uint8_t new_owner = body[0];
        const std::uint8_t party_slot = body[1];
        const auto reason = static_cast<AuthorityReason>(body[2]);
        const std::uint16_t epoch = get_u16(body + 3);
        recent_input_count_ = 0;
        if (sender < kMaxPlayers && sender != local_slot_) {
            std::uint8_t abody[2] = {static_cast<std::uint8_t>(epoch & 0xFFu), static_cast<std::uint8_t>(epoch >> 8)};
            send_to_peer(sender, make_packet(PacketType::AuthorityAck, abody, 2));
        }
        if (cb_.on_authority_grant) cb_.on_authority_grant(new_owner, party_slot, reason, epoch);
        break;
    }
    case PacketType::AuthorityAck: {
        if (body_len < 2 || sender >= kMaxPlayers) return;
        const std::uint16_t ack_epoch = get_u16(body);
        if (ack_epoch == pending_grant_epoch_) {
            pending_grant_mask_ &= static_cast<std::uint8_t>(~(1u << sender));
            if (pending_grant_mask_ == 0) pending_grant_retries_ = 0;
        }
        break;
    }
    case PacketType::AuthorityRequest: {
        if (body_len < 2) return;
        if (cb_.on_authority_request) {
            cb_.on_authority_request(sender, body[0], static_cast<AuthorityReason>(body[1]));
        }
        break;
    }
    case PacketType::StateChunk:
    case PacketType::MemcardChunk: {
        if (body_len < 17) return;
        const std::uint32_t bid = get_u32(body);
        const std::uint32_t total = get_u32(body + 4);
        const std::uint32_t crc = get_u32(body + 8);
        const std::uint32_t idx = get_u32(body + 12);
        const std::uint8_t bfl = body[16];
        const std::uint8_t *chunk = body + 17;
        const std::size_t clen = body_len - 17;

        const bool is_state = (type == PacketType::StateChunk);
        const std::uint32_t max_bytes = is_state ? kMaxStateBlobBytes : kMaxMemcardBlobBytes;
        if (total == 0 || total > max_bytes) break;

        BlobTransferIn &in = is_state ? state_in_ : mc_in_;
        std::uint32_t &last_done = is_state ? last_completed_state_blob_id_ : last_completed_mc_blob_id_;

        // Always ack chunks so sender finishes cleanly, even for already-completed blobs
        std::vector<std::uint8_t> ack;
        put_u32(ack, bid);
        put_u32(ack, idx);
        send_to_peer(sender, make_packet(is_state ? PacketType::StateAck : PacketType::MemcardAck,
                                         ack.data(), ack.size()));

        if (bid <= last_done || (in.blob_id != 0 && bid < in.blob_id)) {
            break; // stale retransmit from an older blob
        }
        if (in.blob_id != bid || in.total_bytes != total) {
            if (!in.begin(bid, total, crc, bfl, max_bytes)) break;
        }
        in.add_chunk(idx, chunk, clen);

        if (in.complete() && in.verify_crc()) {
            const std::uint32_t done_id = in.blob_id;
            const std::uint8_t done_fl = in.flags;
            auto finished = std::move(in.data);
            last_done = done_id;
            in = BlobTransferIn{};
            if (is_state) {
                if (cb_.on_state_blob) cb_.on_state_blob(sender, done_id, finished, done_fl);
            } else {
                if (cb_.on_memcard_mirror) cb_.on_memcard_mirror(sender, finished);
            }
        }
        break;
    }
    case PacketType::StateAck:
    case PacketType::MemcardAck: {
        if (body_len < 8 || sender >= kMaxPlayers) return;
        const std::uint32_t bid = get_u32(body);
        const std::uint32_t idx = get_u32(body + 4);
        BlobTransferOut &out = (type == PacketType::StateAck) ? state_out_ : mc_out_;
        if (out.blob_id == bid && idx < out.total_chunks) {
            const bool was_done = ((out.ack_mask[idx] & out.target_mask) == out.target_mask);
            out.ack_mask[idx] |= static_cast<std::uint8_t>(1u << sender);
            const bool now_done = ((out.ack_mask[idx] & out.target_mask) == out.target_mask);
            if (!was_done && now_done) {
                ++out.acked_count;
                ++stats_.chunks_acked;
            }
        }
        break;
    }
    case PacketType::SyncHash: {
        if (body_len < 12) return;
        const std::uint32_t frame = get_u32(body);
        const std::uint64_t hash = get_u64(body + 4);
        if (cb_.on_sync_hash) cb_.on_sync_hash(sender, frame, hash);
        break;
    }
    case PacketType::ResyncRequest: {
        const bool force_full = (body_len >= 1) ? (body[0] != 0) : false;
        if (cb_.on_resync_request) cb_.on_resync_request(sender, force_full);
        break;
    }
    case PacketType::RingCallout: {
        if (body_len < 3) return;
        const auto outcome = static_cast<RingOutcome>(body[0]);
        const std::uint8_t combo = body[1];
        std::size_t off = 2;
        std::string cname;
        get_str(body, body_len, off, cname);
        if (cb_.on_ring_callout) cb_.on_ring_callout(sender, outcome, combo, cname);
        break;
    }
    case PacketType::TrophyShare: {
        std::size_t off = 0;
        std::string tid, tname;
        if (!get_str(body, body_len, off, tid)) return;
        if (!get_str(body, body_len, off, tname)) return;
        if (cb_.on_trophy_share) cb_.on_trophy_share(sender, tid, tname);
        break;
    }
    case PacketType::EmotePing: {
        if (body_len < 1) return;
        if (cb_.on_emote_ping) cb_.on_emote_ping(sender, static_cast<EmoteKind>(body[0]));
        break;
    }
    case PacketType::LanBeacon: {
        if (body_len < 6) return;
        if (pkt_room_id != 0 && pkt_room_id == room_id_) return; // ignore own beacon
        LanRoomInfo info;
        info.game = static_cast<GameKind>(body[0] & 3u);
        info.player_count = body[1];
        info.max_players = body[2];
        const std::uint16_t game_port = get_u16(body + 3);
        std::size_t off = 5;
        if (!get_str(body, body_len, off, info.room_code)) return;
        if (!get_str(body, body_len, off, info.host_name)) return;
        if (!get_str(body, body_len, off, info.campaign_name)) return;
        info.endpoint = Endpoint{from.ipv4, game_port};
        info.last_seen_ms = now;
        if (cb_.on_lan_room_found) cb_.on_lan_room_found(info);
        break;
    }
    default:
        break;
    }
}

} // namespace coop
