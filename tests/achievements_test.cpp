#include "achievements.hpp"
#include "quickmenu.hpp"
#include "screenshot.hpp"
#include "ui_draw.hpp"
#include "ps2/memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
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

void clear_ram_block() {
    std::memset(ps2::g_ram + 0x00100100u, 0, 0x3500u);
}

void init_valid_ssh_save(const char *map_id = "AP_02_U", float playtime = 120.0f) {
    clear_ram_block();
    static constexpr char kMagic[8] = {'S', 'S', 'H', '_', 'S', 'A', 'V', 'E'};
    for (int i = 0; i < 8; ++i) ps2::st8(0x00100104u + i, static_cast<std::uint8_t>(kMagic[i]));
    ps2::st32(0x00100120u, 0x304C5442u);  // "BTL0"
    ps2::st32(0x001022ECu, 0x61444C46u);  // "FLDa"
    ps2::st32(0x00102D1Cu, 0x554E454Du);  // "MENU"
    std::uint32_t pt_bits = 0;
    std::memcpy(&pt_bits, &playtime, sizeof pt_bits);
    ps2::st32(0x00102300u, pt_bits);
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t c = (i < static_cast<int>(std::strlen(map_id))) ? static_cast<std::uint8_t>(map_id[i]) : 0u;
        ps2::st8(0x0010230Cu + i, c);
    }
    ps2::st8(0x00100132u, 0u);  // wallet_idx = 0
}

void set_map(const char *map_id) {
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t c = (i < static_cast<int>(std::strlen(map_id))) ? static_cast<std::uint8_t>(map_id[i]) : 0u;
        ps2::st8(0x0010230Cu + i, c);
    }
}

void set_party_slot(int slot, std::uint8_t char_id, std::uint8_t lv, const char *name,
                    std::uint16_t hp_max = 100, std::uint16_t hp_cur = 100) {
    const std::uint32_t base = 0x00100250u + static_cast<std::uint32_t>(slot) * 0x90u;
    std::memset(ps2::g_ram + base, 0, 0x90u);
    ps2::st8(base + 0x0Eu, char_id);
    ps2::st8(base + 0x0Fu, lv);
    for (int i = 0; i < 15 && name[i]; ++i) ps2::st8(base + 0x16u + i, static_cast<std::uint8_t>(name[i]));
    ps2::st16(base + 0x28u, hp_max);
    ps2::st16(base + 0x2Au, hp_cur);
}

void set_gold(std::uint32_t active, std::uint32_t cumulative) {
    ps2::st32(0x00102D30u, active);
    ps2::st32(0x00103018u, cumulative);
}

void set_battle_ring(std::uint32_t entered, std::uint32_t won, std::uint32_t attempts,
                     std::uint32_t successes, std::uint32_t perfects) {
    ps2::st32(0x001015ECu, entered);
    ps2::st32(0x001016E4u, won);
    ps2::st32(0x001017C8u, attempts);
    ps2::st32(0x001017CCu, successes);
    ps2::st32(0x001017D0u, perfects);
}

void set_treasures(int count) {
    std::memset(ps2::g_ram + 0x00102B90u, 0, 128u);
    for (int i = 0; i < count && i < 1024; ++i) {
        const std::uint32_t a = 0x00102B90u + static_cast<std::uint32_t>(i / 8);
        ps2::st8(a, static_cast<std::uint8_t>(ps2::ld8(a) | (1u << (i % 8))));
    }
}

void set_item_types(int count) {
    std::memset(ps2::g_ram + 0x00100DECu, 0, 0x200u);
    for (int i = 0; i < count && i < 128; ++i) ps2::st32(0x00100DECu + static_cast<std::uint32_t>(i) * 4u, 1u);
}

bool is_unlocked(const achievements::Manager &m, const char *id) {
    for (const auto &e : m.entries())
        if (e.id == id) return e.unlocked;
    return false;
}

