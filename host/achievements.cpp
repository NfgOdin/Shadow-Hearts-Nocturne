#include "achievements.hpp"

#include "ps2/memory.hpp"
#include "ps2/os.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace achievements {
namespace {

constexpr std::uint32_t rgba_u32(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r & 255) |
           (static_cast<std::uint32_t>(g & 255) << 8) |
           (static_cast<std::uint32_t>(b & 255) << 16) |
           (static_cast<std::uint32_t>(a & 255) << 24);
}

std::uint32_t lerp_rgba(std::uint32_t c0, std::uint32_t c1, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const int r0 = static_cast<int>(c0 & 255u), g0 = static_cast<int>((c0 >> 8) & 255u), b0 = static_cast<int>((c0 >> 16) & 255u), a0 = static_cast<int>((c0 >> 24) & 255u);
    const int r1 = static_cast<int>(c1 & 255u), g1 = static_cast<int>((c1 >> 8) & 255u), b1 = static_cast<int>((c1 >> 16) & 255u), a1 = static_cast<int>((c1 >> 24) & 255u);
    return rgba_u32(static_cast<int>(r0 + (r1 - r0) * t + 0.5f),
                    static_cast<int>(g0 + (g1 - g0) * t + 0.5f),
                    static_cast<int>(b0 + (b1 - b0) * t + 0.5f),
                    static_cast<int>(a0 + (a1 - a0) * t + 0.5f));
}

// 5 x 8 built-in bitmap font (same glyph set as host/quickmenu.cpp)
constexpr const char *kFont[95] = {
    "..... ..... ..... ..... ..... ..... ..... .....",  // space
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#.. .....",  // !
    ".#.#. .#.#. ..... ..... ..... ..... ..... .....",  // "
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#. .....",  // #
    "..#.. .#### #.#.. .###. ..#.# ####. ..#.. .....",  // $
    "##... ##..# ...#. ..#.. .#... #..## ...## .....",  // %
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.# .....",  // &
    "..#.. ..#.. ..... ..... ..... ..... ..... .....",  // '
    "...#. ..#.. .#... .#... .#... ..#.. ...#. .....",  // (
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#... .....",  // )
    "..... ..#.. #.#.# .###. #.#.# ..#.. ..... .....",  // *
    "..... ..#.. ..#.. ##### ..#.. ..#.. ..... .....",  // +
    "..... ..... ..... ..... ..... .##.. ..#.. .#...",  // ,
    "..... ..... ..... ##### ..... ..... ..... .....",  // -
    "..... ..... ..... ..... ..... .##.. .##.. .....",  // .
    "..... ....# ...#. ..#.. .#... #.... ..... .....",  // /
    ".###. #...# #..## #.#.# ##..# #...# .###. .....",  // 0
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // 1
    ".###. #...# ....# ...#. ..#.. .#... ##### .....",  // 2
    "##### ...#. ..#.. ...#. ....# #...# .###. .....",  // 3
    "...#. ..##. .#.#. #..#. ##### ...#. ...#. .....",  // 4
    "##### #.... ####. ....# ....# #...# .###. .....",  // 5
    "..##. .#... #.... ####. #...# #...# .###. .....",  // 6
    "##### ....# ...#. ..#.. .#... .#... .#... .....",  // 7
    ".###. #...# #...# .###. #...# #...# .###. .....",  // 8
    ".###. #...# #...# .#### ....# ...#. .##.. .....",  // 9
    "..... .##.. .##.. ..... .##.. .##.. ..... .....",  // :
    "..... .##.. .##.. ..... .##.. ..#.. .#... .....",  // ;
    "...#. ..#.. .#... #.... .#... ..#.. ...#. .....",  // <
    "..... ..... ##### ..... ##### ..... ..... .....",  // =
    ".#... ..#.. ...#. ....# ...#. ..#.. .#... .....",  // >
    ".###. #...# ....# ...#. ..#.. ..... ..#.. .....",  // ?
    ".###. #...# ....# .##.# #.#.# #.#.# .###. .....",  // @
    ".###. #...# #...# ##### #...# #...# #...# .....",  // A
    "####. #...# #...# ####. #...# #...# ####. .....",  // B
    ".###. #...# #.... #.... #.... #...# .###. .....",  // C
    "####. #...# #...# #...# #...# #...# ####. .....",  // D
    "##### #.... #.... ####. #.... #.... ##### .....",  // E
    "##### #.... #.... ####. #.... #.... #.... .....",  // F
    ".###. #...# #.... #.### #...# #...# .#### .....",  // G
    "#...# #...# #...# ##### #...# #...# #...# .....",  // H
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // I
    "..### ...#. ...#. ...#. ...#. #..#. .##.. .....",  // J
    "#...# #..#. #.#.. ##... #.#.. #..#. #...# .....",  // K
    "#.... #.... #.... #.... #.... #.... ##### .....",  // L
    "#...# ##.## #.#.# #.#.# #...# #...# #...# .....",  // M
    "#...# #...# ##..# #.#.# #..## #...# #...# .....",  // N
    ".###. #...# #...# #...# #...# #...# .###. .....",  // O
    "####. #...# #...# ####. #.... #.... #.... .....",  // P
    ".###. #...# #...# #...# #.#.# #..#. .##.# .....",  // Q
    "####. #...# #...# ####. #.#.. #..#. #...# .....",  // R
    ".#### #.... #.... .###. ....# ....# ####. .....",  // S
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // T
    "#...# #...# #...# #...# #...# #...# .###. .....",  // U
    "#...# #...# #...# #...# #...# .#.#. ..#.. .....",  // V
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#. .....",  // W
    "#...# #...# .#.#. ..#.. .#.#. #...# #...# .....",  // X
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.. .....",  // Y
    "##### ....# ...#. ..#.. .#... #.... ##### .....",  // Z
    ".###. .#... .#... .#... .#... .#... .###. .....",  // [
    "..... #.... .#... ..#.. ...#. ....# ..... .....",  // backslash
    ".###. ...#. ...#. ...#. ...#. ...#. .###. .....",  // ]
    "..#.. .#.#. #...# ..... ..... ..... ..... .....",  // ^
    "..... ..... ..... ..... ..... ..... ##### .....",  // _
    ".#... ..#.. ..... ..... ..... ..... ..... .....",  // `
    "..... ..... .###. ....# .#### #...# .#### .....",  // a
    "#.... #.... #.##. ##..# #...# #...# ####. .....",  // b
    "..... ..... .###. #.... #.... #...# .###. .....",  // c
    "....# ....# .##.# #..## #...# #...# .#### .....",  // d
    "..... ..... .###. #...# ##### #.... .###. .....",  // e
    "..##. .#..# .#... ###.. .#... .#... .#... .....",  // f
    "..... ..... .#### #...# #...# .#### ....# .###.",  // g
    "#.... #.... #.##. ##..# #...# #...# #...# .....",  // h
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###. .....",  // i
    "...#. ..... ..##. ...#. ...#. ...#. #..#. .##..",  // j
    "#.... #.... #..#. #.#.. ##... #.#.. #..#. .....",  // k
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###. .....",  // l
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.# .....",  // m
    "..... ..... #.##. ##..# #...# #...# #...# .....",  // n
    "..... ..... .###. #...# #...# #...# .###. .....",  // o
    "..... ..... ####. #...# #...# ####. #.... #....",  // p
    "..... ..... .#### #...# #...# .#### ....# ....#",  // q
    "..... ..... #.##. ##..# #.... #.... #.... .....",  // r
    "..... ..... .###. #.... .###. ....# ####. .....",  // s
    ".#... .#... ###.. .#... .#... .#..# ..##. .....",  // t
    "..... ..... #...# #...# #...# #..## .##.# .....",  // u
    "..... ..... #...# #...# #...# .#.#. ..#.. .....",  // v
    "..... ..... #...# #...# #.#.# #.#.# .#.#. .....",  // w
    "..... ..... #...# .#.#. ..#.. .#.#. #...# .....",  // x
    "..... ..... #...# #...# #...# .#### ....# .###.",  // y
    "..... ..... ##### ...#. ..#.. .#... ##### .....",  // z
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#. .....",  // {
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. .....",  // |
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#... .....",  // }
    "..... ..... .#... #.#.# ...#. ..... ..... .....",  // ~
};

