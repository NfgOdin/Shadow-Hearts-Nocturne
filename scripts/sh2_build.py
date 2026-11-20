#!/usr/bin/env python3
"""One-shot ahead-of-time builder for Shadow Hearts: Covenant PC Port.

Extracts SLUS_210.41 + all three MWo3 overlays (OPENING.BIN, FIELD.BIN, BATTLE.BIN),
translates 100% of EE code + all recorded VU1/VU0 vector programs, compiles and links
ps2run.exe with a live console progress bar, preserves memcard.ps2 & keys.ini, and
clears stale ee-live / vu1-live DLL caches.
"""
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
GAME_DIR = os.path.join(ROOT, "games", "sh2-covenant")
GEN_DIR = os.path.join(ROOT, "generated", "sh2-covenant")
BUILD_DIR = os.path.join(ROOT, "build")
TOOLS_BUILD = os.path.join(BUILD_DIR, "tools")
NATIVE_BUILD = os.path.join(BUILD_DIR, "native-sh2-covenant")

sys.path.insert(0, SCRIPT_DIR)
import sh2_prepare_overlays  # noqa: E402


def fmt_time(sec: float) -> str:
    sec = max(0, int(sec))
    m, s = divmod(sec, 60)
    return f"{m}m {s:02d}s" if m else f"{s}s"


def draw_bar(label: str, current: int, total: int, elapsed: float, detail: str = "") -> None:
    pct = (100.0 * current / total) if total > 0 else 0.0
    width = 28
    filled = int(width * current / total) if total > 0 else 0
    bar = "#" * filled + "-" * (width - filled)
    eta = ""
    if current > 0 and total > current:
        rem = elapsed * (total - current) / current
        eta = f" | ETA {fmt_time(rem)}"
    line = f"\r  {label} [{bar}] {pct:5.1f}% ({current}/{total}) | {fmt_time(elapsed)}{eta}"
    if detail:
        line += f" | {detail}"
    # Pad to overwrite previous longer line
    sys.stdout.write(line[:118].ljust(118))
    sys.stdout.flush()


def run_ninja_with_progress(cmd: list[str], label: str) -> None:
    t0 = time.monotonic()
    pat = re.compile(r"^\[(\d+)/(\d+)\]\s*(.*)$")
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
    saw_progress = False
    error_lines: list[str] = []
    last_cur, last_tot = 0, 0
    assert proc.stdout is not None
    for raw in proc.stdout:
        line = raw.rstrip("\r\n")
        m = pat.match(line)
        if m:
            cur, tot, msg = int(m.group(1)), int(m.group(2)), m.group(3)
            last_cur, last_tot = cur, tot
            saw_progress = True
            short = msg.split("/")[-1] if "/" in msg else msg
            draw_bar(label, cur, tot, time.monotonic() - t0, short[:28])
        else:
            if line and not line.startswith("ninja: no work to do"):
                if "error:" in line.lower() or "failed" in line.lower() or "fatal" in line.lower():
                    error_lines.append(line)
            if line.startswith("ninja: no work to do"):
                print(f"  {label}: already up to date.")
    rc = proc.wait()
    if saw_progress:
        if rc == 0 and last_tot > 0:
            draw_bar(label, last_tot, last_tot, time.monotonic() - t0, "done")
        sys.stdout.write("\n")
        sys.stdout.flush()
    if rc != 0:
        for e in error_lines[-20:]:
            print("  ERR:", e)
        raise RuntimeError(f"Command failed with exit code {rc}: {' '.join(cmd)}")


def link_or_copy_iso(src: str, dst: str, label: str) -> None:
    src = src.strip().strip('"').strip("'")
    if not os.path.isfile(src):
        raise RuntimeError(f"{label} file not found: {src}")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.abspath(src).lower() == os.path.abspath(dst).lower():
        return
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
        print(f"  Linked {label} -> {dst} (zero extra disk space)")
        return
    except OSError:
        pass
    total = os.path.getsize(src)
    copied = 0
    t0 = time.monotonic()
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        while True:
            buf = fin.read(8 * 1024 * 1024)
            if not buf:
                break
            fout.write(buf)
            copied += len(buf)
            draw_bar(f"Copy {label}", copied >> 20, max(1, total >> 20), time.monotonic() - t0, "MiB")
    sys.stdout.write("\n")


