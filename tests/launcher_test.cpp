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
        std::ofstream(tmp_root / "Shadow Hearts Covenant.exe") << "exe";
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
    st.paths.root_dir = tmp_root;
    launcher::refresh_all(st);

    check(st.ready_to_play, "mock installation detected as ready to play");
    check(st.occupied_slots == 1 && st.latest_slot == 0, "Slot 0 detected as occupied & latest");
    check(st.slots[0].area == "Yokohama" && st.slots[0].max_level == 48 && st.slots[0].disc == 2,
          "Slot 0 metadata parsed accurately");
    check(st.slots[0].has_thumb && st.slots[0].thumb_w == 16 && st.slots[0].thumb_h == 12 &&
          st.slots[0].thumb_bgra.size() == 192u, "Slot 0 PS2THB1 thumbnail decoded to BGRA");

    // Verify launch command targets Shadow Hearts Covenant.exe with both discs
    const std::wstring cmd_win = launcher::build_launch_command(st.paths, false);
    const std::wstring cmd_fs = launcher::build_launch_command(st.paths, true);
    check(cmd_win.find(L"Shadow Hearts Covenant.exe") != std::wstring::npos, "launch command targets Shadow Hearts Covenant.exe");
    check(cmd_win.find(L"--disc ") != std::wstring::npos && cmd_win.find(L"--disc2 ") != std::wstring::npos,
          "launch command passes Disc 1 and Disc 2");
    check(cmd_win.find(L"--window") != std::wstring::npos && cmd_fs.find(L"--fullscreen") != std::wstring::npos,
          "launch command toggles --window and --fullscreen accurately");

    // Verify timestamped memcard backup
    std::filesystem::path bkp;
    std::string bmsg;
    check(launcher::backup_memcard(st.paths, &bkp, &bmsg) && std::filesystem::is_regular_file(bkp, ec),
          "backup_memcard creates timestamped .ps2 backup");

    std::filesystem::remove_all(tmp_root, ec);
}

void test_headless_d2d_renders(const std::string &dump_dir) {
    const std::filesystem::path root = std::filesystem::current_path();
    launcher::LauncherApp app(root);

    // Populate a few representative unlocked trophies in-memory so the visual preview shows unlocked + locked + missable states
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

    const std::filesystem::path out_dir = dump_dir.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(dump_dir);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

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
