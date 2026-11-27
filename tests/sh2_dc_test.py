#!/usr/bin/env python3
"""Disc-free unit tests for Shadow Hearts: Covenant Director's Cut clean-room upgrader."""
from __future__ import annotations

import os
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

import sh2_dc_upgrade as dc  # noqa: E402


def test_pkf_key_roundtrip() -> None:
    for name in ("FIELD.BIN", "KS_00_U.MSC", "SH3_SP.LSD", "006W00.SSE", "_QUIZ.MPS", "TAILOR.DAT"):
        enc = dc.encode_pkf_key(name)
        assert len(enc) == 8
        assert dc.decode_pkf_key(enc) == name, f"round-trip failed for {name}: {dc.decode_pkf_key(enc)!r}"


def test_pkf_tree_insert_and_serialize() -> None:
    # Build synthetic 2-level PKFINFO.TBL:
    # root: [FIELD.BIN, FIELD(dir -> 2), LSD(dir -> 5)]
    # FIELD: [MPS(dir -> 1), OLD.DAT]
    # MPS: [A.MSC]
    # LSD: [N001.LSD]
    ents = [
        (dc.encode_pkf_key("FIELD.BIN"), 0, 1024),
        (dc.encode_pkf_key("FIELD"), 0, 0x80000000 | (3 * 16)),
        (dc.encode_pkf_key("LSD"), 0, 0x80000000 | (6 * 16)),
        # idx 3 (FIELD children)
        (dc.encode_pkf_key("MPS"), 128, 0x80000000 | (2 * 16)),
        (dc.encode_pkf_key("OLD.DAT"), 256, 512),
        # idx 5 (FIELD/MPS children)
        (dc.encode_pkf_key("A.MSC"), 0, 400),
        # idx 6 (LSD children)
        (dc.encode_pkf_key("N001.LSD"), 0, 8192),
    ]
    raw = struct.pack("<HHI", len(ents), 16, len(ents) * 16) + b"\x00" * 8
    raw += b"".join(k + struct.pack("<II", o, s) for k, o, s in ents)

    tbl = dc.PkfTable(raw)
    files = tbl.list_files()
    assert files["FIELD/MPS/A.MSC"] == ("FIELD.PKB", 128, 400)
    assert files["LSD/N001.LSD"] == ("LSD.PKB", 0, 8192)

    # Add a new DC file and replace an existing one
    tbl.set_or_add_file("FIELD/MPS/KS_00_U.MSC", 128 + 1024, 30000)
    tbl.set_or_add_file("LSD/N001.LSD", 4096, 9000)
    tbl.set_or_add_file("LSD/SH3_SP.LSD", 8192, 50000)

    rebuilt = tbl.serialize()
    tbl2 = dc.PkfTable(rebuilt)
    f2 = tbl2.list_files()
    assert f2["FIELD/MPS/A.MSC"] == ("FIELD.PKB", 128, 400)
    assert f2["FIELD/MPS/KS_00_U.MSC"] == ("FIELD.PKB", 128 + 1024, 30000)
    assert f2["LSD/N001.LSD"] == ("LSD.PKB", 4096, 9000)
    assert f2["LSD/SH3_SP.LSD"] == ("LSD.PKB", 8192, 50000)


def test_tailor_dat_splice() -> None:
    def make_tailor(count: int, msg_extra_empty: bool) -> bytes:
        entries = []
        for i in range(count - 1):
            entries.append((b"mdl\x00", struct.pack("<4I", i, i + 1, i + 2, i + 3)))
        # Last entry: msg\0 with 22 entries, slot 21 empty if US
        n = 22
        offs = []
        pool = bytearray()
        for i in range(n):
            offs.append(len(pool))
            if i == 21 and msg_extra_empty:
                pool += b"\x00"
            else:
                pool += f"Dress{i}\x00".encode("ascii")
        msg = struct.pack("<H", n) + struct.pack(f"<{n}H", *offs) + bytes(pool)
        entries.append((b"msg\x00", msg))

        first = (8 + 8 * len(entries) + 15) & ~15
        body = bytearray()
        eoffs = []
        for _, d in entries:
            eoffs.append(first + len(body))
            body += d + bytes(-len(d) % 16)
        hdr = bytearray(first)
        struct.pack_into("<II", hdr, 0, len(entries), first + len(body))
        for i, (t, _) in enumerate(entries):
            hdr[8 + 8 * i : 12 + 8 * i] = t
            struct.pack_into("<I", hdr, 12 + 8 * i, eoffs[i])
        return bytes(hdr) + bytes(body)

    us_td = make_tailor(31, msg_extra_empty=True)
    dc_td = make_tailor(32, msg_extra_empty=False)
    merged = dc.patch_tailor_dat(us_td, dc_td)
    cnt, total = struct.unpack_from("<II", merged, 0)
    assert cnt == 32
    assert total == len(merged)
    assert dc.FOOL_COSTUME_DESC.encode("ascii") in merged


