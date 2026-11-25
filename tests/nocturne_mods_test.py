#!/usr/bin/env python3
"""Unit tests for scripts/nocturne_mods.py and assets/mods/catalog.json."""
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import nocturne_mods as nm  # noqa: E402


def test_catalog_and_offline_install() -> None:
    cat_path = REPO_ROOT / "assets" / "mods" / "catalog.json"
    assert cat_path.is_file(), "assets/mods/catalog.json must exist"
    cat = json.loads(cat_path.read_text(encoding="utf-8"))
    assert len(cat.get("mods", [])) >= 7, "Expected at least 7 official mods in catalog"

    # Verify every catalog file exists in the repository
    for m in cat["mods"]:
        base = REPO_ROOT / m["repo_path"]
        for rel in m["files"]:
            assert (base / rel).is_file(), f"Missing catalog file: {base / rel}"

    with tempfile.TemporaryDirectory(prefix="nocturne_mods_test_") as tmp:
        inst = Path(tmp)
        (inst / "games" / "koudelka").mkdir(parents=True)
        (inst / "games" / "koudelka" / "keys.ini").write_text("active_mods = \n", encoding="utf-8")

        # List Koudelka mods (should include koudelka + all, exclude sh1 & all_ps2)
        listed = nm.list_catalog_mods(inst, "koudelka", "main", offline=True)
        assert listed["ok"] is True
        ids = {m["id"] for m in listed["mods"]}
        assert "kdl_nemeton_lantern" in ids
        assert "kdl_nemeton_tracker" in ids
        assert "quad_party_vitals_hud" in ids
        assert "sh1_chronicle_unleashed" not in ids
        assert "trilogy_crimson_flow" not in ids

        # Install shared mod -> goes to games/shared/mods/PartyVitalsHUD
        r1 = nm.install_catalog_mod(inst, "koudelka", "quad_party_vitals_hud", "main", offline=True)
        assert r1["ok"] is True
        shared_dir = inst / "games" / "shared" / "mods" / "PartyVitalsHUD"
        assert (shared_dir / "mod.json").is_file()
        assert (shared_dir / "scripts" / "party_vitals_hud.lua").is_file()

        # Install game-specific mod -> goes to games/koudelka/mods/NemetonTracker
        r2 = nm.install_catalog_mod(inst, "koudelka", "kdl_nemeton_tracker", "main", offline=True)
        assert r2["ok"] is True
        kdl_mod = inst / "games" / "koudelka" / "mods" / "NemetonTracker"
        assert (kdl_mod / "mod.json").is_file()

        # Write user mod_config.ini and reinstall -> must preserve user config
        cfg = kdl_mod / "mod_config.ini"
        cfg.write_text("low_durability_warn = 40\n", encoding="utf-8")
        r3 = nm.install_catalog_mod(inst, "koudelka", "kdl_nemeton_tracker", "main", offline=True)
        assert r3["ok"] is True and r3["updated"] is True
        assert "low_durability_warn = 40" in cfg.read_text(encoding="utf-8")

        # Status now reports 'installed'
        listed2 = nm.list_catalog_mods(inst, "koudelka", "main", offline=True)
        by_id = {m["id"]: m for m in listed2["mods"]}
        assert by_id["quad_party_vitals_hud"]["status"] == "installed"
        assert by_id["kdl_nemeton_tracker"]["status"] == "installed"

    print("PASS: test_catalog_and_offline_install")


if __name__ == "__main__":
    test_catalog_and_offline_install()
    print("ALL NOCTURNE MODS CATALOG TESTS PASSED!")
