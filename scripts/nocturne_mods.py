#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — GitHub Mod Catalog & 1-Click Installer CLI.

Used by the Launcher's Mod Browser (Tools -> Packs & Mods -> Mod Catalog) and
directly from the command line to list, download, install, and update official
and community mods from GitHub (with automatic offline fallback to local
assets/mods/catalog.json).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_REPO = "NfgOdin/Shadow-Hearts-Nocturne"
USER_AGENT = "ShadowHeartsNocturne-ModBrowser/1.0"
PRESERVE_FILES = {"mod_config.ini", "mod_state.ini", "settings.json"}


def parse_semver(ver: str) -> tuple[int, ...]:
    parts: list[int] = []
    for tok in str(ver or "0").strip().split("."):
        digits = "".join(ch for ch in tok if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts) or (0,)


def norm_slug(s: str) -> str:
    low = (s or "").strip().lower()
    if low in ("sh2", "covenant"):
        return "sh2-covenant"
    if low in ("sh3", "ftnw"):
        return "sh3-ftnw"
    return low


def mod_matches_game(target_game: str, game_slug: str) -> bool:
    tg = (target_game or "").strip().lower()
    gs = norm_slug(game_slug)
    if not tg or tg in ("all", "shared", "quadrilogy", "*") or not gs:
        return True
    for tok in (t.strip() for t in tg.split(",")):
        if tok in ("all", "shared", "quadrilogy", "*"):
            return True
        if tok in ("all_ps2", "ps2", "trilogy") and gs in ("sh1", "sh2-covenant", "sh3-ftnw"):
            return True
        if norm_slug(tok) == gs:
            return True
    return False


def _raw_url(repo: str, branch: str, repo_rel_path: str) -> str:
    clean = repo_rel_path.lstrip("/").replace("\\", "/")
    return f"https://raw.githubusercontent.com/{repo}/{branch}/{clean}"


def _fetch_bytes(url: str, timeout: int = 12) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def load_catalog(channel: str, offline: bool = False) -> tuple[dict, str]:
    """Return (catalog_dict, source_label). Falls back to local assets/mods/catalog.json."""
    branch = "nightly" if (channel or "").strip().lower() == "nightly" else "main"
    local_path = REPO_ROOT / "assets" / "mods" / "catalog.json"

    if not offline:
        try:
            url = _raw_url(DEFAULT_REPO, branch, "assets/mods/catalog.json")
            data = _fetch_bytes(url, timeout=10)
            parsed = json.loads(data.decode("utf-8"))
            if isinstance(parsed, dict) and isinstance(parsed.get("mods"), list):
                return parsed, f"github:{branch}"
        except Exception:
            pass

    if local_path.is_file():
        parsed = json.loads(local_path.read_text(encoding="utf-8"))
        return parsed, "local"

    return {"schema_version": 1, "repository": DEFAULT_REPO, "mods": []}, "empty"


def resolve_target_dir(install_dir: Path, game_slug: str, entry: dict) -> Path:
    folder = str(entry.get("folder") or entry.get("id") or "").strip()
    if not folder or "/" in folder or "\\" in folder or ".." in folder:
        raise ValueError(f"Unsafe mod folder name: {folder!r}")
    scope = str(entry.get("install_scope") or "").strip().lower()
    if scope == "shared" or str(entry.get("game") or "").strip().lower() in ("all", "shared", "all_ps2"):
        return install_dir / "games" / "shared" / "mods" / folder
    return install_dir / "games" / norm_slug(game_slug) / "mods" / folder


def installed_version(install_dir: Path, game_slug: str, entry: dict) -> tuple[str, str]:
    """Return (status, installed_ver) where status in {'not_installed','installed','update_available'}."""
    folder = str(entry.get("folder") or entry.get("id") or "")
    candidates = [
        install_dir / "games" / norm_slug(game_slug) / "mods" / folder,
        install_dir / "games" / "shared" / "mods" / folder,
    ]
    for cand in candidates:
        mj = cand / "mod.json"
        if mj.is_file():
            try:
                meta = json.loads(mj.read_text(encoding="utf-8"))
                inst_v = str(meta.get("version") or "1.0.0")
            except Exception:
                inst_v = "1.0.0"
            cat_v = str(entry.get("version") or "1.0.0")
            if parse_semver(cat_v) > parse_semver(inst_v):
                return "update_available", inst_v
            return "installed", inst_v
    return "not_installed", ""


def list_catalog_mods(install_dir: Path, game_slug: str, channel: str, offline: bool = False) -> dict:
    cat, source = load_catalog(channel, offline=offline)
    out_mods: list[dict] = []
    for entry in cat.get("mods", []):
        if not isinstance(entry, dict):
            continue
        if game_slug and not mod_matches_game(str(entry.get("game") or ""), game_slug):
            continue
        st, inst_v = installed_version(install_dir, game_slug, entry)
        item = dict(entry)
        item["status"] = st
        item["installed_version"] = inst_v
        out_mods.append(item)
    return {"ok": True, "source": source, "game": game_slug, "mods": out_mods}


