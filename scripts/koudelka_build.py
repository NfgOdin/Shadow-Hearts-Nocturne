#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Koudelka (1999, PS1 NTSC-U, 4 Discs) End-to-End Builder.

1. Prepares the ISO-free deduplicated 4-disc virtual folder under
   games/koudelka/disc_files/ (preserving bit-exact Mode 2 Form 2 XA/STR streams)
   and extracts the PS-X EXE (SLUS_010.51) + 6 code overlays.
2. Builds psxrecomp and statically translates SLUS_010.51 + all 6 overlays
   (4,669 functions / 255,300 MIPS R3000A + GTE instructions) into 16 C++20 units.
3. Compiles psxrun + psxruntime + sh2launcher (using -j 3 for 8 GB RAM safety)
   and stages games/koudelka/Koudelka.exe + Shadow Hearts Nocturne.exe.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
GAME_DIR = os.path.join(REPO_ROOT, "games", "koudelka")
GEN_DIR = os.path.join(REPO_ROOT, "generated", "koudelka")
BUILD_DIR = os.path.join(REPO_ROOT, "build", "native-koudelka")

sys.path.insert(0, SCRIPT_DIR)
import koudelka_prepare  # noqa: E402

OVERLAY_SPECS = [
    ("BATTLE.BIN", "0x800B6DF0"),
    ("CAPTURE.BIN", "0x800B6DF0"),
    ("EFFECT.BIN", "0x800D5DAC"),
    ("FIELD.BIN", "0x800B6DF0"),
    ("GAMEEND.BIN", "0x800B6DF0"),
    ("MOVIE.BIN", "0x800B6DF0"),
]


def portable_env() -> dict[str, str]:
    env = os.environ.copy()
    portable = os.path.join(REPO_ROOT, "tools", "portable")
    bins = [
        os.path.join(portable, "llvm-mingw", "bin"),
        os.path.join(portable, "cmake", "bin"),
        os.path.join(portable, "ninja"),
        os.path.join(portable, "python"),
        os.path.join(portable, "mingit", "cmd"),
        os.path.join(portable, "mingit", "usr", "bin"),
        os.path.join(portable, "busybox"),
    ]
    existing = [p for p in bins if os.path.isdir(p)]
    if existing:
        env["PATH"] = os.pathsep.join(existing) + os.pathsep + env.get("PATH", "")
    return env


def ensure_sdl2(env: dict[str, str]) -> None:
    sdl2_hdr = os.path.join(REPO_ROOT, "third_party", "sdl2", "include", "SDL2", "SDL.h")
    if os.path.isfile(sdl2_hdr):
        return
    sh = shutil.which("sh", path=env.get("PATH")) or os.path.join(
        REPO_ROOT, "tools", "portable", "mingit", "usr", "bin", "sh.exe"
    )
    print("  Downloading SDL2 into third_party/sdl2...")
    run_cmd([sh, os.path.join(SCRIPT_DIR, "fetch_sdl2.sh")], env)


