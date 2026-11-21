#pragma once

// Shared, zero-runtime-dependency Shadow Hearts: Covenant 110-achievement catalog,
// tier metadata, and theme palettes used by both Shadow Hearts Covenant.exe and
// the Shadow Hearts Nocturne.exe launcher.

#include <algorithm>
#include <array>
#include <cctype>
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

enum class CatalogGame : int {
    Covenant = 0,
    ShadowHearts1 = 1,
};

struct CatalogItem {
    const char *id;
    const char *title;
    const char *description;
    Tier tier;
    bool secret;
    bool missable;
};

// Display order of the 8 Trophy Gallery filter tabs across both the Launcher and in-game F1 overlay:
//   All -> Mark -> Sigil -> Crest -> Sovereign -> Follies -> Curiosities -> Covenant (pinnacle far-right)
struct TierTabSpec {
    int tier_filter;      // -1 = All, else static_cast<int>(Tier)
    const char *label;
};

inline constexpr std::array<TierTabSpec, 8> kGalleryTierTabs = {{
    {-1, "All"},
    {static_cast<int>(Tier::Mark), "Mark"},
    {static_cast<int>(Tier::Sigil), "Sigil"},
    {static_cast<int>(Tier::Crest), "Crest"},
    {static_cast<int>(Tier::Sovereign), "Sovereign"},
    {static_cast<int>(Tier::Folly), "Follies"},
    {static_cast<int>(Tier::Curiosity), "Curiosities"},
    {static_cast<int>(Tier::Covenant), "Covenant"},
}};

inline constexpr std::uint32_t catalog_rgba(int r, int g, int b, int a = 255) {
    return static_cast<std::uint32_t>(r & 255) |
           (static_cast<std::uint32_t>(g & 255) << 8) |
           (static_cast<std::uint32_t>(b & 255) << 16) |
           (static_cast<std::uint32_t>(a & 255) << 24);
}

