-- Nemeton Lantern — Koudelka Item Glow Mod (v1.1.0)
-- Highlights map pickup glints and interactive sparkles with a soft pulsing halo.
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
    if level <= 0 then return 0.75 end
    if level >= 2 then return 1.45 end
    return 1.15
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
    if lantern_enabled then
        set_item_glow(get_base_strength(level), get_palette_hex(preset))
    else
        set_item_glow(0.0, "#000000")
    end
    update_hud(preset, level)
end

function on_boot()
    apply_lantern_now()
    local preset = floor(get_setting("color_preset", 0) + 0.5)
    local key_lbl = get_setting_str("toggle_key", "*")
    log("Nemeton Lantern active (" .. get_palette_name(preset) .. ", " .. key_lbl .. " to toggle)")
end

function on_setting_changed(key, val)
    apply_lantern_now()
end

function on_hotkey(key)
    local toggle_key = get_setting_str("toggle_key", "*")
    if key == toggle_key then
        lantern_enabled = not lantern_enabled
        local preset = floor(get_setting("color_preset", 0) + 0.5)
        local level = floor(get_setting("intensity", 1) + 0.5)
        if lantern_enabled then
            set_item_glow(get_base_strength(level), get_palette_hex(preset))
            toast("Nemeton Lantern: ON (" .. get_palette_name(preset) .. ")")
        else
            set_item_glow(0.0, "#000000")
            toast("Nemeton Lantern: OFF")
        end
        update_hud(preset, level)
    end
end

function on_frame()
    local preset = floor(get_setting("color_preset", 0) + 0.5)
    local level = floor(get_setting("intensity", 1) + 0.5)
    if not lantern_enabled then
        set_item_glow(0.0, "#000000")
        return
    end

    local base = get_base_strength(level)
    local speed = get_setting("pulse_speed", 1.0)
    local strength = base
    if speed > 0.01 then
        -- Calm gothic breathing cycle: smoothstep exhale (35%) -> vivid inhale (115%) over ~1.8s
        local f = frame() * speed
        local phase = (f % 110.0) / 110.0
        local tri = 1.0 - abs(phase * 2.0 - 1.0)
        local smooth = tri * tri * (3.0 - 2.0 * tri)
        strength = clamp(base * (0.35 + 0.80 * smooth), 0.18, 1.50)
    end
    set_item_glow(strength, get_palette_hex(preset))
    if (frame() % 30) == 0 then
        update_hud(preset, level)
    end
end
