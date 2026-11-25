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

struct OpponentProfile {
    std::string id;
    std::string name;
    std::string title;
    AiDifficulty difficulty = AiDifficulty::Easy;
    int reward_essence = 40;
    std::vector<std::string> deck_ids;
};

inline const std::vector<OpponentProfile> &opponents() {
    static const std::vector<OpponentProfile> kOpps = {
        {
            "alias", "Alias", "The Wandering Peddler (Easy)", AiDifficulty::Easy, 40,
            {"kdl_alias", "kdl_alias", "kdl_gremlin", "kdl_gremlin", "kdl_sorrow", "kdl_sorrow",
             "kdl_ogden", "kdl_ogden", "kdl_bessy", "kdl_bessy", "kdl_mimic", "kdl_mimic",
             "kdl_bone_knight", "kdl_bone_knight", "kdl_banshee", "kdl_cursed_armor",
             "kdl_charlotte", "kdl_gargoyle", "sh1_ghoul", "sh1_bakeneko"}
        },
        {
            "roger", "Roger Bacon", "The Immortal Alchemist (Normal)", AiDifficulty::Normal, 75,
            {"kdl_gremlin", "kdl_sorrow", "kdl_bessy", "kdl_banshee", "kdl_nightgaunt",
             "kdl_koudelka", "kdl_edward", "kdl_james", "kdl_roger", "kdl_elaine",
             "sh1_yuri", "sh1_alice", "sh1_zhuzhen", "sh1_halley", "sh1_roger",
             "sh1_raging_tiger", "sh1_death_emperor", "sh2_lucia", "sh2_blanca", "sh3_roger"}
        },
        {
            "simon", "Albert Simon", "The False Prophet (Hard)", AiDifficulty::Hard, 120,
            {"sh1_ghoul", "sh1_qinggu", "sh2_gargoyle_hound", "sh3_alcatraz_guard",
             "sh1_malice_puppet", "sh2_shadow_knight", "sh3_blood_golem", "kdl_cursed_armor",
             "sh1_yuri", "sh2_yuri", "sh2_kurando", "sh1_dehuai", "sh2_rasputin",
             "kdl_elaine_aberration", "sh1_atman", "sh1_albert_simon",
             "sh2_asmodeus", "sh2_kato", "sh1_meta_god", "sh2_umbral_god"}
        }
    };
    return kOpps;
}

