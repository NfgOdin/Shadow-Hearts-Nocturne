#pragma once

// Pure C++20 data, configuration, save-slot, achievement, screenshot, and maintenance layer
// for Shadow Hearts Nocturne.exe (zero PS2 emulator runtime dependency).

#include "achievements_catalog.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

enum class GameId : int {
    ShadowHearts1 = 0,
    Covenant = 1,
    FromTheNewWorld = 2,
    Koudelka = 3,
    Count = 4
};

struct GameProfile {
    GameId id = GameId::Covenant;
    const char *slug = "sh2-covenant";
    const char *roman = "II";
    const char *title_top = "SHADOW HEARTS:";
    const char *title_main = "COVENANT";
    const char *full_title = "Shadow Hearts: Covenant";
    const char *short_name = "Covenant";
    const char *era_subtitle = "1915 Europe & Japan";
    const char *tagline = "";
    const char *serials = "SLUS-21041 / 21044";
    const char *disc1_label = "Disc 1 (SLUS-21041)";
    const char *disc2_label = "Disc 2 (SLUS-21044)";
    const char *exe_name = "SH2C.exe";
    const char *build_dir_name = "native-sh2-covenant";
    const char *screenshot_subdir = "Covenant";
    const char *screenshot_prefix = "ShadowHeartsCovenant";
    const char *play_button_label = "PLAY COVENANT";
    const char *progress_heading = "COVENANT PROGRESS";
    const char *coming_soon_msg = "";
    int disc_count = 2;
    bool has_achievement_catalog = true;
    bool has_pkb_extractor = true;
};

struct GlobalLauncherConfig {
    bool chimes = true;
    bool auto_enter_last_game = false;   // Default OFF: always open on the 3-panel Trilogy Triptych
    GameId last_game = GameId::Covenant;
    std::string update_channel = "main"; // "main" (stable) or "nightly" (opt-in latest)
    std::string source_repo;             // Optional path to source repo when installed into a standalone folder
    // Wave E: Co-op Lobby preferences
    std::string coop_player_name = "Player 1";
    std::string coop_relay_url = "137.131.24.235:47191"; // Default public Nocturne Co-op Relay (editable/overridable in Co-op Lobby & launcher.ini)
    std::string coop_campaign = "nocturne_coop";
    // Wave F: Judgment Ring Dojo personal bests
    int dojo_best_streak = 0;
    int dojo_best_perfect = 0;
};

struct UpdateCheckResult {
    bool checked = false;
    bool checking = false;
    bool ok = false;
    bool update_available = false;
    std::string channel = "main";
    std::string local_short;
    std::string local_date;
    std::string remote_short;
    std::string remote_date;
    std::string remote_subject;
    std::string error;
};

const GameProfile &game_profile(GameId id);
GameId game_id_from_slug(const std::string &slug);