// 9x9 pixel-art tier emblems (Mark, Sigil, Crest, Sovereign, The Covenant, Folly, Curiosity)
constexpr const char *kEmblems[7][9] = {
    { // Mark: Diamond Seal
        "....#....", "...###...", "..##.##..", ".##...##.", "##..#..##", ".##...##.", "..##.##..", "...###...", "....#...."
    },
    { // Sigil: Arcane Ring & Cross
        "..#####..", ".##...##.", "##..#..##", "#...#...#", "#########", "#...#...#", "##..#..##", ".##...##.", "..#####.."
    },
    { // Crest: Twin-Winged Shield
        "##..#..##", "###.#.###", ".#######.", ".##.#.##.", "..#####..", "..##.##..", "...###...", "...###...", "....#...."
    },
    { // Sovereign: Crown
        "#...#...#", "##.###.##", "#########", "#########", ".#######.", ".#.#.#.#.", ".#######.", "..#####..", "........."
    },
    { // The Covenant: Radiant Eclipse / Judgment Ring Star
        "....#....", ".#..#..#.", "..#####..", ".##.#.##.", "###.#.###", ".##.#.##.", "..#####..", ".#..#..#.", "....#...."
    },
    { // Folly: Jester Mask / Starburst
        ".##...##.", "###...###", "..##.##..", "...###...", "##.#.#.##", "...###...", "..#...#..", ".##...##.", "........."
    },
    { // Curiosity: Occult Eye / Key
        ".........", "..#####..", ".##...##.", "##.###.##", "#.#.#.#.#", "##.###.##", ".##...##.", "..#####..", "........."
    },
};

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

}  // namespace

const char *tier_name(Tier t) {
    switch (t) {
    case Tier::Mark: return "Tier 1: Mark";
    case Tier::Sigil: return "Tier 2: Sigil";
    case Tier::Crest: return "Tier 3: Crest";
    case Tier::Sovereign: return "Tier 4: Sovereign";
    case Tier::Covenant: return "Tier 5: The Covenant";
    case Tier::Folly: return "Follies";
    case Tier::Curiosity: return "Curiosities";
    default: return "All";
    }
}

const char *tier_short(Tier t) {
    switch (t) {
    case Tier::Mark: return "MARK";
    case Tier::Sigil: return "SIGIL";
    case Tier::Crest: return "CREST";
    case Tier::Sovereign: return "SOVEREIGN";
    case Tier::Covenant: return "THE COVENANT";
    case Tier::Folly: return "FOLLY";
    case Tier::Curiosity: return "CURIOSITY";
    default: return "";
    }
}

