#include "psx/gpu_hw.hpp"
#include "psx/gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
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

constexpr const char *kPresentHlsl = R"HLSL(
cbuffer PresentCB : register(b0) {
    float4 u_uv_rect;     // x=u0, y=v0, z=du, w=dv
    float4 u_post;        // x=brightness, y=contrast, z=scanlines, w=disp_h
    float4 u_extra;       // x=visual_preset, y=cas_on, z=inv_tex_w, w=inv_tex_h
    float4 u_fsr;         // x=fsr_on, y=tex_w, z=tex_h, w=0
};
Texture2D<float4> g_src : register(t0);
SamplerState g_samp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut vs_present(uint vid : SV_VertexID) {
    VsOut o;
    float2 p = float2((vid == 1u) ? 3.0f : -1.0f, (vid == 2u) ? -3.0f : 1.0f);
    o.pos = float4(p, 0.0f, 1.0f);
    float2 t = float2((p.x + 1.0f) * 0.5f, (1.0f - p.y) * 0.5f);
    o.uv = u_uv_rect.xy + t * u_uv_rect.zw;
    return o;
}

float fsr_luma(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

float fsr_weight(float2 off, float2 f, float2 dir, float2 perp, float stretch, float inv_s) {
    float2 d = off - f;
    float a = dot(d, dir) * inv_s;
    float b = dot(d, perp) * stretch;
    float d2 = min(a * a + b * b, 4.0f);
    float w = (0.4f * d2 - 1.0f);
    float t = max(0.0f, 1.0f - 0.25f * d2);
    return w * w * t * t;
}

float3 sample_fsr_easu(float2 uv) {
    float2 sz = max(u_fsr.yz, float2(1.0f, 1.0f));
    float2 inv = u_extra.zw;
    float2 pp = uv * sz - 0.5f;
    float2 fp = floor(pp);
    float2 f = pp - fp;
    float2 base = (fp + 0.5f) * inv;
    float2 uv_min = u_uv_rect.xy + 0.5f * inv;
    float2 uv_max = u_uv_rect.xy + u_uv_rect.zw - 0.5f * inv;

    float3 b = g_src.SampleLevel(g_samp, clamp(base + float2( 0.0f, -1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 c = g_src.SampleLevel(g_samp, clamp(base + float2( 1.0f, -1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 e = g_src.SampleLevel(g_samp, clamp(base + float2(-1.0f,  0.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 f0= g_src.SampleLevel(g_samp, clamp(base + float2( 0.0f,  0.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 g = g_src.SampleLevel(g_samp, clamp(base + float2( 1.0f,  0.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 h = g_src.SampleLevel(g_samp, clamp(base + float2( 2.0f,  0.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 i = g_src.SampleLevel(g_samp, clamp(base + float2(-1.0f,  1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 j = g_src.SampleLevel(g_samp, clamp(base + float2( 0.0f,  1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 k = g_src.SampleLevel(g_samp, clamp(base + float2( 1.0f,  1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 l = g_src.SampleLevel(g_samp, clamp(base + float2( 2.0f,  1.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 n = g_src.SampleLevel(g_samp, clamp(base + float2( 0.0f,  2.0f) * inv, uv_min, uv_max), 0).rgb;
    float3 o = g_src.SampleLevel(g_samp, clamp(base + float2( 1.0f,  2.0f) * inv, uv_min, uv_max), 0).rgb;

    float lb = fsr_luma(b), lc = fsr_luma(c), le = fsr_luma(e), lf = fsr_luma(f0);
    float lg = fsr_luma(g), lh = fsr_luma(h), li = fsr_luma(i), lj = fsr_luma(j);
    float lk = fsr_luma(k), ll = fsr_luma(l), ln = fsr_luma(n), lo = fsr_luma(o);

    float2 dir = float2((lg - le) + (lk - li) + (lh - lf) + (ll - lj),
                        (lj - lb) + (lk - lc) + (ln - lf) + (lo - lg));
    float dlen = length(dir);
    dir = (dlen > 1e-4f) ? (dir / dlen) : float2(1.0f, 0.0f);
    float2 perp = float2(-dir.y, dir.x);
    float aniso = saturate(dlen * 2.2f);
    float stretch = lerp(1.0f, 1.85f, aniso);
    float inv_s = 1.0f / stretch;

    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    float w;
    w = fsr_weight(float2( 0.0f, -1.0f), f, dir, perp, stretch, inv_s); acc += b  * w; wsum += w;
    w = fsr_weight(float2( 1.0f, -1.0f), f, dir, perp, stretch, inv_s); acc += c  * w; wsum += w;
    w = fsr_weight(float2(-1.0f,  0.0f), f, dir, perp, stretch, inv_s); acc += e  * w; wsum += w;
    w = fsr_weight(float2( 0.0f,  0.0f), f, dir, perp, stretch, inv_s); acc += f0 * w; wsum += w;
    w = fsr_weight(float2( 1.0f,  0.0f), f, dir, perp, stretch, inv_s); acc += g  * w; wsum += w;
    w = fsr_weight(float2( 2.0f,  0.0f), f, dir, perp, stretch, inv_s); acc += h  * w; wsum += w;
    w = fsr_weight(float2(-1.0f,  1.0f), f, dir, perp, stretch, inv_s); acc += i  * w; wsum += w;
    w = fsr_weight(float2( 0.0f,  1.0f), f, dir, perp, stretch, inv_s); acc += j  * w; wsum += w;
    w = fsr_weight(float2( 1.0f,  1.0f), f, dir, perp, stretch, inv_s); acc += k  * w; wsum += w;
    w = fsr_weight(float2( 2.0f,  1.0f), f, dir, perp, stretch, inv_s); acc += l  * w; wsum += w;
    w = fsr_weight(float2( 0.0f,  2.0f), f, dir, perp, stretch, inv_s); acc += n  * w; wsum += w;
    w = fsr_weight(float2( 1.0f,  2.0f), f, dir, perp, stretch, inv_s); acc += o  * w; wsum += w;

    float3 mn4 = min(min(f0, g), min(j, k));
    float3 mx4 = max(max(f0, g), max(j, k));
    return clamp(acc / max(wsum, 1e-5f), mn4, mx4);
}

float4 ps_present(VsOut i) : SV_Target {
    float3 c = (u_fsr.x > 0.5f) ? sample_fsr_easu(i.uv) : g_src.Sample(g_samp, i.uv).rgb;
    if (u_extra.y > 0.5f) {
        float2 dx = float2(u_extra.z, 0.0f);
        float2 dy = float2(0.0f, u_extra.w);
        float3 n = g_src.Sample(g_samp, i.uv - dy).rgb;
        float3 s = g_src.Sample(g_samp, i.uv + dy).rgb;
        float3 w = g_src.Sample(g_samp, i.uv - dx).rgb;
        float3 e = g_src.Sample(g_samp, i.uv + dx).rgb;
        float3 mn = min(c, min(min(n, s), min(w, e)));
        float3 mx = max(c, max(max(n, s), max(w, e)));
        float3 avg = (n + s + w + e) * 0.25f;
        float3 sharp = c + (c - avg) * 0.45f;
        c = clamp(sharp, mn, mx);
    }
    int preset = (int)(u_extra.x + 0.5f);
    if (preset == 1) {
        float lum = dot(c, float3(0.299f, 0.587f, 0.114f));
        c = lerp(float3(lum, lum, lum), c, 0.85f) * float3(0.96f, 0.96f, 1.05f);
        c = saturate((c - 0.5f) * 1.08f + 0.48f);
    } else if (preset == 2) {
        c = saturate((c - 0.5f) * 1.05f + 0.5f);
    } else if (preset == 3) {
        float lum = dot(c, float3(0.299f, 0.587f, 0.114f));
        c = saturate(lerp(float3(lum, lum, lum), c, 1.18f));
    }
    c = saturate((c - 0.5f) * u_post.y + 0.5f + u_post.x);
    if (u_post.z > 0.001f) {
        float row = floor((i.uv.y - u_uv_rect.y) / max(u_uv_rect.w, 1e-5f) * u_post.w);
        if (fmod(row, 2.0f) >= 1.0f) {
            c *= (1.0f - u_post.z * 0.45f);
        }
    }
    return float4(c, 1.0f);
}

float4 ps_ui(VsOut i) : SV_Target {
    return g_src.Sample(g_samp, i.uv);
}
)HLSL";

struct PresentCbData {
    float uv_rect[4];
    float post[4];
    float extra[4];
    float fsr[4];
};

bool compile_or_load_dxbc(const char *src, const char *tag, const char *entry, const char *target,
                          std::vector<std::uint8_t> &out_bc) {
    std::uint64_t h = 14695981039346656037ull;
    auto mix_str = [&](const char *s) {
        for (; *s; ++s) { h ^= static_cast<std::uint8_t>(*s); h *= 1099511628211ull; }
        h ^= 0xFFu; h *= 1099511628211ull;
    };
    mix_str(src);
    mix_str(entry);
    mix_str(target);

    std::filesystem::path cache_file;
    if (const char *dir = std::getenv("NOCTURNE_SHADER_CACHE_DIR"); dir && dir[0]) {
        char fname[96];
        std::snprintf(fname, sizeof fname, "%s_%s_%016llx.dxbc", tag, entry, static_cast<unsigned long long>(h));
        cache_file = std::filesystem::path(dir) / fname;
        std::ifstream ifs(cache_file, std::ios::binary | std::ios::ate);
        if (ifs.is_open()) {
            const auto sz = ifs.tellg();
            if (sz > 32 && sz < 8 * 1024 * 1024) {
                out_bc.resize(static_cast<std::size_t>(sz));
                ifs.seekg(0);
                if (ifs.read(reinterpret_cast<char *>(out_bc.data()), sz)) return true;
            }
        }
    }

    ComPtr<ID3DBlob> blob, err_blob;
    if (FAILED(D3DCompile(src, std::strlen(src), tag, nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err_blob)) || !blob) {
        return false;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(blob->GetBufferPointer());
    out_bc.assign(bytes, bytes + blob->GetBufferSize());
    if (!cache_file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_file.parent_path(), ec);
        std::ofstream ofs(cache_file, std::ios::binary | std::ios::trunc);
        if (ofs.is_open()) {
            ofs.write(reinterpret_cast<const char *>(out_bc.data()), static_cast<std::streamsize>(out_bc.size()));
        }
    }
    return true;
}

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
    ComPtr<ID3D11ShaderResourceView> rt_srv;
    ComPtr<ID3D11Texture2D> staging_tex[2];
    int staging_w = 0;
    int staging_h = 0;
    int staging_idx = 0;
    bool staging_primed = false;

    // Direct DXGI Flip-Model Swapchain + GPU Post-FX + UI Overlay resources (zero CPU readback!)
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11RenderTargetView> bb_rtv;
    int sc_w = 0, sc_h = 0;
    ComPtr<ID3D11VertexShader> vs_pres;
    ComPtr<ID3D11PixelShader> ps_pres;
    ComPtr<ID3D11PixelShader> ps_ui;
    ComPtr<ID3D11Buffer> pres_cbo;
    ComPtr<ID3D11SamplerState> samp_point;
    ComPtr<ID3D11SamplerState> samp_linear;
    ComPtr<ID3D11BlendState> blend_ui_alpha;
    ComPtr<ID3D11Texture2D> ui_tex;
    ComPtr<ID3D11ShaderResourceView> ui_srv;
    int ui_tex_w = 0, ui_tex_h = 0;
    ComPtr<ID3D11Texture2D> sw_tex;
    ComPtr<ID3D11ShaderResourceView> sw_srv;
    int sw_tex_w = 0, sw_tex_h = 0;

    static constexpr std::size_t kMaxBatchVerts = 32768;
    std::vector<HwVertex> batch;
    std::size_t vbo_write_offset = 0;
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

        rt_srv.Reset();
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
        if (FAILED(device->CreateShaderResourceView(rt_tex.Get(), nullptr, &rt_srv))) return false;

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

        // #4: NO_OVERWRITE ring buffer — only DISCARD on wrap to avoid driver buffer-renaming stalls.
        const std::size_t count = batch.size();
        D3D11_MAP map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (vbo_write_offset + count > kMaxBatchVerts || vbo_write_offset == 0) {
            vbo_write_offset = 0;
            map_type = D3D11_MAP_WRITE_DISCARD;
        }
        const UINT start_vert = static_cast<UINT>(vbo_write_offset);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(vbo.Get(), 0, map_type, 0, &mapped))) {
            auto *dst = static_cast<HwVertex *>(mapped.pData) + vbo_write_offset;
            std::memcpy(dst, batch.data(), count * sizeof(HwVertex));
            ctx->Unmap(vbo.Get(), 0);
            vbo_write_offset += count;
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

        ctx->Draw(static_cast<UINT>(count), start_vert);
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

    auto load_or_compile_dxbc = [](const char *entry, const char *target, std::vector<std::uint8_t> &out_bc) -> bool {
        // Compute FNV-1a 64-bit hash of HLSL source + entry + target for automatic cache invalidation.
        std::uint64_t h = 14695981039346656037ull;
        auto mix_str = [&](const char *s) {
            for (; *s; ++s) { h ^= static_cast<std::uint8_t>(*s); h *= 1099511628211ull; }
            h ^= 0xFFu; h *= 1099511628211ull;
        };
        mix_str(kHlslSource);
        mix_str(entry);
        mix_str(target);

        std::filesystem::path cache_file;
        if (const char *dir = std::getenv("NOCTURNE_SHADER_CACHE_DIR"); dir && dir[0]) {
            char fname[96];
            std::snprintf(fname, sizeof fname, "psx_%s_%016llx.dxbc", entry, static_cast<unsigned long long>(h));
            cache_file = std::filesystem::path(dir) / fname;
            std::ifstream ifs(cache_file, std::ios::binary | std::ios::ate);
            if (ifs.is_open()) {
                const auto sz = ifs.tellg();
                if (sz > 32 && sz < 8 * 1024 * 1024) {
                    out_bc.resize(static_cast<std::size_t>(sz));
                    ifs.seekg(0);
                    if (ifs.read(reinterpret_cast<char *>(out_bc.data()), sz)) return true;
                }
            }
        }

        ComPtr<ID3DBlob> blob, err_blob;
        if (FAILED(D3DCompile(kHlslSource, std::strlen(kHlslSource), "psx_hw.hlsl",
                              nullptr, nullptr, entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err_blob)) || !blob) {
            return false;
        }
        const auto *bytes = static_cast<const std::uint8_t *>(blob->GetBufferPointer());
        out_bc.assign(bytes, bytes + blob->GetBufferSize());
        if (!cache_file.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cache_file.parent_path(), ec);
            std::ofstream ofs(cache_file, std::ios::binary | std::ios::trunc);
            if (ofs.is_open()) {
                ofs.write(reinterpret_cast<const char *>(out_bc.data()), static_cast<std::streamsize>(out_bc.size()));
            }
        }
        return true;
    };

    std::vector<std::uint8_t> vs_bc, ps_bc;
    if (!load_or_compile_dxbc("vs_main", "vs_4_0", vs_bc)) return false;
    if (!load_or_compile_dxbc("ps_main", "ps_4_0", ps_bc)) return false;

    if (FAILED(impl_->device->CreateVertexShader(vs_bc.data(), vs_bc.size(), nullptr, &impl_->vs))) return false;
    if (FAILED(impl_->device->CreatePixelShader(ps_bc.data(), ps_bc.size(), nullptr, &impl_->ps))) return false;

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
                                                vs_bc.data(), vs_bc.size(),
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

bool GpuHwBackend::init_swapchain(void *hwnd_ptr, int win_w, int win_h) {
#ifndef _WIN32
    (void)hwnd_ptr; (void)win_w; (void)win_h;
    return false;
#else
    if (!impl_->available || !hwnd_ptr || win_w <= 0 || win_h <= 0) return false;
    HWND hwnd = static_cast<HWND>(hwnd_ptr);

    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(impl_->device.As(&dxgi_dev))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), &factory))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(win_w);
    sd.Height = static_cast<UINT>(win_h);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = factory->CreateSwapChainForHwnd(impl_->device.Get(), hwnd, &sd, nullptr, nullptr, &sc);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.Scaling = DXGI_SCALING_NONE;
        hr = factory->CreateSwapChainForHwnd(impl_->device.Get(), hwnd, &sd, nullptr, nullptr, &sc);
        if (FAILED(hr)) return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), &bb))) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(impl_->device->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) return false;

    std::vector<std::uint8_t> vs_bc, ps_bc, ui_bc;
    if (!compile_or_load_dxbc(kPresentHlsl, "psx_pres", "vs_present", "vs_4_0", vs_bc)) return false;
    if (!compile_or_load_dxbc(kPresentHlsl, "psx_pres", "ps_present", "ps_4_0", ps_bc)) return false;
    if (!compile_or_load_dxbc(kPresentHlsl, "psx_pres", "ps_ui", "ps_4_0", ui_bc)) return false;

    if (FAILED(impl_->device->CreateVertexShader(vs_bc.data(), vs_bc.size(), nullptr, &impl_->vs_pres))) return false;
    if (FAILED(impl_->device->CreatePixelShader(ps_bc.data(), ps_bc.size(), nullptr, &impl_->ps_pres))) return false;
    if (FAILED(impl_->device->CreatePixelShader(ui_bc.data(), ui_bc.size(), nullptr, &impl_->ps_ui))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(PresentCbData);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(impl_->device->CreateBuffer(&cbd, nullptr, &impl_->pres_cbo))) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(impl_->device->CreateSamplerState(&samp, &impl_->samp_point))) return false;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(impl_->device->CreateSamplerState(&samp, &impl_->samp_linear))) return false;

    D3D11_BLEND_DESC ubd{};
    ubd.RenderTarget[0].BlendEnable = TRUE;
    ubd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ubd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ubd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ubd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ubd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ubd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ubd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(impl_->device->CreateBlendState(&ubd, &impl_->blend_ui_alpha))) return false;

    impl_->swapchain = std::move(sc);
    impl_->bb_rtv = std::move(rtv);
    impl_->sc_w = win_w;
    impl_->sc_h = win_h;
    return true;
#endif
}

bool GpuHwBackend::swapchain_active() const noexcept {
#ifdef _WIN32
    return impl_->available && impl_->swapchain != nullptr;
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {
void present_composite_impl(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                            IDXGISwapChain1 *sc, ComPtr<ID3D11RenderTargetView> &bb_rtv, int &sc_w, int &sc_h,
                            ID3D11VertexShader *vs_pres, ID3D11PixelShader *ps_pres, ID3D11PixelShader *ps_ui,
                            ID3D11Buffer *pres_cbo, ID3D11SamplerState *samp_pt, ID3D11SamplerState *samp_lin,
                            ID3D11RasterizerState *rs, ID3D11BlendState *blend_opaque, ID3D11BlendState *blend_ui,
                            ID3D11ShaderResourceView *game_srv, float u0, float v0, float du, float dv,
                            float inv_tex_w, float inv_tex_h,
                            const GpuHwBackend::PresentParams &p,
                            ComPtr<ID3D11Texture2D> &ui_tex, ComPtr<ID3D11ShaderResourceView> &ui_srv,
                            int &ui_tex_w, int &ui_tex_h,
                            const std::uint32_t *ui_rgba, int ui_w, int ui_h, int ui_dst_h, bool ui_dirty) {
    if (p.win_w > 0 && p.win_h > 0 && (p.win_w != sc_w || p.win_h != sc_h)) {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        bb_rtv.Reset();
        if (SUCCEEDED(sc->ResizeBuffers(0, static_cast<UINT>(p.win_w), static_cast<UINT>(p.win_h),
                                        DXGI_FORMAT_UNKNOWN, 0))) {
            ComPtr<ID3D11Texture2D> bb;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), &bb))) {
                dev->CreateRenderTargetView(bb.Get(), nullptr, &bb_rtv);
                sc_w = p.win_w;
                sc_h = p.win_h;
            }
        }
    }
    if (!bb_rtv) return;

    ID3D11RenderTargetView *rtvs[1] = {bb_rtv.Get()};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(bb_rtv.Get(), black);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->RSSetState(rs);
    D3D11_RECT full_sc{0, 0, sc_w, sc_h};
    ctx->RSSetScissorRects(1, &full_sc);
    ctx->VSSetShader(vs_pres, nullptr, 0);
    ID3D11Buffer *cbos[1] = {pres_cbo};
    ctx->VSSetConstantBuffers(0, 1, cbos);
    ctx->PSSetConstantBuffers(0, 1, cbos);

    if (!p.display_disabled && game_srv && p.dst_w > 0 && p.dst_h > 0) {
        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(p.dst_x);
        vp.TopLeftY = static_cast<float>(p.dst_y);
        vp.Width = static_cast<float>(p.dst_w);
        vp.Height = static_cast<float>(p.dst_h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        PresentCbData cb{};
        cb.uv_rect[0] = u0; cb.uv_rect[1] = v0; cb.uv_rect[2] = du; cb.uv_rect[3] = dv;
        cb.post[0] = p.brightness; cb.post[1] = p.contrast; cb.post[2] = p.scanlines;
        cb.post[3] = static_cast<float>(std::max(1, p.base_h));
        cb.extra[0] = static_cast<float>(p.visual_preset);
        cb.extra[1] = p.cas_sharpening ? 1.0f : 0.0f;
        cb.extra[2] = inv_tex_w;
        cb.extra[3] = inv_tex_h;
        cb.fsr[0] = p.fsr_upscale ? 1.0f : 0.0f;
        cb.fsr[1] = (inv_tex_w > 0.0f) ? (1.0f / inv_tex_w) : 1.0f;
        cb.fsr[2] = (inv_tex_h > 0.0f) ? (1.0f / inv_tex_h) : 1.0f;
        cb.fsr[3] = 0.0f;
        ctx->UpdateSubresource(pres_cbo, 0, nullptr, &cb, 0, 0);

        ctx->OMSetBlendState(blend_opaque, nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(ps_pres, nullptr, 0);
        ID3D11ShaderResourceView *srvs[1] = {game_srv};
        ctx->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState *samps[1] = {p.linear_filter ? samp_lin : samp_pt};
        ctx->PSSetSamplers(0, 1, samps);
        ctx->Draw(3, 0);
    }

    if (ui_rgba && ui_w > 0 && ui_h > 0 && ui_dst_h > 0) {
        if (!ui_tex || ui_tex_w != ui_w || ui_tex_h != ui_h) {
            ui_srv.Reset();
            ui_tex.Reset();
            D3D11_TEXTURE2D_DESC ud{};
            ud.Width = static_cast<UINT>(ui_w);
            ud.Height = static_cast<UINT>(ui_h);
            ud.MipLevels = 1;
            ud.ArraySize = 1;
            ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            ud.SampleDesc.Count = 1;
            ud.Usage = D3D11_USAGE_DEFAULT;
            ud.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&ud, nullptr, &ui_tex))) {
                dev->CreateShaderResourceView(ui_tex.Get(), nullptr, &ui_srv);
                ui_tex_w = ui_w;
                ui_tex_h = ui_h;
                ui_dirty = true;
            }
        }
        if (ui_tex && ui_srv) {
            if (ui_dirty) {
                ctx->UpdateSubresource(ui_tex.Get(), 0, nullptr, ui_rgba,
                                       static_cast<UINT>(ui_w * sizeof(std::uint32_t)), 0);
            }
            D3D11_VIEWPORT uvp{0.0f, 0.0f, static_cast<float>(sc_w), static_cast<float>(ui_dst_h), 0.0f, 1.0f};
            ctx->RSSetViewports(1, &uvp);
            PresentCbData ucb{};
            ucb.uv_rect[0] = 0.0f; ucb.uv_rect[1] = 0.0f; ucb.uv_rect[2] = 1.0f; ucb.uv_rect[3] = 1.0f;
            ctx->UpdateSubresource(pres_cbo, 0, nullptr, &ucb, 0, 0);
            ctx->OMSetBlendState(blend_ui, nullptr, 0xFFFFFFFFu);
            ctx->PSSetShader(ps_ui, nullptr, 0);
            ID3D11ShaderResourceView *usrvs[1] = {ui_srv.Get()};
            ctx->PSSetShaderResources(0, 1, usrvs);
            ID3D11SamplerState *usamps[1] = {samp_lin};
            ctx->PSSetSamplers(0, 1, usamps);
            ctx->Draw(3, 0);
        }
    }

    ID3D11ShaderResourceView *null_srv[1] = {nullptr};
    ctx->PSSetShaderResources(0, 1, null_srv);
    sc->Present(0, 0);
}
}  // namespace
#endif

bool GpuHwBackend::present_direct(const std::uint16_t *vram_1x, const PresentParams &p,
                                  const std::uint32_t *ui_rgba, int ui_w, int ui_h, int ui_dst_h, bool ui_dirty) {
#ifndef _WIN32
    (void)vram_1x; (void)p; (void)ui_rgba; (void)ui_w; (void)ui_h; (void)ui_dst_h; (void)ui_dirty;
    return false;
#else
    if (!swapchain_active() || !impl_->rt_srv) return false;
    if (impl_->vram_dirty && vram_1x) impl_->sync_vram_texture(vram_1x);
    impl_->flush_batch();

    const int s = impl_->scale;
    const float rt_w = static_cast<float>(Gpu::kVramWidth * s);
    const float rt_h = static_cast<float>(Gpu::kVramHeight * s);
    const float u0 = static_cast<float>((p.disp_x * s) % (Gpu::kVramWidth * s)) / rt_w;
    const float v0 = static_cast<float>((p.disp_y * s) % (Gpu::kVramHeight * s)) / rt_h;
    const float du = static_cast<float>(p.base_w * s) / rt_w;
    const float dv = static_cast<float>(p.base_h * s) / rt_h;
    present_composite_impl(impl_->device.Get(), impl_->ctx.Get(),
                           impl_->swapchain.Get(), impl_->bb_rtv, impl_->sc_w, impl_->sc_h,
                           impl_->vs_pres.Get(), impl_->ps_pres.Get(), impl_->ps_ui.Get(),
                           impl_->pres_cbo.Get(), impl_->samp_point.Get(), impl_->samp_linear.Get(),
                           impl_->rs.Get(), impl_->blend_opaque.Get(), impl_->blend_ui_alpha.Get(),
                           impl_->rt_srv.Get(), u0, v0, du, dv, 1.0f / rt_w, 1.0f / rt_h,
                           p, impl_->ui_tex, impl_->ui_srv, impl_->ui_tex_w, impl_->ui_tex_h,
                           ui_rgba, ui_w, ui_h, ui_dst_h, ui_dirty);
    return true;
#endif
}

bool GpuHwBackend::present_sw_direct(const std::uint32_t *rgba, int w, int h, const PresentParams &p,
                                     const std::uint32_t *ui_rgba, int ui_w, int ui_h, int ui_dst_h, bool ui_dirty) {
#ifndef _WIN32
    (void)rgba; (void)w; (void)h; (void)p; (void)ui_rgba; (void)ui_w; (void)ui_h; (void)ui_dst_h; (void)ui_dirty;
    return false;
#else
    if (!swapchain_active()) return false;
    ID3D11ShaderResourceView *srv = nullptr;
    if (rgba && w > 0 && h > 0) {
        if (!impl_->sw_tex || impl_->sw_tex_w != w || impl_->sw_tex_h != h) {
            impl_->sw_srv.Reset();
            impl_->sw_tex.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->sw_tex))) {
                impl_->device->CreateShaderResourceView(impl_->sw_tex.Get(), nullptr, &impl_->sw_srv);
                impl_->sw_tex_w = w;
                impl_->sw_tex_h = h;
            }
        }
        if (impl_->sw_tex && impl_->sw_srv) {
            impl_->ctx->UpdateSubresource(impl_->sw_tex.Get(), 0, nullptr, rgba,
                                          static_cast<UINT>(w * sizeof(std::uint32_t)), 0);
            srv = impl_->sw_srv.Get();
        }
    }
    const float inv_w = (w > 0) ? (1.0f / static_cast<float>(w)) : 0.0f;
    const float inv_h = (h > 0) ? (1.0f / static_cast<float>(h)) : 0.0f;
    present_composite_impl(impl_->device.Get(), impl_->ctx.Get(),
                           impl_->swapchain.Get(), impl_->bb_rtv, impl_->sc_w, impl_->sc_h,
                           impl_->vs_pres.Get(), impl_->ps_pres.Get(), impl_->ps_ui.Get(),
                           impl_->pres_cbo.Get(), impl_->samp_point.Get(), impl_->samp_linear.Get(),
                           impl_->rs.Get(), impl_->blend_opaque.Get(), impl_->blend_ui_alpha.Get(),
                           srv, 0.0f, 0.0f, 1.0f, 1.0f, inv_w, inv_h,
                           p, impl_->ui_tex, impl_->ui_srv, impl_->ui_tex_w, impl_->ui_tex_h,
                           ui_rgba, ui_w, ui_h, ui_dst_h, ui_dirty);
    return true;
#endif
}

}  // namespace psx
