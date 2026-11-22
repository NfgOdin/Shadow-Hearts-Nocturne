#include "launcher_app.hpp"
#include "launcher_data.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++g_fails;
    }
}

void test_catalog_and_settings_roundtrip() {
    const auto &items = achievements::catalog_items();
    check(items.size() == 110u, "shared catalog has 110 achievements");

    // Verify shared gallery tier tab order puts Covenant at far right after Follies and Curiosities
    {
        static constexpr int kExp[8] = {-1, 0, 1, 2, 3, 5, 6, 4};
        for (int i = 0; i < 8; ++i) {
            check(achievements::kGalleryTierTabs[static_cast<std::size_t>(i)].tier_filter == kExp[i],
                  "kGalleryTierTabs orders Covenant far-right after Curiosities and Follies");
        }
        check(std::string(achievements::kGalleryTierTabs[5].label) == "Follies" &&
              std::string(achievements::kGalleryTierTabs[6].label) == "Curiosities" &&
              std::string(achievements::kGalleryTierTabs[7].label) == "Covenant",
              "kGalleryTierTabs labels end with Follies, Curiosities, Covenant");
    }

    // Comment-preserving keys.ini round-trip test
    const std::filesystem::path tmp_ini = std::filesystem::temp_directory_path() / "sh2_launcher_test_keys.ini";
    {
        std::ofstream out(tmp_ini, std::ios::binary);
        out << "# Custom Header Comment\n"
               "dpad_up = Up\n"
               "cross = X\n"
               "scale = 2\n"
               "gpu_api = dx12\n"
               "# Trailing comment\n";
    }
    auto s = launcher::load_settings(tmp_ini);
    check(s.scale == 2 && s.gpu_api == "dx12", "initial keys.ini parsed accurately");

    s.scale = 6;
    s.gpu_api = "vulkan";
    s.aspect = 1;
    s.ssao = 45;
    s.bloom = 30;
    s.filmic = true;
    s.toast_theme = 2;
    s.launcher_hide_on_play = false;
    s.launcher_fullscreen = true;
    check(launcher::save_settings(tmp_ini, s), "save_settings wrote keys.ini cleanly");

    std::ifstream in(tmp_ini, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(raw.find("# Custom Header Comment") != std::string::npos, "keys.ini top comment preserved");
    check(raw.find("cross = X") != std::string::npos, "keys.ini keybindings preserved");
    check(raw.find("# Trailing comment") != std::string::npos, "keys.ini trailing comment preserved");

    auto s2 = launcher::load_settings(tmp_ini);
    check(s2.scale == 6 && s2.gpu_api == "vulkan" && s2.ssao == 45 && s2.bloom == 30 &&
          s2.filmic && s2.toast_theme == 2 && !s2.launcher_hide_on_play && s2.launcher_fullscreen,
          "all modified launcher & graphics settings round-tripped verbatim");

    std::error_code ec;
    std::filesystem::remove(tmp_ini, ec);

    // First-run keys.ini creation preserves default keybindings & high scale (12x) + 240 FPS cap
    const std::filesystem::path fresh_ini = std::filesystem::temp_directory_path() / "sh2_launcher_fresh_keys.ini";
    std::filesystem::remove(fresh_ini, ec);
    launcher::Settings fs_set;
    fs_set.scale = 12;
    fs_set.fps_cap = "240";
    check(launcher::save_settings(fresh_ini, fs_set), "first-run save_settings creates fresh keys.ini");
    {
        std::ifstream fin(fresh_ini, std::ios::binary);
        std::string fraw((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        check(fraw.find("cross       = X") != std::string::npos && fraw.find("dpad_up     = Up") != std::string::npos,
              "first-run save_settings seeds default keybindings & comments");
    }
    const auto fs_reloaded = launcher::load_settings(fresh_ini);
    check(fs_reloaded.scale == 12 && fs_reloaded.fps_cap == "240",
          "scale=12 and fps_cap=240 round-trip cleanly without clobbering");
    std::filesystem::remove(fresh_ini, ec);
}

void test_slots_backup_and_launch_cmd() {
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "sh2_launcher_mock_root";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    const std::filesystem::path gdir = tmp_root / "games" / "sh2-covenant";
    std::filesystem::create_directories(gdir / "states", ec);
    std::filesystem::create_directories(gdir / "disc1_files", ec);
    std::filesystem::create_directories(gdir / "disc2_files", ec);
    {
        std::ofstream(gdir / "SH2C.exe") << "exe";
        std::ofstream(gdir / "disc1_files" / "disc_manifest.tsv") << "m1";
        std::ofstream(gdir / "disc2_files" / "disc_manifest.tsv") << "m2";
        std::ofstream(gdir / "memcard.ps2", std::ios::binary) << "PS2MEMCARD_TEST_PAYLOAD";
        std::ofstream(gdir / "states" / "slot_0.state", std::ios::binary) << "STATE_DATA_1234";
        std::ofstream(gdir / "states" / "slot_0.meta")
            << "timestamp=2026-11-20 00:30:00\ndisc=2\narea=Yokohama\nlevel=48\nplaytime=7560\n";
        // Write synthetic 16x12 PS2THB1 RGB24 thumbnail
        std::ofstream tf(gdir / "states" / "slot_0.thumb", std::ios::binary);
        static constexpr char kMagic[8] = {'P', 'S', '2', 'T', 'H', 'B', '1', 0};
        const std::uint32_t w = 16, h = 12;
        tf.write(kMagic, 8);
        tf.write(reinterpret_cast<const char *>(&w), 4);
        tf.write(reinterpret_cast<const char *>(&h), 4);
        std::vector<std::uint8_t> rgb(w * h * 3, 0x80u);
        tf.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    }

    launcher::LauncherState st;
    st.game_id = launcher::GameId::Covenant;
    st.paths.root_dir = tmp_root;
    launcher::refresh_all(st);

    check(st.ready_to_play && st.exe_present, "mock Covenant installation detected as ready to play via games/sh2-covenant/SH2C.exe");
    check(st.occupied_slots == 1 && st.latest_slot == 0, "Slot 0 detected as occupied & latest");
    check(st.slots[0].area == "Yokohama" && st.slots[0].max_level == 48 && st.slots[0].disc == 2,
          "Slot 0 metadata parsed accurately");
    check(st.slots[0].has_thumb && st.slots[0].thumb_w == 16 && st.slots[0].thumb_h == 12 &&
          st.slots[0].thumb_bgra.size() == 192u, "Slot 0 PS2THB1 thumbnail decoded to BGRA");

    // Verify Trilogy summaries report Covenant ready, SH1 75-trophy catalog present, and SH3 coming soon
    {
        const auto sums = launcher::load_trilogy_summaries(tmp_root);
        check(!sums[0].exe_present && !sums[0].ready_to_play && sums[0].trophies_total == 75, "SH1 summary reports 75-trophy catalog");
        check(sums[1].exe_present && sums[1].ready_to_play && sums[1].trophies_total == 110 && sums[1].occupied_slots == 1,
              "Covenant summary reports ready to play with 110 trophies and 1 save slot");
        check(!sums[2].exe_present && !sums[2].ready_to_play && sums[2].trophies_total == 0, "FTNW summary reports coming soon");
    }

    // Verify global launcher.ini round-trip
    {
        launcher::GlobalLauncherConfig gcfg;
        gcfg.chimes = false;
        gcfg.auto_enter_last_game = true;
        gcfg.last_game = launcher::GameId::ShadowHearts1;
        check(launcher::save_global_config(tmp_root, gcfg), "save_global_config writes launcher.ini cleanly");
        const auto g2 = launcher::load_global_config(tmp_root);
        check(!g2.chimes && g2.auto_enter_last_game && g2.last_game == launcher::GameId::ShadowHearts1,
              "global launcher.ini round-trips chimes, auto_enter_last_game, and last_game");
    }

    // Verify launch command targets games/sh2-covenant/SH2C.exe with both discs and optional --load-slot
    const std::wstring cmd_win = launcher::build_launch_command(st.paths, false);
    const std::wstring cmd_fs = launcher::build_launch_command(st.paths, true);
    const std::wstring cmd_slot = launcher::build_launch_command(st.paths, false, 3);
    const std::wstring cmd_auto = launcher::build_launch_command(st.paths, false, 10);
    check(cmd_win.find(L"SH2C.exe") != std::wstring::npos, "launch command targets games/sh2-covenant/SH2C.exe");
    check(cmd_win.find(L"--disc ") != std::wstring::npos && cmd_win.find(L"--disc2 ") != std::wstring::npos,
          "launch command passes Disc 1 and Disc 2");
    check(cmd_win.find(L"--window") != std::wstring::npos && cmd_fs.find(L"--fullscreen") != std::wstring::npos,
          "launch command toggles --window and --fullscreen accurately");
    check(cmd_win.find(L"--load-slot") == std::wstring::npos && cmd_slot.find(L"--load-slot 3") != std::wstring::npos &&
          cmd_auto.find(L"--load-slot auto") != std::wstring::npos,
          "launch command appends --load-slot N and --load-slot auto accurately");

    // Verify dedicated autosave.state discovery and deletion
    {
        std::ofstream(gdir / "states" / "autosave.state", std::ios::binary) << "AUTOSTATE";
        std::ofstream(gdir / "states" / "autosave.meta") << "area=Paris\nlevel=32\ndisc=1\nplaytime=7200\n";
        launcher::load_save_slots(st);
        check(st.autosave_slot.exists && st.autosave_slot.area == "Paris" && st.autosave_slot.max_level == 32,
              "load_save_slots discovers autosave.state and metadata");
        check(launcher::delete_save_slot(st.paths, 10) && !std::filesystem::exists(gdir / "states" / "autosave.state", ec),
              "delete_save_slot(10) removes autosave.state cleanly");
    }

    // Verify VU1 microprogram cache scanner detects baked vs unbaked recordings
    std::filesystem::create_directories(tmp_root / "generated" / "sh2-covenant", ec);
    std::filesystem::create_directories(gdir / "vu1", ec);
    std::ofstream(tmp_root / "generated" / "sh2-covenant" / "vu1_table.cpp")
        << "const ps2::vu::Program kVuPrograms[] = {\n"
        << "    {0x0123456789abcdefull, vu1_0123456789ABCDEF, 1},\n"
        << "};\n";
    std::ofstream(gdir / "vu1" / "0123456789ABCDEF_0100.bin") << "baked";
    std::ofstream(gdir / "vu1" / "FEDCBA9876543210_0200.bin") << "new";
    launcher::evaluate_health(st);
    check(st.vu1_cache.baked_programs == 1 && st.vu1_cache.captured_unique == 2 && st.vu1_cache.unbaked_count == 1,
          "VU1 health scan detects baked vs newly captured unbaked microprograms");

    // Verify timestamped memcard backup
    std::filesystem::path bkp;
    std::string bmsg;
    check(launcher::backup_memcard(st.paths, &bkp, &bmsg) && std::filesystem::is_regular_file(bkp, ec),
          "backup_memcard creates timestamped .ps2 backup");

    // Verify per-game HD Texture Pack discovery, enable/disable, priority reordering & keys.ini persistence
    std::filesystem::create_directories(gdir / "mods" / "textures" / "HD_UI", ec);
    std::filesystem::create_directories(gdir / "mods" / "textures" / "HD_World", ec);
    std::filesystem::create_directories(gdir / "mods" / "fmv", ec);
    {
        std::ofstream(gdir / "mods" / "textures" / "HD_UI" / "0123456789abcdef.png") << "png1";
        std::ofstream(gdir / "mods" / "textures" / "HD_World" / "fedcba9876543210.png") << "png2";
        std::ofstream(gdir / "mods" / "fmv" / "0011223344556677.mp4") << "mp4data";
    }
    launcher::refresh_all(st);
    check(st.texture_packs.size() == 2 && st.fmv_mods.mp4_count == 1, "discovers 2 texture packs and 1 FMV .mp4");
    check(launcher::move_texture_pack(st, 1, -1) && st.texture_packs[0].name == "HD_World", "move_texture_pack reorders priority");
    check(launcher::toggle_texture_pack(st, 1) && !st.texture_packs[1].enabled, "toggle_texture_pack disables pack");
    st.settings.tex_dump = true;
    st.settings.fmv_dump = true;
    st.settings.true_widescreen = false;
    st.settings.autosave = false;
    check(launcher::save_settings(st.paths.keys_ini, st.settings), "save_settings writes HD mod & Wave 3 settings");
    const auto reloaded = launcher::load_settings(st.paths.keys_ini);
    check(reloaded.tex_dump && reloaded.fmv_dump && reloaded.texture_packs == "HD_World,!HD_UI" &&
          !reloaded.true_widescreen && !reloaded.autosave,
          "HD texture pack order, true_widescreen, and autosave flags round-trip through keys.ini");

    // Verify clear_live_caches removes both files and work subdirectories (ee-live/work-*, vu1-live/*)
    std::filesystem::create_directories(gdir / "ee-live" / "work-001", ec);
    std::filesystem::create_directories(gdir / "vu1-live" / "sub-002", ec);
    std::ofstream(gdir / "ee-live" / "work-001" / "gen.cpp") << "x";
    std::ofstream(gdir / "vu1-live" / "sub-002" / "vu.dll") << "y";
    std::ofstream(gdir / "vu1-live" / "cached.dll") << "z";
    const int cleared = launcher::clear_live_caches(st.paths);
    check(cleared >= 3 && !std::filesystem::exists(gdir / "ee-live" / "work-001", ec) &&
          !std::filesystem::exists(gdir / "vu1-live" / "sub-002", ec),
          "clear_live_caches removes ee-live/work-* and vu1-live/* subdirectories");

    // Verify Koudelka save-state parity: legacy root slot_N.state auto-migration, .meta + PS2THB1 .thumb, and Koudelka_*.png screenshots
    {
        const std::filesystem::path kdir = tmp_root / "games" / "koudelka";
        const std::filesystem::path kstates = kdir / "states";
        const std::filesystem::path kshots = tmp_root / "koudelka_shots";
        std::filesystem::create_directories(kdir, ec);
        std::filesystem::create_directories(kshots, ec);
        std::ofstream(kdir / "slot_0.state", std::ios::binary) << "PSXSTAT2_LEGACY";
        std::filesystem::create_directories(kstates, ec);
        std::ofstream(kstates / "slot_2.state", std::ios::binary) << "PSXSTAT2_SLOT2";
        std::ofstream(kstates / "slot_2.meta") << "timestamp=2026-11-21 20:00:00\nframe=1200\ndisc=2\narea=Nemeton Monastery (Disc 2)\nplaytime=20.00\n";
        {
            std::ofstream tf(kstates / "slot_2.thumb", std::ios::binary);
            static constexpr char kMagic[8] = {'P', 'S', '2', 'T', 'H', 'B', '1', 0};
            const std::uint32_t tw = 160u, th = 112u;
            tf.write(kMagic, 8);
            tf.write(reinterpret_cast<const char *>(&tw), 4);
            tf.write(reinterpret_cast<const char *>(&th), 4);
            std::vector<char> rgb(160u * 112u * 3u, 0x40);
            tf.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
        }
        std::ofstream(kshots / "Koudelka_2026-11-21_20-00-00.png", std::ios::binary) << "PNG";

        launcher::LauncherState kst;
        kst.game_id = launcher::GameId::Koudelka;
        kst.paths = launcher::resolve_paths(tmp_root, launcher::GameId::Koudelka);
        kst.paths.screenshots_dir = kshots;
        launcher::load_save_slots(kst);
        launcher::load_screenshots(kst);
        check(kst.occupied_slots == 2 && kst.slots[0].exists &&
                  std::filesystem::is_regular_file(kstates / "slot_0.state", ec) &&
                  !std::filesystem::exists(kdir / "slot_0.state", ec),
              "Koudelka legacy root slot_0.state auto-migrated to states/slot_0.state");
        check(kst.slots[2].exists && kst.slots[2].disc == 2 && kst.slots[2].has_thumb &&
                  kst.slots[2].thumb_w == 160 && kst.slots[2].thumb_h == 112 &&
                  kst.slots[2].area == "Nemeton Monastery (Disc 2)",
              "Koudelka slot_2 loads .meta and 160x112 PS2THB1 .thumb cleanly");
        check(kst.screenshots.size() == 1 && kst.screenshots[0].filename == "Koudelka_2026-11-21_20-00-00.png",
              "Koudelka_ prefixed screenshots discovered by Launcher");
        check(launcher::delete_save_slot(kst.paths, 2) && !std::filesystem::exists(kstates / "slot_2.state", ec) &&
                  !std::filesystem::exists(kstates / "slot_2.thumb", ec),
              "Koudelka delete_save_slot removes .state, .meta, and .thumb");
    }

    // Verify Update Channel persistence in launcher.ini + update check JSON parser
    {
        launcher::GlobalLauncherConfig gcfg;
        gcfg.update_channel = "nightly";
        check(launcher::save_global_config(tmp_root, gcfg), "save_global_config writes update_channel");
        const auto gread = launcher::load_global_config(tmp_root);
        check(gread.update_channel == "nightly", "load_global_config reads update_channel=nightly");

        const auto parsed = launcher::parse_update_check_json(
            R"({"ok": true, "channel": "nightly", "local_short": "78421ca", "local_date": "2026-11-21", "remote_short": "d53129e", "remote_date": "2026-11-21", "remote_subject": "Add standalone installer", "update_available": true})");
        check(parsed.ok && parsed.update_available && parsed.channel == "nightly" &&
                  parsed.local_short == "78421ca" && parsed.remote_short == "d53129e" &&
                  parsed.remote_subject == "Add standalone installer",
              "parse_update_check_json parses GitHub check payload cleanly");

        // Verify standalone install folder resolves scripts_root & build_cmd from launcher.ini source_repo
        const std::filesystem::path fake_repo = tmp_root / "fake_source_repo";
        const std::filesystem::path fake_inst = tmp_root / "standalone_install";
        std::filesystem::create_directories(fake_repo / "scripts", ec);
        std::filesystem::create_directories(fake_inst / "games" / "koudelka", ec);
        std::ofstream(fake_repo / "Shadow Hearts Nocturne Setup.cmd") << "@echo off\n";
        std::ofstream(fake_repo / "scripts" / "nocturne_update.py") << "# ok\n";
        launcher::GlobalLauncherConfig scfg;
        scfg.source_repo = fake_repo.string();
        scfg.update_channel = "nightly";
        check(launcher::save_global_config(fake_inst, scfg), "save_global_config writes source_repo");
        const auto spaths = launcher::resolve_paths(fake_inst, launcher::GameId::Koudelka);
        check(spaths.root_dir == std::filesystem::absolute(fake_inst, ec).lexically_normal() &&
                  spaths.scripts_root == fake_repo.lexically_normal() &&
                  spaths.build_cmd == (fake_repo / "Shadow Hearts Nocturne Setup.cmd").lexically_normal() &&
                  spaths.update_script == (fake_repo / "scripts" / "nocturne_update.py").lexically_normal(),
              "standalone custom install resolves scripts_root, Setup.cmd, and nocturne_update.py via source_repo");
    }

    std::filesystem::remove_all(tmp_root, ec);
}

void test_headless_d2d_renders(const std::string &dump_dir) {
    const std::filesystem::path root = std::filesystem::current_path();
    launcher::LauncherApp app(root);

    const std::filesystem::path out_dir = dump_dir.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(dump_dir);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    // 1. Verify default startup lands on the 3-panel Gothic Trilogy Triptych
    check(app.current_page() == launcher::Page::Trilogy, "Launcher opens on Trilogy Triptych by default");
    {
        const std::filesystem::path p = out_dir / "launcher_trilogy.png";
        check(app.render_to_png(p, 1220, 740) && std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 8000u,
              "Direct2D headless Trilogy Triptych render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(p, ec);
    }
    {
        app.set_gamepad_nav_preview(true, "trilogy_card_1", true);
        const std::filesystem::path p = out_dir / "launcher_trilogy_gamepad.png";
        check(app.render_to_png(p, 1220, 740), "Direct2D Trilogy Triptych with gamepad focus ring succeeded");
        app.gamepad_navigate(-1, 0);
        check(app.focused_id() == "trilogy_card_0", "Gamepad Left on Triptych moves focus from Covenant -> Shadow Hearts 1");
        app.gamepad_navigate(0, -1);
        check(app.focused_id() == "trilogy_card_3", "Gamepad Up from Shadow Hearts 1 focuses top-left Koudelka prequel pill");
        app.gamepad_navigate(0, 1);
        check(app.focused_id() == "trilogy_card_0", "Gamepad Down from Koudelka prequel pill returns to Shadow Hearts 1");
        app.set_gamepad_nav_preview(false, "", false);
        if (dump_dir.empty()) std::filesystem::remove(p, ec);
    }

    // 2. Render Koudelka prequel, Shadow Hearts 1 (blood-moon) & From the New World (1929 Art-Deco) hubs
    {
        app.select_game(launcher::GameId::Koudelka, true);
        check(app.active_game() == launcher::GameId::Koudelka && app.current_page() == launcher::Page::Home,
              "Selecting Koudelka opens its dedicated Home hub");
        const std::filesystem::path kdl_home = out_dir / "launcher_koudelka_home.png";
        check(app.render_to_png(kdl_home, 1220, 740), "Direct2D Koudelka Home hub render succeeded");

        app.select_game(launcher::GameId::ShadowHearts1, true);
        check(app.active_game() == launcher::GameId::ShadowHearts1 && app.current_page() == launcher::Page::Home,
              "Selecting Shadow Hearts 1 opens its dedicated Home hub");
        const std::filesystem::path sh1_home = out_dir / "launcher_sh1_home.png";
        check(app.render_to_png(sh1_home, 1220, 740), "Direct2D Shadow Hearts 1 Home hub render succeeded");
        app.set_page(launcher::Page::Trophies);
        const std::filesystem::path sh1_tr = out_dir / "launcher_sh1_trophies.png";
        check(app.render_to_png(sh1_tr, 1220, 740), "Direct2D Shadow Hearts 1 Chronicle-In-Progress Trophy page succeeded");

        app.select_game(launcher::GameId::FromTheNewWorld, true);
        const std::filesystem::path sh3_home = out_dir / "launcher_sh3_home.png";
        check(app.render_to_png(sh3_home, 1220, 740), "Direct2D From the New World Home hub render succeeded");
        if (dump_dir.empty()) {
            std::filesystem::remove(kdl_home, ec);
            std::filesystem::remove(sh1_home, ec);
            std::filesystem::remove(sh1_tr, ec);
            std::filesystem::remove(sh3_home, ec);
        }
    }

    // 3. Enter Covenant hub and verify all 5 pages + reordered far-right Covenant trophy tab
    app.select_game(launcher::GameId::Covenant, true);
    auto &st = app.state();
    int marked = 0;
    for (auto &t : st.trophies) {
        if (!t.unlocked && (t.tier == achievements::Tier::Mark || t.tier == achievements::Tier::Sigil) && marked < 18) {
            t.unlocked = true;
            t.unlocked_at = "2026-11-20 00:45:00";
            ++st.trophies_unlocked;
            ++st.tier_unlocked[static_cast<int>(t.tier)];
            ++marked;
        }
    }
    st.best_ring_streak = 14;
    st.best_perfect_streak = 4;

    struct RenderJob { launcher::Page page; const char *file; };
    const RenderJob jobs[5] = {
        {launcher::Page::Home, "launcher_home.png"},
        {launcher::Page::Settings, "launcher_settings.png"},
        {launcher::Page::Trophies, "launcher_trophies.png"},
        {launcher::Page::SavesMedia, "launcher_saves.png"},
        {launcher::Page::Tools, "launcher_tools.png"},
    };
    for (const auto &j : jobs) {
        app.set_page(j.page);
        if (j.page == launcher::Page::Trophies) {
            app.set_trophy_tier(-1);
            app.set_reveal_secrets(true);
        }
        const std::filesystem::path p = out_dir / j.file;
        const bool ok = app.render_to_png(p, 1220, 740);
        check(ok && std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 8000u,
              "Direct2D headless page render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(p, ec);
    }
    // Verify scrolled Settings NEVER registers HitRegions overlapping the titlebar (y < 46)
    app.set_page(launcher::Page::Settings);
    app.set_settings_scroll(200.0f);
    {
        const std::filesystem::path p = out_dir / "launcher_settings_scrolled_tmp.png";
        check(app.render_to_png(p, 1220, 740), "Scrolled Settings render succeeded");
        check(!app.any_non_titlebar_hit_in_y_band(0.0f, 46.0f),
              "Scrolled Settings does NOT register HitRegions overlapping the titlebar (y < 46)");
        std::filesystem::remove(p, ec);
    }
    app.set_settings_scroll(0.0f);

    // Verify > 10 HD texture packs scroll cleanly without leaking HitRegions above list viewport
    app.set_page(launcher::Page::Tools);
    app.set_tools_subtab(1);
    {
        const auto saved_packs = app.state().texture_packs;
        app.state().texture_packs.clear();
        for (int i = 0; i < 12; ++i) {
            launcher::TexturePackInfo pk;
            pk.name = "Community_HD_Pack_" + std::to_string(i + 1);
            pk.png_count = static_cast<std::size_t>(100 + i * 25);
            pk.bytes = static_cast<std::uint64_t>(16ull + i) * 1024ull * 1024ull;
            pk.enabled = (i % 3 != 2);
            app.state().texture_packs.push_back(std::move(pk));
        }
        app.set_tools_mods_scroll(180.0f);
        const std::filesystem::path p = out_dir / "launcher_mods_scroll_tmp.png";
        check(app.render_to_png(p, 1220, 740), "Scrolled >10 HD texture packs render succeeded");
        check(!app.any_non_titlebar_hit_in_y_band(0.0f, 46.0f),
              "Scrolled HD texture pack list does NOT leak HitRegions into titlebar");
        std::filesystem::remove(p, ec);
        app.set_tools_mods_scroll(0.0f);
        app.state().texture_packs = saved_packs;
    }
    {
        const std::filesystem::path p = out_dir / "launcher_mods.png";
        const bool ok = app.render_to_png(p, 1220, 740);
        check(ok && std::filesystem::is_regular_file(p, ec) && std::filesystem::file_size(p, ec) > 8000u,
              "Direct2D headless Tools > HD Packs & Mods render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(p, ec);
    }
    app.set_tools_subtab(0);

    // Verify Gamepad spatial navigation, page cycling, slider adjustment, and antique-gold focus ring rendering
    {
        app.set_page(launcher::Page::Home);
        app.set_gamepad_nav_preview(true, "home_play", true);
        const std::filesystem::path gp_png = out_dir / "launcher_home_gamepad.png";
        check(app.render_to_png(gp_png, 1220, 740), "Home render with gamepad focus ring & PlayStation legend succeeded");
        app.gamepad_navigate(+1, 0);
        check(app.focused_id() != "home_play" && !app.focused_id().empty(),
              ("Gamepad D-pad Right moves focus spatially off home_play (" + app.focused_id() + ")").c_str());
        app.gamepad_switch_page(+1);
        check(app.current_page() == launcher::Page::Settings, "Gamepad RB switches Home -> Settings");
        app.gamepad_back();
        check(app.current_page() == launcher::Page::Home, "Gamepad B returns from Settings -> Home");
        app.gamepad_back();
        check(app.current_page() == launcher::Page::Trilogy, "Gamepad B on Home returns cleanly to Trilogy Triptych");
        app.select_game(launcher::GameId::Covenant, true);
        app.set_gamepad_nav_preview(false, "", false);
        if (dump_dir.empty()) std::filesystem::remove(gp_png, ec);
    }

    // Regression test: sustain 800 Home frames (~13s @ 60fps) then switch to Trophy Gallery
    // (which calls draw_text(..., ellipsis = false) -> SetTrimming(nullptr)) + cycle all tiers & tear down.
    // Previously caused a 0xc00000fd / 0xc000041d stack overflow from chained CreateEllipsisTrimmingSign calls.
    check(app.render_stress_frames(800, 120, 1220, 740),
          "Sustained 800 Home + 120 Trophy Gallery frames succeeded without trimming-sign stack overflow");
}

}  // namespace

int main(int argc, char **argv) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string dump_dir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-dir") == 0) dump_dir = argv[i + 1];
    }

    test_catalog_and_settings_roundtrip();
    test_slots_backup_and_launch_cmd();
    test_headless_d2d_renders(dump_dir);

    if (SUCCEEDED(hr)) CoUninitialize();
    if (g_fails == 0) {
        std::printf("ALL LAUNCHER & DIRECT2D VISUAL TESTS PASSED!\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed.\n", g_fails);
    return 1;
}
