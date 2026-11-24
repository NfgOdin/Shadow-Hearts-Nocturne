#include "coop_session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    host_disconnected_.store(false, std::memory_order_release);
    authority_epoch_ = 1;
    active_party_slot_ = 0;
    observer_primed_ = false;
    sync_mismatch_streak_ = 0;
    last_auto_checkpoint_ms_ = 0;
    last_init_resync_ms_ = 0;
    prev_authority_buttons_ = 0;
    prev_authority_buttons_valid_ = false;
    kdl_observer_primed_ = false;
    kdl_return_host_on_field_ = false;
    kdl_post_battle_levelup_ = false;
    kdl_phase_ = KdlPhase::Unknown;
    kdl_char_ = 0xFFu;
    kdl_room_id_ = 0u;
    kdl_pending_phase_ = KdlPhase::Unknown;
    kdl_pending_char_ = 0xFFu;
    kdl_pending_frames_ = 0;
    auto_switch_cooldown_ = 0;
    kdl_last_diag_sig_ = 0;
    pending_grant_active_ = false;
    pending_grant_started_ms_ = 0;
    jitter_.reset(0);
    push_banner(cfg.is_host ? "CO-OP ROOM HOSTED" : "CONNECTING TO CO-OP...",
                cfg.is_host ? ("Campaign: " + settings_.campaign_name) : "Syncing with host...",
                0xD8B458FFu, 3200);
    return true;
}

void CoopSession::stop() {
    wait_compress_idle();
    if (!active_) return;
    transport_.close();
    state_baseline_.clear();
    host_disconnected_.store(false, std::memory_order_release);
    active_ = false;
}

void CoopSession::wait_compress_idle() {
    if (compress_thread_.joinable()) compress_thread_.join();
    compress_busy_.store(false, std::memory_order_release);
    compress_ready_.store(false, std::memory_order_release);
}

