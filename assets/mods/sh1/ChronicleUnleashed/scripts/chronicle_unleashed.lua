-- ============================================================================
-- Chronicle Unleashed — Shadow Hearts (1913, SLUS-20347) Flagship Layer 3 Mod
-- ============================================================================
-- Features:
--   1. Unlocks all 6 heroes (Yuri, Alice, Zhuzhen, Margarete, Keith, Halley)
--      as soon as active gameplay starts, scaling newly awakened heroes to
--      match Yuri's current level & EXP.
--   2. Live Bench EXP Share: after every battle, automatically syncs benched
--      party members' EXP & level so nobody falls behind.
--   3. Live Gothic HUD toasts via toast() when heroes awaken or sync.
-- ============================================================================

local PARTY_BASE = 0x00100280
local STRIDE     = 0x50
local announced  = 0
local last_battles = 0

function on_boot()
    announced = 0
    last_battles = 0
end

function on_frame()
    -- Poll 4 times per second (every 15 frames) for near-zero overhead
    if (frame() % 15) ~= 0 then
        return
    end

    -- Verify Shadow Hearts 1 (SLUS-20347) ELF signature ("Changchun" at 0x004CAE40)
    if read_u8(0x004CAE40) ~= 0x43 or read_u8(0x004CAE41) ~= 0x68 then
        return
    end

    -- Require active gameplay: Yuri joined (bit 6 of 0x00100280) and playtime >= 1s
    local yuri_flags = read_u8(PARTY_BASE)
    if bit_and(yuri_flags, 0x40) == 0 then
        return
    end
    if read_u32(0x0010000C) < 1000 then
        return
    end

    local yuri_lv  = read_u8(PARTY_BASE + 0x03)
    local yuri_exp = read_u32(PARTY_BASE + 0x04)
    if yuri_lv < 1 or yuri_lv > 99 then
        return
    end

    -- 1. Unlock all 6 heroes & match Yuri's level/EXP if not yet joined
    local newly_unlocked = 0
    for i = 1, 5 do
        local base = PARTY_BASE + i * STRIDE
        local flags = read_u8(base)
        if bit_and(flags, 0x40) == 0 then
            write_u8(base, bit_or(flags, 0x40))
            if read_u8(base + 0x03) < yuri_lv then
                write_u8(base + 0x03, yuri_lv)
            end
            if read_u32(base + 0x04) < yuri_exp then
                write_u32(base + 0x04, yuri_exp)
            end
            newly_unlocked = newly_unlocked + 1
        end
    end

    if announced == 0 then
        announced = 1
        if newly_unlocked > 0 then
            toast("Chronicle Unleashed: All 6 Heroes Unlocked + Bench EXP Share Active!")
        else
            toast("Chronicle Unleashed: Live Bench EXP Share Active!")
        end
    end

    -- 2. Live Bench EXP Share on battle victory
    local battles = read_u32(0x001014A4)
    if last_battles > 0 and battles > last_battles then
        local max_exp = yuri_exp
        local max_lv  = yuri_lv
        for i = 1, 5 do
            local base = PARTY_BASE + i * STRIDE
            local ce = read_u32(base + 0x04)
            local cl = read_u8(base + 0x03)
            if ce > max_exp then max_exp = ce end
            if cl > max_lv and cl <= 99 then max_lv = cl end
        end

        local boosted = 0
        for i = 0, 5 do
            local base = PARTY_BASE + i * STRIDE
            if bit_and(read_u8(base), 0x40) ~= 0 then
                local ce = read_u32(base + 0x04)
                if ce < max_exp then
                    write_u32(base + 0x04, max_exp)
                    boosted = boosted + 1
                end
                if read_u8(base + 0x03) < max_lv then
                    write_u8(base + 0x03, max_lv)
                end
            end
        end

        if boosted > 0 then
            toast("Bench EXP Share: Synced " .. boosted .. " hero(es) to Lv " .. max_lv)
        end
    end
    last_battles = battles
end
