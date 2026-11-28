#!/usr/bin/env python3
"""One-shot ahead-of-time builder for Shadow Hearts (2001, SLUS-20347) in Shadow Hearts: Nocturne.

Unpacks the user's own PS2 NTSC-U Shadow Hearts disc image (.iso/.chd/.cso/.cue/.bin) into an
ISO-free SSD folder (games/sh1/disc_files/), verifies bit-for-bit integrity against the source
image, translates SLUS_203.47 (taking advantage of its unstripped .symtab) + any recorded VU1/VU0
microprograms, compiles build/native-sh1/ps2run.exe with PS2_GAME_ID=sh1, and stages
games/sh1/SH1.exe + Shadow Hearts Nocturne.exe (Trilogy Launcher).
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
GAME_DIR = os.path.join(ROOT, "games", "sh1")
DISC_DIR = os.path.join(GAME_DIR, "disc_files")
GEN_DIR = os.path.join(ROOT, "generated", "sh1")
BUILD_DIR = os.path.join(ROOT, "build")
TOOLS_BUILD = os.path.join(BUILD_DIR, "tools")
NATIVE_BUILD = os.path.join(BUILD_DIR, "native-sh1")
FINAL_EXE = os.path.join(GAME_DIR, "SH1.exe")
LAUNCHER_EXE = os.path.join(ROOT, "Shadow Hearts Nocturne.exe")

sys.path.insert(0, SCRIPT_DIR)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nocturne_ui as ui  # noqa: E402
from ps2_disc_toolkit.cli import extract_disc, verify_disc  # noqa: E402


def configure_install_dir(install_dir: str) -> None:
    global GAME_DIR, DISC_DIR, FINAL_EXE, LAUNCHER_EXE
    install_dir = os.path.abspath(install_dir)
    GAME_DIR = os.path.join(install_dir, "games", "sh1")
    DISC_DIR = os.path.join(GAME_DIR, "disc_files")
    FINAL_EXE = os.path.join(GAME_DIR, "SH1.exe")
    LAUNCHER_EXE = os.path.join(install_dir, "Shadow Hearts Nocturne.exe")


def fmt_time(sec: float) -> str:
    return ui.fmt_time(sec)


def is_unpacked_disc(path: str) -> bool:
    return bool(path) and os.path.isdir(path) and os.path.isfile(os.path.join(path, "disc_manifest.tsv"))


def run_ninja_with_progress(cmd: list[str], label: str) -> None:
    ui.run_ninja_with_progress(cmd, label, cwd=ROOT)


def ensure_third_party() -> None:
    sdl2_hdr = os.path.join(ROOT, "third_party", "sdl2", "include", "SDL2", "SDL.h")
    wgpu_lib = os.path.join(ROOT, "third_party", "wgpu-native", "lib", "libwgpu_native.a")
    dxc_dll = os.path.join(ROOT, "third_party", "dxc", "dxcompiler.dll")
    sh = shutil.which("sh") or os.path.join(ROOT, "tools", "portable", "mingit", "usr", "bin", "sh.exe")
    if not os.path.isfile(sdl2_hdr):
        ui.run_spinner_cmd([sh, os.path.join(SCRIPT_DIR, "fetch_sdl2.sh")], "SDL2 SDK", "Downloading SDL2 into third_party/sdl2...")
    if not os.path.isfile(wgpu_lib):
        ui.run_spinner_cmd([sh, os.path.join(SCRIPT_DIR, "fetch_wgpu.sh")], "wgpu-native", "Downloading wgpu-native into third_party/wgpu-native...")
    if os.name == "nt" and not os.path.isfile(dxc_dll):
        try:
            ui.run_spinner_cmd([sh, os.path.join(SCRIPT_DIR, "fetch_dxc.sh")], "DXC", "Downloading Microsoft DXC (dxcompiler.dll + dxil.dll)...")
        except (OSError, RuntimeError) as exc:
            print(f"  [warn] Could not download DXC ({exc}); Direct3D 12 will use FXC until scripts/fetch_dxc.sh succeeds.")


def ensure_chdunpack() -> str:
    ensure_tools_cmake()
    exe = os.path.join(TOOLS_BUILD, "chdunpack.exe" if os.name == "nt" else "chdunpack")
    if not os.path.isfile(exe):
        run_ninja_with_progress(["cmake", "--build", TOOLS_BUILD, "--target", "chdunpack"], "chdunpack")
    if not os.path.isfile(exe):
        raise RuntimeError(f"Expected chdunpack binary not found: {exe}")
    return exe


def unpack_and_verify(src: str, dst_dir: str) -> tuple[str, str | None]:
    if is_unpacked_disc(src):
        print(f"  Using existing unpacked Disc folder: {src}")
        return src, None
    if not os.path.isfile(src):
        raise RuntimeError(f"Disc image not found: {src}")
    ext = os.path.splitext(src)[1].lower()
    if ext not in (".iso", ".chd", ".cso", ".cue", ".bin"):
        raise RuntimeError(f"Unsupported disc format '{ext}': {src}")
    os.makedirs(os.path.dirname(dst_dir), exist_ok=True)
    temp_iso: str | None = None
    work_src = src
    if ext == ".chd":
        chdunpack = ensure_chdunpack()
        temp_iso = os.path.join(GAME_DIR, ".tmp_sh1_chd.iso")
        ui.run_spinner_cmd([chdunpack, src, temp_iso], "CHD", f"Decompressing {os.path.basename(src)}...")
        if not os.path.isfile(temp_iso):
            raise RuntimeError(f"chdunpack did not produce {temp_iso}")
        work_src = temp_iso
    try:
        with ui.Spinner("Unpack", f"Unpacking {os.path.basename(work_src)} -> disc_files/...") as sp:
            extract_disc(work_src, dst_dir, skip_existing=True)
            if not is_unpacked_disc(dst_dir):
                raise RuntimeError(f"Failed to unpack {work_src} into {dst_dir}")
            sp.set_detail(f"Verifying bit-for-bit integrity against {os.path.basename(work_src)}...")
            ok = verify_disc(work_src, dst_dir)
            if ok is False:
                raise RuntimeError(f"Bit-for-bit verification failed for {work_src} vs {dst_dir}")
    finally:
        if temp_iso and os.path.exists(temp_iso):
            try:
                os.remove(temp_iso)
            except OSError:
                pass
    return dst_dir, os.path.abspath(src)


def resolve_disc(disc_arg: str | None, interactive: bool) -> tuple[str, str | None]:
    os.makedirs(GAME_DIR, exist_ok=True)
    if disc_arg:
        return unpack_and_verify(disc_arg.strip().strip('"').strip("'"), DISC_DIR)
    if is_unpacked_disc(DISC_DIR):
        print(f"  Using unpacked Disc folder: {DISC_DIR}")
        return DISC_DIR, None
    legacy_iso = os.path.join(GAME_DIR, "disc.iso")
    if os.path.isfile(legacy_iso):
        return unpack_and_verify(legacy_iso, DISC_DIR)
    if interactive:
        print(f"  Unpacked Shadow Hearts disc not found in {DISC_DIR}.")
        p = input("  Press [B] to browse, or paste/drag Shadow Hearts (USA) disc image (.iso/.chd/.cso/.cue/.bin): ").strip()
        if p.lower() == "b":
            p = ui.pick_file_dialog("Select Shadow Hearts (USA) Disc Image")
        if p:
            return unpack_and_verify(p.strip('"').strip("'"), DISC_DIR)
    raise RuntimeError(
        "Missing Shadow Hearts disc! Pass --disc \"path/to/Shadow Hearts (USA).iso\" or place unpacked files in games/sh1/disc_files/."
    )


def find_boot_elf(disc_dir: str) -> str:
    layer0 = os.path.join(disc_dir, "layer0")
    sys_cnf = os.path.join(layer0, "SYSTEM.CNF")
    if not os.path.isfile(sys_cnf):
        raise RuntimeError(f"Missing SYSTEM.CNF inside {layer0}")
    boot_name = None
    with open(sys_cnf, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            m = re.search(r"BOOT2\s*=\s*cdrom0:\\([^\s;]+)", line, re.I)
            if m:
                boot_name = m.group(1).strip()
                break
    if not boot_name:
        raise RuntimeError(f"Could not parse BOOT2 from {sys_cnf}")
    elf_path = os.path.join(layer0, boot_name)
    if not os.path.isfile(elf_path):
        raise RuntimeError(f"Boot ELF '{boot_name}' not found at {elf_path}")
    print(f"  Verified boot ELF: {boot_name} ({os.path.getsize(elf_path):,} bytes)")
    return elf_path


def find_runtime_build_dir() -> str | None:
    matches = sorted(glob.glob(os.path.join(BUILD_DIR, "runtime-native-*")))
    if matches:
        return matches[-1]
    sh2_rt = os.path.join(BUILD_DIR, "native-sh2-covenant", "libps2runtime.a")
    if os.path.isfile(sh2_rt):
        return os.path.join(BUILD_DIR, "native-sh2-covenant")
    return None


def ensure_tools_cmake() -> None:
    if os.path.isfile(os.path.join(TOOLS_BUILD, "build.ninja")):
        return
    os.makedirs(TOOLS_BUILD, exist_ok=True)
    ui.run_spinner_cmd(
        ["cmake", "-S", ROOT, "-B", TOOLS_BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=clang++"],
        "CMake (Tools)",
        "Configuring recompiler tools...",
    )


def ensure_native_cmake(runtime_lib: str | None) -> None:
    if os.path.isfile(os.path.join(NATIVE_BUILD, "build.ninja")):
        return
    flags_file = os.path.join(BUILD_DIR, "cxx_flags")
    extra_flags = open(flags_file, "r", encoding="utf-8").read().split() if os.path.isfile(flags_file) else []
    cmd = [
        "cmake", "-S", ROOT, "-B", NATIVE_BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=clang++",
        f"-DPS2_GENERATED={GEN_DIR}",
        "-DPS2_GAME_ID=sh1",
    ] + extra_flags
    if runtime_lib:
        cmd.append(f"-DPS2_RUNTIME_LIB={runtime_lib}")
    ui.run_spinner_cmd(cmd, "CMake (SH1)", "Configuring Shadow Hearts native build...")


def clear_live_caches() -> int:
    removed = 0
    for sub in ("ee-live", "vu1-live"):
        folder = os.path.join(GAME_DIR, sub)
        if not os.path.isdir(folder):
            continue
        for entry in os.listdir(folder):
            full = os.path.join(folder, entry)
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


def main() -> int:
    rec_jobs, _ = ui.recommended_compile_jobs()
    ap = argparse.ArgumentParser(description="Build Shadow Hearts (2001, SLUS-20347) Native PC Port")
    ap.add_argument("--disc", help="Path to Shadow Hearts (USA) disc image (.iso/.chd/.cso/.cue/.bin) or unpacked folder")
    ap.add_argument("--install-dir", default=None, help="Target install folder (default: saved install_config or repo root)")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
    ap.add_argument("--jobs", type=int, default=rec_jobs, help=f"Parallel Ninja compile jobs (auto-recommended: {rec_jobs})")
    ap.add_argument("--delete-sources", action="store_true", help="Delete source disc image after bit-exact verification")
    ap.add_argument("--keep-sources", action="store_true", help="Keep source disc image without prompting")
    ap.add_argument("--skip-app-check", action="store_true", help="Skip running-app prompt (already checked by caller)")
    ap.add_argument("--game-dll-only", action="store_true", help="Only build & stage games/sh1/game.dll (skip shared engine build)")
    args = ap.parse_args()

    overall_t0 = time.monotonic()
    install_dir = ui.resolve_install_dir(args.install_dir)
    configure_install_dir(install_dir)
    interactive = not args.non_interactive

    ui.print_banner(
        "Shadow Hearts: Nocturne — Shadow Hearts (1913, SLUS-20347) Builder",
        f"Install folder: {install_dir}  |  Compile jobs: {args.jobs}",
    )

    if not args.skip_app_check:
        if not ui.ensure_apps_closed(
            ["Shadow Hearts Nocturne.exe", "sh2launcher.exe", "nocturne_ps2.exe", "SH1.exe"],
            interactive=interactive,
        ):
            return ui.EXIT_USER_CANCELLED

    # Step 1: Unpack & verify disc into <install_dir>/games/sh1/disc_files/
    ui.print_section("[1/5] Preparing ISO-free disc folder (games/sh1/disc_files/)...")
    disc_dir, unpacked_src = resolve_disc(args.disc, interactive=interactive)
    elf_path = find_boot_elf(disc_dir)

    # Step 2: Ensure third-party SDKs, recompiler tools & runtime library
    ui.print_section("[2/5] Checking third-party SDKs, recompiler tools (eerecomp, vurecomp), and runtime...")
    ensure_third_party()
    ensure_tools_cmake()
    run_ninja_with_progress(["cmake", "--build", TOOLS_BUILD, "--target", "eerecomp", "vurecomp", "--", f"-j{args.jobs}"], "Tools")
    rt_dir = find_runtime_build_dir()
    rt_lib = None
    if rt_dir:
        run_ninja_with_progress(["cmake", "--build", rt_dir, "--target", "ps2runtime", "--", f"-j{args.jobs}"], "Runtime")
        cand = os.path.join(rt_dir, "libps2runtime.a")
        if os.path.isfile(cand):
            rt_lib = cand

    # Step 3: Translate monolithic SLUS_203.47 (unstripped .symtab) & VU1/VU0 microprograms
    ui.print_section("[3/5] Translating SLUS_203.47 and recorded VU1/VU0 programs...")
    os.makedirs(GEN_DIR, exist_ok=True)
    eerecomp = os.path.join(TOOLS_BUILD, "eerecomp.exe")
    vurecomp = os.path.join(TOOLS_BUILD, "vurecomp.exe")

    ee_cmd = [eerecomp, elf_path, "--out", GEN_DIR]
    entries_txt = os.path.join(GAME_DIR, "entries.txt")
    if os.path.isfile(entries_txt):
        ee_cmd.extend(["--entries", entries_txt])
    ui.run_spinner_cmd(ee_cmd, "EE Recomp", "Translating SLUS_203.47 -> C++20...", echo_stdout=True)

    vu1_dir = os.path.join(GAME_DIR, "vu1")
    vu0_dir = os.path.join(GAME_DIR, "vu0")
    os.makedirs(vu1_dir, exist_ok=True)
    os.makedirs(vu0_dir, exist_ok=True)
    ui.run_spinner_cmd([vurecomp, vu1_dir, GEN_DIR, "--vu0-dir", vu0_dir], "VU Recomp", "Translating VU1/VU0 microprograms...", echo_stdout=True)

    # Step 4: Compile native C++ game module (games/sh1/game.dll) + shared engine
    ui.print_section(f"[4/5] Compiling SH1 game.dll (-j {args.jobs})...")
    ensure_native_cmake(rt_lib)
    run_ninja_with_progress(
        ["cmake", "--build", NATIVE_BUILD, "--target", "ps2_game_dll", "--", f"-j{args.jobs}"],
        "SH1 Game DLL",
    )

    # Step 5: Stage games/sh1/game.dll + shared engine (nocturne_ps2.exe & Shadow Hearts Nocturne.exe)
    ui.print_section(f"[5/5] Staging games/sh1/game.dll -> {install_dir}...")
    src_dll = os.path.join(NATIVE_BUILD, "game.dll")
    dst_dll = os.path.join(GAME_DIR, "game.dll")
    if not os.path.isfile(src_dll):
        raise RuntimeError(f"Expected built game.dll not found: {src_dll}")
    os.makedirs(GAME_DIR, exist_ok=True)
    shutil.copy2(src_dll, dst_dll)
    print(f"  Staged game module: {dst_dll}")
    if not args.game_dll_only:
        ui.build_and_stage_shared_engine(install_dir, args.jobs, targets=["sh2launcher", "nocturne_ps2"])
    # Clean up legacy per-game EXE after successful game.dll migration
    if os.path.isfile(FINAL_EXE) and os.path.isfile(dst_dll):
        try:
            os.remove(FINAL_EXE)
            print("  Migrated & cleaned up legacy games/sh1/SH1.exe (now using shared engine + game.dll)")
        except OSError:
            pass
    removed = clear_live_caches()
    print(f"  Cleared {removed} stale cache file(s) from ee-live / vu1-live.")

    if unpacked_src and args.delete_sources and not os.path.abspath(unpacked_src).lower().startswith(os.path.abspath(GAME_DIR).lower() + os.sep):
        try:
            os.remove(unpacked_src)
            print(f"  Removed verified source image: {unpacked_src}")
        except OSError as exc:
            print(f"  [warn] Could not remove {unpacked_src}: {exc}")

    ui.print_banner(
        f"DONE in {fmt_time(time.monotonic() - overall_t0)}!",
        "Launch 'Shadow Hearts Nocturne.exe' -> select Shadow Hearts -> PLAY SHADOW HEARTS.",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
