#include "coop_session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace coop {

namespace {

std::string sanitize_campaign(const std::string &in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            out.push_back(c);
            if (out.size() >= 32) break;
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    return out.empty() ? "nocturne_coop" : out;
}

const char *emote_label(EmoteKind e) {
    switch (e) {
    case EmoteKind::Nice:      return "NICE!";
    case EmoteKind::Heal:      return "HEAL UP!";
    case EmoteKind::Combo:     return "CHAIN COMBO!";
    case EmoteKind::PassStick: return "PASS THE STICK!";
    }
    return "PING!";
}

const char *reason_label(AuthorityReason r) {
    switch (r) {
    case AuthorityReason::BattleTurn:  return "BATTLE TURN";
    case AuthorityReason::RelayCombo:  return "RELAY COMBO!";
    case AuthorityReason::PassStick:   return "PASS THE STICK";
    case AuthorityReason::ManualF6:    return "HANDOFF";
    case AuthorityReason::Initial:     return "SESSION START";
    case AuthorityReason::LevelUp:     return "LEVEL UP!";
    case AuthorityReason::FieldReturn: return "EXPLORATION";
    }
    return "TURN";
}

const char *koudelka_char_name(std::uint8_t slot) {
    switch (slot) {
    case 0: return "Koudelka";
    case 1: return "Edward";
    case 2: return "James";
    default: return "Party";
    }
}

} // namespace

bool CoopSession::start(const LaunchConfig &cfg, const std::string &game_root_dir, SessionHooks hooks) {
    stop();
    if (!cfg.enabled) return false;

    launch_ = cfg;
    settings_ = cfg.settings;
    hooks_ = std::move(hooks);
    settings_.campaign_name = sanitize_campaign(settings_.campaign_name);

    // Prepare isolated co-op save & state directories (never touch solo saves)
    std::filesystem::path base = game_root_dir.empty() ? std::filesystem::current_path()
                                                       : std::filesystem::path(game_root_dir);
    std::filesystem::path cdir = base / "coop_saves" / settings_.campaign_name;
    std::error_code ec;
    std::filesystem::create_directories(cdir / "states", ec);
    coop_save_dir_ = cdir.string();

    setup_callbacks();

    const std::uint16_t bind_port = cfg.is_host ? (cfg.local_port ? cfg.local_port : kDefaultGamePort) : 0;
    if (!transport_.open(bind_port, cfg.is_host)) {
        std::fprintf(stderr, "[coop] failed to open UDP socket on port %u\n", bind_port);
        return false;
    }

    bool ok = false;
    if (cfg.is_host) {
        ok = transport_.host_room(cfg.player_name, settings_, cfg.relay_addr);
        authority_owner_ = 0;
    } else if (!cfg.room_code.empty() && !cfg.relay_addr.empty()) {
        ok = transport_.join_by_code(cfg.player_name, cfg.room_code, cfg.relay_addr);
        authority_owner_ = 0;
    } else {
        ok = transport_.join_direct(cfg.player_name, cfg.direct_addr);
        authority_owner_ = 0;
    }

    if (!ok) {
        transport_.close();
        return false;
    }

    active_ = true;
    authority_epoch_ = 1;
    active_party_slot_ = 0;
    observer_primed_ = false;
    kdl_observer_primed_ = false;
    kdl_return_host_on_field_ = false;
    kdl_phase_ = KdlPhase::Unknown;
    kdl_char_ = 0xFFu;
    kdl_pending_phase_ = KdlPhase::Unknown;
    kdl_pending_char_ = 0xFFu;
    kdl_pending_frames_ = 0;
    auto_switch_cooldown_ = 0;
    jitter_.reset(0);
    push_banner(cfg.is_host ? "CO-OP ROOM HOSTED" : "CONNECTING TO CO-OP...",
                cfg.is_host ? ("Campaign: " + settings_.campaign_name) : "Syncing with host...",
                0xD8B458FFu, 3200);
    return true;
}

void CoopSession::stop() {
    if (!active_) return;
    transport_.close();
    state_baseline_.clear();
    active_ = false;
}

std::string CoopSession::coop_memcard_path(const std::string &filename) const {
    if (coop_save_dir_.empty()) return filename;
    return (std::filesystem::path(coop_save_dir_) / filename).string();
}

std::string CoopSession::coop_state_dir() const {
    if (coop_save_dir_.empty()) return "";
    return (std::filesystem::path(coop_save_dir_) / "states").string();
}

