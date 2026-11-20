#include "quickmenu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace quickmenu {
namespace {

constexpr const char *kFont[95] = {
    "..... ..... ..... ..... ..... ..... ..... .....",  // space
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.. .....",  // !
    ".#.#. .#.#. ..... ..... ..... ..... ..... .....",  // "
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#. .....",  // #
    "..#.. .#### #.#.. .###. ..#.# ####. ..#.. .....",  // $
    "##... ##..# ...#. ..#.. .#... #..## ...## .....",  // %
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.# .....",  // &
    "..#.. ..#.. ..... ..... ..... ..... ..... .....",  // '
    "...#. ..#.. .#... .#... .#... ..#.. ...#. .....",  // (
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#... .....",  // )
    "..... ..#.. #.#.# .###. #.#.# ..#.. ..... .....",  // *
    "..... ..#.. ..#.. ##### ..#.. ..#.. ..... .....",  // +
    "..... ..... ..... ..... ..... .##.. ..#.. .#...",  // ,
    "..... ..... ..... ##### ..... ..... ..... .....",  // -
    "..... ..... ..... ..... ..... .##.. .##.. .....",  // .
    "..... ....# ...#. ..#.. .#... #.... ..... .....",  // /
    ".###. #...# #..## #.#.# ##..# #...# .###. .....",  // 0
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // 1
    ".###. #...# ....# ...#. ..#.. .#... ##### .....",  // 2
    "##### ...#. ..#.. ...#. ....# #...# .###. .....",  // 3
    "...#. ..##. .#.#. #..#. ##### ...#. ...#. .....",  // 4
    "##### #.... ####. ....# ....# #...# .###. .....",  // 5
    "..##. .#... #.... ####. #...# #...# .###. .....",  // 6
    "##### ....# ...#. ..#.. .#... .#... .#... .....",  // 7
    ".###. #...# #...# .###. #...# #...# .###. .....",  // 8
    ".###. #...# #...# .#### ....# ...#. .##.. .....",  // 9
    "..... .##.. .##.. ..... .##.. .##.. ..... .....",  // :
    "..... .##.. .##.. ..... .##.. ..#.. .#... .....",  // ;
    "...#. ..#.. .#... #.... .#... ..#.. ...#. .....",  // <
    "..... ..... ##### ..... ##### ..... ..... .....",  // =
    ".#... ..#.. ...#. ....# ...#. ..#.. .#... .....",  // >
    ".###. #...# ....# ...#. ..#.. ..... ..#.. .....",  // ?
    ".###. #...# ....# .##.# #.#.# #.#.# .###. .....",  // @
    ".###. #...# #...# ##### #...# #...# #...# .....",  // A
    "####. #...# #...# ####. #...# #...# ####. .....",  // B
    ".###. #...# #.... #.... #.... #...# .###. .....",  // C
    "####. #...# #...# #...# #...# #...# ####. .....",  // D
    "##### #.... #.... ####. #.... #.... ##### .....",  // E
    "##### #.... #.... ####. #.... #.... #.... .....",  // F
    ".###. #...# #.... #.### #...# #...# .#### .....",  // G
    "#...# #...# #...# ##### #...# #...# #...# .....",  // H
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // I
    "..### ...#. ...#. ...#. ...#. #..#. .##.. .....",  // J
    "#...# #..#. #.#.. ##... #.#.. #..#. #...# .....",  // K
    "#.... #.... #.... #.... #.... #.... ##### .....",  // L
    "#...# ##.## #.#.# #.#.# #...# #...# #...# .....",  // M
    "#...# #...# ##..# #.#.# #..## #...# #...# .....",  // N
    ".###. #...# #...# #...# #...# #...# .###. .....",  // O
    "####. #...# #...# ####. #.... #.... #.... .....",  // P
    ".###. #...# #...# #...# #.#.# #..#. .##.# .....",  // Q
    "####. #...# #...# ####. #.#.. #..#. #...# .....",  // R
    ".#### #.... #.... .###. ....# ....# ####. .....",  // S
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // T
    "#...# #...# #...# #...# #...# #...# .###. .....",  // U
    "#...# #...# #...# #...# #...# .#.#. ..#.. .....",  // V
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#. .....",  // W
    "#...# #...# .#.#. ..#.. .#.#. #...# #...# .....",  // X
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.. .....",  // Y
    "##### ....# ...#. ..#.. .#... #.... ##### .....",  // Z
    ".###. .#... .#... .#... .#... .#... .###. .....",  // [
    "..... #.... .#... ..#.. ...#. ....# ..... .....",  // backslash
    ".###. ...#. ...#. ...#. ...#. ...#. .###. .....",  // ]
    "..#.. .#.#. #...# ..... ..... ..... ..... .....",  // ^
    "..... ..... ..... ..... ..... ..... ##### .....",  // _
    ".#... ..#.. ..... ..... ..... ..... ..... .....",  // `
    "..... ..... .###. ....# .#### #...# .#### .....",  // a
    "#.... #.... #.##. ##..# #...# #...# ####. .....",  // b
    "..... ..... .###. #.... #.... #...# .###. .....",  // c
    "....# ....# .##.# #..## #...# #...# .#### .....",  // d
    "..... ..... .###. #...# ##### #.... .###. .....",  // e
    "..##. .#..# .#... ###.. .#... .#... .#... .....",  // f
    "..... ..... .#### #...# #...# .#### ....# .###.",  // g
    "#.... #.... #.##. ##..# #...# #...# #...# .....",  // h
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###. .....",  // i
    "...#. ..... ..##. ...#. ...#. ...#. #..#. .##..",  // j
    "#.... #.... #..#. #.#.. ##... #.#.. #..#. .....",  // k
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // l
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.# .....",  // m
    "..... ..... #.##. ##..# #...# #...# #...# .....",  // n
    "..... ..... .###. #...# #...# #...# .###. .....",  // o
    "..... ..... ####. #...# #...# ####. #.... #....",  // p
    "..... ..... .#### #...# #...# .#### ....# ....#",  // q
    "..... ..... #.##. ##..# #.... #.... #.... .....",  // r
    "..... ..... .###. #.... .###. ....# ####. .....",  // s
    ".#... .#... ###.. .#... .#... .#..# ..##. .....",  // t
    "..... ..... #...# #...# #...# #..## .##.# .....",  // u
    "..... ..... #...# #...# #...# .#.#. ..#.. .....",  // v
    "..... ..... #...# #...# #.#.# #.#.# .#.#. .....",  // w
    "..... ..... #...# .#.#. ..#.. .#.#. #...# .....",  // x
    "..... ..... #...# #...# #...# .#### ....# .###.",  // y
    "..... ..... ##### ...#. ..#.. .#... ##### .....",  // z
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#. .....",  // {
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // |
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#... .....",  // }
    "..... ..... .#... #.#.# ...#. ..... ..... .....",  // ~
};

constexpr std::uint32_t rgba(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
}

constexpr std::uint32_t kPanel = rgba(12, 13, 20, 224), kFrame = rgba(102, 108, 132, 255), kTitle = rgba(242, 202, 112, 255),
                        kText = rgba(230, 232, 240, 255), kBar = rgba(236, 190, 88, 248), kBarText = rgba(16, 16, 20, 255),
                        kDim = rgba(144, 150, 168, 255), kBarDim = rgba(84, 62, 24, 255),
                        kTabBg = rgba(26, 28, 42, 235), kTabActive = rgba(186, 140, 56, 245),
                        kProgressBg = rgba(32, 36, 52, 255), kProgressFill = rgba(226, 180, 74, 255),
                        kGreen = rgba(110, 231, 180, 255);

// Main Hub (8 clean items)
constexpr int kMainEntries = 8;
constexpr Command kMainCmds[kMainEntries] = {
    Command::resume, Command::save, Command::load, Command::open_save_states, Command::screenshot, Command::open_achievements, Command::open_settings, Command::quit
};
constexpr const char *kMainKeys[kMainEntries] = {"F1", "F5", "F8", "F6/F7", "F12", "", "", ""};

// Settings > Graphics Tab (26 items + Back = 27)
constexpr int kGfxEntries = 27;
constexpr Command kGfxCmds[kGfxEntries] = {
    Command::scale_pow2, Command::scale_custom_begin, Command::fps_cap, Command::fps_speed_sync, Command::present_mode,
    Command::look, Command::sharp_2d, Command::aspect, Command::brightness, Command::contrast,
    Command::saturation, Command::gamma, Command::scanlines, Command::deinterlace,
    Command::ssao, Command::bloom, Command::ssgi, Command::filmic, Command::vignette,
    Command::toast_theme, Command::gpu_adapter, Command::gpu_api, Command::gpu_submit, Command::gpu_binning,
    Command::fullscreen, Command::reset_graphics, Command::none
};

// Settings > Audio Tab (7 items + Back = 8)
constexpr int kAudEntries = 8;
constexpr Command kAudCmds[kAudEntries] = {
    Command::master_volume, Command::mute, Command::audio_device, Command::audio_stretch, Command::chime_volume,
    Command::bgm_volume, Command::sfx_volume, Command::none
};

// Settings > Controls Tab (3 items + Back = 4)
constexpr int kCtlEntries = 4;
constexpr Command kCtlCmds[kCtlEntries] = {Command::slot_picker_toggle, Command::keys, Command::change_keys, Command::none};

constexpr int kSettingsVisible = 12;
constexpr int kKeysVisible = 11;
constexpr int kAchVisible = 10;
constexpr int kSettingsChars = 64;
constexpr int kKeysPageChars = 58;
constexpr int kAchChars = 66;

struct Canvas {
    Picture &p;
    void fill(int x, int y, int w, int h, std::uint32_t color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(p.width, x + w), y1 = std::min(p.height, y + h);
        for (int j = y0; j < y1; ++j) std::fill(p.rgba.begin() + j * p.width + x0, p.rgba.begin() + j * p.width + x1, color);
    }
    void text(int x, int y, int s, const std::string &line, std::uint32_t color) {
        for (unsigned char c : line) {
            const char *glyph = kFont[(c >= 32 && c <= 126 ? c : '?') - 32];
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 5; ++col)
                    if (glyph[row * 6 + col] == '#') fill(x + col * s, y + row * s, s, s, color);
            x += 6 * s;
        }
    }
    void border(int s, std::uint32_t color) {
        const int frame = std::max(1, s / 2);
        fill(0, 0, p.width, frame, color);
        fill(0, p.height - frame, p.width, frame, color);
        fill(0, 0, frame, p.height, color);
        fill(p.width - frame, 0, frame, p.height, color);
    }
};

