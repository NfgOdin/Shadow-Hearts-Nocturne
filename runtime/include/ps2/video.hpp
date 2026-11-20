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
    int aspect_mode = 0;      //  0 = 4:3 Classic, 1 = 16:9 Widescreen, 2 = Stretch to Fill
    bool deinterlace = true;  //  true = weave deinterlace on, false = off
    // Phase 2: Depth-aware lighting & post-processing (all off by default for authentic PS2 visuals)
    float ssao = 0.0f;        //  0.00 ..  1.00 (Depth-aware ambient occlusion)
    float bloom = 0.0f;       //  0.00 ..  1.00 (HDR glow on magic/lights/Judgment Ring)
    float ssgi = 0.0f;        //  0.00 ..  1.00 (Depth-aware indirect light bounce)
    bool filmic = false;      //  ACES filmic tone mapping curve
    float vignette = 0.0f;    //  0.00 ..  1.00 (Gothic edge vignette)
};
void set_video_params(const VideoParams &params);
VideoParams video_params();

// High-FPS speed sync: when running above 60 Hz with speed sync enabled, frame_cycles() scales to keep the
// emulated IOP/SPU2 audio clock at exact 48 kHz real-time while the game's internal timestep is scaled.
void set_fps_sync(double target_fps, bool speed_sync);
double fps_sync_target();
bool fps_sync_enabled();
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
