#pragma once

// Koudelka (1898, PS1, SLUS-01051..01102) live PS1-RAM achievement/trophy tracker,
// JSON persistence (compatible with Shadow Hearts Nocturne.exe Launcher Trophy Gallery),
// and Gothic Nocturne software-overlay unlock toast renderer.

#include "achievements_catalog.hpp"
#include "ui_draw.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace psx_ach {

struct Entry {
    std::string id;
    std::string title;
    std::string description;
    achievements::Tier tier = achievements::Tier::Mark;
    bool secret = false;
    bool missable = false;
    bool unlocked = false;
    std::string unlocked_at;
    std::string progress_text;
    double progress_ratio = 0.0;
};

struct PartySnapshot {
    bool valid = false;
    int max_level = 0;
    int joined_count = 0;
    std::array<int, 3> level{};
    std::array<std::uint32_t, 3> exp{};
    std::array<bool, 3> joined{};
    std::array<bool, 3> active{};
    std::array<std::uint16_t, 3> hp_cur{};
    std::array<std::uint16_t, 3> hp_max{};
    std::array<std::uint16_t, 3> mp_cur{};
    std::array<std::uint16_t, 3> mp_max{};
    int max_base_stat = 0;
    int max_gear_bonus = 0;
    int max_hp = 0;
    int max_mp = 0;
    std::uint32_t room_id = 0;
    std::uint32_t playtime_frames = 0;
};

namespace detail {

constexpr std::uint32_t kRamBase = 0x80000000u;
constexpr std::uint32_t kRamSize = 0x00200000u;

inline bool in_ram(std::uint32_t va, std::uint32_t len = 1u) noexcept {
    if (va < kRamBase) return false;
    const std::uint32_t off = va - kRamBase;
    return off <= kRamSize && len <= (kRamSize - off);
}

inline std::uint8_t rd8(const std::uint8_t *ram, std::uint32_t va) noexcept {
    if (!ram || !in_ram(va, 1u)) return 0u;
    return ram[va - kRamBase];
}

inline std::uint16_t rd16(const std::uint8_t *ram, std::uint32_t va) noexcept {
    if (!ram || !in_ram(va, 2u)) return 0u;
    std::uint16_t v = 0;
    std::memcpy(&v, ram + (va - kRamBase), 2);
    return v;
}

inline std::uint32_t rd32(const std::uint8_t *ram, std::uint32_t va) noexcept {
    if (!ram || !in_ram(va, 4u)) return 0u;
    std::uint32_t v = 0;
    std::memcpy(&v, ram + (va - kRamBase), 4);
    return v;
}

inline bool match_ascii(const std::uint8_t *ram, std::uint32_t va, const char *s) noexcept {
    if (!ram || !s) return false;
    const std::size_t n = std::strlen(s);
    if (!in_ram(va, static_cast<std::uint32_t>(n + 1u))) return false;
    const std::uint8_t *p = ram + (va - kRamBase);
    return std::memcmp(p, s, n) == 0 && p[n] == 0u;
}

inline std::string now_timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

inline std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 32) out.push_back(c);
    }
    return out;
}

}  // namespace detail

