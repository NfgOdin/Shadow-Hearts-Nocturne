#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace achievements {

enum class Tier : int {
    Mark = 0,
    Sigil = 1,
    Crest = 2,
    Sovereign = 3,
    Covenant = 4,
    Folly = 5,
    Curiosity = 6,
    Count = 7
};

enum class ToastTheme : int {
    TierTinted = 0,     // Dynamic Tier-Tinted Gradients (default)
    SoulGraveyard = 1,  // Soul Graveyard Eclipse
    JudgmentRing = 2,   // Judgment Ring Crimson & Brass
    OccultGrimoire = 3, // 1915 Occult Grimoire
    Count = 4
};

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

    // Load / save persistence at "<game_dir>/achievements.json"
    void load(const std::string &json_path);
    void save() const;

    // Poll PS2 EE RAM once per rendered frame (cost < 0.01 ms). Queues any newly unlocked achievements.
    void poll_ram(int active_disc);

    // Advance the active top-right toast animation and render its RGBA buffer when visible.
    ToastTickResult tick_toast(double now_seconds, int window_w, int window_h, int theme_index, ToastPicture &out);

    // Accessors for the F1 Achievements viewer
    const std::vector<Entry> &entries() const { return entries_; }
    int unlocked_count() const;
    int total_count() const { return static_cast<int>(entries_.size()); }
    int tier_unlocked(Tier t) const;
    int tier_total(Tier t) const;

    // Helper for gradient colors (RGBA, R in low byte) used by both toast and F1 viewer
    static void theme_colors(Tier t, int theme_index, std::uint32_t &grad_left, std::uint32_t &grad_right, std::uint32_t &border, std::uint32_t &accent);

private:
    void init_catalog();
    void unlock(std::size_t idx);
    void check_capstone();
    void render_toast(const Entry &e, int window_w, int window_h, int theme_index, float slide_t, ToastPicture &out);

    std::string path_;
    std::vector<Entry> entries_;
    std::vector<std::size_t> toast_queue_;

    // Active toast animation state
    bool toast_active_ = false;
    std::size_t toast_idx_ = 0;
    double toast_start_ = 0.0;
    bool toast_shot_taken_ = false;
    int last_toast_x_ = -99999;
    int last_toast_theme_ = -1;
    int last_win_w_ = 0, last_win_h_ = 0;

    // Live delta trackers (persisted high-water marks + session delta detectors)
    bool ram_initialized_ = false;
    std::uint32_t prev_ring_attempts_ = 0;
    std::uint32_t prev_ring_successes_ = 0;
    std::uint32_t prev_ring_perfects_ = 0;
    std::uint32_t prev_battles_won_ = 0;
    int current_ring_streak_ = 0;
    int best_ring_streak_ = 0;
    bool missed_in_current_battle_ = false;
    std::vector<std::string> visited_regions_;
};

}  // namespace achievements