void CoopSession::setup_callbacks() {
    TransportCallbacks cb;

    cb.on_room_created = [this](const std::string &code, std::uint32_t) {
        if (!launch_.code_out_file.empty() && !code.empty()) {
            const std::filesystem::path dst(launch_.code_out_file);
            const std::filesystem::path tmp = dst.string() + ".tmp";
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (ofs.is_open()) {
                ofs.write(code.data(), static_cast<std::streamsize>(code.size()));
                ofs.flush();
                ofs.close();
                std::error_code ec;
                std::filesystem::rename(tmp, dst, ec);
                if (ec) {
                    std::ofstream direct(dst, std::ios::binary | std::ios::trunc);
                    if (direct.is_open()) {
                        direct.write(code.data(), static_cast<std::streamsize>(code.size()));
                    }
                    std::filesystem::remove(tmp, ec);
                }
            }
        }
        push_banner("CO-OP ROOM READY", "Code: " + format_room_code_pretty(code), 0xD8B458FFu, 4500);
    };

    cb.on_joined_room = [this](std::uint8_t slot, std::uint32_t) {
        char sub[64];
        std::snprintf(sub, sizeof(sub), "Joined as P%u  |  Waiting for host checkpoint", unsigned(slot) + 1u);
        push_banner("CONNECTED TO CO-OP", sub, 0x70D898FFu, 3200);
        transport_.send_resync_request(true);
    };

    cb.on_error = [this](const std::string &reason) {
        push_banner("CO-OP NOTICE", reason, 0xE05050FFu, 4000);
    };

    cb.on_peer_joined = [this](std::uint8_t slot, const PeerInfo &peer) {
        push_banner("PARTNER JOINED", peer.name + " (P" + std::to_string(unsigned(slot) + 1u) + ")", 0x70D898FFu, 3000);
        // Send initial full keyframe to the newly joined peer so they immediately match our frame
        if (has_authority()) {
            broadcast_checkpoint(false, slot, true);
        }
    };

    cb.on_peer_left = [this](std::uint8_t slot) {
        push_banner("PARTNER DISCONNECTED", "P" + std::to_string(unsigned(slot) + 1u) + " left the session", 0xE07050FFu, 3000);
        // If the disconnected peer held authority, reclaim to host (P1) immediately
        if (authority_owner_ == slot && is_host()) {
            grant_authority_to(0, active_party_slot_, AuthorityReason::PassStick);
        }
    };

    cb.on_lobby_config = [this](const LobbySettings &cfg) {
        settings_ = cfg;
    };

    cb.on_input_frame = [this](std::uint8_t sender, const PadSample &sample, std::uint16_t epoch) {
        if (sender != authority_owner_ || epoch < authority_epoch_) return;
        jitter_.push(sample);
    };

    cb.on_authority_grant = [this](std::uint8_t new_owner, std::uint8_t party_slot,
                                   AuthorityReason reason, std::uint16_t epoch) {
        if (epoch < authority_epoch_) return;
        authority_epoch_ = epoch;
        authority_owner_ = new_owner;
        active_party_slot_ = party_slot;
        jitter_.reset(0);
        auto_switch_cooldown_ = 30;
        kdl_pending_frames_ = 0;

        if (settings_.game == GameKind::Koudelka) {
            if (reason == AuthorityReason::BattleTurn) {
                kdl_observer_primed_ = true;
                kdl_phase_ = KdlPhase::Battle;
                kdl_char_ = party_slot;
                kdl_return_host_on_field_ = true;
            } else if (reason == AuthorityReason::LevelUp) {
                kdl_observer_primed_ = true;
                kdl_phase_ = KdlPhase::LevelUp;
                kdl_char_ = party_slot;
                kdl_return_host_on_field_ = true;
            } else if (reason == AuthorityReason::FieldReturn) {
                kdl_observer_primed_ = true;
                kdl_phase_ = KdlPhase::Field;
                kdl_char_ = party_slot;
                kdl_return_host_on_field_ = false;
            }
        }

        const bool is_local = (new_owner == transport_.local_slot());
        if (settings_.game == GameKind::Koudelka) {
            const std::string cname = koudelka_char_name(party_slot);
            if (reason == AuthorityReason::LevelUp) {
                if (is_local) {
                    push_banner("LEVEL UP — " + cname + "!", "Allocate Bonus Points", 0xFFE060FFu, 3200);
                } else {
                    push_banner(authority_owner_name() + " — LEVEL UP (" + cname + ")", "Allocating Bonus Points", 0x8AA8E0FFu, 2400);
                }
            } else if (reason == AuthorityReason::FieldReturn) {
                if (is_local) {
                    push_banner("FIELD EXPLORATION", "Control returned to Host", 0x70D898FFu, 2200);
                } else {
                    push_banner("FIELD EXPLORATION", authority_owner_name() + " exploring", 0x8AA8E0FFu, 2000);
                }
            } else if (reason == AuthorityReason::BattleTurn) {
                if (is_local) {
                    push_banner("YOUR TURN — " + cname, "Battle Turn", 0xF0C850FFu, 2600);
                } else {
                    push_banner(authority_owner_name() + "'S TURN — " + cname, "Battle Turn", 0x8AA8E0FFu, 2000);
                }
            } else if (is_local) {
                push_banner("YOUR TURN!", reason_label(reason), 0xF0C850FFu, 2600);
            } else {
                push_banner(authority_owner_name() + "'S TURN", reason_label(reason), 0x8AA8E0FFu, 2000);
            }
        } else if (is_local) {
            push_banner("YOUR TURN — RING READY!",
                        std::string(reason_label(reason)) + "  (Slot " + std::to_string(unsigned(party_slot) + 1u) + ")",
                        0xF0C850FFu, 2800);
        } else {
            push_banner(authority_owner_name() + "'S TURN",
                        reason_label(reason), 0x8AA8E0FFu, 2000);
        }
    };

    cb.on_authority_request = [this](std::uint8_t requester, std::uint8_t desired_owner, AuthorityReason reason) {
        if (!has_authority()) return;
        const std::uint8_t target = (desired_owner < kMaxPlayers) ? desired_owner : requester;
        grant_authority_to(target, active_party_slot_, reason);
    };

    cb.on_state_blob = [this](std::uint8_t sender, std::uint32_t, const std::vector<std::uint8_t> &blob, std::uint8_t flags) {
        if (sender == transport_.local_slot() || !hooks_.load_state_mem) return;
        std::vector<std::uint8_t> full;
        const std::vector<std::uint8_t> empty_base;
        const bool is_delta = (flags & 0x01u) != 0;
        if (!decompress_delta(blob, is_delta ? state_baseline_ : empty_base, full)) {
            // Baseline mismatch: request a full keyframe from authority
            request_resync(true);
            return;
        }
        if (hooks_.load_state_mem(full.data(), full.size())) {
            state_baseline_ = std::move(full);
            if (flags & 0x02u) {
                jitter_.reset(0);
                push_banner("SYNCED QUICK-LOAD", "Loaded partner quick-state", 0x80C8F0FFu, 2200);
            } else {
                push_banner("CO-OP SYNCED", "Matched host game state", 0x70D898FFu, 2000);
            }
        }
    };

    cb.on_sync_hash = [this](std::uint8_t sender, std::uint32_t /*frame*/, std::uint64_t remote_hash) {
        if (has_authority() || sender != authority_owner_ || !hooks_.compute_ram_hash) return;
        if (transport_.state_transfer_active()) return;
        const std::uint64_t local_hash = hooks_.compute_ram_hash();
        const std::uint64_t now = steady_now_ms();
        // Only trigger an automatic delta resync if mismatch persists past an 8s cooldown
        // so normal 4-6 frame network latency skew never thrashes playback or FMVs.
        if (local_hash != remote_hash && now - last_resync_req_ms_ > 8000) {
            last_resync_req_ms_ = now;
            request_resync(state_baseline_.empty());
        }
    };

    cb.on_resync_request = [this](std::uint8_t sender, bool force_full) {
        if (!has_authority() || transport_.state_transfer_active()) return;
        if (force_full) state_baseline_.clear();
        broadcast_checkpoint(false, sender, force_full);
    };

    cb.on_memcard_mirror = [this](std::uint8_t sender, const std::vector<std::uint8_t> &compressed_mc) {
        if (sender == transport_.local_slot() || !settings_.mirror_memcard || memcard_path_.empty()) return;
        std::vector<std::uint8_t> raw;
        if (!decompress_delta(compressed_mc, {}, raw)) return;
        std::ofstream ofs(memcard_path_, std::ios::binary | std::ios::trunc);
        if (ofs.is_open()) {
            ofs.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
            ofs.close();
            push_banner("CO-OP SAVE MIRRORED", "Campaign memory card backed up locally", 0x70D898FFu, 2400);
            if (hooks_.on_memcard_mirrored) hooks_.on_memcard_mirrored(memcard_path_);
        }
    };

    cb.on_ring_callout = [this](std::uint8_t sender, RingOutcome outcome, std::uint8_t combo_count,
                                const std::string &char_name) {
        const std::string who = (sender < kMaxPlayers && !transport_.peers()[sender].name.empty())
                                    ? transport_.peers()[sender].name
                                    : ("P" + std::to_string(unsigned(sender) + 1u));
        std::string title;
        std::uint32_t col = 0xD8B458FFu;
        switch (outcome) {
        case RingOutcome::Perfect:    title = who + " — PERFECT STRIKE!"; col = 0xFFE060FFu; break;
        case RingOutcome::Strike:     title = who + " — STRIKE!";         col = 0x70E098FFu; break;
        case RingOutcome::Miss:       title = who + " — RING MISS!";      col = 0xE05858FFu; break;
        case RingOutcome::RelayCombo: title = who + " — RELAY COMBO x" + std::to_string(combo_count) + "!"; col = 0xFF9840FFu; break;
        case RingOutcome::Hit:        title = who + " — RING HIT!";       col = 0xB8D0F0FFu; break;
        }
        push_banner(title, char_name.empty() ? "Judgment Ring" : char_name, col, 2200);
    };

    cb.on_trophy_share = [this](std::uint8_t sender, const std::string &tid, const std::string &tname) {
        if (!settings_.share_trophies) return;
        const std::string who = (sender < kMaxPlayers && !transport_.peers()[sender].name.empty())
                                    ? transport_.peers()[sender].name
                                    : ("P" + std::to_string(unsigned(sender) + 1u));
        push_banner("SHARED TROPHY UNLOCKED", tname + " (via " + who + ")", 0xF0C850FFu, 3600);
        if (hooks_.on_shared_trophy) hooks_.on_shared_trophy(tid, tname);
    };

    cb.on_emote_ping = [this](std::uint8_t sender, EmoteKind emote) {
        if (!settings_.allow_pings) return;
        const std::string who = (sender < kMaxPlayers && !transport_.peers()[sender].name.empty())
                                    ? transport_.peers()[sender].name
                                    : ("P" + std::to_string(unsigned(sender) + 1u));
        push_banner(who + ": " + emote_label(emote), "Partner Ping", 0x98C8FFFFu, 2000);
    };

    transport_.set_callbacks(std::move(cb));
}

