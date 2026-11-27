#include "game_profile.hpp"
#include "quickmenu.hpp"
#include "screenshot.hpp"
#include "ui_draw.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace quickmenu {
namespace {

const char *game_display_title() {
    return nocturne::active_ps2_game_title();
}

// Main Hub (10 items across Shadow Hearts 1, Covenant, and From the New World)
constexpr int kMainEntries = 10;
constexpr Command kMainCmds[kMainEntries] = {
    Command::resume, Command::save, Command::load, Command::open_save_states, Command::photo_mode, Command::screenshot, Command::open_achievements, Command::open_mods, Command::open_settings, Command::quit
};
constexpr const char *kMainKeys[kMainEntries] = {"F1", "F5", "F8", "F6 / F7", "F4", "F12", "", "", "", ""};

// Settings > Graphics Tab (48 items + Back = 49)
constexpr int kGfxEntries = 49;
constexpr Command kGfxCmds[kGfxEntries] = {
    Command::gfx_preset, Command::scale_pow2, Command::scale_custom_begin, Command::adaptive_scale, Command::fps_cap, Command::fps_speed_sync, Command::present_mode,
    Command::look, Command::sharp_2d, Command::sharp_3d, Command::anisotropy, Command::fsr_upscale, Command::cas_sharpen, Command::aa_mode, Command::lod_bias,
    Command::aspect, Command::true_widescreen, Command::hud_anchor, Command::brightness, Command::contrast,
    Command::saturation, Command::color_temp, Command::gamma, Command::scanlines, Command::crt_mask, Command::film_grain, Command::deinterlace,
    Command::ssao, Command::bloom, Command::ssgi, Command::filmic, Command::vignette,
    Command::toast_theme, Command::gpu_adapter, Command::gpu_api, Command::gpu_submit, Command::gpu_binning,
    Command::hd_textures, Command::hd_fmv, Command::tex_dump, Command::fmv_dump, Command::reload_packs,
    Command::fullscreen, Command::perf_hud, Command::ring_hud, Command::strict_ring, Command::title_perf, Command::reset_graphics, Command::none
};

// Settings > Audio Tab (7 items + Back = 8)
constexpr int kAudEntries = 8;
constexpr Command kAudCmds[kAudEntries] = {
    Command::master_volume, Command::mute, Command::audio_device, Command::audio_stretch, Command::chime_volume,
    Command::bgm_volume, Command::sfx_volume, Command::none
};

// Settings > Controls & Co-op Tab (7 core + 10 Wave E Co-op + Back = 18)
constexpr int kCtlEntries = 18;
constexpr Command kCtlCmds[kCtlEntries] = {
    Command::fast_disc, Command::switch_disc, Command::slot_picker_toggle, Command::autosave,
    Command::turbo_speed, Command::keys, Command::change_keys,
    Command::coop_pass_control, Command::coop_resync,
    Command::coop_slot_1, Command::coop_slot_2, Command::coop_slot_3, Command::coop_slot_4,
    Command::coop_auto_turn, Command::coop_relay_combos, Command::coop_hud_toggle, Command::coop_ping_toggle,
    Command::none
};

constexpr int kSettingsVisible = 11;
constexpr int kKeysVisible = 10;

// ---- Shared Nocturne Gothic Palette & Chrome Primitives ----

namespace pal {
constexpr std::uint32_t kBgTop        = ui_draw::rgba(13, 12, 24, 246);
constexpr std::uint32_t kBgBot        = ui_draw::rgba(24, 17, 36, 246);
constexpr std::uint32_t kGlowGold     = ui_draw::rgba(186, 130, 56, 32);
constexpr std::uint32_t kGoldFrame    = ui_draw::rgba(196, 154, 78, 255);
constexpr std::uint32_t kGoldHair     = ui_draw::rgba(244, 210, 128, 68);
constexpr std::uint32_t kDivider      = ui_draw::rgba(156, 124, 74, 115);
constexpr std::uint32_t kTitle        = ui_draw::rgba(246, 212, 126, 255);
constexpr std::uint32_t kText         = ui_draw::rgba(234, 236, 244, 252);
constexpr std::uint32_t kTextSoft     = ui_draw::rgba(184, 190, 212, 240);
constexpr std::uint32_t kDim          = ui_draw::rgba(146, 154, 180, 232);
constexpr std::uint32_t kRowIdleBg    = ui_draw::rgba(22, 22, 38, 170);
constexpr std::uint32_t kRowIdleBdr   = ui_draw::rgba(82, 86, 116, 120);
constexpr std::uint32_t kRowLitLeft   = ui_draw::rgba(116, 36, 48, 240);
constexpr std::uint32_t kRowLitRight  = ui_draw::rgba(172, 122, 46, 236);
constexpr std::uint32_t kRowLitBdr    = ui_draw::rgba(248, 216, 132, 255);
constexpr std::uint32_t kDangerLeft   = ui_draw::rgba(146, 28, 38, 246);
constexpr std::uint32_t kDangerRight  = ui_draw::rgba(198, 66, 52, 242);
constexpr std::uint32_t kDangerBdr    = ui_draw::rgba(255, 168, 146, 255);
constexpr std::uint32_t kCardBgTop    = ui_draw::rgba(18, 17, 32, 242);
constexpr std::uint32_t kCardBgBot    = ui_draw::rgba(26, 22, 42, 242);
constexpr std::uint32_t kCardBdr      = ui_draw::rgba(136, 114, 78, 190);
constexpr std::uint32_t kOnBg         = ui_draw::rgba(24, 96, 74, 244);
constexpr std::uint32_t kOnBdr        = ui_draw::rgba(116, 234, 186, 255);
constexpr std::uint32_t kOnText       = ui_draw::rgba(218, 255, 238, 255);
constexpr std::uint32_t kOffBg        = ui_draw::rgba(28, 30, 48, 236);
constexpr std::uint32_t kOffBdr       = ui_draw::rgba(98, 106, 136, 200);
constexpr std::uint32_t kKeyBg        = ui_draw::rgba(20, 22, 36, 238);
constexpr std::uint32_t kKeyBdr       = ui_draw::rgba(146, 122, 82, 205);
constexpr std::uint32_t kEmerald      = ui_draw::rgba(130, 242, 194, 255);
}  // namespace pal

void draw_nocturne_panel(ui_draw::Surface &surf, int w, int h, float sc) {
    const float rad = std::max(8.0f, 14.0f * sc);
    surf.fill_gradient_rounded_rect(0, 0, w, h, rad, pal::kBgTop, pal::kBgBot, 0.22f, false);
    surf.set_clip(2, 2, w - 4, h - 4);
    surf.fill_radial_glow(static_cast<float>(w) * 0.5f, -static_cast<float>(h) * 0.14f,
                          static_cast<float>(w) * 0.65f, pal::kGlowGold, ui_draw::with_alpha(pal::kGlowGold, 0));
    surf.reset_clip();
    surf.stroke_rounded_rect(0, 0, w, h, rad, std::max(1.5f, 2.0f * sc), pal::kGoldFrame);
    surf.stroke_rounded_rect(3, 3, w - 6, h - 6, std::max(4.0f, rad - 3.0f), 1.0f, pal::kGoldHair);
}

void draw_nocturne_row(ui_draw::Surface &surf, int x, int y, int w, int h, float sc, bool lit, bool danger = false, bool subtle = false) {
    const float rad = std::max(4.0f, 7.0f * sc);
    if (lit) {
        surf.fill_gradient_rounded_rect(x, y, w, h, rad,
                                        danger ? pal::kDangerLeft : pal::kRowLitLeft,
                                        danger ? pal::kDangerRight : pal::kRowLitRight, 0.18f, true);
        surf.stroke_rounded_rect(x, y, w, h, rad, std::max(1.2f, 1.5f * sc), danger ? pal::kDangerBdr : pal::kRowLitBdr);
        const int bar_w = std::max(3, static_cast<int>(3.5f * sc + 0.5f));
        surf.fill_rounded_rect(x + 2, y + 3, bar_w, std::max(4, h - 6), 1.5f, danger ? pal::kDangerBdr : pal::kTitle);
    } else if (!subtle) {
        surf.fill_rounded_rect(x, y, w, h, rad, pal::kRowIdleBg);
        surf.stroke_rounded_rect(x, y, w, h, rad, 1.0f, pal::kRowIdleBdr);
    } else {
        surf.fill_rounded_rect(x, y, w, h, rad, ui_draw::rgba(18, 19, 32, 130));
        surf.stroke_rounded_rect(x, y, w, h, rad, 1.0f, ui_draw::rgba(72, 76, 106, 85));
    }
}

void draw_toggle_widget(ui_draw::Surface &surf, int right_x, int cy, int h, bool on, bool lit, float sc) {
    const int sw_h = std::max(16, std::min(h - 6, static_cast<int>(22.0f * sc + 0.5f)));
    const int sw_w = std::max(46, static_cast<int>(54.0f * sc + 0.5f));
    const int sx = right_x - sw_w;
    const int sy = cy - sw_h / 2;
    const float rad = static_cast<float>(sw_h) * 0.5f;

    const std::uint32_t bg = on ? pal::kOnBg : pal::kOffBg;
    const std::uint32_t bdr = on ? pal::kOnBdr : (lit ? pal::kRowLitBdr : pal::kOffBdr);
    surf.fill_rounded_rect(sx, sy, sw_w, sw_h, rad, bg);
    surf.stroke_rounded_rect(sx, sy, sw_w, sw_h, rad, 1.15f, bdr);

    const float knob_r = std::max(5.0f, static_cast<float>(sw_h - 6) * 0.5f);
    const float kx = on ? static_cast<float>(sx + sw_w - 3) - knob_r : static_cast<float>(sx + 3) + knob_r;
    const float ky = static_cast<float>(sy) + static_cast<float>(sw_h) * 0.5f;
    surf.fill_circle(kx, ky, knob_r, on ? pal::kOnText : ui_draw::rgba(176, 184, 206, 240));

    const int lbl_px = std::max(9, static_cast<int>(10.5f * sc + 0.5f));
    const std::string lbl = on ? "ON" : "OFF";
    const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, true, lbl);
    const int lx = on ? (sx + std::max(6, static_cast<int>(7.0f * sc))) : (sx + sw_w - tm.width - std::max(6, static_cast<int>(7.0f * sc)));
    ui_draw::draw_text(surf, lx, cy - tm.height / 2, ui_draw::FontStyle::Sans, lbl_px, true, lbl,
                       on ? pal::kOnText : pal::kDim, false);
}

void draw_slider_widget(ui_draw::Surface &surf, int right_x, int cy, int bar_w, float ratio,
                        const std::string &val_text, bool lit, float sc) {
    const int val_px = std::max(10, static_cast<int>(12.5f * sc + 0.5f));
    const int val_w = std::max(46, static_cast<int>(58.0f * sc + 0.5f));
    const auto vm = ui_draw::measure_text(ui_draw::FontStyle::Sans, val_px, true, val_text);
    ui_draw::draw_text(surf, right_x - vm.width, cy - vm.height / 2, ui_draw::FontStyle::Sans, val_px, true, val_text,
                       lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kTitle, false);

    const int bx = right_x - val_w - bar_w;
    const int bh = std::max(7, static_cast<int>(9.0f * sc + 0.5f));
    const int by = cy - bh / 2;
    const float cl = std::clamp(ratio, 0.0f, 1.0f);
    surf.draw_progress_bar(bx, by, bar_w, bh, cl,
                           ui_draw::rgba(16, 18, 32, 240),
                           ui_draw::rgba(176, 56, 62, 255),
                           ui_draw::rgba(248, 206, 108, 255),
                           lit ? pal::kRowLitBdr : ui_draw::rgba(116, 104, 84, 200));
    const float kx = static_cast<float>(bx) + static_cast<float>(bar_w) * cl;
    const float kr = std::max(4.5f, static_cast<float>(bh) * 0.72f);
    surf.fill_circle(kx, static_cast<float>(cy), kr, lit ? ui_draw::rgba(255, 242, 198, 255) : pal::kTitle);
    surf.stroke_circle(kx, static_cast<float>(cy), kr, 1.0f, ui_draw::rgba(46, 28, 22, 220));
}

int draw_selector_pill(ui_draw::Surface &surf, int right_x, int cy, int h, const std::string &val_text,
                       bool lit, bool locked, int max_w, float sc) {
    const int pill_h = std::max(18, std::min(h - 6, static_cast<int>(24.0f * sc + 0.5f)));
    const int px_sz = std::max(10, static_cast<int>(12.0f * sc + 0.5f));
    const std::string display = locked ? val_text : ("<  " + val_text + "  >");
    const int pad_x = std::max(14, static_cast<int>(18.0f * sc + 0.5f));
    const std::string fitted = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, px_sz, true, display, std::max(48, max_w - pad_x));
    const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, px_sz, true, fitted);
    const int pw = std::min(max_w, tm.width + pad_x);
    const int px = right_x - pw;
    const int py = cy - pill_h / 2;
    const std::uint32_t bg = locked ? ui_draw::rgba(24, 26, 40, 210) : (lit ? ui_draw::rgba(36, 20, 30, 240) : pal::kKeyBg);
    const std::uint32_t bdr = locked ? pal::kOffBdr : (lit ? pal::kRowLitBdr : pal::kKeyBdr);
    const std::uint32_t tc = locked ? pal::kDim : (lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kText);
    ui_draw::draw_pill(surf, px, py, pill_h, ui_draw::FontStyle::Sans, px_sz, true, fitted, bg, bdr, tc);
    return pw;
}

int draw_keycap_pill(ui_draw::Surface &surf, int right_x, int cy, int h, const std::string &text, bool lit, float sc) {
    if (text.empty()) return 0;
    const int pill_h = std::max(17, std::min(h - 6, static_cast<int>(22.0f * sc + 0.5f)));
    const int px_sz = std::max(9, static_cast<int>(11.0f * sc + 0.5f));
    const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, px_sz, true, text);
    const int pw = tm.width + std::max(14, static_cast<int>(16.0f * sc + 0.5f));
    const int px = right_x - pw;
    const int py = cy - pill_h / 2;
    const std::uint32_t bg = lit ? ui_draw::rgba(32, 18, 26, 242) : pal::kKeyBg;
    const std::uint32_t bdr = lit ? pal::kRowLitBdr : pal::kKeyBdr;
    const std::uint32_t tc = lit ? pal::kTitle : pal::kTextSoft;
    ui_draw::draw_pill(surf, px, py, pill_h, ui_draw::FontStyle::Sans, px_sz, true, text, bg, bdr, tc);
    return pw;
}

void draw_gothic_scrollbar(ui_draw::Surface &surf, int sb_x, int track_y, int sb_w, int track_h,
                           int total_items, int visible_items, int top_index) {
    surf.fill_rounded_rect(sb_x, track_y, sb_w, track_h, static_cast<float>(sb_w) * 0.5f, ui_draw::rgba(14, 15, 26, 220));
    if (total_items > visible_items && visible_items > 0) {
        const int thumb_h = std::max(24, track_h * visible_items / total_items);
        const int max_top = std::max(1, total_items - visible_items);
        const int thumb_y = track_y + static_cast<int>(static_cast<float>(track_h - thumb_h) *
                                                       static_cast<float>(top_index) / static_cast<float>(max_top) + 0.5f);
        surf.fill_gradient_rounded_rect(sb_x + 1, thumb_y, sb_w - 2, thumb_h, static_cast<float>(sb_w - 2) * 0.5f,
                                        ui_draw::rgba(186, 144, 66, 240), ui_draw::rgba(244, 208, 122, 240), 0.1f, false);
    } else if (total_items > 0) {
        surf.fill_rounded_rect(sb_x + 1, track_y + 2, sb_w - 2, std::max(4, track_h - 4),
                               static_cast<float>(sb_w - 2) * 0.5f, ui_draw::rgba(92, 98, 128, 140));
    }
}

bool is_side_command(Command c) {
    switch (c) {
    case Command::gfx_preset: case Command::scale_pow2: case Command::adaptive_scale: case Command::fps_cap: case Command::fps_speed_sync: case Command::present_mode:
    case Command::look: case Command::sharp_2d: case Command::sharp_3d: case Command::anisotropy:
    case Command::fsr_upscale: case Command::cas_sharpen: case Command::aa_mode: case Command::lod_bias:
    case Command::aspect: case Command::true_widescreen: case Command::hud_anchor: case Command::brightness:
    case Command::contrast: case Command::saturation: case Command::color_temp: case Command::gamma: case Command::scanlines:
    case Command::crt_mask: case Command::film_grain:
    case Command::deinterlace: case Command::ssao: case Command::bloom: case Command::ssgi:
    case Command::filmic: case Command::vignette: case Command::toast_theme:
    case Command::gpu_adapter: case Command::gpu_api: case Command::gpu_submit: case Command::gpu_binning:
    case Command::hd_textures: case Command::hd_fmv: case Command::tex_dump: case Command::fmv_dump:
    case Command::fullscreen: case Command::perf_hud: case Command::ring_hud: case Command::strict_ring: case Command::title_perf:
    case Command::master_volume: case Command::mute: case Command::audio_device: case Command::audio_stretch:
    case Command::chime_volume: case Command::bgm_volume: case Command::sfx_volume:
    case Command::fast_disc: case Command::switch_disc: case Command::slot_picker_toggle: case Command::autosave: case Command::turbo_speed:
        return true;
    default:
        return false;
    }
}

enum class WidgetKind { action, toggle, slider, selector, text_input };

struct SettingRowInfo {
    std::string label;
    std::string value;
    std::string badge;
    std::string help;
    WidgetKind kind = WidgetKind::action;
    bool toggle_on = false;
    float slider_ratio = 0.0f;
    bool locked = false;
    bool danger = false;
};

