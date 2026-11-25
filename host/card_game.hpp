#pragma once

// Shadow Hearts: Nocturne — "Judgment Ring Duel" Custom Card Game Engine (v2 Tactical Edition)
// Original Shadow Hearts-native ruleset:
//   - 5-Node Circular Altar Ring (Nodes 0..4 clockwise, adjacent modulo 5)
//   - 20 Sanity HP, Growing Soul Energy (1 -> 6 per round), 20-card decks
//   - Clockwise Judgment Combo Chain: consecutive Node wins during the sweep
//     grant +1 Chain Power to the next clockwise Node & +1 bonus Sanity damage!
//   - Elemental Counter System: Fire > Wind > Earth > Water > Fire (+1 Clash Power);
//     Light <-> Dark (Volatile: +1 bonus Sanity damage on win)
//   - Universal Ring Harmony: adjacent friendly cards of the same Element gain +1 Power
//     (Resonance ability amplifies this to +3 Power)
//   - Harmonixer Fusion: play a strictly higher-cost card over an ally to discount its
//     cost and inherit +1 Power (+2 for Harmonixer ability)
//   - Channel Card (Anti-Brick): once per round, discard 1 card from hand for +1 Soul Energy & +1 Sanity
//   - Alternating Initiative: Odd rounds AI deploys first (you counter-deploy); Even rounds you lead
//   - Berserk Surge: at <= 6 Sanity, all your units gain +1 Power (Shadow Hearts Sanity tension!)
//   - Optional Live Judgment Ring Strike toggle (+1 Hit / +2 Red Strike)

#include "card_album.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace card_game {

constexpr int kNodeCount = 5;
constexpr int kDeckSize = 20;
constexpr int kMaxCopiesPerCard = 2;
constexpr int kMaxLegendaries = 2;
constexpr int kMaxExotics = 3;
constexpr int kStartingSanity = 20;
constexpr int kBerserkThreshold = 6;
constexpr int kMaxSoulEnergy = 6;
constexpr int kStartingHandSize = 4;
constexpr int kMaxHandSize = 7;

// Elemental Advantage: Fire > Wind > Earth > Water > Fire (+1 Clash Power)
inline bool element_beats(cards::Element atk, cards::Element def) noexcept {
    return (atk == cards::Element::Fire  && def == cards::Element::Wind)  ||
           (atk == cards::Element::Wind  && def == cards::Element::Earth) ||
           (atk == cards::Element::Earth && def == cards::Element::Water) ||
           (atk == cards::Element::Water && def == cards::Element::Fire);
}

// Volatile Opposites: Light <-> Dark (+1 bonus Sanity damage on clash win)
inline bool element_volatile(cards::Element a, cards::Element b) noexcept {
    return (a == cards::Element::Light && b == cards::Element::Dark) ||
           (a == cards::Element::Dark  && b == cards::Element::Light);
}

enum class AiDifficulty : int {
    Easy = 0,    // Alias the Peddler
    Normal = 1,  // Roger Bacon
    Hard = 2     // Albert Simon
};

enum class ArenaMod : int {
    None = 0,               // Alias: Balanced Altar
    NemetonCandlelight = 1, // Koudelka: Light & Dark cards gain +1 Power
    EmigreLeylines = 2,     // Roger Bacon: Ring Harmony grants +2 Power (+4 Resonance)
    PapillonFusion = 3,     // Joachim: Harmonixer Fusion grants +1 extra Power
    NeametoSurge = 4,       // Albert Simon: Both players start at 2 Soul Energy on Round 1
    GateOfMalice = 5        // Umbral Sovereign: Combo Chains deal +2 bonus Sanity & Berserk triggers at <= 8 SP
};

struct OpponentProfile {
    std::string id;
    std::string name;
    std::string title;
    AiDifficulty difficulty = AiDifficulty::Easy;
    int reward_essence = 40;
    cards::SeriesGame reward_game = cards::SeriesGame::Koudelka;
    int reward_luck_tier = 0;
    ArenaMod arena_mod = ArenaMod::None;
    std::string arena_name;
    std::string arena_desc;
    std::vector<std::string> signature_drops;
    std::vector<std::string> deck_ids;
};

inline const std::vector<OpponentProfile> &opponents() {
    static const std::vector<OpponentProfile> kOpps = {
        {
            "alias", "Alias", "I. Wandering Peddler (Easy)", AiDifficulty::Easy, 40,
            cards::SeriesGame::Koudelka, 0,
            ArenaMod::None, "Nemeton Curiosity Shop", "Standard Altar — Pure fundamentals, no arena twist.",
            {"kdl_alias", "kdl_gremlin", "kdl_sorrow", "kdl_mimic", "kdl_bone_knight"},
            {"kdl_alias", "kdl_alias", "kdl_gremlin", "kdl_gremlin", "kdl_sorrow", "kdl_sorrow",
             "kdl_ogden", "kdl_ogden", "kdl_bessy", "kdl_bessy", "kdl_mimic", "kdl_mimic",
             "kdl_bone_knight", "kdl_bone_knight", "kdl_banshee", "kdl_cursed_armor",
             "kdl_charlotte", "kdl_gargoyle", "sh1_ghoul", "sh1_bakeneko"}
        },
        {
            "koudelka", "Koudelka", "II. Clairvoyant Medium (Easy+)", AiDifficulty::Easy, 55,
            cards::SeriesGame::Koudelka, 1,
            ArenaMod::NemetonCandlelight, "Nemeton Candlelight", "Light & Dark cards gain +1 Power on every Pillar.",
            {"kdl_koudelka", "kdl_edward", "kdl_james", "kdl_banshee", "kdl_gargoyle", "kdl_elaine"},
            {"kdl_sorrow", "kdl_sorrow", "kdl_bessy", "kdl_ogden", "kdl_gremlin", "kdl_mimic",
             "kdl_banshee", "kdl_banshee", "kdl_cursed_armor", "kdl_charlotte", "kdl_patrick",
             "kdl_koudelka", "kdl_koudelka", "kdl_edward", "kdl_edward", "kdl_james",
             "kdl_elaine", "kdl_gargoyle", "kdl_plant_boss", "kdl_charlotte_boss"}
        },
        {
            "roger", "Roger Bacon", "III. Immortal Alchemist (Normal)", AiDifficulty::Normal, 75,
            cards::SeriesGame::ShadowHearts1, 1,
            ArenaMod::EmigreLeylines, "Emigre Leylines", "Ring Harmony grants +2 Power (+4 for Resonance)!",
            {"kdl_roger", "sh1_roger", "sh1_yuri", "sh1_alice", "sh1_zhuzhen", "sh1_raging_tiger"},
            {"kdl_gremlin", "kdl_sorrow", "kdl_bessy", "kdl_banshee", "kdl_nightgaunt",
             "kdl_koudelka", "kdl_edward", "kdl_james", "kdl_roger", "kdl_elaine",
             "sh1_yuri", "sh1_alice", "sh1_zhuzhen", "sh1_halley", "sh1_roger",
             "sh1_raging_tiger", "sh1_death_emperor", "sh2_lucia", "sh2_blanca", "sh3_roger"}
        },
        {
            "joachim", "Joachim", "IV. Grand Papillon (Normal+)", AiDifficulty::Normal, 95,
            cards::SeriesGame::Covenant, 1,
            ArenaMod::PapillonFusion, "Grand Papillon Ring", "Harmonixer Fusion grants +1 extra Power (+2 / +3)!",
            {"sh2_joachim", "sh2_yuri", "sh2_karin", "sh2_blanca", "sh2_kurando", "sh2_neo_amon"},
            {"sh2_domremy_soldier", "sh2_gargoyle_hound", "sh2_manticore", "sh2_man_eater",
             "sh2_shadow_knight", "sh2_shadow_knight", "sh2_yuri", "sh2_karin", "sh2_gepetto",
             "sh2_blanca", "sh2_joachim", "sh2_joachim", "sh2_lucia", "sh2_anastasia",
             "sh2_kurando", "sh2_veronica", "sh2_rasputin", "sh2_neo_amon", "sh2_jeanne", "sh2_amon"}
        },
        {
            "simon", "Albert Simon", "V. The False Prophet (Hard)", AiDifficulty::Hard, 120,
            cards::SeriesGame::ShadowHearts1, 2,
            ArenaMod::NeametoSurge, "Neameto Cataclysm", "Both players start Round 1 with 2 Soul Energy!",
            {"sh1_albert_simon", "sh1_atman", "sh1_czernobog", "sh1_meta_god", "sh2_asmodeus"},
            {"sh1_ghoul", "sh1_qinggu", "sh2_gargoyle_hound", "sh3_alcatraz_guard",
             "sh1_malice_puppet", "sh2_shadow_knight", "sh3_blood_golem", "kdl_cursed_armor",
             "sh1_yuri", "sh2_yuri", "sh2_kurando", "sh1_dehuai", "sh2_rasputin",
             "kdl_elaine_aberration", "sh1_atman", "sh1_albert_simon",
             "sh2_asmodeus", "sh2_kato", "sh1_meta_god", "sh2_umbral_god"}
        },
        {
            "sovereign", "Umbral Sovereign", "VI. Gate Grandmaster (Master)", AiDifficulty::Hard, 160,
            cards::SeriesGame::FromTheNewWorld, 2,
            ArenaMod::GateOfMalice, "Gate of Malice", "Chains deal +2 bonus Sanity & Berserk triggers at <= 8 SP!",
            {"kdl_elaine_final", "sh1_seraphic_radiance", "sh2_dark_seraphim", "sh3_awaker", "sh3_lady", "sh3_malice_goddess"},
            {"sh3_automaton", "sh3_alcatraz_guard", "sh3_desert_scorpion", "sh3_voodoo_doll",
             "sh3_blood_golem", "sh3_johnny", "sh3_shania", "sh3_natan", "sh3_ricardo", "sh3_hilda",
             "sh3_killer_boss", "sh3_tatanenka", "sh3_tirawa", "sh3_gilbert_boss",
             "sh2_kato", "kdl_patrick_boss", "sh3_awaker", "sh3_lady", "sh2_susanoo", "sh3_malice_goddess"}
        }
    };
    return kOpps;
}

