#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace coop {

constexpr std::uint8_t kProtoVersion = 1;
constexpr std::uint16_t kDefaultGamePort = 47192;
constexpr std::uint16_t kLanBeaconPort = 47190;
constexpr std::uint16_t kDefaultRelayPort = 47191;
constexpr int kMaxPlayers = 4;
constexpr std::size_t kChunkPayloadBytes = 1100;

enum class GameKind : std::uint8_t {
    Koudelka = 0,
    SH1 = 1,
    SH2 = 2,
    SH3 = 3,
};

enum class PacketType : std::uint8_t {
    // Peer-to-peer / Lobby / Gameplay
    Hello = 0x01,
    Welcome = 0x02,
    Heartbeat = 0x03,
    HeartbeatAck = 0x04,
    LobbyConfig = 0x05,
    ReadyToggle = 0x06,
    LaunchTogether = 0x07,
    Leave = 0x08,

    InputFrame = 0x10,
    AuthorityGrant = 0x11,
    AuthorityAck = 0x12,
    AuthorityRequest = 0x13,

    StateChunk = 0x20,
    StateAck = 0x21,
    SyncHash = 0x22,
    ResyncRequest = 0x23,

    MemcardChunk = 0x30,
    MemcardAck = 0x31,
    QuickSaveNotify = 0x32,
    QuickLoadTrigger = 0x33,

    RingCallout = 0x40,
    TrophyShare = 0x41,
    EmotePing = 0x42,

    LanBeacon = 0x50,

    // Signaling & Relay server (matches tools/coop_relay/coop_relay.py)
    RelayCreateRoom = 0x80,
    RelayRoomCreated = 0x81,
    RelayJoinRoom = 0x82,
    RelayJoinOk = 0x83,
    RelayPeerEndpoint = 0x84,
    RelayJoinErr = 0x85,
    RelayForward = 0x86,
    RelayKeepalive = 0x87,
    RelayLeave = 0x88,
};

enum class AuthorityReason : std::uint8_t {
    Initial = 0,
    BattleTurn = 1,
    RelayCombo = 2,
    PassStick = 3,
    ManualF6 = 4,
};

enum class RingOutcome : std::uint8_t {
    Hit = 0,
    Strike = 1,
    Perfect = 2,
    Miss = 3,
    RelayCombo = 4,
};

enum class EmoteKind : std::uint8_t {
    Nice = 0,
    Heal = 1,
    Combo = 2,
    PassStick = 3,
};

struct Endpoint {
    std::uint32_t ipv4 = 0; // host byte order (e.g. 0x7F000001 for 127.0.0.1)
    std::uint16_t port = 0; // host byte order

    bool valid() const { return ipv4 != 0 && port != 0; }
    bool operator==(const Endpoint &o) const { return ipv4 == o.ipv4 && port == o.port; }
    bool operator!=(const Endpoint &o) const { return !(*this == o); }
    std::string to_string() const;
    static Endpoint parse(const std::string &host_port, std::uint16_t default_port = kDefaultGamePort);
};

struct PadSample {
    std::uint32_t frame = 0;
    std::uint32_t buttons = 0xFFFFu; // active-low PS1/PS2 button mask (0xFFFF = all released)
    std::uint8_t lx = 0x80;
    std::uint8_t ly = 0x80;
    std::uint8_t rx = 0x80;
    std::uint8_t ry = 0x80;
};

struct PeerInfo {
    bool connected = false;
    bool ready = false;
    bool direct_ok = false; // true once a direct non-relayed packet is received
    std::uint8_t slot = 0;
    std::string name;
    Endpoint direct_ep;
    std::uint32_t rtt_ms = 0;
    std::uint64_t last_seen_ms = 0;
    std::uint64_t last_ping_sent_ms = 0;
};

struct LobbySettings {
    GameKind game = GameKind::SH2;
    std::uint8_t max_players = 4;
    std::string campaign_name = "nocturne_coop";
    std::array<std::uint8_t, 4> slot_owner = {0, 1, 2, 3}; // party slot 0..3 -> player id 0..3
    bool auto_battle_turn = true;
    bool relay_combos = true;
    bool pass_the_stick = true;
    bool show_hud = true;
    bool allow_pings = true;
    bool share_trophies = true;
    bool mirror_memcard = true;
};

struct LanRoomInfo {
    std::string host_name;
    std::string room_code;
    std::string campaign_name;
    GameKind game = GameKind::SH2;
    std::uint8_t player_count = 1;
    std::uint8_t max_players = 4;
    Endpoint endpoint;
    std::uint64_t last_seen_ms = 0;
};

