#pragma once
#include <cstdint>

namespace ps2 {
// Display-only options, independent of emulated GS state and save states.
// bit 0: edge antialiasing.
// bit 2: sharp enlargement (cubic plus light contrast-adaptive sharpening), the other two bits are not used with it.
// bit 1: soft enlargement (a smooth blend everywhere) instead of the sharp one (hard pixels, only their borders blend).
void set_video_options(std::uint32_t options);
std::uint32_t video_options();

// Live image & display controls adjusted from the F1 Settings > Graphics tab (applied immediately in blit.wgsl and gpu.cpp).
struct VideoParams {
    float brightness = 0.0f;  // -0.50 .. +0.50
    float contrast = 1.0f;    //  0.50 ..  1.50
    float saturation = 1.0f;  //  0.00 ..  2.00
    float gamma = 1.0f;       //  0.50 ..  2.00
    float scanlines = 0.0f;   //  0.00 ..  1.00 (0 = off)
    // Aspect modes:
    //  0 = 4:3 Classic
    //  1 = 16:9 Widescreen
    //  2 = Stretch to Fill
    //  3 = Auto (Match Window Aspect)
    //  4 = 16:10 Widescreen
    //  5 = 21:9 Ultrawide
    //  6 = 32:9 Super-Ultrawide
    int aspect_mode = 0;
    float window_aspect = 16.0f / 9.0f;  // Live host window aspect ratio (used for Auto / Stretch)
    bool true_widescreen = true;  // true = Hor+ 3D camera projection & frustum expansion for widescreen/ultrawide
    int hud_anchor_mode = 2;      // 0 = Off (Stretched HUD), 1 = Ultrawide Only (21:9/32:9), 2 = All Widescreen (16:10+) [default]
    bool deinterlace = true;      // true = weave deinterlace on, false = off
    // Phase 2: Depth-aware lighting & post-processing (all off by default for authentic PS2 visuals)
    float ssao = 0.0f;        //  0.00 ..  1.00 (Depth-aware ambient occlusion)
    float bloom = 0.0f;       //  0.00 ..  1.00 (HDR glow on magic/lights/Judgment Ring)
    float ssgi = 0.0f;        //  0.00 ..  1.00 (Depth-aware indirect light bounce)
    bool filmic = false;      //  ACES filmic tone mapping curve
    float vignette = 0.0f;    //  0.00 ..  1.00 (Gothic edge vignette)
    // Wave D: Performance, Anisotropy, Present Mode, Selective Turbo Skip & Photo Mode
    int anisotropic_filtering = 16;  // 1 = Off, 4 = 4x, 8 = 8x, 16 = 16x
    int present_mode_pref = 0;       // 0 = Low-Latency Auto (Mailbox/Immediate), 1 = Tear-Free VSync (Fifo), 2 = Unlocked Immediate
    bool skip_present = false;       // Selective Turbo frame-skip: run GS draws into VRAM, skip swapchain blit/present
    bool photo_mode = false;         // F1 / F4 Photo Mode active
    bool photo_hide_hud = false;     // Hide 2D HUD / Judgment Ring / battle menu primitives during Photo Mode
    float photo_pan_x = 0.0f;        // -0.45 .. +0.45 UV pan
    float photo_pan_y = 0.0f;        // -0.45 .. +0.45 UV pan
    float photo_zoom = 1.0f;         //  0.50 ..  3.00 FOV / zoom
    float photo_roll_deg = 0.0f;     // -45.0 .. +45.0 Dutch tilt in degrees
    // Wave G: Next-gen pure-WGSL shaders & GS Mipmap LOD Bias
    int fsr_upscale = 0;             // 0 = Off, 1 = FSR 1.0 Quality (EASU+RCAS), 2 = FSR 1.0 Ultra
    float cas_sharpen = 0.0f;        //  0.00 ..  1.00 AMD Contrast-Adaptive Sharpening
    int aa_mode = 0;                 // 0 = Off, 1 = FXAA-Fast, 2 = FXAA-Quality
    float color_temp = 0.0f;         // -1.00 (Cool Moonlight) .. +1.00 (Warm Candlelight)
    float film_grain = 0.0f;         //  0.00 ..  1.00 subtle animated cinema grain
    int crt_mask = 0;                // 0 = Off, 1 = Aperture Grille, 2 = Shadow Mask
    float lod_bias = -0.5f;          // -2.00 .. +2.00 GS Mipmap LOD bias
};
float effective_target_aspect(const VideoParams &params, float fallback_window_aspect = 16.0f / 9.0f);
float effective_hud_pillar_scale(const VideoParams &params, float fallback_window_aspect = 16.0f / 9.0f);
void set_video_params(const VideoParams &params);
VideoParams video_params();
bool true_widescreen_active();
std::uint32_t true_widescreen_scale_bits();
std::uint32_t true_widescreen_clip_bits();
bool photo_hide_hud_active();
float hud_pillar_scale();
std::int32_t lod_bias_fixed();

// True while IPU/FMV video decoding is active (auto-holds 60 Hz so movies never desync).
bool fmv_active();
struct FmvStreamInfo {
    bool active = false;
    std::uint64_t stream_hash = 0;  // 64-bit hash of first 4 KiB starting at MPEG-2 sequence header (00 00 01 B3)
    std::uint64_t frame_index = 0;  // MPEG-2 picture count decoded so far in this cutscene
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double fps = 29.97;
};
FmvStreamInfo fmv_stream_info();
void set_fmv_dump_enabled(bool on, const char *dumps_dir = nullptr);
bool fmv_dump_enabled();
// Hardware SPU2 channel gains (Core 0 / ADMA music & voices vs Core 1 sound effects), 0.0..1.0.
void set_audio_channel_gains(float bgm_gain, float sfx_gain);
}
