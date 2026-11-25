-- ============================================================================
-- Chronicle Unleashed v2.0.0 — Shadow Hearts (1913, SLUS-20347)
-- ============================================================================
-- Zero-polling Lua 5.4 semantic hooks + live F1 -> MODS settings.

local synced_battles = 0

local function refresh_hud()
    if get_setting("show_hud", 1) == 0 then
        hud_clear("chronicle")
        return
    end
    local bench = get_setting("bench_exp_share", 1) ~= 0
    local lbl = bench and ("BENCH EXP  (Lv " .. get_max_level() .. ")") or "ROSTER ACTIVE"
    hud_set("chronicle", "CHRONICLE", lbl, "#D8A858")
end

local function apply_unlock_if_enabled()
    if get_setting("unlock_all_heroes", 1) == 0 then return 0 end
    return unlock_all_party()
end

function on_boot()
    synced_battles = 0
end

function on_game_ready()
    local awakened = apply_unlock_if_enabled()
    refresh_hud()
    if awakened > 0 then
        toast(string.format("Chronicle Unleashed: All 6 Heroes Unlocked (%d awakened) + Bench EXP Share Active!", awakened))
    else
        toast("Chronicle Unleashed: Live Bench EXP Share Active!")
    end
end

function on_battle_win(total_wins)
    if get_setting("bench_exp_share", 1) == 0 then return end
    local boosted = sync_bench_exp()
    synced_battles = synced_battles + 1
    refresh_hud()
    if boosted > 0 then
        toast(string.format("Bench EXP Share: Synced %d hero(es) to Lv %d", boosted, get_max_level()))
    end
end

function on_setting_changed(key, val)
    if key == "unlock_all_heroes" and val ~= 0 and is_in_game() then
        local awakened = apply_unlock_if_enabled()
        if awakened > 0 then
            toast(string.format("Chronicle Unleashed: Awakened %d hero(es)!", awakened))
        end
    end
    refresh_hud()
end