const char *toast_theme_name(int theme) {
    static const char *const kNames[] = {"Tier-Tinted Gradients", "Soul Graveyard Eclipse", "Judgment Ring Crimson", "1915 Occult Grimoire"};
    return kNames[theme >= 0 && theme < static_cast<int>(ToastTheme::Count) ? theme : 0];
}

const char *toast_theme_word(int theme) {
    static const char *const kWords[] = {"tier_tinted", "soul_graveyard", "judgment_ring", "occult_grimoire"};
    return kWords[theme >= 0 && theme < static_cast<int>(ToastTheme::Count) ? theme : 0];
}

int parse_toast_theme(const std::string &word) {
    std::string w;
    for (unsigned char c : word)
        if (std::isalnum(c)) w.push_back(static_cast<char>(std::tolower(c)));
    if (w == "soulgraveyard" || w == "eclipse" || w == "1") return 1;
    if (w == "judgmentring" || w == "crimson" || w == "2") return 2;
    if (w == "occultgrimoire" || w == "grimoire" || w == "3") return 3;
    return 0;
}

void Manager::theme_colors(Tier t, int theme_index, std::uint32_t &grad_left, std::uint32_t &grad_right, std::uint32_t &border, std::uint32_t &accent) {
    if (theme_index == static_cast<int>(ToastTheme::SoulGraveyard)) {
        grad_left = rgba_u32(18, 12, 30, 240);
        grad_right = rgba_u32(59, 17, 43, 240);
        border = rgba_u32(226, 180, 90, 255);
        accent = rgba_u32(244, 210, 130, 255);
        return;
    }
    if (theme_index == static_cast<int>(ToastTheme::JudgmentRing)) {
        grad_left = rgba_u32(27, 20, 16, 240);
        grad_right = rgba_u32(92, 22, 32, 240);
        border = rgba_u32(242, 193, 78, 255);
        accent = rgba_u32(255, 214, 112, 255);
        return;
    }
    if (theme_index == static_cast<int>(ToastTheme::OccultGrimoire)) {
        grad_left = rgba_u32(15, 23, 42, 240);
        grad_right = rgba_u32(30, 41, 59, 240);
        border = rgba_u32(56, 189, 248, 255);
        accent = rgba_u32(214, 192, 141, 255);
        return;
    }
    // Default: Dynamic Tier-Tinted Gradients
    switch (t) {
    case Tier::Mark:
        grad_left = rgba_u32(35, 39, 51, 242);
        grad_right = rgba_u32(124, 86, 52, 242);
        border = rgba_u32(200, 155, 99, 255);
        accent = rgba_u32(236, 196, 140, 255);
        break;
    case Tier::Sigil:
        grad_left = rgba_u32(19, 34, 61, 242);
        grad_right = rgba_u32(36, 123, 160, 242);
        border = rgba_u32(86, 207, 225, 255);
        accent = rgba_u32(142, 232, 245, 255);
        break;
    case Tier::Crest:
        grad_left = rgba_u32(38, 17, 56, 242);
        grad_right = rgba_u32(131, 33, 83, 242);
        border = rgba_u32(224, 108, 159, 255);
        accent = rgba_u32(248, 156, 198, 255);
        break;
    case Tier::Sovereign:
        grad_left = rgba_u32(26, 20, 14, 244);
        grad_right = rgba_u32(184, 130, 30, 244);
        border = rgba_u32(247, 201, 72, 255);
        accent = rgba_u32(255, 228, 132, 255);
        break;
    case Tier::Covenant:
        grad_left = rgba_u32(20, 8, 18, 248);
        grad_right = rgba_u32(120, 18, 38, 248);
        border = rgba_u32(255, 226, 138, 255);
        accent = rgba_u32(255, 214, 92, 255);
        break;
    case Tier::Folly:
        grad_left = rgba_u32(38, 17, 51, 242);
        grad_right = rgba_u32(196, 60, 132, 242);
        border = rgba_u32(255, 158, 187, 255);
        accent = rgba_u32(255, 196, 216, 255);
        break;
    case Tier::Curiosity:
    default:
        grad_left = rgba_u32(12, 31, 29, 242);
        grad_right = rgba_u32(36, 140, 118, 242);
        border = rgba_u32(110, 231, 200, 255);
        accent = rgba_u32(162, 245, 222, 255);
        break;
    }
}

Manager::Manager() { init_catalog(); }

