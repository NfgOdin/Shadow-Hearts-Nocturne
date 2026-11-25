#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Update Checker & Smart Incremental Auto-Updater.

Checks the latest GitHub commit on the user's chosen release channel:
  - 'main'    (Stable, playtest-verified releases)
  - 'nightly' (Opt-in latest features, Koudelka updates & fixes)

Works seamlessly on BOTH Git clones and standalone ZIP / bootstrapper installs.
Preserves all saves, keys.ini, mods, unpacked virtual discs, and portable tools,
and incrementally rebuilds ONLY the games you actually have installed.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
REPO_SLUG = "NfgOdin/Shadow-Hearts-Nocturne"
VALID_CHANNELS = ("main", "nightly")

sys.path.insert(0, SCRIPT_DIR)
import nocturne_setup as setup  # noqa: E402
import nocturne_ui as ui  # noqa: E402


def _git_cmd(args: list[str]) -> str:
    git_exe = shutil.which("git") or os.path.join(REPO_ROOT, "tools", "portable", "mingit", "cmd", "git.exe")
    if not os.path.isdir(os.path.join(REPO_ROOT, ".git")):
        return ""
    try:
        res = subprocess.run(
            [git_exe] + args,
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        if res.returncode == 0:
            return res.stdout.strip()
    except Exception:
        pass
    return ""


def detect_local_version(cfg: dict) -> dict:
    """Return local channel, commit SHA, date, and subject (combining git + install_config.json)."""
    git_sha = _git_cmd(["rev-parse", "HEAD"])
    git_branch = _git_cmd(["rev-parse", "--abbrev-ref", "HEAD"])
    git_date = _git_cmd(["show", "-s", "--format=%cs", "HEAD"])
    git_subj = _git_cmd(["show", "-s", "--format=%s", "HEAD"])

    channel = str(cfg.get("update_channel") or "").strip().lower()
    if channel not in VALID_CHANNELS:
        channel = git_branch.lower() if git_branch.lower() in VALID_CHANNELS else "main"

    sha = git_sha or str(cfg.get("installed_commit") or "").strip()
    date = git_date or str(cfg.get("installed_date") or "").strip()
    subj = git_subj or str(cfg.get("installed_subject") or "").strip()
    return {
        "channel": channel,
        "sha": sha,
        "short_sha": sha[:7] if sha else "unknown",
        "date": date,
        "subject": subj,
        "is_git": bool(git_sha),
        "git_branch": git_branch,
    }


def fetch_github_latest(channel: str, timeout: float = 10.0) -> dict:
    """Query GitHub API for the latest commit on `channel` ('main' or 'nightly')."""
    if channel not in VALID_CHANNELS:
        raise ValueError(f"Unsupported channel: {channel}")
    url = f"https://api.github.com/repos/{REPO_SLUG}/commits/{channel}"
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "ShadowHeartsNocturne-Updater/1.0",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"GitHub API returned HTTP {exc.code} for channel '{channel}'.") from exc
    except Exception as exc:
        raise RuntimeError(f"Could not reach GitHub ({exc}). Check your internet connection.") from exc

    sha = str(data.get("sha") or "").strip()
    commit = data.get("commit") or {}
    msg = str(commit.get("message") or "").splitlines()[0].strip() if commit.get("message") else ""
    committer = commit.get("committer") or commit.get("author") or {}
    raw_date = str(committer.get("date") or "")
    date_str = raw_date[:10] if len(raw_date) >= 10 else ""
    if not sha:
        raise RuntimeError("GitHub API response did not include a commit SHA.")
    return {
        "channel": channel,
        "sha": sha,
        "short_sha": sha[:7],
        "date": date_str,
        "subject": msg,
    }


def compare_versions(local: dict, remote: dict) -> dict:
    """Compare local installed version against remote GitHub commit."""
    same_channel = (local.get("channel") == remote.get("channel"))
    local_sha = (local.get("sha") or "").lower()
    remote_sha = (remote.get("sha") or "").lower()
    up_to_date = bool(local_sha and remote_sha and local_sha == remote_sha and same_channel)
    return {
        "channel": remote["channel"],
        "local_sha": local.get("sha", ""),
        "local_short": local.get("short_sha", "unknown"),
        "local_date": local.get("date", ""),
        "local_subject": local.get("subject", ""),
        "remote_sha": remote["sha"],
        "remote_short": remote["short_sha"],
        "remote_date": remote["date"],
        "remote_subject": remote["subject"],
        "up_to_date": up_to_date,
        "update_available": not up_to_date,
        "channel_switch": not same_channel,
    }


