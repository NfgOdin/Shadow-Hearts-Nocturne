#pragma once

// Shadow Hearts: Nocturne — Unified Collectible Card Album & Reward Engine
// Header-only C++20 data model, percentage rarity/foil/animated roller, Essence crafting,
// hidden map pickup definitions, and atomic JSON persistence shared across the Launcher,
// Koudelka (PS1), and Shadow Hearts 1/2/3 (PS2).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cards {

enum class SeriesGame : int {
    Koudelka = 0,
    ShadowHearts1 = 1,
    Covenant = 2,
    FromTheNewWorld = 3,
    Count = 4
};

inline const char *series_game_name(SeriesGame g) noexcept {
    switch (g) {
    case SeriesGame::Koudelka:        return "Koudelka";
    case SeriesGame::ShadowHearts1:   return "Shadow Hearts";
    case SeriesGame::Covenant:        return "Covenant";
    case SeriesGame::FromTheNewWorld: return "From the New World";
    default:                          return "Unknown";
    }
}

inline const char *series_game_short(SeriesGame g) noexcept {
    switch (g) {
    case SeriesGame::Koudelka:        return "KOUDELKA";
    case SeriesGame::ShadowHearts1:   return "SH I";
    case SeriesGame::Covenant:        return "SH II";
    case SeriesGame::FromTheNewWorld: return "SH III";
    default:                          return "NOCTURNE";
    }
}

enum class CardCategory : int {
    MainCharacter = 0,
    Npc = 1,
    Beast = 2,
    Count = 3
};

inline const char *category_name(CardCategory c) noexcept {
    switch (c) {
    case CardCategory::MainCharacter: return "Main Characters";
    case CardCategory::Npc:           return "NPCs";
    case CardCategory::Beast:         return "Beasts";
    default:                          return "All";
    }
}

inline const char *category_short(CardCategory c, bool is_boss = false) noexcept {
    if (c == CardCategory::Beast && is_boss) return "BOSS";
    switch (c) {
    case CardCategory::MainCharacter: return "MAIN";
    case CardCategory::Npc:           return "NPC";
    case CardCategory::Beast:         return "BEAST";
    default:                          return "CARD";
    }
}

// Hybrid Rarity Model:
//  - 5 core rarities (Common..Legendary), each of which can independently roll Foil shimmer.
//  - Animated is the ultra-rare pinnacle tier above Legendary (always includes Foil shimmer
//    plus a layered 6-10 second procedural animation flourish).
enum class Rarity : int {
    Common = 0,
    Uncommon = 1,
    Rare = 2,
    Exotic = 3,
    Legendary = 4,
    Animated = 5,
    Count = 6
};

// 11 collectible variants per card:
//   0=Common, 1=Common Foil, 2=Uncommon, 3=Uncommon Foil, 4=Rare, 5=Rare Foil,
//   6=Exotic, 7=Exotic Foil, 8=Legendary, 9=Legendary Foil, 10=Animated
constexpr int kVariantCount = 11;

inline int variant_index(Rarity r, bool foil) noexcept {
    if (r == Rarity::Animated) return 10;
    const int ri = std::clamp(static_cast<int>(r), 0, 4);
    return ri * 2 + (foil ? 1 : 0);
}

inline Rarity variant_rarity(int vidx) noexcept {
    if (vidx >= 10) return Rarity::Animated;
    return static_cast<Rarity>(std::clamp(vidx / 2, 0, 4));
}

inline bool variant_is_foil(int vidx) noexcept {
    if (vidx >= 10) return true;  // Animated always has foil shimmer
    return (vidx & 1) != 0;
}

inline const char *rarity_name(Rarity r) noexcept {
    switch (r) {
    case Rarity::Common:    return "Common";
    case Rarity::Uncommon:  return "Uncommon";
    case Rarity::Rare:      return "Rare";
    case Rarity::Exotic:    return "Exotic";
    case Rarity::Legendary: return "Legendary";
    case Rarity::Animated:  return "Animated";
    default:                return "Common";
    }
}

inline std::string variant_label(Rarity r, bool foil) {
    if (r == Rarity::Animated) return "Animated";
    std::string s = rarity_name(r);
    if (foil) s += " Foil";
    return s;
}

inline std::string variant_label(int vidx) {
    return variant_label(variant_rarity(vidx), variant_is_foil(vidx));
}

