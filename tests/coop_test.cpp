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

    // Full initial snapshot (no baseline) with repeated VRAM/struct patterns -> LZ0 must compress >4x
    std::vector<std::uint8_t> full_snap(512u * 1024u, 0);
    for (std::size_t i = 0; i < full_snap.size(); ++i) {
        if ((i & 0x3FFu) < 256u) {
            full_snap[i] = static_cast<std::uint8_t>(((i & 0x1Fu) * 7u + 0x21u) & 0xFFu); // repeated tile
        } else if ((i & 0x3FFu) < 512u) {
            full_snap[i] = 0xAAu; // overlapping RLE/LZ pattern (dist=1)
        }
    }
    const auto full_comp = coop::compress_delta(full_snap, {});
    std::vector<std::uint8_t> full_out;
    check(full_comp.size() < full_snap.size() / 8u,
          "Delta-LZ0 compresses 512 KiB full snapshot with repeated patterns by >8x");
    check(coop::decompress_delta(full_comp, {}, full_out) && full_out == full_snap,
          "Delta-LZ0 full snapshot decompresses bit-for-bit");

    // Edge cases: empty, 1-byte, 3-byte
    for (std::size_t sz : {std::size_t(0), std::size_t(1), std::size_t(3)}) {
        std::vector<std::uint8_t> tiny(sz, 0x7Bu);
        auto tc = coop::compress_delta(tiny, {});
        std::vector<std::uint8_t> tout;
        check(coop::decompress_delta(tc, {}, tout) && tout == tiny,
              "Delta-LZ0 handles tiny/empty buffers cleanly");
    }
}