def detect_installed_games(install_dir: str) -> list[str]:
    """Return game IDs currently installed under install_dir/games/ (game.dll OR legacy per-game EXE)."""
    installed: list[str] = []
    for g in setup.GAMES:
        gdir = os.path.join(install_dir, "games", g["slug"])
        dll_path = os.path.join(gdir, "game.dll")
        exe_path = os.path.join(gdir, g["exe"])
        if os.path.isfile(dll_path) or os.path.isfile(exe_path):
            installed.append(g["id"])
    return installed


def _file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _copy_if_changed(src: str, dst: str) -> bool:
    """Copy src -> dst ONLY when content differs, preserving dst mtime for unchanged files (prevents Ninja rebuilds)."""
    if os.path.isfile(dst):
        try:
            if os.path.getsize(src) == os.path.getsize(dst) and _file_sha256(src) == _file_sha256(dst):
                return False
        except OSError:
            pass
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def _wait_for_pid_exit(pid: int, timeout_s: float = 12.0) -> None:
    if pid <= 0:
        return
    deadline = time.monotonic() + timeout_s
    if os.name == "nt":
        SYNCHRONIZE = 0x00100000
        handle = ctypes.windll.kernel32.OpenProcess(SYNCHRONIZE, False, int(pid))
        if handle:
            try:
                ctypes.windll.kernel32.WaitForSingleObject(handle, int(timeout_s * 1000))
            finally:
                ctypes.windll.kernel32.CloseHandle(handle)
        return
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
            time.sleep(0.1)
        except OSError:
            return


def _atomic_replace_with_retry(src: str, dst: str, retries: int = 12, delay_s: float = 0.25) -> None:
    tmp_dst = dst + ".upd_tmp"
    shutil.copy2(src, tmp_dst)
    last_err: OSError | None = None
    for _ in range(max(1, retries)):
        try:
            os.replace(tmp_dst, dst)
            return
        except OSError as exc:
            last_err = exc
            time.sleep(delay_s)
    try:
        os.remove(tmp_dst)
    except OSError:
        pass
    if last_err is not None:
        raise last_err


def _relaunch_launcher(install_dir: str, launcher_exe_override: str | None = None) -> bool:
    candidates: list[str] = [
        os.path.join(install_dir, "Shadow Hearts Nocturne.exe"),
    ]
    if launcher_exe_override:
        candidates.append(launcher_exe_override)
    candidates.append(os.path.join(REPO_ROOT, "Shadow Hearts Nocturne.exe"))

    launcher_exe = ""
    for cand in candidates:
        if cand and os.path.isfile(cand):
            launcher_exe = os.path.abspath(cand)
            break
    if not launcher_exe:
        return False

    launch_cwd = os.path.dirname(launcher_exe) or install_dir
    if os.name == "nt":
        # Explicitly launch visibly (SW_SHOWNORMAL = 1) via ShellExecuteW so a hidden
        # updater helper (SW_HIDE) or exiting console window never hides/terminates the GUI.
        try:
            rc = int(ctypes.windll.shell32.ShellExecuteW(None, "open", launcher_exe, None, launch_cwd, 1))
            if rc > 32:
                return True
        except Exception:
            pass
        try:
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 1  # SW_SHOWNORMAL
            flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            subprocess.Popen([launcher_exe], cwd=launch_cwd, startupinfo=si, creationflags=flags, close_fds=True)
            return True
        except OSError:
            return False

    try:
        subprocess.Popen([launcher_exe], cwd=launch_cwd, close_fds=True)
        return True
    except OSError:
        return False


