-- ============================================================================
-- Crimson Flow — Judgment Ring Momentum (SH1 / SH2: Covenant / SH3: FTNW)
-- ============================================================================
-- Auto-detects the running PS2 Shadow Hearts title via verified EE RAM
-- signatures and rewards consecutive Judgment Ring Perfect Strikes with
-- escalating Cash, Bonus EXP (SH1), or Party HP Surges (SH2/SH3) + live
-- Gothic HUD callouts via toast(). Missing a Ring shatters the Flow streak!
-- ============================================================================

local initialized   = 0
local game_kind     = 0   -- 1=SH1, 2=SH2, 3=SH3
local prev_attempts = 0
local prev_success  = 0
local prev_perfect  = 0
local hit_streak    = 0
local perf_streak   = 0
local best_perf     = 0

function on_boot()
    initialized = 0
    game_kind = 0
    hit_streak = 0
    perf_streak = 0
    best_perf = 0
end

function detect_game()
    -- SH1 (SLUS-20347): "Changchun" at 0x004CAE40, playtime >= 1s, Yuri joined
    if read_u8(0x004CAE40) == 0x43 and read_u8(0x004CAE41) == 0x68 then
        if bit_and(read_u8(0x00100280), 0x40) ~= 0 and read_u32(0x0010000C) >= 1000 then
            return 1
        end
    end
    -- SH2: Covenant (SLUS-21041): "SSH_" at 0x00100000, playtime float >= 1.0
    if read_u8(0x00100000) == 0x53 and read_u8(0x00100001) == 0x53 and read_u8(0x00100002) == 0x48 then
        if read_f32(0x00102300) >= 1.0 then
            return 2
        end
    end
    -- SH3: From the New World (SLUS-21326): "SH3C" at 0x0010001D, playtime >= 1s
    if read_u8(0x0010001D) == 0x53 and read_u8(0x0010001E) == 0x48 and read_u8(0x0010001F) == 0x33 then
        if read_u32(0x00100098) >= 1000 then
            return 3
        end
    end
    return 0
end

function read_attempts(kind)
    if kind == 1 then return read_u32(0x001014AC) end
    if kind == 2 then return read_u32(0x001017C8) end
    local a = 0
    for i = 0, 6 do a = a + read_u32(0x00102778 + i * 0x90 + 0x0C) end
    return a
end

function read_successes(kind)
    if kind == 1 then return read_u32(0x001014B0) end
    if kind == 2 then return read_u32(0x001017CC) end
    local s = 0
    for i = 0, 6 do s = s + read_u32(0x00102778 + i * 0x90 + 0x18) end
    return s
end

function read_perfects(kind)
    if kind == 1 then return read_u32(0x001014B4) end
    if kind == 2 then return read_u32(0x001017D0) end
    local p = 0
    for i = 0, 6 do p = p + read_u32(0x00102778 + i * 0x90 + 0x1C) end
    return p
end

function grant_cash(kind, amount)
    if amount <= 0 then return end
    if kind == 1 then
        local c = read_u32(0x00100008)
        write_u32(0x00100008, min(c + amount, 9999999))
    elseif kind == 2 then
        local widx = bit_and(read_u8(0x00100132), 3)
        local addr = 0x00102D30 + widx * 4
        local c = read_u32(addr)
        write_u32(addr, min(c + amount, 9999999))
    elseif kind == 3 then
        local c = read_u32(0x00100D54)
        write_u32(0x00100D54, min(c + amount, 9999999))
    end
end