// --- Utilities & Delta Compression ---
std::uint32_t crc32(const std::uint8_t *data, std::size_t len);
std::uint32_t fnv1a_32(const std::uint8_t *data, std::size_t len);
std::uint64_t fnv1a_64(const std::uint8_t *data, std::size_t len);
std::uint64_t steady_now_ms();
std::string format_room_code_pretty(const std::string &code); // "K7M4PX" -> "K7M-4PX"
std::string normalize_room_code(const std::string &input);    // "k7m-4px" -> "K7M4PX"

// Fast XOR-delta + zero-run RLE compression (zero external deps, ~500:1 on unchanged RAM)
std::vector<std::uint8_t> compress_delta(const std::vector<std::uint8_t> &current,
                                         const std::vector<std::uint8_t> &baseline);
bool decompress_delta(const std::vector<std::uint8_t> &compressed,
                      const std::vector<std::uint8_t> &baseline,
                      std::vector<std::uint8_t> &out);

// --- Adaptive 2-4 Frame Input Jitter Buffer ---
class InputJitterBuffer {
public:
    void reset(std::uint32_t start_frame = 0);
    void push(const PadSample &sample);
    // Returns true if an exact or interpolated sample is available for target_frame
    bool pop_for_frame(std::uint32_t target_frame, PadSample &out);
    std::size_t buffered_count() const { return frames_.size(); }
    std::uint32_t latest_frame() const { return latest_frame_; }

private:
    std::map<std::uint32_t, PadSample> frames_;
    PadSample last_returned_{};
    bool has_last_ = false;
    std::uint32_t latest_frame_ = 0;
};

// --- Reliable Fragmented Blob Transfer (for Delta States & Mirrored Memcards) ---
struct BlobTransferIn {
    std::uint32_t blob_id = 0;
    std::uint32_t total_bytes = 0;
    std::uint32_t total_chunks = 0;
    std::uint32_t expected_crc = 0;
    std::uint8_t flags = 0; // bit 0 = is_delta, bit 1 = is_quickload
    std::vector<std::uint8_t> data;
    std::vector<bool> received;
    std::uint32_t received_count = 0;

    void begin(std::uint32_t id, std::uint32_t total, std::uint32_t crc, std::uint8_t fl);
    bool add_chunk(std::uint32_t chunk_idx, const std::uint8_t *bytes, std::size_t len);
    bool complete() const { return total_chunks > 0 && received_count == total_chunks; }
    bool verify_crc() const { return complete() && crc32(data.data(), data.size()) == expected_crc; }
};

struct BlobTransferOut {
    std::uint32_t blob_id = 0;
    std::uint32_t expected_crc = 0;
    std::uint8_t flags = 0;
    std::uint8_t target_slot = 0xFF; // 0xFF = all peers
    std::vector<std::uint8_t> data;
    std::uint32_t total_chunks = 0;
    std::vector<bool> acked;
    std::uint32_t acked_count = 0;
    std::uint64_t last_send_ms = 0;
    std::uint32_t next_cursor = 0;

    void begin(std::uint32_t id, std::vector<std::uint8_t> payload, std::uint8_t fl, std::uint8_t target = 0xFF);
    bool done() const { return total_chunks == 0 || acked_count >= total_chunks; }
};

// --- Callbacks delivered by CoopTransport::poll() ---
struct TransportCallbacks {
    std::function<void(const std::string &room_code, std::uint32_t room_id)> on_room_created;
    std::function<void(std::uint8_t assigned_slot, std::uint32_t room_id)> on_joined_room;
    std::function<void(const std::string &reason)> on_error;
    std::function<void(std::uint8_t slot, const PeerInfo &peer)> on_peer_joined;
    std::function<void(std::uint8_t slot)> on_peer_left;
    std::function<void(const LobbySettings &cfg)> on_lobby_config;
    std::function<void(const LobbySettings &cfg, std::uint16_t game_port)> on_launch_together;
    std::function<void(std::uint8_t sender, const PadSample &sample, std::uint16_t epoch)> on_input_frame;
    std::function<void(std::uint8_t new_owner, std::uint8_t party_slot, AuthorityReason reason, std::uint16_t epoch)> on_authority_grant;
    std::function<void(std::uint8_t requester, std::uint8_t desired_owner, AuthorityReason reason)> on_authority_request;
    std::function<void(std::uint8_t sender, std::uint32_t blob_id, const std::vector<std::uint8_t> &blob, std::uint8_t flags)> on_state_blob;
    std::function<void(std::uint8_t sender, std::uint32_t frame, std::uint64_t hash)> on_sync_hash;
    std::function<void(std::uint8_t sender)> on_resync_request;
    std::function<void(std::uint8_t sender, const std::vector<std::uint8_t> &compressed_mc)> on_memcard_mirror;
    std::function<void(std::uint8_t sender, RingOutcome outcome, std::uint8_t combo_count, const std::string &char_name)> on_ring_callout;
    std::function<void(std::uint8_t sender, const std::string &trophy_id, const std::string &trophy_name)> on_trophy_share;
    std::function<void(std::uint8_t sender, EmoteKind emote)> on_emote_ping;
    std::function<void(const LanRoomInfo &room)> on_lan_room_found;
};