def fetch_prebuilt_engine_release(channel: str, timeout: float = 10.0) -> dict | None:
    """Query GitHub Releases for rolling `engine-<channel>` prebuilt Windows binaries."""
    tag = f"engine-{channel}"
    url = f"https://api.github.com/repos/{REPO_SLUG}/releases/tags/{tag}"
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "ShadowHeartsNocturne-Updater/1.0",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
    except Exception:
        return None
    assets = {str(a.get("name") or ""): str(a.get("browser_download_url") or "") for a in (data.get("assets") or [])}
    if "nocturne-engine-win64.zip" not in assets or "engine-manifest.json" not in assets:
        return None
    try:
        mreq = urllib.request.Request(
            assets["engine-manifest.json"],
            headers={"User-Agent": "ShadowHeartsNocturne-Updater/1.0"},
        )
        with urllib.request.urlopen(mreq, timeout=timeout) as mresp:
            manifest = json.loads(mresp.read().decode("utf-8", errors="replace"))
    except Exception:
        return None
    return {
        "tag": tag,
        "zip_url": assets["nocturne-engine-win64.zip"],
        "manifest": manifest,
    }


def try_install_prebuilt_engine(channel: str, target_sha: str, install_dir: str, quiet: bool = False) -> bool:
    """Download & stage prebuilt Shadow Hearts Nocturne.exe + engine/nocturne_ps2.exe + engine/nocturne_psx.exe."""
    rel = fetch_prebuilt_engine_release(channel)
    if not rel:
        return False
    manifest_sha = str(rel["manifest"].get("commit") or "").strip().lower()
    if target_sha and manifest_sha and manifest_sha != target_sha.lower():
        # Release build for this commit is still running on GitHub Actions; fall back cleanly to local build.
        if not quiet:
            print(ui.paint(
                f"  [note] Prebuilt engine release is at {manifest_sha[:7]} (latest commit is {target_sha[:7]}); "
                "using fast local incremental engine build.",
                ui.Style.YELLOW,
            ))
        return False

    with tempfile.TemporaryDirectory(prefix="nocturne_eng_") as td:
        zip_path = os.path.join(td, "engine.zip")
        extract_dir = os.path.join(td, "extract")
        os.makedirs(extract_dir, exist_ok=True)
        try:
            if quiet:
                req = urllib.request.Request(rel["zip_url"], headers={"User-Agent": "ShadowHeartsNocturne-Updater/1.0"})
                with urllib.request.urlopen(req, timeout=60) as resp, open(zip_path, "wb") as out:
                    shutil.copyfileobj(resp, out)
            else:
                _download_zip_with_progress(rel["zip_url"], zip_path)
            expected_sha = str(rel["manifest"].get("sha256") or "").strip().lower()
            if expected_sha:
                import hashlib
                h = hashlib.sha256()
                with open(zip_path, "rb") as hf:
                    for chunk in iter(lambda: hf.read(65536), b""):
                        h.update(chunk)
                actual_sha = h.hexdigest().lower()
                if actual_sha != expected_sha:
                    raise RuntimeError(f"SHA-256 mismatch for engine.zip (expected {expected_sha[:12]}, got {actual_sha[:12]})")
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(extract_dir)
        except Exception as exc:
            if not quiet:
                print(ui.paint(f"  [note] Prebuilt engine download skipped ({exc}); falling back to local build.", ui.Style.YELLOW))
            return False

        os.makedirs(install_dir, exist_ok=True)
        os.makedirs(os.path.join(install_dir, "engine"), exist_ok=True)
        expected = [
            ("Shadow Hearts Nocturne.exe", os.path.join(install_dir, "Shadow Hearts Nocturne.exe")),
            (os.path.join("engine", "nocturne_ps2.exe"), os.path.join(install_dir, "engine", "nocturne_ps2.exe")),
            (os.path.join("engine", "nocturne_psx.exe"), os.path.join(install_dir, "engine", "nocturne_psx.exe")),
        ]
        for rel_src, dst in expected:
            src = os.path.join(extract_dir, rel_src)
            if not os.path.isfile(src):
                return False
            _atomic_replace_with_retry(src, dst)
        # Also refresh shared DLLs if bundled in the engine zip (e.g. SDL2.dll)
        for fn in os.listdir(extract_dir):
            if fn.lower().endswith(".dll"):
                ui.copy_dll_if_needed(os.path.join(extract_dir, fn), os.path.join(install_dir, fn))
    if not quiet:
        print(ui.paint("  Installed prebuilt Nocturne engine & launcher (zero compiling needed!).", ui.Style.GREEN))
    return True


