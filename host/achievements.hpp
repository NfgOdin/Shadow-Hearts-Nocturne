#pragma once

#include "achievements_catalog.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace achievements {

const char *tier_name(Tier t);
const char *tier_short(Tier t);
const char *toast_theme_name(int theme);
const char *toast_theme_word(int theme);
int parse_toast_theme(const std::string &word);

struct Entry {
    std::string id;
    std::string title;
    std::string description;
    Tier tier = Tier::Mark;
    bool secret = false;
    bool missable = false;
    bool unlocked = false;
    std::string unlocked_at;   // "YYYY-MM-DD HH:MM:SS"
    std::string progress_text; // e.g. "40 / 10,000 Gold" or "3 / 20 Streak"
    double progress_ratio = 0.0;
};

struct ToastPicture {
    std::vector<std::uint32_t> rgba;
    std::uint32_t width = 0, height = 0, x = 0, y = 0;
    bool active = false;
};

struct ToastTickResult {
    bool changed = false;             // overlay buffer or position updated
    bool chime = false;               // trigger harmonic achievement unlock chime now
    bool take_screenshot = false;     // capture game + toast screenshot now
    std::string screenshot_title;     // achievement title for "Achievement_<Title>_<Date>.png"
};

class Manager {
public:
    Manager();
    ~Manager();
    Manager(const Manager &) = delete;
    Manager &operator=(const Manager &) = delete;

    // Load / save persistence at "<game_dir>/achievements.json"
    void load(const std::string &json_path);
    void save() const;
    void flush();  // Wait for any pending background save to finish cleanly before exit.

    // Poll PS2 EE RAM once per rendered frame (cost < 0.01 ms). Queues any newly unlocked achievements.
    void poll_ram(int active_disc);

    // Advance the active top-right toast animation and render its RGBA buffer when visible.
    ToastTickResult tick_toast(double now_seconds, int window_w, int window_h, int theme_index, ToastPicture &out);

    // Accessors for the F1 Trophy Gallery viewer
    const std::vector<Entry> &entries() const { return entries_; }
    int unlocked_count() const;
    int total_count() const { return static_cast<int>(entries_.size()); }
    int tier_unlocked(Tier t) const;
    int tier_total(Tier t) const;
    std::size_t pending_toast_count() const { return toast_queue_.size() + (toast_active_ ? 1u : 0u); }

    // Helper for gradient colors (RGBA, R in low byte) used by both toast and F1 viewer
    static void theme_colors(Tier t, int theme_index, std::uint32_t &grad_left, std::uint32_t &grad_right, std::uint32_t &border, std::uint32_t &accent);

private:
    struct ToastItem {
        bool is_burst = false;
        std::size_t entry_idx = 0;
        int burst_count = 0;
        Tier burst_tier = Tier::Mark;
        std::uint64_t serial = 0;
    };

    void init_catalog();
    void render_toast_pixels(const ToastItem &item, int window_w, int window_h, int theme_index, ToastPicture &out);
    void writer_loop();

    std::string path_;
    std::vector<Entry> entries_;
    std::vector<ToastItem> toast_queue_;
    std::uint64_t next_toast_serial_ = 1;

    // Active toast animation state & cached RGBA buffer
    bool toast_active_ = false;
    ToastItem active_toast_{};
    double toast_start_ = 0.0;
    bool toast_shot_taken_ = false;
    int last_toast_x_ = -99999;
    int last_toast_theme_ = -1;
    int last_win_w_ = 0, last_win_h_ = 0;
    std::uint64_t cached_toast_serial_ = 0;
    int cached_toast_theme_ = -1;
    int cached_win_w_ = 0, cached_win_h_ = 0;
    ToastPicture cached_toast_;

    // Background atomic JSON writer (avoids blocking frame loop on disk I/O)
    mutable std::mutex save_mutex_;
    mutable std::condition_variable save_cv_;
    mutable std::string pending_path_;
    mutable std::string pending_json_;
    mutable bool save_pending_ = false;
    mutable bool save_writing_ = false;
    bool save_stop_ = false;
    mutable std::thread save_thread_;

    // Live delta trackers (persisted high-water marks + session delta detectors)
    bool ram_initialized_ = false;
    std::uint32_t prev_ring_attempts_ = 0;
    std::uint32_t prev_ring_successes_ = 0;
    std::uint32_t prev_ring_perfects_ = 0;
    std::uint32_t prev_battles_entered_ = 0;
    std::uint32_t prev_battles_won_ = 0;
    std::uint32_t prev_wallet_idx_ = 0;
    std::uint32_t prev_active_gold_ = 0;

    int current_ring_streak_ = 0;
    int best_ring_streak_ = 0;
    int current_perfect_streak_ = 0;
    int best_perfect_streak_ = 0;
    int current_flawless_battle_streak_ = 0;
    int best_flawless_battle_streak_ = 0;
    int current_miss_streak_ = 0;
    int best_miss_streak_ = 0;

    std::uint32_t ring_attempts_in_current_battle_ = 0;
    bool missed_in_current_battle_ = false;
    bool non_perfect_in_current_battle_ = false;

    int perfect_battle_wins_ = 0;
    int clutch_wins_ = 0;
    int nicolai_won_battles_ = 0;
    bool joachim_won_battle_ = false;
    bool mikasa_won_battle_ = false;
    std::uint64_t total_gold_spent_ = 0;
    std::uint64_t biggest_single_spend_ = 0;
    std::vector<std::string> visited_regions_;
};

}  // namespace achievements