SettingRowInfo describe_setting_row(SettingsTab tab, int i, const Values &v, bool typing_scale, const std::string &scale_input) {
    SettingRowInfo r;
    if (tab == SettingsTab::graphics) {
        if (i == kGfxEntries - 1) {
            r.label = "Back to Pause Menu";
            r.value = "Esc";
            r.help = "Return to the main pause menu.";
            return r;
        }
        switch (kGfxCmds[i]) {
        case Command::gfx_preset: {
            static const char *const kPresets[] = {"Nocturne Turbo (Vulkan)", "Balanced 1080p", "Authentic PS2", "4K Ultra"};
            r.label = "1-Click Performance Preset";
            r.kind = WidgetKind::selector;
            r.value = kPresets[std::clamp(v.preset_index, 0, 3)];
            r.badge = "Enter = Apply";
            r.help = "Press Left/Right to pick a preset, or Enter to apply it immediately (Nocturne Turbo = Vulkan + 16:9 Hor+ + SSAO 20).";
            return r;
        }
        case Command::scale_pow2: {
            r.label = "Internal Resolution (2^n)";
            r.kind = WidgetKind::selector;
            r.locked = v.scale_locked;
            const int sc = v.scale_locked ? v.scale_now : v.scale_next;
            r.value = std::to_string(sc) + "x  " + std::to_string(512 * sc) + "x" + std::to_string(448 * sc);
            r.badge = v.scale_locked ? "PS2_SCALE"
                : ("~" + Menu::format_bytes(v.vram_bytes_next) + (sc == v.scale_now ? " (live)" : (sc <= v.scale_live_limit ? " (live)" : " (next start)")));
            r.help = "Internal PS2 GS multiplier. Scales up to your launch VRAM buffer switch live immediately; higher scales apply next launch.";
            return r;
        }
        case Command::scale_custom_begin: {
            r.label = "Custom Resolution Scale (1-" + std::to_string(v.scale_max) + ")";
            r.kind = WidgetKind::text_input;
            r.locked = v.scale_locked;
            if (v.scale_locked) {
                r.value = "Locked";
                r.badge = "PS2_SCALE";
            } else if (typing_scale) {
                const int typed = scale_input.empty() ? 0 : std::atoi(scale_input.c_str());
                r.value = scale_input + "_";
                if (typed >= 1) r.badge = std::to_string(512 * typed) + "x" + std::to_string(448 * typed) + " (~" + Menu::format_bytes(v.vram_bytes_custom) + ")";
            } else {
                r.value = "Enter to type  [" + std::to_string(v.scale_next) + "x]";
            }
            r.help = typing_scale ? "Type digits 0-9, Backspace to erase, Enter to apply live, or Esc to cancel."
                                  : "Press Enter to type any integer scale from 1x up to your GPU's max safe multiplier.";
            return r;
        }
        case Command::adaptive_scale:
            r.label = "Adaptive Battle Scale";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.adaptive_scale;
            r.badge = v.adaptive_scale ? "auto 4x->3x" : "";
            r.help = "Temporarily trims 4x scale to 3x during heavy multi-spell battles to keep 60 FPS locked.";
            return r;
        case Command::fps_cap:
            r.label = "Frame Rate Limit";
            r.kind = WidgetKind::selector;
            r.value = v.fps_cap_label;
            r.help = "60 FPS is native PS2 speed (recommended). Caps above 60 FPS speed up VBlank-locked gameplay.";
            return r;
        case Command::fps_speed_sync:
            r.label = "FMV 60 FPS Lock";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.fps_speed_sync;
            r.help = "Auto-holds native 60/50 Hz during FMV cutscenes when running above 60 FPS / fast-forward.";
            return r;
        case Command::present_mode:
            r.label = "VSync / Present Mode";
            r.kind = WidgetKind::selector;
            r.value = v.present_mode;
            r.help = "Mailbox gives tear-free low latency; Fifo enforces strict VSync; Immediate minimizes input lag.";
            return r;
        case Command::look:
            r.label = "Picture Filter / Look";
            r.kind = WidgetKind::selector;
            r.value = v.look;
            r.help = "Controls presentation filtering and edge sharpening applied to the final frame.";
            return r;
        case Command::sharp_2d: {
            static const char *const kNames[] = {"Soft", "Sharp", "Sharpest"};
            const auto name = [](int n) { return kNames[n >= 0 && n <= 2 ? n : 1]; };
            r.label = "2D Text & UI Sharpness";
            r.kind = WidgetKind::selector;
            r.locked = v.sharp_locked;
            r.value = name(v.sharp_locked ? v.sharp_now : v.sharp_next);
            r.badge = v.sharp_locked ? "PS2_SHARP_2D" : (v.sharp_next != v.sharp_now ? "next start" : "");
            r.help = "Controls 2D UI, Judgment Ring, and Shift-JIS dialogue text sampling sharpness.";
            return r;
        }
        case Command::sharp_3d:
            r.label = "3D Texture Filter (Sharp-Bilinear)";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.sharp_3d_next;
            r.badge = (v.sharp_3d_next != v.sharp_3d_now) ? "next start" : "";
            r.help = "Smooth Hermite 3D bilinear sampling for crisper floors, walls, and models at oblique angles (zero FPS cost).";
            return r;
        case Command::anisotropy:
            r.label = "Anisotropic Filtering";
            r.kind = WidgetKind::selector;
            r.value = v.anisotropy <= 1 ? "Off (1x)" : (std::to_string(v.anisotropy) + "x Anisotropic");
            r.help = "Hardware 16x anisotropic texture filtering for razor-sharp oblique surfaces at high resolution.";
            return r;
        case Command::fsr_upscale:
            r.label = "AMD FSR 1.0 Spatial Upscaling";
            r.kind = WidgetKind::selector;
            r.value = v.fsr_upscale == 2 ? "Ultra (EASU+RCAS)" : (v.fsr_upscale == 1 ? "Quality (EASU+RCAS)" : "Off");
            r.help = "Pure-WGSL edge-adaptive Lanczos spatial upscaling + sharpening (NVIDIA, AMD & Intel, zero ghosting).";
            return r;
        case Command::cas_sharpen:
            r.label = "AMD CAS Sharpening";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.cas_sharpen) / 100.0f;
            r.value = v.cas_sharpen == 0 ? "Off" : (std::to_string(v.cas_sharpen) + "%");
            r.help = "Contrast-Adaptive Sharpening for crisp upscaled 3D textures and HD packs without ringing.";
            return r;
        case Command::aa_mode:
            r.label = "FXAA Edge Anti-Aliasing";
            r.kind = WidgetKind::selector;
            r.value = v.aa_mode == 2 ? "FXAA Quality" : (v.aa_mode == 1 ? "FXAA Fast" : "Off");
            r.help = "Luminance-aware subpixel edge smoothing to eliminate jagged stair-stepping on gothic silhouettes.";
            return r;
        case Command::lod_bias: {
            char lbuf[24];
            std::snprintf(lbuf, sizeof lbuf, "%+.1f", static_cast<double>(v.lod_bias) * 0.1);
            r.label = "Mipmap LOD Bias";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.lod_bias + 20) / 40.0f;
            r.value = lbuf;
            r.help = "Negative values (-0.5 default) select crisper higher-resolution mipmap levels on angled surfaces.";
            return r;
        }
        case Command::color_temp:
            r.label = "Gothic Color Temperature";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.color_temp + 50) / 100.0f;
            r.value = v.color_temp == 0 ? "Neutral" : (v.color_temp < 0 ? ("Cool " + std::to_string(v.color_temp)) : ("Warm +" + std::to_string(v.color_temp)));
            r.help = "Shifts scene mood from Cool Moonlight (-50) to Warm Candlelight (+50).";
            return r;
        case Command::crt_mask:
            r.label = "CRT Phosphor Mask";
            r.kind = WidgetKind::selector;
            r.value = v.crt_mask == 2 ? "Shadow Mask" : (v.crt_mask == 1 ? "Aperture Grille" : "Off");
            r.help = "Subtle RGB phosphor triad mask that pairs cleanly with CRT Scanlines.";
            return r;
        case Command::film_grain:
            r.label = "Cinema Film Grain";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.film_grain) / 100.0f;
            r.value = v.film_grain == 0 ? "Off" : (std::to_string(v.film_grain) + "%");
            r.help = "Subtle animated horror cinema film grain for atmospheric gothic presentation.";
            return r;
        case Command::aspect: {
            static const char *const kAspect[] = {
                "4:3 Classic", "16:9 Widescreen", "Stretch to Fill",
                "Auto (Match Window)", "16:10 Widescreen", "21:9 Ultrawide", "32:9 Super-Ultrawide"
            };
            r.label = "Aspect Ratio";
            r.kind = WidgetKind::selector;
            r.value = kAspect[std::clamp(v.aspect_mode, 0, 6)];
            r.help = "Choose Auto Window Match, 4:3, 16:10, 16:9, 21:9 Ultrawide, 32:9 Super-Ultrawide, or Stretch.";
            return r;
        }
        case Command::true_widescreen:
            r.label = "True Hor+ Widescreen Camera";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.true_widescreen;
            r.badge = (v.aspect_mode != 0 && v.true_widescreen) ? "active" : "";
            r.help = "Dynamically expands 3D camera FOV & clip frustum to match 16:10, 16:9, 21:9, 32:9, or Auto window aspect.";
            return r;
        case Command::hud_anchor: {
            static const char *const kAnchor[] = {
                "Off (Stretched HUD)",
                "Ultrawide Only (21:9 / 32:9)",
                "All Widescreen (16:10+)"
            };
            r.label = "2D HUD & Judgment Ring Pillar-Anchor";
            r.kind = WidgetKind::selector;
            r.value = kAnchor[std::clamp(v.hud_anchor_mode, 0, 2)];
            r.help = "Keeps the Judgment Ring perfectly circular and 2D HUD centered in 4:3 while 3D worlds fill the screen.";
            return r;
        }
        case Command::brightness:
            r.label = "Brightness";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.brightness + 50) / 100.0f;
            r.value = std::string(v.brightness > 0 ? "+" : "") + std::to_string(v.brightness) + "%";
            r.help = "Adjusts overall image luminance offset (-50% to +50%).";
            return r;
        case Command::contrast:
            r.label = "Contrast";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.contrast - 50) / 100.0f;
            r.value = std::to_string(v.contrast) + "%";
            r.help = "Adjusts separation between deep shadows and bright highlights (50% to 150%).";
            return r;
        case Command::saturation:
            r.label = "Color Saturation";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.saturation) / 200.0f;
            r.value = std::to_string(v.saturation) + "%";
            r.help = "Adjusts color richness (0% monochrome, 100% original, up to 200% vivid).";
            return r;
        case Command::gamma: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v.gamma) / 100.0);
            r.label = "Gamma Curve";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.gamma - 50) / 150.0f;
            r.value = buf;
            r.help = "Fine-tunes midtone shadow brightness (1.00 = neutral PS2 reference).";
            return r;
        }
        case Command::scanlines:
            r.label = "CRT Scanlines";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.scanlines) / 100.0f;
            r.value = v.scanlines <= 0 ? "Off" : (std::to_string(v.scanlines) + "%");
            r.help = "Optional subtle CRT scanline overlay intensity (0% = clean modern pixels).";
            return r;
        case Command::deinterlace:
            r.label = "Deinterlacing";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.deinterlace;
            r.help = "Eliminates PS2 480i field weave jitter during menus and cutscenes.";
            return r;
        case Command::ssao:
            r.label = "SSAO (Depth Occlusion)";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.ssao) / 100.0f;
            r.value = v.ssao <= 0 ? "Off" : (std::to_string(v.ssao) + "%");
            r.help = "Adds depth-aware ambient contact shadows in corners and around characters.";
            return r;
        case Command::bloom:
            r.label = "Bloom / HDR Glow";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.bloom) / 100.0f;
            r.value = v.bloom <= 0 ? "Off" : (std::to_string(v.bloom) + "%");
            r.help = "Soft atmospheric halo around spells, lanterns, and bright highlights.";
            return r;
        case Command::ssgi:
            r.label = "SSGI Light Bounce";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.ssgi) / 100.0f;
            r.value = v.ssgi <= 0 ? "Off" : (std::to_string(v.ssgi) + "%");
            r.help = "Screen-space indirect color bounce lighting from bright surfaces.";
            return r;
        case Command::filmic:
            r.label = "Filmic Tone Mapping";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.filmic;
            r.help = "Applies cinematic highlight roll-off to prevent blown-out spell effects.";
            return r;
        case Command::vignette:
            r.label = "Gothic Vignette";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.vignette) / 100.0f;
            r.value = v.vignette <= 0 ? "Off" : (std::to_string(v.vignette) + "%");
            r.help = "Darkens the outer corners of the frame for a moodier gothic presentation.";
            return r;
        case Command::toast_theme:
            r.label = "Achievement Toast Theme";
            r.kind = WidgetKind::selector;
            r.value = achievements::toast_theme_name(v.toast_theme);
            r.help = "Visual palette used for in-game achievement unlock banners.";
            return r;
        case Command::gpu_adapter:
            r.label = "Graphics Adapter";
            r.kind = WidgetKind::selector;
            r.locked = v.gpu_adapter_locked;
            r.value = v.gpu_adapter;
            r.badge = v.gpu_adapter_locked ? "ENV" : (v.gpu_adapter_next ? "next start" : "");
            r.help = "Selects which GPU renders the game (takes effect on next launch).";
            return r;
        case Command::gpu_api:
            r.label = "Graphics API";
            r.kind = WidgetKind::selector;
            r.locked = v.gpu_api_locked;
            r.value = v.gpu_api;
            r.badge = v.gpu_api_locked ? "ENV" : (v.gpu_api_next ? "next start" : "");
            r.help = "Vulkan is fastest & smoothest on Windows (recommended); Direct3D 12 as alternative (next launch).";
            return r;
        case Command::gpu_submit:
            r.label = "Async GPU Submit";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.gpu_submit_on;
            r.help = "Overlaps CPU emulation with GPU command execution for smoother 60 FPS pacing.";
            return r;
        case Command::gpu_binning:
            r.label = "GPU Tile Binning";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.gpu_binning_on;
            r.help = "Accelerates Compute Rasterizer fallback via GPU tile binning (bypassed when HW Rasterizer is On).";
            return r;
        case Command::hd_textures:
            r.label = "HD Texture Packs";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.hd_textures_on;
            r.badge = v.hd_textures_detail;
            r.help = "Zero-hitch async 2x/4x/8x HD texture replacement loaded from mods/textures/.";
            return r;
        case Command::hd_fmv:
            r.label = "HD FMV Cutscenes";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.hd_fmv_on;
            r.badge = v.hd_fmv_detail;
            r.help = "Hardware-decoded .mp4 cutscene replacement from mods/fmv/ synced to PS2 audio.";
            return r;
        case Command::tex_dump:
            r.label = "Dump Textures (GPU)";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.tex_dump_on;
            r.badge = v.tex_dumped_count > 0 ? (std::to_string(v.tex_dumped_count) + " PNGs") : "";
            r.help = "Saves newly encountered static textures to dumps/textures/ for upscaling.";
            return r;
        case Command::fmv_dump:
            r.label = "Dump FMV Streams";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.fmv_dump_on;
            r.help = "Logs cutscene IDs and saves clean MPEG-2 streams to dumps/fmv/ during playback.";
            return r;
        case Command::reload_packs:
            r.label = "Reload HD Texture & FMV Packs";
            r.value = "Enter";
            r.help = "Hot-reloads newly added texture packs and HD .mp4 cutscenes from mods/ without restarting.";
            return r;
        case Command::fullscreen:
            r.label = "Full Screen";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.fullscreen;
            r.help = "Toggles borderless fullscreen display (also bound to F11 / Alt+Enter).";
            return r;
        case Command::perf_hud:
            r.label = "Fullscreen Corner FPS HUD (F10)";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.perf_hud;
            r.help = "Shows a compact live FPS, frame time, resolution, and Turbo indicator pill in the top-left corner.";
            return r;
        case Command::ring_hud:
            r.label = "Battle Judgment Ring Streak HUD";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.ring_hud;
            r.help = "Optional live battle telemetry pill showing Judgment Ring Hit %, Perfects, and current streak (SH2/SH3).";
            return r;
        case Command::strict_ring:
            r.label = "Strict Ring Challenge Badge";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.strict_ring;
            r.help = "Tracks an unbroken zero-miss Judgment Ring challenge streak live in the corner HUD.";
            return r;
        case Command::title_perf:
            r.label = "Titlebar FPS & Perf Telemetry";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.title_perf;
            r.help = "Shows live FPS, frame time, and audio sync metrics in the window titlebar.";
            return r;
        case Command::reset_graphics:
            r.label = "Reset Image Adjustments to Defaults";
            r.value = "Enter";
            r.danger = true;
            r.help = "Resets brightness, contrast, saturation, gamma, scanlines, and lighting sliders.";
            return r;
        default:
            return r;
        }
    }
    if (tab == SettingsTab::audio) {
        if (i == kAudEntries - 1) {
            r.label = "Back to Pause Menu";
            r.value = "Esc";
            r.help = "Return to the main pause menu.";
            return r;
        }
        switch (kAudCmds[i]) {
        case Command::master_volume:
            r.label = "Master Volume";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.master_volume) / 100.0f;
            r.value = std::to_string(v.master_volume) + "%";
            r.help = "Overall output volume scaling for music, voices, SFX, and UI chimes.";
            return r;
        case Command::mute:
            r.label = "Mute All Audio";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.muted;
            r.help = "Silences all game and UI audio immediately.";
            return r;
        case Command::audio_device:
            r.label = "Output Device";
            r.kind = WidgetKind::selector;
            r.value = v.audio_device;
            r.help = "Selects the Windows playback device (reopens audio stream live).";
            return r;
        case Command::audio_stretch:
            r.label = "Audio Time-Stretch";
            r.kind = WidgetKind::toggle;
            r.toggle_on = v.audio_stretch;
            r.help = "Smooths tiny frame-pacing variations to prevent audio pops and crackles.";
            return r;
        case Command::chime_volume:
            r.label = "Achievement & UI Chime";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.chime_volume) / 100.0f;
            r.value = std::to_string(v.chime_volume) + "%";
            r.help = "Volume of the gothic harmonic chime played when unlocking achievements.";
            return r;
        case Command::bgm_volume:
            r.label = "Music & Voices (SPU2 Core 0)";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.bgm_volume) / 100.0f;
            r.value = std::to_string(v.bgm_volume) + "%";
            r.help = "Independent volume control for soundtrack and voiced dialogue.";
            return r;
        case Command::sfx_volume:
            r.label = "Sound Effects (SPU2 Core 1)";
            r.kind = WidgetKind::slider;
            r.slider_ratio = static_cast<float>(v.sfx_volume) / 100.0f;
            r.value = std::to_string(v.sfx_volume) + "%";
            r.help = "Independent volume control for Judgment Ring hits, spells, and menu SFX.";
            return r;
        default:
            return r;
        }
    }
    if (i == kCtlEntries - 1) {
        r.label = "Back to Pause Menu";
        r.value = "Esc";
        r.help = "Return to the main pause menu.";
        return r;
    }
    switch (kCtlCmds[i]) {
    case Command::fast_disc:
        r.label = "Fast Disc Loading (SSD)";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.fast_disc;
        r.help = "Eliminates PS2 DVD seek delays for near-instant area transitions and battles.";
        return r;
    case Command::switch_disc:
        r.label = "Active Disc";
        r.kind = WidgetKind::selector;
        r.locked = (v.disc_count <= 1);
        r.value = v.disc_count <= 1
            ? "Disc 1 (single disc)"
            : ("Disc " + std::to_string(v.active_disc + 1) + " of " + std::to_string(v.disc_count));
        r.help = "Manually swap between Disc 1 and Disc 2 (NTSC-U also swaps automatically at the disc break).";
        return r;
    case Command::slot_picker_toggle:
        r.label = "F5 / F8 Opens Slot Picker";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.slot_picker_on_quickkey;
        r.help = "When ON, F5/F8 opens the visual 0-9 slot picker instead of saving/loading the active slot instantly.";
        return r;
    case Command::autosave:
        r.label = "Area-Transition Autosave";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.autosave;
        r.help = "Automatically writes a rolling Autosave (separate from Slots 0-9) when entering a new area.";
        return r;
    case Command::turbo_speed:
        r.label = "Turbo Speed (Hold Tab / F3 / Sel+R2)";
        r.kind = WidgetKind::selector;
        r.value = v.turbo_speed <= 0 ? "Max (Uncapped)" : (v.turbo_speed == 15 ? "1.5x Speed" : (std::to_string(v.turbo_speed) + "x Speed"));
        r.help = "Fast-forward multiplier with selective non-displayed frame-skipping (1.5x, 2x, 3x, 4x, or Max).";
        return r;
    case Command::keys:
        r.label = "Show Current Keybindings";
        r.value = "Enter";
        r.help = "Prints a summary of the current keyboard and controller mapping to the notice banner.";
        return r;
    case Command::change_keys:
        r.label = "Remap Keyboard & Mouse Controls";
        r.value = "Open  >";
        r.help = "Customize every PS2 button mapping and save directly to keys.ini.";
        return r;
    case Command::coop_pass_control:
        r.label = "Co-op: Pass Control / Stick (F6)";
        r.value = v.coop_active ? (v.coop_room_code.empty() ? "Pass Now" : ("Room " + v.coop_room_code)) : "Solo";
        r.help = "Passes active controller authority to the next connected co-op partner (or requests control if spectating).";
        return r;
    case Command::coop_resync:
        r.label = "Co-op: Force State Checkpoint Resync";
        r.value = v.coop_active ? (std::to_string(v.coop_peer_count) + "P Live") : "Solo";
        r.help = "Broadcasts or requests a fresh compressed delta-state checkpoint to immediately eliminate any drift.";
        return r;
    case Command::coop_slot_1:
    case Command::coop_slot_2:
    case Command::coop_slot_3:
    case Command::coop_slot_4: {
        const int slot = static_cast<int>(kCtlCmds[i]) - static_cast<int>(Command::coop_slot_1);
        r.label = "Co-op: Party Slot " + std::to_string(slot + 1) + " Owner";
        r.kind = WidgetKind::selector;
        r.value = "Player " + std::to_string(static_cast<int>(v.coop_slot_owners[slot] & 3u) + 1);
        r.help = "Assigns which co-op player owns this party member's battle turns and Relay Combo Judgment Rings.";
        return r;
    }
    case Command::coop_auto_turn:
        r.label = "Co-op: Auto Battle-Turn Authority";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.coop_auto_turn;
        r.value = v.coop_auto_turn ? "On" : "Off";
        r.help = "Automatically hands zero-lag Judgment Ring authority to each party member's assigned player.";
        return r;
    case Command::coop_relay_combos:
        r.label = "Co-op: Relay Combos (Baton Pass)";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.coop_relay_combos;
        r.value = v.coop_relay_combos ? "On" : "Off";
        r.help = "Passes Judgment Ring authority mid-combo so teammates chain multi-character Combos together.";
        return r;
    case Command::coop_hud_toggle:
        r.label = "Co-op: Turn Spotlight & Callout HUD";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.coop_show_hud;
        r.value = v.coop_show_hud ? "On" : "Off";
        r.help = "Shows the Turn Spotlight corner pill and live partner Strike/Perfect callout banners.";
        return r;
    case Command::coop_ping_toggle:
        r.label = "Co-op: Spectator Emote Pings (F7)";
        r.kind = WidgetKind::toggle;
        r.toggle_on = v.coop_allow_pings;
        r.value = v.coop_allow_pings ? "On" : "Off";
        r.help = "Allows spectating teammates to send quick non-blocking pings (Nice!, Heal!, Combo!, Pass Stick!).";
        return r;
    default:
        return r;
    }
}

}  // namespace

