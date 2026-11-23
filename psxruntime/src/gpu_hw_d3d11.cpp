#include "psx/gpu_hw.hpp"
#include "psx/gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

namespace psx {

#ifdef _WIN32
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char *kHlslSource = R"HLSL(
cbuffer Constants : register(b0) {
    int u_filter_mode;        // 0=Nearest, 1=Bilinear, 2=Sharp-3D
    int u_scale;
    int u_disable_dithering;  // 1=24-Bit Smooth De-Dither, 0=Authentic PS1 4x4 Bayer Dither
    int u_pad1;
};

Texture2D<uint> VramTex : register(t0);

struct VSIn {
    float3 pos        : POSITION;
    float3 color      : COLOR0;
    float2 uv         : TEXCOORD0;
    uint   texpage_x  : TEXCOORD1;
    uint   texpage_y  : TEXCOORD2;
    uint   clut_x     : TEXCOORD3;
    uint   clut_y     : TEXCOORD4;
    uint   tex_depth  : TEXCOORD5;
    uint   tex_window : TEXCOORD6;
    uint   flags      : TEXCOORD7;
};

struct PSIn {
    float4 pos                  : SV_Position;
    noperspective float3 color  : COLOR0;
    float2 uv                   : TEXCOORD0;
    nointerpolation uint4 tp_cl : TEXCOORD1; // texpage_x, texpage_y, clut_x, clut_y
    nointerpolation uint3 dw_fl : TEXCOORD2; // tex_depth, tex_window, flags
};

PSIn vs_main(VSIn v) {
    PSIn o;
    float w = max(v.pos.z, 1.0f);
    float ndc_x = (v.pos.x / 512.0f) - 1.0f;
    float ndc_y = 1.0f - (v.pos.y / 256.0f);
    o.pos = float4(ndc_x * w, ndc_y * w, 0.5f * w, w);
    o.color = v.color;
    o.uv = v.uv;
    o.tp_cl = uint4(v.texpage_x, v.texpage_y, v.clut_x, v.clut_y);
    o.dw_fl = uint3(v.tex_depth, v.tex_window, v.flags);
    return o;
}

uint sample_vram_texel(int u, int v, uint texpage_x, uint texpage_y,
                       uint depth, uint clut_x, uint clut_y, uint tex_window) {
    int mask_x = int((tex_window & 0x1Fu) << 3);
    int mask_y = int(((tex_window >> 5) & 0x1Fu) << 3);
    int off_x  = int(((tex_window >> 10) & 0x1Fu) << 3);
    int off_y  = int(((tex_window >> 15) & 0x1Fu) << 3);

    int tu = (u & ~mask_x) | (off_x & mask_x);
    int tv = (v & ~mask_y) | (off_y & mask_y);
    int vy = (int(texpage_y) + (tv & 255)) & 511;

    if (depth == 0u) {
        int vx = (int(texpage_x) + ((tu & 255) >> 2)) & 1023;
        uint word = VramTex.Load(int3(vx, vy, 0));
        uint nibble = (word >> ((uint(tu) & 3u) * 4u)) & 0xFu;
        int cx = (int(clut_x) + int(nibble)) & 1023;
        return VramTex.Load(int3(cx, int(clut_y) & 511, 0));
    }
    if (depth == 1u) {
        int vx = (int(texpage_x) + ((tu & 255) >> 1)) & 1023;
        uint word = VramTex.Load(int3(vx, vy, 0));
        uint idx = (word >> ((uint(tu) & 1u) * 8u)) & 0xFFu;
        int cx = (int(clut_x) + int(idx)) & 1023;
        return VramTex.Load(int3(cx, int(clut_y) & 511, 0));
    }
    int vx = (int(texpage_x) + (tu & 255)) & 1023;
    return VramTex.Load(int3(vx, vy, 0));
}

float3 unpack_rgb555(uint p) {
    uint r5 = p & 31u;
    uint g5 = (p >> 5u) & 31u;
    uint b5 = (p >> 10u) & 31u;
    uint r8 = (r5 << 3u) | (r5 >> 2u);
    uint g8 = (g5 << 3u) | (g5 >> 2u);
    uint b8 = (b5 << 3u) | (b5 >> 2u);
    return float3(r8, g8, b8) / 255.0f;
}

float4 ps_main(PSIn i) : SV_Target {
    uint flags = i.dw_fl.z;

    // Direct 1x VRAM blit quad (flag bit 8)
    if ((flags & 0x100u) != 0u) {
        int vx = int(floor(i.uv.x)) & 1023;
        int vy = int(floor(i.uv.y)) & 511;
        uint raw = VramTex.Load(int3(vx, vy, 0));
        return float4(unpack_rgb555(raw), 1.0f);
    }

    bool textured        = (flags & 1u) != 0u;
    bool raw_tex         = (flags & 2u) != 0u;
    bool semi_trans      = (flags & 4u) != 0u;
    bool pass_only_semi  = (flags & 8u) != 0u;
    bool pass_only_opaq  = (flags & 16u) != 0u;
    bool is_3d_poly      = (flags & 32u) != 0u;

    float3 col = i.color;
    bool tex_semi_bit = true;

    if (textured) {
        uint tp_x = i.tp_cl.x;
        uint tp_y = i.tp_cl.y;
        uint cl_x = i.tp_cl.z;
        uint cl_y = i.tp_cl.w;
        uint depth = i.dw_fl.x;
        uint tw    = i.dw_fl.y;

        if (is_3d_poly && u_filter_mode > 0) {
            float u_f = i.uv.x;
            float v_f = i.uv.y;
            if (u_filter_mode == 2) {
                // Smooth Hermite Sharp-3D bilinear
                float2 fl = floor(float2(u_f, v_f));
                float2 fr = frac(float2(u_f, v_f));
                float gain = min(max(float(u_scale), 2.0f), 3.0f);
                float2 s = clamp((fr - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
                s = s * s * (3.0f - 2.0f * s);
                u_f = fl.x + s.x;
                v_f = fl.y + s.y;
            }
            int u0 = int(floor(u_f));
            int v0 = int(floor(v_f));
            float fx = frac(u_f);
            float fy = frac(v_f);

            uint t00 = sample_vram_texel(u0,     v0,     tp_x, tp_y, depth, cl_x, cl_y, tw);
            uint t10 = sample_vram_texel(u0 + 1, v0,     tp_x, tp_y, depth, cl_x, cl_y, tw);
            uint t01 = sample_vram_texel(u0,     v0 + 1, tp_x, tp_y, depth, cl_x, cl_y, tw);
            uint t11 = sample_vram_texel(u0 + 1, v0 + 1, tp_x, tp_y, depth, cl_x, cl_y, tw);

            uint center = sample_vram_texel(int(floor(i.uv.x + 0.5f)), int(floor(i.uv.y + 0.5f)),
                                            tp_x, tp_y, depth, cl_x, cl_y, tw);
            if (center == 0u) discard;
            tex_semi_bit = (center & 0x8000u) != 0u;

            float w00 = (t00 != 0u) ? ((1.0f - fx) * (1.0f - fy)) : 0.0f;
            float w10 = (t10 != 0u) ? (fx * (1.0f - fy)) : 0.0f;
            float w01 = (t01 != 0u) ? ((1.0f - fx) * fy) : 0.0f;
            float w11 = (t11 != 0u) ? (fx * fy) : 0.0f;
            float wsum = w00 + w10 + w01 + w11;
            float3 tcol = (wsum > 0.001f)
                ? ((unpack_rgb555(t00) * w00 + unpack_rgb555(t10) * w10 +
                    unpack_rgb555(t01) * w01 + unpack_rgb555(t11) * w11) / wsum)
                : unpack_rgb555(center);

            if (raw_tex) {
                col = tcol;
            } else {
                col = clamp(tcol * (i.color * (255.0f / 128.0f)), 0.0f, 1.0f);
            }
        } else {
            float bias = is_3d_poly ? 0.5f : 0.0f;
            int tu = int(floor(i.uv.x + bias));
            int tv = int(floor(i.uv.y + bias));
            uint texel = sample_vram_texel(tu, tv, tp_x, tp_y, depth, cl_x, cl_y, tw);
            if (texel == 0u) discard;
            tex_semi_bit = (texel & 0x8000u) != 0u;
            float3 tcol = unpack_rgb555(texel);
            if (raw_tex) {
                col = tcol;
            } else {
                col = clamp(tcol * (i.color * (255.0f / 128.0f)), 0.0f, 1.0f);
            }
        }
    }

    if (pass_only_semi && !tex_semi_bit) discard;
    if (pass_only_opaq && tex_semi_bit) discard;

    // Optional authentic PS1 4x4 Bayer dither matrix + 5-bit quantization when u_disable_dithering == 0
    if (u_disable_dithering == 0 && is_3d_poly && !raw_tex) {
        static const int kDither[16] = {
            -4,  0, -3,  1,
             2, -2,  3, -1,
            -3,  1, -4,  0,
             3, -1,  2, -2
        };
        int dx = int(i.pos.x / max(u_scale, 1)) & 3;
        int dy = int(i.pos.y / max(u_scale, 1)) & 3;
        float d = float(kDither[dy * 4 + dx]) / 255.0f;
        col = floor(clamp(col + d, 0.0f, 1.0f) * 31.0f + 0.5f) / 31.0f;
    }

    bool apply_semi = semi_trans && (!textured || tex_semi_bit);
    return float4(col, apply_semi ? 0.5f : 1.0f);
}
)HLSL";

struct CbData {
    int filter_mode = 0;
    int scale = 2;
    int disable_dithering = 1;
    int pad1 = 0;
};

}  // namespace
#endif

struct GpuHwBackend::Impl {
    int scale = 2;
    int filter_mode = 0;
    bool disable_dithering = true;
    bool available = false;

#ifdef _WIN32
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vbo;
    ComPtr<ID3D11Buffer> cbo;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11BlendState> blend_opaque;
    ComPtr<ID3D11BlendState> blend_semi[4];