// Read live Koudelka party/stat snapshot from 2 MiB PS1 RAM (`SLUS_010.51`).
inline PartySnapshot inspect_ram(const std::uint8_t *ram) {
    PartySnapshot snap;
    if (!ram) return snap;

    // Verify SLUS_010.51 EXE signatures in PS1 RAM
    if (!detail::match_ascii(ram, 0x8001231Cu, "KoudelkaDiscNo") ||
        !detail::match_ascii(ram, 0x8001705Cu, "Koudelka")) {
        return snap;
    }

    // Fixed global game-manager pointer at 0x800B4100 -> party actor pointers at [root-0x54, -0x50, -0x4C]
    const std::uint32_t root = detail::rd32(ram, 0x800B4100u);
    if (root < 0x80010100u || root >= 0x801FF000u) return snap;

    static constexpr const char *kNames[3] = {"Koudelka", "Edward", "James"};
    static constexpr std::uint32_t kPtrOffsets[3] = {0x54u, 0x50u, 0x4Cu};

    bool koudelka_ok = false;
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t s1 = detail::rd32(ram, root - kPtrOffsets[i]);
        if (!detail::in_ram(s1, 0x500u)) continue;
        if (!detail::match_ascii(ram, s1 + 0x146u, kNames[i])) continue;
        if (detail::rd8(ram, s1 + 0x4BCu) != static_cast<std::uint8_t>(i)) continue;

        const std::uint32_t lv = detail::rd32(ram, s1 + 0x2D8u);
        const std::uint32_t xp = detail::rd32(ram, s1 + 0x2DCu);
        const std::uint16_t hp_c = detail::rd16(ram, s1 + 0x410u);
        const std::uint16_t hp_m = detail::rd16(ram, s1 + 0x412u);
        const std::uint16_t mp_c = detail::rd16(ram, s1 + 0x420u);
        const std::uint16_t mp_m = detail::rd16(ram, s1 + 0x422u);
        const bool joined = (detail::rd8(ram, s1 + 0x4BAu) != 0u);
        const bool active = (detail::rd8(ram, s1 + 0x4BBu) != 0u);

        if (lv == 0u || lv > 99u || hp_m < 10u || hp_m > 9999u) continue;
        if (i == 0 && joined) koudelka_ok = true;

        snap.level[i] = static_cast<int>(lv);
        snap.exp[i] = xp;
        snap.joined[i] = joined;
        snap.active[i] = active;
        snap.hp_cur[i] = hp_c;
        snap.hp_max[i] = hp_m;
        snap.mp_cur[i] = mp_c;
        snap.mp_max[i] = mp_m;

        if (joined) {
            ++snap.joined_count;
            if (static_cast<int>(lv) > snap.max_level) snap.max_level = static_cast<int>(lv);
            if (static_cast<int>(hp_m) > snap.max_hp) snap.max_hp = static_cast<int>(hp_m);
            if (static_cast<int>(mp_m) > snap.max_mp) snap.max_mp = static_cast<int>(mp_m);

            int base_sum = 0;
            int eff_sum = 0;
            for (std::uint32_t k = 0; k < 8u; ++k) {
                const int b = static_cast<int>(detail::rd8(ram, s1 + 0x424u + k));
                const int e = static_cast<int>(detail::rd8(ram, s1 + 0x43Cu + k));
                if (b > snap.max_base_stat && b <= 99) snap.max_base_stat = b;
                base_sum += b;
                eff_sum += e;
            }
            const int gear_bonus = std::max(0, eff_sum - base_sum);
            if (gear_bonus > snap.max_gear_bonus) snap.max_gear_bonus = gear_bonus;
        }
    }

    if (!koudelka_ok) return snap;
    snap.valid = true;
    snap.room_id = detail::rd32(ram, 0x800B6B08u);
    if (snap.room_id == 0u || snap.room_id >= 0x400u) {
        snap.room_id = detail::rd32(ram, 0x800B41D0u);
    }
    snap.playtime_frames = detail::rd32(ram, 0x800B42E8u);
    return snap;
}

class Tracker {
public:
    Tracker() { init_catalog(); }

