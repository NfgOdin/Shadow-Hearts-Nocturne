-- ============================================================================
-- Director's Cut Dungeon Warp (QA Tester) v1.1.0 — Shadow Hearts: Covenant
-- Uses CField's native queued map-load slot (CField + 0xA35) so you can test
-- Nahash, Cannes Boat, Deeper Neam Ruins, Sea of Woods, Dog Shrine, Quiz, or
-- DB_00_U from any loaded field save without touching memcard.ps2 or .states!
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
}

local ADDR_FIELD_MGR = 0x0043FE54  -- *(u32*)(mgr + 0x90) -> CField*
local ADDR_FLDA_MAP  = 0x0010230C  -- active map ID string in SSH_SAVE FLDa
local OFF_CFIELD_PTR = 0x90
local OFF_MAP_STATE  = 0x17C
local OFF_WARP_FLAGS = 0x180
local OFF_WARP_NAME  = 0xA35

local cycle_offset = 0
local last_setting_idx = -1

local function active_dest_index()
    local cfg_idx = floor(get_setting("destination", 0))
    if cfg_idx ~= last_setting_idx then
        last_setting_idx = cfg_idx
        cycle_offset = 0
    end
    local idx = ((cfg_idx + cycle_offset) % #DESTS) + 1
    return idx
end

local function read_ascii(addr, max_len)
    local chars = {}
    for i = 0, max_len - 1 do
        local b = read_u8(addr + i)
        if b == 0 then break end
        if b >= 32 and b <= 126 then
            chars[#chars + 1] = string.char(b)
        end
    end
    return table.concat(chars)
end

local function write_ascii(addr, text, buf_len)
    for i = 1, #text do
        write_u8(addr + (i - 1), string.byte(text, i))
    end
    for i = #text, buf_len - 1 do
        write_u8(addr + i, 0)
    end
end

local function get_cfield()
    local mgr = read_u32(ADDR_FIELD_MGR)
    if mgr < 0x00100000 or mgr >= 0x02000000 then return 0 end
    local cf = read_u32(mgr + OFF_CFIELD_PTR)
    if cf < 0x00100000 or cf >= 0x02000000 then return 0 end
    return cf
end

local function trigger_warp(dest)
    local cf = get_cfield()
    if cf == 0 then
        toast("DC Warp: Load into a field map first (not on title/battle/menu).")
        return
    end
    local state = read_u8(cf + OFF_MAP_STATE)
    if state ~= 0 then
        toast("DC Warp: Map transition already in progress...")
        return
    end
    -- Ensure CField::Update() warp gate (0x4f34e8) accepts the queued map immediately
    local fl = read_u8(cf + OFF_WARP_FLAGS)
    if (fl & 0xE3) ~= 0 then
        write_u8(cf + OFF_WARP_FLAGS, fl & 0x1C)
    end
    write_ascii(cf + OFF_WARP_NAME, dest.map, 16)
    toast(string.format("DC Warp -> %s (%s)!", dest.label, dest.map))
end

function on_game_ready()
    if game_slug() ~= "sh2-covenant" then return end
    local hk = get_setting_str("warp_key", "F6")
    local ck = get_setting_str("cycle_key", "F7")
    toast(string.format("DC Warp QA Ready (%s = Warp, %s = Next Target)", hk, ck))
end

function on_frame()
    if game_slug() ~= "sh2-covenant" then return end
    if get_setting("show_hud", 1) == 0 or not is_in_game() then
        hud_clear("dc_warp_qa")
        return
    end
    local d = DESTS[active_dest_index()]
    local cur = read_ascii(ADDR_FLDA_MAP, 16)
    local hk = get_setting_str("warp_key", "F6")
    local ck = get_setting_str("cycle_key", "F7")
    if cur == "" then cur = "---" end
    hud_text(
        "dc_warp_qa",
        string.format("DC WARP [%s] -> %s (%s)  |  %s: Go  %s: Next", cur, d.label, d.map, hk, ck),
        16, 16, "#F3D37A"
    )
end

function on_hotkey(key)
    if game_slug() ~= "sh2-covenant" then return end
    if not is_in_game() then return end
    local hk = get_setting_str("warp_key", "F6")
    local ck = get_setting_str("cycle_key", "F7")
    if key == ck then
        cycle_offset = (cycle_offset + 1) % #DESTS
        local d = DESTS[active_dest_index()]
        toast(string.format("DC Warp Target: %s (%s)", d.label, d.map))
    elseif key == hk then
        trigger_warp(DESTS[active_dest_index()])
    end
end