inline std::vector<std::string> trial_deck_ids() {
    return {
        "kdl_alias", "kdl_gremlin", "kdl_sorrow", "kdl_nemeton_ward",
        "kdl_bessy", "kdl_soul_drain_spell", "kdl_bone_knight", "sh1_bakeneko",
        "sh2_ring_shift", "sh2_harmonixer_surge",
        "kdl_banshee", "kdl_cursed_armor", "sh1_judgment_reversal", "sh1_raging_tiger",
        "sh1_purge_malice", "kdl_emigre_ritual",
        "kdl_koudelka", "kdl_edward", "sh1_yuri", "sh2_karin"
    };
}

struct DeckValidation {
    bool valid = false;
    int count = 0;
    int legendary_count = 0;
    int exotic_count = 0;
    std::string error;
};

inline DeckValidation validate_deck(const std::vector<std::string> &deck,
                                    const cards::Collection *col = nullptr) {
    DeckValidation v;
    v.count = static_cast<int>(deck.size());
    std::unordered_map<std::string, int> freq;
    for (const auto &cid : deck) {
        const cards::CardDef *def = cards::find_card_def(cid);
        if (!def) {
            v.error = "Unknown card in deck: " + cid;
            return v;
        }
        const int c = ++freq[cid];
        if (c > kMaxCopiesPerCard) {
            v.error = "Max " + std::to_string(kMaxCopiesPerCard) + " copies of " + def->name + ".";
            return v;
        }
        if (col) {
            const auto *oc = col->find_owned(cid);
            if (!oc || oc->total_copies() < c) {
                v.error = "Not enough owned copies of " + def->name + ".";
                return v;
            }
        }
        if (def->rarity == cards::Rarity::Legendary) ++v.legendary_count;
        if (def->rarity == cards::Rarity::Exotic) ++v.exotic_count;
    }
    if (v.legendary_count > kMaxLegendaries) {
        v.error = "Max " + std::to_string(kMaxLegendaries) + " Legendary cards per deck.";
        return v;
    }
    if (v.exotic_count > kMaxExotics) {
        v.error = "Max " + std::to_string(kMaxExotics) + " Exotic cards per deck.";
        return v;
    }
    if (v.count != kDeckSize) {
        v.error = "Deck needs exactly " + std::to_string(kDeckSize) + " cards (" + std::to_string(v.count) + "/20).";
        return v;
    }
    v.valid = true;
    return v;
}

inline std::vector<std::string> build_auto_deck(const cards::Collection &col) {
    std::vector<const cards::CardDef *> owned_defs;
    for (const auto &c : cards::catalog()) {
        const auto *oc = col.find_owned(c.id);
        if (!oc || !oc->unlocked()) continue;
        owned_defs.push_back(&c);
    }
    std::sort(owned_defs.begin(), owned_defs.end(), [](const cards::CardDef *a, const cards::CardDef *b) {
        if (a->cost != b->cost) return a->cost < b->cost;
        return a->power > b->power;
    });
    std::vector<std::string> deck;
    int leg = 0, exo = 0;
    for (int pass = 0; pass < 2 && static_cast<int>(deck.size()) < kDeckSize; ++pass) {
        for (const auto *def : owned_defs) {
            if (static_cast<int>(deck.size()) >= kDeckSize) break;
            const auto *oc = col.find_owned(def->id);
            if (!oc || oc->total_copies() <= pass) continue;
            if (def->rarity == cards::Rarity::Legendary && leg >= kMaxLegendaries) continue;
            if (def->rarity == cards::Rarity::Exotic && exo >= kMaxExotics) continue;
            deck.push_back(def->id);
            if (def->rarity == cards::Rarity::Legendary) ++leg;
            if (def->rarity == cards::Rarity::Exotic) ++exo;
        }
    }
    return deck;
}

struct BoardUnit {
    bool occupied = false;
    std::string card_id;
    int base_power = 0;
    int cost = 0;
    int fusion_bonus = 0;
    int erosion = 0;     // Permanent battle wear-and-tear (-1 per hard clash / Mistletoe / Breaker)
    int round_buff = 0;  // Temporary round buff/debuff (cleared after sweep)
    bool ward_active = false;
    bool is_boss = false;
    cards::Element element = cards::Element::Dark;
    cards::AbilityId ability = cards::AbilityId::None;
};

struct AltarNode {
    BoardUnit slots[2];     // 0 = Player, 1 = AI
    int seal_owner = -1;    // -1 = Neutral, 0 = Player, 1 = AI
};

struct PlayerState {
    int sanity = kStartingSanity;
    int soul_energy = 1;
    int max_soul_energy = 1;
    int next_play_discount = 0;  // Awaker's Vision spell discount
    int consec_majority_sweeps = 0;
    bool channeled_this_round = false;
    std::vector<std::string> deck;
    std::vector<std::string> hand;
    std::vector<std::string> graveyard;

    bool is_berserk() const noexcept {
        return sanity > 0 && sanity <= kBerserkThreshold;
    }
};

struct PlayPreview {
    bool legal = false;
    bool is_spell = false;
    int eff_cost = 0;
    bool is_fusion = false;
    int fusion_bonus = 0;
    bool elem_advantage = false;
    bool elem_volatile = false;
    bool harmony_active = false;
    int projected_power = 0;
    int enemy_power = 0;
    int projected_margin = 0;  // >0 win, ==0 tie, <0 lose
};

