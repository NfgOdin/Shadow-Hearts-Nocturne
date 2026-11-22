#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace launcher {

enum class DojoMode : int {
    Standard = 0,   // Classic 3-hit Attack Ring
    Tight = 1,      // Narrow Mind's Eye wedges + faster sweep
    Blind = 2,      // Wedges fade invisible after 95 deg
    Fake = 3,       // Variable speed modulation (slow -> surge)
    Gamble = 4,     // Single razor-thin Lottery / Gamble Strike wedge
    RelayCombo = 5, // Consecutive escalating combo sweeps
    Count = 6
};

inline const char *dojo_mode_name(DojoMode m) {
    switch (m) {
    case DojoMode::Standard:   return "Standard (3-Hit)";
    case DojoMode::Tight:      return "Tight (Technical)";
    case DojoMode::Blind:      return "Blind Ring";
    case DojoMode::Fake:       return "Fake (Modulated)";
    case DojoMode::Gamble:     return "Gamble / Lottery";
    case DojoMode::RelayCombo: return "Relay Combo";
    default:                   return "Standard";
    }
}

inline const char *dojo_mode_desc(DojoMode m) {
    switch (m) {
    case DojoMode::Standard:   return "Classic 3-wedge Shadow Hearts attack ring. Land crimson trailing edges for Perfect Strikes.";
    case DojoMode::Tight:      return "Narrower hit & strike areas with faster needle velocity — simulates Technical Ring / Mind's Eye.";
    case DojoMode::Blind:      return "Hit wedges fade away once the needle passes 95 deg. Pure rhythm and muscle memory.";
    case DojoMode::Fake:       return "Needle subtly accelerates and decelerates mid-sweep to test nerves under pressure.";
    case DojoMode::Gamble:     return "Single razor-thin high-stakes strike wedge (Lottery / Gamble Ring practice).";
    case DojoMode::RelayCombo: return "Back-to-back combo sweeps that speed up each chain — ideal for SH2/SH3 & Co-op Relay Combos.";
    default:                   return "";
    }
}

enum class DojoGrade : int {
    None = 0,
    Perfect = 1,
    Hit = 2,
    Miss = 3
};

struct RingZone {
    float start_deg = 0.0f;  // green hit area start (0..360, clockwise from top)
    float end_deg = 0.0f;    // hit + strike area end
    float strike_deg = 0.0f; // crimson Perfect Strike starts here (ends at end_deg)
    bool resolved = false;
    DojoGrade result = DojoGrade::None;
};

struct TimingSample {
    DojoGrade grade = DojoGrade::Miss;
    float delta_ms = 0.0f; // negative = early, positive = late, 0 = dead center of Strike zone
};

class RingDojo {
public:
    RingDojo() { reset_ring(false); }

    void set_mode(DojoMode m) {
        mode_ = m;
        combo_chain_ = 0;
        reset_ring(false);
    }
    DojoMode mode() const { return mode_; }

    void set_best_streak(int best, int best_perfect) {
        best_streak_ = std::max(best_streak_, best);
        best_perfect_streak_ = std::max(best_perfect_streak_, best_perfect);
    }

    void start_sweep() {
        reset_ring(true);
    }

    void reset_stats() {
        attempts_ = hits_ = perfects_ = misses_ = 0;
        streak_ = perfect_streak_ = 0;
        combo_chain_ = 0;
        history_.clear();
        last_grade_ = DojoGrade::None;
        last_label_ = "Press SPACE, Click, or Gamepad A to spin the Judgment Ring";
    }