    ComPtr<ID3D11Texture2D> vram_tex;
    ComPtr<ID3D11ShaderResourceView> vram_srv;

    ComPtr<ID3D11Texture2D> rt_tex;
    ComPtr<ID3D11RenderTargetView> rt_rtv;
    ComPtr<ID3D11Texture2D> staging_tex[2];
    int staging_w = 0;
    int staging_h = 0;
    int staging_idx = 0;
    bool staging_primed = false;

    static constexpr std::size_t kMaxBatchVerts = 32768;
    std::vector<HwVertex> batch;
    int cur_blend_key = -1;  // -1=opaque, 0..3=semi_mode
    int cur_clip_x1 = 0, cur_clip_y1 = 0, cur_clip_x2 = 1023, cur_clip_y2 = 511;

    bool vram_dirty = true;
    int vram_dirty_x0 = 0, vram_dirty_y0 = 0;
    int vram_dirty_x1 = Gpu::kVramWidth, vram_dirty_y1 = Gpu::kVramHeight;
    bool batch_has_3d_textured = false;
    bool cbo_dirty = true;

    void mark_dirty_rect(int x, int y, int w, int h) noexcept {
        if (w <= 0 || h <= 0) {
            vram_dirty_x0 = 0; vram_dirty_y0 = 0;
            vram_dirty_x1 = Gpu::kVramWidth; vram_dirty_y1 = Gpu::kVramHeight;
            vram_dirty = true;
            return;
        }
        // PS1 VRAM wraps modulo 1024x512; if a rect wraps across an edge, mark full axis
        const int rx0 = x & (Gpu::kVramWidth - 1);
        const int ry0 = y & (Gpu::kVramHeight - 1);
        const int rx1 = (rx0 + w > Gpu::kVramWidth) ? Gpu::kVramWidth : (rx0 + w);
        const int ry1 = (ry0 + h > Gpu::kVramHeight) ? Gpu::kVramHeight : (ry0 + h);
        const int lx0 = (rx0 + w > Gpu::kVramWidth) ? 0 : rx0;
        const int ly0 = (ry0 + h > Gpu::kVramHeight) ? 0 : ry0;
        if (!vram_dirty) {
            vram_dirty_x0 = lx0; vram_dirty_y0 = ly0;
            vram_dirty_x1 = rx1; vram_dirty_y1 = ry1;
            vram_dirty = true;
        } else {
            if (lx0 < vram_dirty_x0) vram_dirty_x0 = lx0;
            if (ly0 < vram_dirty_y0) vram_dirty_y0 = ly0;
            if (rx1 > vram_dirty_x1) vram_dirty_x1 = rx1;
            if (ry1 > vram_dirty_y1) vram_dirty_y1 = ry1;
        }
    }
    const std::uint16_t *pending_vram_ptr = nullptr;
    int dirty_x1 = 0, dirty_y1 = 0, dirty_x2 = 1024, dirty_y2 = 512;

