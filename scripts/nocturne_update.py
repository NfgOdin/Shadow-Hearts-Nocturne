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
import json
import os
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
    """Return game IDs that are currently installed under install_dir/games/."""
    installed: list[str] = []
    for g in setup.GAMES:
        exe_path = os.path.join(install_dir, "games", g["slug"], g["exe"])
        if os.path.isfile(exe_path):
            installed.append(g["id"])
    return installed


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
                    shutil.copy2(os.path.join(root, fn), os.path.join(dst_dir, fn))
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


def run_interactive_updater(channel_override: str | None, force: bool, non_interactive: bool) -> int:
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

        if cmp["up_to_date"] and not force:
            if non_interactive:
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
                return 0
            if ans == "c":
                channel = other
                cfg["update_channel"] = channel
                ui.save_install_config(cfg)
                continue
            if ans == "r":
                force = True
                break
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
                return run_interactive_updater(channel, force=False, non_interactive=False)
            if ans in ("n", "no"):
                print("  Update cancelled.")
                return 0
            print("  Please enter Y, C, or N.")

    if not ui.ensure_apps_closed(ui.ALL_NOCTURNE_EXES, interactive=not non_interactive, pause_on_cancel=True):
        return ui.EXIT_USER_CANCELLED

    ui.print_section(f"[1/2] Updating source files to {channel} ({remote['short_sha']})...")
    method = update_repo_source(channel)
    stamp_installed_version(cfg, channel, remote)
    print(f"  Source updated via {method} to {remote['short_sha']}.")

    if installed_games:
        ui.print_section(f"[2/2] Rebuilding {len(installed_games)} installed game(s) + Launcher (-j {jobs})...")
        rc = setup.execute_build_queue(
            selected_ids=installed_games,
            install_dir=install_dir,
            jobs=jobs,
            disc_plan={},
            delete_sources=False,
            make_desktop=False,
            make_startmenu=False,
            interactive_end=not non_interactive,
        )
        if rc != 0:
            return rc
    else:
        ui.print_banner(
            f"Source updated to {channel} ({remote['short_sha']})!",
            "Run [W] Step-by-Step Setup Walkthrough to install your games.",
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Check for & install Shadow Hearts: Nocturne updates from GitHub.")
    ap.add_argument("--check", action="store_true", help="Only check for updates without modifying files")
    ap.add_argument("--json", action="store_true", help="Output check result as JSON (for Launcher integration)")
    ap.add_argument("--channel", choices=VALID_CHANNELS, default=None, help="Release channel (main or nightly)")
    ap.add_argument("--set-channel", choices=VALID_CHANNELS, default=None, help="Persist preferred release channel and exit")
    ap.add_argument("--stamp", action="store_true", help="Stamp current installed_commit/channel into install_config.json")
    ap.add_argument("--force", action="store_true", help="Rebuild even if already on the latest commit")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
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

    return run_interactive_updater(args.channel, force=args.force, non_interactive=args.non_interactive)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nUpdate cancelled.")
        raise SystemExit(ui.EXIT_USER_CANCELLED)
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