    void update(float dt_sec) {
        if (!sweeping_) return;
        dt_sec = std::clamp(dt_sec, 0.0f, 0.05f);

        float speed = base_speed_deg_per_sec();
        if (mode_ == DojoMode::Fake) {
            // Smooth sinusoidal speed modulation (0.65x..1.55x)
            const float rad = needle_deg_ * (3.14159265f / 180.0f) * 2.0f;
            speed *= (1.05f + 0.42f * std::sin(rad));
        } else if (mode_ == DojoMode::RelayCombo) {
            speed *= (1.0f + std::min(combo_chain_, 8) * 0.09f);
        }
        current_speed_ = speed;
        needle_deg_ += speed * dt_sec;

        // Check if needle passed an unresolved zone completely -> automatic Miss
        for (auto &z : zones_) {
            if (!z.resolved && needle_deg_ > z.end_deg + 4.0f) {
                z.resolved = true;
                z.result = DojoGrade::Miss;
                record_result(DojoGrade::Miss, +((needle_deg_ - (z.strike_deg + z.end_deg) * 0.5f) / std::max(speed, 1.0f)) * 1000.0f);
                // In authentic Shadow Hearts, missing a zone aborts remaining combo hits
                if (mode_ == DojoMode::RelayCombo) combo_chain_ = 0;
                sweeping_ = false;
                return;
            }
        }

        if (needle_deg_ >= 360.0f) {
            needle_deg_ = 360.0f;
            sweeping_ = false;
            if (mode_ == DojoMode::RelayCombo && all_zones_hit()) {
                ++combo_chain_;
                reset_ring(true);
            }
        }
    }

    // Triggered at 0 ms latency on Space / Enter / Click / Gamepad A
    DojoGrade trigger_press() {
        if (!sweeping_) {
            start_sweep();
            return DojoGrade::None;
        }
        // Find first unresolved zone
        for (auto &z : zones_) {
            if (z.resolved) continue;
            const float strike_mid = (z.strike_deg + z.end_deg) * 0.5f;
            const float delta_ms = ((needle_deg_ - strike_mid) / std::max(current_speed_, 1.0f)) * 1000.0f;

            if (needle_deg_ >= z.strike_deg && needle_deg_ <= z.end_deg) {
                z.resolved = true;
                z.result = DojoGrade::Perfect;
                record_result(DojoGrade::Perfect, delta_ms);
                check_sweep_complete();
                return DojoGrade::Perfect;
            }
            if (needle_deg_ >= z.start_deg && needle_deg_ < z.strike_deg) {
                z.resolved = true;
                z.result = DojoGrade::Hit;
                record_result(DojoGrade::Hit, delta_ms);
                check_sweep_complete();
                return DojoGrade::Hit;
            }
            // Pressed outside the active zone -> Miss
            z.resolved = true;
            z.result = DojoGrade::Miss;
            record_result(DojoGrade::Miss, delta_ms);
            if (mode_ == DojoMode::RelayCombo) combo_chain_ = 0;
            sweeping_ = false;
            return DojoGrade::Miss;
        }
        return DojoGrade::None;
    }

    // Visibility alpha (0..1) of hit wedges for Blind mode (fades out between 30..70 deg before Zone 1)
    float zone_alpha() const {
        if (mode_ != DojoMode::Blind || !sweeping_) return 1.0f;
        if (needle_deg_ <= 30.0f) return 1.0f;
        if (needle_deg_ >= 70.0f) return 0.0f;
        return 1.0f - (needle_deg_ - 30.0f) / 40.0f;
    }

    bool sweeping() const { return sweeping_; }
    float needle_deg() const { return needle_deg_; }
    const std::vector<RingZone> &zones() const { return zones_; }
    const std::vector<TimingSample> &history() const { return history_; }

    DojoGrade last_grade() const { return last_grade_; }
    float last_delta_ms() const { return last_delta_ms_; }
    const std::string &last_label() const { return last_label_; }

    int attempts() const { return attempts_; }
    int hits() const { return hits_; }
    int perfects() const { return perfects_; }
    int misses() const { return misses_; }
    int streak() const { return streak_; }
    int best_streak() const { return best_streak_; }
    int perfect_streak() const { return perfect_streak_; }
    int best_perfect_streak() const { return best_perfect_streak_; }
    int combo_chain() const { return combo_chain_; }

    float hit_rate_pct() const { return attempts_ > 0 ? (100.0f * static_cast<float>(hits_) / static_cast<float>(attempts_)) : 0.0f; }
    float perfect_rate_pct() const { return attempts_ > 0 ? (100.0f * static_cast<float>(perfects_) / static_cast<float>(attempts_)) : 0.0f; }

private:
    float base_speed_deg_per_sec() const {
        switch (mode_) {
        case DojoMode::Standard:   return 225.0f; // ~1.6s full revolution (classic SH2)
        case DojoMode::Tight:      return 275.0f;
        case DojoMode::Blind:      return 230.0f;
        case DojoMode::Fake:       return 235.0f;
        case DojoMode::Gamble:     return 300.0f;
        case DojoMode::RelayCombo: return 245.0f;
        default:                   return 225.0f;
        }
    }