enum class DuelPhase : int {
    PlayerDeploy = 0,
    LiveRing = 1,
    Sweeping = 2,
    Finished = 3,
    Mulligan = 4
};

struct DuelVfxEvent {
    int node_idx = -1;       // 0..4 or -1 for center
    int player_idx = -1;     // 0 or 1 when tied to a specific card slot
    std::string text;        // Floating combat text (e.g. "-4 SP!", "FUSED +2P!", "CHAIN x3!")
    std::uint32_t rgb = 0xFFD060u;
    float shake = 0.0f;      // 0..1 screen shake intensity
    bool shockwave = true;
    bool is_summon = false;  // Triggers physical Summon Slam drop-in animation
    int sfx_kind = 0;        // 1=CardSlam, 2=CardFuse, 3=ClashHit, 4=ChainBell, 5=WardBlock
};

struct PowerBreakdown {
    int base = 0;
    int fusion = 0;
    int harmony = 0;
    int elem_adv = 0;
    int covenant = 0;
    int berserk = 0;
    int arena = 0;
    int round_buff = 0;
    int live_ring = 0;
    int total = 0;
};

class DuelMatch {
public:
    AltarNode nodes[kNodeCount]{};
    PlayerState players[2]{};
    int round_num = 1;
    bool ai_deployed_first = false;  // Odd rounds: AI deploys first so Player counter-deploys!
    DuelPhase phase = DuelPhase::PlayerDeploy;
    int winner = -1;  // -1 = ongoing, 0 = Player, 1 = AI, 2 = Draw
    std::string win_reason;
    bool using_trial_deck = true;
    bool live_ring_enabled = false;
    int live_ring_bonus = 0;
    float live_ring_angle = 0.0f;  // 0..360 deg
    int sweep_cursor = 0;          // 0..4 step index during animated sweep
    bool reverse_sweep_this_round = false;  // Judgment Reversal: sweeps 4->3->2->1->0 CCW!
    float sweep_timer = 0.0f;
    int chain_owner = -1;          // Current clockwise Combo Chain owner during sweep
    int chain_streak = 0;          // Consecutive wins in current sweep (1..5)
    int opponent_idx = 0;
    bool reward_claimed = false;
    std::array<bool, kStartingHandSize> mulligan_marks{};
    std::vector<std::string> log;
    std::vector<DuelVfxEvent> pending_vfx;
    std::mt19937_64 rng{0x53484E4F43545552ull};

    static int left_node(int n) noexcept { return (n + kNodeCount - 1) % kNodeCount; }
    static int right_node(int n) noexcept { return (n + 1) % kNodeCount; }

    void start(const std::vector<std::string> &player_deck, int opp_index,
               bool is_trial, bool enable_live_ring, std::uint64_t seed = 0,
               bool skip_mulligan = false) {
        for (int i = 0; i < kNodeCount; ++i) nodes[i] = AltarNode{};
        players[0] = PlayerState{};
        players[1] = PlayerState{};
        round_num = 1;
        ai_deployed_first = false;
        phase = skip_mulligan ? DuelPhase::PlayerDeploy : DuelPhase::Mulligan;
        mulligan_marks.fill(false);
        winner = -1;
        win_reason.clear();
        using_trial_deck = is_trial;
        live_ring_enabled = enable_live_ring;
        live_ring_bonus = 0;
        live_ring_angle = 0.0f;
        sweep_cursor = 0;
        reverse_sweep_this_round = false;
        sweep_timer = 0.0f;
        chain_owner = -1;
        chain_streak = 0;
        reward_claimed = false;
        log.clear();

        if (seed == 0) {
            seed = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ull;
        }
        rng.seed(seed);

        const auto &opps = opponents();
        opponent_idx = std::clamp(opp_index, 0, static_cast<int>(opps.size()) - 1);
        players[0].deck = player_deck.empty() ? trial_deck_ids() : player_deck;
        players[1].deck = opps[static_cast<std::size_t>(opponent_idx)].deck_ids;

        // Arena Modifier: Neameto Cataclysm starts both duelists at 2 Soul Energy on Round 1!
        if (opps[static_cast<std::size_t>(opponent_idx)].arena_mod == ArenaMod::NeametoSurge) {
            players[0].max_soul_energy = 2;
            players[0].soul_energy = 2;
            players[1].max_soul_energy = 2;
            players[1].soul_energy = 2;
        }

        std::shuffle(players[0].deck.begin(), players[0].deck.end(), rng);
        std::shuffle(players[1].deck.begin(), players[1].deck.end(), rng);

        for (int i = 0; i < kStartingHandSize; ++i) {
            draw_card(0);
            draw_card(1);
        }
        push_log("Judgment Ring Duel vs " + opps[static_cast<std::size_t>(opponent_idx)].name +
                 " [" + opps[static_cast<std::size_t>(opponent_idx)].arena_name + "]");
        if (skip_mulligan) {
            begin_round_initiative();
        }
    }

    void toggle_mulligan_mark(int idx) noexcept {
        if (phase != DuelPhase::Mulligan) return;
        if (idx >= 0 && idx < static_cast<int>(players[0].hand.size()) && idx < kStartingHandSize) {
            mulligan_marks[static_cast<std::size_t>(idx)] = !mulligan_marks[static_cast<std::size_t>(idx)];
        }
    }

    int mulligan_mark_count() const noexcept {
        int n = 0;
        for (bool m : mulligan_marks) if (m) ++n;
        return n;
    }

    void confirm_mulligan() {
        if (phase != DuelPhase::Mulligan) return;
        std::vector<std::string> returned;
        for (int i = static_cast<int>(players[0].hand.size()) - 1; i >= 0; --i) {
            if (i < kStartingHandSize && mulligan_marks[static_cast<std::size_t>(i)]) {
                returned.push_back(players[0].hand[static_cast<std::size_t>(i)]);
                players[0].hand.erase(players[0].hand.begin() + i);
            }
        }
        if (!returned.empty()) {
            for (const auto &cid : returned) players[0].deck.push_back(cid);
            std::shuffle(players[0].deck.begin(), players[0].deck.end(), rng);
            for (std::size_t k = 0; k < returned.size(); ++k) draw_card(0);
            push_log("Mulligan: redrew " + std::to_string(returned.size()) + " card(s).");
        }
        mulligan_marks.fill(false);
        phase = DuelPhase::PlayerDeploy;
        begin_round_initiative();
    }

    ArenaMod active_arena_mod() const noexcept {
        return opponents()[static_cast<std::size_t>(opponent_idx)].arena_mod;
    }

    bool player_is_berserk(int p) const noexcept {
        const int thresh = (active_arena_mod() == ArenaMod::GateOfMalice) ? 8 : kBerserkThreshold;
        return players[p].sanity > 0 && players[p].sanity <= thresh;
    }

    int count_seals(int p) const noexcept {
        int n = 0;
        for (int i = 0; i < kNodeCount; ++i) {
            if (nodes[i].seal_owner == p) ++n;
        }
        return n;
    }

    bool has_ring_harmony(int node_idx, int p) const noexcept {
        const BoardUnit &u = nodes[node_idx].slots[p];
        if (!u.occupied) return false;
        const BoardUnit &lu = nodes[left_node(node_idx)].slots[p];
        const BoardUnit &ru = nodes[right_node(node_idx)].slots[p];
        return (lu.occupied && lu.element == u.element) ||
               (ru.occupied && ru.element == u.element);
    }

