#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Mod Studio v2 (6-Tab Developer & Modder Workbench).

One-stop standalone desktop workbench for modders targeting Koudelka (PS1),
Shadow Hearts (1913), Shadow Hearts: Covenant (1915), and Shadow Hearts: From
the New World (1929).

Zero external dependencies (Python 3 stdlib only); opens in a dedicated
Edge/Chrome --app desktop window. Strictly a developer/modder tool.
"""
from __future__ import annotations

import argparse
import http.server
import io
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import webbrowser
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

GAMES = [
    {"slug": "sh1", "name": "Shadow Hearts (1913)", "exe": "SH1.exe"},
    {"slug": "sh2-covenant", "name": "Shadow Hearts: Covenant (1915)", "exe": "SH2.exe"},
    {"slug": "sh3-ftnw", "name": "Shadow Hearts: From the New World (1929)", "exe": "SH3.exe"},
    {"slug": "koudelka", "name": "Koudelka (1898, PS1)", "exe": "Koudelka.exe"},
]
VALID_SLUGS = {g["slug"] for g in GAMES}

VERIFIED_RAM_MAP: dict[str, list[dict[str, str]]] = {
    "sh1": [
        {"addr": "0x00100008", "type": "u32", "name": "Cash (Gold)", "notes": "Active party currency"},
        {"addr": "0x0010000C", "type": "u32", "name": "Playtime (ms)", "notes": "Milliseconds played (>=1000 in-game)"},
        {"addr": "0x00100280", "type": "struct[6] * 0x50", "name": "Party Array", "notes": "+0x00 flags (bit 0x40 joined), +0x03 Lv, +0x04 EXP, +0x10 HP, +0x12 MP, +0x14 MaxHP, +0x16 MaxMP"},
        {"addr": "0x001014A4", "type": "u32", "name": "Battles Won", "notes": "Total victories"},
        {"addr": "0x001014AC", "type": "u32", "name": "Ring Attempts", "notes": "Judgment Ring total attempts"},
        {"addr": "0x001014B0", "type": "u32", "name": "Ring Successes", "notes": "Judgment Ring total hits"},
        {"addr": "0x001014B4", "type": "u32", "name": "Ring Perfects", "notes": "Judgment Ring perfect strikes"},
        {"addr": "0x004CAE40", "type": "ascii", "name": "Location Signature", "notes": "Starts with 'Ch' when loaded in-game"},
    ],
    "sh2-covenant": [
        {"addr": "0x00100000", "type": "ascii", "name": "Save Signature", "notes": "'SSH_' header when save is active"},
        {"addr": "0x00100132", "type": "u8", "name": "Active Wallet Index", "notes": "0..3; active Cash at 0x00102D30 + (idx&3)*4"},
        {"addr": "0x00100250", "type": "struct[8] * 0x90", "name": "Party Array", "notes": "+0x0F Lv, +0x10 EXP, +0x16 Active, +0x28 MaxHP, +0x2A HP, +0x2C MaxMP, +0x2E MP"},
        {"addr": "0x001016E4", "type": "u32", "name": "Battles Won", "notes": "Total victories"},
        {"addr": "0x001017C8", "type": "u32", "name": "Ring Attempts", "notes": "Judgment Ring attempts"},
        {"addr": "0x001017CC", "type": "u32", "name": "Ring Successes", "notes": "Judgment Ring hits"},
        {"addr": "0x001017D0", "type": "u32", "name": "Ring Perfects", "notes": "Judgment Ring perfect strikes"},
        {"addr": "0x00102300", "type": "f32", "name": "Playtime (sec)", "notes": "Float seconds played"},
        {"addr": "0x00102D30", "type": "u32[4]", "name": "Party Cash Wallets", "notes": "Indexed by u8[0x00100132] & 3"},
    ],
    "sh3-ftnw": [
        {"addr": "0x0010001D", "type": "ascii", "name": "Save Signature", "notes": "'SH3C' when loaded in-game"},
        {"addr": "0x00100098", "type": "u32", "name": "Playtime (ms)", "notes": "Milliseconds played"},
        {"addr": "0x00100D54", "type": "u32", "name": "Cash (Dollars)", "notes": "Active party currency"},
        {"addr": "0x00100D70", "type": "struct[7] * 0x84", "name": "Party Array", "notes": "+0x03 Lv, +0x04 EXP, +0x0A Active, +0x1C HP, +0x1E MaxHP, +0x20 MP, +0x22 MaxMP"},
        {"addr": "0x00102734", "type": "u32", "name": "Battles Won", "notes": "Total victories"},
        {"addr": "0x00102778", "type": "struct[7] * 0x90", "name": "Per-Hero Ring Stats", "notes": "+0x0C Attempts, +0x18 Successes, +0x1C Perfects"},
    ],
    "koudelka": [
        {"addr": "0x000B59A0", "type": "u32 ptr", "name": "Party Root Pointer", "notes": "Actor ptrs at root-0x44 (Koudelka), root-0x48 (Edward), root-0x4C (James)"},
        {"addr": "actor+0x146", "type": "ascii", "name": "Actor Name", "notes": "'Koudelka', 'Edward', 'James'"},
        {"addr": "actor+0x2D8", "type": "u16", "name": "Hero Level", "notes": "Current level"},
        {"addr": "actor+0x2DC", "type": "u32", "name": "Hero EXP", "notes": "Current experience points"},
        {"addr": "actor+0x410", "type": "u16", "name": "Hero HP / MaxHP", "notes": "+0x410 Cur HP, +0x412 Max HP, +0x414 Cur MP, +0x416 Max MP"},
        {"addr": "actor+0x4BA", "type": "u8", "name": "Hero Joined Flag", "notes": "Non-zero when in active party"},
    ],
}

MOD_PRESETS: dict[str, dict] = {
    "blank": {
        "label": "Blank Starter",
        "meta": {"name": "My Custom Mod", "author": "Modder", "version": "1.0.0", "description": "Custom gameplay mod."},
        "rules": {"start_cash": 0, "win_cash": 0, "win_exp": 0, "win_heal": 0, "bench_exp": False, "unlock_party": False,
                  "perf_cash": 0, "perf_streak_scale": False, "miss_penalty": 0, "miss_alert": False,
                  "welcome_toast": "My Custom Mod Active!", "win_toast": "", "perf_toast": ""},
        "settings": [],
    },
    "crimson_bounty": {
        "label": "Ring Momentum Bounty",
        "meta": {"name": "Crimson Ring Bounty", "author": "Modder", "version": "1.0.0", "description": "Escalating Cash & pinned streak HUD for Perfect Judgment Rings."},
        "rules": {"start_cash": 250, "win_cash": 100, "win_exp": 0, "win_heal": 10, "bench_exp": False, "unlock_party": False,
                  "perf_cash": 100, "perf_streak_scale": True, "miss_penalty": 75, "miss_alert": True,
                  "welcome_toast": "Crimson Ring Bounty Active!", "win_toast": "", "perf_toast": "CRIMSON STRIKE"},
        "settings": [
            {"key": "bounty_mult", "label": "Bounty Multiplier", "type": "int", "default": 1, "min": 1, "max": 5, "step": 1},
        ],
    },
    "hardcore": {
        "label": "Hardcore Ring Tax",
        "meta": {"name": "Hardcore Ring Tax", "author": "Modder", "version": "1.0.0", "description": "Heavy cash penalty on missed Judgment Rings, no free heals."},
        "rules": {"start_cash": 0, "win_cash": 0, "win_exp": 0, "win_heal": 0, "bench_exp": False, "unlock_party": False,
                  "perf_cash": 150, "perf_streak_scale": True, "miss_penalty": 250, "miss_alert": True,
                  "welcome_toast": "Hardcore Ring Tax Enabled — Don't Miss!", "win_toast": "", "perf_toast": "CLUTCH PERFECT"},
        "settings": [],
    },
    "bench_share": {
        "label": "Bench EXP & Full Roster",
        "meta": {"name": "Full Roster EXP Share", "author": "Modder", "version": "1.0.0", "description": "Keeps benched party members synced after every victory."},
        "rules": {"start_cash": 0, "win_cash": 0, "win_exp": 50, "win_heal": 15, "bench_exp": True, "unlock_party": True,
                  "perf_cash": 0, "perf_streak_scale": False, "miss_penalty": 0, "miss_alert": False,
                  "welcome_toast": "Full Roster + Bench EXP Share Active!", "win_toast": "", "perf_toast": ""},
        "settings": [],
    },
    "hotkey_utility": {
        "label": "F5 Party Heal + HUD Counter",
        "meta": {"name": "Sanctuary Hotkey", "author": "Modder", "version": "1.0.0", "description": "Press F5 to heal the party and track total heals in a persistent HUD widget."},
        "rules": {"start_cash": 0, "win_cash": 0, "win_exp": 0, "win_heal": 0, "bench_exp": False, "unlock_party": False,
                  "perf_cash": 0, "perf_streak_scale": False, "miss_penalty": 0, "miss_alert": False,
                  "welcome_toast": "", "win_toast": "", "perf_toast": ""},
        "custom_lua": (
            "-- Sanctuary Hotkey: demonstrates on_hotkey(), save_get/save_set(), hud_set(), and get_setting()\n"
            "function on_game_ready()\n"
            "    local uses = save_get(\"heal_uses\", 0)\n"
            "    hud_set(\"sanctuary\", \"SANCTUARY HEALS\", tostring(uses), \"#7CD992\")\n"
            "    toast(\"Sanctuary Hotkey Ready — Press F5 to Heal Party\")\n"
            "end\n\n"
            "function on_hotkey(key)\n"
            "    if key == \"F5\" and is_in_game() then\n"
            "        local pct = get_setting(\"heal_pct\", 50)\n"
            "        heal_party(pct)\n"
            "        local uses = save_get(\"heal_uses\", 0) + 1\n"
            "        save_set(\"heal_uses\", uses)\n"
            "        hud_set(\"sanctuary\", \"SANCTUARY HEALS\", tostring(uses), \"#7CD992\")\n"
            "        log(\"Sanctuary F5 heal #\" .. uses .. \" (+\" .. pct .. \"% HP)\")\n"
            "        toast(\"Sanctuary Heal! +\" .. pct .. \"% HP (Total: \" .. uses .. \")\")\n"
            "    end\n"
            "end\n"
        ),
        "settings": [
            {"key": "heal_pct", "label": "F5 Heal Percent", "type": "int", "default": 50, "min": 10, "max": 100, "step": 10},
        ],
    },
}


def sanitize_folder(name: str) -> str:
    clean = re.sub(r"[^A-Za-z0-9_\-]+", "_", (name or "").strip()).strip("_")
    return clean or "MyCustomMod"


def _lua_escape(s: str) -> str:
    return (s or "").replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "")


def generate_lua_from_rules(meta: dict, rules: dict) -> str:
    """Generate a clean, readable, commented Layer 3 Semantic Lua script from visual rules."""
    mod_name = str(meta.get("name") or "My Custom Mod").strip()
    author = str(meta.get("author") or "Community").strip()
    game = str(meta.get("game") or "sh1").strip()

    start_cash = int(rules.get("start_cash") or 0)
    win_cash = int(rules.get("win_cash") or 0)
    win_exp = int(rules.get("win_exp") or 0)
    win_heal = int(rules.get("win_heal") or 0)
    bench_exp = bool(rules.get("bench_exp"))
    unlock_party = bool(rules.get("unlock_party"))
    perf_cash = int(rules.get("perf_cash") or 0)
    perf_streak_scale = bool(rules.get("perf_streak_scale"))
    miss_penalty = int(rules.get("miss_penalty") or 0)
    miss_alert = bool(rules.get("miss_alert"))

    welcome_toast = str(rules.get("welcome_toast") or "").strip()
    win_toast = str(rules.get("win_toast") or "").strip()
    perf_toast = str(rules.get("perf_toast") or "").strip()

    lines = [
        "-- ==========================================================================",
        f"-- {mod_name} (by {author}) — Target: {game}",
        "-- Generated with Nocturne Mod Studio v2 (Layer 3 Semantic Lua API)",
        "-- ==========================================================================",
        "",
        "local boot_applied = 0",
        "",
        "function on_boot()",
        "    boot_applied = 0",
        "end",
        "",
        "function on_game_ready()",
        "    if boot_applied ~= 0 then return end",
        "    boot_applied = 1",
    ]
    if start_cash > 0:
        lines.append(f"    add_cash({start_cash})")
    if unlock_party:
        lines.append("    unlock_all_party()")
    if welcome_toast:
        lines.append(f'    toast("{_lua_escape(welcome_toast)}")')
    elif start_cash > 0 or unlock_party:
        lines.append(f'    toast("{_lua_escape(mod_name)} Active!")')
    lines.append("end")

    if win_cash > 0 or win_exp > 0 or win_heal > 0 or bench_exp or win_toast:
        lines += ["", "function on_battle_win(total_wins)"]
        if win_cash > 0:
            lines.append(f"    add_cash({win_cash})")
        if win_exp > 0:
            lines.append(f"    add_party_exp({win_exp})")
        if win_heal > 0:
            lines.append(f"    heal_party({win_heal})")
        if bench_exp:
            lines.append("    sync_bench_exp()")
        if win_toast:
            lines.append(f'    toast("{_lua_escape(win_toast)}")')
        else:
            parts = []
            if win_cash > 0:
                parts.append(f"+{win_cash} Cash")
            if win_exp > 0:
                parts.append(f"+{win_exp} EXP")
            if win_heal > 0:
                parts.append(f"+{win_heal}% HP")
            if bench_exp:
                parts.append("Bench Synced")
            if parts:
                lines.append(f'    toast("Victory Bonus: {" | ".join(parts)}")')
        lines.append("end")

    if perf_cash > 0 or perf_toast:
        lines += ["", "function on_ring_perfect(streak)"]
        if perf_cash > 0:
            if perf_streak_scale:
                lines.append(f"    local reward = {perf_cash} * streak")
            else:
                lines.append(f"    local reward = {perf_cash}")
            lines.append("    add_cash(reward)")
            lines.append('    hud_set("ring_streak", "RING STREAK", "x" .. streak, "#E65C6E")')
            if perf_toast:
                lines.append(f'    toast("{_lua_escape(perf_toast)} (x" .. streak .. " | +" .. reward .. " Cash)")')
            else:
                lines.append('    toast("PERFECT RING x" .. streak .. "!  +" .. reward .. " Cash")')
        elif perf_toast:
            lines.append(f'    toast("{_lua_escape(perf_toast)} (x" .. streak .. ")")')
        lines.append("end")

    if miss_penalty > 0 or miss_alert:
        lines += ["", "function on_ring_miss(prev_streak)"]
        lines.append('    hud_clear("ring_streak")')
        if miss_penalty > 0:
            lines.append(f"    add_cash(-{miss_penalty})")
        if miss_alert and miss_penalty > 0:
            lines.append(f'    toast("RING MISSED! -{miss_penalty} Cash (Streak " .. prev_streak .. " broken)")')
        elif miss_alert:
            lines.append('    if prev_streak >= 2 then toast("RING STREAK BROKEN! (Was x" .. prev_streak .. ")") end')
        elif miss_penalty > 0:
            lines.append(f'    toast("Ring Miss Penalty: -{miss_penalty} Cash")')
        lines.append("end")

    lines.append("")
    return "\n".join(lines)


def generate_pnach_from_rows(mod_name: str, rows: list[dict]) -> str:
    """Serialize structured .pnach rows into standard PCSX2/Nocturne .pnach text."""
    lines = [
        f"// {mod_name} — Layer 2 Memory Patches (.pnach)",
        "// Format: patch=<0=boot|1=every-frame>,EE,<hex_addr>,<byte|short|word|extended>,<hex_value>",
        "",
    ]
    for r in rows or []:
        if not r.get("enabled", True):
            continue
        addr = str(r.get("addr") or "").strip().lower().replace("0x", "").upper()
        val = str(r.get("value") or "").strip().lower().replace("0x", "").upper()
        if not addr or not val:
            continue
        width = str(r.get("width") or "word").strip().lower()
        if width not in ("byte", "short", "word", "extended"):
            width = "word"
        mode = "0" if r.get("boot_only") else "1"
        comment = str(r.get("comment") or "").strip()
        line = f"patch={mode},EE,{addr.zfill(8)},{width},{val.zfill(8)}"
        if comment:
            line += f"  // {comment}"
        lines.append(line)
    lines.append("")
    return "\n".join(lines)


def validate_lua_syntax(src: str) -> tuple[bool, list[str]]:
    """Validate Layer 3 Lua script syntax (balanced blocks, quotes, and parentheses)."""
    errors: list[str] = []
    block_stack: list[tuple[str, int]] = []
    paren_depth = 0
    in_block_comment = False

    for lineno, raw in enumerate(src.splitlines(), start=1):
        i = 0
        n = len(raw)
        line_tokens: list[str] = []
        while i < n:
            if in_block_comment:
                end = raw.find("]]", i)
                if end == -1:
                    break
                in_block_comment = False
                i = end + 2
                continue
            ch = raw[i]
            if ch == "-" and i + 1 < n and raw[i + 1] == "-":
                if i + 3 < n and raw[i + 2 : i + 4] == "[[":
                    in_block_comment = True
                    i += 4
                    continue
                break
            if ch in ('"', "'"):
                q = ch
                i += 1
                closed = False
                while i < n:
                    if raw[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    if raw[i] == q:
                        closed = True
                        i += 1
                        break
                    i += 1
                if not closed:
                    errors.append(f"Line {lineno}: Unclosed string literal.")
                continue
            if ch == "(":
                paren_depth += 1
                i += 1
                continue
            if ch == ")":
                paren_depth -= 1
                if paren_depth < 0:
                    errors.append(f"Line {lineno}: Unexpected closing ')'.")
                    paren_depth = 0
                i += 1
                continue
            if ch.isalpha() or ch == "_":
                j = i + 1
                while j < n and (raw[j].isalnum() or raw[j] == "_"):
                    j += 1
                line_tokens.append(raw[i:j])
                i = j
                continue
            i += 1

        for tok in line_tokens:
            if tok in ("function", "if", "for", "while"):
                block_stack.append((tok, lineno))
            elif tok == "end":
                if not block_stack:
                    errors.append(f"Line {lineno}: Unexpected 'end' without matching block.")
                else:
                    block_stack.pop()

    if in_block_comment:
        errors.append("Unclosed block comment '--[['.")
    if paren_depth > 0:
        errors.append(f"Unclosed '(' ({paren_depth} missing closing parenthesis).")
    for kw, ln in block_stack:
        errors.append(f"Line {ln}: Unclosed '{kw}' block (missing 'end').")

    return (len(errors) == 0, errors)


def list_game_mods(install_dir: str, game_slug: str) -> list[dict]:
    mods_dir = os.path.join(install_dir, "games", game_slug, "mods")
    out: list[dict] = []
    if not os.path.isdir(mods_dir):
        return out
    for entry in sorted(os.listdir(mods_dir)):
        mdir = os.path.join(mods_dir, entry)
        if not os.path.isdir(mdir):
            continue
        meta: dict = {
            "folder": entry, "id": entry.lower(), "name": entry,
            "version": "1.0.0", "author": "Community", "description": "",
            "settings": [], "assets": {"files": [], "textures": [], "video": [], "plugins": []},
        }
        mj = os.path.join(mdir, "mod.json")
        if os.path.isfile(mj):
            try:
                with open(mj, "r", encoding="utf-8") as f:
                    data = json.load(f)
                for k in ("id", "name", "version", "author", "description"):
                    if data.get(k):
                        meta[k] = str(data[k])
                if isinstance(data.get("studio_rules"), dict):
                    meta["studio_rules"] = data["studio_rules"]
                if isinstance(data.get("settings"), list):
                    meta["settings"] = data["settings"]
            except Exception:
                pass
        lua_code = ""
        sdir = os.path.join(mdir, "scripts")
        if os.path.isdir(sdir):
            for sf in sorted(os.listdir(sdir)):
                if sf.lower().endswith(".lua"):
                    try:
                        with open(os.path.join(sdir, sf), "r", encoding="utf-8", errors="replace") as f:
                            lua_code = f.read()
                        meta["script_file"] = sf
                        break
                    except OSError:
                        pass
        pnach_code = ""
        pdir = os.path.join(mdir, "patches")
        if os.path.isdir(pdir):
            for pf in sorted(os.listdir(pdir)):
                if pf.lower().endswith(".pnach"):
                    try:
                        with open(os.path.join(pdir, pf), "r", encoding="utf-8", errors="replace") as f:
                            pnach_code = f.read()
                        meta["pnach_file"] = pf
                        break
                    except OSError:
                        pass
        for layer in ("files", "textures", "video", "plugins"):
            ldir = os.path.join(mdir, layer)
            if os.path.isdir(ldir):
                found = []
                for root, _, fnames in os.walk(ldir):
                    for fn in sorted(fnames):
                        rel = os.path.relpath(os.path.join(root, fn), ldir).replace("\\", "/")
                        found.append(rel)
                meta["assets"][layer] = found
        meta["lua_code"] = lua_code
        meta["pnach_code"] = pnach_code
        out.append(meta)
    return out


def detect_game_conflicts(install_dir: str, game_slug: str) -> list[dict]:
    """Scan all mods for `game_slug` and detect overlapping files/, textures/, video/, and .pnach addresses."""
    mods = list_game_mods(install_dir, game_slug)
    conflicts: list[dict] = []
    owners: dict[tuple[str, str], str] = {}
    for m in mods:
        folder = m["folder"]
        for layer in ("files", "textures", "video"):
            for rel in m["assets"].get(layer, []):
                key = (layer, rel.lower())
                if key in owners:
                    conflicts.append({"layer": layer, "resource": rel, "winner": owners[key], "shadowed": folder})
                else:
                    owners[key] = folder
        for line in (m.get("pnach_code") or "").splitlines():
            clean = line.split("//", 1)[0].split("#", 1)[0].strip()
            if not clean.lower().startswith("patch="):
                continue
            parts = [p.strip() for p in clean.split("=", 1)[1].split(",")]
            if len(parts) >= 5:
                addr = parts[2].upper()
                key = ("patches", addr)
                if key in owners and owners[key] != folder:
                    conflicts.append({"layer": "patches", "resource": f"0x{addr}", "winner": owners[key], "shadowed": folder})
                elif key not in owners:
                    owners[key] = folder
    return conflicts


def generate_cpp_plugin_scaffold(install_dir: str, game_slug: str, folder: str, mod_name: str) -> dict:
    """Create a starter Layer 4 C++ .dll plugin project inside games/<slug>/mods/<folder>/plugins_src/."""
    if game_slug not in VALID_SLUGS:
        return {"ok": False, "error": f"Unknown game slug: {game_slug}"}
    clean_folder = sanitize_folder(folder)
    mdir = os.path.join(install_dir, "games", game_slug, "mods", clean_folder)
    src_dir = os.path.join(mdir, "plugins_src")
    os.makedirs(src_dir, exist_ok=True)
    os.makedirs(os.path.join(mdir, "plugins"), exist_ok=True)

    cpp_path = os.path.join(src_dir, f"{clean_folder.lower()}_plugin.cpp")
    cpp_src = (
        f"// {mod_name} — Layer 4 Native C++ Plugin (NocturneModApiV1)\n"
        "// Build with: build_plugin.cmd  (requires tools/portable/llvm-mingw/bin/clang++.exe)\n"
        "// Enable in game via: allow_native_plugins = 1 in keys.ini\n"
        '#include "nocturne_mod_api.h"\n'
        "#include <cstdio>\n\n"
        "static void on_boot(const NocturneModApiV1 *api) {\n"
        f'    std::fprintf(stderr, "[{clean_folder}] Layer 4 plugin booted for %s (RAM=%u bytes)\\n",\n'
        "                 api->game_slug ? api->game_slug : \"unknown\", api->guest_ram_size);\n"
        "}\n\n"
        "static void on_frame(const NocturneModApiV1 *api, uint64_t frame_index) {\n"
        "    (void)api;\n"
        "    (void)frame_index;\n"
        "    // Example: uint32_t cash = api->read_u32(api->guest_ram, api->guest_ram_size, 0x00100008u);\n"
        "}\n\n"
        "extern \"C\" NOCTURNE_MOD_EXPORT bool NocturneModInitV1(const NocturneModApiV1 *api, NocturneModCallbacksV1 *out_cbs) {\n"
        "    if (!api || !out_cbs || api->api_version != NOCTURNE_MOD_API_VERSION) return false;\n"
        "    out_cbs->on_boot = on_boot;\n"
        "    out_cbs->on_frame = on_frame;\n"
        "    return true;\n"
        "}\n"
    )
    with open(cpp_path, "w", encoding="utf-8") as f:
        f.write(cpp_src)

    cmd_path = os.path.join(src_dir, "build_plugin.cmd")
    clang_path = os.path.join(REPO_ROOT, "tools", "portable", "llvm-mingw", "bin", "clang++.exe")
    sdk_inc = os.path.join(REPO_ROOT, "sdk")
    cmd_src = (
        "@echo off\r\n"
        f'"{clang_path}" -shared -O2 -std=c++20 -I "{sdk_inc}" "%~dp0{clean_folder.lower()}_plugin.cpp" '
        f'-o "%~dp0..\\plugins\\{clean_folder.lower()}.dll"\r\n'
        "if %ERRORLEVEL% NEQ 0 (echo Build FAILED! & pause & exit /b 1)\r\n"
        f"echo Built plugins\\{clean_folder.lower()}.dll successfully!\r\n"
        "pause\r\n"
    )
    with open(cmd_path, "w", encoding="utf-8") as f:
        f.write(cmd_src)

    return {"ok": True, "cpp_path": cpp_path, "cmd_path": cmd_path, "message": f"Created Layer 4 C++ SDK scaffold in {src_dir}"}


def _enable_mod_in_keys_ini(install_dir: str, game_slug: str, folder_name: str) -> None:
    keys_path = os.path.join(install_dir, "games", game_slug, "keys.ini")
    if not os.path.isfile(keys_path):
        return
    try:
        with open(keys_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return
    found = False
    for idx, line in enumerate(lines):
        if line.strip().lower().startswith("active_mods"):
            eq = line.find("=")
            if eq != -1:
                cur = [t.strip() for t in line[eq + 1 :].split(",") if t.strip()]
                new_toks = []
                seen = False
                for tok in cur:
                    bare = tok[1:].strip() if tok.startswith("!") else tok
                    if bare.lower() == folder_name.lower():
                        new_toks.append(folder_name)
                        seen = True
                    else:
                        new_toks.append(tok)
                if not seen:
                    new_toks.append(folder_name)
                lines[idx] = "active_mods = " + ",".join(new_toks)
                found = True
                break
    if not found:
        lines.append("active_mods = " + folder_name)
    try:
        with open(keys_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except OSError:
        pass


def install_mod_to_game(install_dir: str, game_slug: str, payload: dict) -> dict:
    if game_slug not in VALID_SLUGS:
        return {"ok": False, "error": f"Unknown game slug: {game_slug}"}
    meta = payload.get("meta") or {}
    rules = payload.get("rules") or {}
    settings = payload.get("settings") or []
    lua_code = str(payload.get("lua_code") or "")
    pnach_code = str(payload.get("pnach_code") or "")
    if not lua_code.strip():
        lua_code = generate_lua_from_rules(meta, rules)

    ok, errs = validate_lua_syntax(lua_code)
    if not ok:
        return {"ok": False, "error": "Lua syntax error: " + "; ".join(errs), "errors": errs}

    folder = sanitize_folder(str(meta.get("folder") or meta.get("name") or "MyCustomMod"))
    mdir = os.path.join(install_dir, "games", game_slug, "mods", folder)
    os.makedirs(os.path.join(mdir, "scripts"), exist_ok=True)
    if payload.get("include_layers"):
        for sub in ("files", "textures", "video", "patches", "plugins"):
            os.makedirs(os.path.join(mdir, sub), exist_ok=True)

    manifest = {
        "id": folder.lower(),
        "name": str(meta.get("name") or folder).strip(),
        "version": str(meta.get("version") or "1.0.0").strip(),
        "author": str(meta.get("author") or "Community").strip(),
        "game": game_slug,
        "description": str(meta.get("description") or "Created with Nocturne Mod Studio.").strip(),
        "studio_rules": rules,
        "settings": settings,
    }
    with open(os.path.join(mdir, "mod.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    script_name = sanitize_folder(folder).lower() + ".lua"
    with open(os.path.join(mdir, "scripts", script_name), "w", encoding="utf-8") as f:
        f.write(lua_code)

    if pnach_code.strip():
        os.makedirs(os.path.join(mdir, "patches"), exist_ok=True)
        pnach_name = sanitize_folder(folder).lower() + ".pnach"
        with open(os.path.join(mdir, "patches", pnach_name), "w", encoding="utf-8") as f:
            f.write(pnach_code)

    _enable_mod_in_keys_ini(install_dir, game_slug, folder)
    return {
        "ok": True,
        "folder": folder,
        "path": mdir,
        "message": f"Installed & enabled '{manifest['name']}' in games/{game_slug}/mods/{folder}/",
    }


def build_nocturne_mod_zip_bytes(payload: dict, install_dir: str | None = None) -> tuple[str, bytes]:
    meta = payload.get("meta") or {}
    rules = payload.get("rules") or {}
    settings = payload.get("settings") or []
    game_slug = str(payload.get("game") or meta.get("game") or "sh1")
    folder = sanitize_folder(str(meta.get("folder") or meta.get("name") or "MyCustomMod"))
    lua_code = str(payload.get("lua_code") or "") or generate_lua_from_rules(meta, rules)
    pnach_code = str(payload.get("pnach_code") or "")

    manifest = {
        "id": folder.lower(),
        "name": str(meta.get("name") or folder).strip(),
        "version": str(meta.get("version") or "1.0.0").strip(),
        "author": str(meta.get("author") or "Community").strip(),
        "game": game_slug,
        "description": str(meta.get("description") or "Created with Nocturne Mod Studio.").strip(),
        "studio_rules": rules,
        "settings": settings,
    }

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(f"{folder}/mod.json", json.dumps(manifest, indent=2) + "\n")
        zf.writestr(f"{folder}/scripts/{folder.lower()}.lua", lua_code)
        if pnach_code.strip():
            zf.writestr(f"{folder}/patches/{folder.lower()}.pnach", pnach_code)
        # Include any bundled files/, textures/, video/, plugins/ already in the mod folder
        if install_dir:
            mdir = os.path.join(install_dir, "games", game_slug, "mods", folder)
            for layer in ("files", "textures", "video", "plugins"):
                ldir = os.path.join(mdir, layer)
                if os.path.isdir(ldir):
                    for root, _, fnames in os.walk(ldir):
                        for fn in sorted(fnames):
                            full = os.path.join(root, fn)
                            rel = os.path.relpath(full, mdir).replace("\\", "/")
                            zf.write(full, f"{folder}/{rel}")
    return f"{folder}.nocturne-mod", buf.getvalue()


def launch_game_dev_mode(install_dir: str, game_slug: str) -> dict:
    """Launch the game executable with --mod-dev so hot-reload, F9, and F10 Inspector are active."""
    ginfo = next((g for g in GAMES if g["slug"] == game_slug), None)
    if not ginfo:
        return {"ok": False, "error": f"Unknown game slug: {game_slug}"}
    gdir = os.path.join(install_dir, "games", game_slug)
    exe_path = os.path.join(gdir, ginfo["exe"])
    if not os.path.isfile(exe_path):
        # Fallback to engine binary in repo build
        if game_slug == "koudelka":
            cand = os.path.join(install_dir, "build", "native-koudelka", "psxrun.exe")
        else:
            cand = os.path.join(install_dir, "build", "native-sh2-covenant", "nocturne_ps2.exe")
        if os.path.isfile(cand):
            exe_path = cand
        else:
            return {"ok": False, "error": f"Game executable not found: {exe_path}"}
    cmd = [exe_path, "--game-dir", gdir, "--mod-dev"]
    try:
        subprocess.Popen(cmd, cwd=gdir)
        return {"ok": True, "message": f"Launched {os.path.basename(exe_path)} in --mod-dev mode (Auto Hot-Reload + F9 + F10 Inspector)"}
    except OSError as exc:
        return {"ok": False, "error": str(exc)}


HTML_PAGE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Nocturne Mod Studio v2 — Modder Workbench</title>
<style>
  :root {
    --bg: #0b0910; --panel: #14101d; --panel2: #1c1628; --border: #3a2d4f;
    --gold: #e6c88a; --crimson: #c23b4d; --text: #ece7f4; --dim: #9e94b2;
    --good: #6ed697; --warn: #f0b95b; --bad: #f06e7a;
  }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font-family:"Segoe UI",system-ui,sans-serif; font-size:13px; display:flex; flex-direction:column; height:100vh; overflow:hidden; }
  header { display:flex; align-items:center; justify-content:space-between; padding:10px 18px; background:linear-gradient(90deg,#171022,#23142a,#140f1d); border-bottom:1px solid var(--border); }
  .brand { font-family:Georgia,serif; font-size:18px; color:var(--gold); letter-spacing:.5px; }
  .brand small { color:var(--dim); font-family:"Segoe UI",sans-serif; font-size:11px; margin-left:8px; }
  .top-controls { display:flex; gap:8px; align-items:center; }
  select, input, textarea { background:#0f0c17; color:var(--text); border:1px solid var(--border); border-radius:6px; padding:6px 8px; font-family:inherit; font-size:12.5px; }
  select:focus, input:focus, textarea:focus { outline:none; border-color:var(--gold); }
  button { background:var(--panel2); color:var(--text); border:1px solid var(--border); border-radius:6px; padding:6px 12px; cursor:pointer; font-size:12.5px; font-weight:600; transition:all .12s; }
  button:hover { border-color:var(--gold); background:#251d36; }
  button.primary { background:linear-gradient(135deg,#8c2233,#b83246); border-color:var(--gold); color:#fff; }
  button.primary:hover { filter:brightness(1.12); }
  button.gold { background:rgba(230,200,138,.14); border-color:var(--gold); color:var(--gold); }
  nav.tabs { display:flex; gap:4px; padding:8px 18px 0; background:#100c18; border-bottom:1px solid var(--border); }
  nav.tabs button { border-radius:7px 7px 0 0; border-bottom:none; padding:8px 14px; color:var(--dim); background:transparent; border-color:transparent; }
  nav.tabs button.active { background:var(--panel); color:var(--gold); border-color:var(--border); }
  main { flex:1; overflow:auto; padding:16px 18px; }
  .tab-pane { display:none; height:100%; }
  .tab-pane.active { display:block; }
  .grid2 { display:grid; grid-template-columns:1fr 1fr; gap:14px; }
  .card { background:var(--panel); border:1px solid var(--border); border-radius:9px; padding:14px; margin-bottom:12px; }
  .card h3 { margin:0 0 10px; font-size:13px; color:var(--gold); text-transform:uppercase; letter-spacing:.8px; }
  .row { display:flex; gap:10px; align-items:center; margin-bottom:8px; flex-wrap:wrap; }
  .row label { min-width:150px; color:var(--dim); font-size:12px; }
  .row input[type=number], .row input[type=text] { flex:1; min-width:120px; }
  .code-box { width:100%; min-height:360px; font-family:Consolas,"Cascadia Code",monospace; font-size:12.5px; line-height:1.45; resize:vertical; }
  .pill-bar { display:flex; flex-wrap:wrap; gap:6px; margin-bottom:10px; }
  .pill { padding:4px 9px; border-radius:999px; font-size:11.5px; background:#1e172c; border:1px solid var(--border); color:var(--gold); cursor:pointer; }
  .pill:hover { background:#2a203d; }
  table { width:100%; border-collapse:collapse; font-size:12px; }
  th, td { text-align:left; padding:6px 8px; border-bottom:1px solid #261e36; }
  th { color:var(--gold); font-weight:600; }
  .mono { font-family:Consolas,monospace; color:#c8d8ff; }
  .status-bar { padding:7px 18px; background:#100c18; border-top:1px solid var(--border); display:flex; justify-content:space-between; align-items:center; font-size:12px; color:var(--dim); }
  .ok { color:var(--good); } .err { color:var(--bad); } .warn { color:var(--warn); }
</style>
</head>
<body>
<header>
  <div class="brand">NOCTURNE MOD STUDIO <small>v2 Developer Workbench — 4-Layer Mod Engine</small></div>
  <div class="top-controls">
    <select id="gameSelect" title="Target Game"></select>
    <select id="existingSelect" title="Load Existing Mod"><option value="">+ New Mod</option></select>
    <button class="gold" onclick="doLaunchDev()">Launch Game (--mod-dev)</button>
    <button class="primary" onclick="doInstall()">Save &amp; Install</button>
    <button onclick="doExport()">Export .nocturne-mod</button>
  </div>
</header>

<nav class="tabs">
  <button class="active" onclick="showTab('rules',this)">1. Rules &amp; Presets</button>
  <button onclick="showTab('lua',this)">2. Lua IDE &amp; Settings</button>
  <button onclick="showTab('pnach',this)">3. Memory Patches (.pnach)</button>
  <button onclick="showTab('assets',this)">4. Assets &amp; Layers</button>
  <button onclick="showTab('sdk',this)">5. RAM Map &amp; C++ SDK</button>
  <button onclick="showTab('test',this)">6. Conflicts &amp; Dev Tools</button>
</nav>

<main>
  <!-- TAB 1: RULES & PRESETS -->
  <div id="tab-rules" class="tab-pane active">
    <div class="card">
      <h3>Starter Presets &amp; Mod Metadata</h3>
      <div class="pill-bar" id="presetBar"></div>
      <div class="grid2">
        <div>
          <div class="row"><label>Mod Name</label><input id="m_name" type="text" value="My Custom Mod" oninput="onNameEdited()"></div>
          <div class="row"><label>Folder ID</label><input id="m_folder" type="text" value="MyCustomMod"></div>
        </div>
        <div>
          <div class="row"><label>Author &amp; Version</label><input id="m_author" type="text" value="Modder" style="max-width:150px"><input id="m_version" type="text" value="1.0.0" style="max-width:90px"></div>
          <div class="row"><label>Description</label><input id="m_desc" type="text" value="Created with Nocturne Mod Studio v2."></div>
        </div>
      </div>
    </div>

    <div class="grid2">
      <div>
        <div class="card">
          <h3>Economy &amp; Party Progression</h3>
          <div class="row"><label>Starting Cash Bonus</label><input id="r_start_cash" type="number" value="0" oninput="syncLua()"></div>
          <div class="row"><label>Battle Win Cash Bonus</label><input id="r_win_cash" type="number" value="0" oninput="syncLua()"></div>
          <div class="row"><label>Battle Win Party EXP</label><input id="r_win_exp" type="number" value="0" oninput="syncLua()"></div>
          <div class="row"><label>Post-Battle Party Heal %</label><input id="r_win_heal" type="number" value="0" min="0" max="100" oninput="syncLua()"></div>
          <div class="row"><label><input id="r_bench_exp" type="checkbox" onchange="syncLua()"> Sync Bench EXP on Win</label>
                           <label><input id="r_unlock_party" type="checkbox" onchange="syncLua()"> Unlock All Party Members</label></div>
        </div>
        <div class="card">
          <h3>Judgment Ring &amp; Gothic HUD Toasts</h3>
          <div class="row"><label>Perfect Ring Cash</label><input id="r_perf_cash" type="number" value="0" oninput="syncLua()"></div>
          <div class="row"><label><input id="r_perf_scale" type="checkbox" onchange="syncLua()"> Scale Reward by Streak</label>
                           <label><input id="r_miss_alert" type="checkbox" onchange="syncLua()"> Alert on Streak Break</label></div>
          <div class="row"><label>Ring Miss Cash Penalty</label><input id="r_miss_penalty" type="number" value="0" oninput="syncLua()"></div>
          <div class="row"><label>Welcome Toast</label><input id="r_welcome_toast" type="text" value="My Custom Mod Active!" oninput="syncLua()"></div>
          <div class="row"><label>Victory Toast</label><input id="r_win_toast" type="text" placeholder="(auto-generated if blank)" oninput="syncLua()"></div>
          <div class="row"><label>Perfect Strike Toast</label><input id="r_perf_toast" type="text" placeholder="(auto-generated if blank)" oninput="syncLua()"></div>
        </div>
      </div>
      <div class="card">
        <h3>Live Generated Semantic Lua <label style="float:right;font-size:11px;color:var(--dim)"><input id="customCodeToggle" type="checkbox"> Custom Code Lock</label></h3>
        <textarea id="luaPreview" class="code-box" readonly></textarea>
      </div>
    </div>
  </div>

  <!-- TAB 2: LUA IDE & SETTINGS -->
  <div id="tab-lua" class="tab-pane">
    <div class="grid2">
      <div class="card">
        <h3>Layer 3 Lua Script Editor</h3>
        <div class="pill-bar">
          <span class="pill" onclick="insertSnippet('on_hotkey')">+ on_hotkey(F5)</span>
          <span class="pill" onclick="insertSnippet('hud_set')">+ hud_set()</span>
          <span class="pill" onclick="insertSnippet('save')">+ save_get/set()</span>
          <span class="pill" onclick="insertSnippet('setting')">+ get_setting()</span>
          <span class="pill" onclick="insertSnippet('hero')">+ Per-Hero HP/MP</span>
          <span class="pill" onclick="insertSnippet('battle')">+ on_battle_win</span>
          <span class="pill" onclick="doValidateLua()">Validate Syntax</span>
          <span class="pill" onclick="doCopyLua()">Copy Lua</span>
        </div>
        <textarea id="luaEditor" class="code-box" oninput="onLuaEdited()"></textarea>
        <div id="luaValResult" style="margin-top:8px;font-size:12px;"></div>
      </div>
      <div class="card">
        <h3>Configurable Mod Settings (mod.json "settings")</h3>
        <p style="color:var(--dim);margin-top:0">Expose tunable knobs via <code>get_setting("key", default)</code> and persisted <code>mod_config.ini</code>.</p>
        <table id="settingsTable">
          <thead><tr><th>Key</th><th>Label</th><th>Type</th><th>Default</th><th>Min</th><th>Max</th><th></th></tr></thead>
          <tbody></tbody>
        </table>
        <div style="margin-top:10px"><button onclick="addSettingRow()">+ Add Setting</button></div>
      </div>
    </div>
  </div>

  <!-- TAB 3: PNACH -->
  <div id="tab-pnach" class="tab-pane">
    <div class="card">
      <h3>Layer 2 Memory &amp; Code Patches (.pnach)</h3>
      <table id="pnachTable">
        <thead><tr><th>On</th><th>Boot</th><th>Hex Address</th><th>Width</th><th>Hex Value</th><th>Comment</th><th></th></tr></thead>
        <tbody></tbody>
      </table>
      <div class="row" style="margin-top:10px">
        <button onclick="addPnachRow()">+ Add Patch Row</button>
        <button onclick="rebuildPnachText()">Sync Table -> Raw .pnach</button>
      </div>
      <textarea id="pnachEditor" class="code-box" style="min-height:200px;margin-top:10px" placeholder="// Raw .pnach patches (optional)"></textarea>
    </div>
  </div>

  <!-- TAB 4: ASSETS & LAYERS -->
  <div id="tab-assets" class="tab-pane">
    <div class="card">
      <h3>Unified Multi-Layer Mod Folders</h3>
      <p style="color:var(--dim);margin-top:0">Every <code>.nocturne-mod</code> automatically bundles all 4 layers in one folder. Click <b>Save &amp; Install</b> with "Create All Layer Folders" checked to scaffold them on disk.</p>
      <div class="row">
        <label><input id="includeLayers" type="checkbox" checked> Create all layer subfolders (<code>files/</code>, <code>textures/</code>, <code>video/</code>, <code>patches/</code>, <code>plugins/</code>)</label>
        <button onclick="openModFolder()">Open Mod Folder in Explorer</button>
      </div>
      <div id="assetSummary" style="margin-top:10px"></div>
    </div>
  </div>

  <!-- TAB 5: RAM MAP & C++ SDK -->
  <div id="tab-sdk" class="tab-pane">
    <div class="grid2">
      <div class="card">
        <h3>Verified RAM Map (<span id="ramGameLabel"></span>)</h3>
        <input id="ramFilter" type="text" placeholder="Filter addresses, fields, notes..." style="width:100%;margin-bottom:8px" oninput="renderRamMap()">
        <table>
          <thead><tr><th>Address</th><th>Type</th><th>Field</th><th>Notes</th></tr></thead>
          <tbody id="ramTableBody"></tbody>
        </table>
      </div>
      <div class="card">
        <h3>Layer 4 Native C++ (.dll) Plugin SDK</h3>
        <p style="color:var(--dim);margin-top:0">For advanced engine/memory mods requiring native C++20 speed, generate a ready-to-compile <code>NocturneModApiV1</code> plugin project linked to <code>sdk/nocturne_mod_api.h</code>.</p>
        <p style="color:var(--warn);font-size:12px">Safety Note: Players must explicitly set <code>allow_native_plugins = 1</code> in <code>keys.ini</code> to load Layer 4 <code>.dll</code> plugins.</p>
        <button class="gold" onclick="doGenCppSdk()">Generate Layer 4 C++ Plugin Project</button>
        <div id="sdkResult" style="margin-top:10px;font-size:12px;"></div>
      </div>
    </div>
  </div>

  <!-- TAB 6: CONFLICTS & DEV TOOLS -->
  <div id="tab-test" class="tab-pane">
    <div class="grid2">
      <div class="card">
        <h3>Live Developer Workflow (--mod-dev)</h3>
        <ul style="color:var(--dim);line-height:1.7;padding-left:18px;margin-top:0">
          <li><b>Auto Hot-Reload:</b> Saving any <code>.lua</code>, <code>.pnach</code>, or <code>mod_config.ini</code> reloads automatically within 0.5s!</li>
          <li><b>F9 Key:</b> Instant manual reload of all active mods.</li>
          <li><b>F10 Key:</b> Toggle the in-game <b>Live Modder Inspector &amp; Lua Log Console</b> (shows live Cash, Party HP/MP/Lv, Ring stats, Conflicts, and <code>log()</code> output).</li>
          <li>Regular players never see dev tools unless <code>--mod-dev</code> or <code>mod_dev_mode = 1</code> is set.</li>
        </ul>
        <button class="primary" onclick="doLaunchDev()">Launch Active Game in --mod-dev Mode</button>
      </div>
      <div class="card">
        <h3>Cross-Mod Conflict Checker</h3>
        <button onclick="refreshConflicts()" style="margin-bottom:10px">Scan Active Game Mods for Conflicts</button>
        <div id="conflictList" style="color:var(--dim)">Click Scan to check overlapping files, textures, videos, and .pnach addresses.</div>
      </div>
    </div>
  </div>
</main>

<div class="status-bar">
  <span id="statusText">Ready — Nocturne Mod Studio v2</span>
  <span id="installDirLabel"></span>
</div>

<script>
let STATE = { games: [], presets: {}, ram_map: {}, mods: [], settings: [], pnach_rows: [] };

async function api(path, body) {
  const opts = body !== undefined ? { method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify(body) } : {};
  const r = await fetch(path, opts);
  return await r.json();
}
function setStatus(msg, cls="") {
  const el = document.getElementById("statusText");
  el.textContent = msg;
  el.className = cls;
}
function showTab(name, btn) {
  document.querySelectorAll(".tab-pane").forEach(p => p.classList.remove("active"));
  document.querySelectorAll("nav.tabs button").forEach(b => b.classList.remove("active"));
  document.getElementById("tab-" + name).classList.add("active");
  btn.classList.add("active");
  if (name === "test") refreshConflicts();
}

function getPayload() {
  return {
    game: document.getElementById("gameSelect").value,
    meta: {
      name: document.getElementById("m_name").value,
      folder: document.getElementById("m_folder").value,
      author: document.getElementById("m_author").value,
      version: document.getElementById("m_version").value,
      description: document.getElementById("m_desc").value,
      game: document.getElementById("gameSelect").value,
    },
    rules: {
      start_cash: +document.getElementById("r_start_cash").value || 0,
      win_cash: +document.getElementById("r_win_cash").value || 0,
      win_exp: +document.getElementById("r_win_exp").value || 0,
      win_heal: +document.getElementById("r_win_heal").value || 0,
      bench_exp: document.getElementById("r_bench_exp").checked,
      unlock_party: document.getElementById("r_unlock_party").checked,
      perf_cash: +document.getElementById("r_perf_cash").value || 0,
      perf_streak_scale: document.getElementById("r_perf_scale").checked,
      miss_penalty: +document.getElementById("r_miss_penalty").value || 0,
      miss_alert: document.getElementById("r_miss_alert").checked,
      welcome_toast: document.getElementById("r_welcome_toast").value,
      win_toast: document.getElementById("r_win_toast").value,
      perf_toast: document.getElementById("r_perf_toast").value,
    },
    settings: collectSettings(),
    lua_code: document.getElementById("luaEditor").value,
    pnach_code: document.getElementById("pnachEditor").value,
    include_layers: document.getElementById("includeLayers").checked,
  };
}

function onNameEdited() {
  if (!document.getElementById("existingSelect").value) {
    const clean = (document.getElementById("m_name").value || "").replace(/[^A-Za-z0-9]+/g, "");
    if (clean) document.getElementById("m_folder").value = clean;
  }
  syncLua();
}
async function doCopyLua() {
  try {
    await navigator.clipboard.writeText(document.getElementById("luaEditor").value || "");
    setStatus("Copied Lua script to clipboard", "ok");
  } catch (e) {
    setStatus("Clipboard copy unavailable", "warn");
  }
}
document.addEventListener("keydown", (ev) => {
  if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === "s") {
    ev.preventDefault();
    doInstall();
  }
});

async function syncLua() {
  if (document.getElementById("customCodeToggle").checked) return;
  const p = getPayload();
  const res = await api("/api/generate_lua", p);
  document.getElementById("luaPreview").value = res.lua_code || "";
  document.getElementById("luaEditor").value = res.lua_code || "";
}
function onLuaEdited() {
  document.getElementById("customCodeToggle").checked = true;
  document.getElementById("luaPreview").value = document.getElementById("luaEditor").value;
}

function insertSnippet(kind) {
  const snips = {
    on_hotkey: '\nfunction on_hotkey(key)\n    if key == "F5" and is_in_game() then\n        toast("F5 Hotkey Triggered!")\n    end\nend\n',
    hud_set: '\nhud_set("counter", "CUSTOM COUNTER", "42", "#E6C88A")\n',
    save: '\nlocal count = save_get("my_count", 0) + 1\nsave_set("my_count", count)\n',
    setting: '\nlocal mult = get_setting("my_mult", 2)\n',
    hero: '\nfor i = 0, get_hero_count() - 1 do\n    if get_hero_joined(i) then\n        set_hero_hp(i, get_hero_max_hp(i))\n    end\nend\n',
    battle: '\nfunction on_battle_win(total_wins)\n    log("Battle #" .. total_wins .. " won!")\nend\n',
  };
  const ed = document.getElementById("luaEditor");
  ed.value += snips[kind] || "";
  onLuaEdited();
}

async function doValidateLua() {
  const res = await api("/api/validate_lua", { lua_code: document.getElementById("luaEditor").value });
  const el = document.getElementById("luaValResult");
  if (res.ok) { el.innerHTML = '<span class="ok">Syntax OK — ready to install!</span>'; }
  else { el.innerHTML = '<span class="err">' + (res.errors || []).join("<br>") + '</span>'; }
}

function addSettingRow(s={key:"",label:"",type:"int",default:1,min:0,max:100,step:1}) {
  const tb = document.querySelector("#settingsTable tbody");
  const tr = document.createElement("tr");
  tr.innerHTML = `<td><input value="${s.key||""}" style="width:90px"></td>
    <td><input value="${s.label||""}" style="width:120px"></td>
    <td><select><option ${s.type==="int"?"selected":""}>int</option><option ${s.type==="bool"?"selected":""}>bool</option><option ${s.type==="float"?"selected":""}>float</option></select></td>
    <td><input type="number" value="${s.default??1}" style="width:60px"></td>
    <td><input type="number" value="${s.min??0}" style="width:60px"></td>
    <td><input type="number" value="${s.max??100}" style="width:60px"></td>
    <td><button onclick="this.closest('tr').remove()">x</button></td>`;
  tb.appendChild(tr);
}
function collectSettings() {
  const out = [];
  document.querySelectorAll("#settingsTable tbody tr").forEach(tr => {
    const inp = tr.querySelectorAll("input,select");
    const key = inp[0].value.trim();
    if (!key) return;
    out.push({ key, label: inp[1].value.trim() || key, type: inp[2].value, default: +inp[3].value||0, min: +inp[4].value||0, max: +inp[5].value||100, step: 1 });
  });
  return out;
}

function addPnachRow(r={enabled:true,boot_only:false,addr:"00100008",width:"word",value:"00002710",comment:"Example"}) {
  const tb = document.querySelector("#pnachTable tbody");
  const tr = document.createElement("tr");
  tr.innerHTML = `<td><input type="checkbox" ${r.enabled!==false?"checked":""}></td>
    <td><input type="checkbox" ${r.boot_only?"checked":""}></td>
    <td><input value="${r.addr||""}" class="mono" style="width:100px"></td>
    <td><select><option>word</option><option>short</option><option>byte</option><option>extended</option></select></td>
    <td><input value="${r.value||""}" class="mono" style="width:100px"></td>
    <td><input value="${r.comment||""}" style="width:160px"></td>
    <td><button onclick="this.closest('tr').remove()">x</button></td>`;
  tb.appendChild(tr);
}
async function rebuildPnachText() {
  const rows = [];
  document.querySelectorAll("#pnachTable tbody tr").forEach(tr => {
    const c = tr.querySelectorAll("input,select");
    rows.push({ enabled: c[0].checked, boot_only: c[1].checked, addr: c[2].value, width: c[3].value, value: c[4].value, comment: c[5].value });
  });
  const res = await api("/api/generate_pnach", { mod_name: document.getElementById("m_name").value, rows });
  document.getElementById("pnachEditor").value = res.pnach_code || "";
  setStatus("Synced .pnach table to raw patch text", "ok");
}

function applyPreset(key) {
  const pr = STATE.presets[key];
  if (!pr) return;
  document.getElementById("m_name").value = pr.meta.name;
  document.getElementById("m_folder").value = pr.meta.name.replace(/[^A-Za-z0-9]+/g, "");
  document.getElementById("m_desc").value = pr.meta.description;
  const r = pr.rules || {};
  document.getElementById("r_start_cash").value = r.start_cash || 0;
  document.getElementById("r_win_cash").value = r.win_cash || 0;
  document.getElementById("r_win_exp").value = r.win_exp || 0;
  document.getElementById("r_win_heal").value = r.win_heal || 0;
  document.getElementById("r_bench_exp").checked = !!r.bench_exp;
  document.getElementById("r_unlock_party").checked = !!r.unlock_party;
  document.getElementById("r_perf_cash").value = r.perf_cash || 0;
  document.getElementById("r_perf_scale").checked = !!r.perf_streak_scale;
  document.getElementById("r_miss_penalty").value = r.miss_penalty || 0;
  document.getElementById("r_miss_alert").checked = !!r.miss_alert;
  document.getElementById("r_welcome_toast").value = r.welcome_toast || "";
  document.getElementById("r_win_toast").value = r.win_toast || "";
  document.getElementById("r_perf_toast").value = r.perf_toast || "";
  document.querySelector("#settingsTable tbody").innerHTML = "";
  (pr.settings || []).forEach(addSettingRow);
  if (pr.custom_lua) {
    document.getElementById("customCodeToggle").checked = true;
    document.getElementById("luaEditor").value = pr.custom_lua;
    document.getElementById("luaPreview").value = pr.custom_lua;
  } else {
    document.getElementById("customCodeToggle").checked = false;
    syncLua();
  }
  setStatus("Loaded preset: " + pr.label, "ok");
}

function renderRamMap() {
  const slug = document.getElementById("gameSelect").value;
  document.getElementById("ramGameLabel").textContent = slug;
  const q = (document.getElementById("ramFilter").value || "").toLowerCase();
  const rows = (STATE.ram_map[slug] || []).filter(r =>
    !q || (r.addr + " " + r.name + " " + r.notes).toLowerCase().includes(q)
  );
  const tb = document.getElementById("ramTableBody");
  tb.innerHTML = rows.map(r => `<tr><td class="mono">${r.addr}</td><td>${r.type}</td><td><b>${r.name}</b></td><td style="color:var(--dim)">${r.notes}</td></tr>`).join("");
}

async function refreshGameMods() {
  const slug = document.getElementById("gameSelect").value;
  const res = await api("/api/mods?game=" + encodeURIComponent(slug));
  STATE.mods = res.mods || [];
  const sel = document.getElementById("existingSelect");
  sel.innerHTML = '<option value="">+ New Mod</option>' +
    STATE.mods.map(m => `<option value="${m.folder}">${m.name} (${m.folder})</option>`).join("");
  renderRamMap();
}

function loadSelectedMod() {
  const folder = document.getElementById("existingSelect").value;
  const m = STATE.mods.find(x => x.folder === folder);
  if (!m) { applyPreset("blank"); return; }
  document.getElementById("m_name").value = m.name;
  document.getElementById("m_folder").value = m.folder;
  document.getElementById("m_author").value = m.author || "Modder";
  document.getElementById("m_version").value = m.version || "1.0.0";
  document.getElementById("m_desc").value = m.description || "";
  document.getElementById("customCodeToggle").checked = true;
  document.getElementById("luaEditor").value = m.lua_code || "";
  document.getElementById("luaPreview").value = m.lua_code || "";
  document.getElementById("pnachEditor").value = m.pnach_code || "";
  document.querySelector("#settingsTable tbody").innerHTML = "";
  (m.settings || []).forEach(addSettingRow);
  const a = m.assets || {};
  document.getElementById("assetSummary").innerHTML =
    `<b>Layer 1 files/:</b> ${(a.files||[]).length} &nbsp;|&nbsp; <b>textures/:</b> ${(a.textures||[]).length} &nbsp;|&nbsp; <b>video/:</b> ${(a.video||[]).length} &nbsp;|&nbsp; <b>plugins/:</b> ${(a.plugins||[]).length}`;
  setStatus("Loaded existing mod: " + m.name, "ok");
}

async function doInstall() {
  const res = await api("/api/install", getPayload());
  if (res.ok) { setStatus(res.message, "ok"); await refreshGameMods(); }
  else { setStatus(res.error || "Install failed", "err"); }
}
async function doExport() {
  const r = await fetch("/api/export_zip", { method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(getPayload()) });
  const blob = await r.blob();
  const fn = (document.getElementById("m_folder").value || "MyCustomMod") + ".nocturne-mod";
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob); a.download = fn; a.click();
  setStatus("Exported " + fn, "ok");
}
async function doLaunchDev() {
  const res = await api("/api/launch_dev", { game: document.getElementById("gameSelect").value });
  setStatus(res.ok ? res.message : (res.error || "Launch failed"), res.ok ? "ok" : "err");
}
async function doGenCppSdk() {
  const res = await api("/api/gen_cpp_sdk", {
    game: document.getElementById("gameSelect").value,
    folder: document.getElementById("m_folder").value,
    name: document.getElementById("m_name").value,
  });
  document.getElementById("sdkResult").innerHTML = res.ok
    ? `<span class="ok">${res.message}</span>`
    : `<span class="err">${res.error}</span>`;
}
async function openModFolder() {
  const res = await api("/api/open_folder", { game: document.getElementById("gameSelect").value, folder: document.getElementById("m_folder").value });
  setStatus(res.message || res.error || "Opened folder");
}
async function refreshConflicts() {
  const slug = document.getElementById("gameSelect").value;
  const res = await api("/api/conflicts?game=" + encodeURIComponent(slug));
  const list = res.conflicts || [];
  const el = document.getElementById("conflictList");
  if (!list.length) { el.innerHTML = '<span class="ok">No conflicts detected across installed mods!</span>'; return; }
  el.innerHTML = list.map(c => `<div class="warn">[${c.layer}] <code>${c.resource}</code>: <b>${c.winner}</b> overrides <b>${c.shadowed}</b></div>`).join("");
}

async function boot() {
  const cfg = await api("/api/bootstrap");
  STATE.games = cfg.games; STATE.presets = cfg.presets; STATE.ram_map = cfg.ram_map;
  document.getElementById("installDirLabel").textContent = cfg.install_dir;
  const gsel = document.getElementById("gameSelect");
  gsel.innerHTML = cfg.games.map(g => `<option value="${g.slug}">${g.name}</option>`).join("");
  gsel.value = cfg.default_game || "sh1";
  gsel.onchange = refreshGameMods;
  document.getElementById("existingSelect").onchange = loadSelectedMod;
  const pb = document.getElementById("presetBar");
  Object.keys(cfg.presets).forEach(k => {
    const sp = document.createElement("span");
    sp.className = "pill"; sp.textContent = cfg.presets[k].label;
    sp.onclick = () => applyPreset(k);
    pb.appendChild(sp);
  });
  await refreshGameMods();
  await syncLua();
  setInterval(() => fetch("/api/ping").catch(()=>{}), 4000);
}
boot();
</script>
</body>
</html>
"""


