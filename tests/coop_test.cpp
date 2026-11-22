#include "coop_net.hpp"
#include "coop_session.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

int g_fails = 0;

void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    } else {
        std::printf("PASS: %s\n", msg);
    }
}

void test_delta_compression() {
    // Simulate a 2 MiB state snapshot with 4 KiB changed between turns
    std::vector<std::uint8_t> base(2u * 1024u * 1024u, 0);
    for (std::size_t i = 0; i < base.size(); ++i) {
        base[i] = static_cast<std::uint8_t>((i * 131u + 17u) & 0xFFu);
    }
    std::vector<std::uint8_t> cur = base;
    for (std::size_t i = 0x40000u; i < 0x41000u; ++i) {
        cur[i] ^= static_cast<std::uint8_t>((i & 0xFFu) ^ 0x5Au);
    }

    const auto comp = coop::compress_delta(cur, base);
    check(comp.size() < 8192u, "Delta compression shrinks 2 MiB RAM with 4 KiB change to < 8 KiB");

    std::vector<std::uint8_t> out;
    check(coop::decompress_delta(comp, base, out) && out == cur,
          "Delta decompression restores 2 MiB state bit-for-bit");

    // Corrupt 1 byte -> CRC must reject
    auto bad = comp;
    bad[bad.size() / 2] ^= 0xFFu;
    std::vector<std::uint8_t> bad_out;
    check(!coop::decompress_delta(bad, base, bad_out),
          "Delta decompression rejects corrupted payload via CRC-32");
}

void test_room_codes_and_cli() {
    check(coop::normalize_room_code("k7m-4px") == "K7M4PX", "normalize_room_code strips dash and uppercases");
    check(coop::format_room_code_pretty("K7M4PX") == "K7M-4PX", "format_room_code_pretty formats XXX-XXX");

    coop::LaunchConfig cfg;
    cfg.enabled = true;
    cfg.is_host = true;
    cfg.player_name = "Yuri";
    cfg.relay_addr = "relay.example.com:47191";
    cfg.local_port = 47222;
    cfg.settings.campaign_name = "covenant_duo";
    cfg.settings.slot_owner = {0, 1, 0, 1};

    const std::string cli = coop::build_coop_cli_args(cfg);
    check(cli.find("--coop-host") != std::string::npos &&
          cli.find("--coop-campaign \"covenant_duo\"") != std::string::npos &&
          cli.find("--coop-slots 0101") != std::string::npos,
          "build_coop_cli_args encodes host, campaign, and party slots");
}

