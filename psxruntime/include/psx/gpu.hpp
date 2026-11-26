#pragma once

#include "psx/model_swap.hpp"
#include "psx/pgxp.hpp"
#include "psx/system.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace psx {

class GpuHwBackend;

// PlayStation 1 Graphics Processing Unit (GP0 / GP1).
// Owns 1024x512 16-bit VRAM plus a 1x..4x scaled RGBA8888 software canvas and an
// optional Direct3D 11 1x..8x hardware renderer with PGXP subpixel precision and
// perspective-correct texture filtering.
class Gpu final : public GpuSink {
public:
    static constexpr int kVramWidth = 1024;
    static constexpr int kVramHeight = 512;

    std::vector<std::uint16_t> vram;     // 1024 x 512 16-bit RGB5551 VRAM
    std::vector<std::uint32_t> hires;    // (1024*scale) x (512*scale) RGBA8888 software canvas
    int scale = 2;                       // internal resolution multiplier (1..4 software, 1..8 D3D11 hardware)
    bool sharp_3d = false;               // legacy toggle (maps to filter_3d = 2)
    int filter_3d = 0;                   // 0 = Nearest, 1 = Bilinear, 2 = Sharp-3D
    bool pgxp_enabled = true;            // PGXP GTE->GPU subpixel precision + perspective-correct UVs
    bool hw_renderer = true;             // Direct3D 11 hardware rasterizer (falls back to software if false/unavailable)
    // Wave D: Visual Suite Parity & Photo Mode
    bool disable_dithering = true;       // true = 24-Bit Smooth De-Dither, false = Authentic PS1 4x4 Bayer Dither
    int visual_preset = 0;               // 0 = Raw PS1, 1 = Gothic Noir, 2 = Crisp Classic, 3 = Vibrant HD, 4 = 1898 Candlelight
    bool cas_sharpening = false;         // Contrast-Adaptive Sharpening post-pass
    bool fsr_upscale = false;            // Optional AMD FSR 1.0 (EASU) edge-directed spatial upscaling (D3D11)
    bool vsync = false;                  // D3D11 VSync (false = ALLOW_TEARING unlocked FPS, safe on 30Hz TVs)
    float bloom = 0.0f;                  // 0.0 .. 1.0 Gothic Candlelight HDR Bloom (D3D11)
    bool fxaa = false;                   // Fast edge anti-aliasing (D3D11)
    float item_glow_strength = 0.0f;         // Mod-driven item glow base strength (0 = off, up to 1.5)
    std::uint32_t item_glow_rgb = 0xFFCC44u; // Mod-driven glow tint (0xRRGGBB)
    float item_glow_pulse_speed = 1.0f;      // Mod-driven breathing & sweep speed (0.0 = steady, 1.0 = default)
    float vignette = 0.0f;               // 0.0 .. 1.0 subtle Gothic edge vignette
    bool photo_mode = false;             // F1 / F4 Photo Mode active
    bool photo_hide_hud = false;         // Hide 2D HUD / dialogue / battle menu sprites in Photo Mode
    float photo_pan_x = 0.0f;            // -0.45 .. +0.45 UV pan
    float photo_pan_y = 0.0f;            // -0.45 .. +0.45 UV pan
    float photo_zoom = 1.0f;             //  0.50 ..  3.00 FOV / zoom
    float photo_roll_deg = 0.0f;         // -45.0 .. +45.0 Dutch tilt in degrees

    // Drawing environment registers (GP0 E1..E6)
    std::uint32_t draw_mode = 0;         // GP0(E1)
    std::uint32_t tex_window = 0;        // GP0(E2)
    int clip_x1 = 0, clip_y1 = 0;        // GP0(E3)
    int clip_x2 = 1023, clip_y2 = 511;   // GP0(E4)
    int offset_x = 0, offset_y = 0;      // GP0(E5)
    bool force_set_mask = false;         // GP0(E6) bit 0
    bool check_mask_before_draw = false; // GP0(E6) bit 1

    // Display control registers (GP1 00..08)
    std::uint32_t gpustat = 0x14802000u;
    int disp_vram_x = 0;
    int disp_vram_y = 0;
    int disp_h1 = 0x200, disp_h2 = 0xC00;
    int disp_v1 = 0x010, disp_v2 = 0x100;
    std::uint32_t disp_mode = 0;
    bool display_disabled = false;
    bool is_24bit = false;
    std::uint32_t gpu_info_latch = 2u;

    // Telemetry counters
    std::uint64_t primitives_drawn = 0;
    std::uint64_t vram_uploads = 0;
    std::uint64_t pgxp_hits = 0;
    std::uint64_t pgxp_misses = 0;