    bool create_render_target(int new_scale) {
        scale = std::clamp(new_scale, 1, 8);
        const int rt_w = Gpu::kVramWidth * scale;
        const int rt_h = Gpu::kVramHeight * scale;

        rt_rtv.Reset();
        rt_tex.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(rt_w);
        td.Height = static_cast<UINT>(rt_h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &rt_tex))) return false;
        if (FAILED(device->CreateRenderTargetView(rt_tex.Get(), nullptr, &rt_rtv))) return false;

        const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(rt_rtv.Get(), clear_col);
        return true;
    }

    void upload_vram_now(const std::uint16_t *vram_1x) {
        if (!vram_dirty || !vram_1x || !available) return;
        const int x0 = std::clamp(vram_dirty_x0, 0, Gpu::kVramWidth - 1);
        const int y0 = std::clamp(vram_dirty_y0, 0, Gpu::kVramHeight - 1);
        const int x1 = std::clamp(vram_dirty_x1, x0 + 1, Gpu::kVramWidth);
        const int y1 = std::clamp(vram_dirty_y1, y0 + 1, Gpu::kVramHeight);
        D3D11_BOX box{};
        box.left = static_cast<UINT>(x0);
        box.top = static_cast<UINT>(y0);
        box.front = 0;
        box.right = static_cast<UINT>(x1);
        box.bottom = static_cast<UINT>(y1);
        box.back = 1;
        constexpr UINT kRowBytes = static_cast<UINT>(Gpu::kVramWidth) * static_cast<UINT>(sizeof(std::uint16_t));
        const std::uint16_t *src = vram_1x + static_cast<std::size_t>(y0) * Gpu::kVramWidth + x0;
        ctx->UpdateSubresource(vram_tex.Get(), 0, &box, src, kRowBytes, 0);
        vram_dirty = false;
    }

    void flush_batch() {
        if (batch.empty() || !available) return;

        // If batch contains only pending VRAM blits (no 3D triangles relying on older VRAM),
        // upload the latest VRAM once right before drawing the coalesced batch.
        if (vram_dirty && !batch_has_3d_textured && pending_vram_ptr) {
            upload_vram_now(pending_vram_ptr);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(vbo.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, batch.data(), batch.size() * sizeof(HwVertex));
            ctx->Unmap(vbo.Get(), 0);
        }

        if (cbo_dirty) {
            CbData cb{filter_mode, scale, disable_dithering ? 1 : 0, 0};
            ctx->UpdateSubresource(cbo.Get(), 0, nullptr, &cb, 0, 0);
            cbo_dirty = false;
        }

        const int rt_w = Gpu::kVramWidth * scale;
        const int rt_h = Gpu::kVramHeight * scale;
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(rt_w);
        vp.Height = static_cast<float>(rt_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        D3D11_RECT sc{};
        sc.left = std::clamp(cur_clip_x1 * scale, 0, rt_w);
        sc.top = std::clamp(cur_clip_y1 * scale, 0, rt_h);
        sc.right = std::clamp((cur_clip_x2 + 1) * scale, 0, rt_w);
        sc.bottom = std::clamp((cur_clip_y2 + 1) * scale, 0, rt_h);
        ctx->RSSetScissorRects(1, &sc);
        ctx->RSSetState(rs.Get());

        ID3D11RenderTargetView *rtvs[1] = {rt_rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, nullptr);

        if (cur_blend_key < 0) {
            ctx->OMSetBlendState(blend_opaque.Get(), nullptr, 0xFFFFFFFFu);
        } else {
            const int m = cur_blend_key & 3;
            const float bf[4] = {
                (m == 3) ? 0.25f : 0.5f,
                (m == 3) ? 0.25f : 0.5f,
                (m == 3) ? 0.25f : 0.5f,
                1.0f
            };
            ctx->OMSetBlendState(blend_semi[m].Get(), bf, 0xFFFFFFFFu);
        }

        ctx->IASetInputLayout(layout.Get());
        UINT stride = sizeof(HwVertex);
        UINT offset = 0;
        ID3D11Buffer *vbos[1] = {vbo.Get()};
        ctx->IASetVertexBuffers(0, 1, vbos, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer *cbos[1] = {cbo.Get()};
        ctx->PSSetConstantBuffers(0, 1, cbos);
        ID3D11ShaderResourceView *srvs[1] = {vram_srv.Get()};
        ctx->PSSetShaderResources(0, 1, srvs);

        ctx->Draw(static_cast<UINT>(batch.size()), 0);
        batch.clear();
        batch_has_3d_textured = false;
    }

    void sync_vram_texture(const std::uint16_t *vram_1x) {
        if (!vram_dirty || !vram_1x || !available) return;
        pending_vram_ptr = vram_1x;
        if (batch_has_3d_textured) {
            // Draw queued 3D triangles against the PREVIOUS VRAM texture before overwriting it
            flush_batch();
        }
        upload_vram_now(vram_1x);
    }

    void append_triangle(const HwVertex &v0, const HwVertex &v1, const HwVertex &v2,
                         int blend_key, int cx1, int cy1, int cx2, int cy2) {
        if (blend_key != cur_blend_key ||
            cx1 != cur_clip_x1 || cy1 != cur_clip_y1 ||
            cx2 != cur_clip_x2 || cy2 != cur_clip_y2 ||
            batch.size() + 3 > kMaxBatchVerts) {
            flush_batch();
            cur_blend_key = blend_key;
            cur_clip_x1 = cx1;
            cur_clip_y1 = cy1;
            cur_clip_x2 = cx2;
            cur_clip_y2 = cy2;
        }
        batch.push_back(v0);
        batch.push_back(v1);
        batch.push_back(v2);
    }
#endif
};

GpuHwBackend::GpuHwBackend() : impl_(std::make_unique<Impl>()) {}
GpuHwBackend::~GpuHwBackend() = default;

bool GpuHwBackend::is_available() const noexcept {
    return impl_->available;
}

int GpuHwBackend::scale() const noexcept {
    return impl_->scale;
}

void GpuHwBackend::set_filter_mode(int mode) noexcept {
    if (impl_->filter_mode != mode) {
#ifdef _WIN32
        impl_->flush_batch();
        impl_->cbo_dirty = true;
#endif
        impl_->filter_mode = mode;
    }
}

void GpuHwBackend::set_disable_dithering(bool disable) noexcept {
    if (impl_->disable_dithering != disable) {
#ifdef _WIN32
        impl_->flush_batch();
        impl_->cbo_dirty = true;
#endif
        impl_->disable_dithering = disable;
    }
}

void GpuHwBackend::notify_vram_dirty(int x, int y, int w, int h) noexcept {
#ifdef _WIN32
    if (impl_->batch_has_3d_textured) {
        impl_->flush_batch();
    }
#endif
    impl_->mark_dirty_rect(x, y, w, h);
}

bool GpuHwBackend::init(int initial_scale) {
#ifndef _WIN32
    (void)initial_scale;
    return false;
#else
    if (impl_->available) return true;

    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    D3D_FEATURE_LEVEL fl_out{};
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fls, 3, D3D11_SDK_VERSION,
                                   &impl_->device, &fl_out, &impl_->ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               fls, 3, D3D11_SDK_VERSION,
                               &impl_->device, &fl_out, &impl_->ctx);
        if (FAILED(hr)) return false;
    }

    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    if (FAILED(D3DCompile(kHlslSource, std::strlen(kHlslSource), "psx_hw.hlsl",
                          nullptr, nullptr, "vs_main", "vs_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err_blob))) {
        return false;
    }
    if (FAILED(D3DCompile(kHlslSource, std::strlen(kHlslSource), "psx_hw.hlsl",
                          nullptr, nullptr, "ps_main", "ps_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err_blob))) {
        return false;
    }

    if (FAILED(impl_->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &impl_->vs))) return false;
    if (FAILED(impl_->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &impl_->ps))) return false;

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HwVertex, x),          D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HwVertex, r),          D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(HwVertex, u),          D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, texpage_x),  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, texpage_y),  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, clut_x),     D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, clut_y),     D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 5, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, tex_depth),  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 6, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, tex_window), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 7, DXGI_FORMAT_R32_UINT,        0, offsetof(HwVertex, flags),      D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(impl_->device->CreateInputLayout(ied, static_cast<UINT>(std::size(ied)),
                                                vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                                &impl_->layout))) {
        return false;
    }

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = static_cast<UINT>(Impl::kMaxBatchVerts * sizeof(HwVertex));
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(impl_->device->CreateBuffer(&vbd, nullptr, &impl_->vbo))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CbData);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(impl_->device->CreateBuffer(&cbd, nullptr, &impl_->cbo))) return false;

    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.ScissorEnable = TRUE;
    rsd.DepthClipEnable = FALSE;
    if (FAILED(impl_->device->CreateRasterizerState(&rsd, &impl_->rs))) return false;

    // Blend states: Opaque + PS1 Semi-transparency modes 0..3
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(impl_->device->CreateBlendState(&bd, &impl_->blend_opaque))) return false;

    // Mode 0: 0.5*B + 0.5*F (uses SRC_ALPHA / INV_SRC_ALPHA where PS outputs alpha=0.5 for semi, 1.0 for opaque)
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(impl_->device->CreateBlendState(&bd, &impl_->blend_semi[0]))) return false;

    // Mode 1: 1.0*B + 1.0*F
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    if (FAILED(impl_->device->CreateBlendState(&bd, &impl_->blend_semi[1]))) return false;

    // Mode 2: 1.0*B - 1.0*F
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    if (FAILED(impl_->device->CreateBlendState(&bd, &impl_->blend_semi[2]))) return false;

    // Mode 3: 1.0*B + 0.25*F
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    if (FAILED(impl_->device->CreateBlendState(&bd, &impl_->blend_semi[3]))) return false;

    // 1024x512 R16_UINT VRAM texture (DEFAULT usage enables fast dirty-rect UpdateSubresource uploads)
    D3D11_TEXTURE2D_DESC vtd{};
    vtd.Width = Gpu::kVramWidth;
    vtd.Height = Gpu::kVramHeight;
    vtd.MipLevels = 1;
    vtd.ArraySize = 1;
    vtd.Format = DXGI_FORMAT_R16_UINT;
    vtd.SampleDesc.Count = 1;
    vtd.Usage = D3D11_USAGE_DEFAULT;
    vtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    vtd.CPUAccessFlags = 0;
    if (FAILED(impl_->device->CreateTexture2D(&vtd, nullptr, &impl_->vram_tex))) return false;
    if (FAILED(impl_->device->CreateShaderResourceView(impl_->vram_tex.Get(), nullptr, &impl_->vram_srv))) return false;

    if (!impl_->create_render_target(initial_scale)) return false;
    impl_->cbo_dirty = true;

    impl_->batch.reserve(Impl::kMaxBatchVerts);
    impl_->available = true;
    return true;
