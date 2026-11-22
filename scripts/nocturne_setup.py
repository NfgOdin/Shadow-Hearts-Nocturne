#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Step-by-Step Setup Walkthrough & All-4 Orchestrator.

Collects game selection (including All 4), install folder, disc images (with a smart
multi-game ROMs folder scanner + native Windows browse dialogs), hardware-aware compile
threads, shortcuts, and keep/delete-originals preference BEFORE starting the build.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

sys.path.insert(0, SCRIPT_DIR)
import koudelka_prepare  # noqa: E402
import nocturne_ui as ui  # noqa: E402

GAMES = [
    {
        "id": "koudelka",
        "num": "1",
        "title": "Koudelka (1898, PS1 — 4 Discs)",
        "slug": "koudelka",
        "exe": "Koudelka.exe",
        "script": "koudelka_build.py",
        "discs": 4,
    },
    {
        "id": "sh1",
        "num": "2",
        "title": "Shadow Hearts (1913, PS2)",
        "slug": "sh1",
        "exe": "SH1.exe",
        "script": "sh1_build.py",
        "discs": 1,
    },
    {
        "id": "sh2",
        "num": "3",
        "title": "Shadow Hearts: Covenant (1915, PS2 — 2 Discs)",
        "slug": "sh2-covenant",
        "exe": "SH2C.exe",
        "script": "sh2_build.py",
        "discs": 2,
    },
    {
        "id": "sh3",
        "num": "4",
        "title": "Shadow Hearts: From the New World (1929, PS2)",
        "slug": "sh3-ftnw",
        "exe": "SH3.exe",
        "script": "sh3_build.py",
        "discs": 1,
    },
]

PS2_SERIAL_TO_SLOT = {
    "SLUS_203.47": "sh1",
    "SLUS_210.41": "sh2_d1",
    "SLUS_210.44": "sh2_d2",
    "SLUS_213.26": "sh3",
}


# ---------------------------------------------------------------------------
# Smart Multi-Game Disc Scanner (Header-Verified for ISO/BIN + Fallback for CHD/CSO)
# ---------------------------------------------------------------------------


def _read_iso_sector(f, lba: int, sector_size: int, user_off: int) -> bytes:
    f.seek(lba * sector_size + user_off)
    return f.read(2048)


