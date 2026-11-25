#pragma once

// Shadow Hearts: Nocturne — "Judgment Ring Duel" Custom Card Game Engine
// Original Shadow Hearts-native ruleset:
//   - 5-Node Circular Altar Ring (Nodes 0..4 clockwise, adjacent modulo 5)
//   - 20 Sanity HP, Growing Soul Energy (1 -> 6 per round), 20-card decks
//   - Harmonixer Fusion: play a higher-cost card over an ally on a Node to
//     discount its cost and inherit +1 Power (+2 for Harmonixer ability)
//   - Clockwise Judgment Hand sweep (Node 0 -> 1 -> 2 -> 3 -> 4) with Elemental
//     Resonance, Boss Ward, 11 Nocturne Abilities, and optional Live Ring Strike
//   - Victory by draining opponent Sanity to 0, holding 4+ Altar Seals at once,
//     or holding 3+ Altar Seals for 2 consecutive sweeps.

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
constexpr int kMaxSoulEnergy = 6;
constexpr int kStartingHandSize = 4;
constexpr int kMaxHandSize = 7;

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
    // Pick up to 20 legal owned cards balanced across the cost curve
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
    std::vector<std::string> deck;
    std::vector<std::string> hand;
    std::vector<std::string> graveyard;
};

enum class DuelPhase : int {
    PlayerDeploy = 0,
    LiveRing = 1,
    Sweeping = 2,
    Finished = 3
};

class DuelMatch {
public:
    AltarNode nodes[kNodeCount]{};
    PlayerState players[2]{};
    int round_num = 1;
    DuelPhase phase = DuelPhase::PlayerDeploy;
    int winner = -1;  // -1 = ongoing, 0 = Player, 1 = AI, 2 = Draw
    std::string win_reason;
    bool using_trial_deck = true;
    bool live_ring_enabled = false;
    int live_ring_bonus = 0;
    float live_ring_angle = 0.0f;  // 0..360 deg
    int sweep_cursor = 0;          // 0..4 during animated sweep
    float sweep_timer = 0.0f;
    int opponent_idx = 0;
    bool reward_claimed = false;
    std::vector<std::string> log;
    std::mt19937_64 rng{0x53484E4F43545552ull};

    static int left_node(int n) noexcept { return (n + kNodeCount - 1) % kNodeCount; }
    static int right_node(int n) noexcept { return (n + 1) % kNodeCount; }

    void start(const std::vector<std::string> &player_deck, int opp_index,
               bool is_trial, bool enable_live_ring, std::uint64_t seed = 0) {
        for (int i = 0; i < kNodeCount; ++i) nodes[i] = AltarNode{};
        players[0] = PlayerState{};
        players[1] = PlayerState{};
        round_num = 1;
        phase = DuelPhase::PlayerDeploy;
        winner = -1;
        win_reason.clear();
        using_trial_deck = is_trial;
        live_ring_enabled = enable_live_ring;
        live_ring_bonus = 0;
        live_ring_angle = 0.0f;
        sweep_cursor = 0;
        sweep_timer = 0.0f;
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
    }

    int count_seals(int p) const noexcept {
        int n = 0;
        for (int i = 0; i < kNodeCount; ++i) {
            if (nodes[i].seal_owner == p) ++n;
        }
        return n;
    }

    int effective_power(int node_idx, int p) const noexcept {
        const BoardUnit &u = nodes[node_idx].slots[p];
        if (!u.occupied) return 0;
        int pwr = u.base_power + u.fusion_bonus + u.round_buff;
        if (u.ability == cards::AbilityId::Resonance) {
            const BoardUnit &lu = nodes[left_node(node_idx)].slots[p];
            const BoardUnit &ru = nodes[right_node(node_idx)].slots[p];
            if ((lu.occupied && lu.element == u.element) ||
                (ru.occupied && ru.element == u.element)) {
                pwr += 2;
            }
        }
        if (u.ability == cards::AbilityId::Covenant) {
            pwr += count_seals(p);
        }
        if (p == 0 && live_ring_bonus > 0) {
            // Apply Live Ring strike bonus to player's highest-base-power occupied node
            int best_node = -1, best_bp = -1;
            for (int i = 0; i < kNodeCount; ++i) {
                if (nodes[i].slots[0].occupied && nodes[i].slots[0].base_power > best_bp) {
                    best_bp = nodes[i].slots[0].base_power;
                    best_node = i;
                }
            }
            if (node_idx == best_node) pwr += live_ring_bonus;
        }
        return std::max(0, pwr);
    }

