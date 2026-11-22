#include "psx/gpu.hpp"
#include "psx/mdec.hpp"
#include "psx/spu.hpp"
#include "psx/state.hpp"
#include "psx/system.hpp"

#include "keymap.hpp"
#include "screenshot.hpp"
#include "stretch.hpp"
#include "ui_draw.hpp"

#ifdef PSX_HAS_GENERATED
#include "psx_units.hpp"
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

double steady_seconds() {
    using Clock = std::chrono::steady_clock;
    static const auto t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

std::string read_text_file(const fs::path &p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

void write_text_file(const fs::path &p, const std::string &text) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (ofs.is_open()) ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
}

#ifdef _WIN32
bool has_std_handle(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

void connect_stream(std::FILE *stream, DWORD which, bool console) {
    const char *target = console ? "CONOUT$" : "NUL";
    if (!std::freopen(target, "wb", stream)) std::freopen("NUL", "wb", stream);
    const wchar_t *wide_target = console ? L"CONOUT$" : L"NUL";
    HANDLE h = CreateFileW(wide_target, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) SetStdHandle(which, h);
}

void connect_output() {
    const bool need_out = !has_std_handle(STD_OUTPUT_HANDLE);
    const bool need_err = !has_std_handle(STD_ERROR_HANDLE);
    if (need_out || need_err) {
        const bool console = AttachConsole(ATTACH_PARENT_PROCESS) || GetLastError() == ERROR_ACCESS_DENIED;
        if (need_out) connect_stream(stdout, STD_OUTPUT_HANDLE, console);
        if (need_err) connect_stream(stderr, STD_ERROR_HANDLE, console);
    }
    for (std::FILE *stream : {stdout, stderr}) {
        const int fd = _fileno(stream);
        if (fd >= 0) _setmode(fd, _O_BINARY);
    }
}

std::string narrow_utf8(const wchar_t *w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

fs::path locate_game_dir(const char *argv0) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t mod_buf[MAX_PATH * 4]{};
    const DWORD mod_n = GetModuleFileNameW(nullptr, mod_buf, static_cast<DWORD>(std::size(mod_buf)));
    if (mod_n > 0 && mod_n < std::size(mod_buf)) {
        const fs::path exe_dir = fs::path(mod_buf).parent_path();
        if (fs::exists(exe_dir / "disc_files" / "disc_manifest.tsv", ec)) return exe_dir;
        if (fs::exists(exe_dir / "games" / "koudelka" / "disc_files" / "disc_manifest.tsv", ec)) {
            return exe_dir / "games" / "koudelka";
        }
    }
#endif
    if (argv0 && *argv0) {
        const fs::path exe_dir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
        if (fs::exists(exe_dir / "disc_files" / "disc_manifest.tsv", ec)) return exe_dir;
        if (fs::exists(exe_dir / "games" / "koudelka" / "disc_files" / "disc_manifest.tsv", ec)) {
            return exe_dir / "games" / "koudelka";
        }
    }
    const fs::path cwd = fs::current_path(ec);
    if (fs::exists(cwd / "disc_files" / "disc_manifest.tsv", ec)) return cwd;
    if (fs::exists(cwd / "games" / "koudelka" / "disc_files" / "disc_manifest.tsv", ec)) {
        return cwd / "games" / "koudelka";
    }
    return cwd;
}

void apply_post_fx(std::vector<std::uint32_t> &rgba, int w, int h, const keymap::Config &cfg) {
    const bool need_color = (cfg.brightness != 0 || cfg.contrast != 100);
    const bool need_scan = (cfg.scanlines > 0);
    if (!need_color && !need_scan) return;

    const int br_offset = (cfg.brightness * 255) / 100;
    const int contrast_mul = (cfg.contrast * 256) / 100;
    const int scan_Keep = 256 - std::clamp(cfg.scanlines * 2, 0, 220);

    for (int y = 0; y < h; ++y) {
        const bool is_scanline = need_scan && ((y & 1) != 0);
        std::uint32_t *row = rgba.data() + y * w;
        for (int x = 0; x < w; ++x) {
            std::uint32_t px = row[x];
            int r = px & 0xFF;
            int g = (px >> 8) & 0xFF;
            int b = (px >> 16) & 0xFF;
            if (need_color) {
                r = (((r - 128) * contrast_mul) >> 8) + 128 + br_offset;
                g = (((g - 128) * contrast_mul) >> 8) + 128 + br_offset;
                b = (((b - 128) * contrast_mul) >> 8) + 128 + br_offset;
                r = std::clamp(r, 0, 255);
                g = std::clamp(g, 0, 255);
                b = std::clamp(b, 0, 255);
            }
            if (is_scanline) {
                r = (r * scan_Keep) >> 8;
                g = (g * scan_Keep) >> 8;
                b = (b * scan_Keep) >> 8;
            }
            row[x] = static_cast<std::uint32_t>(r) |
                     (static_cast<std::uint32_t>(g) << 8) |
                     (static_cast<std::uint32_t>(b) << 16) |
                     0xFF000000u;
        }
    }
}

struct UiRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const noexcept {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

constexpr int kPsxTabCount = 4;

int psx_tab_row_count(int tab) noexcept {
    switch (tab) {
    case 0: return 8;  // Video
    case 1: return 5;  // Audio
    case 2: return 5;  // Save States
    case 3: return 5;  // Disc & System
    default: return 8;
    }
}

bool psx_row_is_stepper(int tab, int row) noexcept {
    if (tab == 0) return true;  // All 8 Video rows support < / >
    if (tab == 1) return true;  // All 5 Audio rows support < / >
    if (tab == 2) return (row == 0);  // Active Save Slot
    if (tab == 3) return (row <= 2);  // Active Disc, Turbo toggle, Turbo speed
    return false;
}

struct PsxMenuLayout {
    UiRect modal;
    UiRect close_btn;
    UiRect tabs[kPsxTabCount];
    UiRect rows[8];
    UiRect left_btn[8];
    UiRect right_btn[8];
    int row_count = 0;
};

PsxMenuLayout compute_psx_menu_layout(int sw, int sh, int tab) {
    PsxMenuLayout L{};
    L.row_count = psx_tab_row_count(tab);
    const int mw = std::clamp(sw - 48, 300, 720);
    const int mh = std::clamp(sh - 40, 260, 540);
    const int mx = (sw - mw) / 2;
    const int my = (sh - mh) / 2;
    L.modal = {mx, my, mw, mh};
    L.close_btn = {mx + mw - 102, my + 14, 84, 30};

    const int tab_y = my + 64;
    const int tab_h = 34;
    const int tab_gap = 8;
    const int tab_total_w = mw - 36;
    const int tab_w = (tab_total_w - tab_gap * (kPsxTabCount - 1)) / kPsxTabCount;
    for (int t = 0; t < kPsxTabCount; ++t) {
        L.tabs[t] = {mx + 18 + t * (tab_w + tab_gap), tab_y, tab_w, tab_h};
    }

    const int row_y0 = tab_y + tab_h + 14;
    const int avail_h = std::max(120, mh - (row_y0 - my) - 44);
    const int row_h = std::clamp(avail_h / std::max(1, L.row_count), 32, 46);
    const int btn_w = 34;
    const int val_span = std::min(250, mw / 2 - 20);
    for (int i = 0; i < L.row_count; ++i) {
        const int ry = row_y0 + i * row_h;
        L.rows[i] = {mx + 18, ry, mw - 36, row_h - 5};
        if (psx_row_is_stepper(tab, i)) {
            const int rx_end = mx + mw - 24;
            L.right_btn[i] = {rx_end - btn_w, ry + 3, btn_w, row_h - 11};
            L.left_btn[i] = {rx_end - val_span, ry + 3, btn_w, row_h - 11};
        }
    }
    return L;
}

void draw_quick_menu_overlay(ui_draw::Surface &surf, int tab, int cursor, int hover_tab, int hover_row,
                             bool hover_close, const keymap::Config &cfg, const psx::System &sys,
                             const psx::Gpu &gpu, bool turbo_toggle, bool slot_exists,
                             const std::string &toast_msg) {
    using namespace ui_draw;
    const int sw = surf.width;
    const int sh = surf.height;

    // Dark translucent Gothic backdrop
    surf.fill_rect(0, 0, sw, sh, rgba(8, 6, 12, 195));

    const PsxMenuLayout L = compute_psx_menu_layout(sw, sh, tab);
    const int mx = L.modal.x, my = L.modal.y, mw = L.modal.w, mh = L.modal.h;

    surf.fill_gradient_rounded_rect(mx, my, mw, mh, 12.0f, rgba(24, 17, 28, 248), rgba(14, 10, 18, 248));
    surf.stroke_rounded_rect(mx, my, mw, mh, 12.0f, 1.5f, rgba(196, 154, 88, 230));

    // Gothic Header
    draw_text(surf, mx + 22, my + 12, FontStyle::Serif, 20, true,
              "KOUDELKA (1898 PREQUEL) — QUICK MENU", rgba(240, 210, 145));
    char sub[160];
    std::snprintf(sub, sizeof(sub), "Disc %d/4  •  Scale %dx (%dx%d)  •  Slot %d  •  Mouse / Touch / Pad",
                  sys.cdrom.active_disc(), gpu.scale, 320 * gpu.scale, 240 * gpu.scale, cfg.save_slot);
    draw_text(surf, mx + 22, my + 38, FontStyle::Sans, 12, false, sub, rgba(178, 168, 188));

    // Close / Resume clickable pill button
    surf.fill_rounded_rect(L.close_btn.x, L.close_btn.y, L.close_btn.w, L.close_btn.h, 6.0f,
                           hover_close ? rgba(128, 42, 56, 235) : rgba(48, 30, 42, 225));
    surf.stroke_rounded_rect(L.close_btn.x, L.close_btn.y, L.close_btn.w, L.close_btn.h, 6.0f, 1.0f,
                             hover_close ? rgba(240, 198, 118, 240) : rgba(175, 138, 82, 180));
    const auto cm = measure_text(FontStyle::Sans, 12, true, "Resume");
    draw_text(surf, L.close_btn.x + (L.close_btn.w - cm.width) / 2, L.close_btn.y + 7,
              FontStyle::Sans, 12, true, "Resume", rgba(245, 226, 185));

    // Tab bar
    static constexpr const char *kTabNames[kPsxTabCount] = {
        "VIDEO", "AUDIO", "SAVE STATES", "DISC & SYSTEM"
    };
    for (int t = 0; t < kPsxTabCount; ++t) {
        const UiRect &tr = L.tabs[t];
        const bool active = (t == tab);
        const bool hov = (t == hover_tab);
        surf.fill_rounded_rect(tr.x, tr.y, tr.w, tr.h, 7.0f,
                               active ? rgba(102, 36, 52, 240)
                                      : (hov ? rgba(54, 34, 48, 225) : rgba(28, 20, 34, 215)));
        surf.stroke_rounded_rect(tr.x, tr.y, tr.w, tr.h, 7.0f, active ? 1.5f : 1.0f,
                                 active ? rgba(232, 188, 110, 245) : rgba(140, 112, 74, 150));
        const auto tm = measure_text(FontStyle::Sans, 12, true, kTabNames[t]);
        draw_text(surf, tr.x + (tr.w - tm.width) / 2, tr.y + (tr.h - 16) / 2,
                  FontStyle::Sans, 12, true, kTabNames[t],
                  active ? rgba(255, 238, 195) : rgba(190, 182, 200));
    }

    const char *aspect_names[3] = {"4:3 Original", "16:9 Widescreen", "Stretch"};
    const char *labels[8] = {};
    char vals[8][64] = {};

    if (tab == 0) {
        labels[0] = "Internal Resolution Scale";
        labels[1] = "Aspect Ratio";
        labels[2] = "Display Mode";
        labels[3] = "Upscale Filter (2D / Output)";
        labels[4] = "Brightness";
        labels[5] = "Contrast";
        labels[6] = "CRT Scanlines";
        labels[7] = "Performance HUD (F10)";
        std::snprintf(vals[0], 64, "%dx (%dx%d)", gpu.scale, 320 * gpu.scale, 240 * gpu.scale);
        std::snprintf(vals[1], 64, "%s", aspect_names[std::clamp(cfg.aspect, 0, 2)]);
        std::snprintf(vals[2], 64, "%s", cfg.fullscreen ? "Fullscreen" : "Windowed");
        std::snprintf(vals[3], 64, "%s", cfg.sharp_2d <= 0 ? "Smooth (Linear)" : "Crisp (Nearest)");
        std::snprintf(vals[4], 64, "%+d%%", cfg.brightness);
        std::snprintf(vals[5], 64, "%d%%", cfg.contrast);
        if (cfg.scanlines == 0) std::snprintf(vals[6], 64, "Off");
        else std::snprintf(vals[6], 64, "%d%%", cfg.scanlines);
        std::snprintf(vals[7], 64, "%s", cfg.perf_hud ? "On" : "Off");
    } else if (tab == 1) {
        labels[0] = "Master Volume";
        labels[1] = "Mute Audio";
        labels[2] = "BGM / FMV (XA) Volume";
        labels[3] = "SFX / SPU Voice Volume";
        labels[4] = "Audio Time-Stretch";
        std::snprintf(vals[0], 64, "%d%%", cfg.master_volume);
        std::snprintf(vals[1], 64, "%s", cfg.mute ? "Muted" : "Off");
        std::snprintf(vals[2], 64, "%d%%", cfg.bgm_volume);
        std::snprintf(vals[3], 64, "%d%%", cfg.sfx_volume);
        std::snprintf(vals[4], 64, "%s", cfg.audio_stretch != 0 ? "On" : "Off");
    } else if (tab == 2) {
        labels[0] = "Active Save Slot";
        labels[1] = "Save State to Active Slot (F5)";
        labels[2] = "Load State from Active Slot (F8)";
        labels[3] = "Take Clean Screenshot (F12)";
        labels[4] = "Flush PS1 Memory Card Now";
        std::snprintf(vals[0], 64, "Slot %d (%s)", cfg.save_slot, slot_exists ? "Saved" : "Empty");
        std::snprintf(vals[1], 64, "slot_%d.state", cfg.save_slot);
        std::snprintf(vals[2], 64, "%s", slot_exists ? "Load & Resume" : "Empty");
        std::snprintf(vals[3], 64, "PNG Capture");
        std::snprintf(vals[4], 64, "memcard.mcr");
    } else {
        const int ad = sys.cdrom.active_disc();
        const int tm = std::clamp(cfg.turbo_speed > 0 ? cfg.turbo_speed : 3, 2, 4);
        labels[0] = "Active Disc (1-4)";
        labels[1] = "Turbo / Fast-Forward (F3)";
        labels[2] = "Turbo Speed Multiplier";
        labels[3] = "Resume Game";
        labels[4] = "Save Memcard & Quit";
        std::snprintf(vals[0], 64, "Disc %d (SLUS-01%s)", ad,
                      ad == 1 ? "051" : (ad == 2 ? "100" : (ad == 3 ? "101" : "102")));
        std::snprintf(vals[1], 64, "%s", turbo_toggle ? "On" : "Off");
        std::snprintf(vals[2], 64, "%dx", tm);
        std::snprintf(vals[3], 64, "F1 / Esc");
        std::snprintf(vals[4], 64, "Exit");
    }

    for (int i = 0; i < L.row_count; ++i) {
        const UiRect &rr = L.rows[i];
        const bool sel = (i == cursor);
        const bool hov = (i == hover_row);
        if (sel) {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(96, 34, 50, 220));
            surf.stroke_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, 1.25f, rgba(226, 182, 108, 235));
        } else if (hov) {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(52, 34, 46, 180));
            surf.stroke_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, 1.0f, rgba(165, 130, 82, 150));
        } else {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(24, 18, 30, 140));
        }

        const int ty = rr.y + (rr.h - 17) / 2;
        draw_text(surf, rr.x + 14, ty, FontStyle::Sans, 14, sel,
                  labels[i], sel ? rgba(255, 240, 205) : rgba(218, 214, 224));

        if (psx_row_is_stepper(tab, i)) {
            const UiRect &lb = L.left_btn[i];
            const UiRect &rb = L.right_btn[i];
            const auto btn_bg = sel ? rgba(62, 24, 36, 230) : rgba(36, 26, 44, 210);
            const auto btn_bd = sel ? rgba(226, 182, 108, 220) : rgba(130, 108, 76, 150);
            surf.fill_rounded_rect(lb.x, lb.y, lb.w, lb.h, 5.0f, btn_bg);
            surf.stroke_rounded_rect(lb.x, lb.y, lb.w, lb.h, 5.0f, 1.0f, btn_bd);
            surf.fill_rounded_rect(rb.x, rb.y, rb.w, rb.h, 5.0f, btn_bg);
            surf.stroke_rounded_rect(rb.x, rb.y, rb.w, rb.h, 5.0f, 1.0f, btn_bd);
            draw_text(surf, lb.x + 12, lb.y + (lb.h - 16) / 2, FontStyle::Sans, 14, true, "<", rgba(240, 205, 130));
            draw_text(surf, rb.x + 12, rb.y + (rb.h - 16) / 2, FontStyle::Sans, 14, true, ">", rgba(240, 205, 130));

            const int mid_x = lb.x + lb.w;
            const int mid_w = rb.x - mid_x;
            const auto vm = measure_text(FontStyle::Sans, 13, true, vals[i]);
            draw_text(surf, mid_x + (mid_w - vm.width) / 2, ty, FontStyle::Sans, 13, true,
                      vals[i], sel ? rgba(248, 212, 132) : rgba(190, 182, 200));
        } else {
            const auto vm = measure_text(FontStyle::Sans, 13, true, vals[i]);
            draw_text(surf, rr.x + rr.w - 16 - vm.width, ty, FontStyle::Sans, 13, true,
                      vals[i], sel ? rgba(248, 212, 132) : rgba(175, 165, 186));
        }
    }

    const std::string footer = toast_msg.empty()
        ? "Click / Tap  •  Q/E or L1/R1: Tabs  •  Arrows/D-Pad: Adjust  •  Enter/Cross: Confirm  •  F1/Esc: Resume"
        : toast_msg;
    draw_text(surf, mx + 22, my + mh - 28, FontStyle::Sans, 11, !toast_msg.empty(),
              footer, toast_msg.empty() ? rgba(158, 150, 170) : rgba(248, 214, 128));
}