std::string Menu::format_bytes(std::uint64_t bytes) {
    char buf[32];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) std::snprintf(buf, sizeof buf, "%.2f GB", mb / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.0f MB", mb);
    return buf;
}

int Menu::count(const Values *) const {
    switch (page_) {
    case Page::main: return kMainEntries;
    case Page::settings:
        return tab_ == SettingsTab::graphics ? kGfxEntries : (tab_ == SettingsTab::audio ? kAudEntries : kCtlEntries);
    case Page::keys: return key_rows_ + 2;
    case Page::achievements: return static_cast<int>(ach_filtered_.size()) + 1;  // +1 for Back
    case Page::mods: return (mod_settings_idx_ >= 0) ? (mod_settings_rows_ + 2) : (mod_rows_ + 1);  // +Reset/+Back or +Back
    case Page::save_states: return 12;  // Slots 0..9 + Auto (10) + Back (11)
    }
    return kMainEntries;
}

void Menu::fit(int total_rows, int visible_rows) {
    const int shown = std::min(total_rows, visible_rows);
    if (selected_ < total_rows) {
        if (selected_ < top_) top_ = selected_;
        if (selected_ >= top_ + shown) top_ = selected_ - shown + 1;
    }
    top_ = std::clamp(top_, 0, std::max(0, total_rows - shown));
}

void Menu::open() {
    open_ = true;
    page_ = Page::main;
    save_mode_ = SavePickerMode::browse;
    direct_picker_ = false;
    selected_ = 0;
    top_ = 0;
    quit_armed_ = false;
    slot_save_armed_ = false;
    slot_delete_armed_ = false;
    capturing_ = false;
    defaults_armed_ = false;
    typing_scale_ = false;
    scale_pristine_ = false;
    scale_input_.clear();
}

void Menu::open_save_picker(SavePickerMode mode, int initial_slot) {
    open();
    page_ = Page::save_states;
    save_mode_ = mode;
    direct_picker_ = (mode != SavePickerMode::browse);
    selected_ = std::clamp(initial_slot, 0, 9);
    top_ = 0;
}

void Menu::select_slot(int slot) {
    if (page_ != Page::save_states) return;
    const int next = std::clamp(slot, 0, 9);
    if (next != selected_) clear_slot_armed();
    selected_ = next;
}

bool Menu::arm_slot_save() {
    slot_delete_armed_ = false;
    if (slot_save_armed_) {
        slot_save_armed_ = false;
        return true;
    }
    slot_save_armed_ = true;
    return false;
}

bool Menu::arm_slot_delete() {
    slot_save_armed_ = false;
    if (slot_delete_armed_) {
        slot_delete_armed_ = false;
        return true;
    }
    slot_delete_armed_ = true;
    return false;
}

void Menu::close() {
    open_ = false;
    capturing_ = false;
    typing_scale_ = false;
    scale_pristine_ = false;
    slot_save_armed_ = false;
    slot_delete_armed_ = false;
    scale_input_.clear();
}

void Menu::switch_tab(int delta) {
    if (page_ != Page::settings) return;
    typing_scale_ = false;
    scale_pristine_ = false;
    const int n = static_cast<int>(SettingsTab::count);
    tab_ = static_cast<SettingsTab>((static_cast<int>(tab_) + delta % n + n) % n);
    selected_ = 0;
    top_ = 0;
}

void Menu::set_tab(SettingsTab t) {
    if (tab_ == t) return;
    typing_scale_ = false;
    scale_pristine_ = false;
    tab_ = t;
    selected_ = 0;
    top_ = 0;
}

void Menu::cycle_ach_tier(int delta) {
    if (page_ != Page::achievements) return;
    int cur = 0;
    for (int i = 0; i < static_cast<int>(achievements::kGalleryTierTabs.size()); ++i) {
        if (achievements::kGalleryTierTabs[static_cast<std::size_t>(i)].tier_filter == ach_tier_filter_) {
            cur = i;
            break;
        }
    }
    const int n = static_cast<int>(achievements::kGalleryTierTabs.size());
    const int next = ((cur + delta) % n + n) % n;
    ach_tier_filter_ = achievements::kGalleryTierTabs[static_cast<std::size_t>(next)].tier_filter;
    selected_ = 0;
    top_ = 0;
}

void Menu::set_ach_tier(int tier_filter) {
    if (page_ != Page::achievements) return;
    tier_filter = std::clamp(tier_filter, -1, 6);
    if (ach_tier_filter_ == tier_filter) return;
    ach_tier_filter_ = tier_filter;
    selected_ = 0;
    top_ = 0;
}

void Menu::cycle_ach_status(int delta) {
    if (page_ != Page::achievements) return;
    ach_status_filter_ = (ach_status_filter_ + delta % 3 + 3) % 3;
    selected_ = 0;
    top_ = 0;
}

void Menu::set_ach_status(int status_filter) {
    if (page_ != Page::achievements) return;
    status_filter = std::clamp(status_filter, 0, 2);
    if (ach_status_filter_ == status_filter) return;
    ach_status_filter_ = status_filter;
    selected_ = 0;
    top_ = 0;
}

void Menu::toggle_reveal_secrets() { reveal_secrets_ = !reveal_secrets_; }

void Menu::page_move(int delta) {
    if (page_ != Page::achievements) {
        move(delta > 0 ? 5 : -5);
        return;
    }
    const int n = static_cast<int>(ach_filtered_.size());
    if (n <= 0) return;
    const int step = std::max(1, ach_visible_ - 1) * (delta >= 0 ? 1 : -1);
    selected_ = std::clamp(selected_ + step, 0, n - 1);
    fit(n, std::max(1, ach_visible_));
}

void Menu::begin_scale_input(int current_scale) {
    typing_scale_ = true;
    scale_pristine_ = true;
    scale_input_ = std::to_string(std::max(1, current_scale));
}

void Menu::push_scale_digit(char d) {
    if (!typing_scale_ || d < '0' || d > '9') return;
    if (scale_pristine_) {
        scale_input_.clear();
        scale_pristine_ = false;
    }
    if (scale_input_.size() < 2) scale_input_.push_back(d);
}

void Menu::backspace_scale_digit() {
    if (!typing_scale_ || scale_input_.empty()) return;
    scale_pristine_ = false;
    scale_input_.pop_back();
}

void Menu::cancel_scale_input() {
    typing_scale_ = false;
    scale_pristine_ = false;
    scale_input_.clear();
}

int Menu::finish_scale_input(int min_val, int max_val) {
    typing_scale_ = false;
    scale_pristine_ = false;
    const int val = scale_input_.empty() ? min_val : std::atoi(scale_input_.c_str());
    scale_input_.clear();
    return std::clamp(val, min_val, std::max(min_val, max_val));
}

bool Menu::back() {
    if (typing_scale_) {
        cancel_scale_input();
        return true;
    }
    if (slot_save_armed_ || slot_delete_armed_) {
        clear_slot_armed();
        return true;
    }
    if (page_ == Page::keys) {
        page_ = Page::settings;
        tab_ = SettingsTab::controls;
        selected_ = settings_selected_;
        top_ = 0;
        capturing_ = false;
        defaults_armed_ = false;
        return true;
    }
    if (page_ == Page::save_states) {
        if (direct_picker_) return false;  // direct F5/F8 picker: Esc closes menu
        page_ = Page::main;
        selected_ = main_selected_;
        top_ = 0;
        return true;
    }
    if (page_ == Page::mods && mod_settings_idx_ >= 0) {
        exit_mod_settings();
        return true;
    }
    if (page_ == Page::settings || page_ == Page::achievements || page_ == Page::mods) {
        page_ = Page::main;
        selected_ = main_selected_;
        top_ = 0;
        return true;
    }
    return false;
}

void Menu::move(int delta) {
    quit_armed_ = false;
    defaults_armed_ = false;
    clear_slot_armed();
    const int n = count();
    if (n <= 0) return;
    selected_ = (selected_ + delta % n + n) % n;
    if (page_ == Page::settings) fit(n, kSettingsVisible);
    else if (page_ == Page::keys) fit(key_rows_, kKeysVisible);
    else if (page_ == Page::achievements) fit(static_cast<int>(ach_filtered_.size()), std::max(1, ach_visible_));
    else if (page_ == Page::mods) fit(mod_settings_idx_ >= 0 ? mod_settings_rows_ : mod_rows_, 7);
}

Command Menu::choose() {
    if (page_ == Page::main) {
        const Command c = kMainCmds[selected_];
        if (c == Command::open_settings) {
            main_selected_ = selected_;
            page_ = Page::settings;
            selected_ = 0;
            top_ = 0;
            return Command::none;
        }
        if (c == Command::open_save_states) {
            main_selected_ = selected_;
            page_ = Page::save_states;
            save_mode_ = SavePickerMode::browse;
            direct_picker_ = false;
            clear_slot_armed();
            selected_ = 0;
            top_ = 0;
            return Command::none;
        }
        if (c == Command::open_achievements) {
            main_selected_ = selected_;
            page_ = Page::achievements;
            selected_ = 0;
            top_ = 0;
            return Command::none;
        }
        if (c == Command::open_mods) {
            main_selected_ = selected_;
            page_ = Page::mods;
            selected_ = 0;
            top_ = 0;
            return Command::open_mods;
        }
        if (c != Command::quit) return c;
        if (quit_armed_) return c;
        quit_armed_ = true;
        return Command::none;
    }
    if (page_ == Page::settings) {
        const int n = count();
        if (selected_ == n - 1) {
            back();
            return Command::none;
        }
        const Command c = tab_ == SettingsTab::graphics ? kGfxCmds[selected_]
                        : (tab_ == SettingsTab::audio ? kAudCmds[selected_] : kCtlCmds[selected_]);
        if (c == Command::change_keys) {
            settings_selected_ = selected_;
            page_ = Page::keys;
            selected_ = 0;
            top_ = 0;
            capturing_ = false;
            defaults_armed_ = false;
            return c;
        }
        return c;
    }
    if (page_ == Page::keys) {
        if (selected_ < key_rows_) {
            capturing_ = true;
            return Command::key_edit;
        }
        if (selected_ == key_rows_) {
            if (!defaults_armed_) {
                defaults_armed_ = true;
                return Command::none;
            }
            defaults_armed_ = false;
            return Command::key_defaults;
        }
        back();
        return Command::none;
    }
    if (page_ == Page::save_states) {
        if (selected_ >= 11) {
            if (!back()) return Command::resume;
            return Command::none;
        }
        if (selected_ == 10) return Command::slot_load;  // Autosave is load-on-select
        return save_mode_ == SavePickerMode::save_pick ? Command::slot_save : Command::slot_load;
    }
    if (page_ == Page::mods) {
        if (mod_settings_idx_ >= 0) {
            if (selected_ < mod_settings_rows_) return Command::mod_setting_step;
            if (selected_ == mod_settings_rows_) return Command::mod_settings_reset;
            exit_mod_settings();
            return Command::none;
        }
        if (selected_ >= mod_rows_) {
            back();
            return Command::none;
        }
        return Command::mod_toggle;
    }
    // Page::achievements
    if (selected_ >= static_cast<int>(ach_filtered_.size())) {
        back();
        return Command::none;
    }
    return Command::none;
}

Command Menu::side() const {
    if (page_ == Page::mods) {
        if (mod_settings_idx_ >= 0) {
            return (selected_ >= 0 && selected_ < mod_settings_rows_) ? Command::mod_setting_step : Command::none;
        }
        return (selected_ >= 0 && selected_ < mod_rows_) ? Command::mod_open_settings : Command::none;
    }
    if (page_ != Page::settings) return Command::none;
    const int n = count();
    if (selected_ >= n - 1) return Command::none;
    const Command c = tab_ == SettingsTab::graphics ? kGfxCmds[selected_]
                    : (tab_ == SettingsTab::audio ? kAudCmds[selected_] : kCtlCmds[selected_]);
    return is_side_command(c) ? c : Command::none;
}

bool Menu::hit_row(int x, int y, int &out_row) const {
    if (capturing_ || typing_scale_) return false;
    if (page_ == Page::achievements) {
        if (ach_back_rect_.contains(x, y)) {
            out_row = static_cast<int>(ach_filtered_.size());
            return true;
        }
        if (ach_card_stride_ > 0 && ach_card_h_ > 0 && x >= entry_x_ && x < entry_x_ + entry_w_ &&
            y >= rows_y_ && y < rows_y_ + ach_shown_ * ach_card_stride_) {
            const int rel = y - rows_y_;
            const int idx = rel / ach_card_stride_;
            if (idx >= 0 && idx < ach_shown_ && (rel % ach_card_stride_) < ach_card_h_) {
                out_row = top_ + idx;
                return true;
            }
        }
        return false;
    }
    if (entry_h_ <= 0) return false;
    if (x < entry_x_ || x >= entry_x_ + entry_w_) return false;
    if (page_ == Page::main) {
        if (y < entry_y_ || y >= entry_y_ + kMainEntries * entry_h_) return false;
        out_row = (y - entry_y_) / entry_h_;
        return true;
    }
    if (page_ == Page::settings) {
        if (y < rows_y_ || y >= rows_y_ + visible_count_ * entry_h_) return false;
        const int now = top_ + (y - rows_y_) / entry_h_;
        if (now < 0 || now >= count()) return false;
        out_row = now;
        return true;
    }
    if (page_ == Page::keys) {
        if (y >= rows_y_ && y < rows_y_ + keys_shown_ * entry_h_) {
            out_row = top_ + (y - rows_y_) / entry_h_;
            return true;
        }
        if (y >= footer_y_ && y < footer_y_ + 2 * entry_h_) {
            out_row = key_rows_ + (y - footer_y_) / entry_h_;
            return true;
        }
        return false;
    }
    if (page_ == Page::mods) {
        if (y >= rows_y_ && y < rows_y_ + mods_shown_ * entry_h_) {
            out_row = top_ + (y - rows_y_) / entry_h_;
            return true;
        }
        if (mod_settings_idx_ >= 0) {
            if (y >= footer_y_ && y < footer_y_ + 2 * entry_h_) {
                out_row = mod_settings_rows_ + (y - footer_y_) / entry_h_;
                return true;
            }
        } else if (y >= footer_y_ && y < footer_y_ + entry_h_) {
            out_row = mod_rows_;
            return true;
        }
        return false;
    }
    if (page_ == Page::save_states) {
        if (y >= rows_y_ && y < rows_y_ + 11 * entry_h_) {
            out_row = (y - rows_y_) / entry_h_;
            return true;
        }
        if (y >= footer_y_ && y < footer_y_ + entry_h_) {
            out_row = 11;
            return true;
        }
        return false;
    }
    return false;
}

bool Menu::select_at(int x, int y) {
    int now = -1;
    if (!hit_row(x, y, now)) return false;
    if (now == selected_) return false;  // avoid redrawing on mouse hover over the same item
    quit_armed_ = false;
    defaults_armed_ = false;
    clear_slot_armed();
    selected_ = now;
    return true;
}

bool Menu::click_at(int x, int y, Command &out_cmd) {
    out_cmd = Command::none;
    if (page_ == Page::settings && y >= tabs_y_ && y < tabs_y_ + tabs_h_) {
        for (int t = 0; t < 3; ++t) {
            if (x >= tab_x0_[t] && x < tab_x1_[t]) {
                set_tab(static_cast<SettingsTab>(t));
                return true;
            }
        }
    }
    if (page_ == Page::settings && settings_scroll_rect_.contains(x, y)) {
        const int n = count();
        if (n > 0) {
            const float frac = settings_scroll_rect_.h > 1
                ? static_cast<float>(y - settings_scroll_rect_.y) / static_cast<float>(settings_scroll_rect_.h - 1)
                : 0.0f;
            selected_ = std::clamp(static_cast<int>(frac * static_cast<float>(n - 1) + 0.5f), 0, n - 1);
            fit(n, kSettingsVisible);
            return true;
        }
    }
    if (page_ == Page::keys && keys_scroll_rect_.contains(x, y) && key_rows_ > 0) {
        const float frac = keys_scroll_rect_.h > 1
            ? static_cast<float>(y - keys_scroll_rect_.y) / static_cast<float>(keys_scroll_rect_.h - 1)
            : 0.0f;
        selected_ = std::clamp(static_cast<int>(frac * static_cast<float>(key_rows_ - 1) + 0.5f), 0, key_rows_ - 1);
        fit(key_rows_, kKeysVisible);
        return true;
    }
    if (page_ == Page::achievements) {
        for (int t = 0; t < 8; ++t) {
            if (ach_tier_rects_[t].contains(x, y)) {
                set_ach_tier(achievements::kGalleryTierTabs[static_cast<std::size_t>(t)].tier_filter);
                return true;
            }
        }
        for (int s = 0; s < 3; ++s) {
            if (ach_status_rects_[s].contains(x, y)) {
                set_ach_status(s);
                return true;
            }
        }
        if (ach_secret_rect_.contains(x, y)) {
            toggle_reveal_secrets();
            return true;
        }
        if (ach_back_rect_.contains(x, y)) {
            back();
            return true;
        }
        if (ach_scroll_rect_.contains(x, y) && !ach_filtered_.empty()) {
            const int n = static_cast<int>(ach_filtered_.size());
            const float frac = ach_scroll_rect_.h > 1
                ? static_cast<float>(y - ach_scroll_rect_.y) / static_cast<float>(ach_scroll_rect_.h - 1)
                : 0.0f;
            selected_ = std::clamp(static_cast<int>(frac * static_cast<float>(n - 1) + 0.5f), 0, n - 1);
            fit(n, std::max(1, ach_visible_));
            return true;
        }
    }
    int now = -1;
    if (!hit_row(x, y, now)) return false;
    if (now != selected_) {
        quit_armed_ = false;
        defaults_armed_ = false;
        clear_slot_armed();
        selected_ = now;
    }
    if (page_ == Page::mods && mod_settings_idx_ < 0 && now >= 0 && now < mod_rows_) {
        // Clicking the "Settings ->" badge area (just left of the ON/OFF pill) drills into mod settings
        if (x >= entry_x_ + entry_w_ - 160 && x < entry_x_ + entry_w_ - 60) {
            out_cmd = Command::mod_open_settings;
            return true;
        }
    }
    out_cmd = choose();
    return true;
}

void Menu::rebuild_ach_filter(const Values &values) {
    ach_filtered_.clear();
    if (!values.ach_entries) return;
    for (std::size_t i = 0; i < values.ach_entries->size(); ++i) {
        const auto &e = (*values.ach_entries)[i];
        if (ach_tier_filter_ >= 0 && static_cast<int>(e.tier) != ach_tier_filter_) continue;
        if (ach_status_filter_ == 1 && !e.unlocked) continue;
        if (ach_status_filter_ == 2 && e.unlocked) continue;
        ach_filtered_.push_back(i);
    }
    const int n = static_cast<int>(ach_filtered_.size());
    if (n == 0) selected_ = 0;
    else if (selected_ > n) selected_ = n - 1;
    fit(n, std::max(1, ach_visible_));
}

