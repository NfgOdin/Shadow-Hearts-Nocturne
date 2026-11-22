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
#include <mmsystem.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

std::string now_display_timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

// Migrate any legacy root-level Koudelka save states (games/koudelka/slot_N.state)
// into games/koudelka/states/slot_N.state so the Nocturne Launcher displays all slots.
void migrate_legacy_states(const fs::path &game_dir, const fs::path &states_dir) {
    std::error_code ec;
    for (int s = 0; s < 10; ++s) {
        const std::string fname = "slot_" + std::to_string(s) + ".state";
        const fs::path old_path = game_dir / fname;
        const fs::path new_path = states_dir / fname;
        if (fs::is_regular_file(old_path, ec) && !fs::exists(new_path, ec)) {
            fs::create_directories(states_dir, ec);
            fs::rename(old_path, new_path, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(old_path, new_path, fs::copy_options::overwrite_existing, ec);
                if (!ec) fs::remove(old_path, ec);
            }
        }
    }
}

bool write_slot_thumb_ps2thb1(const fs::path &thumb_path, const std::vector<std::uint32_t> &rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) return false;
    constexpr std::uint32_t kThumbW = 160u;
    constexpr std::uint32_t kThumbH = 112u;
    static constexpr char kMagic[8] = {'P', 'S', '2', 'T', 'H', 'B', '1', 0};

    std::vector<std::uint8_t> out(16u + static_cast<std::size_t>(kThumbW) * kThumbH * 3u, 0u);
    std::memcpy(out.data(), kMagic, 8);
    std::memcpy(out.data() + 8, &kThumbW, 4);
    std::memcpy(out.data() + 12, &kThumbH, 4);

    std::uint8_t *dst = out.data() + 16;
    for (std::uint32_t y = 0; y < kThumbH; ++y) {
        const double sy = (static_cast<double>(y) + 0.5) * static_cast<double>(h) / static_cast<double>(kThumbH) - 0.5;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, h - 1);
        const int y1 = std::min(y0 + 1, h - 1);
        const double fy = std::clamp(sy - y0, 0.0, 1.0);
        for (std::uint32_t x = 0; x < kThumbW; ++x) {
            const double sx = (static_cast<double>(x) + 0.5) * static_cast<double>(w) / static_cast<double>(kThumbW) - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, w - 1);
            const int x1 = std::min(x0 + 1, w - 1);
            const double fx = std::clamp(sx - x0, 0.0, 1.0);

            const std::uint32_t c00 = rgba[static_cast<std::size_t>(y0) * w + x0];
            const std::uint32_t c10 = rgba[static_cast<std::size_t>(y0) * w + x1];
            const std::uint32_t c01 = rgba[static_cast<std::size_t>(y1) * w + x0];
            const std::uint32_t c11 = rgba[static_cast<std::size_t>(y1) * w + x1];

            const double w00 = (1.0 - fx) * (1.0 - fy);
            const double w10 = fx * (1.0 - fy);
            const double w01 = (1.0 - fx) * fy;
            const double w11 = fx * fy;

            auto blend_ch = [&](int shift) -> std::uint8_t {
                const double v = ((c00 >> shift) & 0xFFu) * w00 +
                                 ((c10 >> shift) & 0xFFu) * w10 +
                                 ((c01 >> shift) & 0xFFu) * w01 +
                                 ((c11 >> shift) & 0xFFu) * w11;
                return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v + 0.5), 0, 255));
            };
            const std::size_t di = (static_cast<std::size_t>(y) * kThumbW + x) * 3u;
            dst[di + 0] = blend_ch(0);   // R (ABGR8888 little-endian)
            dst[di + 1] = blend_ch(8);   // G
            dst[di + 2] = blend_ch(16);  // B
        }
    }
    std::ofstream ofs(thumb_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return ofs.good();
}

bool save_koudelka_slot(const fs::path &states_dir, int slot,
                        const psx::System &sys, const psx::Gpu &gpu,
                        const psx::Mdec &mdec, const psx::Spu &spu) {
    std::error_code ec;
    fs::create_directories(states_dir, ec);
    const std::string base = "slot_" + std::to_string(std::clamp(slot, 0, 9));
    const fs::path state_path = states_dir / (base + ".state");
    if (!psx::save_state_file(state_path.string(), sys, gpu, mdec, spu)) return false;

    const std::size_t bytes = fs::is_regular_file(state_path, ec)
                                  ? static_cast<std::size_t>(fs::file_size(state_path, ec))
                                  : 0u;
    const int disc = std::clamp(sys.cdrom.active_disc(), 1, 4);
    const double playtime_sec = static_cast<double>(sys.frame_count) / 60.0;
    char meta[512];
    std::snprintf(meta, sizeof meta,
                  "timestamp=%s\nframe=%llu\nbytes=%zu\ndisc=%d\narea=Nemeton Monastery (Disc %d)\nlevel=0\nplaytime=%.2f\n",
                  now_display_timestamp().c_str(),
                  static_cast<unsigned long long>(sys.frame_count),
                  bytes,
                  disc,
                  disc,
                  playtime_sec);
    write_text_file(states_dir / (base + ".meta"), meta);

    std::vector<std::uint32_t> frame_rgba;
    int fw = 0, fh = 0;
    gpu.present_rgba8888(frame_rgba, fw, fh);
    if (fw > 0 && fh > 0) {
        write_slot_thumb_ps2thb1(states_dir / (base + ".thumb"), frame_rgba, fw, fh);
    }
    return true;
}

