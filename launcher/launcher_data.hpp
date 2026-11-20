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

struct Settings {
    // Display & Graphics (synced with games/sh2-covenant/keys.ini)
    int scale = 4;                  // 1..64 (slider covers 1..8; higher values from keys.ini/F1 preserved)
    int aspect = 1;                 // 0 = 4:3, 1 = 16:9, 2 = stretch
    std::string fps_cap = "60";     // "30", "60", "120", "144", "240", "display", "unlimited"
    bool fps_speed_sync = true;
    std::string gpu_api = "dx12";   // "dx12", "vulkan"
    std::string present_mode = "Mailbox";  // "Mailbox", "Fifo", "Immediate"
    std::string look = "sharp_edges";      // "sharp_edges", "sharp", "crisp", "crisp_edges", "soft", "soft_edges"
    std::string sharp_2d = "sharpest";     // "soft", "sharp", "sharpest"
    bool deinterlace = true;
    bool gpu_binning = true;
    bool adaptive_scale = false;
    int scanlines = 0;              // 0..100

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

    // Disc & Theme
    bool fast_disc = true;
    int toast_theme = 0;            // 0..3 (achievements::ToastTheme)

    // HD Texture Packs, Smart Runtime Dumper & HD FMV Replacement
    bool hd_textures = true;
    bool hd_fmv = true;
    bool tex_dump = false;
    bool fmv_dump = false;
    std::string texture_packs;      // comma-separated pack order ("PackA,PackB,!DisabledPack")

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
    std::filesystem::path game_exe;
    std::filesystem::path game_dir;
    std::filesystem::path keys_ini;
    std::filesystem::path achievements_json;
    std::filesystem::path memcard_path;
    std::filesystem::path states_dir;
    std::filesystem::path logs_dir;
    std::filesystem::path backups_dir;
    std::filesystem::path screenshots_dir;
    std::filesystem::path mods_textures_dir;
    std::filesystem::path mods_fmv_dir;
    std::filesystem::path dumps_textures_dir;
    std::filesystem::path dumps_fmv_dir;
    std::filesystem::path upscale_script;
    std::filesystem::path upscale_fmv_script;
    std::filesystem::path extract_pkb_script;
    std::filesystem::path precompile_vu1_script;
    std::filesystem::path build_cmd;
    std::filesystem::path disc1_path;
    std::filesystem::path disc2_path;
};

struct LauncherState {
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
    int occupied_slots = 0;
    int latest_slot = -1;
    std::vector<ScreenshotItem> screenshots;
    std::filesystem::path hero_screenshot;
    std::vector<TexturePackInfo> texture_packs;
    FmvModSummary fmv_mods;
    std::size_t dumped_tex_count = 0;
    std::size_t dumped_fmv_count = 0;
    Vu1CacheSummary vu1_cache;
    std::vector<HealthCheck> health;
    bool ready_to_play = false;
};

// Resolve default paths given the directory containing Shadow Hearts Nocturne.exe
LauncherPaths resolve_paths(const std::filesystem::path &root_dir);
std::filesystem::path default_screenshots_folder();
std::string sanitize_filename(const std::string &name);

// Load / save keys.ini preserving existing comments and keybindings
Settings load_settings(const std::filesystem::path &keys_ini_path);
bool save_settings(const std::filesystem::path &keys_ini_path, const Settings &s);
std::string upsert_ini_setting(const std::string &text, const std::string &key, const std::string &value);

// Reload all launcher data (settings, 110 trophies, 10 save slots, screenshots, health checks)
void refresh_all(LauncherState &state);

// Individual refresh helpers (also directly unit-testable)
void load_trophies(LauncherState &state);
void load_save_slots(LauncherState &state);
void load_screenshots(LauncherState &state);
void load_texture_packs(LauncherState &state);
void evaluate_health(LauncherState &state);

// Texture pack priority & enable/disable helpers (syncs state.settings.texture_packs)
std::string serialize_texture_packs(const std::vector<TexturePackInfo> &packs);
bool toggle_texture_pack(LauncherState &state, std::size_t index);
bool move_texture_pack(LauncherState &state, std::size_t index, int delta);
std::string format_bytes(std::uint64_t bytes);

// Maintenance actions
bool delete_save_slot(const LauncherPaths &paths, int slot);
bool backup_memcard(const LauncherPaths &paths, std::filesystem::path *out_backup_path = nullptr, std::string *out_msg = nullptr);
int clear_live_caches(const LauncherPaths &paths);

// Construct Windows command line for launching Shadow Hearts Covenant.exe (optional load_slot 0..9)
std::wstring build_launch_command(const LauncherPaths &paths, bool fullscreen, int load_slot = -1);
std::string format_playtime(float seconds);

}  // namespace launcher