bool CoopSession::tick_frame(std::uint32_t frame, const PadSample &local_pad, PadSample &effective_pad) {
    if (!active_) {
        effective_pad = local_pad;
        return false;
    }

    last_frame_ = frame;
    transport_.poll();

    if (has_authority()) {
        // Zero-lag local authority: execute immediately on local machine and stream to peers
        effective_pad = local_pad;
        effective_pad.frame = frame;
        transport_.send_input_frame(effective_pad, authority_epoch_);

        // Observe SH2/SH3 Judgment Ring & Combo RAM for live partner callouts & Relay Combos
        observe_battle_ram();

        // Send lightweight RAM sync hash every 30 frames (~0.5s) for automatic drift detection
        if (hooks_.compute_ram_hash && frame >= last_hash_frame_ + 30) {
            last_hash_frame_ = frame;
            transport_.send_sync_hash(frame, hooks_.compute_ram_hash());
        }
        return true;
    }

    // Spectator: pull from 2-4 frame jitter buffer (lagged slightly behind authority's latest frame)
    const bool active_low = (settings_.game == GameKind::Koudelka);
    const std::uint32_t latest = jitter_.latest_frame();
    const std::uint32_t target = (latest > 2) ? (latest - 2) : latest;
    if (!jitter_.pop_for_frame(target, effective_pad, active_low)) {
        effective_pad = PadSample{};
        // PS1 (Koudelka) uses active-low buttons (0xFFFF = released); PS2 uses active-high (0x0000 = released)
        effective_pad.buttons = active_low ? 0xFFFFu : 0x0000u;
        effective_pad.frame = frame;
    }
    return true;
}