void Manager::init_catalog() {
    entries_.clear();
    const auto add = [&](const char *id, const char *title, const char *desc, Tier tier, bool secret = false) {
        Entry e;
        e.id = id;
        e.title = title;
        e.description = desc;
        e.tier = tier;
        e.secret = secret;
        entries_.push_back(std::move(e));
    };

    // Tier 1: Mark (6)
    add("mark_first_blood", "First Blood on the Ring", "Land your first successful Strike on the Judgment Ring.", Tier::Mark);
    add("mark_first_spark", "First Spark of Soul", "Awaken Yuri in your active party and forge his first Soul Fusion.", Tier::Mark);
    add("mark_pocket_change", "Pocket Change", "Accumulate your first 10,000 Gold across battles and loot.", Tier::Mark);
    add("mark_seasoned_initiate", "Seasoned Initiate", "Reach Level 15 with any party member.", Tier::Mark);
    add("mark_fresh_ink", "Fresh Ink", "Collect 5 Soul Crests or Crest Magic supplies.", Tier::Mark);
    add("mark_getting_rhythm", "Getting the Rhythm", "String together 3 consecutive Judgment Ring successes without a miss.", Tier::Mark);

    // Tier 2: Sigil (5)
    add("sigil_mercenary_vault", "Mercenary's Vault", "Hold 100,000 Gold at one time.", Tier::Sigil);
    add("sigil_veteran_front", "Veteran of the Front", "Reach Level 35 with any party member.", Tier::Sigil);
    add("sigil_tightening_arc", "Tightening the Arc", "Land 25 Perfect Strike Zones on the Judgment Ring.", Tier::Sigil);
    add("sigil_covenant_elements", "Covenant of Elements", "Build Yuri to Level 30+ with a 4-member active Covenant party.", Tier::Sigil);
    add("sigil_traveler_ledger", "Traveler's Ledger", "Explore and progress across 3 distinct regions of 1915 Europe & Asia.", Tier::Sigil);

    // Tier 3: Crest (5)
    add("crest_high_roller", "High Roller's Hoard", "Amass 500,000 Gold.", Tier::Crest);
    add("crest_apex_vessel", "Apex Vessel", "Reach Level 55 with any party member.", Tier::Crest);
    add("crest_razors_edge", "Razor's Edge", "Execute 50 Perfect Strike Zones on the Judgment Ring.", Tier::Crest);
    add("crest_master_grimoire", "Master of the Grimoire", "Acquire 15+ Crests / rare supplies and win 75 battles.", Tier::Crest);
    add("crest_relentless_chain", "Relentless Chain", "Land 6 consecutive Judgment Ring Strike Zones without a miss.", Tier::Crest);

    // Tier 4: Sovereign (5)
    add("sovereign_tycoon", "Tycoon of the Underworld", "Accumulate 2,000,000 Gold across your journey.", Tier::Sovereign);
    add("sovereign_transcendental", "Transcendental Level", "Reach Level 80 or higher on any character.", Tier::Sovereign);
    add("sovereign_clockwork_demon", "Clockwork Demon", "Land 20 consecutive Judgment Ring hits without a single miss.", Tier::Sovereign);
    add("sovereign_bare_knuckles", "Bare Knuckles, No Mercy", "Win 150 battles while maintaining at least 90% Judgment Ring accuracy.", Tier::Sovereign);
    add("sovereign_ring_of_iron", "Ring of Iron", "Land 250 Perfect Strike Zones with at least 85% Judgment Ring accuracy.", Tier::Sovereign);

    // Tier 5: The Covenant (1 Capstone)
    add("covenant_sealed", "The Covenant Sealed", "Unlock every single Mark, Sigil, Crest, Sovereign, Folly, and Curiosity.", Tier::Covenant);

    // Follies (5)
    add("folly_wrestling_destiny", "Wrestling with Destiny", "Recruit Joachim and bring his wrestling spectacle into a victorious battle.", Tier::Folly);
    add("folly_manly_men", "Manly Men Assemble", "Push Joachim past Level 40 to honor the sheer absurdity of the Man Festival.", Tier::Folly);
    add("folly_decked_out", "Decked Out", "Plunder 20+ hidden chests and collectibles across the world.", Tier::Folly);
    add("folly_panic_button", "Panic Button", "Botch a Judgment Ring input mid-battle and still survive to claim victory.", Tier::Folly);
    add("folly_overkill_theater", "Overkill Theater", "Flex on foes with a 10+ hit Ring streak after reaching Level 25.", Tier::Folly);

    // Curiosities (5 — Secret until unlocked)
    add("curiosity_whispers_graveyard", "Whispers in the Graveyard", "Step inside Yuri's Soul Graveyard and witness its hidden memories.", Tier::Curiosity, true);
    add("curiosity_forgotten_page", "Forgotten Page", "Uncover 10 hidden treasures off the beaten story path.", Tier::Curiosity, true);
    add("curiosity_echoes_1915", "Echoes of 1915", "Cross into Disc 2 (or journey across 5+ distinct world regions).", Tier::Curiosity, true);
    add("curiosity_unlikely_encounter", "The Unlikely Encounter", "Survive 25 battles with at least 15 Perfect Judgment Ring Strikes.", Tier::Curiosity, true);
    add("curiosity_keeper_obscura", "Keeper of Obscura", "Uncover 40+ hidden chests across 6+ distinct world regions.", Tier::Curiosity, true);
}

void Manager::load(const std::string &json_path) {
    path_ = json_path;
    std::FILE *f = ps2::os::open(path_, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);

    // Minimal resilient JSON extractor for our own schema
    const auto find_int = [&](const std::string &key, int &out) {
        const std::size_t k = text.find("\"" + key + "\"");
        if (k == std::string::npos) return;
        const std::size_t c = text.find(':', k);
        if (c == std::string::npos) return;
        out = std::atoi(text.c_str() + c + 1);
    };
    find_int("best_ring_streak", best_ring_streak_);

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

    // Parse unlocked entries
    for (Entry &e : entries_) {
        const std::string needle = "\"" + e.id + "\"";
        const std::size_t pos = text.find(needle);
        if (pos == std::string::npos) continue;
        const std::size_t colon = text.find(':', pos + needle.size());
        if (colon == std::string::npos) continue;
        const std::size_t q1 = text.find('"', colon + 1);
        if (q1 == std::string::npos) continue;
        const std::size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        e.unlocked = true;
        e.unlocked_at = text.substr(q1 + 1, q2 - q1 - 1);
        e.progress_ratio = 1.0;
    }
    std::fprintf(stderr, "[achievements] loaded %d/%d unlocked from %s\n", unlocked_count(), total_count(), path_.c_str());
}