struct PsxSlotPreview {
    int loaded_slot = -1;
    bool exists = false;
    std::string timestamp;
    std::string area;
    std::string size_text;
    int disc = 0;
    std::vector<std::uint32_t> thumb_rgba;
    int thumb_w = 0;
    int thumb_h = 0;
};

PsxSlotPreview load_psx_slot_preview(const fs::path &states_dir, int slot) {
    PsxSlotPreview p;
    slot = std::clamp(slot, 0, 9);
    p.loaded_slot = slot;
    const std::string base = "slot_" + std::to_string(slot);
    const fs::path state_path = states_dir / (base + ".state");
    std::error_code ec;
    p.exists = fs::is_regular_file(state_path, ec);
    if (!p.exists) return p;

    const auto sz = fs::file_size(state_path, ec);
    if (!ec) {
        char buf[32];
        if (sz >= 1024u * 1024u) {
            std::snprintf(buf, sizeof buf, "%.2f MB", static_cast<double>(sz) / (1024.0 * 1024.0));
        } else {
            std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(sz) / 1024.0);
        }
        p.size_text = buf;
    }

    const std::string meta = read_text_file(states_dir / (base + ".meta"));
    std::size_t pos = 0;
    while (pos < meta.size()) {
        std::size_t nl = meta.find('\n', pos);
        std::string line = (nl == std::string::npos) ? meta.substr(pos) : meta.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos) {
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if (k == "timestamp") p.timestamp = v;
            else if (k == "area") p.area = v;
            else if (k == "disc") p.disc = std::atoi(v.c_str());
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::ifstream tf(states_dir / (base + ".thumb"), std::ios::binary);
    if (tf.is_open()) {
        char hdr[16] = {};
        if (tf.read(hdr, 16) && std::memcmp(hdr, "PS2THB1\0", 8) == 0) {
            std::uint32_t tw = 0, th = 0;
            std::memcpy(&tw, hdr + 8, 4);
            std::memcpy(&th, hdr + 12, 4);
            if (tw > 0 && th > 0 && tw <= 640 && th <= 480) {
                const std::size_t rgb_bytes = static_cast<std::size_t>(tw) * th * 3u;
                std::vector<std::uint8_t> rgb(rgb_bytes);
                if (tf.read(reinterpret_cast<char *>(rgb.data()), static_cast<std::streamsize>(rgb_bytes))) {
                    p.thumb_w = static_cast<int>(tw);
                    p.thumb_h = static_cast<int>(th);
                    p.thumb_rgba.resize(static_cast<std::size_t>(tw) * th);
                    for (std::size_t i = 0; i < p.thumb_rgba.size(); ++i) {
                        p.thumb_rgba[i] = ui_draw::rgba(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2], 255);
                    }
                }
            }
        }
    }
    return p;
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

constexpr int kPsxTabCount = 5;
constexpr int kPsxMaxVisibleRows = 8;

constexpr int kPsxBindActions[] = {
    keymap::Cross, keymap::Circle, keymap::Triangle, keymap::Square,
    keymap::DpadUp, keymap::DpadDown, keymap::DpadLeft, keymap::DpadRight,
    keymap::LeftUp, keymap::LeftDown, keymap::LeftLeft, keymap::LeftRight,
    keymap::L1, keymap::R1, keymap::L2, keymap::R2,
    keymap::Start, keymap::Select,
};
constexpr int kPsxBindCount = static_cast<int>(std::size(kPsxBindActions));
constexpr int kPsxControlsRowCount = kPsxBindCount + 1;

int psx_tab_row_count(int tab) noexcept {
    switch (tab) {
    case 0: return 8;                    // Video
    case 1: return 5;                    // Audio
    case 2: return 5;                    // Save States
    case 3: return kPsxControlsRowCount; // Controls (scrollable)
    case 4: return 5;                    // Disc & System
    default: return 8;
    }
}

bool psx_row_is_stepper(int tab, int row) noexcept {
    if (tab == 0) return true;        // All 8 Video rows support < / >
    if (tab == 1) return true;        // All 5 Audio rows support < / >
    if (tab == 2) return (row == 0);  // Active Save Slot
    if (tab == 3) return false;       // Controls rows capture keys / reset defaults
    if (tab == 4) return (row <= 2);  // Active Disc, Turbo toggle, Turbo speed
    return false;
}

struct PsxMenuLayout {
    UiRect modal;
    UiRect close_btn;
    UiRect tabs[kPsxTabCount];
    UiRect rows[kPsxMaxVisibleRows];
    UiRect left_btn[kPsxMaxVisibleRows];
    UiRect right_btn[kPsxMaxVisibleRows];
    UiRect preview_card;
    int row_count = 0;      // visible row count (<= kPsxMaxVisibleRows)
    int total_rows = 0;     // total rows in this tab
    int scroll_offset = 0;  // first visible row index
};

