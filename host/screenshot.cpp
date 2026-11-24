#include "screenshot.hpp"

#include "ps2/os.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace screenshot {
namespace {

struct ComGuard {
#ifdef _WIN32
    bool init = false;
    ComGuard() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        init = (hr == S_OK || hr == S_FALSE);
    }
    ~ComGuard() {
        if (init) CoUninitialize();
    }
#endif
};

constexpr std::size_t kMaxQueuedScreenshots = 4;

struct ScreenshotWorker {
    std::mutex mtx;
    std::condition_variable cv;
    std::condition_variable done_cv;
    std::deque<std::function<void()>> queue;
    bool busy = false;
    bool stop = false;
    std::thread worker;

    void start_locked() {
        if (worker.joinable()) return;
        worker = std::thread([this] {
            [[maybe_unused]] ComGuard com;
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [this] { return stop || !queue.empty(); });
                    if (stop && queue.empty()) return;
                    job = std::move(queue.front());
                    queue.pop_front();
                    busy = true;
                }
                if (job) job();
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    busy = false;
                    if (queue.empty()) done_cv.notify_all();
                }
            }
        });
    }

    bool enqueue(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(mtx);
        if (queue.size() >= kMaxQueuedScreenshots) {
            std::fprintf(stderr, "[screenshot] write queue full (%zu pending); dropping capture\n", queue.size());
            return false;
        }
        start_locked();
        queue.push_back(std::move(job));
        cv.notify_one();
        return true;
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(mtx);
        done_cv.wait(lk, [this] { return !busy && queue.empty(); });
    }

    ~ScreenshotWorker() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
};

ScreenshotWorker &worker_pool() {
    static ScreenshotWorker w;
    return w;
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

// Fast zero-dependency RFC 1951 fixed-Huffman + hash-table LZ77 DEFLATE encoder.
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
    z.push_back(0x01);  // zlib header (deflate, 32K window, fastest level)

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
    bw.put(kFixed.lit_code[256], kFixed.lit_len[256]);  // end-of-block
    bw.flush();

    // If incompressible data grew beyond stored blocks, fall back to uncompressed stored blocks.
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
    out.insert(out.end(), type, type + 4);
    if (len > 0 && payload) out.insert(out.end(), payload, payload + len);
    const std::uint32_t crc = crc32_bytes(out.data() + start, 4 + len);
    push_be32(out, crc);
}

std::tm local_tm() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    return tm_buf;
}

std::filesystem::path unique_path(const std::filesystem::path &dir, const std::string &stem, const std::string &ext) {
    std::error_code ec;
    std::filesystem::path p = dir / (stem + ext);
    if (!std::filesystem::exists(p, ec)) return p;
    for (int i = 2; i < 1000; ++i) {
        p = dir / (stem + "_" + std::to_string(i) + ext);
        if (!std::filesystem::exists(p, ec)) return p;
    }
    return dir / (stem + ext);
}

}  // namespace

std::string sanitize_filename(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    bool prev_space = false;
    for (unsigned char c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || c < 32) continue;
        if (c == ' ') {
            if (!out.empty() && !prev_space) out.push_back('_');
            prev_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            prev_space = false;
        }
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    return out.empty() ? "Unlocked" : out;
}

std::filesystem::path find_achievement_screenshot(const std::string &achievement_name) {
    if (achievement_name.empty()) return {};
    const std::filesystem::path dir = default_folder();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    const std::string prefix = "Achievement_" + sanitize_filename(achievement_name) + "_";
    std::filesystem::path best;
    std::filesystem::file_time_type best_time{};
    for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        const std::filesystem::path &p = ent.path();
        if (p.extension() != ".png" && p.extension() != ".PNG") continue;
        const std::string fname = ps2::os::utf8(p.filename());
        if (fname.rfind(prefix, 0) != 0) continue;
        const auto mtime = ent.last_write_time(ec);
        if (best.empty() || mtime > best_time) {
            best = p;
            best_time = mtime;
        }
    }
    return best;
}