def probe_ps2_iso_serial(path: str) -> str | None:
    """Fast ISO9660 header probe (~1 ms) to extract BOOT2 serial (e.g. SLUS_210.41) from .iso/.bin."""
    try:
        sz = os.path.getsize(path)
    except OSError:
        return None
    if sz < 20 * 2048:
        return None
    try:
        with open(path, "rb") as f:
            # Try standard 2048-byte ISO first, then 2352-byte Mode 2/Mode 1 raw BIN
            sector_size, user_off = 0, 0
            for ss, uo in ((2048, 0), (2352, 24), (2352, 16)):
                if sz < 20 * ss:
                    continue
                pvd = _read_iso_sector(f, 16, ss, uo)
                if len(pvd) >= 2048 and pvd[0:6] == b"\x01CD001":
                    sector_size, user_off = ss, uo
                    break
            if sector_size == 0:
                return None

            pvd = _read_iso_sector(f, 16, sector_size, user_off)
            root_lba = struct.unpack_from("<I", pvd, 158)[0]
            root_sz = struct.unpack_from("<I", pvd, 166)[0]
            if root_sz == 0 or root_sz > 1024 * 1024:
                return None

            sectors = (root_sz + 2047) // 2048
            buf = bytearray()
            for s in range(min(sectors, 32)):
                buf.extend(_read_iso_sector(f, root_lba + s, sector_size, user_off))

            pos = 0
            while pos + 34 <= len(buf) and pos < root_sz:
                rlen = buf[pos]
                if rlen == 0:
                    pos = ((pos // 2048) + 1) * 2048
                    continue
                elba = struct.unpack_from("<I", buf, pos + 2)[0]
                esize = struct.unpack_from("<I", buf, pos + 10)[0]
                nlen = buf[pos + 32]
                name = buf[pos + 33: pos + 33 + nlen].decode("ascii", "replace").split(";", 1)[0].upper()
                pos += rlen
                if name == "SYSTEM.CNF" and 0 < esize <= 4096:
                    cnf_bytes = _read_iso_sector(f, elba, sector_size, user_off)[:esize]
                    cnf = cnf_bytes.decode("ascii", "replace")
                    m = re.search(r"BOOT2?\s*=\s*cdrom0?:\\?([A-Za-z0-9_.\-]+)", cnf, re.IGNORECASE)
                    if m:
                        return m.group(1).strip().upper()
    except OSError:
        return None
    return None


def _guess_ps2_slot_from_filename(path: str) -> str | None:
    """Fallback identifier for compressed .chd / .cso images based on serial or title."""
    name = os.path.basename(path).lower()
    if "20347" in name or "203.47" in name:
        return "sh1"
    if "21041" in name or "210.41" in name:
        return "sh2_d1"
    if "21044" in name or "210.44" in name:
        return "sh2_d2"
    if "21326" in name or "213.26" in name:
        return "sh3"
    if "shadow hearts" in name:
        if "new world" in name or "ftnw" in name:
            return "sh3"
        if "covenant" in name:
            if re.search(r"(disc|disk|cd)\s*2|\(2\)|_2\b|-2\b", name):
                return "sh2_d2"
            return "sh2_d1"
        return "sh1"
    return None


def _resolve_cue_bin(cue_path: str) -> str | None:
    try:
        with open(cue_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r'FILE\s+"([^"]+)"', line, re.IGNORECASE)
                if m:
                    cand = os.path.join(os.path.dirname(cue_path), m.group(1))
                    if os.path.isfile(cand):
                        return cand
    except OSError:
        pass
    return None


def scan_folder_for_discs(folder: str, max_depth: int = 4) -> dict:
    """Scan a directory tree and identify Koudelka (D1-D4), SH1, SH2 (D1-D2), and SH3 discs."""
    found: dict = {
        "koudelka_discs": {},   # {1: path, 2: path, 3: path, 4: path}
        "koudelka_dirs": set(),
        "sh1": None,
        "sh2_d1": None,
        "sh2_d2": None,
        "sh3": None,
    }
    if not folder or not os.path.isdir(folder):
        return found

    root_depth = folder.rstrip("\\/").count(os.sep)
    candidates: list[str] = []
    for cur, dirs, files in os.walk(folder):
        depth = cur.rstrip("\\/").count(os.sep) - root_depth
        if depth >= max_depth:
            dirs[:] = []
        else:
            dirs[:] = [d for d in dirs if not d.startswith(".") and d.lower() not in ("$recycle.bin", "system volume information")]
        for fn in files:
            ext = os.path.splitext(fn)[1].lower()
            if ext in (".iso", ".bin", ".cue", ".chd", ".cso"):
                candidates.append(os.path.join(cur, fn))

    with ui.Spinner("Scan", f"Scanning {len(candidates)} disc file(s)...") as sp:
        for idx, path in enumerate(candidates, 1):
            sp.set_detail(f"Inspecting ({idx}/{len(candidates)}) {os.path.basename(path)}")
            ext = os.path.splitext(path)[1].lower()

            # Check PS1 Koudelka first on .bin files
            if ext == ".bin":
                kinfo = koudelka_prepare.probe_psx_bin(path)
                if kinfo and kinfo["disc"] not in found["koudelka_discs"]:
                    found["koudelka_discs"][kinfo["disc"]] = path
                    found["koudelka_dirs"].add(os.path.dirname(path))
                    continue

            # Check PS2 header on .iso / .bin / .cue
            probe_target = _resolve_cue_bin(path) if ext == ".cue" else path
            serial = None
            if probe_target and os.path.splitext(probe_target)[1].lower() in (".iso", ".bin"):
                serial = probe_ps2_iso_serial(probe_target)
            if serial and serial in PS2_SERIAL_TO_SLOT:
                slot = PS2_SERIAL_TO_SLOT[serial]
                if not found[slot]:
                    found[slot] = path
                continue

            # Fallback filename probe for compressed .chd / .cso
            if ext in (".chd", ".cso"):
                slot = _guess_ps2_slot_from_filename(path)
                if slot and not found[slot]:
                    found[slot] = path

    return found


def is_game_unpacked(install_dir: str, game_id: str) -> bool:
    if game_id == "koudelka":
        gdir = os.path.join(install_dir, "games", "koudelka")
        return os.path.isfile(os.path.join(gdir, "disc_files", "disc_manifest.tsv")) and os.path.isfile(
            os.path.join(gdir, "exe", "SLUS_010.51")
        )
    if game_id == "sh1":
        return os.path.isfile(os.path.join(install_dir, "games", "sh1", "disc_files", "disc_manifest.tsv"))
    if game_id == "sh2":
        return os.path.isfile(os.path.join(install_dir, "games", "sh2-covenant", "disc1_files", "disc_manifest.tsv"))
    if game_id == "sh3":
        return os.path.isfile(os.path.join(install_dir, "games", "sh3-ftnw", "disc_files", "disc_manifest.tsv"))
    return False


# ---------------------------------------------------------------------------
# Interactive Helpers
# ---------------------------------------------------------------------------


def ask_yes_no(prompt: str, default: bool = True) -> bool:
    hint = "[Y/n]" if default else "[y/N]"
    while True:
        try:
            ans = input(f"  {prompt} {hint}: ").strip().lower()
        except EOFError:
            return default
        if not ans:
            return default
        if ans in ("y", "yes"):
            return True
        if ans in ("n", "no"):
            return False
        print("  Please enter Y or N.")


def prompt_path_with_browse(prompt: str, pick_folder: bool = False, initial_dir: str = "") -> str:
    """Ask user to press [B] for Windows dialog, paste/drag a path, or [S] to skip."""
    while True:
        try:
            raw = input(f"  {prompt}: ").strip()
        except EOFError:
            return ""
        low = raw.lower()
        if low in ("", "s", "skip"):
            return ""
        if low == "b":
            chosen = (
                ui.pick_folder_dialog(prompt, initial_dir=initial_dir)
                if pick_folder
                else ui.pick_file_dialog(prompt, initial_dir=initial_dir)
            )
            if chosen:
                print(f"    Selected: {chosen}")
                return chosen
            print("    (Browse cancelled — press [B] again, paste a path, or [S] to skip.)")
            continue
        cleaned = raw.strip('"').strip("'")
        if os.path.exists(cleaned):
            return cleaned
        print(ui.paint(f"    Path not found: {cleaned}", ui.Style.CRIMSON))


# ---------------------------------------------------------------------------
# Step-by-Step Guided Setup Walkthrough
# ---------------------------------------------------------------------------


def run_wizard() -> int:
    cfg = ui.load_install_config()
    cpus, ram_gb = ui.detect_system_specs()
    rec_jobs, rec_label = ui.recommended_compile_jobs(cpus, ram_gb)

    ui.print_banner(
        "SHADOW HEARTS: NOCTURNE — STEP-BY-STEP SETUP WALKTHROUGH",
        "Configure everything upfront, then sit back while your PC builds the collection.",
    )
    print(f"  Detected PC: {ui.paint(f'{cpus} CPU threads', ui.Style.BOLD)}, {ui.paint(f'{ram_gb:.1f} GB RAM', ui.Style.BOLD)} (Auto preset: {rec_label}, {rec_jobs} threads)")

    # -----------------------------------------------------------------------
    # STEP 1: Select Games
    # -----------------------------------------------------------------------
    ui.print_section("STEP 1 of 6 — Choose Which Games to Install / Update")
    print(f"    [{ui.paint('A', ui.Style.GOLD)}] All 4 Games (Koudelka + Shadow Hearts + Covenant + From the New World)  {ui.paint('(Recommended)', ui.Style.GREEN)}")
    for g in GAMES:
        print(f"    [{g['num']}] {g['title']}")
    print(f"    [M] Skip walkthrough & open Advanced Command Menu")
    print("  Tip: Type A for all 4, a single number (e.g. 1), or comma-separated (e.g. 1,2,3).")

    selected_ids: list[str] = []
    while not selected_ids:
        try:
            raw = input("\n  Your selection [A]: ").strip().lower()
        except EOFError:
            raw = "a"
        if not raw or raw in ("a", "all"):
            selected_ids = [g["id"] for g in GAMES]
            break
        if raw == "m":
            return 2  # Signal Setup.cmd to open Advanced Command Menu
        num_map = {g["num"]: g["id"] for g in GAMES}
        num_map.update({"k": "koudelka", "koudelka": "koudelka", "sh1": "sh1", "sh2": "sh2", "sh3": "sh3"})
        tokens = [t.strip() for t in re.split(r"[\s,]+", raw) if t.strip()]
        picked: list[str] = []
        valid = True
        for tok in tokens:
            if tok in num_map:
                if num_map[tok] not in picked:
                    picked.append(num_map[tok])
            else:
                valid = False
                break
        if valid and picked:
            selected_ids = picked
        else:
            print(ui.paint("  Invalid selection. Enter A, 1-4, or comma-separated numbers like 1,3.", ui.Style.CRIMSON))

    # -----------------------------------------------------------------------
    # STEP 2: Install Location
    # -----------------------------------------------------------------------
    ui.print_section("STEP 2 of 6 — Choose Install Location")
    default_dir = cfg.get("install_dir") or REPO_ROOT
    print(f"  Default folder: {ui.paint(default_dir, ui.Style.BOLD)}")
    print("  Shadow Hearts Nocturne.exe, required DLLs, and games\\ will be staged here.")
    print("  Existing memory cards, save states, mods, and keys.ini are always preserved.")
    print("  Press [Enter] to use default, [B] to browse with Windows folder picker, or paste a folder path.")

    install_dir = default_dir
    while True:
        try:
            raw = input(f"\n  Install folder [{default_dir}]: ").strip()
        except EOFError:
            raw = ""
        if not raw:
            install_dir = default_dir
            break
        if raw.lower() == "b":
            chosen = ui.pick_folder_dialog("Choose Shadow Hearts: Nocturne Install Location", initial_dir=default_dir)
            if chosen:
                install_dir = os.path.abspath(chosen)
                break
            print("    (Browse cancelled — press [Enter] for default, [B] to browse, or type a path.)")
            continue
        install_dir = os.path.abspath(os.path.expanduser(raw.strip('"').strip("'")))
        break
    print(f"  -> Install location: {ui.paint(install_dir, ui.Style.GREEN)}")

    # -----------------------------------------------------------------------
    # STEP 3: Locate Disc Images (Smart ROMs Folder Scan + Browse for Missing)
    # -----------------------------------------------------------------------
    ui.print_section("STEP 3 of 6 — Locate Your Disc Images (ISO / CHD / CSO / BIN+CUE)")
    print("  Each game is unpacked once into an ISO-free folder so you won't need the disc images to play.")

    already_unpacked = {gid: is_game_unpacked(install_dir, gid) for gid in selected_ids}
    need_discs = [gid for gid in selected_ids if not already_unpacked[gid]]

    disc_plan: dict = {
        "koudelka_sources": [],
        "sh1": None,
        "sh2_d1": None,
        "sh2_d2": None,
        "sh3": None,
    }
    last_rom_dir = cfg.get("last_rom_dir") or ""

    if not need_discs:
        print(ui.paint("  All selected games already have unpacked virtual disc folders — no disc images needed!", ui.Style.GREEN))
    else:
        print("\n  Fastest way: point at a folder containing your ROMs/ISOs and we'll auto-detect all matching discs.")
        scan_dir = prompt_path_with_browse(
            "Press [B] to browse a ROMs folder, paste path, or [Enter] to pick game-by-game",
            pick_folder=True,
            initial_dir=last_rom_dir,
        )
        scanned = scan_folder_for_discs(scan_dir) if scan_dir else {}
        if scan_dir:
            last_rom_dir = scan_dir
            if scanned.get("koudelka_discs"):
                disc_plan["koudelka_sources"] = sorted(scanned["koudelka_dirs"])
            for k in ("sh1", "sh2_d1", "sh2_d2", "sh3"):
                if scanned.get(k):
                    disc_plan[k] = scanned[k]

        # Print scan results checklist
        print("\n  Disc Detection Status:")
        for gid in selected_ids:
            if already_unpacked[gid]:
                print(f"    {ui.paint('[OK]', ui.Style.GREEN)} {gid.upper():<9} Already unpacked in {install_dir}\\games\\")
            elif gid == "koudelka":
                kd = scanned.get("koudelka_discs", {})
                if len(kd) == 4:
                    print(f"    {ui.paint('[OK]', ui.Style.GREEN)} KOUDELKA  All 4 discs found in {disc_plan['koudelka_sources'][0]}")
                else:
                    print(f"    {ui.paint('[--]', ui.Style.YELLOW)} KOUDELKA  Found {len(kd)}/4 discs")
            elif gid == "sh2":
                d1 = disc_plan["sh2_d1"]
                d2 = disc_plan["sh2_d2"]
                s1 = os.path.basename(d1) if d1 else "MISSING"
                s2 = os.path.basename(d2) if d2 else "optional / missing"
                tag = ui.paint("[OK]", ui.Style.GREEN) if (d1 and d2) else ui.paint("[--]", ui.Style.YELLOW)
                print(f"    {tag} SH2       Disc 1: {s1}  |  Disc 2: {s2}")
            else:
                p = disc_plan.get(gid)
                if p:
                    print(f"    {ui.paint('[OK]', ui.Style.GREEN)} {gid.upper():<9} {os.path.basename(p)}")
                else:
                    print(f"    {ui.paint('[--]', ui.Style.YELLOW)} {gid.upper():<9} Not found yet")

        # Prompt ONLY for missing selected games
        final_selected: list[str] = []
        for gid in selected_ids:
            if already_unpacked[gid]:
                final_selected.append(gid)
                continue

            if gid == "koudelka":
                kd = scanned.get("koudelka_discs", {})
                if len(kd) < 4:
                    print(f"\n  {ui.paint('Koudelka (1898, PS1)', ui.Style.BOLD)} needs a folder with all 4 NTSC-U .bin/.cue discs.")
                    while True:
                        folder = prompt_path_with_browse(
                            "[B] Browse Koudelka folder, paste path, or [S] Skip Koudelka",
                            pick_folder=True,
                            initial_dir=last_rom_dir,
                        )
                        if not folder:
                            print("    Skipping Koudelka for now.")
                            break
                        found_k = koudelka_prepare.find_koudelka_discs([folder])
                        if len(found_k) == 4:
                            disc_plan["koudelka_sources"] = [folder]
                            last_rom_dir = folder
                            print(ui.paint("    Found all 4 Koudelka discs!", ui.Style.GREEN))
                            final_selected.append(gid)
                            break
                        missing = [d for d in (1, 2, 3, 4) if d not in found_k]
                        print(ui.paint(f"    Only found {len(found_k)}/4 discs (missing Disc {missing}). Try another folder or [S] to skip.", ui.Style.YELLOW))
                else:
                    final_selected.append(gid)

            elif gid == "sh1":
                if not disc_plan["sh1"]:
                    print(f"\n  {ui.paint('Shadow Hearts (1913, PS2)', ui.Style.BOLD)} — select SLUS-20347 disc image.")
                    p = prompt_path_with_browse(
                        "[B] Browse file, paste/drag path, or [S] Skip SH1",
                        pick_folder=False,
                        initial_dir=last_rom_dir,
                    )
                    if p:
                        disc_plan["sh1"] = p
                        last_rom_dir = os.path.dirname(p)
                        final_selected.append(gid)
                    else:
                        print("    Skipping Shadow Hearts (1913) for now.")
                else:
                    final_selected.append(gid)

            elif gid == "sh2":
                if not disc_plan["sh2_d1"]:
                    print(f"\n  {ui.paint('Shadow Hearts: Covenant (1915, PS2)', ui.Style.BOLD)} — select Disc 1 (SLUS-21041).")
                    p1 = prompt_path_with_browse(
                        "[B] Browse Disc 1, paste/drag path, or [S] Skip Covenant",
                        pick_folder=False,
                        initial_dir=last_rom_dir,
                    )
                    if not p1:
                        print("    Skipping Shadow Hearts: Covenant for now.")
                        continue
                    disc_plan["sh2_d1"] = p1
                    last_rom_dir = os.path.dirname(p1)
                if not disc_plan["sh2_d2"]:
                    print(f"  {ui.paint('Shadow Hearts: Covenant — Disc 2 (SLUS-21044)', ui.Style.BOLD)} (recommended for seamless auto-swap):")
                    p2 = prompt_path_with_browse(
                        "[B] Browse Disc 2, paste/drag path, or [Enter] to continue with Disc 1 only",
                        pick_folder=False,
                        initial_dir=last_rom_dir,
                    )
                    if p2:
                        disc_plan["sh2_d2"] = p2
                        last_rom_dir = os.path.dirname(p2)
                final_selected.append(gid)

            elif gid == "sh3":
                if not disc_plan["sh3"]:
                    print(f"\n  {ui.paint('Shadow Hearts: From the New World (1929, PS2)', ui.Style.BOLD)} — select SLUS-21326 disc image.")
                    p = prompt_path_with_browse(
                        "[B] Browse file, paste/drag path, or [S] Skip SH3",
                        pick_folder=False,
                        initial_dir=last_rom_dir,
                    )
                    if p:
                        disc_plan["sh3"] = p
                        last_rom_dir = os.path.dirname(p)
                        final_selected.append(gid)
                    else:
                        print("    Skipping From the New World for now.")
                else:
                    final_selected.append(gid)

        selected_ids = final_selected

    if not selected_ids:
        print(ui.paint("\n  No games selected to install. Re-run Setup whenever your disc images are ready!", ui.Style.YELLOW))
        ui.wait_for_keypress("Press any key to close...")
        return 0

    # -----------------------------------------------------------------------
    # STEP 4: Compile Performance (Hardware-Aware Presets)
    # -----------------------------------------------------------------------
    ui.print_section("STEP 4 of 6 — Build Performance (CPU Compile Threads)")
    presets = ui.build_job_presets(cpus, ram_gb)
    for idx, (label, j, desc) in enumerate(presets, 1):
        star = ui.paint("  <-- Recommended for your PC", ui.Style.GREEN) if idx == 1 else ""
        print(f"    [{idx}] {label:<34} ({desc}){star}")
    print(f"    Or enter any custom thread count (1-{cpus}).")

    chosen_jobs = presets[0][1]
    while True:
        try:
            raw = input(f"\n  Select preset [1]: ").strip()
        except EOFError:
            raw = ""
        if not raw or raw == "1":
            chosen_jobs = presets[0][1]
            break
        if raw in ("2", "3", "4"):
            chosen_jobs = presets[int(raw) - 1][1]
            break
        if raw.isdigit() and 1 <= int(raw) <= max(64, cpus):
            chosen_jobs = int(raw)
            break
        print("  Enter 1-4 or a thread count number.")
    print(f"  -> Compile parallelism: {ui.paint(f'{chosen_jobs} thread(s)', ui.Style.GREEN)}")

    # -----------------------------------------------------------------------
    # STEP 5: Shortcuts & Source Disc Cleanup
    # -----------------------------------------------------------------------
    ui.print_section("STEP 5 of 6 — Shortcuts & Source Disc Options")
    make_desktop = ask_yes_no("Create a Desktop shortcut for Shadow Hearts Nocturne.exe?", default=bool(cfg.get("create_desktop_shortcut", True)))
    make_startmenu = ask_yes_no("Create a Start Menu shortcut?", default=bool(cfg.get("create_startmenu_shortcut", False)))
    delete_sources = ask_yes_no(
        "Delete original source ISO/BIN/CHD files AFTER bit-exact verification? (Recommended: No — keep backups)",
        default=False,
    )

    # -----------------------------------------------------------------------
    # STEP 6: Review Summary, Check Running Apps, & Execute Install
    # -----------------------------------------------------------------------
    ui.print_section("STEP 6 of 6 — Final Installation Summary")
    game_by_id = {g["id"]: g for g in GAMES}
    print(f"  Install Location : {ui.paint(install_dir, ui.Style.BOLD)}")
    print(f"  Compile Threads  : {chosen_jobs} ({cpus} CPU threads / {ram_gb:.1f} GB RAM detected)")
    print(f"  Shortcuts        : Desktop={'Yes' if make_desktop else 'No'}, Start Menu={'Yes' if make_startmenu else 'No'}")
    print(f"  Original Discs   : {'Delete after bit-exact verify' if delete_sources else 'Keep originals safely'}")
    print("  Games to Build   :")
    for gid in selected_ids:
        print(f"    • {game_by_id[gid]['title']}")

    if not ask_yes_no("\nStart installation now?", default=True):
        print(ui.paint("\n  Installation cancelled. No files were changed.", ui.Style.YELLOW))
        ui.wait_for_keypress("Press any key to close...")
        return ui.EXIT_USER_CANCELLED

    # Graceful running-app check BEFORE doing any build work
    target_exes = ["Shadow Hearts Nocturne.exe", "sh2launcher.exe"] + [game_by_id[gid]["exe"] for gid in selected_ids]
    if not ui.ensure_apps_closed(target_exes, interactive=True, pause_on_cancel=True):
        return ui.EXIT_USER_CANCELLED

    # Persist preferences so future updates & Setup.cmd remember install_dir & performance preset
    cfg.update({
        "install_dir": install_dir,
        "jobs": chosen_jobs,
        "create_desktop_shortcut": make_desktop,
        "create_startmenu_shortcut": make_startmenu,
        "delete_source_images": delete_sources,
        "last_rom_dir": last_rom_dir,
    })
    ui.save_install_config(cfg)

    # Execute all selected builders in sequence
    return execute_build_queue(
        selected_ids=selected_ids,
        install_dir=install_dir,
        jobs=chosen_jobs,
        disc_plan=disc_plan,
        delete_sources=delete_sources,
        make_desktop=make_desktop,
        make_startmenu=make_startmenu,
        interactive_end=True,
    )


def execute_build_queue(
    selected_ids: list[str],
    install_dir: str,
    jobs: int,
    disc_plan: dict | None = None,
    delete_sources: bool = False,
    make_desktop: bool = False,
    make_startmenu: bool = False,
    interactive_end: bool = True,
) -> int:
    disc_plan = disc_plan or {}
    game_by_id = {g["id"]: g for g in GAMES}
    py_exe = sys.executable
    t0 = time.monotonic()
    total_games = len(selected_ids)

    for idx, gid in enumerate(selected_ids, 1):
        g = game_by_id[gid]
        ui.print_banner(
            f"INSTALLING GAME {idx}/{total_games} — {g['title']}",
            f"Overall progress: {idx - 1}/{total_games} completed  |  Elapsed: {ui.fmt_time(time.monotonic() - t0)}",
        )
        cmd = [
            py_exe, "-u", os.path.join(SCRIPT_DIR, g["script"]),
            "--install-dir", install_dir,
            "--jobs", str(jobs),
            "--skip-app-check",
        ]
        if delete_sources:
            cmd.append("--delete-sources")
        else:
            cmd.append("--keep-sources")

        if gid == "koudelka":
            for src_dir in disc_plan.get("koudelka_sources") or []:
                cmd.extend(["--source", src_dir])
            if delete_sources:
                cmd.append("--verify")
        elif gid == "sh1" and disc_plan.get("sh1"):
            cmd.extend(["--disc", disc_plan["sh1"]])
        elif gid == "sh2":
            if disc_plan.get("sh2_d1"):
                cmd.extend(["--disc1", disc_plan["sh2_d1"]])
            if disc_plan.get("sh2_d2"):
                cmd.extend(["--disc2", disc_plan["sh2_d2"]])
        elif gid == "sh3" and disc_plan.get("sh3"):
            cmd.extend(["--disc", disc_plan["sh3"]])

        res = subprocess.run(cmd, cwd=REPO_ROOT)
        if res.returncode == ui.EXIT_USER_CANCELLED:
            return ui.EXIT_USER_CANCELLED
        if res.returncode != 0:
            print(ui.paint(f"\n[ERROR] {g['title']} failed (exit code {res.returncode}).", ui.Style.CRIMSON), file=sys.stderr)
            return res.returncode

    # Create Desktop / Start Menu shortcuts if requested
    shortcuts = ui.create_launcher_shortcuts(install_dir, desktop=make_desktop, start_menu=make_startmenu)
    if shortcuts:
        ui.print_section("Created Shortcuts:")
        for lnk in shortcuts:
            print(f"  • {lnk}")

    launcher_exe = os.path.join(install_dir, "Shadow Hearts Nocturne.exe")
    ui.print_banner(
        f"ALL {total_games} GAME(S) INSTALLED SUCCESSFULLY in {ui.fmt_time(time.monotonic() - t0)}!",
        f"Collection ready at: {install_dir}",
    )

    if interactive_end and os.path.isfile(launcher_exe):
        if ask_yes_no("Launch Shadow Hearts Nocturne.exe now?", default=True):
            try:
                os.startfile(launcher_exe)  # type: ignore[attr-defined]
            except Exception:
                subprocess.Popen([launcher_exe], cwd=install_dir)
    return 0


def run_build_all(install_dir: str, jobs: int, non_interactive: bool = False) -> int:
    selected_ids = ["koudelka", "sh1", "sh2", "sh3"]
    if not ui.ensure_apps_closed(ui.ALL_NOCTURNE_EXES, interactive=not non_interactive, pause_on_cancel=True):
        return ui.EXIT_USER_CANCELLED
    return execute_build_queue(
        selected_ids=selected_ids,
        install_dir=install_dir,
        jobs=jobs,
        disc_plan={},
        delete_sources=False,
        make_desktop=False,
        make_startmenu=False,
        interactive_end=not non_interactive,
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Nocturne Setup Walkthrough & All-4 Builder")
    ap.add_argument("--wizard", action="store_true", help="Run the interactive 6-step Setup Walkthrough")
    ap.add_argument("--build-all", action="store_true", help="Build / Update ALL 4 games (Koudelka + SH1 + SH2 + SH3)")
    ap.add_argument("--install-dir", default=None, help="Override install folder")
    ap.add_argument("--jobs", type=int, default=0, help="Override Ninja compile jobs")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
    ap.add_argument("--check-first-run", action="store_true", help="Exit 0 if no games installed yet, 1 if at least one game is installed")
    ap.add_argument("--print-install-dir", action="store_true", help="Print the active install_dir and exit")
    args = ap.parse_args(argv)

    cfg = ui.load_install_config()
    install_dir = ui.resolve_install_dir(args.install_dir)
    jobs = args.jobs if args.jobs > 0 else int(cfg.get("jobs") or ui.recommended_compile_jobs()[0])

    if args.print_install_dir:
        print(install_dir)
        return 0
    if args.check_first_run:
        has_any = ui.any_game_installed(install_dir) or ui.any_game_installed(REPO_ROOT)
        return 1 if has_any else 0
    if args.build_all:
        return run_build_all(install_dir, jobs, non_interactive=args.non_interactive)
    return run_wizard()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nSetup cancelled by user.")
        raise SystemExit(ui.EXIT_USER_CANCELLED)
