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
    Resonance = 1,     // +2 Power if an adjacent Altar Node has a friendly card of the same Element
    Ward = 2,          // Survives 1 lost Altar clash without being banished
    SoulDrain = 3,     // On Altar win, restore +2 Sanity
    MaliceStrike = 4,  // On Altar win, deal +2 bonus Sanity damage
    Harmonixer = 5,    // When Fused over an ally, gain +2 extra Power (instead of +1)
    ArcaneEcho = 6,    // On Play: grant +1 Power to adjacent friendly Nodes this round
    Sanctuary = 7,     // While holding a Seal, cannot be banished by non-Boss cards
    Quickstep = 8,     // On Play: immediately deal 1 Sanity damage to opponent
    Vengeance = 9,     // When banished, deal 2 Sanity damage to opponent
    Covenant = 10,     // +1 Power for each Altar Seal you currently hold
    Eclipse = 11       // On Play: weaken opposing card on this Node by -2 Power this round
};

inline const char *ability_name(AbilityId a) noexcept {
    switch (a) {
    case AbilityId::Resonance:    return "Resonance";
    case AbilityId::Ward:         return "Ward";
    case AbilityId::SoulDrain:    return "Soul Drain";
    case AbilityId::MaliceStrike: return "Malice Strike";
    case AbilityId::Harmonixer:   return "Harmonixer";
    case AbilityId::ArcaneEcho:   return "Arcane Echo";
    case AbilityId::Sanctuary:    return "Sanctuary";
    case AbilityId::Quickstep:    return "Quickstep";
    case AbilityId::Vengeance:    return "Vengeance";
    case AbilityId::Covenant:     return "Covenant";
    case AbilityId::Eclipse:      return "Eclipse";
    default:                      return "None";
    }
}