void test_catalog_integrity() {
    achievements::Manager m;
    check(m.total_count() == 110, "catalog has exactly 110 achievements");
    check(m.tier_total(achievements::Tier::Mark) == 24, "Mark has 24 achievements");
    check(m.tier_total(achievements::Tier::Sigil) == 19, "Sigil has 19 achievements");
    check(m.tier_total(achievements::Tier::Crest) == 15, "Crest has 15 achievements");
    check(m.tier_total(achievements::Tier::Sovereign) == 11, "Sovereign has 11 achievements");
    check(m.tier_total(achievements::Tier::Covenant) == 1, "Covenant has 1 capstone");
    check(m.tier_total(achievements::Tier::Folly) == 15, "Follies has 15 achievements");
    check(m.tier_total(achievements::Tier::Curiosity) == 25, "Curiosities has 25 achievements");

    std::set<std::string> ids;
    int missable_cnt = 0;
    for (const auto &e : m.entries()) {
        check(!e.id.empty() && !e.title.empty() && !e.description.empty(), "entry fields non-empty");
        check(ids.insert(e.id).second, "achievement IDs are unique");
        if (e.tier == achievements::Tier::Curiosity) check(e.secret, "all Curiosities are secret");
        if (e.missable) ++missable_cnt;
    }
    check(missable_cnt == 3, "exactly 3 Curiosities tagged missable");

    // Verify all 32 legacy IDs remain present
    static const char *const kLegacy[32] = {
        "mark_first_blood", "mark_first_spark", "mark_pocket_change", "mark_seasoned_initiate", "mark_fresh_ink", "mark_getting_rhythm",
        "sigil_mercenary_vault", "sigil_veteran_front", "sigil_tightening_arc", "sigil_covenant_elements", "sigil_traveler_ledger",
        "crest_high_roller", "crest_apex_vessel", "crest_razors_edge", "crest_master_grimoire", "crest_relentless_chain",
        "sovereign_tycoon", "sovereign_transcendental", "sovereign_clockwork_demon", "sovereign_bare_knuckles", "sovereign_ring_of_iron",
        "covenant_sealed",
        "folly_wrestling_destiny", "folly_manly_men", "folly_decked_out", "folly_panic_button", "folly_overkill_theater",
        "curiosity_whispers_graveyard", "curiosity_forgotten_page", "curiosity_echoes_1915", "curiosity_unlikely_encounter", "curiosity_keeper_obscura"
    };
    for (const char *id : kLegacy) check(ids.count(id) == 1, "legacy ID preserved");
}

