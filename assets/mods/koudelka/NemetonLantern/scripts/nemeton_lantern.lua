-- Nemeton Lantern — Koudelka Item Glow Mod
-- Highlights map pickup glints and interactive sparkles with a soft pulsing halo.
-- Hotkey: F6 toggles the glow on/off in real time.

lantern_enabled = true

function get_palette_hex(preset)
    if preset == 2 then return "#FF4444" end
    if preset == 3 then return "#66DDFF" end
    if preset == 4 then return "#55FF88" end
    return "#FFCC44"
end

function get_palette_name(preset)
    if preset == 2 then return "Crimson" end
    if preset == 3 then return "Moonlight" end
    if preset == 4 then return "Emerald" end
    return "Gold"
end

function get_base_strength(level)
    if level <= 1 then return 0.55 end
    if level >= 3 then return 1.35 end
    return 0.95
end

function update_hud(preset, level)
    local show_hud = get_setting("show_hud", 1)
    if show_hud == 0 then
        hud_clear("lantern")
        return
    end
    local col = get_palette_hex(preset)
    if lantern_enabled then
        local mode_lbl = "BRIGHT"
        if level <= 1 then mode_lbl = "SUBTLE" end
        if level >= 3 then mode_lbl = "BEACON" end
        hud_set("lantern", "LANTERN", mode_lbl .. " " .. get_palette_name(preset) .. " [F6]", col)
    else
        hud_set("lantern", "LANTERN", "OFF [F6]", "#888888")
    end
end

function on_boot()
    local preset = floor(get_setting("color_preset", 1) + 0.5)
    local level = floor(get_setting("intensity", 2) + 0.5)
    local base = get_base_strength(level)
    set_item_glow(base, get_palette_hex(preset))
    update_hud(preset, level)
    log("Nemeton Lantern active (" .. get_palette_name(preset) .. ", F6 to toggle)")
end

function on_hotkey(key)
    if key == "F6" then
        lantern_enabled = not lantern_enabled
        local preset = floor(get_setting("color_preset", 1) + 0.5)
        local level = floor(get_setting("intensity", 2) + 0.5)
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
    local preset = floor(get_setting("color_preset", 1) + 0.5)
    local level = floor(get_setting("intensity", 2) + 0.5)
    if not lantern_enabled then
        set_item_glow(0.0, "#000000")
        return
    end

    local base = get_base_strength(level)
    local speed = get_setting("pulse_speed", 1.0)
    local strength = base
    if speed > 0.01 then
        -- Smooth breathing cycle using smoothstep over a 90-frame period
        local f = frame() * speed
        local phase = (f % 90.0) / 90.0
        local tri = 1.0 - abs(phase * 2.0 - 1.0)
        local smooth = tri * tri * (3.0 - 2.0 * tri)
        strength = clamp(base * (0.72 + 0.42 * smooth), 0.15, 1.50)
    end
    set_item_glow(strength, get_palette_hex(preset))
    if (frame() % 30) == 0 then
        update_hud(preset, level)
    end
end