void Manager::save() const {
    if (path_.empty()) return;
    std::string out = "{\n";
    out += "  \"best_ring_streak\": " + std::to_string(best_ring_streak_) + ",\n";
    out += "  \"visited_regions\": [";
    for (std::size_t i = 0; i < visited_regions_.size(); ++i)
        out += std::string(i ? ", " : "") + "\"" + json_escape(visited_regions_[i]) + "\"";
    out += "],\n";
    out += "  \"unlocked\": {\n";
    bool first = true;
    for (const Entry &e : entries_) {
        if (!e.unlocked) continue;
        if (!first) out += ",\n";
        first = false;
        out += "    \"" + json_escape(e.id) + "\": \"" + json_escape(e.unlocked_at) + "\"";
    }
    out += "\n  }\n}\n";
    if (std::FILE *f = ps2::os::open(path_, "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
}

void Manager::unlock(std::size_t idx) {
    if (idx >= entries_.size() || entries_[idx].unlocked) return;
    entries_[idx].unlocked = true;
    entries_[idx].unlocked_at = now_timestamp();
    entries_[idx].progress_ratio = 1.0;
    toast_queue_.push_back(idx);
    std::fprintf(stderr, "[achievement] UNLOCKED [%s] %s\n", tier_short(entries_[idx].tier), entries_[idx].title.c_str());
    save();
    check_capstone();
}

void Manager::check_capstone() {
    std::size_t cap_idx = entries_.size();
    bool all_others = true;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].tier == Tier::Covenant) cap_idx = i;
        else if (!entries_[i].unlocked) all_others = false;
    }
    if (all_others && cap_idx < entries_.size() && !entries_[cap_idx].unlocked) {
        entries_[cap_idx].unlocked = true;
        entries_[cap_idx].unlocked_at = now_timestamp();
        entries_[cap_idx].progress_ratio = 1.0;
        toast_queue_.push_back(cap_idx);
        std::fprintf(stderr, "[achievement] CAPSTONE UNLOCKED: %s!\n", entries_[cap_idx].title.c_str());
        save();
    }
}