std::string fit_text(const std::string &t, std::size_t max) {
    return t.size() <= max ? t : (max >= 4 ? t.substr(0, max - 3) + "..." : t.substr(0, max));
}

bool is_side_command(Command c) {
    switch (c) {
    case Command::scale_pow2: case Command::fps_cap: case Command::fps_speed_sync: case Command::present_mode:
    case Command::look: case Command::sharp_2d: case Command::aspect: case Command::brightness:
    case Command::contrast: case Command::saturation: case Command::gamma: case Command::scanlines:
    case Command::deinterlace: case Command::ssao: case Command::bloom: case Command::ssgi:
    case Command::filmic: case Command::vignette: case Command::toast_theme:
    case Command::gpu_adapter: case Command::gpu_api: case Command::gpu_submit: case Command::gpu_binning:
    case Command::fullscreen:
    case Command::master_volume: case Command::mute: case Command::audio_device: case Command::audio_stretch:
    case Command::chime_volume: case Command::bgm_volume: case Command::sfx_volume:
    case Command::slot_picker_toggle:
        return true;
    default:
        return false;
    }
}

std::string main_entry_text(int i, const Values &v, bool quit_armed) {
    switch (kMainCmds[i]) {
    case Command::resume: return "Resume";
    case Command::save: return "Quick save   (Slot " + std::to_string(v.active_slot) + ")";
    case Command::load: return "Quick load   (Slot " + std::to_string(v.active_slot) + ")";
    case Command::open_save_states: return "Save States  (0-9)  >";
    case Command::screenshot: return "Take screenshot";
    case Command::open_achievements:
        return "Achievements   (" + std::to_string(v.ach_unlocked) + " / " + std::to_string(v.ach_total) + ")";
    case Command::open_settings: return "Settings  >";
    case Command::quit: return quit_armed ? "Really quit?  Enter / Cross again" : "Quit the game";
    default: return "";
    }
}

