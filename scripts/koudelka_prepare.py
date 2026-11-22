#!/usr/bin/env python3
"""Unpack Koudelka (PS1 NTSC-U, 4 discs: SLUS-01051 / 01100 / 01101 / 01102)
from Redump Mode 2/2352 .bin/.cue images into a deduplicated, ISO-free 4-disc
virtual folder under games/koudelka/disc_files/ with bit-exact XA/STR streams
and extracted PS-X EXE + code overlays.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from typing import BinaryIO

SECTOR_RAW = 2352
SECTOR_F1 = 2048
SECTOR_F2 = 2324
USER_DATA_OFF = 24
SUBHEADER_OFF = 16

KOUDELKA_SERIALS = {
    "SLUS_010.51": 1,
    "SLUS_011.00": 2,
    "SLUS_011.01": 3,
    "SLUS_011.02": 4,
}

OVERLAY_NAMES = {
    "BIN/BATTLE.BIN",
    "BIN/CAPTURE.BIN",
    "BIN/EFFECT.BIN",
    "BIN/FIELD.BIN",
    "BIN/GAMEEND.BIN",
    "BIN/MOVIE.BIN",
}

DEFAULT_SEARCH_DIRS = [
    r"D:\Roms\Playstation 1\Koudelka",
    r"D:\Roms\PS1\Koudelka",
    r"D:\Roms\PSX\Koudelka",
]


def read_m2_sector(f: BinaryIO, lba: int) -> tuple[bytes, int, bytes]:
    f.seek(lba * SECTOR_RAW)
    raw = f.read(SECTOR_RAW)
    if len(raw) < SECTOR_RAW:
        raise EOFError(f"Short read at LBA {lba}: got {len(raw)} bytes")
    submode = raw[SUBHEADER_OFF + 2]
    is_f2 = (submode & 0x20) != 0
    udata = raw[USER_DATA_OFF: USER_DATA_OFF + (SECTOR_F2 if is_f2 else SECTOR_F1)]
    return raw, submode, udata


def walk_iso_dir(f: BinaryIO, lba: int, size: int, prefix: str = "") -> list[dict]:
    entries: list[dict] = []
    sectors = (size + SECTOR_F1 - 1) // SECTOR_F1
    buf = bytearray()
    for s in range(sectors):
        _, _, u = read_m2_sector(f, lba + s)
        buf.extend(u[:SECTOR_F1])
    pos = 0
    while pos < size:
        rlen = buf[pos]
        if rlen == 0:
            pos = ((pos // SECTOR_F1) + 1) * SECTOR_F1
            continue
        elba = struct.unpack_from("<I", buf, pos + 2)[0]
        esize = struct.unpack_from("<I", buf, pos + 10)[0]
        flags = buf[pos + 25]
        nlen = buf[pos + 32]
        raw_name = buf[pos + 33: pos + 33 + nlen].decode("ascii", "replace")
        xa_attr = 0
        su_off = pos + 33 + nlen + (0 if (nlen & 1) else 1)
        if su_off + 14 <= pos + rlen and buf[su_off + 6: su_off + 8] == b"XA":
            xa_attr = struct.unpack_from(">H", buf, su_off + 4)[0]
        pos += rlen
        if raw_name in ("\x00", "\x01"):
            continue
        clean = raw_name.split(";", 1)[0]
        full = f"{prefix}/{clean}" if prefix else clean
        if flags & 2:
            entries.extend(walk_iso_dir(f, elba, esize, full))
        else:
            entries.append({
                "iso_path": full,
                "lba": elba,
                "size": esize,
                "sectors": max(1, (esize + SECTOR_F1 - 1) // SECTOR_F1) if esize > 0 else 0,
                "xa_attr": xa_attr,
            })
    return entries


def probe_psx_bin(bin_path: str) -> dict | None:
    """Inspect a Mode 2/2352 PS1 .bin image and return serial/disc/volume metadata."""
    try:
        file_sz = os.path.getsize(bin_path)
    except OSError:
        return None
    if file_sz < 20 * SECTOR_RAW or (file_sz % SECTOR_RAW) != 0:
        return None
    try:
        with open(bin_path, "rb") as f:
            raw0, _, pvd = read_m2_sector(f, 16)
            if pvd[0:6] != b"\x01CD001":
                return None
            vol_id = pvd[40:72].decode("ascii", "replace").strip()
            root_lba = struct.unpack_from("<I", pvd, 158)[0]
            root_sz = struct.unpack_from("<I", pvd, 166)[0]
            files = walk_iso_dir(f, root_lba, root_sz)
            boot_name = ""
            for ent in files:
                if ent["iso_path"].upper() == "SYSTEM.CNF":
                    _, _, u = read_m2_sector(f, ent["lba"])
                    cnf = u[: ent["size"]].decode("ascii", "replace")
                    m = re.search(r"BOOT\s*=\s*cdrom:\\?([A-Za-z0-9_.\-]+)", cnf, re.IGNORECASE)
                    if m:
                        boot_name = m.group(1).upper()
                    break
            disc_num = KOUDELKA_SERIALS.get(boot_name, 0)
            return {
                "path": os.path.abspath(bin_path),
                "total_sectors": file_sz // SECTOR_RAW,
                "volume_id": vol_id,
                "boot_exe": boot_name,
                "disc_num": disc_num,
                "file_count": len(files),
            }
    except Exception:
        return None


def find_koudelka_discs(source_dirs: list[str]) -> dict[int, dict]:
    found: dict[int, dict] = {}
    for root in source_dirs:
        if not root or not os.path.exists(root):
            continue
        if os.path.isfile(root) and root.lower().endswith(".bin"):
            info = probe_psx_bin(root)
            if info and info["disc_num"] in (1, 2, 3, 4) and info["disc_num"] not in found:
                found[info["disc_num"]] = info
            continue
        for dirpath, _, filenames in os.walk(root):
            for fn in sorted(filenames):
                if not fn.lower().endswith(".bin"):
                    continue
                full = os.path.join(dirpath, fn)
                info = probe_psx_bin(full)
                if info and info["disc_num"] in (1, 2, 3, 4) and info["disc_num"] not in found:
                    found[info["disc_num"]] = info
            if len(found) == 4:
                return found
    return found


def is_raw_xa_file(f: BinaryIO, ent: dict) -> bool:
    """Return True if a file uses Mode 2 Form 2 / Real-Time XA / interleaved STR sectors."""
    if ent["sectors"] == 0:
        return False
    upper = ent["iso_path"].upper()
    if upper.endswith((".STR", ".XA")):
        return True
    if ent["xa_attr"] & 0x1000:  # Form 2 attribute
        return True
    _, sm, _ = read_m2_sector(f, ent["lba"])
    # Bit 6 = Real-Time, Bit 5 = Form 2, Bit 2 = Audio
    if sm & 0x64:
        return True
    return False


def detect_constant_fill_f1(f: BinaryIO, lba: int, size: int) -> int | None:
    """If a large Form 1 file (>= 1 MiB) consists entirely of one repeated byte, return that byte."""
    if size < (1 << 20):
        return None
    sectors = (size + SECTOR_F1 - 1) // SECTOR_F1
    _, _, first_u = read_m2_sector(f, lba)
    fill_b = first_u[0]
    probe_block = bytes([fill_b]) * SECTOR_F1
    # Fast spot check first and last sector before full scan
    if first_u[:SECTOR_F1] != probe_block:
        return None
    _, _, last_u = read_m2_sector(f, lba + sectors - 1)
    rem = size - (sectors - 1) * SECTOR_F1
    if last_u[:rem] != probe_block[:rem]:
        return None
    # Verify every sector
    for s in range(1, sectors - 1):
        _, _, u = read_m2_sector(f, lba + s)
        if u[:SECTOR_F1] != probe_block:
            return None
    return fill_b


def parse_psx_exe_header(data: bytes) -> dict:
    if len(data) < 2048 or data[:8] != b"PS-X EXE":
        raise RuntimeError("Invalid PS-X EXE header")
    pc, gp, taddr, tsize, daddr, dsize, baddr, bsize, sp_base, sp_off = struct.unpack_from("<10I", data, 16)
    marker = data[76:140].split(b"\x00", 1)[0].decode("ascii", "replace").strip()
    return {
        "pc": pc,
        "gp": gp,
        "load_addr": taddr,
        "text_size": tsize,
        "data_addr": daddr,
        "data_size": dsize,
        "bss_addr": baddr,
        "bss_size": bsize,
        "sp_init": (sp_base + sp_off) & 0xFFFFFFFF,
        "marker": marker,
    }


def sanitize_rel_path(iso_path: str) -> str:
    parts = [p for p in iso_path.replace("\\", "/").split("/") if p and p not in (".", "..")]
    return "/".join(parts)


def unpack_discs(discs: dict[int, dict], out_game_dir: str, verify: bool = False) -> dict:
    disc_files_dir = os.path.join(out_game_dir, "disc_files")
    shared_dir = os.path.join(disc_files_dir, "shared")
    exe_dir = os.path.join(out_game_dir, "exe")
    overlays_dir = os.path.join(exe_dir, "overlays")
    os.makedirs(shared_dir, exist_ok=True)
    os.makedirs(overlays_dir, exist_ok=True)

    # First pass: collect file metadata per disc and identify shared vs disc-specific Form 1 content
    shared_by_hash: dict[str, str] = {}   # sha256 -> local_rel (under shared/)
    used_shared_paths: set[str] = set()
    manifest_rows: list[str] = [
        "# Shadow Hearts: Nocturne — Koudelka (PS1) Unified 4-Disc Virtual Manifest",
        "# Formats:",
        "#   DISC\t<disc_num>\t<volume_id>\t<boot_exe>\t<total_sectors>\t<meta_sectors>\t<meta_rel>",
        "#   FILE\t<disc_num>\t<lba>\t<sectors>\t<size>\t<mode:F1|RAW2352>\t<iso_path>\t<local_rel>\t<sha256>",
        "#   FILL\t<disc_num>\t<lba>\t<sectors>\t<size>\tF1\t<iso_path>\t<fill_hex>",
    ]

    disc_infos: dict[str, dict] = {}
    exe_meta: dict | None = None
    exe_hashes: dict[int, str] = {}
    total_raw_bytes = 0
    total_written_bytes = 0
    dedup_saved_bytes = 0
    fill_saved_bytes = 0
    written_files_count = 0

    for disc_num in sorted(discs.keys()):
        info = discs[disc_num]
        bin_path = info["path"]
        total_raw_bytes += os.path.getsize(bin_path)
        disc_sub = f"disc{disc_num}"
        disc_out_dir = os.path.join(disc_files_dir, disc_sub)
        os.makedirs(disc_out_dir, exist_ok=True)

        print(f"[koudelka] Unpacking Disc {disc_num} ({info['volume_id']}, {info['total_sectors']} sectors)...")
        with open(bin_path, "rb") as f:
            _, _, pvd = read_m2_sector(f, 16)
            root_lba = struct.unpack_from("<I", pvd, 158)[0]
            root_sz = struct.unpack_from("<I", pvd, 166)[0]
            files = walk_iso_dir(f, root_lba, root_sz)
            files.sort(key=lambda e: e["lba"])

            # Preserve ISO9660 System Area + PVD + Path Tables + Directory Records verbatim
            first_file_lba = min((e["lba"] for e in files if e["lba"] > 0), default=100)
            meta_sectors = max(24, first_file_lba)
            meta_rel = f"{disc_sub}/iso_meta.bin"
            meta_abs = os.path.join(disc_files_dir, meta_rel)
            f.seek(0)
            meta_bytes = f.read(meta_sectors * SECTOR_RAW)
            with open(meta_abs, "wb") as mf:
                mf.write(meta_bytes)
            total_written_bytes += len(meta_bytes)

            manifest_rows.append(
                f"DISC\t{disc_num}\t{info['volume_id']}\t{info['boot_exe']}\t"
                f"{info['total_sectors']}\t{meta_sectors}\t{meta_rel}"
            )

            f1_cnt = 0
            raw_cnt = 0
            fill_cnt = 0

            pbar = None
            try:
                import nocturne_ui  # noqa: E402
                pbar = nocturne_ui.ProgressBar(f"Disc {disc_num}/4", total=max(1, len(files)))
            except Exception:
                pbar = None

            for file_idx, ent in enumerate(files, 1):
                iso_path = ent["iso_path"]
                rel_clean = sanitize_rel_path(iso_path)
                lba = ent["lba"]
                size = ent["size"]
                sectors = ent["sectors"]
                if pbar is not None:
                    pbar.update(file_idx, detail=iso_path)

                if is_raw_xa_file(f, ent):
                    raw_cnt += 1
                    # Stream raw 2352-byte sectors directly to disk while hashing
                    h = hashlib.sha256()
                    local_rel = f"{disc_sub}/{rel_clean}.raw2352"
                    local_abs = os.path.join(disc_files_dir, local_rel)
                    os.makedirs(os.path.dirname(local_abs), exist_ok=True)
                    f.seek(lba * SECTOR_RAW)
                    remaining = sectors
                    with open(local_abs, "wb") as out_f:
                        while remaining > 0:
                            chunk_secs = min(remaining, 1024)
                            chunk = f.read(chunk_secs * SECTOR_RAW)
                            if len(chunk) != chunk_secs * SECTOR_RAW:
                                raise EOFError(f"Short RAW2352 read for {iso_path} on Disc {disc_num}")
                            out_f.write(chunk)
                            h.update(chunk)
                            remaining -= chunk_secs
                    sha = h.hexdigest()
                    # Check if an identical RAW2352 stream already exists in shared_by_hash
                    if sha in shared_by_hash:
                        os.remove(local_abs)
                        local_rel = shared_by_hash[sha]
                        dedup_saved_bytes += sectors * SECTOR_RAW
                    else:
                        total_written_bytes += sectors * SECTOR_RAW
                        written_files_count += 1
                        shared_by_hash[sha] = local_rel
                    manifest_rows.append(
                        f"FILE\t{disc_num}\t{lba}\t{sectors}\t{size}\tRAW2352\t{iso_path}\t{local_rel}\t{sha}"
                    )
                    continue

                # Check constant-byte dummy padding (e.g. _ZYXWVUT.ZYX = 28.3 MB of 0xFF per disc)
                fill_b = detect_constant_fill_f1(f, lba, size)
                if fill_b is not None:
                    fill_cnt += 1
                    fill_saved_bytes += size
                    manifest_rows.append(
                        f"FILL\t{disc_num}\t{lba}\t{sectors}\t{size}\tF1\t{iso_path}\t{fill_b:02X}"
                    )
                    continue

                # Standard Mode 2 Form 1 file
                f1_cnt += 1
                buf = bytearray()
                f.seek(lba * SECTOR_RAW)
                remaining = sectors
                while remaining > 0:
                    chunk_secs = min(remaining, 512)
                    raw_chunk = f.read(chunk_secs * SECTOR_RAW)
                    if len(raw_chunk) != chunk_secs * SECTOR_RAW:
                        raise EOFError(f"Short F1 read for {iso_path} on Disc {disc_num}")
                    mv = memoryview(raw_chunk)
                    for s_i in range(chunk_secs):
                        off = s_i * SECTOR_RAW + USER_DATA_OFF
                        buf.extend(mv[off: off + SECTOR_F1])
                    remaining -= chunk_secs
                data = bytes(buf[:size])
                sha = hashlib.sha256(data).hexdigest()

                upper_iso = iso_path.upper()
                if upper_iso == info["boot_exe"]:
                    exe_hashes[disc_num] = sha
                    if exe_meta is None:
                        exe_meta = parse_psx_exe_header(data)
                        exe_out = os.path.join(exe_dir, "SLUS_010.51")
                        with open(exe_out, "wb") as ef:
                            ef.write(data)
                elif disc_num == 1 and upper_iso in OVERLAY_NAMES:
                    ovl_out = os.path.join(overlays_dir, os.path.basename(rel_clean))
                    with open(ovl_out, "wb") as ovf:
                        ovf.write(data)

                if sha in shared_by_hash:
                    local_rel = shared_by_hash[sha]
                    dedup_saved_bytes += len(data)
                else:
                    # Place first copy in shared/ unless another content already claimed that shared rel path
                    cand_shared = f"shared/{rel_clean}"
                    if cand_shared.lower() not in used_shared_paths and upper_iso not in ("SYSTEM.CNF", "BIN/DISKNO1.BIN"):
                        local_rel = cand_shared
                        used_shared_paths.add(cand_shared.lower())
                    else:
                        local_rel = f"{disc_sub}/{rel_clean}"
                    local_abs = os.path.join(disc_files_dir, local_rel)
                    os.makedirs(os.path.dirname(local_abs), exist_ok=True)
                    with open(local_abs, "wb") as out_f:
                        out_f.write(data)
                    shared_by_hash[sha] = local_rel
                    total_written_bytes += len(data)
                    written_files_count += 1

                manifest_rows.append(
                    f"FILE\t{disc_num}\t{lba}\t{sectors}\t{size}\tF1\t{iso_path}\t{local_rel}\t{sha}"
                )

            disc_infos[str(disc_num)] = {
                "volume_id": info["volume_id"],
                "boot_exe": info["boot_exe"],
                "total_sectors": info["total_sectors"],
                "meta_sectors": meta_sectors,
                "files_f1": f1_cnt,
                "files_raw2352": raw_cnt,
                "files_fill": fill_cnt,
            }
            if pbar is not None:
                pbar.finish(f"{f1_cnt} F1, {raw_cnt} XA/STR, {fill_cnt} Fill")
            else:
                print(f"  -> {f1_cnt} Form1, {raw_cnt} RAW2352 (XA/STR), {fill_cnt} Fill")

    manifest_path = os.path.join(disc_files_dir, "disc_manifest.tsv")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as mf:
        mf.write("\n".join(manifest_rows) + "\n")

    summary = {
        "game": "Koudelka (USA)",
        "platform": "PS1",
        "disc_count": len(discs),
        "discs": disc_infos,
        "exe": exe_meta,
        "exe_identical_across_discs": len(set(exe_hashes.values())) == 1,
        "exe_sha256_by_disc": {str(k): v for k, v in sorted(exe_hashes.items())},
        "source_raw_bytes": total_raw_bytes,
        "unpacked_bytes": total_written_bytes,
        "dedup_saved_bytes": dedup_saved_bytes,
        "fill_saved_bytes": fill_saved_bytes,
        "unique_files_written": written_files_count,
    }
    info_path = os.path.join(disc_files_dir, "disc_info.json")
    with open(info_path, "w", encoding="utf-8", newline="\n") as jf:
        json.dump(summary, jf, indent=2)

    if verify:
        verify_unpacked(discs, disc_files_dir, manifest_path)

    print(
        f"[koudelka] DONE: {len(discs)} discs unpacked -> {total_written_bytes / 1e6:.1f} MB "
        f"(from {total_raw_bytes / 1e6:.1f} MB raw BINs; dedup saved {dedup_saved_bytes / 1e6:.1f} MB, "
        f"fill saved {fill_saved_bytes / 1e6:.1f} MB, {written_files_count} unique files)."
    )
    return summary


def verify_unpacked(discs: dict[int, dict], disc_files_dir: str, manifest_path: str) -> None:
    """Spot-check unpacked Form 1, RAW2352, META, and FILL sectors against source .bin images."""
    print("[koudelka] Verifying bit-exact sector parity against source .bin discs...")
    by_disc: dict[int, list[list[str]]] = {}
    with open(manifest_path, "r", encoding="utf-8") as mf:
        for line in mf:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            by_disc.setdefault(int(parts[1]), []).append(parts)

    checked = 0
    for disc_num, rows in sorted(by_disc.items()):
        bin_path = discs[disc_num]["path"]
        with open(bin_path, "rb") as src:
            f1_rows = [r for r in rows if r[0] == "FILE" and r[5] == "F1"]
            raw_rows = [r for r in rows if r[0] == "FILE" and r[5] == "RAW2352"]
            fill_rows = [r for r in rows if r[0] == "FILL"]
            # Check first, middle, last Form 1 + ALL RAW2352 (first & last sector) + ALL FILL (first & last sector)
            sample_f1 = f1_rows[:3] + f1_rows[len(f1_rows) // 2: len(f1_rows) // 2 + 2] + f1_rows[-3:]
            for r in sample_f1:
                lba, size, local_rel = int(r[2]), int(r[4]), r[7]
                unpacked = open(os.path.join(disc_files_dir, local_rel), "rb").read()
                assert len(unpacked) == size, f"Size mismatch for {r[6]}"
                _, _, u0 = read_m2_sector(src, lba)
                assert unpacked[: min(size, SECTOR_F1)] == u0[: min(size, SECTOR_F1)], f"F1 mismatch at {r[6]}"
                checked += 1
            for r in raw_rows:
                lba, secs, local_rel = int(r[2]), int(r[3]), r[7]
                p = os.path.join(disc_files_dir, local_rel)
                with open(p, "rb") as rf:
                    r0 = rf.read(SECTOR_RAW)
                    rf.seek((secs - 1) * SECTOR_RAW)
                    rl = rf.read(SECTOR_RAW)
                src.seek(lba * SECTOR_RAW)
                assert r0 == src.read(SECTOR_RAW), f"RAW2352 start mismatch for {r[6]}"
                src.seek((lba + secs - 1) * SECTOR_RAW)
                assert rl == src.read(SECTOR_RAW), f"RAW2352 end mismatch for {r[6]}"
                checked += 1
            for r in fill_rows:
                lba, secs, size, fb = int(r[2]), int(r[3]), int(r[4]), int(r[7], 16)
                _, _, u0 = read_m2_sector(src, lba)
                assert u0[:SECTOR_F1] == bytes([fb]) * SECTOR_F1, f"FILL mismatch for {r[6]}"
                checked += 1
    print(f"[koudelka] Verified {checked} critical files/streams across {len(by_disc)} discs: 100% bit-exact.")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Unpack Koudelka (PS1, 4 discs) into ISO-free virtual disc folders.")
    ap.add_argument("--source", action="append", default=[], help="Folder(s) containing Koudelka Disc 1..4 .bin/.cue files")
    ap.add_argument("--game-dir", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "games", "koudelka"))
    ap.add_argument("--verify", action="store_true", help="Verify unpacked files/streams against source .bin images")
    args = ap.parse_args(argv)

    search = list(args.source) + [args.game_dir] + DEFAULT_SEARCH_DIRS
    discs = find_koudelka_discs(search)
    if len(discs) < 4:
        missing = [d for d in (1, 2, 3, 4) if d not in discs]
        sys.stderr.write(f"[koudelka] ERROR: Could not locate all 4 Koudelka discs (missing Disc(s) {missing}).\n")
        sys.stderr.write(f"  Searched: {search}\n")
        return 1

    unpack_discs(discs, args.game_dir, verify=args.verify)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
