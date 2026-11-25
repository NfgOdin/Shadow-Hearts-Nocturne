#pragma once

#include "coop_net.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace coop {

struct LaunchConfig {
    bool enabled = false;
    bool is_host = false;
    std::string player_name = "Player";
    std::string room_code;        // 6-char room code (for relay join)
    std::string direct_addr;      // host IP:port (for direct join)
    std::string relay_addr;       // optional relay host:port
    std::string code_out_file;    // optional file where host writes its live relay room code for Launcher handoff
    std::uint16_t local_port = kDefaultGamePort;
    LobbySettings settings{};
};

struct HudBanner {
    std::string title;
    std::string subtitle;
    std::uint32_t accent_rgba = 0xD8B458FFu; // gothic gold
    std::uint64_t expires_ms = 0;
};

struct SessionHooks {
    // Serialize current emulator state to uncompressed bytes
    std::function<bool(std::vector<std::uint8_t> &out)> save_state_mem;
    // Load uncompressed state bytes into emulator
    std::function<bool(const std::uint8_t *data, std::size_t size)> load_state_mem;
    // Compute lightweight 64-bit sync hash of current RAM
    std::function<std::uint64_t()> compute_ram_hash;
    // Read 32-bit little-endian word from emulated RAM (returns 0 if out of range)
    std::function<std::uint32_t(std::uint32_t addr)> read_ram_u32;
    // Called when a remote peer shares a trophy unlock
    std::function<void(const std::string &trophy_id, const std::string &trophy_name)> on_shared_trophy;
    // Called when a mirrored memory card arrives and is written to disk
    std::function<void(const std::string &mc_path)> on_memcard_mirrored;
};

// High-level Turn-Authority Co-op session controller shared by PS2 (SH1/SH2/SH3) and PSX (Koudelka) hosts.
class CoopSession {
public:
    CoopSession() = default;
    ~CoopSession() { stop(); }

    bool start(const LaunchConfig &cfg, const std::string &game_root_dir, SessionHooks hooks);
    void stop();
    bool active() const { return active_; }

    // Isolated co-op paths (never touch solo memcard or solo save states)
    std::string coop_save_dir() const { return coop_save_dir_; }
    std::string coop_memcard_path(const std::string &filename) const;
    std::string coop_state_dir() const;

    // Called once per emulated frame before stepping the CPU:
    // - If local player holds authority: uses local_pad with 0 ms lag and streams it to peers.
    // - If spectating: fills effective_pad from the adaptive 2-4 frame jitter buffer.
    // Returns true if effective_pad should be applied to controller port 0.
    bool tick_frame(std::uint32_t frame, const PadSample &local_pad, PadSample &effective_pad);

    // Authority handoff & user actions
    void pass_authority_next(AuthorityReason reason = AuthorityReason::ManualF6);
    void grant_authority_to(std::uint8_t target_player, std::uint8_t party_slot, AuthorityReason reason);
    void request_resync(bool force_full = false);
    void broadcast_checkpoint(bool is_quickload = false, std::uint8_t target_slot = 0xFF,
                              bool force_full = false, bool silent = false);
    void mirror_memcard_file(const std::string &mc_path);
    void drain_memcard_mirror(std::uint32_t timeout_ms = 2500);
    void share_trophy_unlock(const std::string &trophy_id, const std::string &trophy_name);
    void send_emote(EmoteKind emote);
    void cycle_emote_ping();

    // Settings updates (live from F1 -> CO-OP)
    void set_slot_owner(int party_slot, std::uint8_t player_slot);
    void set_auto_battle_turn(bool v);
    void set_relay_combos(bool v);
    void set_show_hud(bool v);
    void set_allow_pings(bool v);
    void set_debug_telemetry(bool enabled);
    bool debug_telemetry() const { return debug_telemetry_; }
    void set_engine_telemetry(const std::string &line) { engine_diag_line_ = line; }

    // Accessors for HUD and F1 -> CO-OP menu
    bool is_host() const { return transport_.is_host(); }
    bool has_authority() const { return !active_ || transport_.local_slot() == authority_owner_; }
    bool host_disconnected() const { return host_disconnected_.load(std::memory_order_acquire); }
    std::uint8_t local_slot() const { return transport_.local_slot(); }
    std::uint8_t authority_owner() const { return authority_owner_; }
    std::uint8_t active_party_slot() const { return active_party_slot_; }
    std::uint16_t authority_epoch() const { return authority_epoch_; }
    const std::string &room_code() const { return transport_.room_code(); }
    const LobbySettings &settings() const { return settings_; }
    const std::array<PeerInfo, kMaxPlayers> &peers() const { return transport_.peers(); }
    int connected_peers() const { return transport_.connected_peer_count(); }
    std::uint32_t max_peer_rtt_ms() const;
    std::string authority_owner_name() const;
    std::string status_summary() const;
    std::string kdl_turn_diag() const { return kdl_diag_line_; }

    // Active HUD banner (Turn Handoff / Ring Callout / Emote Ping), nullptr if expired
    const HudBanner *active_banner() const;

private:
    enum class KdlPhase : std::uint8_t {
        Unknown = 0,
        Field = 1,
        Battle = 2,
        LevelUp = 3,
        Other = 4,
    };