void CoopSession::observe_battle_ram() {
    if (!hooks_.read_ram_u32 || !has_authority()) return;

    if (settings_.game == GameKind::Koudelka) {
        observe_koudelka_ram();
        return;
    }

    std::uint32_t addr_att = 0, addr_hit = 0, addr_perf = 0, addr_miss = 0, addr_combo = 0;
    if (settings_.game == GameKind::SH2) {
        addr_att = 0x002634F8u;
        addr_hit = 0x002634FCu;
        addr_combo = 0x00263500u;
        addr_perf = 0x00263504u;
        addr_miss = 0x00263508u;
    } else if (settings_.game == GameKind::SH3) {
        addr_att = 0x00338274u;
        addr_hit = 0x00338278u;
        addr_perf = 0x0033827Cu;
        addr_miss = 0x00338284u;
        addr_combo = 0x00338294u;
    } else {
        return;
    }

    const std::uint32_t att = hooks_.read_ram_u32(addr_att);
    const std::uint32_t hit = hooks_.read_ram_u32(addr_hit);
    const std::uint32_t perf = hooks_.read_ram_u32(addr_perf);
    const std::uint32_t miss = hooks_.read_ram_u32(addr_miss);
    const std::uint32_t combo = hooks_.read_ram_u32(addr_combo);

    if (!observer_primed_) {
        prev_ring_attempts_ = att;
        prev_ring_hits_ = hit;
        prev_ring_perfects_ = perf;
        prev_ring_misses_ = miss;
        prev_combo_hits_ = combo;
        observer_primed_ = true;
        return;
    }

    // Sanity bound against uninitialized RAM or title-screen resets
    const auto delta_ok = [](std::uint32_t cur, std::uint32_t prev) {
        return cur > prev && (cur - prev) <= 8u;
    };

    if (delta_ok(perf, prev_ring_perfects_)) {
        transport_.send_ring_callout(RingOutcome::Perfect, static_cast<std::uint8_t>(std::min<std::uint32_t>(combo, 99u)), "");
    } else if (delta_ok(miss, prev_ring_misses_)) {
        transport_.send_ring_callout(RingOutcome::Miss, 0, "");
    } else if (delta_ok(hit, prev_ring_hits_)) {
        if (settings_.relay_combos && delta_ok(combo, prev_combo_hits_) && combo >= 2) {
            transport_.send_ring_callout(RingOutcome::RelayCombo, static_cast<std::uint8_t>(std::min<std::uint32_t>(combo, 99u)), "");
            // Relay Combo baton pass to the next party slot owner!
            const std::uint8_t next_party = static_cast<std::uint8_t>((active_party_slot_ + 1u) % 4u);
            const std::uint8_t next_owner = settings_.slot_owner[next_party];
            if (next_owner < kMaxPlayers && next_owner != authority_owner_ && transport_.peers()[next_owner].connected) {
                grant_authority_to(next_owner, next_party, AuthorityReason::RelayCombo);
            }
        } else if (delta_ok(att, prev_ring_attempts_)) {
            transport_.send_ring_callout(RingOutcome::Strike, 1, "");
        }
    }

    prev_ring_attempts_ = att;
    prev_ring_hits_ = hit;
    prev_ring_perfects_ = perf;
    prev_ring_misses_ = miss;
    prev_combo_hits_ = combo;
}