    void load(const std::filesystem::path &json_path) {
        path_ = json_path;
        std::ifstream ifs(path_, std::ios::binary);
        if (!ifs.is_open()) return;
        const std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (text.empty()) return;

        const auto find_int = [&](const std::string &key, int &out) {
            const std::size_t k = text.find("\"" + key + "\"");
            if (k == std::string::npos) return;
            const std::size_t c = text.find(':', k);
            if (c == std::string::npos) return;
            out = std::atoi(text.c_str() + c + 1);
        };
        const auto find_bool = [&](const std::string &key, bool &out) {
            const std::size_t k = text.find("\"" + key + "\"");
            if (k == std::string::npos) return;
            const std::size_t c = text.find(':', k);
            if (c == std::string::npos) return;
            std::size_t p = c + 1;
            while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
            if (text.compare(p, 4, "true") == 0 || (p < text.size() && text[p] == '1')) out = true;
        };

        find_int("battles_won", battles_won_);
        find_int("unscathed_wins", unscathed_wins_);
        find_int("clutch_wins", clutch_wins_);
        find_bool("last_standing_won", last_standing_won_);
        find_bool("witnessed_fallen", witnessed_fallen_);
        find_bool("witnessed_dry_well", witnessed_dry_well_);

        if (const std::size_t rk = text.find("\"visited_regions\""); rk != std::string::npos) {
            const std::size_t lb = text.find('[', rk), rb = text.find(']', rk);
            if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                visited_rooms_.clear();
                std::size_t p = lb + 1;
                while (p < rb) {
                    const std::size_t q1 = text.find('"', p);
                    if (q1 == std::string::npos || q1 >= rb) break;
                    const std::size_t q2 = text.find('"', q1 + 1);
                    if (q2 == std::string::npos || q2 >= rb) break;
                    if (q2 > q1 + 1) visited_rooms_.push_back(text.substr(q1 + 1, q2 - q1 - 1));
                    p = q2 + 1;
                }
            }
        }

        const std::size_t ublk = text.find("\"unlocked\"");
        const std::size_t lb_u = (ublk != std::string::npos) ? text.find('{', ublk) : std::string::npos;
        const std::size_t scope = (lb_u != std::string::npos) ? lb_u : 0u;
        const std::size_t rb_u = (lb_u != std::string::npos) ? text.find('}', lb_u) : text.size();
        const std::size_t limit = (rb_u != std::string::npos) ? rb_u : text.size();

