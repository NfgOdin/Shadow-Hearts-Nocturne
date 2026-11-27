#pragma once

// Shadow Hearts: Nocturne — Unified Collectible Card Album & Fixed-Rarity Reward Engine
// Header-only C++20 data model for all 4 chronicles (Koudelka, Shadow Hearts I,
// Covenant, From the New World):
//   - Fixed per-card Rarity (Common..Legendary) determined by creature/boss difficulty
//   - 3 collectible cosmetic Finishes per card (Standard, Foil, Animated)
//   - Judgment Ring Duel stats (Soul Cost, Power, Element, Ability) & obtain hints
//   - Essence crafting/dismantling, 20-card deck persistence, and atomic JSON save/load
//     with automatic lossless migration from legacy 11-variant collections.

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
    CrestSpell = 3,
    Count = 4
};

inline const char *category_name(CardCategory c) noexcept {
    switch (c) {
    case CardCategory::MainCharacter: return "Main Characters";
    case CardCategory::Npc:           return "NPCs";
    case CardCategory::Beast:         return "Beasts";
    case CardCategory::CrestSpell:    return "Crests & Spells";
    default:                          return "All";
    }
}

inline const char *category_short(CardCategory c, bool is_boss = false) noexcept {
    if (c == CardCategory::Beast && is_boss) return "BOSS";
    switch (c) {
    case CardCategory::MainCharacter: return "MAIN";
    case CardCategory::Npc:           return "NPC";
    case CardCategory::Beast:         return "BEAST";
    case CardCategory::CrestSpell:    return "CREST";
    default:                          return "CARD";
    }
}

// Fixed Per-Card Rarity (determined by creature/boss difficulty & story tier)
enum class Rarity : int {
    Common = 0,     // Early-game enemies, minor NPCs
    Uncommon = 1,   // Mid-game enemies, recurring NPCs, early mini-bosses
    Rare = 2,       // Main party characters, late-game beasts, major story bosses
    Exotic = 3,     // Optional/secret bosses, Harmonixer fusions, major villains
    Legendary = 4,  // Final bosses, superbosses, apex mythic forms
    Count = 5
};

// Cosmetic Collector Finish (does NOT alter a card's fixed Rarity)
enum class CardFinish : int {
    Standard = 0,
    Foil = 1,
    Animated = 2,
    Count = 3
};

constexpr int kFinishCount = 3;

inline const char *rarity_name(Rarity r) noexcept {
    switch (r) {
    case Rarity::Common:    return "Common";
    case Rarity::Uncommon:  return "Uncommon";
    case Rarity::Rare:      return "Rare";
    case Rarity::Exotic:    return "Exotic";
    case Rarity::Legendary: return "Legendary";
    default:                return "Common";
    }
}

inline const char *finish_name(CardFinish f) noexcept {
    switch (f) {
    case CardFinish::Standard: return "Standard";
    case CardFinish::Foil:     return "Foil";
    case CardFinish::Animated: return "Animated";
    default:                   return "Standard";
    }
}

inline const char *finish_name(int fidx) noexcept {
    return finish_name(static_cast<CardFinish>(std::clamp(fidx, 0, kFinishCount - 1)));
}

inline bool finish_is_foil(CardFinish f) noexcept {
    return f == CardFinish::Foil || f == CardFinish::Animated;
}

inline bool finish_is_animated(CardFinish f) noexcept {
    return f == CardFinish::Animated;
}

inline std::string card_tier_label(Rarity r, CardFinish f) {
    std::string s = rarity_name(r);
    if (f == CardFinish::Foil) s += " Foil";
    else if (f == CardFinish::Animated) s += " Animated";
    return s;
}

// Backward-compatible helper used by host/psx_main.cpp toast formatting
inline std::string variant_label(Rarity r, bool foil) {
    return card_tier_label(r, foil ? CardFinish::Foil : CardFinish::Standard);
}

inline std::string variant_label(Rarity r, CardFinish f) {
    return card_tier_label(r, f);
}