    PowerBreakdown effective_power_breakdown(int node_idx, int p, int chain_bonus = 0) const noexcept {
        PowerBreakdown pb;
        const BoardUnit &u = nodes[node_idx].slots[p];
        if (!u.occupied) return pb;
        pb.base = u.base_power;
        pb.fusion = u.fusion_bonus;
        pb.round_buff = u.round_buff - u.erosion;
        if (u.ability == cards::AbilityId::Necromancy) {
            pb.covenant += std::min(3, static_cast<int>(players[p].graveyard.size()) / 2);
        }
        if (has_ring_harmony(node_idx, p)) {
            const bool leyline = (active_arena_mod() == ArenaMod::EmigreLeylines);
            pb.harmony = (u.ability == cards::AbilityId::Resonance) ? (leyline ? 4 : 3) : (leyline ? 2 : 1);
        }
        const BoardUnit &opp = nodes[node_idx].slots[1 - p];
        if (opp.occupied && element_beats(u.element, opp.element)) {
            pb.elem_adv = 1;
        }
        if (u.ability == cards::AbilityId::Covenant) {
            pb.covenant = count_seals(p);
        }
        if (player_is_berserk(p)) {
            pb.berserk = 1;
        }
        if (active_arena_mod() == ArenaMod::NemetonCandlelight &&
            (u.element == cards::Element::Light || u.element == cards::Element::Dark)) {
            pb.arena = 1;
        }
        if (p == 0 && live_ring_bonus > 0) {
            int best_node = -1, best_bp = -1;
            for (int i = 0; i < kNodeCount; ++i) {
                if (nodes[i].slots[0].occupied && nodes[i].slots[0].base_power > best_bp) {
                    best_bp = nodes[i].slots[0].base_power;
                    best_node = i;
                }
            }
            if (node_idx == best_node) pb.live_ring = live_ring_bonus;
        }
        pb.total = std::max(0, pb.base + pb.fusion + pb.harmony + pb.elem_adv +
                               pb.covenant + pb.berserk + pb.arena + pb.round_buff + pb.live_ring + chain_bonus);
        return pb;
    }

    // Effective power on node_idx for player p (includes Fusion, Harmony/Resonance,
    // Elemental Advantage, Covenant, Berserk Surge, Live Ring Strike, and active Chain bonus)
    int effective_power(int node_idx, int p, int chain_bonus = 0) const noexcept {
        return effective_power_breakdown(node_idx, p, chain_bonus).total;
    }

    // Anti-Brick Mechanic: once per round, discard 1 card from hand for +1 Soul Energy & +1 Sanity
    bool can_channel_card(int p, int hand_idx) const noexcept {
        if (phase != DuelPhase::PlayerDeploy || p < 0 || p > 1) return false;
        if (players[p].channeled_this_round) return false;
        return hand_idx >= 0 && hand_idx < static_cast<int>(players[p].hand.size());
    }

    bool channel_card(int p, int hand_idx) {
        if (!can_channel_card(p, hand_idx)) return false;
        const std::string cid = players[p].hand[static_cast<std::size_t>(hand_idx)];
        const cards::CardDef *def = cards::find_card_def(cid);
        players[p].hand.erase(players[p].hand.begin() + hand_idx);
        players[p].graveyard.push_back(cid);
        players[p].channeled_this_round = true;
        players[p].soul_energy += 1;
        players[p].sanity = std::min(kStartingSanity, players[p].sanity + 1);
        push_log(std::string(p == 0 ? "You" : "Opponent") + " channeled " +
                 (def ? def->name : cid) + " (+1 Soul Energy, +1 Sanity)!");
        return true;
    }

    bool can_play_card(int p, int hand_idx, int node_idx, int *eff_cost_out = nullptr,
                       bool *is_fusion_out = nullptr) const {
        if (p < 0 || p > 1 || node_idx < 0 || node_idx >= kNodeCount) return false;
        if (hand_idx < 0 || hand_idx >= static_cast<int>(players[p].hand.size())) return false;
        const cards::CardDef *def = cards::find_card_def(players[p].hand[static_cast<std::size_t>(hand_idx)]);
        if (!def) return false;
        const int disc = players[p].next_play_discount;
        if (def->is_spell()) {
            const int eff_cost = std::max(0, def->cost - disc);
            if (eff_cost_out) *eff_cost_out = eff_cost;
            if (is_fusion_out) *is_fusion_out = false;
            if (players[p].soul_energy < eff_cost) return false;
            // Validate target requirements for targeted Crest Spells
            switch (def->ability) {
            case cards::AbilityId::SpellRevive: {
                if (nodes[node_idx].slots[p].occupied) return false;
                for (const auto &gid : players[p].graveyard) {
                    if (const auto *gd = cards::find_card_def(gid); gd && !gd->is_spell()) return true;
                }
                return false;
            }
            case cards::AbilityId::SpellWardBuff:
            case cards::AbilityId::SpellRingShift:
            case cards::AbilityId::SpellSealLock:
                return nodes[node_idx].slots[p].occupied;
            case cards::AbilityId::SpellPurge:
            case cards::AbilityId::SpellMistletoe:
                return nodes[node_idx].slots[1 - p].occupied;
            default:
                return true;  // Global spells can be cast on any Pillar
            }
        }
        const BoardUnit &existing = nodes[node_idx].slots[p];
        bool fusion = false;
        int eff_cost = def->cost;
        if (existing.occupied) {
            if (def->cost <= existing.cost) return false;
            fusion = true;
            eff_cost = std::max(1, def->cost - existing.cost);
        }
        eff_cost = std::max(0, eff_cost - disc);
        if (eff_cost_out) *eff_cost_out = eff_cost;
        if (is_fusion_out) *is_fusion_out = fusion;
        return players[p].soul_energy >= eff_cost;
    }

    // Live tactical preview for UI hover/selection
    PlayPreview preview_play(int hand_idx, int node_idx) const {
        PlayPreview pv;
        if (!can_play_card(0, hand_idx, node_idx, &pv.eff_cost, &pv.is_fusion)) return pv;
        const cards::CardDef *def = cards::find_card_def(players[0].hand[static_cast<std::size_t>(hand_idx)]);
        if (!def) return pv;
        pv.legal = true;
        pv.is_spell = def->is_spell();
        if (pv.is_spell) return pv;
        pv.fusion_bonus = pv.is_fusion ? ((def->ability == cards::AbilityId::Harmonixer) ? 2 : 1) : 0;
        if (pv.is_fusion && active_arena_mod() == ArenaMod::PapillonFusion) pv.fusion_bonus += 1;

        int pwr = def->power + pv.fusion_bonus;
        const BoardUnit &lu = nodes[left_node(node_idx)].slots[0];
        const BoardUnit &ru = nodes[right_node(node_idx)].slots[0];
        if ((lu.occupied && lu.element == def->element) || (ru.occupied && ru.element == def->element)) {
            pv.harmony_active = true;
            const bool leyline = (active_arena_mod() == ArenaMod::EmigreLeylines);
            pwr += (def->ability == cards::AbilityId::Resonance) ? (leyline ? 4 : 3) : (leyline ? 2 : 1);
        }
        const BoardUnit &opp = nodes[node_idx].slots[1];
        if (opp.occupied) {
            if (element_beats(def->element, opp.element)) {
                pv.elem_advantage = true;
                pwr += 1;
            }
            pv.elem_volatile = element_volatile(def->element, opp.element);
            pv.enemy_power = effective_power(node_idx, 1);
            if (def->ability == cards::AbilityId::Eclipse) {
                pv.enemy_power = std::max(0, pv.enemy_power - 2);
            } else if (def->ability == cards::AbilityId::Breaker) {
                pv.enemy_power = std::max(0, pv.enemy_power - 1);
            }
        }
        if (def->ability == cards::AbilityId::Covenant) pwr += count_seals(0);
        if (def->ability == cards::AbilityId::Necromancy) pwr += std::min(3, static_cast<int>(players[0].graveyard.size()) / 2);
        if (player_is_berserk(0)) pwr += 1;
        if (active_arena_mod() == ArenaMod::NemetonCandlelight &&
            (def->element == cards::Element::Light || def->element == cards::Element::Dark)) {
            pwr += 1;
        }
        pv.projected_power = pwr;
        pv.projected_margin = pwr - pv.enemy_power;
        return pv;
    }

