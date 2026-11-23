#pragma once

// Shared Gothic overlays for the Nocturne Mod Engine:
// 1. Custom Pinned HUD Widgets (`hud_set(id, label, value, color_hex)`) for all players.
// 2. Developer-Only `F10` Live Modder Inspector & Lua Log Console (strictly gated behind --mod-dev / mod_dev_mode).

#include "mod_engine.hpp"
#include "ui_draw.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace mod_overlays {

inline std::uint32_t rgb_hex_to_rgba(std::uint32_t rgb, int alpha = 255) {
    const int r = static_cast<int>((rgb >> 16) & 0xFFu);
    const int g = static_cast<int>((rgb >> 8) & 0xFFu);
    const int b = static_cast<int>(rgb & 0xFFu);
    return ui_draw::rgba(r, g, b, alpha);
}

inline void draw_hud_widgets(ui_draw::Surface &s, const std::vector<mod_engine::HudWidget> &widgets) {
    if (widgets.empty() || s.width < 320 || s.height < 200) return;
    const float sc = std::clamp(static_cast<float>(s.height) / 720.0f, 0.72f, 1.35f);
    const int pad_x = static_cast<int>(12 * sc);
    const int pill_h = static_cast<int>(26 * sc);
    const int gap = static_cast<int>(6 * sc);
    const int font_sz = std::max(11, static_cast<int>(13 * sc));
    const int margin = static_cast<int>(14 * sc);

    int y = margin + static_cast<int>(44 * sc);
    for (const auto &w : widgets) {
        const std::string lbl = w.label.empty() ? w.id : w.label;
        const std::string txt = w.value.empty() ? lbl : (lbl + "  " + w.value);
        const auto m_lbl = ui_draw::measure_text(ui_draw::FontStyle::Sans, font_sz, false, lbl + "  ");
        const auto m_all = ui_draw::measure_text(ui_draw::FontStyle::Sans, font_sz, true, txt);
        const int pill_w = std::min(s.width - margin * 2, m_all.width + pad_x * 2 + 8);
        const int x = s.width - margin - pill_w;

        const std::uint32_t accent = rgb_hex_to_rgba(w.color_rgb, 255);
        s.fill_rounded_rect(x, y, pill_w, pill_h, 6.0f * sc, ui_draw::rgba(14, 12, 20, 212));
        s.stroke_rounded_rect(x, y, pill_w, pill_h, 6.0f * sc, 1.2f, ui_draw::with_alpha(accent, 195));
        s.fill_rounded_rect(x + 2, y + 3, std::max(3, static_cast<int>(3 * sc)), pill_h - 6, 2.0f, accent);

        const int ty = y + (pill_h - font_sz) / 2 - 1;
        ui_draw::draw_text(s, x + pad_x, ty, ui_draw::FontStyle::Sans, font_sz, false,
                           lbl, ui_draw::rgba(205, 198, 218, 240));
        if (!w.value.empty()) {
            ui_draw::draw_text(s, x + pad_x + m_lbl.width, ty, ui_draw::FontStyle::Sans, font_sz, true,
                               w.value, accent);
        }
        y += pill_h + gap;
        if (y + pill_h > s.height - margin) break;
    }
}