    Gpu();
    ~Gpu() override;

    void reset() override;
    void set_scale(int new_scale);
    void set_pgxp_enabled(bool en) noexcept;
    void set_hw_renderer(bool en);
    void set_filter_3d(int mode) noexcept;
    void set_disable_dithering(bool disable) noexcept;
    bool is_hw_renderer_active() const noexcept;

    void write_gp0(std::uint32_t word) override;
    bool in_cpu_to_vram() const noexcept override { return phase_ == Phase::CpuToVram; }
    void write_gp0_words(const std::uint32_t *words, std::uint32_t count) override;
    void write_gp1(std::uint32_t word) override;
    std::uint32_t read_data() override;
    std::uint32_t read_stat() const override;
    void on_vblank() override;

    // Extract current display frame as packed RGBA8888 (width = disp_w * scale, height = disp_h * scale).
    void present_rgba8888(std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h) const;

    // Zero-readback direct D3D11 swapchain presentation (skips GPU->CPU->GPU round-trip in live gameplay)
    bool init_direct_present(void *hwnd, int win_w, int win_h);
    bool direct_present_active() const noexcept;
    bool present_direct(int dst_x, int dst_y, int dst_w, int dst_h, int win_w, int win_h,
                        bool linear_filter, float brightness, float contrast, float scanlines,
                        const std::uint32_t *ui_rgba, int ui_w, int ui_h, int ui_dst_h, bool ui_dirty) const;

    // Rebuild hires canvas (and D3D11 RT) from current 1x VRAM (used after scale change or save-state load).
    void sync_hires_from_vram();

    static inline std::uint32_t rgb555_to_rgba8888(std::uint16_t p) noexcept {
        const std::uint32_t r5 = p & 31u;
        const std::uint32_t g5 = (p >> 5) & 31u;
        const std::uint32_t b5 = (p >> 10) & 31u;
        const std::uint32_t r8 = (r5 << 3) | (r5 >> 2);
        const std::uint32_t g8 = (g5 << 3) | (g5 >> 2);
        const std::uint32_t b8 = (b5 << 3) | (b5 >> 2);
        return r8 | (g8 << 8) | (b8 << 16) | 0xFF000000u;
    }

    // Layer 1d: High-Poly Skinned .glb Model Swap (pure client-side D3D11 visual replacement)
    const model_swap::GlbModel *model_swap_koudelka = nullptr;
    model_swap::ModelDrawParams model_swap_params{};
    bool model_swap_hide_original = true;

private:
    enum class Phase : std::uint8_t {
        Command,
        Args,
        PolyLine,
        CpuToVram,
    };

    struct Vertex {
        int x = 0, y = 0;
        float fx = 0.0f, fy = 0.0f, fz = 1.0f;
        bool has_pgxp = false;
        std::uint16_t gte_slot = 0xFFFFu;
        std::uint8_t r = 128, g = 128, b = 128;
        int u = 0, v = 0;
    };

    Phase phase_ = Phase::Command;
    std::uint8_t cur_cmd_ = 0;
    int args_needed_ = 0;
    bool polyline_shaded_ = false;
    bool polyline_expect_color_ = false;
    std::vector<std::uint32_t> cmd_buf_;
    std::vector<pgxp::VertexData> cmd_pgxp_;

    // CPU <-> VRAM transfer state
    int xfer_x_ = 0, xfer_y_ = 0, xfer_w_ = 0, xfer_h_ = 0;
    int xfer_cur_x_ = 0, xfer_cur_y_ = 0;
    std::vector<std::uint16_t> read_fifo_;
    std::size_t read_fifo_pos_ = 0;

    mutable std::unique_ptr<GpuHwBackend> hw_;