        for (Entry &e : entries_) {
            const std::string needle = "\"" + e.id + "\"";
            const std::size_t pos = text.find(needle, scope);
            if (pos == std::string::npos || pos >= limit) continue;
            const std::size_t colon = text.find(':', pos + needle.size());
            if (colon == std::string::npos || colon >= limit) continue;
            const std::size_t q1 = text.find('"', colon + 1);
            if (q1 == std::string::npos || q1 >= limit) continue;
            const std::size_t q2 = text.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= limit) continue;
            e.unlocked = true;
            e.unlocked_at = text.substr(q1 + 1, q2 - q1 - 1);
            e.progress_ratio = 1.0;
        }
    }

    void save() const {
        if (path_.empty()) return;
        std::error_code ec;
        if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);

        std::string out;
        out.reserve(4096);
        out += "{\n";
        out += "  \"version\": 1,\n";
        out += "  \"game\": \"koudelka\",\n";
        out += "  \"battles_won\": " + std::to_string(battles_won_) + ",\n";
        out += "  \"unscathed_wins\": " + std::to_string(unscathed_wins_) + ",\n";
        out += "  \"clutch_wins\": " + std::to_string(clutch_wins_) + ",\n";
        out += std::string("  \"last_standing_won\": ") + (last_standing_won_ ? "true" : "false") + ",\n";
        out += std::string("  \"witnessed_fallen\": ") + (witnessed_fallen_ ? "true" : "false") + ",\n";
        out += std::string("  \"witnessed_dry_well\": ") + (witnessed_dry_well_ ? "true" : "false") + ",\n";
        out += "  \"visited_regions\": [";
        for (std::size_t i = 0; i < visited_rooms_.size(); ++i) {
            if (i > 0) out += ", ";
            out += "\"" + detail::json_escape(visited_rooms_[i]) + "\"";
        }
        out += "],\n";
        out += "  \"unlocked\": {\n";
        bool first = true;
        for (const Entry &e : entries_) {
            if (!e.unlocked) continue;
            if (!first) out += ",\n";
            first = false;
            out += "    \"" + detail::json_escape(e.id) + "\": \"" + detail::json_escape(e.unlocked_at) + "\"";
        }
        out += "\n  }\n}\n";

        std::filesystem::path tmp = path_;
        tmp += ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return;
            ofs.write(out.data(), static_cast<std::streamsize>(out.size()));
            ofs.flush();
            if (!ofs.good()) return;
        }
        std::filesystem::rename(tmp, path_, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(tmp, path_, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
        }
    }

    void poll_ram(const std::uint8_t *ram, int active_disc, double now_sec) {
        const PartySnapshot snap = inspect_ram(ram);
        if (!snap.valid) return;
        last_snap_ = snap;

        bool dirty = false;

        // Track visited rooms in Nemeton Monastery
        if (snap.room_id > 0u && snap.room_id < 0x400u) {
            char rcode[16];
            std::snprintf(rcode, sizeof rcode, "R%03X", static_cast<unsigned>(snap.room_id));
            if (std::find(visited_rooms_.begin(), visited_rooms_.end(), rcode) == visited_rooms_.end()) {
                visited_rooms_.emplace_back(rcode);
                dirty = true;
            }
        }

        // Track fallen (0 HP) and drained MP (0 MP) quirks among active joined members
        int alive_active = 0;
        int fallen_active = 0;
        bool any_clutch = false;
        bool all_full_hp = true;
        std::uint64_t total_exp = 0;
        for (int i = 0; i < 3; ++i) {
            if (!snap.joined[i]) continue;
            total_exp += snap.exp[i];
            if (snap.hp_cur[i] == 0u) {
                ++fallen_active;
                if (!witnessed_fallen_) {
                    witnessed_fallen_ = true;
                    dirty = true;
                }
                all_full_hp = false;
            } else {
                ++alive_active;
                if (snap.hp_cur[i] < snap.hp_max[i]) all_full_hp = false;
                if (snap.hp_max[i] >= 10u &&
                    static_cast<std::uint32_t>(snap.hp_cur[i]) * 100u <= static_cast<std::uint32_t>(snap.hp_max[i]) * 15u) {
                    any_clutch = true;
                }
            }
            if (snap.mp_max[i] >= 4u && snap.mp_cur[i] == 0u && !witnessed_dry_well_) {
                witnessed_dry_well_ = true;
                dirty = true;
            }
        }

        // Detect battle victories via EXP delta (filtering out save-state reloads)
        if (!ram_seeded_) {
            ram_seeded_ = true;
            prev_total_exp_ = total_exp;
            prev_playtime_frames_ = snap.playtime_frames;
        } else {
            const bool reloaded = (snap.playtime_frames + 30u < prev_playtime_frames_) ||
                                  (total_exp < prev_total_exp_);
            if (!reloaded && total_exp > prev_total_exp_ && (total_exp - prev_total_exp_) <= 500000ull) {
                ++battles_won_;
                dirty = true;
                if (all_full_hp && alive_active > 0) ++unscathed_wins_;
                if (any_clutch) ++clutch_wins_;
                if (snap.joined_count >= 3 && alive_active == 1 && fallen_active >= 2) {
                    last_standing_won_ = true;
                }
            }
            prev_total_exp_ = total_exp;
            prev_playtime_frames_ = snap.playtime_frames;
        }

        const int rooms = static_cast<int>(visited_rooms_.size());
        const double pt_sec = static_cast<double>(snap.playtime_frames) / 60.0;
        const int pt_min = static_cast<int>(pt_sec / 60.0);
        const int trio_min_lv = (snap.joined[0] && snap.joined[1] && snap.joined[2])
            ? std::min({snap.level[0], snap.level[1], snap.level[2]})
            : 0;

        std::vector<std::size_t> new_unlocks;
        std::string stamp;
        auto eval = [&](const char *id, bool cond, int cur, int goal, const char *unit) {
            const auto it = id_to_index_.find(id);
            if (it == id_to_index_.end()) return;
            Entry &e = entries_[it->second];
            if (goal > 0) {
                const int clamped = std::clamp(cur, 0, goal);
                e.progress_ratio = e.unlocked ? 1.0 : (static_cast<double>(clamped) / static_cast<double>(goal));
                e.progress_text = std::to_string(clamped) + " / " + std::to_string(goal) + " " + unit;
            } else {
                e.progress_ratio = (e.unlocked || cond) ? 1.0 : 0.0;
                e.progress_text = (e.unlocked || cond) ? "Completed" : unit;
            }
            if (!e.unlocked && cond) {
                if (stamp.empty()) stamp = detail::now_timestamp();
                e.unlocked = true;
                e.unlocked_at = stamp;
                e.progress_ratio = 1.0;
                new_unlocks.push_back(it->second);
            }
        };

        // Tier 1: Mark (12)
        eval("kdl_mark_first_blood", battles_won_ >= 1, battles_won_, 1, "victories");
        eval("kdl_mark_battles_5", battles_won_ >= 5, battles_won_, 5, "victories");
        eval("kdl_mark_edward_joins", snap.joined[1], snap.joined[1] ? 1 : 0, 1, "recruited");
        eval("kdl_mark_james_joins", snap.joined[2], snap.joined[2] ? 1 : 0, 1, "recruited");
        eval("kdl_mark_full_trio", snap.joined_count >= 3, snap.joined_count, 3, "party");
        eval("kdl_mark_lv5", snap.max_level >= 5, snap.max_level, 5, "Lv");
        eval("kdl_mark_lv10", snap.max_level >= 10, snap.max_level, 10, "Lv");
        eval("kdl_mark_gear_10", snap.max_gear_bonus >= 10, snap.max_gear_bonus, 10, "bonus");
        eval("kdl_mark_rooms_5", rooms >= 5, rooms, 5, "rooms");
        eval("kdl_mark_unscathed_1", unscathed_wins_ >= 1, unscathed_wins_, 1, "wins");
        eval("kdl_mark_first_hour", pt_min >= 60, pt_min, 60, "min");
        eval("kdl_mark_stat_18", snap.max_base_stat >= 18, snap.max_base_stat, 18, "stat");

        // Tier 2: Sigil (10)
        eval("kdl_sigil_battles_20", battles_won_ >= 20, battles_won_, 20, "victories");
        eval("kdl_sigil_battles_50", battles_won_ >= 50, battles_won_, 50, "victories");
        eval("kdl_sigil_koudelka_15", snap.joined[0] && snap.level[0] >= 15, snap.level[0], 15, "Lv");
        eval("kdl_sigil_edward_15", snap.joined[1] && snap.level[1] >= 15, snap.joined[1] ? snap.level[1] : 0, 15, "Lv");
        eval("kdl_sigil_james_15", snap.joined[2] && snap.level[2] >= 15, snap.joined[2] ? snap.level[2] : 0, 15, "Lv");
        eval("kdl_sigil_stat_25", snap.max_base_stat >= 25, snap.max_base_stat, 25, "stat");
        eval("kdl_sigil_hp_500", snap.max_hp >= 500, snap.max_hp, 500, "HP");
        eval("kdl_sigil_mp_100", snap.max_mp >= 100, snap.max_mp, 100, "MP");
        eval("kdl_sigil_rooms_15", rooms >= 15, rooms, 15, "rooms");
        eval("kdl_sigil_five_hours", pt_min >= 300, pt_min, 300, "min");

        // Tier 3: Crest (8)
        eval("kdl_crest_battles_100", battles_won_ >= 100, battles_won_, 100, "victories");
        eval("kdl_crest_koudelka_25", snap.joined[0] && snap.level[0] >= 25, snap.level[0], 25, "Lv");
        eval("kdl_crest_edward_25", snap.joined[1] && snap.level[1] >= 25, snap.joined[1] ? snap.level[1] : 0, 25, "Lv");
        eval("kdl_crest_james_25", snap.joined[2] && snap.level[2] >= 25, snap.joined[2] ? snap.level[2] : 0, 25, "Lv");
        eval("kdl_crest_trio_25", trio_min_lv >= 25, trio_min_lv, 25, "min Lv");
        eval("kdl_crest_stat_45", snap.max_base_stat >= 45, snap.max_base_stat, 45, "stat");
        eval("kdl_crest_hp_1000", snap.max_hp >= 1000, snap.max_hp, 1000, "HP");
        eval("kdl_crest_rooms_30", rooms >= 30, rooms, 30, "rooms");

        // Tier 4: Sovereign (6)
        eval("kdl_sovereign_battles_250", battles_won_ >= 250, battles_won_, 250, "victories");
        eval("kdl_sovereign_lv40", snap.max_level >= 40, snap.max_level, 40, "Lv");
        eval("kdl_sovereign_trio_40", trio_min_lv >= 40, trio_min_lv, 40, "min Lv");
        eval("kdl_sovereign_stat_70", snap.max_base_stat >= 70, snap.max_base_stat, 70, "stat");
        eval("kdl_sovereign_hp_2000", snap.max_hp >= 2000, snap.max_hp, 2000, "HP");
        eval("kdl_sovereign_rooms_50", rooms >= 50, rooms, 50, "rooms");

        // Follies (7)
        eval("kdl_folly_clutch", clutch_wins_ >= 1, clutch_wins_, 1, "clutch");
        eval("kdl_folly_last_standing", last_standing_won_, last_standing_won_ ? 1 : 0, 1, "wins");
        eval("kdl_folly_fallen", witnessed_fallen_, witnessed_fallen_ ? 1 : 0, 1, "fallen");
        eval("kdl_folly_dry_well", witnessed_dry_well_, witnessed_dry_well_ ? 1 : 0, 1, "drained");
        eval("kdl_folly_ironclad", snap.max_gear_bonus >= 25, snap.max_gear_bonus, 25, "bonus");
        eval("kdl_folly_unscathed_5", unscathed_wins_ >= 5, unscathed_wins_, 5, "wins");
        eval("kdl_folly_vigil", pt_min >= 1200, pt_min, 1200, "min");

        // Curiosities (6)
        eval("kdl_curiosity_disc1", active_disc >= 1 && snap.playtime_frames >= 60u, active_disc >= 1 ? 1 : 0, 1, "Disc 1");
        eval("kdl_curiosity_disc2", active_disc >= 2, active_disc >= 2 ? 1 : 0, 1, "Disc 2");
        eval("kdl_curiosity_disc3", active_disc >= 3, active_disc >= 3 ? 1 : 0, 1, "Disc 3");
        eval("kdl_curiosity_disc4", active_disc >= 4, active_disc >= 4 ? 1 : 0, 1, "Disc 4");
        eval("kdl_curiosity_mp_250", snap.max_mp >= 250, snap.max_mp, 250, "MP");
        eval("kdl_curiosity_rooms_75", rooms >= 75, rooms, 75, "rooms");

        // Tier 5: Capstone (1)
        int non_cap_unlocked = 0, non_cap_total = 0;
        for (const Entry &e : entries_) {
            if (e.tier == achievements::Tier::Covenant) continue;
            ++non_cap_total;
            if (e.unlocked) ++non_cap_unlocked;
        }
        eval("kdl_nemeton_absolved", non_cap_total > 0 && non_cap_unlocked >= non_cap_total,
             non_cap_unlocked, non_cap_total, "trophies");

        if (!new_unlocks.empty()) {
            for (std::size_t idx : new_unlocks) {
                toast_queue_.push_back(idx);
            }
            if (!toast_active_ && !toast_queue_.empty()) {
                active_toast_idx_ = toast_queue_.front();
                toast_queue_.pop_front();
                toast_start_sec_ = now_sec;
                toast_active_ = true;
            }
            save();
        } else if (dirty) {
            save();
        }
    }

    bool has_active_toast(double now_sec) {
        constexpr double kDuration = 4.5;
        while (toast_active_ && (now_sec - toast_start_sec_) >= kDuration) {
            if (!toast_queue_.empty()) {
                active_toast_idx_ = toast_queue_.front();
                toast_queue_.pop_front();
                toast_start_sec_ = now_sec;
            } else {
                toast_active_ = false;
            }
        }
        return toast_active_;
    }

    void draw_active_toast(ui_draw::Surface &surf, int theme_index, double now_sec) {
        if (!has_active_toast(now_sec) || active_toast_idx_ >= entries_.size()) return;
        const Entry &e = entries_[active_toast_idx_];
        const double elapsed = now_sec - toast_start_sec_;
        constexpr double kDuration = 4.5;

        float slide = 1.0f;
        if (elapsed < 0.35) {
            const float t = static_cast<float>(elapsed / 0.35);
            slide = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        } else if (elapsed > kDuration - 0.4) {
            const float t = static_cast<float>((kDuration - elapsed) / 0.4);
            slide = std::clamp(t, 0.0f, 1.0f);
        }

        const int tw = std::min(368, std::max(280, surf.width - 28));
        const int th = 78;
        const int tx = surf.width - static_cast<int>(slide * (tw + 16));
        const int ty = 44;

        std::uint32_t gl = 0, gr = 0, bd = 0, ac = 0;
        achievements::catalog_theme_colors(e.tier, theme_index, gl, gr, bd, ac);

        surf.fill_gradient_rounded_rect(tx, ty, tw, th, 10.0f, gl, gr, 0.22f, true);
        surf.stroke_rounded_rect(tx, ty, tw, th, 10.0f, 1.5f, bd);

        ui_draw::draw_tier_medallion(surf, static_cast<float>(tx + 36), static_cast<float>(ty + th / 2),
                                     22.0f, static_cast<int>(e.tier), gl, gr, bd, ac, true, false);

        const int lx = tx + 68;
        const int max_lw = tw - 82;
        std::string hdr = std::string("TROPHY UNLOCKED  •  ") + achievements::catalog_tier_short(e.tier);
        ui_draw::draw_text(surf, lx, ty + 9, ui_draw::FontStyle::Sans, 10, true, hdr, ac);
        ui_draw::draw_text(surf, lx, ty + 25, ui_draw::FontStyle::Serif, 15, true,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Serif, 15, true, e.title, max_lw),
                           ui_draw::rgba(255, 245, 222));
        ui_draw::draw_text(surf, lx, ty + 47, ui_draw::FontStyle::Sans, 11, false,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Sans, 11, false, e.description, max_lw),
                           ui_draw::rgba(218, 212, 228));
    }

    const std::vector<Entry> &entries() const noexcept { return entries_; }
    const PartySnapshot &last_snapshot() const noexcept { return last_snap_; }

    int total_count() const noexcept { return static_cast<int>(entries_.size()); }
    int unlocked_count() const noexcept {
        int c = 0;
        for (const Entry &e : entries_) if (e.unlocked) ++c;
        return c;
    }
    int battles_won() const noexcept { return battles_won_; }
    int rooms_visited() const noexcept { return static_cast<int>(visited_rooms_.size()); }