    bool play_card(int p, int hand_idx, int node_idx) {
        int eff_cost = 0;
        bool fusion = false;
        if (!can_play_card(p, hand_idx, node_idx, &eff_cost, &fusion)) return false;

        const std::string cid = players[p].hand[static_cast<std::size_t>(hand_idx)];
        const cards::CardDef *def = cards::find_card_def(cid);
        if (!def) return false;

        players[p].soul_energy -= eff_cost;
        players[p].next_play_discount = 0;
        players[p].hand.erase(players[p].hand.begin() + hand_idx);
        const std::string who = (p == 0) ? "You" : opponents()[static_cast<std::size_t>(opponent_idx)].name;
        const int opp = 1 - p;

        // Crest Spell Resolution (goes straight to Graveyard after firing!)
        if (def->is_spell()) {
            players[p].graveyard.push_back(cid);
            push_log(who + " cast CREST [" + def->name + "]!");
            switch (def->ability) {
            case cards::AbilityId::SpellRevive: {
                int best_idx = -1, best_pwr = -1;
                for (int gi = 0; gi < static_cast<int>(players[p].graveyard.size()); ++gi) {
                    const auto *gd = cards::find_card_def(players[p].graveyard[static_cast<std::size_t>(gi)]);
                    if (gd && !gd->is_spell() && gd->power > best_pwr) { best_pwr = gd->power; best_idx = gi; }
                }
                if (best_idx >= 0) {
                    const std::string rid = players[p].graveyard[static_cast<std::size_t>(best_idx)];
                    players[p].graveyard.erase(players[p].graveyard.begin() + best_idx);
                    if (const auto *rd = cards::find_card_def(rid)) {
                        BoardUnit &sl = nodes[node_idx].slots[p];
                        sl = BoardUnit{};
                        sl.occupied = true;
                        sl.card_id = rd->id;
                        sl.base_power = rd->power;
                        sl.cost = rd->cost;
                        sl.ward_active = (rd->ability == cards::AbilityId::Ward || rd->is_boss);
                        sl.is_boss = rd->is_boss;
                        sl.element = rd->element;
                        sl.ability = rd->ability;
                        pending_vfx.push_back({node_idx, p, "REVIVED " + rd->name + "!", 0xE078FFu, 0.55f, true, true, 2});
                    }
                }
                break;
            }
            case cards::AbilityId::SpellWardBuff:
                nodes[node_idx].slots[p].ward_active = true;
                nodes[node_idx].slots[p].round_buff += 2;
                pending_vfx.push_back({node_idx, p, "WARD +2P!", 0x80E4FFu, 0.30f, true, false, 5});
                break;
            case cards::AbilityId::SpellBloodDraw:
                draw_card(p);
                draw_card(p);
                players[p].sanity = std::max(1, players[p].sanity - 2);
                pending_vfx.push_back({-1, p, "DRAW 2 (-2 SP)", 0xC868F8u, 0.25f, true, false, 2});
                break;
            case cards::AbilityId::SpellReverseCCW:
                reverse_sweep_this_round = !reverse_sweep_this_round;
                draw_card(p);
                pending_vfx.push_back({-1, p, "RING REVERSED CCW!", 0x60F0B8u, 0.45f, true, false, 4});
                break;
            case cards::AbilityId::SpellGraveBuff: {
                const int bonus = std::clamp(static_cast<int>(players[p].graveyard.size()) / 2, 1, 3);
                for (int ni = 0; ni < kNodeCount; ++ni) {
                    if (nodes[ni].slots[p].occupied) nodes[ni].slots[p].round_buff += bonus;
                }
                pending_vfx.push_back({-1, p, "GRAVEYARD +" + std::to_string(bonus) + "P ALL!", 0xB878FFu, 0.40f, true, false, 2});
                break;
            }
            case cards::AbilityId::SpellPurge:
                nodes[node_idx].slots[opp].ward_active = false;
                nodes[node_idx].slots[opp].round_buff -= 3;
                pending_vfx.push_back({node_idx, opp, "PURGED -3P!", 0xFFF090u, 0.45f, true, false, 5});
                break;
            case cards::AbilityId::SpellMistletoe:
                nodes[node_idx].slots[opp].ward_active = false;
                nodes[node_idx].slots[opp].erosion += 2;
                pending_vfx.push_back({node_idx, opp, "MISTLETOE -2 EROSION!", 0xF05880u, 0.55f, true, false, 3});
                break;
            case cards::AbilityId::SpellRingShift: {
                const int dst = right_node(node_idx);
                std::swap(nodes[node_idx].slots[p], nodes[dst].slots[p]);
                if (nodes[dst].slots[p].occupied) nodes[dst].slots[p].round_buff += 1;
                pending_vfx.push_back({dst, p, "RING SHIFT +1P!", 0x70E8B8u, 0.30f, true, true, 2});
                break;
            }
            case cards::AbilityId::SpellSoulSurge:
                players[p].soul_energy += 2;
                pending_vfx.push_back({-1, p, "+2 SOUL ENERGY!", 0x70C8FFu, 0.35f, true, false, 2});
                break;
            case cards::AbilityId::SpellSealLock:
                nodes[node_idx].seal_owner = p;
                players[p].sanity = std::min(kStartingSanity, players[p].sanity + 3);
                pending_vfx.push_back({node_idx, p, "SEAL LOCK +3 SP!", 0xF8D068u, 0.35f, true, false, 4});
                break;
            case cards::AbilityId::SpellMaliceNova:
                for (int ni = 0; ni < kNodeCount; ++ni) {
                    if (nodes[ni].slots[opp].occupied) nodes[ni].slots[opp].round_buff -= 2;
                }
                players[opp].sanity = std::max(0, players[opp].sanity - 2);
                pending_vfx.push_back({-1, opp, "MALICE NOVA -2P ALL!", 0xFF5868u, 0.65f, true, false, 3});
                break;
            case cards::AbilityId::SpellAwakerVis:
                draw_card(p);
                draw_card(p);
                players[p].next_play_discount += 1;
                pending_vfx.push_back({-1, p, "DRAW 2 & -1 COST!", 0x70D8FFu, 0.30f, true, false, 2});
                break;
            default: break;
            }
            check_victory();
            return true;
        }

        BoardUnit &slot = nodes[node_idx].slots[p];
        int inherited_bonus = 0;
        if (fusion) {
            players[p].graveyard.push_back(slot.card_id);
            inherited_bonus = (def->ability == cards::AbilityId::Harmonixer) ? 2 : 1;
            if (active_arena_mod() == ArenaMod::PapillonFusion) inherited_bonus += 1;
        }

        slot = BoardUnit{};
        slot.occupied = true;
        slot.card_id = def->id;
        slot.base_power = def->power;
        slot.cost = def->cost;
        slot.fusion_bonus = inherited_bonus;
        slot.ward_active = (def->ability == cards::AbilityId::Ward || def->is_boss);
        slot.is_boss = def->is_boss;
        slot.element = def->element;
        slot.ability = def->ability;

        push_log(who + (fusion ? " FUSED " : " played ") + def->name + " on Node " + std::to_string(node_idx + 1) +
                 (fusion ? (" (+" + std::to_string(inherited_bonus) + " Pwr)") : ""));
        pending_vfx.push_back({node_idx, p, fusion ? ("FUSED +" + std::to_string(inherited_bonus) + "P!") : def->name,
                               fusion ? 0xE078FFu : cards::element_color_rgb(def->element),
                               fusion ? 0.55f : 0.22f, true, true, fusion ? 2 : 1});

        if (def->ability == cards::AbilityId::Quickstep) {
            players[opp].sanity = std::max(0, players[opp].sanity - 1);
            push_log("  Quickstep: -1 Sanity to " + std::string(opp == 0 ? "You" : "Opponent") + "!");
        } else if (def->ability == cards::AbilityId::ArcaneEcho) {
            if (nodes[left_node(node_idx)].slots[p].occupied) nodes[left_node(node_idx)].slots[p].round_buff += 1;
            if (nodes[right_node(node_idx)].slots[p].occupied) nodes[right_node(node_idx)].slots[p].round_buff += 1;
        } else if (def->ability == cards::AbilityId::Eclipse) {
            if (nodes[node_idx].slots[opp].occupied) {
                nodes[node_idx].slots[opp].round_buff -= 2;
                push_log("  Eclipse: weakened opposing card on Node " + std::to_string(node_idx + 1) + " (-2 Pwr)!");
            }
        } else if (def->ability == cards::AbilityId::DrawCard) {
            draw_card(p);
            push_log("  Arcane Insight: drew 1 card!");
        } else if (def->ability == cards::AbilityId::Breaker) {
            if (nodes[node_idx].slots[opp].occupied) {
                nodes[node_idx].slots[opp].ward_active = false;
                nodes[node_idx].slots[opp].erosion += 1;
                push_log("  Ward Breaker: stripped Ward & inflicted -1 Erosion!");
            }
        } else if (def->ability == cards::AbilityId::RampSoul) {
            players[p].soul_energy += 1;
            push_log("  Leyline Tap: +1 Soul Energy this round!");
        }
        check_victory();
        return true;
    }

