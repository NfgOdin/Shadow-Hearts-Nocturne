#pragma once

// Standalone Native C++ Win32 + Direct2D / DirectWrite / WIC / DWM desktop launcher
// for Shadow Hearts: Nocturne (launches Shadow Hearts Covenant.exe).

#include "launcher_audio.hpp"
#include "launcher_data.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#endif

namespace launcher {

enum class Page : int {
    Home = 0,
    Settings = 1,
    Trophies = 2,
    SavesMedia = 3,
    Tools = 4,
    Count = 5
};

class LauncherApp {
public:
    explicit LauncherApp(const std::filesystem::path &root_dir);
    ~LauncherApp();
    LauncherApp(const LauncherApp &) = delete;
    LauncherApp &operator=(const LauncherApp &) = delete;

    // Interactive desktop lifecycle
    int run(HINSTANCE hinst, int show_cmd);

    // Programmatic page control & headless Direct2D-to-PNG screenshot rendering (used by automated tests)
    void set_page(Page p);
    void set_trophy_tier(int tier_filter);
    void set_reveal_secrets(bool show);
    void set_saves_subtab(int subtab);  // 0 = Save States, 1 = Screenshots
    LauncherState &state() { return state_; }
    bool render_to_png(const std::filesystem::path &out_png, int width = 1220, int height = 740);
    bool render_stress_frames(int home_frames, int trophy_frames, int width = 1220, int height = 740);

private:
#ifdef _WIN32
    struct Palette {
        D2D1_COLOR_F bg_top{};
        D2D1_COLOR_F bg_bot{};
        D2D1_COLOR_F sidebar_bg{};
        D2D1_COLOR_F card_bg{};
        D2D1_COLOR_F card_hover{};
        D2D1_COLOR_F border{};
        D2D1_COLOR_F border_hi{};
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accent_crimson{};
        D2D1_COLOR_F text{};
        D2D1_COLOR_F text_dim{};
        D2D1_COLOR_F good{};
        D2D1_COLOR_F warn{};
    };

    struct Ember {
        float x, y, vx, vy, radius, alpha, phase;
    };

    struct HitRegion {
        std::string id;
        D2D1_RECT_F rect{};
        std::function<void()> on_click;
        std::function<void(float)> on_drag;  // normalized 0..1 for sliders/scrollbars
        bool is_slider = false;
    };

    bool init_factories();
    void release_device_resources();
    bool ensure_hwnd_target();
    void init_embers();
    Palette current_palette() const;

    // Bitmap & brush helpers (cached per RenderTarget generation)
    ID2D1Bitmap *load_bitmap_cached(ID2D1RenderTarget *rt, const std::filesystem::path &path, int max_w, int max_h);
    ID2D1Bitmap *load_slot_bitmap_cached(ID2D1RenderTarget *rt, const SaveSlot &slot);
    ID2D1SolidColorBrush *solid_brush(ID2D1RenderTarget *rt, D2D1_COLOR_F col);
    void clear_bitmap_caches();