def test_sse_bank_id_preserve() -> None:
    us = bytearray(64)
    us[0:4] = b"SDRH"
    struct.pack_into("<I", us, 0x0C, 0x9005)
    struct.pack_into("<H", us, 0x2A, 0x9005)
    jp = bytearray(64)
    jp[0:4] = b"SDRH"
    struct.pack_into("<I", jp, 0x0C, 0x9006)
    struct.pack_into("<H", jp, 0x2A, 0x9006)
    jp[48:64] = b"\xAB" * 16

    patched = dc.patch_sse_voice_bank(bytes(us), bytes(jp))
    assert struct.unpack_from("<I", patched, 0x0C)[0] == 0x9005
    assert struct.unpack_from("<H", patched, 0x2A)[0] == 0x9005
    assert patched[48:64] == b"\xAB" * 16


def test_lsdh_pcms_swap_and_sbtd_graft() -> None:
    def make_lsd(rate: int, pcms_payload: bytes, sbtd_payload: bytes) -> bytes:
        hdr = bytearray(0x40)
        hdr[0:4] = b"LSDH"
        # Track 0: SBTD (2 qwords = 32 bytes)
        t0 = bytearray(32)
        t0[0:4] = b"SBTD"
        struct.pack_into("<HH", t0, 0x10, 2, 0)
        struct.pack_into("<I", t0, 0x18, (len(sbtd_payload) + 15) // 16)
        # Track 1: PCMS (4 qwords = 64 bytes)
        t1 = bytearray(64)
        t1[0:4] = b"PCMS"
        struct.pack_into("<HH", t1, 0x10, 4, 2)
        struct.pack_into("<II", t1, 0x24, (rate << 16) | 2, len(pcms_payload) // 2)
        # Packets: SBTD + 2 PCMS packets + terminator
        half = len(pcms_payload) // 2
        p0 = struct.pack("<4I", 0, 0, len(sbtd_payload) // 16, 0) + sbtd_payload
        p1 = struct.pack("<4I", 1, 0, half // 16, 0) + pcms_payload[:half]
        p2 = struct.pack("<4I", 1, 1, half // 16, 0) + pcms_payload[half:]
        end = struct.pack("<4I", 0xFFFFFFFF, 0, 0, 0)
        return bytes(hdr + t0 + t1 + p0 + p1 + p2 + end)

    us_lsd = make_lsd(48000, b"\x11" * 64, b"EN_SUBS_________" * 2)
    dc_lsd = make_lsd(48000, b"\x22" * 64, b"JP_SUBS_________" * 1)
    swapped, st = dc.swap_lsd_pcms_audio(us_lsd, dc_lsd)
    assert st == "swapped"
    assert b"EN_SUBS_________" in swapped
    assert b"\x22" * 32 in swapped
    assert b"\x11" * 32 not in swapped

    # Rate mismatch cleanly skipped
    dc_44 = make_lsd(44100, b"\x33" * 64, b"JP_SUBS_________")
    kept, st2 = dc.swap_lsd_pcms_audio(us_lsd, dc_44)
    assert st2 == "skipped_rate"
    assert kept == us_lsd

    # SBTD subtitle graft into DC movie
    grafted = dc.graft_lsd_sbtd_subtitles(dc_lsd, us_lsd)
    assert b"EN_SUBS_________" in grafted
    assert b"\x22" * 32 in grafted


def test_iso_meta_patch() -> None:
    # 2 sectors in disc_meta.bin: sector 16 (PVD) + sector 17 (root dir with PKFINFO.TBL)
    pvd = bytearray(2048)
    pvd[0:6] = b"\x01CD001"
    struct.pack_into("<I", pvd, 80, 1000)
    struct.pack_into(">I", pvd, 84, 1000)

    rdir = bytearray(2048)
    nm = b"PKFINFO.TBL;1"
    rec = bytearray(34 + len(nm))
    rec[0] = len(rec)
    struct.pack_into("<I", rec, 2, 50)
    struct.pack_into(">I", rec, 6, 50)
    struct.pack_into("<I", rec, 10, 71680)
    struct.pack_into(">I", rec, 14, 71680)
    rec[32] = len(nm)
    rec[33 : 33 + len(nm)] = nm
    rdir[0 : len(rec)] = rec

    patched = dc._patch_iso_dir_and_pvd(
        bytes(pvd + rdir),
        [(16, 2, 0)],
        old_tbl_lba=50,
        old_tbl_size=71680,
        new_tbl_lba=2000,
        new_tbl_size=74000,
        new_total_sectors=2040,
    )
    assert struct.unpack_from("<I", patched, 80)[0] == 2040
    assert struct.unpack_from(">I", patched, 84)[0] == 2040
    assert struct.unpack_from("<I", patched, 2048 + 2)[0] == 2000
    assert struct.unpack_from(">I", patched, 2048 + 6)[0] == 2000
    assert struct.unpack_from("<I", patched, 2048 + 10)[0] == 74000
    assert struct.unpack_from(">I", patched, 2048 + 14)[0] == 74000


if __name__ == "__main__":
    test_pkf_key_roundtrip()
    test_pkf_tree_insert_and_serialize()
    test_tailor_dat_splice()
    test_sse_bank_id_preserve()
    test_lsdh_pcms_swap_and_sbtd_graft()
    test_iso_meta_patch()
    print("sh2_dc_test: ALL 6 TESTS PASSED")
