#!/usr/bin/env python3
"""One-shot ahead-of-time builder for Shadow Hearts: Nocturne (Shadow Hearts: Covenant PC Port).

Unpacks Disc 1 (and optional Disc 2) from .iso/.chd/.cso/.bin/.cue into ISO-free
virtual disc folders (games/sh2-covenant/disc1_files and disc2_files) with
cross-disc NTFS hardlinking and bit-for-bit verification, fetches DXC for fast
Direct3D 12 shader compilation, translates 100% of EE code + all three MWo3 overlays
+ recorded VU1/VU0 vector programs, builds and packages "Shadow Hearts Nocturne.exe",
preserves memcard.ps2 & keys.ini, and clears stale ee-live / vu1-live caches.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)
GAME_DIR = os.path.join(ROOT, "games", "sh2-covenant")
DISC1_DIR = os.path.join(GAME_DIR, "disc1_files")
DISC2_DIR = os.path.join(GAME_DIR, "disc2_files")
GEN_DIR = os.path.join(ROOT, "generated", "sh2-covenant")
BUILD_DIR = os.path.join(ROOT, "build")
TOOLS_BUILD = os.path.join(BUILD_DIR, "tools")
NATIVE_BUILD = os.path.join(BUILD_DIR, "native-sh2-covenant")
FINAL_EXE = os.path.join(GAME_DIR, "SH2C.exe")
LAUNCHER_EXE = os.path.join(ROOT, "Shadow Hearts Nocturne.exe")

sys.path.insert(0, SCRIPT_DIR)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nocturne_ui as ui  # noqa: E402
import sh2_prepare_overlays  # noqa: E402
from ps2_disc_toolkit.cli import extract_disc, verify_disc  # noqa: E402


def configure_install_dir(install_dir: str) -> None:
    global GAME_DIR, DISC1_DIR, DISC2_DIR, FINAL_EXE, LAUNCHER_EXE
    install_dir = os.path.abspath(install_dir)
    GAME_DIR = os.path.join(install_dir, "games", "sh2-covenant")
    DISC1_DIR = os.path.join(GAME_DIR, "disc1_files")
    DISC2_DIR = os.path.join(GAME_DIR, "disc2_files")
    FINAL_EXE = os.path.join(GAME_DIR, "SH2C.exe")
    LAUNCHER_EXE = os.path.join(install_dir, "Shadow Hearts Nocturne.exe")


def fmt_time(sec: float) -> str:
    return ui.fmt_time(sec)


def fmt_bytes(n: int) -> str:
    if n >= (1 << 30):
        return f"{n / (1 << 30):.2f} GiB"
    return f"{n / (1 << 20):.1f} MiB"


def run_ninja_with_progress(cmd: list[str], label: str) -> None:
    ui.run_ninja_with_progress(cmd, label, cwd=ROOT)


def is_unpacked_disc(path: str | None) -> bool:
    return bool(path and os.path.isdir(path) and os.path.isfile(os.path.join(path, "disc_manifest.tsv")))


def is_inside_dir(path: str, parent: str) -> bool:
    try:
        return os.path.commonpath([os.path.abspath(path), os.path.abspath(parent)]) == os.path.abspath(parent)
    except ValueError:
        return False


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(4 * 1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def verify_unpacked(image_path: str, out_dir: str) -> None:
    try:
        verify_disc(image_path, out_dir, full=True)
    except SystemExit as exc:
        if exc.code not in (0, None):
            raise RuntimeError(f"Full bit-for-bit verification failed for {image_path}: {exc}") from exc


def ensure_chdunpack() -> str:
    exe_name = "chdunpack.exe" if os.name == "nt" else "chdunpack"
    cand = os.path.join(TOOLS_BUILD, exe_name)
    if os.path.isfile(cand):
        return cand
    ensure_tools_cmake()
    print("  Building native C++ chdunpack (multi-threaded ZSTD/LZMA/zlib .chd extractor)...")
    run_ninja_with_progress(["cmake", "--build", TOOLS_BUILD, "--target", "chdunpack"], "chdunpack")
    if not os.path.isfile(cand):
        raise RuntimeError(f"Failed to build {exe_name} in {TOOLS_BUILD}")
    return cand


def unpack_and_verify(src_path: str, out_dir: str, label: str) -> tuple[str, bool]:
    """Unpack a disc image (.iso/.chd/.cso/.bin/.cue) into out_dir and verify bit-for-bit.
    Returns (resolved_dir, unpacked_from_image).
    """
    src = src_path.strip().strip('"').strip("'")
    if is_unpacked_disc(src):
        if os.path.abspath(src).lower() == os.path.abspath(out_dir).lower():
            return out_dir, False
        print(f"  Using existing unpacked {label} directory: {src}")
        return os.path.abspath(src), False
    if not os.path.isfile(src):
        raise RuntimeError(f"{label} image not found: {src}")
    os.makedirs(GAME_DIR, exist_ok=True)

    # Native C++ chdunpack supports ZSTD, LZMA, zlib, and FLAC .chd images on all cores without Python zstandard.
    temp_iso: str | None = None
    work_src = src
    if src.lower().endswith(".chd"):
        chdunpack = ensure_chdunpack()
        safe_tag = re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_") or "disc"
        temp_iso = os.path.join(GAME_DIR, f".tmp_{safe_tag}_chd.iso")
        print(f"  Decompressing {label} .chd via native chdunpack -> {os.path.basename(temp_iso)} ...")
        proc = subprocess.run([chdunpack, src, temp_iso], cwd=ROOT)
        if proc.returncode != 0 or not os.path.isfile(temp_iso):
            raise RuntimeError(f"chdunpack failed for {src} (exit code {proc.returncode})")
        work_src = temp_iso

    try:
        print(f"  Unpacking {label} from {work_src} -> {out_dir} ...")
        extract_disc(work_src, out_dir, skip_existing=True)
        print(f"  Verifying {label} bit-for-bit against source image...")
        verify_unpacked(work_src, out_dir)
    finally:
        if temp_iso and os.path.exists(temp_iso):
            try:
                os.remove(temp_iso)
            except OSError:
                pass
    return out_dir, True


def dedupe_disc_folders(disc1_dir: str, disc2_dir: str) -> tuple[int, int]:
    """Hardlink byte-identical extracted files in disc2_dir to disc1_dir (zero duplicate space on NTFS/ext4)."""
    if not (is_unpacked_disc(disc1_dir) and is_unpacked_disc(disc2_dir)):
        return 0, 0
    if os.path.abspath(disc1_dir).lower() == os.path.abspath(disc2_dir).lower():
        return 0, 0
    linked_files = 0
    saved_bytes = 0
    for sub in ("layer0", "layer1", "unindexed"):
        root2 = os.path.join(disc2_dir, sub)
        if not os.path.isdir(root2):
            continue
        for dirpath, _, filenames in os.walk(root2):
            for fn in filenames:
                p2 = os.path.join(dirpath, fn)
                rel = os.path.relpath(p2, disc2_dir)
                p1 = os.path.join(disc1_dir, rel)
                if not os.path.isfile(p1):
                    continue
                try:
                    st1 = os.stat(p1)
                    st2 = os.stat(p2)
                except OSError:
                    continue
                if st1.st_size <= 0 or st1.st_size != st2.st_size:
                    continue
                try:
                    if os.path.samefile(p1, p2):
                        continue
                except OSError:
                    pass
                if sha256_file(p1) != sha256_file(p2):
                    continue
                tmp = p2 + ".hl_tmp"
                try:
                    if os.path.exists(tmp):
                        os.remove(tmp)
                    os.link(p1, tmp)
                    os.replace(tmp, p2)
                    linked_files += 1
                    saved_bytes += st1.st_size
                except OSError:
                    if os.path.exists(tmp):
                        try:
                            os.remove(tmp)
                        except OSError:
                            pass
    if linked_files:
        print(f"  [dedupe] Hardlinked {linked_files} identical file(s) between Disc 1 & Disc 2 (saved {fmt_bytes(saved_bytes)})")
    return linked_files, saved_bytes


def find_cue_companion_bins(cue_path: str) -> list[str]:
    bins: list[str] = []
    if not cue_path.lower().endswith(".cue") or not os.path.isfile(cue_path):
        return bins
    cue_dir = os.path.dirname(os.path.abspath(cue_path))
    try:
        text = open(cue_path, "r", encoding="utf-8", errors="ignore").read()
    except OSError:
        return bins
    for m in re.finditer(r'^\s*FILE\s+(?:"([^"]+)"|(\S+))', text, re.IGNORECASE | re.MULTILINE):
        ref = m.group(1) or m.group(2)
        if not ref:
            continue
        cand = os.path.join(cue_dir, ref)
        if os.path.isfile(cand):
            bins.append(os.path.abspath(cand))
    return bins


def ensure_unpacked_discs(
    disc1_arg: str | None,
    disc2_arg: str | None,
    interactive: bool,
) -> tuple[str, str | None, list[str]]:
    """Ensure Disc 1 (and optional Disc 2) exist as unpacked virtual disc folders.
    Never copies source ISOs into games/sh2-covenant/. Auto-upgrades & removes legacy
    games/sh2-covenant/disc.iso and disc2.iso after bit-for-bit verification.
    Returns (disc1_dir, disc2_dir_or_None, verified_external_source_images).
    """
    legacy_d1 = os.path.join(GAME_DIR, "disc.iso")
    legacy_d2 = os.path.join(GAME_DIR, "disc2.iso")
    verified_external: list[str] = []

    # --- Disc 1 ---
    disc1_dir = DISC1_DIR
    legacy_d1_verified = False
    if disc1_arg:
        src1 = disc1_arg.strip().strip('"').strip("'")
        disc1_dir, from_img = unpack_and_verify(src1, DISC1_DIR, "Disc 1")
        if from_img and os.path.abspath(src1).lower() == os.path.abspath(legacy_d1).lower():
            legacy_d1_verified = True
        if from_img and not is_inside_dir(src1, GAME_DIR):
            verified_external.append(os.path.abspath(src1))
    elif is_unpacked_disc(DISC1_DIR):
        print(f"  Using unpacked Disc 1 folder: {DISC1_DIR}")
    elif os.path.isfile(legacy_d1):
        print("  Upgrading legacy games\\sh2-covenant\\disc.iso to ISO-free disc1_files\\...")
        disc1_dir, _ = unpack_and_verify(legacy_d1, DISC1_DIR, "Disc 1")
        legacy_d1_verified = True
    else:
        if not interactive:
            raise RuntimeError(
                f"Missing unpacked Disc 1 in {DISC1_DIR}. Pass --disc1 <path-to-iso/chd/cso/cue/bin>."
            )
        print("  Unpacked Disc 1 not found in games\\sh2-covenant\\disc1_files\\.")
        p1 = input("  Paste or drag-and-drop Shadow Hearts: Covenant (USA) Disc 1 (.iso/.chd/.cso/.cue/.bin): ").strip()
        src1 = p1.strip('"').strip("'")
        disc1_dir, from_img = unpack_and_verify(src1, DISC1_DIR, "Disc 1")
        if from_img and not is_inside_dir(src1, GAME_DIR):
            verified_external.append(os.path.abspath(src1))

    # Clean up legacy internal games/sh2-covenant/disc.iso once disc1_files is verified bit-for-bit
    if os.path.isfile(legacy_d1) and is_unpacked_disc(disc1_dir):
        try:
            if not legacy_d1_verified:
                verify_unpacked(legacy_d1, disc1_dir)
            os.remove(legacy_d1)
            print("  Removed redundant legacy copy: games\\sh2-covenant\\disc.iso")
        except Exception as exc:
            print(f"  [warn] Kept legacy disc.iso ({exc})")

    # --- Disc 2 (optional) ---
    disc2_dir: str | None = None
    legacy_d2_verified = False
    if disc2_arg:
        src2 = disc2_arg.strip().strip('"').strip("'")
        disc2_dir, from_img = unpack_and_verify(src2, DISC2_DIR, "Disc 2")
        if from_img and os.path.abspath(src2).lower() == os.path.abspath(legacy_d2).lower():
            legacy_d2_verified = True
        if from_img and not is_inside_dir(src2, GAME_DIR):
            verified_external.append(os.path.abspath(src2))
    elif is_unpacked_disc(DISC2_DIR):
        disc2_dir = DISC2_DIR
        print(f"  Using unpacked Disc 2 folder: {DISC2_DIR}")
    elif os.path.isfile(legacy_d2):
        print("  Upgrading legacy games\\sh2-covenant\\disc2.iso to ISO-free disc2_files\\...")
        disc2_dir, _ = unpack_and_verify(legacy_d2, DISC2_DIR, "Disc 2")
        legacy_d2_verified = True
    elif interactive:
        print("  Unpacked Disc 2 not found in games\\sh2-covenant\\disc2_files\\.")
        p2 = input("  Paste or drag-and-drop Disc 2 (.iso/.chd/.cso/.cue/.bin, or press Enter to skip): ").strip()
        if p2:
            src2 = p2.strip('"').strip("'")
            disc2_dir, from_img = unpack_and_verify(src2, DISC2_DIR, "Disc 2")
            if from_img and not is_inside_dir(src2, GAME_DIR):
                verified_external.append(os.path.abspath(src2))

    # Clean up legacy internal games/sh2-covenant/disc2.iso once disc2_files is verified bit-for-bit
    if disc2_dir and os.path.isfile(legacy_d2) and is_unpacked_disc(disc2_dir):
        try:
            if not legacy_d2_verified:
                verify_unpacked(legacy_d2, disc2_dir)
            os.remove(legacy_d2)
            print("  Removed redundant legacy copy: games\\sh2-covenant\\disc2.iso")
        except Exception as exc:
            print(f"  [warn] Kept legacy disc2.iso ({exc})")

    if disc2_dir and is_unpacked_disc(disc1_dir) and is_unpacked_disc(disc2_dir):
        # Auto-swap if the user accidentally supplied Disc 2 first and Disc 1 second
        try:
            b1 = sh2_prepare_overlays.detect_boot_name(disc1_dir)
            b2 = sh2_prepare_overlays.detect_boot_name(disc2_dir)
            if b1 == "SLUS_210.44" and b2 == "SLUS_210.41":
                print("  [note] Detected Disc 2 and Disc 1 in reversed order — swapping automatically!")
                if (
                    os.path.abspath(disc1_dir).lower() == os.path.abspath(DISC1_DIR).lower()
                    and os.path.abspath(disc2_dir).lower() == os.path.abspath(DISC2_DIR).lower()
                ):
                    tmp_swap = os.path.join(GAME_DIR, ".swap_disc_tmp")
                    if os.path.exists(tmp_swap):
                        shutil.rmtree(tmp_swap, ignore_errors=True)
                    os.rename(DISC1_DIR, tmp_swap)
                    os.rename(DISC2_DIR, DISC1_DIR)
                    os.rename(tmp_swap, DISC2_DIR)
                else:
                    disc1_dir, disc2_dir = disc2_dir, disc1_dir
        except Exception:
            pass
        dedupe_disc_folders(disc1_dir, disc2_dir)

    return disc1_dir, disc2_dir, verified_external


def maybe_offer_delete_sources(sources: list[str], interactive: bool, force_delete: bool | None = None) -> None:
    if force_delete is False or not sources:
        return
    if force_delete is None and not interactive:
        return
    targets: list[str] = []
    seen: set[str] = set()
    for src in sources:
        for p in [src] + find_cue_companion_bins(src):
            key = os.path.abspath(p).lower()
            if key not in seen and os.path.isfile(p):
                seen.add(key)
                targets.append(os.path.abspath(p))
    if not targets:
        return
    total_bytes = sum(os.path.getsize(p) for p in targets)
    do_delete = bool(force_delete)
    if force_delete is None:
        print("\n  Your disc image(s) have been unpacked and verified bit-for-bit.")
        print(f"  You no longer need the original image(s) to play ({fmt_bytes(total_bytes)} can be freed):")
        for p in targets:
            print(f"    - {p} ({fmt_bytes(os.path.getsize(p))})")
        try:
            ans = input("  Delete original source image(s) now? [y/N]: ").strip().lower()
        except EOFError:
            ans = ""
        do_delete = ans in ("y", "yes")
    if do_delete:
        for p in targets:
            try:
                os.remove(p)
                print(f"  Deleted: {p}")
            except OSError as exc:
                print(f"  [warn] Could not delete {p}: {exc}")
    else:
        print("  Kept original source image(s).")


def find_runtime_build_dir() -> str | None:
    matches = sorted(glob.glob(os.path.join(BUILD_DIR, "runtime-native-*")))
    return matches[-1] if matches else None


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


def ensure_tools_cmake() -> None:
    if os.path.isfile(os.path.join(TOOLS_BUILD, "build.ninja")):
        return
    os.makedirs(TOOLS_BUILD, exist_ok=True)
    cmd = [
        "cmake", "-S", ROOT, "-B", TOOLS_BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=clang++",
    ]
    ui.run_spinner_cmd(cmd, "CMake (Tools)", "Configuring recompiler tools...")


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
        "-DCMAKE_CXX_COMPILER=clang++",
        f"-DPS2_GENERATED={GEN_DIR}",
    ] + extra_flags
    if runtime_lib:
        cmd.append(f"-DPS2_RUNTIME_LIB={runtime_lib}")
    ui.run_spinner_cmd(cmd, "CMake (SH2)", "Configuring Covenant native build...")


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


def check_exes_not_locked(game_dll_only: bool = False) -> None:
    targets = (
        (os.path.join(NATIVE_BUILD, "game.dll"), os.path.join(GAME_DIR, "game.dll"))
        if game_dll_only
        else (os.path.join(NATIVE_BUILD, "ps2run.exe"), os.path.join(NATIVE_BUILD, "sh2launcher.exe"), FINAL_EXE, LAUNCHER_EXE)
    )
    for exe in targets:
        if not os.path.isfile(exe):
            continue
        try:
            with open(exe, "r+b"):
                pass
        except PermissionError:
            raise RuntimeError(
                f"{os.path.basename(exe)} is currently running! Please close Shadow Hearts: Covenant / Nocturne and re-run this builder."
            )


def stage_executable_and_dlls(install_dir: str) -> None:
    """Copy ps2run.exe -> '<install_dir>/games/sh2-covenant/SH2C.exe' and sh2launcher.exe -> '<install_dir>/Shadow Hearts Nocturne.exe' along with shared DLLs."""
    src_exe = os.path.join(NATIVE_BUILD, "ps2run.exe")
    if not os.path.isfile(src_exe):
        raise RuntimeError(f"Expected built game executable not found: {src_exe}")
    os.makedirs(os.path.dirname(FINAL_EXE), exist_ok=True)
    shutil.copy2(src_exe, FINAL_EXE)
    print(f"  Staged game executable:     {FINAL_EXE}")

    src_launcher = os.path.join(NATIVE_BUILD, "sh2launcher.exe")
    if not os.path.isfile(src_launcher):
        raise RuntimeError(f"Expected built launcher executable not found: {src_launcher}")
    os.makedirs(install_dir, exist_ok=True)
    shutil.copy2(src_launcher, LAUNCHER_EXE)
    print(f"  Staged launcher executable: {LAUNCHER_EXE}")

    # Ensure DXC DLLs sit next to ps2run.exe and shared in install_dir
    dxc_dir = os.path.join(ROOT, "third_party", "dxc")
    for dll in ("dxcompiler.dll", "dxil.dll"):
        src = os.path.join(dxc_dir, dll)
        if os.path.isfile(src):
            ui.copy_dll_if_needed(src, os.path.join(NATIVE_BUILD, dll))

    ui.stage_shared_runtime_dlls(install_dir, build_dir=NATIVE_BUILD)

    # Auto-migrate legacy pre-Trilogy root layout (old root EXE, mods/, dumps/, and screenshots)
    legacy_exe = os.path.join(ROOT, "Shadow Hearts Covenant.exe")
    if os.path.isfile(legacy_exe) and os.path.isfile(FINAL_EXE):
        try:
            os.remove(legacy_exe)
            print("  Cleaned up legacy root Shadow Hearts Covenant.exe (now games/sh2-covenant/SH2C.exe)")
        except OSError:
            pass
    for sub in ("mods", "dumps"):
        old_dir = os.path.join(ROOT, sub)
        new_dir = os.path.join(GAME_DIR, sub)
        if os.path.isdir(old_dir) and not os.path.exists(new_dir):
            try:
                shutil.move(old_dir, new_dir)
                print(f"  Migrated legacy {sub}/ -> games/sh2-covenant/{sub}/")
            except OSError as exc:
                print(f"  [warn] Could not migrate {sub}/ ({exc})")
    if os.name == "nt":
        prof = os.environ.get("USERPROFILE", "")
        old_shots = os.path.join(prof, "Pictures", "Game Screenshots") if prof else ""
        new_shots = os.path.join(prof, "Pictures", "Shadow Hearts Nocturne", "Covenant") if prof else ""
        if old_shots and os.path.isdir(old_shots):
            moved_shots = 0
            for fn in os.listdir(old_shots):
                if fn.startswith(("Achievement_", "ShadowHearts")) and fn.lower().endswith(".png"):
                    os.makedirs(new_shots, exist_ok=True)
                    try:
                        shutil.move(os.path.join(old_shots, fn), os.path.join(new_shots, fn))
                        moved_shots += 1
                    except OSError:
                        pass
            if moved_shots:
                print(f"  Migrated {moved_shots} Covenant screenshot(s) -> Pictures/Shadow Hearts Nocturne/Covenant/")


def ensure_portable_toolchain_on_path() -> None:
    portable = os.path.join(ROOT, "tools", "portable")
    extra = [
        os.path.join(portable, "llvm-mingw", "bin"),
        os.path.join(portable, "cmake", "bin"),
        os.path.join(portable, "ninja"),
        os.path.join(portable, "python"),
        os.path.join(portable, "mingit", "cmd"),
        os.path.join(portable, "mingit", "usr", "bin"),
    ]
    existing = [p for p in extra if os.path.isdir(p)]
    if existing:
        cur = os.environ.get("PATH", "")
        os.environ["PATH"] = os.pathsep.join(existing) + os.pathsep + cur


def main() -> int:
    ensure_portable_toolchain_on_path()
    rec_jobs, _ = ui.recommended_compile_jobs()
    ap = argparse.ArgumentParser(description="Build Shadow Hearts: Nocturne (Shadow Hearts: Covenant PC Port)")
    ap.add_argument("--disc1", help="Path to Disc 1 image (.iso/.chd/.cso/.cue/.bin) or unpacked disc1_files folder")
    ap.add_argument("--disc2", help="Path to Disc 2 image (.iso/.chd/.cso/.cue/.bin) or unpacked disc2_files folder")
    ap.add_argument("--install-dir", default=None, help="Target install folder (default: saved install_config or repo root)")
    ap.add_argument("--jobs", type=int, default=rec_jobs, help=f"Parallel Ninja compile jobs (auto-recommended: {rec_jobs})")
    ap.add_argument("--non-interactive", action="store_true", help="Fail instead of prompting for missing discs")
    ap.add_argument("--delete-sources", action="store_true", help="Delete source disc images after bit-exact verification")
    ap.add_argument("--keep-sources", action="store_true", help="Keep source disc images without prompting")
    ap.add_argument("--skip-app-check", action="store_true", help="Skip running-app prompt (already checked by caller)")
    ap.add_argument("--game-dll-only", action="store_true", help="Only build & stage games/sh2-covenant/game.dll (skip shared engine build)")
    args = ap.parse_args()

    overall_t0 = time.monotonic()
    install_dir = ui.resolve_install_dir(args.install_dir)
    configure_install_dir(install_dir)
    interactive = not args.non_interactive

    ui.print_banner(
        "Shadow Hearts: Nocturne — Shadow Hearts: Covenant (1915, 2 Discs) Builder",
        f"Install folder: {install_dir}  |  Compile jobs: {args.jobs}  |  Preserves saves & keys.ini",
    )

    if not args.skip_app_check:
        if not ui.ensure_apps_closed(
            ["Shadow Hearts Nocturne.exe", "sh2launcher.exe", "SH2C.exe"],
            interactive=interactive,
        ):
            return ui.EXIT_USER_CANCELLED

    check_exes_not_locked(game_dll_only=args.game_dll_only)

    # Step 1: Unpack & verify virtual disc folders, then extract SLUS_210.41 + overlays
    ui.print_section("[1/5] Preparing unpacked virtual disc folders and extracting overlays...")
    disc1_dir, disc2_dir, verified_external = ensure_unpacked_discs(
        args.disc1, args.disc2, interactive=interactive
    )
    sh2_prepare_overlays.prepare(GAME_DIR, disc1_dir, disc2_dir)

    # Step 2: Build tools & shared runtime
    ui.print_section("[2/5] Building recompiler tools and PS2 runtime...")
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

    # Step 3: Translate EE code (main ELF + 100% of OPENING.BIN, FIELD.BIN, BATTLE.BIN) & VU1/VU0 programs
    ui.print_section("[3/5] Translating main executable, all 3 overlays, and vector programs...")
    os.makedirs(GEN_DIR, exist_ok=True)
    elf_path = os.path.join(GAME_DIR, "disc", "SLUS_210.41")
    eerecomp = os.path.join(TOOLS_BUILD, "eerecomp.exe")
    vurecomp = os.path.join(TOOLS_BUILD, "vurecomp.exe")

    ui.run_spinner_cmd(
        [
            eerecomp, elf_path,
            "--entries", os.path.join(GAME_DIR, "entries.txt"),
            "--overlays", os.path.join(GAME_DIR, "overlays"),
            "--out", GEN_DIR,
        ],
        "EE Recomp",
        "Translating SLUS_210.41 + OPENING/FIELD/BATTLE overlays -> C++20...",
        echo_stdout=True,
    )

    vu1_dir = os.path.join(GAME_DIR, "vu1")
    vu0_dir = os.path.join(GAME_DIR, "vu0")
    os.makedirs(vu1_dir, exist_ok=True)
    os.makedirs(vu0_dir, exist_ok=True)
    vu1_bins = glob.glob(os.path.join(vu1_dir, "*.bin"))
    vu0_bins = glob.glob(os.path.join(vu0_dir, "*.bin"))
    if vu1_bins or vu0_bins:
        ui.run_spinner_cmd(
            [vurecomp, vu1_dir, GEN_DIR, "--vu0-dir", vu0_dir],
            "VU Recomp",
            "Translating recorded VU1/VU0 microprograms...",
            echo_stdout=True,
        )
    else:
        for old_vu in glob.glob(os.path.join(GEN_DIR, "vu1_*.cpp")):
            os.remove(old_vu)
        print("  No recorded VU1/VU0 programs yet (vu-live will record and compile them automatically).")

    # Step 4: Compile native C++ game module (games/sh2-covenant/game.dll) + shared engine
    ui.print_section(f"[4/5] Compiling SH2 Covenant game.dll (-j {args.jobs})...")
    ensure_native_cmake(rt_lib)
    run_ninja_with_progress(["cmake", "--build", NATIVE_BUILD, "--target", "ps2_game_dll", "--", f"-j{args.jobs}"], "SH2 Game DLL")

    # Step 5: Stage games/sh2-covenant/game.dll + shared engine & clear stale live caches
    ui.print_section(f"[5/5] Staging games/sh2-covenant/game.dll -> {install_dir}...")
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
            print("  Migrated & cleaned up legacy games/sh2-covenant/SH2C.exe (now using shared engine + game.dll)")
        except OSError:
            pass
    removed = clear_live_caches()
    print(f"  Removed {removed} stale cache file(s) from ee-live / vu1-live.")
    print(f"  memcard.ps2 preserved: {'yes' if os.path.isfile(os.path.join(GAME_DIR, 'memcard.ps2')) else 'new card on launch'}")
    print(f"  keys.ini preserved:    {'yes' if os.path.isfile(os.path.join(GAME_DIR, 'keys.ini')) else 'custom defaults on launch'}")

    del_flag = True if args.delete_sources else (False if args.keep_sources else None)
    maybe_offer_delete_sources(verified_external, interactive=interactive, force_delete=del_flag)

    ui.print_banner(
        f"DONE in {fmt_time(time.monotonic() - overall_t0)}!",
        "Launch 'Shadow Hearts Nocturne.exe' -> select Covenant -> PLAY COVENANT.",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)