    void run_ai_deploy() {
        if (phase == DuelPhase::Finished) return;
        const AiDifficulty diff = opponents()[static_cast<std::size_t>(opponent_idx)].difficulty;
        int plays_left = (diff == AiDifficulty::Easy) ? 2 : (diff == AiDifficulty::Normal ? 3 : 4);

        // 1. Smart Channeling: if Normal/Hard AI has no playable card, channel highest-cost brick
        if (diff != AiDifficulty::Easy && !players[1].channeled_this_round && players[1].hand.size() >= 3u) {
            bool any_playable = false;
            for (int h = 0; h < static_cast<int>(players[1].hand.size()); ++h) {
                for (int n = 0; n < kNodeCount; ++n) {
                    if (can_play_card(1, h, n)) { any_playable = true; break; }
                }
                if (any_playable) break;
            }
            if (!any_playable) {
                int highest_idx = 0, highest_cost = -1;
                for (int h = 0; h < static_cast<int>(players[1].hand.size()); ++h) {
                    const auto *d = cards::find_card_def(players[1].hand[static_cast<std::size_t>(h)]);
                    if (d && d->cost > highest_cost) { highest_cost = d->cost; highest_idx = h; }
                }
                channel_card(1, highest_idx);
            }
        }

        // 2. Tactical Play Loop (understands Crest Spells, Seal Lethal, Chain-Breaking & Fusions!)
        while (plays_left-- > 0 && phase != DuelPhase::Finished) {
            const int player_seals = count_seals(0);
            const int ai_seals = count_seals(1);
            int best_h = -1, best_n = -1;
            int best_score = -1000;

            for (int h = 0; h < static_cast<int>(players[1].hand.size()); ++h) {
                const cards::CardDef *def = cards::find_card_def(players[1].hand[static_cast<std::size_t>(h)]);
                if (!def) continue;

                for (int n = 0; n < kNodeCount; ++n) {
                    int cost = 0;
                    bool fusion = false;
                    if (!can_play_card(1, h, n, &cost, &fusion)) continue;

                    int score = 0;
                    if (def->is_spell()) {
                        // Evaluate Crest Spells contextually instead of randomly!
                        switch (def->ability) {
                        case cards::AbilityId::SpellSoulSurge:
                            score = (players[1].hand.size() >= 3u) ? 58 : 10;
                            break;
                        case cards::AbilityId::SpellAwakerVis:
                            score = (players[1].hand.size() <= 5u) ? 56 : 18;
                            break;
                        case cards::AbilityId::SpellBloodDraw:
                            score = (players[1].sanity >= 10 && players[1].hand.size() <= 4u) ? 46 : -20;
                            break;
                        case cards::AbilityId::SpellRevive:
                            score = 64 + (nodes[n].slots[0].occupied ? 12 : 18);
                            break;
                        case cards::AbilityId::SpellPurge:
                        case cards::AbilityId::SpellMistletoe: {
                            const auto &eu = nodes[n].slots[0];
                            score = 26 + effective_power(n, 0) * 6 + (eu.ward_active ? 26 : 0) +
                                    (nodes[n].seal_owner == 0 ? 18 : 0);
                            break;
                        }
                        case cards::AbilityId::SpellMaliceNova: {
                            int enemy_cnt = 0;
                            for (int k = 0; k < kNodeCount; ++k) if (nodes[k].slots[0].occupied) ++enemy_cnt;
                            score = (enemy_cnt >= 2) ? (35 + enemy_cnt * 16) : -10;
                            break;
                        }
                        case cards::AbilityId::SpellWardBuff: {
                            const auto &myu = nodes[n].slots[1];
                            score = (nodes[n].slots[0].occupied && !myu.ward_active) ? 44 : 14;
                            break;
                        }
                        case cards::AbilityId::SpellSealLock:
                            score = (nodes[n].seal_owner != 1 ? 38 : 14) + (players[1].sanity <= 12 ? 22 : 6);
                            break;
                        case cards::AbilityId::SpellGraveBuff:
                            score = (players[1].graveyard.size() >= 4u) ? 52 : 8;
                            break;
                        case cards::AbilityId::SpellReverseCCW:
                            // Smart Reversal: fire if Player stacked Nodes 0+1 while AI holds Nodes 3 or 4!
                            score = (!reverse_sweep_this_round && nodes[0].slots[0].occupied && nodes[1].slots[0].occupied &&
                                     (nodes[4].slots[1].occupied || nodes[3].slots[1].occupied)) ? 60 : -15;
                            break;
                        case cards::AbilityId::SpellRingShift:
                            score = (!nodes[right_node(n)].slots[1].occupied && !nodes[right_node(n)].slots[0].occupied) ? 24 : -10;
                            break;
                        default:
                            score = 20;
                            break;
                        }
                    } else {
                        // Unit Deployment / Harmonixer Fusion tactical evaluation
                        score = def->power * 10 - cost * 2;
                        if (fusion) {
                            score += (def->ability == cards::AbilityId::Harmonixer) ? 24 : 10;
                            if (active_arena_mod() == ArenaMod::PapillonFusion) score += 10;
                        }
                        if (def->ability == cards::AbilityId::DrawCard || def->ability == cards::AbilityId::RampSoul) {
                            score += 18;
                        }
                        const BoardUnit &enemy = nodes[n].slots[0];
                        if (enemy.occupied) {
                            const int ep = effective_power(n, 0);
                            int myp = def->power + (fusion ? 1 : 0) + (element_beats(def->element, enemy.element) ? 1 : 0);
                            if (def->ability == cards::AbilityId::Eclipse) myp += 2;
                            if (def->ability == cards::AbilityId::Breaker && enemy.ward_active) score += 24;
                            if (myp > ep) {
                                score += 30;
                                // Emergency Seal Defense: stop Player from reaching 3-4 Seal victory!
                                if (nodes[n].seal_owner == 0 && player_seals >= 2) score += 42;
                                // Chain-Breaker: contest Node 1 or 2 when Player holds the previous clockwise Node!
                                if (diff != AiDifficulty::Easy && nodes[left_node(n)].slots[0].occupied) score += 26;
                            } else if (diff == AiDifficulty::Hard) {
                                // Only sacrifice a blocker if it inflicts Soul Erosion (within 2 Power)
                                score += (ep - myp <= 2 && enemy.base_power >= 6) ? 4 : -24;
                            }
                        } else {
                            // Open Pillar: prefer Seal Lethal push & clockwise Chain-Building!
                            score += 14 + (kNodeCount - n) * 2;
                            if (ai_seals >= 2 && nodes[n].seal_owner != 1) score += 34;
                            if (diff != AiDifficulty::Easy && nodes[left_node(n)].slots[1].occupied) score += 18;
                        }
                        // Reward Elemental Harmony with adjacent AI Pillars
                        const BoardUnit &lu = nodes[left_node(n)].slots[1];
                        const BoardUnit &ru = nodes[right_node(n)].slots[1];
                        if ((lu.occupied && lu.element == def->element) || (ru.occupied && ru.element == def->element)) {
                            score += (def->ability == cards::AbilityId::Resonance) ? 26 : 12;
                        }
                    }

                    if (diff == AiDifficulty::Easy) {
                        score += static_cast<int>(rng() % 22u);
                    }
                    if (score > best_score) {
                        best_score = score;
                        best_h = h;
                        best_n = n;
                    }
                }
            }
            if (best_h < 0 || best_score < 0) break;
            play_card(1, best_h, best_n);
        }
    }