// Primary RGBA accent colors (0xRRGGBB) per fixed rarity tier
inline std::uint32_t rarity_color_rgb(Rarity r) noexcept {
    switch (r) {
    case Rarity::Common:    return 0xA8B0BCu;  // Weathered Pewter
    case Rarity::Uncommon:  return 0x52C488u;  // Monastery Emerald
    case Rarity::Rare:      return 0x58A6F8u;  // Astral Sapphire
    case Rarity::Exotic:    return 0xD85CF0u;  // Arcane Amethyst
    case Rarity::Legendary: return 0xF5C24Cu;  // Gilded Gold
    default:                return 0xA8B0BCu;
    }
}

// Essence values scale by both fixed Rarity and cosmetic Finish
inline int dismantle_essence_value(Rarity r, CardFinish f) noexcept {
    static constexpr int kBase[5] = {12, 28, 65, 160, 420};
    const int base = kBase[std::clamp(static_cast<int>(r), 0, 4)];
    if (f == CardFinish::Animated) return base * 4;
    if (f == CardFinish::Foil) return base * 2;
    return base;
}

inline int craft_essence_cost(Rarity r, CardFinish f) noexcept {
    static constexpr int kBase[5] = {45, 110, 260, 640, 1600};
    const int base = kBase[std::clamp(static_cast<int>(r), 0, 4)];
    if (f == CardFinish::Animated) return base * 4;
    if (f == CardFinish::Foil) return base * 2;
    return base;
}

// Judgment Ring Duel Elements & Abilities
enum class Element : int {
    Fire = 0,
    Water = 1,
    Wind = 2,
    Earth = 3,
    Light = 4,
    Dark = 5,
    Count = 6
};

inline const char *element_name(Element e) noexcept {
    switch (e) {
    case Element::Fire:  return "Fire";
    case Element::Water: return "Water";
    case Element::Wind:  return "Wind";
    case Element::Earth: return "Earth";
    case Element::Light: return "Light";
    case Element::Dark:  return "Dark";
    default:             return "Dark";
    }
}

inline std::uint32_t element_color_rgb(Element e) noexcept {
    switch (e) {
    case Element::Fire:  return 0xF06A42u;
    case Element::Water: return 0x4CAEE8u;
    case Element::Wind:  return 0x5CD69Cu;
    case Element::Earth: return 0xC89B54u;
    case Element::Light: return 0xF8E288u;
    case Element::Dark:  return 0xA86CF0u;
    default:             return 0xA86CF0u;
    }
}

enum class AbilityId : int {
    None = 0,
    Resonance = 1,     // +2 Power (+3 total w/ Harmony) if adjacent to same Element
    Ward = 2,          // Survives 1 lost Altar clash without being banished
    SoulDrain = 3,     // On Altar win, restore +2 Sanity
    MaliceStrike = 4,  // On Altar win, deal +2 bonus Sanity damage
    Harmonixer = 5,    // When Fused over an ally, gain +2 extra Power (instead of +1)
    ArcaneEcho = 6,    // On Play: grant +1 Power to adjacent friendly Nodes this round
    Sanctuary = 7,     // While holding a Seal, cannot be banished by non-Boss cards
    Quickstep = 8,     // On Play: immediately deal 1 Sanity damage to opponent
    Vengeance = 9,     // When banished, deal 2 Sanity damage to opponent
    Covenant = 10,     // +1 Power for each Altar Seal you currently hold
    Eclipse = 11,      // On Play: weaken opposing card on this Node by -2 Power this round
    DrawCard = 12,     // On Play: draw 1 card
    Necromancy = 13,   // +1 Power for every 2 cards in your Graveyard (max +3)
    Breaker = 14,      // On Play: strip enemy Ward on this Pillar & inflict 1 permanent Erosion
    RampSoul = 15,     // On Play: gain +1 Soul Energy this round
    // Crest Spell effects (cast directly from hand — do not occupy a Pillar)
    SpellRevive = 16,     // Revive strongest unit from your Graveyard onto target open Pillar
    SpellWardBuff = 17,   // Grant Ward +2 Power this round to your unit on target Pillar
    SpellBloodDraw = 18,  // Draw 2 cards, lose 2 Sanity
    SpellReverseCCW = 19, // Reverse Judgment Blade sweep Counter-Clockwise (5->1) + draw 1
    SpellGraveBuff = 20,  // All your units gain +1 Power per 2 Graveyard cards (max +3) this round
    SpellPurge = 21,      // Strip Ward & inflict -3 Power this round to enemy on target Pillar
    SpellMistletoe = 22,  // Strip Ward & inflict -2 permanent Erosion to enemy on target Pillar
    SpellRingShift = 23,  // Shift/swap your unit on target Pillar clockwise + grant +1 Power
    SpellSoulSurge = 24,  // Gain +2 Soul Energy this round
    SpellSealLock = 25,   // Claim target Pillar's Seal (if you have a unit there) & heal +3 Sanity
    SpellMaliceNova = 26, // Inflict -2 Power this round to ALL enemies & deal 2 Sanity damage
    SpellAwakerVis = 27   // Draw 2 cards & next card you play this round costs 1 less Soul
};