// --- Non-Blocking UDP Transport (Direct IP + LAN Discovery + Relay/STUN Hole-Punch) ---
class CoopTransport {
public:
    CoopTransport();
    ~CoopTransport();

    CoopTransport(const CoopTransport &) = delete;
    CoopTransport &operator=(const CoopTransport &) = delete;

    // Bind local UDP socket (port = 0 lets OS choose ephemeral port)
    bool open(std::uint16_t local_port = 0, bool listen_lan_beacons = false);
    void close();
    bool is_open() const;
    std::uint16_t local_port() const { return local_port_; }

    void set_callbacks(TransportCallbacks cb) { cb_ = std::move(cb); }

    // Host a room (optionally registering with a Relay Server for a 6-char Room Code)
    bool host_room(const std::string &player_name, const LobbySettings &cfg,
                   const std::string &relay_host_port = "");
    // Join via 6-char Room Code through a Relay Server
    bool join_by_code(const std::string &player_name, const std::string &room_code,
                      const std::string &relay_host_port);
    // Join directly by IP:Port (LAN / VPN / Port-Forwarded)
    bool join_direct(const std::string &player_name, const std::string &host_ip_port);

    // Send messages
    void broadcast_lobby_config(const LobbySettings &cfg);
    void set_ready(bool ready);
    void send_launch_together(const LobbySettings &cfg, std::uint16_t game_port);
    void send_input_frame(const PadSample &sample, std::uint16_t epoch);
    void send_authority_grant(std::uint8_t new_owner, std::uint8_t party_slot, AuthorityReason reason, std::uint16_t epoch);
    void send_authority_request(std::uint8_t desired_owner, AuthorityReason reason);
    void send_state_blob(const std::vector<std::uint8_t> &compressed_blob, std::uint8_t flags, std::uint8_t target_slot = 0xFF);
    void send_sync_hash(std::uint32_t frame, std::uint64_t hash);
    void send_resync_request();
    void send_memcard_mirror(const std::vector<std::uint8_t> &compressed_mc);
    void send_ring_callout(RingOutcome outcome, std::uint8_t combo_count, const std::string &char_name);
    void send_trophy_share(const std::string &trophy_id, const std::string &trophy_name);
    void send_emote_ping(EmoteKind emote);

    // Pump incoming packets, heartbeats, hole-punching, LAN beacons, and chunked blob retransmits
    void poll();

    // Accessors
    bool is_host() const { return is_host_; }
    std::uint8_t local_slot() const { return local_slot_; }
    std::uint32_t room_id() const { return room_id_; }
    const std::string &room_code() const { return room_code_; }
    const std::array<PeerInfo, kMaxPlayers> &peers() const { return peers_; }
    const LobbySettings &lobby_settings() const { return lobby_cfg_; }
    int connected_peer_count() const;

private:
    std::vector<std::uint8_t> make_packet(PacketType type, const std::uint8_t *body, std::size_t body_len,
                                          std::uint8_t flags = 0);
    void send_raw(const std::vector<std::uint8_t> &pkt, const Endpoint &ep);
    void send_to_peer(std::uint8_t slot, const std::vector<std::uint8_t> &pkt);
    void broadcast_to_peers(const std::vector<std::uint8_t> &pkt);
    void handle_packet(const std::uint8_t *data, std::size_t len, const Endpoint &from);
    void pump_blob_transfers(std::uint64_t now_ms);
    void send_lan_beacon();

    std::uintptr_t sock_ = ~std::uintptr_t(0);
    std::uintptr_t beacon_sock_ = ~std::uintptr_t(0);
    std::uint16_t local_port_ = 0;

    bool is_host_ = false;
    bool using_relay_ = false;
    std::uint8_t local_slot_ = 0;
    std::uint32_t room_id_ = 0;
    std::uint32_t seq_ = 1;
    std::string player_name_ = "Player";
    std::string room_code_;
    Endpoint relay_ep_{};
    LobbySettings lobby_cfg_{};

    std::array<PeerInfo, kMaxPlayers> peers_{};
    std::array<PadSample, 3> recent_inputs_{};
    int recent_input_count_ = 0;

    // Reliable blob channels (state + memcard)
    std::uint32_t next_blob_id_ = 1;
    BlobTransferOut state_out_{};
    BlobTransferIn state_in_{};
    BlobTransferOut mc_out_{};
    BlobTransferIn mc_in_{};

    std::uint64_t last_heartbeat_ms_ = 0;
    std::uint64_t last_keepalive_ms_ = 0;
    std::uint64_t last_beacon_ms_ = 0;
    TransportCallbacks cb_{};
};

} // namespace coop