// Essence values for dismantling duplicates & crafting specific variants
inline int dismantle_essence_value(int vidx) noexcept {
    static constexpr int kBase[6] = {10, 25, 60, 150, 400, 1000};
    const Rarity r = variant_rarity(vidx);
    if (r == Rarity::Animated) return kBase[5];
    const int base = kBase[static_cast<int>(r)];
    return variant_is_foil(vidx) ? (base * 2) : base;
}

inline int craft_essence_cost(int vidx) noexcept {
    static constexpr int kBase[6] = {40, 100, 240, 600, 1600, 5000};
    const Rarity r = variant_rarity(vidx);
    if (r == Rarity::Animated) return kBase[5];
    const int base = kBase[static_cast<int>(r)];
    return variant_is_foil(vidx) ? (base * 2) : base;
}

// Primary RGBA accent colors (0xRRGGBB) per rarity tier
inline std::uint32_t rarity_color_rgb(Rarity r) noexcept {
    switch (r) {
    case Rarity::Common:    return 0xA8B0BCu;  // Weathered Pewter
    case Rarity::Uncommon:  return 0x52C488u;  // Monastery Emerald
    case Rarity::Rare:      return 0x58A6F8u;  // Astral Sapphire
    case Rarity::Exotic:    return 0xD85CF0u;  // Arcane Amethyst
    case Rarity::Legendary: return 0xF5C24Cu;  // Gilded Gold
    case Rarity::Animated:  return 0xFF78B4u;  // Prismatic Rose-Gold
    default:                return 0xA8B0BCu;
    }
}

struct CardDef {
    std::string id;
    std::string name;
    std::string subtitle;
    std::string arcana;
    SeriesGame game = SeriesGame::Koudelka;
    CardCategory category = CardCategory::MainCharacter;
    bool is_boss = false;
    std::string art_rel_path;       // Relative to repo/install root (e.g. "assets/cards/koudelka/koudelka_art.png")
    std::string lore;
    float anim_interval_sec = 8.0f; // 6.0..10.0s dramatic flourish cycle for Animated tier
    std::uint32_t aura_rgb = 0xD8A850u;
    // Reserved extensible fields for the future in-launcher Card Game
    int future_power = 0;
    std::string future_element;
};

struct HiddenPickupDef {
    std::string id;
    SeriesGame game = SeriesGame::Koudelka;
    std::uint32_t room_id = 0;
    std::string area_name;
    std::string hint;
    std::string fixed_card_id;  // Empty = roll from game's pool
    int luck_tier = 1;          // 1 = boosted, 2 = high-luck
};