std::string settings_entry_text(SettingsTab tab, int i, const Values &v, bool typing_scale, const std::string &scale_input) {
    if (tab == SettingsTab::graphics) {
        if (i == kGfxEntries - 1) return "Back";
        switch (kGfxCmds[i]) {
        case Command::scale_pow2: {
            if (v.scale_locked) return "Resolution: " + std::to_string(v.scale_now) + "x (set by PS2_SCALE)";
            const int sc = v.scale_next;
            const std::string dims = std::to_string(512 * sc) + "x" + std::to_string(448 * sc);
            return "Resolution (2^n): < " + std::to_string(sc) + "x > " + dims + " (~" + Menu::format_bytes(v.vram_bytes_next) + ")" +
                   (sc != v.scale_now ? " *" : "");
        }
        case Command::scale_custom_begin: {
            if (v.scale_locked) return "Custom scale: locked by PS2_SCALE";
            if (typing_scale) {
                const int typed = scale_input.empty() ? 0 : std::atoi(scale_input.c_str());
                const std::string prev = typed >= 1 ? (" -> " + std::to_string(512 * typed) + "x" + std::to_string(448 * typed) + " (~" + Menu::format_bytes(v.vram_bytes_custom) + ")") : "";
                return "Type scale (1-" + std::to_string(v.scale_max) + "): " + scale_input + "_" + prev;
            }
            return "Custom scale (1-" + std::to_string(v.scale_max) + "):  Enter to type   [now " + std::to_string(v.scale_next) + "x]";
        }
        case Command::fps_cap: return "FPS limit:  < " + v.fps_cap_label + " >";
        case Command::fps_speed_sync: return std::string("High-FPS speed sync:  < ") + (v.fps_speed_sync ? "on" : "off") + " >";
        case Command::present_mode: return "VSync / Present:  < " + v.present_mode + " >";
        case Command::look: return "Picture look:  < " + v.look + " >";
        case Command::sharp_2d: {
            static const char *const kNames[] = {"soft", "sharp", "sharpest"};
            const auto name = [](int n) { return kNames[n >= 0 && n <= 2 ? n : 1]; };
            if (v.sharp_locked) return std::string("Text and menus: ") + name(v.sharp_now) + " (PS2_SHARP_2D)";
            return std::string("Text and menus:  < ") + name(v.sharp_next) + " >" + (v.sharp_next != v.sharp_now ? "  next start" : "");
        }
        case Command::aspect: {
            static const char *const kAspect[] = {"4:3 Classic", "16:9 Widescreen", "Stretch to Fill"};
            return std::string("Aspect ratio:  < ") + kAspect[std::clamp(v.aspect_mode, 0, 2)] + " >";
        }
        case Command::brightness: return "Brightness:  < " + std::string(v.brightness > 0 ? "+" : "") + std::to_string(v.brightness) + "% >";
        case Command::contrast: return "Contrast:  < " + std::to_string(v.contrast) + "% >";
        case Command::saturation: return "Saturation:  < " + std::to_string(v.saturation) + "% >";
        case Command::gamma: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v.gamma) / 100.0);
            return std::string("Gamma:  < ") + buf + " >";
        }
        case Command::scanlines: return "CRT scanlines:  < " + (v.scanlines <= 0 ? std::string("off") : (std::to_string(v.scanlines) + "%")) + " >";
        case Command::deinterlace: return std::string("Deinterlacing:  < ") + (v.deinterlace ? "on" : "off") + " >";
        case Command::ssao: return "SSAO (depth):  < " + (v.ssao <= 0 ? std::string("off") : (std::to_string(v.ssao) + "%")) + " >";
        case Command::bloom: return "Bloom / HDR glow:  < " + (v.bloom <= 0 ? std::string("off") : (std::to_string(v.bloom) + "%")) + " >";
        case Command::ssgi: return "SSGI light bounce:  < " + (v.ssgi <= 0 ? std::string("off") : (std::to_string(v.ssgi) + "%")) + " >";
        case Command::filmic: return std::string("Filmic tone map:  < ") + (v.filmic ? "on" : "off") + " >";
        case Command::vignette: return "Gothic vignette:  < " + (v.vignette <= 0 ? std::string("off") : (std::to_string(v.vignette) + "%")) + " >";
        case Command::toast_theme: return std::string("Toast gradient:  < ") + achievements::toast_theme_name(v.toast_theme) + " >";
        case Command::gpu_adapter:
            if (v.gpu_adapter_locked) return "Graphics chip: " + v.gpu_adapter + " (PS2_GPU_ADAPTER)";
            return "Graphics chip:  < " + v.gpu_adapter + " >" + (v.gpu_adapter_next ? " *" : "");
        case Command::gpu_api:
            if (v.gpu_api_locked) return "Graphics API: " + v.gpu_api + " (PS2_GPU_BACKEND)";
            return "Graphics API:  < " + v.gpu_api + " >" + (v.gpu_api_next ? "  next start" : "");
        case Command::gpu_submit: return std::string("Async GPU submit:  < ") + (v.gpu_submit_on ? "on" : "off") + " >";
        case Command::gpu_binning: return std::string("GPU tile binning:  < ") + (v.gpu_binning_on ? "on" : "off") + " >";
        case Command::fullscreen: return std::string("Full screen:  < ") + (v.fullscreen ? "on" : "off") + " >";
        case Command::reset_graphics: return "Reset image adjustments to defaults";
        default: return "";
        }
    }
    if (tab == SettingsTab::audio) {
        if (i == kAudEntries - 1) return "Back";
        switch (kAudCmds[i]) {
        case Command::master_volume: return "Master volume:  < " + std::to_string(v.master_volume) + "% >";
        case Command::mute: return std::string("Mute audio:  < ") + (v.muted ? "on" : "off") + " >";
        case Command::audio_device: return "Output device:  < " + fit_text(v.audio_device, 36) + " >";
        case Command::audio_stretch: return std::string("Audio time-stretch:  < ") + (v.audio_stretch ? "on" : "off") + " >";
        case Command::chime_volume: return "Achievement & UI chime:  < " + std::to_string(v.chime_volume) + "% >";
        case Command::bgm_volume: return "Music & voices (Core 0):  < " + std::to_string(v.bgm_volume) + "% >";
        case Command::sfx_volume: return "Sound effects (Core 1):  < " + std::to_string(v.sfx_volume) + "% >";
        default: return "";
        }
    }
    if (i == kCtlEntries - 1) return "Back";
    if (kCtlCmds[i] == Command::slot_picker_toggle)
        return std::string("F5/F8 opens slot picker:  < ") + (v.slot_picker_on_quickkey ? "on" : "off") + " >";
    return kCtlCmds[i] == Command::keys ? "Show keys" : "Change keys  >";
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
    case Page::save_states: return 11;  // Slots 0..9 + Back
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
    capturing_ = false;
    defaults_armed_ = false;
    typing_scale_ = false;
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
    selected_ = std::clamp(slot, 0, 9);
}