void CoopSession::observe_koudelka_ram() {
    if (!settings_.auto_battle_turn || !hooks_.read_ram_u32 || !has_authority()) return;
    if (auto_switch_cooldown_ > 0) {
        --auto_switch_cooldown_;
        return;
    }
    if (transport_.state_transfer_active()) return;

    // Verify Koudelka (SLUS_010.51..54) main PS1 EXE signature at 0x8001705C ("Koud")
    if (hooks_.read_ram_u32(0x01705Cu) != 0x64756F4Bu) {
        kdl_observer_primed_ = false;
        kdl_pending_frames_ = 0;
        return;
    }

    // Global GameManager pointer at 0x800AF470
    const std::uint32_t gm = hooks_.read_ram_u32(0x0AF470u);
    if (gm < 0x80010000u || gm >= 0x801FF000u) return;
    const std::uint32_t gm_off = gm - 0x80000000u;

    // Overlay / mode markers verified from Koudelka SLUS_010.51 MIPS code:
    //  - 0x800B6DF0: active overlay ID (4 = Menu/Level-Up overlay, 5 = Field/Battle overlay)
    //  - 0x800D5DAC: sub-overlay header word when 0x800B6DF0 == 5 (9 = Battle, 10 = Field)
    //  - *(gm + 0x358): LevelUpBonusScreen* (non-null ONLY while Level-Up Bonus Points screen is active)
    //  - 0x800B4D5C: active player character index (0 = Koudelka, 1 = Edward, 2 = James),
    //                updated by Battle turn scheduler (0x800C6320) and Level-Up screen (0x800DCBE4 / 0x800DD4A4)
    const std::uint32_t ovl_id = hooks_.read_ram_u32(0x0B6DF0u);
    const std::uint32_t sub_ovl = hooks_.read_ram_u32(0x0D5DACu);
    const std::uint32_t lvlup_ptr = hooks_.read_ram_u32(gm_off + 0x358u);
    const std::uint32_t active_char_u32 = hooks_.read_ram_u32(0x0B4D5Cu);

    KdlPhase phase = KdlPhase::Other;
    if (ovl_id == 4u && lvlup_ptr >= 0x80010000u && lvlup_ptr < 0x801FF000u) {
        phase = KdlPhase::LevelUp;
    } else if (ovl_id == 5u && sub_ovl == 9u) {
        phase = KdlPhase::Battle;
    } else if (ovl_id == 5u && sub_ovl == 10u) {
        phase = KdlPhase::Field;
    }
    const std::uint8_t active_char = (active_char_u32 < 3u) ? static_cast<std::uint8_t>(active_char_u32) : 0xFFu;

    // 3-frame debounce to ignore transient mid-overlay-load states
    if (phase != kdl_pending_phase_ || active_char != kdl_pending_char_) {
        kdl_pending_phase_ = phase;
        kdl_pending_char_ = active_char;
        kdl_pending_frames_ = 1;
        return;
    }
    if (kdl_pending_frames_ < 3) {
        ++kdl_pending_frames_;
        if (kdl_pending_frames_ < 3) return;
    }

    if (!kdl_observer_primed_) {
        kdl_observer_primed_ = true;
        kdl_phase_ = phase;
        kdl_char_ = active_char;
        kdl_return_host_on_field_ = (phase == KdlPhase::Battle || phase == KdlPhase::LevelUp);
        return;
    }

    if ((phase == KdlPhase::Battle || phase == KdlPhase::LevelUp) && active_char < 3u) {
        kdl_return_host_on_field_ = true;
        if (phase != kdl_phase_ || active_char != kdl_char_) {
            kdl_phase_ = phase;
            kdl_char_ = active_char;

            std::uint8_t target_owner = settings_.slot_owner[active_char];
            if (target_owner >= kMaxPlayers || !transport_.peers()[target_owner].connected) {
                target_owner = 0; // Fallback to Host (P1) if assigned slot isn't connected
            }
            const AuthorityReason reason = (phase == KdlPhase::LevelUp)
                                               ? AuthorityReason::LevelUp
                                               : AuthorityReason::BattleTurn;
            if (target_owner != authority_owner_ && transport_.peers()[target_owner].connected) {
                grant_authority_to(target_owner, active_char, reason);
            } else {
                active_party_slot_ = active_char;
                const std::string cname = koudelka_char_name(active_char);
                if (phase == KdlPhase::LevelUp) {
                    push_banner("LEVEL UP — " + cname + "!", "Allocate Bonus Points", 0xFFE060FFu, 3000);
                } else {
                    push_banner("YOUR TURN — " + cname, "Battle Turn", 0xF0C850FFu, 2200);
                }
            }
        }
        return;
    }

    if (phase == KdlPhase::Field) {
        kdl_phase_ = phase;
        kdl_char_ = active_char;
        if (kdl_return_host_on_field_) {
            kdl_return_host_on_field_ = false;
            if (authority_owner_ != 0 && transport_.peers()[0].connected) {
                grant_authority_to(0, 0, AuthorityReason::FieldReturn);
            }
        }
        return;
    }

    // Other (loading / menus / FMV): keep kdl_return_host_on_field_ intact across overlay loads
    kdl_phase_ = phase;
    kdl_char_ = active_char;
}