inline const char *ability_description(AbilityId a) noexcept {
    switch (a) {
    case AbilityId::Resonance:    return "+2 Power if adjacent to a friendly card of the same Element.";
    case AbilityId::Ward:         return "Armored: survives 1 lost Altar clash without being banished.";
    case AbilityId::SoulDrain:    return "When winning an Altar clash, restore +2 Sanity.";
    case AbilityId::MaliceStrike: return "When winning an Altar clash, deal +2 bonus Sanity damage.";
    case AbilityId::Harmonixer:   return "Fusing over an ally grants +2 extra Power instead of +1.";
    case AbilityId::ArcaneEcho:   return "On Play: adjacent friendly Altar Nodes gain +1 Power this round.";
    case AbilityId::Sanctuary:    return "While holding this Node's Seal, non-Boss cards cannot banish it.";
    case AbilityId::Quickstep:    return "On Play: deal 1 direct Sanity damage to the opponent.";
    case AbilityId::Vengeance:    return "When banished from an Altar, deal 2 Sanity damage to the opponent.";
    case AbilityId::Covenant:     return "Gains +1 Power for every Altar Seal you currently control.";
    case AbilityId::Eclipse:      return "On Play: opposing card on this Node loses -2 Power this round.";
    default:                      return "Reliable Altar vanguard with no special trigger.";
    }
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
    int power = 2;                   // 1..10 base Power
    Element element = Element::Dark;
    AbilityId ability = AbilityId::None;
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

inline const std::vector<CardDef> &catalog() {
    static const std::vector<CardDef> kCards = {
        // =========================================================================
        //  KOUDELKA (1898) — 24 Cards
        // =========================================================================
        // --- Main Characters (3) ---
        {"kdl_koudelka", "Koudelka Iasant", "The Clairvoyant Medium", "II  •  THE HIGH PRIESTESS",
         SeriesGame::Koudelka, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/koudelka/koudelka_art.png",
         "A sharp-tongued young Romani medium drawn to Nemeton Monastery on Halloween night, 1898, by a restless spirit's plea.",
         "Find the Nemeton Entrance Relic or unlock Koudelka Trophies.",
         7.0f, 0xD94B7Au, 3, 5, Element::Dark, AbilityId::SoulDrain},
        {"kdl_edward", "Edward Plunkett", "The Reluctant Adventurer", "VII  •  THE CHARIOT",
         SeriesGame::Koudelka, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/koudelka/edward_art.png",
         "A brash Londoner chasing treasure inside Nemeton Monastery, fighting alongside a psychic and a bishop to survive the night.",
         "Find Edward's Encounter Relic or unlock Koudelka Trophies.",
         8.5f, 0xE8A844u, 3, 5, Element::Fire, AbilityId::Quickstep},
        {"kdl_james", "James O'Flaherty", "Bishop of the Vatican", "V  •  THE HIEROPHANT",
         SeriesGame::Koudelka, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/koudelka/james_art.png",
         "An austere Vatican emissary dispatched to recover a forbidden grimoire stolen from Rome, driven by devotion to Elaine.",
         "Find the Abbey Sanctuary Relic or unlock Koudelka Trophies.",
         10.0f, 0x66A8F0u, 4, 6, Element::Light, AbilityId::Sanctuary},

        // --- NPCs (7) ---
        {"kdl_alias", "Alias", "The Wandering Peddler", "0  •  THE FOOL",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Common, false,
         "assets/cards/koudelka/alias_art.png",
         "A bizarre figure roaming the haunted monastery with a sack of curiosities, offering strange bargains in the dark.",
         "Trade with Alias in Nemeton Monastery or earn early drops.",
         6.0f, 0xD8B44Cu, 1, 2, Element::Earth, AbilityId::Quickstep},
        {"kdl_ogden", "Ogden Hartman", "Caretaker of the Abbey", "IX  •  THE HERMIT",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Common, false,
         "assets/cards/koudelka/ogden_art.png",
         "The weary groundskeeper of Nemeton Monastery, guarding far more of the abbey's blood-stained past than he admits.",
         "Find the Caretakers' Quarters Relic in Disc 1.",
         8.0f, 0xC89858u, 1, 3, Element::Earth, AbilityId::None},
        {"kdl_bessy", "Bessy Hartman", "Mistress of the Manor", "VI  •  THE LOVERS",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Common, false,
         "assets/cards/koudelka/bessy_art.png",
         "Ogden's devoted wife, tending to the decrepit abbey long after sanity fled its candlelit halls.",
         "Find the Monastery Dining Hall Relic in Disc 1.",
         8.0f, 0xD47A92u, 2, 3, Element::Water, AbilityId::ArcaneEcho},
        {"kdl_charlotte", "Charlotte D'Lota", "The Weeping Maiden", "XII  •  THE HANGED MAN",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/koudelka/charlotte_art.png",
         "A forsaken noblewoman left to perish in the monastery prison, still longing for a promised ring.",
         "Encounter Charlotte in the Monastery Prison (Disc 2).",
         9.5f, 0xB86CE8u, 2, 4, Element::Water, AbilityId::Vengeance},
        {"kdl_patrick", "Patrick Heyworth", "Master of Nemeton", "IV  •  THE EMPEROR",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/koudelka/patrick_art.png",
         "The grieving aristocrat who turned Nemeton Monastery into a forbidden laboratory of resurrection.",
         "Reach Disc 3 and confront Patrick in the Abbey.",
         9.0f, 0x9C58D8u, 3, 4, Element::Dark, AbilityId::Eclipse},
        {"kdl_elaine", "Elaine Heyworth", "The Calling Spirit", "III  •  THE EMPRESS",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Rare, false,
         "assets/cards/koudelka/elaine_art.png",
         "A gentle soul whose tragic death triggered the nightmare of Nemeton, summoning Koudelka across the sea.",
         "Uncover Elaine's truth in Disc 3 or earn Story Trophies.",
         7.5f, 0x88D4F8u, 3, 5, Element::Light, AbilityId::ArcaneEcho},
        {"kdl_roger", "Roger Bacon", "The Immortal Alchemist", "I  •  THE MAGICIAN",
         SeriesGame::Koudelka, CardCategory::Npc, Rarity::Exotic, false,
         "assets/cards/koudelka/roger_art.png",
         "The centuries-old sage who crafted the Emigre Manuscript, watching the fate of Nemeton and the Shadow Hearts era to come.",
         "Find the Alchemist's Study Relic in Disc 3.",
         6.5f, 0x52D0A4u, 4, 6, Element::Wind, AbilityId::Covenant},

        // --- Regular Beasts (8) ---
        {"kdl_gremlin", "Gremlin", "Abbey Lurker", "XV  •  THE IMP",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/koudelka/gremlin_art.png",
         "A spiteful lesser fiend skittering across the damp flagstones of Nemeton's outer corridors.",
         "Defeat Gremlins in early Nemeton Monastery battles.",
         6.5f, 0x88A078u, 1, 2, Element::Wind, AbilityId::Resonance},
        {"kdl_sorrow", "Sorrow", "Wailing Spirit of Nemeton", "XIII  •  DEATH",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/koudelka/sorrow_art.png",
         "A gaunt apparition born from decades of torment inside the abbey, draining warmth with spectral claws.",
         "Defeat Sorrow spirits in Disc 1 battles.",
         7.0f, 0x7A88C8u, 1, 2, Element::Dark, AbilityId::SoulDrain},
        {"kdl_mimic", "Mimic", "Cursed Reliquary", "XV  •  THE DEVIL",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/koudelka/mimic_art.png",
         "A grotesque trap animated by Nemeton's malice, disguising itself as antique furnishings to ambush treasure hunters.",
         "Defeat Mimics in monastery storerooms.",
         8.0f, 0xC86442u, 2, 3, Element::Earth, AbilityId::Vengeance},
        {"kdl_bone_knight", "Bone Knight", "Risen Abbey Sentinel", "XI  •  JUSTICE",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/koudelka/bone_knight_art.png",
         "Skeletal remains of forgotten soldiers clad in rusted plate, marching endlessly through the courtyard.",
         "Defeat Bone Knights in the Nemeton Courtyard.",
         8.5f, 0xB8C0CCu, 2, 4, Element::Earth, AbilityId::None},
        {"kdl_banshee", "Banshee", "Herald of the Grave", "XVIII  •  THE MOON",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/koudelka/banshee_art.png",
         "A shrieking phantom drifting through moonlit cloisters whose wail foretells imminent doom.",
         "Defeat Banshees in Disc 2 cloisters.",
         6.5f, 0x9C72F0u, 2, 4, Element::Wind, AbilityId::Eclipse},
        {"kdl_cursed_armor", "Cursed Armor", "Hollow Iron Husk", "VIII  •  STRENGTH",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/koudelka/cursed_armor_art.png",
         "Empty Victorian ceremonial armor possessed by vengeful spirits, shaking floorboards as it advances.",
         "Defeat Cursed Armor in Disc 2 halls.",
         9.0f, 0xD45252u, 3, 4, Element::Fire, AbilityId::Ward},
        {"kdl_nightgaunt", "Nightgaunt", "Shadow of the Spire", "XVIII  •  TWILIGHT",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/koudelka/nightgaunt_art.png",
         "A leathery, faceless terror haunting the upper belltower passages of Nemeton.",
         "Defeat Nightgaunts in Disc 3 upper towers.",
         7.5f, 0x7E64B8u, 3, 5, Element::Dark, AbilityId::Resonance},
        {"kdl_chimera", "Chimera", "Fleshwork Abomination", "XVI  •  CHAOS",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Rare, false,
         "assets/cards/koudelka/chimera_art.png",
         "An unnatural amalgam stitched together by Nemeton's dark alchemy, prowling the deepest crypts.",
         "Defeat Chimeras in late Disc 3/4 crypts.",
         8.5f, 0xD8744Cu, 4, 6, Element::Fire, AbilityId::MaliceStrike},

        // --- Bosses (6) ---
        {"kdl_gargoyle", "Gargoyle", "Stone Guardian of the Belltower", "XVI  •  THE TOWER",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Uncommon, true,
         "assets/cards/koudelka/gargoyle_art.png",
         "A monstrous winged sentinel torn free from the cathedral spire by dark magic.",
         "Defeat the Gargoyle boss in Disc 1.",
         7.5f, 0xE07A48u, 3, 5, Element::Wind, AbilityId::Ward},
        {"kdl_plant_boss", "Rafflesia", "Blight of the Conservatory", "X  •  WHEEL OF FORTUNE",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/koudelka/rafflesia_art.png",
         "A towering carnivorous horror blooming in the overgrown monastery greenhouse, choking the air with spores.",
         "Defeat Rafflesia in the Monastery Conservatory.",
         8.5f, 0x4CC878u, 4, 6, Element::Earth, AbilityId::Ward},
        {"kdl_charlotte_boss", "Charlotte (Wraith)", "Specter of Broken Vows", "XIV  •  TEMPERANCE",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/koudelka/charlotte_boss_art.png",
         "Consumed by heartbreak and fury, Charlotte's spirit unleashes relentless arcane storms upon the living.",
         "Defeat Charlotte's Wraith in Disc 2.",
         7.0f, 0xE04C9Cu, 4, 6, Element::Water, AbilityId::MaliceStrike},
        {"kdl_patrick_boss", "Patrick (Possessed)", "Vessel of Obsession", "IV  •  FALL OF REASON",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/koudelka/patrick_boss_art.png",
         "Driven past the brink of madness by the Emigre Manuscript, Patrick channels Nemeton's full wrath.",
         "Defeat Patrick in Disc 3's climax.",
         8.5f, 0xB84CE0u, 5, 7, Element::Dark, AbilityId::Eclipse},
        {"kdl_elaine_aberration", "Elaine (Aberration)", "The Failed Resurrection", "XX  •  JUDGEMENT",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/koudelka/elaine_aberration_art.png",
         "The horrifying consequence of Patrick's forbidden Emigre ritual—a twisted vessel crying out beneath the altar.",
         "Defeat Elaine (Aberration) in Disc 4.",
         9.5f, 0xF05858u, 5, 8, Element::Dark, AbilityId::Ward},
        {"kdl_elaine_final", "Elaine (Final Form)", "Seraph of Nemeton's Ruin", "XXI  •  THE WORLD",
         SeriesGame::Koudelka, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/koudelka/elaine_final_art.png",
         "The cataclysmic apotheosis of the Emigre Manuscript's curse, embodying both the tragedy of 1898 and the dawn of Shadow Hearts.",
         "Defeat Elaine's Final Form to complete Koudelka.",
         10.0f, 0xFFD060u, 6, 9, Element::Light, AbilityId::Covenant},

        // =========================================================================
        //  SHADOW HEARTS I (1913) — 24 Cards
        // =========================================================================
        // --- Main Characters (6) ---
        {"sh1_yuri", "Yuri Hyuga", "The Harmonixer", "VII  •  THE CHARIOT",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/yuri_art.png",
         "A brash, foul-mouthed drifter who carries the forbidden power to fuse with the souls of defeated monsters.",
         "Begin Yuri's journey aboard the Trans-Siberian Express.",
         7.5f, 0xD84858u, 3, 5, Element::Dark, AbilityId::Harmonixer},
        {"sh1_alice", "Alice Elliot", "Priestess of Light", "II  •  THE HIGH PRIESTESS",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/alice_art.png",
         "A compassionate exorcist whose gentle soul and healing miracles anchor Yuri against the encroaching Malice.",
         "Recruit Alice Elliot in Shadow Hearts I.",
         8.0f, 0x90D4F8u, 3, 5, Element::Light, AbilityId::SoulDrain},
        {"sh1_zhuzhen", "Zhuzhen Liu", "Master of Yin & Yang", "IX  •  THE HERMIT",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/zhuzhen_art.png",
         "A seasoned Taoist feng shui master whose dry wit masks decades of hunting abominations across Asia.",
         "Recruit Zhuzhen in Zhaoyang Village.",
         8.5f, 0xE89448u, 3, 5, Element::Fire, AbilityId::ArcaneEcho},
        {"sh1_margarete", "Margarete Zelle", "The Secret Agent", "XI  •  JUSTICE",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/margarete_art.png",
         "A daring espionage agent armed with firearms, explosives, and sharp instincts in the shadow of war.",
         "Recruit Margarete in Dalian.",
         7.0f, 0xE8C050u, 3, 5, Element::Water, AbilityId::Quickstep},
        {"sh1_keith", "Keith Valentine", "Lord of Blue Castle", "XVIII  •  THE MOON",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/keith_art.png",
         "A centuries-old vampire nobleman who awakens from boredom to join Yuri's crusade across Europe.",
         "Recruit Keith Valentine at Blue Castle.",
         9.0f, 0xA474E8u, 4, 6, Element::Earth, AbilityId::SoulDrain},
        {"sh1_halley", "Halley Brancket", "The London Psychic", "XVII  •  THE STAR",
         SeriesGame::ShadowHearts1, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh1/halley_art.png",
         "Koudelka's spirited son, leading the London orphans with inherited telekinetic power and fierce loyalty.",
         "Recruit Halley Brancket in London.",
         7.5f, 0x68D8A8u, 3, 5, Element::Wind, AbilityId::Resonance},

        // --- NPCs (3) ---
        {"sh1_kawashima", "Yoshiko Kawashima", "Imperial Lieutenant", "IV  •  THE EMPEROR",
         SeriesGame::ShadowHearts1, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/sh1/kawashima_art.png",
         "A stern Imperial officer caught between military duty and the supernatural horrors stalking Shanghai.",
         "Encounter Kawashima in Shanghai.",
         8.0f, 0xC4A058u, 2, 4, Element::Fire, AbilityId::Quickstep},
        {"sh1_roger", "Roger Bacon (1913)", "Keeper of the Emigre", "I  •  THE MAGICIAN",
         SeriesGame::ShadowHearts1, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/sh1/roger_art.png",
         "Still tinkering in Wales, the real Roger Bacon holds the secrets needed to stop Albert Simon.",
         "Visit Roger Bacon's cottage in Wales.",
         7.0f, 0x58D0A0u, 2, 4, Element::Wind, AbilityId::ArcaneEcho},
        {"sh1_fox_face", "Fox Face", "Masked Demon of the Graveyard", "XV  •  THE DEVIL",
         SeriesGame::ShadowHearts1, CardCategory::Npc, Rarity::Exotic, false,
         "assets/cards/sh1/fox_face_art.png",
         "The sinister mask-wearing entity lurking inside Yuri's Graveyard, feeding on the Four Masks' malice.",
         "Confront Fox Face in Yuri's Graveyard.",
         9.0f, 0xE85870u, 5, 7, Element::Dark, AbilityId::MaliceStrike},

        // --- Beasts & Fusions (8) ---
        {"sh1_ghoul", "Ghoul", "Train Car Stalker", "XIII  •  DECAY",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh1/ghoul_art.png",
         "A corpse animated by stray Malice, prowling forgotten Manchurian rail lines and villages.",
         "Defeat Ghouls in early China encounters.",
         7.0f, 0x80947Cu, 1, 2, Element::Dark, AbilityId::None},
        {"sh1_bakeneko", "Bakeneko", "Lantern Trickster", "0  •  MISCHIEF",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh1/bakeneko_art.png",
         "A mischievous feline yokai whose eerie fireballs unsettle travelers at dusk.",
         "Defeat Bakeneko in Zhaoyang or Fengtian.",
         6.5f, 0xE08448u, 1, 2, Element::Fire, AbilityId::Resonance},
        {"sh1_qinggu", "Qinggu", "Temple Sentinel", "VIII  •  VIGOR",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh1/qinggu_art.png",
         "A stone-skinned spirit guarding ruined temples across the Chinese countryside.",
         "Defeat Qinggu in early Asian dungeons.",
         8.0f, 0xB89860u, 2, 3, Element::Earth, AbilityId::Ward},
        {"sh1_malice_puppet", "Malice Puppet", "Strings of Hatred", "XII  •  THE MARIONETTE",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh1/malice_puppet_art.png",
         "A grotesque doll twitching with residual resentment, striking when least expected.",
         "Defeat Malice Puppets in the Doll House.",
         7.5f, 0xB068D8u, 2, 4, Element::Water, AbilityId::Eclipse},
        {"sh1_raging_tiger", "Raging Tiger", "Fire Fusion Soul", "VIII  •  STRENGTH",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh1/raging_tiger_art.png",
         "A blazing Harmonixer spirit that answers Yuri's call with ferocious flame.",
         "Unlock the Tier-2 Fire Fusion in Yuri's Graveyard.",
         7.0f, 0xF06838u, 3, 5, Element::Fire, AbilityId::Harmonixer},
        {"sh1_death_emperor", "Death Emperor", "Dark Fusion Soul", "XIII  •  DEATH",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh1/death_emperor_art.png",
         "A towering spectral warlord forged from Yuri's darkest battle memories.",
         "Unlock the Tier-2 Dark Fusion in Yuri's Graveyard.",
         8.5f, 0x8C58C8u, 3, 5, Element::Dark, AbilityId::Harmonixer},
        {"sh1_czernobog", "Czernobog", "Lord of the Black Sun", "XV  •  THE SHADOW",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Exotic, false,
         "assets/cards/sh1/czernobog_art.png",
         "The supreme Dark Fusion soul—an abyssal deity of devastation mastered only at the edge of madness.",
         "Master the Tier-3 Dark Fusion in Shadow Hearts I.",
         9.0f, 0x9848F0u, 5, 8, Element::Dark, AbilityId::Harmonixer},
        {"sh1_seraphic_radiance", "Seraphic Radiance", "Pinnacle of Harmonixer Light", "XIX  •  THE SUN",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Legendary, false,
         "assets/cards/sh1/seraphic_radiance_art.png",
         "Yuri's ultimate hidden fusion—a blinding six-winged seraph born from pure resolve and triumph over the Graveyard.",
         "Defeat Seraphic Radiance in Yuri's Graveyard (Optional Superboss).",
         10.0f, 0xFFE078u, 6, 9, Element::Light, AbilityId::Harmonixer},

        // --- Bosses (7) ---
        {"sh1_yamaraja", "Yamaraja", "Judge of Zhaoyang", "XVI  •  WRATH",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Uncommon, true,
         "assets/cards/sh1/yamaraja_art.png",
         "A terrifying crimson demon summoned to devour Zhaoyang Village.",
         "Defeat Yamaraja in Zhaoyang Village.",
         7.5f, 0xE05048u, 3, 5, Element::Fire, AbilityId::Ward},
        {"sh1_dehuai", "Dehuai", "Mad Feng Shui Master", "XVI  •  THE TOWER",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/sh1/dehuai_art.png",
         "Zhuzhen's fallen rival who twisted sacred feng shui into a weapon of mass slaughter in Shanghai.",
         "Defeat Dehuai atop the Shanghai tower.",
         8.0f, 0xD864A8u, 4, 6, Element::Dark, AbilityId::Eclipse},
        {"sh1_olga", "Olga", "Bloody Bride of Bistritz", "XIV  •  TEMPERANCE",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/sh1/olga_art.png",
         "A tragic specter bound to the haunted halls of Bistritz, lashing out with freezing malice.",
         "Defeat Olga in Bistritz.",
         8.5f, 0x60A8E8u, 4, 6, Element::Water, AbilityId::SoulDrain},
        {"sh1_li_li", "Li Li", "Cataclysm of the Kowloon", "X  •  WHEEL OF FORTUNE",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/sh1/li_li_art.png",
         "The ancient dragon-spirit awakened beneath Shanghai, threatening to swallow the city whole.",
         "Defeat Li Li at the end of Disc 1 (Asia).",
         9.0f, 0x58D088u, 4, 6, Element::Wind, AbilityId::Ward},
        {"sh1_atman", "Atman", "Warden of the Graveyard", "XX  •  JUDGEMENT",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh1/atman_art.png",
         "The colossal manifestation of Yuri's inner Malice, testing whether his heart can endure Alice's sacrifice.",
         "Defeat Atman inside Yuri's Graveyard.",
         9.5f, 0xC84CE8u, 5, 8, Element::Dark, AbilityId::Vengeance},
        {"sh1_albert_simon", "Albert Simon", "The False Prophet", "IV  •  THE USURPER",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh1/albert_simon_art.png",
         "A brilliant, bitter occultist who stole Roger Bacon's identity to summon a god and purge humanity.",
         "Defeat Albert Simon atop Neameto.",
         9.5f, 0xF0A848u, 5, 8, Element::Light, AbilityId::Covenant},
        {"sh1_meta_god", "Meta-God", "Architect of Extinction", "XXI  •  THE WORLD",
         SeriesGame::ShadowHearts1, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/sh1/meta_god_art.png",
         "The alien celestial god pulled down from the heavens onto Neameto to erase the world in blinding fire.",
         "Defeat Meta-God to complete Shadow Hearts I.",
         10.0f, 0xFFD868u, 6, 10, Element::Light, AbilityId::Ward},

        // =========================================================================
        //  SHADOW HEARTS: COVENANT (1915) — 26 Cards
        // =========================================================================
        // --- Main Characters (8) ---
        {"sh2_yuri", "Yuri Hyuga (Covenant)", "The Cursed Harmonixer", "VII  •  THE CHARIOT",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/yuri_art.png",
         "Scarred by the Mistletoe Curse yet unbroken in spirit, Yuri fights across war-torn Europe and Japan to reclaim his soul.",
         "Begin Shadow Hearts: Covenant in Domremy.",
         7.5f, 0xE85060u, 3, 5, Element::Dark, AbilityId::Harmonixer},
        {"sh2_karin", "Karin Koenig", "Lieutenant of the Kaiser", "XI  •  JUSTICE",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/karin_art.png",
         "A proud, noble German officer wielding swordsmanship and unwavering devotion to Yuri.",
         "Join forces with Karin Koenig in Domremy.",
         8.0f, 0xF07850u, 3, 5, Element::Fire, AbilityId::Resonance},
        {"sh2_gepetto", "Gepetto", "The Master Puppeteer", "IX  •  THE HERMIT",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/gepetto_art.png",
         "Alice's grieving uncle who channels his love into Cornelia, a living marionette of deadly grace.",
         "Recruit Gepetto in Paris.",
         8.5f, 0x70B0F0u, 3, 5, Element::Water, AbilityId::ArcaneEcho},
        {"sh2_blanca", "Blanca", "The White Wolf", "VIII  •  STRENGTH",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/blanca_art.png",
         "A fiercely intelligent white wolf who wins the Wolf Bout crown and guards his pack without hesitation.",
         "Recruit Blanca in the Apoina Tower.",
         7.0f, 0x80E0B0u, 3, 5, Element::Wind, AbilityId::Quickstep},
        {"sh2_joachim", "Joachim Valentine", "Grand Papillon", "XIX  •  THE SUN",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/joachim_art.png",
         "Keith's flamboyant vampire brother—wrestling champion, defender of justice, and wielder of absurdly heavy objects.",
         "Recruit Joachim Valentine in Le Havre.",
         7.5f, 0xF5B848u, 4, 6, Element::Earth, AbilityId::Ward},
        {"sh2_lucia", "Lucia", "Dancer of Fortune", "X  •  WHEEL OF FORTUNE",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/lucia_art.png",
         "A radiant Florentine fortune-teller weaving tarot destiny and aromatherapy oils on the battlefield.",
         "Recruit Lucia in Florence.",
         8.0f, 0xE878C0u, 3, 5, Element::Wind, AbilityId::ArcaneEcho},
        {"sh2_anastasia", "Anastasia Romanov", "The Snap-Shot Princess", "III  •  THE EMPRESS",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/anastasia_art.png",
         "The spirited Russian princess whose enchanted camera captures monster techniques straight onto film.",
         "Recruit Anastasia in St. Petersburg.",
         7.0f, 0x88D0F8u, 3, 5, Element::Water, AbilityId::Covenant},
        {"sh2_kurando", "Kurando Inugami", "Heir of the Dog God", "II  •  THE BLADE",
         SeriesGame::Covenant, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh2/kurando_art.png",
         "Yuri's stoic cousin in Japan, blessed with the Inugami bloodline and blazing demonic transformations.",
         "Recruit Kurando Inugami in Disc 2 (Japan).",
         8.5f, 0xF0C860u, 4, 6, Element::Light, AbilityId::Harmonixer},

        // --- NPCs (3) ---
        {"sh2_lenny", "Lenny Curtis", "The Boisterous Squire", "0  •  THE FOOL",
         SeriesGame::Covenant, CardCategory::Npc, Rarity::Common, false,
         "assets/cards/sh2/lenny_art.png",
         "A bumbling yet loyal associate of Sapiente Gladio whose bluster rarely matches his nerve.",
         "Encounter Lenny Curtis in early Europe chapters.",
         6.5f, 0xB8A078u, 1, 2, Element::Earth, AbilityId::None},
        {"sh2_nicolai", "Nicolai Conrad", "Vatican Inquisitor", "V  •  THE HIEROPHANT",
         SeriesGame::Covenant, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/sh2/nicolai_art.png",
         "The silver-tongued inquisitor who drove the Mistletoe Curse into Yuri's heart on behalf of Sapiente Gladio.",
         "Confront Nicolai in St. Marguerite Island.",
         8.0f, 0xA880E8u, 3, 4, Element::Light, AbilityId::Eclipse},
        {"sh2_jeanne", "Jeanne", "Oracle of the Soul", "XVII  •  THE STAR",
         SeriesGame::Covenant, CardCategory::Npc, Rarity::Exotic, false,
         "assets/cards/sh2/jeanne_art.png",
         "The serene guardian of Yuri's Soul Graveyard who guides Harmonixers through the trials of fusion.",
         "Awaken Jeanne's trials inside Yuri's Graveyard.",
         9.0f, 0x9CE0FFu, 4, 6, Element::Light, AbilityId::Sanctuary},

        // --- Beasts & Fusions (8) ---
        {"sh2_domremy_soldier", "Possessed Soldier", "Pawn of the Front", "XVI  •  STRIFE",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh2/domremy_soldier_art.png",
         "A trench soldier warped by Sapiente Gladio's occult experiments in northern France.",
         "Defeat soldiers in Domremy Forest.",
         7.0f, 0x98A090u, 1, 2, Element::Fire, AbilityId::None},
        {"sh2_gargoyle_hound", "Gargoyle Hound", "Cathedral Stalker", "XVIII  •  THE HOUND",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh2/gargoyle_hound_art.png",
         "A jagged stone beast animated by dark crests to hunt intruders through ruined abbeys.",
         "Defeat Gargoyle Hounds in Apoina Tower.",
         7.5f, 0xA09080u, 1, 3, Element::Earth, AbilityId::Resonance},
        {"sh2_manticore", "Manticore", "Venomous Chimera", "XV  •  VENOM",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh2/manticore_art.png",
         "A winged predator with a scorpion tail lurking along European mountain passes.",
         "Defeat Manticores in Disc 1 dungeons.",
         7.5f, 0xD88048u, 2, 3, Element::Wind, AbilityId::Quickstep},
        {"sh2_man_eater", "Man-Eater", "Labyrinth Devourer", "XV  •  HUNGER",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh2/man_eater_art.png",
         "A carnivorous monstrosity lurking in subterranean ruins, draining vitality from unwary prey.",
         "Defeat Man-Eaters in Wine Cellar or Caves.",
         8.0f, 0x58C078u, 2, 4, Element::Earth, AbilityId::SoulDrain},
        {"sh2_shadow_knight", "Shadow Knight", "Phantom Duelist", "XI  •  IRON WILL",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh2/shadow_knight_art.png",
         "An armored revenant guarding cursed sanctuaries in Russia and Japan.",
         "Defeat Shadow Knights in Petrograd or Nihonbashi.",
         8.5f, 0x8088D8u, 3, 5, Element::Dark, AbilityId::Ward},
        {"sh2_neo_amon", "Neo-Amon", "Reforged Demon King", "XV  •  THE DEVIL",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Exotic, false,
         "assets/cards/sh2/neo_amon_art.png",
         "Amon's power reborn through Yuri's unyielding will—blazing with violet hellfire and absolute fury.",
         "Unlock Neo-Amon in Yuri's Graveyard (Disc 2).",
         9.0f, 0xD84CF0u, 5, 8, Element::Dark, AbilityId::Harmonixer},
        {"sh2_amon", "Amon", "Demon of the Mistletoe", "XVI  •  THE TOWER",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Legendary, false,
         "assets/cards/sh2/amon_art.png",
         "The iconic winged demon bound to Yuri's soul—a terrifying pillar of flame and ruin.",
         "Reclaim Amon's soul in Shadow Hearts: Covenant.",
         9.5f, 0xF05848u, 6, 9, Element::Fire, AbilityId::MaliceStrike},
        {"sh2_dark_seraphim", "Dark Seraphim", "Eclipse of Heaven", "XX  •  JUDGEMENT",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Legendary, false,
         "assets/cards/sh2/dark_seraphim_art.png",
         "The supreme forbidden fusion uniting light and abyss—unlockable only by conquering the deepest Graveyard trial.",
         "Defeat the Dark Seraphim optional superboss in Disc 2.",
         10.0f, 0xE8C0FFu, 6, 9, Element::Light, AbilityId::Harmonixer},

        // --- Bosses (7) ---
        {"sh2_veronica", "Veronica Vera", "Siren of Sapiente Gladio", "XIV  •  TEMPTATION",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Uncommon, true,
         "assets/cards/sh2/veronica_art.png",
         "A ruthless executive of Sapiente Gladio whose sadistic schemes plague Yuri across Europe.",
         "Defeat Veronica Vera in Disc 1.",
         7.5f, 0xE04888u, 3, 5, Element::Fire, AbilityId::Eclipse},
        {"sh2_rasputin", "Grigori Rasputin", "The Mad Monk", "XV  •  THE DEVIL",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/sh2/rasputin_art.png",
         "The scheming mystic of Petrograd who fused with Asmodeus to seize Russia's throne.",
         "Defeat Rasputin in the Idar Flamme.",
         8.5f, 0xA858E0u, 4, 6, Element::Dark, AbilityId::Vengeance},
        {"sh2_asmodeus", "Asmodeus", "Demon of Lust & Ruin", "XVI  •  CATACLYSM",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh2/asmodeus_art.png",
         "The colossal demon unleashed by Rasputin atop the floating fortress Idar Flamme.",
         "Defeat Asmodeus at the end of Disc 1.",
         9.0f, 0xE85890u, 5, 8, Element::Dark, AbilityId::Ward},
        {"sh2_astaroth", "Astaroth", "Duke of the Fallen", "XVIII  •  THE ECLIPSE",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh2/astaroth_art.png",
         "The sinister demon inhabiting Nicolai, commanding illusions and devastating shadow magic.",
         "Defeat Astaroth in the Foreigners' Cemetery.",
         9.0f, 0x7068F0u, 5, 8, Element::Dark, AbilityId::Eclipse},
        {"sh2_kato", "Kato Masaji", "Architect of the New World", "IV  •  THE EMPEROR",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh2/kato_art.png",
         "A broken visionary willing to rewrite history itself so those he loved need never suffer.",
         "Defeat Kato Masaji in Asuka Stone Platform.",
         9.5f, 0xF0B858u, 5, 8, Element::Fire, AbilityId::Covenant},
        {"sh2_susanoo", "Susano-o", "God of the Raging Storm", "XIX  •  TEMPEST",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/sh2/susanoo_art.png",
         "The legendary storm deity guarding the pinnacle of the Dog Shrine—a supreme optional trial.",
         "Defeat Susano-o in the Dog Shrine (Optional Superboss).",
         10.0f, 0x60D8FFu, 6, 9, Element::Wind, AbilityId::Ward},
        {"sh2_umbral_god", "Umbral God", "Sovereign of Malice", "XXI  •  THE WORLD",
         SeriesGame::Covenant, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/sh2/umbral_god_art.png",
         "The ultimate incarnation of human sorrow summoned at the climax of Covenant to unmake reality.",
         "Defeat the final boss of Shadow Hearts: Covenant.",
         10.0f, 0xFFD060u, 6, 10, Element::Dark, AbilityId::MaliceStrike},

        // =========================================================================
        //  SHADOW HEARTS: FROM THE NEW WORLD (1929) — 24 Cards
        // =========================================================================
        // --- Main Characters (7) ---
        {"sh3_johnny", "Johnny Garland", "The Young Detective", "0  •  THE FOOL",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/johnny_art.png",
         "A 16-year-old New York private eye whose missing memories hide the power of the Awaker.",
         "Begin Johnny Garland's investigation in New York.",
         7.5f, 0x58B4F8u, 3, 5, Element::Water, AbilityId::Quickstep},
        {"sh3_shania", "Shania", "Spirit-Fusing Huntress", "VII  •  THE CHARIOT",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/shania_art.png",
         "A fierce Native American bounty hunter who channels sacred totem spirits in combat.",
         "Team up with Shania in New York.",
         8.0f, 0xF07848u, 3, 5, Element::Fire, AbilityId::Harmonixer},
        {"sh3_natan", "Natan", "Guardian of the Plains", "XI  •  JUSTICE",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/natan_art.png",
         "Shania's stoic mentor, blending twin-revolver gun-fu with ancient guardian discipline.",
         "Recruit Natan in the American Southwest.",
         7.5f, 0xD0A058u, 3, 5, Element::Earth, AbilityId::Resonance},
        {"sh3_frank", "Frank Goldfinger", "The Ninja of Brazil", "VIII  •  STRENGTH",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/frank_art.png",
         "An eccentric American-turned-ninja who crafts absurd swords out of cacti, marlins, and corn.",
         "Recruit Frank Goldfinger in Alcatraz.",
         7.0f, 0x60D088u, 3, 5, Element::Wind, AbilityId::Quickstep},
        {"sh3_mao", "Mao", "The Drunken Fist Cat", "X  •  WHEEL OF FORTUNE",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/mao_art.png",
         "A giant talking movie-star cat who masters drunken kung fu and steals every scene.",
         "Recruit Mao in Hollywood / Las Vegas.",
         7.0f, 0xF5C858u, 3, 5, Element::Water, AbilityId::ArcaneEcho},
        {"sh3_ricardo", "Ricardo Gomez", "The Mariachi Gunslinger", "VI  •  THE LOVERS",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/ricardo_art.png",
         "A suave Mexican guitarist whose serenades conceal a hidden rocket launcher and machine gun.",
         "Recruit Ricardo Gomez in Roswell / Mexico.",
         8.0f, 0xE86858u, 4, 6, Element::Fire, AbilityId::SoulDrain},
        {"sh3_hilda", "Hilda Valentine", "Princess of the Bat", "XVIII  •  THE MOON",
         SeriesGame::FromTheNewWorld, CardCategory::MainCharacter, Rarity::Rare, false,
         "assets/cards/sh3/hilda_art.png",
         "The proud youngest Valentine vampire, shifting forms with calorie count and firing pink energy blasts.",
         "Recruit Hilda Valentine in Chicago / Chichen Itza.",
         8.5f, 0xE878D8u, 4, 6, Element::Dark, AbilityId::Covenant},

        // --- NPCs (3) ---
        {"sh3_killer", "Killer", "The Laughing Phantom", "XV  •  THE DEVIL",
         SeriesGame::FromTheNewWorld, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/sh3/killer_art.png",
         "A grinning, blood-soaked murderer trailing Johnny from the very first case in New York.",
         "Cross paths with Killer in Arkham University.",
         7.5f, 0xD84858u, 2, 4, Element::Dark, AbilityId::Vengeance},
        {"sh3_gilbert", "Gilbert", "The Hollow Alchemist", "XII  •  THE HANGED MAN",
         SeriesGame::FromTheNewWorld, CardCategory::Npc, Rarity::Uncommon, false,
         "assets/cards/sh3/gilbert_art.png",
         "A mysterious occultist serving Lady's grand design to open the Gate of Malice.",
         "Confront Gilbert during the Americas campaign.",
         8.0f, 0x9C74E0u, 3, 4, Element::Water, AbilityId::Eclipse},
        {"sh3_roger", "Roger Bacon (1929)", "Eternal Wanderer", "I  •  THE MAGICIAN",
         SeriesGame::FromTheNewWorld, CardCategory::Npc, Rarity::Exotic, false,
         "assets/cards/sh3/roger_art.png",
         "Still alive and meddling in 1929, Roger once again lends cryptic wisdom to save humanity.",
         "Meet Roger Bacon in From the New World.",
         6.5f, 0x58D8A8u, 4, 6, Element::Wind, AbilityId::ArcaneEcho},

        // --- Beasts & Fusions (8) ---
        {"sh3_automaton", "Stray Automaton", "Clockwork Menace", "XVI  •  IRON",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh3/automaton_art.png",
         "A malfunctioning mechanical construct stalking the back alleys of 1929 New York.",
         "Defeat Automatons in early New York.",
         7.0f, 0xA8B0BCu, 1, 2, Element::Earth, AbilityId::None},
        {"sh3_alcatraz_guard", "Cursed Warden", "Phantom of the Rock", "XI  •  CHAINS",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh3/alcatraz_guard_art.png",
         "A spectral jailer patrolling the haunted cellblocks of Alcatraz Island.",
         "Defeat Cursed Wardens in Alcatraz.",
         7.5f, 0x7888B0u, 1, 3, Element::Water, AbilityId::Ward},
        {"sh3_desert_scorpion", "Desert Scorpion", "Canyon Stinger", "VIII  •  STING",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Common, false,
         "assets/cards/sh3/desert_scorpion_art.png",
         "An oversized armored arachnid emerging from the sun-baked sands of the Grand Canyon.",
         "Defeat Desert Scorpions in the Southwest.",
         7.0f, 0xD89448u, 2, 3, Element::Fire, AbilityId::Quickstep},
        {"sh3_voodoo_doll", "Voodoo Doll", "Hexed Effigy", "XIII  •  CURSE",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh3/voodoo_doll_art.png",
         "A stitched fetish crackling with dark swamp hexes in the Caribbean and Mayan ruins.",
         "Defeat Voodoo Dolls in mid-game ruins.",
         7.5f, 0xA860D8u, 2, 4, Element::Dark, AbilityId::Eclipse},
        {"sh3_blood_golem", "Blood Golem", "Altar Construct", "VIII  •  COLOSSUS",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Uncommon, false,
         "assets/cards/sh3/blood_golem_art.png",
         "A hulking crimson statue awakened within ancient Mesoamerican temples.",
         "Defeat Blood Golems in Chichen Itza.",
         8.5f, 0xD04848u, 3, 5, Element::Earth, AbilityId::Ward},
        {"sh3_tatanenka", "Tatanenka", "Thunderbird Totem", "XIX  •  THUNDER",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Exotic, false,
         "assets/cards/sh3/tatanenka_art.png",
         "Shania's blazing wind-and-lightning spirit fusion, descending from storm clouds with sacred fury.",
         "Master Shania's Thunderbird Fetish Fusion.",
         8.5f, 0x60D8FFu, 5, 7, Element::Wind, AbilityId::Harmonixer},
        {"sh3_tirawa", "Tirawa", "Celestial Sovereign", "XVII  •  THE COSMOS",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Exotic, false,
         "assets/cards/sh3/tirawa_art.png",
         "The ultimate sacred spirit totem bestowed upon Shania after conquering the spirit trials.",
         "Unlock Tirawa's hidden fusion in From the New World.",
         9.5f, 0xF8D878u, 5, 8, Element::Light, AbilityId::Harmonixer},
        {"sh3_awaker", "Awaker", "True Form of Johnny", "XX  •  AWAKENING",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Legendary, false,
         "assets/cards/sh3/awaker_art.png",
         "Johnny's latent cosmic form unleashed—shattering the boundary between human will and divine Malice.",
         "Unlock Johnny's Awaker transformation in the endgame.",
         10.0f, 0x70E0FFu, 6, 9, Element::Light, AbilityId::Covenant},

        // --- Bosses (6) ---
        {"sh3_edna", "Edna Capone", "Matriarch of Chicago", "III  •  AMBITION",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Uncommon, true,
         "assets/cards/sh3/edna_art.png",
         "The ruthless underworld boss whose crimson ambush tests Johnny's crew in Chicago.",
         "Defeat Edna's forces in Chicago.",
         7.5f, 0xE06068u, 3, 5, Element::Fire, AbilityId::Quickstep},
        {"sh3_enforcer", "Syndicate Behemoth", "Armored Gangster Horror", "XVI  •  IRON FIST",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Uncommon, true,
         "assets/cards/sh3/enforcer_art.png",
         "A Malice-mutated syndicate brute tearing through the Chicago hideout.",
         "Defeat the Syndicate Behemoth boss.",
         8.0f, 0xB88858u, 3, 5, Element::Earth, AbilityId::Ward},
        {"sh3_killer_boss", "Killer (Awakened)", "Avatar of Slaughter", "XIII  •  REAPER",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Rare, true,
         "assets/cards/sh3/killer_boss_art.png",
         "Shedding his human guise, Killer reveals a monstrous blade-wielding form fueled by pure malice.",
         "Defeat Killer's true form.",
         8.5f, 0xE84458u, 4, 6, Element::Dark, AbilityId::MaliceStrike},
        {"sh3_gilbert_boss", "Gilbert (Malice)", "Herald of the Gate", "XVI  •  THE TOWER",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Exotic, true,
         "assets/cards/sh3/gilbert_boss_art.png",
         "Transformed by the Gate's torrent of energy, Gilbert stands as the final guardian before Lady.",
         "Defeat Gilbert inside the Gate.",
         9.0f, 0xB460F0u, 5, 8, Element::Water, AbilityId::Eclipse},
        {"sh3_lady", "Lady", "Bringer of the New World", "XX  •  JUDGEMENT",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/sh3/lady_art.png",
         "The enigmatic woman tied to Johnny's forgotten past, seeking to unseal the ancient Gate of Malice.",
         "Defeat Lady in the climax of From the New World.",
         9.5f, 0xF088D8u, 6, 9, Element::Dark, AbilityId::Sanctuary},
        {"sh3_malice_goddess", "Malice Goddess", "Heart of the Gate", "XXI  •  THE WORLD",
         SeriesGame::FromTheNewWorld, CardCategory::Beast, Rarity::Legendary, true,
         "assets/cards/sh3/malice_goddess_art.png",
         "The primordial divinity of Malice awakened beyond the Gate—the final trial of the Shadow Hearts saga.",
         "Defeat the final boss of From the New World.",
         10.0f, 0xFFD870u, 6, 10, Element::Dark, AbilityId::Ward},
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

    // Grant a specific card using its FIXED rarity + chosen cosmetic Finish
    DropRecord grant_card(const std::string &card_id, CardFinish finish, const std::string &source) {
        const CardDef *def = find_card_def(card_id);
        const Rarity fixed_rar = def ? def->rarity : Rarity::Common;
        const int fidx = std::clamp(static_cast<int>(finish), 0, kFinishCount - 1);
        OwnedCard &oc = owned[card_id];
        const bool new_card = !oc.unlocked();
        const bool new_fin = (oc.counts[static_cast<std::size_t>(fidx)] == 0);
        oc.counts[static_cast<std::size_t>(fidx)] += 1;
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

    // Dismantle 1 duplicate copy of a finish (requires count > 1 so last copy is protected)
    bool dismantle_duplicate(const std::string &card_id, CardFinish fin, int &gained_out) {
        gained_out = 0;
        const CardDef *def = find_card_def(card_id);
        if (!def) return false;
        const int fidx = std::clamp(static_cast<int>(fin), 0, kFinishCount - 1);
        auto it = owned.find(card_id);
        if (it == owned.end()) return false;
        int &cnt = it->second.counts[static_cast<std::size_t>(fidx)];
        if (cnt <= 1) return false;
        --cnt;
        gained_out = dismantle_essence_value(def->rarity, fin);
        essence += gained_out;
        save();
        return true;
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