struct Settings {
    // Display & Graphics — Ultra-Pure 1999/2001 Stock 1x Baseline out of the box
    int scale = 1;                  // 1..64 (1 = Native 1x stock; players can bump to 2x..8x anytime)
    int aspect = 0;                 // 0 = 4:3 (stock), 1 = 16:9, 2 = stretch, 3 = auto, 4 = 16:10, 5 = 21:9, 6 = 32:9
    bool true_widescreen = false;   // Hor+ 3D camera projection (opt-in when using 16:9+)
    int hud_anchor = 0;             // 0 = Off (stock), 1 = Ultrawide Only, 2 = All Widescreen (16:10+)
    int anisotropy = 1;             // 1 = Off (stock), 4, 8, 16
    bool dedither = false;          // Koudelka 24-bit Smooth De-Dithering (off = authentic PS1 dither)
    int visual_preset = 0;          // Koudelka preset: 0=Raw PS1, 1=Gothic Noir, 2=Crisp Classic, 3=Vibrant HD
    int cas_sharpen = 0;            // Wave G: AMD CAS Contrast-Adaptive Sharpening (0..100%)
    int fsr_upscale = 0;            // Wave G: 0=Off, 1=FSR 1.0 Quality, 2=FSR 1.0 Ultra
    int aa_mode = 0;                // Wave G: 0=Off, 1=FXAA-Fast, 2=FXAA-Quality
    int color_temp = 0;             // Wave G: -50 (Cool Moonlight) .. +50 (Warm Candlelight)
    int film_grain = 0;             // Wave G: 0..100% animated cinema grain
    int crt_mask = 0;               // Wave G: 0=Off, 1=Aperture Grille, 2=Shadow Mask
    int lod_bias = 0;               // Wave G: Mipmap LOD bias in tenths (-20..+20, default 0 = 0.0 stock)
    int brightness = 0;             // -50..+50
    int contrast = 100;             //  50..150
    int gamma = 100;                //  50..200
    bool gpu_submit_thread = true;  // Async GPU submit thread
    bool title_perf = false;        // Titlebar FPS & telemetry
    // Koudelka PS1 hardware renderer settings
    int renderer = 1;               // 1 = D3D11 Hardware GPU (auto CPU fallback), 0 = Software CPU
    bool pgxp = false;              // PGXP subpixel vertex precision (opt-in enhancement)
    int filter_3d = 0;              // 0 = Nearest (stock), 1 = Bilinear, 2 = Sharp-3D
    // Wave G: Expert Engine & GPU Controls (persisted in keys.ini & exported to PS2_* env vars)
    int vram_budget = 0;            // 0 = Auto, 512, 1024, 2048, 4096 MB
    bool vram_guard = true;         // PS2_VRAM_GUARD
    bool hw_rasterizer = true;      // PS2_GPU_HW
    bool dual_source_blend = true;  // PS2_GPU_HW_DUAL
    bool tex_hot_reload = false;    // PS2_TEX_HOT_RELOAD
    std::string fps_cap = "60";     // "30", "60", "120", "144", "240", "display", "unlimited"
    bool fps_speed_sync = true;
    std::string gpu_api = "auto";   // "auto" (Vulkan -> DX12 DXC -> DX12 FXC), "vulkan", "dx12"
    std::string present_mode = "Mailbox";  // "Mailbox", "Fifo", "Immediate"
    std::string look = "crisp";            // "crisp" (1-tap stock), "sharp_edges", "sharp", "crisp_edges", "soft", "soft_edges"
    std::string sharp_2d = "soft";         // "soft" (stock), "sharp", "sharpest"
    bool sharp_3d = false;          // sharp-bilinear 3D texture filter (crisper polygons)
    bool deinterlace = true;
    bool gpu_binning = true;
    bool adaptive_scale = false;
    bool perf_hud = false;          // fullscreen corner FPS / Turbo HUD (F10)
    bool ring_hud = false;          // Wave F: live battle Judgment Ring streak HUD pill
    bool strict_ring_challenge = false; // Wave F: live no-miss Strict Ring Challenge badge
    int scanlines = 0;              // 0..100
    int saturation = 100;           // 0..200

    // Depth-Aware Lighting & Post-Processing
    int ssao = 0;                   // 0..100
    int bloom = 0;                  // 0..100
    int ssgi = 0;                   // 0..100
    bool filmic = false;
    int vignette = 0;               // 0..100

    // Audio
    int master_volume = 100;        // 0..100
    int bgm_volume = 100;           // 0..100
    int sfx_volume = 100;           // 0..100
    int chime_volume = 100;         // 0..100
    bool mute = false;
    bool audio_stretch = true;

    // Disc, Turbo & Theme
    bool fast_disc = true;
    bool autosave = true;           // rolling area-transition autosave (autosave.state)
    int turbo_speed = 3;            // 2, 3, 4, or 0 = Max (Hold Tab / F3 / Select+R2)
    int toast_theme = 0;            // 0..3 (achievements::ToastTheme)

    // HD Texture Packs, Video Packs, Gameplay Mods & Runtime Dumpers
    bool hd_textures = true;
    bool hd_fmv = true;
    bool mods_enabled = true;
    bool tex_dump = false;
    bool fmv_dump = false;
    std::string texture_packs;      // comma-separated pack order ("PackA,PackB,!DisabledPack")
    std::string video_packs;        // comma-separated video pack order ("VideoPackA,!DisabledPack")
    std::string active_mods;        // comma-separated gameplay mod order ("ModA,ModB,!DisabledMod")

    // Launcher preferences (stored cleanly in keys.ini)
    bool launcher_hide_on_play = true;
    bool launcher_chimes = true;
    bool launcher_hero_screenshot = true;
    bool launcher_fullscreen = false;
};

struct TrophyEntry {
    std::string id;
    std::string title;
    std::string description;
    achievements::Tier tier = achievements::Tier::Mark;
    bool secret = false;
    bool missable = false;
    bool unlocked = false;
    std::string unlocked_at;
    std::filesystem::path screenshot_path;
};

struct SaveSlot {
    int slot = 0;
    bool exists = false;
    std::size_t bytes = 0;
    std::string timestamp;
    std::string area;
    int max_level = 0;
    float playtime_sec = 0.0f;
    int disc = 1;
    std::uint64_t frame = 0;
    bool has_thumb = false;
    int thumb_w = 0;
    int thumb_h = 0;
    std::vector<std::uint32_t> thumb_bgra;  // Premultiplied/opaque BGRA ready for Direct2D
    std::filesystem::path state_path;
};

