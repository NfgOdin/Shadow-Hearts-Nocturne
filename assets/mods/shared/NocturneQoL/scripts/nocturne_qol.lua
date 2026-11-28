-- ============================================================================
-- Nocturne QoL Suite v1.0.0 — All 4 Quadrilogy Titles
-- Post-Battle HP/MP Recovery + Cross-Game Bench EXP Share + Sanctuary Hotkey
-- ============================================================================

local function apply_victory_qol()
    local hp_pct = floor(get_setting("post_battle_hp", 25))
    local mp_pct = floor(get_setting("post_battle_mp", 15))
    local bench = get_setting("bench_exp_share", 1) ~= 0

    local notes = {}
    if hp_pct > 0 and heal_party(hp_pct) > 0 then
        notes[#notes + 1] = string.format("+%d%% HP", hp_pct)
    end
    if mp_pct > 0 and restore_party_mp(mp_pct) > 0 then
        notes[#notes + 1] = string.format("+%d%% MP", mp_pct)
    end
    if bench and sync_bench_exp() > 0 then
        notes[#notes + 1] = "Bench EXP Synced"
    end
    if #notes > 0 then
        toast("Nocturne QoL: " .. table.concat(notes, "  |  "))
    end
end

function on_game_ready()
    toast("Nocturne QoL Suite Active (Configure in F1 -> MODS)")
end

function on_battle_win(total_wins)
    apply_victory_qol()
end

-- Koudelka fires on_battle_end (no global wins counter)
function on_battle_end()
    if game_slug() == "koudelka" then
        apply_victory_qol()
    end
end

function on_hotkey(key)
    if get_setting("sanctuary_hotkey", 1) == 0 then return end
    if not is_in_game() then return end
    local hk = get_setting_str("heal_key", "5")
    if key == hk then
        local h = heal_party(100)
        local m = restore_party_mp(100)
        local uses = floor(tonumber(save_get("sanctuary_uses", 0)) or 0) + 1
        save_set("sanctuary_uses", uses)
        if h > 0 or m > 0 then
            toast(string.format("Sanctuary Blessing! Party Restored (Use #%d)", uses))
        else
            toast("Sanctuary Blessing: Party is already at full HP/MP")
        end
    end
end
