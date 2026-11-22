#pragma once

#include "coop_net.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coop {

struct LaunchConfig {
    bool enabled = false;
    bool is_host = false;
    std::string player_name = "Player";
    std::string room_code;        // 6-char room code (for relay join)
    std::string direct_addr;      // host IP:port (for direct join)
    std::string relay_addr;       // optional relay host:port
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
    void request_resync();
    void broadcast_checkpoint(bool is_quickload = false, std::uint8_t target_slot = 0xFF);
    void mirror_memcard_file(const std::string &mc_path);
    void share_trophy_unlock(const std::string &trophy_id, const std::string &trophy_name);
    void send_emote(EmoteKind emote);
    void cycle_emote_ping();

    // Settings updates (live from F1 -> CO-OP)
    void set_slot_owner(int party_slot, std::uint8_t player_slot);
    void set_auto_battle_turn(bool v);
    void set_relay_combos(bool v);
    void set_show_hud(bool v);
    void set_allow_pings(bool v);

    // Accessors for HUD and F1 -> CO-OP menu
    bool is_host() const { return transport_.is_host(); }
    bool has_authority() const { return !active_ || transport_.local_slot() == authority_owner_; }
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

    // Active HUD banner (Turn Handoff / Ring Callout / Emote Ping), nullptr if expired
    const HudBanner *active_banner() const;

private:
    void observe_battle_ram();
    void push_banner(const std::string &title, const std::string &subtitle,
                     std::uint32_t accent_rgba, std::uint64_t duration_ms = 2600);
    void setup_callbacks();

    bool active_ = false;
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
    std::uint32_t last_hash_frame_ = 0;
    std::uint64_t last_resync_req_ms_ = 0;
    int next_emote_idx_ = 0;

    // Delta-state baseline (uncompressed last synced snapshot)
    std::vector<std::uint8_t> state_baseline_;

    // RAM battle/ring observer previous counters (SH2 / SH3)
    bool observer_primed_ = false;
    std::uint32_t prev_ring_attempts_ = 0;
    std::uint32_t prev_ring_hits_ = 0;
    std::uint32_t prev_ring_perfects_ = 0;
    std::uint32_t prev_ring_misses_ = 0;
    std::uint32_t prev_combo_hits_ = 0;

    HudBanner banner_{};
};

// Parse CLI flags (--coop-host, --coop-join, --coop-code, --coop-relay, --coop-name, --coop-campaign, --coop-port, --coop-slots)
void parse_coop_cli_args(int argc, char **argv, GameKind game, LaunchConfig &out);

// Build CLI arguments for spawning a co-op game process from the Gothic Launcher
std::string build_coop_cli_args(const LaunchConfig &cfg);

} // namespace coop