void draw_hud_and_toast(ui_draw::Surface &surf, bool show_hud, double fps, bool turbo, int turbo_mul,
                        int active_disc, int scale, const std::string &toast_msg, double toast_until, double now) {
    using namespace ui_draw;
    if (show_hud || turbo) {
        char hud[128];
        if (turbo) {
            std::snprintf(hud, sizeof(hud), ">> TURBO %dx  •  %.1f FPS  •  DISC %d  •  %dx",
                          turbo_mul, fps, active_disc, scale);
        } else {
            std::snprintf(hud, sizeof(hud), "%.1f FPS (%.2f ms)  •  DISC %d/4  •  %dx",
                          fps, fps > 0.1 ? (1000.0 / fps) : 0.0, active_disc, scale);
        }
        draw_pill(surf, 12, 10, 24, FontStyle::Sans, 12, true, hud,
                  rgba(16, 12, 20, 215),
                  turbo ? rgba(235, 185, 85, 235) : rgba(165, 130, 80, 190),
                  turbo ? rgba(255, 225, 140) : rgba(235, 225, 210));
    }

    if (!toast_msg.empty() && now < toast_until) {
        const auto tm = measure_text(FontStyle::Sans, 13, true, toast_msg);
        const int pw = tm.width + 28;
        const int px = std::max(12, surf.width - pw - 14);
        draw_pill(surf, px, 10, 26, FontStyle::Sans, 13, true, toast_msg,
                  rgba(26, 18, 28, 235), rgba(215, 175, 95, 235), rgba(250, 230, 180));
    }
}