void Menu::draw_main(int window_width, int window_height, const Values &values, Picture &out) {
    // Sleek centered Nocturne Gothic card (~45% screen width) so the paused game world stays visible
    const int panel_w = std::clamp(window_width * 45 / 100, std::min(window_width - 24, 440), 620);
    const float sc = std::clamp(static_cast<float>(panel_w) / 540.0f, 0.72f, 1.25f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const int pad = spx(22.0f, 12);
    const int header_h = spx(54.0f, 38);
    const int row_stride = spx(42.0f, 30);
    const int row_gap = spx(5.0f, 3);
    const int row_h = row_stride - row_gap;
    const int footer_h = spx(28.0f, 20);
    const int panel_h = std::min(window_height - 20, pad + header_h + spx(10.0f, 6) + kMainEntries * row_stride + spx(10.0f, 6) + footer_h + pad);

    out.width = panel_w;
    out.height = panel_h;
    out.x = std::max(0, (window_width - panel_w) / 2);
    out.y = std::max(0, (window_height - panel_h) / 2);
    out.rgba.assign(static_cast<std::size_t>(panel_w) * panel_h, 0u);

    ui_draw::Surface surf(out.rgba.data(), panel_w, panel_h);
    draw_nocturne_panel(surf, panel_w, panel_h, sc);

    const int ix = pad;
    const int iw = panel_w - pad * 2;
    int y = pad;

    // Header: Georgia Gothic Title + Active Slot / Disc pill
    const int title_px = spx(22.0f, 15);
    const int sub_px = spx(11.5f, 9);
    ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, title_px, true,
                       game_display_title(), pal::kTitle, true);
    ui_draw::draw_text(surf, ix, y + spx(28.0f, 20), ui_draw::FontStyle::Sans, sub_px, false,
                       "PAUSE MENU", pal::kDim, false);

    const std::string meta_pill = "Disc " + std::to_string(values.active_disc + 1) + "  •  Slot " + std::to_string(values.active_slot);
    const int mp_px = spx(11.0f, 9);
    const int mp_h = spx(24.0f, 18);
    const int mp_w = ui_draw::measure_text(ui_draw::FontStyle::Sans, mp_px, true, meta_pill).width + spx(18.0f, 12);
    ui_draw::draw_pill(surf, ix + iw - mp_w, y + spx(6.0f, 3), mp_h, ui_draw::FontStyle::Sans, mp_px, true,
                       meta_pill, ui_draw::rgba(28, 24, 46, 235), pal::kCardBdr, pal::kTitle);

    y = pad + header_h;
    surf.fill_rect(ix, y, iw, 1, pal::kDivider);
    y += spx(10.0f, 6);

    entry_x_ = out.x + ix;
    entry_y_ = out.y + y;
    entry_w_ = iw;
    entry_h_ = row_stride;

    const int lbl_px = spx(15.0f, 11);
    const int sub_row_px = spx(11.5f, 9);

    for (int i = 0; i < kMainEntries; ++i) {
        const Command c = kMainCmds[i];
        const bool lit = (i == selected_);
        const bool is_quit = (c == Command::quit);
        const bool danger = is_quit && quit_armed_;
        draw_nocturne_row(surf, ix, y, iw, row_h, sc, lit, danger, false);

        std::string label;
        std::string right_badge = kMainKeys[i];
        std::string sub_tag;
        bool is_trophy_pill = false;
        switch (c) {
        case Command::resume:            label = "Resume Game"; break;
        case Command::save:              label = "Quick Save"; sub_tag = "Slot " + std::to_string(values.active_slot); break;
        case Command::load:              label = "Quick Load"; sub_tag = "Slot " + std::to_string(values.active_slot); break;
        case Command::open_save_states:  label = "Save States (0-9)"; break;
        case Command::photo_mode:        label = "Photo Mode (Free-Cam)"; sub_tag = "HUD Toggle & Tilt"; break;
        case Command::screenshot:        label = "Take Screenshot"; break;
        case Command::open_achievements:
            label = "Trophy Gallery";
            is_trophy_pill = true;
            break;
        case Command::open_mods:
            label = "Gameplay Mods";
            sub_tag = values.active_mod_count > 0
                ? (std::to_string(values.active_mod_count) + " active (live toggle)")
                : "Layer 1-3 Mod Manager";
            right_badge = ">";
            break;
        case Command::open_settings:     label = "Settings"; right_badge = ">"; break;
        case Command::quit:              label = quit_armed_ ? "Confirm Quit?  Press Enter / Cross" : "Quit to Desktop"; break;
        default: break;
        }

        const int cy = y + row_h / 2;
        const int lx = ix + spx(16.0f, 10);
        const auto lm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, lit || is_quit, label);
        const std::uint32_t tc = danger ? ui_draw::rgba(255, 232, 224, 255)
                                        : (lit ? ui_draw::rgba(255, 246, 214, 255)
                                               : (is_quit ? ui_draw::rgba(232, 166, 166, 240) : pal::kText));
        ui_draw::draw_text(surf, lx, cy - lm.height / 2, ui_draw::FontStyle::Sans, lbl_px, lit || danger, label, tc, lit);

        if (!sub_tag.empty()) {
            const auto sm = ui_draw::measure_text(ui_draw::FontStyle::Sans, sub_row_px, false, sub_tag);
            ui_draw::draw_text(surf, lx + lm.width + spx(10.0f, 6), cy - sm.height / 2 + 1,
                               ui_draw::FontStyle::Sans, sub_row_px, false, sub_tag,
                               lit ? pal::kTitle : pal::kDim, false);
        }

        const int rx = ix + iw - spx(12.0f, 8);
        if (is_trophy_pill) {
            const std::string ttxt = std::to_string(values.ach_unlocked) + " / " + std::to_string(values.ach_total);
            const int ph = spx(22.0f, 17);
            const int ppx = spx(11.0f, 9);
            const int pw = ui_draw::measure_text(ui_draw::FontStyle::Sans, ppx, true, ttxt).width + spx(18.0f, 12);
            ui_draw::draw_pill(surf, rx - pw, cy - ph / 2, ph, ui_draw::FontStyle::Sans, ppx, true, ttxt,
                               lit ? ui_draw::rgba(32, 20, 26, 242) : ui_draw::rgba(24, 72, 58, 235),
                               lit ? pal::kRowLitBdr : pal::kOnBdr,
                               lit ? pal::kTitle : pal::kOnText);
        } else if (!right_badge.empty()) {
            draw_keycap_pill(surf, rx, cy, row_h, right_badge, lit, sc);
        }

        y += row_stride;
    }

    // Footer hint
    const int fy = panel_h - pad - footer_h + spx(6.0f, 3);
    surf.fill_rect(ix, fy - spx(8.0f, 4), iw, 1, pal::kDivider);
    const std::string hint = "Enter / Cross: Select   •   F1 / Esc / Circle: Resume";
    const int hpx = spx(11.5f, 9);
    const auto hm = ui_draw::measure_text(ui_draw::FontStyle::Sans, hpx, false, hint);
    ui_draw::draw_text(surf, ix + std::max(0, (iw - hm.width) / 2), fy, ui_draw::FontStyle::Sans, hpx, false, hint, pal::kDim, false);
}

void Menu::draw_settings(int window_width, int window_height, const Values &values, Picture &out) {
    const int n = count();
    const int shown = std::min(n, kSettingsVisible);
    visible_count_ = shown;
    fit(n, kSettingsVisible);

    const int panel_w = std::clamp(window_width * 78 / 100, std::min(window_width - 24, 740), 1160);
    const int panel_h = std::clamp(window_height * 86 / 100, std::min(window_height - 20, 520), 820);
    out.width = panel_w;
    out.height = panel_h;
    out.x = std::max(0, (window_width - panel_w) / 2);
    out.y = std::max(0, (window_height - panel_h) / 2);
    out.rgba.assign(static_cast<std::size_t>(panel_w) * panel_h, 0u);

    const float sc = std::clamp(std::min(static_cast<float>(panel_w) / 980.0f, static_cast<float>(panel_h) / 640.0f), 0.70f, 1.30f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    ui_draw::Surface surf(out.rgba.data(), panel_w, panel_h);
    draw_nocturne_panel(surf, panel_w, panel_h, sc);

    const int pad = spx(22.0f, 12);
    const int ix = pad;
    const int iw = panel_w - pad * 2;
    int y = pad;

    // 1. Header
    const int title_px = spx(22.0f, 15);
    ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, title_px, true,
                       std::string(game_display_title()) + "  —  Settings", pal::kTitle, true);
    if (n > shown) {
        const std::string rng = std::to_string(top_ + 1) + "–" + std::to_string(top_ + shown) + " of " + std::to_string(n);
        const int rpx = spx(12.0f, 9);
        const auto rm = ui_draw::measure_text(ui_draw::FontStyle::Sans, rpx, true, rng);
        ui_draw::draw_text(surf, ix + iw - rm.width, y + spx(6.0f, 3), ui_draw::FontStyle::Sans, rpx, true, rng, pal::kDim, false);
    }
    y += spx(34.0f, 24);

    // 2. Gothic Pill Tab Bar: [ Graphics ] [ Audio ] [ Controls ]
    const int tab_h = spx(34.0f, 24);
    const int tab_gap = spx(10.0f, 6);
    const int tab_w = (iw - 2 * tab_gap) / 3;
    tabs_y_ = out.y + y;
    tabs_h_ = tab_h;
    static const char *const kTabNames[3] = {"Graphics & HD Packs", "Audio & Mixing", "Controls & Disc"};
    const int tab_px = spx(13.5f, 10);
    for (int t = 0; t < 3; ++t) {
        const int tx = ix + t * (tab_w + tab_gap);
        const bool active = static_cast<int>(tab_) == t;
        const float trad = static_cast<float>(spx(8.0f, 5));
        if (active) {
            surf.fill_gradient_rounded_rect(tx, y, tab_w, tab_h, trad, pal::kRowLitLeft, pal::kRowLitRight, 0.16f, true);
            surf.stroke_rounded_rect(tx, y, tab_w, tab_h, trad, 1.4f, pal::kRowLitBdr);
        } else {
            surf.fill_rounded_rect(tx, y, tab_w, tab_h, trad, ui_draw::rgba(22, 24, 40, 225));
            surf.stroke_rounded_rect(tx, y, tab_w, tab_h, trad, 1.0f, pal::kRowIdleBdr);
        }
        const std::string lbl = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, tab_px, active, kTabNames[t], tab_w - spx(16.0f, 10));
        const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, tab_px, active, lbl);
        ui_draw::draw_text(surf, tx + (tab_w - tm.width) / 2, y + (tab_h - tm.height) / 2,
                           ui_draw::FontStyle::Sans, tab_px, active, lbl,
                           active ? ui_draw::rgba(255, 246, 214, 255) : pal::kTextSoft, active);
        tab_x0_[t] = out.x + tx;
        tab_x1_[t] = out.x + tx + tab_w;
    }
    y += tab_h + spx(10.0f, 6);
    surf.fill_rect(ix, y, iw, 1, pal::kDivider);
    y += spx(10.0f, 6);

    // 3. Scrollable Settings List with Real Gothic Widgets
    const int footer_total_h = spx(66.0f, 48);
    const int body_h = std::max(spx(220.0f, 160), panel_h - y - footer_total_h - pad);
    const int scroll_w = (n > shown) ? spx(10.0f, 7) : 0;
    const int scroll_gap = (n > shown) ? spx(10.0f, 6) : 0;
    const int list_w = iw - scroll_w - scroll_gap;

    const int row_stride = std::max(spx(28.0f, 22), body_h / kSettingsVisible);
    const int row_gap = spx(4.0f, 2);
    const int row_h = std::max(20, row_stride - row_gap);

    entry_x_ = out.x + ix;
    entry_w_ = list_w;
    entry_h_ = row_stride;
    rows_y_ = out.y + y;

    const int lbl_px = spx(13.5f, 10);
    const int badge_px = spx(10.5f, 8);
    const int slider_bar_w = spx(136.0f, 84);
    const int sel_max_w = std::min(spx(300.0f, 170), list_w * 46 / 100);

    std::string active_help;
    for (int r = 0; r < shown; ++r) {
        const int idx = top_ + r;
        const bool lit = (idx == selected_);
        const SettingRowInfo info = describe_setting_row(tab_, idx, values, typing_scale_, scale_input_);
        if (lit) active_help = info.help;

        const int ry = y + r * row_stride;
        const bool is_back = (idx == n - 1);
        draw_nocturne_row(surf, ix, ry, list_w, row_h, sc, lit, info.danger, is_back);

        const int cy = ry + row_h / 2;
        const int lx = ix + spx(14.0f, 9);
        const int rx = ix + list_w - spx(12.0f, 8);

        // Draw right-side widget first so we know exact space left for label & badge
        int widget_w = 0;
        switch (info.kind) {
        case WidgetKind::toggle:
            draw_toggle_widget(surf, rx, cy, row_h, info.toggle_on, lit, sc);
            widget_w = spx(62.0f, 52);
            break;
        case WidgetKind::slider:
            draw_slider_widget(surf, rx, cy, slider_bar_w, info.slider_ratio, info.value, lit, sc);
            widget_w = slider_bar_w + spx(66.0f, 52);
            break;
        case WidgetKind::selector:
            widget_w = draw_selector_pill(surf, rx, cy, row_h, info.value, lit, info.locked, sel_max_w, sc) + spx(8.0f, 6);
            break;
        case WidgetKind::text_input: {
            const bool typing_here = typing_scale_ && lit;
            widget_w = draw_selector_pill(surf, rx, cy, row_h, info.value, lit || typing_here, info.locked, sel_max_w, sc) + spx(8.0f, 6);
            break;
        }
        case WidgetKind::action:
            if (!info.value.empty()) {
                widget_w = draw_keycap_pill(surf, rx, cy, row_h, info.value, lit, sc) + spx(8.0f, 6);
            }
            break;
        }

        // Optional status badge (e.g. "next start", "(2 packs)", "~240 MB") to left of widget
        int badge_w = 0;
        if (!info.badge.empty()) {
            const int bph = spx(20.0f, 15);
            const std::string bfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, badge_px, false, info.badge, spx(150.0f, 90));
            const int bw = ui_draw::measure_text(ui_draw::FontStyle::Sans, badge_px, false, bfit).width + spx(12.0f, 8);
            const int bx = rx - widget_w - bw - spx(4.0f, 2);
            if (bx > lx + spx(140.0f, 90)) {
                ui_draw::draw_pill(surf, bx, cy - bph / 2, bph, ui_draw::FontStyle::Sans, badge_px, false, bfit,
                                   ui_draw::rgba(18, 20, 34, 215), ui_draw::rgba(110, 102, 86, 170),
                                   lit ? pal::kTitle : pal::kTextSoft);
                badge_w = bw + spx(10.0f, 6);
            }
        }

        // Left-side label
        const int max_lbl_w = std::max(80, (rx - widget_w - badge_w) - lx - spx(8.0f, 4));
        const std::string lfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, lbl_px, lit, info.label, max_lbl_w);
        const auto lm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, lit, lfit);
        const std::uint32_t lcol = info.danger ? (lit ? ui_draw::rgba(255, 236, 228, 255) : ui_draw::rgba(236, 158, 158, 240))
                                               : (lit ? ui_draw::rgba(255, 246, 214, 255) : (info.locked ? pal::kDim : pal::kText));
        ui_draw::draw_text(surf, lx, cy - lm.height / 2, ui_draw::FontStyle::Sans, lbl_px, lit, lfit, lcol, lit);
    }

    // Gothic scrollbar for 33-item Graphics tab
    if (n > shown) {
        const int sb_x = ix + list_w + scroll_gap;
        const int track_h = shown * row_stride - row_gap;
        settings_scroll_rect_ = {out.x + sb_x - 4, out.y + y, scroll_w + 8, track_h};
        draw_gothic_scrollbar(surf, sb_x, y, scroll_w, track_h, n, shown, top_);
    } else {
        settings_scroll_rect_ = {};
    }

    // 4. Contextual Description Bar + Controls Footer
    const int fy = panel_h - pad - footer_total_h + spx(6.0f, 3);
    surf.fill_rect(ix, fy - spx(6.0f, 3), iw, 1, pal::kDivider);

    const int desc_box_h = spx(30.0f, 22);
    surf.fill_rounded_rect(ix, fy, iw, desc_box_h, static_cast<float>(spx(6.0f, 4)), ui_draw::rgba(16, 16, 28, 225));
    surf.stroke_rounded_rect(ix, fy, iw, desc_box_h, static_cast<float>(spx(6.0f, 4)), 1.0f, ui_draw::rgba(124, 104, 72, 160));
    surf.fill_diamond(static_cast<float>(ix + spx(13.0f, 9)), static_cast<float>(fy + desc_box_h / 2), 4.0f * sc, 4.0f * sc, pal::kTitle);
    const int hpx = spx(12.0f, 9);
    const std::string hfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, hpx, false, active_help, iw - spx(36.0f, 22));
    const auto hm = ui_draw::measure_text(ui_draw::FontStyle::Sans, hpx, false, hfit);
    ui_draw::draw_text(surf, ix + spx(24.0f, 16), fy + (desc_box_h - hm.height) / 2,
                       ui_draw::FontStyle::Sans, hpx, false, hfit,
                       typing_scale_ ? pal::kTitle : ui_draw::rgba(226, 222, 206, 245), false);

    const int hint_y = fy + desc_box_h + spx(8.0f, 5);
    const int fpx = spx(11.0f, 8);
    const std::string hint = typing_scale_
        ? "0-9: Type Scale   •   Backspace: Erase   •   Enter: Apply Live   •   Esc: Cancel"
        : "Q / E or L1 / R1: Switch Tab   •   Left / Right: Adjust   •   Enter / Click: Toggle   •   Esc / Circle: Back";
    const std::string fit_hint = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, fpx, false, hint, iw);
    ui_draw::draw_text(surf, ix, hint_y, ui_draw::FontStyle::Sans, fpx, false, fit_hint, pal::kDim, false);
}