#endif
}

void GpuHwBackend::set_scale(int new_scale, const std::uint16_t *vram_1x) {
#ifdef _WIN32
    if (!impl_->available) return;
    const int clamped = std::clamp(new_scale, 1, 8);
    if (clamped == impl_->scale) return;
    impl_->flush_batch();
    impl_->create_render_target(clamped);
    impl_->cbo_dirty = true;
    sync_full_vram(vram_1x);
#else
    (void)new_scale;
    (void)vram_1x;
#endif
}

void GpuHwBackend::blit_vram_rect_to_rt(const std::uint16_t *vram_1x, int x, int y, int w, int h) {
#ifdef _WIN32
    if (!impl_->available || !vram_1x || w <= 0 || h <= 0) return;
    // If 3D textured triangles are queued, flush them against the previous VRAM before queuing this blit.
    // Otherwise coalesce consecutive MDEC/LoadImage/FillRect blits into 1 single VRAM upload & Draw call!
    if (impl_->batch_has_3d_textured) {
        impl_->flush_batch();
    }
    impl_->mark_dirty_rect(x, y, w, h);
    impl_->pending_vram_ptr = vram_1x;

    const float x0 = static_cast<float>(x);
    const float y0 = static_cast<float>(y);
    const float x1 = static_cast<float>(x + w);
    const float y1 = static_cast<float>(y + h);

    HwVertex v00{}, v10{}, v01{}, v11{};
    v00.x = x0; v00.y = y0; v00.z = 1.0f; v00.u = x0; v00.v = y0; v00.flags = 0x100u;
    v10.x = x1; v10.y = y0; v10.z = 1.0f; v10.u = x1; v10.v = y0; v10.flags = 0x100u;
    v01.x = x0; v01.y = y1; v01.z = 1.0f; v01.u = x0; v01.v = y1; v01.flags = 0x100u;
    v11.x = x1; v11.y = y1; v11.z = 1.0f; v11.u = x1; v11.v = y1; v11.flags = 0x100u;

    impl_->append_triangle(v00, v10, v01, -1, 0, 0, Gpu::kVramWidth - 1, Gpu::kVramHeight - 1);
    impl_->append_triangle(v10, v11, v01, -1, 0, 0, Gpu::kVramWidth - 1, Gpu::kVramHeight - 1);
#else
    (void)vram_1x; (void)x; (void)y; (void)w; (void)h;
#endif
}

