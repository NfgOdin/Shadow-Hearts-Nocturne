#pragma once

// The quick menu (F1, or Select then Start / Guide on a controller): a panel drawn over the game picture in the window, while the game
// waits. Compact Main Hub -> Settings (Graphics / Audio / Controls tabs) -> Change Keys, plus the Achievements Viewer.

#include "achievements.hpp"
#include "ps2/system.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace quickmenu {

enum class SettingsTab : int { graphics = 0, audio = 1, controls = 2, count = 3 };
enum class SavePickerMode : int { browse = 0, save_pick = 1, load_pick = 2 };

enum class Command {
    none,
    // Main Hub
    resume, save, load, open_save_states, screenshot, open_achievements, open_settings, quit,
    // Save States Page
    slot_save, slot_load, slot_delete,
    // Graphics Tab
    scale_pow2, scale_custom_begin, fps_cap, fps_speed_sync, present_mode,
    look, sharp_2d, aspect, brightness, contrast, saturation, gamma,
    scanlines, deinterlace, ssao, bloom, ssgi, filmic, vignette,
    toast_theme, gpu_adapter, gpu_api, gpu_submit, gpu_binning,
    fullscreen, reset_graphics,
    // Audio Tab
    master_volume, mute, audio_device, audio_stretch, chime_volume, bgm_volume, sfx_volume,
    // Controls Tab & Keys Page
    slot_picker_toggle, keys, change_keys, key_edit, key_defaults
};

struct Values {
    // Graphics
    std::string look;
    int scale_now = 1;
    int scale_next = 1;
    int scale_max = 8;
    bool scale_locked = false;
    std::uint64_t vram_bytes_next = 0;  // estimated VRAM for scale_next
    std::uint64_t vram_bytes_custom = 0; // estimated VRAM for currently typed custom scale
    int sharp_now = 1, sharp_next = 1;
    bool sharp_locked = false;
    std::string fps_cap_label;     // e.g., "60 FPS", "Match Display (144 Hz)", "Unlimited"
    bool fps_speed_sync = true;
    std::string present_mode;      // e.g., "Mailbox", "Fifo", "Immediate"
    int aspect_mode = 0;           // 0 = 4:3, 1 = 16:9, 2 = Stretch
    int brightness = 0;            // -50..50
    int contrast = 100;            // 50..150
    int saturation = 100;          // 0..200
    int gamma = 100;               // 50..200
    int scanlines = 0;             // 0..100
    bool deinterlace = true;
    int ssao = 0;                  // 0..100
    int bloom = 0;                 // 0..100
    int ssgi = 0;                  // 0..100
    bool filmic = false;
    int vignette = 0;              // 0..100
    int toast_theme = 0;
    std::string gpu_adapter;
    bool gpu_adapter_next = false;
    bool gpu_adapter_locked = false;
    std::string gpu_api;
    bool gpu_api_next = false;
    bool gpu_api_locked = false;
    bool gpu_submit_on = true;
    bool gpu_binning_on = true;
    bool fullscreen = false;

    // Controls / Save Slots
    int active_slot = 0;
    bool slot_picker_on_quickkey = false;
    std::array<ps2::SaveSlotInfo, 10> slots{};
    const std::vector<std::uint32_t> *slot_thumb_rgba = nullptr;
    int slot_thumb_w = 0, slot_thumb_h = 0;

    // Audio
    int master_volume = 100;
    bool muted = false;
    std::string audio_device;      // "System Default" or device name
    bool audio_stretch = true;
    int chime_volume = 100;
    int bgm_volume = 100;
    int sfx_volume = 100;

    // Keys page
    struct KeyRow {
        std::string label, keys;
    };
    std::vector<KeyRow> key_rows;
    std::string key_note;

    // Achievements page
    const std::vector<achievements::Entry> *ach_entries = nullptr;
    int ach_unlocked = 0;
    int ach_total = 0;
};

struct Picture {
    std::vector<std::uint32_t> rgba;
    int width = 0, height = 0, x = 0, y = 0;
};

