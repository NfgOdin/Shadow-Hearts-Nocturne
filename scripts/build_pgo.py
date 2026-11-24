#!/usr/bin/env python3
"""Optional Profile-Guided Optimization (PGO) builder for Shadow Hearts: Nocturne.

Runs a 3-step Clang PGO pipeline across all installed games (Koudelka + SH1/SH2/SH3):
  1. Build instrumented binaries into build/pgo-gen-{ps2,psx}
  2. Run headless training frames to collect .profraw branch/layout profiles
  3. Merge with llvm-profdata and rebuild Release builds with -DPS2_PGO_USE=...
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools", "portable")
CMAKE = os.path.join(TOOLS, "cmake", "bin", "cmake.exe")
NINJA = os.path.join(TOOLS, "ninja", "ninja.exe")
LLVM_BIN = os.path.join(TOOLS, "llvm-mingw", "bin")
CLANGXX = os.path.join(LLVM_BIN, "clang++.exe")
WINDRES = os.path.join(LLVM_BIN, "windres.exe")
PROFDATA = os.path.join(LLVM_BIN, "llvm-profdata.exe")


def make_env() -> dict[str, str]:
    env = os.environ.copy()
    env["PATH"] = LLVM_BIN + os.pathsep + env.get("PATH", "")
    return env


def run(cmd: list[str], env: dict[str, str] | None = None, check: bool = True) -> int:
    print("+", " ".join(cmd), flush=True)
    res = subprocess.run(cmd, cwd=ROOT, env=env if env is not None else make_env())
    if check and res.returncode != 0:
        raise RuntimeError(f"Command failed ({res.returncode}): {' '.join(cmd)}")
    return res.returncode


def cmake_path(p: str) -> str:
    return p.replace("\\", "/")


def configure_and_build(build_dir: str, targets: list[str], extra_cmake: list[str], jobs: int) -> None:
    cmd = [
        CMAKE, "-S", cmake_path(ROOT), "-B", cmake_path(build_dir), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_MAKE_PROGRAM={cmake_path(NINJA)}",
        f"-DCMAKE_CXX_COMPILER={cmake_path(CLANGXX)}",
        f"-DCMAKE_RC_COMPILER={cmake_path(WINDRES)}",
        f"-DPS2_WGPU_DIR={cmake_path(os.path.join(ROOT, 'third_party', 'wgpu-native'))}",
        f"-DCMAKE_PREFIX_PATH={cmake_path(os.path.join(ROOT, 'third_party', 'sdl2'))}",
    ] + extra_cmake
    run(cmd)
    run([NINJA, "-C", build_dir, "-j", str(jobs)] + targets)


def merge_profiles(raw_dir: str, out_file: str) -> bool:
    raws = sorted(glob.glob(os.path.join(raw_dir, "*.profraw")))
    if not raws:
        print(f"  [warn] No .profraw files produced in {raw_dir}; skipping PGO merge.")
        return False
    run([PROFDATA, "merge", "-output=" + out_file] + raws)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Build PGO-optimized Nocturne PS2 + PS1 engines.")
    ap.add_argument("--frames", type=int, default=240, help="Headless training frames per game (default: 240)")
    ap.add_argument("--jobs", type=int, default=3, help="Ninja parallel jobs (default: 3)")
    ap.add_argument("--ps2-only", action="store_true", help="Only run PGO for nocturne_ps2")
    ap.add_argument("--psx-only", action="store_true", help="Only run PGO for nocturne_psx (Koudelka)")
    args = ap.parse_args()

    if not os.path.isfile(PROFDATA):
        print(f"ERROR: llvm-profdata not found at {PROFDATA}", file=sys.stderr)
        return 1

    prof_root = os.path.join(ROOT, "build", "pgo-profiles")
    os.makedirs(prof_root, exist_ok=True)

    if not args.psx_only:
        ps2_raw = os.path.join(prof_root, "ps2-raw")
        ps2_merged = os.path.join(prof_root, "ps2.profdata")
        shutil.rmtree(ps2_raw, ignore_errors=True)
        os.makedirs(ps2_raw, exist_ok=True)
        gen_dir = os.path.join(ROOT, "build", "pgo-gen-ps2")
        sh2_gen = os.path.join(ROOT, "generated", "sh2-covenant")
        extra = [f"-DPS2_PGO_GENERATE={ps2_raw.replace(os.sep, '/')}"]
        if os.path.isdir(sh2_gen):
            extra.append(f"-DPS2_GENERATED={sh2_gen.replace(os.sep, '/')}")
        print("\n[PGO 1/3] Building instrumented nocturne_ps2...")
        configure_and_build(gen_dir, ["nocturne_ps2"], extra, args.jobs)
        for dll in ("SDL2.dll", "dxcompiler.dll", "dxil.dll"):
            src_dll = os.path.join(ROOT, dll)
            if os.path.isfile(src_dll):
                shutil.copy2(src_dll, os.path.join(gen_dir, dll))

        print(f"\n[PGO 2/3] Collecting PS2 profiles ({args.frames} frames per installed game)...")
        inst_exe = os.path.join(gen_dir, "nocturne_ps2.exe")
        for slug in ("sh1", "sh2-covenant", "sh3-ftnw"):
            gdir = os.path.join(ROOT, "games", slug)
            if os.path.isfile(os.path.join(gdir, "game.dll")):
                run([inst_exe, "--game", slug, "--game-dir", gdir, "--frames", str(args.frames)], check=False)

        if merge_profiles(ps2_raw, ps2_merged):
            print("\n[PGO 3/3] Rebuilding Release nocturne_ps2 with PGO profile...")
            out_dir = os.path.join(ROOT, "build", "native-sh2-covenant")
            use_extra = [f"-DPS2_PGO_USE={ps2_merged.replace(os.sep, '/')}", "-DPS2_PGO_GENERATE="]
            if os.path.isdir(sh2_gen):
                use_extra.append(f"-DPS2_GENERATED={sh2_gen.replace(os.sep, '/')}")
            configure_and_build(out_dir, ["nocturne_ps2"], use_extra, args.jobs)
            shutil.copy2(os.path.join(out_dir, "nocturne_ps2.exe"), os.path.join(ROOT, "engine", "nocturne_ps2.exe"))
            print("  Staged PGO-optimized engine/nocturne_ps2.exe")

    if not args.ps2_only:
        psx_raw = os.path.join(prof_root, "psx-raw")
        psx_merged = os.path.join(prof_root, "psx.profdata")
        shutil.rmtree(psx_raw, ignore_errors=True)
        os.makedirs(psx_raw, exist_ok=True)
        gen_dir = os.path.join(ROOT, "build", "pgo-gen-psx")
        extra = [f"-DPS2_PGO_GENERATE={psx_raw.replace(os.sep, '/')}"]
        print("\n[PGO 1/3] Building instrumented nocturne_psx (Koudelka)...")
        configure_and_build(gen_dir, ["nocturne_psx"], extra, args.jobs)
        for dll in ("SDL2.dll",):
            src_dll = os.path.join(ROOT, dll)
            if os.path.isfile(src_dll):
                shutil.copy2(src_dll, os.path.join(gen_dir, dll))

        print(f"\n[PGO 2/3] Collecting Koudelka profile ({args.frames} frames)...")
        inst_exe = os.path.join(gen_dir, "nocturne_psx.exe")
        kgame = os.path.join(ROOT, "games", "koudelka")
        kdir = os.path.join(kgame, "disc_files")
        if os.path.isdir(kdir) and os.path.isfile(os.path.join(kgame, "game.dll")):
            run([inst_exe, "--game-dir", kgame, "--disc-dir", kdir, "--headless", "--frames", str(args.frames)], check=False)

        if merge_profiles(psx_raw, psx_merged):
            print("\n[PGO 3/3] Rebuilding Release nocturne_psx with PGO profile...")
            out_dir = os.path.join(ROOT, "build", "native-koudelka")
            use_extra = [f"-DPS2_PGO_USE={psx_merged.replace(os.sep, '/')}", "-DPS2_PGO_GENERATE="]
            configure_and_build(out_dir, ["nocturne_psx"], use_extra, args.jobs)
            shutil.copy2(os.path.join(out_dir, "nocturne_psx.exe"), os.path.join(ROOT, "engine", "nocturne_psx.exe"))
            print("  Staged PGO-optimized engine/nocturne_psx.exe")

    print("\nPGO build complete!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
