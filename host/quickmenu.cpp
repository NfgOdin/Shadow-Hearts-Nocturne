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

// Settings > Controls Tab (4 items + Back = 5)
constexpr int kCtlEntries = 5;
constexpr Command kCtlCmds[kCtlEntries] = {Command::fast_disc, Command::slot_picker_toggle, Command::keys, Command::change_keys, Command::none};

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
    case Command::fast_disc: case Command::slot_picker_toggle:
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
    if (kCtlCmds[i] == Command::fast_disc)
        return std::string("Fast disc loading (SSD):  < ") + (v.fast_disc ? "on" : "off") + " >";
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
    // -1 (All), 0..6 (7 Tiers) => 8 values
    const int cur = ach_tier_filter_ + 1;
    ach_tier_filter_ = ((cur + delta) % 8 + 8) % 8 - 1;
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
    clear_slot_armed();
    const int n = count();
    if (n <= 0) return;
    selected_ = (selected_ + delta % n + n) % n;
    if (page_ == Page::settings) fit(n, kSettingsVisible);
    else if (page_ == Page::keys) fit(key_rows_, kKeysVisible);
    else if (page_ == Page::achievements) fit(static_cast<int>(ach_filtered_.size()), std::max(1, ach_visible_));
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
    if (page_ == Page::save_states) {
        if (y >= rows_y_ && y < rows_y_ + 10 * entry_h_) {
            out_row = (y - rows_y_) / entry_h_;
            return true;
        }
        if (y >= footer_y_ && y < footer_y_ + entry_h_) {
            out_row = 10;
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
    if (page_ == Page::achievements) {
        for (int t = 0; t < 8; ++t) {
            if (ach_tier_rects_[t].contains(x, y)) {
                set_ach_tier(t - 1);
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

namespace {

struct CachedAchThumb {
    bool searched = false;
    bool ok = false;
    int w = 0, h = 0;
    std::vector<std::uint32_t> rgba;
};

const CachedAchThumb &lookup_ach_thumb(const std::string &id, const std::string &title, int max_w, int max_h) {
    static std::unordered_map<std::string, CachedAchThumb> cache;
    const std::string key = id + "#" + std::to_string(max_w) + "x" + std::to_string(max_h);
    auto &entry = cache[key];
    if (!entry.searched) {
        entry.searched = true;
        const auto p = screenshot::find_achievement_screenshot(title);
        if (!p.empty()) {
            entry.ok = ui_draw::load_png_thumbnail(p, max_w, max_h, entry.rgba, entry.w, entry.h);
        }
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
                       "Shadow Hearts: Covenant  —  Trophy Gallery", ui_draw::rgba(246, 212, 126, 255), true);

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
    static const char *const kTabShort[8] = {"All", "Mark", "Sigil", "Crest", "Sovereign", "Covenant", "Follies", "Curiosities"};
    for (int t = 0; t < 8; ++t) {
        const int tx = inner_x + t * (tab_w + tab_gap);
        const bool active = (ach_tier_filter_ == t - 1);
        std::uint32_t gl, gr, bdr, acc;
        if (t == 0) {
            gl = ui_draw::rgba(46, 36, 26, 240);
            gr = ui_draw::rgba(128, 92, 42, 240);
            bdr = ui_draw::rgba(232, 188, 92, 255);
            acc = ui_draw::rgba(255, 228, 150, 255);
        } else {
            achievements::Manager::theme_colors(static_cast<achievements::Tier>(t - 1), values.toast_theme, gl, gr, bdr, acc);
        }
        const float tr = static_cast<float>(spx(7.0f, 4));
        if (active) {
            surf.fill_gradient_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, gl, gr, 0.20f, true);
            surf.stroke_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, 1.6f, bdr);
        } else {
            surf.fill_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, ui_draw::rgba(22, 24, 38, 225));
            surf.stroke_rounded_rect(tx, tab_y, tab_w, tab_bar_h, tr, 1.0f, ui_draw::rgba(74, 80, 108, 170));
        }
        const int cu = (t == 0) ? values.ach_unlocked : tier_u[t - 1];
        const int ct = (t == 0) ? values.ach_total : tier_t[t - 1];
        const std::string tlabel = std::string(kTabShort[t]) + " (" + std::to_string(cu) + "/" + std::to_string(ct) + ")";
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
                const auto &thumb = lookup_ach_thumb(e.id, e.title, iw - 8, img_h);
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
        my += 11 * s;
        if (slot_delete_armed_)
            c.text(right_x, my, s, fit_text("Press Del/Y again to DELETE!", right_chars), kTitle);
        else if (slot_save_armed_)
            c.text(right_x, my, s, fit_text("Press again to OVERWRITE!", right_chars), kTitle);
    } else {
        c.text(right_x, my, s, "Press X (or F5) to save here.", kDim);
    }

    const int hint_y = out.height - pad * s - 8 * s;
    const std::string hint = save_mode_ == SavePickerMode::save_pick
        ? "Enter / 0-9 / Cross: Save to Slot     Esc / Circle: Cancel"
        : (save_mode_ == SavePickerMode::load_pick
            ? "Enter / 0-9 / Cross: Load Slot     Esc / Circle: Cancel"
            : "Enter/L: Load   X/F5: Save   Del/Y: Delete   0-9: Jump   Esc: Back");
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
