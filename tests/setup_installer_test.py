#!/usr/bin/env python3
"""Disc-free regression test suite for Shadow Hearts: Nocturne Setup & Installer."""
from __future__ import annotations

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
        "koudelka_prepare.py",
        "koudelka_build.py",
        "sh1_build.py",
        "sh2_build.py",
        "sh3_build.py",
    ]
    for s in scripts:
        full = os.path.join(SCRIPTS_DIR, s)
        py_compile.compile(full, doraise=True)

    # 2. CLI --help check on all 5 entrypoints
    for s in ("nocturne_setup.py", "koudelka_build.py", "sh1_build.py", "sh2_build.py", "sh3_build.py"):
        res = subprocess.run([sys.executable, os.path.join(SCRIPTS_DIR, s), "--help"], capture_output=True, text=True)
        assert res.returncode == 0, f"--help failed for {s}: {res.stderr}"
        if s != "nocturne_setup.py":
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

    print("PASS: setup_installer_test (all 8 installer/scanner/app-close/hardware-preset checks passed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