void GpuHwBackend::sync_full_vram(const std::uint16_t *vram_1x) {
    blit_vram_rect_to_rt(vram_1x, 0, 0, Gpu::kVramWidth, Gpu::kVramHeight);
}

void GpuHwBackend::draw_triangle(const HwVertex &v0, const HwVertex &v1, const HwVertex &v2,
                                 bool semi_trans, int semi_mode,
                                 int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                                 const std::uint16_t *vram_1x) {
#ifdef _WIN32
    if (!impl_->available) return;
    const bool textured = (v0.flags & 1u) != 0u;
    if (textured && impl_->vram_dirty) {
        impl_->sync_vram_texture(vram_1x);
    }
    if (textured) {
        impl_->batch_has_3d_textured = true;
    }

    if (!semi_trans) {
        impl_->append_triangle(v0, v1, v2, -1, clip_x1, clip_y1, clip_x2, clip_y2);
        return;
    }

    const int m = semi_mode & 3;
    if (!textured || m == 0) {
        // Mode 0 or untextured semi-transparent triangle handles opaque+semi in 1 pass
        impl_->append_triangle(v0, v1, v2, m, clip_x1, clip_y1, clip_x2, clip_y2);
        return;
    }

    // Modes 1..3 on textured triangles: Pass 1 draws opaque texels (bit 15 == 0), Pass 2 draws semi texels (bit 15 == 1)
    HwVertex o0 = v0, o1 = v1, o2 = v2;
    o0.flags |= 16u; o1.flags |= 16u; o2.flags |= 16u;
    impl_->append_triangle(o0, o1, o2, -1, clip_x1, clip_y1, clip_x2, clip_y2);

    HwVertex s0 = v0, s1 = v1, s2 = v2;
    s0.flags |= 8u; s1.flags |= 8u; s2.flags |= 8u;
    impl_->append_triangle(s0, s1, s2, m, clip_x1, clip_y1, clip_x2, clip_y2);
#else
    (void)v0; (void)v1; (void)v2; (void)semi_trans; (void)semi_mode;
    (void)clip_x1; (void)clip_y1; (void)clip_x2; (void)clip_y2; (void)vram_1x;
#endif
}