inline std::vector<std::string> trial_deck_ids() {
    return {
        "kdl_alias", "kdl_gremlin", "kdl_sorrow", "kdl_ogden",
        "kdl_bessy", "kdl_mimic", "kdl_bone_knight", "sh1_bakeneko",
        "sh2_gargoyle_hound", "sh3_alcatraz_guard",
        "kdl_banshee", "kdl_cursed_armor", "kdl_gargoyle", "sh1_raging_tiger",
        "sh2_shadow_knight", "sh3_voodoo_doll",
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
    int round_buff = 0;  // ArcaneEcho (+1) or Eclipse (-2), cleared after sweep
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
    Finished = 3
};

struct DuelVfxEvent {
    int node_idx = -1;       // 0..4 or -1 for center
    int player_idx = -1;     // 0 or 1 when tied to a specific card slot
    std::string text;        // Floating combat text (e.g. "-4 SP!", "FUSED +2P!", "CHAIN x3!")
    std::uint32_t rgb = 0xFFD060u;
    float shake = 0.0f;      // 0..1 screen shake intensity
    bool shockwave = true;
    bool is_summon = false;  // Triggers physical Summon Slam drop-in animation
};

struct PowerBreakdown {
    int base = 0;
    int fusion = 0;
    int harmony = 0;
    int elem_adv = 0;
    int covenant = 0;
    int berserk = 0;
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
    int sweep_cursor = 0;          // 0..4 during animated sweep
    float sweep_timer = 0.0f;
    int chain_owner = -1;          // Current clockwise Combo Chain owner during sweep
    int chain_streak = 0;          // Consecutive wins in current sweep (1..5)
    int opponent_idx = 0;
    bool reward_claimed = false;
    std::vector<std::string> log;
    std::vector<DuelVfxEvent> pending_vfx;
    std::mt19937_64 rng{0x53484E4F43545552ull};

    static int left_node(int n) noexcept { return (n + kNodeCount - 1) % kNodeCount; }
    static int right_node(int n) noexcept { return (n + 1) % kNodeCount; }

    void start(const std::vector<std::string> &player_deck, int opp_index,
               bool is_trial, bool enable_live_ring, std::uint64_t seed = 0) {
        for (int i = 0; i < kNodeCount; ++i) nodes[i] = AltarNode{};
        players[0] = PlayerState{};
        players[1] = PlayerState{};
        round_num = 1;
        ai_deployed_first = false;
        phase = DuelPhase::PlayerDeploy;
        winner = -1;
        win_reason.clear();
        using_trial_deck = is_trial;
        live_ring_enabled = enable_live_ring;
        live_ring_bonus = 0;
        live_ring_angle = 0.0f;
        sweep_cursor = 0;
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

        std::shuffle(players[0].deck.begin(), players[0].deck.end(), rng);
        std::shuffle(players[1].deck.begin(), players[1].deck.end(), rng);

        for (int i = 0; i < kStartingHandSize; ++i) {
            draw_card(0);
            draw_card(1);
        }
        push_log("Judgment Ring Duel vs " + opps[static_cast<std::size_t>(opponent_idx)].name + " — Round 1!");
        begin_round_initiative();
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
        pb.round_buff = u.round_buff;
        if (has_ring_harmony(node_idx, p)) {
            pb.harmony = (u.ability == cards::AbilityId::Resonance) ? 3 : 1;
        }
        const BoardUnit &opp = nodes[node_idx].slots[1 - p];
        if (opp.occupied && element_beats(u.element, opp.element)) {
            pb.elem_adv = 1;
        }
        if (u.ability == cards::AbilityId::Covenant) {
            pb.covenant = count_seals(p);
        }
        if (players[p].is_berserk()) {
            pb.berserk = 1;
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
                               pb.covenant + pb.berserk + pb.round_buff + pb.live_ring + chain_bonus);
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
        const BoardUnit &existing = nodes[node_idx].slots[p];
        bool fusion = false;
        int eff_cost = def->cost;
        if (existing.occupied) {
            if (def->cost <= existing.cost) return false;
            fusion = true;
            eff_cost = std::max(1, def->cost - existing.cost);
        }
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
        pv.fusion_bonus = pv.is_fusion ? ((def->ability == cards::AbilityId::Harmonixer) ? 2 : 1) : 0;

        int pwr = def->power + pv.fusion_bonus;
        const BoardUnit &lu = nodes[left_node(node_idx)].slots[0];
        const BoardUnit &ru = nodes[right_node(node_idx)].slots[0];
        if ((lu.occupied && lu.element == def->element) || (ru.occupied && ru.element == def->element)) {
            pv.harmony_active = true;
            pwr += (def->ability == cards::AbilityId::Resonance) ? 3 : 1;
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
            }
        }
        if (def->ability == cards::AbilityId::Covenant) pwr += count_seals(0);
        if (players[0].is_berserk()) pwr += 1;
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
        players[p].hand.erase(players[p].hand.begin() + hand_idx);

        BoardUnit &slot = nodes[node_idx].slots[p];
        int inherited_bonus = 0;
        if (fusion) {
            players[p].graveyard.push_back(slot.card_id);
            inherited_bonus = (def->ability == cards::AbilityId::Harmonixer) ? 2 : 1;
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

        const std::string who = (p == 0) ? "You" : opponents()[static_cast<std::size_t>(opponent_idx)].name;
        push_log(who + (fusion ? " FUSED " : " played ") + def->name + " on Node " + std::to_string(node_idx + 1) +
                 (fusion ? (" (+" + std::to_string(inherited_bonus) + " Pwr)") : ""));
        pending_vfx.push_back({node_idx, p, fusion ? ("FUSED +" + std::to_string(inherited_bonus) + "P!") : def->name,
                               fusion ? 0xE078FFu : cards::element_color_rgb(def->element),
                               fusion ? 0.55f : 0.22f, true, true});

        const int opp = 1 - p;
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
        }
        check_victory();
        return true;
    }

    void run_ai_deploy() {
        if (phase == DuelPhase::Finished) return;
        const AiDifficulty diff = opponents()[static_cast<std::size_t>(opponent_idx)].difficulty;
        int plays_left = (diff == AiDifficulty::Easy) ? 1 : 3;

        // Hard/Normal AI can Channel a high-cost brick if it has no playable card
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

        while (plays_left-- > 0 && phase != DuelPhase::Finished) {
            int best_h = -1, best_n = -1;
            int best_score = -1000;
            for (int h = 0; h < static_cast<int>(players[1].hand.size()); ++h) {
                const cards::CardDef *def = cards::find_card_def(players[1].hand[static_cast<std::size_t>(h)]);
                if (!def) continue;
                for (int n = 0; n < kNodeCount; ++n) {
                    int cost = 0;
                    bool fusion = false;
                    if (!can_play_card(1, h, n, &cost, &fusion)) continue;
                    int score = def->power * 10 - cost * 2;
                    if (fusion) score += (def->ability == cards::AbilityId::Harmonixer) ? 20 : 8;
                    const BoardUnit &enemy = nodes[n].slots[0];
                    if (enemy.occupied) {
                        const int ep = effective_power(n, 0);
                        int myp = def->power + (fusion ? 1 : 0) + (element_beats(def->element, enemy.element) ? 1 : 0);
                        if (myp > ep) score += 28;
                        else if (diff == AiDifficulty::Hard) score -= 20;
                    } else {
                        // Prefer earlier clockwise nodes (0, 1) to start Combo Chains
                        score += 14 + (kNodeCount - n) * 2;
                    }
                    // Reward Elemental Harmony with adjacent AI nodes
                    const BoardUnit &lu = nodes[left_node(n)].slots[1];
                    const BoardUnit &ru = nodes[right_node(n)].slots[1];
                    if ((lu.occupied && lu.element == def->element) || (ru.occupied && ru.element == def->element)) {
                        score += (def->ability == cards::AbilityId::Resonance) ? 24 : 10;
                    }
                    if (diff == AiDifficulty::Easy) {
                        score += static_cast<int>(rng() % 25u);
                    }
                    if (score > best_score) {
                        best_score = score;
                        best_h = h;
                        best_n = n;
                    }
                }
            }
            if (best_h < 0) break;
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

        if (u0.occupied && !u1.occupied) {
            advance_chain(0);
            nodes[n].seal_owner = 0;
            const int dmg = std::max(1, p0 / 2) + (chain_streak >= 2 ? 1 : 0);
            players[1].sanity = std::max(0, players[1].sanity - dmg);
            pending_vfx.push_back({n, 0, (chain_streak >= 2 ? ("CHAIN x" + std::to_string(chain_streak) + "  -" + std::to_string(dmg) + " SP!") : ("-" + std::to_string(dmg) + " SP")),
                                   chain_streak >= 2 ? 0xFFD860u : 0x5CE8A0u, dmg >= 3 ? 0.45f : 0.20f, true, false});
            push_log("Node " + std::to_string(n + 1) + ": Unopposed!" +
                     (chain_streak >= 2 ? (" [CHAIN x" + std::to_string(chain_streak) + "]") : "") +
                     " Dealt " + std::to_string(dmg) + " Sanity.");
        } else if (!u0.occupied && u1.occupied) {
            advance_chain(1);
            nodes[n].seal_owner = 1;
            const int dmg = std::max(1, p1 / 2) + (chain_streak >= 2 ? 1 : 0);
            players[0].sanity = std::max(0, players[0].sanity - dmg);
            pending_vfx.push_back({n, 1, "-" + std::to_string(dmg) + " YOUR SP", 0xF05868u, dmg >= 3 ? 0.50f : 0.22f, true, false});
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

    void resolve_full_sweep() {
        if (phase == DuelPhase::PlayerDeploy) end_player_deploy();
        if (phase == DuelPhase::LiveRing) {
            live_ring_bonus = 0;
            phase = DuelPhase::Sweeping;
            sweep_cursor = 0;
        }
        while (phase == DuelPhase::Sweeping && sweep_cursor < kNodeCount) {
            resolve_node(sweep_cursor++);
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
                    resolve_node(sweep_cursor++);
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
        if (chain_streak >= 2) dmg += 1;  // Clockwise Combo Chain bonus!
        players[lose_p].sanity = std::max(0, players[lose_p].sanity - dmg);

        if (win_u.ability == cards::AbilityId::SoulDrain) {
            players[win_p].sanity = std::min(kStartingSanity, players[win_p].sanity + 2);
        }

        const std::string chain_tag = (chain_streak >= 2) ? (" [CHAIN x" + std::to_string(chain_streak) + "]") : "";
        const bool protected_by_sanctuary =
            (lose_u.ability == cards::AbilityId::Sanctuary && !win_u.is_boss);
        if (lose_u.ward_active) {
            lose_u.ward_active = false;
            pending_vfx.push_back({n, lose_p, "WARD! -" + std::to_string(dmg) + " SP", 0x70D8FFu, 0.35f, true, false});
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won" + chain_tag + " (+" + std::to_string(dmg) + " dmg), Ward absorbed banish!");
        } else if (protected_by_sanctuary) {
            pending_vfx.push_back({n, lose_p, "SANCTUARY! -" + std::to_string(dmg) + " SP", 0xFFE888u, 0.35f, true, false});
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
                                   dmg >= 3 ? 0.65f : 0.35f, true, false});
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