inline const char *ability_name(AbilityId a) noexcept {
    switch (a) {
    case AbilityId::Resonance:      return "Resonance";
    case AbilityId::Ward:           return "Ward";
    case AbilityId::SoulDrain:      return "Soul Drain";
    case AbilityId::MaliceStrike:   return "Malice Strike";
    case AbilityId::Harmonixer:     return "Harmonixer";
    case AbilityId::ArcaneEcho:     return "Arcane Echo";
    case AbilityId::Sanctuary:      return "Sanctuary";
    case AbilityId::Quickstep:      return "Quickstep";
    case AbilityId::Vengeance:      return "Vengeance";
    case AbilityId::Covenant:       return "Covenant";
    case AbilityId::Eclipse:        return "Eclipse";
    case AbilityId::DrawCard:       return "Arcane Insight";
    case AbilityId::Necromancy:     return "Spirit Call";
    case AbilityId::Breaker:        return "Ward Breaker";
    case AbilityId::RampSoul:       return "Leyline Tap";
    case AbilityId::SpellRevive:    return "Crest: Resurrection";
    case AbilityId::SpellWardBuff:  return "Crest: Sanctuary Ward";
    case AbilityId::SpellBloodDraw: return "Crest: Blood Draw";
    case AbilityId::SpellReverseCCW:return "Crest: Ring Reversal";
    case AbilityId::SpellGraveBuff: return "Crest: Graveyard Call";
    case AbilityId::SpellPurge:     return "Crest: Exorcist Purge";
    case AbilityId::SpellMistletoe: return "Crest: Mistletoe";
    case AbilityId::SpellRingShift: return "Crest: Ring Shift";
    case AbilityId::SpellSoulSurge: return "Crest: Soul Surge";
    case AbilityId::SpellSealLock:  return "Crest: Gate Seal";
    case AbilityId::SpellMaliceNova:return "Crest: Malice Nova";
    case AbilityId::SpellAwakerVis: return "Crest: Awaker Vision";
    default:                        return "None";
    }
}