void CoopSession::pass_authority_next(AuthorityReason reason) {
    if (!active_) return;
    const auto &peers = transport_.peers();
    if (!has_authority()) {
        // Ask current authority to pass to us
        transport_.send_authority_request(transport_.local_slot(), reason);
        push_banner("REQUESTED CONTROL", "Asked " + authority_owner_name() + " to pass the stick", 0x98C8FFFFu, 2000);
        return;
    }
    for (int step = 1; step < kMaxPlayers; ++step) {
        const std::uint8_t cand = static_cast<std::uint8_t>((authority_owner_ + step) % kMaxPlayers);
        if (peers[cand].connected) {
            const std::uint8_t next_party = static_cast<std::uint8_t>((active_party_slot_ + 1u) %
                                                                       (settings_.game == GameKind::Koudelka ? 3u : 4u));
            grant_authority_to(cand, next_party, reason);
            return;
        }
    }
}

void CoopSession::grant_authority_to(std::uint8_t target_player, std::uint8_t party_slot, AuthorityReason reason) {
    if (!active_ || target_player >= kMaxPlayers) return;
    if (!transport_.peers()[target_player].connected) return;

    // Send a compressed delta-state checkpoint right before handing off so the new authority starts 100% synced
    if (has_authority()) {
        broadcast_checkpoint(false, 0xFF);
    }

    ++authority_epoch_;
    authority_owner_ = target_player;
    active_party_slot_ = party_slot;
    jitter_.reset(0);
    auto_switch_cooldown_ = 30;
    kdl_pending_frames_ = 0;
    transport_.send_authority_grant(target_player, party_slot, reason, authority_epoch_);

    const bool is_local = (target_player == transport_.local_slot());
    if (settings_.game == GameKind::Koudelka) {
        const std::string cname = koudelka_char_name(party_slot);
        if (reason == AuthorityReason::LevelUp) {
            if (is_local) {
                push_banner("LEVEL UP — " + cname + "!", "Allocate Bonus Points", 0xFFE060FFu, 3200);
            } else {
                push_banner("LEVEL UP — " + cname, "Passed to " + authority_owner_name(), 0x8AA8E0FFu, 2400);
            }
        } else if (reason == AuthorityReason::FieldReturn) {
            push_banner("FIELD EXPLORATION", "Returned to " + authority_owner_name(), 0x8AA8E0FFu, 2000);
        } else if (reason == AuthorityReason::BattleTurn) {
            if (is_local) {
                push_banner("YOUR TURN — " + cname, "Battle Turn", 0xF0C850FFu, 2600);
            } else {
                push_banner("PASSED TO " + authority_owner_name() + " (" + cname + ")", "Battle Turn", 0x8AA8E0FFu, 2000);
            }
        } else if (is_local) {
            push_banner("YOUR TURN!", reason_label(reason), 0xF0C850FFu, 2600);
        } else {
            push_banner("PASSED TO " + authority_owner_name(), reason_label(reason), 0x8AA8E0FFu, 2000);
        }
    } else if (is_local) {
        push_banner("YOUR TURN — RING READY!", reason_label(reason), 0xF0C850FFu, 2600);
    } else {
        push_banner("PASSED TO " + authority_owner_name(), reason_label(reason), 0x8AA8E0FFu, 2000);
    }
}