void test_ram_bugfixes_and_trackers() {
    achievements::Manager m;
    init_valid_ssh_save("AP_02_U", 120.0f);
    // Slot 4 with char_id=4 (Anastasia) must NOT unlock Joachim achievements!
    set_party_slot(0, 0, 10, "Yuri", 200, 200);
    set_party_slot(4, 4, 45, "Anastasia", 180, 180);
    set_party_slot(13, 11, 2, "Nicolai", 38, 34);  // Guest Nicolai in non-contiguous slot 13
    set_gold(60000, 60000);
    set_battle_ring(1, 0, 0, 0, 0);
    m.poll_ram(1);

    check(!is_unlocked(m, "folly_manly_men"), "Anastasia (char_id 4) does NOT trigger Joachim Lv 40");
    check(!is_unlocked(m, "folly_wrestling_destiny"), "Anastasia does NOT trigger Joachim battle win");
    check(is_unlocked(m, "sigil_anastasia_lens"), "Anastasia (char_id 4, Lv 45) unlocks Anastasia Lv 25");

    // Now test Soul Graveyard GY_00_D fix
    set_map("GY_00_D");
    m.poll_ram(1);
    check(is_unlocked(m, "curiosity_whispers_graveyard"), "GY_00_D unlocks Whispers in the Graveyard");

    // Test Joachim at char_id=6 + clutch win + spending spree + Nicolai missable + Perfect battle
    set_map("AP_02_U");
    set_party_slot(6, 6, 42, "Joachim", 300, 30);  // 10% HP (<=15% critical HP!)
    set_gold(8000, 60000);  // Spent 52,000 Gold in one frame!
    set_battle_ring(1, 0, 2, 2, 2);  // 2 consecutive Perfect hits
    m.poll_ram(1);
    set_battle_ring(1, 1, 2, 2, 2);  // Win the battle!
    m.poll_ram(1);

    check(is_unlocked(m, "folly_manly_men"), "Joachim (char_id 6, Lv 42) unlocks Manly Men");
    check(is_unlocked(m, "folly_wrestling_destiny"), "Joachim battle win unlocks Wrestling with Destiny");
    check(is_unlocked(m, "folly_hairs_breadth"), "Winning at 10% HP unlocks By a Hair's Breadth");
    check(is_unlocked(m, "folly_big_spender"), "Spending 52k in one shot unlocks Retail Therapy");
    check(is_unlocked(m, "mark_first_purchase"), "Spending 52k unlocks Patron of the Magimels");
    check(is_unlocked(m, "mark_double_perfect"), "2 consecutive Perfects unlocks Twin Bullseyes");
    check(is_unlocked(m, "mark_clean_sweep"), "0-miss win unlocks Clean Sweep");

    // 2 more battles with Nicolai -> 3 total -> Missable "Before the Betrayal"
    for (int b = 2; b <= 3; ++b) {
        set_battle_ring(static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b - 1), 2, 2, 2);
        m.poll_ram(1);
        set_battle_ring(static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b), 2, 2, 2);
        m.poll_ram(1);
    }
    check(is_unlocked(m, "curiosity_missable_nicolai"), "3 wins with Nicolai unlocks missable Before the Betrayal");

    // 3 consecutive Ring misses -> Butterfingers + Panic Button on win
    set_battle_ring(4, 3, 2, 2, 2);
    m.poll_ram(1);
    set_battle_ring(4, 3, 5, 2, 2);  // 3 misses in a row
    m.poll_ram(1);
    set_battle_ring(4, 4, 5, 2, 2);  // still win!
    m.poll_ram(1);
    check(is_unlocked(m, "folly_butterfingers"), "3 consecutive misses unlocks Butterfingers");
    check(is_unlocked(m, "folly_panic_button"), "Winning after a miss unlocks Panic Button");

    // Battleship Mikasa missable
    set_map("MI_00_U");
    set_battle_ring(5, 4, 5, 2, 2);
    m.poll_ram(1);
    set_battle_ring(5, 5, 5, 2, 2);
    m.poll_ram(1);
    check(is_unlocked(m, "curiosity_missable_mikasa"), "Winning a battle on MI_00_U unlocks Boarding the Mikasa");

    // Split-vblank Judgment Ring settle test: attempts increments on vblank N, successes on vblank N+1 (no false miss!)
    {
        achievements::Manager m_split;
        init_valid_ssh_save("AP_02_U", 120.0f);
        set_party_slot(0, 0, 10, "Yuri", 200, 200);
        set_gold(50000, 50000);
        set_battle_ring(1, 0, 0, 0, 0);
        m_split.poll_ram(1);
        for (std::uint32_t k = 1; k <= 3; ++k) {
            // Vblank N: attempts increments alone
            set_battle_ring(1, 0, k, k - 1, 0);
            m_split.poll_ram(1);
            // Vblank N+1: successes catches up
            set_battle_ring(1, 0, k, k, 0);
            m_split.poll_ram(1);
        }
        check(is_unlocked(m_split, "mark_getting_rhythm"), "split-vblank ring updates preserve 3-hit streak without false miss");

        // Save-reload gold drop guard: playtime jumps backward -> must NOT count as gold spent!
        init_valid_ssh_save("AP_02_U", 60.0f);  // earlier save
        set_party_slot(0, 0, 10, "Yuri", 200, 200);
        set_gold(1000, 1000);  // 49,000 lower than previous save
        set_battle_ring(1, 0, 3, 3, 0);
        m_split.poll_ram(1);
        check(!is_unlocked(m_split, "mark_first_purchase"), "save-state reload with lower gold does NOT falsely trigger spending");
    }
}