class Menu {
public:
    bool is_open() const { return open_; }
    void open();
    void close();
    void move(int delta);
    Command choose();
    Command side() const;
    bool quit_armed() const { return quit_armed_; }
    bool select_at(int x, int y);
    bool click_at(int x, int y, Command &out_cmd);

    // Page queries & tab/filter controls
    bool on_main_page() const { return page_ == Page::main; }
    bool on_settings_page() const { return page_ == Page::settings; }
    bool on_keys_page() const { return page_ == Page::keys; }
    bool on_achievements_page() const { return page_ == Page::achievements; }
    bool on_save_states_page() const { return page_ == Page::save_states; }
    SavePickerMode save_picker_mode() const { return save_mode_; }
    int selected_slot() const { return (page_ == Page::save_states && selected_ >= 0 && selected_ < 10) ? selected_ : -1; }
    void open_save_picker(SavePickerMode mode, int initial_slot);
    void select_slot(int slot);
    SettingsTab settings_tab() const { return tab_; }
    void switch_tab(int delta);
    void set_tab(SettingsTab t);

    // Achievements viewer filters
    void cycle_ach_tier(int delta);
    void cycle_ach_status(int delta);
    void toggle_reveal_secrets();

    // Custom scale typing mode (Graphics tab)
    bool typing_scale() const { return typing_scale_; }
    const std::string &scale_input() const { return scale_input_; }
    void begin_scale_input(int current_scale);
    void push_scale_digit(char d);
    void backspace_scale_digit();
    void cancel_scale_input();
    int finish_scale_input(int min_val, int max_val);

    // Keys page
    void set_key_rows(int count) { key_rows_ = count; }
    bool capturing() const { return capturing_; }
    int key_row() const { return selected_; }
    void stop_capture() { capturing_ = false; }

    bool back();
    void draw(int window_width, int window_height, const Values &values, Picture &out);
    static void draw_note(int window_width, int window_height, const std::string &text, Picture &out);
    static std::string format_bytes(std::uint64_t bytes);

private:
    enum class Page { main, settings, keys, achievements, save_states };

    int count(const Values *v = nullptr) const;
    void fit(int total_rows, int visible_rows);
    void rebuild_ach_filter(const Values &values);
    void draw_main(int window_width, int window_height, const Values &values, Picture &out);
    void draw_settings(int window_width, int window_height, const Values &values, Picture &out);
    void draw_keys(int window_width, int window_height, const Values &values, Picture &out);
    void draw_achievements(int window_width, int window_height, const Values &values, Picture &out);
    void draw_save_states(int window_width, int window_height, const Values &values, Picture &out);

    bool open_ = false;
    Page page_ = Page::main;
    SavePickerMode save_mode_ = SavePickerMode::browse;
    bool direct_picker_ = false;  // opened straight into save_states via F5/F8 (Esc closes menu)
    SettingsTab tab_ = SettingsTab::graphics;
    int selected_ = 0;
    int top_ = 0;
    int main_selected_ = 0;
    int settings_selected_ = 0;
    bool quit_armed_ = false;

    // Keys page state
    int key_rows_ = 0;
    int keys_shown_ = 0;
    bool capturing_ = false;
    bool defaults_armed_ = false;

    // Custom scale typing state
    bool typing_scale_ = false;
    std::string scale_input_;

    // Achievements viewer state
    int ach_tier_filter_ = -1;   // -1 = All, 0..6 = Tier
    int ach_status_filter_ = 0;  // 0 = All, 1 = Unlocked, 2 = Locked
    bool reveal_secrets_ = false;
    std::vector<std::size_t> ach_filtered_;
    int ach_shown_ = 0;

    // Layout hitboxes for mouse navigation
    int entry_x_ = 0, entry_y_ = 0, entry_w_ = 0, entry_h_ = 0;
    int rows_y_ = 0, footer_y_ = 0, visible_count_ = 0;
    int tabs_y_ = 0, tabs_h_ = 0;
    int tab_x0_[3] = {}, tab_x1_[3] = {};
};

}  // namespace quickmenu