std::filesystem::path default_folder() {
    std::filesystem::path folder;
    if (const std::string env_dir = ps2::os::env("PS2_SCREENSHOTS_DIR"); !env_dir.empty()) {
        folder = ps2::os::path(env_dir);
    } else {
        std::filesystem::path base;
#ifdef _WIN32
        PWSTR wpath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &wpath)) && wpath) {
            base = std::filesystem::path(wpath);
            CoTaskMemFree(wpath);
        } else if (const std::string profile = ps2::os::env("USERPROFILE"); !profile.empty()) {
            base = ps2::os::path(profile) / "Pictures";
        }
#else
        if (const std::string home = ps2::os::env("HOME"); !home.empty()) base = ps2::os::path(home) / "Pictures";
#endif
        if (base.empty()) base = std::filesystem::current_path();
        folder = base / "Shadow Hearts Nocturne" / "Covenant";
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return folder;
}

bool write_png(const std::filesystem::path &path, const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
    if (!rgba || width == 0 || height == 0) return false;
    const std::size_t row_bytes = 1 + static_cast<std::size_t>(width) * 3;
    const std::size_t raw_size = row_bytes * height;
    std::vector<std::uint8_t> raw(raw_size);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t *dst = raw.data() + y * row_bytes;
        dst[0] = 1;  // PNG filter type 1 (Sub): predicts each RGB channel from left pixel
        const std::uint32_t *src = rgba + static_cast<std::size_t>(y) * width;
        std::uint8_t pr = 0, pg = 0, pb = 0;
        std::size_t k = 1;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t px = src[x];
            const std::uint8_t r = static_cast<std::uint8_t>(px & 0xFFu);
            const std::uint8_t g = static_cast<std::uint8_t>((px >> 8) & 0xFFu);
            const std::uint8_t b = static_cast<std::uint8_t>((px >> 16) & 0xFFu);
            dst[k++] = static_cast<std::uint8_t>(r - pr);
            dst[k++] = static_cast<std::uint8_t>(g - pg);
            dst[k++] = static_cast<std::uint8_t>(b - pb);
            pr = r;
            pg = g;
            pb = b;
        }
    }

    const std::vector<std::uint8_t> z = build_zlib_stream(raw.data(), raw_size);

    std::vector<std::uint8_t> png;
    png.reserve(z.size() + 128);
    static constexpr std::uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), kSig, kSig + 8);

    std::vector<std::uint8_t> ihdr;
    ihdr.reserve(13);
    push_be32(ihdr, width);
    push_be32(ihdr, height);
    ihdr.push_back(8);  // 8-bit
    ihdr.push_back(2);  // truecolor RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    push_chunk(png, "IHDR", ihdr.data(), ihdr.size());
    push_chunk(png, "IDAT", z.data(), z.size());
    push_chunk(png, "IEND", nullptr, 0);

    std::FILE *f = ps2::os::open(ps2::os::utf8(path), "wb");
    if (!f) return false;
    const std::size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return written == png.size();
}

