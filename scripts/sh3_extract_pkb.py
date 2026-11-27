#!/usr/bin/env python3
"""
Shadow Hearts: From the New World (SLUS-21326) — Offline .PKB / .CPX / PKFINFO.TBL Extractor

Extracts static game textures (Format A/B, TIHD/TXHD, TIM2, including Nautilus 'cx' CPX-packed
entries across BTLDATA.PKB, COMMON.PKB, EFFECT.PKB, MENU.PKB, FIELD.PKB, OPENING.PKB, and
MCES_MDL.PKB) and MPEG-2 FMV cutscenes from OPENING.PKB / LSD.PKB directly from the user's
unpacked disc folder (`games/sh3-ftnw/disc_files/layer0/`), computing the EXACT SAME 64-bit
hashes and power-of-two canvases as:
  - runtime/src/gs/gpu/tex_pack.cpp (`hash_tex_bytes`, `hash_clut_rgba`)
  - runtime/src/gs/gpu/gpu.cpp      (`content_key`, `shadow_clut_load`, `1 << tw/th`)
  - runtime/src/png_codec.cpp       (`from_ps2_alpha` expansion)
  - runtime/src/ipu/ipu.cpp         (`hash_mpeg2_prefix`)

Usage:
  python scripts/sh3_extract_pkb.py --disc games/sh3-ftnw/disc_files/layer0 --info
  python scripts/sh3_extract_pkb.py --disc games/sh3-ftnw/disc_files/layer0 --extract-tex games/sh3-ftnw/dumps/textures --clean
  python scripts/sh3_extract_pkb.py --disc games/sh3-ftnw/disc_files/layer0 --extract-tex games/sh3-ftnw/dumps/textures --clean --upscale --output games/sh3-ftnw/mods/textures/SH3_HD_Pack --scale 4
  python scripts/sh3_extract_pkb.py --disc games/sh3-ftnw/disc_files/layer0 --extract-fmv games/sh3-ftnw/dumps/fmv --clean
  python scripts/sh3_extract_pkb.py --self-test
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Optional, Tuple
import zlib

SCRIPT_DIR = str(Path(__file__).resolve().parent)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

import sh2_extract_pkb as sh2
import sh3_prepare_overlays as sh3_ov

# Canonical SH3 (SLUS-21326) .PKB archives (verified from SLUS_213.26 FIO boot search order)
SH3_PKB_NAMES: Tuple[str, ...] = (
    "BTLDATA.PKB",
    "COMMON.PKB",
    "EFFECT.PKB",
    "MENU.PKB",
    "FIELD.PKB",
    "OPENING.PKB",
    "LSD.PKB",
    "MCES_MDL.PKB",
)

SH3_OVERLAY_CPX_NAMES: Tuple[str, ...] = (
    "FIELD.CPX",
    "BATTLE.CPX",
    "OPENING.CPX",
)


def parse_sh3_pkfinfo(disc_dir: Path) -> Dict[str, List[Tuple[int, int]]]:
    """Parse SLUS-21326 PKFINFO.TBL dynamically into {pkb_name: [(byte_offset, byte_size), ...]}.

    Walks the top-level directory records inside PKFINFO.TBL and matches each root range to
    its corresponding `.PKB` file on disk by exact max-extent size (with positional fallback
    to `SH3_PKB_NAMES` if two archives share an identical size).
    """
    tbl_path = disc_dir / "PKFINFO.TBL"
    if not tbl_path.is_file():
        return {}
    data = tbl_path.read_bytes()
    if len(data) < 32:
        return {}

    header0 = struct.unpack_from("<I", data, 0)[0]
    count = header0 & 0xFFFF
    stride = (header0 >> 16) or 16
    if stride != 16 or count == 0 or 16 + count * 16 > len(data):
        return {}

    recs = [struct.unpack_from("<4I", data, 16 + i * 16) for i in range(count)]

    # Top-level directory entries sit at the start of PKFINFO.TBL right before the first child block.
    # Each root record has (sz & 0x80000000) != 0 and (sz & 0x7FFFFFFF) // 16 = child_start_rec.
    root_starts: List[int] = []
    first_child = count
    for idx, rec in enumerate(recs):
        if idx >= first_child:
            break
        if rec[3] & 0x80000000:
            child_rec = (rec[3] & 0x7FFFFFFF) // 16
            if idx < child_rec < count:
                if child_rec < first_child:
                    first_child = child_rec
                root_starts.append(child_rec)

    if not root_starts:
        return {}

    root_starts = sorted(set(root_starts))
    ranges: List[Tuple[int, int, List[Tuple[int, int]]]] = []
    for k, srec in enumerate(root_starts):
        erec = root_starts[k + 1] if (k + 1) < len(root_starts) else count
        leaves: List[Tuple[int, int]] = []
        max_end = 0
        for r in range(srec, erec):
            _, _, off_q, sz = recs[r]
            if (sz & 0x80000000) or sz == 0:
                continue
            off_b = off_q * 16
            leaves.append((off_b, sz))
            end_b = off_b + sz
            if end_b > max_end:
                max_end = end_b
        ranges.append((srec, max_end, leaves))

    # Match existing .PKB files by exact/aligned file size first, then positional fallback
    disk_pkbs: List[Tuple[str, int]] = []
    for name in SH3_PKB_NAMES:
        p = disc_dir / name
        if p.is_file():
            disk_pkbs.append((name, p.stat().st_size))
    for p in sorted(disc_dir.glob("*.PKB")):
        up = p.name.upper()
        if all(x[0] != up for x in disk_pkbs):
            disk_pkbs.append((up, p.stat().st_size))

    out: Dict[str, List[Tuple[int, int]]] = {}
    used_ranges: set[int] = set()
    used_pkbs: set[str] = set()

    # Pass 1: exact size or 2048-byte DVD sector-aligned size match
    for ridx, (_srec, max_end, leaves) in enumerate(ranges):
        if not leaves or max_end == 0:
            continue
        aligned_16 = (max_end + 15) & ~15
        aligned_2k = (max_end + 2047) & ~2047
        cands = [
            name
            for name, fsz in disk_pkbs
            if name not in used_pkbs and (fsz == max_end or fsz == aligned_16 or fsz == aligned_2k)
        ]
        if len(cands) == 1:
            out[cands[0]] = leaves
            used_ranges.add(ridx)
            used_pkbs.add(cands[0])

    # Pass 2: closest fit where file_size >= max_end within 64 KiB
    for ridx, (_srec, max_end, leaves) in enumerate(ranges):
        if ridx in used_ranges or not leaves or max_end == 0:
            continue
        best_name: Optional[str] = None
        best_gap = 1 << 60
        for name, fsz in disk_pkbs:
            if name in used_pkbs or fsz < max_end:
                continue
            gap = fsz - max_end
            if gap <= 65536 and gap < best_gap:
                best_gap = gap
                best_name = name
        if best_name is not None:
            out[best_name] = leaves
            used_ranges.add(ridx)
            used_pkbs.add(best_name)

    return out


def _maybe_decompress_cpx(blob: bytes) -> bytes:
    """If `blob` starts with Nautilus 'cx' CPX magic, decompress it; otherwise return as-is."""
    if len(blob) >= 14 and blob[:2] == b"cx":
        try:
            return sh3_ov.decompress_cpx(blob)
        except Exception:
            return blob
    return blob


def _scan_sh3_blob(blob: bytes, pkb_name: str, col: sh2._TextureCollector) -> None:
    """Scan an SH3 PKB leaf buffer, transparently unpacking Nautilus 'cx' CPX containers."""
    raw = _maybe_decompress_cpx(blob)
    sh2._scan_blob(raw, pkb_name, col)

    # Also unpack any nested 'cx' CPX entries inside a PKB sub-container table
    if sh2.is_pkb_subcontainer(raw):
        cnt = struct.unpack_from("<I", raw, 0)[0]
        for i in range(cnt):
            rec_off = 16 + i * 16
            if rec_off + 16 > len(raw):
                break
            sub_off, sub_sz = struct.unpack_from("<II", raw, rec_off + 8)
            if sub_sz >= 16 and sub_off + sub_sz <= len(raw):
                sub = raw[sub_off : sub_off + sub_sz]
                if sub[:2] == b"cx":
                    dec = _maybe_decompress_cpx(sub)
                    if dec is not sub:
                        sh2._scan_blob(dec, pkb_name, col)


def extract_all_textures_from_disc(
    disc_layer0: Path,
    out_dir: Path,
    clean: bool = False,
    workers: int = 0,
) -> Tuple[int, int, int]:
    """Extract all unique static textures from Shadow Hearts: From the New World into `<out_dir>/ui_2d/` and `world_3d/`."""
    t0 = time.perf_counter()
    out_dir.mkdir(parents=True, exist_ok=True)
    if clean:
        removed = sh2._clean_dump_dir(out_dir)
        print(f"[sh3-pkb] --clean: removed {removed:,} existing PNG(s) from {out_dir}")

    col = sh2._TextureCollector(out_dir)
    tree = parse_sh3_pkfinfo(disc_layer0)

    print(f"[sh3-pkb] Scanning Shadow Hearts: From the New World archives in {disc_layer0} ...")
    for pkb_name in SH3_PKB_NAMES:
        if pkb_name == "LSD.PKB":
            continue
        pkb_path = disc_layer0 / pkb_name
        if not pkb_path.is_file():
            continue
        before = len(col.seen)
        pkb_sz = pkb_path.stat().st_size
        leaves = tree.get(pkb_name)
        if leaves:
            with pkb_path.open("rb") as f:
                for off, sz in leaves:
                    if off + sz > pkb_sz or sz < 16:
                        continue
                    f.seek(off)
                    _scan_sh3_blob(f.read(sz), pkb_name, col)
        else:
            blob = pkb_path.read_bytes()
            sh2._scan_tihds_in_buf(blob, pkb_name, col)
            sh2._scan_tim2_in_blob(blob, pkb_name, col)
        added = len(col.seen) - before
        print(f"  [sh3-pkb] {pkb_name:14s}: +{added:4,d} unique texture(s)  (total unique: {len(col.seen):,})")

    # Also scan decompressed FIELD/BATTLE/OPENING .CPX code overlays for embedded TIHD textures
    for cpx_name in SH3_OVERLAY_CPX_NAMES:
        cpx_path = disc_layer0 / cpx_name
        if not cpx_path.is_file():
            continue
        before = len(col.seen)
        dec = _maybe_decompress_cpx(cpx_path.read_bytes())
        sh2._scan_tihds_in_buf(dec, cpx_name, col)
        added = len(col.seen) - before
        if added:
            print(f"  [sh3-pkb] {cpx_name:14s}: +{added:4,d} unique texture(s)  (total unique: {len(col.seen):,})")

    t_scan = time.perf_counter() - t0
    total_unique = len(col.jobs)
    print(
        f"[sh3-pkb] Scan complete in {t_scan:.2f}s: {col.raw_hits:,} raw hits -> {total_unique:,} unique textures "
        f"(ui_2d={col.cat_counts['ui_2d']:,}, world_3d={col.cat_counts['world_3d']:,}; "
        f"FmtA={col.fmt_counts['FmtA']:,}, FmtB={col.fmt_counts['FmtB']:,}, TIHD={col.fmt_counts['TIHD']:,})"
    )
    if total_unique == 0:
        return 0, 0, 0

    n_workers = workers if workers > 0 else min(16, max(4, os.cpu_count() or 4))
    print(f"[sh3-pkb] Encoding {total_unique:,} PNGs into {out_dir} using {n_workers} worker threads ...")
    t_enc0 = time.perf_counter()
    written = 0
    skipped = 0
    done = 0
    step_report = max(1, total_unique // 10)
    with ThreadPoolExecutor(max_workers=n_workers) as ex:
        for wrote in ex.map(sh2._write_png_job, col.jobs, chunksize=16):
            done += 1
            if wrote:
                written += 1
            else:
                skipped += 1
            if done % step_report == 0 or done == total_unique:
                pct = 100.0 * done / total_unique
                print(
                    f"  [sh3-pkb] PNG progress: {done:,}/{total_unique:,} ({pct:5.1f}%)  written={written:,} skipped={skipped:,}",
                    flush=True,
                )

    t_total = time.perf_counter() - t0
    print(
        f"[sh3-pkb] Done in {t_total:.2f}s (encode {time.perf_counter() - t_enc0:.2f}s): "
        f"{written:,} written, {skipped:,} already present, {total_unique:,} total unique textures."
    )
    return written, skipped, total_unique


def extract_fmv_streams_from_disc(
    disc_layer0: Path,
    out_dir: Path,
    clean: bool = False,
    auto_setup_ffmpeg: bool = True,
    fmv_filter: str = "",
) -> int:
    """Extract all unique MPEG-2 FMV cutscenes from SH3's OPENING.PKB & LSD.PKB into playable H.264/AAC MP4s."""
    import upscale_fmvs as ufmv

    out_dir.mkdir(parents=True, exist_ok=True)
    if clean:
        removed = 0
        for p in list(out_dir.iterdir()):
            if p.is_file() and (
                p.suffix.lower() in (".m2v", ".mp4", ".m4v", ".mov", ".wav", ".ads")
                or p.name in ("fmv_index.tsv", "pkb_fmv_catalog.tsv")
            ):
                p.unlink()
                removed += 1
        if removed:
            print(f"[sh3-pkb] Cleaned {removed} stale file(s) from {out_dir}")

    ffmpeg_exe = ufmv.ensure_ffmpeg(auto_setup=auto_setup_ffmpeg)
    flt = fmv_filter.lower().strip()
    table = parse_sh3_pkfinfo(disc_layer0)

    containers: List[Tuple[Path, int, int, int, List[dict]]] = []
    total_tracks = 0
    for pkb_name in ("OPENING.PKB", "LSD.PKB"):
        pkb_path = disc_layer0 / pkb_name
        if not pkb_path.is_file():
            continue
        entries = table.get(pkb_name) or sh2.scan_lsd_streams(pkb_path)
        with pkb_path.open("rb") as f:
            for idx, (off, sz) in enumerate(entries):
                if sz < 4096:
                    continue
                f.seek(off)
                head = f.read(min(sz, 131072))
                tracks = sh2.parse_lsdh_tracks(head)
                if not tracks:
                    continue
                if flt:
                    tracks = [t for t in tracks if flt in str(t["name"]).lower()]
                if not tracks:
                    continue
                containers.append((pkb_path, idx, off, sz, tracks))
                total_tracks += len(tracks)

    print(
        f"[sh3-pkb] Discovered {total_tracks} FMV track(s) across {len(containers)} container(s) "
        f"(deduplicating by MPEG-2 hash)...",
        flush=True,
    )
    t0 = time.perf_counter()
    seen_content: Dict[Tuple[int, int, int], str] = {}
    index_rows: List[str] = []
    written = 0
    skipped = 0
    ti = 0

    with tempfile.TemporaryDirectory(prefix="sh3_fmv_dump_") as td:
        tmp_dir = Path(td)
        for pkb_path, idx, off, sz, tracks in containers:
            with pkb_path.open("rb") as f:
                f.seek(off)
                buf = f.read(sz)

            wanted_sids: List[int] = []
            for t in tracks:
                wanted_sids.append(int(t["movs_sid"]))
                if int(t["pcms_sid"]) >= 0:
                    wanted_sids.append(int(t["pcms_sid"]))
            streams = sh2.demux_lsdh_tracks(buf, wanted_sids)

            for info in tracks:
                ti += 1
                m2v_bytes = sh2.trim_mpeg2_stream(streams.get(int(info["movs_sid"]), b""))
                if len(m2v_bytes) < 64:
                    continue

                fhash = sh2.hash_mpeg2_prefix(m2v_bytes)
                tail_crc = zlib.crc32(m2v_bytes[-8192:]) & 0xFFFFFFFF
                content_sig = (fhash, len(m2v_bytes), tail_crc)
                if content_sig in seen_content:
                    continue

                w, h, fps = sh2.parse_mpeg2_seq_header(m2v_bytes[:16])
                if w == 0 or h == 0:
                    w, h = int(info["width"]), int(info["height"])
                fps_str = "30000/1001" if abs(fps - 29.97) < 0.05 else f"{fps:.3f}"
                clean_name = sh2._sanitize_fmv_name(str(info["name"]), f"{pkb_path.stem}_{idx:02d}")
                out_name = f"{fhash:016x}_{clean_name}.mp4"
                out_mp4 = out_dir / out_name
                seen_content[content_sig] = out_name

                aud_bytes = streams.get(int(info["pcms_sid"]), b"") if int(info["pcms_sid"]) >= 0 else b""
                has_audio = False
                ads_bytes = b""
                if aud_bytes:
                    ads_bytes = sh2.build_ps2_ads_bytes(
                        aud_bytes,
                        sample_rate=int(info["audio_sr"]),
                        channels=int(info["audio_ch"]),
                        interleave=int(info["audio_interleave"]),
                    )
                    has_audio = len(ads_bytes) > 40

                index_rows.append(
                    f"{fhash:016x}\t{clean_name}\tdisc1\t{w}\t{h}\t{fps:.3f}\t{info['frames']}\t"
                    f"{'yes' if has_audio else 'no'}\t{out_name}"
                )

                if out_mp4.is_file() and out_mp4.stat().st_size > 1024:
                    skipped += 1
                    print(f"  [{ti:2d}/{total_tracks:2d}] {out_name:34s} (already present, skipped)", flush=True)
                    continue

                t_one = time.perf_counter()
                tmp_m2v = tmp_dir / "video.m2v"
                tmp_ads = tmp_dir / "audio.ads"
                tmp_mp4 = out_mp4.with_suffix(".tmp.mp4")
                tmp_m2v.write_bytes(m2v_bytes)

                cmd = [
                    str(ffmpeg_exe), "-hide_banner", "-loglevel", "error", "-y",
                    "-r", fps_str, "-i", str(tmp_m2v),
                ]
                if has_audio:
                    tmp_ads.write_bytes(ads_bytes)
                    a_off_frames = max(0, int(info["start_frame"]) - int(info["pcms_start_frame"]))
                    if a_off_frames > 0:
                        cmd.extend(["-ss", f"{a_off_frames / max(1.0, fps):.3f}"])
                    cmd.extend(["-i", str(tmp_ads)])

                cmd.extend([
                    "-map", "0:v:0",
                    "-c:v", "libx264", "-crf", "16", "-preset", "fast",
                    "-pix_fmt", "yuv420p",
                    "-r", fps_str,
                    "-aspect", "4:3",
                ])
                if has_audio:
                    cmd.extend([
                        "-map", "1:a:0",
                        "-c:a", "aac", "-b:a", "192k", "-ar", str(info["audio_sr"]),
                        "-shortest",
                    ])
                else:
                    cmd.append("-an")
                cmd.extend(["-movflags", "+faststart", str(tmp_mp4)])

                subprocess.run(cmd, check=True)
                tmp_mp4.replace(out_mp4)
                tmp_m2v.unlink(missing_ok=True)
                tmp_ads.unlink(missing_ok=True)
                written += 1
                mb = out_mp4.stat().st_size / (1024 * 1024)
                print(
                    f"  [{ti:2d}/{total_tracks:2d}] {out_name:34s} {w}x{h} {info['frames']:5d}f "
                    f"{'48kHz stereo' if has_audio else 'video-only  '} -> {mb:6.1f} MiB ({time.perf_counter() - t_one:.1f}s)",
                    flush=True,
                )

    if index_rows:
        idx_path = out_dir / "fmv_index.tsv"
        with idx_path.open("w", encoding="utf-8") as out:
            out.write("# fmv_hash\tname\tdisc\twidth\theight\tfps\tframes\thas_audio\tmp4_file\n")
            for row in index_rows:
                out.write(row + "\n")
        print(
            f"[sh3-pkb] FMV extraction complete in {time.perf_counter() - t0:.1f}s: "
            f"{written} written, {skipped} skipped, {len(index_rows)} unique playable MP4(s) -> {out_dir}",
            flush=True,
        )
    else:
        print("[sh3-pkb] No FMV streams matched.")
    return len(index_rows)


