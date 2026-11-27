#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Koudelka (1999, PS1 NTSC-U, 4 Discs) End-to-End Builder.

1. Prepares the ISO-free deduplicated 4-disc virtual folder under
   <install_dir>/games/koudelka/disc_files/ (preserving bit-exact Mode 2 Form 2 XA/STR streams)
   and extracts the PS-X EXE (SLUS_010.51) + 6 code overlays.
2. Builds psxrecomp and statically translates SLUS_010.51 + all 6 overlays
   (4,669 functions / 255,300 MIPS R3000A + GTE instructions) into 16 C++20 units.
3. Compiles psxrun + psxruntime + sh2launcher with a live progress bar
   and stages <install_dir>/games/koudelka/Koudelka.exe + Shadow Hearts Nocturne.exe.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
GEN_DIR = os.path.join(REPO_ROOT, "generated", "koudelka")
BUILD_DIR = os.path.join(REPO_ROOT, "build", "native-koudelka")

sys.path.insert(0, SCRIPT_DIR)
import koudelka_prepare  # noqa: E402
import nocturne_ui as ui  # noqa: E402

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
    ui.run_spinner_cmd(
        [sh, os.path.join(SCRIPT_DIR, "fetch_sdl2.sh")],
        "SDL2 SDK",
        "Downloading SDL2 into third_party/sdl2...",
        env=env,
    )


def maybe_delete_koudelka_sources(
    discs: dict[int, dict],
    game_dir: str,
    delete_flag: bool | None,
    interactive: bool,
) -> None:
    if not discs:
        return
    if delete_flag is False:
        return
    # Never delete files located inside game_dir
    targets: list[str] = []
    game_abs = os.path.abspath(game_dir).lower()
    for info in discs.values():
        p = info.get("path", "")
        if not p or not os.path.isfile(p):
            continue
        p_abs = os.path.abspath(p)
        if p_abs.lower().startswith(game_abs + os.sep):
            continue
        targets.append(p_abs)
        cue = os.path.splitext(p_abs)[0] + ".cue"
        if os.path.isfile(cue):
            targets.append(cue)
    if not targets:
        return

    do_delete = bool(delete_flag)
    if delete_flag is None and interactive:
        ans = input("\n  Delete original Koudelka source .bin/.cue files now that virtual discs are verified? [y/N]: ").strip().lower()
        do_delete = ans in ("y", "yes")
    if not do_delete:
        return
    removed = 0
    for p in targets:
        try:
            os.remove(p)
            removed += 1
        except OSError as exc:
            print(f"  [warn] Could not remove {os.path.basename(p)}: {exc}")
    if removed:
        print(f"  Removed {removed} original Koudelka source file(s).")