void Menu::draw_keys(int window_width, int window_height, const Values &values, Picture &out) {
    key_rows_ = static_cast<int>(values.key_rows.size());
    fit(key_rows_, kKeysVisible);
    const int n = key_rows_;
    const int shown = std::min(n, kKeysVisible);
    keys_shown_ = shown;

    const int panel_w = std::clamp(window_width * 66 / 100, std::min(window_width - 24, 640), 980);
    const int panel_h = std::clamp(window_height * 84 / 100, std::min(window_height - 20, 500), 780);
    out.width = panel_w;
    out.height = panel_h;
    out.x = std::max(0, (window_width - panel_w) / 2);
    out.y = std::max(0, (window_height - panel_h) / 2);
    out.rgba.assign(static_cast<std::size_t>(panel_w) * panel_h, 0u);

    const float sc = std::clamp(std::min(static_cast<float>(panel_w) / 820.0f, static_cast<float>(panel_h) / 620.0f), 0.70f, 1.25f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    ui_draw::Surface surf(out.rgba.data(), panel_w, panel_h);
    draw_nocturne_panel(surf, panel_w, panel_h, sc);

    const int pad = spx(22.0f, 12);
    const int ix = pad;
    const int iw = panel_w - pad * 2;
    int y = pad;

    // Header
    ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, spx(22.0f, 15), true,
                       "Change Keyboard & Mouse Controls", pal::kTitle, true);
    if (n > shown) {
        const std::string rng = std::to_string(top_ + 1) + "–" + std::to_string(top_ + shown) + " of " + std::to_string(n);
        const int rpx = spx(12.0f, 9);
        const auto rm = ui_draw::measure_text(ui_draw::FontStyle::Sans, rpx, true, rng);
        ui_draw::draw_text(surf, ix + iw - rm.width, y + spx(6.0f, 3), ui_draw::FontStyle::Sans, rpx, true, rng, pal::kDim, false);
    }
    y += spx(34.0f, 24);
    surf.fill_rect(ix, y, iw, 1, pal::kDivider);
    y += spx(10.0f, 6);

    const int scroll_w = (n > shown) ? spx(10.0f, 7) : 0;
    const int scroll_gap = (n > shown) ? spx(10.0f, 6) : 0;
    const int list_w = iw - scroll_w - scroll_gap;

    const int footer_note_h = spx(58.0f, 42);
    const int action_gap = spx(8.0f, 5);
    const int avail_rows_h = std::max(spx(220.0f, 160), panel_h - y - footer_note_h - action_gap - pad);
    const int row_stride = std::max(spx(28.0f, 22), avail_rows_h / (kKeysVisible + 2));
    const int row_gap = spx(4.0f, 2);
    const int row_h = std::max(20, row_stride - row_gap);

    entry_x_ = out.x + ix;
    entry_w_ = list_w;
    entry_h_ = row_stride;
    rows_y_ = out.y + y;

    const int lbl_px = spx(13.5f, 10);
    const int bind_max_w = list_w * 54 / 100;

    for (int r = 0; r < shown; ++r) {
        const int index = top_ + r;
        const bool lit = (index == selected_);
        const bool cap = (lit && capturing_);
        const int ry = y + r * row_stride;
        draw_nocturne_row(surf, ix, ry, list_w, row_h, sc, lit, cap, false);

        const int cy = ry + row_h / 2;
        const int lx = ix + spx(14.0f, 9);
        const int rx = ix + list_w - spx(12.0f, 8);
        const Values::KeyRow &row = values.key_rows[index];

        if (cap) {
            const std::string prompt = "Press any key or mouse button...  (Esc to cancel)";
            const auto pm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, true, prompt);
            ui_draw::draw_text(surf, lx, cy - pm.height / 2, ui_draw::FontStyle::Sans, lbl_px, true, prompt,
                               ui_draw::rgba(255, 240, 224, 255), true);
        } else {
            const auto lm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, lit, row.label);
            ui_draw::draw_text(surf, lx, cy - lm.height / 2, ui_draw::FontStyle::Sans, lbl_px, lit, row.label,
                               lit ? ui_draw::rgba(255, 246, 214, 255) : pal::kText, lit);
            if (!row.keys.empty()) {
                const int px_sz = spx(11.5f, 9);
                const int ph = std::max(18, std::min(row_h - 6, spx(23.0f, 17)));
                const std::string kfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, px_sz, true, row.keys, bind_max_w - spx(18.0f, 12));
                const int kw = ui_draw::measure_text(ui_draw::FontStyle::Sans, px_sz, true, kfit).width + spx(16.0f, 12);
                ui_draw::draw_pill(surf, rx - kw, cy - ph / 2, ph, ui_draw::FontStyle::Sans, px_sz, true, kfit,
                                   lit ? ui_draw::rgba(32, 18, 26, 242) : pal::kKeyBg,
                                   lit ? pal::kRowLitBdr : pal::kKeyBdr,
                                   lit ? pal::kTitle : pal::kTextSoft);
            }
        }
    }

    if (n > shown) {
        const int sb_x = ix + list_w + scroll_gap;
        const int track_h = shown * row_stride - row_gap;
        keys_scroll_rect_ = {out.x + sb_x - 4, out.y + y, scroll_w + 8, track_h};
        draw_gothic_scrollbar(surf, sb_x, y, scroll_w, track_h, n, shown, top_);
    } else {
        keys_scroll_rect_ = {};
    }

    int fy_rows = y + shown * row_stride + action_gap;
    footer_y_ = out.y + fy_rows;

    // Footer action 1: Reset to defaults
    {
        const bool lit = (selected_ == n);
        draw_nocturne_row(surf, ix, fy_rows, list_w, row_h, sc, lit, defaults_armed_, false);
        const std::string txt = defaults_armed_ ? "Confirm Reset to Default Keys?  Press Enter / Cross again" : "Restore Default Keyboard Bindings";
        const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, lit || defaults_armed_, txt);
        ui_draw::draw_text(surf, ix + spx(14.0f, 9), fy_rows + (row_h - tm.height) / 2, ui_draw::FontStyle::Sans, lbl_px,
                           lit || defaults_armed_, txt,
                           defaults_armed_ ? ui_draw::rgba(255, 232, 224, 255) : (lit ? ui_draw::rgba(255, 246, 214, 255) : ui_draw::rgba(236, 174, 168, 240)), lit);
        fy_rows += row_stride;
    }
    // Footer action 2: Back
    {
        const bool lit = (selected_ == n + 1);
        draw_nocturne_row(surf, ix, fy_rows, list_w, row_h, sc, lit, false, true);
        const std::string txt = "Back to Settings";
        const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, lit, txt);
        ui_draw::draw_text(surf, ix + spx(14.0f, 9), fy_rows + (row_h - tm.height) / 2, ui_draw::FontStyle::Sans, lbl_px, lit, txt,
                           lit ? ui_draw::rgba(255, 246, 214, 255) : pal::kTextSoft, lit);
        draw_keycap_pill(surf, ix + list_w - spx(12.0f, 8), fy_rows + row_h / 2, row_h, "Esc", lit, sc);
    }

    // Bottom notes
    const int ny = panel_h - pad - footer_note_h + spx(6.0f, 3);
    surf.fill_rect(ix, ny - spx(6.0f, 3), iw, 1, pal::kDivider);
    const int npx = spx(11.5f, 9);
    if (!values.key_note.empty()) {
        ui_draw::draw_text(surf, ix, ny, ui_draw::FontStyle::Sans, npx, true,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Sans, npx, true, values.key_note, iw), pal::kTitle, false);
    }
    ui_draw::draw_text(surf, ix, ny + spx(19.0f, 14), ui_draw::FontStyle::Sans, spx(11.0f, 8), false,
                       "Enter / Click: Rebind Action   •   Esc / Circle: Back", pal::kTextSoft, false);
    ui_draw::draw_text(surf, ix, ny + spx(35.0f, 26), ui_draw::FontStyle::Sans, spx(10.5f, 8), false,
                       "Reserved hotkeys: F1 (Menu), F2, F5/F8 (Save/Load), F6/F7 (Slot), F11 (Fullscreen), F12 (Screenshot)", pal::kDim, false);
}

namespace {

struct CachedAchThumb {
    bool searched = false;
    bool ok = false;
    int w = 0, h = 0;
    std::vector<std::uint32_t> rgba;
};

const CachedAchThumb &lookup_ach_thumb(const std::string &id, const std::string &title,
                                       const std::string &fallback_title, int max_w, int max_h) {
    static std::unordered_map<std::string, CachedAchThumb> cache;
    const std::string key = id + "#" + std::to_string(max_w) + "x" + std::to_string(max_h);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() >= 32u) cache.clear();
    auto &entry = cache[key];
    entry.searched = true;
    auto p = screenshot::find_achievement_screenshot(title);
    if (p.empty() && !fallback_title.empty()) p = screenshot::find_achievement_screenshot(fallback_title);
    if (!p.empty()) {
        entry.ok = ui_draw::load_png_thumbnail(p, max_w, max_h, entry.rgba, entry.w, entry.h);
    }
    return entry;
}

}  // namespace

