-- ============================================================================
-- Nemeton Tracker v1.0.0 — Koudelka (PS1) Exploration & Battle Companion HUD
-- 100% Co-op Safe (Pure Client-Side Read-Only HUD & Callouts)
-- Features: Room ID Tracker, Battle Turn Banner, Level-Up Callouts, and
--           Weapon Durability at a Glance with Low-Durability Alerts.
-- ============================================================================

local HERO_META = {
    [0] = { name = "Koudelka", color = "#E66898" },
    [1] = { name = "Edward",   color = "#68B8E6" },
    [2] = { name = "James",    color = "#E6C870" },
}

local tracker_visible = true
local current_room = -1
local in_battle = false
local turn_count = 0
local active_actor = -1
local warned_slot = { [0] = false, [1] = false, [2] = false }

local function durability_color(ratio)
    if ratio <= 0.20 then return "#FF4455" end
    if ratio <= 0.45 then return "#F0A840" end
    return "#70D898"
end

local function refresh_pills()
    if not tracker_visible then
        hud_clear_all()
        return
    end
    if get_setting("show_room_pill", 1) ~= 0 and current_room >= 0 then
        hud_set("room", "NEMETON ROOM", string.format("#%03d", current_room), "#D8B878")
    else
        hud_clear("room")
    end
    if get_setting("show_turn_banner", 1) ~= 0 and in_battle and active_actor >= 0 then
        local hm = HERO_META[active_actor] or { name = "Party", color = "#E6C88A" }
        hud_set("turn", "ACTIVE TURN", string.format("%s (T%d)", hm.name, turn_count), hm.color)
    else
        hud_clear("turn")
    end
end

function on_boot()
    tracker_visible = true
    in_battle = false
    turn_count = 0
    active_actor = -1
    warned_slot = { [0] = false, [1] = false, [2] = false }
end

function on_game_ready()
    local r = floor(read_u32(0x000B6B08))
    if r > 0 and r < 1024 then current_room = r end
    refresh_pills()
    log("Nemeton Tracker v1.0.0 ready (Co-op Safe)")
end

function on_area_load(room_id)
    current_room = floor(room_id)
    refresh_pills()
end

function on_battle_start()
    in_battle = true
    turn_count = 0
    active_actor = -1
    warned_slot = { [0] = false, [1] = false, [2] = false }
    refresh_pills()
end

function on_battle_end()
    in_battle = false
    active_actor = -1
    refresh_pills()
end

function on_turn_begin(char_slot)
    active_actor = floor(char_slot)
    turn_count = turn_count + 1
    refresh_pills()
end

function on_level_up(char_slot)
    local s = floor(char_slot)
    local hm = HERO_META[s] or { name = get_hero_name(s) }
    toast(string.format("%s reached Level %d!", hm.name, floor(get_hero_level(s))))
end

function on_hotkey(key)
    local tk = get_setting_str("toggle_key", "F8")
    if key == tk then
        tracker_visible = not tracker_visible
        refresh_pills()
        toast("Nemeton Tracker: " .. (tracker_visible and "ON" or "OFF"))
    end
end

function on_setting_changed(key, val)
    refresh_pills()
end

function on_frame()
    if not tracker_visible or not is_in_game() then return end
    if get_setting("show_durability", 1) == 0 then return end

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
    draw_text(px + 10, py + 6, "WEAPON DURABILITY", 11, "#E6C88A")
    local y = py + 24
    for _, it in ipairs(items) do
        draw_text(px + 10, y, it.name, 11, "#DDD6E8")
        draw_bar(px + 84, y, 122, 13, it.ratio, durability_color(it.ratio), "#1C1626",
                 string.format("%d/%d", it.cur, it.maxd))
        y = y + 22
    end
end
