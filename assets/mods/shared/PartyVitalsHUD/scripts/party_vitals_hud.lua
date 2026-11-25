-- ============================================================================
-- Party Vitals HUD v1.0.0 — Pure Client-Side Gothic 2D Canvas Overlay
-- Works across Koudelka, SH1, SH2: Covenant & SH3: FTNW (100% Co-op Safe)
-- ============================================================================

local hud_visible = true

local function hp_color(ratio)
    if ratio <= 0.25 then return "#E84855" end
    if ratio <= 0.50 then return "#E8A848" end
    return "#64C884"
end

function on_boot()
    hud_visible = true
end

function on_hotkey(key)
    local tk = get_setting_str("toggle_key", "F7")
    if key == tk then
        hud_visible = not hud_visible
        toast("Party Vitals HUD: " .. (hud_visible and "ON" or "OFF"))
    end
end

function on_frame()
    if not hud_visible or not is_in_game() then return end

    local count = floor(get_hero_count())
    local heroes = {}
    for slot = 0, count - 1 do
        if get_hero_joined(slot) then
            local max_hp = get_hero_max_hp(slot)
            if max_hp > 0 then
                heroes[#heroes + 1] = {
                    name = get_hero_name(slot),
                    lv = floor(get_hero_level(slot)),
                    hp = floor(get_hero_hp(slot)),
                    mhp = floor(max_hp),
                    mp = floor(get_hero_mp(slot)),
                    mmp = floor(get_hero_max_mp(slot)),
                }
            end
        end
        if #heroes >= 4 then break end
    end
    if #heroes == 0 then return end

    local compact = get_setting("compact", 0) ~= 0
    local opacity = floor(get_setting("opacity", 195))
    local pos = floor(get_setting("position", 0) + 0.5)

    local row_h = compact and 26 or 42
    local panel_w = 228
    local panel_h = 18 + (#heroes * row_h)
    local px = 14
    local py = 720 - panel_h - 16
    if pos == 1 then
        px = 14
        py = 14
    elseif pos == 2 then
        px = 960 - panel_w - 14
        py = 720 - panel_h - 16
    end

    draw_rect(px, py, panel_w, panel_h, "#110E18", opacity)
    local y = py + 8
    for _, h in ipairs(heroes) do
        local label = string.format("%s  Lv%d", h.name, h.lv)
        draw_text(px + 10, y, label, 11, "#E6C88A")
        local hratio = clamp(h.hp / max(1, h.mhp), 0.0, 1.0)
        draw_bar(px + 98, y, 120, 13, hratio, hp_color(hratio), "#1C1626",
                 string.format("%d/%d", h.hp, h.mhp))
        if not compact and h.mmp > 0 then
            local mratio = clamp(h.mp / max(1, h.mmp), 0.0, 1.0)
            draw_bar(px + 98, y + 16, 120, 11, mratio, "#5A9CE6", "#161424",
                     string.format("MP %d/%d", h.mp, h.mmp))
        end
        y = y + row_h
    end
end