void Manager::poll_ram(int active_disc) {
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
    const std::string map_id(map_buf);
    if (map_id.size() < 2 || !(map_id[0] >= 'A' && map_id[0] <= 'Z' && map_id[1] >= 'A' && map_id[1] <= 'Z')) return;

    // 1. Gold / Cash (active wallet at 0x00102D30 + wallet_idx*4, cumulative at 0x00103018)
    const std::uint32_t wallet_idx = ps2::ld8(0x00100132u) & 3u;
    const std::uint32_t active_gold = ps2::ld32(0x00102D30u + wallet_idx * 4u);
    const std::uint32_t cum_gold = ps2::ld32(0x00103018u);
    const std::uint32_t best_gold = std::max(active_gold, cum_gold);

    // 2. Party Characters (14 slots at 0x00100250, stride 0x90)
    int max_level = 0, active_members = 0, yuri_lv = 0, joachim_lv = 0;
    for (int slot = 0; slot < 14; ++slot) {
        const std::uint32_t base = 0x00100250u + static_cast<std::uint32_t>(slot) * 0x90u;
        const std::uint8_t lv = ps2::ld8(base + 0x0Fu);
        const std::uint8_t c0 = ps2::ld8(base + 0x16u);
        if (c0 == 0 || lv == 0 || lv > 99) continue;
        ++active_members;
        if (lv > max_level) max_level = lv;
        if (slot == 0) yuri_lv = lv;     // Yuri
        if (slot == 4) joachim_lv = lv;  // Joachim
    }
    if (active_members <= 0) return;

    // 3. Battle & Judgment Ring Stats (BTL0)
    const std::uint32_t battles_entered = ps2::ld32(0x001015ECu);
    const std::uint32_t battles_won = ps2::ld32(0x001016E4u);
    const std::uint32_t ring_attempts = ps2::ld32(0x001017C8u);
    const std::uint32_t ring_successes = ps2::ld32(0x001017CCu);
    const std::uint32_t ring_perfects = ps2::ld32(0x001017D0u);

    // 4. Live Streak & Panic-Button Delta Detection
    if (!ram_initialized_) {
        ram_initialized_ = true;
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
        prev_battles_won_ = battles_won;
    } else {
        if (ring_attempts > prev_ring_attempts_ && ring_attempts - prev_ring_attempts_ <= 16) {
            const std::uint32_t da = ring_attempts - prev_ring_attempts_;
            const std::uint32_t ds = ring_successes >= prev_ring_successes_ ? (ring_successes - prev_ring_successes_) : 0u;
            if (ds >= da) {
                current_ring_streak_ += static_cast<int>(ds);
                if (current_ring_streak_ > best_ring_streak_) {
                    best_ring_streak_ = current_ring_streak_;
                    save();
                }
            } else {
                current_ring_streak_ = 0;
                missed_in_current_battle_ = true;
            }
        } else if (ring_attempts < prev_ring_attempts_) {
            // Save state or older save loaded: reset session delta baseline cleanly
            current_ring_streak_ = 0;
            missed_in_current_battle_ = false;
        }
        prev_ring_attempts_ = ring_attempts;
        prev_ring_successes_ = ring_successes;
        prev_ring_perfects_ = ring_perfects;
    }

    bool panicked_and_won = false;
    if (battles_won > prev_battles_won_ && battles_won - prev_battles_won_ <= 4) {
        if (missed_in_current_battle_) panicked_and_won = true;
        missed_in_current_battle_ = false;
    }
    prev_battles_won_ = battles_won;

    // 5. Inventory / Crest count (non-zero item/crest buckets in BTL0+3260..3772)
    int item_types_owned = 0;
    for (std::uint32_t a = 0x00100DECu; a < 0x00100FECu; a += 4u)
        if (ps2::ld32(a) > 0 && ps2::ld32(a) <= 99) ++item_types_owned;

    // 6. Track visited Map / Region Code (FLDa @ 0x0010230C) & Treasures (TR @ 0x00102B8C)
    {
        const std::string prefix = map_id.substr(0, 2);
        if (std::find(visited_regions_.begin(), visited_regions_.end(), prefix) == visited_regions_.end()) {
            visited_regions_.push_back(prefix);
            save();
        }
    }
    const int regions_visited = static_cast<int>(visited_regions_.size());
    const int treasures_found = popcount_range(0x00102B90u, 128u);
    const double ring_acc = ring_attempts > 0 ? (100.0 * static_cast<double>(ring_successes) / static_cast<double>(ring_attempts)) : 0.0;

    // 7. Evaluate every achievement and update live progress text/ratios
    const auto eval = [&](const char *id, bool cond, const std::string &prog, double ratio) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].id != id) continue;
            entries_[i].progress_text = prog;
            entries_[i].progress_ratio = entries_[i].unlocked ? 1.0 : std::clamp(ratio, 0.0, 1.0);
            if (cond && !entries_[i].unlocked) unlock(i);
            return;
        }
    };

    // Tier 1: Mark
    eval("mark_first_blood", ring_successes >= 1, std::to_string(ring_successes) + " / 1 Strike", ring_successes >= 1 ? 1.0 : 0.0);
    eval("mark_first_spark", yuri_lv >= 1 && battles_won >= 1, yuri_lv >= 1 ? "Yuri active" : "Waiting for Yuri", yuri_lv >= 1 ? 1.0 : 0.0);
    eval("mark_pocket_change", best_gold >= 10000, fmt_commas(best_gold) + " / 10,000 Gold", static_cast<double>(best_gold) / 10000.0);
    eval("mark_seasoned_initiate", max_level >= 15, "Lv " + std::to_string(max_level) + " / 15", static_cast<double>(max_level) / 15.0);
    eval("mark_fresh_ink", item_types_owned >= 12 && treasures_found >= 3, std::to_string(std::min(item_types_owned, 12)) + "/12 items, " + std::to_string(std::min(treasures_found, 3)) + "/3 chests", std::min(static_cast<double>(item_types_owned) / 12.0, static_cast<double>(treasures_found) / 3.0));
    eval("mark_getting_rhythm", best_ring_streak_ >= 3, "Best streak: " + std::to_string(best_ring_streak_) + " / 3", static_cast<double>(best_ring_streak_) / 3.0);

    // Tier 2: Sigil
    eval("sigil_mercenary_vault", active_gold >= 100000, fmt_commas(active_gold) + " / 100,000 Gold", static_cast<double>(active_gold) / 100000.0);
    eval("sigil_veteran_front", max_level >= 35, "Lv " + std::to_string(max_level) + " / 35", static_cast<double>(max_level) / 35.0);
    eval("sigil_tightening_arc", ring_perfects >= 25, std::to_string(ring_perfects) + " / 25 Perfects", static_cast<double>(ring_perfects) / 25.0);
    eval("sigil_covenant_elements", yuri_lv >= 30 && active_members >= 4, "Yuri Lv " + std::to_string(yuri_lv) + " / 30, Party " + std::to_string(active_members) + "/4", std::min(1.0, static_cast<double>(yuri_lv) / 30.0));
    eval("sigil_traveler_ledger", regions_visited >= 3, std::to_string(regions_visited) + " / 3 regions", static_cast<double>(regions_visited) / 3.0);

    // Tier 3: Crest
    eval("crest_high_roller", best_gold >= 500000, fmt_commas(best_gold) + " / 500,000 Gold", static_cast<double>(best_gold) / 500000.0);
    eval("crest_apex_vessel", max_level >= 55, "Lv " + std::to_string(max_level) + " / 55", static_cast<double>(max_level) / 55.0);
    eval("crest_razors_edge", ring_perfects >= 50, std::to_string(ring_perfects) + " / 50 Perfects", static_cast<double>(ring_perfects) / 50.0);
    eval("crest_master_grimoire", item_types_owned >= 15 && battles_won >= 75, std::to_string(item_types_owned) + "/15 items, " + std::to_string(battles_won) + "/75 wins", std::min(1.0, static_cast<double>(battles_won) / 75.0));
    eval("crest_relentless_chain", best_ring_streak_ >= 6, "Best streak: " + std::to_string(best_ring_streak_) + " / 6", static_cast<double>(best_ring_streak_) / 6.0);

    // Tier 4: Sovereign
    eval("sovereign_tycoon", best_gold >= 2000000, fmt_commas(best_gold) + " / 2,000,000 Gold", static_cast<double>(best_gold) / 2000000.0);
    eval("sovereign_transcendental", max_level >= 80, "Lv " + std::to_string(max_level) + " / 80", static_cast<double>(max_level) / 80.0);
    eval("sovereign_clockwork_demon", best_ring_streak_ >= 20, "Best streak: " + std::to_string(best_ring_streak_) + " / 20", static_cast<double>(best_ring_streak_) / 20.0);
    eval("sovereign_bare_knuckles", battles_won >= 150 && ring_acc >= 90.0, std::to_string(battles_won) + "/150 wins (" + std::to_string(static_cast<int>(ring_acc)) + "% Ring)", static_cast<double>(battles_won) / 150.0);
    eval("sovereign_ring_of_iron", ring_perfects >= 250 && ring_acc >= 85.0, std::to_string(ring_perfects) + "/250 Perfects (" + std::to_string(static_cast<int>(ring_acc)) + "% Ring)", static_cast<double>(ring_perfects) / 250.0);

    // Tier 5: Capstone progress
    {
        const int non_cap_unlocked = unlocked_count() - tier_unlocked(Tier::Covenant);
        const int non_cap_total = total_count() - tier_total(Tier::Covenant);
        eval("covenant_sealed", non_cap_unlocked >= non_cap_total && non_cap_total > 0,
             std::to_string(non_cap_unlocked) + " / " + std::to_string(non_cap_total) + " achievements",
             non_cap_total > 0 ? static_cast<double>(non_cap_unlocked) / static_cast<double>(non_cap_total) : 0.0);
    }

    // Follies
    eval("folly_wrestling_destiny", joachim_lv >= 1 && battles_won >= 10, joachim_lv >= 1 ? "Joachim recruited!" : "Recruit Joachim", joachim_lv >= 1 ? 1.0 : 0.0);
    eval("folly_manly_men", joachim_lv >= 40, "Joachim Lv " + std::to_string(joachim_lv) + " / 40", static_cast<double>(joachim_lv) / 40.0);
    eval("folly_decked_out", treasures_found >= 20, std::to_string(treasures_found) + " / 20 chests", static_cast<double>(treasures_found) / 20.0);
    eval("folly_panic_button", panicked_and_won, "Win a battle after missing a Ring", panicked_and_won ? 1.0 : 0.0);
    eval("folly_overkill_theater", max_level >= 25 && best_ring_streak_ >= 10, "Lv " + std::to_string(max_level) + "/25, Streak " + std::to_string(best_ring_streak_) + "/10", std::min(static_cast<double>(max_level) / 25.0, static_cast<double>(best_ring_streak_) / 10.0));

    // Curiosities
    const bool in_graveyard = map_id.rfind("SG", 0) == 0 || map_id.rfind("GR", 0) == 0 || map_id.rfind("YM", 0) == 0;
    eval("curiosity_whispers_graveyard", in_graveyard, in_graveyard ? "Soul Graveyard reached" : "Explore hidden memories", in_graveyard ? 1.0 : 0.0);
    eval("curiosity_forgotten_page", treasures_found >= 10, std::to_string(treasures_found) + " / 10 hidden treasures", static_cast<double>(treasures_found) / 10.0);
    eval("curiosity_echoes_1915", active_disc >= 2 || regions_visited >= 5, "Disc " + std::to_string(active_disc) + ", " + std::to_string(regions_visited) + "/5 regions", static_cast<double>(regions_visited) / 5.0);
    eval("curiosity_unlikely_encounter", battles_entered >= 25 && ring_perfects >= 15, std::to_string(battles_entered) + "/25 battles, " + std::to_string(ring_perfects) + "/15 Perfects", std::min(static_cast<double>(battles_entered) / 25.0, static_cast<double>(ring_perfects) / 15.0));
    eval("curiosity_keeper_obscura", treasures_found >= 40 && regions_visited >= 6, std::to_string(treasures_found) + "/40 treasures, " + std::to_string(regions_visited) + "/6 regions", std::min(static_cast<double>(treasures_found) / 40.0, static_cast<double>(regions_visited) / 6.0));
}