void Menu::draw_achievements(int window_width, int window_height, const Values &values, Picture &out) {
    // High-resolution Trophy Gallery sized proportionally to the native window
    const int panel_w = std::clamp(window_width * 90 / 100, std::min(window_width, 620), 1560);
    const int panel_h = std::clamp(window_height * 88 / 100, std::min(window_height, 440), 960);
    out.width = panel_w;
    out.height = panel_h;
    out.x = std::max(0, (window_width - panel_w) / 2);
    out.y = std::max(0, (window_height - panel_h) / 2);
    out.rgba.assign(static_cast<std::size_t>(panel_w) * panel_h, 0u);

    const float sc = std::clamp(std::min(static_cast<float>(panel_w) / 1180.0f, static_cast<float>(panel_h) / 720.0f), 0.62f, 1.35f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const int pad = spx(22.0f, 10);
    const int header_h = spx(68.0f, 44);
    const int tab_bar_h = spx(36.0f, 24);
    const int footer_h = spx(32.0f, 22);
    const int body_y = pad + header_h + spx(10.0f, 5) + tab_bar_h + spx(12.0f, 6);
    const int body_h = std::max(spx(160.0f, 120), panel_h - body_y - footer_h - pad);

    // Compute how many tall achievement cards fit cleanly in the left deck
    const int card_gap = spx(8.0f, 4);
    const int target_card_h = spx(74.0f, 48);
    ach_visible_ = std::clamp((body_h + card_gap) / std::max(1, target_card_h + card_gap), 4, 8);
    ach_card_h_ = std::max(44, (body_h - (ach_visible_ - 1) * card_gap) / ach_visible_);
    ach_card_stride_ = ach_card_h_ + card_gap;

    rebuild_ach_filter(values);
    const int n = static_cast<int>(ach_filtered_.size());
    const int shown = std::min(n, ach_visible_);
    ach_shown_ = shown;

    ui_draw::Surface surf(out.rgba.data(), panel_w, panel_h);
    const float outer_rad = static_cast<float>(spx(14.0f, 8));

    // Gothic midnight velvet backdrop + beveled gold frame
    surf.fill_gradient_rounded_rect(0, 0, panel_w, panel_h, outer_rad,
                                    ui_draw::rgba(13, 12, 24, 246), ui_draw::rgba(24, 17, 36, 246), 0.22f, false);
    // Soft warm top ambient glow
    surf.fill_radial_glow(static_cast<float>(panel_w) * 0.25f, 0.0f, static_cast<float>(panel_w) * 0.45f,
                          ui_draw::rgba(186, 130, 56, 30), ui_draw::rgba(186, 130, 56, 0));
    surf.stroke_rounded_rect(0, 0, panel_w, panel_h, outer_rad, std::max(1.5f, 2.0f * sc), ui_draw::rgba(196, 154, 78, 255));
    surf.stroke_rounded_rect(3, 3, panel_w - 6, panel_h - 6, std::max(4.0f, outer_rad - 3.0f), 1.0f, ui_draw::rgba(244, 210, 128, 65));

    // Per-tier counts for tabs & mastery panel
    int tier_u[7] = {}, tier_t[7] = {};
    if (values.ach_entries) {
        for (const auto &e : *values.ach_entries) {
            const int ti = std::clamp(static_cast<int>(e.tier), 0, 6);
            ++tier_t[ti];
            if (e.unlocked) ++tier_u[ti];
        }
    }

    // -------------------------------------------------------------------------
    // 1. Header Banner: Title, Overall Completion Bar, Status Pills, Back Pill
    // -------------------------------------------------------------------------
    const int inner_x = pad;
    const int inner_w = panel_w - pad * 2;
    const int title_px = spx(24.0f, 14);
    const int sub_px = spx(13.0f, 10);
    const int pill_h = spx(28.0f, 20);
    const int pill_px = spx(12.0f, 9);

    ui_draw::draw_text(surf, inner_x, pad, ui_draw::FontStyle::Serif, title_px, true,
                       std::string(game_display_title()) + "  —  Trophy Gallery", ui_draw::rgba(246, 212, 126, 255), true);

    const int pct = values.ach_total > 0 ? (values.ach_unlocked * 100 / values.ach_total) : 0;
    const std::string pct_str = std::to_string(values.ach_unlocked) + " / " + std::to_string(values.ach_total) +
                                " Unlocked  (" + std::to_string(pct) + "%)";
    const int bar_y = pad + spx(34.0f, 22);
    const int bar_w = std::min(spx(340.0f, 180), inner_w * 42 / 100);
    const int bar_h = spx(12.0f, 8);
    const float overall_ratio = values.ach_total > 0 ? static_cast<float>(values.ach_unlocked) / static_cast<float>(values.ach_total) : 0.0f;
    surf.draw_progress_bar(inner_x, bar_y, bar_w, bar_h, overall_ratio,
                           ui_draw::rgba(24, 26, 42, 255),
                           ui_draw::rgba(196, 134, 42, 255), ui_draw::rgba(255, 224, 124, 255),
                           ui_draw::rgba(130, 106, 62, 210));
    ui_draw::draw_text(surf, inner_x + bar_w + spx(12.0f, 6), bar_y - spx(2.0f, 1),
                       ui_draw::FontStyle::Sans, sub_px, true, pct_str, ui_draw::rgba(228, 230, 240, 245), true);

    // Top-Right Action Controls: [All] [Unlocked] [Locked]  [Reveal Secrets]  [Back]
    const int ctrl_y = pad + spx(4.0f, 2);
    int rx = inner_x + inner_w;
    const bool back_lit = selected_ >= n;
    {
        const std::string blbl = "Back (Esc)";
        const int bw = ui_draw::measure_text(ui_draw::FontStyle::Sans, pill_px, true, blbl).width + spx(20.0f, 12);
        rx -= bw;
        const std::uint32_t bg = back_lit ? ui_draw::rgba(232, 184, 78, 250) : ui_draw::rgba(46, 30, 44, 235);
        const std::uint32_t bdr = back_lit ? ui_draw::rgba(255, 232, 160, 255) : ui_draw::rgba(166, 118, 96, 220);
        const std::uint32_t tc = back_lit ? ui_draw::rgba(18, 14, 12, 255) : ui_draw::rgba(242, 226, 214, 255);
        ui_draw::draw_pill(surf, rx, ctrl_y, pill_h, ui_draw::FontStyle::Sans, pill_px, true, blbl, bg, bdr, tc);
        ach_back_rect_ = {out.x + rx, out.y + ctrl_y, bw, pill_h};
        rx -= spx(10.0f, 6);
    }
    {
        const std::string slbl = reveal_secrets_ ? "Secrets: Shown" : "Reveal Secrets";
        const int sw = ui_draw::measure_text(ui_draw::FontStyle::Sans, pill_px, true, slbl).width + spx(20.0f, 12);
        rx -= sw;
        const std::uint32_t bg = reveal_secrets_ ? ui_draw::rgba(24, 96, 86, 240) : ui_draw::rgba(26, 30, 46, 235);
        const std::uint32_t bdr = reveal_secrets_ ? ui_draw::rgba(110, 231, 200, 255) : ui_draw::rgba(92, 104, 134, 210);
        const std::uint32_t tc = reveal_secrets_ ? ui_draw::rgba(218, 255, 244, 255) : ui_draw::rgba(186, 194, 216, 245);
        ui_draw::draw_pill(surf, rx, ctrl_y, pill_h, ui_draw::FontStyle::Sans, pill_px, true, slbl, bg, bdr, tc);
        ach_secret_rect_ = {out.x + rx, out.y + ctrl_y, sw, pill_h};
        rx -= spx(12.0f, 6);
    }
    static const char *const kStatusLbl[3] = {"All", "Unlocked", "Locked"};
    for (int st = 2; st >= 0; --st) {
        const std::string lbl = kStatusLbl[st];
        const int sw = ui_draw::measure_text(ui_draw::FontStyle::Sans, pill_px, true, lbl).width + spx(18.0f, 10);
        rx -= sw;
        const bool active = (ach_status_filter_ == st);
        const std::uint32_t bg = active ? ui_draw::rgba(196, 146, 56, 245) : ui_draw::rgba(26, 28, 44, 230);
        const std::uint32_t bdr = active ? ui_draw::rgba(252, 224, 142, 255) : ui_draw::rgba(82, 88, 118, 200);
        const std::uint32_t tc = active ? ui_draw::rgba(18, 14, 10, 255) : ui_draw::rgba(184, 192, 214, 240);
        ui_draw::draw_pill(surf, rx, ctrl_y, pill_h, ui_draw::FontStyle::Sans, pill_px, true, lbl, bg, bdr, tc);
        ach_status_rects_[st] = {out.x + rx, out.y + ctrl_y, sw, pill_h};
        rx -= spx(6.0f, 4);
    }

    // Divider line
    const int div1_y = pad + header_h;
    surf.fill_rect(inner_x, div1_y, inner_w, 1, ui_draw::rgba(140, 114, 72, 120));

    // -------------------------------------------------------------------------
    // 2. Interactive 8-Tab Tier Strip (All + 7 Tiers with live unlocked/total)
    // -------------------------------------------------------------------------
    const int tab_y = div1_y + spx(8.0f, 4);
    const int tab_gap = spx(6.0f, 3);
    const int tab_w = (inner_w - tab_gap * 7) / 8;
    const int tab_font = spx(11.5f, 9);
    for (int t = 0; t < 8; ++t) {
        const auto &tab_spec = achievements::kGalleryTierTabs[static_cast<std::size_t>(t)];
        const int tx = inner_x + t * (tab_w + tab_gap);
        const bool active = (ach_tier_filter_ == tab_spec.tier_filter);
        std::uint32_t gl, gr, bdr, acc;
        if (tab_spec.tier_filter < 0) {
            gl = ui_draw::rgba(46, 36, 26, 240);
            gr = ui_draw::rgba(128, 92, 42, 240);
            bdr = ui_draw::rgba(232, 188, 92, 255);
            acc = ui_draw::rgba(255, 228, 150, 255);
        } else {
            achievements::Manager::theme_colors(static_cast<achievements::Tier>(tab_spec.tier_filter), values.toast_theme, gl, gr, bdr, acc);
        }
        const float tr = static_cast<float>(spx(7.0f, 4));
        if (active) {
            surf.fill_gradient_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, gl, gr, 0.20f, true);
            surf.stroke_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, 1.6f, bdr);
        } else {
            surf.fill_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, ui_draw::rgba(22, 24, 38, 225));
            surf.stroke_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, 1.0f, ui_draw::rgba(74, 80, 108, 170));
        }
        const int cu = (tab_spec.tier_filter < 0) ? values.ach_unlocked : tier_u[tab_spec.tier_filter];
        const int ct = (tab_spec.tier_filter < 0) ? values.ach_total : tier_t[tab_spec.tier_filter];
        const std::string tlabel = std::string(tab_spec.label) + " (" + std::to_string(cu) + "/" + std::to_string(ct) + ")";
        const std::string fit_lbl = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, tab_font, active, tlabel, tab_w - 8);
        const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, tab_font, active, fit_lbl);
        ui_draw::draw_text(surf, tx + (tab_w - tm.width) / 2, tab_y + (tab_bar_h - tm.height) / 2,
                           ui_draw::FontStyle::Sans, tab_font, active, fit_lbl,
                           active ? ui_draw::rgba(255, 250, 240, 255) : ui_draw::lerp_color(acc, ui_draw::rgba(170, 178, 200, 235), 0.65f),
                           active);
        ach_tier_rects_[t] = {out.x + tx, out.y + tab_y, tab_w, tab_bar_h};
    }

    // -------------------------------------------------------------------------
    // 3. Two-Column Body: Left Scrollable Card Deck + Right Showcase Inspector
    // -------------------------------------------------------------------------
    const int col_gap = spx(16.0f, 8);
    const int scroll_w = spx(10.0f, 6);
    const int scroll_gap = spx(8.0f, 4);
    const int left_total_w = (inner_w - col_gap) * 58 / 100;
    const int card_w = left_total_w - scroll_w - scroll_gap;
    const int right_x = inner_x + left_total_w + col_gap;
    const int right_w = inner_x + inner_w - right_x;

    entry_x_ = out.x + inner_x;
    entry_w_ = card_w;
    entry_h_ = ach_card_h_;
    rows_y_ = out.y + body_y;
    footer_y_ = out.y + panel_h - footer_h - pad;

    const int card_title_px = spx(15.5f, 11);
    const int card_desc_px = spx(11.5f, 9);
    const int card_meta_px = spx(11.0f, 9);
    const float card_rad = static_cast<float>(spx(9.0f, 5));

    if (n == 0) {
        surf.fill_rounded_rect(inner_x, body_y, card_w, spx(90.0f, 64), card_rad, ui_draw::rgba(20, 22, 36, 220));
        surf.stroke_rounded_rect(inner_x, body_y, card_w, spx(90.0f, 64), card_rad, 1.0f, ui_draw::rgba(82, 88, 118, 170));
        ui_draw::draw_text(surf, inner_x + spx(20.0f, 10), body_y + spx(32.0f, 20),
                           ui_draw::FontStyle::Serif, spx(16.0f, 12), false,
                           "No achievements match the active filter yet.", ui_draw::rgba(176, 182, 204, 235), true);
    } else {
        for (int r = 0; r < shown; ++r) {
            const int idx = top_ + r;
            const auto &e = (*values.ach_entries)[ach_filtered_[idx]];
            const bool lit = (idx == selected_);
            const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
            const int cy = body_y + r * ach_card_stride_;

            std::uint32_t gl, gr, bdr, acc;
            achievements::Manager::theme_colors(e.tier, values.toast_theme, gl, gr, bdr, acc);

            // Card background gradient
            if (lit) {
                const std::uint32_t c_left = ui_draw::lerp_color(gl, ui_draw::rgba(44, 34, 52, 248), 0.25f);
                const std::uint32_t c_right = ui_draw::lerp_color(gr, ui_draw::rgba(74, 52, 36, 248), 0.20f);
                surf.fill_gradient_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad, c_left, c_right, 0.20f, true);
                surf.stroke_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad, 2.0f, ui_draw::rgba(248, 214, 118, 255));
                // Luminous left accent bar
                surf.fill_rounded_rect(inner_x + 2, cy + spx(8.0f, 4), spx(4.0f, 3), ach_card_h_ - spx(16.0f, 8), 2.0f, acc);
            } else if (e.unlocked) {
                const std::uint32_t c_left = ui_draw::with_alpha(ui_draw::scale_rgb(gl, 0.78f), 232);
                const std::uint32_t c_right = ui_draw::with_alpha(ui_draw::scale_rgb(gr, 0.62f), 232);
                surf.fill_gradient_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad, c_left, c_right, 0.24f, true);
                surf.stroke_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad, 1.2f, ui_draw::with_alpha(bdr, 185));
            } else {
                surf.fill_gradient_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad,
                                                ui_draw::rgba(20, 22, 36, 232), ui_draw::rgba(28, 30, 48, 232), 0.18f, false);
                surf.stroke_rounded_rect(inner_x, cy, card_w, ach_card_h_, card_rad, 1.0f, ui_draw::rgba(66, 72, 98, 175));
            }

            // Left Ornate Tier Medallion
            const float med_r = std::min(static_cast<float>(ach_card_h_) * 0.36f, static_cast<float>(spx(24.0f, 15)));
            const float med_cx = static_cast<float>(inner_x + spx(14.0f, 8)) + med_r;
            const float med_cy = static_cast<float>(cy) + static_cast<float>(ach_card_h_) * 0.5f;
            ui_draw::draw_tier_medallion(surf, med_cx, med_cy, med_r, static_cast<int>(e.tier), gl, gr, bdr, acc, e.unlocked, masked);

            // Text area inside card
            const int tx = inner_x + spx(26.0f, 14) + static_cast<int>(med_r * 2.0f);
            const int text_right = inner_x + card_w - spx(14.0f, 8);
            const int avail_w = std::max(80, text_right - tx);

            // Right-side status / tag pill on top row
            int right_edge = text_right;
            const int tag_h = spx(18.0f, 14);
            const int tag_px = spx(10.0f, 8);
            const int top_y = cy + spx(8.0f, 4);
            if (e.unlocked) {
                const std::string dt = e.unlocked_at.size() >= 10 ? e.unlocked_at.substr(0, 10) : "UNLOCKED";
                const int dw = ui_draw::measure_text(ui_draw::FontStyle::Sans, tag_px, true, dt).width + spx(14.0f, 8);
                right_edge -= dw;
                ui_draw::draw_pill(surf, right_edge, top_y, tag_h, ui_draw::FontStyle::Sans, tag_px, true, dt,
                                   ui_draw::rgba(20, 74, 56, 235), ui_draw::rgba(110, 231, 180, 240), ui_draw::rgba(196, 255, 228, 255));
                right_edge -= spx(6.0f, 4);
            } else if (masked) {
                const int sw = ui_draw::measure_text(ui_draw::FontStyle::Sans, tag_px, true, "SECRET").width + spx(14.0f, 8);
                right_edge -= sw;
                ui_draw::draw_pill(surf, right_edge, top_y, tag_h, ui_draw::FontStyle::Sans, tag_px, true, "SECRET",
                                   ui_draw::rgba(18, 56, 54, 230), ui_draw::rgba(110, 231, 200, 210), ui_draw::rgba(176, 246, 228, 255));
                right_edge -= spx(6.0f, 4);
            }
            if (e.missable && !masked) {
                const int mw = ui_draw::measure_text(ui_draw::FontStyle::Sans, tag_px, true, "MISSABLE").width + spx(14.0f, 8);
                right_edge -= mw;
                ui_draw::draw_pill(surf, right_edge, top_y, tag_h, ui_draw::FontStyle::Sans, tag_px, true, "MISSABLE",
                                   ui_draw::rgba(92, 46, 16, 235), ui_draw::rgba(250, 178, 72, 245), ui_draw::rgba(255, 228, 172, 255));
                right_edge -= spx(6.0f, 4);
            }

            // Card Title
            const std::string raw_title = masked ? "Secret Achievement" : e.title;
            const std::string fit_title = ui_draw::fit_text_px(ui_draw::FontStyle::Serif, card_title_px, true, raw_title, std::max(40, right_edge - tx - 4));
            const std::uint32_t title_col = lit ? ui_draw::rgba(255, 250, 236, 255) : (e.unlocked ? acc : ui_draw::rgba(226, 230, 242, 245));
            ui_draw::draw_text(surf, tx, top_y - 1, ui_draw::FontStyle::Serif, card_title_px, true, fit_title, title_col, true);

            // Card Description line
            const std::string raw_desc = masked ? "Hidden Curiosity — press Tab to reveal unlock hint." : e.description;
            const std::string fit_desc = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, card_desc_px, false, raw_desc, avail_w);
            const int desc_y = top_y + spx(21.0f, 14);
            ui_draw::draw_text(surf, tx, desc_y, ui_draw::FontStyle::Sans, card_desc_px, false, fit_desc,
                               lit ? ui_draw::rgba(232, 232, 242, 245) : ui_draw::rgba(166, 174, 196, 230), false);

            // Bottom Row: Inline Progress Bar + Compact Progress Text
            const int pbar_y = cy + ach_card_h_ - spx(15.0f, 10);
            const int pbar_h = spx(7.0f, 5);
            const std::string ptxt = e.unlocked ? std::string(achievements::tier_short(e.tier))
                                   : (masked ? std::string("Hidden") : (e.progress_text.empty() ? std::string("0%") : e.progress_text));
            const std::string fit_ptxt = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, card_meta_px, false, ptxt, avail_w * 46 / 100);
            const int ptxt_w = ui_draw::measure_text(ui_draw::FontStyle::Sans, card_meta_px, false, fit_ptxt).width;
            const int pbar_w = std::max(48, avail_w - ptxt_w - spx(10.0f, 6));
            const float pratio = e.unlocked ? 1.0f : (masked ? 0.0f : static_cast<float>(std::clamp(e.progress_ratio, 0.0, 1.0)));
            surf.draw_progress_bar(tx, pbar_y, pbar_w, pbar_h, pratio,
                                   ui_draw::rgba(14, 15, 24, 235),
                                   e.unlocked ? ui_draw::rgba(56, 184, 134, 255) : ui_draw::scale_rgb(bdr, 0.85f),
                                   e.unlocked ? ui_draw::rgba(128, 244, 194, 255) : acc,
                                   ui_draw::rgba(82, 88, 114, 180));
            ui_draw::draw_text(surf, text_right - ptxt_w, pbar_y - spx(4.0f, 2),
                               ui_draw::FontStyle::Sans, card_meta_px, false, fit_ptxt,
                               e.unlocked ? ui_draw::rgba(154, 244, 206, 240) : ui_draw::rgba(178, 186, 208, 230), false);
        }
    }

    // Interactive Scrollbar Track + Proportional Thumb
    const int sb_x = inner_x + card_w + scroll_gap;
    surf.fill_rounded_rect(sb_x, body_y, scroll_w, body_h, static_cast<float>(scroll_w) * 0.5f, ui_draw::rgba(22, 24, 38, 225));
    ach_scroll_rect_ = {out.x + sb_x - 4, out.y + body_y, scroll_w + 8, body_h};
    if (n > ach_visible_ && ach_visible_ > 0) {
        const float frac_vis = static_cast<float>(ach_visible_) / static_cast<float>(n);
        const int thumb_h = std::max(spx(28.0f, 18), static_cast<int>(static_cast<float>(body_h) * frac_vis + 0.5f));
        const int max_top = std::max(1, n - ach_visible_);
        const int thumb_y = body_y + static_cast<int>(static_cast<float>(body_h - thumb_h) * static_cast<float>(top_) / static_cast<float>(max_top) + 0.5f);
        surf.fill_gradient_rounded_rect(sb_x + 1, thumb_y, scroll_w - 2, thumb_h, static_cast<float>(scroll_w - 2) * 0.5f,
                                        ui_draw::rgba(186, 144, 66, 240), ui_draw::rgba(244, 208, 122, 240), 0.1f, false);
    } else if (n > 0) {
        surf.fill_rounded_rect(sb_x + 1, body_y + 2, scroll_w - 2, body_h - 4, static_cast<float>(scroll_w - 2) * 0.5f,
                               ui_draw::rgba(92, 98, 128, 140));
    }

    // -------------------------------------------------------------------------
    // 4. Right-Side Showcase Inspector Panel
    // -------------------------------------------------------------------------
    const float insp_rad = static_cast<float>(spx(11.0f, 6));
    surf.fill_gradient_rounded_rect(right_x, body_y, right_w, body_h, insp_rad,
                                    ui_draw::rgba(18, 17, 32, 242), ui_draw::rgba(26, 22, 42, 242), 0.22f, false);
    surf.stroke_rounded_rect(right_x, body_y, right_w, body_h, insp_rad, 1.3f, ui_draw::rgba(136, 114, 78, 190));

    const int ip = spx(18.0f, 10);
    const int ix = right_x + ip;
    const int iw = right_w - ip * 2;
    int iy = body_y + ip;

    if (selected_ < n && values.ach_entries) {
        const auto &e = (*values.ach_entries)[ach_filtered_[selected_]];
        const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
        std::uint32_t gl, gr, bdr, acc;
        achievements::Manager::theme_colors(e.tier, values.toast_theme, gl, gr, bdr, acc);

        // Top ambient tier halo behind enlarged medallion
        const float big_r = static_cast<float>(spx(38.0f, 24));
        const float mcx = static_cast<float>(right_x) + static_cast<float>(right_w) * 0.5f;
        const float mcy = static_cast<float>(iy) + big_r + static_cast<float>(spx(4.0f, 2));
        surf.set_clip(right_x + 2, body_y + 2, right_w - 4, body_h - 4);
        surf.fill_radial_glow(mcx, mcy, big_r * 2.4f, ui_draw::with_alpha(e.unlocked ? acc : bdr, e.unlocked ? 55 : 25),
                              ui_draw::with_alpha(bdr, 0));
        surf.reset_clip();
        ui_draw::draw_tier_medallion(surf, mcx, mcy, big_r, static_cast<int>(e.tier), gl, gr, bdr, acc, e.unlocked, masked);
        iy = static_cast<int>(mcy + big_r) + spx(12.0f, 6);

        // Centered Showcase Title
        const std::string show_title = masked ? "Secret Achievement" : e.title;
        const int st_px = spx(19.0f, 13);
        const std::string fit_st = ui_draw::fit_text_px(ui_draw::FontStyle::Serif, st_px, true, show_title, iw);
        const auto stm = ui_draw::measure_text(ui_draw::FontStyle::Serif, st_px, true, fit_st);
        ui_draw::draw_text(surf, right_x + (right_w - stm.width) / 2, iy, ui_draw::FontStyle::Serif, st_px, true,
                           fit_st, e.unlocked ? acc : ui_draw::rgba(244, 240, 232, 255), true);
        iy += stm.height + spx(8.0f, 4);

        // Centered Pill Row (Tier + Unlocked/Locked + Optional Missable)
        const int sp_h = spx(22.0f, 16);
        const int sp_px = spx(11.0f, 8);
        const std::string tier_lbl = achievements::tier_name(e.tier);
        const std::string stat_lbl = e.unlocked ? "UNLOCKED" : "LOCKED";
        const int w1 = ui_draw::measure_text(ui_draw::FontStyle::Sans, sp_px, true, tier_lbl).width + spx(16.0f, 10);
        const int w2 = ui_draw::measure_text(ui_draw::FontStyle::Sans, sp_px, true, stat_lbl).width + spx(16.0f, 10);
        const int w3 = (e.missable && !masked) ? (ui_draw::measure_text(ui_draw::FontStyle::Sans, sp_px, true, "MISSABLE").width + spx(16.0f, 10)) : 0;
        const int pgap = spx(6.0f, 4);
        const int total_pw = w1 + pgap + w2 + (w3 > 0 ? (pgap + w3) : 0);
        int px = right_x + (right_w - total_pw) / 2;
        ui_draw::draw_pill(surf, px, iy, sp_h, ui_draw::FontStyle::Sans, sp_px, true, tier_lbl,
                           ui_draw::with_alpha(gl, 235), bdr, acc);
        px += w1 + pgap;
        ui_draw::draw_pill(surf, px, iy, sp_h, ui_draw::FontStyle::Sans, sp_px, true, stat_lbl,
                           e.unlocked ? ui_draw::rgba(20, 82, 60, 240) : ui_draw::rgba(34, 38, 56, 235),
                           e.unlocked ? ui_draw::rgba(110, 231, 180, 255) : ui_draw::rgba(110, 118, 146, 210),
                           e.unlocked ? ui_draw::rgba(210, 255, 234, 255) : ui_draw::rgba(192, 198, 218, 240));
        if (w3 > 0) {
            px += w2 + pgap;
            ui_draw::draw_pill(surf, px, iy, sp_h, ui_draw::FontStyle::Sans, sp_px, true, "MISSABLE",
                               ui_draw::rgba(92, 46, 16, 240), ui_draw::rgba(250, 178, 72, 250), ui_draw::rgba(255, 232, 178, 255));
        }
        iy += sp_h + spx(14.0f, 8);

        // Full Wrapped Description
        const std::string full_desc = masked
            ? "Hidden Curiosity — explore 1915 Europe & Japan or press Tab (or click Reveal Secrets) to unveil its unlock hint."
            : e.description;
        const int desc_h = ui_draw::draw_wrapped_text(surf, ix, iy, iw, 4, spx(4.0f, 2),
                                                      ui_draw::FontStyle::Sans, spx(13.5f, 10), false,
                                                      full_desc, ui_draw::rgba(222, 226, 238, 245), false);
        iy += std::max(desc_h, spx(38.0f, 24)) + spx(12.0f, 6);

        // Progress / Timestamp Card
        const int pbox_h = spx(54.0f, 38);
        surf.fill_rounded_rect(ix, iy, iw, pbox_h, static_cast<float>(spx(8.0f, 5)), ui_draw::rgba(14, 15, 26, 230));
        surf.stroke_rounded_rect(ix, iy, iw, pbox_h, static_cast<float>(spx(8.0f, 5)), 1.0f, ui_draw::rgba(76, 82, 110, 170));
        const int pb_pad = spx(10.0f, 6);
        const std::string meta_line = e.unlocked
            ? ("Unlocked on " + e.unlocked_at)
            : (masked ? std::string("Progress: Hidden")
                      : ("Progress: " + (e.progress_text.empty() ? std::string("Not yet started") : e.progress_text)));
        const int pct_i = e.unlocked ? 100 : (masked ? 0 : static_cast<int>(std::clamp(e.progress_ratio, 0.0, 1.0) * 100.0 + 0.5));
        const std::string pct_s = masked ? "?" : (std::to_string(pct_i) + "%");
        const int pct_w = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(12.5f, 9), true, pct_s).width;
        ui_draw::draw_text(surf, ix + pb_pad, iy + spx(8.0f, 5), ui_draw::FontStyle::Sans, spx(12.5f, 9), true,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(12.5f, 9), true, meta_line, iw - pb_pad * 2 - pct_w - 8),
                           e.unlocked ? ui_draw::rgba(130, 242, 194, 255) : ui_draw::rgba(224, 228, 240, 245), false);
        ui_draw::draw_text(surf, ix + iw - pb_pad - pct_w, iy + spx(8.0f, 5), ui_draw::FontStyle::Sans, spx(12.5f, 9), true,
                           pct_s, e.unlocked ? ui_draw::rgba(130, 242, 194, 255) : acc, false);
        surf.draw_progress_bar(ix + pb_pad, iy + pbox_h - spx(17.0f, 12), iw - pb_pad * 2, spx(9.0f, 6),
                               e.unlocked ? 1.0f : (masked ? 0.0f : static_cast<float>(std::clamp(e.progress_ratio, 0.0, 1.0))),
                               ui_draw::rgba(24, 26, 42, 255),
                               e.unlocked ? ui_draw::rgba(56, 184, 134, 255) : ui_draw::scale_rgb(bdr, 0.85f),
                               e.unlocked ? ui_draw::rgba(138, 248, 200, 255) : acc,
                               ui_draw::rgba(92, 98, 126, 180));
        iy += pbox_h + spx(14.0f, 8);

        // Bottom Showcase Area: Captured Screenshot Thumbnail (if unlocked & present) OR Tier Mastery Breakdown
        const int rem_h = (body_y + body_h - ip) - iy;
        if (rem_h >= spx(84.0f, 58)) {
            bool drew_shot = false;
            if (e.unlocked) {
                const int img_h = std::max(40, rem_h - spx(26.0f, 18));
                const auto &thumb = lookup_ach_thumb(e.id, e.title, e.screenshot_title, iw - 8, img_h);
                if (thumb.ok && !thumb.rgba.empty() && thumb.w > 0 && thumb.h > 0) {
                    ui_draw::draw_text(surf, ix, iy, ui_draw::FontStyle::Sans, spx(11.5f, 9), true,
                                       "CAPTURED MOMENT", ui_draw::rgba(216, 186, 118, 235), false);
                    const int fy = iy + spx(18.0f, 13);
                    const int fw = std::min(iw, thumb.w + 6);
                    const int fh = std::min(rem_h - spx(20.0f, 14), thumb.h + 6);
                    const int fx = ix + (iw - fw) / 2;
                    surf.fill_rounded_rect(fx, fy, fw, fh, 6.0f, ui_draw::rgba(8, 8, 14, 245));
                    surf.blit_rgba_fit(fx + 3, fy + 3, fw - 6, fh - 6, thumb.rgba.data(), thumb.w, thumb.h, 4.0f);
                    surf.stroke_rounded_rect(fx, fy, fw, fh, 6.0f, 1.3f, ui_draw::with_alpha(bdr, 210));
                    drew_shot = true;
                }
            }
            if (!drew_shot) {
                ui_draw::draw_text(surf, ix, iy, ui_draw::FontStyle::Sans, spx(11.5f, 9), true,
                                   "TIER MASTERY BREAKDOWN", ui_draw::rgba(216, 186, 118, 235), false);
                int my = iy + spx(20.0f, 14);
                static constexpr int kMasteryOrder[6] = {0, 1, 2, 3, 5, 6};
                static const char *const kMasteryNames[6] = {"Mark", "Sigil", "Crest", "Sovereign", "Follies", "Curiosities"};
                const int row_h = std::clamp((rem_h - spx(22.0f, 14)) / 6, spx(14.0f, 11), spx(24.0f, 16));
                const int lbl_w = spx(76.0f, 52);
                const int cnt_w = spx(46.0f, 34);
                const int mbar_w = std::max(40, iw - lbl_w - cnt_w - spx(12.0f, 8));
                const int mf_px = spx(11.0f, 9);
                for (int k = 0; k < 6 && my + row_h <= body_y + body_h - ip + 4; ++k) {
                    const int ti = kMasteryOrder[k];
                    std::uint32_t mgl, mgr, mbdr, macc;
                    achievements::Manager::theme_colors(static_cast<achievements::Tier>(ti), values.toast_theme, mgl, mgr, mbdr, macc);
                    const float mrat = tier_t[ti] > 0 ? static_cast<float>(tier_u[ti]) / static_cast<float>(tier_t[ti]) : 0.0f;
                    ui_draw::draw_text(surf, ix, my, ui_draw::FontStyle::Sans, mf_px, false, kMasteryNames[k],
                                       ui_draw::lerp_color(macc, ui_draw::rgba(220, 224, 236, 240), 0.35f), false);
                    surf.draw_progress_bar(ix + lbl_w, my + spx(4.0f, 2), mbar_w, spx(7.0f, 5), mrat,
                                           ui_draw::rgba(14, 15, 26, 235), ui_draw::scale_rgb(mbdr, 0.85f), macc,
                                           ui_draw::rgba(72, 78, 104, 165));
                    const std::string cstr = std::to_string(tier_u[ti]) + "/" + std::to_string(tier_t[ti]);
                    const int cw = ui_draw::measure_text(ui_draw::FontStyle::Sans, mf_px, false, cstr).width;
                    ui_draw::draw_text(surf, ix + iw - cw, my, ui_draw::FontStyle::Sans, mf_px, false, cstr,
                                       tier_u[ti] >= tier_t[ti] && tier_t[ti] > 0 ? ui_draw::rgba(130, 242, 194, 255) : ui_draw::rgba(176, 184, 206, 235), false);
                    my += row_h;
                }
            }
        }
    } else {
        ui_draw::draw_text(surf, ix, iy + spx(20.0f, 12), ui_draw::FontStyle::Serif, spx(16.0f, 12), false,
                           "Select an achievement to inspect.", ui_draw::rgba(176, 182, 204, 230), false);
    }

    // -------------------------------------------------------------------------
    // 5. Footer Controls & Position Counter
    // -------------------------------------------------------------------------
    const int fy = panel_h - footer_h - pad / 2;
    surf.fill_rect(inner_x, fy - spx(6.0f, 3), inner_w, 1, ui_draw::rgba(140, 114, 72, 100));
    const int foot_px = spx(11.5f, 9);
    const std::string pos_str = (n > 0 && selected_ < n)
        ? ("Showing " + std::to_string(selected_ + 1) + " of " + std::to_string(n) + "  (Page " +
           std::to_string(selected_ / std::max(1, ach_visible_) + 1) + " / " +
           std::to_string((n + std::max(1, ach_visible_) - 1) / std::max(1, ach_visible_)) + ")")
        : (n > 0 ? ("Back  (" + std::to_string(n) + " achievements)") : std::string("0 matching achievements"));
    ui_draw::draw_text(surf, inner_x, fy + spx(4.0f, 2), ui_draw::FontStyle::Sans, foot_px, true,
                       pos_str, ui_draw::rgba(218, 194, 134, 240), false);
    const std::string hint = "Up/Down/Wheel: Browse  |  PgUp/PgDn: Jump  |  Q/E or Click: Tier  |  L/R: Status  |  Tab: Secrets  |  Esc: Back";
    const std::string fit_hint = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, foot_px, false, hint, inner_w - spx(230.0f, 140));
    const int hw = ui_draw::measure_text(ui_draw::FontStyle::Sans, foot_px, false, fit_hint).width;
    ui_draw::draw_text(surf, inner_x + inner_w - hw, fy + spx(4.0f, 2), ui_draw::FontStyle::Sans, foot_px, false,
                       fit_hint, ui_draw::rgba(154, 162, 186, 235), false);
}

