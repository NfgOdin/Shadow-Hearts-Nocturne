#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace psx {

struct HwVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
    float r = 0.5f;
    float g = 0.5f;
    float b = 0.5f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t texpage_x = 0;
    std::uint32_t texpage_y = 0;
    std::uint32_t clut_x = 0;
    std::uint32_t clut_y = 0;
    std::uint32_t tex_depth = 0;
    std::uint32_t tex_window = 0;
    std::uint32_t flags = 0;  // bit0=textured, bit1=raw_tex, bit2=semi_trans, bit3=pass_only_semi, bit4=pass_only_opaque
};

class GpuHwBackend {
public:
    GpuHwBackend();
    ~GpuHwBackend();

    bool init(int scale);
    bool is_available() const noexcept;
    int scale() const noexcept;
    void set_scale(int new_scale, const std::uint16_t *vram_1x);
    void set_filter_mode(int mode) noexcept;  // 0=Nearest, 1=Bilinear, 2=Sharp-3D
    void set_disable_dithering(bool disable) noexcept;

    // Mark 1x VRAM dirty (will upload to GPU R16_UINT texture before next textured draw or blit)
    void notify_vram_dirty(int x, int y, int w, int h) noexcept;

    // Blit a rectangle from 1x CPU VRAM to the scaled GPU render target (for FillRect / LoadImage / CopyVram / state load)
    void blit_vram_rect_to_rt(const std::uint16_t *vram_1x, int x, int y, int w, int h);

    // Full sync of 1x VRAM to GPU render target
    void sync_full_vram(const std::uint16_t *vram_1x);

    // Queue a triangle on the hardware GPU
    void draw_triangle(const HwVertex &v0, const HwVertex &v1, const HwVertex &v2,
                       bool semi_trans, int semi_mode,
                       int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                       const std::uint16_t *vram_1x);

    // Flush queued draws and extract display region as packed RGBA8888 (width = base_w * scale, height = base_h * scale)
    bool read_display_rgba8888(const std::uint16_t *vram_1x,
                               int disp_x, int disp_y, int base_w, int base_h,
                               std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace psx