    struct GlowTri {
        float x[3], y[3], z[3], u[3], v[3];
        std::uint32_t texpage_x = 0, texpage_y = 0, clut_x = 0, clut_y = 0;
        std::uint32_t tex_depth = 0, tex_window = 0;
    };
    struct MeshCluster {
        std::uint32_t key = 0;
        float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
        int tri_count = 0;
    };
    struct ValidGlowCluster {
        std::uint32_t key = 0;
        float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    };
    static constexpr int kMaxMeshClusters = 32;
    static constexpr int kMaxPendingGlowTris = 160;
    mutable MeshCluster mesh_clusters_[kMaxMeshClusters]{};
    mutable int mesh_cluster_count_ = 0;
    mutable ValidGlowCluster prev_valid_clusters_[kMaxMeshClusters]{};
    mutable int prev_valid_cluster_count_ = 0;
    mutable GlowTri pending_glow_tris_[kMaxPendingGlowTris]{};
    mutable int pending_glow_count_ = 0;
    mutable int pending_glow_cluster_idx_ = -1;
    mutable int pending_glow_clip_x1_ = 0, pending_glow_clip_y1_ = 0;
    mutable int pending_glow_clip_x2_ = 1023, pending_glow_clip_y2_ = 511;
    mutable bool glow_frame_dirty_ = false;
    mutable float glow_anim_phase_ = 0.0f;
    // Per-frame tracked 3D bounds & live GTE bone slots of Koudelka's PS1 character mesh
    mutable float ms_min_x_ = 1e9f, ms_max_x_ = -1e9f;
    mutable float ms_min_y_ = 1e9f, ms_max_y_ = -1e9f;
    mutable float ms_z_sum_ = 0.0f;
    mutable float ms_r_sum_ = 0.0f, ms_g_sum_ = 0.0f, ms_b_sum_ = 0.0f;
    mutable int ms_tri_count_ = 0;
    mutable int ms_clip_x1_ = 0, ms_clip_y1_ = 0, ms_clip_x2_ = 320, ms_clip_y2_ = 240;
    mutable int ms_off_x_ = 0, ms_off_y_ = 0;
    mutable std::uint16_t ms_gte_slots_[32]{};
    mutable int ms_gte_slot_count_ = 0;
    mutable bool ms_flushed_this_frame_ = false;
    // Last valid 16-bone PS1 skeleton (used if 1 frame clips a limb)
    mutable bool ms_have_last_skel_ = false;
    mutable std::array<model_swap::Ps1BonePose, 16> ms_last_skel_{};
    mutable float ms_last_cam_h_ = 246.0f, ms_last_cam_ofx_ = 160.0f, ms_last_cam_ofy_ = 120.0f;
    // Battle enemy cluster centroid (used so Koudelka faces the enemy in battle!)
    mutable float ms_enemy_x_sum_ = 0.0f, ms_enemy_z_sum_ = 0.0f;
    mutable int ms_enemy_tri_count_ = 0;
    // Temporal smoothing & live camera-relative motion tracking across frames
    mutable bool ms_have_prev_ = false;
    mutable bool ms_prev_in_battle_ = false;
    mutable float ms_prev_rel_cx_ = 0.0f, ms_prev_rel_feet_y_ = 0.0f, ms_prev_z_ = 0.0f;
    mutable float ms_smooth_k_ = 0.0f;      // smoothed perspective constant (height * depth_z)
    mutable float ms_smooth_bh_ = 0.0f;     // smoothed screen height (prevents grow/shrink jitter)
    mutable float ms_vel_x_ = 0.0f, ms_vel_z_ = 0.0f;
    mutable float ms_live_yaw_ = 0.0f;
    mutable float ms_live_speed_ = 0.0f;
    mutable float ms_idle_blend_ = 1.0f;
    mutable float ms_stride_phase_ = 0.0f;
    void flush_model_swap() const;
    void flush_pending_item_glow() const;
    void end_item_glow_frame() const;

    static int command_arg_count(std::uint8_t cmd) noexcept;
    void execute_gp0_packet();
    void put_vram_pixel(int x, int y, std::uint16_t pix);

    std::uint16_t sample_texture(int u, int v, int texpage_x, int texpage_y,
                                 int depth, int clut_x, int clut_y) const noexcept;
    void sample_texture_filtered(float u_f, float v_f, int texpage_x, int texpage_y,
                                 int depth, int clut_x, int clut_y,
                                 int &out_r, int &out_g, int &out_b,
                                 std::uint16_t &out_center_texel) const noexcept;
    static std::uint16_t blend_semi_transparent(std::uint16_t bg, std::uint16_t fg, int mode) noexcept;
    static std::uint32_t blend_semi_transparent32(std::uint32_t bg, std::uint32_t fg, int mode) noexcept;

    void draw_triangle(const Vertex &v0, const Vertex &v1, const Vertex &v2,
                       bool shaded, bool textured, bool semi_trans, bool raw_tex,
                       int texpage_x, int texpage_y, int tex_depth, int semi_mode,
                       int clut_x, int clut_y);
    void draw_line_segment(const Vertex &v0, const Vertex &v1, bool shaded, bool semi_trans, int semi_mode);
    void draw_rectangle(const Vertex &v0, int w, int h, bool textured, bool semi_trans, bool raw_tex,
                        int clut_x, int clut_y);
};

}  // namespace psx
