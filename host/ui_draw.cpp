#include "ui_draw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#endif

namespace ui_draw {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float sd_rounded_box(float px, float py, float bx, float by, float r) {
    const float qx = std::fabs(px) - bx + r;
    const float qy = std::fabs(py) - by + r;
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay;
    const float bax = bx - ax, bay = by - ay;
    const float denom = bax * bax + bay * bay;
    const float h = denom > 1e-6f ? std::clamp((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
    const float dx = pax - bax * h;
    const float dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

#ifndef _WIN32
// Compact portable 5x8 fallback font for non-Windows builds
constexpr const char *kFallbackFont[95] = {
    "..... ..... ..... ..... ..... ..... ..... .....",
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.. .....",
    ".#.#. .#.#. ..... ..... ..... ..... ..... .....",
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#. .....",
    "..#.. .#### #.#.. .###. ..#.# ####. ..#.. .....",
    "##... ##..# ...#. ..#.. .#... #..## ...## .....",
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.# .....",
    "..#.. ..#.. ..... ..... ..... ..... ..... .....",
    "...#. ..#.. .#... .#... .#... ..#.. ...#. .....",
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#... .....",
    "..... ..#.. #.#.# .###. #.#.# ..#.. ..... .....",
    "..... ..#.. ..#.. ##### ..#.. ..#.. ..... .....",
    "..... ..... ..... ..... ..... .##.. ..#.. .#...",
    "..... ..... ..... ##### ..... ..... ..... .....",
    "..... ..... ..... ..... ..... .##.. .##.. .....",
    "..... ....# ...#. ..#.. .#... #.... ..... .....",
    ".###. #...# #..## #.#.# ##..# #...# .###. .....",
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###. .....",
    ".###. #...# ....# ...#. ..#.. .#... ##### .....",
    "##### ...#. ..#.. ...#. ....# #...# .###. .....",
    "...#. ..##. .#.#. #..#. ##### ...#. ...#. .....",
    "##### #.... ####. ....# ....# #...# .###. .....",
    "..##. .#... #.... ####. #...# #...# .###. .....",
    "##### ....# ...#. ..#.. .#... .#... .#... .....",
    ".###. #...# #...# .###. #...# #...# .###. .....",
    ".###. #...# #...# .#### ....# ...#. .##.. .....",
    "..... .##.. .##.. ..... .##.. .##.. ..... .....",
    "..... .##.. .##.. ..... .##.. ..#.. .#...",
    "...#. ..#.. .#... #.... .#... ..#.. ...#. .....",
    "..... ..... ##### ..... ##### ..... ..... .....",
    ".#... ..#.. ...#. ....# ...#. ..#.. .#... .....",
    ".###. #...# ....# ...#. ..#.. ..... ..#.. .....",
    ".###. #...# ....# .##.# #.#.# #.#.# .###. .....",
    ".###. #...# #...# ##### #...# #...# #...# .....",
    "####. #...# #...# ####. #...# #...# ####. .....",
    ".###. #...# #.... #.... #.... #...# .###. .....",
    "####. #...# #...# #...# #...# #...# ####. .....",
    "##### #.... #.... ####. #.... #.... ##### .....",
    "##### #.... #.... ####. #.... #.... #.... .....",
    ".###. #...# #.... #.### #...# #...# .#### .....",
    "#...# #...# #...# ##### #...# #...# #...# .....",
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",
    "..### ...#. ...#. ...#. ...#. #..#. .##.. .....",
    "#...# #..#. #.#.. ##... #.#.. #..#. #...# .....",
    "#.... #.... #.... #.... #.... #.... ##### .....",
    "#...# ##.## #.#.# #.#.# #...# #...# #...# .....",
    "#...# #...# ##..# #.#.# #..## #...# #...# .....",
    ".###. #...# #...# #...# #...# #...# .###. .....",
    "####. #...# #...# ####. #.... #.... #.... .....",
    ".###. #...# #...# #...# #.#.# #..#. .##.# .....",
    "####. #...# #...# ####. #.#.. #..#. #...# .....",
    ".#### #.... #.... .###. ....# ....# ####. .....",
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",
    "#...# #...# #...# #...# #...# #...# .###. .....",
    "#...# #...# #...# #...# #...# .#.#. ..#.. .....",
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#. .....",
    "#...# #...# .#.#. ..#.. .#.#. #...# #...# .....",
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.. .....",
    "##### ....# ...#. ..#.. .#... #.... ##### .....",
    ".###. .#... .#... .#... .#... .#... .###. .....",
    "..... #.... .#... ..#.. ...#. ....# ..... .....",
    ".###. ...#. ...#. ...#. ...#. ...#. .###. .....",
    "..#.. .#.#. #...# ..... ..... ..... ..... .....",
    "..... ..... ..... ..... ..... ..... ##### .....",
    ".#... ..#.. ..... ..... ..... ..... ..... .....",
    "..... ..... .###. ....# .#### #...# .#### .....",
    "#.... #.... #.##. ##..# #...# #...# ####. .....",
    "..... ..... .###. #.... #.... #...# .###. .....",
    "....# ....# .##.# #..## #...# #...# .#### .....",
    "..... ..... .###. #...# ##### #.... .###. .....",
    "..##. .#..# .#... ###.. .#... .#... .#... .....",
    "..... ..... .#### #...# #...# .#### ....# .###.",
    "#.... #.... #.##. ##..# #...# #...# #...# .....",
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###. .....",
    "...#. ..... ..##. ...#. ...#. ...#. #..#. .##..",
    "#.... #.... #..#. #.#.. ##... #.#.. #..#. .....",
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.# .....",
    "..... ..... #.##. ##..# #...# #...# #...# .....",
    "..... ..... .###. #...# #...# #...# .###. .....",
    "..... ..... ####. #...# #...# ####. #.... #....",
    "..... ..... .#### #...# #...# .#### ....# ....#",
    "..... ..... #.##. ##..# #.... #.... #.... .....",
    "..... ..... .###. #.... .###. ....# ####. .....",
    ".#... .#... ###.. .#... .#... .#..# ..##. .....",
    "..... ..... #...# #...# #...# #..## .##.# .....",
    "..... ..... #...# #...# #...# .#.#. ..#.. .....",
    "..... ..... #...# #...# #.#.# #.#.# .#.#. .....",
    "..... ..... #...# .#.#. ..#.. .#.#. #...# .....",
    "..... ..... #...# #...# #...# .#### ....# .###.",
    "..... ..... ##### ...#. ..#.. .#... ##### .....",
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#. .....",
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#... .....",
    "..... ..... .#... #.#.# ...#. ..... ..... .....",
};
#endif

#ifdef _WIN32
struct TextMask {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
};

struct TextKey {
    int style = 0;
    int px_size = 0;
    bool bold = false;
    std::string text;
    bool operator==(const TextKey &o) const {
        return style == o.style && px_size == o.px_size && bold == o.bold && text == o.text;
    }
};

struct TextKeyHash {
    std::size_t operator()(const TextKey &k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.text);
        h ^= static_cast<std::size_t>(k.style * 131 + k.px_size * 17 + (k.bold ? 1 : 0)) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct FontKey {
    int style = 0;
    int px_size = 0;
    bool bold = false;
    bool operator==(const FontKey &o) const {
        return style == o.style && px_size == o.px_size && bold == o.bold;
    }
};

struct FontKeyHash {
    std::size_t operator()(const FontKey &k) const noexcept {
        return static_cast<std::size_t>(k.style * 1009 + k.px_size * 31 + (k.bold ? 1 : 0));
    }
};

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class GdiTextEngine {
public:
    GdiTextEngine() = default;
    ~GdiTextEngine() {
        for (auto &kv : fonts_)
            if (kv.second) DeleteObject(kv.second);
        if (dc_ && old_dib_) SelectObject(dc_, old_dib_);
        if (dib_) DeleteObject(dib_);
        if (dc_) DeleteDC(dc_);
    }

    const TextMask &rasterize(FontStyle style, int px_size, bool bold, const std::string &text) {
        static const TextMask kEmpty{};
        if (text.empty() || px_size <= 0) return kEmpty;
        px_size = std::clamp(px_size, 8, 96);

        TextKey key{static_cast<int>(style), px_size, bold, text};
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }

        ensure_dc();
        HFONT font = get_font(style, px_size, bold);
        HGDIOBJ old_font = SelectObject(dc_, font);

        const std::wstring wtext = utf8_to_wide(text);
        SIZE sz{};
        GetTextExtentPoint32W(dc_, wtext.c_str(), static_cast<int>(wtext.size()), &sz);
        const int tw = std::clamp(static_cast<int>(sz.cx) + 4, 1, 2048);
        const int th = std::clamp(static_cast<int>(sz.cy) + 2, 1, 256);

        ensure_dib(tw, th);
        if (!dib_bits_) {
            SelectObject(dc_, old_font);
            return kEmpty;
        }
        // Clear only the sub-rectangle we are about to render into
        for (int y = 0; y < th; ++y)
            std::memset(dib_bits_ + static_cast<std::size_t>(y) * dib_w_, 0, static_cast<std::size_t>(tw) * 4);

        SetBkMode(dc_, TRANSPARENT);
        SetTextColor(dc_, RGB(255, 255, 255));
        TextOutW(dc_, 1, 0, wtext.c_str(), static_cast<int>(wtext.size()));
        GdiFlush();
        SelectObject(dc_, old_font);

        TextMask mask;
        mask.width = tw;
        mask.height = th;
        mask.alpha.resize(static_cast<std::size_t>(tw) * th);
        for (int y = 0; y < th; ++y) {
            const std::uint32_t *src = dib_bits_ + static_cast<std::size_t>(y) * dib_w_;
            std::uint8_t *dst = mask.alpha.data() + static_cast<std::size_t>(y) * tw;
            for (int x = 0; x < tw; ++x) {
                const std::uint32_t px = src[x];
                const int r = static_cast<int>((px >> 16) & 255u);
                const int g = static_cast<int>((px >> 8) & 255u);
                const int b = static_cast<int>(px & 255u);
                // Perceived luminance of white-on-black antialiased glyph -> crisp alpha coverage
                dst[x] = static_cast<std::uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
            }
        }

        if (lru_.size() >= 1024) {
            cache_map_.erase(lru_.back().first);
            lru_.pop_back();
        }
        lru_.emplace_front(std::move(key), std::move(mask));
        cache_map_[lru_.front().first] = lru_.begin();
        return lru_.front().second;
    }

    TextMetrics measure(FontStyle style, int px_size, bool bold, const std::string &text) {
        if (text.empty() || px_size <= 0) return {0, std::max(8, px_size)};
        px_size = std::clamp(px_size, 8, 96);
        ensure_dc();
        HFONT font = get_font(style, px_size, bold);
        HGDIOBJ old_font = SelectObject(dc_, font);
        const std::wstring wtext = utf8_to_wide(text);
        SIZE sz{};
        GetTextExtentPoint32W(dc_, wtext.c_str(), static_cast<int>(wtext.size()), &sz);
        SelectObject(dc_, old_font);
        return {static_cast<int>(sz.cx) + 2, static_cast<int>(sz.cy)};
    }

private:
    void ensure_dc() {
        if (!dc_) dc_ = CreateCompatibleDC(nullptr);
    }

    void ensure_dib(int need_w, int need_h) {
        if (dib_ && need_w <= dib_w_ && need_h <= dib_h_) return;
        const int nw = std::max(need_w, std::max(dib_w_, 512));
        const int nh = std::max(need_h, std::max(dib_h_, 96));
        if (dib_) {
            if (dc_ && old_dib_) {
                SelectObject(dc_, old_dib_);
                old_dib_ = nullptr;
            }
            DeleteObject(dib_);
            dib_ = nullptr;
            dib_bits_ = nullptr;
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = nw;
        bmi.bmiHeader.biHeight = -nh;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        dib_ = CreateDIBSection(dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib_ && bits) {
            old_dib_ = SelectObject(dc_, dib_);
            dib_bits_ = static_cast<std::uint32_t *>(bits);
            dib_w_ = nw;
            dib_h_ = nh;
        } else if (dib_) {
            DeleteObject(dib_);
            dib_ = nullptr;
        }
    }

    HFONT get_font(FontStyle style, int px_size, bool bold) {
        FontKey fk{static_cast<int>(style), px_size, bold};
        auto it = fonts_.find(fk);
        if (it != fonts_.end()) return it->second;
        if (fonts_.size() >= 64) {
            for (auto &kv : fonts_)
                if (kv.second) DeleteObject(kv.second);
            fonts_.clear();
        }
        const wchar_t *face = (style == FontStyle::Serif) ? L"Georgia" : L"Segoe UI";
        const int weight = bold ? FW_BOLD : (style == FontStyle::Sans ? FW_SEMIBOLD : FW_NORMAL);
        HFONT f = CreateFontW(-px_size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        fonts_[fk] = f;
        return f;
    }

    HDC dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_dib_ = nullptr;
    std::uint32_t *dib_bits_ = nullptr;
    int dib_w_ = 0, dib_h_ = 0;
    std::unordered_map<FontKey, HFONT, FontKeyHash> fonts_;
    using LruList = std::list<std::pair<TextKey, TextMask>>;
    LruList lru_;
    std::unordered_map<TextKey, LruList::iterator, TextKeyHash> cache_map_;
};

std::mutex g_text_mutex;
GdiTextEngine &text_engine() {
    static GdiTextEngine eng;
    return eng;
}
#endif

}  // namespace

std::uint32_t lerp_color(std::uint32_t c0, std::uint32_t c1, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const int r0 = static_cast<int>(c0 & 255u), g0 = static_cast<int>((c0 >> 8) & 255u),
              b0 = static_cast<int>((c0 >> 16) & 255u), a0 = static_cast<int>((c0 >> 24) & 255u);
    const int r1 = static_cast<int>(c1 & 255u), g1 = static_cast<int>((c1 >> 8) & 255u),
              b1 = static_cast<int>((c1 >> 16) & 255u), a1 = static_cast<int>((c1 >> 24) & 255u);
    return rgba(static_cast<int>(r0 + (r1 - r0) * t + 0.5f),
                static_cast<int>(g0 + (g1 - g0) * t + 0.5f),
                static_cast<int>(b0 + (b1 - b0) * t + 0.5f),
                static_cast<int>(a0 + (a1 - a0) * t + 0.5f));
}

std::uint32_t with_alpha(std::uint32_t c, int a) {
    return (c & 0x00FFFFFFu) | (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24);
}

std::uint32_t scale_rgb(std::uint32_t c, float factor) {
    const int r = std::clamp(static_cast<int>((c & 255u) * factor + 0.5f), 0, 255);
    const int g = std::clamp(static_cast<int>(((c >> 8) & 255u) * factor + 0.5f), 0, 255);
    const int b = std::clamp(static_cast<int>(((c >> 16) & 255u) * factor + 0.5f), 0, 255);
    const int a = static_cast<int>((c >> 24) & 255u);
    return rgba(r, g, b, a);
}

void Surface::set_clip(int x, int y, int w, int h) {
    clip_x0 = std::clamp(x, 0, width);
    clip_y0 = std::clamp(y, 0, height);
    clip_x1 = std::clamp(x + w, clip_x0, width);
    clip_y1 = std::clamp(y + h, clip_y0, height);
}

void Surface::reset_clip() {
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = width;
    clip_y1 = height;
}

void Surface::blend_px(int x, int y, std::uint32_t col, float extra_alpha) {
    if (!pixels || x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    const int src_a = std::clamp(static_cast<int>(((col >> 24) & 255u) * extra_alpha + 0.5f), 0, 255);
    if (src_a <= 0) return;
    std::uint32_t &dst = pixels[static_cast<std::size_t>(y) * width + x];
    if (src_a >= 255) {
        dst = with_alpha(col, 255);
        return;
    }
    const int dst_a = static_cast<int>((dst >> 24) & 255u);
    if (dst_a == 0) {
        dst = with_alpha(col, src_a);
        return;
    }
    const int sr = static_cast<int>(col & 255u);
    const int sg = static_cast<int>((col >> 8) & 255u);
    const int sb = static_cast<int>((col >> 16) & 255u);
    const int dr = static_cast<int>(dst & 255u);
    const int dg = static_cast<int>((dst >> 8) & 255u);
    const int db = static_cast<int>((dst >> 16) & 255u);
    const int out_a = src_a + (dst_a * (255 - src_a) + 127) / 255;
    if (out_a <= 0) {
        dst = 0;
        return;
    }
    const int inv = 255 - src_a;
    const int r = (sr * src_a * 255 + dr * dst_a * inv) / (out_a * 255);
    const int g = (sg * src_a * 255 + dg * dst_a * inv) / (out_a * 255);
    const int b = (sb * src_a * 255 + db * dst_a * inv) / (out_a * 255);
    dst = rgba(r, g, b, out_a);
}

void Surface::fill_rect(int x, int y, int w, int h, std::uint32_t col) {
    const int x0 = std::max(clip_x0, x);
    const int y0 = std::max(clip_y0, y);
    const int x1 = std::min(clip_x1, x + w);
    const int y1 = std::min(clip_y1, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    const int a = static_cast<int>((col >> 24) & 255u);
    if (a == 255) {
        for (int j = y0; j < y1; ++j)
            std::fill(pixels + static_cast<std::size_t>(j) * width + x0,
                      pixels + static_cast<std::size_t>(j) * width + x1, col);
        return;
    }
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) blend_px(i, j, col);
}

void Surface::fill_vgrad_rect(int x, int y, int w, int h, std::uint32_t top_col, std::uint32_t bot_col) {
    const int x0 = std::max(clip_x0, x);
    const int y0 = std::max(clip_y0, y);
    const int x1 = std::min(clip_x1, x + w);
    const int y1 = std::min(clip_y1, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; ++j) {
        const float ty = h > 1 ? static_cast<float>(j - y) / static_cast<float>(h - 1) : 0.0f;
        const std::uint32_t row_col = lerp_color(top_col, bot_col, ty);
        const int a = static_cast<int>((row_col >> 24) & 255u);
        if (a == 255) {
            std::fill(pixels + static_cast<std::size_t>(j) * width + x0,
                      pixels + static_cast<std::size_t>(j) * width + x1, row_col);
        } else {
            for (int i = x0; i < x1; ++i) blend_px(i, j, row_col);
        }
    }
}

void Surface::fill_rounded_rect(int x, int y, int w, int h, float radius, std::uint32_t col) {
    if (w <= 0 || h <= 0) return;
    radius = std::clamp(radius, 0.0f, 0.5f * static_cast<float>(std::min(w, h)));
    const int x0 = std::max(clip_x0, x);
    const int y0 = std::max(clip_y0, y);
    const int x1 = std::min(clip_x1, x + w);
    const int y1 = std::min(clip_y1, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    const float cx = static_cast<float>(x) + 0.5f * static_cast<float>(w);
    const float cy = static_cast<float>(y) + 0.5f * static_cast<float>(h);
    const float bx = 0.5f * static_cast<float>(w);
    const float by = 0.5f * static_cast<float>(h);
    for (int j = y0; j < y1; ++j) {
        const float py = static_cast<float>(j) + 0.5f - cy;
        for (int i = x0; i < x1; ++i) {
            const float px = static_cast<float>(i) + 0.5f - cx;
            const float d = sd_rounded_box(px, py, bx, by, radius);
            if (d <= -0.5f) {
                blend_px(i, j, col);
            } else if (d < 0.5f) {
                blend_px(i, j, col, 0.5f - d);
            }
        }
    }
}

void Surface::fill_gradient_rounded_rect(int x, int y, int w, int h, float radius,
                                         std::uint32_t left_col, std::uint32_t right_col,
                                         float vertical_darken, bool top_specular) {
    if (w <= 0 || h <= 0) return;
    radius = std::clamp(radius, 0.0f, 0.5f * static_cast<float>(std::min(w, h)));
    const int x0 = std::max(clip_x0, x);
    const int y0 = std::max(clip_y0, y);
    const int x1 = std::min(clip_x1, x + w);
    const int y1 = std::min(clip_y1, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    const float cx = static_cast<float>(x) + 0.5f * static_cast<float>(w);
    const float cy = static_cast<float>(y) + 0.5f * static_cast<float>(h);
    const float bx = 0.5f * static_cast<float>(w);
    const float by = 0.5f * static_cast<float>(h);
    const std::uint32_t dark = rgba(6, 5, 11, static_cast<int>((left_col >> 24) & 255u));
    const std::uint32_t sheen = rgba(255, 250, 238, 255);
    for (int j = y0; j < y1; ++j) {
        const float py = static_cast<float>(j) + 0.5f - cy;
        const float ty = h > 1 ? static_cast<float>(j - y) / static_cast<float>(h - 1) : 0.0f;
        const float top_gloss = (top_specular && j < y + std::max(2, h / 5))
            ? 0.08f * (1.0f - static_cast<float>(j - y) / static_cast<float>(std::max(2, h / 5)))
            : 0.0f;
        for (int i = x0; i < x1; ++i) {
            const float px = static_cast<float>(i) + 0.5f - cx;
            const float d = sd_rounded_box(px, py, bx, by, radius);
            if (d >= 0.5f) continue;
            const float cov = d <= -0.5f ? 1.0f : (0.5f - d);
            const float tx = w > 1 ? static_cast<float>(i - x) / static_cast<float>(w - 1) : 0.0f;
            std::uint32_t col = lerp_color(left_col, right_col, tx);
            if (vertical_darken > 0.0f) col = lerp_color(col, dark, ty * vertical_darken);
            if (top_gloss > 0.0f) col = lerp_color(col, with_alpha(sheen, static_cast<int>((col >> 24) & 255u)), top_gloss);
            blend_px(i, j, col, cov);
        }
    }
}

void Surface::stroke_rounded_rect(int x, int y, int w, int h, float radius, float thickness, std::uint32_t col) {
    if (w <= 0 || h <= 0 || thickness <= 0.0f) return;
    radius = std::clamp(radius, 0.0f, 0.5f * static_cast<float>(std::min(w, h)));
    const int x0 = std::max(clip_x0, x - 1);
    const int y0 = std::max(clip_y0, y - 1);
    const int x1 = std::min(clip_x1, x + w + 1);
    const int y1 = std::min(clip_y1, y + h + 1);
    if (x0 >= x1 || y0 >= y1) return;
    const float cx = static_cast<float>(x) + 0.5f * static_cast<float>(w);
    const float cy = static_cast<float>(y) + 0.5f * static_cast<float>(h);
    const float bx = 0.5f * static_cast<float>(w);
    const float by = 0.5f * static_cast<float>(h);
    for (int j = y0; j < y1; ++j) {
        const float py = static_cast<float>(j) + 0.5f - cy;
        for (int i = x0; i < x1; ++i) {
            const float px = static_cast<float>(i) + 0.5f - cx;
            const float d = sd_rounded_box(px, py, bx, by, radius);
            // Band inside [-thickness, 0] with 1px smooth AA on both edges
            const float outer_cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            const float inner_cov = std::clamp(0.5f + (d + thickness), 0.0f, 1.0f);
            const float cov = std::min(outer_cov, inner_cov);
            if (cov > 0.001f) blend_px(i, j, col, cov);
        }
    }
}

void Surface::fill_circle(float cx, float cy, float r, std::uint32_t col) {
    if (r <= 0.0f) return;
    const int x0 = std::max(clip_x0, static_cast<int>(std::floor(cx - r - 1.0f)));
    const int y0 = std::max(clip_y0, static_cast<int>(std::floor(cy - r - 1.0f)));
    const int x1 = std::min(clip_x1, static_cast<int>(std::ceil(cx + r + 1.0f)));
    const int y1 = std::min(clip_y1, static_cast<int>(std::ceil(cy + r + 1.0f)));
    for (int j = y0; j < y1; ++j) {
        const float dy = static_cast<float>(j) + 0.5f - cy;
        for (int i = x0; i < x1; ++i) {
            const float dx = static_cast<float>(i) + 0.5f - cx;
            const float d = std::sqrt(dx * dx + dy * dy) - r;
            if (d <= -0.5f) blend_px(i, j, col);
            else if (d < 0.5f) blend_px(i, j, col, 0.5f - d);
        }
    }
}

void Surface::stroke_circle(float cx, float cy, float r, float thickness, std::uint32_t col) {
    if (r <= 0.0f || thickness <= 0.0f) return;
    const float half_t = 0.5f * thickness;
    const int x0 = std::max(clip_x0, static_cast<int>(std::floor(cx - r - half_t - 1.0f)));
    const int y0 = std::max(clip_y0, static_cast<int>(std::floor(cy - r - half_t - 1.0f)));
    const int x1 = std::min(clip_x1, static_cast<int>(std::ceil(cx + r + half_t + 1.0f)));
    const int y1 = std::min(clip_y1, static_cast<int>(std::ceil(cy + r + half_t + 1.0f)));
    for (int j = y0; j < y1; ++j) {
        const float dy = static_cast<float>(j) + 0.5f - cy;
        for (int i = x0; i < x1; ++i) {
            const float dx = static_cast<float>(i) + 0.5f - cx;
            const float d = std::fabs(std::sqrt(dx * dx + dy * dy) - r) - half_t;
            if (d <= -0.5f) blend_px(i, j, col);
            else if (d < 0.5f) blend_px(i, j, col, 0.5f - d);
        }
    }
}

void Surface::fill_radial_glow(float cx, float cy, float r, std::uint32_t center_col, std::uint32_t edge_col) {
    if (r <= 0.0f) return;
    const int x0 = std::max(clip_x0, static_cast<int>(std::floor(cx - r - 1.0f)));
    const int y0 = std::max(clip_y0, static_cast<int>(std::floor(cy - r - 1.0f)));
    const int x1 = std::min(clip_x1, static_cast<int>(std::ceil(cx + r + 1.0f)));
    const int y1 = std::min(clip_y1, static_cast<int>(std::ceil(cy + r + 1.0f)));
    for (int j = y0; j < y1; ++j) {
        const float dy = static_cast<float>(j) + 0.5f - cy;
        for (int i = x0; i < x1; ++i) {
            const float dx = static_cast<float>(i) + 0.5f - cx;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= r) continue;
            const float t = dist / r;
            const float smooth = t * t * (3.0f - 2.0f * t);
            blend_px(i, j, lerp_color(center_col, edge_col, smooth));
        }
    }
}

void Surface::draw_line(float x0f, float y0f, float x1f, float y1f, float thickness, std::uint32_t col) {
    const float half_t = 0.5f * std::max(0.75f, thickness);
    const int x0 = std::max(clip_x0, static_cast<int>(std::floor(std::min(x0f, x1f) - half_t - 1.0f)));
    const int y0 = std::max(clip_y0, static_cast<int>(std::floor(std::min(y0f, y1f) - half_t - 1.0f)));
    const int x1 = std::min(clip_x1, static_cast<int>(std::ceil(std::max(x0f, x1f) + half_t + 1.0f)));
    const int y1 = std::min(clip_y1, static_cast<int>(std::ceil(std::max(y0f, y1f) + half_t + 1.0f)));
    for (int j = y0; j < y1; ++j) {
        const float py = static_cast<float>(j) + 0.5f;
        for (int i = x0; i < x1; ++i) {
            const float px = static_cast<float>(i) + 0.5f;
            const float d = sd_segment(px, py, x0f, y0f, x1f, y1f) - half_t;
            if (d <= -0.5f) blend_px(i, j, col);
            else if (d < 0.5f) blend_px(i, j, col, 0.5f - d);
        }
    }
}

void Surface::fill_diamond(float cx, float cy, float rx, float ry, std::uint32_t col) {
    if (rx <= 0.0f || ry <= 0.0f) return;
    const int x0 = std::max(clip_x0, static_cast<int>(std::floor(cx - rx - 1.0f)));
    const int y0 = std::max(clip_y0, static_cast<int>(std::floor(cy - ry - 1.0f)));
    const int x1 = std::min(clip_x1, static_cast<int>(std::ceil(cx + rx + 1.0f)));
    const int y1 = std::min(clip_y1, static_cast<int>(std::ceil(cy + ry + 1.0f)));
    const float scale = std::min(rx, ry);
    for (int j = y0; j < y1; ++j) {
        const float dy = std::fabs(static_cast<float>(j) + 0.5f - cy) / ry;
        for (int i = x0; i < x1; ++i) {
            const float dx = std::fabs(static_cast<float>(i) + 0.5f - cx) / rx;
            const float d = (dx + dy - 1.0f) * scale * 0.7071f;
            if (d <= -0.5f) blend_px(i, j, col);
            else if (d < 0.5f) blend_px(i, j, col, 0.5f - d);
        }
    }
}

void Surface::draw_progress_bar(int x, int y, int w, int h, float ratio,
                                std::uint32_t bg_col, std::uint32_t fill_left, std::uint32_t fill_right,
                                std::uint32_t border_col) {
    if (w <= 2 || h <= 2) return;
    const float r = 0.5f * static_cast<float>(h);
    fill_rounded_rect(x, y, w, h, r, bg_col);
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (ratio > 0.001f) {
        const int inner_w = std::max(0, w - 2);
        const int fill_w = std::max(h - 2, static_cast<int>(static_cast<float>(inner_w) * ratio + 0.5f));
        const int clamped_w = std::min(inner_w, fill_w);
        if (clamped_w > 0) {
            fill_gradient_rounded_rect(x + 1, y + 1, clamped_w, h - 2, 0.5f * static_cast<float>(h - 2),
                                       fill_left, fill_right, 0.15f, true);
        }
    }
    stroke_rounded_rect(x, y, w, h, r, 1.0f, border_col);
}

void Surface::blit_rgba_fit(int dst_x, int dst_y, int dst_w, int dst_h,
                            const std::uint32_t *src, int src_w, int src_h, float corner_radius) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    corner_radius = std::clamp(corner_radius, 0.0f, 0.5f * static_cast<float>(std::min(dst_w, dst_h)));
    const int x0 = std::max(clip_x0, dst_x);
    const int y0 = std::max(clip_y0, dst_y);
    const int x1 = std::min(clip_x1, dst_x + dst_w);
    const int y1 = std::min(clip_y1, dst_y + dst_h);
    const float cx = static_cast<float>(dst_x) + 0.5f * static_cast<float>(dst_w);
    const float cy = static_cast<float>(dst_y) + 0.5f * static_cast<float>(dst_h);
    const float bx = 0.5f * static_cast<float>(dst_w);
    const float by = 0.5f * static_cast<float>(dst_h);
    for (int j = y0; j < y1; ++j) {
        const float py = static_cast<float>(j) + 0.5f - cy;
        const int sy = std::min(src_h - 1, static_cast<int>(static_cast<std::int64_t>(j - dst_y) * src_h / dst_h));
        for (int i = x0; i < x1; ++i) {
            const float px = static_cast<float>(i) + 0.5f - cx;
            const float d = sd_rounded_box(px, py, bx, by, corner_radius);
            if (d >= 0.5f) continue;
            const float cov = d <= -0.5f ? 1.0f : (0.5f - d);
            const int sx = std::min(src_w - 1, static_cast<int>(static_cast<std::int64_t>(i - dst_x) * src_w / dst_w));
            blend_px(i, j, src[static_cast<std::size_t>(sy) * src_w + sx], cov);
        }
    }
}

TextMetrics measure_text(FontStyle style, int px_size, bool bold, const std::string &text) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_text_mutex);
    return text_engine().measure(style, px_size, bold, text);
#else
    (void)style;
    (void)bold;
    const int s = std::max(1, (px_size + 4) / 8);
    return {static_cast<int>(text.size()) * 6 * s, 8 * s};
#endif
}

std::string fit_text_px(FontStyle style, int px_size, bool bold, const std::string &text, int max_width) {
    if (text.empty() || max_width <= 8) return "";
    if (measure_text(style, px_size, bold, text).width <= max_width) return text;
    const std::string ell = "...";
    const int ell_w = measure_text(style, px_size, bold, ell).width;
    if (ell_w >= max_width) return "";
    std::size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (measure_text(style, px_size, bold, text.substr(0, mid)).width + ell_w <= max_width) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && text[lo - 1] == ' ') --lo;
    return text.substr(0, lo) + ell;
}

void draw_text(Surface &s, int x, int y, FontStyle style, int px_size, bool bold,
               const std::string &text, std::uint32_t color, bool shadow) {
    if (text.empty() || !s.pixels) return;
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_text_mutex);
    const TextMask &mask = text_engine().rasterize(style, px_size, bold, text);
    if (mask.alpha.empty()) return;
    const int base_a = static_cast<int>((color >> 24) & 255u);
    if (shadow) {
        const std::uint32_t sh_col = rgba(4, 4, 8, (base_a * 175) / 255);
        for (int j = 0; j < mask.height; ++j) {
            const int dy = y + j + 1;
            if (dy < s.clip_y0 || dy >= s.clip_y1) continue;
            const std::uint8_t *row = mask.alpha.data() + static_cast<std::size_t>(j) * mask.width;
            for (int i = 0; i < mask.width; ++i) {
                const std::uint8_t a = row[i];
                if (a > 4) s.blend_px(x + i + 1, dy, sh_col, static_cast<float>(a) / 255.0f);
            }
        }
    }
    for (int j = 0; j < mask.height; ++j) {
        const int dy = y + j;
        if (dy < s.clip_y0 || dy >= s.clip_y1) continue;
        const std::uint8_t *row = mask.alpha.data() + static_cast<std::size_t>(j) * mask.width;
        for (int i = 0; i < mask.width; ++i) {
            const std::uint8_t a = row[i];
            if (a > 2) s.blend_px(x + i, dy, color, static_cast<float>(a) / 255.0f);
        }
    }
#else
    (void)style;
    (void)bold;
    const int sc = std::max(1, (px_size + 4) / 8);
    int cx = x;
    for (unsigned char c : text) {
        const char *g = kFallbackFont[(c >= 32 && c <= 126 ? c : '?') - 32];
        for (int r = 0; r < 8; ++r) {
            for (int col = 0; col < 5; ++col) {
                if (g[r * 6 + col] == '#') {
                    if (shadow) s.fill_rect(cx + col * sc + 1, y + r * sc + 1, sc, sc, rgba(4, 4, 8, 170));
                    s.fill_rect(cx + col * sc, y + r * sc, sc, sc, color);
                }
            }
        }
        cx += 6 * sc;
    }
#endif
}

int draw_wrapped_text(Surface &s, int x, int y, int max_width, int max_lines, int line_gap,
                      FontStyle style, int px_size, bool bold, const std::string &text,
                      std::uint32_t color, bool shadow) {
    if (text.empty() || max_width <= 16 || max_lines <= 0) return 0;
    const int line_h = std::max(px_size, measure_text(style, px_size, bold, "Ag").height) + line_gap;
    std::vector<std::string> lines;
    std::size_t i = 0;
    while (i < text.size() && static_cast<int>(lines.size()) < max_lines) {
        while (i < text.size() && text[i] == ' ') ++i;
        if (i >= text.size()) break;
        std::string cur;
        std::size_t next_i = i;
        while (next_i < text.size()) {
            if (text[next_i] == '\n') {
                ++next_i;
                break;
            }
            std::size_t word_end = next_i;
            while (word_end < text.size() && text[word_end] != ' ' && text[word_end] != '\n') ++word_end;
            const std::string word = text.substr(next_i, word_end - next_i);
            const std::string cand = cur.empty() ? word : (cur + " " + word);
            if (measure_text(style, px_size, bold, cand).width <= max_width) {
                cur = cand;
                next_i = word_end;
                while (next_i < text.size() && text[next_i] == ' ') ++next_i;
            } else {
                if (cur.empty()) {
                    cur = fit_text_px(style, px_size, bold, word, max_width);
                    next_i = word_end;
                }
                break;
            }
        }
        if (static_cast<int>(lines.size()) == max_lines - 1 && next_i < text.size()) {
            cur = fit_text_px(style, px_size, bold, cur + "...", max_width);
        }
        lines.push_back(std::move(cur));
        i = next_i;
    }
    int cy = y;
    for (const std::string &ln : lines) {
        draw_text(s, x, cy, style, px_size, bold, ln, color, shadow);
        cy += line_h;
    }
    return static_cast<int>(lines.size()) * line_h;
}

int draw_pill(Surface &s, int x, int y, int h, FontStyle style, int px_size, bool bold,
              const std::string &label, std::uint32_t bg_col, std::uint32_t border_col, std::uint32_t text_col) {
    const TextMetrics m = measure_text(style, px_size, bold, label);
    const int pad_x = std::max(7, h / 2);
    const int w = m.width + pad_x * 2;
    const float rad = 0.5f * static_cast<float>(h);
    s.fill_rounded_rect(x, y, w, h, rad, bg_col);
    s.stroke_rounded_rect(x, y, w, h, rad, 1.0f, border_col);
    const int ty = y + (h - m.height) / 2;
    draw_text(s, x + pad_x, ty, style, px_size, bold, label, text_col, false);
    return w;
}

void draw_tier_medallion(Surface &s, float cx, float cy, float radius, int tier_index,
                         std::uint32_t grad_left, std::uint32_t grad_right,
                         std::uint32_t border_col, std::uint32_t accent_col,
                         bool unlocked, bool secret_masked) {
    if (radius < 6.0f) return;
    const std::uint32_t ring_outer = unlocked ? border_col : with_alpha(scale_rgb(border_col, 0.45f), 205);
    const std::uint32_t ring_inner = unlocked ? accent_col : with_alpha(scale_rgb(accent_col, 0.35f), 170);
    const std::uint32_t core_dark = unlocked ? scale_rgb(grad_left, 0.65f) : rgba(16, 17, 26, 235);
    const std::uint32_t core_mid = unlocked ? scale_rgb(grad_right, 0.85f) : rgba(28, 31, 44, 235);
    const std::uint32_t emblem_hi = unlocked ? accent_col : rgba(120, 128, 148, 170);
    const std::uint32_t emblem_mid = unlocked ? border_col : rgba(84, 90, 108, 150);

    // Subtle outer aura glow when unlocked
    if (unlocked) {
        s.fill_radial_glow(cx, cy, radius * 1.18f, with_alpha(accent_col, 68), with_alpha(accent_col, 0));
    }

    // Medallion base disc + radial inner highlight
    s.fill_circle(cx, cy, radius, with_alpha(core_dark, 245));
    s.fill_radial_glow(cx - radius * 0.16f, cy - radius * 0.18f, radius * 0.92f,
                       with_alpha(core_mid, 215), with_alpha(core_dark, 0));

    // Double metallic bezel rings
    const float rim_t = std::max(1.4f, radius * 0.085f);
    s.stroke_circle(cx, cy, radius - rim_t * 0.5f, rim_t, ring_outer);
    s.stroke_circle(cx, cy, radius * 0.82f, std::max(1.0f, radius * 0.045f), with_alpha(ring_inner, unlocked ? 190 : 110));

    // Cardinal bezel ticks (Judgment Ring motif)
    for (int k = 0; k < 8; ++k) {
        const float ang = static_cast<float>(k) * (kPi * 0.25f);
        const float r0 = radius * (k % 2 == 0 ? 0.78f : 0.84f);
        const float r1 = radius * 0.94f;
        s.draw_line(cx + std::cos(ang) * r0, cy + std::sin(ang) * r0,
                    cx + std::cos(ang) * r1, cy + std::sin(ang) * r1,
                    std::max(1.0f, radius * 0.04f), with_alpha(ring_inner, unlocked ? 185 : 95));
    }

    const float e = radius * 0.50f;
    const float lt = std::max(1.4f, radius * 0.085f);

    if (secret_masked) {
        // Mysterious veiled question/occult key glyph
        s.stroke_circle(cx, cy - e * 0.26f, e * 0.36f, lt, emblem_hi);
        s.draw_line(cx, cy + e * 0.10f, cx, cy + e * 0.42f, lt, emblem_hi);
        s.fill_circle(cx, cy + e * 0.70f, std::max(1.6f, lt * 0.75f), emblem_hi);
        return;
    }

    switch (std::clamp(tier_index, 0, 6)) {
    case 0:  // Mark: Beveled Diamond Seal
        s.fill_diamond(cx, cy, e * 0.95f, e * 1.05f, with_alpha(emblem_mid, 215));
        s.fill_diamond(cx, cy, e * 0.60f, e * 0.68f, with_alpha(emblem_hi, 240));
        s.fill_diamond(cx, cy, e * 0.25f, e * 0.28f, with_alpha(core_dark, 230));
        break;

    case 1:  // Sigil: Arcane Judgment Ring & Inscribed Cross
        s.stroke_circle(cx, cy, e * 0.82f, lt * 1.05f, emblem_hi);
        s.draw_line(cx - e * 1.02f, cy, cx + e * 1.02f, cy, lt * 0.9f, emblem_hi);
        s.draw_line(cx, cy - e * 1.02f, cx, cy + e * 1.02f, lt * 0.9f, emblem_hi);
        s.fill_diamond(cx, cy, e * 0.34f, e * 0.34f, emblem_mid);
        s.fill_circle(cx, cy, e * 0.16f, unlocked ? rgba(255, 252, 240, 250) : emblem_hi);
        break;

    case 2:  // Crest: Twin-Winged Heraldic Shield
        s.draw_line(cx - e * 0.78f, cy - e * 0.62f, cx + e * 0.78f, cy - e * 0.62f, lt * 1.15f, emblem_hi);
        s.draw_line(cx - e * 0.78f, cy - e * 0.62f, cx - e * 0.64f, cy + e * 0.18f, lt * 1.1f, emblem_hi);
        s.draw_line(cx + e * 0.78f, cy - e * 0.62f, cx + e * 0.64f, cy + e * 0.18f, lt * 1.1f, emblem_hi);
        s.draw_line(cx - e * 0.64f, cy + e * 0.18f, cx, cy + e * 0.98f, lt * 1.15f, emblem_hi);
        s.draw_line(cx + e * 0.64f, cy + e * 0.18f, cx, cy + e * 0.98f, lt * 1.15f, emblem_hi);
        s.fill_diamond(cx, cy - e * 0.02f, e * 0.38f, e * 0.50f, emblem_mid);
        s.fill_circle(cx, cy - e * 0.04f, e * 0.18f, emblem_hi);
        break;

    case 3:  // Sovereign: Royal 5-Point Crown
        s.draw_line(cx - e * 0.88f, cy + e * 0.58f, cx + e * 0.88f, cy + e * 0.58f, lt * 1.4f, emblem_hi);
        s.draw_line(cx - e * 0.82f, cy + e * 0.45f, cx - e * 0.92f, cy - e * 0.46f, lt * 1.1f, emblem_hi);
        s.draw_line(cx + e * 0.82f, cy + e * 0.45f, cx + e * 0.92f, cy - e * 0.46f, lt * 1.1f, emblem_hi);
        s.draw_line(cx - e * 0.92f, cy - e * 0.46f, cx - e * 0.38f, cy + e * 0.06f, lt * 1.0f, emblem_hi);
        s.draw_line(cx - e * 0.38f, cy + e * 0.06f, cx, cy - e * 0.78f, lt * 1.15f, emblem_hi);
        s.draw_line(cx, cy - e * 0.78f, cx + e * 0.38f, cy + e * 0.06f, lt * 1.15f, emblem_hi);
        s.draw_line(cx + e * 0.38f, cy + e * 0.06f, cx + e * 0.92f, cy - e * 0.46f, lt * 1.0f, emblem_hi);
        s.fill_circle(cx, cy - e * 0.82f, lt * 0.75f, unlocked ? rgba(255, 248, 210, 255) : emblem_hi);
        s.fill_circle(cx - e * 0.92f, cy - e * 0.50f, lt * 0.65f, emblem_mid);
        s.fill_circle(cx + e * 0.92f, cy - e * 0.50f, lt * 0.65f, emblem_mid);
        break;

    case 4:  // The Covenant: Radiant Eclipse & 8-Ray Judgment Star
        for (int k = 0; k < 8; ++k) {
            const float ang = static_cast<float>(k) * (kPi * 0.25f);
            const float len = (k % 2 == 0) ? e * 1.08f : e * 0.76f;
            s.draw_line(cx, cy, cx + std::cos(ang) * len, cy + std::sin(ang) * len,
                        (k % 2 == 0) ? lt * 1.1f : lt * 0.8f, emblem_hi);
        }
        s.fill_circle(cx, cy, e * 0.46f, emblem_mid);
        s.fill_circle(cx, cy, e * 0.28f, unlocked ? rgba(255, 250, 224, 255) : core_dark);
        break;

    case 5:  // Folly: Theatrical Starburst / Jester Emblem
        for (int k = 0; k < 6; ++k) {
            const float ang = static_cast<float>(k) * (kPi / 3.0f) + kPi / 6.0f;
            s.draw_line(cx, cy, cx + std::cos(ang) * e * 0.92f, cy + std::sin(ang) * e * 0.92f, lt * 0.95f, emblem_mid);
            s.fill_circle(cx + std::cos(ang) * e * 0.94f, cy + std::sin(ang) * e * 0.94f, lt * 0.70f, emblem_hi);
        }
        s.fill_circle(cx, cy, e * 0.36f, emblem_hi);
        s.fill_circle(cx, cy, e * 0.16f, core_dark);
        break;

    case 6:  // Curiosity: Occult All-Seeing Eye
    default:
        // Upper & lower eyelids
        for (int seg = 0; seg < 10; ++seg) {
            const float t0 = -1.0f + 2.0f * static_cast<float>(seg) / 10.0f;
            const float t1 = -1.0f + 2.0f * static_cast<float>(seg + 1) / 10.0f;
            const float x0 = cx + t0 * e * 0.98f;
            const float x1 = cx + t1 * e * 0.98f;
            const float y0u = cy - (1.0f - t0 * t0) * e * 0.62f;
            const float y1u = cy - (1.0f - t1 * t1) * e * 0.62f;
            const float y0d = cy + (1.0f - t0 * t0) * e * 0.62f;
            const float y1d = cy + (1.0f - t1 * t1) * e * 0.62f;
            s.draw_line(x0, y0u, x1, y1u, lt, emblem_hi);
            s.draw_line(x0, y0d, x1, y1d, lt, emblem_hi);
        }
        s.fill_circle(cx, cy, e * 0.40f, emblem_mid);
        s.stroke_circle(cx, cy, e * 0.40f, lt * 0.7f, emblem_hi);
        s.fill_diamond(cx, cy, e * 0.14f, e * 0.34f, core_dark);
        break;
    }

    // Small corner padlock badge if locked (and not secret-masked)
    if (!unlocked) {
        const float lx = cx + radius * 0.48f;
        const float ly = cy + radius * 0.48f;
        const float lr = std::max(5.0f, radius * 0.28f);
        s.fill_circle(lx, ly, lr, rgba(16, 18, 28, 240));
        s.stroke_circle(lx, ly, lr, 1.2f, rgba(140, 148, 170, 220));
        s.stroke_circle(lx, ly - lr * 0.22f, lr * 0.32f, 1.2f, rgba(200, 208, 225, 220));
        s.fill_rounded_rect(static_cast<int>(lx - lr * 0.42f), static_cast<int>(ly - lr * 0.05f),
                            std::max(4, static_cast<int>(lr * 0.84f)), std::max(4, static_cast<int>(lr * 0.66f)),
                            1.5f, rgba(200, 208, 225, 230));
    }
}

bool load_png_thumbnail(const std::filesystem::path &path, int max_w, int max_h,
                        std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h) {
    out_rgba.clear();
    out_w = 0;
    out_h = 0;
#ifdef _WIN32
    if (path.empty() || max_w <= 0 || max_h <= 0) return false;
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool did_co = (co_hr == S_OK || co_hr == S_FALSE);

    IWICImagingFactory *factory = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) && factory) {
        const std::wstring wpath = path.wstring();
        if (SUCCEEDED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
            if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
                UINT sw = 0, sh = 0;
                if (SUCCEEDED(frame->GetSize(&sw, &sh)) && sw > 0 && sh > 0) {
                    const double sx = static_cast<double>(max_w) / static_cast<double>(sw);
                    const double sy = static_cast<double>(max_h) / static_cast<double>(sh);
                    const double sc = std::min({1.0, sx, sy});
                    const UINT dw = std::max<UINT>(1u, static_cast<UINT>(sw * sc + 0.5));
                    const UINT dh = std::max<UINT>(1u, static_cast<UINT>(sh * sc + 0.5));
                    if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler &&
                        SUCCEEDED(scaler->Initialize(frame, dw, dh, WICBitmapInterpolationModeFant)) &&
                        SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv &&
                        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA,
                                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                        out_rgba.resize(static_cast<std::size_t>(dw) * dh);
                        const UINT stride = dw * 4u;
                        const UINT buf_size = stride * dh;
                        if (SUCCEEDED(conv->CopyPixels(nullptr, stride, buf_size,
                                                       reinterpret_cast<BYTE *>(out_rgba.data())))) {
                            out_w = static_cast<int>(dw);
                            out_h = static_cast<int>(dh);
                            ok = true;
                        } else {
                            out_rgba.clear();
                        }
                    }
                }
            }
        }
    }

    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (did_co) CoUninitialize();
    return ok;
#else
    (void)path;
    (void)max_w;
    (void)max_h;
    return false;
#endif
}

}  // namespace ui_draw