std::filesystem::path save_game(const std::uint32_t *rgba, std::uint32_t width, std::uint32_t height) {
    const std::filesystem::path dir = default_folder();
    const std::tm tm_now = local_tm();
    std::string prefix = ps2::os::env("PS2_SCREENSHOT_PREFIX");
    if (prefix.empty()) prefix = "ShadowHeartsCovenant";
    char stamp[128];
    std::snprintf(stamp, sizeof stamp, "%s_%04d-%02d-%02d_%02d-%02d-%02d",
                  prefix.c_str(),
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    const std::filesystem::path path = unique_path(dir, stamp, ".png");
    if (!write_png(path, rgba, width, height)) return {};
    std::fprintf(stderr, "[screenshot] saved %s (%ux%u)\n", ps2::os::utf8(path).c_str(), width, height);
    return path;
}

void save_game_async(std::vector<std::uint32_t> rgba, std::uint32_t width, std::uint32_t height,
                     std::filesystem::path *out_path) {
    if (rgba.empty() || width == 0 || height == 0) return;
    const std::filesystem::path dir = default_folder();
    const std::tm tm_now = local_tm();
    std::string prefix = ps2::os::env("PS2_SCREENSHOT_PREFIX");
    if (prefix.empty()) prefix = "ShadowHeartsCovenant";
    char stamp[128];
    std::snprintf(stamp, sizeof stamp, "%s_%04d-%02d-%02d_%02d-%02d-%02d",
                  prefix.c_str(),
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    if (out_path) *out_path = unique_path(dir, stamp, ".png");
    const std::string stamp_str(stamp);
    worker_pool().enqueue([dir, stamp_str, rgba = std::move(rgba), width, height]() mutable {
        const std::filesystem::path path = unique_path(dir, stamp_str, ".png");
        if (write_png(path, rgba.data(), width, height))
            std::fprintf(stderr, "[screenshot] saved %s (%ux%u)\n", ps2::os::utf8(path).c_str(), width, height);
    });
}

void save_achievement_async(const std::string &achievement_name,
                            std::vector<std::uint32_t> game_rgba, std::uint32_t game_w, std::uint32_t game_h,
                            std::vector<std::uint32_t> toast_rgba, std::uint32_t toast_w, std::uint32_t toast_h,
                            std::uint32_t win_w, std::uint32_t win_h, std::uint32_t toast_x, std::uint32_t toast_y) {
    if (game_rgba.empty() || game_w == 0 || game_h == 0) return;
    worker_pool().enqueue([achievement_name, game_rgba = std::move(game_rgba), game_w, game_h,
                           toast_rgba = std::move(toast_rgba), toast_w, toast_h, win_w, win_h, toast_x, toast_y]() mutable {
        // Composite the top-right achievement toast proportionally onto the captured game frame.
        if (!toast_rgba.empty() && toast_w > 0 && toast_h > 0) {
            const std::uint32_t margin_x = std::max<std::uint32_t>(8u, game_w / 48u);
            const std::uint32_t margin_y = std::max<std::uint32_t>(8u, game_h / 36u);
            double scale = win_h > 0 ? static_cast<double>(game_h) / static_cast<double>(win_h) : 1.0;
            const double max_w = game_w > margin_x * 2u ? static_cast<double>(game_w - margin_x * 2u) : static_cast<double>(game_w);
            if (toast_w * scale > max_w) scale = max_w / static_cast<double>(toast_w);
            const std::uint32_t dst_w = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(toast_w * scale + 0.5));
            const std::uint32_t dst_h = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(toast_h * scale + 0.5));
            const std::uint32_t dst_x = game_w > dst_w + margin_x ? game_w - dst_w - margin_x : 0u;
            const std::uint32_t dst_y = std::min<std::uint32_t>(margin_y, game_h > dst_h ? game_h - dst_h : 0u);
            (void)win_w;
            (void)toast_x;
            (void)toast_y;
            for (std::uint32_t y = 0; y < dst_h && dst_y + y < game_h; ++y) {
                const std::uint32_t sy = std::min(toast_h - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * toast_h / dst_h));
                for (std::uint32_t x = 0; x < dst_w && dst_x + x < game_w; ++x) {
                    const std::uint32_t sx = std::min(toast_w - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * toast_w / dst_w));
                    const std::uint32_t fg = toast_rgba[static_cast<std::size_t>(sy) * toast_w + sx];
                    const std::uint32_t a = (fg >> 24) & 0xFFu;
                    if (a == 0) continue;
                    std::uint32_t &bg = game_rgba[static_cast<std::size_t>(dst_y + y) * game_w + (dst_x + x)];
                    if (a == 255) {
                        bg = fg;
                    } else {
                        const std::uint32_t inv = 255u - a;
                        const std::uint32_t r = ((fg & 0xFFu) * a + (bg & 0xFFu) * inv) / 255u;
                        const std::uint32_t g = (((fg >> 8) & 0xFFu) * a + ((bg >> 8) & 0xFFu) * inv) / 255u;
                        const std::uint32_t b = (((fg >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * inv) / 255u;
                        bg = r | (g << 8) | (b << 16) | 0xFF000000u;
                    }
                }
            }
        }
        const std::filesystem::path dir = default_folder();
        const std::tm tm_now = local_tm();
        char date_buf[32];
        std::snprintf(date_buf, sizeof date_buf, "%04d-%02d-%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
        const std::string stem = "Achievement_" + sanitize_filename(achievement_name) + "_" + date_buf;
        const std::filesystem::path path = unique_path(dir, stem, ".png");
        if (write_png(path, game_rgba.data(), game_w, game_h))
            std::fprintf(stderr, "[achievement] saved screenshot %s (%ux%u)\n", ps2::os::utf8(path).c_str(), game_w, game_h);
    });
}

void wait_pending() {
    worker_pool().wait_idle();
}

}  // namespace screenshot