void test_burst_coalescing_and_json_roundtrip(const std::string &dump_dir) {
    // 1. Test 1-3 unlocks produce individual toasts
    {
        achievements::Manager m;
        init_valid_ssh_save("AP_02_U", 120.0f);
        set_party_slot(0, 0, 1, "Yuri");
        set_gold(3000, 3000);  // Unlocks mark_first_coin (1 unlock)
        m.poll_ram(1);
        check(m.pending_toast_count() == 1u, "1 unlock queues 1 individual toast");
        achievements::ToastPicture pic;
        auto r1 = m.tick_toast(1.0, 1280, 720, 0, pic);
        check(r1.chime && r1.changed && pic.active && pic.width > 100 && pic.height > 40, "single toast renders valid high-res RGBA");
        auto r2 = m.tick_toast(1.7, 1280, 720, 0, pic);
        check(r2.take_screenshot && r2.screenshot_title == "Soldier's Stipend", "single toast triggers screenshot with exact title");
        if (!dump_dir.empty()) {
            screenshot::write_png(std::filesystem::path(dump_dir) / "toast_single.png", pic.rgba.data(), pic.width, pic.height);
        }
    }

    // 2. Test 4+ simultaneous unlocks coalesce into 1 burst summary toast
    {
        achievements::Manager m;
        init_valid_ssh_save("YO_01_U", 150000.0f);
        for (int i = 0; i < 8; ++i) set_party_slot(i, static_cast<std::uint8_t>(i), 99, "Hero");
        set_gold(10000000, 10000000);
        set_battle_ring(520, 500, 1100, 1060, 1000);
        set_treasures(80);
        set_item_types(65);
        m.poll_ram(2);
        check(m.unlocked_count() >= 40, "late-game save unlocks 40+ achievements simultaneously");
        check(m.pending_toast_count() == 1u, "4+ same-frame unlocks coalesce into exactly 1 summary toast");

        achievements::ToastPicture pic;
        auto r1 = m.tick_toast(1.0, 1280, 720, 0, pic);
        check(r1.chime && r1.changed && pic.active && pic.width > 150, "burst summary toast renders cleanly");
        auto r2 = m.tick_toast(1.7, 1280, 720, 0, pic);
        check(r2.take_screenshot && r2.screenshot_title.find("Unlocked ") == 0, "burst toast triggers 1 summary screenshot");
        if (!dump_dir.empty()) {
            screenshot::write_png(std::filesystem::path(dump_dir) / "toast_burst.png", pic.rgba.data(), pic.width, pic.height);
        }

        // Legacy + Extended JSON round-trip test
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "sh2_ach_test_roundtrip.json";
        m.load(tmp.string());
        m.save();
        m.flush();

        achievements::Manager m2;
        m2.load(tmp.string());
        check(m2.unlocked_count() == m.unlocked_count(), "JSON save/load preserves all unlocked achievements");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
}

void test_trophy_gallery_gui_and_hitboxes(const std::string &dump_dir) {
    achievements::Manager m;
    init_valid_ssh_save("PA_01_U", 4200.0f);
    set_party_slot(0, 0, 26, "Yuri");
    set_party_slot(1, 1, 22, "Blanca");
    set_party_slot(2, 2, 20, "Gepetto");
    set_party_slot(5, 5, 25, "Karin");
    set_party_slot(6, 6, 24, "Joachim");
    set_gold(125000, 180000);
    set_battle_ring(48, 44, 140, 132, 32);
    set_treasures(22);
    set_item_types(24);
    m.poll_ram(1);

    quickmenu::Menu menu;
    menu.open();
    // Navigate main hub to Achievements (index 5) and open it
    for (int i = 0; i < 5; ++i) menu.move(1);
    menu.choose();
    check(menu.on_achievements_page(), "menu opened Achievements Trophy Gallery page");

    quickmenu::Values v;
    v.ach_entries = &m.entries();
    v.ach_unlocked = m.unlocked_count();
    v.ach_total = m.total_count();
    v.toast_theme = 0;

    quickmenu::Picture pic;
    menu.draw(1280, 720, v, pic);
    check(pic.width >= 1000 && pic.height >= 600, "Trophy Gallery renders high-res canvas at 1280x720");
    check(!pic.rgba.empty(), "Trophy Gallery RGBA buffer populated");

    int non_zero = 0;
    for (std::uint32_t px : pic.rgba) if (px != 0) ++non_zero;
    check(non_zero > pic.width * pic.height * 8 / 10, "Trophy Gallery pixels drawn");

    if (!dump_dir.empty()) {
        std::filesystem::create_directories(dump_dir);
        screenshot::write_png(std::filesystem::path(dump_dir) / "trophy_gallery_all.png", pic.rgba.data(),
                              static_cast<std::uint32_t>(pic.width), static_cast<std::uint32_t>(pic.height));
    }

    // Test PageDown fast scrolling
    menu.page_move(1);
    check(menu.ach_selected_index() >= 4, "PageDown jumps down by visible card page");
    menu.page_move(-1);
    check(menu.ach_selected_index() == 0, "PageUp jumps back up cleanly");

    // Test Tier cycling & Reveal Secrets on Curiosities (shows Missable pills)
    menu.set_ach_tier(6);  // Curiosities
    menu.toggle_reveal_secrets();
    menu.draw(1280, 720, v, pic);  // Rebuild filter for Curiosities before paging
    for (int k = 0; k < 6; ++k) menu.page_move(1);
    menu.draw(1280, 720, v, pic);
    check(menu.ach_tier_filter() == 6 && menu.ach_reveal_secrets(), "Curiosities tier + reveal secrets active");
    if (!dump_dir.empty()) {
        screenshot::write_png(std::filesystem::path(dump_dir) / "trophy_gallery_curiosities.png", pic.rgba.data(),
                              static_cast<std::uint32_t>(pic.width), static_cast<std::uint32_t>(pic.height));
    }

    // Verify mouse click hit-testing on the "All" tier tab (top-left of tier strip)
    quickmenu::Command click_cmd = quickmenu::Command::none;
    const bool hit_all_tab = menu.click_at(pic.x + 60, pic.y + 100, click_cmd);
    check(hit_all_tab && menu.ach_tier_filter() == -1, "clicking All tier tab switches filter back to All");

    // Verify Trophy Gallery tier tab cycle order: All -> Mark -> Sigil -> Crest -> Sovereign -> Follies -> Curiosities -> Covenant
    {
        static constexpr int kExpectedOrder[8] = {-1, 0, 1, 2, 3, 5, 6, 4};
        menu.set_ach_tier(-1);
        for (int step = 0; step < 8; ++step) {
            check(menu.ach_tier_filter() == kExpectedOrder[step], "Trophy Gallery tab cycle follows All->Mark->Sigil->Crest->Sovereign->Follies->Curiosities->Covenant");
            menu.cycle_ach_tier(+1);
        }
        check(menu.ach_tier_filter() == -1, "Trophy Gallery tab cycle wraps cleanly back to All");
    }

    // Also render at 1920x1080 to ensure high-DPI scaling works cleanly
    menu.set_ach_tier(-1);
    menu.draw(1920, 1080, v, pic);
    check(pic.width >= 1400 && pic.height >= 850, "Trophy Gallery scales cleanly to 1920x1080");
}

void test_gothic_quickmenu_pages_and_hitboxes(const std::string &dump_dir) {
    quickmenu::Values v;
    v.scale_now = 4;
    v.scale_next = 4;
    v.scale_max = 12;
    v.vram_bytes_next = 240ull * 1024 * 1024;
    v.fps_cap_label = "60 FPS";
    v.fps_speed_sync = true;
    v.present_mode = "Mailbox";
    v.look = "Sharp Edges";
    v.sharp_now = 2;
    v.sharp_next = 2;
    v.aspect_mode = 1;
    v.brightness = 5;
    v.contrast = 105;
    v.saturation = 110;
    v.gamma = 100;
    v.scanlines = 0;
    v.deinterlace = true;
    v.ssao = 35;
    v.bloom = 25;
    v.ssgi = 0;
    v.filmic = true;
    v.vignette = 20;
    v.toast_theme = 0;
    v.gpu_adapter = "AMD Radeon RX";
    v.gpu_api = "Direct3D 12";
    v.hd_textures_on = true;
    v.hd_textures_detail = "(1 pack, 5655 PNGs)";
    v.hd_fmv_on = true;
    v.hd_fmv_detail = "(36 .mp4)";
    v.active_slot = 1;
    v.active_disc = 0;
    v.disc_count = 2;
    v.ach_unlocked = 37;
    v.ach_total = 110;
    v.master_volume = 90;
    v.chime_volume = 85;
    v.bgm_volume = 100;
    v.sfx_volume = 80;
    v.audio_device = "System Default";
    v.audio_stretch = true;

    // Populate two realistic save slots + synthetic gothic preview thumbnail
    v.slots[0].exists = true;
    v.slots[0].bytes = 12800000;
    v.slots[0].area = "Domremy Village";
    v.slots[0].max_level = 9;
    v.slots[0].disc = 1;
    v.slots[0].playtime_sec = 4120.0f;
    v.slots[0].timestamp = "2026-11-20 09:42";
    v.slots[1].exists = true;
    v.slots[1].bytes = 14200000;
    v.slots[1].area = "Paris - Champ de Mars";
    v.slots[1].max_level = 24;
    v.slots[1].disc = 1;
    v.slots[1].playtime_sec = 32415.0f;
    v.slots[1].timestamp = "2026-11-20 10:05";
    std::vector<std::uint32_t> syn_thumb(160 * 112);
    for (int y = 0; y < 112; ++y) {
        for (int x = 0; x < 160; ++x) {
            const int r = 28 + (x * 110) / 160;
            const int g = 22 + (y * 75) / 112;
            const int b = 52 + ((160 - x) * 90) / 160;
            syn_thumb[static_cast<std::size_t>(y) * 160 + x] =
                static_cast<std::uint32_t>(r | (g << 8) | (b << 16) | (255u << 24));
        }
    }
    v.slot_thumb_rgba = &syn_thumb;
    v.slot_thumb_w = 160;
    v.slot_thumb_h = 112;

    v.key_rows = {
        {"Cross (Confirm)", "Enter, Z, J"},
        {"Circle (Cancel)", "Backspace, X, K"},
        {"Square (Menu)", "C, U"},
        {"Triangle (Map)", "V, I"},
        {"Left Stick Up", "W"},
        {"Left Stick Down", "S"},
        {"Left Stick Left", "A"},
        {"Left Stick Right", "D"},
        {"L1 / R1", "Q / E"},
        {"Start / Select", "Space / Tab"},
        {"L2 / R2", "1 / 3"},
    };
    v.key_note = "Changes save immediately to keys.ini";

    const auto dump_pic = [&](const char *fname, const quickmenu::Picture &p) {
        if (dump_dir.empty()) return;
        std::filesystem::create_directories(dump_dir);
        screenshot::write_png(std::filesystem::path(dump_dir) / fname, p.rgba.data(),
                              static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height));
    };

    quickmenu::Menu menu;
    quickmenu::Picture pic;
    quickmenu::Command cmd = quickmenu::Command::none;

    // 1. Main Pause Menu
    menu.open();
    menu.draw(1280, 720, v, pic);
    check(pic.width >= 440 && pic.width <= 640 && pic.height >= 360, "Gothic Main Menu sized cleanly as sleek centered card");
    dump_pic("f1_main.png", pic);

    // 2. Save States Page (with rolling Auto row populated)
    v.autosave_slot.slot = 10;
    v.autosave_slot.exists = true;
    v.autosave_slot.area = "Domremy";
    v.autosave_slot.max_level = 12;
    v.autosave_slot.disc = 1;
    v.autosave_slot.bytes = 12450000;
    v.autosave_slot.timestamp = "2026-11-20 15:45:00";
    menu.open_save_picker(quickmenu::SavePickerMode::browse, 1);
    menu.draw(1280, 720, v, pic);
    check(menu.on_save_states_page() && menu.selected_slot() == 1, "Save States page opens on Slot 1");
    dump_pic("f1_save_states.png", pic);

    // 3. Settings -> Graphics (scroll down slightly so sliders & toggles are visible!)
    menu.open();
    for (int i = 0; i < 6; ++i) menu.move(1);  // Settings
    menu.choose();
    check(menu.on_settings_page(), "Opened Settings page");
    for (int i = 0; i < 15; ++i) menu.move(1);  // Highlight SSAO (shows sliders + toggles + scrollbar!)
    menu.draw(1280, 720, v, pic);
    check(pic.width >= 800 && pic.height >= 520, "Gothic Settings page renders high-res canvas");
    dump_pic("f1_settings_gfx.png", pic);

    // Click Audio tab via mouse hitbox (center of middle tab)
    const bool hit_audio = menu.click_at(pic.x + pic.width / 2, pic.y + 72, cmd);
    check(hit_audio && menu.settings_tab() == quickmenu::SettingsTab::audio, "Clicking Audio pill tab switches to Audio");
    menu.draw(1280, 720, v, pic);
    dump_pic("f1_settings_audio.png", pic);

    // Switch to Controls -> Change Keys
    menu.set_tab(quickmenu::SettingsTab::controls);
    menu.draw(1280, 720, v, pic);
    for (int i = 0; i < 6; ++i) menu.move(1);  // change_keys (after fast_disc, switch_disc, slot_picker, autosave, turbo_speed, keys)
    menu.choose();
    check(menu.on_keys_page(), "Opened Change Keys gothic page");
    menu.draw(1280, 720, v, pic);
    dump_pic("f1_keys.png", pic);

    // 4. HUD Notice Pill & Fullscreen Corner FPS/Turbo HUD
    quickmenu::Menu::draw_note(1280, 720, "Saved to Slot 1  —  Paris - Champ de Mars", pic);
    check(pic.width >= 200 && pic.height >= 30 && !pic.rgba.empty(), "Gothic HUD notice pill renders cleanly");
    dump_pic("f1_notice.png", pic);

    quickmenu::Menu::draw_perf_hud(1280, 720, "TURBO 3x  |  179 FPS  |  5.6 ms  |  4x VK", true, pic);
    check(pic.width >= 160 && pic.height >= 24 && !pic.rgba.empty(), "Fullscreen corner FPS/Turbo HUD pill renders cleanly");
}

