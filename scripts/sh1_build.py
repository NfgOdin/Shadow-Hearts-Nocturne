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
from ps2_disc_toolkit.cli import extract_disc, verify_disc  # noqa: E402


def fmt_time(sec: float) -> str:
    sec = max(0, int(sec))
    m, s = divmod(sec, 60)
    return f"{m}m {s:02d}s" if m else f"{s}s"


def is_unpacked_disc(path: str) -> bool:
    return bool(path) and os.path.isdir(path) and os.path.isfile(os.path.join(path, "disc_manifest.tsv"))


def run_ninja_with_progress(cmd: list[str], label: str) -> None:
    t0 = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdout is not None
    step_re = re.compile(r"^\[(\d+)/(\d+)\]\s+(.*)$")
    last_print = 0.0
    output_tail: list[str] = []
    for raw in proc.stdout:
        line = raw.rstrip("\r\n")
        output_tail.append(line)
        if len(output_tail) > 60:
            output_tail.pop(0)
        m = step_re.match(line)
        if m:
            cur, total = int(m.group(1)), int(m.group(2))
            desc = m.group(3)
            now = time.monotonic()
            if cur == total or cur == 1 or now - last_print >= 1.5:
                pct = (cur * 100.0) / max(1, total)
                elapsed = now - t0
                eta = (elapsed * (total - cur) / cur) if cur > 0 else 0.0
                short_desc = desc if len(desc) <= 64 else ("..." + desc[-61:])
                print(
                    f"  [{label}] {cur:3d}/{total:3d} ({pct:5.1f}%)  elapsed {fmt_time(elapsed)}  ETA {fmt_time(eta)}  | {short_desc}",
                    flush=True,
                )
                last_print = now
        elif any(tok in line for tok in ("error:", "fatal error", "FAILED:", "lld-link:", "ld.lld:")):
            print("  " + line, flush=True)
    rc = proc.wait()
    if rc != 0:
        print("\n--- Build failed; recent output ---", file=sys.stderr)
        for ln in output_tail[-30:]:
            print(ln, file=sys.stderr)
        raise RuntimeError(f"{label} failed with exit code {rc}")
    print(f"  [{label}] Complete in {fmt_time(time.monotonic() - t0)}.", flush=True)


def ensure_third_party() -> None:
    sdl2_hdr = os.path.join(ROOT, "third_party", "sdl2", "include", "SDL2", "SDL.h")
    wgpu_lib = os.path.join(ROOT, "third_party", "wgpu-native", "lib", "libwgpu_native.a")
    dxc_dll = os.path.join(ROOT, "third_party", "dxc", "dxcompiler.dll")
    sh = shutil.which("sh") or os.path.join(ROOT, "tools", "portable", "mingit", "usr", "bin", "sh.exe")
    if not os.path.isfile(sdl2_hdr):
        print("  Downloading SDL2 into third_party/sdl2...")
        subprocess.run([sh, os.path.join(SCRIPT_DIR, "fetch_sdl2.sh")], cwd=ROOT, check=True)
    if not os.path.isfile(wgpu_lib):
        print("  Downloading wgpu-native into third_party/wgpu-native...")
        subprocess.run([sh, os.path.join(SCRIPT_DIR, "fetch_wgpu.sh")], cwd=ROOT, check=True)
    if os.name == "nt" and not os.path.isfile(dxc_dll):
        print("  Downloading Microsoft DXC (dxcompiler.dll + dxil.dll) for fast Direct3D 12 shader startup...")
        try:
            subprocess.run([sh, os.path.join(SCRIPT_DIR, "fetch_dxc.sh")], cwd=ROOT, check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"  [warn] Could not download DXC ({exc}); Direct3D 12 will use FXC until scripts/fetch_dxc.sh succeeds.")


def ensure_chdunpack() -> str:
    ensure_tools_cmake()
    exe = os.path.join(TOOLS_BUILD, "chdunpack.exe" if os.name == "nt" else "chdunpack")
    if not os.path.isfile(exe):
        run_ninja_with_progress(["cmake", "--build", TOOLS_BUILD, "--target", "chdunpack"], "chdunpack")
    if not os.path.isfile(exe):
        raise RuntimeError(f"Expected chdunpack binary not found: {exe}")
    return exe


