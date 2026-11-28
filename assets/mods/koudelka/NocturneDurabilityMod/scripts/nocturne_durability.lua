-- ============================================================================
-- Nocturne Durability Mod v1.0.0 — Standalone Koudelka (PS1) Weapon Durability
-- 100% Co-op Safe (Pure Client-Side Read-Only HUD & Callouts)
-- Features:
--   - Live durability readout for whichever weapon is selected in Equip,
--     Items -> Weapon, and Battle -> Action -> Weapon menus.
--   - Optional equipped-weapon durability bars for Koudelka, Edward & James
--     with configurable low-durability warning toasts.
-- ============================================================================

local HERO_META = {
    [0] = { name = "Koudelka", color = "#E66898" },
    [1] = { name = "Edward",   color = "#68B8E6" },
    [2] = { name = "James",    color = "#E6C870" },
}

local hud_visible = true
local warned_slot = { [0] = false, [1] = false, [2] = false }

local function durability_color(ratio)
    if ratio <= 0.20 then return "#FF4455" end
    if ratio <= 0.45 then return "#F0A840" end
    return "#70D898"
end

function on_boot()
    hud_visible = true
    warned_slot = { [0] = false, [1] = false, [2] = false }
end

function on_game_ready()
    log("Nocturne Durability Mod v1.0.0 ready (Co-op Safe)")
end

function on_battle_start()
    warned_slot = { [0] = false, [1] = false, [2] = false }
end

function on_hotkey(key)
    local tk = get_setting_str("toggle_key", "8")
    if key == tk then
        hud_visible = not hud_visible
        toast("Nocturne Durability Mod: " .. (hud_visible and "ON" or "OFF"))
    end
end

function on_frame()
    if not hud_visible or not is_in_game() then return end

    local drew_menu_card = false
    if get_setting("show_menu_durability", 1) ~= 0 then
        local cur = floor(get_selected_weapon_durability())
        local maxd = floor(get_selected_weapon_max_durability())
        if cur >= 0 and maxd > 0 then
            local wname = get_selected_weapon_name()
            if not wname or wname == "" then wname = "Selected Weapon" end
            local ratio = clamp(cur / max(1, maxd), 0.0, 1.0)
            local pct = floor(ratio * 100.0 + 0.5)
            local col = durability_color(ratio)
            local ovl = floor(read_u32(0x000B6DF0))
            local panel_w = 248
            local panel_h = 64
            local px = 960 - panel_w - 18
            local py = (ovl == 5) and 88 or (720 - panel_h - 18)

            draw_rect(px, py, panel_w, panel_h, "#110E18", 232)
            draw_text(px + 10, py + 7, "WEAPON DURABILITY", 10, "#E6C88A")
            draw_text(px + 10, py + 23, wname, 12, "#F2ECE0")
            draw_text(px + panel_w - 42, py + 23, string.format("%d%%", pct), 11, col)
            draw_bar(px + 10, py + 42, panel_w - 20, 15, ratio, col, "#1C1626",
                     string.format("%d / %d", cur, maxd))
            drew_menu_card = true
        end
    end

    if get_setting("show_equipped_hud", 1) == 0 then return end
    -- Avoid overlapping top-right when Battle Weapon menu card is displayed there
    local ovl = floor(read_u32(0x000B6DF0))
    if drew_menu_card and ovl == 5 then return end

    local warn_pct = floor(get_setting("low_durability_warn", 25))
    local items = {}
    for slot = 0, 2 do
        if get_hero_joined(slot) then
            local cur = floor(get_hero_weapon_durability(slot))
            local maxd = floor(get_hero_weapon_max_durability(slot))
            if cur >= 0 and maxd > 0 then
                local ratio = clamp(cur / max(1, maxd), 0.0, 1.0)
                local hm = HERO_META[slot] or { name = get_hero_name(slot) }
                items[#items + 1] = { slot = slot, name = hm.name, cur = cur, maxd = maxd, ratio = ratio }
                if (ratio * 100.0) <= warn_pct then
                    if not warned_slot[slot] then
                        warned_slot[slot] = true
                        toast(string.format("LOW WEAPON DURABILITY: %s (%d/%d)!", hm.name, cur, maxd))
                    end
                else
                    warned_slot[slot] = false
                end
            end
        end
    end

    if #items == 0 then return end
    local panel_w = 216
    local panel_h = 26 + (#items * 22)
    local px = 960 - panel_w - 14
    local py = 14
    draw_rect(px, py, panel_w, panel_h, "#110E18", 205)
    draw_text(px + 10, py + 6, "EQUIPPED DURABILITY", 11, "#E6C88A")
    local y = py + 24
    for _, it in ipairs(items) do
        draw_text(px + 10, y, it.name, 11, "#DDD6E8")
        draw_bar(px + 84, y, 122, 13, it.ratio, durability_color(it.ratio), "#1C1626",
                 string.format("%d/%d", it.cur, it.maxd))
        y = y + 22
    end
end