struct ScreenshotItem {
    std::filesystem::path path;
    std::string filename;
    std::string display_name;
    std::string date_text;
    bool is_achievement = false;
};

struct HealthCheck {
    std::string label;
    std::string detail;
    bool ok = false;
};

struct TexturePackInfo {
    std::string name;
    std::filesystem::path path;
    std::size_t png_count = 0;
    std::uint64_t bytes = 0;
    bool enabled = true;
};

struct VideoPackInfo {
    std::string name;
    std::filesystem::path path;
    std::size_t mp4_count = 0;
    std::uint64_t bytes = 0;
    bool enabled = true;
};

struct ModInfo {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::filesystem::path path;
    int priority = 100;
    bool enabled = true;
    std::size_t file_override_count = 0;
    std::size_t pnach_patch_count = 0;
    std::size_t lua_hook_count = 0;
    std::size_t dll_plugin_count = 0;
    std::vector<std::string> override_rel_paths;
    std::vector<std::uint32_t> patched_addresses;
    std::vector<std::string> conflicts_with;
};

struct PlaytimeStats {
    double solo_seconds = 0.0;
    double coop_seconds = 0.0;
    int sessions_count = 0;
    std::string last_played;
    double effective_total_seconds = 0.0;
};

struct FmvModSummary {
    std::size_t mp4_count = 0;
    std::uint64_t bytes = 0;
};

struct Vu1CacheSummary {
    int baked_programs = 0;
    int captured_unique = 0;
    int unbaked_count = 0;
    int live_dll_count = 0;
};

struct LauncherPaths {
    std::filesystem::path root_dir;
    std::filesystem::path scripts_root;
    std::filesystem::path game_exe;
    std::filesystem::path game_dll;
    std::filesystem::path engine_exe;
    std::string engine_game_arg;
    bool uses_shared_engine = false;
    std::filesystem::path game_dir;
    std::filesystem::path keys_ini;
    std::filesystem::path achievements_json;
    std::filesystem::path playtime_json;
    std::filesystem::path memcard_path;
    std::filesystem::path states_dir;
    std::filesystem::path logs_dir;
    std::filesystem::path backups_dir;
    std::filesystem::path screenshots_dir;
    std::filesystem::path texture_packs_dir; // Dedicated: games/<slug>/texture_packs
    std::filesystem::path video_packs_dir;   // Dedicated: games/<slug>/video_packs
    std::filesystem::path mods_dir;          // Dedicated: games/<slug>/mods (real gameplay mods)
    std::filesystem::path mods_textures_dir; // Legacy fallback: games/<slug>/mods/textures
    std::filesystem::path mods_fmv_dir;      // Legacy fallback: games/<slug>/mods/fmv
    std::filesystem::path dumps_textures_dir;
    std::filesystem::path dumps_fmv_dir;
    std::filesystem::path upscale_script;
    std::filesystem::path upscale_fmv_script;
    std::filesystem::path extract_pkb_script;
    std::filesystem::path precompile_vu1_script;
    std::filesystem::path update_script;
    std::filesystem::path build_cmd;
    std::filesystem::path disc1_path;
    std::filesystem::path disc2_path;
    std::filesystem::path cover_image;
};

struct TrilogyCardSummary {
    GameId id = GameId::Covenant;
    bool exe_present = false;
    bool disc1_ok = false;
    bool disc2_ok = false;
    bool ready_to_play = false;
    int occupied_slots = 0;
    bool has_autosave = false;
    int trophies_unlocked = 0;
    int trophies_total = 0;
    double playtime_seconds = 0.0;
    int screenshot_count = 0;
    int active_mod_count = 0;
    std::string latest_area;
    std::filesystem::path cover_image;
    std::filesystem::path hero_screenshot;
};

struct LauncherState {
    GameId game_id = GameId::Covenant;
    LauncherPaths paths;
    Settings settings;
    std::vector<TrophyEntry> trophies;
    int trophies_unlocked = 0;
    int tier_unlocked[7] = {};
    int tier_total[7] = {};
    int best_ring_streak = 0;
    int best_perfect_streak = 0;
    int regions_visited = 0;
    std::array<SaveSlot, 10> slots{};
    SaveSlot autosave_slot{};
    int occupied_slots = 0;
    int latest_slot = -1;           // 0..9 = manual slot, 10 = autosave_slot, -1 = none
    std::vector<ScreenshotItem> screenshots;
    std::filesystem::path hero_screenshot;
    std::vector<TexturePackInfo> texture_packs;
    std::vector<VideoPackInfo> video_packs;
    std::vector<ModInfo> mods;
    PlaytimeStats playtime;
    FmvModSummary fmv_mods;
    std::size_t dumped_tex_count = 0;
    std::size_t dumped_fmv_count = 0;
    Vu1CacheSummary vu1_cache;
    std::vector<HealthCheck> health;
    bool exe_present = false;
    bool ready_to_play = false;
};