    void observe_battle_ram();
    void observe_koudelka_ram();
    void log_debug_event(const std::string &ev);
    void write_live_telemetry_if_due(bool force = false);
    void commit_authority_grant(std::uint8_t target_player, std::uint8_t party_slot, AuthorityReason reason);
    void maybe_auto_checkpoint();
    void push_banner(const std::string &title, const std::string &subtitle,
                     std::uint32_t accent_rgba, std::uint64_t duration_ms = 2600);
    void setup_callbacks();

    bool active_ = false;
    std::atomic<bool> host_disconnected_{false};
    LaunchConfig launch_{};
    LobbySettings settings_{};
    SessionHooks hooks_{};
    CoopTransport transport_{};
    InputJitterBuffer jitter_{};

    std::string coop_save_dir_;
    std::string memcard_path_;

    std::uint8_t authority_owner_ = 0;
    std::uint8_t active_party_slot_ = 0;
    std::uint16_t authority_epoch_ = 1;
    std::uint32_t last_frame_ = 0;
    std::uint64_t last_auto_checkpoint_ms_ = 0;
    std::uint64_t last_init_resync_ms_ = 0;
    std::uint64_t last_remote_input_ms_ = 0;
    std::uint64_t last_authority_nudge_ms_ = 0;
    std::uint64_t last_telemetry_ms_ = 0;
    std::uint64_t last_tick_wall_ms_ = 0;
    std::uint32_t checkpoints_sent_ = 0;
    std::uint32_t checkpoints_recv_ = 0;
    std::uint32_t hitch_count_ = 0;
    std::uint32_t max_frame_ms_ = 0;
    bool debug_telemetry_ = false;
    std::uint64_t prev_bytes_tx_ = 0;
    std::uint64_t prev_bytes_rx_ = 0;
    std::uint32_t last_ckpt_compress_ms_ = 0;
    std::uint32_t last_ckpt_raw_kb_ = 0;
    std::uint32_t last_ckpt_comp_kb_ = 0;
    std::uint32_t last_ckpt_load_ms_ = 0;
    std::string engine_diag_line_;
    std::deque<std::string> debug_events_;
    int sync_mismatch_streak_ = 0;
    std::uint32_t prev_authority_buttons_ = 0;
    bool prev_authority_buttons_valid_ = false;
    int next_emote_idx_ = 0;

    // Delta-state baseline (uncompressed last synced snapshot) + background Delta-LZ0 compressor
    std::vector<std::uint8_t> state_baseline_;
    struct PendingCompressedBlob {
        std::vector<std::uint8_t> payload;
        std::uint8_t flags = 0;
        std::uint8_t target_slot = 0xFF;
    };
    std::mutex compress_mtx_;
    std::thread compress_thread_;
    std::atomic<bool> compress_busy_{false};
    std::atomic<bool> compress_ready_{false};
    PendingCompressedBlob compress_out_{};
    void pump_compressed_blob();
    void wait_compress_idle();

    // RAM battle/ring observer previous counters (SH2 / SH3)
    bool observer_primed_ = false;
    std::uint32_t prev_ring_attempts_ = 0;
    std::uint32_t prev_ring_hits_ = 0;
    std::uint32_t prev_ring_perfects_ = 0;
    std::uint32_t prev_ring_misses_ = 0;
    std::uint32_t prev_combo_hits_ = 0;

    // Koudelka (PS1) battle turn & level-up bonus-points screen observer
    bool kdl_observer_primed_ = false;
    bool kdl_return_host_on_field_ = false;
    bool kdl_post_battle_levelup_ = false;
    KdlPhase kdl_phase_ = KdlPhase::Unknown;
    std::uint8_t kdl_char_ = 0xFFu;
    std::uint32_t kdl_room_id_ = 0u;
    KdlPhase kdl_pending_phase_ = KdlPhase::Unknown;
    std::uint8_t kdl_pending_char_ = 0xFFu;
    int kdl_pending_frames_ = 0;
    int auto_switch_cooldown_ = 0;
    std::uint64_t kdl_last_diag_sig_ = 0;
    std::string kdl_diag_line_;

    // Ordered Handoff: hold AuthorityGrant until the pre-handoff delta checkpoint is ACKed
    // so the new authority NEVER receives a stale checkpoint rollback after taking control.
    bool pending_grant_active_ = false;
    std::uint8_t pending_grant_target_ = 0;
    std::uint8_t pending_grant_party_ = 0;
    AuthorityReason pending_grant_reason_ = AuthorityReason::ManualF6;
    std::uint64_t pending_grant_started_ms_ = 0;

    HudBanner banner_{};
};

// Parse CLI flags (--coop-host, --coop-join, --coop-code, --coop-relay, --coop-name, --coop-campaign, --coop-port, --coop-slots)
void parse_coop_cli_args(int argc, char **argv, GameKind game, LaunchConfig &out);

// Build CLI arguments for spawning a co-op game process from the Gothic Launcher
std::string build_coop_cli_args(const LaunchConfig &cfg);

} // namespace coop