def create_server(install_dir: str, default_game: str):
    last_ping = [time.monotonic()]

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _send_json(self, obj: dict, code: int = 200):
            raw = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            if parsed.path == "/":
                raw = HTML_PAGE.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            if parsed.path == "/api/ping":
                last_ping[0] = time.monotonic()
                self._send_json({"ok": True})
                return
            if parsed.path == "/api/bootstrap":
                self._send_json({
                    "install_dir": install_dir,
                    "default_game": default_game,
                    "games": GAMES,
                    "presets": MOD_PRESETS,
                    "ram_map": VERIFIED_RAM_MAP,
                })
                return
            if parsed.path == "/api/mods":
                slug = (qs.get("game") or [default_game])[0]
                self._send_json({"mods": list_game_mods(install_dir, slug)})
                return
            if parsed.path == "/api/conflicts":
                slug = (qs.get("game") or [default_game])[0]
                self._send_json({"conflicts": detect_game_conflicts(install_dir, slug)})
                return
            self.send_error(404)

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            if self.path == "/api/generate_lua":
                self._send_json({"lua_code": generate_lua_from_rules(body.get("meta") or {}, body.get("rules") or {})})
                return
            if self.path == "/api/generate_pnach":
                self._send_json({"pnach_code": generate_pnach_from_rows(body.get("mod_name") or "Mod", body.get("rows") or [])})
                return
            if self.path == "/api/validate_lua":
                ok, errs = validate_lua_syntax(str(body.get("lua_code") or ""))
                self._send_json({"ok": ok, "errors": errs})
                return
            if self.path == "/api/install":
                slug = str(body.get("game") or default_game)
                self._send_json(install_mod_to_game(install_dir, slug, body))
                return
            if self.path == "/api/export_zip":
                fname, data = build_nocturne_mod_zip_bytes(body, install_dir=install_dir)
                self.send_response(200)
                self.send_header("Content-Type", "application/zip")
                self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if self.path == "/api/launch_dev":
                self._send_json(launch_game_dev_mode(install_dir, str(body.get("game") or default_game)))
                return
            if self.path == "/api/gen_cpp_sdk":
                self._send_json(generate_cpp_plugin_scaffold(
                    install_dir, str(body.get("game") or default_game),
                    str(body.get("folder") or "MyCustomMod"), str(body.get("name") or "My Custom Mod"),
                ))
                return
            if self.path == "/api/open_folder":
                slug = str(body.get("game") or default_game)
                folder = sanitize_folder(str(body.get("folder") or "MyCustomMod"))
                mdir = os.path.join(install_dir, "games", slug, "mods", folder)
                os.makedirs(mdir, exist_ok=True)
                if sys.platform == "win32":
                    os.startfile(mdir)
                self._send_json({"ok": True, "message": f"Opened {mdir}"})
                return
            self.send_error(404)

    class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    return srv, last_ping