inline void draw_dev_inspector(ui_draw::Surface &s, const mod_engine::Engine &eng,
                               const std::uint8_t *ram, std::size_t ram_size) {
    if (!eng.dev_mode() || s.width < 420 || s.height < 280) return;
    const auto snap = eng.capture_inspector_snapshot(ram, ram_size);

    const float sc = std::clamp(static_cast<float>(s.height) / 720.0f, 0.72f, 1.25f);
    const int margin = static_cast<int>(14 * sc);
    const int panel_w = std::min(s.width - margin * 2, static_cast<int>(490 * sc));
    const int panel_h = std::min(s.height - margin * 2, static_cast<int>(470 * sc));
    const int px = margin;
    const int py = margin;

    s.fill_rounded_rect(px, py, panel_w, panel_h, 9.0f * sc, ui_draw::rgba(11, 10, 17, 232));
    s.stroke_rounded_rect(px, py, panel_w, panel_h, 9.0f * sc, 1.4f, ui_draw::rgba(218, 178, 96, 220));

    const int f_title = std::max(12, static_cast<int>(15 * sc));
    const int f_body = std::max(10, static_cast<int>(12 * sc));
    const int line_h = std::max(15, static_cast<int>(17 * sc));
    int y = py + static_cast<int>(10 * sc);
    const int x = px + static_cast<int>(12 * sc);

    ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Serif, f_title, true,
                       "NOCTURNE MOD DEV INSPECTOR  [F9 Reload | F10 Hide]",
                       ui_draw::rgba(235, 198, 118, 255));
    y += line_h + static_cast<int>(4 * sc);

    char buf[256];
    const char *gname = snap.detected_game == 1 ? "SH1"
                      : snap.detected_game == 2 ? "SH2: Covenant"
                      : snap.detected_game == 3 ? "SH3: FTNW"
                      : snap.detected_game == 4 ? "Koudelka (PS1)" : "Title / Menu";
    std::snprintf(buf, sizeof(buf), "Game: %s (%s)  |  Mods: %zu  |  Patches: %zu  |  Hooks: %zu  |  DLLs: %zu",
                  gname, snap.in_game ? "IN-GAME" : "NOT READY",
                  eng.active_mod_count(), eng.patch_count(), eng.script_op_count(), eng.loaded_plugin_count());
    ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false, buf, ui_draw::rgba(214, 210, 224, 245));
    y += line_h;

    std::snprintf(buf, sizeof(buf), "Cash: %u  |  Wins: %u  |  Rings: %u/%u (Perf %u)  |  Streak: H%d / P%d",
                  snap.cash, snap.battles_won, snap.ring_successes, snap.ring_attempts,
                  snap.ring_perfects, snap.hit_streak, snap.perf_streak);
    ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false, buf, ui_draw::rgba(175, 222, 185, 245));
    y += line_h + static_cast<int>(4 * sc);

    // Party Hero Slots
    ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, true,
                       "PARTY RAM SNAPSHOT", ui_draw::rgba(218, 178, 96, 240));
    y += line_h;
    int shown_heroes = 0;
    for (const auto &h : snap.heroes) {
        if (!h.joined && shown_heroes >= 4) continue;
        std::snprintf(buf, sizeof(buf), "  Slot %d [%c]  Lv %-2d  HP %4d/%-4d  MP %4d/%-4d  EXP %u",
                      h.slot, h.joined ? 'J' : '-', h.lv, h.hp, h.max_hp, h.mp, h.max_mp, h.exp);
        ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false, buf,
                           h.joined ? ui_draw::rgba(225, 222, 236, 240) : ui_draw::rgba(130, 125, 145, 200));
        y += line_h;
        if (++shown_heroes >= 6) break;
    }
    if (snap.heroes.empty()) {
        ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false,
                           "  (Load a save or enter gameplay to inspect live hero RAM)",
                           ui_draw::rgba(145, 140, 160, 210));
        y += line_h;
    }

    // Conflicts summary
    y += static_cast<int>(4 * sc);
    const auto &confs = eng.conflicts();
    if (!confs.empty()) {
        std::snprintf(buf, sizeof(buf), "CONFLICTS (%zu) — top-priority mod wins:", confs.size());
        ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, true, buf, ui_draw::rgba(240, 130, 110, 250));
        y += line_h;
        for (std::size_t i = 0; i < std::min<std::size_t>(confs.size(), 3); ++i) {
            const auto &c = confs[i];
            std::snprintf(buf, sizeof(buf), "  [%s] %s: %s > %s",
                          c.layer.c_str(), c.resource.c_str(), c.winner_mod.c_str(), c.shadowed_mod.c_str());
            ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false,
                               ui_draw::fit_text_px(ui_draw::FontStyle::Sans, f_body, false, buf, panel_w - 24),
                               ui_draw::rgba(245, 180, 160, 235));
            y += line_h;
        }
    }

    // Live Lua Log Console
    y += static_cast<int>(4 * sc);
    ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, true,
                       "LIVE LUA LOG / DEV CONSOLE", ui_draw::rgba(218, 178, 96, 240));
    y += line_h;
    const auto &logs = eng.recent_logs();
    const int avail_lines = std::max(1, (py + panel_h - y - 10) / line_h);
    const std::size_t start_idx = (logs.size() > static_cast<std::size_t>(avail_lines))
                                ? (logs.size() - static_cast<std::size_t>(avail_lines))
                                : 0;
    for (std::size_t i = start_idx; i < logs.size(); ++i) {
        ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false,
                           ui_draw::fit_text_px(ui_draw::FontStyle::Sans, f_body, false, logs[i], panel_w - 24),
                           ui_draw::rgba(195, 212, 232, 235));
        y += line_h;
    }
    if (logs.empty()) {
        ui_draw::draw_text(s, x, y, ui_draw::FontStyle::Sans, f_body, false,
                           "  (Call log(\"hello\") in Lua or save a .lua/.pnach file to hot-reload)",
                           ui_draw::rgba(145, 140, 160, 210));
    }
}

}  // namespace mod_overlays