function grant_party_bonus(kind, streak, pct)
    if kind == 1 then
        -- SH1: award bonus EXP to every joined party member
        local bonus_exp = streak * 50
        for i = 0, 5 do
            local base = 0x00100280 + i * 0x50
            if bit_and(read_u8(base), 0x40) ~= 0 then
                local ce = read_u32(base + 0x04)
                write_u32(base + 0x04, ce + bonus_exp)
            end
        end
        return "+" .. bonus_exp .. " Bonus EXP"
    elseif kind == 2 then
        -- SH2: Covenant: restore pct% HP to living active party members
        for slot = 0, 13 do
            local base = 0x00100250 + slot * 0x90
            local c0 = read_u8(base + 0x16)
            local hp_max = read_u16(base + 0x28)
            local hp_cur = read_u16(base + 0x2A)
            if c0 ~= 0 and hp_max >= 10 and hp_cur > 0 and hp_cur < hp_max then
                local heal = max(1, floor(hp_max * pct / 100))
                write_u16(base + 0x2A, min(hp_max, hp_cur + heal))
            end
        end
        return "+" .. pct .. "% Party HP Surge"
    else
        -- SH3: From the New World: restore pct% HP to living party members
        for i = 0, 6 do
            local base = 0x00100D70 + i * 0x84
            local c0 = read_u8(base + 0x0A)
            local hp_cur = read_u16(base + 0x1C)
            local hp_max = read_u16(base + 0x1E)
            if c0 ~= 0 and hp_max >= 10 and hp_cur > 0 and hp_cur < hp_max then
                local heal = max(1, floor(hp_max * pct / 100))
                write_u16(base + 0x1C, min(hp_max, hp_cur + heal))
            end
        end
        return "+" .. pct .. "% Party HP Surge"
    end
end

function on_frame()
    -- Poll 10 times/sec (every 6 frames) for crisp post-ring response
    if (frame() % 6) ~= 0 then
        return
    end

    local kind = detect_game()
    if kind == 0 then
        return
    end

    local att = read_attempts(kind)
    local suc = read_successes(kind)
    local prf = read_perfects(kind)

    if initialized == 0 or kind ~= game_kind or att < prev_attempts then
        initialized = 1
        game_kind = kind
        prev_attempts = att
        prev_success = suc
        prev_perfect = prf
        hit_streak = 0
        perf_streak = 0
        toast("Crimson Flow Active: Chain Perfect Strikes for Escalating Rewards!")
        return
    end

    if att == prev_attempts then
        return
    end

    local da = att - prev_attempts
    local ds = max(0, suc - prev_success)
    local dp = max(0, prf - prev_perfect)
    prev_attempts = att
    prev_success = suc
    prev_perfect = prf

    if ds < da then
        -- Missed a Judgment Ring!
        if perf_streak >= 2 or hit_streak >= 3 then
            toast("CRIMSON FLOW SHATTERED! (Streak: " .. hit_streak .. "  |  Best Perfects: " .. best_perf .. ")")
        end
        hit_streak = 0
        perf_streak = 0
        return
    end

    hit_streak = hit_streak + ds
    if dp >= da then
        perf_streak = perf_streak + dp
        if perf_streak > best_perf then
            best_perf = perf_streak
        end

        local cash_bonus = perf_streak * 100
        local hp_pct = 5
        local tier = "CRIMSON FLOW"
        if perf_streak >= 10 then
            cash_bonus = perf_streak * 1000
            hp_pct = 20
            tier = "SOVEREIGN OF THE RING"
        elseif perf_streak >= 5 then
            cash_bonus = perf_streak * 500
            hp_pct = 15
            tier = "BLOOD RHYTHM"
        elseif perf_streak >= 3 then
            cash_bonus = perf_streak * 250
            hp_pct = 10
            tier = "CRIMSON FLOW"
        end

        grant_cash(kind, cash_bonus)
        local extra = grant_party_bonus(kind, perf_streak, hp_pct)
        toast(tier .. " x" .. perf_streak .. "!  +" .. cash_bonus .. " Cash  |  " .. extra)
    else
        -- Regular Hit (keeps hit_streak alive, resets Perfect streak)
        perf_streak = 0
        if hit_streak >= 5 and (hit_streak % 5) == 0 then
            local cash_bonus = hit_streak * 40
            grant_cash(kind, cash_bonus)
            toast("Steady Ring Streak x" .. hit_streak .. "!  +" .. cash_bonus .. " Cash")
        end
    end
end
