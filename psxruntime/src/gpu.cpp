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

void Gpu::set_disable_dithering(bool disable) noexcept {
    disable_dithering = disable;
    if (hw_) {
        hw_->set_disable_dithering(disable_dithering);
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
    cmd_pgxp_.clear();
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
    if (is_hw_renderer_active()) return;
    const std::uint32_t c32 = rgb555_to_rgba8888(pix);
    const int s = std::clamp(scale, 1, 4);
    const int hw = kVramWidth * s;
    for (int dy = 0; dy < s; ++dy) {
        std::uint32_t *hrow = hires.data() + (vy * s + dy) * hw + vx * s;
        for (int dx = 0; dx < s; ++dx) hrow[dx] = c32;
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

    const bool use_pgxp = pgxp_enabled && v0.has_pgxp && v1.has_pgxp && v2.has_pgxp &&
                          v0.fz >= 1.0f && v1.fz >= 1.0f && v2.fz >= 1.0f;
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
        const HwVertex hv0 = make_hv(v0);
        const HwVertex hv1 = make_hv(v1);
        const HwVertex hv2 = make_hv(v2);
        if (item_glow_strength > 0.01f && textured) {
            const float min_x = std::min({hv0.x, hv1.x, hv2.x});
            const float max_x = std::max({hv0.x, hv1.x, hv2.x});
            const float min_y = std::min({hv0.y, hv1.y, hv2.y});
            const float max_y = std::max({hv0.y, hv1.y, hv2.y});
            if (!semi_trans && use_pgxp) {
                // Track opaque 3D object clusters by (texpage, clut, buffer_half) so small 3D pickup
                // props (e.g. bread, keys, potions on tables/floors) get a clean radial lantern halo.
                const std::uint32_t key =
                    (static_cast<std::uint32_t>(texpage_x >> 6) & 0xFu) |
                    ((static_cast<std::uint32_t>(texpage_y >> 8) & 1u) << 4) |
                    ((static_cast<std::uint32_t>(clut_x >> 4) & 0x3Fu) << 5) |
                    ((static_cast<std::uint32_t>(clut_y) & 0x1FFu) << 11) |
                    ((clip_y1 > 200 ? 1u : 0u) << 20);
                MeshCluster *slot = nullptr;
                for (int ci = 0; ci < mesh_cluster_count_; ++ci) {
                    if (mesh_clusters_[ci].key == key) {
                        slot = &mesh_clusters_[ci];
                        break;
                    }
                }
                if (!slot && mesh_cluster_count_ < kMaxMeshClusters) {
                    slot = &mesh_clusters_[mesh_cluster_count_++];
                    *slot = MeshCluster{};
                    slot->key = key;
                    slot->clip_x1 = clip_x1;
                    slot->clip_y1 = clip_y1;
                    slot->clip_x2 = clip_x2;
                    slot->clip_y2 = clip_y2;
                }
                if (slot) {
                    slot->min_x = std::min(slot->min_x, min_x);
                    slot->min_y = std::min(slot->min_y, min_y);
                    slot->max_x = std::max(slot->max_x, max_x);
                    slot->max_y = std::max(slot->max_y, max_y);
                    ++slot->tri_count;
                }
            } else if (semi_trans && semi_mode == 1) {
                const float bw = max_x - min_x;
                const float bh = max_y - min_y;
                if (bw >= 2.0f && bw <= 28.0f && bh >= 2.0f && bh <= 28.0f) {
                    const float cx = (min_x + max_x) * 0.5f;
                    const float cy = (min_y + max_y) * 0.5f;
                    const float rad = std::clamp(std::max(bw, bh) * 1.35f, 8.0f, 34.0f);
                    const float str = std::clamp(item_glow_strength * 0.55f, 0.0f, 1.0f);
                    const float gr = std::min(1.0f, (static_cast<float>((item_glow_rgb >> 16) & 0xFFu) / 255.0f) * str);
                    const float gg = std::min(1.0f, (static_cast<float>((item_glow_rgb >> 8) & 0xFFu) / 255.0f) * str);
                    const float gb = std::min(1.0f, (static_cast<float>(item_glow_rgb & 0xFFu) / 255.0f) * str);
                    HwVertex center{};
                    center.x = cx; center.y = cy; center.z = std::min({hv0.z, hv1.z, hv2.z});
                    center.r = gr; center.g = gg; center.b = gb; center.flags = 4u;
                    static constexpr float kCos8[9] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f, 1.0f};
                    static constexpr float kSin8[9] = {0.0f, 0.7071f, 1.0f,  0.7071f,  0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f};
                    for (int seg = 0; seg < 8; ++seg) {
                        HwVertex p1{}, p2{};
                        p1.x = cx + kCos8[seg] * rad;     p1.y = cy + kSin8[seg] * rad;     p1.z = center.z; p1.flags = 4u;
                        p2.x = cx + kCos8[seg + 1] * rad; p2.y = cy + kSin8[seg + 1] * rad; p2.z = center.z; p2.flags = 4u;
                        hw_->draw_triangle(center, p1, p2, true, 1, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
                    }
                }
            }
        }
        hw_->draw_triangle(hv0, hv1, hv2,
                           semi_trans, semi_mode,
                           clip_x1, clip_y1, clip_x2, clip_y2,
                           vram.data());
    }

    if (use_hw) {
        // Fast 1x VRAM sync path when D3D11 owns the high-res canvas (eliminates per-pixel
        // division/modulo/std::floor while keeping 1x VRAM bit-exact for save states & readbacks).
        const float x0 = use_pgxp ? v0.fx : static_cast<float>(v0.x);
        const float y0 = use_pgxp ? v0.fy : static_cast<float>(v0.y);
        const float x1 = use_pgxp ? v1.fx : static_cast<float>(v1.x);
        const float y1 = use_pgxp ? v1.fy : static_cast<float>(v1.y);
        const float x2 = use_pgxp ? v2.fx : static_cast<float>(v2.x);
        const float y2 = use_pgxp ? v2.fy : static_cast<float>(v2.y);

        const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
        if (std::fabs(area) < 1e-4f) return;

        const int bx0 = std::max(clip_x1, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
        const int by0 = std::max(clip_y1, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
        const int bx1 = std::min(clip_x2, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
        const int by1 = std::min(clip_y2, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
        if (bx0 > bx1 || by0 > by1) return;

        const float inv_area = 1.0f / area;
        const std::uint16_t mask_or = force_set_mask ? 0x8000u : 0u;
        const float inv_z0 = use_pgxp ? (1.0f / std::max(1.0f, v0.fz)) : 1.0f;
        const float inv_z1 = use_pgxp ? (1.0f / std::max(1.0f, v1.fz)) : 1.0f;
        const float inv_z2 = use_pgxp ? (1.0f / std::max(1.0f, v2.fz)) : 1.0f;
        const float sign = (area > 0.0f) ? 1.0f : -1.0f;
        const float dw0_dx = (y1 - y2) * sign;
        const float dw1_dx = (y2 - y0) * sign;
        const float s_area = area * sign;
        const float s_inv_area = 1.0f / s_area;

        const int mask_x = static_cast<int>((tex_window & 0x1Fu) << 3);
        const int mask_y = static_cast<int>(((tex_window >> 5) & 0x1Fu) << 3);
        const int off_x = static_cast<int>(((tex_window >> 10) & 0x1Fu) << 3);
        const int off_y = static_cast<int>(((tex_window >> 15) & 0x1Fu) << 3);
        const std::uint16_t *const clut_row = vram.data() + (clut_y & 511) * kVramWidth;
        const std::uint16_t *const vram_ro = vram.data();

        for (int py = by0; py <= by1; ++py) {
            const float fpy = static_cast<float>(py);
            const float fpx0 = static_cast<float>(bx0);
            float w0 = ((x1 - fpx0) * (y2 - fpy) - (y1 - fpy) * (x2 - fpx0)) * sign;
            float w1 = ((x2 - fpx0) * (y0 - fpy) - (y2 - fpy) * (x0 - fpx0)) * sign;
            std::uint16_t *const dst_row = vram.data() + py * kVramWidth;

            for (int px = bx0; px <= bx1; ++px, w0 += dw0_dx, w1 += dw1_dx) {
                const float w2 = s_area - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                const std::uint16_t old_vram = dst_row[px];
                if (check_mask_before_draw && (old_vram & 0x8000u)) continue;

                const float b0 = w0 * s_inv_area;
                const float b1 = w1 * s_inv_area;
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
                    float u_f, v_f;
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
                    const float u_bias = u_f + 0.5f;
                    const float v_bias = v_f + 0.5f;
                    const int iu = static_cast<int>(u_bias) - (u_bias < static_cast<float>(static_cast<int>(u_bias)));
                    const int iv = static_cast<int>(v_bias) - (v_bias < static_cast<float>(static_cast<int>(v_bias)));
                    const int tu = ((iu & ~mask_x) | (off_x & mask_x)) & 255;
                    const int tv = ((iv & ~mask_y) | (off_y & mask_y)) & 255;
                    const int vy_t = (texpage_y + tv) & 511;
                    std::uint16_t texel = 0;
                    if (tex_depth == 0) {
                        const std::uint16_t word = vram_ro[vy_t * kVramWidth + ((texpage_x + (tu >> 2)) & 1023)];
                        const int nibble = (word >> ((tu & 3) * 4)) & 0xF;
                        texel = clut_row[(clut_x + nibble) & 1023];
                    } else if (tex_depth == 1) {
                        const std::uint16_t word = vram_ro[vy_t * kVramWidth + ((texpage_x + (tu >> 1)) & 1023)];
                        const int idx = (word >> ((tu & 1) * 8)) & 0xFF;
                        texel = clut_row[(clut_x + idx) & 1023];
                    } else {
                        texel = vram_ro[vy_t * kVramWidth + ((texpage_x + tu) & 1023)];
                    }
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
                if (apply_semi) {
                    out16 = blend_semi_transparent(old_vram, out16, semi_mode) | mask_or;
                }
                dst_row[px] = out16;
            }
        }
        (void)inv_area;
        ++primitives_drawn;
        return;
    }

    // Software mode: rasterize at full 1x..4x scale with PGXP.
    const int s = std::clamp(scale, 1, 4);
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
    const int eff_filter = use_hw ? 0 : ((filter_3d > 0) ? filter_3d : (sharp_3d ? 2 : 0));

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
    if (photo_mode && photo_hide_hud && (w < 300 || h < 200)) return;
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
        if (item_glow_strength > 0.01f && semi_trans && semi_mode == 1 && textured &&
            w >= 2 && w <= 28 && h >= 2 && h <= 28) {
            const float cx = static_cast<float>(v0.x) + static_cast<float>(w) * 0.5f;
            const float cy = static_cast<float>(v0.y) + static_cast<float>(h) * 0.5f;
            const float rad = std::clamp(std::max(static_cast<float>(w), static_cast<float>(h)) * 1.35f, 8.0f, 34.0f);
            const float str = std::clamp(item_glow_strength, 0.0f, 1.5f);
            const float gr = std::min(1.0f, (static_cast<float>((item_glow_rgb >> 16) & 0xFFu) / 255.0f) * str);
            const float gg = std::min(1.0f, (static_cast<float>((item_glow_rgb >> 8) & 0xFFu) / 255.0f) * str);
            const float gb = std::min(1.0f, (static_cast<float>(item_glow_rgb & 0xFFu) / 255.0f) * str);
            HwVertex center{};
            center.x = cx; center.y = cy; center.z = 1.0f;
            center.r = gr; center.g = gg; center.b = gb; center.flags = 4u;
            static constexpr float kCos8[9] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f, 1.0f};
            static constexpr float kSin8[9] = {0.0f, 0.7071f, 1.0f,  0.7071f,  0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f};
            for (int seg = 0; seg < 8; ++seg) {
                HwVertex p1{}, p2{};
                p1.x = cx + kCos8[seg] * rad;     p1.y = cy + kSin8[seg] * rad;     p1.z = 1.0f; p1.flags = 4u;
                p2.x = cx + kCos8[seg + 1] * rad; p2.y = cy + kSin8[seg + 1] * rad; p2.z = 1.0f; p2.flags = 4u;
                hw_->draw_triangle(center, p1, p2, true, 1, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
            }
        }
        hw_->draw_triangle(q00, q10, q01, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
        hw_->draw_triangle(q10, q11, q01, semi_trans, semi_mode, clip_x1, clip_y1, clip_x2, clip_y2, vram.data());
    }

    const int dy0 = std::max(0, clip_y1 - v0.y);
    const int dy1 = std::min(h - 1, clip_y2 - v0.y);
    const int dx0 = std::max(0, clip_x1 - v0.x);
    const int dx1 = std::min(w - 1, clip_x2 - v0.x);
    if (dy0 > dy1 || dx0 > dx1) {
        ++primitives_drawn;
        return;
    }

    if (use_hw && !textured && !semi_trans && !check_mask_before_draw) {
        const std::uint16_t solid = pack_bgr555(v0.r, v0.g, v0.b, mask_or);
        const int span = dx1 - dx0 + 1;
        for (int dy = dy0; dy <= dy1; ++dy) {
            std::fill_n(vram.data() + (v0.y + dy) * kVramWidth + (v0.x + dx0), span, solid);
        }
        ++primitives_drawn;
        return;
    }

    if (use_hw) {
        const int mask_x = static_cast<int>((tex_window & 0x1Fu) << 3);
        const int mask_y = static_cast<int>(((tex_window >> 5) & 0x1Fu) << 3);
        const int off_x = static_cast<int>(((tex_window >> 10) & 0x1Fu) << 3);
        const int off_y = static_cast<int>(((tex_window >> 15) & 0x1Fu) << 3);
        const std::uint16_t *const clut_row = vram.data() + (clut_y & 511) * kVramWidth;
        const std::uint16_t *const vram_ro = vram.data();

        for (int dy = dy0; dy <= dy1; ++dy) {
            const int vy = v0.y + dy;
            std::uint16_t *const dst_row = vram.data() + vy * kVramWidth;
            const int tv = (((v0.v + dy) & ~mask_y) | (off_y & mask_y)) & 255;
            const std::uint16_t *const src_row = vram_ro + ((texpage_y + tv) & 511) * kVramWidth;

            for (int dx = dx0; dx <= dx1; ++dx) {
                const int vx = v0.x + dx;
                const std::uint16_t old_vram = dst_row[vx];
                if (check_mask_before_draw && (old_vram & 0x8000u)) continue;

                int cr = v0.r, cg = v0.g, cb = v0.b;
                bool apply_semi = semi_trans;
                std::uint16_t tex_mask = 0;

                if (textured) {
                    const int tu = (((v0.u + dx) & ~mask_x) | (off_x & mask_x)) & 255;
                    std::uint16_t texel = 0;
                    if (tex_depth == 0) {
                        const std::uint16_t word = src_row[(texpage_x + (tu >> 2)) & 1023];
                        texel = clut_row[(clut_x + ((word >> ((tu & 3) * 4)) & 0xF)) & 1023];
                    } else if (tex_depth == 1) {
                        const std::uint16_t word = src_row[(texpage_x + (tu >> 1)) & 1023];
                        texel = clut_row[(clut_x + ((word >> ((tu & 1) * 8)) & 0xFF)) & 1023];
                    } else {
                        texel = src_row[(texpage_x + tu) & 1023];
                    }
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
                dst_row[vx] = out16;
            }
        }
        ++primitives_drawn;
        return;
    }

    for (int dy = dy0; dy <= dy1; ++dy) {
        const int vy = v0.y + dy;
        for (int dx = dx0; dx <= dx1; ++dx) {
            const int vx = v0.x + dx;

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

            const std::uint32_t fg32 = pack_rgba8888(cr, cg, cb);
            for (int sy = 0; sy < s; ++sy) {
                std::uint32_t *hrow = hires.data() + (vy * s + sy) * hw + vx * s;
                for (int sx = 0; sx < s; ++sx) {
                    hrow[sx] = apply_semi ? blend_semi_transparent32(hrow[sx], fg32, semi_mode) : fg32;
                }
            }
        }
    }
    ++primitives_drawn;
}

void Gpu::write_gp0_words(const std::uint32_t *words, std::uint32_t count) {
    if (!words || count == 0) return;
    if (phase_ == Phase::CpuToVram && is_hw_renderer_active() &&
        xfer_w_ > 0 && xfer_h_ > 0 && (xfer_w_ & 1) == 0 && (xfer_cur_x_ & 1) == 0 &&
        (xfer_x_ + xfer_w_) <= kVramWidth) {
        if (xfer_cur_x_ == 0 && xfer_cur_y_ == 0) {
            hw_->notify_vram_dirty(xfer_x_, xfer_y_, xfer_w_, xfer_h_);
        }
        const auto *src16 = reinterpret_cast<const std::uint16_t *>(words);
        std::uint32_t rem16 = count * 2u;
        while (rem16 > 0 && phase_ == Phase::CpuToVram) {
            const int row_rem = xfer_w_ - xfer_cur_x_;
            const int take = static_cast<int>(std::min<std::uint32_t>(rem16, static_cast<std::uint32_t>(row_rem)));
            const int vy = (xfer_y_ + xfer_cur_y_) & 511;
            std::memcpy(vram.data() + vy * kVramWidth + xfer_x_ + xfer_cur_x_,
                        src16, static_cast<std::size_t>(take) * sizeof(std::uint16_t));
            src16 += take;
            rem16 -= static_cast<std::uint32_t>(take);
            xfer_cur_x_ += take;
            if (xfer_cur_x_ >= xfer_w_) {
                xfer_cur_x_ = 0;
                if (++xfer_cur_y_ >= xfer_h_) {
                    hw_->blit_vram_rect_to_rt(vram.data(), xfer_x_, xfer_y_, xfer_w_, xfer_h_);
                    phase_ = Phase::Command;
                    cmd_buf_.clear();
                    cmd_pgxp_.clear();
                    break;
                }
            }
        }
        const std::uint32_t consumed_words = count - (rem16 >> 1);
        for (std::uint32_t i = consumed_words; i < count; ++i) {
            write_gp0(words[i]);
        }
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        write_gp0(words[i]);
    }
}

void Gpu::write_gp0(std::uint32_t word) {
    auto push_cmd_word = [&](std::uint32_t w) {
        cmd_buf_.push_back(w);
        pgxp::VertexData pv{};
        if (pgxp_enabled) {
            pgxp::lookup_gp0_word(w, pv);
        }
        cmd_pgxp_.push_back(pv);
    };

    if (phase_ == Phase::CpuToVram) {
        if (xfer_cur_x_ == 0 && xfer_cur_y_ == 0 && is_hw_renderer_active()) {
            hw_->notify_vram_dirty(xfer_x_, xfer_y_, xfer_w_, xfer_h_);
        }
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
                    cmd_pgxp_.clear();
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
            cmd_pgxp_.clear();
            return;
        }
        push_cmd_word(word);
        if (polyline_shaded_) polyline_expect_color_ = !polyline_expect_color_;
        return;
    }

    if (phase_ == Phase::Command) {
        cur_cmd_ = static_cast<std::uint8_t>(word >> 24);
        cmd_buf_.clear();
        cmd_pgxp_.clear();
        push_cmd_word(word);
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
            cmd_pgxp_.clear();
            return;
        }
        phase_ = Phase::Args;
        return;
    }

    // Phase::Args
    push_cmd_word(word);
    if (static_cast<int>(cmd_buf_.size()) >= args_needed_) {
        execute_gp0_packet();
        if (phase_ != Phase::CpuToVram) {
            phase_ = Phase::Command;
            cmd_buf_.clear();
            cmd_pgxp_.clear();
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
        if (is_hw_renderer_active()) {
            hw_->notify_vram_dirty(x0, y0, w, h);
            if (x0 + w <= kVramWidth) {
                for (int dy = 0; dy < h; ++dy) {
                    std::fill_n(vram.data() + ((y0 + dy) & 511) * kVramWidth + x0, w, fill_pix);
                }
            } else {
                const int part1 = kVramWidth - x0;
                const int part2 = w - part1;
                for (int dy = 0; dy < h; ++dy) {
                    std::uint16_t *row = vram.data() + ((y0 + dy) & 511) * kVramWidth;
                    std::fill_n(row + x0, part1, fill_pix);
                    std::fill_n(row, std::min(part2, kVramWidth), fill_pix);
                }
            }
            hw_->blit_vram_rect_to_rt(vram.data(), x0, y0, w, h);
            return;
        }
        for (int dy = 0; dy < h; ++dy) {
            for (int dx = 0; dx < w; ++dx) {
                put_vram_pixel(x0 + dx, y0 + dy, fill_pix);
            }
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
            const std::size_t xy_idx = idx++;
            const std::uint32_t xy = cmd_buf_[xy_idx];
            const int raw_x = sign_extend11(xy & 0xFFFFu);
            const int raw_y = sign_extend11(xy >> 16);
            v[i].x = raw_x + offset_x;
            v[i].y = raw_y + offset_y;

            const pgxp::VertexData pv = (xy_idx < cmd_pgxp_.size()) ? cmd_pgxp_[xy_idx] : pgxp::VertexData{};
            if (pgxp_enabled && pv.fz >= 1.0f && pv.raw_xy == xy) {
                v[i].fx = pv.fx + static_cast<float>(offset_x);
                v[i].fy = pv.fy + static_cast<float>(offset_y);
                v[i].fz = pv.fz;
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

        // Strict polygon-wide PGXP consistency: all vertices of a triangle or quad
        // must have valid PGXP data so we never mix fz=1.0 with fz>>1 or tear quad diagonals.
        bool poly_pgxp = pgxp_enabled;
        for (int i = 0; i < nverts; ++i) {
            if (!v[i].has_pgxp) {
                poly_pgxp = false;
                break;
            }
        }
        if (!poly_pgxp) {
            for (int i = 0; i < nverts; ++i) {
                v[i].has_pgxp = false;
                v[i].fx = static_cast<float>(v[i].x);
                v[i].fy = static_cast<float>(v[i].y);
                v[i].fz = 1.0f;
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
        if (is_hw_renderer_active()) {
            hw_->notify_vram_dirty(dx, dy, w, h);
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
        cmd_pgxp_.clear();
        break;

    case 0x01u:  // Reset command buffer
        phase_ = Phase::Command;
        cmd_buf_.clear();
        cmd_pgxp_.clear();
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
    bool read_ok = false;
    if (is_hw_renderer_active()) {
        flush_item_glow_halos();
        read_ok = hw_->read_display_rgba8888(vram.data(), disp_vram_x, disp_vram_y, base_w, base_h,
                                             out_rgba, out_w, out_h);
    }

    if (!read_ok) {
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

    // Wave D: Koudelka Post-Processing Parity (Photo Mode Free-Cam + CAS Sharpening + Gothic Noir / Crisp Classic / Vibrant HD + Vignette)
    const bool has_photo = photo_mode &&
                           (std::abs(photo_pan_x) > 1e-4f || std::abs(photo_pan_y) > 1e-4f ||
                            std::abs(photo_zoom - 1.0f) > 1e-4f || std::abs(photo_roll_deg) > 1e-3f);
    if (out_w > 2 && out_h > 2 && (has_photo || cas_sharpening || visual_preset != 0 || vignette > 0.001f)) {
        if (has_photo) {
            const std::vector<std::uint32_t> src_img = out_rgba;
            const float aspect = std::max(static_cast<float>(out_w) / static_cast<float>(out_h), 0.1f);
            const float zoom = std::max(photo_zoom, 0.25f);
            const float roll_rad = photo_roll_deg * (3.1415926535f / 180.0f);
            const float c = std::cos(roll_rad), s = std::sin(roll_rad);
            for (int y = 0; y < out_h; ++y) {
                const float v0 = (static_cast<float>(y) + 0.5f) / static_cast<float>(out_h);
                for (int x = 0; x < out_w; ++x) {
                    const float u0 = (static_cast<float>(x) + 0.5f) / static_cast<float>(out_w);
                    float dx = (u0 - 0.5f) / zoom;
                    float dy = (v0 - 0.5f) / zoom;
                    if (std::abs(roll_rad) > 1e-4f) {
                        const float ax = dx * aspect;
                        dx = (ax * c - dy * s) / aspect;
                        dy = ax * s + dy * c;
                    }
                    const float su = dx + (0.5f - photo_pan_x);
                    const float sv = dy + (0.5f - photo_pan_y);
                    if (su < 0.0f || su > 1.0f || sv < 0.0f || sv > 1.0f) {
                        out_rgba[static_cast<std::size_t>(y) * out_w + x] = 0xFF000000u;
                        continue;
                    }
                    const float fx = su * static_cast<float>(out_w) - 0.5f;
                    const float fy = sv * static_cast<float>(out_h) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, out_w - 1);
                    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, out_h - 1);
                    const int x1 = std::min(x0 + 1, out_w - 1);
                    const int y1 = std::min(y0 + 1, out_h - 1);
                    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
                    const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
                    const std::uint32_t p00 = src_img[static_cast<std::size_t>(y0) * out_w + x0];
                    const std::uint32_t p10 = src_img[static_cast<std::size_t>(y0) * out_w + x1];
                    const std::uint32_t p01 = src_img[static_cast<std::size_t>(y1) * out_w + x0];
                    const std::uint32_t p11 = src_img[static_cast<std::size_t>(y1) * out_w + x1];
                    const auto bilerp = [&](int shift) -> std::uint32_t {
                        const float c00 = static_cast<float>((p00 >> shift) & 0xFFu);
                        const float c10 = static_cast<float>((p10 >> shift) & 0xFFu);
                        const float c01 = static_cast<float>((p01 >> shift) & 0xFFu);
                        const float c11 = static_cast<float>((p11 >> shift) & 0xFFu);
                        const float top = c00 + (c10 - c00) * tx;
                        const float bot = c01 + (c11 - c01) * tx;
                        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(top + (bot - top) * ty + 0.5f), 0, 255));
                    };
                    out_rgba[static_cast<std::size_t>(y) * out_w + x] =
                        bilerp(0) | (bilerp(8) << 8) | (bilerp(16) << 16) | 0xFF000000u;
                }
            }
        }
        if (cas_sharpening) {
            const std::vector<std::uint32_t> src_img = out_rgba;
            for (int y = 1; y < out_h - 1; ++y) {
                for (int x = 1; x < out_w - 1; ++x) {
                    const auto unp = [&](int px, int py, float &r, float &g, float &b) {
                        const std::uint32_t p = src_img[static_cast<std::size_t>(py) * out_w + px];
                        r = static_cast<float>(p & 0xFFu);
                        g = static_cast<float>((p >> 8) & 0xFFu);
                        b = static_cast<float>((p >> 16) & 0xFFu);
                    };
                    float cr, cg, cb, nr, ng, nb, sr, sg, sb, wr, wg, wb, er, eg, eb;
                    unp(x, y, cr, cg, cb);
                    unp(x, y - 1, nr, ng, nb);
                    unp(x, y + 1, sr, sg, sb);
                    unp(x - 1, y, wr, wg, wb);
                    unp(x + 1, y, er, eg, eb);
                    const float ar = (nr + sr + wr + er) * 0.25f;
                    const float ag = (ng + sg + wg + eg) * 0.25f;
                    const float ab = (nb + sb + wb + eb) * 0.25f;
                    const float k = 0.22f;
                    const int rr = std::clamp(static_cast<int>(cr + (cr - ar) * k + 0.5f), 0, 255);
                    const int rg = std::clamp(static_cast<int>(cg + (cg - ag) * k + 0.5f), 0, 255);
                    const int rb = std::clamp(static_cast<int>(cb + (cb - ab) * k + 0.5f), 0, 255);
                    out_rgba[static_cast<std::size_t>(y) * out_w + x] =
                        static_cast<std::uint32_t>(rr) | (static_cast<std::uint32_t>(rg) << 8) |
                        (static_cast<std::size_t>(rb) << 16) | 0xFF000000u;
                }
            }
        }
        if (visual_preset != 0 || vignette > 0.001f) {
            float contrast = 1.0f, sat = 1.0f, bright = 0.0f, vig = vignette;
            if (visual_preset == 1) {         // Gothic Noir
                contrast = 1.08f; sat = 0.92f; bright = -0.01f; vig = std::max(vig, 0.25f);
            } else if (visual_preset == 2) {  // Crisp Classic
                contrast = 1.04f; sat = 1.02f; bright = 0.0f;
            } else if (visual_preset == 3) {  // Vibrant HD
                contrast = 1.10f; sat = 1.22f; bright = 0.01f;
            }
            for (int y = 0; y < out_h; ++y) {
                const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(out_h) - 0.5f;
                for (int x = 0; x < out_w; ++x) {
                    const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(out_w) - 0.5f;
                    std::uint32_t p = out_rgba[static_cast<std::size_t>(y) * out_w + x];
                    float r = static_cast<float>(p & 0xFFu) / 255.0f;
                    float g = static_cast<float>((p >> 8) & 0xFFu) / 255.0f;
                    float b = static_cast<float>((p >> 16) & 0xFFu) / 255.0f;
                    r = std::clamp((r - 0.5f) * contrast + 0.5f + bright, 0.0f, 1.0f);
                    g = std::clamp((g - 0.5f) * contrast + 0.5f + bright, 0.0f, 1.0f);
                    b = std::clamp((b - 0.5f) * contrast + 0.5f + bright, 0.0f, 1.0f);
                    const float lum = r * 0.299f + g * 0.587f + b * 0.114f;
                    r = std::clamp(lum + (r - lum) * sat, 0.0f, 1.0f);
                    g = std::clamp(lum + (g - lum) * sat, 0.0f, 1.0f);
                    b = std::clamp(lum + (b - lum) * sat, 0.0f, 1.0f);
                    if (vig > 0.001f) {
                        const float d2 = (nx * nx + ny * ny) * 2.2f;
                        const float t = std::clamp((d2 - 0.22f) / (0.78f - 0.22f), 0.0f, 1.0f);
                        const float vmul = 1.0f - vig * 0.72f * (t * t * (3.0f - 2.0f * t));
                        r *= vmul; g *= vmul; b *= vmul;
                    }
                    const std::uint32_t ir = static_cast<std::uint32_t>(std::clamp(static_cast<int>(r * 255.0f + 0.5f), 0, 255));
                    const std::uint32_t ig = static_cast<std::uint32_t>(std::clamp(static_cast<int>(g * 255.0f + 0.5f), 0, 255));
                    const std::uint32_t ib = static_cast<std::uint32_t>(std::clamp(static_cast<int>(b * 255.0f + 0.5f), 0, 255));
                    out_rgba[static_cast<std::size_t>(y) * out_w + x] = ir | (ig << 8) | (ib << 16) | 0xFF000000u;
                }
            }
        }
    }
}

bool Gpu::init_direct_present(void *hwnd, int win_w, int win_h) {
    if (!hw_) return false;
    if (!hw_->is_available() && !hw_->init(scale)) return false;
    return hw_->init_swapchain(hwnd, win_w, win_h);
}

bool Gpu::direct_present_active() const noexcept {
    return hw_ && hw_->swapchain_active();
}

bool Gpu::present_direct(int dst_x, int dst_y, int dst_w, int dst_h, int win_w, int win_h,
                         bool linear_filter, float brightness, float contrast, float scanlines,
                         const std::uint32_t *ui_rgba, int ui_w, int ui_h, int ui_dst_h, bool ui_dirty) const {
    if (!direct_present_active()) return false;

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

    GpuHwBackend::PresentParams p{};
    p.disp_x = disp_vram_x;
    p.disp_y = disp_vram_y;
    p.base_w = base_w;
    p.base_h = base_h;
    p.dst_x = dst_x;
    p.dst_y = dst_y;
    p.dst_w = dst_w;
    p.dst_h = dst_h;
    p.win_w = win_w;
    p.win_h = win_h;
    p.linear_filter = linear_filter;
    p.brightness = brightness;
    p.contrast = contrast;
    p.scanlines = scanlines;
    p.visual_preset = visual_preset;
    p.cas_sharpening = cas_sharpening;
    p.fsr_upscale = fsr_upscale;
    p.display_disabled = display_disabled;

    const bool has_photo = photo_mode &&
                           (std::abs(photo_pan_x) > 1e-4f || std::abs(photo_pan_y) > 1e-4f ||
                            std::abs(photo_zoom - 1.0f) > 1e-4f || std::abs(photo_roll_deg) > 1e-3f);

    // Zero-readback GPU fast-path: 15-bit HW rendering without Photo Mode free-cam!
    if (!is_24bit && !has_photo && is_hw_renderer_active()) {
        flush_item_glow_halos();
        return hw_->present_direct(vram.data(), p, ui_rgba, ui_w, ui_h, ui_dst_h, ui_dirty);
    }

    // 24-bit MDEC FMVs, Photo Mode free-cam, or Software CPU renderer: upload RGBA straight into the same swapchain
    thread_local std::vector<std::uint32_t> sw_buf;
    int fw = 0, fh = 0;
    present_rgba8888(sw_buf, fw, fh);
    // present_rgba8888 already applied visual_preset & CAS when !is_24bit; don't double-apply on GPU
    if (!is_24bit) {
        p.visual_preset = 0;
        p.cas_sharpening = false;
    }
    return hw_->present_sw_direct(sw_buf.data(), fw, fh, p, ui_rgba, ui_w, ui_h, ui_dst_h, ui_dirty);
}

void Gpu::flush_item_glow_halos() const {
    if (mesh_cluster_count_ <= 0) return;
    if (is_hw_renderer_active() && item_glow_strength > 0.01f) {
        static constexpr float kCos12[13] = {
            1.0f, 0.866025f, 0.5f, 0.0f, -0.5f, -0.866025f,
            -1.0f, -0.866025f, -0.5f, 0.0f, 0.5f, 0.866025f, 1.0f
        };
        static constexpr float kSin12[13] = {
            0.0f, 0.5f, 0.866025f, 1.0f, 0.866025f, 0.5f,
            0.0f, -0.5f, -0.866025f, -1.0f, -0.866025f, -0.5f, 0.0f
        };
        const float str = std::clamp(item_glow_strength, 0.0f, 1.5f);
        const float base_r = static_cast<float>((item_glow_rgb >> 16) & 0xFFu) / 255.0f;
        const float base_g = static_cast<float>((item_glow_rgb >> 8) & 0xFFu) / 255.0f;
        const float base_b = static_cast<float>(item_glow_rgb & 0xFFu) / 255.0f;
        for (int i = 0; i < mesh_cluster_count_; ++i) {
            const MeshCluster &c = mesh_clusters_[i];
            if (c.tri_count < 4 || c.tri_count > 140) continue;
            const float bw = c.max_x - c.min_x;
            const float bh = c.max_y - c.min_y;
            if (bw < 3.0f || bw > 46.0f || bh < 3.0f || bh > 46.0f) continue;

            const float cx = (c.min_x + c.max_x) * 0.5f;
            const float cy = (c.min_y + c.max_y) * 0.5f;
            const float outer_r = std::clamp(std::max(bw, bh) * 1.55f, 13.0f, 34.0f);
            const float inner_r = outer_r * 0.45f;

            // Layer 1: Wide soft radial aura (additive blend fading smoothly to black at outer edge)
            HwVertex outer_c{};
            outer_c.x = cx; outer_c.y = cy; outer_c.z = 1.0f;
            outer_c.r = std::min(1.0f, base_r * str * 0.65f);
            outer_c.g = std::min(1.0f, base_g * str * 0.65f);
            outer_c.b = std::min(1.0f, base_b * str * 0.65f);
            outer_c.flags = 4u;
            for (int seg = 0; seg < 12; ++seg) {
                HwVertex p1{}, p2{};
                p1.x = cx + kCos12[seg] * outer_r;     p1.y = cy + kSin12[seg] * outer_r;     p1.z = 1.0f; p1.flags = 4u;
                p2.x = cx + kCos12[seg + 1] * outer_r; p2.y = cy + kSin12[seg + 1] * outer_r; p2.z = 1.0f; p2.flags = 4u;
                hw_->draw_triangle(outer_c, p1, p2, true, 1, c.clip_x1, c.clip_y1, c.clip_x2, c.clip_y2, vram.data());
            }

            // Layer 2: Warm core shimmer directly over the pickup item
            HwVertex inner_c{};
            inner_c.x = cx; inner_c.y = cy; inner_c.z = 1.0f;
            inner_c.r = std::min(1.0f, (base_r * 0.85f + 0.15f) * str * 0.55f);
            inner_c.g = std::min(1.0f, (base_g * 0.85f + 0.15f) * str * 0.55f);
            inner_c.b = std::min(1.0f, (base_b * 0.85f + 0.15f) * str * 0.55f);
            inner_c.flags = 4u;
            for (int seg = 0; seg < 12; ++seg) {
                HwVertex p1{}, p2{};
                p1.x = cx + kCos12[seg] * inner_r;     p1.y = cy + kSin12[seg] * inner_r;     p1.z = 1.0f; p1.flags = 4u;
                p2.x = cx + kCos12[seg + 1] * inner_r; p2.y = cy + kSin12[seg + 1] * inner_r; p2.z = 1.0f; p2.flags = 4u;
                hw_->draw_triangle(inner_c, p1, p2, true, 1, c.clip_x1, c.clip_y1, c.clip_x2, c.clip_y2, vram.data());
            }
        }
    }
    mesh_cluster_count_ = 0;
}

}  // namespace psx