private:
    void init_catalog() {
        entries_.clear();
        id_to_index_.clear();
        const auto &items = achievements::catalog_items_for(achievements::CatalogGame::Koudelka);
        entries_.reserve(items.size());
        for (const auto &it : items) {
            Entry e;
            e.id = it.id;
            e.title = it.title;
            e.description = it.description;
            e.tier = it.tier;
            e.secret = it.secret;
            e.missable = it.missable;
            id_to_index_.emplace(e.id, entries_.size());
            entries_.push_back(std::move(e));
        }
    }

    std::filesystem::path path_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> id_to_index_;
    std::vector<std::string> visited_rooms_;
    PartySnapshot last_snap_{};

    int battles_won_ = 0;
    int unscathed_wins_ = 0;
    int clutch_wins_ = 0;
    bool last_standing_won_ = false;
    bool witnessed_fallen_ = false;
    bool witnessed_dry_well_ = false;

    bool ram_seeded_ = false;
    std::uint64_t prev_total_exp_ = 0;
    std::uint32_t prev_playtime_frames_ = 0;

    std::deque<std::size_t> toast_queue_;
    bool toast_active_ = false;
    std::size_t active_toast_idx_ = 0;
    double toast_start_sec_ = 0.0;
};

}  // namespace psx_ach