def run_cmd(cmd: list[str], env: dict[str, str]) -> None:
    res = subprocess.run(cmd, cwd=REPO_ROOT, env=env)
    if res.returncode != 0:
        raise RuntimeError(f"Command failed (exit {res.returncode}): {' '.join(cmd)}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build & stage Koudelka (PS1, 4 discs) for Shadow Hearts: Nocturne.")
    ap.add_argument("--source", action="append", default=[], help="Folder containing Koudelka Disc 1..4 .bin/.cue files")
    ap.add_argument("--jobs", type=int, default=3, help="Parallel Ninja compile jobs (default: 3 for 8 GB RAM)")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt for paths")
    ap.add_argument("--verify", action="store_true", help="Verify unpacked virtual disc sectors against source .bin files")
    args = ap.parse_args(argv)

    print("==============================================================================")
    print("  Shadow Hearts: Nocturne — Koudelka (1999, PS1 NTSC-U, 4 Discs) Builder")
    print("==============================================================================")

    manifest_path = os.path.join(GAME_DIR, "disc_files", "disc_manifest.tsv")
    exe_path = os.path.join(GAME_DIR, "exe", "SLUS_010.51")

    if os.path.isfile(manifest_path) and os.path.isfile(exe_path) and not args.source:
        print(f"\n[1/4] 4-disc virtual folder already prepared: {os.path.join(GAME_DIR, 'disc_files')}")
    else:
        print("\n[1/4] Locating & unpacking Koudelka Discs 1-4 (Mode 2/2352 -> deduplicated virtual discs)...")
        search = list(args.source) + [GAME_DIR] + koudelka_prepare.DEFAULT_SEARCH_DIRS
        discs = koudelka_prepare.find_koudelka_discs(search)
        if len(discs) < 4 and not args.non_interactive:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"  Could not auto-locate Disc(s) {missing}.")
            user_dir = input("  Enter folder path containing Koudelka Disc 1-4 .bin/.cue files: ").strip().strip('"')
            if user_dir:
                discs = koudelka_prepare.find_koudelka_discs([user_dir] + search)
        if len(discs) < 4:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"[ERROR] Missing Koudelka Disc(s) {missing}. Place all 4 NTSC-U discs in one folder and retry.")
            return 1
        koudelka_prepare.unpack_discs(discs, GAME_DIR, verify=args.verify)

    env = portable_env()
    ensure_sdl2(env)
    cmake_exe = os.path.join(REPO_ROOT, "tools", "portable", "cmake", "bin", "cmake.exe")
    ninja_exe = os.path.join(REPO_ROOT, "tools", "portable", "ninja", "ninja.exe")
    clang_exe = os.path.join(REPO_ROOT, "tools", "portable", "llvm-mingw", "bin", "clang++.exe")

    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(GEN_DIR, exist_ok=True)

    print("\n[2/4] Building psxrecomp & translating SLUS_010.51 + 6 Koudelka overlays -> C++20...")
    cmake_common = [
        cmake_exe, "-S", REPO_ROOT, "-B", BUILD_DIR, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_MAKE_PROGRAM={ninja_exe}".replace("\\", "/"),
        f"-DCMAKE_CXX_COMPILER={clang_exe}".replace("\\", "/"),
    ]
    # Configure without PSX_GENERATED first so a fresh install with an empty generated/koudelka/
    # folder can build psxrecomp before psxgame has any .cpp units to compile.
    run_cmd(cmake_common + ["-DPSX_GENERATED="], env)
    run_cmd([ninja_exe, "-C", BUILD_DIR, "-j", str(args.jobs), "psxrecomp"], env)

    psxrecomp_exe = os.path.join(BUILD_DIR, "psxrecomp.exe")
    recomp_cmd = [psxrecomp_exe, exe_path, "--out", GEN_DIR, "--units", "16"]
    ovl_dir = os.path.join(GAME_DIR, "exe", "overlays")
    for ovl_name, base_addr in OVERLAY_SPECS:
        ovl_path = os.path.join(ovl_dir, ovl_name)
        if os.path.isfile(ovl_path):
            recomp_cmd.extend(["--overlay", f"{ovl_path}:{base_addr}"])
    run_cmd(recomp_cmd, env)

    print(f"\n[3/4] Compiling Koudelka.exe (psxrun) + Gothic Launcher (-j {args.jobs})...")
    run_cmd(cmake_common + [f"-DPSX_GENERATED={GEN_DIR}".replace("\\", "/")], env)
    run_cmd([ninja_exe, "-C", BUILD_DIR, "-j", str(args.jobs), "psxrun", "sh2launcher"], env)

    print("\n[4/4] Staging games/koudelka/Koudelka.exe & Shadow Hearts Nocturne.exe...")
    psxrun_src = os.path.join(BUILD_DIR, "psxrun.exe")
    koudelka_dst = os.path.join(GAME_DIR, "Koudelka.exe")
    shutil.copy2(psxrun_src, koudelka_dst)

    for sdl_src in (
        os.path.join(BUILD_DIR, "SDL2.dll"),
        os.path.join(REPO_ROOT, "third_party", "sdl2", "bin", "SDL2.dll"),
    ):
        if os.path.isfile(sdl_src):
            shutil.copy2(sdl_src, os.path.join(GAME_DIR, "SDL2.dll"))
            break

    launcher_src = os.path.join(BUILD_DIR, "sh2launcher.exe")
    launcher_dst = os.path.join(REPO_ROOT, "Shadow Hearts Nocturne.exe")
    if os.path.isfile(launcher_src):
        try:
            shutil.copy2(launcher_src, launcher_dst)
        except PermissionError:
            print("  [NOTE] Shadow Hearts Nocturne.exe is currently running; skipped overwrite.")

    print("\n==============================================================================")
    print(f"  SUCCESS: Koudelka (4 Discs) staged at: {koudelka_dst}")
    print("==============================================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
