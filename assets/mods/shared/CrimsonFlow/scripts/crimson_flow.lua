-- ============================================================================
-- Crimson Flow v2.0.0 — Judgment Ring Momentum (SH1 / SH2: Covenant / SH3)
-- ============================================================================
-- Powered by Lua 5.4 tables & zero-polling semantic Ring hooks.

local TIERS = {
    { min = 10, name = "SOVEREIGN OF THE RING", cash_per = 1000, hp_pct = 20, color = "#FF4C68" },
    { min = 5,  name = "BLOOD RHYTHM",          cash_per = 500,  hp_pct = 15, color = "#F06458" },
    { min = 3,  name = "CRIMSON FLOW",          cash_per = 250,  hp_pct = 10, color = "#E68C58" },
    { min = 1,  name = "CRIMSON SPARK",         cash_per = 100,  hp_pct = 5,  color = "#E6C88A" },
}

local cur_perf = 0
local best_perf = 0

local function pick_tier(streak)
    for _, t in ipairs(TIERS) do
        if streak >= t.min then return t end
    end
    return TIERS[#TIERS]
end

local function refresh_hud()
    if get_setting("show_hud", 1) == 0 then
        hud_clear("crimson_flow")
        return
    end
    local tier = pick_tier(cur_perf)
    local val = string.format("x%d  (Best %d)", cur_perf, best_perf)
    hud_set("crimson_flow", "CRIMSON FLOW", val, cur_perf > 0 and tier.color or "#A096B0")
end

function on_boot()
    cur_perf = 0
    best_perf = floor(tonumber(save_get("best_perf", 0)) or 0)
end

function on_game_ready()
    best_perf = floor(tonumber(save_get("best_perf", 0)) or 0)
    refresh_hud()
    toast("Crimson Flow v2.0 Active — Chain Perfect Strikes for Escalating Rewards!")
end

function on_ring_perfect(streak)
    cur_perf = floor(streak)
    if cur_perf > best_perf then
        best_perf = cur_perf
        save_set("best_perf", best_perf)
    end
    local tier = pick_tier(cur_perf)
    local mult = get_setting("reward_mult", 1.0)
    local cash_bonus = max(1, floor(cur_perf * tier.cash_per * mult + 0.5))
    add_cash(cash_bonus)

    local extra = ""
    if get_setting("enable_party_surge", 1) ~= 0 then
        if get_hero_count() == 6 then
            local exp_bonus = max(10, floor(cur_perf * 50 * mult + 0.5))
            add_party_exp(exp_bonus)
            extra = string.format("  |  +%d Bonus EXP", exp_bonus)
        else
            heal_party(tier.hp_pct)
            extra = string.format("  |  +%d%% Party HP", tier.hp_pct)
        end
    end

    refresh_hud()
    toast(string.format("%s x%d!  +%d Cash%s", tier.name, cur_perf, cash_bonus, extra))
end

function on_ring_hit(streak)
    cur_perf = 0
    local s = floor(streak)
    refresh_hud()
    if s >= 5 and (s % 5) == 0 then
        local mult = get_setting("reward_mult", 1.0)
        local cash_bonus = max(1, floor(s * 40 * mult + 0.5))
        add_cash(cash_bonus)
        toast(string.format("Steady Ring Streak x%d!  +%d Cash", s, cash_bonus))
    end
end

function on_ring_miss(prev_streak)
    local had_streak = (cur_perf >= 2) or (prev_streak >= 3)
    cur_perf = 0
    refresh_hud()
    if had_streak and get_setting("miss_alert", 1) ~= 0 then
        toast(string.format("CRIMSON FLOW SHATTERED! (Streak: %d  |  All-Time Best: %d)", floor(prev_streak), best_perf))
    end
end

function on_setting_changed(key, val)
    refresh_hud()
end