void Menu::close() {
    open_ = false;
    capturing_ = false;
    typing_scale_ = false;
    scale_input_.clear();
}

void Menu::switch_tab(int delta) {
    if (page_ != Page::settings) return;
    typing_scale_ = false;
    const int n = static_cast<int>(SettingsTab::count);
    tab_ = static_cast<SettingsTab>((static_cast<int>(tab_) + delta % n + n) % n);
    selected_ = 0;
    top_ = 0;
}

void Menu::set_tab(SettingsTab t) {
    if (tab_ == t) return;
    typing_scale_ = false;
    tab_ = t;
    selected_ = 0;
    top_ = 0;
}

void Menu::cycle_ach_tier(int delta) {
    if (page_ != Page::achievements) return;
    // -1 (All), 0..6 (7 Tiers) => 8 values
    const int cur = ach_tier_filter_ + 1;
    ach_tier_filter_ = ((cur + delta) % 8 + 8) % 8 - 1;
    selected_ = 0;
    top_ = 0;
}

void Menu::cycle_ach_status(int delta) {
    if (page_ != Page::achievements) return;
    ach_status_filter_ = (ach_status_filter_ + delta % 3 + 3) % 3;
    selected_ = 0;
    top_ = 0;
}

void Menu::toggle_reveal_secrets() { reveal_secrets_ = !reveal_secrets_; }

void Menu::begin_scale_input(int current_scale) {
    typing_scale_ = true;
    scale_input_ = std::to_string(std::max(1, current_scale));
}

void Menu::push_scale_digit(char d) {
    if (!typing_scale_ || d < '0' || d > '9') return;
    if (scale_input_.size() < 2) scale_input_.push_back(d);
}

void Menu::backspace_scale_digit() {
    if (!typing_scale_ || scale_input_.empty()) return;
    scale_input_.pop_back();
}

void Menu::cancel_scale_input() {
    typing_scale_ = false;
    scale_input_.clear();
}

int Menu::finish_scale_input(int min_val, int max_val) {
    typing_scale_ = false;
    const int val = scale_input_.empty() ? min_val : std::atoi(scale_input_.c_str());
    scale_input_.clear();
    return std::clamp(val, min_val, std::max(min_val, max_val));
}