def ensure_isos(disc1_arg: str | None, disc2_arg: str | None, interactive: bool) -> tuple[str, str | None]:
    disc1 = os.path.join(GAME_DIR, "disc.iso")
    disc2 = os.path.join(GAME_DIR, "disc2.iso")

    if disc1_arg:
        link_or_copy_iso(disc1_arg, disc1, "Disc 1")
    if disc2_arg:
        link_or_copy_iso(disc2_arg, disc2, "Disc 2")

    if not os.path.isfile(disc1):
        if not interactive:
            raise RuntimeError(f"Missing {disc1}. Pass --disc1 <path> or place disc.iso in games\\sh2-covenant\\.")
        print("  Disc 1 ISO not found in games\\sh2-covenant\\disc.iso.")
        p1 = input("  Paste or drag-and-drop Shadow Hearts: Covenant (USA) Disc 1 .iso path: ").strip()
        link_or_copy_iso(p1, disc1, "Disc 1")

    if not os.path.isfile(disc2):
        # Try auto-detecting sibling Disc 2 if Disc 1 was passed on CLI
        if interactive:
            print("  Disc 2 ISO not found in games\\sh2-covenant\\disc2.iso.")
            p2 = input("  Paste or drag-and-drop Disc 2 .iso path (or press Enter to skip): ").strip()
            if p2:
                link_or_copy_iso(p2, disc2, "Disc 2")

    return disc1, disc2 if os.path.isfile(disc2) else None


def find_runtime_build_dir() -> str | None:
    matches = sorted(glob.glob(os.path.join(BUILD_DIR, "runtime-native-*")))
    return matches[-1] if matches else None


def ensure_native_cmake(runtime_lib: str | None) -> None:
    if os.path.isfile(os.path.join(NATIVE_BUILD, "build.ninja")):
        return
    flags_file = os.path.join(BUILD_DIR, "cxx_flags")
    extra_flags = []
    if os.path.isfile(flags_file):
        extra_flags = open(flags_file, "r", encoding="utf-8").read().split()
    cmd = [
        "cmake", "-S", ROOT, "-B", NATIVE_BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DPS2_GENERATED={GEN_DIR}",
    ] + extra_flags
    if runtime_lib:
        cmd.append(f"-DPS2_RUNTIME_LIB={runtime_lib}")
    print("  Configuring native CMake project...")
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
                    if entry.startswith("work-"):
                        shutil.rmtree(full, ignore_errors=True)
                        removed += 1
                elif entry.endswith((".dll", ".mark")):
                    os.remove(full)
                    removed += 1
            except OSError:
                pass
    return removed


def check_ps2run_not_locked() -> None:
    exe = os.path.join(NATIVE_BUILD, "ps2run.exe")
    if not os.path.isfile(exe):
        return
    try:
        with open(exe, "r+b"):
            pass
    except PermissionError:
        raise RuntimeError(
            "ps2run.exe is currently running! Please close the Shadow Hearts: Covenant window and re-run this builder."
        )


