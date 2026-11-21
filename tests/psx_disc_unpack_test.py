#!/usr/bin/env python3
"""Disc-free unit test for scripts/koudelka_prepare.py using synthetic Mode 2/2352 PS1 .bin images."""
from __future__ import annotations

import json
import os
import shutil
import struct
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
import koudelka_prepare as kp  # noqa: E402


def make_m2_sector(user_data: bytes, submode: int = 0x08, channel: int = 0) -> bytes:
    buf = bytearray(2352)
    buf[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
    buf[15] = 2  # Mode 2
    subhdr = bytes([1, channel, submode, 0])
    buf[16:20] = subhdr
    buf[20:24] = subhdr
    max_len = 2324 if (submode & 0x20) else 2048
    buf[24: 24 + min(len(user_data), max_len)] = user_data[:max_len]
    return bytes(buf)


def make_dir_record(name: bytes, lba: int, size: int, is_dir: bool = False) -> bytes:
    rec = bytearray(33 + len(name))
    rec[0] = len(rec) + (len(rec) & 1)
    if len(rec) & 1:
        rec.append(0)
    struct.pack_into("<I", rec, 2, lba)
    struct.pack_into(">I", rec, 6, lba)
    struct.pack_into("<I", rec, 10, size)
    struct.pack_into(">I", rec, 14, size)
    rec[25] = 2 if is_dir else 0
    rec[32] = len(name)
    rec[33: 33 + len(name)] = name
    return bytes(rec)


def build_synthetic_disc(path: str, vol_id: str, boot_exe: str, unique_payload: bytes, shared_payload: bytes,
                         str_sectors: list[bytes], exe_payload: bytes) -> None:
    # Layout:
    #   LBA 0..15: system area
    #   LBA 16: PVD
    #   LBA 18: root directory (1 sector)
    #   LBA 20: SYSTEM.CNF
    #   LBA 22: boot_exe (2 sectors)
    #   LBA 25: SHARED.DAT (1 sector)
    #   LBA 26: UNIQUE.DAT (1 sector)
    #   LBA 28: MOVIE.STR (len(str_sectors) raw sectors)
    #   LBA 35: DUMMY.ZYX (512 sectors = 1 MiB of 0xFF)
    cnf = f"BOOT=cdrom:\\{boot_exe};1\r\nTCB=4\r\nEVENT=10\r\nSTACK=80180000\r\n".encode("ascii")
    fill_sectors = 512  # 1 MiB
    fill_size = fill_sectors * 2048

    root_dir = bytearray()
    root_dir.extend(make_dir_record(b"\x00", 18, 2048, is_dir=True))
    root_dir.extend(make_dir_record(b"\x01", 18, 2048, is_dir=True))
    root_dir.extend(make_dir_record(b"SYSTEM.CNF;1", 20, len(cnf)))
    root_dir.extend(make_dir_record(f"{boot_exe};1".encode("ascii"), 22, len(exe_payload)))
    root_dir.extend(make_dir_record(b"SHARED.DAT;1", 25, len(shared_payload)))
    root_dir.extend(make_dir_record(b"UNIQUE.DAT;1", 26, len(unique_payload)))
    root_dir.extend(make_dir_record(b"MOVIE.STR;1", 28, len(str_sectors) * 2048))
    root_dir.extend(make_dir_record(b"DUMMY.ZYX;1", 35, fill_size))

    pvd = bytearray(2048)
    pvd[0:6] = b"\x01CD001"
    pvd[40:72] = vol_id.encode("ascii").ljust(32, b" ")
    struct.pack_into("<I", pvd, 158, 18)
    struct.pack_into("<I", pvd, 166, len(root_dir))

    total_sectors = 35 + fill_sectors + 4
    sectors = [make_m2_sector(b"") for _ in range(total_sectors)]
    sectors[16] = make_m2_sector(bytes(pvd))
    sectors[18] = make_m2_sector(bytes(root_dir))
    sectors[20] = make_m2_sector(cnf, submode=0x89)
    for i in range((len(exe_payload) + 2047) // 2048):
        sectors[22 + i] = make_m2_sector(exe_payload[i * 2048: (i + 1) * 2048])
    sectors[25] = make_m2_sector(shared_payload)
    sectors[26] = make_m2_sector(unique_payload)
    for i, raw_sec in enumerate(str_sectors):
        sectors[28 + i] = raw_sec
    ff_sec = make_m2_sector(b"\xff" * 2048)
    for i in range(fill_sectors):
        sectors[35 + i] = ff_sec

    with open(path, "wb") as f:
        for s in sectors:
            f.write(s)


def main() -> int:
    tmp = tempfile.mkdtemp(prefix="psx_disc_test_")
    try:
        src_dir = os.path.join(tmp, "src")
        out_dir = os.path.join(tmp, "out_game")
        os.makedirs(src_dir, exist_ok=True)

        # Synthetic 4 KiB PS-X EXE (2048-byte header + 2048-byte text)
        exe = bytearray(4096)
        exe[0:8] = b"PS-X EXE"
        struct.pack_into("<10I", exe, 16, 0x800A0A20, 0, 0x80010000, 0x800, 0, 0, 0, 0, 0x801FFFF0, 0)
        exe[76:100] = b"Sony Computer Ent Inc.\x00\x00"
        exe[2048:2064] = b"\x08\x00\xe0\x03\x00\x00\x00\x00" * 2
        shared = b"KOUDELKA_SHARED_DATA_BLOCK" * 40

        specs = [
            (1, "SLUS_01051", "SLUS_010.51"),
            (2, "SLUS_01100", "SLUS_011.00"),
            (3, "SLUS_01101", "SLUS_011.01"),
            (4, "SLUS_01102", "SLUS_011.02"),
        ]
        for dnum, vol, boot in specs:
            str_secs = [
                make_m2_sector(f"VID_D{dnum}_S0".encode() + b"\x11" * 100, submode=0x48, channel=1),
                make_m2_sector(f"AUD_D{dnum}_S1".encode() + b"\x22" * 2300, submode=0x64, channel=1),
            ]
            build_synthetic_disc(
                os.path.join(src_dir, f"disc{dnum}.bin"),
                vol, boot, f"UNIQUE_DISC_{dnum}".encode("ascii"), shared, str_secs, bytes(exe)
            )

        discs = kp.find_koudelka_discs([src_dir])
        assert sorted(discs.keys()) == [1, 2, 3, 4], f"Expected 4 discs, got {discs.keys()}"

        summary = kp.unpack_discs(discs, out_dir, verify=True)
        assert summary["disc_count"] == 4
        assert summary["exe_identical_across_discs"] is True
        assert summary["exe"]["pc"] == 0x800A0A20
        assert summary["exe"]["load_addr"] == 0x80010000
        assert summary["fill_saved_bytes"] == 4 * 512 * 2048
        assert summary["dedup_saved_bytes"] >= 3 * (len(shared) + len(exe))

        # Check manifest contains DISC, FILE (F1 & RAW2352), and FILL rows
        manifest = open(os.path.join(out_dir, "disc_files", "disc_manifest.tsv"), "r", encoding="utf-8").read()
        assert manifest.count("\nDISC\t") == 4
        assert "\tRAW2352\tMOVIE.STR\t" in manifest
        assert "\nFILL\t1\t35\t512\t1048576\tF1\tDUMMY.ZYX\tFF" in manifest
        assert os.path.isfile(os.path.join(out_dir, "exe", "SLUS_010.51"))
        assert os.path.isfile(os.path.join(out_dir, "disc_files", "disc_info.json"))
        info = json.loads(open(os.path.join(out_dir, "disc_files", "disc_info.json"), "r", encoding="utf-8").read())
        assert info["discs"]["4"]["boot_exe"] == "SLUS_011.02"

        print("PASS: psx_disc_unpack_test (4 synthetic Mode 2/2352 discs, dedup, RAW2352 XA/STR, FILL, PS-X EXE)")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