bool Menu::back() {
    if (typing_scale_) {
        cancel_scale_input();
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
    if (page_ == Page::settings || page_ == Page::achievements) {
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
    const int n = count();
    if (n <= 0) return;
    selected_ = (selected_ + delta % n + n) % n;
    if (page_ == Page::settings) fit(n, kSettingsVisible);
    else if (page_ == Page::keys) fit(key_rows_, kKeysVisible);
    else if (page_ == Page::achievements) fit(static_cast<int>(ach_filtered_.size()), kAchVisible);
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
        if (selected_ >= 10) {
            if (!back()) return Command::resume;
            return Command::none;
        }
        return save_mode_ == SavePickerMode::save_pick ? Command::slot_save : Command::slot_load;
    }
    // Page::achievements
    if (selected_ >= static_cast<int>(ach_filtered_.size())) {
        back();
        return Command::none;
    }
    return Command::none;
}

Command Menu::side() const {
    if (page_ != Page::settings) return Command::none;
    const int n = count();
    if (selected_ >= n - 1) return Command::none;
    const Command c = tab_ == SettingsTab::graphics ? kGfxCmds[selected_]
                    : (tab_ == SettingsTab::audio ? kAudCmds[selected_] : kCtlCmds[selected_]);
    return is_side_command(c) ? c : Command::none;
}

bool Menu::select_at(int x, int y) {
    if (capturing_ || typing_scale_ || entry_h_ <= 0) return false;
    if (page_ == Page::main) {
        if (x < entry_x_ || x >= entry_x_ + entry_w_ || y < entry_y_ || y >= entry_y_ + kMainEntries * entry_h_) return false;
        const int now = (y - entry_y_) / entry_h_;
        if (now != selected_) quit_armed_ = false;
        selected_ = now;
        return true;
    }
    if (page_ == Page::settings) {
        if (x < entry_x_ || x >= entry_x_ + entry_w_ || y < rows_y_ || y >= rows_y_ + visible_count_ * entry_h_) return false;
        const int now = top_ + (y - rows_y_) / entry_h_;
        if (now < 0 || now >= count()) return false;
        selected_ = now;
        return true;
    }
    if (page_ == Page::keys) {
        if (x < entry_x_ || x >= entry_x_ + entry_w_) return false;
        int now = -1;
        if (y >= rows_y_ && y < rows_y_ + keys_shown_ * entry_h_) now = top_ + (y - rows_y_) / entry_h_;
        else if (y >= footer_y_ && y < footer_y_ + 2 * entry_h_) now = key_rows_ + (y - footer_y_) / entry_h_;
        if (now < 0) return false;
        if (now != selected_) defaults_armed_ = false;
        selected_ = now;
        return true;
    }
    if (page_ == Page::save_states) {
        if (x < entry_x_ || x >= entry_x_ + entry_w_) return false;
        if (y >= rows_y_ && y < rows_y_ + 10 * entry_h_) {
            selected_ = (y - rows_y_) / entry_h_;
            return true;
        }
        if (y >= footer_y_ && y < footer_y_ + entry_h_) {
            selected_ = 10;
            return true;
        }
    }
    if (page_ == Page::achievements) {
        if (x < entry_x_ || x >= entry_x_ + entry_w_) return false;
        if (y >= rows_y_ && y < rows_y_ + ach_shown_ * entry_h_) {
            selected_ = top_ + (y - rows_y_) / entry_h_;
            return true;
        }
        if (y >= footer_y_ && y < footer_y_ + entry_h_) {
            selected_ = static_cast<int>(ach_filtered_.size());
            return true;
        }
    }
    return false;
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
    if (!select_at(x, y)) return false;
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
    const int n = static_cast<int>(ach_filtered_.size()) + 1;
    if (selected_ >= n) selected_ = std::max(0, n - 1);
    fit(static_cast<int>(ach_filtered_.size()), kAchVisible);
}

void Menu::draw_main(int window_width, int window_height, const Values &values, Picture &out) {
    static const std::string kTitleText = "Shadow Hearts: Covenant - Quick Menu", kHint = "Enter / Cross: OK     F1 / Esc / Circle: Resume";
    std::string lines[kMainEntries];
    std::size_t chars = std::max(kTitleText.size(), kHint.size());
    for (int i = 0; i < kMainEntries; ++i) {
        lines[i] = main_entry_text(i, values, quit_armed_);
        chars = std::max(chars, lines[i].size() + 6);
    }
    const int pad = 9, bar_h = 15, bar_pad = 6;
    const int units_w = 2 * pad + 2 * bar_pad + static_cast<int>(chars) * 6;
    const int units_h = pad + 8 + 12 + kMainEntries * bar_h + 11 + 8 + pad;
    const int s = std::clamp(std::min(window_height * 4 / 5 / units_h, window_width * 23 / 25 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);
    int y = pad * s;
    c.text((pad + bar_pad) * s, y, s, kTitleText, kTitle);
    y += 14 * s;
    c.fill(pad * s, y, out.width - 2 * pad * s, std::max(1, s / 2), kFrame);
    y += 6 * s;
    entry_x_ = out.x + pad * s;
    entry_y_ = out.y + y;
    entry_w_ = out.width - 2 * pad * s;
    entry_h_ = bar_h * s;
    for (int i = 0; i < kMainEntries; ++i) {
        const bool lit = i == selected_;
        if (lit) c.fill(pad * s, y, entry_w_, bar_h * s, kBar);
        c.text((pad + bar_pad) * s, y + 3 * s, s, lines[i], lit ? kBarText : kText);
        const std::string key = kMainKeys[i];
        if (!key.empty())
            c.text(out.width - (pad + bar_pad) * s - static_cast<int>(key.size()) * 6 * s + s, y + 3 * s, s, key, lit ? kBarDim : kDim);
        y += bar_h * s;
    }
    y += 5 * s;
    c.fill(pad * s, y, entry_w_, std::max(1, s / 2), kFrame);
    y += 6 * s;
    c.text((pad + bar_pad) * s, y, s, kHint, kDim);
}

void Menu::draw_settings(int window_width, int window_height, const Values &values, Picture &out) {
    const int n = count();
    const int shown = std::min(n, kSettingsVisible);
    visible_count_ = shown;
    fit(n, kSettingsVisible);

    const int pad = 8, bar_h = 13, bar_pad = 5, tab_h = 14;
    const int units_w = 2 * pad + 2 * bar_pad + kSettingsChars * 6;
    const int units_h = pad + 8 + 8 + tab_h + 8 + shown * bar_h + 11 + 8 + 10 + pad;
    const int s = std::clamp(std::min(window_height * 86 / 100 / units_h, window_width * 94 / 100 / units_w), 1, 5);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);

    const int inner_w = out.width - 2 * pad * s;
    int y = pad * s;
    c.text((pad + bar_pad) * s, y, s, "Settings  (Q / E or L1 / R1: switch tab)", kTitle);
    if (n > shown) {
        const std::string where = std::to_string(top_ + 1) + "-" + std::to_string(top_ + shown) + " of " + std::to_string(n);
        c.text(out.width - (pad + bar_pad) * s - static_cast<int>(where.size()) * 6 * s + s, y, s, where, kDim);
    }
    y += 12 * s;

    // Tab bar: [ Graphics ] [ Audio ] [ Controls ]
    tabs_y_ = out.y + y;
    tabs_h_ = tab_h * s;
    static const char *const kTabNames[3] = {"Graphics", "Audio", "Controls"};
    const int tab_gap = 4 * s;
    const int tab_w = (inner_w - 2 * tab_gap) / 3;
    for (int t = 0; t < 3; ++t) {
        const int tx = pad * s + t * (tab_w + tab_gap);
        const bool active = static_cast<int>(tab_) == t;
        c.fill(tx, y, tab_w, tab_h * s, active ? kTabActive : kTabBg);
        const std::string lbl = kTabNames[t];
        const int lw = static_cast<int>(lbl.size()) * 6 * s;
        c.text(tx + std::max(0, (tab_w - lw) / 2), y + 3 * s, s, lbl, active ? kBarText : kText);
        tab_x0_[t] = out.x + tx;
        tab_x1_[t] = out.x + tx + tab_w;
    }
    y += (tab_h + 5) * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 5 * s;

    entry_x_ = out.x + pad * s;
    entry_w_ = inner_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;

    for (int r = 0; r < shown; ++r) {
        const int idx = top_ + r;
        const bool lit = idx == selected_;
        if (lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
        const std::string line = settings_entry_text(tab_, idx, values, typing_scale_, scale_input_);
        c.text((pad + bar_pad) * s, y + 2 * s, s, fit_text(line, kSettingsChars), lit ? kBarText : kText);
        y += bar_h * s;
    }

    y += 5 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    const std::string hint1 = typing_scale_
        ? "Type digits (0-9)   Backspace: erase   Enter: apply   Esc: cancel"
        : "Left / Right: adjust     Enter: select / type     Esc / Circle: back";
    const std::string hint2 = (tab_ == SettingsTab::graphics) ? "* Takes effect on next launch (all other adjustments are live)" : "";
    c.text((pad + bar_pad) * s, y, s, fit_text(hint1, kSettingsChars), typing_scale_ ? kTitle : kDim);
    if (!hint2.empty()) c.text((pad + bar_pad) * s, y + 10 * s, s, fit_text(hint2, kSettingsChars), kDim);
}

void Menu::draw_keys(int window_width, int window_height, const Values &values, Picture &out) {
    static const std::string kTitleText = "Change keys - the game waits", kHint = "Enter / click: change a key     Esc / Circle: back",
                             kFixed = "Reserved: F1 F2 F5 F8 F9 F11 F12 (program keys)", kCapture = "Press a key or mouse button... (Esc: cancel)";
    key_rows_ = static_cast<int>(values.key_rows.size());
    fit(key_rows_, kKeysVisible);
    const int n = key_rows_, shown = std::min(n, kKeysVisible);
    keys_shown_ = shown;
    const int pad = 8, bar_h = 12, bar_pad = 5, label_chars = 18, keys_chars = 30;
    const int units_w = 2 * pad + 2 * bar_pad + kKeysPageChars * 6;
    const int units_h = pad + 8 + 12 + shown * bar_h + 3 + 2 * bar_h + 11 + 12 + 12 + 8 + pad;
    const int s = std::clamp(std::min(window_height * 4 / 5 / units_h, window_width * 23 / 25 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);
    const int text_x = (pad + bar_pad) * s, inner_w = out.width - 2 * pad * s;
    int y = pad * s;
    c.text(text_x, y, s, kTitleText, kTitle);
    if (n > shown) {
        const std::string where = std::to_string(top_ + 1) + "-" + std::to_string(top_ + shown) + " of " + std::to_string(n);
        c.text(out.width - text_x - static_cast<int>(where.size()) * 6 * s + s, y, s, where, kDim);
    }
    y += 14 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    entry_x_ = out.x + pad * s;
    entry_w_ = inner_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;
    const auto line = [&](int index, const std::string &left, const std::string &right) {
        const bool lit = index == selected_;
        if (lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
        const std::uint32_t color = lit ? kBarText : kText;
        c.text(text_x, y + 2 * s, s, left, color);
        if (!right.empty()) c.text(text_x + (label_chars + 1) * 6 * s, y + 2 * s, s, fit_text(right, keys_chars), color);
        y += bar_h * s;
    };
    for (int r = 0; r < shown; ++r) {
        const int index = top_ + r;
        const Values::KeyRow &row = values.key_rows[index];
        if (index == selected_ && capturing_) line(index, kCapture, "");
        else line(index, row.label, row.keys);
    }
    y += 3 * s;
    footer_y_ = out.y + y;
    line(n, defaults_armed_ ? "Really put back the default keys?  Enter / Cross again" : "Put back the default keys", "");
    line(n + 1, "Back", "");
    y += 5 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 6 * s;
    c.text(text_x, y, s, fit_text(values.key_note, kKeysPageChars), kTitle);
    y += 12 * s;
    c.text(text_x, y, s, kHint, kDim);
    y += 12 * s;
    c.text(text_x, y, s, kFixed, kDim);
}

void Menu::draw_achievements(int window_width, int window_height, const Values &values, Picture &out) {
    rebuild_ach_filter(values);
    const int n = static_cast<int>(ach_filtered_.size());
    const int shown = std::min(n, kAchVisible);
    ach_shown_ = shown;

    const int pad = 8, bar_h = 12, bar_pad = 5;
    const int units_w = 2 * pad + 2 * bar_pad + kAchChars * 6;
    const int units_h = pad + 8 + 10 + 12 + 8 + std::max(shown, 1) * bar_h + 4 + bar_h + 8 + 10 + 10 + 10 + 10 + pad;
    const int s = std::clamp(std::min(window_height * 88 / 100 / units_h, window_width * 94 / 100 / units_w), 1, 5);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);

    const int inner_w = out.width - 2 * pad * s;
    const int text_x = (pad + bar_pad) * s;
    int y = pad * s;

    // Header + total completion count
    const int pct = values.ach_total > 0 ? (values.ach_unlocked * 100 / values.ach_total) : 0;
    const std::string hdr = "Achievements  -  " + std::to_string(values.ach_unlocked) + " / " + std::to_string(values.ach_total) + " (" + std::to_string(pct) + "%)";
    c.text(text_x, y, s, hdr, kTitle);
    y += 10 * s;

    // Total Progress Bar
    c.fill(pad * s, y, inner_w, 5 * s, kProgressBg);
    if (values.ach_total > 0) {
        const int fill_w = inner_w * values.ach_unlocked / values.ach_total;
        c.fill(pad * s, y, fill_w, 5 * s, kProgressFill);
    }
    y += 9 * s;

    // Filter Bar
    const std::string tier_lbl = ach_tier_filter_ < 0 ? "All Tiers" : achievements::tier_name(static_cast<achievements::Tier>(ach_tier_filter_));
    static const char *const kStatusLbl[3] = {"All", "Unlocked", "Locked"};
    const std::string fbar = "Tier (Q/E): [" + tier_lbl + "]   Show (L/R): [" + kStatusLbl[ach_status_filter_] + "]   Tab: " + (reveal_secrets_ ? "Hide Secrets" : "Reveal Secrets");
    c.text(text_x, y, s, fit_text(fbar, kAchChars), kDim);
    y += 11 * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 5 * s;

    entry_x_ = out.x + pad * s;
    entry_w_ = inner_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;

    if (n == 0) {
        c.text(text_x, y + 2 * s, s, "No achievements match this filter yet.", kDim);
        y += bar_h * s;
    } else {
        for (int r = 0; r < shown; ++r) {
            const int idx = top_ + r;
            const auto &e = (*values.ach_entries)[ach_filtered_[idx]];
            const bool lit = idx == selected_;
            std::uint32_t gl, gr, bdr, acc;
            achievements::Manager::theme_colors(e.tier, values.toast_theme, gl, gr, bdr, acc);
            if (lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
            const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
            const std::string tag = std::string("[") + achievements::tier_short(e.tier) + "]";
            const std::string status = e.unlocked ? "[*]" : "[ ]";
            const std::string title = masked ? "Secret Achievement" : e.title;
            const std::string left = status + " " + tag + " " + title;
            c.text(text_x, y + 2 * s, s, fit_text(left, 46), lit ? kBarText : (e.unlocked ? acc : kText));
            const std::string right = e.unlocked ? e.unlocked_at.substr(0, 10) : (masked ? "Secret" : fit_text(e.progress_text, 18));
            c.text(out.width - text_x - static_cast<int>(right.size()) * 6 * s + s, y + 2 * s, s, right, lit ? kBarDim : (e.unlocked ? kGreen : kDim));
            y += bar_h * s;
        }
    }

    y += 4 * s;
    footer_y_ = out.y + y;
    const bool back_lit = selected_ >= n;
    if (back_lit) c.fill(pad * s, y, inner_w, bar_h * s, kBar);
    c.text(text_x, y + 2 * s, s, "Back", back_lit ? kBarText : kText);
    y += (bar_h + 4) * s;
    c.fill(pad * s, y, inner_w, std::max(1, s / 2), kFrame);
    y += 5 * s;

    // Detail footer for the highlighted achievement
    if (selected_ < n && values.ach_entries) {
        const auto &e = (*values.ach_entries)[ach_filtered_[selected_]];
        const bool masked = e.secret && !e.unlocked && !reveal_secrets_;
        const std::string desc = masked ? "Hidden Curiosity - press Tab to reveal unlock hint." : e.description;
        c.text(text_x, y, s, fit_text(desc, kAchChars), kTitle);
        y += 10 * s;
        const std::string meta = e.unlocked
            ? ("Unlocked on " + e.unlocked_at + "  (" + achievements::tier_name(e.tier) + ")")
            : (masked ? std::string("Progress: hidden") : ("Progress: " + (e.progress_text.empty() ? std::string("Not yet started") : e.progress_text)));
        c.text(text_x, y, s, fit_text(meta, kAchChars), e.unlocked ? kGreen : kText);
        y += 10 * s;
    } else {
        y += 20 * s;
    }
    c.text(text_x, y, s, "Up/Down: browse   Q/E: Tier   Left/Right: Status   Esc: Back", kDim);
}

void Menu::draw_save_states(int window_width, int window_height, const Values &values, Picture &out) {
    const int pad = 10, bar_h = 13, gap = 8;
    const int left_chars = 31;
    const int right_chars = 30;
    const int units_w = pad * 2 + (left_chars + right_chars) * 6 + gap;
    const int units_h = pad * 2 + 12 + 5 + 10 * bar_h + 6 + bar_h + 14;
    const int s = std::clamp(std::min(window_width * 90 / 100 / units_w, window_height * 88 / 100 / units_h), 1, 5);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, (window_height - out.height) / 2);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);

    const int left_x = pad * s;
    const int left_w = left_chars * 6 * s;
    const int right_x = left_x + left_w + gap * s;
    const int right_w = right_chars * 6 * s;
    int y = pad * s;

    const std::string title = save_mode_ == SavePickerMode::save_pick ? "Quick Save - Choose Slot (0-9)"
                            : (save_mode_ == SavePickerMode::load_pick ? "Quick Load - Choose Slot (0-9)" : "Save States (Slots 0-9)");
    c.text(left_x + 2 * s, y, s, title, kTitle);
    const std::string active_lbl = "Active: Slot " + std::to_string(values.active_slot);
    c.text(out.width - pad * s - static_cast<int>(active_lbl.size()) * 6 * s, y, s, active_lbl, kGreen);
    y += 12 * s;
    c.fill(left_x, y, out.width - 2 * pad * s, std::max(1, s / 2), kFrame);
    y += 5 * s;

    entry_x_ = out.x + left_x;
    entry_w_ = left_w;
    entry_h_ = bar_h * s;
    rows_y_ = out.y + y;

    const int list_top_y = y;
    for (int i = 0; i < 10; ++i) {
        const auto &sl = values.slots[i];
        const bool lit = selected_ == i;
        if (lit) c.fill(left_x, y, left_w, bar_h * s, kBar);
        const std::string star = (i == values.active_slot) ? "*" : " ";
        std::string summary;
        if (!sl.exists) {
            summary = "(Empty)";
        } else {
            summary = !sl.area.empty() ? sl.area : ("Frame " + std::to_string(sl.frame));
            if (sl.max_level > 0) summary += " Lv" + std::to_string(sl.max_level);
        }
        const std::string line = star + "Slot " + std::to_string(i) + "  " + fit_text(summary, 14);
        c.text(left_x + 3 * s, y + 2 * s, s, line, lit ? kBarText : (sl.exists ? kText : kDim));
        if (sl.exists && sl.bytes > 0) {
            const std::string sz = format_bytes(sl.bytes);
            c.text(left_x + left_w - static_cast<int>(sz.size()) * 6 * s - 3 * s, y + 2 * s, s, sz, lit ? kBarDim : kDim);
        }
        y += bar_h * s;
    }

    y += 6 * s;
    footer_y_ = out.y + y;
    const bool back_lit = selected_ >= 10;
    if (back_lit) c.fill(left_x, y, left_w, bar_h * s, kBar);
    c.text(left_x + 3 * s, y + 2 * s, s, direct_picker_ ? "Cancel (Esc)" : "Back", back_lit ? kBarText : kText);

    // Right Column: Thumbnail + Metadata Panel for the highlighted slot
    const int thumb_box_w = std::min(right_w, 160 * s);
    const int thumb_box_h = thumb_box_w * 112 / 160;
    const int tx0 = right_x + (right_w - thumb_box_w) / 2;
    const int ty0 = list_top_y;
    c.fill(tx0 - s, ty0 - s, thumb_box_w + 2 * s, thumb_box_h + 2 * s, kFrame);
    c.fill(tx0, ty0, thumb_box_w, thumb_box_h, rgba(18, 20, 30, 255));

    const int cur_slot = (selected_ >= 0 && selected_ < 10) ? selected_ : values.active_slot;
    const auto &cur = values.slots[cur_slot];
    if (cur.exists && values.slot_thumb_rgba && !values.slot_thumb_rgba->empty() && values.slot_thumb_w > 0 && values.slot_thumb_h > 0) {
        const auto *src = values.slot_thumb_rgba->data();
        for (int py = 0; py < thumb_box_h; ++py) {
            const int sy = py * values.slot_thumb_h / thumb_box_h;
            const int dy = ty0 + py;
            if (dy < 0 || dy >= out.height) continue;
            for (int px = 0; px < thumb_box_w; ++px) {
                const int sx = px * values.slot_thumb_w / thumb_box_w;
                const int dx = tx0 + px;
                if (dx >= 0 && dx < out.width) out.rgba[static_cast<std::size_t>(dy) * out.width + dx] = src[sy * values.slot_thumb_w + sx];
            }
        }
    } else {
        const std::string ph = cur.exists ? "No Preview" : "Empty Slot";
        c.text(tx0 + (thumb_box_w - static_cast<int>(ph.size()) * 6 * s) / 2, ty0 + (thumb_box_h - 8 * s) / 2, s, ph, kDim);
    }

    int my = ty0 + thumb_box_h + 7 * s;
    c.text(right_x, my, s, "Slot " + std::to_string(cur_slot) + (cur_slot == values.active_slot ? "  [Active]" : ""), kTitle);
    my += 11 * s;
    if (cur.exists) {
        if (!cur.timestamp.empty()) {
            c.text(right_x, my, s, fit_text("Saved: " + cur.timestamp, right_chars), kText);
            my += 10 * s;
        }
        std::string info1 = "Disc " + std::to_string(cur.disc);
        if (!cur.area.empty()) info1 += "  Area: " + cur.area;
        if (cur.max_level > 0) info1 += "  Lv " + std::to_string(cur.max_level);
        c.text(right_x, my, s, fit_text(info1, right_chars), kGreen);
        my += 10 * s;
        if (cur.playtime_sec > 0.5f) {
            const int sec = static_cast<int>(cur.playtime_sec);
            char pt[48];
            std::snprintf(pt, sizeof pt, "Playtime: %d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
            c.text(right_x, my, s, pt, kText);
            my += 10 * s;
        }
        c.text(right_x, my, s, "Size: " + format_bytes(cur.bytes) + " (LZ4)", kDim);
    } else {
        c.text(right_x, my, s, "Press S (or X) to save here.", kDim);
    }

    const int hint_y = out.height - pad * s - 8 * s;
    const std::string hint = save_mode_ == SavePickerMode::save_pick
        ? "Enter / 0-9 / Cross: Save to Slot     Esc / Circle: Cancel"
        : (save_mode_ == SavePickerMode::load_pick
            ? "Enter / 0-9 / Cross: Load Slot     Esc / Circle: Cancel"
            : "Enter/L: Load   S/X: Save   Del/Y: Delete   0-9: Jump   Esc: Back");
    c.text(left_x + 2 * s, hint_y, s, fit_text(hint, left_chars + right_chars), kDim);
}

void Menu::draw(int window_width, int window_height, const Values &values, Picture &out) {
    switch (page_) {
    case Page::main: draw_main(window_width, window_height, values, out); break;
    case Page::settings: draw_settings(window_width, window_height, values, out); break;
    case Page::keys: draw_keys(window_width, window_height, values, out); break;
    case Page::achievements: draw_achievements(window_width, window_height, values, out); break;
    case Page::save_states: draw_save_states(window_width, window_height, values, out); break;
    }
}

void Menu::draw_note(int window_width, int window_height, const std::string &text, Picture &out) {
    const int pad = 5;
    const int units_w = 2 * pad + static_cast<int>(text.size()) * 6, units_h = 2 * pad + 8;
    const int s = std::clamp(std::min(window_height / 240, window_width * 9 / 10 / units_w), 1, 6);
    out.width = units_w * s;
    out.height = units_h * s;
    out.x = std::max(0, (window_width - out.width) / 2);
    out.y = std::max(0, window_height / 20);
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height, kPanel);
    Canvas c{out};
    c.border(s, kFrame);
    c.text(pad * s, pad * s, s, text, kTitle);
}

}  // namespace quickmenu
