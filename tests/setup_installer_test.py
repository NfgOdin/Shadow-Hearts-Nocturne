#!/usr/bin/env python3
"""Disc-free regression test suite for Shadow Hearts: Nocturne Setup & Installer."""
from __future__ import annotations

import io
import os
import py_compile
import struct
import subprocess
import sys
import tempfile
from unittest import mock

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
sys.path.insert(0, SCRIPTS_DIR)

import nocturne_setup as setup  # noqa: E402
import nocturne_ui as ui  # noqa: E402
import nocturne_update as upd  # noqa: E402


def make_synthetic_ps2_iso(path: str, boot_serial: str) -> None:
    """Create a tiny valid 2048-byte/sector ISO9660 image with SYSTEM.CNF at LBA 19."""
    total_sectors = 24
    buf = bytearray(total_sectors * 2048)

    # LBA 16: Primary Volume Descriptor
    pvd_off = 16 * 2048
    buf[pvd_off: pvd_off + 6] = b"\x01CD001"
    buf[pvd_off + 6] = 1
    struct.pack_into("<I", buf, pvd_off + 158, 18)     # root dir LBA
    struct.pack_into("<I", buf, pvd_off + 166, 2048)   # root dir size

    # LBA 18: Root directory containing SYSTEM.CNF;1 at LBA 19
    cnf_text = f"BOOT2 = cdrom0:\\{boot_serial};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode("ascii")
    dir_off = 18 * 2048
    name = b"SYSTEM.CNF;1"
    rec_len = 33 + len(name) + (0 if (len(name) & 1) else 1)
    buf[dir_off] = rec_len
    struct.pack_into("<I", buf, dir_off + 2, 19)
    struct.pack_into("<I", buf, dir_off + 10, len(cnf_text))
    buf[dir_off + 25] = 0
    buf[dir_off + 32] = len(name)
    buf[dir_off + 33: dir_off + 33 + len(name)] = name

    # LBA 19: SYSTEM.CNF contents
    cnf_off = 19 * 2048
    buf[cnf_off: cnf_off + len(cnf_text)] = cnf_text

    with open(path, "wb") as f:
        f.write(buf)