def unpack_and_verify(src: str, dst_dir: str) -> str:
    if is_unpacked_disc(src):
        print(f"  Using existing unpacked Disc folder: {src}")
        return src
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
        print(f"  Decompressing .chd via native chdunpack -> {os.path.basename(temp_iso)} ...")
        proc = subprocess.run([chdunpack, src, temp_iso], cwd=ROOT)
        if proc.returncode != 0 or not os.path.isfile(temp_iso):
            raise RuntimeError(f"chdunpack failed for {src} (exit code {proc.returncode})")
        work_src = temp_iso
    t0 = time.monotonic()
    try:
        print(f"  Unpacking Shadow Hearts Disc ({os.path.basename(work_src)}) -> {dst_dir} ...")
        extract_disc(work_src, dst_dir, skip_existing=True)
        if not is_unpacked_disc(dst_dir):
            raise RuntimeError(f"Failed to unpack {work_src} into {dst_dir}")
        print(f"  Verifying unpacked files bit-for-bit against {os.path.basename(work_src)} ...")
        ok = verify_disc(work_src, dst_dir)
        if ok is False:
            raise RuntimeError(f"Bit-for-bit verification failed for {work_src} vs {dst_dir}")
    finally:
        if temp_iso and os.path.exists(temp_iso):
            try:
                os.remove(temp_iso)
            except OSError:
                pass
    print(f"  Disc unpacked & verified bit-for-bit in {fmt_time(time.monotonic() - t0)}.")
    return dst_dir