void Menu::draw_save_states(int window_width, int window_height, const Values &values, Picture &out) {
    const int panel_w = std::clamp(window_width * 78 / 100, std::min(window_width - 24, 740), 1160);
    const int panel_h = std::clamp(window_height * 84 / 100, std::min(window_height - 20, 500), 800);
    out.width = panel_w;
    out.height = panel_h;
    out.x = std::max(0, (window_width - panel_w) / 2);
    out.y = std::max(0, (window_height - panel_h) / 2);
    out.rgba.assign(static_cast<std::size_t>(panel_w) * panel_h, 0u);

    const float sc = std::clamp(std::min(static_cast<float>(panel_w) / 980.0f, static_cast<float>(panel_h) / 620.0f), 0.70f, 1.28f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    ui_draw::Surface surf(out.rgba.data(), panel_w, panel_h);
    draw_nocturne_panel(surf, panel_w, panel_h, sc);

    const int pad = spx(22.0f, 12);
    const int ix = pad;
    const int iw = panel_w - pad * 2;
    int y = pad;

    // 1. Header
    const std::string title = save_mode_ == SavePickerMode::save_pick ? "Quick Save  —  Choose Slot (0–9)"
                            : (save_mode_ == SavePickerMode::load_pick ? "Quick Load  —  Choose Slot (0–9)"
                                                                       : (std::string(game_display_title()) + "  —  Save States"));
    ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, spx(22.0f, 15), true, title, pal::kTitle, true);
    const std::string active_lbl = "Active: Slot " + std::to_string(values.active_slot);
    const int apx = spx(11.5f, 9);
    const int aph = spx(24.0f, 18);
    const int apw = ui_draw::measure_text(ui_draw::FontStyle::Sans, apx, true, active_lbl).width + spx(18.0f, 12);
    ui_draw::draw_pill(surf, ix + iw - apw, y + spx(2.0f, 1), aph, ui_draw::FontStyle::Sans, apx, true, active_lbl,
                       ui_draw::rgba(24, 72, 58, 235), pal::kOnBdr, pal::kOnText);

    y += spx(36.0f, 26);
    surf.fill_rect(ix, y, iw, 1, pal::kDivider);
    y += spx(12.0f, 7);

    // 2. Two-column body (Left: 10 slots + Back | Right: Rounded Thumbnail Preview + Metadata Card)
    const int footer_h = spx(28.0f, 20);
    const int body_h = std::max(spx(300.0f, 220), panel_h - y - footer_h - pad);
    const int col_gap = spx(16.0f, 10);
    const int left_w = (iw - col_gap) * 53 / 100;
    const int right_x = ix + left_w + col_gap;
    const int right_w = iw - left_w - col_gap;

    const int back_gap = spx(8.0f, 5);
    const int row_stride = std::max(spx(24.0f, 19), (body_h - back_gap) / 12);
    const int row_gap = spx(3.0f, 2);
    const int row_h = std::max(17, row_stride - row_gap);

    entry_x_ = out.x + ix;
    entry_w_ = left_w;
    entry_h_ = row_stride;
    rows_y_ = out.y + y;

    const int lbl_px = spx(13.0f, 10);
    const int sm_px = spx(11.0f, 8);

    for (int i = 0; i < 11; ++i) {
        const bool is_auto = (i == 10);
        const auto &sl = is_auto ? values.autosave_slot : values.slots[i];
        const bool lit = (selected_ == i);
        const bool is_active = (!is_auto && i == values.active_slot);
        const int ry = y + i * row_stride;
        draw_nocturne_row(surf, ix, ry, left_w, row_h, sc, lit, false, !sl.exists);

        const int cy = ry + row_h / 2;
        int cx = ix + spx(14.0f, 9);
        if (is_active || (is_auto && sl.exists)) {
            surf.fill_diamond(static_cast<float>(cx), static_cast<float>(cy), 4.0f * sc, 4.0f * sc,
                              lit ? ui_draw::rgba(255, 246, 206, 255) : (is_auto ? pal::kTitle : pal::kEmerald));
        } else {
            surf.fill_circle(static_cast<float>(cx), static_cast<float>(cy), 2.5f * sc,
                             sl.exists ? pal::kGoldFrame : ui_draw::rgba(86, 92, 118, 170));
        }
        cx += spx(12.0f, 8);

        const std::string slot_lbl = is_auto ? "Auto" : ("Slot " + std::to_string(i));
        const auto slm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, true, slot_lbl);
        ui_draw::draw_text(surf, cx, cy - slm.height / 2, ui_draw::FontStyle::Sans, lbl_px, true, slot_lbl,
                           lit ? ui_draw::rgba(255, 246, 214, 255) : (sl.exists ? pal::kTitle : pal::kDim), lit);
        cx += spx(58.0f, 44);

        const int rx = ix + left_w - spx(10.0f, 6);
        int sz_w = 0;
        if (sl.exists && sl.bytes > 0) {
            sz_w = draw_keycap_pill(surf, rx, cy, row_h, format_bytes(sl.bytes), lit, sc) + spx(8.0f, 5);
        }

        std::string summary;
        if (!sl.exists) {
            summary = is_auto ? "No Area Autosave Yet" : "Empty Slot";
        } else {
            summary = !sl.area.empty() ? sl.area : ("Frame " + std::to_string(sl.frame));
            if (sl.max_level > 0) summary += "  •  Lv " + std::to_string(sl.max_level);
        }
        const int max_sum_w = std::max(40, (rx - sz_w) - cx - spx(6.0f, 4));
        const std::string sfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, lbl_px, false, summary, max_sum_w);
        const auto sm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, false, sfit);
        ui_draw::draw_text(surf, cx, cy - sm.height / 2, ui_draw::FontStyle::Sans, lbl_px, false, sfit,
                           lit ? ui_draw::rgba(255, 244, 218, 255) : (sl.exists ? pal::kText : pal::kDim), false);
    }

    const int back_y = y + 11 * row_stride + back_gap;
    footer_y_ = out.y + back_y;
    {
        const bool back_lit = (selected_ >= 11);
        draw_nocturne_row(surf, ix, back_y, left_w, row_h, sc, back_lit, false, true);
        const std::string blbl = direct_picker_ ? "Cancel" : "Back to Pause Menu";
        const auto bm = ui_draw::measure_text(ui_draw::FontStyle::Sans, lbl_px, back_lit, blbl);
        ui_draw::draw_text(surf, ix + spx(14.0f, 9), back_y + (row_h - bm.height) / 2, ui_draw::FontStyle::Sans, lbl_px, back_lit, blbl,
                           back_lit ? ui_draw::rgba(255, 246, 214, 255) : pal::kTextSoft, back_lit);
        draw_keycap_pill(surf, ix + left_w - spx(10.0f, 6), back_y + row_h / 2, row_h, "Esc", back_lit, sc);
    }

    // 3. Right Column: Rounded Inspector Card with Screenshot Preview & Rich Metadata
    const float card_rad = static_cast<float>(spx(11.0f, 6));
    const int card_h = (back_y + row_h) - y;
    surf.fill_gradient_rounded_rect(right_x, y, right_w, card_h, card_rad, pal::kCardBgTop, pal::kCardBgBot, 0.22f, false);
    surf.stroke_rounded_rect(right_x, y, right_w, card_h, card_rad, 1.3f, pal::kCardBdr);

    const int cp = spx(16.0f, 10);
    const int cx0 = right_x + cp;
    const int cw = right_w - cp * 2;
    int cy_pos = y + cp;

    const int cur_slot = (selected_ >= 0 && selected_ <= 10) ? selected_ : values.active_slot;
    const bool cur_is_auto = (cur_slot == 10);
    const auto &cur = cur_is_auto ? values.autosave_slot : values.slots[cur_slot];

    // Thumbnail frame (16:10 / 4:3 friendly)
    const int thumb_w = cw;
    const int thumb_h = std::min(card_h * 46 / 100, thumb_w * 112 / 160);
    const float trad = static_cast<float>(spx(8.0f, 5));
    surf.fill_rounded_rect(cx0, cy_pos, thumb_w, thumb_h, trad, ui_draw::rgba(10, 11, 20, 245));
    if (cur.exists && values.slot_thumb_rgba && !values.slot_thumb_rgba->empty() && values.slot_thumb_w > 0 && values.slot_thumb_h > 0) {
        surf.blit_rgba_fit(cx0 + 3, cy_pos + 3, thumb_w - 6, thumb_h - 6,
                           values.slot_thumb_rgba->data(), values.slot_thumb_w, values.slot_thumb_h, std::max(3.0f, trad - 2.0f));
    } else {
        const std::string ph = cur.exists ? "No Preview Captured" : (cur_is_auto ? "Auto-Saved on Area Entry" : "Empty Save Slot");
        const auto pm = ui_draw::measure_text(ui_draw::FontStyle::Serif, spx(15.0f, 11), false, ph);
        ui_draw::draw_text(surf, cx0 + (thumb_w - pm.width) / 2, cy_pos + (thumb_h - pm.height) / 2,
                           ui_draw::FontStyle::Serif, spx(15.0f, 11), false, ph, pal::kDim, false);
    }
    surf.stroke_rounded_rect(cx0, cy_pos, thumb_w, thumb_h, trad, 1.3f,
                             cur.exists ? pal::kGoldFrame : ui_draw::rgba(92, 96, 124, 175));
    cy_pos += thumb_h + spx(14.0f, 8);

    // Slot Header + Active / Autosave pill
    const std::string hdr = cur_is_auto ? "Area Autosave" : ("Slot " + std::to_string(cur_slot));
    ui_draw::draw_text(surf, cx0, cy_pos, ui_draw::FontStyle::Serif, spx(20.0f, 14), true, hdr, pal::kTitle, true);
    if (cur_is_auto) {
        const int ph = spx(21.0f, 16);
        const int pw = ui_draw::measure_text(ui_draw::FontStyle::Sans, sm_px, true, "ROLLING").width + spx(16.0f, 10);
        ui_draw::draw_pill(surf, cx0 + cw - pw, cy_pos + spx(2.0f, 1), ph, ui_draw::FontStyle::Sans, sm_px, true, "ROLLING",
                           ui_draw::rgba(56, 36, 22, 235), pal::kGoldFrame, pal::kTitle);
    } else if (cur_slot == values.active_slot) {
        const int ph = spx(21.0f, 16);
        const int pw = ui_draw::measure_text(ui_draw::FontStyle::Sans, sm_px, true, "ACTIVE").width + spx(16.0f, 10);
        ui_draw::draw_pill(surf, cx0 + cw - pw, cy_pos + spx(2.0f, 1), ph, ui_draw::FontStyle::Sans, sm_px, true, "ACTIVE",
                           pal::kOnBg, pal::kOnBdr, pal::kOnText);
    }
    cy_pos += spx(28.0f, 20);

    if (cur.exists) {
        // Metadata badge strip: Disc + Level
        const int ph = spx(22.0f, 16);
        int bx = cx0;
        const std::string d_lbl = "DISC " + std::to_string(cur.disc);
        const int dw = ui_draw::draw_pill(surf, bx, cy_pos, ph, ui_draw::FontStyle::Sans, sm_px, true, d_lbl,
                                          ui_draw::rgba(36, 28, 56, 235), pal::kGoldFrame, pal::kTitle);
        bx += dw + spx(6.0f, 4);
        if (cur.max_level > 0) {
            const std::string lv_lbl = "LV " + std::to_string(cur.max_level);
            const int lw = ui_draw::draw_pill(surf, bx, cy_pos, ph, ui_draw::FontStyle::Sans, sm_px, true, lv_lbl,
                                              pal::kOnBg, pal::kOnBdr, pal::kOnText);
            bx += lw + spx(6.0f, 4);
        }
        if (cur.bytes > 0) {
            ui_draw::draw_pill(surf, bx, cy_pos, ph, ui_draw::FontStyle::Sans, sm_px, false, format_bytes(cur.bytes) + " LZ4",
                               ui_draw::rgba(22, 24, 40, 235), pal::kOffBdr, pal::kTextSoft);
        }
        cy_pos += ph + spx(12.0f, 7);

        if (!cur.area.empty()) {
            ui_draw::draw_text(surf, cx0, cy_pos, ui_draw::FontStyle::Sans, spx(14.0f, 11), true,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(14.0f, 11), true, cur.area, cw),
                               pal::kText, false);
            cy_pos += spx(22.0f, 16);
        }
        if (!cur.timestamp.empty()) {
            ui_draw::draw_text(surf, cx0, cy_pos, ui_draw::FontStyle::Sans, spx(12.5f, 9), false,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(12.5f, 9), false, "Saved:  " + cur.timestamp, cw),
                               pal::kTextSoft, false);
            cy_pos += spx(20.0f, 14);
        }
        if (cur.playtime_sec > 0.5f) {
            const int sec = static_cast<int>(cur.playtime_sec);
            char pt[48];
            std::snprintf(pt, sizeof pt, "Playtime:  %d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
            ui_draw::draw_text(surf, cx0, cy_pos, ui_draw::FontStyle::Sans, spx(12.5f, 9), false, pt, pal::kTextSoft, false);
            cy_pos += spx(22.0f, 16);
        }

        if (slot_delete_armed_ || slot_save_armed_) {
            const std::string warn = slot_delete_armed_
                ? "Press Del / Triangle again to DELETE this slot!"
                : "Press X / F5 again to OVERWRITE this slot!";
            const int wh = spx(32.0f, 24);
            surf.fill_gradient_rounded_rect(cx0, cy_pos, cw, wh, static_cast<float>(spx(6.0f, 4)),
                                            pal::kDangerLeft, pal::kDangerRight, 0.15f, true);
            surf.stroke_rounded_rect(cx0, cy_pos, cw, wh, static_cast<float>(spx(6.0f, 4)), 1.3f, pal::kDangerBdr);
            const std::string wfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(12.0f, 9), true, warn, cw - spx(16.0f, 10));
            const auto wm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(12.0f, 9), true, wfit);
            ui_draw::draw_text(surf, cx0 + (cw - wm.width) / 2, cy_pos + (wh - wm.height) / 2,
                               ui_draw::FontStyle::Sans, spx(12.0f, 9), true, wfit, ui_draw::rgba(255, 240, 232, 255), false);
        }
    } else {
        ui_draw::draw_wrapped_text(surf, cx0, cy_pos, cw, 3, spx(4.0f, 2), ui_draw::FontStyle::Sans, spx(13.0f, 10), false,
                                   "No save state stored in this slot yet. Press X or F5 to capture your exact position and screenshot.",
                                   pal::kDim, false);
    }

    // 4. Footer controls hint
    const int fy = panel_h - pad - footer_h + spx(6.0f, 3);
    surf.fill_rect(ix, fy - spx(6.0f, 3), iw, 1, pal::kDivider);
    const std::string hint = save_mode_ == SavePickerMode::save_pick
        ? "Enter / 0–9 / Cross: Save to Slot   •   Esc / Circle: Cancel"
        : (save_mode_ == SavePickerMode::load_pick
            ? "Enter / 0–9 / Cross: Load Slot   •   Esc / Circle: Cancel"
            : "Enter / L: Load   •   X / F5: Save   •   Del / Y: Delete   •   0–9: Jump   •   Esc: Back");
    ui_draw::draw_text(surf, ix, fy, ui_draw::FontStyle::Sans, spx(11.5f, 9), false,
                       ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(11.5f, 9), false, hint, iw), pal::kDim, false);
}