void CoopSession::pump_compressed_blob() {
    if (!compress_ready_.load(std::memory_order_acquire)) return;
    if (compress_thread_.joinable()) compress_thread_.join();
    PendingCompressedBlob ready;
    {
        std::lock_guard<std::mutex> lk(compress_mtx_);
        ready = std::move(compress_out_);
    }
    compress_ready_.store(false, std::memory_order_release);
    compress_busy_.store(false, std::memory_order_release);
    if (active_ && !ready.payload.empty()) {
        transport_.send_state_blob(ready.payload, ready.flags, ready.target_slot);
    }
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
        if (!is_host() && slot == 0) {
            // The Host ended the co-op session (Leave packet or heartbeat timeout)
            host_disconnected_.store(true, std::memory_order_release);
            push_banner("HOST ENDED SESSION", "Returning to Launcher...", 0xE05050FFu, 4000);
            return;
        }
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
                kdl_phase_ = KdlPhase::Unknown;
                kdl_char_ = party_slot;
                kdl_return_host_on_field_ = false;
                kdl_post_battle_levelup_ = false;
            } else if (reason == AuthorityReason::LevelUp) {
                kdl_observer_primed_ = true;
                kdl_phase_ = KdlPhase::LevelUp;
                kdl_char_ = party_slot;
                kdl_return_host_on_field_ = true;
                kdl_post_battle_levelup_ = true;
            } else if (reason == AuthorityReason::FieldReturn) {
                kdl_observer_primed_ = true;
                kdl_phase_ = KdlPhase::Field;
                // Adopt live 0x800B4D5C (may still hold last battle actor until next battle starts)
                const std::uint32_t live_c = hooks_.read_ram_u32 ? hooks_.read_ram_u32(0x0B4D5Cu) : 0u;
                kdl_char_ = (live_c < 3u) ? static_cast<std::uint8_t>(live_c) : party_slot;
                kdl_return_host_on_field_ = false;
                kdl_post_battle_levelup_ = false;
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
        const bool is_quickload = (flags & 0x02u) != 0;
        // Hard safety guard: never let a late/in-flight checkpoint roll back the active controller owner!
        if (has_authority() && !is_quickload) return;
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
            sync_mismatch_streak_ = 0;
            jitter_.reset(0);
            if (is_quickload) {
                push_banner("SYNCED QUICK-LOAD", "Loaded partner quick-state", 0x80C8F0FFu, 2200);
            } else if ((flags & 0x04u) == 0) {
                push_banner("CO-OP SYNCED", "Matched host game state", 0x70D898FFu, 2000);
            }
        }
    };

    cb.on_sync_hash = [](std::uint8_t /*sender*/, std::uint32_t /*frame*/, std::uint64_t /*remote_hash*/) {
        // NOTE: Do NOT trigger automatic state reloads on frame-skewed live RAM hashes.
        // Because spectators intentionally trail the authority by 2-4 frames in InputJitterBuffer,
        // live RAM hashes naturally differ on animated frames; forcing reloads every 2s causes
        // snap-back / doubled actions and starves battle turn detection. Sync is maintained cleanly
        // via initial join keyframe, ordered authority handoff, room transitions, and F8 quick-load.
    };

    cb.on_resync_request = [this](std::uint8_t sender, bool force_full) {
        if (!has_authority() || compress_busy_.load(std::memory_order_acquire) || transport_.state_transfer_active()) return;
        // Targeted single-peer resync sends a full keyframe without clobbering the shared baseline for other peers.
        broadcast_checkpoint(false, sender, true, !force_full);
    };

    cb.on_memcard_mirror = [this](std::uint8_t sender, const std::vector<std::uint8_t> &compressed_mc) {
        if (sender == transport_.local_slot() || !settings_.mirror_memcard || memcard_path_.empty()) return;
        std::vector<std::uint8_t> raw;
        if (!decompress_delta(compressed_mc, {}, raw)) return;
        const std::filesystem::path dst(memcard_path_);
        const std::filesystem::path tmp = dst.string() + ".tmp";
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (ofs.is_open()) {
            ofs.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
            ofs.flush();
            const bool write_ok = ofs.good();
            ofs.close();
            if (write_ok) {
                bool replaced = false;
#ifdef _WIN32
                replaced = MoveFileExW(tmp.wstring().c_str(), dst.wstring().c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                std::error_code ec;
                std::filesystem::rename(tmp, dst, ec);
                replaced = !ec;
#endif
                if (replaced) {
                    push_banner("CO-OP SAVE MIRRORED", "Campaign memory card backed up locally", 0x70D898FFu, 2400);
                    if (hooks_.on_memcard_mirrored) hooks_.on_memcard_mirrored(memcard_path_);
                }
            }
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
    pump_compressed_blob();
    transport_.poll();

    // Complete an ordered authority handoff the instant all peers have ACKed the pre-handoff checkpoint
    if (pending_grant_active_) {
        const std::uint64_t elapsed = steady_now_ms() - pending_grant_started_ms_;
        if ((!compress_busy_.load(std::memory_order_acquire) && !transport_.state_transfer_active()) || elapsed >= 1500) {
            const std::uint8_t tgt = pending_grant_target_;
            const std::uint8_t pty = pending_grant_party_;
            const AuthorityReason rsn = pending_grant_reason_;
            pending_grant_active_ = false;
            commit_authority_grant(tgt, pty, rsn);
        } else {
            // Keep pad neutral while the tiny Delta-LZ0 handoff checkpoint finishes (~10-40ms)
            // so no extra inputs advance past the captured snapshot.
            const bool active_low = (settings_.game == GameKind::Koudelka);
            effective_pad = PadSample{};
            effective_pad.buttons = active_low ? 0xFFFFu : 0x0000u;
            effective_pad.frame = frame;
            return true;
        }
    }

    if (has_authority()) {
        // Zero-lag local authority: execute immediately on local machine and stream to peers
        effective_pad = local_pad;
        effective_pad.frame = frame;
        transport_.send_input_frame(effective_pad, authority_epoch_);

        // Observe SH2/SH3 Judgment Ring & Combo RAM or Koudelka Battle/Level-Up RAM
        observe_battle_ram();

        // Gentle spectator safety sync on Confirm (Cross = bit 14) rising edge (throttled to >=2s):
        // keeps the spectator snapped into battle Move/Attack animations if a D-pad tap ever drifted,
        // while Hard Rollback Guard guarantees the active player is NEVER rolled back or doubled.
        if (!pending_grant_active_) {
            const bool active_low = (settings_.game == GameKind::Koudelka);
            const std::uint32_t pressed_mask = active_low ? (~local_pad.buttons & 0xFFFFu) : (local_pad.buttons & 0xFFFFu);
            if (prev_authority_buttons_valid_) {
                constexpr std::uint32_t kConfirm = (1u << 14); // Cross / X
                if (((pressed_mask & ~prev_authority_buttons_) & kConfirm) != 0) {
                    maybe_auto_checkpoint();
                }
            }
            prev_authority_buttons_ = pressed_mask;
            prev_authority_buttons_valid_ = true;
        }
        return true;
    }

    // Spectator: if we're connected to the host but haven't received the initial keyframe yet,
    // politely re-request it every 4s until loaded in (never triggers once loaded in).
    if (state_baseline_.empty() && connected_peers() > 1 && !transport_.state_transfer_active()) {
        const std::uint64_t now = steady_now_ms();
        if (last_init_resync_ms_ == 0) {
            last_init_resync_ms_ = now;
        } else if (now - last_init_resync_ms_ >= 4000) {
            last_init_resync_ms_ = now;
            request_resync(true);
        }
    }

    // Spectator: strict monotonic 1-frame-per-tick playout from InputJitterBuffer
    const bool active_low = (settings_.game == GameKind::Koudelka);
    const bool hold_on_stall = (settings_.game != GameKind::Koudelka);
    if (!jitter_.pop_next(effective_pad, active_low, hold_on_stall)) {
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
    if (!hooks_.read_ram_u32 || !has_authority()) return;

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

    const auto is_ps1_ptr = [](std::uint32_t p) {
        return p >= 0x80010000u && p < 0x801FF000u;
    };

    // Room-change auto-checkpoint (keeps guest snapped to host on every door/screen transition)
    const std::uint32_t room_id = hooks_.read_ram_u32(0x0B6B08u);
    if (kdl_observer_primed_ && room_id != 0u && room_id < 0x400u && room_id != kdl_room_id_) {
        kdl_room_id_ = room_id;
        maybe_auto_checkpoint();
    } else if (!kdl_observer_primed_ && room_id != 0u && room_id < 0x400u) {
        kdl_room_id_ = room_id;
    }

    if (!settings_.auto_battle_turn) return;
    if (pending_grant_active_) return;
    if (auto_switch_cooldown_ > 0) {
        --auto_switch_cooldown_;
        return;
    }

    // Canonical Koudelka SLUS_010.51..54 state markers verified from MIPS disassembly:
    //  - *(gm + 0x358): LevelUpBonusScreen* (non-null ONLY while Level-Up Bonus Points screen is open)
    //  - *(gm + 0x36C): canonical is_in_battle flag (1 = Battle set by 0x800CECC4, 0 = Field set by 0x8007CA0C)
    //  - *(gm + 0x378): scene mode (1 = Menu/Level-Up overlay, 2 = Field/Battle)
    //  - *(gm + 0x32C): active scene object; in Battle, *(scene + 0xA0) is the active turn actor pointer
    //                   whose byte at +0x4BC is char_id (0=Koudelka, 1=Edward, 2=James, >=3=Enemy)
    //  - 0x800B6DF0: active overlay ID (4 = Menu/Level-Up, 5 = Field/Battle)
    //  - 0x800D5DAC: sub-overlay marker (9 = Battle EFFECT.BIN, 10 = Field)
    //  - 0x800B4D5C: active player character index (0..2), written by 0x800538B4 on Battle turns & Level-Up
    const std::uint32_t ovl_id = hooks_.read_ram_u32(0x0B6DF0u);
    const std::uint32_t sub_ovl = hooks_.read_ram_u32(0x0D5DACu);
    const std::uint32_t scene_ptr = hooks_.read_ram_u32(gm_off + 0x32Cu);
    const std::uint32_t lvlup_ptr = hooks_.read_ram_u32(gm_off + 0x358u);
    const std::uint32_t battle_flag = hooks_.read_ram_u32(gm_off + 0x36Cu);
    const std::uint32_t scene_mode = hooks_.read_ram_u32(gm_off + 0x378u);
    const std::uint32_t active_char_u32 = hooks_.read_ram_u32(0x0B4D5Cu);

    KdlPhase phase = KdlPhase::Other;
    const bool was_post_battle_levelup = kdl_post_battle_levelup_;
    if (is_ps1_ptr(lvlup_ptr)) {
        phase = KdlPhase::LevelUp;
        kdl_post_battle_levelup_ = true;
    } else if (battle_flag == 1u || (ovl_id == 5u && sub_ovl == 9u)) {
        // Once post-battle Level-Up has started, Koudelka never returns to the same Battle;
        // ignore lingering battle_flag==1 frames during the post-level-up fade so we NEVER
        // falsely hand control to whoever took the last turn before the enemy died.
        phase = kdl_post_battle_levelup_ ? KdlPhase::Other : KdlPhase::Battle;
    } else if (battle_flag == 0u && ovl_id == 5u && (sub_ovl == 10u || scene_mode == 2u)) {
        phase = KdlPhase::Field;
    }

    std::uint8_t active_char = (active_char_u32 < 3u) ? static_cast<std::uint8_t>(active_char_u32) : 0xFFu;
    std::uint32_t diag_actor = 0u;
    std::uint32_t diag_cid = 0xFFu;
    if (phase == KdlPhase::Battle && is_ps1_ptr(scene_ptr)) {
        const std::uint32_t actor_ptr = hooks_.read_ram_u32((scene_ptr - 0x80000000u) + 0xA0u);
        diag_actor = actor_ptr;
        if (is_ps1_ptr(actor_ptr)) {
            const std::uint32_t cid = hooks_.read_ram_u32((actor_ptr - 0x80000000u) + 0x4BCu) & 0xFFu;
            diag_cid = cid;
            if (cid < 3u) {
                active_char = static_cast<std::uint8_t>(cid);
            }
        }
    }

    // Lightweight change-only diagnostic trace for live battle turn troubleshooting
    {
        const std::uint64_t sig = (static_cast<std::uint64_t>(phase) << 40) |
                                  (static_cast<std::uint64_t>(battle_flag & 0xFFu) << 32) |
                                  (static_cast<std::uint64_t>(ovl_id & 0xFFu) << 24) |
                                  (static_cast<std::uint64_t>(sub_ovl & 0xFFu) << 16) |
                                  (static_cast<std::uint64_t>(active_char) << 8) |
                                  static_cast<std::uint64_t>(diag_cid & 0xFFu);
        if (sig != kdl_last_diag_sig_) {
            kdl_last_diag_sig_ = sig;
            char hud_buf[160];
            std::snprintf(hud_buf, sizeof(hud_buf),
                          "CO-OP P%u | ph=%d bf=%u ov=%u/%u sc=%X ac=%X cid=%u b4=%u act=%u sl=%u,%u,%u",
                          unsigned(authority_owner_) + 1u, static_cast<int>(phase), battle_flag, ovl_id, sub_ovl,
                          scene_ptr & 0xFFFFFu, diag_actor & 0xFFFFFu, diag_cid, active_char_u32, unsigned(active_char),
                          unsigned(settings_.slot_owner[0]) + 1u, unsigned(settings_.slot_owner[1]) + 1u, unsigned(settings_.slot_owner[2]) + 1u);
            kdl_diag_line_ = hud_buf;
            if (!coop_save_dir_.empty()) {
                std::ofstream dlog(std::filesystem::path(coop_save_dir_) / "kdl_turns.log", std::ios::app);
                if (dlog.is_open()) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "f=%u phase=%d bflag=%u ovl=%u sub=%u mode=%u scene=0x%08X actor=0x%08X cid=%u b4d5c=%u active=%u owner=P%u slots=%u,%u,%u\n",
                                  last_frame_, static_cast<int>(phase), battle_flag, ovl_id, sub_ovl, scene_mode,
                                  scene_ptr, diag_actor, diag_cid, active_char_u32, unsigned(active_char),
                                  unsigned(authority_owner_) + 1u,
                                  unsigned(settings_.slot_owner[0]), unsigned(settings_.slot_owner[1]), unsigned(settings_.slot_owner[2]));
                    dlog << buf;
                }
            }
        }
    }

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
        kdl_return_host_on_field_ = (phase == KdlPhase::Battle || phase == KdlPhase::LevelUp ||
                                     active_char == 1u || active_char == 2u);
        std::fprintf(stderr, "[coop][kdl] primed phase=%d char=%u ovl=%u sub=%u bflag=%u lvlup=0x%08X slots=%u,%u,%u\n",
                     static_cast<int>(phase), unsigned(active_char), ovl_id, sub_ovl, battle_flag, lvlup_ptr,
                     unsigned(settings_.slot_owner[0]), unsigned(settings_.slot_owner[1]), unsigned(settings_.slot_owner[2]));
        // If we primed directly onto Edward (1) or James (2) (e.g. via F8 Load State mid-battle),
        // hand control immediately to their assigned player instead of waiting for the next turn.
        if (active_char == 1u || active_char == 2u) {
            std::uint8_t target_owner = settings_.slot_owner[active_char];
            if (target_owner >= kMaxPlayers || !transport_.peers()[target_owner].connected) {
                target_owner = 0;
            }
            if (target_owner != authority_owner_ && transport_.peers()[target_owner].connected) {
                const AuthorityReason reason = (phase == KdlPhase::LevelUp)
                                                   ? AuthorityReason::LevelUp
                                                   : AuthorityReason::BattleTurn;
                grant_authority_to(target_owner, active_char, reason);
            }
        }
        return;
    }

    // Verified on live Koudelka (SLUS_010.51..54): during real battles, ovl=5/sub=10 and gm+0x36C
    // can both stay 0/10 while 0x800B4D5C actively tracks the current turn character (0=Koudelka,
    // 1=Edward, 2=James). Trigger turn handoff whenever active_char changes on ovl 4/5, OR when
    // entering Battle/LevelUp (but NOT when returning from LevelUp -> Field).
    const bool returning_from_levelup = (kdl_phase_ == KdlPhase::LevelUp && phase == KdlPhase::Field);
    const bool char_changed_in_play = (!returning_from_levelup && active_char < 3u && active_char != kdl_char_ &&
                                       (ovl_id == 4u || ovl_id == 5u) && !kdl_post_battle_levelup_);
    if (((phase == KdlPhase::Battle || phase == KdlPhase::LevelUp) && active_char < 3u) || char_changed_in_play) {
        if (phase == KdlPhase::Battle || phase == KdlPhase::LevelUp) {
            kdl_return_host_on_field_ = true;
        }
        if (phase != kdl_phase_ || active_char != kdl_char_) {
            const bool phase_changed = (phase != kdl_phase_);
            kdl_phase_ = phase;
            kdl_char_ = active_char;

            std::uint8_t target_owner = settings_.slot_owner[active_char];
            if (target_owner >= kMaxPlayers || !transport_.peers()[target_owner].connected) {
                target_owner = 0; // Fallback to Host (P1) if assigned slot isn't connected
            }
            std::fprintf(stderr, "[coop][kdl] transition phase=%d char=%u (%s) bflag=%u owner=P%u->P%u\n",
                         static_cast<int>(phase), unsigned(active_char), koudelka_char_name(active_char),
                         battle_flag, unsigned(authority_owner_) + 1u, unsigned(target_owner) + 1u);
            const AuthorityReason reason = (phase == KdlPhase::LevelUp)
                                               ? AuthorityReason::LevelUp
                                               : AuthorityReason::BattleTurn;
            if (target_owner != authority_owner_ && transport_.peers()[target_owner].connected) {
                grant_authority_to(target_owner, active_char, reason);
            } else {
                active_party_slot_ = active_char;
                if (phase_changed) maybe_auto_checkpoint();
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
        const bool phase_changed = (kdl_phase_ != KdlPhase::Field);
        const bool was_explicit_battle_or_lvlup = (kdl_phase_ == KdlPhase::Battle || kdl_phase_ == KdlPhase::LevelUp || was_post_battle_levelup);
        kdl_post_battle_levelup_ = false;
        kdl_phase_ = phase;
        kdl_char_ = active_char;
        if (kdl_return_host_on_field_ && (was_explicit_battle_or_lvlup || active_char == 0u)) {
            kdl_return_host_on_field_ = false;
            if (authority_owner_ != 0 && transport_.peers()[0].connected) {
                grant_authority_to(0, 0, AuthorityReason::FieldReturn);
            } else if (phase_changed) {
                maybe_auto_checkpoint();
            }
        } else if (phase_changed) {
            maybe_auto_checkpoint();
        }
        return;
    }

    // Other (loading / menus / FMV): keep kdl_return_host_on_field_ intact across overlay loads
    kdl_phase_ = phase;
    kdl_char_ = active_char;
}

void CoopSession::maybe_auto_checkpoint() {
    if (!active_ || !has_authority() || !hooks_.save_state_mem) return;
    if (connected_peers() <= 1 || transport_.state_transfer_active()) return;
    const std::uint64_t now = steady_now_ms();
    if (last_auto_checkpoint_ms_ != 0 && now - last_auto_checkpoint_ms_ < 2000) return;
    last_auto_checkpoint_ms_ = now;
    broadcast_checkpoint(false, 0xFF, false, true);
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
    if (pending_grant_active_) return;

    // Ordered Handoff: send a silent Delta-LZ0 checkpoint BEFORE transferring authority,
    // and defer AuthorityGrant until peers ACK the checkpoint so the new controller owner
    // NEVER receives a late checkpoint rollback that snaps back or doubles actions.
    if (has_authority() && target_player != transport_.local_slot() && hooks_.save_state_mem) {
        broadcast_checkpoint(false, 0xFF, false, true);
        if (compress_busy_.load(std::memory_order_acquire) || transport_.state_transfer_active()) {
            pending_grant_active_ = true;
            pending_grant_target_ = target_player;
            pending_grant_party_ = party_slot;
            pending_grant_reason_ = reason;
            pending_grant_started_ms_ = steady_now_ms();
            return;
        }
    }
    commit_authority_grant(target_player, party_slot, reason);
}

void CoopSession::commit_authority_grant(std::uint8_t target_player, std::uint8_t party_slot, AuthorityReason reason) {
    if (!active_ || target_player >= kMaxPlayers) return;
    if (!transport_.peers()[target_player].connected) return;

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

void CoopSession::broadcast_checkpoint(bool is_quickload, std::uint8_t target_slot, bool force_full, bool silent) {
    if (!active_ || !hooks_.save_state_mem) return;
    wait_compress_idle();
    std::vector<std::uint8_t> snap;
    if (!hooks_.save_state_mem(snap) || snap.empty()) return;

    const bool is_broadcast = (target_slot == 0xFF);
    const bool use_delta = (is_broadcast && !force_full && !state_baseline_.empty() && state_baseline_.size() == snap.size());
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (use_delta ? 0x01u : 0x00u) | (is_quickload ? 0x02u : 0x00u) | (silent ? 0x04u : 0x00u));

    std::vector<std::uint8_t> prev_base;
    if (is_broadcast) {
        if (use_delta) prev_base = std::move(state_baseline_);
        state_baseline_ = snap;
    }

    // Offload Delta-LZ0 compression to a background thread so 40 MiB PS2 states never hitch the window thread.
    compress_busy_.store(true, std::memory_order_release);
    compress_ready_.store(false, std::memory_order_release);
    compress_thread_ = std::thread([this, cur = std::move(snap), base = std::move(prev_base), flags, target_slot]() mutable {
        auto compressed = compress_delta(cur, base);
        {
            std::lock_guard<std::mutex> lk(compress_mtx_);
            compress_out_.payload = std::move(compressed);
            compress_out_.flags = flags;
            compress_out_.target_slot = target_slot;
        }
        compress_ready_.store(true, std::memory_order_release);
    });
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

void CoopSession::drain_memcard_mirror(std::uint32_t timeout_ms) {
    if (!active_) return;
    const std::uint64_t deadline = steady_now_ms() + timeout_ms;
    while (transport_.memcard_transfer_active() && steady_now_ms() < deadline) {
        transport_.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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
