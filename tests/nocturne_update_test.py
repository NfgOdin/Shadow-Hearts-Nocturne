#!/usr/bin/env python3
"""Regression tests for scripts/nocturne_update.py (bundled mod slug mapping & semver)."""

from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import nocturne_update as upd  # noqa: E402


def test_sync_bundled_mods_uses_slugs() -> None:
    with tempfile.TemporaryDirectory(prefix="nocturne_upd_test_") as td:
        install_dir = Path(td)
        # Verify slug mapping for SH2 (sh2 -> sh2-covenant) and SH3 (sh3 -> sh3-ftnw)
        slug_map = {g["id"]: g["slug"] for g in upd.setup.GAMES}
        assert slug_map["sh2"] == "sh2-covenant", f"unexpected sh2 slug: {slug_map['sh2']}"
        assert slug_map["sh3"] == "sh3-ftnw", f"unexpected sh3 slug: {slug_map['sh3']}"

        # Find any real bundled mod in assets/mods/sh2 or assets/mods/koudelka to test upgrade
        tested = 0
        for gid in ("koudelka", "sh1", "sh2", "sh3"):
            src_root = ROOT / "assets" / "mods" / gid
            if not src_root.is_dir():
                continue
            for src_mod in sorted(src_root.iterdir()):
                src_json = src_mod / "mod.json"
                if not src_json.is_file():
                    continue
                slug = slug_map[gid]
                dst_mod = install_dir / "games" / slug / "mods" / src_mod.name
                dst_mod.mkdir(parents=True, exist_ok=True)
                # Write an older version (0.0.1) + user settings.json that MUST be preserved
                (dst_mod / "mod.json").write_text(json.dumps({"name": src_mod.name, "version": "0.0.1"}), encoding="utf-8")
                (dst_mod / "settings.json").write_text(json.dumps({"user_pref": 42}), encoding="utf-8")

                upd.sync_bundled_mods(install_dir, [gid])

                upgraded_meta = json.loads((dst_mod / "mod.json").read_text(encoding="utf-8"))
                src_meta = json.loads(src_json.read_text(encoding="utf-8"))
                assert upgraded_meta.get("version") == src_meta.get("version"), (
                    f"{gid}/{src_mod.name} was not upgraded under games/{slug}/mods/"
                )
                preserved = json.loads((dst_mod / "settings.json").read_text(encoding="utf-8"))
                assert preserved == {"user_pref": 42}, "user settings.json was clobbered!"
                tested += 1
                break

        assert tested > 0, "expected at least one bundled mod under assets/mods/"
        print(f"PASS: sync_bundled_mods upgraded {tested} game(s) using exact game slugs and preserved settings.json")


def test_parse_semver() -> None:
    assert upd._parse_semver("1.2.3") > upd._parse_semver("1.2.2")
    assert upd._parse_semver("2.0.0") > upd._parse_semver("1.9.9")
    assert upd._parse_semver("v1.10.0") > upd._parse_semver("v1.9.0")
    print("PASS: _parse_semver ordering verified")


if __name__ == "__main__":
    test_parse_semver()
    test_sync_bundled_mods_uses_slugs()
    print("ALL NOCTURNE UPDATER TESTS PASSED!")