    void end_player_deploy() {
        if (phase != DuelPhase::PlayerDeploy) return;
        // On even rounds, player deployed first, so AI responds now
        if (!ai_deployed_first) {
            run_ai_deploy();
            if (phase == DuelPhase::Finished) return;
        }
        live_ring_bonus = 0;
        chain_owner = -1;
        chain_streak = 0;
        reverse_sweep_this_round = false;
        players[0].next_play_discount = 0;
        players[1].next_play_discount = 0;
        if (live_ring_enabled) {
            live_ring_angle = 0.0f;
            phase = DuelPhase::LiveRing;
        } else {
            sweep_cursor = 0;
            sweep_timer = 0.0f;
            phase = DuelPhase::Sweeping;
        }
    }

    void trigger_live_ring_press() {
        if (phase != DuelPhase::LiveRing) return;
        if (live_ring_angle >= 270.0f && live_ring_angle <= 298.0f) {
            live_ring_bonus = 2;
            push_log("STRIKE! Judgment Ring grants +2 Power to your strongest Node!");
        } else if (live_ring_angle >= 205.0f && live_ring_angle <= 298.0f) {
            live_ring_bonus = 1;
            push_log("HIT! Judgment Ring grants +1 Power to your strongest Node!");
        } else {
            live_ring_bonus = 0;
            push_log("Judgment Ring missed — no strike bonus.");
        }
        sweep_cursor = 0;
        reverse_sweep_this_round = false;
        sweep_timer = 0.0f;
        phase = DuelPhase::Sweeping;
    }

    // Resolve a single Altar Node during the clockwise sweep with Combo Chain momentum
    void resolve_node(int n) {
        if (n < 0 || n >= kNodeCount || phase == DuelPhase::Finished) return;
        BoardUnit &u0 = nodes[n].slots[0];
        BoardUnit &u1 = nodes[n].slots[1];
        if (!u0.occupied && !u1.occupied) {
            chain_owner = -1;
            chain_streak = 0;
            return;
        }

        const int c0 = (chain_owner == 0 && chain_streak > 0) ? 1 : 0;
        const int c1 = (chain_owner == 1 && chain_streak > 0) ? 1 : 0;
        const int p0 = effective_power(n, 0, c0);
        const int p1 = effective_power(n, 1, c1);

        const int chain_bonus_dmg = (active_arena_mod() == ArenaMod::GateOfMalice) ? 2 : 1;
        if (u0.occupied && !u1.occupied) {
            advance_chain(0);
            nodes[n].seal_owner = 0;
            const int dmg = std::max(1, p0 / 2) + (chain_streak >= 2 ? chain_bonus_dmg : 0);
            players[1].sanity = std::max(0, players[1].sanity - dmg);
            pending_vfx.push_back({n, 0, (chain_streak >= 2 ? ("CHAIN x" + std::to_string(chain_streak) + "  -" + std::to_string(dmg) + " SP!") : ("-" + std::to_string(dmg) + " SP")),
                                   chain_streak >= 2 ? 0xFFD860u : 0x5CE8A0u, dmg >= 3 ? 0.45f : 0.20f, true, false,
                                   chain_streak >= 2 ? 4 : 3});
            push_log("Node " + std::to_string(n + 1) + ": Unopposed!" +
                     (chain_streak >= 2 ? (" [CHAIN x" + std::to_string(chain_streak) + "]") : "") +
                     " Dealt " + std::to_string(dmg) + " Sanity.");
        } else if (!u0.occupied && u1.occupied) {
            advance_chain(1);
            nodes[n].seal_owner = 1;
            const int dmg = std::max(1, p1 / 2) + (chain_streak >= 2 ? chain_bonus_dmg : 0);
            players[0].sanity = std::max(0, players[0].sanity - dmg);
            pending_vfx.push_back({n, 1, "-" + std::to_string(dmg) + " YOUR SP", 0xF05868u, dmg >= 3 ? 0.50f : 0.22f, true, false, 3});
            push_log("Node " + std::to_string(n + 1) + ": Foe unopposed!" +
                     (chain_streak >= 2 ? (" [CHAIN x" + std::to_string(chain_streak) + "]") : "") +
                     " Lost " + std::to_string(dmg) + " Sanity.");
        } else if (p0 > p1) {
            advance_chain(0);
            apply_clash_win(n, 0, p0 - p1);
        } else if (p1 > p0) {
            advance_chain(1);
            apply_clash_win(n, 1, p1 - p0);
        } else {
            chain_owner = -1;
            chain_streak = 0;
            push_log("Node " + std::to_string(n + 1) + ": Clash tied (" + std::to_string(p0) + " vs " + std::to_string(p1) + ") — Chain broken!");
        }
        check_victory();
    }

    int sweep_node_for_step(int step) const noexcept {
        const int s = std::clamp(step, 0, kNodeCount - 1);
        return reverse_sweep_this_round ? (kNodeCount - 1 - s) : s;
    }

    void resolve_full_sweep() {
        if (phase == DuelPhase::PlayerDeploy) end_player_deploy();
        if (phase == DuelPhase::LiveRing) {
            live_ring_bonus = 0;
            phase = DuelPhase::Sweeping;
            sweep_cursor = 0;
        }
        while (phase == DuelPhase::Sweeping && sweep_cursor < kNodeCount) {
            resolve_node(sweep_node_for_step(sweep_cursor++));
        }
        if (phase == DuelPhase::Sweeping) {
            finish_round_after_sweep();
        }
    }

    void tick(float dt_sec) {
        if (phase == DuelPhase::LiveRing) {
            live_ring_angle += dt_sec * 240.0f;
            if (live_ring_angle >= 360.0f) {
                live_ring_bonus = 0;
                push_log("Judgment Ring expired — no strike bonus.");
                sweep_cursor = 0;
                sweep_timer = 0.0f;
                phase = DuelPhase::Sweeping;
            }
        } else if (phase == DuelPhase::Sweeping) {
            sweep_timer += dt_sec;
            if (sweep_timer >= 0.36f) {
                sweep_timer = 0.0f;
                if (sweep_cursor < kNodeCount) {
                    resolve_node(sweep_node_for_step(sweep_cursor++));
                } else {
                    finish_round_after_sweep();
                }
            }
        }
    }

private:
    void advance_chain(int win_p) noexcept {
        if (chain_owner == win_p) ++chain_streak;
        else { chain_owner = win_p; chain_streak = 1; }
    }