def _safe_rel_path(rel: str) -> Path:
    p = Path(rel)
    if p.is_absolute() or ".." in p.parts:
        raise ValueError(f"Unsafe relative mod file path: {rel!r}")
    return p


def _ensure_keys_ini_entry(install_dir: Path, game_slug: str, folder: str, default_disabled: bool) -> None:
    keys_path = install_dir / "games" / norm_slug(game_slug) / "keys.ini"
    if not keys_path.is_file():
        return
    try:
        lines = keys_path.read_text(encoding="utf-8").splitlines()
    except Exception:
        return
    low_folder = folder.lower()
    found_key = False
    new_lines: list[str] = []
    for line in lines:
        stripped = line.strip()
        if stripped.lower().startswith("active_mods") and "=" in stripped:
            found_key = True
            k, v = line.split("=", 1)
            toks = [t.strip() for t in v.split(",") if t.strip()]
            present = any(t.lstrip("!").strip().lower() == low_folder for t in toks)
            if not present:
                toks.append(("!" + folder) if default_disabled else folder)
            new_lines.append(f"{k.strip()} = {','.join(toks)}")
        else:
            new_lines.append(line)
    if not found_key:
        tok = ("!" + folder) if default_disabled else folder
        new_lines.append(f"active_mods = {tok}")
    keys_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")


def install_catalog_mod(install_dir: Path, game_slug: str, mod_id: str, channel: str, offline: bool = False) -> dict:
    cat, source = load_catalog(channel, offline=offline)
    repo = str(cat.get("repository") or DEFAULT_REPO)
    branch = "nightly" if (channel or "").strip().lower() == "nightly" else "main"

    entry = None
    for m in cat.get("mods", []):
        if isinstance(m, dict) and (m.get("id") == mod_id or m.get("folder") == mod_id):
            entry = m
            break
    if entry is None:
        return {"ok": False, "error": f"Mod '{mod_id}' not found in catalog."}

    target_dir = resolve_target_dir(install_dir, game_slug, entry)
    was_installed = (target_dir / "mod.json").is_file()
    repo_path = str(entry.get("repo_path") or "").strip().strip("/")
    files = entry.get("files") or []
    if not repo_path or not files:
        return {"ok": False, "error": f"Catalog entry '{mod_id}' is missing repo_path or files."}

    local_mod_root = REPO_ROOT / Path(repo_path)
    downloaded: list[tuple[Path, bytes]] = []
    used_source = source

    for rel in files:
        rel_p = _safe_rel_path(str(rel))
        data: bytes | None = None
        if not offline and source.startswith("github:"):
            try:
                url = _raw_url(repo, branch, f"{repo_path}/{rel_p.as_posix()}")
                data = _fetch_bytes(url, timeout=15)
            except Exception:
                data = None
        if data is None:
            local_file = local_mod_root / rel_p
            if local_file.is_file():
                data = local_file.read_bytes()
                used_source = "local"
            else:
                return {"ok": False, "error": f"Failed to download '{rel_p.as_posix()}' and no local copy exists."}
        downloaded.append((rel_p, data))

    # Stage into temp dir then copy into target_dir, preserving user configs & state files
    with tempfile.TemporaryDirectory(prefix="nocturne_mod_") as tmp:
        tmp_root = Path(tmp)
        for rel_p, data in downloaded:
            dst = tmp_root / rel_p
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)

        target_dir.mkdir(parents=True, exist_ok=True)
        for rel_p, _ in downloaded:
            if rel_p.name.lower() in PRESERVE_FILES and (target_dir / rel_p).exists():
                continue
            final_dst = target_dir / rel_p
            final_dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(tmp_root / rel_p, final_dst)

    folder = str(entry.get("folder") or mod_id)
    if not was_installed and game_slug:
        # Freshly downloaded mods start enabled so 1-click Install works immediately
        _ensure_keys_ini_entry(install_dir, game_slug, folder, default_disabled=False)

    return {
        "ok": True,
        "id": entry.get("id"),
        "folder": folder,
        "name": entry.get("name"),
        "version": entry.get("version"),
        "install_dir": str(target_dir),
        "updated": was_installed,
        "source": used_source,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Nocturne Mod Catalog CLI")
    ap.add_argument("--list", action="store_true", help="List available catalog mods as JSON")
    ap.add_argument("--install", metavar="MOD_ID", help="Download and install/update a mod by id or folder")
    ap.add_argument("--game", default="", help="Target game slug (koudelka, sh1, sh2-covenant, sh3-ftnw)")
    ap.add_argument("--channel", default="main", help="GitHub branch/channel (main or nightly)")
    ap.add_argument("--install-dir", default=str(REPO_ROOT), help="Nocturne installation root directory")
    ap.add_argument("--offline", action="store_true", help="Use local assets/mods/catalog.json without network")
    args = ap.parse_args(argv)

    inst_dir = Path(args.install_dir).resolve()
    if args.install:
        res = install_catalog_mod(inst_dir, args.game, args.install, args.channel, offline=args.offline)
        print(json.dumps(res, ensure_ascii=False))
        return 0 if res.get("ok") else 1

    res = list_catalog_mods(inst_dir, args.game, args.channel, offline=args.offline)
    print(json.dumps(res, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