def main(argv: list[str] | None = None) -> int:
    rec_jobs, _ = ui.recommended_compile_jobs()
    ap = argparse.ArgumentParser(description="Build & stage Koudelka (PS1, 4 discs) for Shadow Hearts: Nocturne.")
    ap.add_argument("--source", action="append", default=[], help="Folder containing Koudelka Disc 1..4 .bin/.cue files")
    ap.add_argument("--install-dir", default=None, help="Target install folder (default: saved install_config or repo root)")
    ap.add_argument("--jobs", type=int, default=rec_jobs, help=f"Parallel Ninja compile jobs (auto-recommended: {rec_jobs})")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt for paths")
    ap.add_argument("--verify", action="store_true", help="Verify unpacked virtual disc sectors against source .bin files")
    ap.add_argument("--delete-sources", action="store_true", help="Delete source .bin/.cue images after bit-exact verification")
    ap.add_argument("--keep-sources", action="store_true", help="Keep source .bin/.cue images without prompting")
    ap.add_argument("--skip-app-check", action="store_true", help="Skip running-app prompt (already checked by caller)")
    ap.add_argument("--game-dll-only", action="store_true", help="Only build & stage games/koudelka/game.dll (skip shared engine build)")
    args = ap.parse_args(argv)

    overall_t0 = time.monotonic()
    install_dir = ui.resolve_install_dir(args.install_dir)
    game_dir = os.path.join(install_dir, "games", "koudelka")
    interactive = not args.non_interactive

    ui.print_banner(
        "Shadow Hearts: Nocturne — Koudelka (1898, PS1 NTSC-U, 4 Discs) Builder",
        f"Install folder: {install_dir}  |  Compile jobs: {args.jobs}",
    )

    if not args.skip_app_check:
        if not ui.ensure_apps_closed(
            ["Shadow Hearts Nocturne.exe", "sh2launcher.exe", "nocturne_psx.exe", "Koudelka.exe"],
            interactive=interactive,
        ):
            return ui.EXIT_USER_CANCELLED

    manifest_path = os.path.join(game_dir, "disc_files", "disc_manifest.tsv")
    exe_path = os.path.join(game_dir, "exe", "SLUS_010.51")
    unpacked_discs: dict[int, dict] = {}

    env = portable_env()
    ensure_sdl2(env)
    cmake_exe = os.path.join(REPO_ROOT, "tools", "portable", "cmake", "bin", "cmake.exe")
    ninja_exe = os.path.join(REPO_ROOT, "tools", "portable", "ninja", "ninja.exe")
    clang_exe = os.path.join(REPO_ROOT, "tools", "portable", "llvm-mingw", "bin", "clang++.exe")

    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(GEN_DIR, exist_ok=True)

    cmake_common = [
        cmake_exe, "-S", REPO_ROOT, "-B", BUILD_DIR, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_MAKE_PROGRAM={ninja_exe}".replace("\\", "/"),
        f"-DCMAKE_CXX_COMPILER={clang_exe}".replace("\\", "/"),
    ]

    if os.path.isfile(manifest_path) and os.path.isfile(exe_path) and not args.source:
        ui.print_section(f"[1/4] 4-disc virtual folder already prepared: {os.path.join(game_dir, 'disc_files')}")
    else:
        ui.print_section("[1/4] Locating & unpacking Koudelka Discs 1-4 (.bin/.cue/.chd -> deduplicated virtual discs)...")
        search = list(args.source) + [game_dir] + koudelka_prepare.DEFAULT_SEARCH_DIRS
        discs = koudelka_prepare.find_koudelka_discs(search)
        if len(discs) < 4 and interactive:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"  Could not auto-locate Disc(s) {missing}.")
            choice = input("  Press [B] to browse for folder, or paste folder path containing Disc 1-4 .bin/.cue/.chd: ").strip().strip('"')
            if choice.lower() == "b":
                choice = ui.pick_folder_dialog("Select folder containing Koudelka Disc 1-4 .bin/.cue/.chd files")
            if choice:
                discs = koudelka_prepare.find_koudelka_discs([choice] + search)
        if len(discs) < 4:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"[ERROR] Missing Koudelka Disc(s) {missing}. Place all 4 NTSC-U discs in one folder and retry.")
            return 1
        chdunpack_exe = None
        if any(info.get("is_chd") or str(info.get("path", "")).lower().endswith(".chd") for info in discs.values()):
            ui.run_spinner_cmd(cmake_common + ["-DPSX_GENERATED="], "CMake (CHD)", "Configuring native chdunpack tool...", env=env)
            ui.run_ninja_with_progress([ninja_exe, "-C", BUILD_DIR, "-j", str(args.jobs), "chdunpack"], "chdunpack", env=env)
            cand = os.path.join(BUILD_DIR, "chdunpack.exe")
            if os.path.isfile(cand):
                chdunpack_exe = cand
        do_verify = args.verify or args.delete_sources
        koudelka_prepare.unpack_discs(discs, game_dir, verify=do_verify, chdunpack_exe=chdunpack_exe)
        unpacked_discs = discs

    ui.print_section("[2/4] Building psxrecomp & translating SLUS_010.51 + 6 Koudelka overlays -> C++20...")
    # Configure without PSX_GENERATED first so a fresh install with an empty generated/koudelka/
    # folder can build psxrecomp before psxgame has any .cpp units to compile.
    ui.run_spinner_cmd(
        cmake_common + ["-DPSX_GENERATED="],
        "CMake (Tools)",
        "Configuring psxrecomp build...",
        env=env,
    )
    ui.run_ninja_with_progress(
        [ninja_exe, "-C", BUILD_DIR, "-j", str(args.jobs), "psxrecomp"],
        "psxrecomp",
        env=env,
    )

    psxrecomp_exe = os.path.join(BUILD_DIR, "psxrecomp.exe")
    recomp_cmd = [psxrecomp_exe, exe_path, "--out", GEN_DIR, "--units", "16"]
    ovl_dir = os.path.join(game_dir, "exe", "overlays")
    for ovl_name, base_addr in OVERLAY_SPECS:
        ovl_path = os.path.join(ovl_dir, ovl_name)
        if os.path.isfile(ovl_path):
            recomp_cmd.extend(["--overlay", f"{ovl_path}:{base_addr}"])
    ui.run_spinner_cmd(
        recomp_cmd,
        "Translate",
        "Translating SLUS_010.51 + 6 overlays -> 16 C++20 units...",
        env=env,
        echo_stdout=True,
    )

    ui.print_section(f"[3/4] Compiling Koudelka game.dll (-j {args.jobs})...")
    ui.run_spinner_cmd(
        cmake_common + [f"-DPSX_GENERATED={GEN_DIR}".replace("\\", "/")],
        "CMake (Game)",
        "Configuring Koudelka game module...",
        env=env,
    )
    ui.run_ninja_with_progress(
        [ninja_exe, "-C", BUILD_DIR, "-j", str(args.jobs), "psx_game_dll"],
        "Koudelka Game DLL",
        env=env,
    )

    ui.print_section(f"[4/4] Staging games/koudelka/game.dll -> {install_dir}...")
    os.makedirs(game_dir, exist_ok=True)
    dll_src = os.path.join(BUILD_DIR, "game.dll")
    dll_dst = os.path.join(game_dir, "game.dll")
    if not os.path.isfile(dll_src):
        raise RuntimeError(f"Expected built game.dll not found: {dll_src}")
    shutil.copy2(dll_src, dll_dst)
    print(f"  Staged game module: {dll_dst}")

    if not args.game_dll_only:
        ui.build_and_stage_shared_engine(install_dir, args.jobs, targets=["sh2launcher", "nocturne_psx"], env=env)

    # Clean up legacy per-game EXE after successful game.dll migration
    legacy_exe = os.path.join(game_dir, "Koudelka.exe")
    if os.path.isfile(legacy_exe) and os.path.isfile(dll_dst):
        try:
            os.remove(legacy_exe)
            print("  Migrated & cleaned up legacy games/koudelka/Koudelka.exe (now using shared engine + game.dll)")
        except OSError:
            pass

    del_flag = True if args.delete_sources else (False if args.keep_sources else None)
    maybe_delete_koudelka_sources(unpacked_discs, game_dir, del_flag, interactive=interactive)

    ui.print_banner(
        f"SUCCESS: Koudelka (4 Discs) ready in {ui.fmt_time(time.monotonic() - overall_t0)}!",
        f"Staged at: {dll_dst}",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