inline const std::vector<CardDef> &catalog() {
    static const std::vector<CardDef> kCards = {
        // ==================== KOUDELKA — MAIN CHARACTERS (3) ====================
        {
            "kdl_koudelka",
            "Koudelka Iasant",
            "The Clairvoyant Medium",
            "II  •  THE HIGH PRIESTESS",
            SeriesGame::Koudelka,
            CardCategory::MainCharacter,
            false,
            "assets/cards/koudelka/koudelka_art.png",
            "A sharp-tongued young Romani medium drawn to Nemeton Monastery on Halloween night, 1898, "
            "by a restless spirit's plea. Though haunted by visions of the dead, her resolve burns brighter "
            "than the cursed candles of the abbey.",
            7.0f, 0xD94B7Au, 0, "Dark"
        },
        {
            "kdl_edward",
            "Edward Plunkett",
            "The Reluctant Adventurer",
            "VII  •  THE CHARIOT",
            SeriesGame::Koudelka,
            CardCategory::MainCharacter,
            false,
            "assets/cards/koudelka/edward_art.png",
            "A brash, quick-witted Londoner who entered Nemeton Monastery chasing rumors of hidden treasure "
            "and adventure—only to find himself fighting alongside a psychic and a bishop to survive the night.",
            8.5f, 0xE8A844u, 0, "Fire"
        },
        {
            "kdl_james",
            "James O'Flaherty",
            "Bishop of the Vatican",
            "V  •  THE HIEROPHANT",
            SeriesGame::Koudelka,
            CardCategory::MainCharacter,
            false,
            "assets/cards/koudelka/james_art.png",
            "An austere, razor-minded emissary of the Vatican dispatched to recover a forbidden grimoire "
            "stolen from Rome. Beneath his stern composure lies a fierce devotion to Elaine's memory.",
            10.0f, 0x66A8F0u, 0, "Light"
        },

        // ==================== KOUDELKA — NPCs (7) ====================
        {
            "kdl_patrick",
            "Patrick Heyworth",
            "Master of Nemeton",
            "IV  •  THE EMPEROR",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/patrick_art.png",
            "The grieving aristocrat who turned Nemeton Monastery into a forbidden laboratory of resurrection, "
            "willing to tear open the veil between life and death to bring back his murdered wife.",
            9.0f, 0x9C58D8u, 0, "Dark"
        },
        {
            "kdl_elaine",
            "Elaine Heyworth",
            "The Calling Spirit",
            "III  •  THE EMPRESS",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/elaine_art.png",
            "A gentle soul whose tragic death triggered the nightmare of Nemeton. Her spectral voice summoned "
            "Koudelka across the sea, begging to be laid to rest before Patrick's ritual consumed everything.",
            7.5f, 0x88D4F8u, 0, "Light"
        },
        {
            "kdl_ogden",
            "Ogden Hartman",
            "Caretaker of the Abbey",
            "IX  •  THE HERMIT",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/ogden_art.png",
            "The weary groundskeeper of Nemeton Monastery. Polite yet deeply guarded, Ogden knows far more "
            "about the abbey's blood-stained past than he dares admit to trespassers.",
            8.0f, 0xC89858u, 0, "Earth"
        },
        {
            "kdl_bessy",
            "Bessy Hartman",
            "Mistress of the Manor",
            "VI  •  THE LOVERS",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/bessy_art.png",
            "Ogden's devoted wife, tending to the decrepit abbey long after sanity fled its halls. "
            "Her uneasy warmth hides a quiet sorrow bound to the monastery's darkest secret.",
            8.0f, 0xD47A92u, 0, "Water"
        },
        {
            "kdl_roger",
            "Roger Bacon",
            "The Immortal Alchemist",
            "I  •  THE MAGICIAN",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/roger_art.png",
            "An eccentric, centuries-old sage who crafted the Émigré Manuscript. Appearing out of nowhere "
            "with cryptic riddles, Roger watches the fate of Nemeton—and the Shadow Hearts era to come.",
            6.5f, 0x52D0A4u, 0, "Wind"
        },
        {
            "kdl_charlotte",
            "Charlotte D'Lota",
            "The Weeping Maiden",
            "XII  •  THE HANGED MAN",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/charlotte_art.png",
            "A forsaken young noblewoman betrayed and left to perish within the monastery's prison. "
            "Her longing for a promised ring still echoes down the damp stone corridors.",
            9.5f, 0xB86CE8u, 0, "Water"
        },
        {
            "kdl_alias",
            "Alias",
            "The Wandering Peddler",
            "0  •  THE FOOL",
            SeriesGame::Koudelka,
            CardCategory::Npc,
            false,
            "assets/cards/koudelka/alias_art.png",
            "A bizarre, unsettling figure roaming the haunted monastery with a sack of curiosities, "
            "offering strange bargains to anyone brave—or foolish—enough to trade in the dark.",
            6.0f, 0xD8B44Cu, 0, "Earth"
        },

        // ==================== KOUDELKA — BEASTS (5 Regular + 5 Bosses) ====================
        {
            "kdl_sorrow",
            "Sorrow",
            "Wailing Spirit of Nemeton",
            "XIII  •  DEATH",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            false,
            "assets/cards/koudelka/sorrow_art.png",
            "A gaunt apparition born from decades of torment inside the abbey. Its hollow lament "
            "drains the warmth from the living before its spectral claws strike.",
            7.0f, 0x7A88C8u, 0, "Dark"
        },
        {
            "kdl_banshee",
            "Banshee",
            "Herald of the Grave",
            "XVIII  •  THE MOON",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            false,
            "assets/cards/koudelka/banshee_art.png",
            "A shrieking phantom drifting through moonlit cloisters. Old Welsh folklore warns that "
            "hearing its wail inside Nemeton means death is already standing behind you.",
            6.5f, 0x9C72F0u, 0, "Wind"
        },
        {
            "kdl_mimic",
            "Mimic",
            "Cursed Reliquary",
            "XV  •  THE DEVIL",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            false,
            "assets/cards/koudelka/mimic_art.png",
            "A grotesque trap animated by Nemeton's malice, disguising itself as antique furnishings "
            "to ambush greedy treasure hunters like Edward.",
            8.0f, 0xC86442u, 0, "Earth"
        },
        {
            "kdl_bone_knight",
            "Bone Knight",
            "Risen Abbey Sentinel",
            "XI  •  JUSTICE",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            false,
            "assets/cards/koudelka/bone_knight_art.png",
            "Skeletal remains of forgotten soldiers clad in rusted plate, marching endlessly through "
            "the monastery courtyard with rusted broadswords raised.",
            8.5f, 0xB8C0CCu, 0, "Earth"
        },
        {
            "kdl_cursed_armor",
            "Cursed Armor",
            "Hollow Iron Husk",
            "VIII  •  STRENGTH",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            false,
            "assets/cards/koudelka/cursed_armor_art.png",
            "Empty Victorian ceremonial armor possessed by vengeful spirits. Its heavy footsteps "
            "shake the floorboards long before it emerges from the candlelight.",
            9.0f, 0xD45252u, 0, "Fire"
        },

        // -------------------- KOUDELKA — BOSS BEASTS (Gilded Boss Crown) --------------------
        {
            "kdl_gargoyle",
            "Gargoyle",
            "Stone Guardian of the Belltower",
            "XVI  •  THE TOWER",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            true,
            "assets/cards/koudelka/gargoyle_art.png",
            "A monstrous winged sentinel torn free from the cathedral spire by dark magic. "
            "Its thunderous swoop tests every intruder who dares ascend Nemeton.",
            7.5f, 0xE07A48u, 0, "Wind"
        },
        {
            "kdl_plant_boss",
            "Rafflesia",
            "Blight of the Conservatory",
            "X  •  WHEEL OF FORTUNE",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            true,
            "assets/cards/koudelka/rafflesia_art.png",
            "A towering carnivorous horror blooming in the overgrown monastery greenhouse, "
            "choking the air with venomous spores and thrashing thorny vines.",
            8.5f, 0x4CC878u, 0, "Earth"
        },
        {
            "kdl_charlotte_boss",
            "Charlotte (Wraith)",
            "Specter of Broken Vows",
            "XIV  •  TEMPERANCE",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            true,
            "assets/cards/koudelka/charlotte_boss_art.png",
            "Consumed by heartbreak and fury, Charlotte's spirit manifests as a terrifying "
            "crimson wraith—unleashing relentless arcane storms upon the living.",
            7.0f, 0xE04C9Cu, 0, "Water"
        },
        {
            "kdl_elaine_aberration",
            "Elaine (Aberration)",
            "The Failed Resurrection",
            "XX  •  JUDGEMENT",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            true,
            "assets/cards/koudelka/elaine_aberration_art.png",
            "The horrifying consequence of Patrick's forbidden Émigré ritual—a twisted vessel "
            "of divine agony and unholy flesh crying out from beneath the altar.",
            9.5f, 0xF05858u, 0, "Dark"
        },
        {
            "kdl_elaine_final",
            "Elaine (Final Form)",
            "Seraph of Nemeton's Ruin",
            "XXI  •  THE WORLD",
            SeriesGame::Koudelka,
            CardCategory::Beast,
            true,
            "assets/cards/koudelka/elaine_final_art.png",
            "The cataclysmic apotheosis of the Émigré Manuscript's curse. Towering above the "
            "shattered sanctuary, she embodies both the tragedy of 1898 and the dawn of Shadow Hearts.",
            10.0f, 0xFFD060u, 0, "Light"
        },
    };
    return kCards;
}