inline const char *catalog_tier_name(Tier t) {
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

inline const char *catalog_tier_short(Tier t) {
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

inline const char *catalog_toast_theme_name(int theme) {
    static const char *const kNames[] = {"Tier-Tinted Gradients", "Soul Graveyard Eclipse", "Judgment Ring Crimson", "1915 Occult Grimoire"};
    return kNames[theme >= 0 && theme < static_cast<int>(ToastTheme::Count) ? theme : 0];
}

inline const char *catalog_toast_theme_word(int theme) {
    static const char *const kWords[] = {"tier_tinted", "soul_graveyard", "judgment_ring", "occult_grimoire"};
    return kWords[theme >= 0 && theme < static_cast<int>(ToastTheme::Count) ? theme : 0];
}

inline int catalog_parse_toast_theme(const std::string &word) {
    std::string w;
    for (unsigned char c : word)
        if (std::isalnum(c)) w.push_back(static_cast<char>(std::tolower(c)));
    if (w == "soulgraveyard" || w == "eclipse" || w == "1") return 1;
    if (w == "judgmentring" || w == "crimson" || w == "2") return 2;
    if (w == "occultgrimoire" || w == "grimoire" || w == "3") return 3;
    return 0;
}

inline void catalog_theme_colors(Tier t, int theme_index,
                                 std::uint32_t &grad_left, std::uint32_t &grad_right,
                                 std::uint32_t &border, std::uint32_t &accent) {
    if (theme_index == static_cast<int>(ToastTheme::SoulGraveyard)) {
        grad_left = catalog_rgba(18, 12, 30, 244);
        grad_right = catalog_rgba(59, 17, 43, 244);
        border = catalog_rgba(226, 180, 90, 255);
        accent = catalog_rgba(244, 210, 130, 255);
        return;
    }
    if (theme_index == static_cast<int>(ToastTheme::JudgmentRing)) {
        grad_left = catalog_rgba(27, 20, 16, 244);
        grad_right = catalog_rgba(92, 22, 32, 244);
        border = catalog_rgba(242, 193, 78, 255);
        accent = catalog_rgba(255, 214, 112, 255);
        return;
    }
    if (theme_index == static_cast<int>(ToastTheme::OccultGrimoire)) {
        grad_left = catalog_rgba(15, 23, 42, 244);
        grad_right = catalog_rgba(30, 41, 59, 244);
        border = catalog_rgba(56, 189, 248, 255);
        accent = catalog_rgba(214, 192, 141, 255);
        return;
    }
    switch (t) {
    case Tier::Mark:
        grad_left = catalog_rgba(35, 39, 51, 244);
        grad_right = catalog_rgba(124, 86, 52, 244);
        border = catalog_rgba(200, 155, 99, 255);
        accent = catalog_rgba(236, 196, 140, 255);
        break;
    case Tier::Sigil:
        grad_left = catalog_rgba(19, 34, 61, 244);
        grad_right = catalog_rgba(36, 123, 160, 244);
        border = catalog_rgba(86, 207, 225, 255);
        accent = catalog_rgba(142, 232, 245, 255);
        break;
    case Tier::Crest:
        grad_left = catalog_rgba(38, 17, 56, 244);
        grad_right = catalog_rgba(131, 33, 83, 244);
        border = catalog_rgba(224, 108, 159, 255);
        accent = catalog_rgba(248, 156, 198, 255);
        break;
    case Tier::Sovereign:
        grad_left = catalog_rgba(26, 20, 14, 246);
        grad_right = catalog_rgba(184, 130, 30, 246);
        border = catalog_rgba(247, 201, 72, 255);
        accent = catalog_rgba(255, 228, 132, 255);
        break;
    case Tier::Covenant:
        grad_left = catalog_rgba(20, 8, 18, 248);
        grad_right = catalog_rgba(120, 18, 38, 248);
        border = catalog_rgba(255, 226, 138, 255);
        accent = catalog_rgba(255, 214, 92, 255);
        break;
    case Tier::Folly:
        grad_left = catalog_rgba(38, 17, 51, 244);
        grad_right = catalog_rgba(196, 60, 132, 244);
        border = catalog_rgba(255, 158, 187, 255);
        accent = catalog_rgba(255, 196, 216, 255);
        break;
    case Tier::Curiosity:
    default:
        grad_left = catalog_rgba(12, 31, 29, 244);
        grad_right = catalog_rgba(36, 140, 118, 244);
        border = catalog_rgba(110, 231, 200, 255);
        accent = catalog_rgba(162, 245, 222, 255);
        break;
    }
}

inline const std::vector<CatalogItem> &catalog_items() {
    static const std::vector<CatalogItem> kItems = {
        // Tier 1: Mark — Easy (24)
        {"mark_first_blood", "First Blood on the Ring", "Land your first successful Strike on the Judgment Ring.", Tier::Mark, false, false},
        {"mark_first_victory", "Baptism by Fire", "Claim your very first battle victory.", Tier::Mark, false, false},
        {"mark_first_spark", "First Spark of Soul", "Awaken Yuri in your active party and claim a victory with him.", Tier::Mark, false, false},
        {"mark_first_perfect", "Dead Center", "Land your very first Perfect Strike Zone on the Judgment Ring.", Tier::Mark, false, false},
        {"mark_five_perfects", "Steady Needle", "Land 5 Perfect Strike Zones on the Judgment Ring.", Tier::Mark, false, false},
        {"mark_getting_rhythm", "Getting the Rhythm", "String together 3 consecutive Judgment Ring successes without a miss.", Tier::Mark, false, false},
        {"mark_double_perfect", "Twin Bullseyes", "Land 2 consecutive Perfect Strike Zones in a row.", Tier::Mark, false, false},
        {"mark_clean_sweep", "Clean Sweep", "Win a battle without missing a single Judgment Ring input.", Tier::Mark, false, false},
        {"mark_ring_apprentice", "Spin of Fate", "Complete 25 successful Judgment Ring strikes.", Tier::Mark, false, false},
        {"mark_skirmisher", "Frontline Skirmisher", "Win 10 battles against the horrors of 1915.", Tier::Mark, false, false},
        {"mark_first_steps_lv5", "Finding Your Footing", "Reach Level 5 with any party member.", Tier::Mark, false, false},
        {"mark_rising_blade", "Rising Blade", "Reach Level 10 with any party member.", Tier::Mark, false, false},
        {"mark_seasoned_initiate", "Seasoned Initiate", "Reach Level 15 with any party member.", Tier::Mark, false, false},
        {"mark_first_coin", "Soldier's Stipend", "Accumulate 2,500 Gold across battles and loot.", Tier::Mark, false, false},
        {"mark_pocket_change", "Pocket Change", "Accumulate your first 10,000 Gold across battles and loot.", Tier::Mark, false, false},
        {"mark_first_purchase", "Patron of the Magimels", "Spend at least 2,000 Gold at shops and peddlers.", Tier::Mark, false, false},
        {"mark_treasure_seeker", "Light-Fingered", "Open your first 5 treasure chests.", Tier::Mark, false, false},
        {"mark_fresh_ink", "Fresh Ink", "Collect 12 distinct item or Crest supplies and open 3 treasure chests.", Tier::Mark, false, false},
        {"mark_quartermaster", "Field Quartermaster", "Carry 20 distinct item or Crest types in your inventory.", Tier::Mark, false, false},
        {"mark_karin_bond", "Iron Valkyrie", "Raise Karin to Level 10.", Tier::Mark, false, false},
        {"mark_blanca_pack", "Loyal Fang", "Recruit Blanca into the active Covenant party.", Tier::Mark, false, false},
        {"mark_gepetto_craft", "The Puppeteer's Art", "Recruit Gepetto into the active Covenant party.", Tier::Mark, false, false},
        {"mark_wanderer", "Border Crosser", "Journey across 2 distinct regions of 1915 Europe & Asia.", Tier::Mark, false, false},
        {"mark_first_hour", "Into the Shadows", "Log at least 1 hour of playtime on an active save.", Tier::Mark, false, false},

        // Tier 2: Sigil — Normal (19)
        {"sigil_ring_adept", "Ring Adept", "Complete 100 successful Judgment Ring strikes.", Tier::Sigil, false, false},
        {"sigil_unbroken_six", "Unbroken Rhythm", "String together 6 consecutive Judgment Ring successes without a miss.", Tier::Sigil, false, false},
        {"sigil_tightening_arc", "Tightening the Arc", "Land 25 Perfect Strike Zones on the Judgment Ring.", Tier::Sigil, false, false},
        {"sigil_triple_perfect", "Crimson Cadence", "Land 3 consecutive Perfect Strike Zones in a row.", Tier::Sigil, false, false},
        {"sigil_battle_hardened", "Battle Hardened", "Win 40 battles across your campaign.", Tier::Sigil, false, false},
        {"sigil_flawless_five", "Precision Vanguard", "Win 5 consecutive battles without missing a single Judgment Ring input.", Tier::Sigil, false, false},
        {"sigil_mercenary_vault", "Mercenary's Vault", "Hold 100,000 Gold in your active wallet at one time.", Tier::Sigil, false, false},
        {"sigil_war_chest", "Quartermaster's Hoard", "Accumulate 200,000 total Gold across your journey.", Tier::Sigil, false, false},
        {"sigil_veteran_front", "Veteran of the Front", "Reach Level 35 with any party member.", Tier::Sigil, false, false},
        {"sigil_covenant_elements", "Covenant of Elements", "Build Yuri to Level 30+ with at least 4 core Covenant party members.", Tier::Sigil, false, false},
        {"sigil_balanced_vanguard", "United Front", "Raise at least 4 core party members to Level 25 or higher.", Tier::Sigil, false, false},
        {"sigil_lucia_tarot", "Reader of Fate", "Recruit Lucia and raise her to Level 25.", Tier::Sigil, false, false},
        {"sigil_anastasia_lens", "Imperial Snapshot", "Recruit Anastasia and raise her to Level 25.", Tier::Sigil, false, false},
        {"sigil_kurando_blade", "Heir of Inugami", "Recruit Kurando into the active Covenant party.", Tier::Sigil, false, false},
        {"sigil_full_roster", "The Eight of 1915", "Assemble all 8 core playable Covenant party members.", Tier::Sigil, false, false},
        {"sigil_relic_hunter", "Relic Hunter", "Uncover 30 treasure chests across the world.", Tier::Sigil, false, false},
        {"sigil_traveler_ledger", "Traveler's Ledger", "Explore and progress across 3 distinct regions of 1915 Europe & Asia.", Tier::Sigil, false, false},
        {"sigil_globe_trotter", "Continental Express", "Journey across 8 distinct world regions.", Tier::Sigil, false, false},
        {"sigil_ten_hours", "Deep in the Grimoire", "Log 10 hours of playtime on an active save.", Tier::Sigil, false, false},

        // Tier 3: Crest — Hard (15)
        {"crest_ring_veteran", "Wheel of Iron", "Complete 400 successful Judgment Ring strikes.", Tier::Crest, false, false},
        {"crest_razors_edge", "Razor's Edge", "Execute 50 Perfect Strike Zones on the Judgment Ring.", Tier::Crest, false, false},
        {"crest_century_perfects", "Century of Crimson", "Land 125 Perfect Strike Zones on the Judgment Ring.", Tier::Crest, false, false},
        {"crest_relentless_chain", "Relentless Chain", "Land 12 consecutive Judgment Ring Strike Zones without a miss.", Tier::Crest, false, false},
        {"crest_quad_perfect", "Fourfold Edge", "Land 4 consecutive Perfect Strike Zones without slipping.", Tier::Crest, false, false},
        {"crest_flawless_fifteen", "Untouchable Tempo", "Win 15 consecutive battles without missing a single Judgment Ring input.", Tier::Crest, false, false},
        {"crest_master_grimoire", "Master of the Grimoire", "Acquire 15+ distinct items/Crests and win 75 battles.", Tier::Crest, false, false},
        {"crest_warlord", "Scourge of Sapientes Gladio", "Win 200 battles across your journey.", Tier::Crest, false, false},
        {"crest_high_roller", "High Roller's Hoard", "Amass 500,000 Gold.", Tier::Crest, false, false},
        {"crest_apex_vessel", "Apex Vessel", "Reach Level 55 with any party member.", Tier::Crest, false, false},
        {"crest_level_70", "Harmonixer Ascendant", "Reach Level 70 with any party member.", Tier::Crest, false, false},
        {"crest_all_party_50", "Legion of the Covenant", "Raise all 8 core party members to Level 50 or higher.", Tier::Crest, false, false},
        {"crest_treasure_master", "Master Plunderer", "Open 75 treasure chests across Europe and Japan.", Tier::Crest, false, false},
        {"crest_grand_cartographer", "Atlas of 1915", "Visit 15 distinct world regions across both discs.", Tier::Crest, false, false},
        {"crest_collector", "Occult Armory", "Hold 60 distinct item or Crest types simultaneously.", Tier::Crest, false, false},

        // Tier 4: Sovereign — Super Hard / Hardcore Ceiling (11)
        {"sovereign_tycoon", "Tycoon of the Underworld", "Accumulate 2,000,000 Gold across your journey.", Tier::Sovereign, false, false},
        {"sovereign_ten_million", "Emperor of the Black Market", "Accumulate 10,000,000 Gold across your journey.", Tier::Sovereign, false, false},
        {"sovereign_transcendental", "Transcendental Level", "Reach Level 80 or higher on any character.", Tier::Sovereign, false, false},
        {"sovereign_level_99_party", "Pantheon of Souls", "Raise every single one of the 8 core party members to Level 99.", Tier::Sovereign, false, false},
        {"sovereign_clockwork_demon", "Clockwork Demon", "Land 20 consecutive Judgment Ring hits without a single miss.", Tier::Sovereign, false, false},
        {"sovereign_fifty_streak", "Metronome of God", "Land 50 consecutive Judgment Ring hits without a single miss.", Tier::Sovereign, false, false},
        {"sovereign_bare_knuckles", "Bare Knuckles, No Mercy", "Win 150 battles while maintaining at least 90% Judgment Ring accuracy.", Tier::Sovereign, false, false},
        {"sovereign_five_hundred_wins", "Harbinger of Malice", "Win 500 battles across your journey.", Tier::Sovereign, false, false},
        {"sovereign_ring_of_iron", "Ring of Iron", "Land 250 Perfect Strike Zones with at least 85% Judgment Ring accuracy.", Tier::Sovereign, false, false},
        {"sovereign_thousand_perfects", "One Thousand Sunbeams", "Execute 1,000 Perfect Strike Zones on the Judgment Ring.", Tier::Sovereign, false, false},
        {"sovereign_surgical_perfection", "Surgical Perfection", "Win 250+ battles with at least 95% Ring accuracy and 6 consecutive Perfect Strikes.", Tier::Sovereign, false, false},

        // Tier 5: The Covenant — Capstone (1)
        {"covenant_sealed", "The Covenant Sealed", "Unlock every single Mark, Sigil, Crest, Sovereign, Folly, and Curiosity.", Tier::Covenant, false, false},

        // Follies — Fun, Quirky, Blunders & Challenges (15)
        {"folly_wrestling_destiny", "Wrestling with Destiny", "Recruit Joachim and bring his wrestling spectacle into a victorious battle.", Tier::Folly, false, false},
        {"folly_manly_men", "Manly Men Assemble", "Push Joachim past Level 40 to honor the sheer absurdity of the Man Festival.", Tier::Folly, false, false},
        {"folly_decked_out", "Decked Out", "Plunder 20+ hidden chests and collectibles across the world.", Tier::Folly, false, false},
        {"folly_panic_button", "Panic Button", "Botch a Judgment Ring input mid-battle and still survive to claim victory.", Tier::Folly, false, false},
        {"folly_overkill_theater", "Overkill Theater", "Flex on foes with a 10+ hit Ring streak after reaching Level 25.", Tier::Folly, false, false},
        {"folly_butterfingers", "Butterfingers on the Ring", "Miss 3 consecutive Judgment Ring inputs in a row.", Tier::Folly, false, false},
        {"folly_tactical_retreat", "Brave Sir Robin", "Flee or bail out of 10 battles (battles entered minus won >= 10).", Tier::Folly, false, false},
        {"folly_hairs_breadth", "By a Hair's Breadth", "Win a battle while an active party member clings to life at 15% HP or less.", Tier::Folly, false, false},
        {"folly_big_spender", "Retail Therapy", "Drop 50,000+ Gold in a single shopping spree.", Tier::Folly, false, false},
        {"folly_miser_king", "Dragon's Mattress", "Hoard 500,000 Gold in your active wallet at once without spending it.", Tier::Folly, false, false},
        {"folly_good_boy", "Alpha of the Pack", "Raise Blanca to Level 50 — the goodest wolf in 1915.", Tier::Folly, false, false},
        {"folly_doll_army", "Cornelia's Finest", "Raise Gepetto to Level 50.", Tier::Folly, false, false},
        {"folly_royal_menace", "Princess of Destruction", "Raise Anastasia to Level 50.", Tier::Folly, false, false},
        {"folly_perfect_bout", "Flawless Showboat", "Win 5 battles where every Judgment Ring input landed in the Perfect Zone.", Tier::Folly, false, false},
        {"folly_nocturne_marathon", "Midnight Oil", "Log 40+ hours of playtime on an active save file.", Tier::Folly, false, false},

        // Curiosities — Secret Until Unlocked: Story, Optional Dungeons & Missables (25)
        {"curiosity_whispers_graveyard", "Whispers in the Graveyard", "Step inside Yuri's Soul Graveyard and witness its hidden memories.", Tier::Curiosity, true, false},
        {"curiosity_forgotten_page", "Forgotten Page", "Uncover 10 hidden treasures off the beaten story path.", Tier::Curiosity, true, false},
        {"curiosity_echoes_1915", "Echoes of 1915", "Cross into Disc 2 (or journey across 5+ distinct world regions).", Tier::Curiosity, true, false},
        {"curiosity_unlikely_encounter", "The Unlikely Encounter", "Survive 25 battles with at least 15 Perfect Judgment Ring Strikes.", Tier::Curiosity, true, false},
        {"curiosity_keeper_obscura", "Keeper of Obscura", "Uncover 40+ hidden chests across 6+ distinct world regions.", Tier::Curiosity, true, false},
        {"curiosity_story_domremy", "The Demon of Domremy", "Reach the quiet village of Domremy after the prologue.", Tier::Curiosity, true, false},
        {"curiosity_story_ardennes", "Shadows of Ardennes", "Venture into the mist-shrouded Ardennes Forest.", Tier::Curiosity, true, false},
        {"curiosity_story_paris", "City of Lights & Secrets", "Arrive in 1915 Paris.", Tier::Curiosity, true, false},
        {"curiosity_story_lehavre", "Harbor of Intrigue", "Reach the port of Le Havre and its hidden Wine Cellar.", Tier::Curiosity, true, false},
        {"curiosity_story_rhondda", "Beneath the Rhondda", "Brave the dark shafts of Rhondda Mine after landing in England.", Tier::Curiosity, true, false},
        {"curiosity_story_wales", "Roger's Sanctuary", "Visit eccentric scholar Roger Bacon's cottage in Wales.", Tier::Curiosity, true, false},
        {"curiosity_story_florence", "Florentine Nocturne", "Cross into Italy and arrive in Florence.", Tier::Curiosity, true, false},
        {"curiosity_story_marguerite", "The Iron Prison", "Infiltrate the island fortress of St. Marguerite.", Tier::Curiosity, true, false},
        {"curiosity_story_petrograd", "Winter in Petrograd", "Journey east into Imperial Russia and reach Petrograd.", Tier::Curiosity, true, false},
        {"curiosity_story_goreme", "Valley of Stone", "Explore the Goreme Valley or the Gallery of the Dead.", Tier::Curiosity, true, false},
        {"curiosity_story_idar_flamme", "Ascent of Idar Flamme", "Reach Idar Flamme for the shattering climax of Disc 1.", Tier::Curiosity, true, false},
        {"curiosity_story_yokohama", "Eastward Bound", "Arrive in Yokohama as the Disc 2 chapter begins.", Tier::Curiosity, true, false},
        {"curiosity_story_inugami", "Blood of Inugami", "Reach Inugami Village and uncover Kurando's heritage.", Tier::Curiosity, true, false},
        {"curiosity_story_immortal_mt", "Trial of Immortal Mountain", "Begin the ascent up Immortal Mountain.", Tier::Curiosity, true, false},
        {"curiosity_story_queens_garden", "The Queen's Garden", "Enter the Queen's Garden in the final stretch of the journey.", Tier::Curiosity, true, false},
        {"curiosity_secret_doll_house", "House of Porcelain", "Discover the secret Doll House dungeon.", Tier::Curiosity, true, false},
        {"curiosity_secret_dog_shrine", "Shrine of the Wolf", "Enter the hidden Dog Shrine for Blanca's ultimate trials.", Tier::Curiosity, true, false},
        {"curiosity_missable_nicolai", "Before the Betrayal", "(Missable) Win 3 battles alongside Nicolai in Apoina Tower before he leaves.", Tier::Curiosity, true, true},
        {"curiosity_missable_apoina_plunder", "Apoina's Early Spoils", "(Missable) Plunder 4+ treasure chests while still inside Apoina Tower.", Tier::Curiosity, true, true},
        {"curiosity_missable_mikasa", "Boarding the Mikasa", "(Missable) Claim a battle victory aboard Battleship Mikasa before it departs.", Tier::Curiosity, true, true},
    };
    return kItems;
}

inline const std::vector<CatalogItem> &sh1_catalog_items() {
    static const std::vector<CatalogItem> kItems = {
        // Tier 1: Mark — Easy (16)
        {"sh1_mark_first_strike", "Blood on the Tracks", "Land your first successful Strike on the 1913 Judgment Ring.", Tier::Mark, false, false},
        {"sh1_mark_first_battle", "Baptism of Malice", "Survive and complete your very first battle.", Tier::Mark, false, false},
        {"sh1_mark_first_perfect", "Razor's Mark", "Land your first Perfect Strike Zone on the Judgment Ring.", Tier::Mark, false, false},
        {"sh1_mark_five_perfects", "Steady Pulse", "Land 5 Perfect Strike Zones on the Judgment Ring.", Tier::Mark, false, false},
        {"sh1_mark_three_streak", "Rhythm of 1913", "String together 3 consecutive Judgment Ring successes without a miss.", Tier::Mark, false, false},
        {"sh1_mark_double_perfect", "Twin Precision", "Land 2 consecutive Perfect Strike Zones in a row.", Tier::Mark, false, false},
        {"sh1_mark_clean_battle", "Clean Sweep", "Finish a battle without missing a single Judgment Ring input.", Tier::Mark, false, false},
        {"sh1_mark_ring_25", "Wheel of Fate", "Complete 25 successful Judgment Ring strikes.", Tier::Mark, false, false},
        {"sh1_mark_battles_10", "Night Watch", "Complete 10 battles across China and Europe.", Tier::Mark, false, false},
        {"sh1_mark_lv5", "Awakening Harmonics", "Reach Level 5 with any party member.", Tier::Mark, false, false},
        {"sh1_mark_lv10", "Sharpened Claws", "Reach Level 10 with any party member.", Tier::Mark, false, false},
        {"sh1_mark_lv15", "Seasoned Harmonixer", "Reach Level 15 with any party member.", Tier::Mark, false, false},
        {"sh1_mark_cash_2500", "Traveler's Stipend", "Hold 2,500 Cash on your journey.", Tier::Mark, false, false},
        {"sh1_mark_cash_10000", "Silver Purse", "Hold 10,000 Cash on your journey.", Tier::Mark, false, false},
        {"sh1_mark_shop_discount", "Silver Tongue", "Win your first Shop Ring discount from a merchant.", Tier::Mark, false, false},
        {"sh1_mark_first_hour", "Into the Dark", "Log at least 1 hour of playtime on an active save.", Tier::Mark, false, false},

        // Tier 2: Sigil — Normal (14)
        {"sh1_sigil_alice", "Daughter of Light", "Recruit Alice Elliot and raise her to Level 15.", Tier::Sigil, false, false},
        {"sh1_sigil_zhuzhen", "Master of Yin and Yang", "Recruit Zhuzhen Liu into your party.", Tier::Sigil, false, false},
        {"sh1_sigil_margarete", "Femme Fatale", "Recruit Margarete Gertrude Zelle into your party.", Tier::Sigil, false, false},
        {"sh1_sigil_keith", "Lord of Blue Castle", "Recruit Keith Valentine into your party.", Tier::Sigil, false, false},
        {"sh1_sigil_halley", "Child of London", "Recruit Halley Brancket into your party.", Tier::Sigil, false, false},
        {"sh1_sigil_full_party", "The Six of 1913", "Assemble all 6 playable party members.", Tier::Sigil, false, false},
        {"sh1_sigil_ring_100", "Ring Adept", "Complete 100 successful Judgment Ring strikes.", Tier::Sigil, false, false},
        {"sh1_sigil_perfect_25", "Crimson Needle", "Land 25 Perfect Strike Zones on the Judgment Ring.", Tier::Sigil, false, false},
        {"sh1_sigil_streak_6", "Unbroken Tempo", "String together 6 consecutive Judgment Ring successes without a miss.", Tier::Sigil, false, false},
        {"sh1_sigil_triple_perfect", "Threefold Edge", "Land 3 consecutive Perfect Strike Zones in a row.", Tier::Sigil, false, false},
        {"sh1_sigil_battles_40", "Battle Hardened", "Complete 40 battles across your journey.", Tier::Sigil, false, false},
        {"sh1_sigil_flawless_5", "Iron Discipline", "Complete 5 consecutive battles without missing a single Ring input.", Tier::Sigil, false, false},
        {"sh1_sigil_lv30", "Veteran of Shadows", "Reach Level 30 with any party member.", Tier::Sigil, false, false},
        {"sh1_sigil_ten_hours", "Chronicle of 1913", "Log 10 hours of playtime on an active save.", Tier::Sigil, false, false},

        // Tier 3: Crest — Hard (11)
        {"sh1_crest_ring_300", "Wheel of Iron", "Complete 300 successful Judgment Ring strikes.", Tier::Crest, false, false},
        {"sh1_crest_perfect_100", "Century of Precision", "Land 100 Perfect Strike Zones on the Judgment Ring.", Tier::Crest, false, false},
        {"sh1_crest_streak_12", "Relentless Cadence", "Land 12 consecutive Judgment Ring hits without a miss.", Tier::Crest, false, false},
        {"sh1_crest_quad_perfect", "Fourfold Fang", "Land 4 consecutive Perfect Strike Zones without slipping.", Tier::Crest, false, false},
        {"sh1_crest_flawless_12", "Untouchable Will", "Complete 12 consecutive battles without missing a single Ring input.", Tier::Crest, false, false},
        {"sh1_crest_battles_150", "Bane of Malice", "Complete 150 battles across China and Europe.", Tier::Crest, false, false},
        {"sh1_crest_lv50", "Harmonixer Ascendant", "Reach Level 50 with any party member.", Tier::Crest, false, false},
        {"sh1_crest_party_45", "United Against the Void", "Raise all 6 party members to Level 45 or higher.", Tier::Crest, false, false},
        {"sh1_crest_cash_250k", "Merchant Prince", "Hold 250,000 Cash in your purse.", Tier::Crest, false, false},
        {"sh1_crest_discounts_25", "Master Negotiator", "Win 25 Shop Ring discounts from merchants.", Tier::Crest, false, false},
        {"sh1_crest_areas_12", "Atlas of 1913", "Visit 12 distinct locations across China and Europe.", Tier::Crest, false, false},

        // Tier 4: Sovereign — Super Hard (8)
        {"sh1_sovereign_battles_500", "Scourge of the Four Masks", "Complete 500 battles across your campaign.", Tier::Sovereign, false, false},
        {"sh1_sovereign_perfect_500", "Five Hundred Bullseyes", "Execute 500 Perfect Strike Zones on the Judgment Ring.", Tier::Sovereign, false, false},
        {"sh1_sovereign_streak_20", "Clockwork Soul", "Land 20 consecutive Judgment Ring hits without a miss.", Tier::Sovereign, false, false},
        {"sh1_sovereign_streak_50", "Metronome of Destiny", "Land 50 consecutive Judgment Ring hits without a miss.", Tier::Sovereign, false, false},
        {"sh1_sovereign_accuracy_90", "Surgical Malice", "Complete 150+ battles while maintaining at least 90% Ring accuracy.", Tier::Sovereign, false, false},
        {"sh1_sovereign_lv75", "Vessel of Seraphic Radiance", "Reach Level 75 with any party member.", Tier::Sovereign, false, false},
        {"sh1_sovereign_party_99", "Pantheon of 1913", "Raise all 6 party members to Level 99.", Tier::Sovereign, false, false},
        {"sh1_sovereign_cash_1m", "Baron of the Shadow Market", "Hold 1,000,000 Cash in your purse.", Tier::Sovereign, false, false},

        // Tier 5: Capstone (1)
        {"sh1_shadow_hearts_sealed", "Harmonixer of Destiny", "Unlock every single Mark, Sigil, Crest, Sovereign, Folly, and Curiosity in Shadow Hearts.", Tier::Covenant, false, false},

        // Follies — Fun, Quirky & Blunders (10)
        {"sh1_folly_butterfingers", "Butterfingers on the Ring", "Miss 3 consecutive Judgment Ring inputs in a row.", Tier::Folly, false, false},
        {"sh1_folly_runaway", "Tactical Retreat", "Escape from 10 battles.", Tier::Folly, false, false},
        {"sh1_folly_berserk", "Sanity Snapped", "Witness 5 Berserk or Unconscious incidents in battle.", Tier::Folly, false, false},
        {"sh1_folly_haggler", "Hard Bargain", "Successfully raise a merchant's buying price via the Shop Ring.", Tier::Folly, false, false},
        {"sh1_folly_big_spend", "Lavish Taste", "Spend 25,000+ Cash in a single purchase.", Tier::Folly, false, false},
        {"sh1_folly_panic_win", "Panic Button", "Botch a Judgment Ring input mid-battle and still finish the fight.", Tier::Folly, false, false},
        {"sh1_folly_perfect_bout", "Flawless Showboat", "Finish 5 battles where every Judgment Ring input hit the Perfect Zone.", Tier::Folly, false, false},
        {"sh1_folly_overkill", "Overkill Theater", "Land a 10+ hit Ring streak after reaching Level 25.", Tier::Folly, false, false},
        {"sh1_folly_hoarder", "Dragon's Purse", "Hoard 500,000 Cash at once without spending it.", Tier::Folly, false, false},
        {"sh1_folly_marathon", "Midnight Express", "Log 30+ hours of playtime on an active save file.", Tier::Folly, false, false},

        // Curiosities — Secret Until Unlocked: 1913 Story Milestones & Hidden Dungeons (15)
        {"sh1_curiosity_train", "Trans-Siberian Nightmare", "Begin the fateful journey aboard the Trans-Siberian Express.", Tier::Curiosity, true, false},
        {"sh1_curiosity_zhaoyang", "Village of Shadows", "Arrive in the eerie mountain hamlet of Zhaoyang Village.", Tier::Curiosity, true, false},
        {"sh1_curiosity_fengtian", "Streets of Fengtian", "Reach the bustling city of Fengtian.", Tier::Curiosity, true, false},
        {"sh1_curiosity_dalian", "Harbor of Dalian", "Reach the coastal port of Dalian.", Tier::Curiosity, true, false},
        {"sh1_curiosity_shanghai", "Pearl of the Orient", "Arrive in 1913 Shanghai.", Tier::Curiosity, true, false},
        {"sh1_curiosity_temple_ruins", "Secrets of Temple Ruins", "Venture into the ancient Temple Ruins.", Tier::Curiosity, true, false},
        {"sh1_curiosity_kuihai", "Tower of Kuihai", "Ascend Kuihai Tower for the climax of the Asia chapter.", Tier::Curiosity, true, false},
        {"sh1_curiosity_london", "Crossing to the Old World", "Set foot in foggy 1913 London as the Europe chapter begins.", Tier::Curiosity, true, false},
        {"sh1_curiosity_rouen", "The Bells of Rouen", "Arrive in the historic French town of Rouen.", Tier::Curiosity, true, false},
        {"sh1_curiosity_prague", "Nocturne in Prague", "Journey east into the spires of Prague.", Tier::Curiosity, true, false},
        {"sh1_curiosity_blue_castle", "Castle of the Vampire", "Enter Keith's ancestral home, Blue Castle.", Tier::Curiosity, true, false},
        {"sh1_curiosity_calios", "Asylum of Whispers", "Brave the dark halls of Calios Mental Hospital.", Tier::Curiosity, true, false},
        {"sh1_curiosity_nemeton", "Sanctum of Nemeton", "Reach the cursed ruins of Nemeton Monastery.", Tier::Curiosity, true, false},
        {"sh1_curiosity_kowloon", "Walled City Fortress", "Infiltrate the labyrinthine Kowloon Fortress.", Tier::Curiosity, true, false},
        {"sh1_curiosity_secret_dungeons", "Trials of the Hidden Depths", "Discover one of the optional endgame trials: Ancient Ruins, Cave Temple, or the Dollhouse.", Tier::Curiosity, true, false},
    };
    return kItems;
}

inline const std::vector<CatalogItem> &catalog_items_for(CatalogGame game) {
    return (game == CatalogGame::ShadowHearts1) ? sh1_catalog_items() : catalog_items();
}

}  // namespace achievements