PsxMenuLayout compute_psx_menu_layout(int sw, int sh, int tab, int scroll_offset = 0) {
    PsxMenuLayout L{};
    L.total_rows = psx_tab_row_count(tab);
    L.row_count = std::min(L.total_rows, kPsxMaxVisibleRows);
    const int max_scroll = std::max(0, L.total_rows - L.row_count);
    L.scroll_offset = std::clamp(scroll_offset, 0, max_scroll);

    const int mw = std::clamp(sw - 48, 300, 740);
    const int mh = std::clamp(sh - 40, 260, 560);
    const int mx = (sw - mw) / 2;
    const int my = (sh - mh) / 2;
    L.modal = {mx, my, mw, mh};
    L.close_btn = {mx + mw - 102, my + 14, 84, 30};

    const int tab_y = my + 64;
    const int tab_h = 34;
    const int tab_gap = 6;
    const int tab_total_w = mw - 36;
    const int tab_w = (tab_total_w - tab_gap * (kPsxTabCount - 1)) / kPsxTabCount;
    for (int t = 0; t < kPsxTabCount; ++t) {
        L.tabs[t] = {mx + 18 + t * (tab_w + tab_gap), tab_y, tab_w, tab_h};
    }

    const int row_y0 = tab_y + tab_h + 14;
    const int footer_y = my + mh - 36;
    int avail_h = std::max(120, footer_y - row_y0 - 8);
    // On Save States tab, reserve room below the 5 action rows for the 160x112 slot preview card
    const int preview_reserve = (tab == 2 && avail_h >= 270) ? 132 : 0;
    const int rows_avail_h = std::max(120, avail_h - preview_reserve);
    const int row_h = std::clamp(rows_avail_h / std::max(1, L.row_count), 32, (tab == 2 && preview_reserve > 0) ? 40 : 46);
    const int btn_w = 34;
    const int val_span = std::min(250, mw / 2 - 20);
    for (int i = 0; i < L.row_count; ++i) {
        const int ry = row_y0 + i * row_h;
        L.rows[i] = {mx + 18, ry, mw - 36, row_h - 5};
        const int abs_row = L.scroll_offset + i;
        if (psx_row_is_stepper(tab, abs_row)) {
            const int rx_end = mx + mw - 24;
            L.right_btn[i] = {rx_end - btn_w, ry + 3, btn_w, row_h - 11};
            L.left_btn[i] = {rx_end - val_span, ry + 3, btn_w, row_h - 11};
        }
    }
    if (tab == 2 && preview_reserve > 0) {
        const int py = row_y0 + L.row_count * row_h + 6;
        const int ph = std::max(96, footer_y - py - 6);
        L.preview_card = {mx + 18, py, mw - 36, ph};
    }
    return L;
}