void update_pad_from_inputs(psx::PadState &pad, const keymap::Config &cfg,
                            const std::uint8_t *kb, SDL_GameController *ctrl, bool &f1_combo_latch) {
    std::uint16_t pressed = 0;

    auto action_held = [&](keymap::Action act) -> bool {
        for (int code : cfg.inputs[act]) {
            if (code >= 0 && code < SDL_NUM_SCANCODES && kb[code]) return true;
        }
        return false;
    };

    if (action_held(keymap::DpadUp) || action_held(keymap::LeftUp)) pressed |= psx::kPadUp;
    if (action_held(keymap::DpadDown) || action_held(keymap::LeftDown)) pressed |= psx::kPadDown;
    if (action_held(keymap::DpadLeft) || action_held(keymap::LeftLeft)) pressed |= psx::kPadLeft;
    if (action_held(keymap::DpadRight) || action_held(keymap::LeftRight)) pressed |= psx::kPadRight;
    if (action_held(keymap::Cross)) pressed |= psx::kPadCross;
    if (action_held(keymap::Circle)) pressed |= psx::kPadCircle;
    if (action_held(keymap::Square)) pressed |= psx::kPadSquare;
    if (action_held(keymap::Triangle)) pressed |= psx::kPadTriangle;
    if (action_held(keymap::L1)) pressed |= psx::kPadL1;
    if (action_held(keymap::R1)) pressed |= psx::kPadR1;
    if (action_held(keymap::L2)) pressed |= psx::kPadL2;
    if (action_held(keymap::R2)) pressed |= psx::kPadR2;
    if (action_held(keymap::L3)) pressed |= psx::kPadL3;
    if (action_held(keymap::R3)) pressed |= psx::kPadR3;
    if (action_held(keymap::Start)) pressed |= psx::kPadStart;
    if (action_held(keymap::Select)) pressed |= psx::kPadSelect;

    if (ctrl) {
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP)) pressed |= psx::kPadUp;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pressed |= psx::kPadDown;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) pressed |= psx::kPadLeft;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pressed |= psx::kPadRight;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A)) pressed |= psx::kPadCross;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_B)) pressed |= psx::kPadCircle;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_X)) pressed |= psx::kPadSquare;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_Y)) pressed |= psx::kPadTriangle;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pressed |= psx::kPadL1;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pressed |= psx::kPadR1;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSTICK)) pressed |= psx::kPadL3;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) pressed |= psx::kPadR3;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_START)) pressed |= psx::kPadStart;
        if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_BACK)) pressed |= psx::kPadSelect;
        if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) pressed |= psx::kPadL2;
        if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) pressed |= psx::kPadR2;

        const Sint16 lx = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ly = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        if (ly < -12000) pressed |= psx::kPadUp;
        if (ly >  12000) pressed |= psx::kPadDown;
        if (lx < -12000) pressed |= psx::kPadLeft;
        if (lx >  12000) pressed |= psx::kPadRight;
    }

    const bool combo = (pressed & (psx::kPadStart | psx::kPadSelect)) == (psx::kPadStart | psx::kPadSelect);
    f1_combo_latch = combo;

    // PS1 controller buttons are active-low (0 = pressed, 1 = released)
    pad.buttons = static_cast<std::uint16_t>(~pressed);
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    connect_output();
    std::vector<std::string> utf8_args;
    std::vector<char *> utf8_argv;
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) utf8_args.push_back(narrow_utf8(wide[i]));
        LocalFree(wide);
        for (std::string &a : utf8_args) utf8_argv.push_back(a.data());
        utf8_argv.push_back(nullptr);
        argc = wide_argc;
        argv = utf8_argv.data();
    }
