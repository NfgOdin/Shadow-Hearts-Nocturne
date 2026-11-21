#!/usr/bin/env python3
"""One-click ahead-of-time VU1/VU0 microprogram pre-compiler for Shadow Hearts: Covenant.

Folds all newly captured VU1/VU0 microprograms (games/sh2-covenant/vu1/*.bin and vu0/*.bin)
permanently into Shadow Hearts Covenant.exe via vurecomp + incremental Ninja relink (~5-10s)
without touching EE overlays, discs, or closing Shadow Hearts Nocturne.exe (Launcher).
Eliminates first-spell / boss / Judgement Ring VU1 JIT micro-stutters permanently.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
TOOLS_BUILD = os.path.join(ROOT, "build", "tools")

GAME_CONFIGS = {
    "sh1": {
        "title": "Shadow Hearts",
        "slug": "sh1",
        "build_dir": "native-sh1",
        "exe_name": "SH1.exe",
        "build_cmd": "Build Shadow Hearts.cmd",
    },
    "sh2": {
        "title": "Shadow Hearts: Covenant",
        "slug": "sh2-covenant",
        "build_dir": "native-sh2-covenant",
        "exe_name": "SH2C.exe",
        "build_cmd": "Build Shadow Hearts Covenant.cmd",
    },
    "sh3": {
        "title": "Shadow Hearts: From the New World",
        "slug": "sh3-ftnw",
        "build_dir": "native-sh3-ftnw",
        "exe_name": "SH3.exe",
        "build_cmd": "Build Shadow Hearts From the New World.cmd",
    },
}

GAME_DIR = os.path.join(ROOT, "games", "sh2-covenant")
VU1_DIR = os.path.join(GAME_DIR, "vu1")
VU0_DIR = os.path.join(GAME_DIR, "vu0")
VU1_LIVE_DIR = os.path.join(GAME_DIR, "vu1-live")
GEN_DIR = os.path.join(ROOT, "generated", "sh2-covenant")
NATIVE_BUILD = os.path.join(ROOT, "build", "native-sh2-covenant")
FINAL_EXE = os.path.join(GAME_DIR, "SH2C.exe")
GAME_TITLE = "Shadow Hearts: Covenant"
BUILD_CMD_NAME = "Build Shadow Hearts Covenant.cmd"


def configure_game(game_id: str) -> dict:
    """Switch module paths to target game ('sh1', 'sh2', or 'sh3')."""
    global GAME_DIR, VU1_DIR, VU0_DIR, VU1_LIVE_DIR, GEN_DIR, NATIVE_BUILD, FINAL_EXE, GAME_TITLE, BUILD_CMD_NAME
    key = (game_id or "sh2").strip().lower()
    if key in ("sh2-covenant", "covenant"):
        key = "sh2"
    elif key in ("sh3-ftnw", "ftnw"):
        key = "sh3"
    if key not in GAME_CONFIGS:
        raise ValueError(f"Unsupported game id '{game_id}' (expected sh1, sh2, or sh3)")
    cfg = GAME_CONFIGS[key]
    GAME_TITLE = cfg["title"]
    BUILD_CMD_NAME = cfg["build_cmd"]
    GAME_DIR = os.path.join(ROOT, "games", cfg["slug"])
    VU1_DIR = os.path.join(GAME_DIR, "vu1")
    VU0_DIR = os.path.join(GAME_DIR, "vu0")
    VU1_LIVE_DIR = os.path.join(GAME_DIR, "vu1-live")
    GEN_DIR = os.path.join(ROOT, "generated", cfg["slug"])
    NATIVE_BUILD = os.path.join(ROOT, "build", cfg["build_dir"])
    FINAL_EXE = os.path.join(GAME_DIR, cfg["exe_name"])
    return cfg

BIN_NAME_RE = re.compile(r"^([0-9A-Fa-f]{16})_([0-9A-Fa-f]+)\.bin$")
TABLE_HASH_RE = re.compile(r"0x([0-9A-Fa-f]{16})ull")


def ensure_portable_path() -> None:
    """Prepend bundled portable toolchain directories to PATH so double-click / Launcher runs work."""
    extra = [
        os.path.join(ROOT, "tools", "portable", "cmake", "bin"),
        os.path.join(ROOT, "tools", "portable", "llvm-mingw", "bin"),
        os.path.join(ROOT, "tools", "portable", "mingw64", "bin"),
        os.path.join(ROOT, "tools", "portable", "ninja"),
    ]
    existing = [p for p in extra if os.path.isdir(p)]
    if existing:
        os.environ["PATH"] = os.pathsep.join(existing) + os.pathsep + os.environ.get("PATH", "")


def scan_vu_status(vu1_dir: str | None = None, vu0_dir: str | None = None,
                   gen_dir: str | None = None, vu1_live_dir: str | None = None) -> dict:
    """Inspect captured VU1/VU0 binaries vs baked vu1_table.cpp entries and live DLL cache."""
    vu1_dir = VU1_DIR if vu1_dir is None else vu1_dir
    vu0_dir = VU0_DIR if vu0_dir is None else vu0_dir
    gen_dir = GEN_DIR if gen_dir is None else gen_dir
    vu1_live_dir = VU1_LIVE_DIR if vu1_live_dir is None else vu1_live_dir
    baked_hashes: set[str] = set()
    table_path = os.path.join(gen_dir, "vu1_table.cpp")
    if os.path.isfile(table_path):
        try:
            text = open(table_path, "r", encoding="utf-8", errors="ignore").read()
            baked_hashes = {m.group(1).upper() for m in TABLE_HASH_RE.finditer(text)}
        except OSError:
            pass

    def collect_bins(folder: str) -> tuple[int, set[str]]:
        if not os.path.isdir(folder):
            return 0, set()
        files = 0
        hashes: set[str] = set()
        for name in os.listdir(folder):
            m = BIN_NAME_RE.match(name)
            if m and os.path.isfile(os.path.join(folder, name)):
                files += 1
                hashes.add(m.group(1).upper())
        return files, hashes

    vu1_files, vu1_hashes = collect_bins(vu1_dir)
    vu0_files, vu0_hashes = collect_bins(vu0_dir)
    unbaked_vu1 = sorted(vu1_hashes - baked_hashes)
    unbaked_vu0 = sorted(vu0_hashes - baked_hashes)

    live_dlls = 0
    if os.path.isdir(vu1_live_dir):
        for name in os.listdir(vu1_live_dir):
            if name.endswith((".dll", ".so", ".dylib")):
                live_dlls += 1

    return {
        "baked_programs": len(baked_hashes),
        "captured_vu1_files": vu1_files,
        "captured_vu1_unique": len(vu1_hashes),
        "captured_vu0_files": vu0_files,
        "captured_vu0_unique": len(vu0_hashes),
        "unbaked_vu1_count": len(unbaked_vu1),
        "unbaked_vu0_count": len(unbaked_vu0),
        "unbaked_total": len(unbaked_vu1) + len(unbaked_vu0),
        "unbaked_vu1_hashes": unbaked_vu1,
        "live_dll_count": live_dlls,
    }


def sync_generated_vu_files(src_dir: str, dst_dir: str) -> tuple[int, int, int]:
    """Copy changed VU generated C++ files from src_dir into dst_dir and remove stale ones.

    Only rewrites files whose bytes actually changed so Ninja skips recompiling unchanged units.
    Returns (written_count, unchanged_count, removed_count).
    """
    os.makedirs(dst_dir, exist_ok=True)
    new_names: set[str] = set()
    written = 0
    unchanged = 0
    for name in sorted(os.listdir(src_dir)):
        if not (name.endswith(".cpp") and (name.startswith("vu1_") or name.startswith("vu0_"))):
            continue
        new_names.add(name)
        src_path = os.path.join(src_dir, name)
        dst_path = os.path.join(dst_dir, name)
        src_bytes = open(src_path, "rb").read()
        if os.path.isfile(dst_path):
            try:
                if open(dst_path, "rb").read() == src_bytes:
                    unchanged += 1
                    continue
            except OSError:
                pass
        with open(dst_path, "wb") as f:
            f.write(src_bytes)
        written += 1

    removed = 0
    for name in os.listdir(dst_dir):
        if name.endswith(".cpp") and (name.startswith("vu1_") or name.startswith("vu0_")):
            if name not in new_names:
                try:
                    os.remove(os.path.join(dst_dir, name))
                    removed += 1
                except OSError:
                    pass
    return written, unchanged, removed


def check_game_not_running() -> None:
    """Verify Shadow Hearts Covenant.exe and ps2run.exe are not locked (Launcher may stay open)."""
    for exe in (os.path.join(NATIVE_BUILD, "ps2run.exe"), FINAL_EXE):
        if not os.path.isfile(exe):
            continue
        try:
            with open(exe, "r+b"):
                pass
        except PermissionError:
            raise RuntimeError(
                f"{os.path.basename(exe)} is currently running! Close the game window first "
                "(the Launcher can stay open) and try again."
            )


def ensure_vurecomp() -> str:
    exe_name = "vurecomp.exe" if os.name == "nt" else "vurecomp"
    cand = os.path.join(TOOLS_BUILD, exe_name)
    if os.path.isfile(cand):
        return cand
    if not os.path.isfile(os.path.join(TOOLS_BUILD, "build.ninja")):
        os.makedirs(TOOLS_BUILD, exist_ok=True)
        print("  Configuring recompiler tools project...")
        subprocess.run(
            ["cmake", "-S", ROOT, "-B", TOOLS_BUILD, "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=clang++"],
            cwd=ROOT, check=True,
        )
    print("  Building vurecomp tool...")
    subprocess.run(["cmake", "--build", TOOLS_BUILD, "--target", "vurecomp"], cwd=ROOT, check=True)
    if not os.path.isfile(cand):
        raise RuntimeError(f"Failed to build {exe_name} in {TOOLS_BUILD}")
    return cand


def clear_vu1_live_cache() -> int:
    removed = 0
    if not os.path.isdir(VU1_LIVE_DIR):
        return 0
    for entry in os.listdir(VU1_LIVE_DIR):
        full = os.path.join(VU1_LIVE_DIR, entry)
        try:
            if os.path.isdir(full):
                shutil.rmtree(full, ignore_errors=True)
                removed += 1
            elif entry.endswith((".dll", ".mark", ".obj", ".o", ".so", ".dylib")):
                os.remove(full)
                removed += 1
        except OSError:
            pass
    return removed


def precompile_vu1(force: bool = False, dry_run: bool = False, jobs: int | None = None) -> dict:
    t0 = time.monotonic()
    ensure_portable_path()
    before = scan_vu_status()
    exe_label = os.path.basename(FINAL_EXE)
    print("=" * 74)
    print(f"  {GAME_TITLE} - One-Click VU1 AOT Pre-Compiler")
    print("=" * 74)
    print(f"  Currently baked in EXE:  {before['baked_programs']} VU1 program(s)")
    print(f"  Captured recordings:     {before['captured_vu1_unique']} unique VU1, {before['captured_vu0_unique']} unique VU0")
    print(f"  New unbaked captures:    {before['unbaked_total']}  (live JIT DLLs: {before['live_dll_count']})")

    if before["captured_vu1_unique"] == 0 and before["captured_vu0_unique"] == 0:
        print(f"\n  No recorded VU1/VU0 microprograms found in {VU1_DIR} yet.")
        print("  Play the game to capture spell/effect microprograms automatically.")
        return {"status": "empty", "before": before, "after": before}

    if before["unbaked_total"] == 0 and not force:
        print(f"\n  All captured VU1/VU0 microprograms are ALREADY baked into {exe_label}!")
        if before["live_dll_count"] > 0 and not dry_run:
            cleaned = clear_vu1_live_cache()
            print(f"  Cleaned {cleaned} redundant vu1-live cache entry/entries.")
        return {"status": "up_to_date", "before": before, "after": scan_vu_status()}

    if dry_run:
        print(f"\n  [dry-run] Would run vurecomp, rebuild ps2run.exe incrementally, and stage {exe_label}.")
        return {"status": "dry_run", "before": before, "after": before}

    check_game_not_running()
    if not os.path.isfile(os.path.join(NATIVE_BUILD, "build.ninja")):
        raise RuntimeError(
            f"Native build directory ({os.path.relpath(NATIVE_BUILD, ROOT)}) is not configured yet. "
            f"Run '{BUILD_CMD_NAME}' once first."
        )

    vurecomp = ensure_vurecomp()
    os.makedirs(VU1_DIR, exist_ok=True)
    os.makedirs(VU0_DIR, exist_ok=True)

    print("\n[1/4] Translating captured VU1/VU0 microprograms to C++ via vurecomp...")
    with tempfile.TemporaryDirectory(prefix="sh_vu_gen_") as tmp_out:
        res = subprocess.run(
            [vurecomp, VU1_DIR, tmp_out, "--vu0-dir", VU0_DIR],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            if res.stdout:
                print(res.stdout, file=sys.stderr)
            if res.stderr:
                print(res.stderr, file=sys.stderr)
            raise RuntimeError(f"vurecomp failed with exit code {res.returncode}")
        for line in res.stdout.strip().splitlines():
            print(" ", line)
        written, unchanged, removed = sync_generated_vu_files(tmp_out, GEN_DIR)
        print(f"  Synced generated VU C++ units: {written} updated, {unchanged} unchanged, {removed} stale removed.")

    print("\n[2/4] Incrementally compiling updated VU1 units & relinking ps2run.exe...")
    build_jobs = jobs if (jobs and jobs > 0) else max(2, min(4, os.cpu_count() or 3))
    subprocess.run(
        ["cmake", "--build", NATIVE_BUILD, "--target", "ps2run", "-j", str(build_jobs)],
        cwd=ROOT,
        check=True,
    )

    print(f"\n[3/4] Staging {exe_label}...")
    src_exe = os.path.join(NATIVE_BUILD, "ps2run.exe")
    if not os.path.isfile(src_exe):
        raise RuntimeError(f"Built executable missing: {src_exe}")
    shutil.copy2(src_exe, FINAL_EXE)
    print(f"  Updated: {FINAL_EXE}")

    print("\n[4/4] Cleaning redundant vu1-live JIT cache...")
    cleaned = clear_vu1_live_cache()
    print(f"  Removed {cleaned} redundant vu1-live cache entry/entries (now permanently AOT-baked).")

    after = scan_vu_status()
    elapsed = time.monotonic() - t0
    print("\n" + "=" * 74)
    print(f"  DONE in {elapsed:.1f}s! Baked VU1 programs: {before['baked_programs']} -> {after['baked_programs']}")
    print("  All captured spells & effects will now run at full native speed with zero JIT stutter.")
    print("=" * 74)
    return {"status": "baked", "before": before, "after": after, "elapsed_sec": round(elapsed, 2)}


def self_test() -> None:
    """Hermetic unit test for VU1 status scanner and smart file syncer."""
    with tempfile.TemporaryDirectory(prefix="sh2_vu_test_") as tmp:
        vu1 = os.path.join(tmp, "vu1")
        vu0 = os.path.join(tmp, "vu0")
        gen = os.path.join(tmp, "gen")
        live = os.path.join(tmp, "vu1-live")
        stage = os.path.join(tmp, "stage")
        for d in (vu1, vu0, gen, live, stage):
            os.makedirs(d, exist_ok=True)

        # Create 2 fake captured VU1 bins (1 already baked, 1 new unbaked) + 1 invalid filename
        open(os.path.join(vu1, "0123456789ABCDEF_0100.bin"), "wb").write(b"\x00" * 16)
        open(os.path.join(vu1, "FEDCBA9876543210_0200.bin"), "wb").write(b"\x00" * 16)
        open(os.path.join(vu1, "not_a_vu_bin.txt"), "wb").write(b"ignored")
        open(os.path.join(live, "7c8dda17-001.dll"), "wb").write(b"dll")
        open(os.path.join(gen, "vu1_table.cpp"), "w", encoding="utf-8").write(
            "const ps2::vu::Program kVuPrograms[] = {\n"
            "    {0x0123456789abcdefull, vu1_0123456789ABCDEF, 1},\n"
            "};\n"
        )

        st = scan_vu_status(vu1, vu0, gen, live)
        assert st["baked_programs"] == 1, st
        assert st["captured_vu1_unique"] == 2, st
        assert st["unbaked_vu1_count"] == 1, st
        assert st["unbaked_vu1_hashes"] == ["FEDCBA9876543210"], st
        assert st["live_dll_count"] == 1, st

        # Test sync_generated_vu_files skips identical bytes and removes stale units
        open(os.path.join(gen, "vu1_000.cpp"), "wb").write(b"// unchanged\n")
        open(os.path.join(gen, "vu1_099.cpp"), "wb").write(b"// stale\n")
        open(os.path.join(gen, "ee_000.cpp"), "wb").write(b"// ee untouched\n")
        open(os.path.join(stage, "vu1_000.cpp"), "wb").write(b"// unchanged\n")
        open(os.path.join(stage, "vu1_001.cpp"), "wb").write(b"// new\n")
        open(os.path.join(stage, "vu1_table.cpp"), "wb").write(b"// updated table\n")

        written, unchanged, removed = sync_generated_vu_files(stage, gen)
        assert (written, unchanged, removed) == (2, 1, 1), (written, unchanged, removed)
        assert os.path.isfile(os.path.join(gen, "ee_000.cpp"))
        assert not os.path.isfile(os.path.join(gen, "vu1_099.cpp"))

    print("[sh2_precompile_vu1] self-test passed (all checks OK)")


def main(default_game: str = "sh2") -> None:
    ap = argparse.ArgumentParser(description="One-click VU1/VU0 AOT pre-compiler for the Shadow Hearts Trilogy")
    ap.add_argument("--game", default=default_game, choices=["sh1", "sh2", "sh3"],
                    help="Target game: sh1 (Shadow Hearts), sh2 (Covenant), or sh3 (From the New World)")
    ap.add_argument("--jobs", "-j", type=int, default=0, help="Parallel compile jobs (0 = auto, capped at 4)")
    ap.add_argument("--check", action="store_true", help="Print VU1 captured/baked status and exit")
    ap.add_argument("--json", action="store_true", help="Output --check status as JSON")
    ap.add_argument("--force", action="store_true", help="Force re-running vurecomp and relinking even if up to date")
    ap.add_argument("--dry-run", action="store_true", help="Check what would be baked without modifying files")
    ap.add_argument("--pause", action="store_true", help="Wait for Enter before exiting (useful when launched in a console)")
    ap.add_argument("--self-test", action="store_true", help="Run built-in unit self-test and exit")
    args = ap.parse_args()

    configure_game(args.game)

    if args.self_test:
        self_test()
        return

    if args.check:
        st = scan_vu_status()
        if args.json:
            print(json.dumps(st, indent=2))
        else:
            print(f"[{GAME_TITLE}] Baked VU1: {st['baked_programs']} | Captured: {st['captured_vu1_unique']} VU1, "
                  f"{st['captured_vu0_unique']} VU0 | Unbaked: {st['unbaked_total']} | Live DLLs: {st['live_dll_count']}")
        return

    try:
        precompile_vu1(force=args.force, dry_run=args.dry_run, jobs=args.jobs or None)
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        if args.pause:
            try:
                input("\nPress Enter to close...")
            except EOFError:
                pass
        sys.exit(1)

    if args.pause:
        try:
            input("\nPress Enter to close...")
        except EOFError:
            pass


if __name__ == "__main__":
    main()