void draw_quick_menu_overlay(ui_draw::Surface &surf, int tab, int cursor, int scroll_offset,
                             int hover_tab, int hover_row, bool hover_close, int capturing_key_row,
                             const keymap::Config &cfg, const psx::System &sys,
                             const psx::Gpu &gpu, bool turbo_toggle, const PsxSlotPreview &slot_prev,
                             const std::string &toast_msg) {
    using namespace ui_draw;
    const int sw = surf.width;
    const int sh = surf.height;

    // Dark translucent Gothic backdrop
    surf.fill_rect(0, 0, sw, sh, rgba(8, 6, 12, 195));

    const PsxMenuLayout L = compute_psx_menu_layout(sw, sh, tab, scroll_offset);
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
        "VIDEO", "AUDIO", "SAVE STATES", "CONTROLS", "DISC & SYSTEM"
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
        const auto tm = measure_text(FontStyle::Sans, 11, true, kTabNames[t]);
        draw_text(surf, tr.x + (tr.w - tm.width) / 2, tr.y + (tr.h - 15) / 2,
                  FontStyle::Sans, 11, true, kTabNames[t],
                  active ? rgba(255, 238, 195) : rgba(190, 182, 200));
    }

    const char *aspect_names[3] = {"4:3 Original", "16:9 Widescreen", "Stretch"};
    std::string labels[kPsxMaxVisibleRows];
    std::string vals[kPsxMaxVisibleRows];
    char buf[96];

    if (tab == 0) {
        labels[0] = "Internal Resolution Scale";
        labels[1] = "Aspect Ratio";
        labels[2] = "Display Mode";
        labels[3] = "Upscale Filter (2D / Output)";
        labels[4] = "Brightness";
        labels[5] = "Contrast";
        labels[6] = "CRT Scanlines";
        labels[7] = "Performance HUD (F10)";
        std::snprintf(buf, sizeof buf, "%dx (%dx%d)", gpu.scale, 320 * gpu.scale, 240 * gpu.scale); vals[0] = buf;
        vals[1] = aspect_names[std::clamp(cfg.aspect, 0, 2)];
        vals[2] = cfg.fullscreen ? "Fullscreen" : "Windowed";
        vals[3] = cfg.sharp_2d <= 0 ? "Smooth (Linear)" : "Crisp (Nearest)";
        std::snprintf(buf, sizeof buf, "%+d%%", cfg.brightness); vals[4] = buf;
        std::snprintf(buf, sizeof buf, "%d%%", cfg.contrast); vals[5] = buf;
        if (cfg.scanlines == 0) vals[6] = "Off";
        else { std::snprintf(buf, sizeof buf, "%d%%", cfg.scanlines); vals[6] = buf; }
        vals[7] = cfg.perf_hud ? "On" : "Off";
    } else if (tab == 1) {
        labels[0] = "Master Volume";
        labels[1] = "Mute Audio";
        labels[2] = "BGM / FMV (XA) Volume";
        labels[3] = "SFX / SPU Voice Volume";
        labels[4] = "Audio Time-Stretch";
        std::snprintf(buf, sizeof buf, "%d%%", cfg.master_volume); vals[0] = buf;
        vals[1] = cfg.mute ? "Muted" : "Off";
        std::snprintf(buf, sizeof buf, "%d%%", cfg.bgm_volume); vals[2] = buf;
        std::snprintf(buf, sizeof buf, "%d%%", cfg.sfx_volume); vals[3] = buf;
        vals[4] = cfg.audio_stretch != 0 ? "On" : "Off";
    } else if (tab == 2) {
        labels[0] = "Active Save Slot";
        labels[1] = "Save State to Active Slot (F5)";
        labels[2] = "Load State from Active Slot (F8)";
        labels[3] = "Take Clean Screenshot (F12)";
        labels[4] = "Flush PS1 Memory Card Now";
        std::snprintf(buf, sizeof buf, "Slot %d (%s)", cfg.save_slot, slot_prev.exists ? "Saved" : "Empty"); vals[0] = buf;
        std::snprintf(buf, sizeof buf, "slot_%d.state", cfg.save_slot); vals[1] = buf;
        vals[2] = slot_prev.exists ? "Load & Resume" : "Empty";
        vals[3] = "PNG Capture";
        vals[4] = "memcard.mcr";
    } else if (tab == 3) {
        for (int i = 0; i < L.row_count; ++i) {
            const int abs_row = L.scroll_offset + i;
            if (abs_row < kPsxBindCount) {
                const int act = kPsxBindActions[abs_row];
                labels[i] = keymap::action_label(act);
                if (capturing_key_row == abs_row) {
                    vals[i] = "Press a key... (Esc cancels)";
                } else {
                    vals[i] = keymap::keys_text(cfg, act);
                }
            } else {
                labels[i] = "Restore Default Keyboard Bindings";
                vals[i] = "Reset keys.ini";
            }
        }
    } else {
        const int ad = sys.cdrom.active_disc();
        const int tm = std::clamp(cfg.turbo_speed > 0 ? cfg.turbo_speed : 3, 2, 4);
        labels[0] = "Active Disc (1-4)";
        labels[1] = "Turbo / Fast-Forward (F3)";
        labels[2] = "Turbo Speed Multiplier";
        labels[3] = "Resume Game";
        labels[4] = "Save Memcard & Quit";
        std::snprintf(buf, sizeof buf, "Disc %d (SLUS-01%s)", ad,
                      ad == 1 ? "051" : (ad == 2 ? "100" : (ad == 3 ? "101" : "102")));
        vals[0] = buf;
        vals[1] = turbo_toggle ? "On" : "Off";
        std::snprintf(buf, sizeof buf, "%dx", tm); vals[2] = buf;
        vals[3] = "F1 / Esc";
        vals[4] = "Exit";
    }

    for (int i = 0; i < L.row_count; ++i) {
        const UiRect &rr = L.rows[i];
        const int abs_row = L.scroll_offset + i;
        const bool sel = (abs_row == cursor);
        const bool hov = (i == hover_row);
        const bool cap = (tab == 3 && capturing_key_row == abs_row);
        if (cap) {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(132, 82, 28, 235));
            surf.stroke_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, 1.5f, rgba(255, 224, 130, 250));
        } else if (sel) {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(96, 34, 50, 220));
            surf.stroke_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, 1.25f, rgba(226, 182, 108, 235));
        } else if (hov) {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(52, 34, 46, 180));
            surf.stroke_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, 1.0f, rgba(165, 130, 82, 150));
        } else {
            surf.fill_rounded_rect(rr.x, rr.y, rr.w, rr.h, 7.0f, rgba(24, 18, 30, 140));
        }

        const int ty = rr.y + (rr.h - 17) / 2;
        draw_text(surf, rr.x + 14, ty, FontStyle::Sans, 14, sel || cap,
                  labels[i], (sel || cap) ? rgba(255, 240, 205) : rgba(218, 214, 224));

        if (psx_row_is_stepper(tab, abs_row)) {
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
                      vals[i], cap ? rgba(255, 245, 200) : (sel ? rgba(248, 212, 132) : rgba(175, 165, 186)));
        }
    }

    // Subtle scroll bar for Controls tab
    if (tab == 3 && L.total_rows > L.row_count && L.row_count > 0) {
        const int sb_x = mx + mw - 12;
        const int sb_y0 = L.rows[0].y;
        const int sb_y1 = L.rows[L.row_count - 1].y + L.rows[L.row_count - 1].h;
        const int sb_h = std::max(24, sb_y1 - sb_y0);
        surf.fill_rounded_rect(sb_x, sb_y0, 5, sb_h, 2.5f, rgba(44, 34, 52, 180));
        const int thumb_h = std::max(24, (sb_h * L.row_count) / L.total_rows);
        const int max_s = std::max(1, L.total_rows - L.row_count);
        const int thumb_y = sb_y0 + ((sb_h - thumb_h) * L.scroll_offset) / max_s;
        surf.fill_rounded_rect(sb_x, thumb_y, 5, thumb_h, 2.5f, rgba(215, 174, 102, 220));
    }

    // Save-slot thumbnail + metadata preview card on SAVE STATES tab
    if (tab == 2 && L.preview_card.h >= 90) {
        const UiRect &pc = L.preview_card;
        surf.fill_rounded_rect(pc.x, pc.y, pc.w, pc.h, 8.0f, rgba(18, 13, 24, 215));
        surf.stroke_rounded_rect(pc.x, pc.y, pc.w, pc.h, 8.0f, 1.0f, rgba(155, 122, 74, 175));

        const int pad = 10;
        const int th_h = std::min(112, pc.h - pad * 2);
        const int th_w = std::min(160, (th_h * 160) / 112);
        const int th_x = pc.x + pad;
        const int th_y = pc.y + (pc.h - th_h) / 2;

        if (slot_prev.exists && !slot_prev.thumb_rgba.empty() && slot_prev.thumb_w > 0 && slot_prev.thumb_h > 0) {
            surf.blit_rgba_fit(th_x, th_y, th_w, th_h,
                               slot_prev.thumb_rgba.data(), slot_prev.thumb_w, slot_prev.thumb_h, 5.0f);
            surf.stroke_rounded_rect(th_x, th_y, th_w, th_h, 5.0f, 1.0f, rgba(210, 170, 98, 210));
        } else {
            surf.fill_rounded_rect(th_x, th_y, th_w, th_h, 5.0f, rgba(28, 22, 36, 220));
            surf.stroke_rounded_rect(th_x, th_y, th_w, th_h, 5.0f, 1.0f, rgba(105, 86, 66, 140));
            const std::string ph_txt = slot_prev.exists ? "No Preview" : "Empty Slot";
            const auto pm = measure_text(FontStyle::Sans, 12, false, ph_txt);
            draw_text(surf, th_x + (th_w - pm.width) / 2, th_y + (th_h - 15) / 2,
                      FontStyle::Sans, 12, false, ph_txt, rgba(140, 132, 152));
        }

        const int tx = th_x + th_w + 16;
        int ty = pc.y + 14;
        char hdr[96];
        std::snprintf(hdr, sizeof hdr, "Slot %d  •  %s",
                      cfg.save_slot, slot_prev.exists ? "Saved State" : "Empty");
        draw_text(surf, tx, ty, FontStyle::Serif, 16, true, hdr,
                  slot_prev.exists ? rgba(246, 214, 142) : rgba(168, 158, 180));
        ty += 24;
        if (slot_prev.exists) {
            const std::string area_line = !slot_prev.area.empty()
                ? slot_prev.area
                : (slot_prev.disc > 0 ? ("Nemeton Monastery (Disc " + std::to_string(slot_prev.disc) + ")")
                                      : "Koudelka Save State");
            draw_text(surf, tx, ty, FontStyle::Sans, 13, false, area_line, rgba(224, 216, 232));
            ty += 20;
            std::string detail;
            if (!slot_prev.timestamp.empty()) detail = slot_prev.timestamp;
            if (!slot_prev.size_text.empty()) {
                if (!detail.empty()) detail += "  •  ";
                detail += slot_prev.size_text;
            }
            if (!detail.empty()) {
                draw_text(surf, tx, ty, FontStyle::Sans, 12, false, detail, rgba(176, 166, 190));
                ty += 20;
            }
            draw_text(surf, tx, ty, FontStyle::Sans, 11, false,
                      "F5: Overwrite  •  F8: Load  •  F6/F7: Switch Slot", rgba(145, 138, 162));
        } else {
            draw_text(surf, tx, ty, FontStyle::Sans, 13, false,
                      "Press F5 or click Save State above to capture this slot.", rgba(182, 174, 196));
            ty += 22;
            draw_text(surf, tx, ty, FontStyle::Sans, 11, false,
                      "Saves include a 160x112 preview shared with the Nocturne Launcher.", rgba(145, 138, 162));
        }
    }

    std::string footer = toast_msg;
    if (footer.empty()) {
        if (tab == 3 && capturing_key_row >= 0) {
            footer = "Listening for keyboard key...  •  Press Esc to cancel  •  Reserved keys (F1/F2/F5/F8/F11) cannot be bound";
        } else if (tab == 3) {
            char fbuf[160];
            std::snprintf(fbuf, sizeof fbuf,
                          "Showing %d-%d of %d  •  Click or Enter to Rebind  •  Wheel / Arrows to Scroll  •  F1/Esc: Resume",
                          L.scroll_offset + 1, L.scroll_offset + L.row_count, L.total_rows);
            footer = fbuf;
        } else {
            footer = "Click / Tap  •  Q/E or L1/R1: Tabs  •  Arrows/D-Pad: Adjust  •  Enter/Cross: Confirm  •  F1/Esc: Resume";
        }
    }
    draw_text(surf, mx + 22, my + mh - 28, FontStyle::Sans, 11, !toast_msg.empty() || capturing_key_row >= 0,
              footer, (!toast_msg.empty() || capturing_key_row >= 0) ? rgba(248, 214, 128) : rgba(158, 150, 170));
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
    fs::path memcard_arg;
    int start_disc = 1;
    int cli_scale = 0;
    int cli_fullscreen = -1;  // -1 = keys.ini default, 0 = windowed, 1 = fullscreen
    int max_frames = 0;
    int telemetry_interval = 0;
    bool headless = false;
    bool no_aot = false;
    std::string dump_frame_path;
    std::string load_state_arg;
    std::string load_slot_arg;
    std::string save_state_arg;
    std::string input_script_arg;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--memcard") == 0 && i + 1 < argc) {
            memcard_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            const char *val = argv[++i];
            if (val[0] >= '1' && val[0] <= '4' && val[1] == '\0') {
                start_disc = val[0] - '0';
            }
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            cli_scale = std::clamp(std::atoi(argv[++i]), 1, 4);
        } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
            cli_fullscreen = 1;
        } else if (std::strcmp(argv[i], "--window") == 0) {
            cli_fullscreen = 0;
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
        } else if (std::strcmp(argv[i], "--load-slot") == 0 && i + 1 < argc) {
            load_slot_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--save-state") == 0 && i + 1 < argc) {
            save_state_arg = argv[++i];
        }
    }

    const fs::path states_dir = game_dir / "states";
    migrate_legacy_states(game_dir, states_dir);