#endif
    fs::path game_dir = locate_game_dir(argc > 0 ? argv[0] : nullptr);
    int start_disc = 1;
    int cli_scale = 0;
    int max_frames = 0;
    int telemetry_interval = 0;
    bool headless = false;
    bool no_aot = false;
    std::string dump_frame_path;
    std::string load_state_arg;
    std::string save_state_arg;
    std::string input_script_arg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            start_disc = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            cli_scale = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--telemetry") == 0 && i + 1 < argc) {
            telemetry_interval = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_script_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--no-aot") == 0) {
            no_aot = true;
        } else if (std::strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc) {
            dump_frame_path = argv[++i];
        } else if (std::strcmp(argv[i], "--load-state") == 0 && i + 1 < argc) {
            load_state_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--save-state") == 0 && i + 1 < argc) {
            save_state_arg = argv[++i];
        }
    }

    // 1. Load keys.ini configuration
    const fs::path keys_path = game_dir / "keys.ini";
    keymap::Config cfg = keymap::defaults();
    std::string keys_text = read_text_file(keys_path);
    if (keys_text.empty()) {
        keys_text = keymap::default_file_text();
        write_text_file(keys_path, keys_text);
    }
    std::vector<std::string> problems;
    keymap::parse(keys_text, cfg, problems);
    const int init_scale = (cli_scale > 0) ? cli_scale : (cfg.scale > 0 ? std::clamp(cfg.scale, 1, 4) : 2);

    // 2. Initialize PS1 System + GPU + MDEC + SPU1 + 4-Disc Virtual CD-ROM
    psx::System sys;
    psx::Gpu gpu;
    psx::Mdec mdec;
    psx::Spu spu;
    gpu.set_scale(init_scale);
    gpu.sharp_3d = (cfg.sharp_3d != 0);
    spu.bgm_gain = std::clamp(cfg.bgm_volume, 0, 100) / 100.0f;
    spu.sfx_gain = std::clamp(cfg.sfx_volume, 0, 100) / 100.0f;

    sys.gpu = &gpu;
    sys.mdec = &mdec;
    sys.spu = &spu;

    // Hook XA-ADPCM sectors from CD-ROM directly into SPU1 XA decoder
    std::uint64_t xa_sectors_total = 0;
    sys.cdrom.set_xa_callback([&](const std::uint8_t *raw2352, std::uint8_t, std::uint8_t) {
        ++xa_sectors_total;
        spu.decode_xa_sector(raw2352);
    });

    // Load 128 KB PS1 memory card (memcard.mcr)
    sys.hw.memcards[0].load((game_dir / "memcard.mcr").string());

    // Load 4-disc manifest
    std::string err;
    const fs::path disc_files_dir = game_dir / "disc_files";
    if (!sys.cdrom.load_manifest(disc_files_dir.string(), &err)) {
        std::fprintf(stderr, "[koudelka] error loading 4-disc manifest: %s\n", err.c_str());
        return 1;
    }
    sys.cdrom.set_active_disc_immediate(start_disc);

    // Load PS-X EXE (SLUS_010.51)
    const fs::path exe_path = game_dir / "exe" / "SLUS_010.51";
    if (!sys.load_psx_exe(exe_path.string(), &err)) {
        std::fprintf(stderr, "[koudelka] error loading PS-X EXE: %s\n", err.c_str());
        return 1;
    }

#ifdef PSX_HAS_GENERATED
    if (!no_aot) {
        psx_gen::register_all(sys);
    }
