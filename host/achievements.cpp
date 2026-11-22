#include "achievements.hpp"
#include "ui_draw.hpp"

#include "ps2/memory.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace achievements {
namespace {

constexpr std::uint32_t rgba_u32(int r, int g, int b, int a = 255) {
    return ui_draw::rgba(r, g, b, a);
}

std::string now_timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

std::string fmt_commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    std::string out;
    const std::size_t len = s.size();
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) out.push_back(',');
        out.push_back(s[i]);
    }
    return out;
}

std::string fmt_hours(float seconds) {
    const int total_min = std::max(0, static_cast<int>(seconds / 60.0f));
    const int hrs = total_min / 60;
    const int mins = total_min % 60;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%dh %02dm", hrs, mins);
    return buf;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 32) out.push_back(c);
    }
    return out;
}

int popcount_range(std::uint32_t addr, std::uint32_t bytes) {
    int bits = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        std::uint8_t b = ps2::ld8(addr + i);
        while (b) {
            bits += (b & 1u);
            b >>= 1;
        }
    }
    return bits;
}

int tier_priority(Tier t) {
    switch (t) {
    case Tier::Covenant: return 6;
    case Tier::Sovereign: return 5;
    case Tier::Crest: return 4;
    case Tier::Curiosity: return 3;
    case Tier::Sigil: return 2;
    case Tier::Folly: return 1;
    case Tier::Mark:
    default: return 0;
    }
}

}  // namespace

const char *tier_name(Tier t) { return catalog_tier_name(t); }
const char *tier_short(Tier t) { return catalog_tier_short(t); }
const char *toast_theme_name(int theme) { return catalog_toast_theme_name(theme); }
const char *toast_theme_word(int theme) { return catalog_toast_theme_word(theme); }
int parse_toast_theme(const std::string &word) { return catalog_parse_toast_theme(word); }

void Manager::theme_colors(Tier t, int theme_index, std::uint32_t &grad_left, std::uint32_t &grad_right, std::uint32_t &border, std::uint32_t &accent) {
    catalog_theme_colors(t, theme_index, grad_left, grad_right, border, accent);
}

Manager::Manager(CatalogGame game) : game_(game) { init_catalog(); }

void Manager::init_catalog() {
    entries_.clear();
    id_to_index_.clear();
    const auto &items = catalog_items_for(game_);
    entries_.reserve(items.size());
    id_to_index_.reserve(items.size() * 2u);
    for (const CatalogItem &it : items) {
        Entry e;
        e.id = it.id;
        e.title = it.title;
        e.description = it.description;
        e.tier = it.tier;
        e.secret = it.secret;
        e.missable = it.missable;
        const std::size_t idx = entries_.size();
        id_to_index_.emplace(e.id, idx);
        entries_.push_back(std::move(e));
    }
    last_prog_key_.assign(entries_.size(), ~0ull);
}