void test_sh1_catalog_and_ram_tracking() {
    achievements::Manager m(achievements::CatalogGame::ShadowHearts1);
    check(m.total_count() == 75, "SH1 catalog has exactly 75 achievements");
    check(m.tier_total(achievements::Tier::Mark) == 16, "SH1 Mark has 16 achievements");
    check(m.tier_total(achievements::Tier::Sigil) == 14, "SH1 Sigil has 14 achievements");
    check(m.tier_total(achievements::Tier::Crest) == 11, "SH1 Crest has 11 achievements");
    check(m.tier_total(achievements::Tier::Sovereign) == 8, "SH1 Sovereign has 8 achievements");
    check(m.tier_total(achievements::Tier::Covenant) == 1, "SH1 Capstone has 1 achievement");
    check(m.tier_total(achievements::Tier::Folly) == 10, "SH1 Follies has 10 achievements");
    check(m.tier_total(achievements::Tier::Curiosity) == 15, "SH1 Curiosities has 15 achievements");

    std::set<std::string> ids;
    for (const auto &e : m.entries()) {
        check(ids.insert(e.id).second, "SH1 achievement IDs are unique");
        if (e.tier == achievements::Tier::Curiosity) check(e.secret, "all SH1 Curiosities are secret");
    }

    // Seed SH1 read-only ELF WorldData signature & live save RAM at 0x00100000
    std::memset(ps2::g_ram, 0, ps2::kRamSize);
    auto write_cstr = [](std::uint32_t addr, const char *s) {
        for (std::uint32_t i = 0; ; ++i) {
            ps2::st8(addr + i, static_cast<std::uint8_t>(s[i]));
            if (s[i] == '\0') break;
        }
    };
    write_cstr(0x004CAE40u, "Changchun");
    write_cstr(0x004CAE50u, "Trans-Siberian Express");

    // Seed active gameplay: Yuri Lv 15, Alice Lv 15, Zhuzhen Lv 12; Area 1 (Trans-Siberian Express); Cash 30,000
    ps2::st32(0x00100008u, 30000u);
    ps2::st32(0x0010000Cu, 12000u);  // 12s playtime
    ps2::st16(0x00100028u, 1u);      // Trans-Siberian Express
    ps2::st8(0x00100280u + 0u * 0x50u, 0x40u);
    ps2::st8(0x00100280u + 0u * 0x50u + 3u, 15u);
    ps2::st8(0x00100280u + 1u * 0x50u, 0x40u);
    ps2::st8(0x00100280u + 1u * 0x50u + 3u, 15u);
    ps2::st8(0x00100280u + 2u * 0x50u, 0x40u);
    ps2::st8(0x00100280u + 2u * 0x50u + 3u, 12u);
    ps2::st32(0x001014A4u, 10u);  // battles
    ps2::st32(0x001014A8u, 0u);   // escapes
    ps2::st32(0x001014ACu, 25u);  // ring attempts
    ps2::st32(0x001014B0u, 25u);  // ring successes
    ps2::st32(0x001014B4u, 5u);   // ring perfects
    ps2::st32(0x00101528u, 1u);   // shop discount
    ps2::st32(0x00101530u, 1u);   // shop price raise
    m.poll_ram(1);

    check(is_unlocked(m, "sh1_mark_first_strike"), "SH1 unlocks first Ring strike");
    check(is_unlocked(m, "sh1_mark_battles_10"), "SH1 unlocks 10 battles");
    check(is_unlocked(m, "sh1_mark_lv15"), "SH1 unlocks Lv 15");
    check(is_unlocked(m, "sh1_mark_shop_discount"), "SH1 unlocks Shop Ring discount");
    check(is_unlocked(m, "sh1_sigil_alice"), "SH1 unlocks Alice Lv 15");
    check(is_unlocked(m, "sh1_sigil_zhuzhen"), "SH1 unlocks Zhuzhen recruitment");
    check(is_unlocked(m, "sh1_folly_haggler"), "SH1 unlocks Shop price raise folly");
    check(is_unlocked(m, "sh1_curiosity_train"), "SH1 unlocks Trans-Siberian Express curiosity");
    check(!is_unlocked(m, "sh1_curiosity_london"), "SH1 keeps London curiosity locked before visiting");

    // Spend 26,000 Cash in one go + land 3 consecutive Ring hits + finish a clean battle + visit Dollhouse (Area 30)
    ps2::st32(0x00100008u, 4000u);
    ps2::st16(0x00100028u, 30u);  // Dollhouse
    for (std::uint32_t k = 1u; k <= 3u; ++k) {
        ps2::st32(0x0010000Cu, 12000u + k * 500u);
        ps2::st32(0x001014ACu, 25u + k);
        ps2::st32(0x001014B0u, 25u + k);
        ps2::st32(0x001014B4u, 5u + k);
        m.poll_ram(1);
    }
    ps2::st32(0x0010000Cu, 14500u);
    ps2::st32(0x001014A4u, 11u);  // battle finished cleanly
    m.poll_ram(1);

    check(is_unlocked(m, "sh1_mark_three_streak"), "SH1 tracks 3-hit Ring streak");
    check(is_unlocked(m, "sh1_sigil_triple_perfect"), "SH1 tracks 3-hit Perfect streak");
    check(is_unlocked(m, "sh1_mark_clean_battle"), "SH1 tracks clean sweep battle");
    check(is_unlocked(m, "sh1_folly_big_spend"), "SH1 tracks 25,000+ single Cash spend");
    check(is_unlocked(m, "sh1_curiosity_secret_dungeons"), "SH1 unlocks secret dungeon curiosity at Dollhouse");
}

}  // namespace

int main(int argc, char **argv) {
    std::string dump_dir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-dir") == 0) dump_dir = argv[i + 1];
    }
    test_catalog_integrity();
    test_ram_bugfixes_and_trackers();
    test_burst_coalescing_and_json_roundtrip(dump_dir);
    test_trophy_gallery_gui_and_hitboxes(dump_dir);
    test_gothic_quickmenu_pages_and_hitboxes(dump_dir);
    test_sh1_catalog_and_ram_tracking();

    if (g_fails == 0) {
        std::printf("ALL ACHIEVEMENTS & TROPHY GALLERY TESTS PASSED!\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed.\n", g_fails);
    return 1;
}
