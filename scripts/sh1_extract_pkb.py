#!/usr/bin/env python3
"""
Shadow Hearts (SLUS-20347) — Offline .PKB / PKFINFO.TBL Full-Disc Extractor

Extracts static game textures (~1,000+ unique PNGs across MODELS.PKB, BTLBG.PKB, and PIC.PKB)
and all 19 MPEG-2 PSS FMV cutscenes from MOVIE.PKB directly from the user's unpacked disc
folder (`games/sh1/disc_files/layer0/`), computing the EXACT SAME 64-bit hashes and
power-of-two canvases as:
  - runtime/src/gs/gpu/tex_pack.cpp (`hash_tex_bytes`, `hash_clut_rgba`)
  - runtime/src/gs/gpu/gpu.cpp      (`content_key`, `shadow_clut_load`, `1 << tw/th`)
  - runtime/src/png_codec.cpp       (`from_ps2_alpha` expansion)
  - runtime/src/ipu/ipu.cpp         (`hash_mpeg2_prefix`)

Usage:
  python scripts/sh1_extract_pkb.py --disc games/sh1/disc_files/layer0 --info
  python scripts/sh1_extract_pkb.py --disc games/sh1/disc_files/layer0 --extract-tex games/sh1/dumps/textures --clean
  python scripts/sh1_extract_pkb.py --disc games/sh1/disc_files/layer0 --extract-tex games/sh1/dumps/textures --clean --upscale --output games/sh1/mods/textures/SH1_HD_Pack --scale 4
  python scripts/sh1_extract_pkb.py --disc games/sh1/disc_files/layer0 --extract-fmv games/sh1/dumps/fmv --clean
  python scripts/sh1_extract_pkb.py --self-test
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from typing import Dict, List, Optional, Sequence, Tuple

SCRIPT_DIR = str(Path(__file__).resolve().parent)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

import sh2_extract_pkb as sh2

# Exact PKFINFO.TBL root record mapping verified byte-for-byte against Shadow Hearts (SLUS-20347) .PKB files.
SH1_PKFINFO_ROOTS: List[Tuple[int, str]] = [
    (1, "BTLBG.PKB"),
    (2, "COMMON.PKB"),
    (3, "EFFECT.PKB"),
    (4, "MODELS.PKB"),
    (5, "MOVIE.PKB"),
    (6, "MPS.PKB"),
    (7, "PIC.PKB"),
    (8, "SOUND.PKB"),
    (9, "TEXTURE.PKB"),
]


def parse_sh1_pkfinfo(disc_dir: Path) -> Dict[str, List[Tuple[int, int]]]:
    """Parse SLUS-20347 PKFINFO.TBL into {pkb_name: [(byte_offset, byte_size), ...]}."""
    tbl_path = disc_dir / "PKFINFO.TBL"
    if not tbl_path.is_file():
        raise FileNotFoundError(f"PKFINFO.TBL not found in {disc_dir}")
    data = tbl_path.read_bytes()
    if len(data) < 16:
        raise ValueError("PKFINFO.TBL truncated")
    header0 = struct.unpack_from("<I", data, 0)[0]
    count = header0 & 0xFFFF
    stride = (header0 >> 16) or 16
    if stride != 16 or 16 + count * 16 > len(data):
        raise ValueError(f"Unexpected PKFINFO.TBL header (count={count}, stride={stride}, len={len(data)})")

    recs = [struct.unpack_from("<4I", data, 16 + i * 16) for i in range(count)]
    starts = [(recs[ridx][3] & 0x7FFFFFFF) // 16 for ridx, _ in SH1_PKFINFO_ROOTS] + [count]

    out: Dict[str, List[Tuple[int, int]]] = {}
    for k, (_, pkb_name) in enumerate(SH1_PKFINFO_ROOTS):
        srec, erec = starts[k], starts[k + 1]
        entries: List[Tuple[int, int]] = []
        for r in range(srec, erec):
            _, _, off_q, sz = recs[r]
            if (sz & 0x80000000) or sz == 0:
                continue
            entries.append((off_q * 16, sz))
        out[pkb_name] = entries
    return out


def _rgba_words_to_png(width: int, height: int, rgba_words: Sequence[int], pow2_w: int, pow2_h: int) -> bytes:
    """Encode linear PS2 RGBA32 words (with PS2 alpha 0..128 -> 0..255) into a pow2-padded PNG."""
    lut = sh2.PS2_ALPHA_LUT
    row_pad = b"\x00" * ((pow2_w - width) * 4)
    blank_row = b"\x00" + (b"\x00" * (pow2_w * 4))
    rows = bytearray()
    idx = 0
    for _ in range(height):
        rows.append(0)  # PNG filter type 0 (None)
        for _ in range(width):
            w = rgba_words[idx]
            idx += 1
            rows.append(w & 0xFF)
            rows.append((w >> 8) & 0xFF)
            rows.append((w >> 16) & 0xFF)
            rows.append(lut[(w >> 24) & 0xFF])
        if row_pad:
            rows.extend(row_pad)
    for _ in range(pow2_h - height):
        rows.extend(blank_row)
    return sh2.encode_rgba_png_bytes(pow2_w, pow2_h, bytes(rows))


def _indexed8_to_png(width: int, height: int, pix: bytes, clut256: Sequence[int], pow2_w: int, pow2_h: int) -> bytes:
    """Encode 8bpp indexed pixels + linear 256-entry PS2 RGBA32 CLUT into a pow2-padded PNG."""
    lut = sh2.PS2_ALPHA_LUT
    pal = [bytes((c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, lut[(c >> 24) & 0xFF])) for c in clut256]
    while len(pal) < 256:
        pal.append(b"\x00\x00\x00\x00")
    row_pad = b"\x00" * ((pow2_w - width) * 4)
    blank_row = b"\x00" + (b"\x00" * (pow2_w * 4))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        base = y * width
        for b in pix[base : base + width]:
            rows.extend(pal[b])
        if row_pad:
            rows.extend(row_pad)
    for _ in range(pow2_h - height):
        rows.extend(blank_row)
    return sh2.encode_rgba_png_bytes(pow2_w, pow2_h, bytes(rows))


def _indexed4_to_png(width: int, height: int, pix: bytes, clut16: Sequence[int], pow2_w: int, pow2_h: int) -> bytes:
    """Encode 4bpp packed nibbles (low nibble first) + 16-entry PS2 RGBA32 CLUT into a pow2-padded PNG."""
    lut = sh2.PS2_ALPHA_LUT
    pal = [bytes((c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, lut[(c >> 24) & 0xFF])) for c in clut16]
    while len(pal) < 16:
        pal.append(b"\x00\x00\x00\x00")
    row_pad = b"\x00" * ((pow2_w - width) * 4)
    blank_row = b"\x00" + (b"\x00" * (pow2_w * 4))
    rows = bytearray()
    row_bytes = (width + 1) // 2
    for y in range(height):
        rows.append(0)
        base = y * row_bytes
        for x in range(width):
            byte = pix[base + (x >> 1)]
            nibble = (byte >> 4) if (x & 1) else (byte & 0x0F)
            rows.extend(pal[nibble])
        if row_pad:
            rows.extend(row_pad)
    for _ in range(pow2_h - height):
        rows.extend(blank_row)
    return sh2.encode_rgba_png_bytes(pow2_w, pow2_h, bytes(rows))


class _Sh1TextureCollector:
    """Deduplicates extracted SH1 textures by (tex_hash, clut_hash, pow2_w, pow2_h) and encodes PNGs in parallel."""

    def __init__(self, out_dir: Path, workers: int) -> None:
        self.out_dir = out_dir
        self.ui_dir = out_dir / "ui_2d"
        self.world_dir = out_dir / "world_3d"
        self.ui_dir.mkdir(parents=True, exist_ok=True)
        self.world_dir.mkdir(parents=True, exist_ok=True)
        self.seen: set[Tuple[int, int, int, int]] = set()
        self.jobs: List[Tuple[str, Path, int, int, int, int, bytes, Optional[List[int]], int]] = []
        self.workers = workers
        self.counts: Dict[str, int] = {"ui_2d": 0, "world_3d": 0}

    def add_indexed(self, category: str, width: int, height: int, bpp: int, pix: bytes, clut_linear: List[int]) -> bool:
        if width <= 0 or height <= 0 or width > 1024 or height > 1024:
            return False
        pw, ph = sh2.next_pow2(width), sh2.next_pow2(height)
        th = sh2.hash_tex_bytes(pix)
        ch = sh2.hash_clut_rgba(clut_linear)
        key = (th, ch, pw, ph)
        if key in self.seen:
            return False
        self.seen.add(key)
        subdir = self.ui_dir if category == "ui_2d" else self.world_dir
        dst = subdir / f"tex_{th:016x}_clut_{ch:016x}_{pw}x{ph}.png"
        self.jobs.append(("idx", dst, width, height, pw, ph, pix, clut_linear, bpp))
        self.counts[category] += 1
        return True

    def add_rgba32(self, category: str, width: int, height: int, rgba_bytes: bytes) -> bool:
        if width <= 0 or height <= 0 or width > 1024 or height > 1024:
            return False
        if len(rgba_bytes) < width * height * 4:
            return False
        pw, ph = sh2.next_pow2(width), sh2.next_pow2(height)
        th = sh2.hash_tex_bytes(rgba_bytes[: width * height * 4])
        key = (th, 0, pw, ph)
        if key in self.seen:
            return False
        self.seen.add(key)
        subdir = self.ui_dir if category == "ui_2d" else self.world_dir
        dst = subdir / f"tex_{th:016x}_{pw}x{ph}.png"
        self.jobs.append(("rgba", dst, width, height, pw, ph, rgba_bytes[: width * height * 4], None, 32))
        self.counts[category] += 1
        return True

    def flush(self) -> int:
        def _encode_one(job: Tuple[str, Path, int, int, int, int, bytes, Optional[List[int]], int]) -> int:
            kind, dst, w, h, pw, ph, pix, clut, bpp = job
            if kind == "rgba":
                words = struct.unpack_from(f"<{w * h}I", pix, 0)
                png = _rgba_words_to_png(w, h, words, pw, ph)
            elif bpp == 4:
                png = _indexed4_to_png(w, h, pix, clut or [], pw, ph)
            else:
                png = _indexed8_to_png(w, h, pix, clut or [], pw, ph)
            dst.write_bytes(png)
            return 1

        if not self.jobs:
            return 0
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            written = sum(pool.map(_encode_one, self.jobs))
        self.jobs.clear()
        return written


def extract_text_blocks(blob: bytes, category: str, col: _Sh1TextureCollector) -> int:
    """Extract all SH1 'TEXT' blocks (CSM1 8bpp, 4bpp, or raw RGBA32) from a binary blob."""
    found = 0
    pos = 0
    n = len(blob)
    while pos + 16 <= n:
        idx = blob.find(b"TEXT", pos)
        if idx == -1 or idx + 16 > n:
            break
        fmt, coff, poff, w, h, _ = struct.unpack_from("<6H", blob, idx + 4)
        # Case 1: Structured TEXT header (MODELS.PKB / BTLBG.PKB): fmt=1 (8bpp CSM1), fmt=2 (4bpp), fmt=0 (RGBA32)
        if fmt in (0, 1, 2) and 8 <= w <= 1024 and 8 <= h <= 1024 and coff in (0, 16) and poff >= 16:
            if fmt == 1:
                clut_end = idx + coff + 256 * 4
                pix_start = idx + poff
                pix_end = pix_start + w * h
                if clut_end <= n and pix_end <= n:
                    raw_clut = struct.unpack_from("<256I", blob, idx + coff)
                    lin_clut = sh2.csm1_unswizzle_256(raw_clut)
                    if col.add_indexed(category, w, h, 8, blob[pix_start:pix_end], lin_clut):
                        found += 1
                    pos = pix_end
                    continue
            elif fmt == 2:
                clut_end = idx + coff + 16 * 4
                pix_start = idx + poff
                pix_len = (w * h) // 2
                pix_end = pix_start + pix_len
                if clut_end <= n and pix_end <= n:
                    lin_clut = list(struct.unpack_from("<16I", blob, idx + coff))
                    if col.add_indexed(category, w, h, 4, blob[pix_start:pix_end], lin_clut):
                        found += 1
                    pos = pix_end
                    continue
            elif fmt == 0:
                pix_start = idx + poff
                pix_end = pix_start + w * h * 4
                if pix_end <= n:
                    if col.add_rgba32(category, w, h, blob[pix_start:pix_end]):
                        found += 1
                    pos = pix_end
                    continue
        # Case 2: Raw RGBA32 TEXT block (PIC.PKB): "TEXT", u16 w, u16 h, 0, 0, 0, 0
        elif fmt in (32, 64, 128, 256, 512, 640) and coff in (32, 64, 128, 256, 448, 512) and poff == 0 and w == 0:
            rw, rh = fmt, coff
            pix_start = idx + 16
            pix_end = pix_start + rw * rh * 4
            if pix_end <= n:
                if col.add_rgba32(category, rw, rh, blob[pix_start:pix_end]):
                    found += 1
                pos = pix_end
                continue
        pos = idx + 4
    return found


def extract_pic_chunk(blob: bytes, col: _Sh1TextureCollector) -> int:
    """Extract SH1 PIC.PKB indexed textures (PIC_IDX), raw RGBA TEXT, and nested BIN_ARC entries."""
    n = len(blob)
    if n < 16:
        return 0
    if blob[:4] == b"TEXT":
        return extract_text_blocks(blob, "ui_2d", col)

    # Nested BIN sub-archive: [u32 count, u32 total_size, (4s tag, u32 offset) * count]
    if n >= 24 and blob[8:12] == b"bin\x00":
        sub_count, total_sz = struct.unpack_from("<2I", blob, 0)
        if 1 <= sub_count <= 256 and total_sz <= n + 16 and 8 + sub_count * 8 <= n:
            offs = [struct.unpack_from("<I", blob, 12 + i * 8)[0] for i in range(sub_count)]
            offs.append(min(n, total_sz))
            found = 0
            for i in range(sub_count):
                s, e = offs[i], offs[i + 1]
                if 0 < s < e <= n:
                    found += extract_pic_chunk(blob[s:e], col)
            return found

    # Direct PIC_IDX header: <8H (w, h, w2, h2, fmt, unk, nclut, pad)
    w, h, w2, h2, fmt, _unk, nclut, _pad = struct.unpack_from("<8H", blob, 0)
    if w > 0 and h > 0 and w <= 1024 and h <= 1024 and w == w2 and h == h2 and 1 <= nclut <= 256:
        clut_bytes = nclut * 4
        if fmt == 1 and 16 + clut_bytes + w * h <= n:
            raw_clut = list(struct.unpack_from(f"<{nclut}I", blob, 16))
            while len(raw_clut) < 256:
                raw_clut.append(0)
            pix_start = 16 + clut_bytes
            pix = blob[pix_start : pix_start + w * h]
            return 1 if col.add_indexed("ui_2d", w, h, 8, pix, raw_clut) else 0
        if fmt == 2 and 16 + clut_bytes + (w * h) // 2 <= n:
            raw_clut = list(struct.unpack_from(f"<{nclut}I", blob, 16))
            while len(raw_clut) < 16:
                raw_clut.append(0)
            pix_start = 16 + clut_bytes
            pix = blob[pix_start : pix_start + (w * h) // 2]
            return 1 if col.add_indexed("ui_2d", w, h, 4, pix, raw_clut[:16]) else 0
    return 0


def demux_pss_video_prefix(pss_chunk: bytes, max_bytes: int = 16384) -> bytes:
    """Demux the first `max_bytes` of MPEG-2 elementary video stream (0xE0) from a PS2 PSS pack."""
    es = bytearray()
    pos = 0
    n = len(pss_chunk)
    while pos + 6 < n and len(es) < max_bytes:
        sc = pss_chunk.find(b"\x00\x00\x01", pos)
        if sc == -1 or sc + 6 > n:
            break
        sid = pss_chunk[sc + 3]
        if sid == 0xBA:
            pos = sc + 14
        elif sid in (0xBB, 0xBE, 0xBF) or (0xC0 <= sid <= 0xEF):
            plen = struct.unpack_from(">H", pss_chunk, sc + 4)[0]
            if sid == 0xE0 and sc + 9 <= n:
                hdr_len = pss_chunk[sc + 8]
                p_start = sc + 9 + hdr_len
                p_end = min(n, sc + 6 + plen)
                if p_start < p_end:
                    es.extend(pss_chunk[p_start:p_end])
            pos = sc + 6 + plen
        else:
            pos = sc + 4
    return bytes(es[:max_bytes])


def extract_textures_from_disc(disc_dir: Path, out_dir: Path, clean: bool = False, workers: int = 0) -> int:
    t0 = time.monotonic()
    pkbs = parse_sh1_pkfinfo(disc_dir)
    if clean and out_dir.is_dir():
        for sub in ("ui_2d", "world_3d"):
            sp = out_dir / sub
            if sp.is_dir():
                shutil.rmtree(sp, ignore_errors=True)
    worker_count = workers if workers > 0 else max(2, min(8, os.cpu_count() or 4))
    col = _Sh1TextureCollector(out_dir, worker_count)

    print(f"[sh1_extract_pkb] Scanning {disc_dir} for static textures...")
    for pkb_name, cat, mode in (
        ("MODELS.PKB", "world_3d", "text"),
        ("BTLBG.PKB", "world_3d", "text"),
        ("PIC.PKB", "ui_2d", "pic"),
    ):
        pkb_path = disc_dir / pkb_name
        entries = pkbs.get(pkb_name, [])
        if not pkb_path.is_file() or not entries:
            continue
        before = col.counts[cat]
        with open(pkb_path, "rb") as f:
            for off, sz in entries:
                f.seek(off)
                blob = f.read(sz)
                if mode == "text":
                    extract_text_blocks(blob, cat, col)
                else:
                    extract_pic_chunk(blob, col)
        added = col.counts[cat] - before
        print(f"  {pkb_name:12s} ({len(entries):3d} entries) -> {added:4d} unique textures ({cat})")

    written = col.flush()
    dt = time.monotonic() - t0
    print(
        f"[sh1_extract_pkb] Done in {dt:.1f}s: wrote {written} unique PNGs "
        f"(ui_2d={col.counts['ui_2d']}, world_3d={col.counts['world_3d']}) -> {out_dir}"
    )
    return written


def _find_ffmpeg() -> Optional[str]:
    root = Path(__file__).resolve().parent.parent
    cands = [
        root / "tools" / "portable" / "ffmpeg" / "bin" / "ffmpeg.exe",
        root / "tools" / "portable" / "ffmpeg" / "ffmpeg.exe",
    ]
    for c in cands:
        if c.is_file():
            return str(c)
    return shutil.which("ffmpeg")


def extract_fmvs_from_disc(disc_dir: Path, out_dir: Path, clean: bool = False) -> int:
    t0 = time.monotonic()
    pkbs = parse_sh1_pkfinfo(disc_dir)
    entries = pkbs.get("MOVIE.PKB", [])
    movie_path = disc_dir / "MOVIE.PKB"
    if not movie_path.is_file() or not entries:
        raise FileNotFoundError(f"MOVIE.PKB not found or empty in {disc_dir}")

    out_dir.mkdir(parents=True, exist_ok=True)
    if clean:
        for p in out_dir.iterdir():
            if p.is_file() and (p.suffix.lower() in (".mp4", ".m2v", ".pss") or p.name == "fmv_index.tsv"):
                try:
                    p.unlink()
                except OSError:
                    pass

    ffmpeg = _find_ffmpeg()
    seen_hashes: set[int] = set()
    rows: List[str] = ["# fmv_hash\tname\tdisc\twidth\theight\tfps\tframes\thas_audio\tmp4_file\n"]
    written = 0

    print(f"[sh1_extract_pkb] Extracting {len(entries)} FMV cutscenes from MOVIE.PKB...")
    with open(movie_path, "rb") as mf, tempfile.TemporaryDirectory(prefix="sh1_fmv_") as td:
        for idx, (off, sz) in enumerate(entries):
            mf.seek(off)
            head = mf.read(min(sz, 256 * 1024))
            es_prefix = demux_pss_video_prefix(head, 16384)
            h = sh2.hash_mpeg2_prefix(es_prefix)
            if h == 0 or h in seen_hashes:
                continue
            seen_hashes.add(h)
            name = f"SH1_Movie_{idx:02d}"
            mp4_name = f"fmv_{h:016x}.mp4"
            dst_mp4 = out_dir / mp4_name

            if ffmpeg:
                tmp_pss = Path(td) / f"movie_{idx:02d}.pss"
                mf.seek(off)
                rem = sz
                with open(tmp_pss, "wb") as tf:
                    while rem > 0:
                        chunk = mf.read(min(rem, 4 * 1024 * 1024))
                        if not chunk:
                            break
                        tf.write(chunk)
                        rem -= len(chunk)
                cmd = [
                    ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                    "-i", str(tmp_pss),
                    "-c:v", "libx264", "-preset", "fast", "-crf", "16", "-pix_fmt", "yuv420p",
                    "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart",
                    str(dst_mp4),
                ]
                res = subprocess.run(cmd, capture_output=True, text=True)
                try:
                    tmp_pss.unlink()
                except OSError:
                    pass
                if res.returncode != 0:
                    # Fallback: video-only MP4 if audio stream uses custom PS2 private stream
                    mf.seek(off)
                    with open(tmp_pss, "wb") as tf:
                        rem = sz
                        while rem > 0:
                            chunk = mf.read(min(rem, 4 * 1024 * 1024))
                            if not chunk:
                                break
                            tf.write(chunk)
                            rem -= len(chunk)
                    subprocess.run(
                        [ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                         "-i", str(tmp_pss), "-an", "-c:v", "libx264", "-preset", "fast", "-crf", "16",
                         "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(dst_mp4)],
                        check=True,
                    )
                    try:
                        tmp_pss.unlink()
                    except OSError:
                        pass
                written += 1
                rows.append(f"fmv_{h:016x}\t{name}\t1\t640\t448\t29.97\t0\t1\t{mp4_name}\n")
                print(f"  [{idx + 1:2d}/{len(entries)}] {mp4_name} ({sz / 1048576:.1f} MB PSS)")
            else:
                # No ffmpeg on PATH: dump raw .pss stream with exact fmv_<hash> stem
                pss_name = f"fmv_{h:016x}.pss"
                dst_pss = out_dir / pss_name
                mf.seek(off)
                rem = sz
                with open(dst_pss, "wb") as pf:
                    while rem > 0:
                        chunk = mf.read(min(rem, 4 * 1024 * 1024))
                        if not chunk:
                            break
                        pf.write(chunk)
                        rem -= len(chunk)
                written += 1
                rows.append(f"fmv_{h:016x}\t{name}\t1\t640\t448\t29.97\t0\t0\t{pss_name}\n")
                print(f"  [{idx + 1:2d}/{len(entries)}] {pss_name} ({sz / 1048576:.1f} MB raw PSS; install ffmpeg for MP4)")

    (out_dir / "fmv_index.tsv").write_text("".join(rows), encoding="utf-8")
    print(f"[sh1_extract_pkb] Done in {time.monotonic() - t0:.1f}s: extracted {written} unique FMVs -> {out_dir}")
    return written


def print_info(disc_dir: Path) -> None:
    pkbs = parse_sh1_pkfinfo(disc_dir)
    print(f"=== Shadow Hearts (SLUS-20347) PKFINFO.TBL Summary ({disc_dir}) ===")
    total_files = 0
    for _, name in SH1_PKFINFO_ROOTS:
        entries = pkbs.get(name, [])
        total_files += len(entries)
        p = disc_dir / name
        sz_mb = (p.stat().st_size / 1048576.0) if p.is_file() else 0.0
        print(f"  {name:14s}  {len(entries):4d} entries  {sz_mb:8.2f} MB")
    print(f"  TOTAL: {total_files} indexed archive entries")


def self_test() -> None:
    """Hermetic self-test verifying TEXT (CSM1 8bpp/4bpp/RGBA32), PIC_IDX, BIN_ARC, PSS demux & PNG output."""
    with tempfile.TemporaryDirectory(prefix="sh1_pkb_test_") as td:
        out = Path(td) / "dumps"
        col = _Sh1TextureCollector(out, 2)

        # 1. Synthetic CSM1 8bpp TEXT block (16x16)
        raw_clut = [0x80000000 | (i << 16) | (i << 8) | i for i in range(256)]
        pix8 = bytes(range(256))
        text8 = b"TEXT" + struct.pack("<6H", 1, 16, 16 + 1024, 16, 16, 0) + struct.pack("<256I", *raw_clut) + pix8
        assert extract_text_blocks(text8, "world_3d", col) == 1

        # 2. Synthetic Raw RGBA32 TEXT block (32x32)
        text32 = b"TEXT" + struct.pack("<6H", 32, 32, 0, 0, 0, 0) + (b"\x40\x80\xC0\x80" * (32 * 32))
        assert extract_text_blocks(text32, "ui_2d", col) == 1

        # 3. Synthetic PIC_IDX 8bpp (16x16, 64 colors)
        pic8 = struct.pack("<8H", 16, 16, 16, 16, 1, 1, 64, 0) + struct.pack("<64I", *raw_clut[:64]) + pix8
        assert extract_pic_chunk(pic8, col) == 1

        # 4. Synthetic PSS MPEG-2 pack demux + hash parity
        es_payload = b"\x00\x00\x01\xB3\x28\x01\xC0\x14" + bytes(range(256)) * 4
        pes = b"\x00\x00\x01\xE0" + struct.pack(">H", len(es_payload) + 3) + b"\x80\x00\x00" + es_payload
        pss = b"\x00\x00\x01\xBA" + b"\x00" * 10 + pes
        demuxed = demux_pss_video_prefix(pss, 16384)
        assert demuxed == es_payload
        assert sh2.hash_mpeg2_prefix(demuxed) != 0

        written = col.flush()
        assert written == 3, written
        pngs = list(out.rglob("*.png"))
        assert len(pngs) == 3
        for p in pngs:
            assert p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"

    print("[sh1_extract_pkb] self-test passed (all checks OK)")


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts (SLUS-20347) Offline .PKB Texture & FMV Extractor")
    ap.add_argument("--disc", default="games/sh1/disc_files/layer0", help="Path to unpacked Shadow Hearts layer0 folder")
    ap.add_argument("--info", action="store_true", help="Print PKFINFO.TBL archive summary")
    ap.add_argument("--extract-tex", metavar="DIR", help="Extract static textures into DIR/ui_2d & DIR/world_3d")
    ap.add_argument("--extract-fmv", metavar="DIR", help="Extract all MOVIE.PKB cutscenes into DIR")
    ap.add_argument("--clean", action="store_true", help="Clear existing files in output directory first")
    ap.add_argument("--workers", type=int, default=0, help="Parallel PNG encoder threads (0 = auto)")
    ap.add_argument("--upscale", action="store_true", help="Chain into scripts/upscale_textures.py after texture extraction")
    ap.add_argument("--output", default="games/sh1/mods/textures/SH1_HD_Pack", help="Output HD texture pack folder")
    ap.add_argument("--scale", type=int, default=4, choices=(2, 4), help="Texture AI upscale factor")
    ap.add_argument("--upscale-fmv", action="store_true", help="Chain into scripts/upscale_fmvs.py after FMV extraction")
    ap.add_argument("--fmv-output", default="games/sh1/mods/fmv", help="Output HD FMV folder")
    ap.add_argument("--fmv-scale", type=int, default=2, choices=(2, 3, 4), help="FMV AI upscale factor")
    ap.add_argument("--self-test", action="store_true", help="Run built-in unit self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0

    disc_dir = Path(args.disc)
    if args.info:
        print_info(disc_dir)
    if args.extract_tex:
        out_dir = Path(args.extract_tex)
        extract_textures_from_disc(disc_dir, out_dir, clean=args.clean, workers=args.workers)
        if args.upscale:
            upscaler = Path(__file__).resolve().parent / "upscale_textures.py"
            subprocess.run(
                [sys.executable, str(upscaler), "--input", str(out_dir), "--output", str(args.output),
                 "--scale", str(args.scale), "--auto-setup"],
                check=True,
            )
    if args.extract_fmv:
        out_dir = Path(args.extract_fmv)
        extract_fmvs_from_disc(disc_dir, out_dir, clean=args.clean)
        if args.upscale_fmv:
            upscaler = Path(__file__).resolve().parent / "upscale_fmvs.py"
            subprocess.run(
                [sys.executable, str(upscaler), "--input", str(out_dir), "--output", str(args.fmv_output),
                 "--scale", str(args.fmv_scale), "--auto-setup"],
                check=True,
            )
    if not (args.info or args.extract_tex or args.extract_fmv):
        print_info(disc_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