int Manager::unlocked_count() const {
    int n = 0;
    for (const Entry &e : entries_) if (e.unlocked) ++n;
    return n;
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

void Manager::render_toast(const Entry &e, int window_w, int window_h, int theme_index, float slide_t, ToastPicture &out) {
    const std::string header = std::string(tier_short(e.tier)) + " UNLOCKED";
    const std::string title = e.title;
    const int chars = static_cast<int>(std::max<std::size_t>({header.size(), title.size(), 22u}));
    const int pad = 7, icon_box = 20, gap = 6;
    const int units_w = pad + icon_box + gap + chars * 6 + pad;
    const int units_h = pad + 8 + 4 + 8 + pad;
    const int s = std::clamp(std::min(window_w * 42 / 100 / units_w, window_h / 180), 1, 4);
    const int w = units_w * s;
    const int h = units_h * s;

    const int margin_x = std::max(12, window_w / 48);
    const int margin_y = std::max(12, window_h / 36);
    const int target_x = std::max(0, window_w - w - margin_x);
    const int offscreen_x = window_w + 4;
    const int cur_x = static_cast<int>(offscreen_x + (target_x - offscreen_x) * slide_t + 0.5f);

    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.x = static_cast<std::uint32_t>(std::max(0, cur_x));
    out.y = static_cast<std::uint32_t>(margin_y);
    out.active = true;

    std::uint32_t gl, gr, border, accent;
    theme_colors(e.tier, theme_index, gl, gr, border, accent);

    out.rgba.assign(static_cast<std::size_t>(w) * h, 0u);
    const int frame = std::max(2, s);
    // Smooth horizontal + subtle vertical gradient with beveled corner transparency
    for (int y = 0; y < h; ++y) {
        const float ty = h > 1 ? static_cast<float>(y) / static_cast<float>(h - 1) : 0.0f;
        for (int x = 0; x < w; ++x) {
            // Soft 2-pixel chamfered corners
            const int dx = std::min(x, w - 1 - x);
            const int dy = std::min(y, h - 1 - y);
            if (dx + dy < frame) continue;
            const bool is_border = (x < frame || x >= w - frame || y < frame || y >= h - frame || dx + dy < frame * 2);
            const float tx = w > 1 ? static_cast<float>(x) / static_cast<float>(w - 1) : 0.0f;
            std::uint32_t col;
            if (is_border) {
                col = lerp_rgba(border, accent, 0.35f * (1.0f - ty));
            } else {
                const std::uint32_t horiz = lerp_rgba(gl, gr, tx);
                const std::uint32_t dark = rgba_u32(8, 6, 12, 245);
                col = lerp_rgba(horiz, dark, ty * 0.22f);
            }
            out.rgba[static_cast<std::size_t>(y) * w + x] = col;
        }
    }

    const auto fill = [&](int fx, int fy, int fw, int fh, std::uint32_t col) {
        const int x0 = std::max(0, fx), y0 = std::max(0, fy), x1 = std::min(w, fx + fw), y1 = std::min(h, fy + fh);
        for (int j = y0; j < y1; ++j) std::fill(out.rgba.begin() + j * w + x0, out.rgba.begin() + j * w + x1, col);
    };
    const auto draw_text = [&](int tx, int ty, const std::string &str, std::uint32_t col) {
        for (unsigned char c : str) {
            const char *glyph = kFont[(c >= 32 && c <= 126 ? c : '?') - 32];
            for (int row = 0; row < 8; ++row)
                for (int col_i = 0; col_i < 5; ++col_i)
                    if (glyph[row * 6 + col_i] == '#') fill(tx + col_i * s, ty + row * s, s, s, col);
            tx += 6 * s;
        }
    };

    // Draw Emblem Badge Box on the left
    const int bx = pad * s;
    const int by = (h - icon_box * s) / 2;
    fill(bx, by, icon_box * s, icon_box * s, rgba_u32(12, 10, 18, 220));
    fill(bx, by, icon_box * s, s, border);
    fill(bx, by + (icon_box - 1) * s, icon_box * s, s, border);
    fill(bx, by, s, icon_box * s, border);
    fill(bx + (icon_box - 1) * s, by, s, icon_box * s, border);

    const int ti = std::clamp(static_cast<int>(e.tier), 0, 6);
    const int px_sz = std::max(1, (icon_box * s - 4 * s) / 9);
    const int ex0 = bx + (icon_box * s - 9 * px_sz) / 2;
    const int ey0 = by + (icon_box * s - 9 * px_sz) / 2;
    for (int row = 0; row < 9; ++row)
        for (int col_i = 0; col_i < 9; ++col_i)
            if (kEmblems[ti][row][col_i] == '#') fill(ex0 + col_i * px_sz, ey0 + row * px_sz, px_sz, px_sz, accent);

    // Draw Header (tier) + Achievement Title
    const int text_x = (pad + icon_box + gap) * s;
    draw_text(text_x, pad * s, header, accent);
    draw_text(text_x, (pad + 12) * s, title, rgba_u32(248, 245, 240, 255));
}

ToastTickResult Manager::tick_toast(double now_seconds, int window_w, int window_h, int theme_index, ToastPicture &out) {
    ToastTickResult res;
    constexpr double kSlideIn = 0.35;
    constexpr double kHold = 4.30;
    constexpr double kSlideOut = 0.35;
    constexpr double kTotal = kSlideIn + kHold + kSlideOut;
    constexpr double kShotAt = 0.60;

    if (!toast_active_) {
        if (toast_queue_.empty()) {
            out.active = false;
            return res;
        }
        toast_idx_ = toast_queue_.front();
        toast_queue_.erase(toast_queue_.begin());
        toast_active_ = true;
        toast_start_ = now_seconds;
        toast_shot_taken_ = false;
        last_toast_x_ = -99999;
        res.chime = true;
    }

    const double elapsed = now_seconds - toast_start_;
    if (elapsed >= kTotal) {
        toast_active_ = false;
        out.active = false;
        out.rgba.clear();
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

    const Entry &e = entries_[toast_idx_];
    if (!toast_shot_taken_ && elapsed >= kShotAt) {
        toast_shot_taken_ = true;
        res.take_screenshot = true;
        res.screenshot_title = e.title;
    }

    ToastPicture tmp;
    render_toast(e, window_w, window_h, theme_index, slide_t, tmp);
    if (static_cast<int>(tmp.x) != last_toast_x_ || theme_index != last_toast_theme_ || window_w != last_win_w_ || window_h != last_win_h_) {
        out = std::move(tmp);
        last_toast_x_ = static_cast<int>(out.x);
        last_toast_theme_ = theme_index;
        last_win_w_ = window_w;
        last_win_h_ = window_h;
        res.changed = true;
    }
    return res;
}

}  // namespace achievements