    // Compute Soul Energy cost to play card_id onto node_idx (handles Harmonixer Fusion discount)
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
            // Harmonixer Fusion: requires a strictly higher-cost card
            if (def->cost <= existing.cost) return false;
            fusion = true;
            eff_cost = std::max(1, def->cost - existing.cost);
        }
        if (eff_cost_out) *eff_cost_out = eff_cost;
        if (is_fusion_out) *is_fusion_out = fusion;
        return players[p].soul_energy >= eff_cost;
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

        // On-Play triggers
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

    // AI deploys cards based on difficulty
    void run_ai_deploy() {
        if (phase == DuelPhase::Finished) return;
        const AiDifficulty diff = opponents()[static_cast<std::size_t>(opponent_idx)].difficulty;
        int plays_left = (diff == AiDifficulty::Easy) ? 1 : 3;

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
                    if (fusion) score += (def->ability == cards::AbilityId::Harmonixer) ? 18 : 6;
                    const BoardUnit &enemy = nodes[n].slots[0];
                    if (enemy.occupied) {
                        const int ep = effective_power(n, 0);
                        if (def->power + (fusion ? 1 : 0) > ep) score += 26;
                        else if (diff == AiDifficulty::Hard) score -= 18;
                    } else {
                        score += 12;
                    }
                    if (def->ability == cards::AbilityId::Resonance) {
                        if ((nodes[left_node(n)].slots[1].occupied && nodes[left_node(n)].slots[1].element == def->element) ||
                            (nodes[right_node(n)].slots[1].occupied && nodes[right_node(n)].slots[1].element == def->element)) {
                            score += 20;
                        }
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

    // Begin end-of-round sequence (AI deploy -> optional Live Ring -> Judgment Sweep)
    void end_player_deploy() {
        if (phase != DuelPhase::PlayerDeploy) return;
        run_ai_deploy();
        if (phase == DuelPhase::Finished) return;
        live_ring_bonus = 0;
        if (live_ring_enabled) {
            live_ring_angle = 0.0f;
            phase = DuelPhase::LiveRing;
        } else {
            sweep_cursor = 0;
            sweep_timer = 0.0f;
            phase = DuelPhase::Sweeping;
        }
    }

    // Player presses Space / clicks during Live Judgment Ring (Hit: 210..295 deg, Red Strike: 270..295 deg)
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

    // Resolve a single Altar Node clash during the clockwise sweep
    void resolve_node(int n) {
        if (n < 0 || n >= kNodeCount || phase == DuelPhase::Finished) return;
        BoardUnit &u0 = nodes[n].slots[0];
        BoardUnit &u1 = nodes[n].slots[1];
        if (!u0.occupied && !u1.occupied) return;

        const int p0 = effective_power(n, 0);
        const int p1 = effective_power(n, 1);

        if (u0.occupied && !u1.occupied) {
            nodes[n].seal_owner = 0;
            const int dmg = std::max(1, p0 / 2);
            players[1].sanity = std::max(0, players[1].sanity - dmg);
            push_log("Node " + std::to_string(n + 1) + ": Unopposed! Claimed Seal & dealt " + std::to_string(dmg) + " Sanity.");
        } else if (!u0.occupied && u1.occupied) {
            nodes[n].seal_owner = 1;
            const int dmg = std::max(1, p1 / 2);
            players[0].sanity = std::max(0, players[0].sanity - dmg);
            push_log("Node " + std::to_string(n + 1) + ": Foe unopposed! Lost " + std::to_string(dmg) + " Sanity.");
        } else if (p0 > p1) {
            apply_clash_win(n, 0, p0 - p1);
        } else if (p1 > p0) {
            apply_clash_win(n, 1, p1 - p0);
        } else {
            push_log("Node " + std::to_string(n + 1) + ": Clash tied (" + std::to_string(p0) + " vs " + std::to_string(p1) + ").");
        }
        check_victory();
    }

    // Complete the entire 5-node sweep synchronously (great for unit tests & instant mode)
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

    // Advance live animations (Live Ring spin & step-by-step clockwise sweep)
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
        players[lose_p].sanity = std::max(0, players[lose_p].sanity - dmg);

        if (win_u.ability == cards::AbilityId::SoulDrain) {
            players[win_p].sanity = std::min(kStartingSanity, players[win_p].sanity + 2);
        }

        // Check Sanctuary / Ward before banishing loser
        const bool protected_by_sanctuary =
            (lose_u.ability == cards::AbilityId::Sanctuary && !win_u.is_boss);
        if (lose_u.ward_active) {
            lose_u.ward_active = false;
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won (+" + std::to_string(dmg) + " dmg), enemy Ward absorbed banish!");
        } else if (protected_by_sanctuary) {
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " won (+" + std::to_string(dmg) + " dmg), Sanctuary prevented banish!");
        } else {
            if (lose_u.ability == cards::AbilityId::Vengeance) {
                players[win_p].sanity = std::max(0, players[win_p].sanity - 2);
            }
            players[lose_p].graveyard.push_back(lose_u.card_id);
            lose_u = BoardUnit{};
            push_log("Node " + std::to_string(n + 1) + ": " + (win_p == 0 ? "You" : "Foe") +
                     " claimed Seal, banished foe & dealt " + std::to_string(dmg) + " Sanity!");
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
        // Clear per-round temporary buffs
        for (int i = 0; i < kNodeCount; ++i) {
            nodes[i].slots[0].round_buff = 0;
            nodes[i].slots[1].round_buff = 0;
        }
        live_ring_bonus = 0;

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

        // Advance to next round
        ++round_num;
        for (int p = 0; p < 2; ++p) {
            players[p].max_soul_energy = std::min(kMaxSoulEnergy, players[p].max_soul_energy + 1);
            players[p].soul_energy = players[p].max_soul_energy;
            draw_card(p);
        }
        if (round_num > 14) {
            // Sudden-death tiebreaker on round 15
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
        push_log("--- Round " + std::to_string(round_num) + " (" + std::to_string(players[0].max_soul_energy) + " Soul Energy) ---");
    }
};

}  // namespace card_game
