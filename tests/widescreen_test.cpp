#include "ps2/video.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

bool near_f(std::uint32_t bits, float expected, float eps = 1e-5f) {
    const float got = std::bit_cast<float>(bits);
    return std::fabs(got - expected) <= eps;
}

}  // namespace

int main() {
    // Stock defaults before any set_video_params() call must be neutral 4:3 (1.0f scale, 2.0f clip).
    check(!ps2::true_widescreen_active(), "default: inactive before set_video_params");
    check(ps2::true_widescreen_scale_bits() == 0x3F800000u, "default: scale is 1.0f");
    check(ps2::true_widescreen_clip_bits() == 0x40000000u, "default: clip is 2.0f");

    ps2::VideoParams vp;

    // 4:3 Classic (even with true_widescreen=true)
    vp.aspect_mode = 0;
    vp.true_widescreen = true;
    ps2::set_video_params(vp);
    check(!ps2::true_widescreen_active(), "4:3: inactive");
    check(ps2::true_widescreen_scale_bits() == 0x3F800000u, "4:3: scale is 1.0f");
    check(ps2::true_widescreen_clip_bits() == 0x40000000u, "4:3: clip is 2.0f");

    // 16:9 Widescreen with true_widescreen=false
    vp.aspect_mode = 1;
    vp.true_widescreen = false;
    ps2::set_video_params(vp);
    check(!ps2::true_widescreen_active(), "16:9 opt-out: inactive");
    check(ps2::true_widescreen_scale_bits() == 0x3F800000u, "16:9 opt-out: scale is 1.0f");
    check(ps2::true_widescreen_clip_bits() == 0x40000000u, "16:9 opt-out: clip is 2.0f");

    // 16:9 Widescreen (bit-exact match with original 0.75f / 1.5f constants)
    vp.aspect_mode = 1;
    vp.true_widescreen = true;
    ps2::set_video_params(vp);
    check(ps2::true_widescreen_active(), "16:9: active");
    check(ps2::true_widescreen_scale_bits() == 0x3F400000u, "16:9: scale is bit-exact 0.75f");
    check(ps2::true_widescreen_clip_bits() == 0x3FC00000u, "16:9: clip is bit-exact 1.5f");

    // 16:10 Widescreen
    vp.aspect_mode = 4;
    ps2::set_video_params(vp);
    check(ps2::true_widescreen_active(), "16:10: active");
    check(near_f(ps2::true_widescreen_scale_bits(), (4.0f / 3.0f) / 1.6f), "16:10: scale is 5/6");
    check(near_f(ps2::true_widescreen_clip_bits(), 2.0f * (4.0f / 3.0f) / 1.6f), "16:10: clip is 2 * scale");

    // 21:9 Ultrawide
    vp.aspect_mode = 5;
    ps2::set_video_params(vp);
    check(ps2::true_widescreen_active(), "21:9: active");
    check(near_f(ps2::true_widescreen_scale_bits(), (4.0f / 3.0f) / (21.0f / 9.0f)), "21:9: scale is 4/7");
    check(near_f(ps2::true_widescreen_clip_bits(), 2.0f * (4.0f / 3.0f) / (21.0f / 9.0f)), "21:9: clip is 8/7");

    // 32:9 Super-Ultrawide
    vp.aspect_mode = 6;
    ps2::set_video_params(vp);
    check(ps2::true_widescreen_active(), "32:9: active");
    check(near_f(ps2::true_widescreen_scale_bits(), 0.375f), "32:9: scale is 0.375f");
    check(near_f(ps2::true_widescreen_clip_bits(), 0.75f), "32:9: clip is 0.75f");

    // Auto (Match Window) on 3440x1440 ultrawide monitor
    vp.aspect_mode = 3;
    vp.window_aspect = 3440.0f / 1440.0f;
    ps2::set_video_params(vp);
    const float expected_uw = (4.0f / 3.0f) / (3440.0f / 1440.0f);
    check(ps2::true_widescreen_active(), "Auto 3440x1440: active");
    check(near_f(ps2::true_widescreen_scale_bits(), expected_uw), "Auto 3440x1440: scale matches (4/3)/window_aspect");
    check(near_f(ps2::true_widescreen_clip_bits(), 2.0f * expected_uw), "Auto 3440x1440: clip matches 2*scale");

    return g_failed ? 1 : 0;
}