void test_loopback_turn_authority_session() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "nocturne_coop_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "host", ec);
    fs::create_directories(tmp / "guest", ec);

    std::vector<std::uint8_t> host_ram(64 * 1024, 0x11);
    std::vector<std::uint8_t> guest_ram(64 * 1024, 0x00);
    std::string guest_shared_trophy;
    bool guest_mc_mirrored = false;

    auto make_hooks = [&](std::vector<std::uint8_t> &ram, bool is_guest) {
        coop::SessionHooks h;
        h.save_state_mem = [&ram](std::vector<std::uint8_t> &out) {
            out = ram;
            return true;
        };
        h.load_state_mem = [&ram](const std::uint8_t *d, std::size_t n) {
            ram.assign(d, d + n);
            return true;
        };
        h.compute_ram_hash = [&ram]() {
            return coop::fnv1a_64(ram.data(), ram.size());
        };
        h.read_ram_u32 = [&ram](std::uint32_t addr) -> std::uint32_t {
            if (addr + 4 > ram.size()) return 0;
            std::uint32_t v = 0;
            std::memcpy(&v, ram.data() + addr, 4);
            return v;
        };
        if (is_guest) {
            h.on_shared_trophy = [&](const std::string &tid, const std::string &) { guest_shared_trophy = tid; };
            h.on_memcard_mirrored = [&](const std::string &) { guest_mc_mirrored = true; };
        }
        return h;
    };

    const std::uint16_t port = 47295;
    coop::LaunchConfig hcfg;
    hcfg.enabled = true;
    hcfg.is_host = true;
    hcfg.player_name = "Yuri";
    hcfg.local_port = port;
    hcfg.settings.game = coop::GameKind::SH2;
    hcfg.settings.campaign_name = "test_run";

    coop::LaunchConfig gcfg;
    gcfg.enabled = true;
    gcfg.is_host = false;
    gcfg.player_name = "Karin";
    gcfg.direct_addr = "127.0.0.1:" + std::to_string(port);
    gcfg.settings.game = coop::GameKind::SH2;
    gcfg.settings.campaign_name = "test_run";

    coop::CoopSession host, guest;
    check(host.start(hcfg, (tmp / "host").string(), make_hooks(host_ram, false)), "Host Co-op session starts on loopback UDP");
    check(guest.start(gcfg, (tmp / "guest").string(), make_hooks(guest_ram, true)), "Guest Co-op session connects to Host");

    // Pump 60 ticks so Hello/Welcome + initial checkpoint complete
    coop::PadSample h_in{}, h_eff{}, g_in{}, g_eff{};
    h_in.buttons = 0x4000u; // Cross pressed on Host
    h_in.lx = 0xC0;
    for (std::uint32_t f = 1; f <= 60; ++f) {
        host.tick_frame(f, h_in, h_eff);
        guest.tick_frame(f, g_in, g_eff);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    check(host.connected_peers() == 2 && guest.connected_peers() == 2,
          "Host and Guest both report 2 connected players");
    check(host.has_authority() && !guest.has_authority(),
          "Host starts with Turn Authority and Guest spectates");
    check(h_eff.buttons == 0x4000u && g_eff.buttons == 0x4000u && g_eff.lx == 0xC0,
          "Zero-lag Host pad streams accurately to Guest jitter buffer");
    check(guest_ram == host_ram,
          "Initial compressed state checkpoint synced Guest RAM bit-for-bit");

    // Test Pass-the-Stick / Turn Authority handoff to Guest (P2)
    host_ram[1024] = 0xAB;
    host.pass_authority_next(coop::AuthorityReason::ManualF6);
    g_in.buttons = 0x2000u; // Circle pressed on Guest (P2)
    for (std::uint32_t f = 61; f <= 110; ++f) {
        host.tick_frame(f, h_in, h_eff);
        guest.tick_frame(f, g_in, g_eff);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    check(!host.has_authority() && guest.has_authority(),
          "Turn Authority cleanly transferred from Host (P1) to Guest (P2)");
    check(guest_ram[1024] == 0xAB,
          "Delta-state checkpoint synced Host's last RAM mutation before handoff");
    check(h_eff.buttons == 0x2000u,
          "Guest (P2) now drives Host (P1) with 0 ms local authority");

    // Test Shared Trophy & Spectator Emote Ping
    guest.share_trophy_unlock("sh2_dead_center", "Dead Center");
    host.send_emote(coop::EmoteKind::Nice);
    // Also test Host -> Guest trophy share & memcard mirror
    guest.pass_authority_next(coop::AuthorityReason::PassStick);
    for (std::uint32_t f = 111; f <= 140; ++f) {
        host.tick_frame(f, h_in, h_eff);
        guest.tick_frame(f, g_in, g_eff);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    host.share_trophy_unlock("sh2_perfect", "Bullseye");
    const fs::path host_mc = tmp / "host" / "coop_saves" / "test_run" / "memcard.ps2";
    const fs::path guest_mc = tmp / "guest" / "coop_saves" / "test_run" / "memcard.ps2";
    {
        std::ofstream ofs(host_mc, std::ios::binary);
        std::vector<std::uint8_t> dummy_mc(32768, 0x5C);
        ofs.write(reinterpret_cast<const char *>(dummy_mc.data()), static_cast<std::streamsize>(dummy_mc.size()));
    }
    guest.mirror_memcard_file(guest_mc.string()); // registers guest target path
    host.mirror_memcard_file(host_mc.string());
    for (std::uint32_t f = 141; f <= 190; ++f) {
        host.tick_frame(f, h_in, h_eff);
        guest.tick_frame(f, g_in, g_eff);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    check(guest_shared_trophy == "sh2_perfect", "Shared Trophy broadcast received by partner");
    check(guest_mc_mirrored && fs::exists(guest_mc) && fs::file_size(guest_mc) == 32768u,
          "Isolated Co-op Memory Card mirrored reliably to partner");

    host.stop();
    guest.stop();
    fs::remove_all(tmp, ec);
}

} // namespace

int main() {
    test_delta_compression();
    test_room_codes_and_cli();
    test_loopback_turn_authority_session();
    if (g_fails == 0) {
        std::printf("ALL WAVE E CO-OP TESTS PASSED!\n");
    } else {
        std::fprintf(stderr, "%d WAVE E CO-OP TEST(S) FAILED!\n", g_fails);
    }
    return g_fails ? 1 : 0;
}