inline const CardDef *find_card_def(const std::string &id) {
    for (const auto &c : catalog()) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

inline const std::vector<HiddenPickupDef> &hidden_pickups() {
    static const std::vector<HiddenPickupDef> kPickups = {
        {"kdl_map_01", SeriesGame::Koudelka, 0x01u, "Nemeton Entrance Hall",
         "Tucked beneath the shadowed stonework near the monastery entrance.", "kdl_koudelka", 1},
        {"kdl_map_02", SeriesGame::Koudelka, 0x05u, "Edward's Encounter Room",
         "Left behind where Koudelka first crossed paths with Edward Plunkett.", "kdl_edward", 1},
        {"kdl_map_03", SeriesGame::Koudelka, 0x0Au, "Caretakers' Quarters",
         "Hidden in the quiet manor halls watched over by Ogden and Bessy.", "kdl_ogden", 1},
        {"kdl_map_04", SeriesGame::Koudelka, 0x0Du, "Monastery Dining Hall",
         "A faded tarot card resting beneath a dusty silver candelabrum.", "kdl_bessy", 1},
        {"kdl_map_05", SeriesGame::Koudelka, 0x12u, "Abbey Sanctuary",
         "An ancient tarot relic resting in the heart of Nemeton Monastery.", "kdl_james", 2},
        {"kdl_map_06", SeriesGame::Koudelka, 0x18u, "Alchemist's Study",
         "Left behind by Roger Bacon beside an open parchment of runes.", "kdl_roger", 2},
    };
    return kPickups;
}

struct OwnedCard {
    std::array<int, kVariantCount> counts{};
    std::string first_acquired;
    std::string last_source;

    int total_copies() const noexcept {
        int s = 0;
        for (int c : counts) s += c;
        return s;
    }

    bool unlocked() const noexcept {
        return total_copies() > 0;
    }

    int highest_variant() const noexcept {
        for (int i = kVariantCount - 1; i >= 0; --i) {
            if (counts[static_cast<std::size_t>(i)] > 0) return i;
        }
        return -1;
    }

    bool has_foil() const noexcept {
        for (int i = 0; i < kVariantCount; ++i) {
            if (counts[static_cast<std::size_t>(i)] > 0 && variant_is_foil(i)) return true;
        }
        return false;
    }

    bool has_animated() const noexcept {
        return counts[10] > 0;
    }
};

struct DropRecord {
    std::string card_id;
    Rarity rarity = Rarity::Common;
    bool foil = false;
    bool is_new_card = false;
    bool is_new_variant = false;
    std::string source;
    std::string timestamp;
};

namespace detail {

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

inline std::string extract_json_string(const std::string &obj, const char *key) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t k = obj.find(needle);
    if (k == std::string::npos) return {};
    const std::size_t colon = obj.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    const std::size_t q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    std::string out;
    for (std::size_t i = q1 + 1; i < obj.size(); ++i) {
        if (obj[i] == '\\' && i + 1 < obj.size()) {
            out.push_back(obj[++i]);
        } else if (obj[i] == '"') {
            break;
        } else {
            out.push_back(obj[i]);
        }
    }
    return out;
}

inline int extract_json_int(const std::string &obj, const char *key, int def = 0) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t k = obj.find(needle);
    if (k == std::string::npos) return def;
    const std::size_t colon = obj.find(':', k + needle.size());
    if (colon == std::string::npos) return def;
    std::size_t p = colon + 1;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
    if (p >= obj.size()) return def;
    return std::atoi(obj.c_str() + p);
}

}  // namespace detail