inline const char *ability_description(AbilityId a) noexcept {
    switch (a) {
    case AbilityId::Resonance:      return "+3 Power (w/ Harmony) if adjacent to a friendly card of the same Element.";
    case AbilityId::Ward:           return "Armored: survives 1 lost Altar clash without being banished.";
    case AbilityId::SoulDrain:      return "When winning an Altar clash, restore +2 Sanity.";
    case AbilityId::MaliceStrike:   return "When winning an Altar clash, deal +2 bonus Sanity damage.";
    case AbilityId::Harmonixer:     return "Fusing over an ally grants +2 extra Power instead of +1.";
    case AbilityId::ArcaneEcho:     return "On Play: adjacent friendly Altar Pillars gain +1 Power this round.";
    case AbilityId::Sanctuary:      return "While holding this Pillar's Seal, non-Boss cards cannot banish it.";
    case AbilityId::Quickstep:      return "On Play: deal 1 direct Sanity damage to the opponent.";
    case AbilityId::Vengeance:      return "When banished from an Altar, deal 2 Sanity damage to the opponent.";
    case AbilityId::Covenant:       return "Gains +1 Power for every Altar Seal you currently control.";
    case AbilityId::Eclipse:        return "On Play: opposing card on this Pillar loses -2 Power this round.";
    case AbilityId::DrawCard:       return "On Play: draw 1 card from your deck.";
    case AbilityId::Necromancy:     return "Gains +1 Power for every 2 cards in your Graveyard (max +3).";
    case AbilityId::Breaker:        return "On Play: strip enemy Ward on this Pillar & inflict -1 permanent Erosion.";
    case AbilityId::RampSoul:       return "On Play: gain +1 Soul Energy this round.";
    case AbilityId::SpellRevive:    return "SPELL: Revive the strongest unit from your Graveyard onto an empty Pillar.";
    case AbilityId::SpellWardBuff:  return "SPELL: Grant Ward and +2 Power this round to your unit on target Pillar.";
    case AbilityId::SpellBloodDraw: return "SPELL: Draw 2 cards immediately and lose 2 Sanity.";
    case AbilityId::SpellReverseCCW:return "SPELL: Reverse the Judgment Sweep Counter-Clockwise (5->1) & draw 1 card.";
    case AbilityId::SpellGraveBuff: return "SPELL: All friendly units gain +1 Power per 2 Graveyard cards (max +3) this round.";
    case AbilityId::SpellPurge:     return "SPELL: Strip enemy Ward and inflict -3 Power this round on target Pillar.";
    case AbilityId::SpellMistletoe: return "SPELL: Strip enemy Ward and inflict -2 permanent Erosion on target Pillar.";
    case AbilityId::SpellRingShift: return "SPELL: Move/swap your unit on target Pillar clockwise & grant +1 Power.";
    case AbilityId::SpellSoulSurge: return "SPELL: Immediately gain +2 Soul Energy this round.";
    case AbilityId::SpellSealLock:  return "SPELL: Claim target Pillar's Seal (requires your unit) & restore +3 Sanity.";
    case AbilityId::SpellMaliceNova:return "SPELL: Inflict -2 Power this round to ALL enemy units & deal 2 Sanity damage.";
    case AbilityId::SpellAwakerVis: return "SPELL: Draw 2 cards & your next card this round costs 1 less Soul Energy.";
    default:                        return "Reliable Altar vanguard with no special trigger.";
    }
}

inline bool ability_needs_target_pillar(AbilityId a) noexcept {
    return a == AbilityId::SpellRevive || a == AbilityId::SpellWardBuff ||
           a == AbilityId::SpellPurge || a == AbilityId::SpellMistletoe ||
           a == AbilityId::SpellRingShift || a == AbilityId::SpellSealLock;
}

struct CardDef {
    std::string id;
    std::string name;
    std::string subtitle;
    std::string arcana;
    SeriesGame game = SeriesGame::Koudelka;
    CardCategory category = CardCategory::MainCharacter;
    Rarity rarity = Rarity::Common;  // Fixed rarity determined by creature/boss difficulty
    bool is_boss = false;
    std::string art_rel_path;        // Placeholder slot until PNG artwork is placed here
    std::string lore;
    std::string obtain_hint;         // Shown in empty binder pockets
    float anim_interval_sec = 8.0f;  // 6.0..10.0s flourish cycle for Animated finish
    std::uint32_t aura_rgb = 0xD8A850u;
    // Judgment Ring Duel stats
    int cost = 1;                    // 1..6 Soul Energy
    int power = 2;                   // 0 for Crest Spells, 1..10 base Power for Units
    Element element = Element::Dark;
    AbilityId ability = AbilityId::None;

    bool is_spell() const noexcept { return category == CardCategory::CrestSpell; }
};