bool GpuHwBackend::read_display_rgba8888(const std::uint16_t *vram_1x,
                                         int disp_x, int disp_y, int base_w, int base_h,
                                         std::vector<std::uint32_t> &out_rgba, int &out_w, int &out_h) {
#ifndef _WIN32
    (void)vram_1x; (void)disp_x; (void)disp_y; (void)base_w; (void)base_h;
    (void)out_rgba; (void)out_w; (void)out_h;
    return false;
#else
    if (!impl_->available) return false;
    if (impl_->vram_dirty && vram_1x) {
        impl_->sync_vram_texture(vram_1x);
    }
    impl_->flush_batch();

    const int s = impl_->scale;
    const int rt_w = Gpu::kVramWidth * s;
    const int rt_h = Gpu::kVramHeight * s;
    out_w = base_w * s;
    out_h = base_h * s;

    const int src_x = (disp_x * s) % rt_w;
    const int src_y = (disp_y * s) % rt_h;
    if (src_x + out_w > rt_w || src_y + out_h > rt_h) {
        return false;
    }

    if (!impl_->staging_tex[0] || !impl_->staging_tex[1] || impl_->staging_w != out_w || impl_->staging_h != out_h) {
        impl_->staging_tex[0].Reset();
        impl_->staging_tex[1].Reset();
        D3D11_TEXTURE2D_DESC std{};
        std.Width = static_cast<UINT>(out_w);
        std.Height = static_cast<UINT>(out_h);
        std.MipLevels = 1;
        std.ArraySize = 1;
        std.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std.SampleDesc.Count = 1;
        std.Usage = D3D11_USAGE_STAGING;
        std.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(impl_->device->CreateTexture2D(&std, nullptr, &impl_->staging_tex[0])) ||
            FAILED(impl_->device->CreateTexture2D(&std, nullptr, &impl_->staging_tex[1]))) {
            impl_->staging_tex[0].Reset();
            impl_->staging_tex[1].Reset();
            return false;
        }
        impl_->staging_w = out_w;
        impl_->staging_h = out_h;
        impl_->staging_idx = 0;
        impl_->staging_primed = false;
    }

    D3D11_BOX box{};
    box.left = static_cast<UINT>(src_x);
    box.top = static_cast<UINT>(src_y);
    box.front = 0;
    box.right = static_cast<UINT>(src_x + out_w);
    box.bottom = static_cast<UINT>(src_y + out_h);
    box.back = 1;

    // Double-buffered async staging ring: copy current frame into staging[cur], Flush() so
    // the GPU executes the render+copy immediately in parallel with SDL present + next-frame CPU,
    // and read back previous frame from staging[cur ^ 1] without stalling the GPU pipeline.
    const int cur = impl_->staging_idx;
    const int read_idx = impl_->staging_primed ? (cur ^ 1) : cur;
    impl_->ctx->CopySubresourceRegion(impl_->staging_tex[cur].Get(), 0, 0, 0, 0,
                                      impl_->rt_tex.Get(), 0, &box);
    impl_->ctx->Flush();
    impl_->staging_idx = cur ^ 1;
    impl_->staging_primed = true;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl_->ctx->Map(impl_->staging_tex[read_idx].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    out_rgba.resize(static_cast<std::size_t>(out_w * out_h));
    const auto *src_bytes = static_cast<const std::uint8_t *>(mapped.pData);
    const std::size_t row_bytes = static_cast<std::size_t>(out_w) * sizeof(std::uint32_t);
    if (mapped.RowPitch == row_bytes) {
        std::memcpy(out_rgba.data(), src_bytes, row_bytes * static_cast<std::size_t>(out_h));
    } else {
        for (int y = 0; y < out_h; ++y) {
            std::memcpy(out_rgba.data() + static_cast<std::size_t>(y * out_w),
                        src_bytes + static_cast<std::size_t>(y) * mapped.RowPitch,
                        row_bytes);
        }
    }
    impl_->ctx->Unmap(impl_->staging_tex[read_idx].Get(), 0);
    return true;
#endif
}

}  // namespace psx
