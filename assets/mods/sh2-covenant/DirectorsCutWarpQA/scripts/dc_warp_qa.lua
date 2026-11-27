-- ============================================================================
-- Director's Cut Dungeon Warp (QA Tester) v1.1.1 — Shadow Hearts: Covenant
-- Safely overrides the Title Screen "NEW GAME" starting map string in RAM
-- (SLUS_210.41 @ 0x00434EA0 + OPENING.BIN @ 0x0046C918, guarded by MWo3
-- header check so FIELD.BIN / BATTLE.BIN are never touched).
--
-- Usage: Boot Covenant fresh -> pick target in F1 -> MODS (or press 9) ->
-- click NEW GAME on the Title Screen to enter Nahash / Cannes / Deeper Neam /
-- Sea of Woods / Dog Shrine / Ring Soul Quiz / Debug Room natively!
-- ============================================================================

local DESTS = {
    { label = "Nahash Temple",          map = "KS_00_U" },
    { label = "Cannes Boat (Nahash)",   map = "CA_02_U" },
    { label = "Deeper Neam Ruins",      map = "ND_11_U" },
    { label = "Neam Ruins Portal",      map = "ND_04_A" },
    { label = "Sea of Woods",           map = "JU_00_U" },
    { label = "Dog Shrine",             map = "MS_20_U" },
    { label = "Ring Soul Quiz",         map = "_QUIZ"   },
    { label = "Debug Warp Room",        map = "DB_00_U" },
    { label = "Normal New Game",        map = "OP_00_U" },
}

local ADDR_ELF_START_MAP = 0x00434EA0  -- SLUS_210.41 "OP_00_U\0"
local ADDR_OVL_HDR       = 0x0045EA80  -- MWo3 overlay header base
local ADDR_OPENING_MAP   = 0x0046C918  -- OPENING.BIN "OP_00_U\0"
local MWO3_MAGIC         = 0x336F574D  -- "MWo3"

local VALID_SLOTS = {}
for i = 1, #DESTS do
    VALID_SLOTS[DESTS[i].map] = true
end

local cycle_offset = 0
local last_setting_idx = -1
local last_applied_map = ""

local function active_dest_index()
    local cfg_idx = floor(get_setting("destination", 0))
    if cfg_idx ~= last_setting_idx then
        last_setting_idx = cfg_idx
        cycle_offset = 0
    end
    return ((cfg_idx + cycle_offset) % #DESTS) + 1
end

local function read_ascii8(addr)
    local chars = {}
    for i = 0, 7 do
        local b = read_u8(addr + i)
        if b == 0 then break end
        if b < 32 or b > 126 then return "" end
        chars[#chars + 1] = string.char(b)
    end
    return table.concat(chars)
end

local function write_ascii8(addr, text)
    for i = 1, #text do
        write_u8(addr + (i - 1), string.byte(text, i))
    end
    for i = #text, 7 do
        write_u8(addr + i, 0)
    end
end

local function is_opening_overlay_loaded()
    -- OPENING.BIN has MWo3 magic at 0x45EA80 and text_size ~0xDE00 (< 0x20000);
    -- FIELD.BIN (~0x101540) and BATTLE.BIN (~0x284000) are much larger.
    return read_u32(ADDR_OVL_HDR) == MWO3_MAGIC
       and read_u32(ADDR_OVL_HDR + 8) == ADDR_OVL_HDR
       and read_u32(ADDR_OVL_HDR + 12) < 0x20000
end

local function apply_new_game_target(map_id)
    -- Patch SLUS_210.41 static string slot only if it currently holds a known map ID
    local cur_elf = read_ascii8(ADDR_ELF_START_MAP)
    if VALID_SLOTS[cur_elf] and cur_elf ~= map_id then
        write_ascii8(ADDR_ELF_START_MAP, map_id)
    end
    -- Patch OPENING.BIN string slot ONLY when OPENING.BIN is actively loaded
    if is_opening_overlay_loaded() then
        local cur_ovl = read_ascii8(ADDR_OPENING_MAP)
        if VALID_SLOTS[cur_ovl] and cur_ovl ~= map_id then
            write_ascii8(ADDR_OPENING_MAP, map_id)
        end
    end
end

function on_boot()
    if game_slug() ~= "sh2-covenant" then return end
    local d = DESTS[active_dest_index()]
    apply_new_game_target(d.map)
    last_applied_map = d.map
end

function on_frame()
    if game_slug() ~= "sh2-covenant" then return end
    local d = DESTS[active_dest_index()]
    apply_new_game_target(d.map)
    last_applied_map = d.map

    if get_setting("show_hud", 1) == 0 then
        hud_clear("dc_warp_qa")
        return
    end
    local ck = get_setting_str("cycle_key", "9")
    if is_opening_overlay_loaded() or not is_in_game() then
        hud_set("dc_warp_qa", "NEW GAME WARP", string.format("%s (%s) [%s]", d.label, d.map, ck), "#F3D37A")
    else
        hud_clear("dc_warp_qa")
    end
end

function on_hotkey(key)
    if game_slug() ~= "sh2-covenant" then return end
    local ck = get_setting_str("cycle_key", "9")
    if key == ck then
        cycle_offset = (cycle_offset + 1) % #DESTS
        local d = DESTS[active_dest_index()]
        apply_new_game_target(d.map)
        toast(string.format("DC New Game Warp -> %s (%s)  (Pick NEW GAME on Title Screen)", d.label, d.map))
    end
end