void Menu::draw_mods(int window_width, int window_height, const Values &values, Picture &out) {
    const float sc = std::clamp(static_cast<float>(window_height) / 720.0f, 0.78f, 1.55f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    mod_rows_ = static_cast<int>(values.mod_rows.size());
    if (mod_settings_idx_ >= mod_rows_) {
        mod_settings_idx_ = -1;
        mod_settings_rows_ = 0;
    }
    const bool in_cfg = (mod_settings_idx_ >= 0);
    if (in_cfg) {
        mod_settings_rows_ = static_cast<int>(values.mod_rows[static_cast<std::size_t>(mod_settings_idx_)].settings.size());
    }
    const int kModsVisible = 7;
    const int total_items = in_cfg ? mod_settings_rows_ : mod_rows_;
    const int max_sel = in_cfg ? (mod_settings_rows_ + 1) : mod_rows_;
    if (selected_ > max_sel) selected_ = max_sel;
    fit(total_items, kModsVisible);
    mods_shown_ = std::min(total_items, kModsVisible);

    const int pw = std::clamp(spx(640.0f, 440), std::min(window_width - 16, 440), std::max(440, window_width - 28));
    const int pad = spx(22.0f, 14);
    const int hdr_h = spx(54.0f, 40);
    const int row_h = spx(48.0f, 36);
    const int footer_h = in_cfg ? (row_h * 2) : spx(34.0f, 26);
    const int hint_h = spx(26.0f, 20);
    const int list_rows = std::max(1, mods_shown_);
    const int ph = std::min(window_height - 20, pad * 2 + hdr_h + list_rows * row_h + spx(14.0f, 8) + footer_h + hint_h);

    out.width = pw;
    out.height = ph;
    out.x = std::max(0, (window_width - pw) / 2);
    out.y = std::max(0, (window_height - ph) / 2);
    out.rgba.assign(static_cast<std::size_t>(pw) * ph, 0u);

    ui_draw::Surface surf(out.rgba.data(), pw, ph);
    draw_nocturne_panel(surf, pw, ph, sc);

    const int ix = pad;
    const int iw = pw - pad * 2;
    int y = pad;

    if (in_cfg) {
        const auto &cm = values.mod_rows[static_cast<std::size_t>(mod_settings_idx_)];
        ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, spx(20.0f, 15), true,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Serif, spx(20.0f, 15), true, cm.name + " — SETTINGS", iw),
                           pal::kTitle, true);
        const std::string sub = "Live per-mod settings  •  Saved immediately to mod_config.ini";
        ui_draw::draw_text(surf, ix, y + spx(26.0f, 20), ui_draw::FontStyle::Sans, spx(11.5f, 9), false,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(11.5f, 9), false, sub, iw), pal::kDim, false);
        y += hdr_h;
        surf.fill_rect(ix, y - spx(6.0f, 4), iw, 1, pal::kDivider);

        entry_x_ = out.x + ix;
        entry_w_ = iw;
        entry_h_ = row_h;
        rows_y_ = out.y + y;

        for (int i = 0; i < mods_shown_; ++i) {
            const int idx = top_ + i;
            if (idx < 0 || idx >= mod_settings_rows_) break;
            const auto &sd = cm.settings[static_cast<std::size_t>(idx)];
            const bool lit = (idx == selected_);
            const int ry = y + i * row_h;
            const int rh = row_h - spx(4.0f, 2);
            draw_nocturne_row(surf, ix, ry, iw, rh, sc, lit, false, false);

            const std::string val_txt = "< " + sd.value + " >";
            const auto vm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(13.0f, 10), true, val_txt);
            const int vx = ix + iw - vm.width - spx(12.0f, 8);
            ui_draw::draw_text(surf, vx, ry + (rh - vm.height) / 2, ui_draw::FontStyle::Sans, spx(13.0f, 10), true,
                               val_txt, lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kTitle, lit);

            const int text_max = std::max(100, (vx - ix) - spx(20.0f, 12));
            ui_draw::draw_text(surf, ix + spx(12.0f, 8), ry + spx(6.0f, 4), ui_draw::FontStyle::Sans, spx(13.5f, 11), lit,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(13.5f, 11), lit, sd.label, text_max),
                               lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kText, lit);
            const std::string desc = sd.description.empty() ? "Left/Right or Enter to adjust" : sd.description;
            ui_draw::draw_text(surf, ix + spx(12.0f, 8), ry + spx(25.0f, 19), ui_draw::FontStyle::Sans, spx(10.5f, 8), false,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(10.5f, 8), false, desc, text_max),
                               pal::kDim, false);
        }
        y += list_rows * row_h + spx(8.0f, 4);
        footer_y_ = out.y + y;

        const bool rst_lit = (selected_ == mod_settings_rows_);
        draw_nocturne_row(surf, ix, y, iw, row_h - spx(4.0f, 2), sc, rst_lit, false, true);
        const auto rm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(13.0f, 10), rst_lit, "Reset to Defaults");
        ui_draw::draw_text(surf, ix + spx(12.0f, 8), y + (row_h - spx(4.0f, 2) - rm.height) / 2,
                           ui_draw::FontStyle::Sans, spx(13.0f, 10), rst_lit, "Reset to Defaults",
                           rst_lit ? ui_draw::rgba(255, 226, 156, 255) : pal::kTitle, rst_lit);
        y += row_h;

        const bool back_lit = (selected_ == mod_settings_rows_ + 1);
        draw_nocturne_row(surf, ix, y, iw, row_h - spx(4.0f, 2), sc, back_lit, false, true);
        const auto bm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(13.0f, 10), back_lit, "< Back to Mods List");
        ui_draw::draw_text(surf, ix + spx(12.0f, 8), y + (row_h - spx(4.0f, 2) - bm.height) / 2,
                           ui_draw::FontStyle::Sans, spx(13.0f, 10), back_lit, "< Back to Mods List",
                           back_lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kTextSoft, back_lit);

        const int fy = ph - pad - spx(14.0f, 10);
        surf.fill_rect(ix, fy - spx(6.0f, 3), iw, 1, pal::kDivider);
        ui_draw::draw_text(surf, ix, fy, ui_draw::FontStyle::Sans, spx(11.0f, 9), false,
                           "Left / Right / Enter: Change Setting Live   •   Esc / Circle: Back to Mods",
                           pal::kDim, false);
        return;
    }

    ui_draw::draw_text(surf, ix, y, ui_draw::FontStyle::Serif, spx(20.0f, 15), true,
                       "GAMEPLAY MODS (LIVE)", pal::kTitle, true);
    const std::string sub = std::to_string(values.active_mod_count) + " / " + std::to_string(mod_rows_) +
                            " active  •  Layer 2 (.pnach) & Layer 3 (.lua) hot-toggle immediately";
    ui_draw::draw_text(surf, ix, y + spx(26.0f, 20), ui_draw::FontStyle::Sans, spx(11.5f, 9), false,
                       ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(11.5f, 9), false, sub, iw), pal::kDim, false);
    y += hdr_h;
    surf.fill_rect(ix, y - spx(6.0f, 4), iw, 1, pal::kDivider);

    entry_x_ = out.x + ix;
    entry_w_ = iw;
    entry_h_ = row_h;
    rows_y_ = out.y + y;

    if (mod_rows_ == 0) {
        ui_draw::draw_text(surf, ix + spx(12.0f, 8), y + spx(12.0f, 8), ui_draw::FontStyle::Sans, spx(13.0f, 10), false,
                           "No mods installed in games/<game>/mods/ yet. Install or create mods in the Launcher.",
                           pal::kTextSoft, false);
        y += row_h;
    } else {
        for (int i = 0; i < mods_shown_; ++i) {
            const int idx = top_ + i;
            if (idx < 0 || idx >= mod_rows_) break;
            const auto &m = values.mod_rows[static_cast<std::size_t>(idx)];
            const bool lit = (idx == selected_);
            const int ry = y + i * row_h;
            const int rh = row_h - spx(4.0f, 2);
            draw_nocturne_row(surf, ix, ry, iw, rh, sc, lit, false, !m.enabled);

            const std::string pill_txt = m.enabled ? "ON" : "OFF";
            const int pill_w = spx(46.0f, 36);
            const int pill_h = spx(22.0f, 18);
            const int px = ix + iw - pill_w - spx(10.0f, 6);
            const int py = ry + (rh - pill_h) / 2;
            surf.fill_rounded_rect(px, py, pill_w, pill_h, 5.0f,
                                   m.enabled ? ui_draw::rgba(32, 108, 66, 220) : ui_draw::rgba(46, 36, 52, 200));
            surf.stroke_rounded_rect(px, py, pill_w, pill_h, 5.0f, 1.0f,
                                     m.enabled ? ui_draw::rgba(110, 226, 154, 220) : pal::kDivider);
            const auto pm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(11.0f, 9), true, pill_txt);
            ui_draw::draw_text(surf, px + (pill_w - pm.width) / 2, py + (pill_h - pm.height) / 2,
                               ui_draw::FontStyle::Sans, spx(11.0f, 9), true, pill_txt,
                               m.enabled ? ui_draw::rgba(232, 255, 240, 255) : pal::kDim, false);

            int right_edge = px;
            if (!m.settings.empty()) {
                const std::string cfg_txt = "Settings ->";
                const int cfg_w = spx(86.0f, 68);
                const int cx = px - cfg_w - spx(8.0f, 6);
                surf.fill_rounded_rect(cx, py, cfg_w, pill_h, 5.0f, ui_draw::rgba(82, 58, 28, 210));
                surf.stroke_rounded_rect(cx, py, cfg_w, pill_h, 5.0f, 1.0f, pal::kTitle);
                const auto cm2 = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(10.5f, 8), true, cfg_txt);
                ui_draw::draw_text(surf, cx + (cfg_w - cm2.width) / 2, py + (pill_h - cm2.height) / 2,
                                   ui_draw::FontStyle::Sans, spx(10.5f, 8), true, cfg_txt,
                                   ui_draw::rgba(255, 236, 178, 255), false);
                right_edge = cx;
            }

            const int text_max = std::max(100, (right_edge - ix) - spx(20.0f, 12));
            const std::string title = std::to_string(idx + 1) + ". " + m.name + "  v" + m.version;
            ui_draw::draw_text(surf, ix + spx(12.0f, 8), ry + spx(6.0f, 4), ui_draw::FontStyle::Sans, spx(13.5f, 11), lit || m.enabled,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(13.5f, 11), lit || m.enabled, title, text_max),
                               lit ? ui_draw::rgba(255, 244, 206, 255) : (m.enabled ? pal::kText : pal::kDim), lit);

            std::string meta = "Files: " + std::to_string(m.file_count) +
                               "  •  Patches: " + std::to_string(m.patch_count) +
                               "  •  Lua Scripts: " + std::to_string(m.script_count);
            if (m.file_count > 0) meta += "  (files need restart)";
            ui_draw::draw_text(surf, ix + spx(12.0f, 8), ry + spx(25.0f, 19), ui_draw::FontStyle::Sans, spx(10.5f, 8), false,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx(10.5f, 8), false, meta, text_max),
                               m.script_count > 0 ? pal::kTitle : pal::kDim, false);
        }
        y += mods_shown_ * row_h;
    }

    y += spx(8.0f, 4);
    footer_y_ = out.y + y;
    const bool back_lit = (selected_ == mod_rows_);
    draw_nocturne_row(surf, ix, y, iw, footer_h, sc, back_lit, false, true);
    const auto bm = ui_draw::measure_text(ui_draw::FontStyle::Sans, spx(13.0f, 10), back_lit, "Back to Main Menu");
    ui_draw::draw_text(surf, ix + spx(12.0f, 8), y + (footer_h - bm.height) / 2, ui_draw::FontStyle::Sans, spx(13.0f, 10), back_lit,
                       "Back to Main Menu", back_lit ? ui_draw::rgba(255, 244, 206, 255) : pal::kTextSoft, back_lit);

    const int fy = ph - pad - spx(14.0f, 10);
    surf.fill_rect(ix, fy - spx(6.0f, 3), iw, 1, pal::kDivider);
    ui_draw::draw_text(surf, ix, fy, ui_draw::FontStyle::Sans, spx(11.0f, 9), false,
                       "Enter / Cross: Toggle Mod   •   Right: Mod Settings   •   Esc / Circle: Back",
                       pal::kDim, false);
}

void Menu::draw(int window_width, int window_height, const Values &values, Picture &out) {
    switch (page_) {
    case Page::main: draw_main(window_width, window_height, values, out); break;
    case Page::settings: draw_settings(window_width, window_height, values, out); break;
    case Page::keys: draw_keys(window_width, window_height, values, out); break;
    case Page::achievements: draw_achievements(window_width, window_height, values, out); break;
    case Page::mods: draw_mods(window_width, window_height, values, out); break;
    case Page::save_states: draw_save_states(window_width, window_height, values, out); break;
    }
}

void Menu::draw_note(int window_width, int window_height, const std::string &text, Picture &out) {
    const float sc = std::clamp(static_cast<float>(window_height) / 720.0f, 0.80f, 1.60f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const int px_sz = spx(15.0f, 12);
    const int pad_x = spx(22.0f, 14);
    const int pad_y = spx(11.0f, 7);
    const int icon_gap = spx(16.0f, 12);
    const int max_w = std::max(200, window_width * 86 / 100);
    const std::string fitted = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, px_sz, true, text, max_w - pad_x * 2 - icon_gap);
    const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, px_sz, true, fitted);

    out.width = std::min(max_w, tm.width + pad_x * 2 + icon_gap);
    out.height = std::max(spx(38.0f, 28), tm.height + pad_y * 2);
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(spx(16.0f, 10), window_height / 24);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, 0u);

    ui_draw::Surface surf(out.rgba.data(), out.width, out.height);
    const float rad = static_cast<float>(out.height) * 0.5f;
    surf.fill_gradient_rounded_rect(0, 0, out.width, out.height, rad, pal::kBgTop, pal::kBgBot, 0.24f, true);
    surf.stroke_rounded_rect(0, 0, out.width, out.height, rad, std::max(1.4f, 1.6f * sc), pal::kGoldFrame);
    surf.stroke_rounded_rect(2, 2, out.width - 4, out.height - 4, std::max(4.0f, rad - 2.0f), 1.0f, pal::kGoldHair);

    const float dy = static_cast<float>(out.height) * 0.5f;
    surf.fill_diamond(static_cast<float>(pad_x + spx(2.0f, 1)), dy, 4.5f * sc, 4.5f * sc, pal::kTitle);
    ui_draw::draw_text(surf, pad_x + icon_gap, (out.height - tm.height) / 2,
                       ui_draw::FontStyle::Sans, px_sz, true, fitted, pal::kTitle, true);
}

void Menu::draw_perf_hud(int window_width, int window_height, const std::string &text, bool turbo_on, Picture &out) {
    const float sc = std::clamp(static_cast<float>(window_height) / 720.0f, 0.80f, 1.50f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const int px_sz = spx(12.5f, 10);
    const int pad_x = spx(12.0f, 8);
    const int pad_y = spx(6.0f, 4);
    const int dot_gap = spx(12.0f, 9);
    const int max_w = std::max(180, window_width * 65 / 100);
    const std::string fitted = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, px_sz, true, text, max_w - pad_x * 2 - dot_gap);
    const auto tm = ui_draw::measure_text(ui_draw::FontStyle::Sans, px_sz, true, fitted);

    out.width = std::min(max_w, tm.width + pad_x * 2 + dot_gap);
    out.height = std::max(spx(26.0f, 20), tm.height + pad_y * 2);
    out.x = spx(14.0f, 8);
    out.y = spx(12.0f, 8);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, 0u);

    ui_draw::Surface surf(out.rgba.data(), out.width, out.height);
    const float rad = static_cast<float>(out.height) * 0.5f;
    const std::uint32_t bg_l = turbo_on ? ui_draw::rgba(74, 24, 30, 212) : ui_draw::rgba(12, 13, 24, 198);
    const std::uint32_t bg_r = turbo_on ? ui_draw::rgba(116, 68, 22, 212) : ui_draw::rgba(22, 20, 36, 198);
    const std::uint32_t bdr  = turbo_on ? pal::kRowLitBdr : pal::kCardBdr;
    const std::uint32_t txt  = turbo_on ? ui_draw::rgba(255, 242, 188, 255) : pal::kText;
    surf.fill_gradient_rounded_rect(0, 0, out.width, out.height, rad, bg_l, bg_r, 0.18f, true);
    surf.stroke_rounded_rect(0, 0, out.width, out.height, rad, 1.2f, bdr);
    surf.fill_diamond(static_cast<float>(pad_x + spx(2.0f, 1)), static_cast<float>(out.height) * 0.5f,
                      3.5f * sc, 3.5f * sc, turbo_on ? pal::kTitle : pal::kEmerald);
    ui_draw::draw_text(surf, pad_x + dot_gap, (out.height - tm.height) / 2,
                       ui_draw::FontStyle::Sans, px_sz, true, fitted, txt, false);
}

void Menu::draw_photo_mode_bar(int window_width, int window_height, float zoom, float roll_deg,
                               float pan_x, float pan_y, bool frozen, bool hud_hidden, Picture &out) {
    const float sc = std::clamp(static_cast<float>(window_height) / 720.0f, 0.80f, 1.45f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const int bar_w = std::clamp(window_width * 88 / 100, std::min(window_width - 16, 520), 1120);
    const int bar_h = spx(62.0f, 46);
    out.width = bar_w;
    out.height = bar_h;
    out.x = std::max(0, (window_width - bar_w) / 2);
    out.y = std::max(0, window_height - bar_h - spx(16.0f, 10));
    out.rgba.assign(static_cast<std::size_t>(bar_w) * bar_h, 0u);

    ui_draw::Surface surf(out.rgba.data(), bar_w, bar_h);
    draw_nocturne_panel(surf, bar_w, bar_h, sc);

    char stat_buf[256];
    std::snprintf(stat_buf, sizeof stat_buf,
                  "PHOTO MODE  •  %s  •  2D HUD: %s  •  Zoom: %.2fx  •  Tilt: %+.1f deg  •  Pan: (%+.2f, %+.2f)",
                  frozen ? "FROZEN" : "LIVE",
                  hud_hidden ? "HIDDEN" : "SHOWN",
                  static_cast<double>(zoom),
                  static_cast<double>(roll_deg),
                  static_cast<double>(pan_x),
                  static_cast<double>(pan_y));

    const int top_px = spx(13.0f, 10);
    const int bot_px = spx(11.0f, 9);
    const int pad_x = spx(18.0f, 12);
    const std::string line1 = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, top_px, true, stat_buf, bar_w - pad_x * 2);
    const std::string line2 = ui_draw::fit_text_px(
        ui_draw::FontStyle::Sans, bot_px, false,
        "WASD/L-Stick: Pan  •  Wheel/L2-R2: Zoom  •  Q-E/L1: Tilt  •  Space/R1: +1 Frame  •  P/Start: Live  •  Tab/Square: Hide HUD  •  R/L3: Reset  •  H/Tri: Bar  •  F12/Cross: Capture  •  Esc/F4: Exit",
        bar_w - pad_x * 2);

    ui_draw::draw_text(surf, pad_x, spx(10.0f, 6), ui_draw::FontStyle::Sans, top_px, true, line1, pal::kTitle, true);
    ui_draw::draw_text(surf, pad_x, spx(34.0f, 24), ui_draw::FontStyle::Sans, bot_px, false, line2, pal::kTextSoft, false);
}

void Menu::draw_coop_hud(int window_width, int window_height, const std::string &spotlight,
                         bool your_turn, const std::string &banner_title,
                         const std::string &banner_sub, std::uint32_t banner_accent, Picture &out) {
    const float sc = std::clamp(static_cast<float>(window_height) / 720.0f, 0.78f, 1.45f);
    const auto spx = [&](float base, int lo = 1) -> int { return std::max(lo, static_cast<int>(base * sc + 0.5f)); };

    const bool has_banner = !banner_title.empty();
    const int hud_w = std::clamp(window_width * 92 / 100, std::min(window_width - 12, 420), 960);
    const int pill_h = spx(30.0f, 22);
    const int banner_h = has_banner ? spx(54.0f, 40) : 0;
    const int gap = has_banner ? spx(8.0f, 6) : 0;
    const int total_h = pill_h + gap + banner_h;

    out.width = hud_w;
    out.height = total_h;
    out.x = std::max(0, (window_width - hud_w) / 2);
    out.y = spx(12.0f, 8);
    out.rgba.assign(static_cast<std::size_t>(hud_w) * total_h, 0u);

    ui_draw::Surface surf(out.rgba.data(), hud_w, total_h);

    // 1. Compact Turn Spotlight Pill (centered top)
    const int pill_px = spx(12.0f, 10);
    const int pill_pad = spx(16.0f, 10);
    const int pill_w = std::clamp(ui_draw::measure_text(ui_draw::FontStyle::Sans, pill_px, true, spotlight).width + pill_pad * 2 + spx(14.0f, 8),
                                  spx(200.0f, 150), hud_w);
    const int pill_x = (hud_w - pill_w) / 2;
    const std::uint32_t pill_bg = your_turn ? ui_draw::rgba(36, 22, 16, 228) : ui_draw::rgba(16, 20, 34, 218);
    const std::uint32_t pill_border = your_turn ? pal::kGoldFrame : ui_draw::rgba(116, 146, 210, 220);
    surf.fill_rounded_rect(pill_x, 0, pill_w, pill_h, spx(8.0f, 5), pill_bg);
    surf.stroke_rounded_rect(pill_x, 0, pill_w, pill_h, spx(8.0f, 5), 1.5f, pill_border);
    // Live authority status dot
    surf.fill_rounded_rect(pill_x + spx(10.0f, 7), (pill_h - spx(8.0f, 6)) / 2,
                           spx(8.0f, 6), spx(8.0f, 6), spx(4.0f, 3),
                           your_turn ? ui_draw::rgba(246, 204, 78, 255) : ui_draw::rgba(110, 176, 245, 255));
    const std::string fit_spot = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, pill_px, true, spotlight, pill_w - pill_pad * 2);
    ui_draw::draw_text(surf, pill_x + spx(24.0f, 16), (pill_h - pill_px) / 2 - 1,
                       ui_draw::FontStyle::Sans, pill_px, true, fit_spot,
                       your_turn ? pal::kTitle : pal::kText, true);

    // 2. Dramatic Turn-Handoff / Ring Callout / Emote Ping Banner
    if (has_banner) {
        const int by = pill_h + gap;
        const int bw = std::clamp(hud_w * 72 / 100, spx(280.0f, 220), spx(540.0f, 400));
        const int bx = (hud_w - bw) / 2;
        const std::uint32_t acc = ui_draw::rgba(
            static_cast<std::uint8_t>((banner_accent >> 24) & 0xFFu),
            static_cast<std::uint8_t>((banner_accent >> 16) & 0xFFu),
            static_cast<std::uint8_t>((banner_accent >> 8) & 0xFFu),
            245);
        surf.fill_rounded_rect(bx, by, bw, banner_h, spx(10.0f, 6), ui_draw::rgba(14, 12, 24, 238));
        surf.stroke_rounded_rect(bx, by, bw, banner_h, spx(10.0f, 6), 1.5f, acc);
        surf.fill_rect(bx + spx(6.0f, 4), by + spx(6.0f, 4), spx(4.0f, 3), banner_h - spx(12.0f, 8), acc);

        const int tpx = spx(14.0f, 11);
        const int spx_sub = spx(11.0f, 9);
        const std::string tfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, tpx, true, banner_title, bw - spx(32.0f, 20));
        const std::string sfit = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, spx_sub, false, banner_sub, bw - spx(32.0f, 20));
        ui_draw::draw_text(surf, bx + spx(18.0f, 12), by + spx(8.0f, 5), ui_draw::FontStyle::Sans, tpx, true, tfit, acc, true);
        if (!sfit.empty()) {
            ui_draw::draw_text(surf, bx + spx(18.0f, 12), by + spx(30.0f, 22), ui_draw::FontStyle::Sans, spx_sub, false, sfit, pal::kTextSoft, false);
        }
    }
}

}  // namespace quickmenu