def main() -> int:
    # 1. Syntax compile check for all installer/builder scripts
    scripts = [
        "nocturne_ui.py",
        "nocturne_setup.py",
        "nocturne_update.py",
        "koudelka_prepare.py",
        "koudelka_build.py",
        "sh1_build.py",
        "sh2_build.py",
        "sh3_build.py",
    ]
    for s in scripts:
        full = os.path.join(SCRIPTS_DIR, s)
        py_compile.compile(full, doraise=True)

    # 2. CLI --help check on all 6 entrypoints
    for s in ("nocturne_setup.py", "nocturne_update.py", "koudelka_build.py", "sh1_build.py", "sh2_build.py", "sh3_build.py"):
        res = subprocess.run([sys.executable, os.path.join(SCRIPTS_DIR, s), "--help"], capture_output=True, text=True)
        assert res.returncode == 0, f"--help failed for {s}: {res.stderr}"
        if s not in ("nocturne_setup.py", "nocturne_update.py"):
            assert "--install-dir" in res.stdout, f"{s} missing --install-dir"
            assert "--jobs" in res.stdout, f"{s} missing --jobs"
            assert "--skip-app-check" in res.stdout, f"{s} missing --skip-app-check"

    # 3. Hardware-aware compile job presets
    j8, lbl8 = ui.recommended_compile_jobs(cpus=8, ram_gb=8.0)
    assert j8 == 3 and "Safe" in lbl8, f"Expected Safe 3 for 8 GB, got {j8} ({lbl8})"

    j16, lbl16 = ui.recommended_compile_jobs(cpus=12, ram_gb=16.0)
    assert j16 == 6 and "Balanced" in lbl16, f"Expected Balanced 6 for 16 GB, got {j16} ({lbl16})"

    j64, lbl64 = ui.recommended_compile_jobs(cpus=16, ram_gb=64.0)
    assert j64 == 16 and "Turbo" in lbl64, f"Expected Turbo 16 for 64 GB/16t, got {j64} ({lbl64})"

    j64_32, lbl64_32 = ui.recommended_compile_jobs(cpus=32, ram_gb=64.0)
    assert j64_32 == 32 and "Turbo" in lbl64_32, f"Expected Turbo 32 for 64 GB/32t, got {j64_32} ({lbl64_32})"

    presets = ui.build_job_presets(cpus=16, ram_gb=64.0)
    assert len(presets) == 4 and presets[0][1] == 16 and presets[1][1] == 3

    # 4. ProgressBar & Spinner sanity
    pb = ui.ProgressBar("Test", total=10)
    pb.update(5, detail="halfway", force=True)
    pb.finish("all good")
    with ui.Spinner("TestSpin", "checking") as sp:
        sp.set_detail("almost done")

    # 5. Install config round-trip
    with tempfile.TemporaryDirectory() as td:
        cfg_path = os.path.join(td, "install_config.json")
        ui.save_install_config({"install_dir": r"D:\Games\Nocturne", "jobs": 16, "create_desktop_shortcut": True}, cfg_path)
        loaded = ui.load_install_config(cfg_path)
        assert loaded["install_dir"] == r"D:\Games\Nocturne"
        assert loaded["jobs"] == 16
        assert loaded["create_desktop_shortcut"] is True

    # 6. Running-app check: Yes auto-closes & continues; No cancels cleanly without closing
    with mock.patch.object(ui, "find_running_exes", return_value=["Shadow Hearts Nocturne.exe"]), \
         mock.patch.object(ui, "close_exes_gracefully", return_value=[]) as mock_close, \
         mock.patch("builtins.input", return_value="n"), \
         mock.patch.object(ui, "wait_for_keypress") as mock_pause:
        ok = ui.ensure_apps_closed(["Shadow Hearts Nocturne.exe"], interactive=True, pause_on_cancel=True)
        assert ok is False
        assert mock_close.call_count == 0
        assert mock_pause.call_count == 1

    with mock.patch.object(ui, "find_running_exes", return_value=["Shadow Hearts Nocturne.exe"]), \
         mock.patch.object(ui, "close_exes_gracefully", return_value=[]) as mock_close, \
         mock.patch("builtins.input", return_value="y"):
        ok = ui.ensure_apps_closed(["Shadow Hearts Nocturne.exe"], interactive=True, pause_on_cancel=True)
        assert ok is True
        assert mock_close.call_count == 1

    # 7. Smart multi-game ISO header scanner
    with tempfile.TemporaryDirectory() as td:
        iso_sh1 = os.path.join(td, "random_name_1.iso")
        iso_sh2_1 = os.path.join(td, "random_name_2a.iso")
        iso_sh2_2 = os.path.join(td, "random_name_2b.iso")
        iso_sh3 = os.path.join(td, "random_name_3.iso")
        make_synthetic_ps2_iso(iso_sh1, "SLUS_203.47")
        make_synthetic_ps2_iso(iso_sh2_1, "SLUS_210.41")
        make_synthetic_ps2_iso(iso_sh2_2, "SLUS_210.44")
        make_synthetic_ps2_iso(iso_sh3, "SLUS_213.26")

        found = setup.scan_folder_for_discs(td)
        assert found["sh1"] == iso_sh1, f"SH1 mismatch: {found}"
        assert found["sh2_d1"] == iso_sh2_1, f"SH2 D1 mismatch: {found}"
        assert found["sh2_d2"] == iso_sh2_2, f"SH2 D2 mismatch: {found}"
        assert found["sh3"] == iso_sh3, f"SH3 mismatch: {found}"

    # 8. Filename fallback for compressed CHD images
    assert setup._guess_ps2_slot_from_filename("Shadow Hearts - Covenant (USA) (Disc 2).chd") == "sh2_d2"
    assert setup._guess_ps2_slot_from_filename("Shadow Hearts - From the New World (USA).chd") == "sh3"
    assert setup._guess_ps2_slot_from_filename("Shadow Hearts (USA).chd") == "sh1"

    # 9. Updater version comparison & installed-game detection
    same = upd.compare_versions(
        {"channel": "nightly", "sha": "abc1234000", "short_sha": "abc1234", "date": "2026-11-21", "subject": "a"},
        {"channel": "nightly", "sha": "abc1234000", "short_sha": "abc1234", "date": "2026-11-21", "subject": "a"},
    )
    assert same["up_to_date"] is True and same["update_available"] is False

    newer = upd.compare_versions(
        {"channel": "main", "sha": "1111111000", "short_sha": "1111111", "date": "2026-11-20", "subject": "old"},
        {"channel": "main", "sha": "2222222000", "short_sha": "2222222", "date": "2026-11-21", "subject": "new"},
    )
    assert newer["up_to_date"] is False and newer["update_available"] is True and newer["channel_switch"] is False

    ch_sw = upd.compare_versions(
        {"channel": "main", "sha": "1111111000", "short_sha": "1111111", "date": "2026-11-20", "subject": "old"},
        {"channel": "nightly", "sha": "1111111000", "short_sha": "1111111", "date": "2026-11-20", "subject": "old"},
    )
    assert ch_sw["channel_switch"] is True and ch_sw["update_available"] is True

    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "games", "koudelka"), exist_ok=True)
        os.makedirs(os.path.join(td, "games", "sh1"), exist_ok=True)
        open(os.path.join(td, "games", "koudelka", "Koudelka.exe"), "wb").close()
        open(os.path.join(td, "games", "sh1", "SH1.exe"), "wb").close()
        assert upd.detect_installed_games(td) == ["koudelka", "sh1"]

        # Verify sync_launcher_source_repo writes source_repo while preserving existing launcher.ini keys
        ini_p = os.path.join(td, "launcher.ini")
        with open(ini_p, "w", encoding="utf-8") as f:
            f.write("# Keep me\nchimes = off\n")
        ui.sync_launcher_source_repo(td, repo_root=r"C:\MyRepo")
        raw = open(ini_p, "r", encoding="utf-8").read()
        assert "# Keep me" in raw and "chimes = off" in raw and r"source_repo = C:\MyRepo" in raw

    # 10. Self-update handoff passes --apply + --relaunch, and up-to-date non-interactive still relaunches
    fake_remote = {"channel": "main", "sha": "deadbeef1234", "short_sha": "deadbee", "date": "2026-11-22", "subject": "ok"}
    fake_local = {"channel": "main", "sha": "deadbeef1234", "short_sha": "deadbee", "date": "2026-11-22", "subject": "ok", "is_git": True, "git_branch": "main"}
    with mock.patch.object(upd, "fetch_github_latest", return_value=fake_remote), \
         mock.patch.object(upd, "detect_local_version", return_value=fake_local), \
         mock.patch.object(ui, "load_install_config", return_value={"update_channel": "main"}), \
         mock.patch.object(ui, "resolve_install_dir", return_value=REPO_ROOT), \
         mock.patch.object(upd, "_relaunch_launcher", return_value=True) as mock_rel:
        rc = upd.run_interactive_updater("main", force=False, non_interactive=True, relaunch=True, launcher_exe=r"C:\Fake\Shadow Hearts Nocturne.exe")
        assert rc == 0
        assert mock_rel.call_count == 1, f"Expected relaunch on up-to-date non-interactive update, got {mock_rel.call_count}"

    with mock.patch.object(upd, "_wait_for_pid_exit"), \
         mock.patch.object(upd, "detect_local_version", return_value=fake_local), \
         mock.patch.object(upd, "fetch_github_latest", return_value=fake_remote), \
         mock.patch.object(upd, "update_repo_source", return_value="git"), \
         mock.patch.object(upd, "try_install_prebuilt_engine", return_value=False), \
         mock.patch.object(upd, "detect_installed_games", return_value=[]), \
         mock.patch.object(ui, "load_install_config", return_value={"update_channel": "main"}), \
         mock.patch.object(ui, "resolve_install_dir", return_value=REPO_ROOT), \
         mock.patch.object(upd.subprocess, "Popen") as mock_popen:
        rc = upd.run_self_update("main", wait_pid=0, relaunch=True, launcher_exe=r"C:\Fake\Shadow Hearts Nocturne.exe")
        assert rc == 0
        assert mock_popen.call_count == 1
        spawned_cmd = mock_popen.call_args[0][0]
        assert "--apply" in spawned_cmd and "--relaunch" in spawned_cmd and "--non-interactive" in spawned_cmd, f"Bad handoff cmd: {spawned_cmd}"
        assert "--launcher-exe" in spawned_cmd, f"Missing --launcher-exe in handoff cmd: {spawned_cmd}"

    # 11. Nocturne Mod Studio: visual rule -> Lua generation, syntax validation, install & .nocturne-mod ZIP export
    import zipfile
    import nocturne_mod_studio as studio

    py_compile.compile(os.path.join(SCRIPTS_DIR, "nocturne_mod_studio.py"), doraise=True)

    studio_help = subprocess.run(
        [sys.executable, os.path.join(SCRIPTS_DIR, "nocturne_mod_studio.py"), "--help"],
        capture_output=True, text=True, timeout=15
    )
    assert studio_help.returncode == 0 and "--game" in studio_help.stdout, studio_help.stderr

    meta = {"name": "Crimson Bounty", "author": "Tester", "version": "1.0", "game": "sh2-covenant", "folder": "CrimsonBounty"}
    rules = {
        "start_cash": 500,
        "win_cash": 150,
        "win_heal": 25,
        "perf_cash": 100,
        "perf_streak_scale": True,
        "miss_penalty": 50,
        "welcome_toast": "Crimson Bounty Active!",
    }
    lua_src = studio.generate_lua_from_rules(meta, rules)
    assert "function on_game_ready()" in lua_src and "add_cash(500)" in lua_src
    assert "function on_battle_win(total_wins)" in lua_src and "heal_party(25)" in lua_src
    assert "function on_ring_perfect(streak)" in lua_src and "function on_ring_miss(prev_streak)" in lua_src

    v_ok, v_errs = studio.validate_lua_syntax(lua_src)
    assert v_ok is True and not v_errs, v_errs

    v_bad, b_errs = studio.validate_lua_syntax("function on_battle_win(t)\n  if t > 0 then\n    add_cash(10)\nend\n")
    assert v_bad is False and len(b_errs) >= 1, b_errs

    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, "games", "sh2-covenant"), exist_ok=True)
        with open(os.path.join(td, "games", "sh2-covenant", "keys.ini"), "w", encoding="utf-8") as kf:
            kf.write("fullscreen = 0\n")
        res = studio.install_mod_to_game(td, "sh2-covenant", {
            "meta": meta,
            "rules": rules,
            "lua_code": lua_src,
        })
        assert res["ok"] is True, res
        mod_dir = os.path.join(td, "games", "sh2-covenant", "mods", "CrimsonBounty")
        assert os.path.isfile(os.path.join(mod_dir, "mod.json"))
        assert os.path.isfile(os.path.join(mod_dir, "scripts", "crimsonbounty.lua"))
        keys_txt = open(os.path.join(td, "games", "sh2-covenant", "keys.ini"), "r", encoding="utf-8").read()
        assert "CrimsonBounty" in keys_txt
        listed = studio.list_game_mods(td, "sh2-covenant")
        assert any(m["folder"] == "CrimsonBounty" and m["name"] == "Crimson Bounty" for m in listed), listed

    fname, zip_bytes = studio.build_nocturne_mod_zip_bytes({
        "game": "sh2-covenant", "meta": meta, "rules": rules, "lua_code": lua_src,
        "pnach_code": studio.generate_pnach_from_rows("Crimson Bounty", [
            {"enabled": True, "boot_only": False, "addr": "00102D30", "width": "word", "value": "00002710", "comment": "10k Gold"}
        ]),
    })
    assert fname.endswith(".nocturne-mod")
    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        names = set(zf.namelist())
        assert "CrimsonBounty/mod.json" in names and "CrimsonBounty/scripts/crimsonbounty.lua" in names, names
        assert "CrimsonBounty/patches/crimsonbounty.pnach" in names, names

    # Verify Mod Studio v2 conflict checker & Layer 4 C++ SDK scaffold generator
    with tempfile.TemporaryDirectory() as td2:
        studio.install_mod_to_game(td2, "sh1", {
            "meta": {"name": "Alpha", "folder": "Alpha", "game": "sh1"},
            "lua_code": "function on_boot() end\n",
            "pnach_code": "patch=0,EE,00100008,word,000003E8\n",
        })
        studio.install_mod_to_game(td2, "sh1", {
            "meta": {"name": "Beta", "folder": "Beta", "game": "sh1"},
            "lua_code": "function on_boot() end\n",
            "pnach_code": "patch=0,EE,00100008,word,000007D0\n",
        })
        confs = studio.detect_game_conflicts(td2, "sh1")
        assert len(confs) == 1 and confs[0]["layer"] == "patches" and confs[0]["winner"] == "Alpha", confs
        sdk_res = studio.generate_cpp_plugin_scaffold(td2, "sh1", "Alpha", "Alpha")
        assert sdk_res["ok"] is True and os.path.isfile(sdk_res["cpp_path"]) and os.path.isfile(sdk_res["cmd_path"]), sdk_res

    print("PASS: setup_installer_test (all 11 installer/updater/relaunch/studio v2 checks passed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
