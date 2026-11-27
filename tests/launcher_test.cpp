#include "launcher_app.hpp"
#include "launcher_data.hpp"
#include "mod_engine.hpp"
#include "psx_achievements.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
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
               "settings_schema = 2\n"
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

    // First-run keys.ini creation preserves default keybindings & high scale (12x),
    // and normalizes any legacy high FPS cap back to locked native 60 FPS.
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
    check(fs_reloaded.scale == 12 && fs_reloaded.fps_cap == "60",
          "scale=12 round-trips cleanly and legacy fps_cap normalizes to locked 60");
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

    // Verify Trilogy summaries report Covenant ready, SH1 75-trophy catalog, and SH3 75-trophy catalog
    {
        const auto sums = launcher::load_trilogy_summaries(tmp_root);
        check(!sums[0].exe_present && !sums[0].ready_to_play && sums[0].trophies_total == 75, "SH1 summary reports 75-trophy catalog");
        check(sums[1].exe_present && sums[1].ready_to_play && sums[1].trophies_total == 110 && sums[1].occupied_slots == 1,
              "Covenant summary reports ready to play with 110 trophies and 1 save slot");
        check(!sums[2].exe_present && !sums[2].ready_to_play && sums[2].trophies_total == 75, "FTNW summary reports 75-trophy catalog");
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

void test_wave_c_packs_mods_playtime_and_save_bundle() {
    std::error_code ec;
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "sh2_wave_c_test";
    std::filesystem::remove_all(tmp_root, ec);

    const std::filesystem::path gdir = tmp_root / "games" / "sh2-covenant";
    std::filesystem::create_directories(gdir / "texture_packs" / "SH2_4K_Portraits", ec);
    std::filesystem::create_directories(gdir / "video_packs" / "SH2_4K_Cutscenes", ec);
    std::filesystem::create_directories(gdir / "mods" / "hardcore_ring" / "files" / "DATA", ec);
    std::filesystem::create_directories(gdir / "mods" / "hardcore_ring" / "patches", ec);
    std::filesystem::create_directories(gdir / "mods" / "hardcore_ring" / "scripts", ec);
    std::filesystem::create_directories(gdir / "mods" / "alt_rebalance" / "files" / "DATA", ec);
    std::filesystem::create_directories(gdir / "states", ec);

    std::ofstream(gdir / "texture_packs" / "SH2_4K_Portraits" / "0123456789abcdef_0000000000000000_256x256.png", std::ios::binary) << "PNGDATA";
    std::ofstream(gdir / "video_packs" / "SH2_4K_Cutscenes" / "fedcba9876543210_intro.mp4", std::ios::binary) << "MP4DATA12345";

    std::ofstream(gdir / "mods" / "hardcore_ring" / "mod.json")
        << R"({"id":"hardcore_ring","name":"Hardcore Judgment Ring","author":"Odin","version":"2.1","description":"Tighter strike zones","priority":150})";
    std::ofstream(gdir / "mods" / "hardcore_ring" / "files" / "DATA" / "BTL_PARAM.BIN") << "MOD_BTL_1";
    std::ofstream(gdir / "mods" / "hardcore_ring" / "patches" / "ring.pnach")
        << "patch=1,EE,20100100,extended,000003E7\n";
    std::ofstream(gdir / "mods" / "hardcore_ring" / "scripts" / "hooks.lua")
        << "function on_boot()\n  write_u16(0x00100200, 0x1234)\nend\n"
        << "function on_frame()\n  write_u8(0x00100204, 0x7F)\nend\n";

    std::ofstream(gdir / "mods" / "alt_rebalance" / "mod.json")
        << R"({"id":"alt_rebalance","name":"Alt Enemy Rebalance","author":"Yuri","version":"1.0","priority":90})";
    std::ofstream(gdir / "mods" / "alt_rebalance" / "files" / "DATA" / "BTL_PARAM.BIN") << "MOD_BTL_2";

    // 1. Verify separated texture_packs/, video_packs/, and mods/ discovery + conflict detection
    launcher::LauncherState st;
    st.game_id = launcher::GameId::Covenant;
    st.paths = launcher::resolve_paths(tmp_root, launcher::GameId::Covenant);
    launcher::load_texture_packs(st);

    check(st.texture_packs.size() == 1 && st.texture_packs[0].name == "SH2_4K_Portraits" && st.texture_packs[0].png_count == 1,
          "Dedicated texture_packs/ directory discovered cleanly");
    check(st.video_packs.size() == 1 && st.video_packs[0].name == "SH2_4K_Cutscenes" && st.video_packs[0].mp4_count == 1 &&
              st.fmv_mods.mp4_count == 1,
          "Dedicated video_packs/ directory discovered cleanly");
    check(st.mods.size() == 2 && st.mods[0].name == "Hardcore Judgment Ring" &&
              st.mods[0].file_override_count == 1 && st.mods[0].pnach_patch_count == 1 && st.mods[0].lua_hook_count == 1,
          "Real mod.json + Layer 1/2/3 counts discovered cleanly from mods/");
    check(!st.mods[0].conflicts_with.empty() && !st.mods[1].conflicts_with.empty(),
          "Mod conflict detector flags two enabled mods overriding DATA/BTL_PARAM.BIN");

    check(launcher::toggle_mod(st, 1) && !st.mods[1].enabled && st.mods[0].conflicts_with.empty(),
          "Disabling conflicting mod clears conflict warning immediately");

    // 2. Verify mod_engine::Engine runtime execution (Layer 1 file override, Layer 2 .pnach, Layer 3 Lua, Layer 4 C-API)
    mod_engine::Engine eng;
    eng.load_from_game_dir(gdir, "hardcore_ring,!alt_rebalance", true);
    check(eng.active_mod_count() == 1 && eng.file_override_count() == 1 && eng.patch_count() == 1 && eng.script_op_count() == 2,
          "mod_engine::Engine loads active mod layers and skips !disabled mods");
    check(!eng.resolve_file_override("DATA/BTL_PARAM.BIN").empty(),
          "mod_engine::Engine resolves Layer 1 virtual disc file override");

    std::vector<std::uint8_t> fake_ram(0x200000u, 0u);
    eng.on_boot(fake_ram.data(), fake_ram.size());
    eng.on_frame(fake_ram.data(), fake_ram.size());
    std::uint32_t patched32 = 0;
    std::uint16_t lua16 = 0;
    std::memcpy(&patched32, fake_ram.data() + 0x00100100u, 4);
    std::memcpy(&lua16, fake_ram.data() + 0x00100200u, 2);
    check(patched32 == 999u && lua16 == 0x1234u && fake_ram[0x00100204u] == 0x7Fu,
          "mod_engine::Engine applies Layer 2 .pnach and Layer 3 Lua on_boot/on_frame writes accurately");

    NocturneModApiV1 capi = mod_engine::Engine::make_c_api("sh2-covenant", fake_ram.data(), static_cast<std::uint32_t>(fake_ram.size()));
    capi.write_u32(capi.guest_ram, capi.guest_ram_size, 0x00100300u, 0xDEADBEEFu);
    check(capi.read_u32(capi.guest_ram, capi.guest_ram_size, 0x00100300u) == 0xDEADBEEFu,
          "Layer 4 NocturneModApiV1 C-API reads and writes guest RAM accurately");

    // 3. Verify Playtime Tracker recording & save-slot playtime reconciliation
    check(launcher::record_playtime_session(st.paths, 5400.0, false), "record_playtime_session writes solo session");
    check(launcher::record_playtime_session(st.paths, 1800.0, true), "record_playtime_session writes coop session");
    launcher::load_playtime(st);
    check(st.playtime.sessions_count == 2 && st.playtime.solo_seconds >= 5399.0 &&
              st.playtime.coop_seconds >= 1799.0 && st.playtime.effective_total_seconds >= 7198.0,
          "load_playtime aggregates solo + coop sessions and effective_total_seconds");

    // 4. Verify .nocturne-save.zip Save & State Bundle Export & Import round-trip
    std::ofstream(st.paths.memcard_path, std::ios::binary) << "MEMCARD_PS2_PAYLOAD";
    std::ofstream(st.paths.achievements_json, std::ios::binary) << R"({"unlocked":{"first_blood":"2026-11-22"}})";
    std::ofstream(st.paths.states_dir / "slot_0.state", std::ios::binary) << "SLOT0_STATE_BYTES";
    std::ofstream(st.paths.states_dir / "slot_0.meta", std::ios::binary) << "area=Domremy Church\ndisc=1\n";

    std::filesystem::path bundle_path;
    std::string export_msg;
    check(launcher::export_save_bundle(st.paths, &bundle_path, &export_msg) &&
              std::filesystem::is_regular_file(bundle_path, ec) &&
              std::filesystem::file_size(bundle_path, ec) > 120u,
          "export_save_bundle creates valid .nocturne-save.zip archive");

    // Delete original files and restore from .nocturne-save.zip
    std::filesystem::remove(st.paths.memcard_path, ec);
    std::filesystem::remove(st.paths.states_dir / "slot_0.state", ec);
    std::string import_msg;
    check(launcher::import_save_bundle(st.paths, bundle_path, &import_msg) &&
              std::filesystem::is_regular_file(st.paths.memcard_path, ec) &&
              std::filesystem::is_regular_file(st.paths.states_dir / "slot_0.state", ec),
          "import_save_bundle verifies CRC-32 and restores memcard + save states from .nocturne-save.zip");

    std::filesystem::remove_all(tmp_root, ec);
}

void test_koudelka_trophies_and_ram_tracker() {
    const auto &kitems = achievements::catalog_items_for(achievements::CatalogGame::Koudelka);
    check(kitems.size() == 50u, "Koudelka catalog has exactly 50 trophies");

    std::unordered_set<std::string> ids;
    int tier_counts[7] = {};
    for (const auto &it : kitems) {
        ids.insert(it.id);
        const int ti = static_cast<int>(it.tier);
        if (ti >= 0 && ti < 7) ++tier_counts[ti];
    }
    check(ids.size() == 50u, "all 50 Koudelka trophy IDs are unique");
    check(tier_counts[static_cast<int>(achievements::Tier::Mark)] == 12 &&
          tier_counts[static_cast<int>(achievements::Tier::Sigil)] == 10 &&
          tier_counts[static_cast<int>(achievements::Tier::Crest)] == 8 &&
          tier_counts[static_cast<int>(achievements::Tier::Sovereign)] == 6 &&
          tier_counts[static_cast<int>(achievements::Tier::Covenant)] == 1 &&
          tier_counts[static_cast<int>(achievements::Tier::Folly)] == 7 &&
          tier_counts[static_cast<int>(achievements::Tier::Curiosity)] == 6,
          "Koudelka 50-trophy tier distribution matches 12/10/8/6/1/7/6");

    // Build synthetic 2 MiB PS1 RAM matching verified SLUS_010.51 layout
    std::vector<std::uint8_t> ram(0x200000u, 0u);
    auto wr_str = [&](std::uint32_t va, const char *s) {
        std::memcpy(ram.data() + (va - 0x80000000u), s, std::strlen(s) + 1);
    };
    auto wr8 = [&](std::uint32_t va, std::uint8_t v) { ram[va - 0x80000000u] = v; };
    auto wr16 = [&](std::uint32_t va, std::uint16_t v) { std::memcpy(ram.data() + (va - 0x80000000u), &v, 2); };
    auto wr32 = [&](std::uint32_t va, std::uint32_t v) { std::memcpy(ram.data() + (va - 0x80000000u), &v, 4); };

    wr_str(0x8001231Cu, "KoudelkaDiscNo");
    wr_str(0x8001705Cu, "Koudelka");

    const std::uint32_t root = 0x801A1E4Cu;
    const std::uint32_t p_kdl = 0x80190000u;
    const std::uint32_t p_edw = 0x80191000u;
    const std::uint32_t p_jam = 0x80192000u;
    wr32(0x800B4100u, root);
    wr32(root - 0x54u, p_kdl);
    wr32(root - 0x50u, p_edw);
    wr32(root - 0x4Cu, p_jam);

    auto init_actor = [&](std::uint32_t base, const char *name, std::uint8_t id,
                          std::uint32_t lv, std::uint32_t exp, std::uint16_t hpc, std::uint16_t hpm,
                          std::uint16_t mpc, std::uint16_t mpm, bool joined, bool active) {
        wr_str(base + 0x146u, name);
        wr32(base + 0x2D8u, lv);
        wr32(base + 0x2DCu, exp);
        wr16(base + 0x410u, hpc);
        wr16(base + 0x412u, hpm);
        wr16(base + 0x420u, mpc);
        wr16(base + 0x422u, mpm);
        for (int s = 0; s < 8; ++s) {
            wr8(base + 0x424u + static_cast<std::uint32_t>(s), 10u);
            wr8(base + 0x43Cu + static_cast<std::uint32_t>(s), 14u);
        }
        wr8(base + 0x4BAu, joined ? 1u : 0u);
        wr8(base + 0x4BBu, active ? 1u : 0u);
        wr8(base + 0x4BCu, id);
    };

    init_actor(p_kdl, "Koudelka", 0u, 1u, 0u, 169u, 169u, 22u, 22u, true, true);
    init_actor(p_edw, "Edward", 1u, 1u, 0u, 256u, 256u, 8u, 8u, false, false);
    init_actor(p_jam, "James", 2u, 1u, 0u, 225u, 225u, 40u, 40u, false, false);
    wr32(0x800B6B08u, 0x22u);
    wr32(0x800B42E8u, 600u);  // 10s playtime

    const auto snap = psx_ach::inspect_ram(ram.data());
    check(snap.valid && snap.max_level == 1 && snap.joined_count == 1 &&
          snap.room_id == 0x22u && snap.playtime_frames == 600u,
          "psx_ach::inspect_ram decodes synthetic SLUS_010.51 party snapshot cleanly");

    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "sh2_kdl_ach_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    const std::filesystem::path kdir = tmp_root / "games" / "koudelka";
    std::filesystem::create_directories(kdir, ec);
    const std::filesystem::path ach_json = kdir / "achievements.json";

    psx_ach::Tracker tr;
    tr.load(ach_json);
    check(tr.total_count() == 50 && tr.unlocked_count() == 0, "fresh Koudelka tracker starts with 0/50 unlocked");

    // 1. Seed initial state in Disc 1 Room 0x22 -> unlocks kdl_curiosity_disc1
    tr.poll_ram(ram.data(), 1, 1.0);
    check(tr.unlocked_count() >= 1 && tr.has_active_toast(1.5),
          "Disc 1 arrival unlocks trophy and queues Gothic unlock toast");

    // 2. Win a battle unscathed (EXP jump, HP full)
    wr32(p_kdl + 0x2DCu, 120u);
    wr32(0x800B42E8u, 1200u);
    tr.poll_ram(ram.data(), 1, 2.0);
    check(tr.battles_won() == 1, "EXP jump increments Koudelka battles_won");

    // 3. Recruit Edward & James, win a clutch battle (HP < 15%), and swap to Disc 2
    wr8(p_edw + 0x4BAu, 1u);
    wr8(p_edw + 0x4BBu, 1u);
    wr8(p_jam + 0x4BAu, 1u);
    wr8(p_jam + 0x4BBu, 1u);
    wr16(p_kdl + 0x410u, 15u);  // < 15% of 169
    wr32(p_kdl + 0x2DCu, 300u);
    wr32(0x800B42E8u, 2400u);
    tr.poll_ram(ram.data(), 2, 3.0);

    auto has_unlocked = [](const psx_ach::Tracker &t, const char *id) {
        for (const auto &e : t.entries()) {
            if (e.id == id) return e.unlocked;
        }
        return false;
    };
    check(has_unlocked(tr, "kdl_curiosity_disc1"), "kdl_curiosity_disc1 unlocked");
    check(has_unlocked(tr, "kdl_curiosity_disc2"), "kdl_curiosity_disc2 unlocked");
    check(has_unlocked(tr, "kdl_mark_first_blood"), "kdl_mark_first_blood unlocked");
    check(has_unlocked(tr, "kdl_mark_unscathed_1"), "kdl_mark_unscathed_1 unlocked");
    check(has_unlocked(tr, "kdl_mark_edward_joins"), "kdl_mark_edward_joins unlocked");
    check(has_unlocked(tr, "kdl_mark_james_joins"), "kdl_mark_james_joins unlocked");
    check(has_unlocked(tr, "kdl_mark_full_trio"), "kdl_mark_full_trio unlocked");
    check(has_unlocked(tr, "kdl_folly_clutch"), "kdl_folly_clutch unlocked");
    const int saved_unlocked = tr.unlocked_count();

    // 4. Reload in a fresh Tracker AND in launcher::load_trophies()
    psx_ach::Tracker tr2;
    tr2.load(ach_json);
    check(tr2.unlocked_count() == saved_unlocked && tr2.battles_won() == 2,
          "Koudelka achievements.json round-trips unlocked trophies & counters in psx_ach::Tracker");

    launcher::LauncherState lst;
    lst.game_id = launcher::GameId::Koudelka;
    lst.paths = launcher::resolve_paths(tmp_root, launcher::GameId::Koudelka);
    launcher::load_trophies(lst);
    check(lst.trophies.size() == 50u && lst.trophies_unlocked == saved_unlocked,
          "Launcher load_trophies loads all 50 Koudelka trophies + unlocked count from achievements.json");

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
        check(app.focused_id() == "trilogy_card_0", "Gamepad Left on Quadrilogy moves focus from Covenant -> Shadow Hearts 1");
        app.gamepad_navigate(-1, 0);
        check(app.focused_id() == "trilogy_card_3", "Gamepad Left from Shadow Hearts 1 focuses Koudelka (Card #1)");
        app.gamepad_navigate(1, 0);
        check(app.focused_id() == "trilogy_card_0", "Gamepad Right from Koudelka returns to Shadow Hearts 1");
        app.gamepad_navigate(0, 1);
        check(app.focused_id() == "quad_archives", "Gamepad Down from Quadrilogy cards focuses centered Archives & Lore banner");
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
        app.set_page(launcher::Page::Trophies);
        const std::filesystem::path kdl_tr = out_dir / "launcher_koudelka_trophies.png";
        check(app.render_to_png(kdl_tr, 1220, 740), "Direct2D Koudelka 50-Trophy Gallery page render succeeded");

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
        check(launcher::game_profile(launcher::GameId::FromTheNewWorld).has_achievement_catalog,
              "From the New World profile enables the 75-trophy catalog");
        {
            launcher::LauncherState sh3_st;
            sh3_st.game_id = launcher::GameId::FromTheNewWorld;
            launcher::load_trophies(sh3_st);
            check(sh3_st.trophies.size() == 75u, "Launcher load_trophies loads all 75 SH3 trophies");
        }
        app.set_page(launcher::Page::Trophies);
        const std::filesystem::path sh3_tr = out_dir / "launcher_sh3_trophies.png";
        check(app.render_to_png(sh3_tr, 1220, 740), "Direct2D From the New World 75-Trophy Gallery page render succeeded");
        if (dump_dir.empty()) {
            std::filesystem::remove(kdl_home, ec);
            std::filesystem::remove(kdl_tr, ec);
            std::filesystem::remove(sh1_home, ec);
            std::filesystem::remove(sh1_tr, ec);
            std::filesystem::remove(sh3_home, ec);
            std::filesystem::remove(sh3_tr, ec);
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

        // Verify Video Packs tab (mode 1) and Gameplay Mods tab (mode 2) Direct2D rendering
        app.set_packs_view_mode(1);
        const std::filesystem::path p_vid = out_dir / "launcher_video_packs.png";
        check(app.render_to_png(p_vid, 1220, 740), "Direct2D Tools > Video Packs tab render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(p_vid, ec);

        app.set_packs_view_mode(2);
        const std::filesystem::path p_gmod = out_dir / "launcher_gameplay_mods.png";
        check(app.render_to_png(p_gmod, 1220, 740), "Direct2D Tools > Gameplay Mods tab render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(p_gmod, ec);
        app.set_packs_view_mode(0);
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

    // Wave E & F: Verify Direct2D headless rendering of Co-op Lobby & Judgment Ring Dojo pages
    {
        app.set_page(launcher::Page::Coop);
        const std::filesystem::path coop_png = out_dir / "launcher_coop_lobby.png";
        check(app.render_to_png(coop_png, 1220, 740) && std::filesystem::file_size(coop_png, ec) > 8000u,
              "Direct2D Co-op Lobby page render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(coop_png, ec);

        app.set_page(launcher::Page::RingDojo);
        const std::filesystem::path dojo_png = out_dir / "launcher_ring_dojo.png";
        check(app.render_to_png(dojo_png, 1220, 740) && std::filesystem::file_size(dojo_png, ec) > 8000u,
              "Direct2D Judgment Ring Dojo page render succeeded");
        if (dump_dir.empty()) std::filesystem::remove(dojo_png, ec);
    }

    // Regression test: sustain 800 Home frames (~13s @ 60fps) then switch to Trophy Gallery
    // (which calls draw_text(..., ellipsis = false) -> SetTrimming(nullptr)) + cycle all tiers & tear down.
    // Previously caused a 0xc00000fd / 0xc000041d stack overflow from chained CreateEllipsisTrimmingSign calls.
    check(app.render_stress_frames(800, 120, 1220, 740),
          "Sustained 800 Home + 120 Trophy Gallery frames succeeded without trimming-sign stack overflow");
}

void test_wave_f_dojo_and_archive_installer() {
    // 1. Judgment Ring Dojo timing, grades, Blind fade, and Relay Combo escalation
    launcher::RingDojo dojo;
    dojo.set_mode(launcher::DojoMode::Standard);
    dojo.start_sweep();
    check(dojo.sweeping() && dojo.zones().size() == 3u, "RingDojo Standard mode starts with 3 attack zones");

    // Advance needle into zone 0 strike edge (109..118 deg at 225 deg/s -> ~0.50s)
    for (int i = 0; i < 50; ++i) dojo.update(0.01f); // 112.5 deg
    check(dojo.trigger_press() == launcher::DojoGrade::Perfect,
          "RingDojo awards Perfect Strike when pressed inside crimson strike edge");
    check(dojo.perfects() == 1 && dojo.streak() == 1 && dojo.best_streak() == 1,
          "RingDojo updates Perfect count and streak accurately");

    // Advance into zone 1 green hit area (176..205 deg -> ~0.35s more -> ~191 deg)
    for (int i = 0; i < 35; ++i) dojo.update(0.01f);
    check(dojo.trigger_press() == launcher::DojoGrade::Hit,
          "RingDojo awards Hit when pressed in green area before strike edge");

    // Blind mode alpha fade check (fades out between 30..70 deg before Zone 1)
    dojo.set_mode(launcher::DojoMode::Blind);
    dojo.start_sweep();
    check(dojo.zone_alpha() > 0.99f, "RingDojo Blind mode starts visible before 30 deg");
    for (int i = 0; i < 32; ++i) dojo.update(0.01f); // ~73.6 deg
    check(dojo.zone_alpha() <= 0.01f, "RingDojo Blind mode fades zones invisible after 70 deg");

    // 2. .nocturne-mod / .zip auto-classifier, Stored ZIP extraction, path-traversal guard & Mod Scaffold
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "sh2_wave_f_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    const launcher::LauncherPaths paths = launcher::resolve_paths(tmp, launcher::GameId::Covenant);
    std::filesystem::create_directories(paths.mods_dir, ec);
    std::filesystem::create_directories(paths.texture_packs_dir, ec);
    std::filesystem::create_directories(paths.video_packs_dir, ec);

    // Test Mod Scaffold creation
    std::string msg;
    check(launcher::create_mod_scaffold(paths, "Hardcore Test", &msg) &&
          std::filesystem::is_regular_file(paths.mods_dir / "Hardcore_Test" / "mod.json", ec) &&
          std::filesystem::is_regular_file(paths.mods_dir / "Hardcore_Test" / "patches" / "custom.pnach", ec) &&
          std::filesystem::is_regular_file(paths.mods_dir / "Hardcore_Test" / "scripts" / "hooks.lua", ec),
          "create_mod_scaffold generates mod.json, .pnach, and .lua template cleanly");

    // Helper to build a minimal Stored PKZIP file
    auto make_stored_zip = [](const std::filesystem::path &zip_out, const std::string &entry_name, const std::string &payload) {
        auto crc32_calc = [](const std::uint8_t *d, std::size_t n) {
            std::uint32_t c = 0xFFFFFFFFu;
            for (std::size_t i = 0; i < n; ++i) {
                c ^= d[i];
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            return c ^ 0xFFFFFFFFu;
        };
        std::vector<std::uint8_t> b;
        auto w16 = [&](std::uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); };
        auto w32 = [&](std::uint32_t v) { w16(v & 0xFFFF); w16((v >> 16) & 0xFFFF); };
        const std::uint32_t crc = crc32_calc(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
        w32(0x04034B50u); w16(20); w16(0); w16(0); w16(0); w16(0);
        w32(crc); w32(static_cast<std::uint32_t>(payload.size())); w32(static_cast<std::uint32_t>(payload.size()));
        w16(static_cast<std::uint16_t>(entry_name.size())); w16(0);
        b.insert(b.end(), entry_name.begin(), entry_name.end());
        b.insert(b.end(), payload.begin(), payload.end());
        std::ofstream ofs(zip_out, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    };

    const std::filesystem::path mod_zip = tmp / "BalancePack.nocturne-mod.zip";
    make_stored_zip(mod_zip, "patches/balance.pnach", "patch=1,EE,00200000,word,00000002\n");
    launcher::ArchivePackKind kind = launcher::ArchivePackKind::Unknown;
    check(launcher::import_mod_or_pack_archive(paths, mod_zip, &kind, &msg) &&
          kind == launcher::ArchivePackKind::GameplayMod &&
          std::filesystem::is_regular_file(paths.mods_dir / "BalancePack" / "patches" / "balance.pnach", ec),
          "import_mod_or_pack_archive auto-classifies and installs Gameplay Mod ZIP");

    const std::filesystem::path tex_zip = tmp / "CrispUI.zip";
    make_stored_zip(tex_zip, "12345678.png", "PNGDATA");
    check(launcher::import_mod_or_pack_archive(paths, tex_zip, &kind, &msg) &&
          kind == launcher::ArchivePackKind::TexturePack &&
          std::filesystem::is_regular_file(paths.texture_packs_dir / "CrispUI" / "12345678.png", ec),
          "import_mod_or_pack_archive auto-classifies and installs Texture Pack ZIP");

    const std::filesystem::path evil_zip = tmp / "Evil.zip";
    make_stored_zip(evil_zip, "../escape.txt", "BAD");
    check(!launcher::import_mod_or_pack_archive(paths, evil_zip, &kind, &msg),
          "import_mod_or_pack_archive rejects ../ path traversal");

    // 3. Wave D/F/G Settings keys.ini + launcher.ini persistence round-trip
    launcher::Settings s = launcher::load_settings(paths.keys_ini);
    s.hud_anchor = 1;
    s.anisotropy = 8;
    s.ring_hud = true;
    s.strict_ring_challenge = true;
    s.fsr_upscale = 2;
    s.cas_sharpen = 45;
    s.aa_mode = 2;
    s.lod_bias = -8;
    s.color_temp = -25;
    s.film_grain = 20;
    s.crt_mask = 1;
    s.brightness = 15;
    s.contrast = 95;
    s.gamma = 115;
    s.gpu_submit_thread = false;
    s.title_perf = true;
    s.renderer = 0;
    s.pgxp = false;
    s.filter_3d = 2;
    s.vram_budget = 2048;
    s.vram_guard = false;
    s.hw_rasterizer = false;
    s.dual_source_blend = false;
    s.tex_hot_reload = true;
    check(launcher::save_settings(paths.keys_ini, s), "save_settings writes Wave D/F/G settings");
    const launcher::Settings s2 = launcher::load_settings(paths.keys_ini);
    check(s2.hud_anchor == 1 && s2.anisotropy == 8 && s2.ring_hud && s2.strict_ring_challenge &&
          s2.fsr_upscale == 2 && s2.cas_sharpen == 45 && s2.aa_mode == 2 &&
          s2.lod_bias == -8 &&
          s2.color_temp == -25 && s2.film_grain == 20 && s2.crt_mask == 1 &&
          s2.brightness == 15 && s2.contrast == 95 && s2.gamma == 115 &&
          !s2.gpu_submit_thread && s2.title_perf &&
          s2.renderer == 0 && !s2.pgxp && s2.filter_3d == 2 &&
          s2.vram_budget == 2048 && !s2.vram_guard && !s2.hw_rasterizer &&
          !s2.dual_source_blend && s2.tex_hot_reload,
          "load_settings round-trips all Wave D/F/G graphics, shader, Koudelka, and expert engine settings");

    launcher::GlobalLauncherConfig gc = launcher::load_global_config(tmp);
    gc.dojo_best_streak = 42;
    gc.dojo_best_perfect = 19;
    check(launcher::save_global_config(tmp, gc), "save_global_config writes Ring Dojo personal bests");
    const launcher::GlobalLauncherConfig gc2 = launcher::load_global_config(tmp);
    check(gc2.dojo_best_streak == 42 && gc2.dojo_best_perfect == 19,
          "load_global_config round-trips Ring Dojo personal bests");

    // 4. One-time settings_schema = 2 migration to Pure Stock 1x (preserves audio/mods, never resets twice)
    {
        const std::filesystem::path legacy_ini = tmp / "legacy_keys.ini";
        {
            std::ofstream ofs(legacy_ini);
            ofs << "scale = 4\naspect = 16:9\ngpu_api = vulkan\nssao = 50\nmaster_volume = 70\nactive_mods = CrimsonFlow\n";
        }
        launcher::Settings migrated = launcher::load_settings(legacy_ini);
        check(migrated.scale == 1 && migrated.aspect == 0 && migrated.gpu_api == "auto" && migrated.ssao == 0 &&
              migrated.master_volume == 70 && migrated.active_mods == "CrimsonFlow",
              "One-time settings_schema=2 migration resets graphics to Pure Stock 1x while preserving audio & mods");
        migrated.scale = 2;
        migrated.aspect = 1;
        check(launcher::save_settings(legacy_ini, migrated), "save_settings persists user choices after migration");
        const launcher::Settings after = launcher::load_settings(legacy_ini);
        check(after.scale == 2 && after.aspect == 1,
              "Post-migration user settings (2x / 16:9) are remembered and never reset again");
    }

    std::filesystem::remove_all(tmp, ec);
}

void test_layer3_lua_and_mod_manager() {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "sh2_layer3_mod_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp / "games" / "sh1" / "mods", ec);

    launcher::LauncherState st;
    st.game_id = launcher::GameId::ShadowHearts1;
    st.paths = launcher::resolve_paths(tmp, launcher::GameId::ShadowHearts1);
    st.paths.scripts_root = std::filesystem::current_path();

    // 1. Install flagship SH1 Layer 3 mod: Chronicle Unleashed
    std::string msg;
    check(launcher::install_flagship_sh1_mod(st, &msg), "install_flagship_sh1_mod succeeded");
    check(st.mods.size() == 1u && st.mods[0].enabled && st.mods[0].lua_hook_count >= 1u,
          "Chronicle Unleashed appears enabled with Layer 3 Lua script");

    // 2. Duplicate & Delete mod in Launcher
    check(launcher::duplicate_mod(st, 0, &msg), "duplicate_mod copies Chronicle Unleashed");
    check(st.mods.size() == 2u && !st.mods[1].enabled, "duplicated mod is inserted disabled");
    check(launcher::delete_mod(st, 1, &msg), "delete_mod removes duplicated copy cleanly");
    check(st.mods.size() == 1u, "only original Chronicle Unleashed remains after delete");

    // 3. Execute Chronicle Unleashed Layer 3 Lua script against synthetic SH1 EE RAM
    mod_engine::Engine eng;
    eng.set_game_slug("sh1");
    eng.load_from_game_dir(st.paths.game_dir, st.settings.active_mods, true);
    check(eng.active_mod_count() == 1u && eng.script_op_count() >= 2u,
          "mod_engine loaded Chronicle Unleashed Lua script");

    std::vector<std::uint8_t> ram(8u * 1024u * 1024u, 0u);  // 8 MiB synthetic SH1 RAM
    auto wr32 = [&](std::uint32_t addr, std::uint32_t v) { std::memcpy(ram.data() + addr, &v, 4); };
    auto rd32 = [&](std::uint32_t addr) { std::uint32_t v = 0; std::memcpy(&v, ram.data() + addr, 4); return v; };
    std::memcpy(ram.data() + 0x004CAE40u, "Changchun", 9);
    wr32(0x0010000Cu, 5000u);       // 5s playtime
    ram[0x00100280u] = 0x40u;       // Yuri joined
    ram[0x00100280u + 0x03u] = 7u;  // Yuri Lv 7
    wr32(0x00100280u + 0x04u, 1200u); // Yuri EXP 1200
    wr32(0x001014A4u, 3u);          // 3 battles won

    eng.on_boot(ram.data(), ram.size());
    for (int f = 0; f < 15; ++f) eng.on_frame(ram.data(), ram.size());

    // All 5 remaining heroes (i=1..5) must now be unlocked at Lv 7, EXP 1200
    bool all_unlocked = true;
    for (int i = 1; i <= 5; ++i) {
        const std::uint32_t base = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
        if ((ram[base] & 0x40u) == 0u || ram[base + 0x03u] != 7u || rd32(base + 0x04u) != 1200u) {
            all_unlocked = false;
        }
    }
    check(all_unlocked, "Chronicle Unleashed Lua script unlocked all 6 SH1 heroes at Yuri's Lv/EXP");
    const auto toasts1 = eng.take_toasts();
    check(!toasts1.empty() && toasts1[0].find("All 6 Heroes Unlocked") != std::string::npos,
          "Chronicle Unleashed emitted live Gothic HUD unlock toast");

    // Simulate winning battle #4 where Yuri levels to Lv 8, EXP 1950 while bench stays at EXP 1200
    ram[0x00100280u + 0x03u] = 8u;
    wr32(0x00100280u + 0x04u, 1950u);
    wr32(0x001014A4u, 4u);
    for (int f = 0; f < 15; ++f) eng.on_frame(ram.data(), ram.size());

    bool bench_synced = true;
    for (int i = 1; i <= 5; ++i) {
        const std::uint32_t base = 0x00100280u + static_cast<std::uint32_t>(i) * 0x50u;
        if (ram[base + 0x03u] != 8u || rd32(base + 0x04u) != 1950u) bench_synced = false;
    }
    check(bench_synced, "Chronicle Unleashed Bench EXP Share synced all benched heroes after battle win");
    const auto toasts2 = eng.take_toasts();
    check(!toasts2.empty() && toasts2[0].find("Bench EXP Share") != std::string::npos,
          "Chronicle Unleashed emitted Bench EXP Share toast");

    // 4. Install & test Crimson Flow (Judgment Ring Momentum) across SH1, SH2: Covenant & SH3: FTNW
    check(launcher::install_crimson_flow_mod(st, &msg), "install_crimson_flow_mod succeeded");
    mod_engine::Engine cf_eng;
    cf_eng.load_from_game_dir(st.paths.game_dir, "CrimsonFlow,!ChronicleUnleashed", true);
    check(cf_eng.active_mod_count() == 1u && cf_eng.script_op_count() >= 5u,
          "mod_engine loaded Crimson Flow Lua script");

    // SH1 Perfect Strike streak -> Cash + Bonus EXP + Miss Shatter
    wr32(0x00100008u, 1000u);  // SH1 Cash = 1000
    wr32(0x001014ACu, 10u);    // ring_attempts
    wr32(0x001014B0u, 10u);    // ring_successes
    wr32(0x001014B4u, 5u);     // ring_perfects
    cf_eng.on_boot(ram.data(), ram.size());
    for (int f = 0; f < 6; ++f) cf_eng.on_frame(ram.data(), ram.size());
    (void)cf_eng.take_toasts();  // initial active toast

    // Chain 3 consecutive Perfect Strikes
    for (int s = 1; s <= 3; ++s) {
        wr32(0x001014ACu, 10u + static_cast<std::uint32_t>(s));
        wr32(0x001014B0u, 10u + static_cast<std::uint32_t>(s));
        wr32(0x001014B4u, 5u + static_cast<std::uint32_t>(s));
        for (int f = 0; f < 6; ++f) cf_eng.on_frame(ram.data(), ram.size());
    }
    // Rewards: x1 (+100), x2 (+200), x3 (+750) = +1050 Cash -> 2050
    check(rd32(0x00100008u) == 2050u, "Crimson Flow SH1 awarded escalating Cash (+1050) across 3 Perfects");
    const auto cf_toasts = cf_eng.take_toasts();
    check(cf_toasts.size() >= 3u && cf_toasts.back().find("CRIMSON FLOW x3!") != std::string::npos,
          "Crimson Flow SH1 emitted CRIMSON FLOW x3! callout");

    // Miss on attempt #14 -> shatters streak!
    wr32(0x001014ACu, 14u);
    for (int f = 0; f < 6; ++f) cf_eng.on_frame(ram.data(), ram.size());
    const auto shatter_t = cf_eng.take_toasts();
    check(!shatter_t.empty() && shatter_t[0].find("SHATTERED") != std::string::npos,
          "Crimson Flow SH1 emitted SHATTERED toast on Ring miss");

    // SH2: Covenant detection + Cash + Party HP Surge
    std::fill(ram.begin(), ram.end(), 0u);
    std::memcpy(ram.data() + 0x00100000u, "SSH_SAVE", 8);
    const float sh2_pt = 120.0f;
    std::memcpy(ram.data() + 0x00102300u, &sh2_pt, 4);
    ram[0x00100132u] = 0u;     // wallet 0
    wr32(0x00102D30u, 500u);   // SH2 active gold = 500
    ram[0x00100250u + 0x16u] = 'Y';
    std::uint16_t hm = 200u, hc = 100u;
    std::memcpy(ram.data() + 0x00100250u + 0x28u, &hm, 2);
    std::memcpy(ram.data() + 0x00100250u + 0x2Au, &hc, 2);
    cf_eng.on_boot(ram.data(), ram.size());
    for (int f = 0; f < 6; ++f) cf_eng.on_frame(ram.data(), ram.size());
    (void)cf_eng.take_toasts();
    wr32(0x001017C8u, 1u); wr32(0x001017CCu, 1u); wr32(0x001017D0u, 1u);
    for (int f = 0; f < 6; ++f) cf_eng.on_frame(ram.data(), ram.size());
    std::uint16_t hc_after = 0;
    std::memcpy(&hc_after, ram.data() + 0x00100250u + 0x2Au, 2);
    check(rd32(0x00102D30u) == 600u && hc_after == 110u,
          "Crimson Flow SH2: Covenant granted +100 Cash & +5% HP Surge on Perfect Strike");

    // 5. Semantic Layer 3 Lua API & Auto-Event Hooks (zero hex addresses required!)
    {
        const std::filesystem::path sem_dir = tmp / "games" / "sh2" / "mods" / "SemanticStudioMod";
        std::filesystem::create_directories(sem_dir / "scripts", ec);
        {
            std::ofstream mf(sem_dir / "mod.json");
            mf << "{\"name\":\"Semantic Studio Mod\",\"author\":\"Tester\",\"version\":\"1.0\"}\n";
        }
        {
            std::ofstream lf(sem_dir / "scripts" / "mod.lua");
            lf << "function on_game_ready()\n"
               << "  add_cash(250)\n"
               << "  toast(\"SEM_READY:\" .. get_cash())\n"
               << "end\n"
               << "function on_battle_win(total)\n"
               << "  add_cash(100)\n"
               << "  heal_party(10)\n"
               << "  toast(\"SEM_WIN:\" .. total)\n"
               << "end\n"
               << "function on_ring_perfect(streak)\n"
               << "  add_cash(50 * streak)\n"
               << "  toast(\"SEM_PERF:\" .. streak)\n"
               << "end\n"
               << "function on_ring_miss(prev_streak)\n"
               << "  toast(\"SEM_MISS:\" .. prev_streak)\n"
               << "end\n";
        }
        mod_engine::Engine sem_eng;
        sem_eng.set_game_slug("sh2");
        sem_eng.load_from_game_dir(tmp / "games" / "sh2", "SemanticStudioMod", true);
        check(sem_eng.active_mod_count() == 1u && sem_eng.script_op_count() == 4u,
              "Semantic Studio Mod loaded 4 auto-event hooks");

        // Reset SH2 RAM: cash=500, battles=2, rings=0/0/0, party[0] HP=100/200
        wr32(0x00102D30u, 500u);
        wr32(0x001016E4u, 2u);
        wr32(0x001017C8u, 0u); wr32(0x001017CCu, 0u); wr32(0x001017D0u, 0u);
        hm = 200u; hc = 100u;
        std::memcpy(ram.data() + 0x00100250u + 0x28u, &hm, 2);
        std::memcpy(ram.data() + 0x00100250u + 0x2Au, &hc, 2);

        sem_eng.on_boot(ram.data(), ram.size());
        for (int f = 0; f < 6; ++f) sem_eng.on_frame(ram.data(), ram.size());
        auto st1 = sem_eng.take_toasts();
        check(rd32(0x00102D30u) == 750u && !st1.empty() && st1[0] == "SEM_READY:750",
              "Semantic on_game_ready fired once and add_cash(250) updated SH2 wallet");

        // Trigger battle win (2 -> 3)
        wr32(0x001016E4u, 3u);
        for (int f = 0; f < 6; ++f) sem_eng.on_frame(ram.data(), ram.size());
        auto st2 = sem_eng.take_toasts();
        std::uint16_t hc_sem = 0;
        std::memcpy(&hc_sem, ram.data() + 0x00100250u + 0x2Au, 2);
        check(rd32(0x00102D30u) == 850u && hc_sem == 120u && !st2.empty() && st2[0] == "SEM_WIN:3",
              "Semantic on_battle_win fired with total=3, +100 cash & +10% party heal");

        // Trigger 2 Perfect Strikes then 1 Miss
        wr32(0x001017C8u, 1u); wr32(0x001017CCu, 1u); wr32(0x001017D0u, 1u);
        for (int f = 0; f < 6; ++f) sem_eng.on_frame(ram.data(), ram.size());
        wr32(0x001017C8u, 2u); wr32(0x001017CCu, 2u); wr32(0x001017D0u, 2u);
        for (int f = 0; f < 6; ++f) sem_eng.on_frame(ram.data(), ram.size());
        wr32(0x001017C8u, 3u);  // miss
        for (int f = 0; f < 6; ++f) sem_eng.on_frame(ram.data(), ram.size());
        auto st3 = sem_eng.take_toasts();
        check(rd32(0x00102D30u) == 1000u && st3.size() == 3u &&
              st3[0] == "SEM_PERF:1" && st3[1] == "SEM_PERF:2" && st3[2] == "SEM_MISS:2",
              "Semantic on_ring_perfect & on_ring_miss dispatched exact streak & cash math");
    }

    // 6. All-in-One Mod Engine v2: Persistent Save, Settings, Hotkeys, Per-Hero API, HUD Widgets, Conflicts & Inspector
    {
        const std::filesystem::path m1 = tmp / "games" / "sh2" / "mods" / "ModAlpha";
        const std::filesystem::path m2 = tmp / "games" / "sh2" / "mods" / "ModBeta";
        std::filesystem::create_directories(m1 / "scripts", ec);
        std::filesystem::create_directories(m1 / "textures", ec);
        std::filesystem::create_directories(m1 / "patches", ec);
        std::filesystem::create_directories(m2 / "textures", ec);
        std::filesystem::create_directories(m2 / "patches", ec);
        {
            std::ofstream f(m1 / "mod.json");
            f << "{\"name\":\"Mod Alpha\",\"settings\":[{\"key\":\"heal_val\",\"label\":\"Heal\",\"type\":\"int\",\"default\":180,\"min\":10,\"max\":500}]}\n";
        }
        {
            std::ofstream f(m1 / "textures" / "hero.png"); f << "PNG1";
            std::ofstream f2(m2 / "textures" / "hero.png"); f2 << "PNG2";
            std::ofstream p1(m1 / "patches" / "a.pnach"); p1 << "patch=0,EE,00102D30,word,000003E8\n";
            std::ofstream p2(m2 / "patches" / "b.pnach"); p2 << "patch=0,EE,00102D30,word,000007D0\n";
        }
        {
            std::ofstream lf(m1 / "scripts" / "alpha.lua");
            lf << "function on_hotkey(key)\n"
               << "  if key == \"F5\" then\n"
               << "    local target_hp = get_setting(\"heal_val\", 150)\n"
               << "    set_hero_hp(0, target_hp)\n"
               << "    local uses = save_get(\"f5_uses\", 0) + 1\n"
               << "    save_set(\"f5_uses\", uses)\n"
               << "    hud_set(\"counter\", \"HEALS\", tostring(uses), \"#7CD992\")\n"
               << "    log(\"F5_USED:\" .. uses .. \":\" .. get_hero_hp(0))\n"
               << "  end\n"
               << "end\n";
        }

        mod_engine::Engine v2;
        v2.set_game_slug("sh2-covenant");
        v2.set_dev_mode(true);
        v2.load_from_game_dir(tmp / "games" / "sh2", "ModAlpha,ModBeta,!SemanticStudioMod", true);
        check(v2.texture_override_count() == 1u && v2.conflicts().size() == 2u,
              "Engine v2 detects texture & .pnach conflicts across load order");

        // Trigger F5 hotkey -> sets hero 0 HP to setting default (180), saves f5_uses=1, pins HUD widget, logs
        v2.on_boot(ram.data(), ram.size());
        v2.set_input_state(mod_engine::mod_input::kKeyF5, 0u);
        v2.on_frame(ram.data(), ram.size());
        v2.set_input_state(0u, 0u);
        v2.on_frame(ram.data(), ram.size());

        std::uint16_t hp0 = 0;
        std::memcpy(&hp0, ram.data() + 0x00100250u + 0x2Au, 2);
        check(hp0 == 180u, "Per-hero set_hero_hp + get_setting applied setting default (180)");
        check(v2.hud_widgets().size() == 1u && v2.hud_widgets()[0].value == "1",
              "hud_set pinned Gothic HUD widget with live counter");
        check(!v2.recent_logs().empty() && v2.recent_logs().back().find("F5_USED:1:180") != std::string::npos,
              "Structured Lua log() captured in dev inspector ring buffer");

        // Reload engine -> verify mod_state.ini persisted f5_uses=1 -> next F5 produces f5_uses=2!
        v2.manual_reload(ram.data(), ram.size(), false);
        v2.set_input_state(mod_engine::mod_input::kKeyF5, 0u);
        v2.on_frame(ram.data(), ram.size());
        check(v2.hud_widgets().size() == 1u && v2.hud_widgets()[0].value == "2",
              "save_get/save_set persisted mod state across manual_reload");

        const auto snap = v2.capture_inspector_snapshot(ram.data(), ram.size());
        check(snap.in_game && snap.detected_game == 2 && !snap.heroes.empty() && snap.heroes[0].hp == 180,
              "capture_inspector_snapshot reports live SH2 party RAM accurately");
    }

    // 7. Verify Per-Mod Configurable Settings (bool, int, float, choice, hotkey),
    //    mod_config.ini round-trip, get_setting / get_setting_str, live in-place update
    //    preserving Lua globals + on_setting_changed(key, val), and Reset to Defaults.
    {
        const std::filesystem::path gdir = tmp / "games" / "sh2-covenant";
        const std::filesystem::path mdir = gdir / "mods" / "lantern_cfg";
        std::filesystem::create_directories(mdir / "scripts", ec);
        std::ofstream(mdir / "mod.json") << R"({
            "id": "lantern_cfg",
            "name": "Nemeton Lantern",
            "version": "1.2",
            "author": "Odin",
            "description": "Configurable item glow",
            "settings": [
                {"key": "enabled", "label": "Glow Enabled", "description": "Master switch", "type": "bool", "default": 1},
                {"key": "strength", "label": "Glow Strength", "type": "float", "default": 0.75, "min": 0.25, "max": 1.5, "step": 0.25},
                {"key": "palette", "label": "Glow Color", "type": "choice", "options": ["Gold", "Cyan", "Crimson"], "default": "Cyan"},
                {"key": "toggle_key", "label": "Toggle Key", "type": "hotkey", "default": "*"}
            ]
        })";
        std::ofstream(mdir / "scripts" / "lantern.lua") <<
            "local counter = 0\n"
            "function on_boot()\n"
            "  counter = 42\n"
            "  hud_set('cfg', 'CFG', get_setting_str('palette', '') .. ':' .. get_setting_str('toggle_key', ''), '#E6C88A')\n"
            "end\n"
            "function on_setting_changed(k, v)\n"
            "  counter = counter + 1\n"
            "  hud_set('chg', k, tostring(v) .. ':' .. tostring(counter), '#7CD992')\n"
            "end\n";

        auto scanned = mod_engine::Engine::scan_installed_mods(gdir, "lantern_cfg");
        std::size_t idx = scanned.size();
        for (std::size_t i = 0; i < scanned.size(); ++i) {
            if (scanned[i].folder == "lantern_cfg") { idx = i; break; }
        }
        check(idx < scanned.size() && scanned[idx].settings.size() == 4u,
              "scan_installed_mods parses all 4 setting types (bool, float, choice, hotkey)");
        if (idx < scanned.size() && scanned[idx].settings.size() == 4u) {
            auto &sets = scanned[idx].settings;
            check(mod_engine::format_mod_setting_value(sets[0]) == "ON" &&
                  mod_engine::format_mod_setting_value(sets[1]) == "0.75" &&
                  mod_engine::format_mod_setting_value(sets[2]) == "Cyan" &&
                  mod_engine::format_mod_setting_value(sets[3]) == "*",
                  "Default values format accurately across bool/float/choice/hotkey");

            mod_engine::Engine eng;
            eng.load_from_game_dir(gdir, "lantern_cfg", true);
            std::vector<std::uint8_t> ram(0x10000u, 0u);
            eng.on_boot(ram.data(), ram.size());
            check(!eng.hud_widgets().empty() && eng.hud_widgets()[0].value == "Cyan:*",
                  "Lua get_setting_str() reads choice label and hotkey default");

            // Step choice (Cyan -> Crimson) and hotkey (* -> F5) and apply live without resetting counter=42
            mod_engine::step_mod_setting(sets[2], 1);
            mod_engine::step_mod_setting(sets[3], 1);
            check(mod_engine::format_mod_setting_value(sets[2]) == "Crimson" &&
                  mod_engine::format_mod_setting_value(sets[3]) == "F5",
                  "step_mod_setting advances choice and hotkey cleanly");
            check(eng.apply_mod_settings_live("lantern_cfg", sets, ram.data(), ram.size()),
                  "apply_mod_settings_live updates running script in-place");

            // counter was 42; 2 settings changed -> counter should be 44 (not reset!)
            bool saw_live_hook = false;
            for (const auto &w : eng.hud_widgets()) {
                if (w.label == "toggle_key" && w.value == "F5:44") saw_live_hook = true;
            }
            check(saw_live_hook,
                  "on_setting_changed(key, val) fired live while preserving Lua runtime globals");

            // Re-scan from disk to verify mod_config.ini persistence
            auto rescanned = mod_engine::Engine::scan_installed_mods(gdir, "lantern_cfg");
            bool persisted = false;
            for (const auto &rm : rescanned) {
                if (rm.folder == "lantern_cfg" && rm.settings.size() == 4u) {
                    persisted = (mod_engine::format_mod_setting_value(rm.settings[2]) == "Crimson" &&
                                 mod_engine::format_mod_setting_value(rm.settings[3]) == "F5");
                }
            }
            check(persisted, "mod_config.ini persists choice and hotkey overrides across restarts");

            // Reset to defaults
            mod_engine::reset_mod_settings(sets);
            check(mod_engine::format_mod_setting_value(sets[2]) == "Cyan" &&
                  mod_engine::format_mod_setting_value(sets[3]) == "*",
                  "reset_mod_settings restores all defaults cleanly");
        }
        check(mod_engine::mod_input::key_mask_from_name("*") == mod_engine::mod_input::kKeyAsterisk &&
              mod_engine::mod_input::key_mask_from_name("KP_MULTIPLY") == mod_engine::mod_input::kKeyAsterisk,
              "mod_input maps * and KP_MULTIPLY to kKeyAsterisk");
    }

    // 8. Verify Archives & Lore Library (per-game manuals, manga, custom import, progress, natural sort)
    {
        launcher::LauncherState lst;
        lst.game_id = launcher::GameId::Koudelka;
        lst.paths = launcher::resolve_paths(tmp, launcher::GameId::Koudelka);
        launcher::load_library(lst);
        check(lst.library.size() == 14u, "Default Archives catalog loads 4 game manuals + 4 strategy guides + 6 manga/novel/lore books");
        const auto *kman = launcher::find_game_manual(lst, launcher::GameId::Koudelka);
        check(kman != nullptr && kman->file_path.empty(), "Koudelka manual starts unimported");

        // Import a mock PDF for Koudelka's manual and a mock CBZ for Manga Vol 1
        const std::filesystem::path src_pdf = tmp / "Koudelka_Manual.pdf";
        const std::filesystem::path src_cbz = tmp / "Koudelka_Vol1.cbz";
        std::ofstream(src_pdf, std::ios::binary) << "%PDF-1.4 mock manual";
        std::ofstream(src_cbz, std::ios::binary) << "PK\x03\x04 mock cbz";
        std::string msg;
        check(launcher::import_library_file(lst, "manual_koudelka", src_pdf, &msg),
              "import_library_file imports Koudelka manual PDF into library/manuals/");
        check(launcher::import_library_file(lst, "koudelka_manga_v1", src_cbz, &msg),
              "import_library_file imports Koudelka Manga Vol 1 CBZ into library/books/");

        kman = launcher::find_game_manual(lst, launcher::GameId::Koudelka);
        check(kman != nullptr && !kman->file_path.empty() && kman->bytes > 0,
              "find_game_manual locates imported Koudelka booklet");

        // Save & reload bookmark progress
        check(launcher::save_library_progress(lst.paths.library_dir, "koudelka_manga_v1", 12, 180, true, true),
              "save_library_progress writes bookmark to library/progress.ini");
        launcher::load_library(lst);
        bool prog_ok = false;
        for (const auto &it : lst.library) {
            if (it.id == "koudelka_manga_v1" && it.last_page == 12 && it.total_pages == 180 && it.saved_spread && it.saved_rtl) {
                prog_ok = true;
            }
        }
        check(prog_ok, "load_library restores saved page, total_pages, spread, and RTL state");

        // Natural alphanumeric page sort (page_2.jpg before page_10.jpg)
        check(launcher::natural_less("page_2.jpg", "page_10.jpg") && !launcher::natural_less("page_10.jpg", "page_2.jpg"),
              "natural_less orders manga pages numerically (page_2 < page_10)");

        // Ignore macOS __MACOSX/ and AppleDouble ._* metadata entries in CBZ caches
        check(launcher::is_ignored_archive_path("__MACOSX/._a000.jpg.jpg"),
              "is_ignored_archive_path rejects __MACOSX AppleDouble entries");
        check(launcher::is_ignored_archive_path("._cover.jpg") && launcher::is_ignored_archive_path(".DS_Store"),
              "is_ignored_archive_path rejects dot-prefixed metadata files");
        check(!launcher::is_ignored_archive_path("a000.jpg.jpg") && !launcher::is_ignored_archive_path("ch01/page_001.png"),
              "is_ignored_archive_path accepts normal manga page paths");
        {
            const std::filesystem::path mock_cache = lst.paths.library_dir / ".cache" / "koudelka_manga_v1";
            std::error_code ec;
            std::filesystem::create_directories(mock_cache / "__MACOSX", ec);
            std::ofstream(mock_cache / "__MACOSX" / "._a000.jpg.jpg", std::ios::binary) << "\x00\x05\x16\x07Mac";
            std::ofstream(mock_cache / "a000.jpg.jpg", std::ios::binary) << "\xFF\xD8\xFF\xE0";
            std::ofstream(mock_cache / "a001.jpg.jpg", std::ios::binary) << "\xFF\xD8\xFF\xE0";
            std::ofstream(mock_cache / "_cbz_ready.txt", std::ios::binary) << "ok";
            const auto cover = launcher::find_cached_cover_page(lst.paths.library_dir, "koudelka_manga_v1");
            check(cover.filename() == "a000.jpg.jpg",
                  "find_cached_cover_page skips __MACOSX/._* and picks real cover page");
            launcher::LibraryReader rdr;
            rdr.open("koudelka_manga_v1", src_cbz, lst.paths.library_dir / ".cache", 0);
            for (int w = 0; w < 200 && rdr.is_busy(); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(rdr.page_count() == 2 && rdr.page_image_path(0).filename() == "a000.jpg.jpg" &&
                      rdr.page_image_path(1).filename() == "a001.jpg.jpg",
                  "LibraryReader filters __MACOSX/._* and indexes only real manga pages");
            rdr.close();
        }

        // Remove imported manual cleanly
        check(launcher::remove_library_file(lst, "manual_koudelka", &msg) &&
              launcher::find_game_manual(lst, launcher::GameId::Koudelka)->file_path.empty(),
              "remove_library_file deletes local copy cleanly");
    }

    // 9. Verify Phase 1 Total-Control Mod Engine APIs (2D Canvas, AOB scan_bytes, read/write_str,
    //    pad_press/pad_block, Koudelka lifecycle hooks, and 1-click .nocturne-mod export)
    {
        const std::filesystem::path gdir = tmp / "games" / "koudelka_tc";
        const std::filesystem::path mdir = gdir / "mods" / "total_ctrl";
        std::filesystem::create_directories(mdir / "scripts", ec);
        std::ofstream(mdir / "mod.json") << R"({"id":"total_ctrl","name":"Total Control Test","version":"1.0","author":"Odin"})";
        std::ofstream(mdir / "scripts" / "tc.lua") <<
            "function on_boot()\n"
            "  write_str(0x001000, 'Koudelka')\n"
            "  local pos = scan_bytes('4B 6F ?? 64 65 6C 6B 61', 0x000F00, 0x001200)\n"
            "  hud_set('aob', 'AOB', read_str(pos, 16) .. '@' .. tostring(pos), '#E6C88A')\n"
            "end\n"
            "function on_frame()\n"
            "  draw_rect(20, 20, 200, 40, '#121018', 200)\n"
            "  draw_bar(24, 28, 192, 20, 0.75, '#D84848', '#16121C', 'BOSS HP')\n"
            "  draw_text(24, 52, 'TURN READY', 14, '#FFE080')\n"
            "  pad_press('CROSS')\n"
            "  pad_block('START')\n"
            "end\n"
            "function on_area_load(r) hud_set('area', 'ROOM', tostring(r), '#7CD992') end\n"
            "function on_battle_start() hud_set('btl', 'BTL', 'START', '#FF6666') end\n"
            "function on_turn_begin(c) hud_set('trn', 'TURN', tostring(c), '#FFCC44') end\n";

        mod_engine::Engine tc;
        tc.load_from_game_dir(gdir, "total_ctrl", true);
        std::vector<std::uint8_t> psx_ram(0x200000u, 0u);
        auto wr32 = [&](std::uint32_t off, std::uint32_t v) { std::memcpy(psx_ram.data() + off, &v, 4); };
        // Prime Koudelka RAM signature for semantic lifecycle hooks
        wr32(0x000B59A0u, 0x80100100u);
        wr32(0x00100100u - 0x44u, 0x80100200u);
        psx_ram[0x00100200u + 0x146u] = 'K';
        wr32(0x000AF470u, 0x801F1590u);
        wr32(0x000B6B08u, 34u);

        tc.on_boot(psx_ram.data(), psx_ram.size());
        bool aob_ok = false;
        for (const auto &w : tc.hud_widgets()) {
            if (w.label == "AOB" && w.value == "Koudelka@4096") aob_ok = true;
        }
        check(aob_ok, "Lua write_str + scan_bytes(wildcard) + read_str work accurately");

        for (int f = 0; f < 6; ++f) tc.on_frame(psx_ram.data(), psx_ram.size());
        check(tc.canvas_cmds().size() == 3u &&
              (tc.pad_inject_mask() & mod_engine::mod_input::kPadCross) != 0u &&
              (tc.pad_block_mask() & mod_engine::mod_input::kPadStart) != 0u,
              "Lua 2D canvas (draw_rect/draw_bar/draw_text) and pad_press/pad_block work accurately");

        // Trigger room change + battle start + Edward's turn (char 1)
        wr32(0x000B6B08u, 42u);
        wr32(0x001F1590u + 0x36Cu, 1u);
        wr32(0x000B4D5Cu, 1u);
        for (int f = 0; f < 6; ++f) tc.on_frame(psx_ram.data(), psx_ram.size());
        bool saw_area = false, saw_btl = false, saw_trn = false;
        for (const auto &w : tc.hud_widgets()) {
            if (w.label == "ROOM" && w.value == "42") saw_area = true;
            if (w.label == "BTL" && w.value == "START") saw_btl = true;
            if (w.label == "TURN" && w.value == "1") saw_trn = true;
        }
        check(saw_area && saw_btl && saw_trn,
              "Koudelka lifecycle hooks on_area_load, on_battle_start, and on_turn_begin fire accurately");

        // Verify 1-click .nocturne-mod export package
        launcher::LauncherState est;
        est.paths.backups_dir = tmp / "exports";
        launcher::ModInfo emi{};
        emi.name = "Total Control Test";
        emi.path = mdir;
        est.mods.push_back(emi);
        std::filesystem::path pkg_out;
        std::string emsg;
        check(launcher::export_mod_package(est, 0, &pkg_out, &emsg) &&
              std::filesystem::is_regular_file(pkg_out, ec) &&
              std::filesystem::file_size(pkg_out, ec) > 100u,
              "export_mod_package creates valid shareable .nocturne-mod archive");
    }

    std::filesystem::remove_all(tmp, ec);
}

void test_card_binder_and_judgment_ring_duel() {
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "sh2_card_duel_test";
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    // 1. Verify full 4-game catalog with fixed per-card rarities + 12 Crest Spells
    const auto &cat = cards::catalog();
    check(cat.size() >= 110u, "Card catalog has full 4-game anthology roster + 12 Crest Spells (>=110 cards)");
    int per_game[4] = {0, 0, 0, 0};
    int spell_count = 0;
    bool all_have_hints_and_stats = true;
    for (const auto &c : cat) {
        const int gi = static_cast<int>(c.game);
        if (gi >= 0 && gi < 4) ++per_game[gi];
        if (c.is_spell()) {
            ++spell_count;
            if (c.obtain_hint.empty() || c.cost < 1 || c.cost > 6) all_have_hints_and_stats = false;
        } else if (c.obtain_hint.empty() || c.cost < 1 || c.cost > 6 || c.power < 1 || c.power > 10) {
            all_have_hints_and_stats = false;
        }
    }
    check(spell_count == 12, "Catalog includes all 12 Shadow Hearts Crest Spells");
    check(per_game[0] >= 20 && per_game[1] >= 20 && per_game[2] >= 20 && per_game[3] >= 20,
          "All 4 Shadow Hearts chronicles have >=20 cards each");
    check(all_have_hints_and_stats, "Every card has obtain hint and valid Judgment Ring Duel stats");

    // 2. Verify fixed rarity invariant + legacy v1 JSON migration
    const std::filesystem::path col_path = tmp / "cards_collection.json";
    {
        std::ofstream ofs(col_path, std::ios::binary);
        ofs << R"({"version":1,"essence":250,"owned":[{"id":"kdl_elaine_final","counts":[1,2,0,0,0,0,0,0,0,0,1],"first_acquired":"2026-11-20","last_source":"Boss"}]})";
    }
    cards::Collection col;
    check(col.load(col_path), "Collection loads legacy v1 JSON cleanly");
    const auto *oc = col.find_owned("kdl_elaine_final");
    check(oc && oc->counts[0] == 1 && oc->counts[1] == 2 && oc->counts[2] == 1,
          "Legacy 11-variant counts migrate losslessly into [Standard, Foil, Animated]");

    // Rolling a card always preserves its fixed rarity
    const auto drop = col.roll_single_card(cards::SeriesGame::Koudelka, 0, "Test Drop", "kdl_elaine_final");
    check(drop.rarity == cards::Rarity::Legendary,
          "Fixed rarity invariant: Elaine Final Form is always Legendary even on luck_tier=0");

    // 3. Verify 20-card Deck Validation & Rarity Caps
    const auto trial = card_game::trial_deck_ids();
    const auto tv = card_game::validate_deck(trial, nullptr);
    check(tv.valid && tv.count == 20, "Built-in Trial Deck passes 20-card validation");

    std::vector<std::string> too_many_leg = trial;
    too_many_leg[0] = "kdl_elaine_final";
    too_many_leg[1] = "sh1_meta_god";
    too_many_leg[2] = "sh2_umbral_god";
    const auto lv = card_game::validate_deck(too_many_leg, nullptr);
    check(!lv.valid, "Deck validator enforces max 2 Legendary cap");

    // 4. Verify Judgment Ring Duel v2 Mechanics:
    //    Elemental Advantage, Ring Harmony, Channel Card, Alternating Initiative, Combo Chain, Berserk
    check(card_game::element_beats(cards::Element::Fire, cards::Element::Wind) &&
          card_game::element_beats(cards::Element::Wind, cards::Element::Earth) &&
          card_game::element_beats(cards::Element::Earth, cards::Element::Water) &&
          card_game::element_beats(cards::Element::Water, cards::Element::Fire) &&
          card_game::element_volatile(cards::Element::Light, cards::Element::Dark),
          "Elemental counter cycle & Light/Dark volatile clash rules work accurately");

    card_game::DuelMatch match;
    match.start(trial, 0, true, false, 424242u);
    check(match.phase == card_game::DuelPhase::Mulligan &&
          match.players[0].hand.size() == static_cast<std::size_t>(card_game::kStartingHandSize),
          "Judgment Ring Duel opens in Opening Mulligan phase with full starting hand");
    match.toggle_mulligan_mark(0);
    match.toggle_mulligan_mark(1);
    check(match.mulligan_mark_count() == 2, "Mulligan marks 2 cards for swap");
    match.confirm_mulligan();
    check(match.phase == card_game::DuelPhase::PlayerDeploy &&
          match.players[0].hand.size() == static_cast<std::size_t>(card_game::kStartingHandSize) &&
          match.players[0].sanity == 20 && match.players[0].soul_energy == 1 && match.ai_deployed_first,
          "Confirming Mulligan keeps starting hand size and begins Round 1 AI-first counter-deploy initiative");

    // Verify Channel Card (+1 Soul Energy, once per round)
    check(match.channel_card(0, 0) && match.players[0].soul_energy == 2 && !match.can_channel_card(0, 0),
          "Channel Card grants +1 Soul Energy once per round to prevent bricked hands");

    // Play a card if available, then resolve sweep
    for (int h = 0; h < static_cast<int>(match.players[0].hand.size()); ++h) {
        if (match.can_play_card(0, h, 0)) {
            check(match.play_card(0, h, 0), "Player deploys opening card onto Altar Node 1");
            break;
        }
    }
    match.resolve_full_sweep();
    check(match.round_num == 2 || match.phase == card_game::DuelPhase::Finished,
          "Clockwise Judgment Hand sweep resolves all 5 Nodes and advances round");
    if (match.round_num == 2) {
        check(!match.ai_deployed_first, "Round 2 alternates initiative so Player leads first");
    }

    // Verify Berserk Surge (+1 Power at <= 6 Sanity)
    match.players[0].sanity = 5;
    check(match.players[0].is_berserk(), "Berserk Surge activates at <= 6 Sanity");

    // Simulate up to 16 rounds to guarantee match terminates cleanly
    for (int r = 0; r < 16 && match.phase != card_game::DuelPhase::Finished; ++r) {
        for (int h = 0; h < static_cast<int>(match.players[0].hand.size()); ++h) {
            for (int n = 0; n < card_game::kNodeCount; ++n) {
                if (match.can_play_card(0, h, n)) {
                    match.play_card(0, h, n);
                    break;
                }
            }
        }
        match.resolve_full_sweep();
    }
    check(match.phase == card_game::DuelPhase::Finished && match.winner >= 0,
          "Full Judgment Ring Duel match finishes cleanly with a winner");

    // Verify Crest Spell casting (Soul Surge, Counter-Clockwise Ring Reversal, Graveyard Resurrection)
    {
        card_game::DuelMatch sm;
        sm.start(trial, 0, true, false, 999u, true);
        sm.players[0].soul_energy = 6;
        sm.players[0].hand = {"sh2_harmonixer_surge", "sh1_judgment_reversal", "kdl_emigre_ritual"};
        sm.players[0].graveyard = {"kdl_elaine_final"};
        check(sm.play_card(0, 0, 0) && sm.players[0].soul_energy == 7,
              "Crest Spell Harmonixer Surge grants +2 Soul Energy");
        check(sm.play_card(0, 0, 0) && sm.reverse_sweep_this_round && sm.sweep_node_for_step(0) == 4,
              "Crest Spell Judgment Reversal flips sweep Counter-Clockwise (5->1)");
        check(sm.play_card(0, 0, 0) && sm.nodes[0].slots[0].occupied &&
              sm.nodes[0].slots[0].card_id == "kdl_elaine_final",
              "Crest Spell Emigre Resurrection revives strongest Graveyard unit onto Pillar");
    }

    // Verify Smart AI Seal Defense & Crest Spell Casting (Hard AI contests Player's 3-Seal threat!)
    {
        card_game::DuelMatch aim;
        aim.start(trial, 4, true, false, 12345u, true);  // Hard AI (Albert Simon)
        aim.nodes[0].seal_owner = 0;
        aim.nodes[1].seal_owner = 0;
        aim.nodes[2].seal_owner = 0;
        aim.nodes[2].slots[0].occupied = true;
        aim.nodes[2].slots[0].card_id = "kdl_edward";
        aim.nodes[2].slots[0].base_power = 4;
        aim.nodes[2].slots[0].ward_active = true;
        aim.players[1].soul_energy = 6;
        aim.players[1].hand = {"sh1_purge_malice", "sh1_yuri"};
        aim.run_ai_deploy();
        check(!aim.nodes[2].slots[0].ward_active && aim.nodes[2].slots[1].occupied,
              "Hard AI casts Exorcist's Purge to strip Ward and contests Player's 3-Seal threat Pillar");
    }

    // Verify In-Game NPC Challengers, 3-Difficulty Wager Scaling & True Triple Triad Card Removal
    {
        const auto *roger_npc = card_game::find_npc_for_room(cards::SeriesGame::Koudelka, 0x18u);
        check(roger_npc != nullptr && roger_npc->wager_type == card_game::WagerType::Card &&
              roger_npc->wager_card_id == "kdl_roger",
              "Koudelka room 0x18 maps to Roger Bacon Card Wager NPC Challenger");
        check(card_game::scaled_money_wager(400, card_game::AiDifficulty::Easy) == 400 &&
              card_game::scaled_money_wager(400, card_game::AiDifficulty::Normal) == 600 &&
              card_game::scaled_money_wager(400, card_game::AiDifficulty::Hard) == 800,
              "Money wager scales cleanly across Easy / Normal / Hard");
        col.grant_card("kdl_alias", cards::CardFinish::Standard, "Test Ante");
        check(col.remove_card_copy("kdl_alias"),
              "Collection::remove_card_copy removes forfeited ante card on Card Wager loss");
    }

    // 5. Verify 6-Duelist Anthology Ladder, Boss Arena Modifiers & Headless Direct2D renders
    check(card_game::opponents().size() == 6u, "6-Duelist Anthology Ladder is populated");
    {
        card_game::DuelMatch simon_match;
        simon_match.start(trial, 4, true, false, 777u);  // Albert Simon: Neameto Cataclysm (2 starting Soul Energy)
        check(simon_match.active_arena_mod() == card_game::ArenaMod::NeametoSurge &&
              simon_match.players[0].max_soul_energy == 2,
              "Boss Arena Modifier (Neameto Cataclysm) grants 2 starting Soul Energy on Round 1");
    }
    col.duel_rules_seen = true;
    col.duel_wins = 9;
    col.duel_win_streak = 4;
    check(col.save() && col.load(col_path) && col.duel_rules_seen && col.duel_wins == 9 &&
          col.duel_win_streak == 4 && std::string(cards::duelist_rank_name(col.duel_wins)) == "Gold Inquisitor",
          "Duel rules_seen, W/L, win streak & Duelist Rank persist cleanly");

    launcher::LauncherApp app(tmp);
    app.set_page(launcher::Page::CardAlbum);
    app.set_card_view_mode(0);
    check(app.render_to_png(tmp / "binder_preview.png", 1220, 740),
          "Headless Direct2D renders 3-Ring Card Binder cleanly");
    app.start_card_duel_preview(0, true);
    check(app.render_to_png(tmp / "duel_preview.png", 1220, 740),
          "Headless Direct2D renders Judgment Ring Duel arena cleanly");
    app.set_card_rules_open(true);
    check(app.render_to_png(tmp / "rules_preview.png", 1220, 740),
          "Headless Direct2D renders Judgment Ring Duel Rules Grimoire modal cleanly");

    std::filesystem::remove_all(tmp, ec);
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
    test_wave_c_packs_mods_playtime_and_save_bundle();
    test_wave_f_dojo_and_archive_installer();
    test_layer3_lua_and_mod_manager();
    test_koudelka_trophies_and_ram_tracker();
    test_card_binder_and_judgment_ring_duel();
    test_headless_d2d_renders(dump_dir);

    if (SUCCEEDED(hr)) CoUninitialize();
    if (g_fails == 0) {
        std::printf("ALL LAUNCHER & DIRECT2D VISUAL TESTS PASSED!\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed.\n", g_fails);
    return 1;
}