void CoopSession::request_resync(bool force_full) {
    if (!active_) return;
    transport_.send_resync_request(force_full);
    if (force_full) {
        push_banner("RESYNCING...", "Requesting full checkpoint from authority", 0x80C8F0FFu, 1800);
    }
}

void CoopSession::broadcast_checkpoint(bool is_quickload, std::uint8_t target_slot, bool force_full) {
    if (!active_ || !hooks_.save_state_mem) return;
    std::vector<std::uint8_t> snap;
    if (!hooks_.save_state_mem(snap) || snap.empty()) return;

    const bool use_delta = (!force_full && !state_baseline_.empty() && state_baseline_.size() == snap.size());
    std::vector<std::uint8_t> compressed = compress_delta(snap, use_delta ? state_baseline_ : std::vector<std::uint8_t>{});
    std::uint8_t flags = (use_delta ? 0x01u : 0x00u) | (is_quickload ? 0x02u : 0x00u);
    state_baseline_ = std::move(snap);
    transport_.send_state_blob(compressed, flags, target_slot);
}

void CoopSession::mirror_memcard_file(const std::string &mc_path) {
    memcard_path_ = mc_path;
    if (!active_ || !has_authority() || !settings_.mirror_memcard || mc_path.empty()) return;
    std::ifstream ifs(mc_path, std::ios::binary);
    if (!ifs.is_open()) return;
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (raw.empty()) return;
    auto comp = compress_delta(raw, {});
    transport_.send_memcard_mirror(comp);
}

void CoopSession::share_trophy_unlock(const std::string &trophy_id, const std::string &trophy_name) {
    if (!active_ || !settings_.share_trophies) return;
    transport_.send_trophy_share(trophy_id, trophy_name);
}

void CoopSession::send_emote(EmoteKind emote) {
    if (!active_ || !settings_.allow_pings) return;
    transport_.send_emote_ping(emote);
    push_banner("SENT PING: " + std::string(emote_label(emote)), "Broadcast to partners", 0x98C8FFFFu, 1600);
}

void CoopSession::cycle_emote_ping() {
    send_emote(static_cast<EmoteKind>(next_emote_idx_ % 4));
    next_emote_idx_ = (next_emote_idx_ + 1) % 4;
}

void CoopSession::set_slot_owner(int party_slot, std::uint8_t player_slot) {
    if (party_slot < 0 || party_slot >= 4) return;
    settings_.slot_owner[party_slot] = player_slot & 3u;
    if (is_host()) transport_.broadcast_lobby_config(settings_);
}

void CoopSession::set_auto_battle_turn(bool v) {
    settings_.auto_battle_turn = v;
    if (is_host()) transport_.broadcast_lobby_config(settings_);
}

void CoopSession::set_relay_combos(bool v) {
    settings_.relay_combos = v;
    if (is_host()) transport_.broadcast_lobby_config(settings_);
}

void CoopSession::set_show_hud(bool v) {
    settings_.show_hud = v;
}

void CoopSession::set_allow_pings(bool v) {
    settings_.allow_pings = v;
    if (is_host()) transport_.broadcast_lobby_config(settings_);
}

std::uint32_t CoopSession::max_peer_rtt_ms() const {
    std::uint32_t m = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i != transport_.local_slot() && transport_.peers()[i].connected) {
            m = std::max(m, transport_.peers()[i].rtt_ms);
        }
    }
    return m;
}

std::string CoopSession::authority_owner_name() const {
    if (authority_owner_ < kMaxPlayers && !transport_.peers()[authority_owner_].name.empty()) {
        return transport_.peers()[authority_owner_].name;
    }
    return "P" + std::to_string(unsigned(authority_owner_) + 1u);
}

