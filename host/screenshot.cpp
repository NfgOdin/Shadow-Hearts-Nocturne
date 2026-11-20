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
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

namespace screenshot {
namespace {

std::mutex g_pending_mutex;
std::condition_variable g_pending_cv;
int g_pending_writes = 0;

void push_be32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

std::uint32_t crc32_bytes(const std::uint8_t *data, std::size_t len) {
    static std::uint32_t table[256] = {};
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
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

std::filesystem::path default_folder() {
    std::filesystem::path base;
#ifdef _WIN32
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &wpath)) && wpath) {
        base = std::filesystem::path(wpath);
        CoTaskMemFree(wpath);
    } else if (const char *profile = std::getenv("USERPROFILE")) {
        base = std::filesystem::path(profile) / "Pictures";
    }
#else
    if (const char *home = std::getenv("HOME")) base = std::filesystem::path(home) / "Pictures";
#endif
    if (base.empty()) base = std::filesystem::current_path();
    const std::filesystem::path folder = base / "Game Screenshots";
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
        dst[0] = 0;  // filter type 0 (None)
        const std::uint32_t *src = rgba + static_cast<std::size_t>(y) * width;
        std::size_t k = 1;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t px = src[x];
            dst[k++] = static_cast<std::uint8_t>(px & 0xFFu);
            dst[k++] = static_cast<std::uint8_t>((px >> 8) & 0xFFu);
            dst[k++] = static_cast<std::uint8_t>((px >> 16) & 0xFFu);
        }
    }

    // Build zlib stream with uncompressed DEFLATE stored blocks (fast, exact, zero dependencies).
    std::uint32_t s1 = 1, s2 = 0;
    constexpr std::size_t kMaxBlock = 65535;
    const std::size_t num_blocks = (raw_size + kMaxBlock - 1) / kMaxBlock;
    std::vector<std::uint8_t> z;
    z.reserve(2 + raw_size + num_blocks * 5 + 4);
    z.push_back(0x78);
    z.push_back(0x01);

    std::size_t pos = 0;
    while (pos < raw_size) {
        const std::size_t chunk = std::min(kMaxBlock, raw_size - pos);
        const bool last = (pos + chunk == raw_size);
        z.push_back(last ? 0x01 : 0x00);
        const std::uint16_t len16 = static_cast<std::uint16_t>(chunk);
        const std::uint16_t nlen16 = static_cast<std::uint16_t>(~len16);
        z.push_back(static_cast<std::uint8_t>(len16 & 0xFFu));
        z.push_back(static_cast<std::uint8_t>((len16 >> 8) & 0xFFu));
        z.push_back(static_cast<std::uint8_t>(nlen16 & 0xFFu));
        z.push_back(static_cast<std::uint8_t>((nlen16 >> 8) & 0xFFu));
        const std::uint8_t *p = raw.data() + pos;
        z.insert(z.end(), p, p + chunk);
        for (std::size_t i = 0; i < chunk; ++i) {
            s1 += p[i];
            if (s1 >= 65521u) s1 -= 65521u;
            s2 += s1;
            if (s2 >= 65521u) s2 -= 65521u;
        }
        pos += chunk;
    }
    push_be32(z, (s2 << 16) | s1);

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
    char stamp[64];
    std::snprintf(stamp, sizeof stamp, "ShadowHeartsCovenant_%04d-%02d-%02d_%02d-%02d-%02d",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    const std::filesystem::path path = unique_path(dir, stamp, ".png");
    if (!write_png(path, rgba, width, height)) return {};
    std::fprintf(stderr, "[screenshot] saved %s (%ux%u)\n", ps2::os::utf8(path).c_str(), width, height);
    return path;
}

void save_achievement_async(const std::string &achievement_name,
                            std::vector<std::uint32_t> game_rgba, std::uint32_t game_w, std::uint32_t game_h,
                            std::vector<std::uint32_t> toast_rgba, std::uint32_t toast_w, std::uint32_t toast_h,
                            std::uint32_t win_w, std::uint32_t win_h, std::uint32_t toast_x, std::uint32_t toast_y) {
    if (game_rgba.empty() || game_w == 0 || game_h == 0) return;
    {
        std::lock_guard<std::mutex> lk(g_pending_mutex);
        ++g_pending_writes;
    }
    std::thread([achievement_name, game_rgba = std::move(game_rgba), game_w, game_h,
                 toast_rgba = std::move(toast_rgba), toast_w, toast_h, win_w, win_h, toast_x, toast_y]() mutable {
        struct PendingGuard {
            ~PendingGuard() {
                std::lock_guard<std::mutex> lk(g_pending_mutex);
                if (--g_pending_writes <= 0) g_pending_cv.notify_all();
            }
        } guard;
        // Composite the top-right achievement toast proportionally onto the captured game frame.
        if (!toast_rgba.empty() && toast_w > 0 && toast_h > 0) {
            const double scale = win_h > 0 ? static_cast<double>(game_h) / static_cast<double>(win_h) : 1.0;
            const std::uint32_t dst_w = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(toast_w * scale + 0.5));
            const std::uint32_t dst_h = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(toast_h * scale + 0.5));
            const std::uint32_t margin_x = std::max<std::uint32_t>(8u, game_w / 48u);
            const std::uint32_t margin_y = std::max<std::uint32_t>(8u, game_h / 36u);
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
    }).detach();
}

void wait_pending() {
    std::unique_lock<std::mutex> lk(g_pending_mutex);
    g_pending_cv.wait(lk, [] { return g_pending_writes <= 0; });
}

}  // namespace screenshot