def _parse_semver(ver: str) -> tuple[int, ...]:
    parts: list[int] = []
    for tok in str(ver or "0").strip().split("."):
        digits = "".join(ch for ch in tok if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts) or (0,)


def sync_bundled_mods(install_dir: Path, installed_games: list[str]) -> None:
    """Upgrade already-installed official bundled mods when assets/mods/ has a newer version.
    Always preserves user settings (mod_config.ini, mod_state.ini, settings.json) and never
    touches custom/third-party mods.
    """
    preserve = {"settings.json", "mod_config.ini", "mod_state.ini"}

    def _upgrade_pair(src_root: Path, dst_root: Path) -> None:
        if not src_root.is_dir() or not dst_root.is_dir():
            return
        for src_mod in sorted(src_root.iterdir()):
            if not src_mod.is_dir():
                continue
            src_json = src_mod / "mod.json"
            dst_mod = dst_root / src_mod.name
            dst_json = dst_mod / "mod.json"
            if not src_json.is_file() or not dst_json.is_file():
                continue
            try:
                src_meta = json.loads(src_json.read_text(encoding="utf-8"))
                dst_meta = json.loads(dst_json.read_text(encoding="utf-8"))
            except Exception:
                continue
            src_ver_s = str(src_meta.get("version", "0.0.0"))
            dst_ver_s = str(dst_meta.get("version", "0.0.0"))
            if _parse_semver(src_ver_s) <= _parse_semver(dst_ver_s):
                continue
            for root_p, _dirs, files in os.walk(src_mod):
                rel_dir = Path(root_p).relative_to(src_mod)
                target_dir = dst_mod / rel_dir
                target_dir.mkdir(parents=True, exist_ok=True)
                for fn in files:
                    if fn.lower() in preserve:
                        continue
                    shutil.copy2(Path(root_p) / fn, target_dir / fn)
            print(ui.paint(
                f"  Upgraded bundled mod {src_mod.name} ({dst_ver_s} -> {src_ver_s}; user config preserved)",
                ui.Style.CYAN,
            ))

    slug_map = {g["id"]: g["slug"] for g in setup.GAMES}
    shared_src = Path(REPO_ROOT) / "assets" / "mods" / "shared"
    # Upgrade shared mods installed in games/shared/mods/
    _upgrade_pair(shared_src, Path(install_dir) / "games" / "shared" / "mods")
    for gid in installed_games:
        slug = slug_map.get(gid, gid)
        dst_root = Path(install_dir) / "games" / slug / "mods"
        _upgrade_pair(Path(REPO_ROOT) / "assets" / "mods" / gid, dst_root)
        # Also upgrade legacy per-game copies of shared mods if present
        _upgrade_pair(shared_src, dst_root)


def plan_game_dll_updates(install_dir: str, installed_games: list[str], force: bool = False) -> list[tuple[str, str]]:
    """Return [(game_id, reason), ...] for installed games whose game.dll needs (re)building."""
    ps2_eng = os.path.join(install_dir, "engine", "nocturne_ps2.exe")
    psx_eng = os.path.join(install_dir, "engine", "nocturne_psx.exe")
    ps2_abi = ui.probe_engine_abi(ps2_eng, install_dir=install_dir)
    psx_abi = ui.probe_engine_abi(psx_eng, install_dir=install_dir)

    game_by_id = {g["id"]: g for g in setup.GAMES}
    needs: list[tuple[str, str]] = []
    for gid in installed_games:
        g = game_by_id[gid]
        dll_path = os.path.join(install_dir, "games", g["slug"], "game.dll")
        if force:
            needs.append((gid, "forced rebuild"))
            continue
        if not os.path.isfile(dll_path):
            needs.append((gid, "one-time migration from legacy EXE -> game.dll"))
            continue
        kind = "psx" if gid == "koudelka" else "ps2"
        eng_abi = psx_abi if kind == "psx" else ps2_abi
        if not eng_abi:
            continue
        dll_abi = ui.probe_game_dll_abi(dll_path, kind)
        if not dll_abi or dll_abi.get("abi") != eng_abi.get("abi") or dll_abi.get("layout") != eng_abi.get("layout"):
            needs.append((gid, "recompiler ABI updated"))
    return needs