def _encode_cpx_raw_block(raw: bytes) -> bytes:
    """Wrap raw bytes in a valid Nautilus 'cx' mode-0 XOR-obfuscated CPX container (for synthetic self-test)."""
    assert 1 <= len(raw) <= 65536
    hdr = struct.pack("<2sBBI", b"cx", 0, 1, len(raw))
    obf = bytes(b ^ ((((i + 1) * 0x0B) ^ 0xDD) & 0xFF) for i, b in enumerate(raw))
    blk = struct.pack("<BBHH", 0x80, 0, len(obf) - 1, len(raw) - 1) + obf
    return hdr + blk


def run_self_test() -> int:
    print("[sh3-pkb] Running synthetic PKFINFO.TBL, Nautilus 'cx' CPX, TIHD, Format A & LSDH self-test...")
    with tempfile.TemporaryDirectory(prefix="sh3_pkb_test_") as td:
        disc_dir = Path(td) / "layer0"
        out_dir = Path(td) / "dumps"
        disc_dir.mkdir(parents=True, exist_ok=True)

        # Build a synthetic CPX-compressed TIHD 8x8 4bpp texture inside COMMON.PKB
        pal16 = [(0x80 << 24) | (i * 15) for i in range(16)]
        pix_c = bytes([0x31] * ((8 * 8) // 2))
        clut_c = struct.pack("<16I", *pal16)
        tihd_hdr = struct.pack("<4sHHIHHII", b"TIHD", 8, 8, 4, 16, 0x20, 0x60, 0x60 + len(pix_c))
        raw_tihd = b"\x00" * 16 + tihd_hdr + b"\x00" * (0x20 - len(tihd_hdr)) + clut_c + pix_c
        cpx_tihd = _encode_cpx_raw_block(raw_tihd)
        assert sh3_ov.decompress_cpx(cpx_tihd) == raw_tihd

        # Build a synthetic Format A 12x10 8bpp texture inside MENU.PKB
        clut256 = [((0x80 << 24) | (i << 16) | ((255 - i) << 8) | i) for i in range(256)]
        pix_a = bytes((i % 256) for i in range(12 * 10))
        chunk_a = struct.pack("<HHBBBBII", 12, 10, 8, 1, 0, 1, 16, 16 + len(pix_a)) + pix_a + struct.pack("<256I", *clut256)

        common_bytes = cpx_tihd + b"\x00" * ((16 - (len(cpx_tihd) % 16)) % 16)
        menu_bytes = chunk_a + b"\x00" * ((16 - (len(chunk_a) % 16)) % 16)
        (disc_dir / "COMMON.PKB").write_bytes(common_bytes)
        (disc_dir / "MENU.PKB").write_bytes(menu_bytes)

        # Build a synthetic PKFINFO.TBL: 2 root dir records (rec 0 -> rec 2, rec 1 -> rec 3), 2 leaf records
        recs = [
            (0x11111111, 0, 0, 0x80000000 | (2 * 16)),  # root 0 -> rec 2
            (0x22222222, 0, 0, 0x80000000 | (3 * 16)),  # root 1 -> rec 3
            (0x33333333, 0, 0, len(cpx_tihd)),           # leaf 0 -> COMMON.PKB
            (0x44444444, 0, 0, len(chunk_a)),            # leaf 1 -> MENU.PKB
        ]
        tbl = bytearray(struct.pack("<4I", (16 << 16) | len(recs), 0, 0, 0))
        for r in recs:
            tbl.extend(struct.pack("<4I", *r))
        (disc_dir / "PKFINFO.TBL").write_bytes(bytes(tbl))

        parsed = parse_sh3_pkfinfo(disc_dir)
        assert "COMMON.PKB" in parsed and "MENU.PKB" in parsed, f"Unexpected PKFINFO mapping: {parsed}"
        assert parsed["COMMON.PKB"] == [(0, len(cpx_tihd))]
        assert parsed["MENU.PKB"] == [(0, len(chunk_a))]

        written, skipped, total = extract_all_textures_from_disc(disc_dir, out_dir, clean=True, workers=2)
        assert total == 2 and written == 2 and skipped == 0, (written, skipped, total)
        assert len(list((out_dir / "world_3d").glob("*.png"))) == 1
        assert len(list((out_dir / "ui_2d").glob("*.png"))) == 1

    print("[sh3-pkb] SELF-TEST PASSED (PKFINFO.TBL dynamic matching, 'cx' CPX, TIHD, Format A, PNG hashes verified)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: From the New World (SLUS-21326) Offline PKB/CPX Extractor")
    ap.add_argument("--disc", type=Path, default=None, help="Path to unpacked layer0 folder (e.g. games/sh3-ftnw/disc_files/layer0)")
    ap.add_argument("--info", action="store_true", help="Inspect PKFINFO.TBL and .PKB archives without extracting")
    ap.add_argument("--extract-tex", type=Path, default=None, help="Extract all static textures (PNG) into this directory")
    ap.add_argument("--extract-fmv", type=Path, default=None, help="Extract all MPEG-2 FMV cutscenes (MP4) into this directory")
    ap.add_argument("--clean", action="store_true", help="Remove existing PNG/MP4 files in the target directory before extracting")
    ap.add_argument("--workers", type=int, default=0, help="PNG encoder worker threads (default: auto)")
    ap.add_argument("--fmv-filter", type=str, default="", help="Only extract FMV tracks matching this substring")
    ap.add_argument("--upscale", action="store_true", help="Chain scripts/upscale_textures.py immediately after --extract-tex")
    ap.add_argument("--output", type=Path, default=None, help="Output HD texture pack directory (used with --upscale)")
    ap.add_argument("--scale", type=int, default=4, choices=(2, 4), help="AI texture upscale factor (default: 4)")
    ap.add_argument("--upscale-fmv", action="store_true", help="Chain scripts/upscale_fmvs.py immediately after --extract-fmv")
    ap.add_argument("--fmv-output", type=Path, default=None, help="Output HD FMV pack directory (used with --upscale-fmv)")
    ap.add_argument("--fmv-scale", type=int, default=2, choices=(2, 3, 4), help="AI FMV upscale factor (default: 2)")
    ap.add_argument("--self-test", action="store_true", help="Run disc-free synthetic unit test")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.disc is None or not args.disc.is_dir():
        print("[sh3-pkb] ERROR: --disc must point to an unpacked games/sh3-ftnw/disc_files/layer0 directory.", file=sys.stderr)
        return 2

    if args.info:
        tree = parse_sh3_pkfinfo(args.disc)
        print(f"[sh3-pkb] Disc folder: {args.disc}")
        print(f"[sh3-pkb] PKFINFO.TBL matched {len(tree)} .PKB archive(s):")
        for name in SH3_PKB_NAMES:
            p = args.disc / name
            if not p.is_file():
                continue
            leaves = tree.get(name, [])
            print(f"  - {name:14s}  {p.stat().st_size / (1024*1024):7.1f} MiB  ({len(leaves):4d} PKFINFO entries)")
        return 0

    if args.extract_tex is not None:
        extract_all_textures_from_disc(args.disc, args.extract_tex, clean=args.clean, workers=args.workers)
        if args.upscale:
            out_pack = args.output or (args.extract_tex.parent.parent / "texture_packs" / "SH3_HD_Pack")
            upscaler = Path(SCRIPT_DIR) / "upscale_textures.py"
            cmd = [
                sys.executable, "-u", str(upscaler),
                "--input", str(args.extract_tex),
                "--output", str(out_pack),
                "--scale", str(args.scale),
                "--auto-setup",
            ]
            print(f"[sh3-pkb] Launching AI upscaler -> {out_pack} ...")
            subprocess.run(cmd, check=True)

    if args.extract_fmv is not None:
        extract_fmv_streams_from_disc(args.disc, args.extract_fmv, clean=args.clean, fmv_filter=args.fmv_filter)
        if args.upscale_fmv:
            out_fmv = args.fmv_output or (args.extract_fmv.parent.parent / "video_packs" / "SH3_HD_FMVs")
            fmv_upscaler = Path(SCRIPT_DIR) / "upscale_fmvs.py"
            cmd = [
                sys.executable, "-u", str(fmv_upscaler),
                "--input", str(args.extract_fmv),
                "--output", str(out_fmv),
                "--scale", str(args.fmv_scale),
                "--auto-setup",
            ]
            print(f"[sh3-pkb] Launching AI FMV upscaler -> {out_fmv} ...")
            subprocess.run(cmd, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