class Collection {
public:
    int essence = 0;
    std::unordered_map<std::string, OwnedCard> owned;
    std::unordered_set<std::string> collected_pickups;
    std::vector<DropRecord> history;  // newest last, capped at 40
    std::filesystem::path file_path;
    std::filesystem::file_time_type last_mtime{};
    bool has_mtime = false;

    static std::filesystem::path default_path_for_root(const std::filesystem::path &root_dir) {
        return root_dir / "cards_collection.json";
    }

    int unique_cards_owned() const noexcept {
        int n = 0;
        for (const auto &c : catalog()) {
            auto it = owned.find(c.id);
            if (it != owned.end() && it->second.unlocked()) ++n;
        }
        return n;
    }

    int total_copies_owned() const noexcept {
        int n = 0;
        for (const auto &kv : owned) n += kv.second.total_copies();
        return n;
    }

    int total_foils_owned() const noexcept {
        int n = 0;
        for (const auto &kv : owned) {
            for (int i = 0; i < kVariantCount; ++i) {
                if (variant_is_foil(i)) n += kv.second.counts[static_cast<std::size_t>(i)];
            }
        }
        return n;
    }

    int total_animated_owned() const noexcept {
        int n = 0;
        for (const auto &kv : owned) n += kv.second.counts[10];
        return n;
    }