#endif
    sys.aot_enabled = !no_aot;

    if (!load_state_arg.empty()) {
        psx::load_state_file(load_state_arg, sys, gpu, mdec, spu);
    }

    // 3. Headless execution path (for automated boot/cutscene/save-state verification)
    if (headless) {
        struct InputEvent { int start_frame = 0; int end_frame = 0; std::uint16_t mask = 0; };
        std::vector<InputEvent> script_events;
        if (!input_script_arg.empty()) {
            std::size_t p = 0;
            while (p < input_script_arg.size()) {
                std::size_t c = input_script_arg.find(',', p);
                std::string tok = (c == std::string::npos) ? input_script_arg.substr(p) : input_script_arg.substr(p, c - p);
                const std::size_t c1 = tok.find(':');
                const std::size_t c2 = (c1 != std::string::npos) ? tok.find(':', c1 + 1) : std::string::npos;
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    const int sf = std::atoi(tok.substr(0, c1).c_str());
                    const std::string btn = tok.substr(c1 + 1, c2 - c1 - 1);
                    const int dur = std::max(1, std::atoi(tok.substr(c2 + 1).c_str()));
                    std::uint16_t m = 0;
                    if (btn == "start") m = psx::kPadStart;
                    else if (btn == "select") m = psx::kPadSelect;
                    else if (btn == "cross" || btn == "x") m = psx::kPadCross;
                    else if (btn == "circle" || btn == "o") m = psx::kPadCircle;
                    else if (btn == "square") m = psx::kPadSquare;
                    else if (btn == "triangle") m = psx::kPadTriangle;
                    else if (btn == "up") m = psx::kPadUp;
                    else if (btn == "down") m = psx::kPadDown;
                    else if (btn == "left") m = psx::kPadLeft;
                    else if (btn == "right") m = psx::kPadRight;
                    else if (btn == "l1") m = psx::kPadL1;
                    else if (btn == "r1") m = psx::kPadR1;
                    if (m != 0) script_events.push_back({sf, sf + dur, m});
                }
                if (c == std::string::npos) break;
                p = c + 1;
            }
        }

        double audio_sum_sq = 0.0;
        std::uint64_t audio_samples = 0;
        spu.set_audio_callback([&](const std::int16_t *lr, std::size_t frames) {
            for (std::size_t i = 0; i < frames * 2u; ++i) {
                const double v = static_cast<double>(lr[i]) / 32768.0;
                audio_sum_sq += v * v;
            }
            audio_samples += frames * 2u;
        });

        auto compute_frame_metrics = [&](int &fw, int &fh, double &non_black_pct, double &mean_luma,
                                         double &mean_r, double &mean_g, double &mean_b, std::uint64_t &hash) {
            std::vector<std::uint32_t> rgba;
            gpu.present_rgba8888(rgba, fw, fh);
            std::uint64_t nb = 0;
            double luma_sum = 0.0, r_sum = 0.0, g_sum = 0.0, b_sum = 0.0;
            std::uint64_t h = 14695981039346656037ull;
            const std::size_t n = rgba.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t px = rgba[i];
                const int r = px & 0xFF;
                const int g = (px >> 8) & 0xFF;
                const int b = (px >> 16) & 0xFF;
                if ((r | g | b) > 4) ++nb;
                r_sum += r; g_sum += g; b_sum += b;
                luma_sum += (0.299 * r + 0.587 * g + 0.114 * b);
                h = (h ^ px) * 1099511628211ull;
            }
            const double inv = (n > 0) ? (1.0 / static_cast<double>(n)) : 0.0;
            non_black_pct = 100.0 * static_cast<double>(nb) * inv;
            mean_luma = luma_sum * inv;
            mean_r = r_sum * inv;
            mean_g = g_sum * inv;
            mean_b = b_sum * inv;
            hash = h;
        };

        const double t0 = steady_seconds();
        const int target_frames = (max_frames > 0) ? max_frames : 60;
        for (int f = 0; f < target_frames && !sys.cpu.halted; ++f) {
            std::uint16_t pressed = 0;
            for (const auto &ev : script_events) {
                if (f >= ev.start_frame && f < ev.end_frame) pressed |= ev.mask;
            }
            sys.hw.pads[0].buttons = static_cast<std::uint16_t>(~pressed);
            sys.run_frame();
            if (telemetry_interval > 0 && ((f + 1) % telemetry_interval == 0 || f == 0)) {
                int fw = 0, fh = 0;
                double nb = 0.0, luma = 0.0, mr = 0.0, mg = 0.0, mb = 0.0;
                std::uint64_t fhash = 0;
                compute_frame_metrics(fw, fh, nb, luma, mr, mg, mb, fhash);
                const double rms = (audio_samples > 0) ? std::sqrt(audio_sum_sq / static_cast<double>(audio_samples)) : 0.0;
                const double ms = (steady_seconds() - t0) * 1000.0;
                int spu_active = 0;
                for (int vi = 0; vi < psx::Spu::kNumVoices; ++vi) {
                    if (spu.voices[vi].phase != psx::Spu::EnvPhase::Off) ++spu_active;
                }
                std::printf("[telem f=%4d t=%6.0fms] pc=%08X prims=%llu mdec=%llu xa=%llu rms=%.4f spu=%d nb=%5.1f%% luma=%5.1f rgb=(%5.1f,%5.1f,%5.1f) hash=%016llX %dx%d\n",
                            f + 1, ms, sys.cpu.pc,
                            static_cast<unsigned long long>(gpu.primitives_drawn),
                            static_cast<unsigned long long>(mdec.blocks_decoded),
                            static_cast<unsigned long long>(xa_sectors_total),
                            rms, spu_active, nb, luma, mr, mg, mb, static_cast<unsigned long long>(fhash), fw, fh);
                audio_sum_sq = 0.0;
                audio_samples = 0;
                std::fflush(stdout);
            }
        }
        std::vector<std::uint32_t> frame_rgba;
        int fw = 0, fh = 0;
        gpu.present_rgba8888(frame_rgba, fw, fh);

        if (!dump_frame_path.empty() && fw > 0 && fh > 0) {
            screenshot::write_png(dump_frame_path, frame_rgba.data(),
                                  static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh));
        }
        if (!save_state_arg.empty()) {
            psx::save_state_file(save_state_arg, sys, gpu, mdec, spu);
        }
        const double total_ms = (steady_seconds() - t0) * 1000.0;
        std::printf("[koudelka-headless] ran %llu frames in %.1f ms (%.1f fps) (pc=0x%08X ra=0x%08X sp=0x%08X aot=%llu interp=%llu gpu_prims=%llu vram_uploads=%llu mdec_blks=%llu xa=%llu disc=%d frame=%dx%d)\n",
                    static_cast<unsigned long long>(sys.frame_count), total_ms,
                    total_ms > 0.1 ? (1000.0 * sys.frame_count / total_ms) : 0.0,
                    sys.cpu.pc, sys.cpu.r[31], sys.cpu.r[29],
                    static_cast<unsigned long long>(sys.aot_hits),
                    static_cast<unsigned long long>(sys.interp_fallbacks),
                    static_cast<unsigned long long>(gpu.primitives_drawn),
                    static_cast<unsigned long long>(gpu.vram_uploads),
                    static_cast<unsigned long long>(mdec.blocks_decoded),
                    static_cast<unsigned long long>(xa_sectors_total),
                    sys.cdrom.active_disc(), fw, fh);
        std::fflush(stdout);
        return 0;
    }

    // 4. Interactive Windowed / Fullscreen SDL2 + SoundPipe initialization
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[koudelka] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.sharp_2d <= 0 ? "linear" : "nearest");

    std::uint32_t win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    SDL_Window *window = SDL_CreateWindow(
        "Koudelka (1898 Prequel) — Disc 1/4",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 720, win_flags);
    if (!window) {
        std::fprintf(stderr, "[koudelka] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }

    // Audio time-stretcher pipe (48,000 Hz stereo)
    ps2sound::PipeConfig pcfg;
    pcfg.stretch = (cfg.audio_stretch != 0);
    ps2sound::SoundPipe sound_pipe(pcfg);
    sound_pipe.set_volume(std::clamp(cfg.master_volume, 0, 100) / 100.0f);
    sound_pipe.set_muted(cfg.mute != 0);

    spu.set_audio_callback([&](const std::int16_t *lr, std::size_t frames) {
        sound_pipe.push(lr, frames, steady_seconds());
    });

    SDL_AudioSpec want = {}, have = {};
    want.freq = ps2sound::kRate;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    want.userdata = &sound_pipe;
    want.callback = [](void *ud, Uint8 *stream, int len) {
        auto *pipe = static_cast<ps2sound::SoundPipe *>(ud);
        pipe->render(reinterpret_cast<std::int16_t *>(stream),
                     static_cast<std::size_t>(len) / (2 * sizeof(std::int16_t)),
                     steady_seconds());
    };
    const SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(
        cfg.audio_device.empty() ? nullptr : cfg.audio_device.c_str(),
        0, &want, &have, 0);
    if (audio_dev != 0) {
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    SDL_GameController *controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) break;
        }
    }

    SDL_Texture *tex = nullptr;
    int tex_w = 0, tex_h = 0;
    bool tex_filter_dirty = false;
    std::vector<std::uint32_t> frame_rgba;

    SDL_Texture *ui_tex = nullptr;
    int ui_tex_w = 0, ui_tex_h = 0;
    std::vector<std::uint32_t> ui_rgba;

    bool running = true;
    bool menu_open = false;
    int menu_tab = 0;
    int menu_cursor = 0;
    int hover_tab = -1;
    int hover_row = -1;
    bool hover_close = false;
    bool turbo_toggle = false;
    bool prev_combo = false;
    std::string toast_msg;
    double toast_until = 0.0;

    auto show_toast = [&](const std::string &msg) {
        toast_msg = msg;
        toast_until = steady_seconds() + 2.8;
    };

    auto slot_path = [&](int slot) -> std::string {
        return (game_dir / ("slot_" + std::to_string(std::clamp(slot, 0, 9)) + ".state")).string();
    };

    auto persist_keys = [&]() {
        write_text_file(keys_path, keys_text);
    };

    auto get_ui_size = [&](int &uw, int &uh) {
        int rw = 960, rh = 720;
        SDL_GetRendererOutputSize(renderer, &rw, &rh);
        rw = std::max(rw, 640);
        rh = std::max(rh, 480);
        // Cap software UI canvas cleanly at 1600x1200 while preserving aspect ratio
        if (rw > 1600 || rh > 1200) {
            const double sc = std::min(1600.0 / rw, 1200.0 / rh);
            rw = std::max(640, static_cast<int>(rw * sc + 0.5));
            rh = std::max(480, static_cast<int>(rh * sc + 0.5));
        }
        uw = rw;
        uh = rh;
    };

    auto map_window_to_ui = [&](int wx, int wy, int &ux, int &uy) {
        int win_w = 1, win_h = 1;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int uw = 960, uh = 720;
        get_ui_size(uw, uh);
        ux = (win_w > 0) ? (wx * uw) / win_w : wx;
        uy = (win_h > 0) ? (wy * uh) / win_h : wy;
    };

    auto take_clean_screenshot = [&]() {
        int sw = 0, sh = 0;
        std::vector<std::uint32_t> clean;
        gpu.present_rgba8888(clean, sw, sh);
        if (sw > 0 && sh > 0) {
            screenshot::save_game_async(std::move(clean),
                                        static_cast<std::uint32_t>(sw),
                                        static_cast<std::uint32_t>(sh));
            show_toast("Screenshot Saved");
        }
    };

    auto step_menu_item = [&](int tab, int row, int dir) {
        if (tab == 0) {  // VIDEO
            if (row == 0) {
                const int ns = std::clamp(gpu.scale + dir, 1, 4);
                if (ns != gpu.scale) {
                    gpu.set_scale(ns);
                    cfg.scale = ns;
                    keys_text = keymap::with_scale(keys_text, ns);
                    persist_keys();
                    show_toast("Internal Scale: " + std::to_string(ns) + "x (" +
                               std::to_string(320 * ns) + "x" + std::to_string(240 * ns) +
                               ") — sharpens on Resume");
                }
            } else if (row == 1) {
                cfg.aspect = (cfg.aspect + 3 + dir) % 3;
                keys_text = keymap::with_setting(keys_text, "aspect",
                    cfg.aspect == 0 ? "4:3" : (cfg.aspect == 1 ? "16:9" : "stretch"));
                persist_keys();
            } else if (row == 2) {
                cfg.fullscreen = !cfg.fullscreen;
                SDL_SetWindowFullscreen(window, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                keys_text = keymap::with_setting(keys_text, "fullscreen", cfg.fullscreen ? "1" : "0");
                persist_keys();
            } else if (row == 3) {
                cfg.sharp_2d = (cfg.sharp_2d <= 0) ? 1 : 0;
                SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.sharp_2d <= 0 ? "linear" : "nearest");
                tex_filter_dirty = true;
                keys_text = keymap::with_sharp_2d(keys_text, cfg.sharp_2d);
                persist_keys();
                show_toast(cfg.sharp_2d <= 0 ? "Upscale Filter: Smooth (Linear)" : "Upscale Filter: Crisp (Nearest)");
            } else if (row == 4) {
                cfg.brightness = std::clamp(cfg.brightness + dir * 5, -50, 50);
                keys_text = keymap::with_setting(keys_text, "brightness", std::to_string(cfg.brightness));
                persist_keys();
            } else if (row == 5) {
                cfg.contrast = std::clamp(cfg.contrast + dir * 5, 50, 150);
                keys_text = keymap::with_setting(keys_text, "contrast", std::to_string(cfg.contrast));
                persist_keys();
            } else if (row == 6) {
                cfg.scanlines = std::clamp(cfg.scanlines + dir * 10, 0, 100);
                keys_text = keymap::with_setting(keys_text, "scanlines", std::to_string(cfg.scanlines));
                persist_keys();
            } else if (row == 7) {
                cfg.perf_hud = !cfg.perf_hud;
                keys_text = keymap::with_setting(keys_text, "perf_hud", cfg.perf_hud ? "1" : "0");
                persist_keys();
            }
        } else if (tab == 1) {  // AUDIO
            if (row == 0) {
                cfg.mute = 0;
                cfg.master_volume = std::clamp(cfg.master_volume + dir * 5, 0, 100);
                sound_pipe.set_muted(false);
                sound_pipe.set_volume(cfg.master_volume / 100.0f);
                keys_text = keymap::with_setting(keys_text, "master_volume", std::to_string(cfg.master_volume));
                persist_keys();
            } else if (row == 1) {
                cfg.mute = !cfg.mute;
                sound_pipe.set_muted(cfg.mute != 0);
                keys_text = keymap::with_setting(keys_text, "mute", cfg.mute ? "1" : "0");
                persist_keys();
            } else if (row == 2) {
                cfg.bgm_volume = std::clamp(cfg.bgm_volume + dir * 5, 0, 100);
                spu.bgm_gain = cfg.bgm_volume / 100.0f;
                keys_text = keymap::with_setting(keys_text, "bgm_volume", std::to_string(cfg.bgm_volume));
                persist_keys();
            } else if (row == 3) {
                cfg.sfx_volume = std::clamp(cfg.sfx_volume + dir * 5, 0, 100);
                spu.sfx_gain = cfg.sfx_volume / 100.0f;
                keys_text = keymap::with_setting(keys_text, "sfx_volume", std::to_string(cfg.sfx_volume));
                persist_keys();
            } else if (row == 4) {
                cfg.audio_stretch = (cfg.audio_stretch == 0) ? 1 : 0;
                sound_pipe.set_stretch_enabled(cfg.audio_stretch != 0);
                keys_text = keymap::with_setting(keys_text, "audio_stretch", cfg.audio_stretch ? "1" : "0");
                persist_keys();
            }
        } else if (tab == 2) {  // SAVE STATES
            if (row == 0) {
                cfg.save_slot = (cfg.save_slot + 10 + dir) % 10;
                keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                persist_keys();
            }
        } else if (tab == 3) {  // DISC & SYSTEM
            if (row == 0) {
                const int nd = ((sys.cdrom.active_disc() - 1 + 4 + dir) % 4) + 1;
                sys.cdrom.swap_disc(nd);
                show_toast("Swapping to Koudelka Disc " + std::to_string(nd) + "...");
            } else if (row == 1) {
                turbo_toggle = !turbo_toggle;
                show_toast(turbo_toggle ? "Turbo Mode: ON" : "Turbo Mode: OFF");
            } else if (row == 2) {
                const int cur = std::clamp(cfg.turbo_speed > 0 ? cfg.turbo_speed : 3, 2, 4);
                cfg.turbo_speed = std::clamp(cur + dir, 2, 4);
                keys_text = keymap::with_setting(keys_text, "turbo_speed", std::to_string(cfg.turbo_speed));
                persist_keys();
            }
        }
    };

    auto activate_menu_item = [&](int tab, int row) {
        if (tab == 0 || tab == 1) {
            step_menu_item(tab, row, 1);
            return;
        }
        if (tab == 2) {
            if (row == 0) {
                step_menu_item(tab, row, 1);
            } else if (row == 1) {
                if (psx::save_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                    show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                } else {
                    show_toast("Save State Failed");
                }
            } else if (row == 2) {
                if (psx::load_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                    sound_pipe.flush();
                    show_toast("Loaded State from Slot " + std::to_string(cfg.save_slot));
                    menu_open = false;
                    sound_pipe.set_paused(false);
                } else {
                    show_toast("Slot " + std::to_string(cfg.save_slot) + " is Empty");
                }
            } else if (row == 3) {
                take_clean_screenshot();
            } else if (row == 4) {
                if (sys.hw.memcards[0].save()) show_toast("Saved PS1 Memory Card (memcard.mcr)");
                else show_toast("Memory Card Save Failed");
            }
        } else if (tab == 3) {
            if (row <= 2) {
                step_menu_item(tab, row, 1);
            } else if (row == 3) {
                menu_open = false;
                sound_pipe.set_paused(false);
            } else if (row == 4) {
                sys.hw.memcards[0].save();
                running = false;
            }
        }
    };

    auto handle_menu_pointer_hover = [&](int ux, int uy) {
        int uw = 960, uh = 720;
        get_ui_size(uw, uh);
        const PsxMenuLayout L = compute_psx_menu_layout(uw, uh, menu_tab);
        hover_close = L.close_btn.contains(ux, uy);
        hover_tab = -1;
        for (int t = 0; t < kPsxTabCount; ++t) {
            if (L.tabs[t].contains(ux, uy)) { hover_tab = t; break; }
        }
        hover_row = -1;
        for (int i = 0; i < L.row_count; ++i) {
            if (L.rows[i].contains(ux, uy)) { hover_row = i; menu_cursor = i; break; }
        }
    };

    auto handle_menu_pointer_click = [&](int ux, int uy) {
        int uw = 960, uh = 720;
        get_ui_size(uw, uh);
        const PsxMenuLayout L = compute_psx_menu_layout(uw, uh, menu_tab);
        if (!L.modal.contains(ux, uy) || L.close_btn.contains(ux, uy)) {
            menu_open = false;
            sound_pipe.set_paused(false);
            return;
        }
        for (int t = 0; t < kPsxTabCount; ++t) {
            if (L.tabs[t].contains(ux, uy)) {
                menu_tab = t;
                menu_cursor = 0;
                return;
            }
        }
        for (int i = 0; i < L.row_count; ++i) {
            if (L.rows[i].contains(ux, uy)) {
                menu_cursor = i;
                if (psx_row_is_stepper(menu_tab, i)) {
                    if (L.left_btn[i].contains(ux, uy)) {
                        step_menu_item(menu_tab, i, -1);
                        return;
                    }
                    if (L.right_btn[i].contains(ux, uy)) {
                        step_menu_item(menu_tab, i, 1);
                        return;
                    }
                }
                activate_menu_item(menu_tab, i);
                return;
            }
        }
    };

    double fps_smooth = 60.0;
    double last_frame_t = steady_seconds();
    int last_disc_shown = -1;

    while (running && !sys.cpu.halted) {
        const double frame_start = steady_seconds();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                controller = SDL_GameControllerOpen(ev.cdevice.which);
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            } else if (ev.type == SDL_MOUSEMOTION && menu_open && ev.motion.which != SDL_TOUCH_MOUSEID) {
                int ux = 0, uy = 0;
                map_window_to_ui(ev.motion.x, ev.motion.y, ux, uy);
                handle_menu_pointer_hover(ux, uy);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN && menu_open &&
                       ev.button.button == SDL_BUTTON_LEFT && ev.button.which != SDL_TOUCH_MOUSEID) {
                int ux = 0, uy = 0;
                map_window_to_ui(ev.button.x, ev.button.y, ux, uy);
                handle_menu_pointer_click(ux, uy);
            } else if (ev.type == SDL_MOUSEWHEEL && menu_open) {
                const int rc = psx_tab_row_count(menu_tab);
                if (ev.wheel.y > 0) menu_cursor = (menu_cursor + rc - 1) % rc;
                else if (ev.wheel.y < 0) menu_cursor = (menu_cursor + 1) % rc;
            } else if (ev.type == SDL_FINGERDOWN && menu_open) {
                int uw = 960, uh = 720;
                get_ui_size(uw, uh);
                const int ux = static_cast<int>(ev.tfinger.x * uw);
                const int uy = static_cast<int>(ev.tfinger.y * uh);
                handle_menu_pointer_hover(ux, uy);
                handle_menu_pointer_click(ux, uy);
            } else if (ev.type == SDL_FINGERMOTION && menu_open) {
                int uw = 960, uh = 720;
                get_ui_size(uw, uh);
                handle_menu_pointer_hover(static_cast<int>(ev.tfinger.x * uw),
                                          static_cast<int>(ev.tfinger.y * uh));
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                const auto btn = ev.cbutton.button;
                if (btn == SDL_CONTROLLER_BUTTON_GUIDE) {
                    menu_open = !menu_open;
                    sound_pipe.set_paused(menu_open);
                } else if (menu_open) {
                    const int rc = psx_tab_row_count(menu_tab);
                    if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) menu_cursor = (menu_cursor + rc - 1) % rc;
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) menu_cursor = (menu_cursor + 1) % rc;
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) step_menu_item(menu_tab, menu_cursor, -1);
                    else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) step_menu_item(menu_tab, menu_cursor, 1);
                    else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                        menu_tab = (menu_tab + kPsxTabCount - 1) % kPsxTabCount;
                        menu_cursor = 0;
                    } else if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                        menu_tab = (menu_tab + 1) % kPsxTabCount;
                        menu_cursor = 0;
                    } else if (btn == SDL_CONTROLLER_BUTTON_A) {
                        activate_menu_item(menu_tab, menu_cursor);
                    } else if (btn == SDL_CONTROLLER_BUTTON_B) {
                        menu_open = false;
                        sound_pipe.set_paused(false);
                    }
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                const SDL_Keycode sym = ev.key.keysym.sym;
                const bool alt = (ev.key.keysym.mod & KMOD_ALT) != 0;

                if (sym == SDLK_F1 || (menu_open && sym == SDLK_ESCAPE)) {
                    menu_open = !menu_open;
                    sound_pipe.set_paused(menu_open);
                } else if (sym == SDLK_F11 || (alt && sym == SDLK_RETURN)) {
                    cfg.fullscreen = !cfg.fullscreen;
                    SDL_SetWindowFullscreen(window, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    keys_text = keymap::with_setting(keys_text, "fullscreen", cfg.fullscreen ? "1" : "0");
                    persist_keys();
                } else if (sym == SDLK_F10) {
                    cfg.perf_hud = !cfg.perf_hud;
                    keys_text = keymap::with_setting(keys_text, "perf_hud", cfg.perf_hud ? "1" : "0");
                    persist_keys();
                } else if (sym == SDLK_F3) {
                    turbo_toggle = !turbo_toggle;
                } else if (sym == SDLK_F2) {
                    cfg.scanlines = (cfg.scanlines == 0) ? 25 : 0;
                    keys_text = keymap::with_setting(keys_text, "scanlines", std::to_string(cfg.scanlines));
                    persist_keys();
                    show_toast(cfg.scanlines ? "CRT Scanlines: ON (25%)" : "CRT Scanlines: OFF");
                } else if (sym == SDLK_F5) {
                    if (psx::save_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                        show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                    } else {
                        show_toast("Save State Failed");
                    }
                } else if (sym == SDLK_F6) {
                    cfg.save_slot = (cfg.save_slot + 9) % 10;
                    keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                    persist_keys();
                    show_toast("Save Slot " + std::to_string(cfg.save_slot));
                } else if (sym == SDLK_F7) {
                    cfg.save_slot = (cfg.save_slot + 1) % 10;
                    keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                    persist_keys();
                    show_toast("Save Slot " + std::to_string(cfg.save_slot));
                } else if (sym == SDLK_F8) {
                    if (psx::load_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                        sound_pipe.flush();
                        show_toast("Loaded State from Slot " + std::to_string(cfg.save_slot));
                    } else {
                        show_toast("Slot " + std::to_string(cfg.save_slot) + " is Empty");
                    }
                } else if (sym == SDLK_F12) {
                    take_clean_screenshot();
                } else if (menu_open) {
                    const int rc = psx_tab_row_count(menu_tab);
                    if (sym == SDLK_UP) {
                        menu_cursor = (menu_cursor + rc - 1) % rc;
                    } else if (sym == SDLK_DOWN) {
                        menu_cursor = (menu_cursor + 1) % rc;
                    } else if (sym == SDLK_q) {
                        menu_tab = (menu_tab + kPsxTabCount - 1) % kPsxTabCount;
                        menu_cursor = 0;
                    } else if (sym == SDLK_e || sym == SDLK_TAB) {
                        menu_tab = (menu_tab + 1) % kPsxTabCount;
                        menu_cursor = 0;
                    } else if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                        step_menu_item(menu_tab, menu_cursor, (sym == SDLK_RIGHT) ? 1 : -1);
                    } else if (sym == SDLK_RETURN || sym == SDLK_SPACE) {
                        activate_menu_item(menu_tab, menu_cursor);
                    }
                }
            }
        }

        const std::uint8_t *kb = SDL_GetKeyboardState(nullptr);
        bool combo_now = false;
        update_pad_from_inputs(sys.hw.pads[0], cfg, kb, controller, combo_now);
        if (combo_now && !prev_combo) {
            menu_open = !menu_open;
            sound_pipe.set_paused(menu_open);
        }
        prev_combo = combo_now;

        const bool pad_turbo = (~sys.hw.pads[0].buttons & (psx::kPadSelect | psx::kPadR2)) == (psx::kPadSelect | psx::kPadR2);
        const bool turbo_active = !menu_open && (kb[SDL_SCANCODE_TAB] || turbo_toggle || pad_turbo);
        const int turbo_mul = std::clamp(cfg.turbo_speed > 0 ? cfg.turbo_speed : 3, 2, 4);

        if (!menu_open) {
            const int steps = turbo_active ? turbo_mul : 1;
            for (int s = 0; s < steps && !sys.cpu.halted; ++s) {
                sys.run_frame();
            }
            sound_pipe.frame(steady_seconds());
        }

        // Update window title when active disc changes
        if (sys.cdrom.active_disc() != last_disc_shown) {
            last_disc_shown = sys.cdrom.active_disc();
            char title[128];
            std::snprintf(title, sizeof(title), "Koudelka (1898 Prequel) — Disc %d/4", last_disc_shown);
            SDL_SetWindowTitle(window, title);
        }

        // Present game frame + independent window-resolution UI overlay (never shrinks with gpu.scale!)
        int fw = 0, fh = 0;
        gpu.present_rgba8888(frame_rgba, fw, fh);
        if (fw > 0 && fh > 0) {
            apply_post_fx(frame_rgba, fw, fh, cfg);

            if (!tex || tex_w != fw || tex_h != fh || tex_filter_dirty) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING, fw, fh);
                tex_w = fw;
                tex_h = fh;
                tex_filter_dirty = false;
            }
            SDL_UpdateTexture(tex, nullptr, frame_rgba.data(), fw * static_cast<int>(sizeof(std::uint32_t)));

            int win_w = 0, win_h = 0;
            SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            SDL_Rect dst = {0, 0, win_w, win_h};
            if (cfg.aspect != 2 && win_w > 0 && win_h > 0) {
                const double target_ar = (cfg.aspect == 1) ? (16.0 / 9.0) : (4.0 / 3.0);
                const double win_ar = static_cast<double>(win_w) / static_cast<double>(win_h);
                if (win_ar > target_ar) {
                    dst.w = static_cast<int>(win_h * target_ar + 0.5);
                    dst.x = (win_w - dst.w) / 2;
                } else {
                    dst.h = static_cast<int>(win_w / target_ar + 0.5);
                    dst.y = (win_h - dst.h) / 2;
                }
            }
            SDL_RenderCopy(renderer, tex, nullptr, &dst);

            const double now = steady_seconds();
            const bool need_ui = menu_open || cfg.perf_hud != 0 || turbo_active || (!toast_msg.empty() && now < toast_until);
            if (need_ui && win_w > 0 && win_h > 0) {
                int uw = 960, uh = 720;
                get_ui_size(uw, uh);
                ui_rgba.assign(static_cast<std::size_t>(uw * uh), 0u);
                ui_draw::Surface ui_surf(ui_rgba.data(), uw, uh);
                draw_hud_and_toast(ui_surf, cfg.perf_hud != 0, fps_smooth, turbo_active, turbo_mul,
                                   sys.cdrom.active_disc(), gpu.scale, toast_msg, toast_until, now);
                if (menu_open) {
                    std::error_code ec;
                    const bool exists = fs::exists(slot_path(cfg.save_slot), ec);
                    draw_quick_menu_overlay(ui_surf, menu_tab, menu_cursor, hover_tab, hover_row,
                                            hover_close, cfg, sys, gpu, turbo_toggle, exists,
                                            (now < toast_until) ? toast_msg : std::string());
                }
                if (!ui_tex || ui_tex_w != uw || ui_tex_h != uh) {
                    if (ui_tex) SDL_DestroyTexture(ui_tex);
                    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
                    ui_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                               SDL_TEXTUREACCESS_STREAMING, uw, uh);
                    if (ui_tex) SDL_SetTextureBlendMode(ui_tex, SDL_BLENDMODE_BLEND);
                    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.sharp_2d <= 0 ? "linear" : "nearest");
                    ui_tex_w = uw;
                    ui_tex_h = uh;
                }
                if (ui_tex) {
                    SDL_UpdateTexture(ui_tex, nullptr, ui_rgba.data(), uw * static_cast<int>(sizeof(std::uint32_t)));
                    SDL_RenderCopy(renderer, ui_tex, nullptr, nullptr);
                }
            }
            SDL_RenderPresent(renderer);
        }

        if (max_frames > 0 && static_cast<int>(sys.frame_count) >= max_frames) {
            if (!dump_frame_path.empty() && fw > 0 && fh > 0) {
                screenshot::write_png(dump_frame_path, frame_rgba.data(),
                                      static_cast<std::uint32_t>(fw), static_cast<std::uint32_t>(fh));
            }
            break;
        }

        // Frame pacing (~60.0 Hz when not in turbo)
        if (!turbo_active) {
            constexpr double kTargetFrameSec = 1.0 / 60.0;
            const double elapsed = steady_seconds() - frame_start;
            if (elapsed < kTargetFrameSec) {
                std::this_thread::sleep_for(std::chrono::duration<double>(kTargetFrameSec - elapsed));
            }
        }
        const double now_t = steady_seconds();
        const double dt = std::max(0.0005, now_t - last_frame_t);
        last_frame_t = now_t;
        fps_smooth = fps_smooth * 0.9 + (1.0 / dt) * 0.1;
    }

    sys.hw.memcards[0].save();
    screenshot::wait_pending();
    if (ui_tex) SDL_DestroyTexture(ui_tex);
    if (tex) SDL_DestroyTexture(tex);
    if (controller) SDL_GameControllerClose(controller);
    if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