def _open_app_window(url: str) -> None:
    if sys.platform == "win32":
        candidates = [
            os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
            os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
            os.path.join(os.environ.get("ProgramFiles", ""), "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google", "Chrome", "Application", "chrome.exe"),
        ]
        for exe in candidates:
            if exe and os.path.isfile(exe):
                prof = os.path.join(tempfile.gettempdir(), "nocturne_mod_studio_profile")
                subprocess.Popen([exe, f"--app={url}", f"--user-data-dir={prof}", "--window-size=1280,820"])
                return
    webbrowser.open(url)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Nocturne — Mod Studio v2 (Developer Workbench)")
    ap.add_argument("--game", default="sh1", choices=sorted(VALID_SLUGS), help="Initial game slug")
    ap.add_argument("--install-dir", default=REPO_ROOT, help="Nocturne root / install directory")
    ap.add_argument("--no-browser", action="store_true", help="Run server without opening a window")
    args = ap.parse_args(argv)

    srv, last_ping = create_server(os.path.abspath(args.install_dir), args.game)
    port = srv.server_address[1]
    url = f"http://127.0.0.1:{port}/"
    print(f"[Nocturne Mod Studio v2] Serving on {url} (install_dir={args.install_dir})")

    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    if not args.no_browser:
        _open_app_window(url)

    try:
        while True:
            time.sleep(2.0)
            if not args.no_browser and (time.monotonic() - last_ping[0]) > 45.0:
                break
    except KeyboardInterrupt:
        pass
    srv.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