    const OwnedCard *find_owned(const std::string &card_id) const {
        auto it = owned.find(card_id);
        return (it != owned.end()) ? &it->second : nullptr;
    }

    bool reload_if_changed() {
        if (file_path.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file_path, ec)) return false;
        const auto mt = std::filesystem::last_write_time(file_path, ec);
        if (ec) return false;
        if (has_mtime && mt == last_mtime) return false;
        return load(file_path);
    }

    bool load(const std::filesystem::path &path) {
        file_path = path;
        essence = 0;
        owned.clear();
        collected_pickups.clear();
        history.clear();

        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            has_mtime = false;
            return false;
        }
        last_mtime = std::filesystem::last_write_time(path, ec);
        has_mtime = !ec;

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        const std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        essence = std::max(0, detail::extract_json_int(text, "essence", 0));

        // Parse "owned": [ { ... }, ... ]
        auto parse_array_objects = [&](const char *arr_key, auto &&fn) {
            const std::string needle = std::string("\"") + arr_key + "\"";
            const std::size_t k = text.find(needle);
            if (k == std::string::npos) return;
            const std::size_t lb = text.find('[', k + needle.size());
            if (lb == std::string::npos) return;
            std::size_t pos = lb + 1;
            int depth = 0;
            std::size_t obj_start = std::string::npos;
            bool in_str = false;
            for (; pos < text.size(); ++pos) {
                const char ch = text[pos];
                if (in_str) {
                    if (ch == '\\' && pos + 1 < text.size()) { ++pos; continue; }
                    if (ch == '"') in_str = false;
                    continue;
                }
                if (ch == '"') { in_str = true; continue; }
                if (ch == ']' && depth == 0) break;
                if (ch == '{') {
                    if (depth == 0) obj_start = pos;
                    ++depth;
                } else if (ch == '}') {
                    --depth;
                    if (depth == 0 && obj_start != std::string::npos) {
                        fn(text.substr(obj_start, pos - obj_start + 1));
                        obj_start = std::string::npos;
                    }
                }
            }
        };

        parse_array_objects("owned", [&](const std::string &obj) {
            const std::string cid = detail::extract_json_string(obj, "id");
            if (cid.empty()) return;
            OwnedCard oc;
            oc.first_acquired = detail::extract_json_string(obj, "first_acquired");
            oc.last_source = detail::extract_json_string(obj, "last_source");
            const std::size_t ck = obj.find("\"counts\"");
            if (ck != std::string::npos) {
                const std::size_t lb = obj.find('[', ck);
                const std::size_t rb = (lb != std::string::npos) ? obj.find(']', lb) : std::string::npos;
                if (lb != std::string::npos && rb != std::string::npos) {
                    std::size_t p = lb + 1;
                    int idx = 0;
                    while (p < rb && idx < kVariantCount) {
                        while (p < rb && (obj[p] == ' ' || obj[p] == ',' || obj[p] == '\t')) ++p;
                        if (p >= rb) break;
                        char *endp = nullptr;
                        const long val = std::strtol(obj.c_str() + p, &endp, 10);
                        oc.counts[static_cast<std::size_t>(idx++)] = std::max(0, static_cast<int>(val));
                        if (!endp || endp == obj.c_str() + p) break;
                        p = static_cast<std::size_t>(endp - obj.c_str());
                    }
                }
            }
            owned[cid] = std::move(oc);
        });

        // Parse "pickups": ["id1", "id2"]
        {
            const std::size_t pk = text.find("\"pickups\"");
            if (pk != std::string::npos) {
                const std::size_t lb = text.find('[', pk);
                const std::size_t rb = (lb != std::string::npos) ? text.find(']', lb) : std::string::npos;
                if (lb != std::string::npos && rb != std::string::npos) {
                    std::size_t p = lb + 1;
                    while (p < rb) {
                        const std::size_t q1 = text.find('"', p);
                        if (q1 == std::string::npos || q1 >= rb) break;
                        const std::size_t q2 = text.find('"', q1 + 1);
                        if (q2 == std::string::npos || q2 > rb) break;
                        collected_pickups.insert(text.substr(q1 + 1, q2 - q1 - 1));
                        p = q2 + 1;
                    }
                }
            }
        }

        parse_array_objects("history", [&](const std::string &obj) {
            DropRecord r;
            r.card_id = detail::extract_json_string(obj, "card_id");
            r.rarity = static_cast<Rarity>(std::clamp(detail::extract_json_int(obj, "rarity", 0), 0, 5));
            r.foil = detail::extract_json_int(obj, "foil", 0) != 0;
            r.source = detail::extract_json_string(obj, "source");
            r.timestamp = detail::extract_json_string(obj, "timestamp");
            if (!r.card_id.empty()) history.push_back(std::move(r));
        });

        return true;
    }

    bool save() const {
        if (file_path.empty()) return false;
        std::error_code ec;
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path(), ec);
        }
        std::string out;
        out.reserve(4096);
        out += "{\n";
        out += "  \"version\": 1,\n";
        out += "  \"essence\": " + std::to_string(essence) + ",\n";

        out += "  \"owned\": [\n";
        bool first = true;
        for (const auto &c : catalog()) {
            auto it = owned.find(c.id);
            if (it == owned.end() || !it->second.unlocked()) continue;
            if (!first) out += ",\n";
            first = false;
            const OwnedCard &oc = it->second;
            out += "    {\"id\": \"" + detail::json_escape(c.id) + "\", \"counts\": [";
            for (int i = 0; i < kVariantCount; ++i) {
                if (i > 0) out += ",";
                out += std::to_string(oc.counts[static_cast<std::size_t>(i)]);
            }
            out += "], \"first_acquired\": \"" + detail::json_escape(oc.first_acquired) +
                   "\", \"last_source\": \"" + detail::json_escape(oc.last_source) + "\"}";
        }
        out += "\n  ],\n";

        out += "  \"pickups\": [";
        bool pfirst = true;
        for (const auto &pid : collected_pickups) {
            if (!pfirst) out += ", ";
            pfirst = false;
            out += "\"" + detail::json_escape(pid) + "\"";
        }
        out += "],\n";

        out += "  \"history\": [\n";
        for (std::size_t i = 0; i < history.size(); ++i) {
            const auto &h = history[i];
            if (i > 0) out += ",\n";
            out += "    {\"card_id\": \"" + detail::json_escape(h.card_id) +
                   "\", \"rarity\": " + std::to_string(static_cast<int>(h.rarity)) +
                   ", \"foil\": " + std::to_string(h.foil ? 1 : 0) +
                   ", \"source\": \"" + detail::json_escape(h.source) +
                   "\", \"timestamp\": \"" + detail::json_escape(h.timestamp) + "\"}";
        }
        out += "\n  ]\n";
        out += "}\n";

        const std::filesystem::path tmp = file_path.string() + ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(out.data(), static_cast<std::streamsize>(out.size()));
            if (!ofs.good()) return false;
        }
        std::filesystem::rename(tmp, file_path, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(tmp, file_path, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
        }
        return !ec;
    }

    // Grant a specific card + rarity + foil directly
    DropRecord grant_card(const std::string &card_id, Rarity rarity, bool foil, const std::string &source) {
        if (rarity == Rarity::Animated) foil = true;
        const int vidx = variant_index(rarity, foil);
        OwnedCard &oc = owned[card_id];
        const bool new_card = !oc.unlocked();
        const bool new_var = (oc.counts[static_cast<std::size_t>(vidx)] == 0);
        oc.counts[static_cast<std::size_t>(vidx)] += 1;
        const std::string ts = detail::now_timestamp();
        if (oc.first_acquired.empty()) oc.first_acquired = ts;
        oc.last_source = source;

        DropRecord rec;
        rec.card_id = card_id;
        rec.rarity = rarity;
        rec.foil = foil;
        rec.is_new_card = new_card;
        rec.is_new_variant = new_var;
        rec.source = source;
        rec.timestamp = ts;

        history.push_back(rec);
        if (history.size() > 40u) {
            history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - 40u));
        }
        save();
        return rec;
    }

    // Roll a single card from a game's pool using percentage drop tables
    // luck_tier: 0 = Battle drop, 1 = Trophy / Hidden Pickup, 2 = Boss / High-Tier Trophy
    // category_filter: -1 = any, 0 = MainCharacter, 1 = Npc, 2 = Beast
    // boss_bias: when true, favors Boss Beasts & Main Characters
    DropRecord roll_single_card(SeriesGame game, int luck_tier, const std::string &source,
                                const std::string &fixed_card_id = {},
                                int category_filter = -1, bool boss_bias = false) {
        static thread_local std::mt19937_64 rng([] {
            std::random_device rd;
            const auto t = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            return std::mt19937_64((static_cast<std::uint64_t>(rd()) << 32) ^ t ^ 0x9E3779B97F4A7C15ull);
        }());

        std::string chosen_id = fixed_card_id;
        if (chosen_id.empty()) {
            std::vector<const CardDef *> pool;
            for (const auto &c : catalog()) {
                if (c.game != game) continue;
                if (category_filter >= 0 && static_cast<int>(c.category) != category_filter) continue;
                if (boss_bias && !c.is_boss && c.category != CardCategory::MainCharacter) continue;
                pool.push_back(&c);
            }
            if (pool.empty()) {
                for (const auto &c : catalog()) {
                    if (c.game == game) pool.push_back(&c);
                }
            }
            if (pool.empty()) {
                for (const auto &c : catalog()) pool.push_back(&c);
            }
            std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1u);
            chosen_id = pool[pick(rng)]->id;
        }

        // Percentage roll [0.0, 100.0)
        std::uniform_real_distribution<double> pct(0.0, 100.0);
        const double r = pct(rng);
        Rarity rar = Rarity::Common;
        double foil_chance = 9.0;

        if (luck_tier >= 2) {
            // High-Luck (Boss / Gold+ Trophy): 12 / 23 / 28 / 22 / 12 / 3.0% Animated
            if (r < 3.0)       rar = Rarity::Animated;
            else if (r < 15.0) rar = Rarity::Legendary;
            else if (r < 37.0) rar = Rarity::Exotic;
            else if (r < 65.0) rar = Rarity::Rare;
            else if (r < 88.0) rar = Rarity::Uncommon;
            else               rar = Rarity::Common;
            foil_chance = 20.0;
        } else if (luck_tier == 1) {
            // Boosted (Trophy / Hidden Map Pickup): 30 / 28 / 22 / 13 / 5.5 / 1.5% Animated
            if (r < 1.5)       rar = Rarity::Animated;
            else if (r < 7.0)  rar = Rarity::Legendary;
            else if (r < 20.0) rar = Rarity::Exotic;
            else if (r < 42.0) rar = Rarity::Rare;
            else if (r < 70.0) rar = Rarity::Uncommon;
            else               rar = Rarity::Common;
            foil_chance = 14.0;
        } else {
            // Standard (Battle Victory): 50 / 26 / 14 / 7 / 2.5 / 0.5% Animated
            if (r < 0.5)       rar = Rarity::Animated;
            else if (r < 3.0)  rar = Rarity::Legendary;
            else if (r < 10.0) rar = Rarity::Exotic;
            else if (r < 24.0) rar = Rarity::Rare;
            else if (r < 50.0) rar = Rarity::Uncommon;
            else               rar = Rarity::Common;
            foil_chance = 9.0;
        }

        const bool foil = (rar == Rarity::Animated) ? true : (pct(rng) < foil_chance);
        return grant_card(chosen_id, rar, foil, source);
    }

    // Dismantle 1 duplicate copy (requires count > 1 so players never lose their last copy)
    bool dismantle_duplicate(const std::string &card_id, int vidx, int &gained_out) {
        gained_out = 0;
        if (vidx < 0 || vidx >= kVariantCount) return false;
        auto it = owned.find(card_id);
        if (it == owned.end()) return false;
        int &cnt = it->second.counts[static_cast<std::size_t>(vidx)];
        if (cnt <= 1) return false;
        --cnt;
        gained_out = dismantle_essence_value(vidx);
        essence += gained_out;
        save();
        return true;
    }

    // Craft a specific rarity/foil variant using Essence
    bool craft_variant(const std::string &card_id, int vidx) {
        if (vidx < 0 || vidx >= kVariantCount || !find_card_def(card_id)) return false;
        const int cost = craft_essence_cost(vidx);
        if (essence < cost) return false;
        essence -= cost;
        grant_card(card_id, variant_rarity(vidx), variant_is_foil(vidx), "Crafted with Essence");
        return true;
    }
};

}  // namespace cards