void test_room_codes_and_cli() {
    check(coop::normalize_room_code("k7m-4px") == "K7M4PX", "normalize_room_code strips dash and uppercases");
    check(coop::format_room_code_pretty("K7M4PX") == "K7M-4PX", "format_room_code_pretty formats XXX-XXX");

    coop::LaunchConfig cfg;
    cfg.enabled = true;
    cfg.is_host = true;
    cfg.player_name = "Yuri";
    cfg.relay_addr = "relay.example.com:47191";
    cfg.code_out_file = "coop_launch_code.tmp";
    cfg.local_port = 47222;
    cfg.settings.campaign_name = "covenant_duo";
    cfg.settings.slot_owner = {0, 1, 0, 1};

    const std::string cli = coop::build_coop_cli_args(cfg);
    check(cli.find("--coop-host") != std::string::npos &&
          cli.find("--coop-code-file \"coop_launch_code.tmp\"") != std::string::npos &&
          cli.find("--coop-campaign \"covenant_duo\"") != std::string::npos &&
          cli.find("--coop-slots 0101") != std::string::npos,
          "build_coop_cli_args encodes host, code-file, campaign, and party slots");

    // Verify PS2 vs PS1 idle spectator button polarity
    {
        coop::CoopSession ps2_guest, ps1_guest;
        coop::LaunchConfig g2;
        g2.enabled = true;
        g2.is_host = false;
        g2.direct_addr = "127.0.0.1:47299";
        g2.settings.game = coop::GameKind::SH2;
        ps2_guest.start(g2, "", {});
        coop::PadSample in{}, eff{};
        ps2_guest.tick_frame(1, in, eff);
        check(eff.buttons == 0x0000u, "PS2 spectator idle buttons default to 0x0000 (active-high released)");
        ps2_guest.stop();

        coop::LaunchConfig g1 = g2;
        g1.settings.game = coop::GameKind::Koudelka;
        ps1_guest.start(g1, "", {});
        ps1_guest.tick_frame(1, in, eff);
        check(eff.buttons == 0xFFFFu, "PS1 (Koudelka) spectator idle buttons default to 0xFFFF (active-low released)");
        ps1_guest.stop();
    }
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

    // 3-Player loopback broadcast test (verifies per-peer ack_mask delivers multi-chunk checkpoints to ALL peers)
    {
        std::vector<std::uint8_t> h3(96 * 1024, 0x33), g1(96 * 1024, 0), g2(96 * 1024, 0);
        for (std::size_t i = 0; i < h3.size(); ++i) h3[i] = static_cast<std::uint8_t>((i * 73u + 9u) & 0xFFu);
        const std::uint16_t p3 = 47298;
        coop::LaunchConfig c_h = hcfg; c_h.local_port = p3;
        coop::LaunchConfig c_1 = gcfg; c_1.player_name = "Karin"; c_1.direct_addr = "127.0.0.1:" + std::to_string(p3);
        coop::LaunchConfig c_2 = gcfg; c_2.player_name = "Gepetto"; c_2.direct_addr = "127.0.0.1:" + std::to_string(p3);

        coop::CoopSession sh, s1, s2;
        sh.start(c_h, (tmp / "host").string(), make_hooks(h3, false));
        s1.start(c_1, (tmp / "guest").string(), make_hooks(g1, false));
        s2.start(c_2, (tmp / "guest").string(), make_hooks(g2, false));

        coop::PadSample z{}, e{};
        for (std::uint32_t f = 1; f <= 80; ++f) {
            sh.tick_frame(f, z, e);
            s1.tick_frame(f, z, e);
            s2.tick_frame(f, z, e);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        h3[4096] ^= 0xFFu;
        sh.broadcast_checkpoint(false, 0xFF);
        for (std::uint32_t f = 81; f <= 150; ++f) {
            sh.tick_frame(f, z, e);
            s1.tick_frame(f, z, e);
            s2.tick_frame(f, z, e);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        check(sh.connected_peers() == 3 && g1 == h3 && g2 == h3,
              "3-Player broadcast delivers multi-chunk delta checkpoint bit-for-bit to ALL peers");
        sh.stop(); s1.stop(); s2.stop();
    }

    fs::remove_all(tmp, ec);
}

void test_koudelka_auto_turn_and_levelup() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "nocturne_coop_kdl_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "host", ec);
    fs::create_directories(tmp / "guest", ec);

    // Simulated 2 MiB PS1 RAM for Host and Guest
    constexpr std::size_t kPsxRamSize = 2u * 1024u * 1024u;
    std::vector<std::uint8_t> host_ram(kPsxRamSize, 0);
    std::vector<std::uint8_t> guest_ram(kPsxRamSize, 0);

    auto wr32 = [](std::vector<std::uint8_t> &ram, std::uint32_t off, std::uint32_t v) {
        std::memcpy(ram.data() + off, &v, 4);
    };
    auto set_kdl_state = [&](std::uint32_t ovl, std::uint32_t sub, std::uint32_t lvlup_ptr, std::uint32_t active_char) {
        for (auto *ram : {&host_ram, &guest_ram}) {
            wr32(*ram, 0x01705Cu, 0x64756F4Bu); // "Koud" EXE signature
            wr32(*ram, 0x0AF470u, 0x801F1590u); // GameManager*
            wr32(*ram, 0x0B6DF0u, ovl);
            wr32(*ram, 0x0D5DACu, sub);
            wr32(*ram, 0x1F1590u + 0x358u, lvlup_ptr);
            wr32(*ram, 0x0B4D5Cu, active_char);
        }
    };

    auto make_hooks = [](std::vector<std::uint8_t> &ram) {
        coop::SessionHooks h;
        h.save_state_mem = [&ram](std::vector<std::uint8_t> &out) {
            // Compact 16 KiB test snapshot (fast loopback)
            out.assign(ram.begin() + 0x0AF000u, ram.begin() + 0x0B3000u);
            return true;
        };
        h.load_state_mem = [&ram](const std::uint8_t *d, std::size_t n) {
            if (n <= 0x4000u) std::memcpy(ram.data() + 0x0AF000u, d, n);
            return true;
        };
        h.compute_ram_hash = [&ram]() {
            return coop::fnv1a_64(ram.data() + 0x0B0000u, 0x1000u);
        };
        h.read_ram_u32 = [&ram](std::uint32_t addr) -> std::uint32_t {
            if (addr + 4u > ram.size()) return 0u;
            std::uint32_t v = 0;
            std::memcpy(&v, ram.data() + addr, 4);
            return v;
        };
        return h;
    };

    // Start in Field (ovl=5, sub=10)
    set_kdl_state(5u, 10u, 0u, 0u);

    const std::uint16_t port = 47297;
    coop::LaunchConfig hcfg;
    hcfg.enabled = true;
    hcfg.is_host = true;
    hcfg.player_name = "HostP1";
    hcfg.local_port = port;
    hcfg.settings.game = coop::GameKind::Koudelka;
    hcfg.settings.max_players = 3;
    hcfg.settings.slot_owner = {0, 1, 2, 0}; // Koudelka=P1, Edward=P2, James=P3 (disconnected -> fallback P1)
    hcfg.settings.auto_battle_turn = true;
    hcfg.settings.campaign_name = "kdl_autoturn";

    coop::LaunchConfig gcfg;
    gcfg.enabled = true;
    gcfg.is_host = false;
    gcfg.player_name = "GuestP2";
    gcfg.direct_addr = "127.0.0.1:" + std::to_string(port);
    gcfg.settings.game = coop::GameKind::Koudelka;
    gcfg.settings.campaign_name = "kdl_autoturn";

    coop::CoopSession host, guest;
    check(host.start(hcfg, (tmp / "host").string(), make_hooks(host_ram)), "Koudelka Host starts");
    check(guest.start(gcfg, (tmp / "guest").string(), make_hooks(guest_ram)), "Koudelka Guest connects");

    std::uint32_t f = 1;
    coop::PadSample z{}, he{}, ge{};
    auto pump = [&](int frames) {
        for (int i = 0; i < frames; ++i, ++f) {
            host.tick_frame(f, z, he);
            guest.tick_frame(f, z, ge);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    pump(50);
    check(host.connected_peers() == 2 && host.has_authority() && !guest.has_authority(),
          "Koudelka Field starts with Host (P1) in control");

    // 1. Battle begins with Edward (char 1 -> P2) taking turn
    set_kdl_state(5u, 9u, 0u, 1u);
    pump(50);
    check(!host.has_authority() && guest.has_authority() && guest.active_party_slot() == 1u,
          "Koudelka Battle auto-switches controller to P2 on Edward's turn");

    // 2. James's turn (char 2 -> P3 disconnected -> falls back to Host P1)
    set_kdl_state(5u, 9u, 0u, 2u);
    pump(50);
    check(host.has_authority() && !guest.has_authority() && host.active_party_slot() == 2u,
          "Koudelka Battle falls back to Host (P1) when James's assigned P3 is disconnected");

    // 3. Battle ends -> brief overlay load (ovl=0) -> Level-Up Bonus Points screen for Edward (char 1 -> P2)
    set_kdl_state(0u, 0u, 0u, 0u);
    pump(10);
    set_kdl_state(4u, 0u, 0x80180000u, 1u);
    pump(50);
    check(!host.has_authority() && guest.has_authority() && guest.active_party_slot() == 1u,
          "Koudelka Level-Up screen auto-switches controller to P2 for Edward's bonus points");
    check(guest.active_banner() != nullptr && guest.active_banner()->title.find("Edward") != std::string::npos,
          "Koudelka Level-Up banner identifies Edward");

    // 4. Level-Up advances to Koudelka (char 0 -> P1)
    set_kdl_state(4u, 0u, 0x80180000u, 0u);
    pump(50);
    check(host.has_authority() && !guest.has_authority() && host.active_party_slot() == 0u,
          "Koudelka Level-Up screen auto-switches controller to P1 for Koudelka's bonus points");

    // 5. Level-Up for Edward again (P2), then closes -> brief load -> returns to Field -> auto-returns to Host (P1)
    set_kdl_state(4u, 0u, 0x80180000u, 1u);
    pump(50);
    check(guest.has_authority(), "P2 controls Edward's second level-up");
    set_kdl_state(0u, 0u, 0u, 1u);
    pump(10);
    set_kdl_state(5u, 10u, 0u, 1u);
    pump(50);
    check(host.has_authority() && !guest.has_authority(),
          "Returning to Field after Battle/Level-Up automatically restores control to Host (P1)");

    // 6. Manual Pass-the-Stick on Field stays with Guest (P2) without being stolen back
    host.pass_authority_next(coop::AuthorityReason::PassStick);
    pump(50);
    check(!host.has_authority() && guest.has_authority(),
          "Manual Pass-the-Stick on Field stays with Guest (P2)");

    // 7. Battle detection via canonical gm+0x36C == 1 (even when sub_ovl != 9) + scene->0xA0 actor->0x4BC
    guest.pass_authority_next(coop::AuthorityReason::PassStick);
    pump(40);
    check(host.has_authority(), "Host regains control before gm+0x36C battle test");
    for (auto *ram : {&host_ram, &guest_ram}) {
        wr32(*ram, 0x0B6DF0u, 5u);
        wr32(*ram, 0x0D5DACu, 0u);             // sub_ovl != 9 (tests canonical gm+0x36C flag)
        wr32(*ram, 0x1F1590u + 0x36Cu, 1u);    // gm->is_in_battle = 1
        wr32(*ram, 0x1F1590u + 0x32Cu, 0x80172390u); // gm->scene
        wr32(*ram, 0x172390u + 0x0A0u, 0x801D1000u); // scene->active_actor
        wr32(*ram, 0x1D1000u + 0x4BCu, 1u);          // actor->char_id = 1 (Edward)
        wr32(*ram, 0x0B4D5Cu, 0u);                   // 0x0B4D5C still 0, actor->0x4BC takes priority
    }
    pump(50);
    check(!host.has_authority() && guest.has_authority() && guest.active_party_slot() == 1u,
          "Koudelka Battle detected via gm+0x36C=1 and actor->0x4BC switches to P2 (Edward)");

    // 7b. Post-battle Level-Up on Koudelka (P1): when Level-Up closes while battle_flag is still 1
    // and actor->0x4BC is still Edward (1), control MUST stay on Host (P1) — no ghost handoff to P2!
    for (auto *ram : {&host_ram, &guest_ram}) {
        wr32(*ram, 0x1F1590u + 0x358u, 0x80090000u); // LevelUpBonusScreen* open
        wr32(*ram, 0x0B4D5Cu, 0u);                   // Koudelka (P1)
    }
    pump(50);
    check(host.has_authority(), "Koudelka Level-Up hands control to Host (P1)");
    for (auto *ram : {&host_ram, &guest_ram}) {
        wr32(*ram, 0x1F1590u + 0x358u, 0u);          // Level-Up closes while battle_flag=1 & actor=Edward!
    }
    pump(25);
    check(host.has_authority() && !guest.has_authority(),
          "Closing Koudelka Level-Up with lingering battle_flag=1 does NOT falsely hand control to P2");

    // 7c. Monotonic InputJitterBuffer::pop_next() plays frames 1-by-1 without repeating or skipping
    {
        coop::InputJitterBuffer jb;
        jb.reset(0);
        for (std::uint32_t pf = 10; pf <= 14; ++pf) {
            coop::PadSample s{};
            s.frame = pf;
            s.buttons = static_cast<std::uint32_t>(0xFFFFu ^ (1u << (pf - 10)));
            jb.push(s);
        }
        coop::PadSample p1{}, p2{}, p3{};
        check(jb.pop_next(p1, true) && jb.pop_next(p2, true) && jb.pop_next(p3, true) &&
              p2.frame == p1.frame + 1 && p3.frame == p2.frame + 1,
              "InputJitterBuffer::pop_next advances strictly 1 frame per tick without repeats or skips");
    }

    // 8. Host exits -> Guest detects host_disconnected() cleanly for MessageBox & auto-close
    check(!guest.host_disconnected(), "Guest host_disconnected() is false while Host is active");
    host.stop();
    for (int i = 0; i < 40 && !guest.host_disconnected(); ++i, ++f) {
        guest.tick_frame(f, z, ge);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(guest.host_disconnected(),
          "Host stop() sends Leave and sets guest.host_disconnected() == true");

    guest.stop();
    fs::remove_all(tmp, ec);
}

} // namespace

int main() {
    test_delta_compression();
    test_room_codes_and_cli();
    test_loopback_turn_authority_session();
    test_koudelka_auto_turn_and_levelup();
    if (g_fails == 0) {
        std::printf("ALL WAVE E CO-OP TESTS PASSED!\n");
    } else {
        std::fprintf(stderr, "%d WAVE E CO-OP TEST(S) FAILED!\n", g_fails);
    }
    return g_fails ? 1 : 0;
}