std::string CoopSession::status_summary() const {
    if (!active_) return "Solo";
    char buf[160];
    const int pct = transport_.state_transfer_percent();
    char sync_buf[32] = "";
    if (pct >= 0) {
        std::snprintf(sync_buf, sizeof(sync_buf), "  |  Sync %d%%", pct);
    }
    if (has_authority()) {
        std::snprintf(buf, sizeof(buf), "YOUR TURN  |  %s (P%u)  |  %dP  |  %u ms%s",
                      authority_owner_name().c_str(), unsigned(authority_owner_) + 1u,
                      connected_peers(), max_peer_rtt_ms(), sync_buf);
    } else {
        std::snprintf(buf, sizeof(buf), "WATCHING  |  %s (P%u)  |  %dP  |  %u ms%s",
                      authority_owner_name().c_str(), unsigned(authority_owner_) + 1u,
                      connected_peers(), max_peer_rtt_ms(), sync_buf);
    }
    return buf;
}

void CoopSession::push_banner(const std::string &title, const std::string &subtitle,
                              std::uint32_t accent_rgba, std::uint64_t duration_ms) {
    banner_.title = title;
    banner_.subtitle = subtitle;
    banner_.accent_rgba = accent_rgba;
    banner_.expires_ms = steady_now_ms() + duration_ms;
}

const HudBanner *CoopSession::active_banner() const {
    if (banner_.title.empty() || steady_now_ms() > banner_.expires_ms) return nullptr;
    return &banner_;
}

// ---- CLI helpers ----

void parse_coop_cli_args(int argc, char **argv, GameKind game, LaunchConfig &out) {
    out.settings.game = game;
    out.settings.max_players = (game == GameKind::Koudelka) ? 3 : 4;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--coop-host") {
            out.enabled = true;
            out.is_host = true;
        } else if (a == "--coop-join") {
            out.enabled = true;
            out.is_host = false;
            out.direct_addr = next();
        } else if (a == "--coop-code") {
            out.enabled = true;
            out.room_code = normalize_room_code(next());
        } else if (a == "--coop-relay") {
            out.relay_addr = next();
        } else if (a == "--coop-code-file") {
            out.code_out_file = next();
        } else if (a == "--coop-name") {
            out.player_name = next();
        } else if (a == "--coop-campaign") {
            out.settings.campaign_name = next();
        } else if (a == "--coop-port") {
            const int p = std::atoi(next().c_str());
            if (p > 0 && p <= 65535) out.local_port = static_cast<std::uint16_t>(p);
        } else if (a == "--coop-slots") {
            const std::string s = next();
            for (int k = 0; k < 4 && k < static_cast<int>(s.size()); ++k) {
                if (s[k] >= '0' && s[k] <= '3') out.settings.slot_owner[k] = static_cast<std::uint8_t>(s[k] - '0');
            }
        } else if (a == "--coop-auto-turn") {
            const std::string v = next();
            out.settings.auto_battle_turn = (v != "0" && v != "false" && v != "off");
        }
    }
}

std::string build_coop_cli_args(const LaunchConfig &cfg) {
    if (!cfg.enabled) return "";
    std::string out;
    if (cfg.is_host) {
        out += " --coop-host";
    } else if (!cfg.room_code.empty() && !cfg.relay_addr.empty()) {
        out += " --coop-code " + normalize_room_code(cfg.room_code);
    } else if (!cfg.direct_addr.empty()) {
        out += " --coop-join \"" + cfg.direct_addr + "\"";
    }
    if (!cfg.relay_addr.empty()) out += " --coop-relay \"" + cfg.relay_addr + "\"";
    if (!cfg.code_out_file.empty()) out += " --coop-code-file \"" + cfg.code_out_file + "\"";
    if (!cfg.player_name.empty()) out += " --coop-name \"" + cfg.player_name + "\"";
    if (!cfg.settings.campaign_name.empty()) out += " --coop-campaign \"" + sanitize_campaign(cfg.settings.campaign_name) + "\"";
    if (cfg.local_port != 0) out += " --coop-port " + std::to_string(cfg.local_port);
    char slots[5] = {
        static_cast<char>('0' + (cfg.settings.slot_owner[0] & 3u)),
        static_cast<char>('0' + (cfg.settings.slot_owner[1] & 3u)),
        static_cast<char>('0' + (cfg.settings.slot_owner[2] & 3u)),
        static_cast<char>('0' + (cfg.settings.slot_owner[3] & 3u)),
        '\0'
    };
    out += " --coop-slots ";
    out += slots;
    if (!cfg.settings.auto_battle_turn) {
        out += " --coop-auto-turn 0";
    }
    return out;
}

} // namespace coop