struct HiddenPickupDef {
    std::string id;
    SeriesGame game = SeriesGame::Koudelka;
    std::uint32_t room_id = 0;
    std::string area_name;
    std::string hint;
    std::string fixed_card_id;  // Empty = roll from game's pool
    int luck_tier = 1;          // 1 = boosted finish odds, 2 = high-luck finish odds
};

}  // namespace cards

#include "card_catalog_data.hpp"

namespace cards {

inline const std::vector<CardDef> &catalog() {
    static const std::vector<CardDef> kCards = [] {
        std::vector<CardDef> out;
        out.reserve(sizeof(kCanonicalCatalogRows) / sizeof(kCanonicalCatalogRows[0]));
        for (const auto &r : kCanonicalCatalogRows) {
            CardDef c;
            c.id = r.id;
            c.name = r.name;
            c.subtitle = r.subtitle;
            c.arcana = r.arcana;
            c.game = r.game;
            c.category = r.category;
            c.rarity = r.rarity;
            c.is_boss = r.is_boss;
            c.art_rel_path = r.art_rel_path;
            c.lore = r.lore;
            c.obtain_hint = r.obtain_hint;
            c.anim_interval_sec = 8.0f;
            c.aura_rgb = r.aura_rgb;
            c.cost = r.cost;
            c.power = r.power;
            c.element = r.element;
            c.ability = r.ability;
            out.push_back(std::move(c));
        }
        return out;
    }();
    return kCards;
}

inline const CardDef *find_card_def(const std::string &id) {
    static const std::unordered_map<std::string, std::size_t> kIndex = [] {
        std::unordered_map<std::string, std::size_t> m;
        const auto &cat = catalog();
        m.reserve(cat.size() * 2u);
        for (std::size_t i = 0; i < cat.size(); ++i) m.emplace(cat[i].id, i);
        return m;
    }();
    const auto it = kIndex.find(id);
    if (it == kIndex.end()) return nullptr;
    return &catalog()[it->second];
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
    // 3 cosmetic finishes for this card's fixed rarity:
    //   0 = Standard, 1 = Foil, 2 = Animated
    std::array<int, kFinishCount> counts{};
    std::string first_acquired;
    std::string last_source;

    int total_copies() const noexcept {
        return counts[0] + counts[1] + counts[2];
    }

    bool unlocked() const noexcept {
        return total_copies() > 0;
    }

    CardFinish highest_finish() const noexcept {
        if (counts[2] > 0) return CardFinish::Animated;
        if (counts[1] > 0) return CardFinish::Foil;
        return CardFinish::Standard;
    }
    CardFinish best_finish() const noexcept { return highest_finish(); }

    int highest_finish_index() const noexcept {
        return static_cast<int>(highest_finish());
    }

    bool has_foil() const noexcept {
        return counts[1] > 0 || counts[2] > 0;
    }

    bool has_animated() const noexcept {
        return counts[2] > 0;
    }
};

struct DropRecord {
    std::string card_id;
    Rarity rarity = Rarity::Common;
    CardFinish finish = CardFinish::Standard;
    bool foil = false;  // true if Foil or Animated (compat with existing toasts)
    bool is_new_card = false;
    bool is_new_finish = false;
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

inline const char *duelist_rank_name(int wins) noexcept {
    if (wins >= 25) return "Grand Harmonixer";
    if (wins >= 15) return "Obsidian Sovereign";
    if (wins >= 9)  return "Gold Inquisitor";
    if (wins >= 5)  return "Silver Medium";
    if (wins >= 2)  return "Bronze Arcanist";
    return "Initiate";
}

inline std::uint32_t duelist_rank_rgb(int wins) noexcept {
    if (wins >= 25) return 0xFF88D0u;
    if (wins >= 15) return 0xC874F8u;
    if (wins >= 9)  return 0xF5C24Cu;
    if (wins >= 5)  return 0x78C8F8u;
    if (wins >= 2)  return 0xD89860u;
    return 0xA8B0BCu;
}

class Collection {
public:
    int essence = 0;
    bool duel_rules_seen = false;
    int duel_wins = 0;
    int duel_losses = 0;
    int duel_win_streak = 0;
    int duel_best_streak = 0;
    std::unordered_map<std::string, OwnedCard> owned;
    std::unordered_set<std::string> collected_pickups;
    std::vector<DropRecord> history;    // newest last, capped at 40
    std::vector<std::string> active_deck;  // Saved 20-card Judgment Ring Duel deck
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
        for (const auto &kv : owned) n += kv.second.counts[1] + kv.second.counts[2];
        return n;
    }