    bool all_zones_hit() const {
        for (const auto &z : zones_) {
            if (!z.resolved || z.result == DojoGrade::Miss) return false;
        }
        return !zones_.empty();
    }

    void check_sweep_complete() {
        for (const auto &z : zones_) {
            if (!z.resolved) return;
        }
        if (mode_ == DojoMode::RelayCombo) {
            ++combo_chain_;
            reset_ring(true);
        } else {
            sweeping_ = false;
        }
    }

    void reset_ring(bool begin_sweep) {
        needle_deg_ = 0.0f;
        sweeping_ = begin_sweep;
        current_speed_ = base_speed_deg_per_sec();
        zones_.clear();

        if (mode_ == DojoMode::Gamble) {
            zones_.push_back({202.0f, 214.0f, 206.0f, false, DojoGrade::None});
        } else if (mode_ == DojoMode::Tight) {
            zones_.push_back({92.0f, 114.0f, 108.0f, false, DojoGrade::None});
            zones_.push_back({186.0f, 208.0f, 202.0f, false, DojoGrade::None});
            zones_.push_back({284.0f, 306.0f, 300.0f, false, DojoGrade::None});
        } else if (mode_ == DojoMode::RelayCombo) {
            const float shift = static_cast<float>((combo_chain_ * 17) % 30);
            zones_.push_back({82.0f + shift, 114.0f + shift, 106.0f + shift, false, DojoGrade::None});
            zones_.push_back({210.0f + shift, 242.0f + shift, 234.0f + shift, false, DojoGrade::None});
        } else {
            // Standard / Blind / Fake 3-hit attack ring
            zones_.push_back({80.0f, 118.0f, 109.0f, false, DojoGrade::None});
            zones_.push_back({176.0f, 214.0f, 205.0f, false, DojoGrade::None});
            zones_.push_back({274.0f, 312.0f, 303.0f, false, DojoGrade::None});
        }
    }

    void record_result(DojoGrade g, float delta_ms) {
        ++attempts_;
        last_grade_ = g;
        last_delta_ms_ = delta_ms;
        history_.push_back({g, delta_ms});
        if (history_.size() > 14) history_.erase(history_.begin());

        char buf[96];
        if (g == DojoGrade::Perfect) {
            ++hits_;
            ++perfects_;
            ++streak_;
            ++perfect_streak_;
            if (streak_ > best_streak_) best_streak_ = streak_;
            if (perfect_streak_ > best_perfect_streak_) best_perfect_streak_ = perfect_streak_;
            std::snprintf(buf, sizeof(buf), "PERFECT STRIKE!  (%+.1f ms)", static_cast<double>(delta_ms));
        } else if (g == DojoGrade::Hit) {
            ++hits_;
            ++streak_;
            perfect_streak_ = 0;
            if (streak_ > best_streak_) best_streak_ = streak_;
            std::snprintf(buf, sizeof(buf), "STRIKE HIT  (%.1f ms EARLY)", static_cast<double>(-delta_ms));
        } else {
            ++misses_;
            streak_ = 0;
            perfect_streak_ = 0;
            std::snprintf(buf, sizeof(buf), "RING MISS  (%+.1f ms %s)",
                          static_cast<double>(delta_ms), delta_ms < 0.0f ? "EARLY" : "LATE");
        }
        last_label_ = buf;
    }

    DojoMode mode_ = DojoMode::Standard;
    bool sweeping_ = false;
    float needle_deg_ = 0.0f;
    float current_speed_ = 225.0f;
    std::vector<RingZone> zones_;
    std::vector<TimingSample> history_;

    DojoGrade last_grade_ = DojoGrade::None;
    float last_delta_ms_ = 0.0f;
    std::string last_label_ = "Press SPACE, Click, or Gamepad A to spin the Judgment Ring";

    int attempts_ = 0;
    int hits_ = 0;
    int perfects_ = 0;
    int misses_ = 0;
    int streak_ = 0;
    int best_streak_ = 0;
    int perfect_streak_ = 0;
    int best_perfect_streak_ = 0;
    int combo_chain_ = 0;
};

} // namespace launcher
