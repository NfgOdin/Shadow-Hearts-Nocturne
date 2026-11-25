#pragma once

// Standalone Native C++ Win32 + Direct2D / DirectWrite / WIC / DWM desktop launcher
// for Shadow Hearts: Nocturne (launches Shadow Hearts Covenant.exe).

#include "launcher_audio.hpp"
#include "launcher_data.hpp"
#include "library_reader.hpp"
#include "ring_dojo.hpp"
#include "card_album.hpp"
#include "card_game.hpp"
#include "coop_net.hpp"
#include "coop_session.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
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
    Trilogy = -1,
    Home = 0,
    Settings = 1,
    Trophies = 2,
    SavesMedia = 3,
    Tools = 4,
    Coop = 5,
    RingDojo = 6,
    Library = 7,
    CardAlbum = 8,
    Count = 9
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
    void select_game(GameId id, bool enter_hub = true);
    void return_to_trilogy();
    GameId active_game() const { return active_game_; }
    GlobalLauncherConfig &global_config() { return global_cfg_; }
    void set_page(Page p);
    void set_trophy_tier(int tier_filter);
    void set_reveal_secrets(bool show);
    void set_saves_subtab(int subtab);  // 0 = Save States, 1 = Screenshots
    void set_tools_subtab(int subtab);  // 0 = System Health, 1 = HD Packs & Mods, 2 = Updates
    void set_packs_view_mode(int mode) { packs_view_mode_ = (mode >= 0 && mode <= 2) ? mode : 0; }
    void set_settings_scroll(float s) { settings_scroll_ = s; }
    void set_tools_mods_scroll(float s) { tools_mods_scroll_ = s; }
    void set_card_view_mode(int mode) { card_view_mode_ = (mode >= 0 && mode <= 2) ? mode : 0; }
    void set_card_rules_open(bool open) { card_rules_open_ = open; }
    void configure_npc_duel(const std::string &npc_id, int difficulty,
                            const std::string &ante_card_id,
                            const std::filesystem::path &result_file);
    void start_card_duel_preview(int opp_idx = 0, bool trial = true) {
        card_duel_opp_idx_ = opp_idx;
        card_duel_use_trial_ = trial;
        card_duel_.start(trial ? card_game::trial_deck_ids() : card_collection_.active_deck,
                         opp_idx, trial, card_duel_live_ring_, 1337u);
        card_duel_started_ = true;
        card_duel_selected_hand_ = -1;
        card_view_mode_ = 2;
    }
    bool any_non_titlebar_hit_in_y_band(float y0, float y1) const;
    // Gamepad / keyboard spatial navigation (also directly unit-testable)
    void gamepad_navigate(int dx, int dy);
    void gamepad_switch_page(int delta);
    bool gamepad_activate();
    void gamepad_back();
    const std::string &focused_id() const { return focused_id_; }
    Page current_page() const { return page_; }
    void set_gamepad_nav_preview(bool active, const std::string &focus_id, bool sony = false);
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
        float slider_val = 0.0f;
    };

    bool init_factories();
    void release_device_resources();
    bool ensure_hwnd_target();
    void init_embers();
    Palette current_palette() const;
    Palette palette_for_game(GameId id, int theme_override) const;

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
    void render_trilogy(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_sidebar(ID2D1RenderTarget *rt, float h, const Palette &pal);
    void render_home(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_settings(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_trophies(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_saves_media(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_tools(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_coop_lobby(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_ring_dojo(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_library(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_card_album(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_card_binder(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_card_deck_builder(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_card_duel(ID2D1RenderTarget *rt, D2D1_RECT_F area, const Palette &pal);
    void render_card_rules_modal(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void render_card_inspector(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void draw_collectible_card(ID2D1RenderTarget *rt, const cards::CardDef &def,
                               const cards::OwnedCard *owned, int display_finish,
                               D2D1_RECT_F rect, bool hovered, bool inspect_mode,
                               float tilt_x, float tilt_y, bool flipped, const Palette &pal);
    void render_reader_overlay(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void open_library_reader(const std::string &item_id);
    void close_library_reader();
    void reader_turn_page(int delta);
    std::filesystem::path pick_book_file_dialog(const std::wstring &title);
    void poll_coop_lobby();
    void trigger_ring_dojo_press();
    void launch_game_with_args(int load_slot, const std::string &extra_args, bool is_coop);
    void render_status_toast(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void render_focus_ring(ID2D1RenderTarget *rt, const Palette &pal);
    void render_lightbox(ID2D1RenderTarget *rt, float w, float h, const Palette &pal);
    void draw_medallion_d2d(ID2D1RenderTarget *rt, float cx, float cy, float r, int tier_index,
                            D2D1_COLOR_F gl, D2D1_COLOR_F gr, D2D1_COLOR_F bdr, D2D1_COLOR_F acc,
                            bool unlocked, bool masked);

    // Interactive widget helpers
    void push_hit(HitRegion hr);
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
    void launch_game(int load_slot = -1);
    void poll_gamepad();
    void ensure_valid_focus();
    void persist_settings(const std::string &toast_msg = {});
    void show_toast(const std::string &msg);
    void open_in_explorer(const std::filesystem::path &path);
    void start_update_check();
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
    IDWriteTextFormat *fmt_roman_ = nullptr;
    IDWriteTextFormat *fmt_h1_ = nullptr;
    IDWriteTextFormat *fmt_h2_ = nullptr;
    IDWriteTextFormat *fmt_card_title_ = nullptr;
    IDWriteTextFormat *fmt_card_name_ = nullptr;
    IDWriteTextFormat *fmt_card_stat_ = nullptr;
    IDWriteTextFormat *fmt_body_ = nullptr;
    IDWriteTextFormat *fmt_small_ = nullptr;
    IDWriteTextFormat *fmt_pill_ = nullptr;
    IDWriteTextFormat *fmt_play_ = nullptr;
    std::unordered_map<IDWriteTextFormat *, IDWriteInlineObject *> trim_signs_;

    ID2D1RenderTarget *bitmap_cache_rt_ = nullptr;
    ID2D1SolidColorBrush *solid_brush_ = nullptr;
    std::unordered_map<std::string, ID2D1Bitmap *> image_cache_;
    std::list<std::string> image_cache_lru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> image_cache_lru_pos_;
    static constexpr std::size_t kMaxCachedBitmaps = 48;
    std::unordered_map<int, ID2D1Bitmap *> slot_thumb_cache_;
    std::vector<HitRegion> hits_;
    bool has_clip_rect_ = false;
    D2D1_RECT_F active_clip_rect_{};
    bool in_render_ = false;
#endif

    LauncherState state_;
    GlobalLauncherConfig global_cfg_{};
    std::array<TrilogyCardSummary, static_cast<std::size_t>(GameId::Count)> trilogy_summaries_{};
    GameId active_game_ = GameId::Covenant;
    ChimeAudio audio_;

    Page page_ = Page::Trilogy;
    float time_sec_ = 0.0f;
    std::vector<Ember> embers_;
    bool anim_running_ = false;
    bool game_running_ = false;

    // Interaction state
    std::string hovered_id_;
    std::string dragging_id_;
    D2D1_RECT_F dragging_rect_{};
    std::string focused_id_;
    bool gamepad_nav_active_ = false;
    bool gamepad_connected_ = false;
    bool gamepad_is_sony_ = false;
    std::uint32_t gp_prev_mask_ = 0;
    int gp_repeat_dx_ = 0;
    int gp_repeat_dy_ = 0;
    float gp_repeat_next_ = 0.0f;

    // Per-page scroll & filter state
    float settings_scroll_ = 0.0f;
    float settings_max_scroll_ = 0.0f;
    int trophy_tier_ = -1;       // -1 = All, 0..6 = Tier
    int trophy_status_ = 0;      // 0 = All, 1 = Unlocked, 2 = Locked
    bool reveal_secrets_ = false;
    int trophy_selected_ = 0;
    int trophy_scroll_ = 0;
    int saves_subtab_ = 0;       // 0 = Save States, 1 = Screenshots
    int tools_subtab_ = 0;       // 0 = System Health, 1 = HD Packs & Mods, 2 = Updates
    int packs_view_mode_ = 0;    // 0 = Texture Packs, 1 = Video Packs, 2 = Gameplay Mods
    int fmv_upscale_scale_ = 2;  // 2, 3, or 4 (realesr-animevideov3)
    double game_launch_time_sec_ = 0.0;
    float tools_mods_scroll_ = 0.0f;
    float tools_mods_max_scroll_ = 0.0f;
    int armed_delete_slot_ = -1;
    int armed_delete_mod_ = -1;
    int config_mod_index_ = -1;  // >= 0 when drilling into per-mod settings sub-view
    float media_scroll_ = 0.0f;
    float media_max_scroll_ = 0.0f;
    std::filesystem::path lightbox_image_;
    std::string lightbox_title_;

    // 1890s Library + Embedded PDF/CBZ Leather Tome Reader
    int library_filter_ = 0;         // 0=Manga, 1=Strategy Guides, 2=Novels & Lore, 3=Game Manuals, 4=Custom
    float library_scroll_ = 0.0f;
    float library_max_scroll_ = 0.0f;
    bool reader_open_ = false;
    std::string reader_item_id_;
    std::string reader_title_;
    std::filesystem::path reader_file_path_;
    int reader_page_ = 0;
    bool reader_spread_ = false;
    bool reader_rtl_ = false;
    int reader_zoom_idx_ = 0;        // 0=Fit, 1=125%, 2=150%, 3=200%
    float reader_pan_x_ = 0.0f;
    float reader_pan_y_ = 0.0f;
    bool reader_panning_ = false;
    D2D1_POINT_2F reader_pan_last_{0.0f, 0.0f};
    std::uint64_t reader_open_start_ms_ = 0;
    bool reader_flipping_ = false;
    int reader_flip_dir_ = 1;        // +1 forward, -1 backward
    int reader_flip_from_page_ = 0;
    std::uint64_t reader_flip_start_ms_ = 0;
    LibraryReader library_reader_;

    // Background GitHub update checker state
    std::mutex update_mu_;
    std::thread update_thread_;
    UpdateCheckResult update_info_;

    // Bottom status toast
    std::string status_toast_;
    float status_toast_until_ = 0.0f;

    // Wave E: Co-op Lobby state
    coop::CoopTransport coop_net_;
    coop::LobbySettings coop_settings_{};
    std::vector<coop::LanRoomInfo> coop_lan_rooms_;
    bool coop_in_lobby_ = false;
    bool coop_hosting_ = false;
    bool coop_ready_ = false;
    bool coop_pending_relay_launch_ = false;
    std::string coop_pending_code_file_;
    std::string coop_pending_live_code_;
    std::uint16_t coop_pending_game_port_ = coop::kDefaultGamePort;
    std::uint64_t coop_pending_launch_start_ms_ = 0;
    std::uint64_t coop_pending_resend_ms_ = 0;
    std::uint64_t coop_pending_close_ms_ = 0;
    bool game_launched_coop_ = false;
    std::string coop_join_input_ = "127.0.0.1:47192";
    int coop_edit_field_ = 0; // 0=none, 1=join code/IP, 2=player name, 3=relay URL, 4=campaign

    // Wave F: Judgment Ring Dojo
    RingDojo ring_dojo_;
    std::uint64_t dojo_last_tick_ms_ = 0;

    // Collectible Card Binder, Deck Builder & Judgment Ring Duel
    cards::Collection card_collection_;
    bool card_album_from_trilogy_ = true;
    int card_view_mode_ = 0;         // 0 = 3-Ring Binder, 1 = Deck Builder, 2 = Judgment Ring Duel
    int card_game_filter_ = -1;      // -1 = All, 0..3 = SeriesGame
    int card_cat_filter_ = -1;       // -1 = All, 0 = Main Characters, 1 = NPCs, 2 = Beasts
    int card_rarity_filter_ = -1;    // -1 = All, 0..4 = Fixed Rarity, 5 = Foil, 6 = Animated
    int card_owned_filter_ = 0;      // 0 = All, 1 = Owned Only, 2 = Missing Only
    int card_binder_spread_ = 0;     // 0-based open 2-page spread (18 sleeves per spread)
    int card_binder_max_spread_ = 0;
    float card_binder_flip_t_ = 0.0f;  // 1.0 -> 0.0 animated page-turn transition
    int card_binder_flip_dir_ = 1;
    bool card_rules_open_ = false;     // Illustrated "How to Play" Gothic Grimoire modal
    std::string card_last_reward_msg_; // Celebratory banner text after Duel victory
    std::string card_last_drop_id_;    // Card ID dropped on Duel victory for live reveal!
    int card_last_drop_finish_ = 0;    // Finish of the dropped victory card
    float card_album_scroll_ = 0.0f;
    float card_deck_scroll_ = 0.0f;
    float card_deck_max_scroll_ = 0.0f;
    std::string card_inspect_id_;
    int card_inspect_finish_ = 0;    // 0 = Standard, 1 = Foil, 2 = Animated
    bool card_inspect_flipped_ = false;
    card_game::DuelMatch card_duel_;
    bool card_duel_started_ = false;
    int card_duel_selected_hand_ = -1;
    bool card_duel_dragging_ = false;
    D2D1_RECT_F duel_node_rects_[5]{};
    int card_duel_opp_idx_ = 0;
    int card_duel_difficulty_ = 1;    // Strictly 3 difficulties: 0=Easy, 1=Normal, 2=Hard
    bool npc_duel_mode_ = false;      // True when launched from inside Koudelka/SH1/SH2/SH3 via Square!
    std::string npc_duel_id_;
    std::string npc_duel_ante_card_;
    std::filesystem::path npc_duel_result_file_;
    bool card_duel_use_trial_ = true;
    bool card_duel_live_ring_ = false;
    // 3D Altar Board live VFX (shockwaves, spark bursts, floating pop-ups, micro screen-shake)
    struct DuelSpark { float x, y, vx, vy, life, max_life, size; std::uint32_t rgb; };
    struct DuelShockwave { float x, y, life, max_life, max_r; std::uint32_t rgb; };
    struct DuelFloatText { float x, y, life, max_life; std::string text; std::uint32_t rgb; };
    std::vector<DuelSpark> duel_sparks_;
    std::vector<DuelShockwave> duel_waves_;
    std::vector<DuelFloatText> duel_floats_;
    float duel_shake_ = 0.0f;
    float duel_sweep_visual_angle_ = -1.5707963f;
    float duel_slam_t_[5][2]{};  // Per-pillar card summon slam animation (1.0 -> 0.0)
    std::vector<std::filesystem::path> card_music_tracks_;
    int card_music_idx_ = 0;
    bool card_music_scanned_ = false;
    void sync_card_music(bool force_next = false);
    D2D1_POINT_2F mouse_pt_{0.0f, 0.0f};
};

}  // namespace launcher
