-- ============================================================================
-- Eclipse Challenge v1.0.0 — Hardcore Judgment Ring Risk/Reward Mode
-- Targets Shadow Hearts I, II: Covenant & III: From the New World
-- ============================================================================

local misses_this_run = 0

local function refresh_hud()
    if get_setting("show_hud", 1) == 0 then
        hud_clear("eclipse")
        return
    end
    local lbl = (misses_this_run == 0) and "FLAWLESS" or string.format("%d MISS(ES)", misses_this_run)
    local col = (misses_this_run == 0) and "#E6C88A" or "#E85868"
    hud_set("eclipse", "ECLIPSE MODE", lbl, col)
end

function on_boot()
    misses_this_run = floor(tonumber(save_get("misses", 0)) or 0)
end

function on_game_ready()
    misses_this_run = floor(tonumber(save_get("misses", 0)) or 0)
    refresh_hud()
    toast("Eclipse Challenge Active — Every Missed Judgment Ring Carries a Price!")
end

function on_ring_miss(prev_streak)
    misses_this_run = misses_this_run + 1
    save_set("misses", misses_this_run)

    local tax = floor(get_setting("miss_cash_tax", 200))
    local drain_pct = floor(get_setting("miss_hp_drain_pct", 10))
    local parts = {}

    if tax > 0 then
        add_cash(-tax)
        parts[#parts + 1] = string.format("-%d Cash", tax)
    end
    if drain_pct > 0 then
        local n = floor(get_hero_count())
        local affected = 0
        for s = 0, n - 1 do
            if get_hero_joined(s) then
                local hp = floor(get_hero_hp(s))
                local mhp = floor(get_hero_max_hp(s))
                if hp > 1 and mhp > 0 then
                    local loss = max(1, floor(mhp * drain_pct / 100))
                    set_hero_hp(s, max(1, hp - loss))
                    affected = affected + 1
                end
            end
        end
        if affected > 0 then
            parts[#parts + 1] = string.format("-%d%% HP", drain_pct)
        end
    end

    refresh_hud()
    if #parts > 0 then
        toast("ECLIPSE PENALTY: " .. table.concat(parts, "  |  "))
    end
end

function on_ring_perfect(streak)
    local bounty = floor(get_setting("perfect_bounty", 150))
    if bounty > 0 then
        local total = bounty * max(1, floor(streak))
        add_cash(total)
        toast(string.format("ECLIPSE CLUTCH PERFECT x%d!  +%d Cash", floor(streak), total))
    end
end

function on_setting_changed(key, val)
    refresh_hud()
end