    int total_animated_owned() const noexcept {
        int n = 0;
        for (const auto &kv : owned) n += kv.second.counts[2];
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
        active_deck.clear();

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

        const int version = detail::extract_json_int(text, "version", 1);
        essence = std::max(0, detail::extract_json_int(text, "essence", 0));
        duel_rules_seen = detail::extract_json_int(text, "duel_rules_seen", 0) != 0;
        duel_wins = std::max(0, detail::extract_json_int(text, "duel_wins", 0));
        duel_losses = std::max(0, detail::extract_json_int(text, "duel_losses", 0));
        duel_win_streak = std::max(0, detail::extract_json_int(text, "duel_win_streak", 0));
        duel_best_streak = std::max(0, detail::extract_json_int(text, "duel_best_streak", 0));

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

        auto parse_string_array = [&](const char *arr_key, auto &&fn) {
            const std::string needle = std::string("\"") + arr_key + "\"";
            const std::size_t pk = text.find(needle);
            if (pk == std::string::npos) return;
            const std::size_t lb = text.find('[', pk + needle.size());
            const std::size_t rb = (lb != std::string::npos) ? text.find(']', lb) : std::string::npos;
            if (lb == std::string::npos || rb == std::string::npos) return;
            std::size_t p = lb + 1;
            while (p < rb) {
                const std::size_t q1 = text.find('"', p);
                if (q1 == std::string::npos || q1 >= rb) break;
                const std::size_t q2 = text.find('"', q1 + 1);
                if (q2 == std::string::npos || q2 > rb) break;
                fn(text.substr(q1 + 1, q2 - q1 - 1));
                p = q2 + 1;
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
                    std::vector<int> raw;
                    std::size_t p = lb + 1;
                    while (p < rb) {
                        while (p < rb && (obj[p] == ' ' || obj[p] == ',' || obj[p] == '\t')) ++p;
                        if (p >= rb) break;
                        char *endp = nullptr;
                        const long val = std::strtol(obj.c_str() + p, &endp, 10);
                        raw.push_back(std::max(0, static_cast<int>(val)));
                        if (!endp || endp == obj.c_str() + p) break;
                        p = static_cast<std::size_t>(endp - obj.c_str());
                    }
                    if (version >= 2 || raw.size() <= 3u) {
                        for (std::size_t i = 0; i < std::min<std::size_t>(raw.size(), 3u); ++i) {
                            oc.counts[i] = raw[i];
                        }
                    } else {
                        // Lossless migration from legacy 11-variant array:
                        // even 0..8 -> Standard, odd 1..9 -> Foil, 10 -> Animated
                        for (std::size_t i = 0; i < raw.size(); ++i) {
                            if (i >= 10u) oc.counts[2] += raw[i];
                            else if ((i & 1u) != 0u) oc.counts[1] += raw[i];
                            else oc.counts[0] += raw[i];
                        }
                    }
                }
            }
            owned[cid] = std::move(oc);
        });

        parse_string_array("pickups", [&](const std::string &pid) {
            collected_pickups.insert(pid);
        });
        parse_string_array("active_deck", [&](const std::string &cid) {
            if (find_card_def(cid) != nullptr) active_deck.push_back(cid);
        });

