#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Standalone Mod Studio (Desktop App).

Hybrid no-code + Lua script creator for Koudelka (PS1), Shadow Hearts (1913),
Shadow Hearts: Covenant (1915), and Shadow Hearts: From the New World (1929).
Zero external dependencies (Python 3 stdlib only); opens in a dedicated
Edge/Chrome --app desktop window and connects seamlessly to the Nocturne Launcher.
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
    {"slug": "sh1", "name": "Shadow Hearts (1913)"},
    {"slug": "sh2-covenant", "name": "Shadow Hearts: Covenant (1915)"},
    {"slug": "sh3-ftnw", "name": "Shadow Hearts: From the New World (1929)"},
    {"slug": "koudelka", "name": "Koudelka (1898, PS1)"},
]
VALID_SLUGS = {g["slug"] for g in GAMES}


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
        "-- Generated with Nocturne Mod Studio (Layer 3 Semantic Lua API)",
        "-- Feel free to customize this script directly in Custom Code Mode!",
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

    # on_battle_win
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

    # on_ring_perfect
    if perf_cash > 0 or perf_toast:
        lines += ["", "function on_ring_perfect(streak)"]
        if perf_cash > 0:
            if perf_streak_scale:
                lines.append(f"    local reward = {perf_cash} * streak")
            else:
                lines.append(f"    local reward = {perf_cash}")
            lines.append("    add_cash(reward)")
            if perf_toast:
                lines.append(f'    toast("{_lua_escape(perf_toast)} (x" .. streak .. " | +" .. reward .. " Cash)")')
            else:
                lines.append('    toast("PERFECT RING x" .. streak .. "!  +" .. reward .. " Cash")')
        elif perf_toast:
            lines.append(f'    toast("{_lua_escape(perf_toast)} (x" .. streak .. ")")')
        lines.append("end")

    # on_ring_miss
    if miss_penalty > 0 or miss_alert:
        lines += ["", "function on_ring_miss(prev_streak)"]
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
        meta = {"folder": entry, "id": entry.lower(), "name": entry, "version": "1.0.0", "author": "Community", "description": ""}
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
        meta["lua_code"] = lua_code
        out.append(meta)
    return out


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
    lua_code = str(payload.get("lua_code") or "")
    if not lua_code.strip():
        lua_code = generate_lua_from_rules(meta, rules)

    ok, errs = validate_lua_syntax(lua_code)
    if not ok:
        return {"ok": False, "error": "Lua syntax error: " + "; ".join(errs), "errors": errs}

    folder = sanitize_folder(str(meta.get("folder") or meta.get("name") or "MyCustomMod"))
    mdir = os.path.join(install_dir, "games", game_slug, "mods", folder)
    os.makedirs(os.path.join(mdir, "scripts"), exist_ok=True)
    if payload.get("include_layers"):
        os.makedirs(os.path.join(mdir, "files"), exist_ok=True)
        os.makedirs(os.path.join(mdir, "patches"), exist_ok=True)

    manifest = {
        "id": folder.lower(),
        "name": str(meta.get("name") or folder).strip(),
        "version": str(meta.get("version") or "1.0.0").strip(),
        "author": str(meta.get("author") or "Community").strip(),
        "game": game_slug,
        "description": str(meta.get("description") or "Created with Nocturne Mod Studio.").strip(),
        "studio_rules": rules,
    }
    with open(os.path.join(mdir, "mod.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    script_name = sanitize_folder(folder).lower() + ".lua"
    with open(os.path.join(mdir, "scripts", script_name), "w", encoding="utf-8") as f:
        f.write(lua_code)

    _enable_mod_in_keys_ini(install_dir, game_slug, folder)
    return {"ok": True, "folder": folder, "path": mdir, "message": f"Installed & enabled '{manifest['name']}' in games/{game_slug}/mods/{folder}/"}


def build_nocturne_mod_zip_bytes(payload: dict) -> tuple[str, bytes]:
    meta = payload.get("meta") or {}
    rules = payload.get("rules") or {}
    game_slug = str(payload.get("game") or meta.get("game") or "sh1")
    folder = sanitize_folder(str(meta.get("folder") or meta.get("name") or "MyCustomMod"))
    lua_code = str(payload.get("lua_code") or "") or generate_lua_from_rules(meta, rules)

    manifest = {
        "id": folder.lower(),
        "name": str(meta.get("name") or folder).strip(),
        "version": str(meta.get("version") or "1.0.0").strip(),
        "author": str(meta.get("author") or "Community").strip(),
        "game": game_slug,
        "description": str(meta.get("description") or "Created with Nocturne Mod Studio.").strip(),
        "studio_rules": rules,
    }

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(f"{folder}/mod.json", json.dumps(manifest, indent=2) + "\n")
        zf.writestr(f"{folder}/scripts/{folder.lower()}.lua", lua_code)
        if payload.get("include_layers"):
            zf.writestr(f"{folder}/patches/README.txt", "Drop .pnach Layer 2 memory patch files here.\n")
            zf.writestr(f"{folder}/files/README.txt", "Drop Layer 1 virtual disc override files here.\n")
    return f"{folder}.nocturne-mod", buf.getvalue()


STUDIO_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>Nocturne Mod Studio — Shadow Hearts: Nocturne</title>
<style>
  :root {
    --bg: #0c0a12; --panel: #151120; --card: #1c162b; --border: #3a2d4a;
    --gold: #e2b86b; --crimson: #a82c42; --crimson-hi: #c93b55;
    --text: #eee7f2; --dim: #9c92ac; --good: #58d69a; --warn: #f0a854; --bad: #f06070;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: radial-gradient(circle at top, #1f1428 0%, var(--bg) 62%); color: var(--text);
         font-family: "Segoe UI", system-ui, sans-serif; height: 100vh; display: flex; flex-direction: column; overflow: hidden; }
  header { display: flex; align-items: center; gap: 12px; padding: 12px 18px; background: rgba(16,11,24,0.95);
           border-bottom: 1px solid var(--border); flex-wrap: wrap; }
  .brand { font-family: Georgia, serif; font-size: 18px; font-weight: 700; color: var(--gold); letter-spacing: 0.6px; margin-right: 8px; }
  select, input[type=text], input[type=number] {
    background: #100c19; color: var(--text); border: 1px solid var(--border); border-radius: 6px; padding: 6px 9px; font-size: 13px;
  }
  select:focus, input:focus, textarea:focus { outline: none; border-color: var(--gold); }
  button { background: var(--card); color: var(--text); border: 1px solid var(--border); border-radius: 7px;
           padding: 7px 13px; font-size: 13px; font-weight: 600; cursor: pointer; transition: 0.15s; }
  button:hover { border-color: var(--gold); background: #261d38; }
  button.primary { background: linear-gradient(135deg, var(--crimson), #7a1f32); border-color: var(--gold); color: #fff7e6; }
  button.primary:hover { background: linear-gradient(135deg, var(--crimson-hi), #96263d); }
  button.good { background: #184a36; border-color: var(--good); color: #e4fff2; }
  .spacer { flex: 1; }
  main { flex: 1; display: grid; grid-template-columns: 460px 1fr; gap: 14px; padding: 14px 18px; overflow: hidden; }
  .col { display: flex; flex-direction: column; gap: 12px; overflow-y: auto; padding-right: 4px; }
  .card { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 14px; }
  .card h3 { margin: 0 0 10px; font-size: 12px; text-transform: uppercase; letter-spacing: 1px; color: var(--gold); }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  label.fld { display: flex; flex-direction: column; gap: 4px; font-size: 12px; color: var(--dim); margin-bottom: 8px; }
  .row { display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 6px 0; border-bottom: 1px solid rgba(255,255,255,0.05); }
  .row:last-child { border-bottom: none; }
  .row span { font-size: 13px; }
  .row small { display: block; font-size: 11px; color: var(--dim); }
  input[type=range] { width: 135px; accent-color: var(--gold); }
  input[type=checkbox] { width: 17px; height: 17px; accent-color: var(--crimson); }
  .val { min-width: 62px; text-align: right; font-weight: 700; color: var(--gold); font-size: 13px; }
  .editor-wrap { flex: 1; display: flex; flex-direction: column; min-height: 0; }
  .editor-bar { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
  textarea#code { flex: 1; width: 100%; background: #0c0914; color: #f2e8dc; border: 1px solid var(--border);
                  border-radius: 8px; padding: 12px; font-family: Consolas, "Cascadia Code", monospace; font-size: 13px;
                  line-height: 1.48; resize: none; tab-size: 4; }
  #status { padding: 9px 14px; border-radius: 7px; font-size: 13px; background: #141022; border: 1px solid var(--border); min-height: 38px; display: flex; align-items: center; }
  #status.ok { border-color: var(--good); color: var(--good); }
  #status.err { border-color: var(--bad); color: var(--bad); }
  details.api { background: #120e1c; border: 1px solid var(--border); border-radius: 8px; padding: 8px 12px; font-size: 12px; }
  details.api summary { cursor: pointer; color: var(--gold); font-weight: 600; }
  details.api code { color: #f5d18c; }
  .pill { font-size: 11px; padding: 2px 8px; border-radius: 99px; background: #261d38; color: var(--gold); border: 1px solid var(--border); }
</style>
</head>
<body>
<header>
  <div class="brand">NOCTURNE MOD STUDIO</div>
  <select id="gameSel" title="Target Game"></select>
  <select id="loadModSel" title="Load Existing Mod"><option value="">+ New Mod...</option></select>
  <button id="btnValidate">Validate Lua</button>
  <div class="spacer"></div>
  <button id="btnExport">Export .nocturne-mod</button>
  <button id="btnInstall" class="primary">Save &amp; Install to Game</button>
</header>

<main>
  <div class="col">
    <div class="card">
      <h3>1. Mod Identity</h3>
      <div class="grid2">
        <label class="fld">Mod Name<input id="mName" type="text" value="My Custom Mod" /></label>
        <label class="fld">Folder ID<input id="mFolder" type="text" value="MyCustomMod" /></label>
      </div>
      <div class="grid2">
        <label class="fld">Author<input id="mAuthor" type="text" value="Community" /></label>
        <label class="fld">Version<input id="mVersion" type="text" value="1.0.0" /></label>
      </div>
      <label class="fld">Description<input id="mDesc" type="text" value="Custom Layer 3 gameplay mod created with Nocturne Mod Studio." /></label>
      <div class="row">
        <div><span>Include Layer 1/2 folders (files/ &amp; patches/)</span><small>Also creates folders for disc overrides &amp; .pnach patches</small></div>
        <input id="mLayers" type="checkbox" />
      </div>
    </div>

    <div class="card">
      <h3>2. Economy (Cash / Gold)</h3>
      <div class="row">
        <div><span>Starting Cash Boost</span><small>Granted once when gameplay loads</small></div>
        <input id="rStartCash" type="range" min="0" max="50000" step="500" value="0" />
        <span class="val" id="vStartCash">0</span>
      </div>
      <div class="row">
        <div><span>Bonus Cash on Battle Win</span><small>Awarded after every victory</small></div>
        <input id="rWinCash" type="range" min="0" max="5000" step="100" value="250" />
        <span class="val" id="vWinCash">+250</span>
      </div>
      <div class="row">
        <div><span>Bonus Cash on Perfect Ring</span><small>Awarded on every Perfect Strike</small></div>
        <input id="rPerfCash" type="range" min="0" max="2000" step="50" value="100" />
        <span class="val" id="vPerfCash">+100</span>
      </div>
    </div>

    <div class="card">
      <h3>3. Party &amp; Progression</h3>
      <div class="row">
        <div><span>Post-Battle Auto-Heal</span><small>Restores % of max HP after battle wins</small></div>
        <input id="rWinHeal" type="range" min="0" max="100" step="5" value="20" />
        <span class="val" id="vWinHeal">20%</span>
      </div>
      <div class="row">
        <div><span>Bonus Party EXP on Victory</span><small>Extra EXP for joined heroes (SH1 / Koudelka)</small></div>
        <input id="rWinExp" type="range" min="0" max="2000" step="50" value="0" />
        <span class="val" id="vWinExp">0</span>
      </div>
      <div class="row">
        <div><span>Live Bench EXP Share (SH1)</span><small>Keeps benched party members synced after battles</small></div>
        <input id="rBenchExp" type="checkbox" />
      </div>
      <div class="row">
        <div><span>Unlock All 6 Heroes Early (SH1)</span><small>Awakens Alice, Zhuzhen, Margarete, Keith &amp; Halley</small></div>
        <input id="rUnlockParty" type="checkbox" />
      </div>
    </div>

    <div class="card">
      <h3>4. Judgment Ring &amp; Custom Toasts</h3>
      <div class="row">
        <div><span>Scale Perfect Reward by Streak</span><small>Multiplies Perfect Cash by consecutive Perfect count</small></div>
        <input id="rPerfScale" type="checkbox" checked />
      </div>
      <div class="row">
        <div><span>Ring Miss Cash Penalty</span><small>High-stakes challenge: lose Cash on a missed Ring</small></div>
        <input id="rMissPen" type="range" min="0" max="1000" step="50" value="0" />
        <span class="val" id="vMissPen">0</span>
      </div>
      <div class="row">
        <div><span>Streak Shattered Alert</span><small>Shows dramatic Gothic callout when a streak breaks</small></div>
        <input id="rMissAlert" type="checkbox" checked />
      </div>
      <label class="fld">Welcome Toast (on game load)<input id="rWelcomeToast" type="text" placeholder="e.g. Blood Moon Mode Active!" /></label>
      <label class="fld">Custom Victory Toast<input id="rWinToast" type="text" placeholder="Leave blank for automatic reward summary" /></label>
      <label class="fld">Custom Perfect Strike Toast<input id="rPerfToast" type="text" placeholder="Leave blank for PERFECT RING xN!" /></label>
    </div>
  </div>

  <div class="col">
    <div class="card editor-wrap">
      <div class="editor-bar">
        <h3 style="margin:0;">Layer 3 Lua Script</h3>
        <span class="pill" id="modePill">Auto-Generated</span>
        <div class="spacer"></div>
        <label style="font-size:12px;color:var(--dim);display:flex;align-items:center;gap:6px;cursor:pointer;">
          <input id="customMode" type="checkbox" /> Custom Code Mode (unlock manual editing)
        </label>
      </div>
      <textarea id="code" spellcheck="false" readonly></textarea>
    </div>

    <details class="api">
      <summary>Semantic Layer 3 Lua API Reference (Zero Hex Addresses Required!)</summary>
      <div style="margin-top:8px;line-height:1.65;color:var(--dim);">
        <b>Automatic Hooks:</b> <code>on_boot()</code>, <code>on_game_ready()</code>, <code>on_frame()</code>, <code>on_battle_win(total)</code>, <code>on_ring_perfect(streak)</code>, <code>on_ring_hit(streak)</code>, <code>on_ring_miss(prev_streak)</code><br/>
        <b>Game Helpers:</b> <code>is_in_game()</code>, <code>get_cash()</code>, <code>add_cash(n)</code>, <code>heal_party(pct)</code>, <code>add_party_exp(n)</code>, <code>sync_bench_exp()</code>, <code>unlock_all_party()</code>, <code>get_max_level()</code>, <code>get_battles_won()</code>, <code>toast("msg")</code><br/>
        <b>Raw Memory &amp; Math:</b> <code>read_u8/u16/u32/s32/f32(addr)</code>, <code>write_u8/u16/u32/f32(addr, val)</code>, <code>bit_and/or/xor</code>, <code>min/max/clamp/floor/randint(lo,hi)</code>, <code>frame()</code>, <code>game_slug()</code>
      </div>
    </details>

    <div id="status">Ready. Adjust visual rules on the left or switch to Custom Code Mode, then click Save &amp; Install to Game.</div>
  </div>
</main>

<script>
const $ = id => document.getElementById(id);
let loadedMods = [];
let manualFolder = false;

function setStatus(msg, cls = "") {
  const el = $("status");
  el.textContent = msg;
  el.className = cls;
}

function collectPayload() {
  return {
    game: $("gameSel").value,
    include_layers: $("mLayers").checked,
    meta: {
      name: $("mName").value,
      folder: $("mFolder").value,
      author: $("mAuthor").value,
      version: $("mVersion").value,
      description: $("mDesc").value,
      game: $("gameSel").value,
    },
    rules: {
      start_cash: +$("rStartCash").value,
      win_cash: +$("rWinCash").value,
      perf_cash: +$("rPerfCash").value,
      win_heal: +$("rWinHeal").value,
      win_exp: +$("rWinExp").value,
      bench_exp: $("rBenchExp").checked,
      unlock_party: $("rUnlockParty").checked,
      perf_streak_scale: $("rPerfScale").checked,
      miss_penalty: +$("rMissPen").value,
      miss_alert: $("rMissAlert").checked,
      welcome_toast: $("rWelcomeToast").value,
      win_toast: $("rWinToast").value,
      perf_toast: $("rPerfToast").value,
    },
    lua_code: $("code").value,
  };
}

function updateLabels() {
  $("vStartCash").textContent = +$("rStartCash").value ? "+" + $("rStartCash").value : "0";
  $("vWinCash").textContent = +$("rWinCash").value ? "+" + $("rWinCash").value : "0";
  $("vPerfCash").textContent = +$("rPerfCash").value ? "+" + $("rPerfCash").value : "0";
  $("vWinHeal").textContent = $("rWinHeal").value + "%";
  $("vWinExp").textContent = +$("rWinExp").value ? "+" + $("rWinExp").value : "0";
  $("vMissPen").textContent = +$("rMissPen").value ? "-" + $("rMissPen").value : "0";
}

async function regenLua() {
  updateLabels();
  if ($("customMode").checked) return;
  const p = collectPayload();
  const res = await fetch("/api/generate", { method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify(p) });
  const data = await res.json();
  $("code").value = data.lua_code || "";
}

async function refreshModsList() {
  const slug = $("gameSel").value;
  const res = await fetch("/api/mods?game=" + encodeURIComponent(slug));
  const data = await res.json();
  loadedMods = data.mods || [];
  const sel = $("loadModSel");
  sel.innerHTML = '<option value="">+ New Mod...</option>';
  for (const m of loadedMods) {
    const opt = document.createElement("option");
    opt.value = m.folder;
    opt.textContent = "Edit: " + m.name + " (" + m.folder + ")";
    sel.appendChild(opt);
  }
}

$("mName").addEventListener("input", () => {
  if (!manualFolder) $("mFolder").value = $("mName").value.replace(/[^A-Za-z0-9_\-]+/g, "_").replace(/^_+|_+$/g, "") || "MyCustomMod";
  regenLua();
});
$("mFolder").addEventListener("input", () => { manualFolder = true; });
$("customMode").addEventListener("change", () => {
  const custom = $("customMode").checked;
  $("code").readOnly = !custom;
  $("modePill").textContent = custom ? "Custom Code" : "Auto-Generated";
  if (!custom) regenLua();
});

for (const id of ["mAuthor","mVersion","mDesc","rStartCash","rWinCash","rPerfCash","rWinHeal","rWinExp","rBenchExp","rUnlockParty","rPerfScale","rMissPen","rMissAlert","rWelcomeToast","rWinToast","rPerfToast"]) {
  $(id).addEventListener("input", regenLua);
  $(id).addEventListener("change", regenLua);
}

$("gameSel").addEventListener("change", async () => { await refreshModsList(); await regenLua(); });

$("loadModSel").addEventListener("change", () => {
  const folder = $("loadModSel").value;
  if (!folder) {
    manualFolder = false;
    $("mName").value = "My Custom Mod"; $("mFolder").value = "MyCustomMod";
    $("customMode").checked = false; $("code").readOnly = true; $("modePill").textContent = "Auto-Generated";
    regenLua(); return;
  }
  const m = loadedMods.find(x => x.folder === folder);
  if (!m) return;
  manualFolder = true;
  $("mName").value = m.name || m.folder;
  $("mFolder").value = m.folder;
  $("mAuthor").value = m.author || "Community";
  $("mVersion").value = m.version || "1.0.0";
  $("mDesc").value = m.description || "";
  if (m.studio_rules) {
    const r = m.studio_rules;
    $("rStartCash").value = r.start_cash || 0;
    $("rWinCash").value = r.win_cash || 0;
    $("rPerfCash").value = r.perf_cash || 0;
    $("rWinHeal").value = r.win_heal || 0;
    $("rWinExp").value = r.win_exp || 0;
    $("rBenchExp").checked = !!r.bench_exp;
    $("rUnlockParty").checked = !!r.unlock_party;
    $("rPerfScale").checked = r.perf_streak_scale !== false;
    $("rMissPen").value = r.miss_penalty || 0;
    $("rMissAlert").checked = r.miss_alert !== false;
    $("rWelcomeToast").value = r.welcome_toast || "";
    $("rWinToast").value = r.win_toast || "";
    $("rPerfToast").value = r.perf_toast || "";
    updateLabels();
  }
  if (m.lua_code) {
    $("customMode").checked = true;
    $("code").readOnly = false;
    $("modePill").textContent = "Custom Code";
    $("code").value = m.lua_code;
  }
  setStatus("Loaded existing mod: " + m.name, "ok");
});

$("btnValidate").addEventListener("click", async () => {
  const res = await fetch("/api/validate", { method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify({ lua_code: $("code").value }) });
  const data = await res.json();
  if (data.ok) setStatus("Lua syntax valid! Ready to install or export.", "ok");
  else setStatus("Syntax error: " + (data.errors || []).join(" | "), "err");
});

$("btnInstall").addEventListener("click", async () => {
  const res = await fetch("/api/install", { method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify(collectPayload()) });
  const data = await res.json();
  if (data.ok) {
    setStatus(data.message + " (Toggle live in-game via F1 -> Gameplay Mods!)", "ok");
    await refreshModsList();
  } else {
    setStatus(data.error || "Install failed", "err");
  }
});

$("btnExport").addEventListener("click", async () => {
  const res = await fetch("/api/export", { method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify(collectPayload()) });
  if (!res.ok) { setStatus("Export failed (check Lua syntax first).", "err"); return; }
  const blob = await res.blob();
  const cd = res.headers.get("Content-Disposition") || "";
  const m = cd.match(/filename="([^"]+)"/);
  const fname = m ? m[1] : "MyCustomMod.nocturne-mod";
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url; a.download = fname; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
  setStatus("Exported shareable archive: " + fname, "ok");
});

async function init() {
  const res = await fetch("/api/info");
  const info = await res.json();
  const sel = $("gameSel");
  for (const g of info.games) {
    const opt = document.createElement("option");
    opt.value = g.slug; opt.textContent = g.name;
    sel.appendChild(opt);
  }
  const params = new URLSearchParams(location.search);
  const gparam = params.get("game") || info.default_game || "sh1";
  sel.value = gparam;
  await refreshModsList();
  await regenLua();
  setInterval(() => fetch("/api/heartbeat").catch(() => {}), 3000);
  window.addEventListener("beforeunload", () => navigator.sendBeacon("/api/shutdown"));
}
init();
</script>
</body>
</html>
"""


def _launch_app_window(url: str) -> None:
    if os.name == "nt":
        candidates = [
            os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
            os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
            os.path.join(os.environ.get("ProgramFiles", ""), "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(os.environ.get("LocalAppData", ""), "Google", "Chrome", "Application", "chrome.exe"),
        ]
        profile_dir = os.path.join(tempfile.gettempdir(), "nocturne_mod_studio_profile")
        for exe in candidates:
            if exe and os.path.isfile(exe):
                try:
                    subprocess.Popen(
                        [exe, f"--app={url}", "--window-size=1280,820", f"--user-data-dir={profile_dir}"],
                        close_fds=True,
                    )
                    return
                except OSError:
                    pass
    webbrowser.open(url)


def run_studio_server(install_dir: str, default_game: str, open_window: bool = True) -> int:
    last_ping = [time.monotonic()]
    shutdown_ev = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, format: str, *args) -> None:  # noqa: A002
            return

        def _send_json(self, obj: dict, status: int = 200) -> None:
            raw = json.dumps(obj).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def _read_json(self) -> dict:
            ln = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(ln) if ln > 0 else b"{}"
            return json.loads(body.decode("utf-8", errors="replace"))

        def do_GET(self) -> None:
            last_ping[0] = time.monotonic()
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path in ("/", "/index.html"):
                raw = STUDIO_HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            if parsed.path == "/api/info":
                self._send_json({"games": GAMES, "default_game": default_game, "install_dir": install_dir})
                return
            if parsed.path == "/api/mods":
                qs = urllib.parse.parse_qs(parsed.query)
                slug = (qs.get("game") or [default_game])[0]
                self._send_json({"mods": list_game_mods(install_dir, slug)})
                return
            if parsed.path == "/api/heartbeat":
                self._send_json({"ok": True})
                return
            self.send_response(404)
            self.end_headers()

        def do_POST(self) -> None:
            last_ping[0] = time.monotonic()
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path == "/api/shutdown":
                self._send_json({"ok": True})
                shutdown_ev.set()
                return
            try:
                payload = self._read_json()
            except Exception as exc:
                self._send_json({"ok": False, "error": str(exc)}, status=400)
                return
            if parsed.path == "/api/generate":
                code = generate_lua_from_rules(payload.get("meta") or {}, payload.get("rules") or {})
                self._send_json({"ok": True, "lua_code": code})
                return
            if parsed.path == "/api/validate":
                ok, errs = validate_lua_syntax(str(payload.get("lua_code") or ""))
                self._send_json({"ok": ok, "errors": errs})
                return
            if parsed.path == "/api/install":
                slug = str(payload.get("game") or default_game)
                res = install_mod_to_game(install_dir, slug, payload)
                self._send_json(res, status=200 if res.get("ok") else 400)
                return
            if parsed.path == "/api/export":
                lua_code = str(payload.get("lua_code") or "")
                ok, errs = validate_lua_syntax(lua_code)
                if not ok:
                    self._send_json({"ok": False, "errors": errs}, status=400)
                    return
                fname, raw = build_nocturne_mod_zip_bytes(payload)
                self.send_response(200)
                self.send_header("Content-Type", "application/zip")
                self.send_header("Content-Disposition", f'attachment; filename="{fname}"')
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            self.send_response(404)
            self.end_headers()

    class ReusableServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    srv = ReusableServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    url = f"http://127.0.0.1:{port}/?game={urllib.parse.quote(default_game)}"
    print(f"[mod-studio] Running Nocturne Mod Studio at {url}", flush=True)

    t = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.25}, daemon=True)
    t.start()

    if open_window:
        _launch_app_window(url)

    try:
        while not shutdown_ev.is_set():
            time.sleep(0.5)
            if open_window and (time.monotonic() - last_ping[0]) > 25.0:
                break
    except KeyboardInterrupt:
        pass
    finally:
        srv.shutdown()
        srv.server_close()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Nocturne — Standalone Mod Studio")
    ap.add_argument("--game", default="sh1", help="Initial game slug (sh1, sh2-covenant, sh3-ftnw, koudelka)")
    ap.add_argument("--install-dir", default=REPO_ROOT, help="Shadow Hearts: Nocturne root install folder")
    ap.add_argument("--no-browser", action="store_true", help="Do not launch desktop app window (headless server)")
    args = ap.parse_args(argv)
    slug = args.game if args.game in VALID_SLUGS else "sh1"
    return run_studio_server(os.path.abspath(args.install_dir), slug, open_window=not args.no_browser)


if __name__ == "__main__":
    raise SystemExit(main())