#ifdef _WIN32
    // Ensure direct launches (outside Shadow Hearts Nocturne.exe) still save F12 screenshots
    // into the Koudelka subfolder with the Koudelka_ filename prefix so the Launcher finds them.
    if (!std::getenv("PS2_SCREENSHOT_PREFIX")) {
        SetEnvironmentVariableA("PS2_SCREENSHOT_PREFIX", "Koudelka");
    }
    if (!std::getenv("PS2_SCREENSHOTS_DIR")) {
        if (const char *prof = std::getenv("USERPROFILE"); prof && *prof) {
            const fs::path shots = fs::path(prof) / "Pictures" / "Shadow Hearts Nocturne" / "Koudelka";
            SetEnvironmentVariableW(L"PS2_SCREENSHOTS_DIR", shots.wstring().c_str());
        }
    }
#endif

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
    if (cli_fullscreen >= 0) cfg.fullscreen = cli_fullscreen;
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
    spu.set_irq_callback([&]() { sys.hw.raise_irq(sys, psx::kIrqSpu); });

    // Hook XA-ADPCM sectors from CD-ROM directly into SPU1 XA decoder
    std::uint64_t xa_sectors_total = 0;
    sys.cdrom.set_xa_callback([&](const std::uint8_t *raw2352, std::uint8_t, std::uint8_t) {
        ++xa_sectors_total;
        spu.decode_xa_sector(raw2352);
    });

    // Load 128 KB PS1 memory card (memcard.mcr)
    const fs::path memcard_path = memcard_arg.empty() ? (game_dir / "memcard.mcr") : memcard_arg;
    sys.hw.memcards[0].load(memcard_path.string());

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

    std::string startup_toast;
    if (load_state_arg.empty() && !load_slot_arg.empty()) {
        if (load_slot_arg == "auto" || load_slot_arg == "autosave") {
            load_state_arg = (states_dir / "autosave.state").string();
            startup_toast = "Loaded Autosave";
        } else {
            const int s = std::clamp(std::atoi(load_slot_arg.c_str()), 0, 9);
            cfg.save_slot = s;
            load_state_arg = (states_dir / ("slot_" + std::to_string(s) + ".state")).string();
            startup_toast = "Loaded State from Slot " + std::to_string(s);
        }
    }
    if (!load_state_arg.empty()) {
        if (!psx::load_state_file(load_state_arg, sys, gpu, mdec, spu)) {
            startup_toast.clear();
        }
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
#ifdef _WIN32
    struct WinTimerPeriodGuard {
        WinTimerPeriodGuard() { timeBeginPeriod(1); }
        ~WinTimerPeriodGuard() { timeEndPeriod(1); }
    } timer_period_guard;
#endif
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
    int controls_scroll = 0;
    int capturing_key_row = -1;
    int hover_tab = -1;
    int hover_row = -1;
    bool hover_close = false;
    bool turbo_toggle = false;
    bool prev_combo = false;
    std::string toast_msg;
    double toast_until = 0.0;

    fs::file_time_type keys_mtime{};
    bool has_keys_mtime = false;
    auto record_keys_mtime = [&]() {
        std::error_code ec;
        const auto mt = fs::last_write_time(keys_path, ec);
        if (!ec) {
            keys_mtime = mt;
            has_keys_mtime = true;
        }
    };
    record_keys_mtime();

    PsxSlotPreview slot_preview;
    auto invalidate_slot_preview = [&]() {
        slot_preview.loaded_slot = -1;
    };
    auto ensure_slot_preview = [&]() {
        if (slot_preview.loaded_slot != cfg.save_slot) {
            slot_preview = load_psx_slot_preview(states_dir, cfg.save_slot);
        }
    };

    auto sync_controls_scroll = [&]() {
        if (menu_tab != 3) {
            controls_scroll = 0;
            return;
        }
        const int max_s = std::max(0, kPsxControlsRowCount - kPsxMaxVisibleRows);
        if (menu_cursor < controls_scroll) controls_scroll = menu_cursor;
        else if (menu_cursor >= controls_scroll + kPsxMaxVisibleRows) {
            controls_scroll = menu_cursor - kPsxMaxVisibleRows + 1;
        }
        controls_scroll = std::clamp(controls_scroll, 0, max_s);
    };

    auto set_menu_open = [&](bool open) {
        menu_open = open;
        capturing_key_row = -1;
        sound_pipe.set_paused(menu_open);
        if (menu_open) invalidate_slot_preview();
    };

    auto show_toast = [&](const std::string &msg) {
        toast_msg = msg;
        toast_until = steady_seconds() + 2.8;
    };
    if (!startup_toast.empty()) {
        show_toast(startup_toast);
    }

    auto slot_path = [&](int slot) -> std::string {
        return (states_dir / ("slot_" + std::to_string(std::clamp(slot, 0, 9)) + ".state")).string();
    };

    auto persist_keys = [&]() {
        write_text_file(keys_path, keys_text);
        record_keys_mtime();
    };

    auto reload_keys_if_modified = [&]() {
        std::error_code ec;
        const auto mt = fs::last_write_time(keys_path, ec);
        if (ec || (has_keys_mtime && mt == keys_mtime)) return;
        const std::string fresh_text = read_text_file(keys_path);
        if (fresh_text.empty()) return;

        keymap::Config fresh = keymap::defaults();
        std::vector<std::string> reload_problems;
        keymap::parse(fresh_text, fresh, reload_problems);
        keys_text = fresh_text;
        cfg = fresh;
        keys_mtime = mt;
        has_keys_mtime = true;

        if (cfg.scale >= 1 && cfg.scale <= 4 && cfg.scale != gpu.scale) {
            gpu.set_scale(cfg.scale);
        }
        gpu.sharp_3d = (cfg.sharp_3d != 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, cfg.sharp_2d <= 0 ? "linear" : "nearest");
        tex_filter_dirty = true;
        sound_pipe.set_volume(std::clamp(cfg.master_volume, 0, 100) / 100.0f);
        sound_pipe.set_muted(cfg.mute != 0);
        sound_pipe.set_stretch_enabled(cfg.audio_stretch != 0);
        spu.bgm_gain = std::clamp(cfg.bgm_volume, 0, 100) / 100.0f;
        spu.sfx_gain = std::clamp(cfg.sfx_volume, 0, 100) / 100.0f;
        invalidate_slot_preview();
        show_toast("Reloaded keys.ini");
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
                invalidate_slot_preview();
            }
        } else if (tab == 4) {  // DISC & SYSTEM
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
                if (save_koudelka_slot(states_dir, cfg.save_slot, sys, gpu, mdec, spu)) {
                    invalidate_slot_preview();
                    show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                } else {
                    show_toast("Save State Failed");
                }
            } else if (row == 2) {
                if (psx::load_state_file(slot_path(cfg.save_slot), sys, gpu, mdec, spu)) {
                    sound_pipe.flush();
                    show_toast("Loaded State from Slot " + std::to_string(cfg.save_slot));
                    set_menu_open(false);
                } else {
                    show_toast("Slot " + std::to_string(cfg.save_slot) + " is Empty");
                }
            } else if (row == 3) {
                take_clean_screenshot();
            } else if (row == 4) {
                if (sys.hw.memcards[0].save()) show_toast("Saved PS1 Memory Card (memcard.mcr)");
                else show_toast("Memory Card Save Failed");
            }
        } else if (tab == 3) {  // CONTROLS
            if (row >= 0 && row < kPsxBindCount) {
                capturing_key_row = row;
                show_toast(std::string("Press a key for ") + keymap::action_label(kPsxBindActions[row]) + " (Esc cancels)");
            } else if (row == kPsxBindCount) {
                capturing_key_row = -1;
                std::vector<int> changed;
                keymap::restore_default_inputs(cfg, changed);
                for (int act : changed) {
                    keys_text = keymap::with_inputs(keys_text, act, cfg.inputs[act]);
                }
                persist_keys();
                show_toast("Restored Default Keyboard Bindings");
            }
        } else if (tab == 4) {  // DISC & SYSTEM
            if (row <= 2) {
                step_menu_item(tab, row, 1);
            } else if (row == 3) {
                set_menu_open(false);
            } else if (row == 4) {
                sys.hw.memcards[0].save();
                running = false;
            }
        }
    };

    auto switch_menu_tab = [&](int new_tab) {
        menu_tab = ((new_tab % kPsxTabCount) + kPsxTabCount) % kPsxTabCount;
        menu_cursor = 0;
        controls_scroll = 0;
        capturing_key_row = -1;
        if (menu_tab == 2) invalidate_slot_preview();
    };

    auto handle_menu_pointer_hover = [&](int ux, int uy) {
        int uw = 960, uh = 720;
        get_ui_size(uw, uh);
        const PsxMenuLayout L = compute_psx_menu_layout(uw, uh, menu_tab, controls_scroll);
        hover_close = L.close_btn.contains(ux, uy);
        hover_tab = -1;
        for (int t = 0; t < kPsxTabCount; ++t) {
            if (L.tabs[t].contains(ux, uy)) { hover_tab = t; break; }
        }
        hover_row = -1;
        for (int i = 0; i < L.row_count; ++i) {
            if (L.rows[i].contains(ux, uy)) {
                hover_row = i;
                if (capturing_key_row < 0) menu_cursor = L.scroll_offset + i;
                break;
            }
        }
    };

    auto handle_menu_pointer_click = [&](int ux, int uy) {
        int uw = 960, uh = 720;
        get_ui_size(uw, uh);
        const PsxMenuLayout L = compute_psx_menu_layout(uw, uh, menu_tab, controls_scroll);
        if (!L.modal.contains(ux, uy) || L.close_btn.contains(ux, uy)) {
            set_menu_open(false);
            return;
        }
        for (int t = 0; t < kPsxTabCount; ++t) {
            if (L.tabs[t].contains(ux, uy)) {
                switch_menu_tab(t);
                return;
            }
        }
        for (int i = 0; i < L.row_count; ++i) {
            if (L.rows[i].contains(ux, uy)) {
                const int abs_row = L.scroll_offset + i;
                menu_cursor = abs_row;
                if (psx_row_is_stepper(menu_tab, abs_row)) {
                    if (L.left_btn[i].contains(ux, uy)) {
                        step_menu_item(menu_tab, abs_row, -1);
                        return;
                    }
                    if (L.right_btn[i].contains(ux, uy)) {
                        step_menu_item(menu_tab, abs_row, 1);
                        return;
                    }
                }
                activate_menu_item(menu_tab, abs_row);
                return;
            }
        }
    };

    double fps_smooth = 60.0;
    double last_frame_t = steady_seconds();
    double next_frame_deadline = last_frame_t;
    int last_disc_shown = -1;

    while (running && !sys.cpu.halted) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                reload_keys_if_modified();
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
            } else if (ev.type == SDL_MOUSEWHEEL && menu_open && capturing_key_row < 0) {
                const int rc = psx_tab_row_count(menu_tab);
                if (menu_tab == 3) {
                    const int max_s = std::max(0, rc - kPsxMaxVisibleRows);
                    if (ev.wheel.y > 0) {
                        controls_scroll = std::max(0, controls_scroll - 1);
                        menu_cursor = std::max(0, menu_cursor - 1);
                    } else if (ev.wheel.y < 0) {
                        controls_scroll = std::min(max_s, controls_scroll + 1);
                        menu_cursor = std::min(rc - 1, menu_cursor + 1);
                    }
                    sync_controls_scroll();
                } else {
                    if (ev.wheel.y > 0) menu_cursor = (menu_cursor + rc - 1) % rc;
                    else if (ev.wheel.y < 0) menu_cursor = (menu_cursor + 1) % rc;
                }
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
                    set_menu_open(!menu_open);
                } else if (menu_open) {
                    if (capturing_key_row >= 0) {
                        if (btn == SDL_CONTROLLER_BUTTON_B) {
                            capturing_key_row = -1;
                            show_toast("Rebind Cancelled");
                        }
                    } else {
                        const int rc = psx_tab_row_count(menu_tab);
                        if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                            menu_cursor = (menu_cursor + rc - 1) % rc;
                            sync_controls_scroll();
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                            menu_cursor = (menu_cursor + 1) % rc;
                            sync_controls_scroll();
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                            step_menu_item(menu_tab, menu_cursor, -1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                            step_menu_item(menu_tab, menu_cursor, 1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                            switch_menu_tab(menu_tab - 1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                            switch_menu_tab(menu_tab + 1);
                        } else if (btn == SDL_CONTROLLER_BUTTON_A) {
                            activate_menu_item(menu_tab, menu_cursor);
                        } else if (btn == SDL_CONTROLLER_BUTTON_B) {
                            set_menu_open(false);
                        }
                    }
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                const SDL_Keycode sym = ev.key.keysym.sym;
                const bool alt = (ev.key.keysym.mod & KMOD_ALT) != 0;

                if (menu_open && capturing_key_row >= 0) {
                    if (sym == SDLK_ESCAPE) {
                        capturing_key_row = -1;
                        show_toast("Rebind Cancelled");
                    } else {
                        const int act = kPsxBindActions[capturing_key_row];
                        std::vector<int> changed;
                        std::string msg;
                        if (keymap::assign_input(cfg, act, static_cast<int>(ev.key.keysym.scancode), changed, msg)) {
                            for (int ch : changed) {
                                keys_text = keymap::with_inputs(keys_text, ch, cfg.inputs[ch]);
                            }
                            persist_keys();
                            capturing_key_row = -1;
                            show_toast(msg);
                        } else {
                            show_toast(msg);
                        }
                    }
                } else if (sym == SDLK_F1 || (menu_open && sym == SDLK_ESCAPE)) {
                    set_menu_open(!menu_open);
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
                    if (save_koudelka_slot(states_dir, cfg.save_slot, sys, gpu, mdec, spu)) {
                        invalidate_slot_preview();
                        show_toast("Saved State to Slot " + std::to_string(cfg.save_slot));
                    } else {
                        show_toast("Save State Failed");
                    }
                } else if (sym == SDLK_F6) {
                    cfg.save_slot = (cfg.save_slot + 9) % 10;
                    keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                    persist_keys();
                    invalidate_slot_preview();
                    show_toast("Save Slot " + std::to_string(cfg.save_slot));
                } else if (sym == SDLK_F7) {
                    cfg.save_slot = (cfg.save_slot + 1) % 10;
                    keys_text = keymap::with_setting(keys_text, "save_slot", std::to_string(cfg.save_slot));
                    persist_keys();
                    invalidate_slot_preview();
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
                        sync_controls_scroll();
                    } else if (sym == SDLK_DOWN) {
                        menu_cursor = (menu_cursor + 1) % rc;
                        sync_controls_scroll();
                    } else if (sym == SDLK_q) {
                        switch_menu_tab(menu_tab - 1);
                    } else if (sym == SDLK_e || sym == SDLK_TAB) {
                        switch_menu_tab(menu_tab + 1);
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
            set_menu_open(!menu_open);
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
                    if (menu_tab == 2) ensure_slot_preview();
                    draw_quick_menu_overlay(ui_surf, menu_tab, menu_cursor, controls_scroll,
                                            hover_tab, hover_row, hover_close, capturing_key_row,
                                            cfg, sys, gpu, turbo_toggle, slot_preview,
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

        // Frame pacing (~60.0 Hz with 1 ms Windows timer resolution + SoundPipe wait accounting
        // so voice/BGM audio never starves or triggers unwanted WSOLA warble during 3D cutscenes).
        if (!turbo_active && !menu_open) {
            const double wait_t0 = steady_seconds();
            if (audio_dev != 0) {
                int guard = 0;
                while (sound_pipe.full() && guard++ < 80) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            constexpr double kTargetFrameSec = 1.0 / 60.0;
            next_frame_deadline += kTargetFrameSec;
            const double now_after_audio = steady_seconds();
            if (now_after_audio < next_frame_deadline) {
                const double rem = next_frame_deadline - now_after_audio;
                if (rem > 0.0015) {
                    std::this_thread::sleep_for(std::chrono::duration<double>(rem - 0.001));
                }
                while (steady_seconds() < next_frame_deadline) {
                    std::this_thread::yield();
                }
            } else if (now_after_audio - next_frame_deadline > 0.1) {
                next_frame_deadline = now_after_audio;
            }
            sound_pipe.note_wait(steady_seconds() - wait_t0);
        } else {
            next_frame_deadline = steady_seconds();
            if (menu_open) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
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