// Resolve per-game paths given the directory containing Shadow Hearts Nocturne.exe
LauncherPaths resolve_paths(const std::filesystem::path &root_dir, GameId game = GameId::Covenant);
std::filesystem::path default_screenshots_folder(GameId game = GameId::Covenant);
std::string sanitize_filename(const std::string &name);

// Load / save global trilogy launcher settings (launcher.ini at root)
GlobalLauncherConfig load_global_config(const std::filesystem::path &root_dir);
bool save_global_config(const std::filesystem::path &root_dir, const GlobalLauncherConfig &cfg);
UpdateCheckResult parse_update_check_json(const std::string &json);

// Lightweight summaries for all 3 Trilogy Triptych cards + Koudelka prequel pill
std::array<TrilogyCardSummary, static_cast<std::size_t>(GameId::Count)> load_trilogy_summaries(const std::filesystem::path &root_dir);

// Load / save keys.ini preserving existing comments and keybindings
Settings load_settings(const std::filesystem::path &keys_ini_path);
bool save_settings(const std::filesystem::path &keys_ini_path, const Settings &s);
std::string upsert_ini_setting(const std::string &text, const std::string &key, const std::string &value);

// Reload all launcher data (settings, trophies, save slots, screenshots, packs, mods, playtime, health)
void refresh_all(LauncherState &state);

// Individual refresh helpers (also directly unit-testable)
void load_trophies(LauncherState &state);
void load_save_slots(LauncherState &state);
void load_screenshots(LauncherState &state);
void load_texture_packs(LauncherState &state);
void load_video_packs(LauncherState &state);
void load_mods(LauncherState &state);
void load_playtime(LauncherState &state);
void evaluate_health(LauncherState &state);

// Playtime session recording
bool record_playtime_session(const LauncherPaths &paths, double elapsed_seconds, bool is_coop = false);
std::string format_playtime(float seconds);

// Texture pack, Video pack & Gameplay Mod priority & enable/disable helpers
std::string serialize_texture_packs(const std::vector<TexturePackInfo> &packs);
bool toggle_texture_pack(LauncherState &state, std::size_t index);
bool move_texture_pack(LauncherState &state, std::size_t index, int delta);

std::string serialize_video_packs(const std::vector<VideoPackInfo> &packs);
bool toggle_video_pack(LauncherState &state, std::size_t index);
bool move_video_pack(LauncherState &state, std::size_t index, int delta);

std::string serialize_mods(const std::vector<ModInfo> &mods);
bool toggle_mod(LauncherState &state, std::size_t index);
bool move_mod(LauncherState &state, std::size_t index, int delta);
bool duplicate_mod(LauncherState &state, std::size_t index, std::string *out_msg = nullptr);
bool delete_mod(LauncherState &state, std::size_t index, std::string *out_msg = nullptr);
bool install_flagship_sh1_mod(LauncherState &state, std::string *out_msg = nullptr);
bool install_crimson_flow_mod(LauncherState &state, std::string *out_msg = nullptr);

std::string format_bytes(std::uint64_t bytes);

// Maintenance & Save Bundle Export / Import actions
bool delete_save_slot(const LauncherPaths &paths, int slot);
bool backup_memcard(const LauncherPaths &paths, std::filesystem::path *out_backup_path = nullptr, std::string *out_msg = nullptr);
bool export_save_bundle(const LauncherPaths &paths, std::filesystem::path *out_bundle_path = nullptr, std::string *out_msg = nullptr);
bool import_save_bundle(const LauncherPaths &paths, const std::filesystem::path &bundle_path, std::string *out_msg = nullptr);
int clear_live_caches(const LauncherPaths &paths);

// Wave F: 1-Click .zip / .nocturne-mod Pack Auto-Installer & Mod Scaffold Generator
enum class ArchivePackKind : int { Unknown = 0, GameplayMod = 1, TexturePack = 2, VideoPack = 3 };
bool import_mod_or_pack_archive(const LauncherPaths &paths, const std::filesystem::path &archive_path,
                                ArchivePackKind *out_kind = nullptr, std::string *out_msg = nullptr);
bool create_mod_scaffold(const LauncherPaths &paths, const std::string &mod_name, std::string *out_msg = nullptr);

// Construct Windows command line for launching Shadow Hearts Covenant.exe (optional load_slot 0..9 + extra CLI args)
std::wstring build_launch_command(const LauncherPaths &paths, bool fullscreen, int load_slot = -1,
                                  const std::string &extra_args = "");

}  // namespace launcher
