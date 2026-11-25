-- Nemeton Lantern — Koudelka Item Glow Mod (v1.2.0)
-- Highlights 3D room pickups with a tight, true-additive shimmer & razor-thin rim
-- in exact PS1 painter's order. Breathing pulse & diagonal glint sweep run natively
-- in the D3D11 renderer for zero per-frame Lua overhead.
-- Hotkey: configurable via F1 -> MODS -> Nemeton Lantern -> Settings (default: *).

lantern_enabled = true

function get_palette_hex(preset)
    if preset == 1 then return "#FF4444" end
    if preset == 2 then return "#66DDFF" end
    if preset == 3 then return "#55FF88" end
    return "#FFCC44"
end

function get_palette_name(preset)
    if preset == 1 then return "Crimson" end
    if preset == 2 then return "Moonlight" end
    if preset == 3 then return "Emerald" end
    return "Gold"
end

function get_base_strength(level)
    if level <= 0 then return 0.70 end
    if level >= 2 then return 1.40 end
    return 1.05
end

function get_mode_label(level)
    if level <= 0 then return "SUBTLE" end
    if level >= 2 then return "BEACON" end
    return "BRIGHT"
end

function update_hud(preset, level)
    local show_hud = get_setting("show_hud", 1)
    if show_hud == 0 then
        hud_clear("lantern")
        return
    end
    local col = get_palette_hex(preset)
    local key_lbl = get_setting_str("toggle_key", "*")
    if lantern_enabled then
        hud_set("lantern", "LANTERN", get_mode_label(level) .. " " .. get_palette_name(preset) .. " [" .. key_lbl .. "]", col)
    else
        hud_set("lantern", "LANTERN", "OFF [" .. key_lbl .. "]", "#888888")
    end
end

function apply_lantern_now()
    local preset = floor(get_setting("color_preset", 0) + 0.5)
    local level = floor(get_setting("intensity", 1) + 0.5)
    local speed = get_setting("pulse_speed", 1.0)
    if lantern_enabled then
        set_item_glow(get_base_strength(level), get_palette_hex(preset), speed)
    else
        set_item_glow(0.0, "#000000", 0.0)
    end
    update_hud(preset, level)
end

function on_boot()
    apply_lantern_now()
    local preset = floor(get_setting("color_preset", 0) + 0.5)
    local key_lbl = get_setting_str("toggle_key", "*")
    log("Nemeton Lantern v1.2.0 active (" .. get_palette_name(preset) .. ", " .. key_lbl .. " to toggle)")
end

function on_setting_changed(key, val)
    apply_lantern_now()
end

function on_hotkey(key)
    local toggle_key = get_setting_str("toggle_key", "*")
    if key == toggle_key then
        lantern_enabled = not lantern_enabled
        apply_lantern_now()
        local preset = floor(get_setting("color_preset", 0) + 0.5)
        if lantern_enabled then
            toast("Nemeton Lantern: ON (" .. get_palette_name(preset) .. ")")
        else
            toast("Nemeton Lantern: OFF")
        end
    end
end