    void begin_round_initiative() {
        players[0].channeled_this_round = false;
        players[1].channeled_this_round = false;
        // Odd rounds (1, 3, 5...): AI deploys first so Player sees their board & counter-deploys!
        // Even rounds (2, 4, 6...): Player leads first, then AI responds.
        ai_deployed_first = ((round_num & 1) != 0);
        if (ai_deployed_first) {
            run_ai_deploy();
        }
    }

    void push_log(const std::string &line) {
        log.push_back(line);
        if (log.size() > 14u) log.erase(log.begin());
    }

    void draw_card(int p) {
        if (players[p].deck.empty()) return;
        if (static_cast<int>(players[p].hand.size()) >= kMaxHandSize) return;
        players[p].hand.push_back(players[p].deck.back());
        players[p].deck.pop_back();
    }

    void apply_clash_win(int n, int win_p, int diff) {
        const int lose_p = 1 - win_p;
        BoardUnit &win_u = nodes[n].slots[win_p];
        BoardUnit &lose_u = nodes[n].slots[lose_p];
        nodes[n].seal_owner = win_p;

        int dmg = std::max(1, diff);
        if (win_u.ability == cards::AbilityId::MaliceStrike) dmg += 2;
        if (element_volatile(win_u.element, lose_u.element)) dmg += 1;
        if (chain_streak >= 2) dmg += (active_arena_mod() == ArenaMod::GateOfMalice) ? 2 : 1;
        players[lose_p].sanity = std::max(0, players[lose_p].sanity - dmg);

        if (win_u.ability == cards::AbilityId::SoulDrain) {
            players[win_p].sanity = std::min(kStartingSanity, players[win_p].sanity + 2);
        }

        // Soul Erosion (Wear-and-Tear): contested close clashes (diff <= 2) or tough blockers
        // wear down the surviving winner by -1 permanent Power (min 1) so Bosses can be chipped down!
        if (diff <= 2 && (win_u.base_power - win_u.erosion) > 1) {
            win_u.erosion += 1;
        }

        const std::string chain_tag = (chain_streak >= 2) ? (" [CHAIN x" + std::to_string(chain_streak) + "]") : "";
        const bool protected_by_sanctuary =
            (lose_u.ability == cards::AbilityId::Sanctuary && !win_u.is_boss);
        if (lose_u.ward_active) {
            lose_u.ward_active = false;
            pending_vfx.push_back({n, lose_p, "WARD! -" + std::to_string(dmg) + " SP", 0x70D8FFu, 0.35f, true, false, 5});
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won" + chain_tag + " (+" + std::to_string(dmg) + " dmg), Ward absorbed banish!");
        } else if (protected_by_sanctuary) {
            pending_vfx.push_back({n, lose_p, "SANCTUARY! -" + std::to_string(dmg) + " SP", 0xFFE888u, 0.35f, true, false, 5});
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won" + chain_tag + " (+" + std::to_string(dmg) + " dmg), Sanctuary held!");
        } else {
            if (lose_u.ability == cards::AbilityId::Vengeance) {
                players[win_p].sanity = std::max(0, players[win_p].sanity - 2);
            }
            players[lose_p].graveyard.push_back(lose_u.card_id);
            lose_u = BoardUnit{};
            pending_vfx.push_back({n, win_p, (chain_streak >= 2 ? ("CHAIN x" + std::to_string(chain_streak) + "  ") : "") +
                                      "-" + std::to_string(dmg) + " SP!",
                                   win_p == 0 ? (chain_streak >= 2 ? 0xFFD860u : 0x5CE8A0u) : 0xF05868u,
                                   dmg >= 3 ? 0.65f : 0.35f, true, false, chain_streak >= 2 ? 4 : 3});
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won" + chain_tag + ", banished foe & dealt " + std::to_string(dmg) + " Sanity!");
        }
    }

    void check_victory() {
        if (phase == DuelPhase::Finished) return;
        if (players[0].sanity <= 0 && players[1].sanity <= 0) {
            winner = 2;
            win_reason = "Mutual Sanity Shatter — Draw!";
            phase = DuelPhase::Finished;
        } else if (players[1].sanity <= 0) {
            winner = 0;
            win_reason = "Opponent's Sanity shattered!";
            phase = DuelPhase::Finished;
        } else if (players[0].sanity <= 0) {
            winner = 1;
            win_reason = "Your Sanity was shattered!";
            phase = DuelPhase::Finished;
        }
    }

    void finish_round_after_sweep() {
        if (phase == DuelPhase::Finished) return;
        for (int i = 0; i < kNodeCount; ++i) {
            nodes[i].slots[0].round_buff = 0;
            nodes[i].slots[1].round_buff = 0;
        }
        live_ring_bonus = 0;
        chain_owner = -1;
        chain_streak = 0;
        reverse_sweep_this_round = false;
        players[0].next_play_discount = 0;
        players[1].next_play_discount = 0;

        // Berserk Sanity drain if holding fewer than 2 Seals
        for (int p = 0; p < 2; ++p) {
            if (players[p].is_berserk() && count_seals(p) < 2) {
                players[p].sanity = std::max(0, players[p].sanity - 1);
            }
        }
        check_victory();
        if (phase == DuelPhase::Finished) return;

        const int s0 = count_seals(0);
        const int s1 = count_seals(1);
        if (s0 >= 4) {
            winner = 0;
            win_reason = "Altar Seal Domination (" + std::to_string(s0) + "/5 Seals)!";
            phase = DuelPhase::Finished;
            return;
        }
        if (s1 >= 4) {
            winner = 1;
            win_reason = "Opponent achieved Altar Seal Domination (" + std::to_string(s1) + "/5 Seals)!";
            phase = DuelPhase::Finished;
            return;
        }
        players[0].consec_majority_sweeps = (s0 >= 3) ? (players[0].consec_majority_sweeps + 1) : 0;
        players[1].consec_majority_sweeps = (s1 >= 3) ? (players[1].consec_majority_sweeps + 1) : 0;
        if (players[0].consec_majority_sweeps >= 2) {
            winner = 0;
            win_reason = "Held Altar Seal Majority (3+ Seals) for 2 consecutive sweeps!";
            phase = DuelPhase::Finished;
            return;
        }
        if (players[1].consec_majority_sweeps >= 2) {
            winner = 1;
            win_reason = "Opponent held Altar Seal Majority for 2 consecutive sweeps!";
            phase = DuelPhase::Finished;
            return;
        }

        ++round_num;
        for (int p = 0; p < 2; ++p) {
            players[p].max_soul_energy = std::min(kMaxSoulEnergy, players[p].max_soul_energy + 1);
            players[p].soul_energy = players[p].max_soul_energy;
            draw_card(p);
        }
        if (round_num > 14) {
            if (players[0].sanity != players[1].sanity) {
                winner = (players[0].sanity > players[1].sanity) ? 0 : 1;
                win_reason = "Final Round Sanity Decision!";
            } else if (s0 != s1) {
                winner = (s0 > s1) ? 0 : 1;
                win_reason = "Final Round Altar Seal Decision!";
            } else {
                winner = 2;
                win_reason = "Final Round Deadlock — Draw!";
            }
            phase = DuelPhase::Finished;
            return;
        }
        phase = DuelPhase::PlayerDeploy;
        push_log("--- Round " + std::to_string(round_num) + " (" + std::to_string(players[0].max_soul_energy) + " Soul) ---");
        begin_round_initiative();
    }
};

}  // namespace card_game