        parse_array_objects("history", [&](const std::string &obj) {
            DropRecord r;
            r.card_id = detail::extract_json_string(obj, "card_id");
            const CardDef *def = find_card_def(r.card_id);
            r.rarity = def ? def->rarity
                           : static_cast<Rarity>(std::clamp(detail::extract_json_int(obj, "rarity", 0), 0, 4));
            const int fin = detail::extract_json_int(obj, "finish", -1);
            if (fin >= 0 && fin < kFinishCount) {
                r.finish = static_cast<CardFinish>(fin);
            } else {
                r.finish = (detail::extract_json_int(obj, "foil", 0) != 0)
                    ? CardFinish::Foil : CardFinish::Standard;
            }
            r.foil = finish_is_foil(r.finish);
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
        out.reserve(6144);
        out += "{\n";
        out += "  \"version\": 2,\n";
        out += "  \"essence\": " + std::to_string(essence) + ",\n";
        out += "  \"duel_rules_seen\": " + std::to_string(duel_rules_seen ? 1 : 0) + ",\n";
        out += "  \"duel_wins\": " + std::to_string(duel_wins) + ",\n";
        out += "  \"duel_losses\": " + std::to_string(duel_losses) + ",\n";
        out += "  \"duel_win_streak\": " + std::to_string(duel_win_streak) + ",\n";
        out += "  \"duel_best_streak\": " + std::to_string(duel_best_streak) + ",\n";

        out += "  \"owned\": [\n";
        bool first = true;
        for (const auto &c : catalog()) {
            auto it = owned.find(c.id);
            if (it == owned.end() || !it->second.unlocked()) continue;
            if (!first) out += ",\n";
            first = false;
            const OwnedCard &oc = it->second;
            out += "    {\"id\": \"" + detail::json_escape(c.id) + "\", \"counts\": [" +
                   std::to_string(oc.counts[0]) + "," +
                   std::to_string(oc.counts[1]) + "," +
                   std::to_string(oc.counts[2]) +
                   "], \"first_acquired\": \"" + detail::json_escape(oc.first_acquired) +
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

        out += "  \"active_deck\": [";
        for (std::size_t i = 0; i < active_deck.size(); ++i) {
            if (i > 0) out += ", ";
            out += "\"" + detail::json_escape(active_deck[i]) + "\"";
        }
        out += "],\n";

        out += "  \"history\": [\n";
        for (std::size_t i = 0; i < history.size(); ++i) {
            const auto &h = history[i];
            if (i > 0) out += ",\n";
            out += "    {\"card_id\": \"" + detail::json_escape(h.card_id) +
                   "\", \"rarity\": " + std::to_string(static_cast<int>(h.rarity)) +
                   ", \"finish\": " + std::to_string(static_cast<int>(h.finish)) +
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

    // Grant a specific card using its FIXED rarity + chosen cosmetic Finish.
    // Strictly 1 unique copy per card (no duplicates). If already owned, upgrades finish if higher.
    DropRecord grant_card(const std::string &card_id, CardFinish finish, const std::string &source) {
        const CardDef *def = find_card_def(card_id);
        const Rarity fixed_rar = def ? def->rarity : Rarity::Common;
        const int fidx = std::clamp(static_cast<int>(finish), 0, kFinishCount - 1);
        OwnedCard &oc = owned[card_id];
        const bool new_card = !oc.unlocked();
        const int prev_best = oc.unlocked() ? static_cast<int>(oc.best_finish()) : -1;
        const bool new_fin = (fidx > prev_best);
        if (new_card || new_fin) {
            oc.counts = {0, 0, 0};
            oc.counts[static_cast<std::size_t>(fidx)] = 1;
        } else {
            // Ensure strictly 1 copy total
            const int keep = std::clamp(prev_best, 0, kFinishCount - 1);
            oc.counts = {0, 0, 0};
            oc.counts[static_cast<std::size_t>(keep)] = 1;
        }
        const std::string ts = detail::now_timestamp();
        if (oc.first_acquired.empty()) oc.first_acquired = ts;
        oc.last_source = source;

        DropRecord rec;
        rec.card_id = card_id;
        rec.rarity = fixed_rar;
        rec.finish = finish;
        rec.foil = finish_is_foil(finish);
        rec.is_new_card = new_card;
        rec.is_new_finish = new_fin;
        rec.source = source;
        rec.timestamp = ts;

        history.push_back(rec);
        if (history.size() > 40u) {
            history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - 40u));
        }
        save();
        return rec;
    }

    // Roll a card drop respecting each card's FIXED rarity tier:
    //   luck_tier == 0 (Normal Battle): Common / Uncommon creatures
    //   luck_tier == 1 (Trophy / Relic / Essence Draw): Uncommon / Rare / Exotic pool
    //   luck_tier >= 2 (Boss Victory): Rare / Exotic / Legendary boss & main pool
    // Only the cosmetic Finish (Standard / Foil / Animated) is rolled!
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
            const Rarity min_rar = (luck_tier >= 2) ? Rarity::Rare
                                 : (luck_tier == 1 ? Rarity::Uncommon : Rarity::Common);
            const Rarity max_rar = (luck_tier >= 2) ? Rarity::Legendary
                                 : (luck_tier == 1 ? Rarity::Exotic : Rarity::Uncommon);

            for (const auto &c : catalog()) {
                if (c.game != game) continue;
                if (category_filter >= 0 && static_cast<int>(c.category) != category_filter) continue;
                if (boss_bias && !c.is_boss && c.category != CardCategory::MainCharacter) continue;
                if (c.rarity < min_rar || c.rarity > max_rar) continue;
                pool.push_back(&c);
            }
            if (pool.empty()) {
                for (const auto &c : catalog()) {
                    if (c.game != game) continue;
                    if (category_filter >= 0 && static_cast<int>(c.category) != category_filter) continue;
                    pool.push_back(&c);
                }
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

        // Roll cosmetic finish ONLY (card rarity is strictly fixed!)
        std::uniform_real_distribution<double> pct(0.0, 100.0);
        const double r = pct(rng);
        CardFinish fin = CardFinish::Standard;
        if (luck_tier >= 2) {
            // Boss Victory: 8% Animated, 24% Foil, 68% Standard
            if (r < 8.0) fin = CardFinish::Animated;
            else if (r < 32.0) fin = CardFinish::Foil;
        } else if (luck_tier == 1) {
            // Trophy / Hidden Relic / Essence Draw: 4% Animated, 18% Foil, 78% Standard
            if (r < 4.0) fin = CardFinish::Animated;
            else if (r < 22.0) fin = CardFinish::Foil;
        } else {
            // Standard Battle Drop: 2% Animated, 10% Foil, 88% Standard
            if (r < 2.0) fin = CardFinish::Animated;
            else if (r < 12.0) fin = CardFinish::Foil;
        }
        return grant_card(chosen_id, fin, source);
    }

    // Remove 1 copy of a card (used for True Triple Triad Card Wager losses — prefers Standard, then Foil, then Animated)
    bool remove_card_copy(const std::string &card_id) {
        auto it = owned.find(card_id);
        if (it == owned.end() || !it->second.unlocked()) return false;
        for (int f = 0; f < kFinishCount; ++f) {
            if (it->second.counts[static_cast<std::size_t>(f)] > 0) {
                --it->second.counts[static_cast<std::size_t>(f)];
                save();
                return true;
            }
        }
        return false;
    }

    // Dismantling is disabled: every card is a unique 1-of-1 collectible (no duplicates)
    bool dismantle_duplicate(const std::string &, CardFinish, int &gained_out) {
        gained_out = 0;
        return false;
    }

    // Craft a specific cosmetic finish of a card using Essence
    bool craft_finish(const std::string &card_id, CardFinish fin) {
        const CardDef *def = find_card_def(card_id);
        if (!def) return false;
        const int cost = craft_essence_cost(def->rarity, fin);
        if (essence < cost) return false;
        essence -= cost;
        grant_card(card_id, fin, "Crafted with Essence");
        return true;
    }
};

}  // namespace cards
