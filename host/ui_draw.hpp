#pragma once

// High-resolution software RGBA2D renderer, cached Win32 TrueType typography, ornate vector
// tier medallions, and WIC PNG thumbnail loader shared by the Trophy Gallery GUI and Unlock Toast.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ui_draw {

enum class FontStyle : int {
    Serif = 0,  // Georgia (Gothic headings, achievement titles)
    Sans = 1    // Segoe UI (clean descriptions, stats, pills)
};

constexpr std::uint32_t rgba(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r & 255) |
           (static_cast<std::uint32_t>(g & 255) << 8) |
           (static_cast<std::uint32_t>(b & 255) << 16) |
           (static_cast<std::uint32_t>(a & 255) << 24);
}

std::uint32_t lerp_color(std::uint32_t c0, std::uint32_t c1, float t);
std::uint32_t with_alpha(std::uint32_t c, int a);
std::uint32_t scale_rgb(std::uint32_t c, float factor);

struct Surface {
    std::uint32_t *pixels = nullptr;
    int width = 0;
    int height = 0;
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;

    Surface() = default;
    Surface(std::uint32_t *px, int w, int h)
        : pixels(px), width(w), height(h), clip_x0(0), clip_y0(0), clip_x1(w), clip_y1(h) {}

    void set_clip(int x, int y, int w, int h);
    void reset_clip();
    void blend_px(int x, int y, std::uint32_t col, float extra_alpha = 1.0f);
    void fill_rect(int x, int y, int w, int h, std::uint32_t col);
    void fill_vgrad_rect(int x, int y, int w, int h, std::uint32_t top_col, std::uint32_t bot_col);
    void fill_rounded_rect(int x, int y, int w, int h, float radius, std::uint32_t col);
    void fill_gradient_rounded_rect(int x, int y, int w, int h, float radius,
                                    std::uint32_t left_col, std::uint32_t right_col,
                                    float vertical_darken = 0.18f, bool top_specular = true);
    void stroke_rounded_rect(int x, int y, int w, int h, float radius, float thickness, std::uint32_t col);
    void fill_circle(float cx, float cy, float r, std::uint32_t col);
    void stroke_circle(float cx, float cy, float r, float thickness, std::uint32_t col);
    void fill_radial_glow(float cx, float cy, float r, std::uint32_t center_col, std::uint32_t edge_col);
    void draw_line(float x0, float y0, float x1, float y1, float thickness, std::uint32_t col);
    void fill_diamond(float cx, float cy, float rx, float ry, std::uint32_t col);
    void draw_progress_bar(int x, int y, int w, int h, float ratio,
                           std::uint32_t bg_col, std::uint32_t fill_left, std::uint32_t fill_right,
                           std::uint32_t border_col);
    void blit_rgba_fit(int dst_x, int dst_y, int dst_w, int dst_h,
                       const std::uint32_t *src, int src_w, int src_h, float corner_radius = 6.0f);
};

// Typography (anti-aliased Win32 GDI TrueType with LRU alpha-mask cache + portable bitmap fallback)
struct TextMetrics {
    int width = 0;
    int height = 0;
};

TextMetrics measure_text(FontStyle style, int px_size, bool bold, const std::string &text);
std::string fit_text_px(FontStyle style, int px_size, bool bold, const std::string &text, int max_width);
void draw_text(Surface &s, int x, int y, FontStyle style, int px_size, bool bold,
               const std::string &text, std::uint32_t color, bool shadow = true);
int draw_wrapped_text(Surface &s, int x, int y, int max_width, int max_lines, int line_gap,
                      FontStyle style, int px_size, bool bold, const std::string &text,
                      std::uint32_t color, bool shadow = true);

// Draw a rounded status/tier pill badge and return its rendered pixel width.
int draw_pill(Surface &s, int x, int y, int h, FontStyle style, int px_size, bool bold,
              const std::string &label, std::uint32_t bg_col, std::uint32_t border_col, std::uint32_t text_col);

// Render a high-resolution multi-tone geometric Tier Medallion at (cx, cy) with outer radius `radius`.
// tier_index: 0=Mark, 1=Sigil, 2=Crest, 3=Sovereign, 4=Covenant, 5=Folly, 6=Curiosity
void draw_tier_medallion(Surface &s, float cx, float cy, float radius, int tier_index,
                         std::uint32_t grad_left, std::uint32_t grad_right,
                         std::uint32_t border_col, std::uint32_t accent_col,
                         bool unlocked, bool secret_masked = false);

// Load a PNG file and scale it down to fit within (max_w x max_h) preserving aspect ratio.
// Uses Windows Imaging Component (WIC) on Windows. Returns false if unavailable or unreadable.
bool load_png_thumbnail(const std::filesystem::path &path, int max_w, int max_h,
                        std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h);

}  // namespace ui_draw
