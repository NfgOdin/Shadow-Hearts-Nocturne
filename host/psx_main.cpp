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

void draw_quick_menu_overlay(ui_draw::Surface &surf, int cursor, const keymap::Config &cfg,
                             const psx::System &sys, const psx::Gpu &gpu, const std::string &toast_msg) {
    using namespace ui_draw;
    const int sw = surf.width;
    const int sh = surf.height;

    // Dark translucent backdrop
    surf.fill_rect(0, 0, sw, sh, rgba(8, 6, 10, 195));

    const int mw = std::min(sw - 32, 540);
    const int mh = std::min(sh - 24, 410);
    const int mx = (sw - mw) / 2;
    const int my = (sh - mh) / 2;

    surf.fill_gradient_rounded_rect(mx, my, mw, mh, 10.0f, rgba(22, 16, 24, 248), rgba(14, 10, 16, 248));
    surf.stroke_rounded_rect(mx, my, mw, mh, 10.0f, 1.5f, rgba(186, 146, 84, 220));

    // Gothic Header
    draw_text(surf, mx + 20, my + 14, FontStyle::Serif, 18, true,
              "KOUDELKA (1898 PREQUEL) — QUICK MENU", rgba(235, 205, 140));
    char sub[128];
    std::snprintf(sub, sizeof(sub), "Disc %d / 4  •  Scale %dx  •  AOT %llu  •  Slot %d",
                  sys.cdrom.active_disc(), gpu.scale,
                  static_cast<unsigned long long>(sys.aot_hits), cfg.save_slot);
    draw_text(surf, mx + 20, my + 38, FontStyle::Sans, 12, false, sub, rgba(175, 165, 180));
    surf.fill_rect(mx + 16, my + 58, mw - 32, 1, rgba(186, 146, 84, 110));

    const char *aspect_names[3] = {"4:3 Original", "16:9 Widescreen", "Stretch"};
    const char * items[9] = {
        "Resume Game",
        "Save State (F5)",
        "Load State (F8)",
        "Save Slot",
        "Internal Resolution Scale",
        "Aspect Ratio",
        "Active Disc (1-4)",
        "Master Volume",
        "Save Memcard & Quit"
    };

    char vals[9][64] = {};
    std::snprintf(vals[0], 64, "F1 / Esc");
    std::snprintf(vals[1], 64, "slot_%d.state", cfg.save_slot);
    std::snprintf(vals[2], 64, "slot_%d.state", cfg.save_slot);
    std::snprintf(vals[3], 64, "< Slot %d >", cfg.save_slot);
    std::snprintf(vals[4], 64, "< %dx (%dx%d) >", gpu.scale, 320 * gpu.scale, 240 * gpu.scale);
    std::snprintf(vals[5], 64, "< %s >", aspect_names[std::clamp(cfg.aspect, 0, 2)]);
    std::snprintf(vals[6], 64, "< Disc %d (SLUS-01%s) >", sys.cdrom.active_disc(),
                  sys.cdrom.active_disc() == 1 ? "051" :
                  (sys.cdrom.active_disc() == 2 ? "100" :
                  (sys.cdrom.active_disc() == 3 ? "101" : "102")));
    if (cfg.mute) std::snprintf(vals[7], 64, "< Muted >");
    else std::snprintf(vals[7], 64, "< %d%% >", cfg.master_volume);
    std::snprintf(vals[8], 64, "Enter / Cross");

    const int row_y0 = my + 68;
    const int row_h = (mh - 115) / 9;

    for (int i = 0; i < 9; ++i) {
        const int ry = row_y0 + i * row_h;
        const bool sel = (i == cursor);
        if (sel) {
            surf.fill_rounded_rect(mx + 16, ry, mw - 32, row_h - 3, 6.0f, rgba(92, 34, 48, 210));
            surf.stroke_rounded_rect(mx + 16, ry, mw - 32, row_h - 3, 6.0f, 1.0f, rgba(220, 178, 106, 230));
        }
        draw_text(surf, mx + 28, ry + (row_h - 18) / 2, FontStyle::Sans, 13, sel,
                  items[i], sel ? rgba(255, 238, 200) : rgba(215, 210, 220));
        const auto vm = measure_text(FontStyle::Sans, 13, true, vals[i]);
        draw_text(surf, mx + mw - 28 - vm.width, ry + (row_h - 18) / 2, FontStyle::Sans, 13, true,
                  vals[i], sel ? rgba(240, 198, 116) : rgba(165, 155, 175));
    }

    const std::string footer = toast_msg.empty()
        ? "Up/Down: Select  •  Left/Right: Adjust  •  Enter/Cross: Confirm  •  Tab/F3: Turbo"
        : toast_msg;
    draw_text(surf, mx + 20, my + mh - 28, FontStyle::Sans, 11, !toast_msg.empty(),
              footer, toast_msg.empty() ? rgba(155, 148, 165) : rgba(245, 210, 125));
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
    bool headless = false;
    bool no_aot = false;
    std::string dump_frame_path;
    std::string load_state_arg;
    std::string save_state_arg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            start_disc = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            cli_scale = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
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
    sys.cdrom.set_xa_callback([&](const std::uint8_t *raw2352, std::uint8_t, std::uint8_t) {
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
        const int target_frames = (max_frames > 0) ? max_frames : 60;
        for (int f = 0; f < target_frames && !sys.cpu.halted; ++f) {
            sys.run_frame();
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
        std::printf("[koudelka-headless] ran %llu frames (pc=0x%08X ra=0x%08X sp=0x%08X aot=%llu interp=%llu gpu_prims=%llu vram_uploads=%llu mdec_blks=%llu disc=%d frame=%dx%d)\n",
                    static_cast<unsigned long long>(sys.frame_count),
                    sys.cpu.pc, sys.cpu.r[31], sys.cpu.r[29],
                    static_cast<unsigned long long>(sys.aot_hits),
                    static_cast<unsigned long long>(sys.interp_fallbacks),
                    static_cast<unsigned long long>(gpu.primitives_drawn),
                    static_cast<unsigned long long>(gpu.vram_uploads),
                    static_cast<unsigned long long>(mdec.blocks_decoded),
                    sys.cdrom.active_disc(), fw, fh);
        return 0;
    }

    // 4. Interactive Windowed / Fullscreen SDL2 + SoundPipe initialization
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[koudelka] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.sharp_2d == 0 ? "linear" : "nearest");

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
    std::vector<std::uint32_t> frame_rgba;

    bool running = true;
    bool menu_open = false;
    int menu_cursor = 0;
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
                    show_toast(cfg.scanlines ? "CRT Scanlines: ON (25%)" : "CRT Scanlines: OFF");
                } else if (sym == SDLK_F5) {
                    if (psx::save_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                        show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                    } else {
                        show_toast("Save State Failed");
                    }
                } else if (sym == SDLK_F8) {
                    if (psx::load_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                        sound_pipe.flush();
                        show_toast("Loaded State from Slot " + std::to_string(cfg.save_slot));
                    } else {
                        show_toast("Slot " + std::to_string(cfg.save_slot) + " is Empty");
                    }
                } else if (sym == SDLK_F12) {
                    int sw = 0, sh = 0;
                    std::vector<std::uint32_t> clean;
                    gpu.present_rgba8888(clean, sw, sh);
                    if (sw > 0 && sh > 0) {
                        screenshot::save_game_async(std::move(clean),
                                                    static_cast<std::uint32_t>(sw),
                                                    static_cast<std::uint32_t>(sh));
                        show_toast("Screenshot Saved");
                    }
                } else if (menu_open) {
                    if (sym == SDLK_UP) {
                        menu_cursor = (menu_cursor + 8) % 9;
                    } else if (sym == SDLK_DOWN) {
                        menu_cursor = (menu_cursor + 1) % 9;
                    } else if (sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                        const int dir = (sym == SDLK_RIGHT) ? 1 : -1;
                        if (menu_cursor == 3) {
                            cfg.save_slot = (cfg.save_slot + 10 + dir) % 10;
                            keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                            persist_keys();
                        } else if (menu_cursor == 4) {
                            const int ns = std::clamp(gpu.scale + dir, 1, 4);
                            gpu.set_scale(ns);
                            cfg.scale = ns;
                            keys_text = keymap::with_scale(keys_text, ns);
                            persist_keys();
                        } else if (menu_cursor == 5) {
                            cfg.aspect = (cfg.aspect + 3 + dir) % 3;
                            keys_text = keymap::with_setting(keys_text, "aspect",
                                cfg.aspect == 0 ? "4:3" : (cfg.aspect == 1 ? "16:9" : "stretch"));
                            persist_keys();
                        } else if (menu_cursor == 6) {
                            const int nd = ((sys.cdrom.active_disc() - 1 + 4 + dir) % 4) + 1;
                            sys.cdrom.swap_disc(nd);
                            show_toast("Swapping to Koudelka Disc " + std::to_string(nd) + "...");
                        } else if (menu_cursor == 7) {
                            cfg.mute = 0;
                            cfg.master_volume = std::clamp(cfg.master_volume + dir * 5, 0, 100);
                            sound_pipe.set_muted(false);
                            sound_pipe.set_volume(cfg.master_volume / 100.0f);
                            keys_text = keymap::with_setting(keys_text, "master_volume", std::to_string(cfg.master_volume));
                            persist_keys();
                        }
                    } else if (sym == SDLK_RETURN || sym == SDLK_SPACE) {
                        if (menu_cursor == 0) {
                            menu_open = false;
                            sound_pipe.set_paused(false);
                        } else if (menu_cursor == 1) {
                            if (psx::save_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                                show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                            }
                        } else if (menu_cursor == 2) {
                            if (psx::load_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                                sound_pipe.flush();
                                show_toast("Loaded State from Slot " + std::to_string(cfg.save_slot));
                                menu_open = false;
                                sound_pipe.set_paused(false);
                            } else {
                                show_toast("Slot " + std::to_string(cfg.save_slot) + " is Empty");
                            }
                        } else if (menu_cursor == 7) {
                            cfg.mute = !cfg.mute;
                            sound_pipe.set_muted(cfg.mute != 0);
                            keys_text = keymap::with_setting(keys_text, "mute", cfg.mute ? "1" : "0");
                            persist_keys();
                        } else if (menu_cursor == 8) {
                            sys.hw.memcards[0].save();
                            running = false;
                        }
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

        // Present frame + overlays
        int fw = 0, fh = 0;
        gpu.present_rgba8888(frame_rgba, fw, fh);
        if (fw > 0 && fh > 0) {
            apply_post_fx(frame_rgba, fw, fh, cfg);
            ui_draw::Surface surf(frame_rgba.data(), fw, fh);
            const double now = steady_seconds();
            draw_hud_and_toast(surf, cfg.perf_hud != 0, fps_smooth, turbo_active, turbo_mul,
                               sys.cdrom.active_disc(), gpu.scale, toast_msg, toast_until, now);
            if (menu_open) {
                draw_quick_menu_overlay(surf, menu_cursor, cfg, sys, gpu,
                                        (now < toast_until) ? toast_msg : std::string());
            }

            if (!tex || tex_w != fw || tex_h != fh) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING, fw, fh);
                tex_w = fw;
                tex_h = fh;
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
    if (tex) SDL_DestroyTexture(tex);
    if (controller) SDL_GameControllerClose(controller);
    if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