def _download_zip_with_progress(url: str, dst_path: str) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "ShadowHeartsNocturne-Updater/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        total = int(resp.headers.get("Content-Length") or 0)
        pbar = ui.ProgressBar("Download", total=max(1, total // 1024)) if total > 0 else None
        sp = ui.Spinner("Download", "Downloading latest source from GitHub...").start() if pbar is None else None
        done = 0
        with open(dst_path, "wb") as out:
            while True:
                chunk = resp.read(131072)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if pbar is not None:
                    pbar.update(done // 1024, detail=f"{done / 1e6:.1f} / {total / 1e6:.1f} MB")
                elif sp is not None:
                    sp.set_detail(f"{done / 1e6:.1f} MB downloaded...")
        if pbar is not None:
            pbar.finish(f"{done / 1e6:.1f} MB")
        elif sp is not None:
            sp.finish(ok=True, summary=f"{done / 1e6:.1f} MB downloaded")


def update_repo_source(channel: str) -> str:
    """Update local source files to latest `channel` commit. Returns method used ('git' or 'zip')."""
    # Try clean fast-forward git update if this is a git clone with clean tracked files
    if os.path.isdir(os.path.join(REPO_ROOT, ".git")):
        git_exe = shutil.which("git") or os.path.join(REPO_ROOT, "tools", "portable", "mingit", "cmd", "git.exe")
        status = _git_cmd(["status", "--porcelain", "-uno"])
        if not status:
            try:
                ui.run_spinner_cmd([git_exe, "fetch", "origin", channel], "Git Fetch", f"Fetching origin/{channel}...")
                cur_branch = _git_cmd(["rev-parse", "--abbrev-ref", "HEAD"])
                if cur_branch != channel:
                    # Switch cleanly to target branch (creating local tracking branch if needed)
                    check_local = subprocess.run(
                        [git_exe, "show-ref", "--verify", "--quiet", f"refs/heads/{channel}"],
                        cwd=REPO_ROOT,
                    )
                    if check_local.returncode == 0:
                        ui.run_spinner_cmd([git_exe, "checkout", channel], "Git Switch", f"Switching to {channel}...")
                    else:
                        ui.run_spinner_cmd([git_exe, "checkout", "-b", channel, f"origin/{channel}"], "Git Switch", f"Checking out {channel}...")
                ui.run_spinner_cmd([git_exe, "pull", "--ff-only", "origin", channel], "Git Pull", f"Fast-forwarding to latest {channel}...")
                return "git"
            except Exception as exc:
                print(ui.paint(f"  [note] Git fast-forward skipped ({exc}); falling back to clean GitHub archive sync...", ui.Style.YELLOW))

    # Standalone ZIP / bootstrapper sync (preserves games/, tools/portable/, third_party/, build/, generated/, saves, keys.ini)
    zip_url = f"https://github.com/{REPO_SLUG}/archive/refs/heads/{channel}.zip"
    with tempfile.TemporaryDirectory(prefix="nocturne_upd_") as td:
        zip_path = os.path.join(td, "repo.zip")
        extract_dir = os.path.join(td, "extract")
        os.makedirs(extract_dir, exist_ok=True)
        _download_zip_with_progress(zip_url, zip_path)
        with ui.Spinner("Extract", "Unpacking & syncing source files (saves & tools preserved)..."):
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(extract_dir)
            top = [os.path.join(extract_dir, d) for d in os.listdir(extract_dir) if os.path.isdir(os.path.join(extract_dir, d))]
            if len(top) != 1:
                raise RuntimeError("Unexpected GitHub archive structure.")
            src_root = top[0]
            for root, dirs, files in os.walk(src_root):
                rel = os.path.relpath(root, src_root)
                dst_dir = REPO_ROOT if rel == "." else os.path.join(REPO_ROOT, rel)
                os.makedirs(dst_dir, exist_ok=True)
                for fn in files:
                    _copy_if_changed(os.path.join(root, fn), os.path.join(dst_dir, fn))
    return "zip"


def stamp_installed_version(cfg: dict, channel: str, remote: dict | None = None) -> dict:
    """Persist channel + installed commit metadata in install_config.json."""
    cfg["update_channel"] = channel
    if remote and remote.get("sha"):
        cfg["installed_commit"] = remote["sha"]
        cfg["installed_date"] = remote.get("date", "")
        cfg["installed_subject"] = remote.get("subject", "")
    else:
        local = detect_local_version(cfg)
        if local.get("sha"):
            cfg["installed_commit"] = local["sha"]
            cfg["installed_date"] = local.get("date", "")
            cfg["installed_subject"] = local.get("subject", "")
    ui.save_install_config(cfg)
    return cfg


def print_status_card(cmp: dict) -> None:
    ch_label = "Nightly (Opt-In)" if cmp["channel"] == "nightly" else "Stable (main)"
    print(f"  Release Channel  : {ui.paint(ch_label, ui.Style.BOLD, ui.Style.CYAN)}")
    loc_line = cmp["local_short"]
    if cmp["local_date"]:
        loc_line += f" ({cmp['local_date']})"
    if cmp["local_subject"]:
        loc_line += f" — {cmp['local_subject'][:54]}"
    rem_line = cmp["remote_short"]
    if cmp["remote_date"]:
        rem_line += f" ({cmp['remote_date']})"
    if cmp["remote_subject"]:
        rem_line += f" — {cmp['remote_subject'][:54]}"
    print(f"  Installed Build  : {loc_line}")
    print(f"  Latest on GitHub : {rem_line}")
    if cmp["up_to_date"]:
        print(f"  Status           : {ui.paint('UP TO DATE — you have the latest build!', ui.Style.BOLD, ui.Style.GREEN)}")
    else:
        tag = "CHANNEL SWITCH / UPDATE AVAILABLE" if cmp["channel_switch"] else "UPDATE AVAILABLE"
        print(f"  Status           : {ui.paint(tag, ui.Style.BOLD, ui.Style.GOLD)}")


def run_interactive_updater(
    channel_override: str | None,
    force: bool,
    non_interactive: bool,
    relaunch: bool = False,
    apply_engine: bool = False,
    launcher_exe: str | None = None,
) -> int:
    cfg = ui.load_install_config()
    local = detect_local_version(cfg)
    channel = (channel_override or local["channel"]).strip().lower()
    if channel not in VALID_CHANNELS:
        channel = "main"

    install_dir = ui.resolve_install_dir()
    jobs = int(cfg.get("jobs") or ui.recommended_compile_jobs()[0])

    ui.print_banner(
        "SHADOW HEARTS: NOCTURNE — UPDATE CHECKER",
        f"Repository: {REPO_SLUG}  |  Install folder: {install_dir}",
    )

    while True:
        with ui.Spinner("GitHub", f"Checking latest '{channel}' commit..."):
            remote = fetch_github_latest(channel)
        local = detect_local_version(cfg)
        cmp = compare_versions(local, remote)
        print()
        print_status_card(cmp)

        if cmp["up_to_date"] and not force and not apply_engine:
            if non_interactive:
                if relaunch:
                    _relaunch_launcher(install_dir, launcher_exe)
                return 0
            other = "nightly" if channel == "main" else "main"
            other_lbl = "Nightly (opt-in latest)" if other == "nightly" else "Stable (main)"
            print()
            print(f"    [C] Switch release channel to {other_lbl}")
            print("    [R] Force re-download & rebuild installed game(s)")
            print("    [Enter] Return")
            try:
                ans = input("\n  Choice [Enter]: ").strip().lower()
            except EOFError:
                if relaunch:
                    _relaunch_launcher(install_dir, launcher_exe)
                return 0
            if ans == "c":
                channel = other
                cfg["update_channel"] = channel
                ui.save_install_config(cfg)
                continue
            if ans == "r":
                force = True
                break
            if relaunch:
                _relaunch_launcher(install_dir, launcher_exe)
            return 0
        break

    installed_games = detect_installed_games(install_dir)
    print()
    if installed_games:
        game_map = {g["id"]: g["title"] for g in setup.GAMES}
        print("  Installed games that will be incrementally updated (saves & keys.ini preserved):")
        for gid in installed_games:
            print(f"    • {game_map[gid]}")
    else:
        print("  No games built yet — source files will be updated so you can run the Setup Walkthrough.")

    if not non_interactive:
        other = "nightly" if channel == "main" else "main"
        print(f"\n  Options: [Y] Update now ({channel})  |  [C] Switch to {other}  |  [N] Cancel")
        while True:
            try:
                ans = input("  Proceed? [Y/c/n]: ").strip().lower()
            except EOFError:
                ans = "n"
            if ans in ("", "y", "yes"):
                break
            if ans == "c":
                channel = other
                cfg["update_channel"] = channel
                ui.save_install_config(cfg)
                return run_interactive_updater(channel, force=False, non_interactive=False, relaunch=relaunch, launcher_exe=launcher_exe)
            if ans in ("n", "no"):
                print("  Update cancelled.")
                return 0
            print("  Please enter Y, C, or N.")

    if not ui.ensure_apps_closed(ui.ALL_NOCTURNE_EXES, interactive=not non_interactive, pause_on_cancel=True):
        return ui.EXIT_USER_CANCELLED

    ui.print_section(f"[1/3] Updating source files to {channel} ({remote['short_sha']})...")
    method = update_repo_source(channel)
    print(f"  Source updated via {method} to {remote['short_sha']} (unchanged file timestamps preserved).")

    ui.print_section(f"[2/3] Updating shared Nocturne Engine & Launcher ({channel})...")
    used_prebuilt = False
    if not force:
        used_prebuilt = try_install_prebuilt_engine(channel, remote["sha"], install_dir, quiet=False)
    if not used_prebuilt:
        ui.build_and_stage_shared_engine(install_dir, jobs)

    stamp_installed_version(cfg, channel, remote)

    if installed_games:
        sync_bundled_mods(install_dir, installed_games)
        rebuild_plan = plan_game_dll_updates(install_dir, installed_games, force=force)
        if rebuild_plan:
            game_map = {g["id"]: g["title"] for g in setup.GAMES}
            ui.print_section(f"[3/3] Building {len(rebuild_plan)} game module(s) (-j {jobs})...")
            for gid, reason in rebuild_plan:
                print(f"    • {game_map[gid]} — {reason}")
            rc = setup.execute_build_queue(
                selected_ids=[gid for gid, _ in rebuild_plan],
                install_dir=install_dir,
                jobs=jobs,
                disc_plan={},
                delete_sources=False,
                make_desktop=False,
                make_startmenu=False,
                interactive_end=not non_interactive,
                game_dll_only=True,
            )
            if rc != 0:
                return rc
        else:
            ui.print_section("[3/3] Checking installed game modules (game.dll)...")
            print(ui.paint(
                f"  All {len(installed_games)} installed game module(s) are ABI-compatible — ZERO game recompiling needed!",
                ui.Style.BOLD,
                ui.Style.GREEN,
            ))
            ui.print_banner(
                f"Updated to {channel} ({remote['short_sha']})!",
                "Shared engine & launcher updated. All saves, keys.ini, mods & game.dll modules preserved.",
            )
    else:
        ui.print_banner(
            f"Updated to {channel} ({remote['short_sha']})!",
            "Run [W] Step-by-Step Setup Walkthrough to install your games.",
        )
    if relaunch:
        _relaunch_launcher(install_dir, launcher_exe)
    return 0


def run_self_update(
    channel_override: str | None,
    wait_pid: int,
    relaunch: bool,
    launcher_exe: str | None = None,
) -> int:
    """Fast headless self-update invoked by Shadow Hearts Nocturne.exe.
    Swaps prebuilt engine binaries in ~3-5s and relaunches immediately. If first-time migration,
    ABI rebuild, or local compile fallback is needed, opens a visible console window automatically.
    """
    _wait_for_pid_exit(wait_pid, timeout_s=12.0)
    cfg = ui.load_install_config()
    local = detect_local_version(cfg)
    channel = (channel_override or local["channel"]).strip().lower()
    if channel not in VALID_CHANNELS:
        channel = "main"
    install_dir = ui.resolve_install_dir()

    try:
        remote = fetch_github_latest(channel)
        update_repo_source(channel)
        prebuilt_ok = try_install_prebuilt_engine(channel, remote["sha"], install_dir, quiet=True)
        installed_games = detect_installed_games(install_dir)
        needs_game_build = plan_game_dll_updates(install_dir, installed_games, force=False) if prebuilt_ok else [("engine", "local build")]

        if prebuilt_ok and not needs_game_build:
            stamp_installed_version(cfg, channel, remote)
            if relaunch:
                _relaunch_launcher(install_dir, launcher_exe)
            return 0
    except Exception:
        pass

    # Needs visible progress (one-time migration, ABI change, or local engine compile fallback).
    # Pass --apply so run_interactive_updater still builds/stages the engine even though
    # update_repo_source() above already fast-forwarded git HEAD to the target commit.
    cmd = [
        sys.executable, "-u", os.path.abspath(__file__),
        "--channel", channel,
        "--non-interactive",
        "--apply",
    ]
    if relaunch:
        cmd.append("--relaunch")
    if launcher_exe:
        cmd.extend(["--launcher-exe", launcher_exe])
    creationflags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
    if os.name == "nt":
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 1  # SW_SHOWNORMAL (do not inherit SW_HIDE from launcher self-update)
        subprocess.Popen(cmd, cwd=REPO_ROOT, startupinfo=si, creationflags=creationflags)
    else:
        subprocess.Popen(cmd, cwd=REPO_ROOT, creationflags=creationflags)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Check for & install Shadow Hearts: Nocturne updates from GitHub.")
    ap.add_argument("--check", action="store_true", help="Only check for updates without modifying files")
    ap.add_argument("--json", action="store_true", help="Output check result as JSON (for Launcher integration)")
    ap.add_argument("--channel", choices=VALID_CHANNELS, default=None, help="Release channel (main or nightly)")
    ap.add_argument("--set-channel", choices=VALID_CHANNELS, default=None, help="Persist preferred release channel and exit")
    ap.add_argument("--stamp", action="store_true", help="Stamp current installed_commit/channel into install_config.json")
    ap.add_argument("--force", action="store_true", help="Rebuild even if already on the latest commit")
    ap.add_argument("--apply", action="store_true", help="Always build/stage engine & check game.dll ABI even if source HEAD matches remote")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
    ap.add_argument("--self-update", action="store_true", help="Silent 1-click launcher self-update (opens console only if compiling is needed)")
    ap.add_argument("--relaunch", action="store_true", help="Relaunch Shadow Hearts Nocturne.exe after updating")
    ap.add_argument("--launcher-exe", default=None, help="Explicit Shadow Hearts Nocturne.exe path to relaunch after updating")
    ap.add_argument("--wait-pid", type=int, default=0, help="Wait for this PID to exit before swapping executables")
    args = ap.parse_args(argv)

    cfg = ui.load_install_config()

    if args.set_channel:
        cfg["update_channel"] = args.set_channel
        ui.save_install_config(cfg)
        if args.json:
            print(json.dumps({"ok": True, "channel": args.set_channel}))
        else:
            print(f"Update channel set to: {args.set_channel}")
        return 0

    if args.stamp:
        ch = args.channel or detect_local_version(cfg)["channel"]
        remote = None
        try:
            remote = fetch_github_latest(ch, timeout=6.0)
        except Exception:
            pass
        stamp_installed_version(cfg, ch, remote)
        return 0

    if args.check:
        local = detect_local_version(cfg)
        channel = args.channel or local["channel"]
        try:
            remote = fetch_github_latest(channel)
            cmp = compare_versions(local, remote)
            cmp["ok"] = True
            if args.json:
                print(json.dumps(cmp))
            else:
                print_status_card(cmp)
            return 0
        except Exception as exc:
            if args.json:
                print(json.dumps({"ok": False, "channel": channel, "error": str(exc)}))
                return 1
            print(ui.paint(f"[ERROR] {exc}", ui.Style.CRIMSON), file=sys.stderr)
            return 1

    if args.self_update:
        return run_self_update(
            args.channel,
            wait_pid=args.wait_pid,
            relaunch=args.relaunch,
            launcher_exe=args.launcher_exe,
        )

    return run_interactive_updater(
        args.channel,
        force=args.force,
        non_interactive=args.non_interactive,
        relaunch=args.relaunch,
        apply_engine=args.apply,
        launcher_exe=args.launcher_exe,
    )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nUpdate cancelled.")
        raise SystemExit(ui.EXIT_USER_CANCELLED)
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