void Manager::load(const std::string &json_path) {
    path_ = json_path;
    std::FILE *f = ps2::os::open(path_, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);

    const auto find_int = [&](const std::string &key, int &out) {
        const std::size_t k = text.find("\"" + key + "\"");
        if (k == std::string::npos) return;
        const std::size_t c = text.find(':', k);
        if (c == std::string::npos) return;
        out = std::atoi(text.c_str() + c + 1);
    };
    const auto find_u64 = [&](const std::string &key, std::uint64_t &out) {
        const std::size_t k = text.find("\"" + key + "\"");
        if (k == std::string::npos) return;
        const std::size_t c = text.find(':', k);
        if (c == std::string::npos) return;
        out = std::strtoull(text.c_str() + c + 1, nullptr, 10);
    };
    const auto find_bool = [&](const std::string &key, bool &out) {
        const std::size_t k = text.find("\"" + key + "\"");
        if (k == std::string::npos) return;
        const std::size_t c = text.find(':', k);
        if (c == std::string::npos) return;
        std::size_t p = c + 1;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (text.compare(p, 4, "true") == 0 || text[p] == '1') out = true;
    };

    find_int("best_ring_streak", best_ring_streak_);
    find_int("best_perfect_streak", best_perfect_streak_);
    find_int("best_flawless_battle_streak", best_flawless_battle_streak_);
    find_int("best_miss_streak", best_miss_streak_);
    find_int("perfect_battle_wins", perfect_battle_wins_);
    find_int("clutch_wins", clutch_wins_);
    find_int("nicolai_won_battles", nicolai_won_battles_);
    find_bool("joachim_won_battle", joachim_won_battle_);
    find_bool("mikasa_won_battle", mikasa_won_battle_);
    find_u64("total_gold_spent", total_gold_spent_);
    find_u64("biggest_single_spend", biggest_single_spend_);

    // Parse visited_regions array
    if (const std::size_t rk = text.find("\"visited_regions\""); rk != std::string::npos) {
        const std::size_t lb = text.find('[', rk), rb = text.find(']', rk);
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            std::size_t p = lb + 1;
            while (p < rb) {
                const std::size_t q1 = text.find('"', p);
                if (q1 == std::string::npos || q1 >= rb) break;
                const std::size_t q2 = text.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= rb) break;
                std::string reg = text.substr(q1 + 1, q2 - q1 - 1);
                if (!reg.empty() && std::find(visited_regions_.begin(), visited_regions_.end(), reg) == visited_regions_.end())
                    visited_regions_.push_back(reg);
                p = q2 + 1;
            }
        }
    }

    // Parse optional "burst_screenshots" map (stored before "unlocked" for backwards compatibility)
    if (const std::size_t bblk = text.find("\"burst_screenshots\""); bblk != std::string::npos) {
        const std::size_t lb = text.find('{', bblk);
        const std::size_t rb = (lb != std::string::npos) ? text.find('}', lb) : std::string::npos;
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
            for (Entry &e : entries_) {
                const std::string needle = "\"" + e.id + "\"";
                const std::size_t pos = text.find(needle, lb);
                if (pos == std::string::npos || pos >= rb) continue;
                const std::size_t colon = text.find(':', pos + needle.size());
                if (colon == std::string::npos || colon >= rb) continue;
                const std::size_t q1 = text.find('"', colon + 1);
                if (q1 == std::string::npos || q1 >= rb) continue;
                const std::size_t q2 = text.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 >= rb) continue;
                e.screenshot_title = text.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    // Parse unlocked entries inside "unlocked" block (if present) or whole document
    const std::size_t unlock_scope = text.find("\"unlocked\"");
    const std::size_t lb_unlock = (unlock_scope != std::string::npos) ? text.find('{', unlock_scope) : std::string::npos;
    const std::size_t search_from = (lb_unlock != std::string::npos) ? lb_unlock : ((unlock_scope != std::string::npos) ? unlock_scope : 0);
    const std::size_t search_end = (lb_unlock != std::string::npos) ? text.find('}', lb_unlock) : text.size();
    const std::size_t limit = (search_end != std::string::npos) ? search_end : text.size();
    for (Entry &e : entries_) {
        const std::string needle = "\"" + e.id + "\"";
        const std::size_t pos = text.find(needle, search_from);
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
    std::fprintf(stderr, "[achievements] loaded %d/%d unlocked from %s\n", unlocked_count(), total_count(), path_.c_str());
}

Manager::~Manager() {
    {
        std::lock_guard<std::mutex> lk(save_mutex_);
        save_stop_ = true;
    }
    save_cv_.notify_all();
    if (save_thread_.joinable()) save_thread_.join();
}

void Manager::writer_loop() {
    for (;;) {
        std::string path, payload;
        {
            std::unique_lock<std::mutex> lk(save_mutex_);
            save_cv_.wait(lk, [this] { return save_pending_ || save_stop_; });
            if (!save_pending_) {
                if (save_stop_) return;
                continue;
            }
            path = pending_path_;
            payload = std::move(pending_json_);
            save_pending_ = false;
            save_writing_ = true;
        }
        std::string why;
        if (!ps2::os::replace_file(path, payload.data(), payload.size(), &why))
            std::fprintf(stderr, "[achievements] save failed: %s\n", why.c_str());
        {
            std::lock_guard<std::mutex> lk(save_mutex_);
            save_writing_ = false;
        }
        save_cv_.notify_all();
    }
}

void Manager::flush() {
    std::unique_lock<std::mutex> lk(save_mutex_);
    save_cv_.wait(lk, [this] { return !save_pending_ && !save_writing_; });
}

void Manager::save() const {
    if (path_.empty()) return;
    std::string out = "{\n";
    out += "  \"best_ring_streak\": " + std::to_string(best_ring_streak_) + ",\n";
    out += "  \"best_perfect_streak\": " + std::to_string(best_perfect_streak_) + ",\n";
    out += "  \"best_flawless_battle_streak\": " + std::to_string(best_flawless_battle_streak_) + ",\n";
    out += "  \"best_miss_streak\": " + std::to_string(best_miss_streak_) + ",\n";
    out += "  \"perfect_battle_wins\": " + std::to_string(perfect_battle_wins_) + ",\n";
    out += "  \"clutch_wins\": " + std::to_string(clutch_wins_) + ",\n";
    out += "  \"nicolai_won_battles\": " + std::to_string(nicolai_won_battles_) + ",\n";
    out += std::string("  \"joachim_won_battle\": ") + (joachim_won_battle_ ? "true" : "false") + ",\n";
    out += std::string("  \"mikasa_won_battle\": ") + (mikasa_won_battle_ ? "true" : "false") + ",\n";
    out += "  \"total_gold_spent\": " + std::to_string(total_gold_spent_) + ",\n";
    out += "  \"biggest_single_spend\": " + std::to_string(biggest_single_spend_) + ",\n";
    out += "  \"visited_regions\": [";
    for (std::size_t i = 0; i < visited_regions_.size(); ++i)
        out += std::string(i ? ", " : "") + "\"" + json_escape(visited_regions_[i]) + "\"";
    out += "],\n";
    out += "  \"burst_screenshots\": {\n";
    bool first_bs = true;
    for (const Entry &e : entries_) {
        if (!e.unlocked || e.screenshot_title.empty()) continue;
        if (!first_bs) out += ",\n";
        first_bs = false;
        out += "    \"" + json_escape(e.id) + "\": \"" + json_escape(e.screenshot_title) + "\"";
    }
    out += "\n  },\n";
    out += "  \"unlocked\": {\n";
    bool first = true;
    for (const Entry &e : entries_) {
        if (!e.unlocked) continue;
        if (!first) out += ",\n";
        first = false;
        out += "    \"" + json_escape(e.id) + "\": \"" + json_escape(e.unlocked_at) + "\"";
    }
    out += "\n  }\n}\n";
    {
        std::lock_guard<std::mutex> lk(save_mutex_);
        if (!save_thread_.joinable() && !save_stop_)
            save_thread_ = std::thread([this] { const_cast<Manager *>(this)->writer_loop(); });
        pending_path_ = path_;
        pending_json_ = std::move(out);
        save_pending_ = true;
    }
    save_cv_.notify_one();
}

void Manager::poll_ram(int active_disc) {
    if (game_ == CatalogGame::ShadowHearts1) {
        poll_ram_sh1();
        return;
    }
    if (game_ == CatalogGame::FromTheNewWorld) {
        poll_ram_sh3();
        return;
    }
    // Verify active NTSC-U SLUS-21041 SSH_SAVE live block at 0x00100100 before reading any fields.
    static constexpr char kMagic[8] = {'S', 'S', 'H', '_', 'S', 'A', 'V', 'E'};
    for (int i = 0; i < 8; ++i)
        if (ps2::ld8(0x00100104u + i) != static_cast<std::uint8_t>(kMagic[i])) return;
    if (ps2::ld32(0x00100120u) != 0x304C5442u || ps2::ld32(0x00102D1Cu) != 0x554E454Du) return;
    if (ps2::ld32(0x001022ECu) != 0x61444C46u) return;  // "FLDa"

    // Guard against title screen / uninitialized cold-boot RAM: require active gameplay (playtime >= 1s & valid map ID).
    const std::uint32_t pt_bits = ps2::ld32(0x00102300u);
    float playtime_sec = 0.0f;
    std::memcpy(&playtime_sec, &pt_bits, sizeof playtime_sec);
    if (!(playtime_sec >= 1.0f && playtime_sec < 1e8f)) return;

    char map_buf[17] = {};
    for (int i = 0; i < 16; ++i) map_buf[i] = static_cast<char>(ps2::ld8(0x0010230Cu + i));
    const std::string_view map_sv(map_buf);
    if (map_sv.size() < 2 || !(map_sv[0] >= 'A' && map_sv[0] <= 'Z' && map_sv[1] >= 'A' && map_sv[1] <= 'Z')) return;

    bool dirty_persist = false;

    // 1. Gold / Cash (active wallet at 0x00102D30 + wallet_idx*4, cumulative at 0x00103018)
    const std::uint32_t wallet_idx = ps2::ld8(0x00100132u) & 3u;
    const std::uint32_t active_gold = ps2::ld32(0x00102D30u + wallet_idx * 4u);
    const std::uint32_t cum_gold = ps2::ld32(0x00103018u);
    const std::uint32_t best_gold = std::max(active_gold, cum_gold);

    // 2. Party Characters (14 slots at 0x00100250, stride 0x90)
    // Verified canonical char_id at +0x0E:
    //   0=Yuri, 1=Blanca, 2=Gepetto, 3=Lucia, 4=Anastasia, 5=Karin, 6=Joachim, 7=Kurando,
    //   8..13=Guests (11=Nicolai, 12=Lenny, 13=Veronica)
    int max_level = 0, active_members = 0;
    std::array<int, 8> core_lv{};
    bool nicolai_active = false;
    bool any_critical_hp = false;
    for (int slot = 0; slot < 14; ++slot) {
        const std::uint32_t base = 0x00100250u + static_cast<std::uint32_t>(slot) * 0x90u;
        const std::uint8_t char_id = ps2::ld8(base + 0x0Eu);
        const std::uint8_t lv = ps2::ld8(base + 0x0Fu);
        const std::uint8_t c0 = ps2::ld8(base + 0x16u);
        if (c0 == 0 || lv == 0 || lv > 99) continue;
        ++active_members;
        if (lv > max_level) max_level = lv;
        if (char_id < 8u) {
            core_lv[char_id] = std::max(core_lv[char_id], static_cast<int>(lv));
        } else if (char_id == 11u) {
            nicolai_active = true;
        }
        const std::uint16_t hp_max = ps2::ld16(base + 0x28u);
        const std::uint16_t hp_cur = ps2::ld16(base + 0x2Au);
        if (hp_max >= 10u && hp_cur > 0u && hp_cur <= hp_max &&
            static_cast<std::uint32_t>(hp_cur) * 100u <= static_cast<std::uint32_t>(hp_max) * 15u) {
            any_critical_hp = true;
        }
    }
    if (active_members <= 0) return;

    const int yuri_lv = core_lv[0];
    const int blanca_lv = core_lv[1];
    const int gepetto_lv = core_lv[2];
    const int lucia_lv = core_lv[3];
    const int anastasia_lv = core_lv[4];
    const int karin_lv = core_lv[5];
    const int joachim_lv = core_lv[6];
    const int kurando_lv = core_lv[7];

    int core_recruited = 0, core_at_25 = 0, core_at_50 = 0, core_at_99 = 0;
    int min_core_level = 99;
    for (int i = 0; i < 8; ++i) {
        if (core_lv[i] >= 1) ++core_recruited;
        if (core_lv[i] >= 25) ++core_at_25;
        if (core_lv[i] >= 50) ++core_at_50;
        if (core_lv[i] >= 99) ++core_at_99;
        if (core_lv[i] < min_core_level) min_core_level = core_lv[i];
    }

    // 3. Battle & Judgment Ring Stats (BTL0)
    const std::uint32_t battles_entered = ps2::ld32(0x001015ECu);
    const std::uint32_t battles_won = ps2::ld32(0x001016E4u);
    const std::uint32_t ring_attempts = ps2::ld32(0x001017C8u);
    const std::uint32_t ring_successes = ps2::ld32(0x001017CCu);
    const std::uint32_t ring_perfects = ps2::ld32(0x001017D0u);
    const std::uint32_t battles_fled = battles_entered >= battles_won ? (battles_entered - battles_won) : 0u;

    // 4. Region Tracking (FLDa @ 0x0010230C) - zero heap allocation on steady frames
    const std::string_view prefix = map_sv.substr(0, 2);
    const auto has_region = [&](std::string_view code) -> bool {
        for (const std::string &r : visited_regions_)
            if (r == code) return true;
        return false;
    };
    if (!has_region(prefix)) {
        visited_regions_.emplace_back(prefix);
        dirty_persist = true;
    }

    // 5. Live Streak, Spending, Flawless & Battle Delta Detection
    auto flush_pending_rings = [&]() {
        if (pending_ring_attempts_ == 0u) return;
        const std::uint32_t da = pending_ring_attempts_;
        const std::uint32_t ds = std::min(pending_ring_successes_, da);
        const std::uint32_t dp = std::min(pending_ring_perfects_, da);
        ring_attempts_in_current_battle_ += da;
        if (ds >= da) {
            current_ring_streak_ += static_cast<int>(ds);
            current_miss_streak_ = 0;
            if (current_ring_streak_ > best_ring_streak_) {
                best_ring_streak_ = current_ring_streak_;
                dirty_persist = true;
            }
        } else {
            current_ring_streak_ = 0;
            current_miss_streak_ += static_cast<int>(da - ds);
            if (current_miss_streak_ > best_miss_streak_) {
                best_miss_streak_ = current_miss_streak_;
                dirty_persist = true;
            }
            if (battles_entered > 0) missed_in_current_battle_ = true;
        }
        if (dp >= da) {
            current_perfect_streak_ += static_cast<int>(dp);
            if (current_perfect_streak_ > best_perfect_streak_) {
                best_perfect_streak_ = current_perfect_streak_;
                dirty_persist = true;
            }
        } else {
            current_perfect_streak_ = 0;
            if (battles_entered > 0) non_perfect_in_current_battle_ = true;
        }
        pending_ring_attempts_ = 0;
        pending_ring_successes_ = 0;
        pending_ring_perfects_ = 0;
        pending_ring_wait_frames_ = 0;
    };

    bool state_reloaded = false;
    if (!ram_initialized_) {
        ram_initialized_ = true;
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles_entered;
        prev_battles_won_ = battles_won;
        prev_wallet_idx_ = wallet_idx;
        prev_active_gold_ = active_gold;
    } else {
        const bool playtime_jumped = (playtime_sec + 0.5f < prev_playtime_sec_) || (playtime_sec > prev_playtime_sec_ + 60.0f);
        if (playtime_jumped || battles_entered < prev_battles_entered_ || battles_won < prev_battles_won_ ||
            ring_attempts < prev_ring_attempts_ || ring_successes < prev_ring_successes_) {
            // Save state or another save file loaded: reset session delta baselines cleanly
            state_reloaded = true;
            current_ring_streak_ = 0;
            current_perfect_streak_ = 0;
            current_flawless_battle_streak_ = 0;
            current_miss_streak_ = 0;
            ring_attempts_in_current_battle_ = 0;
            missed_in_current_battle_ = false;
            non_perfect_in_current_battle_ = false;
            pending_ring_attempts_ = 0;
            pending_ring_successes_ = 0;
            pending_ring_perfects_ = 0;
            pending_ring_wait_frames_ = 0;
        } else {
            if (battles_entered > prev_battles_entered_) {
                // New battle started: clear prior unfinished battle state
                pending_ring_attempts_ = 0;
                pending_ring_successes_ = 0;
                pending_ring_perfects_ = 0;
                pending_ring_wait_frames_ = 0;
                ring_attempts_in_current_battle_ = 0;
                missed_in_current_battle_ = false;
                non_perfect_in_current_battle_ = false;
            }
            const std::uint32_t da = (ring_attempts > prev_ring_attempts_ && ring_attempts - prev_ring_attempts_ <= 16u)
                ? (ring_attempts - prev_ring_attempts_) : 0u;
            const std::uint32_t ds = (ring_successes > prev_ring_successes_ && ring_successes - prev_ring_successes_ <= 16u)
                ? (ring_successes - prev_ring_successes_) : 0u;
            const std::uint32_t dp = (ring_perfects > prev_ring_perfects_ && ring_perfects - prev_ring_perfects_ <= 16u)
                ? (ring_perfects - prev_ring_perfects_) : 0u;

            if (da > 0u) {
                pending_ring_attempts_ += da;
                pending_ring_wait_frames_ = 2;
            }
            if (ds > 0u && pending_ring_attempts_ > 0u) pending_ring_successes_ += ds;
            if (dp > 0u && pending_ring_attempts_ > 0u) pending_ring_perfects_ += dp;

            if (pending_ring_attempts_ > 0u) {
                const bool settled_success = (pending_ring_successes_ >= pending_ring_attempts_);
                const bool battle_ended = (battles_won > prev_battles_won_);
                if (settled_success || battle_ended || pending_ring_wait_frames_ <= 0) {
                    flush_pending_rings();
                } else {
                    --pending_ring_wait_frames_;
                }
            }

            if (wallet_idx == prev_wallet_idx_ && active_gold < prev_active_gold_) {
                const std::uint64_t spent = static_cast<std::uint64_t>(prev_active_gold_ - active_gold);
                if (spent <= 99999999ull) {
                    total_gold_spent_ += spent;
                    if (spent > biggest_single_spend_) biggest_single_spend_ = spent;
                    dirty_persist = true;
                }
            }
        }
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles_entered;
        prev_wallet_idx_ = wallet_idx;
        prev_active_gold_ = active_gold;
    }

    bool panicked_and_won = false;
    if (!state_reloaded && battles_won > prev_battles_won_ && battles_won - prev_battles_won_ <= 4u) {
        const int dw = static_cast<int>(battles_won - prev_battles_won_);
        if (missed_in_current_battle_) {
            panicked_and_won = true;
            current_flawless_battle_streak_ = 0;
        } else if (ring_attempts_in_current_battle_ > 0u) {
            current_flawless_battle_streak_ += dw;
            if (current_flawless_battle_streak_ > best_flawless_battle_streak_) {
                best_flawless_battle_streak_ = current_flawless_battle_streak_;
                dirty_persist = true;
            }
        }
        if (!non_perfect_in_current_battle_ && ring_attempts_in_current_battle_ > 0) {
            ++perfect_battle_wins_;
            dirty_persist = true;
        }
        if (any_critical_hp) {
            ++clutch_wins_;
            dirty_persist = true;
        }
        if (joachim_lv >= 1 && !joachim_won_battle_) {
            joachim_won_battle_ = true;
            dirty_persist = true;
        }
        if (nicolai_active) {
            nicolai_won_battles_ += dw;
            dirty_persist = true;
        }
        if (prefix == "MI" && !mikasa_won_battle_) {
            mikasa_won_battle_ = true;
            dirty_persist = true;
        }
        ring_attempts_in_current_battle_ = 0;
        missed_in_current_battle_ = false;
        non_perfect_in_current_battle_ = false;
    }
    prev_battles_won_ = battles_won;

    // 6. Inventory / Crest count (non-zero item/crest buckets in BTL0+3260..3772) & Treasures
    int item_types_owned = 0;
    for (std::uint32_t a = 0x00100DECu; a < 0x00100FECu; a += 4u) {
        const std::uint32_t cnt = ps2::ld32(a);
        if (cnt > 0 && cnt <= 99) ++item_types_owned;
    }
    const int regions_visited = static_cast<int>(visited_regions_.size());
    const int treasures_found = popcount_range(0x00102B90u, 128u);
    const double ring_acc = ring_attempts > 0 ? (100.0 * static_cast<double>(ring_successes) / static_cast<double>(ring_attempts)) : 0.0;
    const std::uint64_t playtime_min = static_cast<std::uint64_t>(std::max(0, static_cast<int>(playtime_sec / 60.0f)));
    const std::uint64_t ring_acc_int = static_cast<std::uint64_t>(std::max(0, static_cast<int>(ring_acc)));

    // 7. Evaluate every achievement in O(1) with zero steady-state allocations & collect same-frame unlocks
    std::vector<std::size_t> frame_unlocks;
    std::string stamp;
    const auto ensure_stamp = [&]() -> const std::string & {
        if (stamp.empty()) stamp = now_timestamp();
        return stamp;
    };

    const auto pack2 = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
        return (a & 0xFFFFFFFFull) | ((b & 0xFFFFFFFFull) << 32);
    };
    const auto pack3 = [](std::uint64_t a, std::uint64_t b, std::uint64_t c) -> std::uint64_t {
        return (a & 0x1FFFFFull) | ((b & 0x1FFFFFull) << 21) | ((c & 0x3FFFFFull) << 42);
    };

    const auto eval = [&](const char *id, bool cond, std::uint64_t prog_key, double ratio, auto &&fmt_fn) {
        const auto it = id_to_index_.find(std::string_view(id));
        if (it == id_to_index_.end()) return;
        const std::size_t i = it->second;
        Entry &e = entries_[i];
        if (last_prog_key_[i] != prog_key) {
            last_prog_key_[i] = prog_key;
            e.progress_text = fmt_fn();
        }
        e.progress_ratio = e.unlocked ? 1.0 : std::clamp(ratio, 0.0, 1.0);
        if (cond && !e.unlocked) {
            e.unlocked = true;
            e.unlocked_at = ensure_stamp();
            e.progress_ratio = 1.0;
            frame_unlocks.push_back(i);
            std::fprintf(stderr, "[achievement] UNLOCKED [%s] %s\n", tier_short(e.tier), e.title.c_str());
        }
    };

    // --- Tier 1: Mark (24) ---
    eval("mark_first_blood", ring_successes >= 1, ring_successes, ring_successes >= 1 ? 1.0 : 0.0, [&] { return std::to_string(ring_successes) + " / 1 Strike"; });
    eval("mark_first_victory", battles_won >= 1, battles_won, battles_won >= 1 ? 1.0 : 0.0, [&] { return std::to_string(battles_won) + " / 1 win"; });
    eval("mark_first_spark", yuri_lv >= 1 && battles_won >= 1, yuri_lv >= 1 ? 1u : 0u, (yuri_lv >= 1 && battles_won >= 1) ? 1.0 : (yuri_lv >= 1 ? 0.5 : 0.0),
         [&] { return std::string(yuri_lv >= 1 ? "Yuri active" : "Waiting for Yuri"); });
    eval("mark_first_perfect", ring_perfects >= 1, ring_perfects, ring_perfects >= 1 ? 1.0 : 0.0, [&] { return std::to_string(ring_perfects) + " / 1 Perfect"; });
    eval("mark_five_perfects", ring_perfects >= 5, ring_perfects, static_cast<double>(ring_perfects) / 5.0, [&] { return std::to_string(ring_perfects) + " / 5 Perfects"; });
    eval("mark_getting_rhythm", best_ring_streak_ >= 3, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 3.0,
         [&] { return "Best streak: " + std::to_string(best_ring_streak_) + " / 3"; });
    eval("mark_double_perfect", best_perfect_streak_ >= 2, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 2.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 2"; });
    eval("mark_clean_sweep", best_flawless_battle_streak_ >= 1, static_cast<std::uint64_t>(best_flawless_battle_streak_), best_flawless_battle_streak_ >= 1 ? 1.0 : 0.0,
         [&] { return std::to_string(best_flawless_battle_streak_) + " / 1 flawless win"; });
    eval("mark_ring_apprentice", ring_successes >= 25, ring_successes, static_cast<double>(ring_successes) / 25.0, [&] { return std::to_string(ring_successes) + " / 25 Strikes"; });
    eval("mark_skirmisher", battles_won >= 10, battles_won, static_cast<double>(battles_won) / 10.0, [&] { return std::to_string(battles_won) + " / 10 wins"; });
    eval("mark_first_steps_lv5", max_level >= 5, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 5.0, [&] { return "Lv " + std::to_string(max_level) + " / 5"; });
    eval("mark_rising_blade", max_level >= 10, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 10.0, [&] { return "Lv " + std::to_string(max_level) + " / 10"; });
    eval("mark_seasoned_initiate", max_level >= 15, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 15.0, [&] { return "Lv " + std::to_string(max_level) + " / 15"; });
    eval("mark_first_coin", best_gold >= 2500, best_gold, static_cast<double>(best_gold) / 2500.0, [&] { return fmt_commas(best_gold) + " / 2,500 Gold"; });
    eval("mark_pocket_change", best_gold >= 10000, best_gold, static_cast<double>(best_gold) / 10000.0, [&] { return fmt_commas(best_gold) + " / 10,000 Gold"; });
    eval("mark_first_purchase", total_gold_spent_ >= 2000, total_gold_spent_, static_cast<double>(total_gold_spent_) / 2000.0, [&] { return fmt_commas(total_gold_spent_) + " / 2,000 Gold spent"; });
    eval("mark_treasure_seeker", treasures_found >= 5, static_cast<std::uint64_t>(treasures_found), static_cast<double>(treasures_found) / 5.0, [&] { return std::to_string(treasures_found) + " / 5 chests"; });
    eval("mark_fresh_ink", item_types_owned >= 12 && treasures_found >= 3,
         pack2(static_cast<std::uint64_t>(std::min(item_types_owned, 12)), static_cast<std::uint64_t>(std::min(treasures_found, 3))),
         std::min(static_cast<double>(item_types_owned) / 12.0, static_cast<double>(treasures_found) / 3.0),
         [&] { return std::to_string(std::min(item_types_owned, 12)) + "/12 items, " + std::to_string(std::min(treasures_found, 3)) + "/3 chests"; });
    eval("mark_quartermaster", item_types_owned >= 20, static_cast<std::uint64_t>(item_types_owned), static_cast<double>(item_types_owned) / 20.0,
         [&] { return std::to_string(item_types_owned) + " / 20 item types"; });
    eval("mark_karin_bond", karin_lv >= 10, static_cast<std::uint64_t>(karin_lv), static_cast<double>(karin_lv) / 10.0, [&] { return "Karin Lv " + std::to_string(karin_lv) + " / 10"; });
    eval("mark_blanca_pack", blanca_lv >= 1, blanca_lv >= 1 ? 1u : 0u, blanca_lv >= 1 ? 1.0 : 0.0, [&] { return std::string(blanca_lv >= 1 ? "Blanca recruited" : "Recruit Blanca"); });
    eval("mark_gepetto_craft", gepetto_lv >= 1, gepetto_lv >= 1 ? 1u : 0u, gepetto_lv >= 1 ? 1.0 : 0.0, [&] { return std::string(gepetto_lv >= 1 ? "Gepetto recruited" : "Recruit Gepetto"); });
    eval("mark_wanderer", regions_visited >= 2, static_cast<std::uint64_t>(regions_visited), static_cast<double>(regions_visited) / 2.0, [&] { return std::to_string(regions_visited) + " / 2 regions"; });
    eval("mark_first_hour", playtime_sec >= 3600.0f, playtime_min, static_cast<double>(playtime_sec) / 3600.0, [&] { return fmt_hours(playtime_sec) + " / 1h 00m"; });

    // --- Tier 2: Sigil (19) ---
    eval("sigil_ring_adept", ring_successes >= 100, ring_successes, static_cast<double>(ring_successes) / 100.0, [&] { return std::to_string(ring_successes) + " / 100 Strikes"; });
    eval("sigil_unbroken_six", best_ring_streak_ >= 6, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 6.0,
         [&] { return "Best streak: " + std::to_string(best_ring_streak_) + " / 6"; });
    eval("sigil_tightening_arc", ring_perfects >= 25, ring_perfects, static_cast<double>(ring_perfects) / 25.0, [&] { return std::to_string(ring_perfects) + " / 25 Perfects"; });
    eval("sigil_triple_perfect", best_perfect_streak_ >= 3, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 3.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 3"; });
    eval("sigil_battle_hardened", battles_won >= 40, battles_won, static_cast<double>(battles_won) / 40.0, [&] { return std::to_string(battles_won) + " / 40 wins"; });
    eval("sigil_flawless_five", best_flawless_battle_streak_ >= 5, static_cast<std::uint64_t>(best_flawless_battle_streak_), static_cast<double>(best_flawless_battle_streak_) / 5.0,
         [&] { return std::to_string(best_flawless_battle_streak_) + " / 5 flawless wins"; });
    eval("sigil_mercenary_vault", active_gold >= 100000, active_gold, static_cast<double>(active_gold) / 100000.0, [&] { return fmt_commas(active_gold) + " / 100,000 Gold"; });
    eval("sigil_war_chest", best_gold >= 200000, best_gold, static_cast<double>(best_gold) / 200000.0, [&] { return fmt_commas(best_gold) + " / 200,000 Gold"; });
    eval("sigil_veteran_front", max_level >= 35, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 35.0, [&] { return "Lv " + std::to_string(max_level) + " / 35"; });
    eval("sigil_covenant_elements", yuri_lv >= 30 && core_recruited >= 4,
         pack2(static_cast<std::uint64_t>(yuri_lv), static_cast<std::uint64_t>(core_recruited)),
         std::min(static_cast<double>(yuri_lv) / 30.0, static_cast<double>(core_recruited) / 4.0),
         [&] { return "Yuri Lv " + std::to_string(yuri_lv) + "/30, Party " + std::to_string(core_recruited) + "/4"; });
    eval("sigil_balanced_vanguard", core_at_25 >= 4, static_cast<std::uint64_t>(core_at_25), static_cast<double>(core_at_25) / 4.0,
         [&] { return std::to_string(core_at_25) + " / 4 members at Lv 25+"; });
    eval("sigil_lucia_tarot", lucia_lv >= 25, static_cast<std::uint64_t>(lucia_lv), static_cast<double>(lucia_lv) / 25.0, [&] { return "Lucia Lv " + std::to_string(lucia_lv) + " / 25"; });
    eval("sigil_anastasia_lens", anastasia_lv >= 25, static_cast<std::uint64_t>(anastasia_lv), static_cast<double>(anastasia_lv) / 25.0, [&] { return "Anastasia Lv " + std::to_string(anastasia_lv) + " / 25"; });
    eval("sigil_kurando_blade", kurando_lv >= 1, kurando_lv >= 1 ? 1u : 0u, kurando_lv >= 1 ? 1.0 : 0.0, [&] { return std::string(kurando_lv >= 1 ? "Kurando recruited" : "Recruit Kurando"); });
    eval("sigil_full_roster", core_recruited >= 8, static_cast<std::uint64_t>(core_recruited), static_cast<double>(core_recruited) / 8.0, [&] { return std::to_string(core_recruited) + " / 8 core members"; });
    eval("sigil_relic_hunter", treasures_found >= 30, static_cast<std::uint64_t>(treasures_found), static_cast<double>(treasures_found) / 30.0, [&] { return std::to_string(treasures_found) + " / 30 chests"; });
    eval("sigil_traveler_ledger", regions_visited >= 3, static_cast<std::uint64_t>(regions_visited), static_cast<double>(regions_visited) / 3.0, [&] { return std::to_string(regions_visited) + " / 3 regions"; });
    eval("sigil_globe_trotter", regions_visited >= 8, static_cast<std::uint64_t>(regions_visited), static_cast<double>(regions_visited) / 8.0, [&] { return std::to_string(regions_visited) + " / 8 regions"; });
    eval("sigil_ten_hours", playtime_sec >= 36000.0f, playtime_min, static_cast<double>(playtime_sec) / 36000.0, [&] { return fmt_hours(playtime_sec) + " / 10h 00m"; });

    // --- Tier 3: Crest (15) ---
    eval("crest_ring_veteran", ring_successes >= 400, ring_successes, static_cast<double>(ring_successes) / 400.0, [&] { return std::to_string(ring_successes) + " / 400 Strikes"; });
    eval("crest_razors_edge", ring_perfects >= 50, ring_perfects, static_cast<double>(ring_perfects) / 50.0, [&] { return std::to_string(ring_perfects) + " / 50 Perfects"; });
    eval("crest_century_perfects", ring_perfects >= 125, ring_perfects, static_cast<double>(ring_perfects) / 125.0, [&] { return std::to_string(ring_perfects) + " / 125 Perfects"; });
    eval("crest_relentless_chain", best_ring_streak_ >= 12, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 12.0,
         [&] { return "Best streak: " + std::to_string(best_ring_streak_) + " / 12"; });
    eval("crest_quad_perfect", best_perfect_streak_ >= 4, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 4.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 4"; });
    eval("crest_flawless_fifteen", best_flawless_battle_streak_ >= 15, static_cast<std::uint64_t>(best_flawless_battle_streak_), static_cast<double>(best_flawless_battle_streak_) / 15.0,
         [&] { return std::to_string(best_flawless_battle_streak_) + " / 15 flawless wins"; });
    eval("crest_master_grimoire", item_types_owned >= 15 && battles_won >= 75,
         pack2(static_cast<std::uint64_t>(item_types_owned), battles_won),
         std::min(static_cast<double>(item_types_owned) / 15.0, static_cast<double>(battles_won) / 75.0),
         [&] { return std::to_string(item_types_owned) + "/15 items, " + std::to_string(battles_won) + "/75 wins"; });
    eval("crest_warlord", battles_won >= 200, battles_won, static_cast<double>(battles_won) / 200.0, [&] { return std::to_string(battles_won) + " / 200 wins"; });
    eval("crest_high_roller", best_gold >= 500000, best_gold, static_cast<double>(best_gold) / 500000.0, [&] { return fmt_commas(best_gold) + " / 500,000 Gold"; });
    eval("crest_apex_vessel", max_level >= 55, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 55.0, [&] { return "Lv " + std::to_string(max_level) + " / 55"; });
    eval("crest_level_70", max_level >= 70, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 70.0, [&] { return "Lv " + std::to_string(max_level) + " / 70"; });
    eval("crest_all_party_50", core_recruited >= 8 && min_core_level >= 50, static_cast<std::uint64_t>(core_at_50), static_cast<double>(core_at_50) / 8.0,
         [&] { return std::to_string(core_at_50) + " / 8 core at Lv 50+"; });
    eval("crest_treasure_master", treasures_found >= 75, static_cast<std::uint64_t>(treasures_found), static_cast<double>(treasures_found) / 75.0, [&] { return std::to_string(treasures_found) + " / 75 chests"; });
    eval("crest_grand_cartographer", regions_visited >= 15, static_cast<std::uint64_t>(regions_visited), static_cast<double>(regions_visited) / 15.0, [&] { return std::to_string(regions_visited) + " / 15 regions"; });
    eval("crest_collector", item_types_owned >= 60, static_cast<std::uint64_t>(item_types_owned), static_cast<double>(item_types_owned) / 60.0, [&] { return std::to_string(item_types_owned) + " / 60 item types"; });

    // --- Tier 4: Sovereign (11 — True Hardcore Ceiling) ---
    eval("sovereign_tycoon", best_gold >= 2000000, best_gold, static_cast<double>(best_gold) / 2000000.0, [&] { return fmt_commas(best_gold) + " / 2,000,000 Gold"; });
    eval("sovereign_ten_million", best_gold >= 10000000, best_gold, static_cast<double>(best_gold) / 10000000.0, [&] { return fmt_commas(best_gold) + " / 10,000,000 Gold"; });
    eval("sovereign_transcendental", max_level >= 80, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 80.0, [&] { return "Lv " + std::to_string(max_level) + " / 80"; });
    eval("sovereign_level_99_party", core_recruited >= 8 && min_core_level >= 99, static_cast<std::uint64_t>(core_at_99), static_cast<double>(core_at_99) / 8.0,
         [&] { return std::to_string(core_at_99) + " / 8 core at Lv 99"; });
    eval("sovereign_clockwork_demon", best_ring_streak_ >= 20, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 20.0,
         [&] { return "Best streak: " + std::to_string(best_ring_streak_) + " / 20"; });
    eval("sovereign_fifty_streak", best_ring_streak_ >= 50, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 50.0,
         [&] { return "Best streak: " + std::to_string(best_ring_streak_) + " / 50"; });
    eval("sovereign_bare_knuckles", battles_won >= 150 && ring_acc >= 90.0, pack2(battles_won, ring_acc_int),
         std::min(static_cast<double>(battles_won) / 150.0, ring_acc / 90.0),
         [&] { return std::to_string(battles_won) + "/150 wins (" + std::to_string(static_cast<int>(ring_acc)) + "% Ring)"; });
    eval("sovereign_five_hundred_wins", battles_won >= 500, battles_won, static_cast<double>(battles_won) / 500.0, [&] { return std::to_string(battles_won) + " / 500 wins"; });
    eval("sovereign_ring_of_iron", ring_perfects >= 250 && ring_acc >= 85.0, pack2(ring_perfects, ring_acc_int),
         std::min(static_cast<double>(ring_perfects) / 250.0, ring_acc / 85.0),
         [&] { return std::to_string(ring_perfects) + "/250 Perfects (" + std::to_string(static_cast<int>(ring_acc)) + "% Ring)"; });
    eval("sovereign_thousand_perfects", ring_perfects >= 1000, ring_perfects, static_cast<double>(ring_perfects) / 1000.0, [&] { return fmt_commas(ring_perfects) + " / 1,000 Perfects"; });
    eval("sovereign_surgical_perfection", battles_won >= 250 && ring_acc >= 95.0 && best_perfect_streak_ >= 6,
         pack3(battles_won, ring_acc_int, static_cast<std::uint64_t>(best_perfect_streak_)),
         std::min({static_cast<double>(battles_won) / 250.0, ring_acc / 95.0, static_cast<double>(best_perfect_streak_) / 6.0}),
         [&] { return std::to_string(battles_won) + "/250 wins, " + std::to_string(static_cast<int>(ring_acc)) + "% acc, " + std::to_string(best_perfect_streak_) + "/6 Perf"; });

    // --- Follies (15) ---
    eval("folly_wrestling_destiny", joachim_lv >= 1 && joachim_won_battle_, (joachim_won_battle_ ? 2u : (joachim_lv >= 1 ? 1u : 0u)),
         joachim_won_battle_ ? 1.0 : (joachim_lv >= 1 ? 0.5 : 0.0),
         [&] { return std::string(joachim_lv >= 1 ? (joachim_won_battle_ ? "Joachim victorious!" : "Win a battle with Joachim") : "Recruit Joachim"); });
    eval("folly_manly_men", joachim_lv >= 40, static_cast<std::uint64_t>(joachim_lv), static_cast<double>(joachim_lv) / 40.0, [&] { return "Joachim Lv " + std::to_string(joachim_lv) + " / 40"; });
    eval("folly_decked_out", treasures_found >= 20, static_cast<std::uint64_t>(treasures_found), static_cast<double>(treasures_found) / 20.0, [&] { return std::to_string(treasures_found) + " / 20 chests"; });
    eval("folly_panic_button", panicked_and_won, panicked_and_won ? 1u : 0u, panicked_and_won ? 1.0 : 0.0, [&] { return std::string("Win a battle after missing a Ring"); });
    eval("folly_overkill_theater", max_level >= 25 && best_ring_streak_ >= 10, pack2(static_cast<std::uint64_t>(max_level), static_cast<std::uint64_t>(best_ring_streak_)),
         std::min(static_cast<double>(max_level) / 25.0, static_cast<double>(best_ring_streak_) / 10.0),
         [&] { return "Lv " + std::to_string(max_level) + "/25, Streak " + std::to_string(best_ring_streak_) + "/10"; });
    eval("folly_butterfingers", best_miss_streak_ >= 3, static_cast<std::uint64_t>(best_miss_streak_), static_cast<double>(best_miss_streak_) / 3.0,
         [&] { return "Miss streak: " + std::to_string(best_miss_streak_) + " / 3"; });
    eval("folly_tactical_retreat", battles_fled >= 10, battles_fled, static_cast<double>(battles_fled) / 10.0, [&] { return std::to_string(battles_fled) + " / 10 fled"; });
    eval("folly_hairs_breadth", clutch_wins_ >= 1, static_cast<std::uint64_t>(clutch_wins_), clutch_wins_ >= 1 ? 1.0 : 0.0, [&] { return std::to_string(clutch_wins_) + " / 1 clutch win"; });
    eval("folly_big_spender", biggest_single_spend_ >= 50000, biggest_single_spend_, static_cast<double>(biggest_single_spend_) / 50000.0,
         [&] { return fmt_commas(biggest_single_spend_) + " / 50,000 single spend"; });
    eval("folly_miser_king", active_gold >= 500000, active_gold, static_cast<double>(active_gold) / 500000.0, [&] { return fmt_commas(active_gold) + " / 500,000 wallet"; });
    eval("folly_good_boy", blanca_lv >= 50, static_cast<std::uint64_t>(blanca_lv), static_cast<double>(blanca_lv) / 50.0, [&] { return "Blanca Lv " + std::to_string(blanca_lv) + " / 50"; });
    eval("folly_doll_army", gepetto_lv >= 50, static_cast<std::uint64_t>(gepetto_lv), static_cast<double>(gepetto_lv) / 50.0, [&] { return "Gepetto Lv " + std::to_string(gepetto_lv) + " / 50"; });
    eval("folly_royal_menace", anastasia_lv >= 50, static_cast<std::uint64_t>(anastasia_lv), static_cast<double>(anastasia_lv) / 50.0, [&] { return "Anastasia Lv " + std::to_string(anastasia_lv) + " / 50"; });
    eval("folly_perfect_bout", perfect_battle_wins_ >= 5, static_cast<std::uint64_t>(perfect_battle_wins_), static_cast<double>(perfect_battle_wins_) / 5.0,
         [&] { return std::to_string(perfect_battle_wins_) + " / 5 all-Perfect battles"; });
    eval("folly_nocturne_marathon", playtime_sec >= 144000.0f, playtime_min, static_cast<double>(playtime_sec) / 144000.0, [&] { return fmt_hours(playtime_sec) + " / 40h 00m"; });

    // --- Curiosities (25: Story Milestones, Optional Dungeons & Missables) ---
    const auto eval_region = [&](const char *id, bool hit, const char *yes_txt, const char *no_txt) {
        eval(id, hit, hit ? 1u : 0u, hit ? 1.0 : 0.0, [&] { return std::string(hit ? yes_txt : no_txt); });
    };
    eval_region("curiosity_whispers_graveyard", has_region("GY"), "Soul Graveyard reached", "Enter the Soul Graveyard");
    eval("curiosity_forgotten_page", treasures_found >= 10, static_cast<std::uint64_t>(treasures_found), static_cast<double>(treasures_found) / 10.0,
         [&] { return std::to_string(treasures_found) + " / 10 hidden treasures"; });
    eval("curiosity_echoes_1915", active_disc >= 2 || regions_visited >= 5, pack2(static_cast<std::uint64_t>(active_disc), static_cast<std::uint64_t>(regions_visited)),
         active_disc >= 2 ? 1.0 : static_cast<double>(regions_visited) / 5.0,
         [&] { return "Disc " + std::to_string(active_disc) + ", " + std::to_string(regions_visited) + "/5 regions"; });
    eval("curiosity_unlikely_encounter", battles_won >= 25 && ring_perfects >= 15, pack2(battles_won, ring_perfects),
         std::min(static_cast<double>(battles_won) / 25.0, static_cast<double>(ring_perfects) / 15.0),
         [&] { return std::to_string(battles_won) + "/25 wins, " + std::to_string(ring_perfects) + "/15 Perfects"; });
    eval("curiosity_keeper_obscura", treasures_found >= 40 && regions_visited >= 6, pack2(static_cast<std::uint64_t>(treasures_found), static_cast<std::uint64_t>(regions_visited)),
         std::min(static_cast<double>(treasures_found) / 40.0, static_cast<double>(regions_visited) / 6.0),
         [&] { return std::to_string(treasures_found) + "/40 treasures, " + std::to_string(regions_visited) + "/6 regions"; });

    // Story Milestones (verified FIELD.BIN prefixes)
    eval_region("curiosity_story_domremy", has_region("DO"), "Domremy reached", "Reach Domremy");
    eval_region("curiosity_story_ardennes", has_region("AR"), "Ardennes Forest reached", "Enter Ardennes Forest");
    eval_region("curiosity_story_paris", has_region("PA"), "Paris reached", "Arrive in Paris");
    eval_region("curiosity_story_lehavre", has_region("RU") || has_region("WI"), "Le Havre reached", "Reach Le Havre");
    eval_region("curiosity_story_rhondda", has_region("RO"), "Rhondda Mine reached", "Enter Rhondda Mine");
    eval_region("curiosity_story_wales", has_region("WE"), "Wales reached", "Visit Wales");
    eval_region("curiosity_story_florence", has_region("FI"), "Florence reached", "Arrive in Florence");
    eval_region("curiosity_story_marguerite", has_region("SM"), "St. Marguerite reached", "Infiltrate St. Marguerite");
    eval_region("curiosity_story_petrograd", has_region("PE"), "Petrograd reached", "Arrive in Petrograd");
    eval_region("curiosity_story_goreme", has_region("GT") || has_region("SK"), "Goreme / Gallery reached", "Explore Goreme Valley");
    eval_region("curiosity_story_idar_flamme", has_region("IF"), "Idar Flamme reached", "Ascend Idar Flamme");
    eval_region("curiosity_story_yokohama", has_region("YO"), "Yokohama reached", "Arrive in Yokohama");
    eval_region("curiosity_story_inugami", has_region("IN"), "Inugami Village reached", "Reach Inugami Village");
    eval_region("curiosity_story_immortal_mt", has_region("FU"), "Immortal Mountain reached", "Climb Immortal Mountain");
    eval_region("curiosity_story_queens_garden", has_region("QG"), "Queen's Garden reached", "Enter Queen's Garden");

    // Optional / Secret Dungeons
    eval_region("curiosity_secret_doll_house", has_region("DH"), "Doll House discovered", "Find the Doll House");
    eval_region("curiosity_secret_dog_shrine", has_region("DB"), "Dog Shrine discovered", "Find the Dog Shrine");

    // Tagged Missables
    eval("curiosity_missable_nicolai", nicolai_won_battles_ >= 3, static_cast<std::uint64_t>(nicolai_won_battles_), static_cast<double>(nicolai_won_battles_) / 3.0,
         [&] { return std::to_string(nicolai_won_battles_) + " / 3 wins with Nicolai"; });
    eval("curiosity_missable_apoina_plunder", prefix == "AP" && treasures_found >= 4,
         pack2(prefix == "AP" ? 1u : 0u, static_cast<std::uint64_t>(treasures_found)),
         prefix == "AP" ? static_cast<double>(treasures_found) / 4.0 : 0.0,
         [&] { return prefix == "AP" ? (std::to_string(treasures_found) + " / 4 Apoina chests") : std::string("Apoina prologue chests"); });
    eval_region("curiosity_missable_mikasa", mikasa_won_battle_, "Mikasa battle won", "Win a battle aboard Mikasa");

    // --- Tier 5: Capstone ("The Covenant Sealed") ---
    {
        const int non_cap_unlocked = unlocked_count() - tier_unlocked(Tier::Covenant);
        const int non_cap_total = total_count() - tier_total(Tier::Covenant);
        eval("covenant_sealed", non_cap_unlocked >= non_cap_total && non_cap_total > 0,
             pack2(static_cast<std::uint64_t>(non_cap_unlocked), static_cast<std::uint64_t>(non_cap_total)),
             non_cap_total > 0 ? static_cast<double>(non_cap_unlocked) / static_cast<double>(non_cap_total) : 0.0,
             [&] { return std::to_string(non_cap_unlocked) + " / " + std::to_string(non_cap_total) + " achievements"; });
    }

    // 8. Smart Burst Coalescing & Single Batched Save
    if (!frame_unlocks.empty()) {
        if (frame_unlocks.size() <= 3u) {
            for (std::size_t idx : frame_unlocks) {
                ToastItem item;
                item.is_burst = false;
                item.entry_idx = idx;
                item.serial = next_toast_serial_++;
                toast_queue_.push_back(item);
            }
        } else {
            Tier best_tier = Tier::Mark;
            const std::string burst_shot = "Unlocked " + std::to_string(frame_unlocks.size()) + " Achievements";
            for (std::size_t idx : frame_unlocks) {
                entries_[idx].screenshot_title = burst_shot;
                if (tier_priority(entries_[idx].tier) > tier_priority(best_tier))
                    best_tier = entries_[idx].tier;
            }
            ToastItem burst;
            burst.is_burst = true;
            burst.entry_idx = frame_unlocks.front();
            burst.burst_count = static_cast<int>(frame_unlocks.size());
            burst.burst_tier = best_tier;
            burst.serial = next_toast_serial_++;
            toast_queue_.push_back(burst);
            std::fprintf(stderr, "[achievement] BURST SUMMARY: %d achievements unlocked simultaneously\n", burst.burst_count);
        }
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    } else if (dirty_persist) {
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    }
}

void Manager::poll_ram_sh1() {
    // Verify Shadow Hearts (SLUS-20347) loaded ELF signature + live save block at 0x00100000.
    static constexpr const char kSig1[] = "Changchun";
    static constexpr const char kSig2[] = "Trans-Siberian Express";
    for (std::uint32_t i = 0; kSig1[i] != '\0'; ++i)
        if (ps2::ld8(0x004CAE40u + i) != static_cast<std::uint8_t>(kSig1[i])) return;
    for (std::uint32_t i = 0; kSig2[i] != '\0'; ++i)
        if (ps2::ld8(0x004CAE50u + i) != static_cast<std::uint8_t>(kSig2[i])) return;

    // Require active gameplay: Yuri joined (bit 6 of 0x00100280) and playtime >= 1s (ms at 0x0010000C).
    if ((ps2::ld8(0x00100280u) & 0x40u) == 0u) return;
    const std::uint32_t pt_ms = ps2::ld32(0x0010000Cu);
    if (pt_ms < 1000u || pt_ms > 3600000000u) return;
    const float playtime_sec = static_cast<float>(pt_ms) / 1000.0f;

    bool dirty_persist = false;

    // 1. Cash (0x00100008 u32)
    const std::uint32_t cash = ps2::ld32(0x00100008u);

    // 2. Party Roster (6 chars at 0x00100280, stride 0x50: 0=Yuri, 1=Alice, 2=Zhuzhen, 3=Margarete, 4=Keith, 5=Halley)
    std::array<int, 6> lv{};
    int max_level = 0, recruited = 0, at_45 = 0, at_99 = 0;
    for (int i = 0; i < 6; ++i) {
        const std::uint32_t base = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
        const bool joined = (ps2::ld8(base) & 0x40u) != 0u;
        const std::uint8_t c_lv = ps2::ld8(base + 0x03u);
        if (!joined || c_lv == 0u || c_lv > 99u) continue;
        lv[i] = static_cast<int>(c_lv);
        ++recruited;
        if (lv[i] > max_level) max_level = lv[i];
        if (lv[i] >= 45) ++at_45;
        if (lv[i] >= 99) ++at_99;
    }
    if (recruited <= 0) return;

    // 3. Battle, Judgment Ring, Berserk/KO & Shop Ring Stats (verified from ScoreMenu::CalcRank / CBattle)
    const std::uint32_t battles = ps2::ld32(0x001014A4u);
    const std::uint32_t escapes = ps2::ld32(0x001014A8u);
    const std::uint32_t ring_attempts = ps2::ld32(0x001014ACu);
    const std::uint32_t ring_successes = ps2::ld32(0x001014B0u);
    const std::uint32_t ring_perfects = ps2::ld32(0x001014B4u);
    const std::uint32_t berserk_or_ko = ps2::ld32(0x001014BCu) + ps2::ld32(0x001014C0u);
    const std::uint32_t shop_discounts = ps2::ld32(0x00101528u);
    const std::uint32_t shop_raises = ps2::ld32(0x00101530u);

    // 4. Canonical World Area Tracking (0x00100028 u16, normalized across multi-ID revisits)
    const std::uint16_t raw_area = ps2::ld16(0x00100028u);
    auto canonical_area_code = [](std::uint16_t id) -> const char * {
        switch (id) {
        case 0: return "A00";  // Changchun
        case 1: return "A01";  // Trans-Siberian Express
        case 2: return "A02";  // Plains
        case 3: return "A03";  // Zhaoyang Village
        case 4: return "A04";  // Fengtian
        case 5: return "A05";  // Dalian
        case 6: return "A06";  // Shanghai
        case 7: return "A07";  // Temple Ruins
        case 8: case 23: case 25: return "A08";  // Huayuan
        case 9: return "A09";  // Kuihai Tower
        case 10: case 15: return "A10";  // London
        case 11: case 32: return "A11";  // Rouen
        case 12: return "A12";  // Prague
        case 13: case 22: return "A13";  // Blue Castle
        case 14: case 26: case 31: return "A14";  // Bistritz
        case 16: return "A16";  // Calios Mental Hospital
        case 18: return "A18";  // Nemeton Monastery
        case 19: return "A19";  // Old Castle Street
        case 20: return "A20";  // Orphanage
        case 21: return "A21";  // Smuggler's Boat
        case 24: return "A24";  // Kowloon Fortress
        case 28: return "A28";  // Ancient Ruins
        case 29: return "A29";  // Cave Temple
        case 30: return "A30";  // Dollhouse
        default: return nullptr;
        }
    };
    const auto has_area = [&](std::string_view code) -> bool {
        for (const std::string &r : visited_regions_)
            if (r == code) return true;
        return false;
    };
    if (const char *code = canonical_area_code(raw_area)) {
        if (!has_area(code)) {
            visited_regions_.emplace_back(code);
            dirty_persist = true;
        }
    }

    // 5. Live Streak, Spending & Battle Delta Detection (with split-vblank Ring settle window)
    auto flush_pending_rings = [&]() {
        if (pending_ring_attempts_ == 0u) return;
        const std::uint32_t da = pending_ring_attempts_;
        const std::uint32_t ds = std::min(pending_ring_successes_, da);
        const std::uint32_t dp = std::min(pending_ring_perfects_, da);
        ring_attempts_in_current_battle_ += da;
        if (ds >= da) {
            current_ring_streak_ += static_cast<int>(ds);
            current_miss_streak_ = 0;
            if (current_ring_streak_ > best_ring_streak_) {
                best_ring_streak_ = current_ring_streak_;
                dirty_persist = true;
            }
        } else {
            current_ring_streak_ = 0;
            current_miss_streak_ += static_cast<int>(da - ds);
            if (current_miss_streak_ > best_miss_streak_) {
                best_miss_streak_ = current_miss_streak_;
                dirty_persist = true;
            }
            missed_in_current_battle_ = true;
        }
        if (dp >= da) {
            current_perfect_streak_ += static_cast<int>(dp);
            if (current_perfect_streak_ > best_perfect_streak_) {
                best_perfect_streak_ = current_perfect_streak_;
                dirty_persist = true;
            }
        } else {
            current_perfect_streak_ = 0;
            non_perfect_in_current_battle_ = true;
        }
        pending_ring_attempts_ = 0;
        pending_ring_successes_ = 0;
        pending_ring_perfects_ = 0;
        pending_ring_wait_frames_ = 0;
    };

    bool clean_battle_won = false;
    bool panicked_and_won = false;
    bool state_reloaded = false;
    if (!ram_initialized_) {
        ram_initialized_ = true;
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles;
        prev_battles_won_ = battles >= escapes ? (battles - escapes) : 0u;
        prev_active_gold_ = cash;
    } else {
        const std::uint32_t cur_completed = battles >= escapes ? (battles - escapes) : 0u;
        const bool playtime_jumped = (playtime_sec + 0.5f < prev_playtime_sec_) || (playtime_sec > prev_playtime_sec_ + 60.0f);
        if (playtime_jumped || battles < prev_battles_entered_ || ring_attempts < prev_ring_attempts_ ||
            ring_successes < prev_ring_successes_) {
            state_reloaded = true;
            current_ring_streak_ = 0;
            current_perfect_streak_ = 0;
            current_flawless_battle_streak_ = 0;
            current_miss_streak_ = 0;
            ring_attempts_in_current_battle_ = 0;
            missed_in_current_battle_ = false;
            non_perfect_in_current_battle_ = false;
            pending_ring_attempts_ = 0;
            pending_ring_successes_ = 0;
            pending_ring_perfects_ = 0;
            pending_ring_wait_frames_ = 0;
        } else {
            const std::uint32_t da = (ring_attempts > prev_ring_attempts_ && ring_attempts - prev_ring_attempts_ <= 16u)
                ? (ring_attempts - prev_ring_attempts_) : 0u;
            const std::uint32_t ds = (ring_successes > prev_ring_successes_ && ring_successes - prev_ring_successes_ <= 16u)
                ? (ring_successes - prev_ring_successes_) : 0u;
            const std::uint32_t dp = (ring_perfects > prev_ring_perfects_ && ring_perfects - prev_ring_perfects_ <= 16u)
                ? (ring_perfects - prev_ring_perfects_) : 0u;

            if (da > 0u) {
                pending_ring_attempts_ += da;
                pending_ring_wait_frames_ = 2;
            }
            if (ds > 0u && pending_ring_attempts_ > 0u) pending_ring_successes_ += ds;
            if (dp > 0u && pending_ring_attempts_ > 0u) pending_ring_perfects_ += dp;

            const bool battle_finished = (cur_completed > prev_battles_won_);
            if (pending_ring_attempts_ > 0u) {
                const bool settled_success = (pending_ring_successes_ >= pending_ring_attempts_);
                if (settled_success || battle_finished || pending_ring_wait_frames_ <= 0) {
                    flush_pending_rings();
                } else {
                    --pending_ring_wait_frames_;
                }
            }

            if (cash < prev_active_gold_) {
                const std::uint64_t spent = static_cast<std::uint64_t>(prev_active_gold_ - cash);
                if (spent > 0 && spent < 50000000u) {
                    total_gold_spent_ += spent;
                    if (spent > biggest_single_spend_) biggest_single_spend_ = spent;
                    dirty_persist = true;
                }
            }

            if (battle_finished) {
                if (ring_attempts_in_current_battle_ > 0u && !missed_in_current_battle_) {
                    clean_battle_won = true;
                    ++current_flawless_battle_streak_;
                    if (current_flawless_battle_streak_ > best_flawless_battle_streak_) {
                        best_flawless_battle_streak_ = current_flawless_battle_streak_;
                        dirty_persist = true;
                    }
                    if (!non_perfect_in_current_battle_) {
                        ++perfect_battle_wins_;
                        dirty_persist = true;
                    }
                } else if (missed_in_current_battle_) {
                    panicked_and_won = true;
                    current_flawless_battle_streak_ = 0;
                }
                ring_attempts_in_current_battle_ = 0;
                missed_in_current_battle_ = false;
                non_perfect_in_current_battle_ = false;
            }
        }
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles;
        prev_battles_won_ = cur_completed;
        prev_active_gold_ = cash;
    }
    if (state_reloaded) {
        if (dirty_persist) save();
        return;
    }

    const std::uint32_t battles_won = battles >= escapes ? (battles - escapes) : 0u;
    const double ring_acc = ring_attempts > 0u ? (100.0 * static_cast<double>(ring_successes) / static_cast<double>(ring_attempts)) : 0.0;
    const std::uint64_t ring_acc_int = static_cast<std::uint64_t>(std::max(0, static_cast<int>(ring_acc)));
    const std::uint64_t playtime_min = static_cast<std::uint64_t>(std::max(0, static_cast<int>(playtime_sec / 60.0f)));
    const int areas_visited = static_cast<int>(visited_regions_.size());

    // 6. Evaluate all 75 Shadow Hearts (1913) Achievements
    std::vector<std::size_t> frame_unlocks;
    std::string stamp;
    auto eval = [&](const char *id, bool cond, std::uint64_t prog_key, double ratio, auto &&make_prog) {
        auto it = id_to_index_.find(std::string_view(id));
        if (it == id_to_index_.end()) return;
        const std::size_t idx = it->second;
        Entry &e = entries_[idx];
        if (e.unlocked) return;
        if (last_prog_key_[idx] != prog_key) {
            last_prog_key_[idx] = prog_key;
            e.progress_ratio = std::clamp(ratio, 0.0, 1.0);
            e.progress_text = make_prog();
        }
        if (cond) {
            e.unlocked = true;
            e.progress_ratio = 1.0;
            if (stamp.empty()) stamp = now_timestamp();
            e.unlocked_at = stamp;
            e.screenshot_title = e.title;
            frame_unlocks.push_back(idx);
        }
    };
    auto pack2 = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t { return (a << 32) | (b & 0xFFFFFFFFull); };

    // --- Tier 1: Mark (16) ---
    eval("sh1_mark_first_strike", ring_successes >= 1u, ring_successes, ring_successes >= 1u ? 1.0 : 0.0, [&] { return std::to_string(ring_successes) + " / 1 Strike"; });
    eval("sh1_mark_first_battle", battles_won >= 1u, battles_won, battles_won >= 1u ? 1.0 : 0.0, [&] { return std::to_string(battles_won) + " / 1 battle"; });
    eval("sh1_mark_first_perfect", ring_perfects >= 1u, ring_perfects, ring_perfects >= 1u ? 1.0 : 0.0, [&] { return std::to_string(ring_perfects) + " / 1 Perfect"; });
    eval("sh1_mark_five_perfects", ring_perfects >= 5u, ring_perfects, static_cast<double>(ring_perfects) / 5.0, [&] { return std::to_string(ring_perfects) + " / 5 Perfects"; });
    eval("sh1_mark_three_streak", best_ring_streak_ >= 3, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 3.0,
         [&] { return "Streak: " + std::to_string(best_ring_streak_) + " / 3"; });
    eval("sh1_mark_double_perfect", best_perfect_streak_ >= 2, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 2.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 2"; });
    eval("sh1_mark_clean_battle", clean_battle_won || best_flawless_battle_streak_ >= 1, static_cast<std::uint64_t>(best_flawless_battle_streak_),
         (clean_battle_won || best_flawless_battle_streak_ >= 1) ? 1.0 : 0.0, [&] { return std::string("Finish a battle with 0 misses"); });
    eval("sh1_mark_ring_25", ring_successes >= 25u, ring_successes, static_cast<double>(ring_successes) / 25.0, [&] { return std::to_string(ring_successes) + " / 25 Strikes"; });
    eval("sh1_mark_battles_10", battles_won >= 10u, battles_won, static_cast<double>(battles_won) / 10.0, [&] { return std::to_string(battles_won) + " / 10 battles"; });
    eval("sh1_mark_lv5", max_level >= 5, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 5.0, [&] { return "Lv " + std::to_string(max_level) + " / 5"; });
    eval("sh1_mark_lv10", max_level >= 10, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 10.0, [&] { return "Lv " + std::to_string(max_level) + " / 10"; });
    eval("sh1_mark_lv15", max_level >= 15, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 15.0, [&] { return "Lv " + std::to_string(max_level) + " / 15"; });
    eval("sh1_mark_cash_2500", cash >= 2500u, cash, static_cast<double>(cash) / 2500.0, [&] { return fmt_commas(cash) + " / 2,500 Cash"; });
    eval("sh1_mark_cash_10000", cash >= 10000u, cash, static_cast<double>(cash) / 10000.0, [&] { return fmt_commas(cash) + " / 10,000 Cash"; });
    eval("sh1_mark_shop_discount", shop_discounts >= 1u, shop_discounts, shop_discounts >= 1u ? 1.0 : 0.0, [&] { return std::to_string(shop_discounts) + " / 1 discount"; });
    eval("sh1_mark_first_hour", playtime_sec >= 3600.0f, playtime_min, static_cast<double>(playtime_sec) / 3600.0, [&] { return fmt_hours(playtime_sec) + " / 1h 00m"; });

    // --- Tier 2: Sigil (14) ---
    eval("sh1_sigil_alice", lv[1] >= 15, static_cast<std::uint64_t>(lv[1]), static_cast<double>(lv[1]) / 15.0, [&] { return "Alice Lv " + std::to_string(lv[1]) + " / 15"; });
    eval("sh1_sigil_zhuzhen", lv[2] >= 1, static_cast<std::uint64_t>(lv[2]), lv[2] >= 1 ? 1.0 : 0.0, [&] { return std::string(lv[2] >= 1 ? "Zhuzhen joined" : "Recruit Zhuzhen"); });
    eval("sh1_sigil_margarete", lv[3] >= 1, static_cast<std::uint64_t>(lv[3]), lv[3] >= 1 ? 1.0 : 0.0, [&] { return std::string(lv[3] >= 1 ? "Margarete joined" : "Recruit Margarete"); });
    eval("sh1_sigil_keith", lv[4] >= 1, static_cast<std::uint64_t>(lv[4]), lv[4] >= 1 ? 1.0 : 0.0, [&] { return std::string(lv[4] >= 1 ? "Keith joined" : "Recruit Keith"); });
    eval("sh1_sigil_halley", lv[5] >= 1, static_cast<std::uint64_t>(lv[5]), lv[5] >= 1 ? 1.0 : 0.0, [&] { return std::string(lv[5] >= 1 ? "Halley joined" : "Recruit Halley"); });
    eval("sh1_sigil_full_party", recruited >= 6, static_cast<std::uint64_t>(recruited), static_cast<double>(recruited) / 6.0, [&] { return std::to_string(recruited) + " / 6 party members"; });
    eval("sh1_sigil_ring_100", ring_successes >= 100u, ring_successes, static_cast<double>(ring_successes) / 100.0, [&] { return std::to_string(ring_successes) + " / 100 Strikes"; });
    eval("sh1_sigil_perfect_25", ring_perfects >= 25u, ring_perfects, static_cast<double>(ring_perfects) / 25.0, [&] { return std::to_string(ring_perfects) + " / 25 Perfects"; });
    eval("sh1_sigil_streak_6", best_ring_streak_ >= 6, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 6.0,
         [&] { return "Streak: " + std::to_string(best_ring_streak_) + " / 6"; });
    eval("sh1_sigil_triple_perfect", best_perfect_streak_ >= 3, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 3.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 3"; });
    eval("sh1_sigil_battles_40", battles_won >= 40u, battles_won, static_cast<double>(battles_won) / 40.0, [&] { return std::to_string(battles_won) + " / 40 battles"; });
    eval("sh1_sigil_flawless_5", best_flawless_battle_streak_ >= 5, static_cast<std::uint64_t>(best_flawless_battle_streak_), static_cast<double>(best_flawless_battle_streak_) / 5.0,
         [&] { return std::to_string(best_flawless_battle_streak_) + " / 5 flawless battles"; });
    eval("sh1_sigil_lv30", max_level >= 30, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 30.0, [&] { return "Lv " + std::to_string(max_level) + " / 30"; });
    eval("sh1_sigil_ten_hours", playtime_sec >= 36000.0f, playtime_min, static_cast<double>(playtime_sec) / 36000.0, [&] { return fmt_hours(playtime_sec) + " / 10h 00m"; });

    // --- Tier 3: Crest (11) ---
    eval("sh1_crest_ring_300", ring_successes >= 300u, ring_successes, static_cast<double>(ring_successes) / 300.0, [&] { return std::to_string(ring_successes) + " / 300 Strikes"; });
    eval("sh1_crest_perfect_100", ring_perfects >= 100u, ring_perfects, static_cast<double>(ring_perfects) / 100.0, [&] { return std::to_string(ring_perfects) + " / 100 Perfects"; });
    eval("sh1_crest_streak_12", best_ring_streak_ >= 12, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 12.0,
         [&] { return "Streak: " + std::to_string(best_ring_streak_) + " / 12"; });
    eval("sh1_crest_quad_perfect", best_perfect_streak_ >= 4, static_cast<std::uint64_t>(best_perfect_streak_), static_cast<double>(best_perfect_streak_) / 4.0,
         [&] { return "Perfect streak: " + std::to_string(best_perfect_streak_) + " / 4"; });
    eval("sh1_crest_flawless_12", best_flawless_battle_streak_ >= 12, static_cast<std::uint64_t>(best_flawless_battle_streak_), static_cast<double>(best_flawless_battle_streak_) / 12.0,
         [&] { return std::to_string(best_flawless_battle_streak_) + " / 12 flawless battles"; });
    eval("sh1_crest_battles_150", battles_won >= 150u, battles_won, static_cast<double>(battles_won) / 150.0, [&] { return std::to_string(battles_won) + " / 150 battles"; });
    eval("sh1_crest_lv50", max_level >= 50, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 50.0, [&] { return "Lv " + std::to_string(max_level) + " / 50"; });
    eval("sh1_crest_party_45", recruited >= 6 && at_45 >= 6, static_cast<std::uint64_t>(at_45), static_cast<double>(at_45) / 6.0, [&] { return std::to_string(at_45) + " / 6 party at Lv 45+"; });
    eval("sh1_crest_cash_250k", cash >= 250000u, cash, static_cast<double>(cash) / 250000.0, [&] { return fmt_commas(cash) + " / 250,000 Cash"; });
    eval("sh1_crest_discounts_25", shop_discounts >= 25u, shop_discounts, static_cast<double>(shop_discounts) / 25.0, [&] { return std::to_string(shop_discounts) + " / 25 discounts"; });
    eval("sh1_crest_areas_12", areas_visited >= 12, static_cast<std::uint64_t>(areas_visited), static_cast<double>(areas_visited) / 12.0, [&] { return std::to_string(areas_visited) + " / 12 locations"; });

    // --- Tier 4: Sovereign (8) ---
    eval("sh1_sovereign_battles_500", battles_won >= 500u, battles_won, static_cast<double>(battles_won) / 500.0, [&] { return std::to_string(battles_won) + " / 500 battles"; });
    eval("sh1_sovereign_perfect_500", ring_perfects >= 500u, ring_perfects, static_cast<double>(ring_perfects) / 500.0, [&] { return std::to_string(ring_perfects) + " / 500 Perfects"; });
    eval("sh1_sovereign_streak_20", best_ring_streak_ >= 20, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 20.0,
         [&] { return "Streak: " + std::to_string(best_ring_streak_) + " / 20"; });
    eval("sh1_sovereign_streak_50", best_ring_streak_ >= 50, static_cast<std::uint64_t>(best_ring_streak_), static_cast<double>(best_ring_streak_) / 50.0,
         [&] { return "Streak: " + std::to_string(best_ring_streak_) + " / 50"; });
    eval("sh1_sovereign_accuracy_90", battles_won >= 150u && ring_acc >= 90.0, pack2(battles_won, ring_acc_int),
         std::min(static_cast<double>(battles_won) / 150.0, ring_acc / 90.0),
         [&] { return std::to_string(battles_won) + "/150 battles (" + std::to_string(static_cast<int>(ring_acc)) + "% Ring)"; });
    eval("sh1_sovereign_lv75", max_level >= 75, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 75.0, [&] { return "Lv " + std::to_string(max_level) + " / 75"; });
    eval("sh1_sovereign_party_99", recruited >= 6 && at_99 >= 6, static_cast<std::uint64_t>(at_99), static_cast<double>(at_99) / 6.0, [&] { return std::to_string(at_99) + " / 6 party at Lv 99"; });
    eval("sh1_sovereign_cash_1m", cash >= 1000000u, cash, static_cast<double>(cash) / 1000000.0, [&] { return fmt_commas(cash) + " / 1,000,000 Cash"; });

    // --- Follies (10) ---
    eval("sh1_folly_butterfingers", best_miss_streak_ >= 3, static_cast<std::uint64_t>(best_miss_streak_), static_cast<double>(best_miss_streak_) / 3.0,
         [&] { return "Miss streak: " + std::to_string(best_miss_streak_) + " / 3"; });
    eval("sh1_folly_runaway", escapes >= 10u, escapes, static_cast<double>(escapes) / 10.0, [&] { return std::to_string(escapes) + " / 10 escapes"; });
    eval("sh1_folly_berserk", berserk_or_ko >= 5u, berserk_or_ko, static_cast<double>(berserk_or_ko) / 5.0, [&] { return std::to_string(berserk_or_ko) + " / 5 Berserks or KOs"; });
    eval("sh1_folly_haggler", shop_raises >= 1u, shop_raises, shop_raises >= 1u ? 1.0 : 0.0, [&] { return std::to_string(shop_raises) + " / 1 price raise"; });
    eval("sh1_folly_big_spend", biggest_single_spend_ >= 25000u, biggest_single_spend_, static_cast<double>(biggest_single_spend_) / 25000.0,
         [&] { return fmt_commas(biggest_single_spend_) + " / 25,000 single spend"; });
    eval("sh1_folly_panic_win", panicked_and_won, panicked_and_won ? 1u : 0u, panicked_and_won ? 1.0 : 0.0, [&] { return std::string("Finish a battle after missing a Ring"); });
    eval("sh1_folly_perfect_bout", perfect_battle_wins_ >= 5, static_cast<std::uint64_t>(perfect_battle_wins_), static_cast<double>(perfect_battle_wins_) / 5.0,
         [&] { return std::to_string(perfect_battle_wins_) + " / 5 all-Perfect battles"; });
    eval("sh1_folly_overkill", max_level >= 25 && best_ring_streak_ >= 10, pack2(static_cast<std::uint64_t>(max_level), static_cast<std::uint64_t>(best_ring_streak_)),
         std::min(static_cast<double>(max_level) / 25.0, static_cast<double>(best_ring_streak_) / 10.0),
         [&] { return "Lv " + std::to_string(max_level) + "/25, Streak " + std::to_string(best_ring_streak_) + "/10"; });
    eval("sh1_folly_hoarder", cash >= 500000u, cash, static_cast<double>(cash) / 500000.0, [&] { return fmt_commas(cash) + " / 500,000 Cash"; });
    eval("sh1_folly_marathon", playtime_sec >= 108000.0f, playtime_min, static_cast<double>(playtime_sec) / 108000.0, [&] { return fmt_hours(playtime_sec) + " / 30h 00m"; });

    // --- Curiosities (15 — Secret Until Unlocked) ---
    const auto eval_area = [&](const char *id, bool hit, const char *yes_txt, const char *no_txt) {
        eval(id, hit, hit ? 1u : 0u, hit ? 1.0 : 0.0, [&] { return std::string(hit ? yes_txt : no_txt); });
    };
    eval_area("sh1_curiosity_train", has_area("A01"), "Trans-Siberian Express boarded", "Board the Trans-Siberian Express");
    eval_area("sh1_curiosity_zhaoyang", has_area("A03"), "Zhaoyang Village reached", "Reach Zhaoyang Village");
    eval_area("sh1_curiosity_fengtian", has_area("A04"), "Fengtian reached", "Arrive in Fengtian");
    eval_area("sh1_curiosity_dalian", has_area("A05"), "Dalian reached", "Arrive in Dalian");
    eval_area("sh1_curiosity_shanghai", has_area("A06"), "Shanghai reached", "Arrive in Shanghai");
    eval_area("sh1_curiosity_temple_ruins", has_area("A07"), "Temple Ruins reached", "Explore Temple Ruins");
    eval_area("sh1_curiosity_kuihai", has_area("A09"), "Kuihai Tower reached", "Ascend Kuihai Tower");
    eval_area("sh1_curiosity_london", has_area("A10"), "London reached", "Cross into London");
    eval_area("sh1_curiosity_rouen", has_area("A11"), "Rouen reached", "Arrive in Rouen");
    eval_area("sh1_curiosity_prague", has_area("A12"), "Prague reached", "Arrive in Prague");
    eval_area("sh1_curiosity_blue_castle", has_area("A13"), "Blue Castle reached", "Enter Blue Castle");
    eval_area("sh1_curiosity_calios", has_area("A16"), "Calios Mental Hospital reached", "Enter Calios Mental Hospital");
    eval_area("sh1_curiosity_nemeton", has_area("A18"), "Nemeton Monastery reached", "Reach Nemeton Monastery");
    eval_area("sh1_curiosity_kowloon", has_area("A24"), "Kowloon Fortress reached", "Infiltrate Kowloon Fortress");
    eval_area("sh1_curiosity_secret_dungeons", has_area("A28") || has_area("A29") || has_area("A30"),
              "Optional trial discovered", "Discover Ancient Ruins, Cave Temple, or Dollhouse");

    // --- Tier 5: Capstone ("Harmonixer of Destiny") ---
    {
        const int non_cap_unlocked = unlocked_count() - tier_unlocked(Tier::Covenant);
        const int non_cap_total = total_count() - tier_total(Tier::Covenant);
        eval("sh1_shadow_hearts_sealed", non_cap_unlocked >= non_cap_total && non_cap_total > 0,
             pack2(static_cast<std::uint64_t>(non_cap_unlocked), static_cast<std::uint64_t>(non_cap_total)),
             non_cap_total > 0 ? static_cast<double>(non_cap_unlocked) / static_cast<double>(non_cap_total) : 0.0,
             [&] { return std::to_string(non_cap_unlocked) + " / " + std::to_string(non_cap_total) + " achievements"; });
    }

    // 7. Smart Burst Coalescing & Single Batched Save
    if (!frame_unlocks.empty()) {
        if (frame_unlocks.size() <= 3u) {
            for (std::size_t idx : frame_unlocks) {
                ToastItem item;
                item.is_burst = false;
                item.entry_idx = idx;
                item.serial = next_toast_serial_++;
                toast_queue_.push_back(item);
            }
        } else {
            Tier best_tier = Tier::Mark;
            const std::string burst_shot = "Unlocked " + std::to_string(frame_unlocks.size()) + " Achievements";
            for (std::size_t idx : frame_unlocks) {
                entries_[idx].screenshot_title = burst_shot;
                if (tier_priority(entries_[idx].tier) > tier_priority(best_tier))
                    best_tier = entries_[idx].tier;
            }
            ToastItem burst;
            burst.is_burst = true;
            burst.entry_idx = frame_unlocks.front();
            burst.burst_count = static_cast<int>(frame_unlocks.size());
            burst.burst_tier = best_tier;
            burst.serial = next_toast_serial_++;
            toast_queue_.push_back(burst);
            std::fprintf(stderr, "[achievement] BURST SUMMARY: %d achievements unlocked simultaneously\n", burst.burst_count);
        }
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    } else if (dirty_persist) {
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    }
}

void Manager::poll_ram_sh3() {
    // 1. Verify active NTSC-U SLUS-21326 SH3CMNS + FLDa + BTL  + MENU live save block at 0x00100000.
    static constexpr char kMagic[8] = {'S', 'H', '3', 'C', 'M', 'N', 'S', '\0'};
    for (int i = 0; i < 8; ++i)
        if (ps2::ld8(0x0010001Du + static_cast<std::uint32_t>(i)) != static_cast<std::uint8_t>(kMagic[i])) return;
    if (ps2::ld32(0x00100100u) != 0x61444C46u || ps2::ld32(0x00100D40u) != 0x204C5442u ||
        ps2::ld32(0x00103B50u) != 0x554E454Du) return;

    // Require active gameplay (playtime >= 1.0s and < 1000 hours).
    const std::uint32_t playtime_ms = ps2::ld32(0x00100098u);
    if (playtime_ms < 1000u || playtime_ms > 3600000000u) return;
    const float playtime_sec = static_cast<float>(playtime_ms) / 1000.0f;

    // 2. Active Cash (BTL + 0x14 = 0x00100D54)
    const std::uint32_t cash = ps2::ld32(0x00100D54u);
    if (cash > 999999999u) return;

    // 3. Party Roster & Character Stats:
    //    7 party slots at 0x00100D70 (stride 0x84):
    //      0=Johnny, 1=Shania, 2=Natan, 3=Frank, 4=Mao, 5=Hilda, 6=Ricardo
    //      +0x03 = lv, +0x0A = name, +0x1C = hp_cur, +0x1E = hp_max
    //    MENU team rosters (Team A @ 0x00103B60, Team B @ 0x00103B88, Team C @ 0x00103BB0,
    //    Active Battle Team @ 0x00103C68 — 8 bytes each, 0..6 = char_id, 0xFF = empty).
    std::array<bool, 7> recruited{};
    static constexpr std::uint32_t kRosterAddrs[4] = {0x00103B60u, 0x00103B88u, 0x00103BB0u, 0x00103C68u};
    for (std::uint32_t raddr : kRosterAddrs) {
        for (std::uint32_t j = 0; j < 8u; ++j) {
            const std::uint8_t cid = ps2::ld8(raddr + j);
            if (cid < 7u) recruited[cid] = true;
        }
    }

    // 4. Verified BattleRecord struct at 0x00102710 (current playthrough; cumulative mirror at 0x00102D10):
    //    +0x00 (0x00102710): battles_entered
    //    +0x08 (0x00102718): max_single_damage
    //    +0x0C (0x0010271C): enemies_defeated
    //    +0x18 (0x00102728): battles_fled (escapes)
    //    +0x24 (0x00102734): battles_won (victories)
    //    +0x54 (0x00102764): max_combo_hits
    //    +0x68 (0x00102778 + i*0x90, i=0..6): per-character stats:
    //      +0x00 = battles_participated, +0x0C = ring_attempts, +0x18 = ring_successes, +0x1C = ring_perfects
    const std::uint32_t battles_entered = ps2::ld32(0x00102710u);
    const std::uint32_t max_single_damage = ps2::ld32(0x00102718u);
    const std::uint32_t enemies_defeated = ps2::ld32(0x0010271Cu);
    const std::uint32_t battles_fled = ps2::ld32(0x00102728u);
    const std::uint32_t battles_won = ps2::ld32(0x00102734u);
    const std::uint32_t max_combo_hits = ps2::ld32(0x00102764u);

    std::uint32_t ring_attempts = 0, ring_successes = 0, ring_perfects = 0;
    for (int i = 0; i < 7; ++i) {
        const std::uint32_t s2 = 0x00102778u + static_cast<std::uint32_t>(i) * 0x90u;
        if (ps2::ld32(s2) > 0u) recruited[i] = true;
        ring_attempts += ps2::ld32(s2 + 0x0Cu);
        ring_successes += ps2::ld32(s2 + 0x18u);
        ring_perfects += ps2::ld32(s2 + 0x1Cu);
    }

    int max_level = 0, active_members = 0, party_at_45 = 0, party_at_99 = 0;
    std::array<int, 7> char_lv{};
    bool any_critical_hp = false;
    for (int i = 0; i < 7; ++i) {
        const std::uint32_t base = 0x00100D70u + static_cast<std::uint32_t>(i) * 0x84u;
        const std::uint8_t lv = ps2::ld8(base + 0x03u);
        const std::uint8_t c0 = ps2::ld8(base + 0x0Au);
        if (c0 == 0u || lv == 0u || lv > 99u) continue;
        if (!recruited[i]) continue;
        ++active_members;
        char_lv[i] = static_cast<int>(lv);
        if (char_lv[i] > max_level) max_level = char_lv[i];
        if (char_lv[i] >= 45) ++party_at_45;
        if (char_lv[i] >= 99) ++party_at_99;
        const std::uint16_t hp_cur = ps2::ld16(base + 0x1Cu);
        const std::uint16_t hp_max = ps2::ld16(base + 0x1Eu);
        if (hp_max >= 10u && hp_cur > 0u && hp_cur <= hp_max &&
            static_cast<std::uint32_t>(hp_cur) * 100u <= static_cast<std::uint32_t>(hp_max) * 15u) {
            any_critical_hp = true;
        }
    }
    if (active_members <= 0) return;

    bool dirty_persist = false;

    // 5. Live Stage Code Tracking (*(0x003C86E0) - 0x30 -> e.g. "GD_01_U", "NY_01_U")
    char stage_buf[17] = {};
    const std::uint32_t stage_anchor = ps2::ld32(0x003C86E0u);
    if (stage_anchor >= 0x00100030u && stage_anchor < ps2::kRamSize) {
        const std::uint32_t stage_addr = stage_anchor - 0x30u;
        for (int i = 0; i < 16; ++i) {
            const char c = static_cast<char>(ps2::ld8(stage_addr + static_cast<std::uint32_t>(i)));
            if (c == '\0') break;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') stage_buf[i] = c;
            else break;
        }
    }
    const std::string_view stage_sv(stage_buf);
    const auto has_region = [&](std::string_view code) -> bool {
        for (const std::string &r : visited_regions_)
            if (r == code) return true;
        return false;
    };
    if (stage_sv.size() >= 3u && stage_sv[2] == '_' &&
        stage_sv[0] >= 'A' && stage_sv[0] <= 'Z' && stage_sv[1] >= 'A' && stage_sv[1] <= 'Z') {
        const std::string_view prefix = stage_sv.substr(0, 2);
        if (prefix != "OP" && prefix != "LP" && prefix != "ZZ" && !has_region(prefix)) {
            visited_regions_.emplace_back(prefix);
            dirty_persist = true;
        }
    }

    // 6. Live Streak, Spending, Flawless & Battle Delta Detection
    auto flush_pending_rings = [&]() {
        if (pending_ring_attempts_ == 0u) return;
        const std::uint32_t da = pending_ring_attempts_;
        const std::uint32_t ds = std::min(pending_ring_successes_, da);
        const std::uint32_t dp = std::min(pending_ring_perfects_, da);
        ring_attempts_in_current_battle_ += da;
        if (ds >= da) {
            current_ring_streak_ += static_cast<int>(ds);
            current_miss_streak_ = 0;
            if (current_ring_streak_ > best_ring_streak_) {
                best_ring_streak_ = current_ring_streak_;
                dirty_persist = true;
            }
        } else {
            current_ring_streak_ = 0;
            current_miss_streak_ += static_cast<int>(da - ds);
            if (current_miss_streak_ > best_miss_streak_) {
                best_miss_streak_ = current_miss_streak_;
                dirty_persist = true;
            }
            missed_in_current_battle_ = true;
        }
        if (dp >= da) {
            current_perfect_streak_ += static_cast<int>(dp);
            if (current_perfect_streak_ > best_perfect_streak_) {
                best_perfect_streak_ = current_perfect_streak_;
                dirty_persist = true;
            }
        } else {
            current_perfect_streak_ = 0;
            non_perfect_in_current_battle_ = true;
        }
        pending_ring_attempts_ = 0;
        pending_ring_successes_ = 0;
        pending_ring_perfects_ = 0;
        pending_ring_wait_frames_ = 0;
    };

    bool clean_battle_won = false;
    bool panicked_and_won = false;
    bool state_reloaded = false;
    if (!ram_initialized_) {
        ram_initialized_ = true;
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles_entered;
        prev_battles_won_ = battles_won;
        prev_active_gold_ = cash;
    } else {
        const bool playtime_jumped = (playtime_sec + 0.5f < prev_playtime_sec_) || (playtime_sec > prev_playtime_sec_ + 60.0f);
        if (playtime_jumped || battles_entered < prev_battles_entered_ || battles_won < prev_battles_won_ ||
            ring_attempts < prev_ring_attempts_ || ring_successes < prev_ring_successes_) {
            state_reloaded = true;
            current_ring_streak_ = 0;
            current_perfect_streak_ = 0;
            current_flawless_battle_streak_ = 0;
            current_miss_streak_ = 0;
            ring_attempts_in_current_battle_ = 0;
            missed_in_current_battle_ = false;
            non_perfect_in_current_battle_ = false;
            pending_ring_attempts_ = 0;
            pending_ring_successes_ = 0;
            pending_ring_perfects_ = 0;
            pending_ring_wait_frames_ = 0;
        } else {
            if (battles_entered > prev_battles_entered_) {
                pending_ring_attempts_ = 0;
                pending_ring_successes_ = 0;
                pending_ring_perfects_ = 0;
                pending_ring_wait_frames_ = 0;
                ring_attempts_in_current_battle_ = 0;
                missed_in_current_battle_ = false;
                non_perfect_in_current_battle_ = false;
            }
            const std::uint32_t da = (ring_attempts > prev_ring_attempts_ && ring_attempts - prev_ring_attempts_ <= 16u)
                ? (ring_attempts - prev_ring_attempts_) : 0u;
            const std::uint32_t ds = (ring_successes > prev_ring_successes_ && ring_successes - prev_ring_successes_ <= 16u)
                ? (ring_successes - prev_ring_successes_) : 0u;
            const std::uint32_t dp = (ring_perfects > prev_ring_perfects_ && ring_perfects - prev_ring_perfects_ <= 16u)
                ? (ring_perfects - prev_ring_perfects_) : 0u;

            if (da > 0u) {
                pending_ring_attempts_ += da;
                pending_ring_wait_frames_ = 2;
            }
            if (ds > 0u && pending_ring_attempts_ > 0u) pending_ring_successes_ += ds;
            if (dp > 0u && pending_ring_attempts_ > 0u) pending_ring_perfects_ += dp;

            const bool battle_finished = (battles_won > prev_battles_won_ && battles_won - prev_battles_won_ <= 4u);
            if (pending_ring_attempts_ > 0u) {
                const bool settled_success = (pending_ring_successes_ >= pending_ring_attempts_);
                if (settled_success || battle_finished || pending_ring_wait_frames_ <= 0) {
                    flush_pending_rings();
                } else {
                    --pending_ring_wait_frames_;
                }
            }

            if (cash < prev_active_gold_) {
                const std::uint64_t spent = static_cast<std::uint64_t>(prev_active_gold_ - cash);
                if (spent > 0u && spent < 50000000u) {
                    total_gold_spent_ += spent;
                    if (spent > biggest_single_spend_) biggest_single_spend_ = spent;
                    dirty_persist = true;
                }
            }

            if (battle_finished) {
                if (ring_attempts_in_current_battle_ > 0u && !missed_in_current_battle_) {
                    clean_battle_won = true;
                    ++current_flawless_battle_streak_;
                    if (current_flawless_battle_streak_ > best_flawless_battle_streak_) {
                        best_flawless_battle_streak_ = current_flawless_battle_streak_;
                        dirty_persist = true;
                    }
                    if (!non_perfect_in_current_battle_) {
                        ++perfect_battle_wins_;
                        dirty_persist = true;
                    }
                } else if (missed_in_current_battle_) {
                    panicked_and_won = true;
                    current_flawless_battle_streak_ = 0;
                }
                if (any_critical_hp) {
                    ++clutch_wins_;
                    dirty_persist = true;
                }
                ring_attempts_in_current_battle_ = 0;
                missed_in_current_battle_ = false;
                non_perfect_in_current_battle_ = false;
            }
        }
        prev_playtime_sec_ = playtime_sec;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_entered_ = battles_entered;
        prev_battles_won_ = battles_won;
        prev_active_gold_ = cash;
    }
    if (state_reloaded) {
        if (dirty_persist) save();
        return;
    }

    const double ring_acc = ring_attempts > 0u ? (100.0 * static_cast<double>(ring_successes) / static_cast<double>(ring_attempts)) : 0.0;
    const std::uint64_t ring_acc_int = static_cast<std::uint64_t>(std::max(0, static_cast<int>(ring_acc)));
    const std::uint64_t playtime_min = static_cast<std::uint64_t>(std::max(0, static_cast<int>(playtime_sec / 60.0f)));
    const int regions_visited = static_cast<int>(visited_regions_.size());

    // 7. Evaluate all 75 Shadow Hearts: From the New World (1929) Achievements
    std::vector<std::size_t> frame_unlocks;
    std::string stamp;
    auto eval = [&](const char *id, bool cond, std::uint64_t prog_key, double ratio, auto &&make_prog) {
        auto it = id_to_index_.find(std::string_view(id));
        if (it == id_to_index_.end()) return;
        const std::size_t idx = it->second;
        Entry &e = entries_[idx];
        if (last_prog_key_[idx] != prog_key) {
            last_prog_key_[idx] = prog_key;
            e.progress_text = make_prog();
        }
        e.progress_ratio = e.unlocked ? 1.0 : std::clamp(ratio, 0.0, 1.0);
        if (cond && !e.unlocked) {
            e.unlocked = true;
            if (stamp.empty()) stamp = now_timestamp();
            e.unlocked_at = stamp;
            e.progress_ratio = 1.0;
            frame_unlocks.push_back(idx);
            std::fprintf(stderr, "[achievement] UNLOCKED [%s] %s — %s\n", tier_name(e.tier), e.title.c_str(), e.description.c_str());
        }
    };
    const auto pack2 = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
        return (a & 0xFFFFFFFFull) | ((b & 0xFFFFFFFFull) << 32);
    };

    // --- Tier 1: Mark (16) ---
    eval("sh3_mark_first_strike", ring_successes >= 1u, ring_successes, ring_successes >= 1u ? 1.0 : 0.0,
         [&] { return std::to_string(std::min(ring_successes, 1u)) + " / 1 Ring hit"; });
    eval("sh3_mark_three_streak", best_ring_streak_ >= 3, static_cast<std::uint64_t>(best_ring_streak_),
         static_cast<double>(best_ring_streak_) / 3.0, [&] { return std::to_string(best_ring_streak_) + " / 3 streak"; });
    eval("sh3_mark_first_perfect", ring_perfects >= 1u, ring_perfects, ring_perfects >= 1u ? 1.0 : 0.0,
         [&] { return std::to_string(std::min(ring_perfects, 1u)) + " / 1 Perfect"; });
    eval("sh3_mark_battles_10", battles_won >= 10u, battles_won, static_cast<double>(battles_won) / 10.0,
         [&] { return std::to_string(battles_won) + " / 10 victories"; });
    eval("sh3_mark_clean_battle", clean_battle_won || best_flawless_battle_streak_ >= 1,
         static_cast<std::uint64_t>(best_flawless_battle_streak_ > 0 ? 1 : 0),
         (clean_battle_won || best_flawless_battle_streak_ >= 1) ? 1.0 : 0.0,
         [&] { return (clean_battle_won || best_flawless_battle_streak_ >= 1) ? std::string("Clean battle achieved") : std::string("0 / 1 clean battle"); });
    eval("sh3_mark_lv10", max_level >= 10, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 10.0,
         [&] { return "Lv " + std::to_string(max_level) + " / 10"; });
    eval("sh3_mark_lv15", max_level >= 15, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 15.0,
         [&] { return "Lv " + std::to_string(max_level) + " / 15"; });
    eval("sh3_mark_shania_joins", recruited[1], recruited[1] ? 1u : 0u, recruited[1] ? 1.0 : 0.0,
         [&] { return recruited[1] ? std::string("Shania recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_natan_joins", recruited[2], recruited[2] ? 1u : 0u, recruited[2] ? 1.0 : 0.0,
         [&] { return recruited[2] ? std::string("Natan recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_frank_joins", recruited[3], recruited[3] ? 1u : 0u, recruited[3] ? 1.0 : 0.0,
         [&] { return recruited[3] ? std::string("Frank recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_mao_joins", recruited[4], recruited[4] ? 1u : 0u, recruited[4] ? 1.0 : 0.0,
         [&] { return recruited[4] ? std::string("Mao recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_hilda_joins", recruited[5], recruited[5] ? 1u : 0u, recruited[5] ? 1.0 : 0.0,
         [&] { return recruited[5] ? std::string("Hilda recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_ricardo_joins", recruited[6], recruited[6] ? 1u : 0u, recruited[6] ? 1.0 : 0.0,
         [&] { return recruited[6] ? std::string("Ricardo recruited") : std::string("Not yet recruited"); });
    eval("sh3_mark_cash_10k", cash >= 10000u, cash, static_cast<double>(cash) / 10000.0,
         [&] { return fmt_commas(cash) + " / 10,000 Cash"; });
    eval("sh3_mark_first_hour", playtime_sec >= 3600.0f, playtime_min, static_cast<double>(playtime_sec) / 3600.0,
         [&] { return fmt_hours(playtime_sec) + " / 1h 00m"; });
    eval("sh3_mark_regions_3", regions_visited >= 3, static_cast<std::uint64_t>(regions_visited),
         static_cast<double>(regions_visited) / 3.0, [&] { return std::to_string(regions_visited) + " / 3 regions"; });

    // --- Tier 2: Sigil (14) ---
    eval("sh3_sigil_ring_50", ring_successes >= 50u, ring_successes, static_cast<double>(ring_successes) / 50.0,
         [&] { return std::to_string(ring_successes) + " / 50 Ring hits"; });
    eval("sh3_sigil_ring_100", ring_successes >= 100u, ring_successes, static_cast<double>(ring_successes) / 100.0,
         [&] { return std::to_string(ring_successes) + " / 100 Ring hits"; });
    eval("sh3_sigil_perfect_25", ring_perfects >= 25u, ring_perfects, static_cast<double>(ring_perfects) / 25.0,
         [&] { return std::to_string(ring_perfects) + " / 25 Perfects"; });
    eval("sh3_sigil_streak_6", best_ring_streak_ >= 6, static_cast<std::uint64_t>(best_ring_streak_),
         static_cast<double>(best_ring_streak_) / 6.0, [&] { return std::to_string(best_ring_streak_) + " / 6 streak"; });
    eval("sh3_sigil_triple_perfect", best_perfect_streak_ >= 3, static_cast<std::uint64_t>(best_perfect_streak_),
         static_cast<double>(best_perfect_streak_) / 3.0, [&] { return std::to_string(best_perfect_streak_) + " / 3 Perfects in a row"; });
    eval("sh3_sigil_battles_40", battles_won >= 40u, battles_won, static_cast<double>(battles_won) / 40.0,
         [&] { return std::to_string(battles_won) + " / 40 victories"; });
    eval("sh3_sigil_flawless_5", best_flawless_battle_streak_ >= 5, static_cast<std::uint64_t>(best_flawless_battle_streak_),
         static_cast<double>(best_flawless_battle_streak_) / 5.0, [&] { return std::to_string(best_flawless_battle_streak_) + " / 5 flawless battles"; });
    eval("sh3_sigil_johnny_25", char_lv[0] >= 25, static_cast<std::uint64_t>(char_lv[0]), static_cast<double>(char_lv[0]) / 25.0,
         [&] { return "Johnny Lv " + std::to_string(char_lv[0]) + " / 25"; });
    eval("sh3_sigil_shania_25", char_lv[1] >= 25, static_cast<std::uint64_t>(char_lv[1]), static_cast<double>(char_lv[1]) / 25.0,
         [&] { return "Shania Lv " + std::to_string(char_lv[1]) + " / 25"; });
    eval("sh3_sigil_natan_25", char_lv[2] >= 25, static_cast<std::uint64_t>(char_lv[2]), static_cast<double>(char_lv[2]) / 25.0,
         [&] { return "Natan Lv " + std::to_string(char_lv[2]) + " / 25"; });
    eval("sh3_sigil_frank_25", char_lv[3] >= 25, static_cast<std::uint64_t>(char_lv[3]), static_cast<double>(char_lv[3]) / 25.0,
         [&] { return "Frank Lv " + std::to_string(char_lv[3]) + " / 25"; });
    eval("sh3_sigil_lv30", max_level >= 30, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 30.0,
         [&] { return "Lv " + std::to_string(max_level) + " / 30"; });
    eval("sh3_sigil_cash_75k", cash >= 75000u, cash, static_cast<double>(cash) / 75000.0,
         [&] { return fmt_commas(cash) + " / 75,000 Cash"; });
    eval("sh3_sigil_ten_hours", playtime_sec >= 36000.0f, playtime_min, static_cast<double>(playtime_sec) / 36000.0,
         [&] { return fmt_hours(playtime_sec) + " / 10h 00m"; });

    // --- Tier 3: Crest (11) ---
    eval("sh3_crest_ring_300", ring_successes >= 300u, ring_successes, static_cast<double>(ring_successes) / 300.0,
         [&] { return std::to_string(ring_successes) + " / 300 Ring hits"; });
    eval("sh3_crest_perfect_100", ring_perfects >= 100u, ring_perfects, static_cast<double>(ring_perfects) / 100.0,
         [&] { return std::to_string(ring_perfects) + " / 100 Perfects"; });
    eval("sh3_crest_streak_12", best_ring_streak_ >= 12, static_cast<std::uint64_t>(best_ring_streak_),
         static_cast<double>(best_ring_streak_) / 12.0, [&] { return std::to_string(best_ring_streak_) + " / 12 streak"; });
    eval("sh3_crest_quad_perfect", best_perfect_streak_ >= 4, static_cast<std::uint64_t>(best_perfect_streak_),
         static_cast<double>(best_perfect_streak_) / 4.0, [&] { return std::to_string(best_perfect_streak_) + " / 4 Perfects in a row"; });
    eval("sh3_crest_flawless_12", best_flawless_battle_streak_ >= 12, static_cast<std::uint64_t>(best_flawless_battle_streak_),
         static_cast<double>(best_flawless_battle_streak_) / 12.0, [&] { return std::to_string(best_flawless_battle_streak_) + " / 12 flawless battles"; });
    eval("sh3_crest_battles_150", battles_won >= 150u, battles_won, static_cast<double>(battles_won) / 150.0,
         [&] { return std::to_string(battles_won) + " / 150 victories"; });
    eval("sh3_crest_lv50", max_level >= 50, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 50.0,
         [&] { return "Lv " + std::to_string(max_level) + " / 50"; });
    eval("sh3_crest_party_45", party_at_45 >= 7, static_cast<std::uint64_t>(party_at_45), static_cast<double>(party_at_45) / 7.0,
         [&] { return std::to_string(party_at_45) + " / 7 party at Lv 45+"; });
    eval("sh3_crest_cash_250k", cash >= 250000u, cash, static_cast<double>(cash) / 250000.0,
         [&] { return fmt_commas(cash) + " / 250,000 Cash"; });
    eval("sh3_crest_regions_8", regions_visited >= 8, static_cast<std::uint64_t>(regions_visited),
         static_cast<double>(regions_visited) / 8.0, [&] { return std::to_string(regions_visited) + " / 8 regions"; });
    eval("sh3_crest_combo_20", max_combo_hits >= 20u, max_combo_hits, static_cast<double>(max_combo_hits) / 20.0,
         [&] { return std::to_string(max_combo_hits) + " / 20 max combo hits"; });

    // --- Tier 4: Sovereign (8) ---
    eval("sh3_sovereign_battles_500", battles_won >= 500u, battles_won, static_cast<double>(battles_won) / 500.0,
         [&] { return std::to_string(battles_won) + " / 500 victories"; });
    eval("sh3_sovereign_perfect_500", ring_perfects >= 500u, ring_perfects, static_cast<double>(ring_perfects) / 500.0,
         [&] { return std::to_string(ring_perfects) + " / 500 Perfects"; });
    eval("sh3_sovereign_streak_20", best_ring_streak_ >= 20, static_cast<std::uint64_t>(best_ring_streak_),
         static_cast<double>(best_ring_streak_) / 20.0, [&] { return std::to_string(best_ring_streak_) + " / 20 streak"; });
    eval("sh3_sovereign_streak_50", best_ring_streak_ >= 50, static_cast<std::uint64_t>(best_ring_streak_),
         static_cast<double>(best_ring_streak_) / 50.0, [&] { return std::to_string(best_ring_streak_) + " / 50 streak"; });
    eval("sh3_sovereign_accuracy_90", battles_won >= 150u && ring_attempts >= 50u && ring_acc >= 90.0,
         pack2(battles_won, ring_acc_int), std::min(static_cast<double>(battles_won) / 150.0, ring_acc / 90.0),
         [&] { return std::to_string(battles_won) + "/150 wins, " + std::to_string(ring_acc_int) + "%/90% acc"; });
    eval("sh3_sovereign_lv75", max_level >= 75, static_cast<std::uint64_t>(max_level), static_cast<double>(max_level) / 75.0,
         [&] { return "Lv " + std::to_string(max_level) + " / 75"; });
    eval("sh3_sovereign_party_99", party_at_99 >= 7, static_cast<std::uint64_t>(party_at_99), static_cast<double>(party_at_99) / 7.0,
         [&] { return std::to_string(party_at_99) + " / 7 party at Lv 99"; });
    eval("sh3_sovereign_cash_1m", cash >= 1000000u, cash, static_cast<double>(cash) / 1000000.0,
         [&] { return fmt_commas(cash) + " / 1,000,000 Cash"; });

    // --- Follies (10) ---
    eval("sh3_folly_butterfingers", best_miss_streak_ >= 3, static_cast<std::uint64_t>(best_miss_streak_),
         static_cast<double>(best_miss_streak_) / 3.0, [&] { return std::to_string(best_miss_streak_) + " / 3 consecutive misses"; });
    eval("sh3_folly_runaway", battles_fled >= 10u, battles_fled, static_cast<double>(battles_fled) / 10.0,
         [&] { return std::to_string(battles_fled) + " / 10 escapes"; });
    eval("sh3_folly_big_spend", biggest_single_spend_ >= 25000ull, biggest_single_spend_,
         static_cast<double>(biggest_single_spend_) / 25000.0, [&] { return fmt_commas(biggest_single_spend_) + " / 25,000 single spend"; });
    eval("sh3_folly_panic_win", panicked_and_won, panicked_and_won ? 1u : 0u, panicked_and_won ? 1.0 : 0.0,
         [&] { return panicked_and_won ? std::string("Survived a botched Ring") : std::string("Win after missing a Ring"); });
    eval("sh3_folly_perfect_bout", perfect_battle_wins_ >= 5, static_cast<std::uint64_t>(perfect_battle_wins_),
         static_cast<double>(perfect_battle_wins_) / 5.0, [&] { return std::to_string(perfect_battle_wins_) + " / 5 all-Perfect battles"; });
    eval("sh3_folly_clutch", clutch_wins_ >= 1, static_cast<std::uint64_t>(clutch_wins_),
         clutch_wins_ >= 1 ? 1.0 : 0.0, [&] { return std::to_string(std::min(clutch_wins_, 1)) + " / 1 clutch victory"; });
    eval("sh3_folly_overkill", max_level >= 25 && best_ring_streak_ >= 10,
         pack2(static_cast<std::uint64_t>(max_level), static_cast<std::uint64_t>(best_ring_streak_)),
         std::min(static_cast<double>(max_level) / 25.0, static_cast<double>(best_ring_streak_) / 10.0),
         [&] { return "Lv " + std::to_string(max_level) + "/25, Streak " + std::to_string(best_ring_streak_) + "/10"; });
    eval("sh3_folly_hoarder", cash >= 500000u, cash, static_cast<double>(cash) / 500000.0,
         [&] { return fmt_commas(cash) + " / 500,000 Cash"; });
    eval("sh3_folly_marathon", playtime_sec >= 108000.0f, playtime_min, static_cast<double>(playtime_sec) / 108000.0,
         [&] { return fmt_hours(playtime_sec) + " / 30h 00m"; });
    eval("sh3_folly_max_dmg_10k", max_single_damage >= 10000u, max_single_damage,
         static_cast<double>(max_single_damage) / 10000.0, [&] { return fmt_commas(max_single_damage) + " / 10,000 Max Damage"; });

    // --- Curiosities (15 — Secret Until Unlocked) ---
    const auto eval_reg = [&](const char *id, bool hit, const char *yes_txt, const char *no_txt) {
        eval(id, hit, hit ? 1u : 0u, hit ? 1.0 : 0.0, [&] { return std::string(hit ? yes_txt : no_txt); });
    };
    eval_reg("sh3_curiosity_garland", has_region("GD"), "Garland Detective Agency visited", "Start at Garland Detective Agency");
    eval_reg("sh3_curiosity_new_york", has_region("NY") || has_region("TS"), "New York City reached", "Explore 1929 New York");
    eval_reg("sh3_curiosity_arkham", has_region("AR") || has_region("AU"), "Arkham University reached", "Visit Arkham University");
    eval_reg("sh3_curiosity_alcatraz", has_region("AL") || has_region("AC"), "Alcatraz reached", "Infiltrate Alcatraz");
    eval_reg("sh3_curiosity_chicago", has_region("CH") || has_region("CG"), "Chicago reached", "Arrive in Chicago");
    eval_reg("sh3_curiosity_las_vegas", has_region("LV") || has_region("VG"), "Las Vegas reached", "Arrive in Las Vegas");
    eval_reg("sh3_curiosity_roswell", has_region("RS") || has_region("RW"), "Roswell reached", "Infiltrate Roswell");
    eval_reg("sh3_curiosity_grand_canyon", has_region("GC"), "Grand Canyon reached", "Traverse Grand Canyon");
    eval_reg("sh3_curiosity_rio", has_region("RI") || has_region("RJ"), "Rio de Janeiro reached", "Arrive in Rio de Janeiro");
    eval_reg("sh3_curiosity_chichen", has_region("CI") || has_region("CC"), "Chichen Itza reached", "Explore Chichen Itza");
    eval_reg("sh3_curiosity_machu", has_region("MP"), "Machu Picchu reached", "Ascend Machu Picchu");
    eval_reg("sh3_curiosity_caribbean", has_region("CB") || has_region("CR"), "Caribbean reached", "Cross the Caribbean");
    eval_reg("sh3_curiosity_gate", has_region("GT") || has_region("GA"), "The Gate reached", "Reach the Gate");
    eval("sh3_curiosity_combo_50", max_combo_hits >= 50u, max_combo_hits, static_cast<double>(max_combo_hits) / 50.0,
         [&] { return std::to_string(max_combo_hits) + " / 50 max combo hits"; });
    eval("sh3_curiosity_defeats_500", enemies_defeated >= 500u, enemies_defeated, static_cast<double>(enemies_defeated) / 500.0,
         [&] { return std::to_string(enemies_defeated) + " / 500 enemies defeated"; });

    // --- Tier 5: Capstone ("Awakener of the New World") ---
    {
        const int non_cap_unlocked = unlocked_count() - tier_unlocked(Tier::Covenant);
        const int non_cap_total = total_count() - tier_total(Tier::Covenant);
        eval("sh3_new_world_awakened", non_cap_unlocked >= non_cap_total && non_cap_total > 0,
             pack2(static_cast<std::uint64_t>(non_cap_unlocked), static_cast<std::uint64_t>(non_cap_total)),
             non_cap_total > 0 ? static_cast<double>(non_cap_unlocked) / static_cast<double>(non_cap_total) : 0.0,
             [&] { return std::to_string(non_cap_unlocked) + " / " + std::to_string(non_cap_total) + " achievements"; });
    }

    // 8. Smart Burst Coalescing & Single Batched Save
    if (!frame_unlocks.empty()) {
        if (frame_unlocks.size() <= 3u) {
            for (std::size_t idx : frame_unlocks) {
                ToastItem item;
                item.is_burst = false;
                item.entry_idx = idx;
                item.serial = next_toast_serial_++;
                toast_queue_.push_back(item);
            }
        } else {
            Tier best_tier = Tier::Mark;
            const std::string burst_shot = "Unlocked " + std::to_string(frame_unlocks.size()) + " Achievements";
            for (std::size_t idx : frame_unlocks) {
                entries_[idx].screenshot_title = burst_shot;
                if (tier_priority(entries_[idx].tier) > tier_priority(best_tier))
                    best_tier = entries_[idx].tier;
            }
            ToastItem burst;
            burst.is_burst = true;
            burst.entry_idx = frame_unlocks.front();
            burst.burst_count = static_cast<int>(frame_unlocks.size());
            burst.burst_tier = best_tier;
            burst.serial = next_toast_serial_++;
            toast_queue_.push_back(burst);
            std::fprintf(stderr, "[achievement] BURST SUMMARY: %d achievements unlocked simultaneously\n", burst.burst_count);
        }
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    } else if (dirty_persist) {
        for (std::size_t idx : frame_unlocks) {
            recent_unlocks_.emplace_back(entries_[idx].id, entries_[idx].title);
        }
        save();
    }
}

int Manager::unlocked_count() const {
    int n = 0;
    for (const Entry &e : entries_) if (e.unlocked) ++n;
    return n;
}

std::vector<std::pair<std::string, std::string>> Manager::take_recent_unlocks() {
    auto out = std::move(recent_unlocks_);
    recent_unlocks_.clear();
    return out;
}

bool Manager::unlock_by_id(const std::string &id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry &e = entries_[i];
        if (e.id != id) continue;
        if (e.unlocked) return false;
        e.unlocked = true;
        e.unlocked_at = now_timestamp();
        ToastItem item;
        item.entry_idx = i;
        item.serial = next_toast_serial_++;
        toast_queue_.push_back(item);
        save();
        std::fprintf(stderr, "[achievement] SHARED CO-OP UNLOCK [%s] %s\n", tier_short(e.tier), e.title.c_str());
        return true;
    }
    return false;
}

int Manager::tier_unlocked(Tier t) const {
    int n = 0;
    for (const Entry &e : entries_) if (e.tier == t && e.unlocked) ++n;
    return n;
}

int Manager::tier_total(Tier t) const {
    int n = 0;
    for (const Entry &e : entries_) if (e.tier == t) ++n;
    return n;
}

void Manager::render_toast_pixels(const ToastRenderSpec &spec, ToastPicture &out) {
    const Tier tier = spec.tier;
    const int window_w = spec.window_w, window_h = spec.window_h, theme_index = spec.theme_index;

    const float scale = std::clamp(static_cast<float>(std::min(window_w, window_h)) / 900.0f, 0.72f, 1.55f);
    const int hdr_px = std::max(11, static_cast<int>(13.0f * scale + 0.5f));
    const int ttl_px = std::max(14, static_cast<int>(19.0f * scale + 0.5f));
    const int sub_px = std::max(10, static_cast<int>(12.0f * scale + 0.5f));
    const int pad_x = std::max(12, static_cast<int>(16.0f * scale + 0.5f));
    const int pad_y = std::max(10, static_cast<int>(13.0f * scale + 0.5f));
    const int med_r = std::max(18, static_cast<int>(26.0f * scale + 0.5f));
    const int gap = std::max(12, static_cast<int>(15.0f * scale + 0.5f));

    const int max_toast_w = std::max(260, window_w * 42 / 100);
    const int text_area_max = std::max(140, max_toast_w - (pad_x * 2 + med_r * 2 + gap));

    const std::string fit_hdr = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, hdr_px, true, spec.header, text_area_max);
    const std::string fit_ttl = ui_draw::fit_text_px(ui_draw::FontStyle::Serif, ttl_px, true, spec.title, text_area_max);
    const std::string fit_sub = ui_draw::fit_text_px(ui_draw::FontStyle::Sans, sub_px, false, spec.sub, text_area_max);

    const auto m_hdr = ui_draw::measure_text(ui_draw::FontStyle::Sans, hdr_px, true, fit_hdr);
    const auto m_ttl = ui_draw::measure_text(ui_draw::FontStyle::Serif, ttl_px, true, fit_ttl);
    const auto m_sub = ui_draw::measure_text(ui_draw::FontStyle::Sans, sub_px, false, fit_sub);
    const int text_w = std::max({m_hdr.width, m_ttl.width, m_sub.width, static_cast<int>(200.0f * scale)});

    const int w = std::min(max_toast_w, pad_x * 2 + med_r * 2 + gap + text_w + 6);
    const int text_block_h = m_hdr.height + 3 + m_ttl.height + 3 + m_sub.height;
    const int h = std::max(med_r * 2 + pad_y * 2, text_block_h + pad_y * 2);

    const int margin_y = std::max(14, window_h / 34);
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.y = static_cast<std::uint32_t>(margin_y);
    out.active = true;
    out.rgba.assign(static_cast<std::size_t>(w) * h, 0u);

    std::uint32_t gl, gr, border, accent;
    theme_colors(tier, theme_index, gl, gr, border, accent);

    ui_draw::Surface surf(out.rgba.data(), w, h);
    const float rad = std::max(8.0f, 11.0f * scale);

    // Subtle soft drop shadow + beveled tier-tinted gradient card
    surf.fill_rounded_rect(2, 3, w - 2, h - 3, rad, rgba_u32(0, 0, 0, 110));
    surf.fill_gradient_rounded_rect(0, 0, w - 2, h - 3, rad, gl, gr, 0.25f, true);
    surf.stroke_rounded_rect(0, 0, w - 2, h - 3, rad, std::max(1.5f, 1.8f * scale), border);
    surf.stroke_rounded_rect(2, 2, w - 6, h - 7, std::max(4.0f, rad - 2.0f), 1.0f, ui_draw::with_alpha(accent, 90));

    // Left Ornate Tier Medallion
    const float cx = static_cast<float>(pad_x + med_r);
    const float cy = static_cast<float>(h - 3) * 0.5f;
    ui_draw::draw_tier_medallion(surf, cx, cy, static_cast<float>(med_r), static_cast<int>(tier), gl, gr, border, accent, true, false);

    // Text stack on right
    const int tx = pad_x + med_r * 2 + gap;
    int ty = (h - 3 - text_block_h) / 2;
    ui_draw::draw_text(surf, tx, ty, ui_draw::FontStyle::Sans, hdr_px, true, fit_hdr, accent, true);
    ty += m_hdr.height + 3;
    ui_draw::draw_text(surf, tx, ty, ui_draw::FontStyle::Serif, ttl_px, true, fit_ttl, rgba_u32(252, 249, 242, 255), true);
    ty += m_ttl.height + 3;
    ui_draw::draw_text(surf, tx, ty, ui_draw::FontStyle::Sans, sub_px, false, fit_sub, ui_draw::lerp_color(accent, rgba_u32(210, 214, 224, 230), 0.55f), true);
}

ToastTickResult Manager::tick_toast_step(double now_seconds, int window_w, int window_h, int theme_index, ToastRenderSpec &spec_out) {
    ToastTickResult res;
    spec_out = ToastRenderSpec{};
    constexpr double kSlideIn = 0.35;
    constexpr double kHold = 4.30;
    constexpr double kSlideOut = 0.35;
    constexpr double kTotal = kSlideIn + kHold + kSlideOut;
    constexpr double kShotAt = 0.60;

    if (!toast_active_) {
        if (toast_queue_.empty()) {
            return res;
        }
        active_toast_ = toast_queue_.front();
        toast_queue_.erase(toast_queue_.begin());
        toast_active_ = true;
        toast_start_ = now_seconds;
        toast_shot_taken_ = false;
        res.chime = true;
    }

    const double elapsed = now_seconds - toast_start_;
    if (elapsed >= kTotal) {
        toast_active_ = false;
        res.changed = true;
        return res;
    }

    float slide_t = 1.0f;
    if (elapsed < kSlideIn) {
        const float u = static_cast<float>(elapsed / kSlideIn);
        slide_t = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);  // cubic ease-out
    } else if (elapsed > kSlideIn + kHold) {
        const float u = static_cast<float>((elapsed - (kSlideIn + kHold)) / kSlideOut);
        slide_t = 1.0f - u * u;
    }

    if (!toast_shot_taken_ && elapsed >= kShotAt) {
        toast_shot_taken_ = true;
        res.take_screenshot = true;
        res.screenshot_title = active_toast_.is_burst
            ? ("Unlocked " + std::to_string(active_toast_.burst_count) + " Achievements")
            : entries_[active_toast_.entry_idx].title;
    }

    spec_out.active = true;
    spec_out.tier = active_toast_.is_burst ? active_toast_.burst_tier : entries_[active_toast_.entry_idx].tier;
    spec_out.header = active_toast_.is_burst
        ? (std::to_string(active_toast_.burst_count) + " ACHIEVEMENTS UNLOCKED")
        : (std::string(tier_short(spec_out.tier)) + " UNLOCKED");
    spec_out.title = active_toast_.is_burst
        ? std::string("Press F1 to view your Trophy Gallery")
        : entries_[active_toast_.entry_idx].title;
    spec_out.sub = active_toast_.is_burst
        ? (std::to_string(unlocked_count()) + " / " + std::to_string(total_count()) + " Total Unlocked")
        : std::string(tier_name(spec_out.tier));
    spec_out.window_w = window_w;
    spec_out.window_h = window_h;
    spec_out.theme_index = theme_index;
    spec_out.slide_t = slide_t;
    spec_out.serial = active_toast_.serial;
    return res;
}

bool Manager::apply_toast_render(const ToastRenderSpec &spec, ToastPicture &out,
                                 std::uint64_t &cache_serial, int &cache_theme, int &cache_w, int &cache_h, int &last_x) {
    if (!spec.active) {
        const bool was_active = out.active;
        out.active = false;
        out.rgba.clear();
        last_x = -99999;
        return was_active;
    }
    const bool pixels_dirty = (cache_serial != spec.serial || cache_theme != spec.theme_index ||
                               cache_w != spec.window_w || cache_h != spec.window_h || out.rgba.empty());
    if (pixels_dirty) {
        render_toast_pixels(spec, out);
        cache_serial = spec.serial;
        cache_theme = spec.theme_index;
        cache_w = spec.window_w;
        cache_h = spec.window_h;
    }

    const int margin_x = std::max(14, spec.window_w / 48);
    const int target_x = std::max(0, spec.window_w - static_cast<int>(out.width) - margin_x);
    const int offscreen_x = spec.window_w + 4;
    const int cur_x = std::max(0, static_cast<int>(offscreen_x + (target_x - offscreen_x) * spec.slide_t + 0.5f));

    if (pixels_dirty || cur_x != last_x || !out.active) {
        out.x = static_cast<std::uint32_t>(cur_x);
        out.active = true;
        last_x = cur_x;
        return true;
    }
    return false;
}

ToastTickResult Manager::tick_toast(double now_seconds, int window_w, int window_h, int theme_index, ToastPicture &out) {
    ToastRenderSpec spec;
    ToastTickResult res = tick_toast_step(now_seconds, window_w, window_h, theme_index, spec);
    if (apply_toast_render(spec, out, cached_toast_serial_, cached_toast_theme_, cached_win_w_, cached_win_h_, last_toast_x_))
        res.changed = true;
    return res;
}

}  // namespace achievements