def main() -> None:
    ap = argparse.ArgumentParser(description="Build Shadow Hearts: Covenant PC Port (full AOT)")
    ap.add_argument("--disc1", help="Path to Disc 1 ISO")
    ap.add_argument("--disc2", help="Path to Disc 2 ISO")
    ap.add_argument("--non-interactive", action="store_true", help="Fail instead of prompting for missing ISOs")
    args = ap.parse_args()

    overall_t0 = time.monotonic()
    print("=" * 78)
    print("  Shadow Hearts: Covenant (NTSC-U) - Full AOT PC Port Builder")
    print("  Preserves your saves (memcard.ps2) and keybindings (keys.ini)")
    print("=" * 78)

    check_ps2run_not_locked()

    # Step 1: Locate/verify ISOs & extract overlays
    print("\n[1/5] Checking ISOs and extracting SLUS_210.41 + all 3 overlays...")
    disc1, disc2 = ensure_isos(args.disc1, args.disc2, interactive=not args.non_interactive)
    sh2_prepare_overlays.prepare(GAME_DIR, disc1, disc2)

    # Step 2: Build tools & shared runtime
    print("\n[2/5] Building recompiler tools and PS2 runtime...")
    if os.path.isdir(TOOLS_BUILD):
        run_ninja_with_progress(["cmake", "--build", TOOLS_BUILD, "--target", "eerecomp", "vurecomp"], "Tools")
    rt_dir = find_runtime_build_dir()
    rt_lib = None
    if rt_dir:
        run_ninja_with_progress(["cmake", "--build", rt_dir, "--target", "ps2runtime"], "Runtime")
        cand = os.path.join(rt_dir, "libps2runtime.a")
        if os.path.isfile(cand):
            rt_lib = cand

    # Step 3: Translate EE code (main ELF + 100% of OPENING.BIN, FIELD.BIN, BATTLE.BIN) & VU1/VU0 programs
    print("\n[3/5] Translating main executable, all 3 overlays, and vector programs...")
    os.makedirs(GEN_DIR, exist_ok=True)
    elf_path = os.path.join(GAME_DIR, "disc", "SLUS_210.41")
    eerecomp = os.path.join(TOOLS_BUILD, "eerecomp.exe")
    vurecomp = os.path.join(TOOLS_BUILD, "vurecomp.exe")

    t_ee = time.monotonic()
    res = subprocess.run(
        [
            eerecomp, elf_path,
            "--entries", os.path.join(GAME_DIR, "entries.txt"),
            "--overlays", os.path.join(GAME_DIR, "overlays"),
            "--out", GEN_DIR,
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    for line in res.stdout.strip().splitlines():
        print(" ", line)
    print(f"  EE translation finished in {fmt_time(time.monotonic() - t_ee)}.")

    vu1_dir = os.path.join(GAME_DIR, "vu1")
    vu0_dir = os.path.join(GAME_DIR, "vu0")
    vu1_bins = glob.glob(os.path.join(vu1_dir, "*.bin"))
    vu0_bins = glob.glob(os.path.join(vu0_dir, "*.bin"))
    if vu1_bins or vu0_bins:
        res_vu = subprocess.run(
            [vurecomp, vu1_dir, GEN_DIR, "--vu0-dir", vu0_dir],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        for line in res_vu.stdout.strip().splitlines():
            print(" ", line)
    else:
        for old_vu in glob.glob(os.path.join(GEN_DIR, "vu1_*.cpp")):
            os.remove(old_vu)
        print("  No recorded VU1/VU0 programs yet (vu-live will record and compile them automatically).")

    # Step 4: Compile all C++ units & link ps2run.exe with live progress bar
    print("\n[4/5] Compiling native C++ units and linking ps2run.exe...")
    ensure_native_cmake(rt_lib)
    run_ninja_with_progress(["cmake", "--build", NATIVE_BUILD, "--target", "ps2run"], "Build")

    # Step 5: Clear stale live caches
    print("\n[5/5] Finalizing and clearing stale live-compile caches...")
    removed = clear_live_caches()
    print(f"  Removed {removed} stale cache file(s) from ee-live / vu1-live.")
    print(f"  memcard.ps2 preserved: {'yes' if os.path.isfile(os.path.join(GAME_DIR, 'memcard.ps2')) else 'new card on launch'}")
    print(f"  keys.ini preserved:    {'yes' if os.path.isfile(os.path.join(GAME_DIR, 'keys.ini')) else 'custom defaults on launch'}")

    print("\n" + "=" * 78)
    print(f"  DONE in {fmt_time(time.monotonic() - overall_t0)}! Launch 'Play Shadow Hearts Covenant.cmd' to play.")
    print("=" * 78)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        sys.exit(1)
