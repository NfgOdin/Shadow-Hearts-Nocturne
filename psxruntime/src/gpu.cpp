#include "psx/gpu.hpp"
#include "psx/gpu_hw.hpp"
#include "psx/pgxp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psx {

namespace {

inline int sign_extend11(std::uint32_t v) noexcept {
    return static_cast<int>((static_cast<std::int32_t>(v << 21)) >> 21);
}

inline std::uint16_t pack_bgr555(int r8, int g8, int b8, std::uint16_t mask_bit = 0) noexcept {
    const std::uint16_t r5 = static_cast<std::uint16_t>(std::clamp(r8 >> 3, 0, 31));
    const std::uint16_t g5 = static_cast<std::uint16_t>(std::clamp(g8 >> 3, 0, 31));
    const std::uint16_t b5 = static_cast<std::uint16_t>(std::clamp(b8 >> 3, 0, 31));
    return static_cast<std::uint16_t>(r5 | (g5 << 5) | (b5 << 10) | mask_bit);
}

inline std::uint32_t pack_rgba8888(int r8, int g8, int b8) noexcept {
    const std::uint32_t r = static_cast<std::uint32_t>(std::clamp(r8, 0, 255));
    const std::uint32_t g = static_cast<std::uint32_t>(std::clamp(g8, 0, 255));
    const std::uint32_t b = static_cast<std::uint32_t>(std::clamp(b8, 0, 255));
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

}  // namespace

Gpu::Gpu()
    : vram(static_cast<std::size_t>(kVramWidth * kVramHeight), 0),
      hires(static_cast<std::size_t>(kVramWidth * 2 * kVramHeight * 2), 0xFF000000u),
      hw_(std::make_unique<GpuHwBackend>()) {
    reset();
}

Gpu::~Gpu() = default;

bool Gpu::is_hw_renderer_active() const noexcept {
    if (!hw_renderer || !hw_) return false;
    if (!hw_->is_available()) {
        if (!hw_->init(scale)) {
            return false;
        }
    }
    return hw_->is_available();
}

void Gpu::set_pgxp_enabled(bool en) noexcept {
    pgxp_enabled = en;
    pgxp::set_enabled(en);
    if (!en) {
        pgxp::clear();
    }
}

void Gpu::set_hw_renderer(bool en) {
    if (hw_renderer == en) return;
    hw_renderer = en;
    if (hw_renderer && is_hw_renderer_active()) {
        hw_->set_scale(scale, vram.data());
        hw_->sync_full_vram(vram.data());
    } else {
        scale = std::clamp(scale, 1, 4);
        sync_hires_from_vram();
    }
}

void Gpu::set_filter_3d(int mode) noexcept {
    filter_3d = std::clamp(mode, 0, 2);
    sharp_3d = (filter_3d == 2);
    if (hw_) {
        hw_->set_filter_mode(filter_3d);
    }
}

void Gpu::reset() {
    std::fill(vram.begin(), vram.end(), 0u);
    const int max_s = hw_renderer ? 8 : 4;
    scale = std::clamp(scale, 1, max_s);
    const int sw_s = std::clamp(scale, 1, 4);
    hires.assign(static_cast<std::size_t>(kVramWidth * sw_s * kVramHeight * sw_s), 0xFF000000u);
    draw_mode = 0;
    tex_window = 0;
    clip_x1 = 0;
    clip_y1 = 0;
    clip_x2 = 1023;
    clip_y2 = 511;
    offset_x = 0;
    offset_y = 0;
    force_set_mask = false;
    check_mask_before_draw = false;
    gpustat = 0x14802000u;
    disp_vram_x = 0;
    disp_vram_y = 0;
    disp_h1 = 0x200;
    disp_h2 = 0xC00;
    disp_v1 = 0x010;
    disp_v2 = 0x100;
    disp_mode = 0;
    display_disabled = false;
    is_24bit = false;
    gpu_info_latch = 2u;
    phase_ = Phase::Command;
    cmd_buf_.clear();
    read_fifo_.clear();
    read_fifo_pos_ = 0;
    pgxp::set_enabled(pgxp_enabled);
    pgxp::clear();
    if (hw_ && hw_->is_available()) {
        hw_->sync_full_vram(vram.data());
    }
}

void Gpu::set_scale(int new_scale) {
    const int max_s = hw_renderer ? 8 : 4;
    const int clamped = std::clamp(new_scale, 1, max_s);
    const int sw_s = std::clamp(clamped, 1, 4);
    const bool same_scale = (clamped == scale) &&
        (hires.size() == static_cast<std::size_t>(kVramWidth * sw_s * kVramHeight * sw_s));
    scale = clamped;
    if (is_hw_renderer_active()) {
        hw_->set_scale(scale, vram.data());
    }
    if (!same_scale) {
        hires.resize(static_cast<std::size_t>(kVramWidth * sw_s * kVramHeight * sw_s));
        sync_hires_from_vram();
    }
}

void Gpu::sync_hires_from_vram() {
    const int s = std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    if (hires.size() != static_cast<std::size_t>(hw * kVramHeight * s)) {
        hires.resize(static_cast<std::size_t>(hw * kVramHeight * s));
    }
    for (int y = 0; y < kVramHeight; ++y) {
        const std::uint16_t *vrow = vram.data() + y * kVramWidth;
        for (int x = 0; x < kVramWidth; ++x) {
            const std::uint32_t c32 = rgb555_to_rgba8888(vrow[x]);
            for (int dy = 0; dy < s; ++dy) {
                std::uint32_t *hrow = hires.data() + (y * s + dy) * hw + x * s;
                for (int dx = 0; dx < s; ++dx) hrow[dx] = c32;
            }
        }
    }
    if (is_hw_renderer_active()) {
        hw_->sync_full_vram(vram.data());
    }
}

void Gpu::put_vram_pixel(int x, int y, std::uint16_t pix) {
    const int vx = x & 1023;
    const int vy = y & 511;
    vram[vy * kVramWidth + vx] = pix;
    const std::uint32_t c32 = rgb555_to_rgba8888(pix);
    const int s = std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    for (int dy = 0; dy < s; ++dy) {
        std::uint32_t *hrow = hires.data() + (vy * s + dy) * hw + vx * s;
        for (int dx = 0; dx < s; ++dx) hrow[dx] = c32;
    }
    if (hw_) {
        hw_->notify_vram_dirty(vx, vy, 1, 1);
    }
}

int Gpu::command_arg_count(std::uint8_t cmd) noexcept {
    if (cmd == 0x02u) return 3;  // Quick FillRect
    if (cmd >= 0x20u && cmd <= 0x3Fu) {
        const int verts = (cmd & 0x08u) ? 4 : 3;
        const bool shaded = (cmd & 0x10u) != 0;
        const bool textured = (cmd & 0x04u) != 0;
        int words = 1 + verts;          // cmd+color0 + vertex coords
        if (shaded) words += verts - 1; // extra vertex colors
        if (textured) words += verts;   // UV + CLUT/texpage words
        return words;
    }
    if (cmd >= 0x40u && cmd <= 0x5Fu) {
        const bool polyline = (cmd & 0x08u) != 0;
        if (polyline) return -1;  // Variable length terminated by 0x55555555
        const bool shaded = (cmd & 0x10u) != 0;
        return shaded ? 4 : 3;
    }
    if (cmd >= 0x60u && cmd <= 0x7Fu) {
        const bool textured = (cmd & 0x04u) != 0;
        const int size_mode = (cmd >> 3) & 3;
        int words = 2;  // cmd+color + XY
        if (textured) ++words;
        if (size_mode == 0) ++words;  // Variable WH
        return words;
    }
    if (cmd >= 0x80u && cmd <= 0x9Fu) return 4;  // CopyVram
    if (cmd >= 0xA0u && cmd <= 0xBFu) return 3;  // LoadImage header
    if (cmd >= 0xC0u && cmd <= 0xDFu) return 3;  // StoreImage header
    return 1;
}

std::uint16_t Gpu::sample_texture(int u, int v, int texpage_x, int texpage_y,
                                  int depth, int clut_x, int clut_y) const noexcept {
    // Apply GP0(E2) texture window mask & offset
    const int mask_x = static_cast<int>((tex_window & 0x1Fu) << 3);
    const int mask_y = static_cast<int>(((tex_window >> 5) & 0x1Fu) << 3);
    const int off_x = static_cast<int>(((tex_window >> 10) & 0x1Fu) << 3);
    const int off_y = static_cast<int>(((tex_window >> 15) & 0x1Fu) << 3);

    const int tu = (u & ~mask_x) | (off_x & mask_x);
    const int tv = (v & ~mask_y) | (off_y & mask_y);
    const int vy = (texpage_y + (tv & 255)) & 511;

    if (depth == 0) {
        // 4-bit CLUT
        const int vx = (texpage_x + ((tu & 255) >> 2)) & 1023;
        const std::uint16_t texel_word = vram[vy * kVramWidth + vx];
        const int nibble = (texel_word >> ((tu & 3) * 4)) & 0xFu;
        const int cx = (clut_x + nibble) & 1023;
        return vram[(clut_y & 511) * kVramWidth + cx];
    }
    if (depth == 1) {
        // 8-bit CLUT
        const int vx = (texpage_x + ((tu & 255) >> 1)) & 1023;
        const std::uint16_t texel_word = vram[vy * kVramWidth + vx];
        const int idx = (texel_word >> ((tu & 1) * 8)) & 0xFFu;
        const int cx = (clut_x + idx) & 1023;
        return vram[(clut_y & 511) * kVramWidth + cx];
    }
    // 15-bit Direct
    const int vx = (texpage_x + (tu & 255)) & 1023;
    return vram[vy * kVramWidth + vx];
}

void Gpu::sample_texture_filtered(float u_f, float v_f, int texpage_x, int texpage_y,
                                  int depth, int clut_x, int clut_y,
                                  int &out_r, int &out_g, int &out_b,
                                  std::uint16_t &out_center_texel) const noexcept {
    const int eff_mode = (filter_3d > 0) ? filter_3d : (sharp_3d ? 2 : 0);
    const int cu = static_cast<int>(std::floor(u_f + 0.5f));
    const int cv = static_cast<int>(std::floor(v_f + 0.5f));
    out_center_texel = sample_texture(cu, cv, texpage_x, texpage_y, depth, clut_x, clut_y);
    if (out_center_texel == 0u || eff_mode == 0) {
        out_r = ((out_center_texel & 31u) << 3) | ((out_center_texel & 31u) >> 2);
        out_g = (((out_center_texel >> 5) & 31u) << 3) | (((out_center_texel >> 5) & 31u) >> 2);
        out_b = (((out_center_texel >> 10) & 31u) << 3) | (((out_center_texel >> 10) & 31u) >> 2);
        return;
    }

    float su = u_f;
    float sv = v_f;
    if (eff_mode == 2) {
        // Sharp-3D pre-sharpened bilinear
        const float flu = std::floor(su);
        const float flv = std::floor(sv);
        const float fru = su - flu;
        const float frv = sv - flv;
        su = flu + std::clamp((fru - 0.5f) * 2.0f + 0.5f, 0.0f, 1.0f);
        sv = flv + std::clamp((frv - 0.5f) * 2.0f + 0.5f, 0.0f, 1.0f);
    }

    const int u0 = static_cast<int>(std::floor(su));
    const int v0 = static_cast<int>(std::floor(sv));
    const float fx = su - static_cast<float>(u0);
    const float fy = sv - static_cast<float>(v0);

    const std::uint16_t t00 = sample_texture(u0,     v0,     texpage_x, texpage_y, depth, clut_x, clut_y);
    const std::uint16_t t10 = sample_texture(u0 + 1, v0,     texpage_x, texpage_y, depth, clut_x, clut_y);
    const std::uint16_t t01 = sample_texture(u0,     v0 + 1, texpage_x, texpage_y, depth, clut_x, clut_y);
    const std::uint16_t t11 = sample_texture(u0 + 1, v0 + 1, texpage_x, texpage_y, depth, clut_x, clut_y);

    const float w00 = (t00 != 0u) ? ((1.0f - fx) * (1.0f - fy)) : 0.0f;
    const float w10 = (t10 != 0u) ? (fx * (1.0f - fy)) : 0.0f;
    const float w01 = (t01 != 0u) ? ((1.0f - fx) * fy) : 0.0f;
    const float w11 = (t11 != 0u) ? (fx * fy) : 0.0f;
    const float wsum = w00 + w10 + w01 + w11;

    if (wsum <= 0.001f) {
        out_r = ((out_center_texel & 31u) << 3) | ((out_center_texel & 31u) >> 2);
        out_g = (((out_center_texel >> 5) & 31u) << 3) | (((out_center_texel >> 5) & 31u) >> 2);
        out_b = (((out_center_texel >> 10) & 31u) << 3) | (((out_center_texel >> 10) & 31u) >> 2);
        return;
    }

    auto unp_r = [](std::uint16_t t) { return static_cast<float>(((t & 31u) << 3) | ((t & 31u) >> 2)); };
    auto unp_g = [](std::uint16_t t) { return static_cast<float>((((t >> 5) & 31u) << 3) | (((t >> 5) & 31u) >> 2)); };
    auto unp_b = [](std::uint16_t t) { return static_cast<float>((((t >> 10) & 31u) << 3) | (((t >> 10) & 31u) >> 2)); };

    out_r = static_cast<int>((unp_r(t00) * w00 + unp_r(t10) * w10 + unp_r(t01) * w01 + unp_r(t11) * w11) / wsum + 0.5f);
    out_g = static_cast<int>((unp_g(t00) * w00 + unp_g(t10) * w10 + unp_g(t01) * w01 + unp_g(t11) * w11) / wsum + 0.5f);
    out_b = static_cast<int>((unp_b(t00) * w00 + unp_b(t10) * w10 + unp_b(t01) * w01 + unp_b(t11) * w11) / wsum + 0.5f);
}

std::uint16_t Gpu::blend_semi_transparent(std::uint16_t bg, std::uint16_t fg, int mode) noexcept {
    int br = bg & 31, bg_g = (bg >> 5) & 31, bb = (bg >> 10) & 31;
    int fr = fg & 31, fg_g = (fg >> 5) & 31, fb = (fg >> 10) & 31;
    int r = 0, g = 0, b = 0;
    switch (mode & 3) {
    case 0: r = (br + fr) >> 1; g = (bg_g + fg_g) >> 1; b = (bb + fb) >> 1; break;      // 0.5*B + 0.5*F
    case 1: r = br + fr; g = bg_g + fg_g; b = bb + fb; break;                            // 1.0*B + 1.0*F
    case 2: r = br - fr; g = bg_g - fg_g; b = bb - fb; break;                            // 1.0*B - 1.0*F
    case 3: r = br + (fr >> 2); g = bg_g + (fg_g >> 2); b = bb + (fb >> 2); break;       // 1.0*B + 0.25*F
    }
    r = std::clamp(r, 0, 31);
    g = std::clamp(g, 0, 31);
    b = std::clamp(b, 0, 31);
    return static_cast<std::uint16_t>(r | (g << 5) | (b << 10) | (fg & 0x8000u));
}

std::uint32_t Gpu::blend_semi_transparent32(std::uint32_t bg, std::uint32_t fg, int mode) noexcept {
    int br = bg & 255, bg_g = (bg >> 8) & 255, bb = (bg >> 16) & 255;
    int fr = fg & 255, fg_g = (fg >> 8) & 255, fb = (fg >> 16) & 255;
    int r = 0, g = 0, b = 0;
    switch (mode & 3) {
    case 0: r = (br + fr) >> 1; g = (bg_g + fg_g) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg_g + fg_g; b = bb + fb; break;
    case 2: r = br - fr; g = bg_g - fg_g; b = bb - fb; break;
    case 3: r = br + (fr >> 2); g = bg_g + (fg_g >> 2); b = bb + (fb >> 2); break;
    }
    return pack_rgba8888(r, g, b);
}

void Gpu::draw_triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2,
                        bool shaded, bool textured, bool semi_trans, bool raw_tex,
                        int texpage_x, int texpage_y, int tex_depth, int semi_mode,
                        int clut_x, int clut_y) {
    // Reject triangles exceeding PS1 hardware max span (1023 x 511)
    const int min_vx = std::min({v0.x, v1.x, v2.x});
    const int max_vx = std::max({v0.x, v1.x, v2.x});
    const int min_vy = std::min({v0.y, v1.y, v2.y});
    const int max_vy = std::max({v0.y, v1.y, v2.y});
    if (max_vx - min_vx >= 1024 || max_vy - min_vy >= 512) return;

    const bool use_pgxp = pgxp_enabled && (v0.has_pgxp || v1.has_pgxp || v2.has_pgxp);
    const bool use_hw = is_hw_renderer_active();

    // Forward high-res polygon to D3D11 hardware backend when active
    if (use_hw) {
        const std::uint32_t flags = (textured ? 1u : 0u) |
                                    (raw_tex ? 2u : 0u) |
                                    (semi_trans ? 4u : 0u) |
                                    32u;  // bit 5 = 3D polygon
        auto make_hv = [&](const Vertex &v) {
            HwVertex hv{};
            hv.x = use_pgxp ? v.fx : static_cast<float>(v.x);
            hv.y = use_pgxp ? v.fy : static_cast<float>(v.y);
            hv.z = use_pgxp ? std::max(1.0f, v.fz) : 1.0f;
            hv.r = static_cast<float>(shaded ? v.r : v0.r) / 255.0f;
            hv.g = static_cast<float>(shaded ? v.g : v0.g) / 255.0f;
            hv.b = static_cast<float>(shaded ? v.b : v0.b) / 255.0f;
            hv.u = static_cast<float>(v.u);
            hv.v = static_cast<float>(v.v);
            hv.texpage_x = static_cast<std::uint32_t>(texpage_x);
            hv.texpage_y = static_cast<std::uint32_t>(texpage_y);
            hv.clut_x = static_cast<std::uint32_t>(clut_x);
            hv.clut_y = static_cast<std::uint32_t>(clut_y);
            hv.tex_depth = static_cast<std::uint32_t>(tex_depth);
            hv.tex_window = tex_window;
            hv.flags = flags;
            return hv;
        };
        hw_->set_filter_mode((filter_3d > 0) ? filter_3d : (sharp_3d ? 2 : 0));
        hw_->draw_triangle(make_hv(v0), make_hv(v1), make_hv(v2),
                           semi_trans, semi_mode,
                           clip_x1, clip_y1, clip_x2, clip_y2,
                           vram.data());
    }

    // When D3D11 hardware renderer owns the high-res canvas, only rasterize 1x on CPU to keep 1x VRAM accurate
    // for save states, CPU readbacks, and mask bits. In Software mode, rasterize at full 1x..4x scale with PGXP.
    const int s = use_hw ? 1 : std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    const float fs = static_cast<float>(s);
    const float x0 = (use_pgxp ? v0.fx : static_cast<float>(v0.x)) * fs;
    const float y0 = (use_pgxp ? v0.fy : static_cast<float>(v0.y)) * fs;
    const float x1 = (use_pgxp ? v1.fx : static_cast<float>(v1.x)) * fs;
    const float y1 = (use_pgxp ? v1.fy : static_cast<float>(v1.y)) * fs;
    const float x2 = (use_pgxp ? v2.fx : static_cast<float>(v2.x)) * fs;
    const float y2 = (use_pgxp ? v2.fy : static_cast<float>(v2.y)) * fs;

    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (std::fabs(area) < 1e-4f) return;

    const int bx0 = std::max(clip_x1 * s, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
    const int by0 = std::max(clip_y1 * s, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
    const int bx1 = std::min((clip_x2 + 1) * s - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
    const int by1 = std::min((clip_y2 + 1) * s - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
    if (bx0 > bx1 || by0 > by1) return;

    const float inv_area = 1.0f / area;
    const std::uint16_t mask_or = force_set_mask ? 0x8000u : 0u;
    const float inv_z0 = use_pgxp ? (1.0f / std::max(1.0f, v0.fz)) : 1.0f;
    const float inv_z1 = use_pgxp ? (1.0f / std::max(1.0f, v1.fz)) : 1.0f;
    const float inv_z2 = use_pgxp ? (1.0f / std::max(1.0f, v2.fz)) : 1.0f;
    const int eff_filter = (filter_3d > 0) ? filter_3d : (sharp_3d ? 2 : 0);

    for (int py = by0; py <= by1; ++py) {
        const int vy = py / s;
        const float fpy = static_cast<float>(py);
        for (int px = bx0; px <= bx1; ++px) {
            const float fpx = static_cast<float>(px);
            const float w0 = (x1 - fpx) * (y2 - fpy) - (y1 - fpy) * (x2 - fpx);
            const float w1 = (x2 - fpx) * (y0 - fpy) - (y2 - fpy) * (x0 - fpx);
            const float w2 = area - w0 - w1;

            if (area > 0.0f) {
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            } else {
                if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f) continue;
            }

            const int vx = px / s;
            const std::uint16_t old_vram = vram[vy * kVramWidth + vx];
            if (check_mask_before_draw && (old_vram & 0x8000u)) continue;

            const float b0 = w0 * inv_area;
            const float b1 = w1 * inv_area;
            const float b2 = 1.0f - b0 - b1;

            int cr = v0.r, cg = v0.g, cb = v0.b;
            if (shaded) {
                cr = static_cast<int>(b0 * v0.r + b1 * v1.r + b2 * v2.r + 0.5f);
                cg = static_cast<int>(b0 * v0.g + b1 * v1.g + b2 * v2.g + 0.5f);
                cb = static_cast<int>(b0 * v0.b + b1 * v1.b + b2 * v2.b + 0.5f);
            }

            bool apply_semi = semi_trans;
            std::uint16_t tex_mask = 0;

            if (textured) {
                float u_f = 0.0f;
                float v_f = 0.0f;
                if (use_pgxp) {
                    const float wz0 = b0 * inv_z0;
                    const float wz1 = b1 * inv_z1;
                    const float wz2 = b2 * inv_z2;
                    const float inv_w = 1.0f / std::max(1e-7f, wz0 + wz1 + wz2);
                    u_f = (wz0 * v0.u + wz1 * v1.u + wz2 * v2.u) * inv_w;
                    v_f = (wz0 * v0.v + wz1 * v1.v + wz2 * v2.v) * inv_w;
                } else {
                    u_f = b0 * v0.u + b1 * v1.u + b2 * v2.u;
                    v_f = b0 * v0.v + b1 * v1.v + b2 * v2.v;
                }

                int tr = 0, tg = 0, tb = 0;
                std::uint16_t texel = 0;
                if (!use_hw && eff_filter > 0) {
                    sample_texture_filtered(u_f, v_f, texpage_x, texpage_y, tex_depth, clut_x, clut_y,
                                            tr, tg, tb, texel);
                } else {
                    const int tu = static_cast<int>(std::floor(u_f + 0.5f));
                    const int tv = static_cast<int>(std::floor(v_f + 0.5f));
                    texel = sample_texture(tu, tv, texpage_x, texpage_y, tex_depth, clut_x, clut_y);
                    tr = ((texel & 31u) << 3) | ((texel & 31u) >> 2);
                    tg = (((texel >> 5) & 31u) << 3) | (((texel >> 5) & 31u) >> 2);
                    tb = (((texel >> 10) & 31u) << 3) | (((texel >> 10) & 31u) >> 2);
                }
                if (texel == 0u) continue;  // 0x0000 is transparent in PS1 textures!
                tex_mask = texel & 0x8000u;
                apply_semi = semi_trans && (tex_mask != 0);

                if (raw_tex) {
                    cr = tr; cg = tg; cb = tb;
                } else {
                    cr = (tr * cr) >> 7;
                    cg = (tg * cg) >> 7;
                    cb = (tb * cb) >> 7;
                }
            }

            if (!use_hw) {
                std::uint32_t out32 = pack_rgba8888(cr, cg, cb);
                if (apply_semi) {
                    out32 = blend_semi_transparent32(hires[py * hw + px], out32, semi_mode);
                }
                hires[py * hw + px] = out32;
            }

            // Keep 1x VRAM synchronized on center subpixel
            if ((px % s) == 0 && (py % s) == 0) {
                std::uint16_t out16 = pack_bgr555(cr, cg, cb, mask_or | tex_mask);
                if (apply_semi) {
                    out16 = blend_semi_transparent(old_vram, out16, semi_mode) | mask_or;
                }
                vram[vy * kVramWidth + vx] = out16;
            }
        }
    }
    ++primitives_drawn;
}

void Gpu::draw_line_segment(const Vertex &v0, const Vertex &v1, bool shaded, bool semi_trans, int semi_mode) {
    if (std::abs(v1.x - v0.x) >= 1024 || std::abs(v1.y - v0.y) >= 512) return;
    const bool use_hw = is_hw_renderer_active();
    const int s = use_hw ? 1 : std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    const int x0 = v0.x * s, y0 = v0.y * s;
    const int x1 = v1.x * s, y1 = v1.y * s;
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    const std::uint16_t mask_or = force_set_mask ? 0x8000u : 0u;

    for (int i = 0; i <= steps; ++i) {
        const float t = (steps > 0) ? (static_cast<float>(i) / static_cast<float>(steps)) : 0.0f;
        const int px = static_cast<int>(std::floor(x0 + t * (x1 - x0) + 0.5f));
        const int py = static_cast<int>(std::floor(y0 + t * (y1 - y0) + 0.5f));
        const int vx = px / s, vy = py / s;
        if (vx < clip_x1 || vx > clip_x2 || vy < clip_y1 || vy > clip_y2) continue;

        const std::uint16_t old_vram = vram[vy * kVramWidth + vx];
        if (check_mask_before_draw && (old_vram & 0x8000u)) continue;

        const int cr = shaded ? static_cast<int>(v0.r + t * (v1.r - v0.r) + 0.5f) : v0.r;
        const int cg = shaded ? static_cast<int>(v0.g + t * (v1.g - v0.g) + 0.5f) : v0.g;
        const int cb = shaded ? static_cast<int>(v0.b + t * (v1.b - v0.b) + 0.5f) : v0.b;

        if (!use_hw) {
            std::uint32_t out32 = pack_rgba8888(cr, cg, cb);
            if (semi_trans) out32 = blend_semi_transparent32(hires[py * hw + px], out32, semi_mode);
            hires[py * hw + px] = out32;
        }

        if ((px % s) == 0 && (py % s) == 0) {
            std::uint16_t out16 = pack_bgr555(cr, cg, cb, mask_or);
            if (semi_trans) out16 = blend_semi_transparent(old_vram, out16, semi_mode) | mask_or;
            vram[vy * kVramWidth + vx] = out16;
        }
    }

    if (use_hw) {
        const float fx0 = static_cast<float>(v0.x) + 0.5f;
        const float fy0 = static_cast<float>(v0.y) + 0.5f;
        const float fx1 = static_cast<float>(v1.x) + 0.5f;
        const float fy1 = static_cast<float>(v1.y) + 0.5f;
        const float dx = fx1 - fx0;
        const float dy = fy1 - fy0;
        const float len = std::sqrt(dx * dx + dy * dy);
        const float nx = (len > 1e-4f) ? (-dy / len * 0.5f) : 0.0f;
        const float ny = (len > 1e-4f) ? (dx / len * 0.5f) : 0.5f;

        HwVertex a{}, b{}, c{}, d{};
        a.x = fx0 + nx; a.y = fy0 + ny; a.z = 1.0f;
        b.x = fx0 - nx; b.y = fy0 - ny; b.z = 1.0f;
        c.x = fx1 + nx; c.y = fy1 + ny; c.z = 1.0f;
        d.x = fx1 - nx; d.y = fy1 - ny; d.z = 1.0f;
        a.r = b.r = static_cast<float>(v0.r) / 255.0f;
        a.g = b.g = static_cast<float>(v0.g) / 255.0f;
        a.b = b.b = static_cast<float>(v0.b) / 255.0f;
        c.r = d.r = static_cast<float>(shaded ? v1.r : v0.r) / 255.0f;
        c.g = d.g = static_cast<float>(shaded ? v1.g : v0.g) / 255.0f;
        c.b = d.b = static_cast<float>(shaded ? v1.b : v0.b) / 255.0f;
        a.flags = b.flags = c.flags = d.flags = semi_trans ? 4u : 0u;

        hw_->draw_triangle(a, b, c, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
        hw_->draw_triangle(b, d, c, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
    }
    ++primitives_drawn;
}

void Gpu::draw_rectangle(const Vertex &v0, int w, int h, bool textured, bool semi_trans, bool raw_tex,
                         int clut_x, int clut_y) {
    if (w <= 0 || h <= 0) return;
    const int texpage_x = static_cast<int>((draw_mode & 0xFu) * 64u);
    const int texpage_y = static_cast<int>(((draw_mode >> 4) & 1u) * 256u);
    const int semi_mode = static_cast<int>((draw_mode >> 5) & 3u);
    const int tex_depth = static_cast<int>((draw_mode >> 7) & 3u);
    const std::uint16_t mask_or = force_set_mask ? 0x8000u : 0u;
    const bool use_hw = is_hw_renderer_active();
    const int s = std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;

    if (use_hw) {
        const std::uint32_t flags = (textured ? 1u : 0u) |
                                    (raw_tex ? 2u : 0u) |
                                    (semi_trans ? 4u : 0u);
        auto make_rv = [&](int px, int py, int pu, int pv) {
            HwVertex hv{};
            hv.x = static_cast<float>(px);
            hv.y = static_cast<float>(py);
            hv.z = 1.0f;
            hv.r = static_cast<float>(v0.r) / 255.0f;
            hv.g = static_cast<float>(v0.g) / 255.0f;
            hv.b = static_cast<float>(v0.b) / 255.0f;
            hv.u = static_cast<float>(pu);
            hv.v = static_cast<float>(pv);
            hv.texpage_x = static_cast<std::uint32_t>(texpage_x);
            hv.texpage_y = static_cast<std::uint32_t>(texpage_y);
            hv.clut_x = static_cast<std::uint32_t>(clut_x);
            hv.clut_y = static_cast<std::uint32_t>(clut_y);
            hv.tex_depth = static_cast<std::uint32_t>(tex_depth);
            hv.tex_window = tex_window;
            hv.flags = flags;
            return hv;
        };
        const HwVertex q00 = make_rv(v0.x,     v0.y,     v0.u,     v0.v);
        const HwVertex q10 = make_rv(v0.x + w, v0.y,     v0.u + w, v0.v);
        const HwVertex q01 = make_rv(v0.x,     v0.y + h, v0.u,     v0.v + h);
        const HwVertex q11 = make_rv(v0.x + w, v0.y + h, v0.u + w, v0.v + h);
        hw_->draw_triangle(q00, q10, q01, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
        hw_->draw_triangle(q10, q11, q01, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
    }

    for (int dy = 0; dy < h; ++dy) {
        const int vy = v0.y + dy;
        if (vy < clip_y1 || vy > clip_y2) continue;
        for (int dx = 0; dx < w; ++dx) {
            const int vx = v0.x + dx;
            if (vx < clip_x1 || vx > clip_x2) continue;

            const std::uint16_t old_vram = vram[vy * kVramWidth + vx];
            if (check_mask_before_draw && (old_vram & 0x8000u)) continue;

            int cr = v0.r, cg = v0.g, cb = v0.b;
            bool apply_semi = semi_trans;
            std::uint16_t tex_mask = 0;

            if (textured) {
                const std::uint16_t texel = sample_texture(v0.u + dx, v0.v + dy, texpage_x, texpage_y, tex_depth, clut_x, clut_y);
                if (texel == 0u) continue;
                tex_mask = texel & 0x8000u;
                apply_semi = semi_trans && (tex_mask != 0);
                const int tr = ((texel & 31u) << 3) | ((texel & 31u) >> 2);
                const int tg = (((texel >> 5) & 31u) << 3) | (((texel >> 5) & 31u) >> 2);
                const int tb = (((texel >> 10) & 31u) << 3) | (((texel >> 10) & 31u) >> 2);
                if (raw_tex) {
                    cr = tr; cg = tg; cb = tb;
                } else {
                    cr = (tr * cr) >> 7;
                    cg = (tg * cg) >> 7;
                    cb = (tb * cb) >> 7;
                }
            }

            std::uint16_t out16 = pack_bgr555(cr, cg, cb, mask_or | tex_mask);
            if (apply_semi) out16 = blend_semi_transparent(old_vram, out16, semi_mode) | mask_or;
            vram[vy * kVramWidth + vx] = out16;

            if (!use_hw) {
                const std::uint32_t fg32 = pack_rgba8888(cr, cg, cb);
                for (int sy = 0; sy < s; ++sy) {
                    std::uint32_t *hrow = hires.data() + (vy * s + sy) * hw + vx * s;
                    for (int sx = 0; sx < s; ++sx) {
                        hrow[sx] = apply_semi ? blend_semi_transparent32(hrow[sx], fg32, semi_mode) : fg32;
                    }
                }
            }
        }
    }
    ++primitives_drawn;
}

void Gpu::write_gp0(std::uint32_t word) {
    if (phase_ == Phase::CpuToVram) {
        for (int half = 0; half < 2; ++half) {
            const std::uint16_t pix = static_cast<std::uint16_t>((word >> (half * 16)) & 0xFFFFu);
            put_vram_pixel(xfer_x_ + xfer_cur_x_, xfer_y_ + xfer_cur_y_, pix);
            if (++xfer_cur_x_ >= xfer_w_) {
                xfer_cur_x_ = 0;
                if (++xfer_cur_y_ >= xfer_h_) {
                    if (is_hw_renderer_active()) {
                        hw_->blit_vram_rect_to_rt(vram.data(), xfer_x_, xfer_y_, xfer_w_, xfer_h_);
                    }
                    phase_ = Phase::Command;
                    cmd_buf_.clear();
                    return;
                }
            }
        }
        return;
    }

    if (phase_ == Phase::PolyLine) {
        if ((word & 0xF000F000u) == 0x50005000u && !polyline_expect_color_) {
            execute_gp0_packet();
            phase_ = Phase::Command;
            cmd_buf_.clear();
            return;
        }
        cmd_buf_.push_back(word);
        if (polyline_shaded_) polyline_expect_color_ = !polyline_expect_color_;
        return;
    }

    if (phase_ == Phase::Command) {
        cur_cmd_ = static_cast<std::uint8_t>(word >> 24);
        cmd_buf_.clear();
        cmd_buf_.push_back(word);
        args_needed_ = command_arg_count(cur_cmd_);
        if (args_needed_ < 0) {
            phase_ = Phase::PolyLine;
            polyline_shaded_ = (cur_cmd_ & 0x10u) != 0;
            polyline_expect_color_ = false;
            return;
        }
        if (args_needed_ <= 1) {
            execute_gp0_packet();
            cmd_buf_.clear();
            return;
        }
        phase_ = Phase::Args;
        return;
    }

    // Phase::Args
    cmd_buf_.push_back(word);
    if (static_cast<int>(cmd_buf_.size()) >= args_needed_) {
        execute_gp0_packet();
        if (phase_ != Phase::CpuToVram) {
            phase_ = Phase::Command;
            cmd_buf_.clear();
        }
    }
}

void Gpu::execute_gp0_packet() {
    const std::uint8_t cmd = cur_cmd_;
    const std::uint32_t w0 = cmd_buf_[0];

    if (cmd == 0x00u || cmd == 0x01u) return;  // NOP / Clear cache

    if (cmd == 0x02u) {  // Quick FillRect
        const std::uint16_t fill_pix = pack_bgr555(w0 & 0xFFu, (w0 >> 8) & 0xFFu, (w0 >> 16) & 0xFFu);
        const int x0 = static_cast<int>(cmd_buf_[1] & 0x3F0u);
        const int y0 = static_cast<int>((cmd_buf_[1] >> 16) & 0x1FFu);
        int w = static_cast<int>(((cmd_buf_[2] & 0x3FFu) + 0xFu) & ~0xFu);
        int h = static_cast<int>((cmd_buf_[2] >> 16) & 0x1FFu);
        for (int dy = 0; dy < h; ++dy) {
            for (int dx = 0; dx < w; ++dx) {
                put_vram_pixel(x0 + dx, y0 + dy, fill_pix);
            }
        }
        if (is_hw_renderer_active()) {
            hw_->blit_vram_rect_to_rt(vram.data(), x0, y0, w, h);
        }
        return;
    }

    if (cmd == 0x1Fu) {  // IRQ1
        gpustat |= (1u << 24);
        return;
    }

    // Polygons (0x20..0x3F)
    if (cmd >= 0x20u && cmd <= 0x3Fu) {
        const bool is_quad = (cmd & 0x08u) != 0;
        const bool shaded = (cmd & 0x10u) != 0;
        const bool textured = (cmd & 0x04u) != 0;
        const bool semi_trans = (cmd & 0x02u) != 0;
        const bool raw_tex = (cmd & 0x01u) != 0;
        const int nverts = is_quad ? 4 : 3;

        Vertex v[4];
        std::size_t idx = 0;
        int clut_x = 0, clut_y = 0;
        int texpage_x = static_cast<int>((draw_mode & 0xFu) * 64u);
        int texpage_y = static_cast<int>(((draw_mode >> 4) & 1u) * 256u);
        int semi_mode = static_cast<int>((draw_mode >> 5) & 3u);
        int tex_depth = static_cast<int>((draw_mode >> 7) & 3u);

        for (int i = 0; i < nverts; ++i) {
            if (i == 0 || shaded) {
                const std::uint32_t cw = cmd_buf_[idx++];
                v[i].r = static_cast<std::uint8_t>(cw & 0xFFu);
                v[i].g = static_cast<std::uint8_t>((cw >> 8) & 0xFFu);
                v[i].b = static_cast<std::uint8_t>((cw >> 16) & 0xFFu);
            } else {
                v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b;
            }
            const std::uint32_t xy = cmd_buf_[idx++];
            const int raw_x = sign_extend11(xy & 0xFFFFu);
            const int raw_y = sign_extend11(xy >> 16);
            v[i].x = raw_x + offset_x;
            v[i].y = raw_y + offset_y;

            float pfx = 0.0f, pfy = 0.0f, pfz = 1.0f;
            if (pgxp_enabled && pgxp::lookup(xy, pfx, pfy, pfz)) {
                v[i].fx = pfx + static_cast<float>(offset_x);
                v[i].fy = pfy + static_cast<float>(offset_y);
                v[i].fz = pfz;
                v[i].has_pgxp = true;
                ++pgxp_hits;
            } else {
                v[i].fx = static_cast<float>(v[i].x);
                v[i].fy = static_cast<float>(v[i].y);
                v[i].fz = 1.0f;
                v[i].has_pgxp = false;
                if (pgxp_enabled) ++pgxp_misses;
            }

            if (textured) {
                const std::uint32_t uv = cmd_buf_[idx++];
                v[i].u = static_cast<int>(uv & 0xFFu);
                v[i].v = static_cast<int>((uv >> 8) & 0xFFu);
                const std::uint16_t attr = static_cast<std::uint16_t>(uv >> 16);
                if (i == 0) {
                    clut_x = (attr & 0x3Fu) * 16;
                    clut_y = (attr >> 6) & 0x1FFu;
                } else if (i == 1) {
                    texpage_x = (attr & 0xFu) * 64;
                    texpage_y = ((attr >> 4) & 1u) * 256;
                    semi_mode = (attr >> 5) & 3u;
                    tex_depth = (attr >> 7) & 3u;
                    draw_mode = (draw_mode & ~0x1FFu) | (attr & 0x1FFu);
                }
            }
        }

        draw_triangle(v[0], v[1], v[2], shaded, textured, semi_trans, raw_tex,
                      texpage_x, texpage_y, tex_depth, semi_mode, clut_x, clut_y);
        if (is_quad) {
            draw_triangle(v[1], v[2], v[3], shaded, textured, semi_trans, raw_tex,
                          texpage_x, texpage_y, tex_depth, semi_mode, clut_x, clut_y);
        }
        return;
    }

    // Lines & Polylines (0x40..0x5F)
    if (cmd >= 0x40u && cmd <= 0x5Fu) {
        const bool shaded = (cmd & 0x10u) != 0;
        const bool semi_trans = (cmd & 0x02u) != 0;
        const int semi_mode = static_cast<int>((draw_mode >> 5) & 3u);

        std::vector<Vertex> pts;
        std::size_t idx = 0;
        while (idx < cmd_buf_.size()) {
            Vertex pt;
            if (pts.empty() || shaded) {
                const std::uint32_t cw = cmd_buf_[idx++];
                pt.r = static_cast<std::uint8_t>(cw & 0xFFu);
                pt.g = static_cast<std::uint8_t>((cw >> 8) & 0xFFu);
                pt.b = static_cast<std::uint8_t>((cw >> 16) & 0xFFu);
            } else {
                pt.r = pts[0].r; pt.g = pts[0].g; pt.b = pts[0].b;
            }
            if (idx >= cmd_buf_.size()) break;
            const std::uint32_t xy = cmd_buf_[idx++];
            pt.x = sign_extend11(xy & 0xFFFFu) + offset_x;
            pt.y = sign_extend11(xy >> 16) + offset_y;
            pts.push_back(pt);
        }
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            draw_line_segment(pts[i], pts[i + 1], shaded, semi_trans, semi_mode);
        }
        return;
    }

    // Rectangles / Sprites (0x60..0x7F)
    if (cmd >= 0x60u && cmd <= 0x7Fu) {
        const bool textured = (cmd & 0x04u) != 0;
        const bool semi_trans = (cmd & 0x02u) != 0;
        const bool raw_tex = (cmd & 0x01u) != 0;
        const int size_mode = (cmd >> 3) & 3;

        Vertex v0;
        v0.r = static_cast<std::uint8_t>(w0 & 0xFFu);
        v0.g = static_cast<std::uint8_t>((w0 >> 8) & 0xFFu);
        v0.b = static_cast<std::uint8_t>((w0 >> 16) & 0xFFu);
        v0.x = sign_extend11(cmd_buf_[1] & 0xFFFFu) + offset_x;
        v0.y = sign_extend11(cmd_buf_[1] >> 16) + offset_y;

        std::size_t idx = 2;
        int clut_x = 0, clut_y = 0;
        if (textured) {
            const std::uint32_t uv = cmd_buf_[idx++];
            v0.u = static_cast<int>(uv & 0xFFu);
            v0.v = static_cast<int>((uv >> 8) & 0xFFu);
            const std::uint16_t clut = static_cast<std::uint16_t>(uv >> 16);
            clut_x = (clut & 0x3Fu) * 16;
            clut_y = (clut >> 6) & 0x1FFu;
        }
        int w = 0, h = 0;
        if (size_mode == 0) {
            const std::uint32_t wh = cmd_buf_[idx];
            w = static_cast<int>(wh & 0x3FFu);
            h = static_cast<int>((wh >> 16) & 0x1FFu);
        } else if (size_mode == 1) { w = 1; h = 1; }
        else if (size_mode == 2) { w = 8; h = 8; }
        else { w = 16; h = 16; }

        draw_rectangle(v0, w, h, textured, semi_trans, raw_tex, clut_x, clut_y);
        return;
    }

    // VRAM -> VRAM copy (0x80..0x9F)
    if (cmd >= 0x80u && cmd <= 0x9Fu) {
        const int sx = static_cast<int>(cmd_buf_[1] & 0x3FFu);
        const int sy = static_cast<int>((cmd_buf_[1] >> 16) & 0x1FFu);
        const int dx = static_cast<int>(cmd_buf_[2] & 0x3FFu);
        const int dy = static_cast<int>((cmd_buf_[2] >> 16) & 0x1FFu);
        int w = static_cast<int>(((cmd_buf_[3] - 1u) & 0x3FFu) + 1u);
        int h = static_cast<int>((((cmd_buf_[3] >> 16) - 1u) & 0x1FFu) + 1u);
        const std::uint16_t mask_or = force_set_mask ? 0x8000u : 0u;

        std::vector<std::uint16_t> tmp(static_cast<std::size_t>(w * h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                tmp[y * w + x] = vram[((sy + y) & 511) * kVramWidth + ((sx + x) & 1023)];
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int tx = (dx + x) & 1023;
                const int ty = (dy + y) & 511;
                if (check_mask_before_draw && (vram[ty * kVramWidth + tx] & 0x8000u)) continue;
                put_vram_pixel(tx, ty, tmp[y * w + x] | mask_or);
            }
        }
        if (is_hw_renderer_active()) {
            hw_->blit_vram_rect_to_rt(vram.data(), dx, dy, w, h);
        }
        return;
    }

    // CPU -> VRAM upload (0xA0..0xBF)
    if (cmd >= 0xA0u && cmd <= 0xBFu) {
        xfer_x_ = static_cast<int>(cmd_buf_[1] & 0x3FFu);
        xfer_y_ = static_cast<int>((cmd_buf_[1] >> 16) & 0x1FFu);
        xfer_w_ = static_cast<int>(((cmd_buf_[2] - 1u) & 0x3FFu) + 1u);
        xfer_h_ = static_cast<int>((((cmd_buf_[2] >> 16) - 1u) & 0x1FFu) + 1u);
        xfer_cur_x_ = 0;
        xfer_cur_y_ = 0;
        phase_ = Phase::CpuToVram;
        ++vram_uploads;
        return;
    }

    // VRAM -> CPU download (0xC0..0xDF)
    if (cmd >= 0xC0u && cmd <= 0xDFu) {
        const int sx = static_cast<int>(cmd_buf_[1] & 0x3FFu);
        const int sy = static_cast<int>((cmd_buf_[1] >> 16) & 0x1FFu);
        const int w = static_cast<int>(((cmd_buf_[2] - 1u) & 0x3FFu) + 1u);
        const int h = static_cast<int>((((cmd_buf_[2] >> 16) - 1u) & 0x1FFu) + 1u);
        read_fifo_.clear();
        read_fifo_.reserve(static_cast<std::size_t>(w * h + 1));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                read_fifo_.push_back(vram[((sy + y) & 511) * kVramWidth + ((sx + x) & 1023)]);
            }
        }
        if (read_fifo_.size() & 1u) read_fifo_.push_back(0u);
        read_fifo_pos_ = 0;
        return;
    }

    // Environment registers (0xE1..0xE6)
    switch (cmd) {
    case 0xE1u:
        draw_mode = w0 & 0x3FFFu;
        gpustat = (gpustat & ~0x87FFu) | (w0 & 0x7FFu) | ((w0 & 0x800u) << 4);
        break;
    case 0xE2u:
        tex_window = w0 & 0xFFFFFu;
        break;
    case 0xE3u:
        clip_x1 = static_cast<int>(w0 & 0x3FFu);
        clip_y1 = static_cast<int>((w0 >> 10) & 0x1FFu);
        break;
    case 0xE4u:
        clip_x2 = static_cast<int>(w0 & 0x3FFu);
        clip_y2 = static_cast<int>((w0 >> 10) & 0x1FFu);
        break;
    case 0xE5u:
        offset_x = sign_extend11(w0 & 0x7FFu);
        offset_y = sign_extend11((w0 >> 11) & 0x7FFu);
        break;
    case 0xE6u:
        force_set_mask = (w0 & 1u) != 0;
        check_mask_before_draw = (w0 & 2u) != 0;
        gpustat = (gpustat & ~(3u << 11)) | ((w0 & 3u) << 11);
        break;
    default:
        break;
    }
}

void Gpu::write_gp1(std::uint32_t word) {
    const std::uint32_t cmd = (word >> 24) & 0x3Fu;
    switch (cmd) {
    case 0x00u:  // Reset GPU
        draw_mode = 0;
        tex_window = 0;
        clip_x1 = 0; clip_y1 = 0;
        clip_x2 = 1023; clip_y2 = 511;
        offset_x = 0; offset_y = 0;
        force_set_mask = false;
        check_mask_before_draw = false;
        gpustat = 0x14802000u;
        disp_vram_x = 0; disp_vram_y = 0;
        disp_h1 = 0x200; disp_h2 = 0xC00;
        disp_v1 = 0x010; disp_v2 = 0x100;
        disp_mode = 0;
        display_disabled = true;
        is_24bit = false;
        phase_ = Phase::Command;
        cmd_buf_.clear();
        break;

    case 0x01u:  // Reset command buffer
        phase_ = Phase::Command;
        cmd_buf_.clear();
        break;

    case 0x02u:  // Acknowledge GPU IRQ
        gpustat &= ~(1u << 24);
        break;

    case 0x03u:  // Display enable (0=on, 1=off)
        display_disabled = (word & 1u) != 0;
        gpustat = (gpustat & ~(1u << 23)) | ((word & 1u) << 23);
        break;

    case 0x04u:  // DMA direction
        gpustat = (gpustat & ~(3u << 29)) | ((word & 3u) << 29);
        break;

    case 0x05u:  // Display area start in VRAM
        disp_vram_x = static_cast<int>(word & 0x3FEu);
        disp_vram_y = static_cast<int>((word >> 10) & 0x1FFu);
        break;

    case 0x06u:  // Horizontal display range
        disp_h1 = static_cast<int>(word & 0xFFFu);
        disp_h2 = static_cast<int>((word >> 12) & 0xFFFu);
        break;

    case 0x07u:  // Vertical display range
        disp_v1 = static_cast<int>(word & 0x3FFu);
        disp_v2 = static_cast<int>((word >> 10) & 0x3FFu);
        break;

    case 0x08u:  // Display mode
        disp_mode = word & 0xFFu;
        is_24bit = (word & 0x10u) != 0;
        gpustat = (gpustat & ~0x007F4000u) |
                  ((word & 0x3Fu) << 17) |
                  (((word >> 6) & 1u) << 16) |
                  (((word >> 7) & 1u) << 14);
        break;

    default:
        if (cmd >= 0x10u && cmd <= 0x1Fu) {
            switch (word & 7u) {
            case 2u: gpu_info_latch = tex_window; break;
            case 3u: gpu_info_latch = static_cast<std::uint32_t>((clip_y1 << 10) | clip_x1); break;
            case 4u: gpu_info_latch = static_cast<std::uint32_t>((clip_y2 << 10) | clip_x2); break;
            case 5u: gpu_info_latch = static_cast<std::uint32_t>(((offset_y & 0x7FF) << 11) | (offset_x & 0x7FF)); break;
            case 7u: gpu_info_latch = 2u; break;  // GPU version 2
            default: break;
            }
        }
        break;
    }
}

std::uint32_t Gpu::read_data() {
    if (read_fifo_pos_ + 1 < read_fifo_.size()) {
        const std::uint32_t lo = read_fifo_[read_fifo_pos_++];
        const std::uint32_t hi = read_fifo_[read_fifo_pos_++];
        return lo | (hi << 16);
    }
    return gpu_info_latch;
}

std::uint32_t Gpu::read_stat() const {
    std::uint32_t st = gpustat | (1u << 26) | (1u << 28);  // Ready for commands & DMA blocks
    if (read_fifo_pos_ < read_fifo_.size()) st |= (1u << 27);
    st |= (1u << 25);  // DMA request
    return st;
}

void Gpu::on_vblank() {
    // Toggle even/odd interlace field bit 31
    gpustat ^= (1u << 31);
}

void Gpu::present_rgba8888(std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h) const {
    // Determine active PS1 horizontal resolution from GP1(08)
    int base_w = 320;
    if (disp_mode & 0x40u) {
        base_w = 368;
    } else {
        switch (disp_mode & 3u) {
        case 0u: base_w = 256; break;
        case 1u: base_w = 320; break;
        case 2u: base_w = 512; break;
        case 3u: base_w = 640; break;
        }
    }
    int base_h = (disp_mode & 0x04u) ? 480 : 240;
    const int v_lines = std::clamp(disp_v2 - disp_v1, 16, 256);
    if (!(disp_mode & 0x04u) && v_lines >= 200 && v_lines <= 256) {
        base_h = v_lines;
    }

    if (display_disabled) {
        out_w = base_w * scale;
        out_h = base_h * scale;
        out_rgba.assign(static_cast<std::size_t>(out_w * out_h), 0xFF000000u);
        return;
    }

    if (is_24bit) {
        // 24-bit color mode (used by MDEC FMVs & high-color title screens): read 3 bytes per pixel from 1x VRAM
        out_w = base_w;
        out_h = base_h;
        out_rgba.resize(static_cast<std::size_t>(out_w * out_h));
        const auto *vbytes = reinterpret_cast<const std::uint8_t *>(vram.data());
        for (int y = 0; y < base_h; ++y) {
            const int vy = (disp_vram_y + y) & 511;
            const std::uint8_t *scan = vbytes + vy * (kVramWidth * 2);
            const int x_base = (disp_vram_x * 2) & 2047;
            std::uint32_t *dst = out_rgba.data() + y * out_w;
            for (int x = 0; x < base_w; ++x) {
                const int b_off = (x_base + x * 3) & 2047;
                const std::uint32_t r = scan[b_off];
                const std::uint32_t g = scan[(b_off + 1) & 2047];
                const std::uint32_t b = scan[(b_off + 2) & 2047];
                dst[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
        return;
    }

    // 15-bit mode: try D3D11 hardware renderer first when active
    if (is_hw_renderer_active()) {
        if (hw_->read_display_rgba8888(vram.data(), disp_vram_x, disp_vram_y, base_w, base_h,
                                       out_rgba, out_w, out_h)) {
            return;
        }
    }

    // Software fallback: output from hires (1x..4x scaled RGBA8888 canvas)
    const int s = std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    out_w = base_w * s;
    out_h = base_h * s;
    out_rgba.resize(static_cast<std::size_t>(out_w * out_h));

    for (int py = 0; py < out_h; ++py) {
        const int hy = ((disp_vram_y * s) + py) % (kVramHeight * s);
        const std::uint32_t *hrow = hires.data() + hy * hw;
        std::uint32_t *dst = out_rgba.data() + py * out_w;
        for (int px = 0; px < out_w; ++px) {
            const int hx = ((disp_vram_x * s) + px) % hw;
            dst[px] = hrow[hx];
        }
    }
}

}  // namespace psx