def resolve_disc(disc_arg: str | None, interactive: bool) -> str:
    os.makedirs(GAME_DIR, exist_ok=True)
    if disc_arg:
        return unpack_and_verify(disc_arg.strip().strip('"').strip("'"), DISC_DIR)
    if is_unpacked_disc(DISC_DIR):
        print(f"  Using unpacked Disc folder: {DISC_DIR}")
        return DISC_DIR
    legacy_iso = os.path.join(GAME_DIR, "disc.iso")
    if os.path.isfile(legacy_iso):
        return unpack_and_verify(legacy_iso, DISC_DIR)
    if interactive:
        print("  Unpacked Shadow Hearts disc not found in games\\sh1\\disc_files\\.")
        p = input("  Paste or drag-and-drop Shadow Hearts (USA) disc image (.iso/.chd/.cso/.cue/.bin): ").strip()
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
    subprocess.run(
        ["cmake", "-S", ROOT, "-B", TOOLS_BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=clang++"],
        cwd=ROOT,
        check=True,
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
    print("  Configuring Shadow Hearts 1 native CMake project...")
    subprocess.run(cmd, cwd=ROOT, check=True)


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


def stage_executables_and_dlls() -> None:
    src_exe = os.path.join(NATIVE_BUILD, "ps2run.exe")
    if not os.path.isfile(src_exe):
        raise RuntimeError(f"Expected built executable not found: {src_exe}")
    os.makedirs(os.path.dirname(FINAL_EXE), exist_ok=True)
    shutil.copy2(src_exe, FINAL_EXE)
    print(f"  Staged game executable:     {FINAL_EXE}")

    src_launcher = os.path.join(NATIVE_BUILD, "sh2launcher.exe")
    if os.path.isfile(src_launcher):
        try:
            shutil.copy2(src_launcher, LAUNCHER_EXE)
            print(f"  Staged launcher executable: {LAUNCHER_EXE}")
        except OSError as exc:
            print(f"  [warn] Could not overwrite {LAUNCHER_EXE} while open ({exc})")

    # Keep shared DLLs in root directory (passed via PATH by Shadow Hearts Nocturne.exe)
    def copy_dll_if_needed(src: str, dst: str) -> None:
        if not os.path.isfile(src):
            return
        if os.path.isfile(dst) and os.path.getsize(src) == os.path.getsize(dst):
            return
        try:
            shutil.copy2(src, dst)
        except OSError as exc:
            print(f"  [warn] Skipped locked DLL {os.path.basename(dst)} ({exc})")

    dxc_dir = os.path.join(ROOT, "third_party", "dxc")
    for dll in ("dxcompiler.dll", "dxil.dll"):
        copy_dll_if_needed(os.path.join(dxc_dir, dll), os.path.join(ROOT, dll))

    for cand in (
        os.path.join(NATIVE_BUILD, "SDL2.dll"),
        os.path.join(ROOT, "third_party", "sdl2", "bin", "SDL2.dll"),
    ):
        if os.path.isfile(cand):
            copy_dll_if_needed(cand, os.path.join(ROOT, "SDL2.dll"))
            break

    # Copy llvm-mingw runtime DLLs into ROOT so fresh clones launch cleanly on double-click
    mingw_bins = [
        os.path.join(ROOT, "tools", "portable", "llvm-mingw", "bin"),
        os.path.join(ROOT, "tools", "portable", "llvm-mingw", "x86_64-w64-mingw32", "bin"),
    ]
    for dll in ("libwinpthread-1.dll", "libc++.dll", "libunwind.dll"):
        found = None
        for bdir in mingw_bins:
            cand = os.path.join(bdir, dll)
            if os.path.isfile(cand):
                found = cand
                break
        if not found:
            w = shutil.which(dll)
            if w and os.path.isfile(w):
                found = w
        if found:
            copy_dll_if_needed(found, os.path.join(ROOT, dll))


def main() -> None:
    ap = argparse.ArgumentParser(description="Build Shadow Hearts (2001, SLUS-20347) Native PC Port")
    ap.add_argument("--disc", help="Path to Shadow Hearts (USA) disc image (.iso/.chd/.cso/.cue/.bin) or unpacked folder")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
    ap.add_argument("--jobs", type=int, default=3, help="Parallel Ninja compile jobs (default: 3)")
    args = ap.parse_args()

    overall_t0 = time.monotonic()
    print("=" * 78)
    print("  Shadow Hearts: Nocturne — Shadow Hearts (2001, SLUS-20347) PC Port Builder")
    print("=" * 78)

    # Step 1: Unpack & verify disc into games/sh1/disc_files/
    print("\n[1/5] Preparing ISO-free disc folder (games/sh1/disc_files/)...")
    disc_dir = resolve_disc(args.disc, interactive=not args.non_interactive)
    elf_path = find_boot_elf(disc_dir)

    # Step 2: Ensure third-party SDKs, recompiler tools & runtime library
    print("\n[2/5] Checking third-party SDKs, recompiler tools (eerecomp, vurecomp), and runtime...")
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
    print("\n[3/5] Translating SLUS_203.47 and recorded VU1/VU0 programs...")
    os.makedirs(GEN_DIR, exist_ok=True)
    eerecomp = os.path.join(TOOLS_BUILD, "eerecomp.exe")
    vurecomp = os.path.join(TOOLS_BUILD, "vurecomp.exe")

    ee_cmd = [eerecomp, elf_path, "--out", GEN_DIR]
    entries_txt = os.path.join(GAME_DIR, "entries.txt")
    if os.path.isfile(entries_txt):
        ee_cmd.extend(["--entries", entries_txt])
    t_ee = time.monotonic()
    res = subprocess.run(ee_cmd, cwd=ROOT, capture_output=True, text=True)
    if res.returncode != 0:
        if res.stdout:
            print(res.stdout, file=sys.stderr)
        if res.stderr:
            print(res.stderr, file=sys.stderr)
        raise RuntimeError(f"eerecomp failed with exit code {res.returncode}")
    for line in res.stdout.strip().splitlines():
        print(" ", line)
    print(f"  EE translation finished in {fmt_time(time.monotonic() - t_ee)}.")

    vu1_dir = os.path.join(GAME_DIR, "vu1")
    vu0_dir = os.path.join(GAME_DIR, "vu0")
    os.makedirs(vu1_dir, exist_ok=True)
    os.makedirs(vu0_dir, exist_ok=True)
    res_vu = subprocess.run([vurecomp, vu1_dir, GEN_DIR, "--vu0-dir", vu0_dir], cwd=ROOT, capture_output=True, text=True)
    if res_vu.returncode != 0:
        if res_vu.stdout:
            print(res_vu.stdout, file=sys.stderr)
        if res_vu.stderr:
            print(res_vu.stderr, file=sys.stderr)
        raise RuntimeError(f"vurecomp failed with exit code {res_vu.returncode}")
    for line in res_vu.stdout.strip().splitlines():
        print(" ", line)

    # Step 4: Compile native C++ units and link games/sh1/SH1.exe + Launcher
    print("\n[4/5] Compiling native C++ units and linking SH1.exe + Trilogy Launcher...")
    ensure_native_cmake(rt_lib)
    run_ninja_with_progress(
        ["cmake", "--build", NATIVE_BUILD, "--target", "ps2run", "sh2launcher", "--", f"-j{args.jobs}"],
        "Build",
    )

    # Step 5: Stage games/sh1/SH1.exe + Shadow Hearts Nocturne.exe
    print("\n[5/5] Staging games/sh1/SH1.exe & Shadow Hearts Nocturne.exe...")
    stage_executables_and_dlls()
    removed = clear_live_caches()
    print(f"  Cleared {removed} stale cache file(s) from ee-live / vu1-live.")

    print("\n" + "=" * 78)
    print(f"  DONE in {fmt_time(time.monotonic() - overall_t0)}!")
    print("  Launch 'Shadow Hearts Nocturne.exe' -> select Shadow Hearts -> PLAY SHADOW HEARTS.")
    print("=" * 78)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        sys.exit(1)