    // Master Direct2D render pipeline (works on both HWND and offscreen WIC targets)
    void render_frame(ID2D1RenderTarget *rt, float w, float h);
    void render_background(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void render_judgment_ring(ID2D1RenderTarget *rt, float cx, float cy, float radius, float alpha, const Palette &pal);
    void render_titlebar(ID2D1RenderTarget *rt, float w, const Palette &pal);
    void render_sidebar(ID2D1RenderTarget *rt, float h, const Palette &pal);
    void render_home(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_settings(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_trophies(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_saves_media(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_tools(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_status_toast(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void render_lightbox(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void draw_medallion_d2d(ID2D1RenderTarget *rt, float cx, float cy, float r, int tier_index,
                            D2D1_COLOR_F gl, D2D1_COLOR_F gr, D2D1_COLOR_F bdr, D2D1_COLOR_F acc,
                            bool unlocked, bool masked);

    // Interactive widget helpers
    void draw_text(ID2D1RenderTarget *rt, const std::string &utf8, IDWriteTextFormat *fmt,
                   D2D1_RECT_F rect, D2D1_COLOR_F col, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, bool ellipsis = true);
    void draw_button(ID2D1RenderTarget *rt, const std::string &id, D2D1_RECT_F rect, const std::string &label,
                     D2D1_COLOR_F bg, D2D1_COLOR_F border, D2D1_COLOR_F text_col, bool primary,
                     std::function<void()> on_click);
    void draw_toggle_row(ID2D1RenderTarget *rt, const std::string &id, float x, float y, float w,
                         const std::string &label, const std::string &sub, bool value, const Palette &pal,
                         std::function<void(bool)> on_change);
    void draw_slider_row(ID2D1RenderTarget *rt, const std::string &id, float x, float y, float w,
                         const std::string &label, const std::string &val_text, float norm, const Palette &pal,
                         std::function<void(float)> on_change);
    void draw_segmented_row(ID2D1RenderTarget *rt, const std::string &id_prefix, float x, float y, float w,
                            const std::string &label, const std::vector<std::string> &opts, int active_idx,
                            const Palette &pal, std::function<void(int)> on_change);

    // Actions & Win32 message handling
    void launch_game();
    void persist_settings(const std::string &toast_msg = {});
    void show_toast(const std::string &msg);
    void open_in_explorer(const std::filesystem::path &path);
    void start_anim_timer();
    void stop_anim_timer();

    static LRESULT CALLBACK wnd_proc_static(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    ID2D1Factory *d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget *hwnd_rt_ = nullptr;
    IDWriteFactory *dwrite_factory_ = nullptr;
    IWICImagingFactory *wic_factory_ = nullptr;

    IDWriteTextFormat *fmt_hero_ = nullptr;
    IDWriteTextFormat *fmt_h1_ = nullptr;
    IDWriteTextFormat *fmt_h2_ = nullptr;
    IDWriteTextFormat *fmt_card_title_ = nullptr;
    IDWriteTextFormat *fmt_body_ = nullptr;
    IDWriteTextFormat *fmt_small_ = nullptr;
    IDWriteTextFormat *fmt_pill_ = nullptr;
    IDWriteTextFormat *fmt_play_ = nullptr;
    std::unordered_map<IDWriteTextFormat *, IDWriteInlineObject *> trim_signs_;

    ID2D1RenderTarget *bitmap_cache_rt_ = nullptr;
    ID2D1SolidColorBrush *solid_brush_ = nullptr;
    std::unordered_map<std::string, ID2D1Bitmap *> image_cache_;
    std::unordered_map<int, ID2D1Bitmap *> slot_thumb_cache_;
    std::vector<HitRegion> hits_;
    bool in_render_ = false;
#endif

    LauncherState state_;
    ChimeAudio audio_;

    Page page_ = Page::Home;
    float time_sec_ = 0.0f;
    std::vector<Ember> embers_;
    bool anim_running_ = false;
    bool game_running_ = false;

    // Interaction state
    std::string hovered_id_;
    std::string dragging_id_;
    D2D1_RECT_F dragging_rect_{};

    // Per-page scroll & filter state
    float settings_scroll_ = 0.0f;
    float settings_max_scroll_ = 0.0f;
    int trophy_tier_ = -1;       // -1 = All, 0..6 = Tier
    int trophy_status_ = 0;      // 0 = All, 1 = Unlocked, 2 = Locked
    bool reveal_secrets_ = false;
    int trophy_selected_ = 0;
    int trophy_scroll_ = 0;
    int saves_subtab_ = 0;       // 0 = Save States, 1 = Screenshots
    int armed_delete_slot_ = -1;
    float media_scroll_ = 0.0f;
    float media_max_scroll_ = 0.0f;
    std::filesystem::path lightbox_image_;
    std::string lightbox_title_;

    // Bottom status toast
    std::string status_toast_;
    float status_toast_until_ = 0.0f;
};

}  // namespace launcher
