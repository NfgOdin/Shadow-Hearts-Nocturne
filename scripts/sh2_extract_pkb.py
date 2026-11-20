#!/usr/bin/env python3
"""
Shadow Hearts: Covenant — Offline .PKB / PKFINFO.TBL / LSD.PKB Full-Disc Extractor

Extracts all static game textures (~5,650 unique PNGs across Disc 1 & Disc 2) and
catalogs MPEG-2 FMV streams directly from the user's own unpacked disc folders
(`games/sh2-covenant/disc1_files/layer0/` and optionally `disc2_files/layer0/`),
computing the EXACT SAME 64-bit hashes and power-of-two canvases as:
  - runtime/src/gs/gpu/tex_pack.cpp (`hash_tex_bytes`, `hash_clut_rgba`)
  - runtime/src/gs/gpu/gpu.cpp      (`content_key`, `shadow_clut_load`, `1 << tw/th`)
  - runtime/src/png_codec.cpp       (`from_ps2_alpha` expansion)
  - runtime/src/ipu/ipu.cpp         (`hash_mpeg2_prefix`)

Supported native Shadow Hearts: Covenant texture formats (zero external pip deps):
  1. Format A (`tex\\0` Type A & raw Format A): menus, portraits, world map, shop,
     save/load, Solomon's Key, opening slides, and 2D backgrounds.
  2. Format B (`tex\\0` Type B): battle HUD, Judgment Ring, multi-palette 4bpp sheets.
  3. Format C (`TIHD` standalone, inside `tex\\0`, and inside `TXHD` in `SSH1.01a`,
     `SBD `, `EFMG`, and `FIELD.BIN`): 3D characters, monsters, rooms, props, effects.
  4. Standard PS2 `TIM2` fallback.

Usage:
  python scripts/sh2_extract_pkb.py --disc games/sh2-covenant/disc1_files/layer0 --info
  python scripts/sh2_extract_pkb.py --disc games/sh2-covenant/disc1_files/layer0 --extract-tex dumps/textures --clean
  python scripts/sh2_extract_pkb.py --disc games/sh2-covenant/disc1_files/layer0 --extract-tex dumps/textures --clean --upscale --output mods/textures/SH2_HD_Pack --scale 4
  python scripts/sh2_extract_pkb.py --disc games/sh2-covenant/disc1_files/layer0 --extract-fmv dumps/fmv
  python scripts/sh2_extract_pkb.py --self-test
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
from typing import Dict, List, Optional, Sequence, Tuple
import zlib

SCRIPT_DIR = str(Path(__file__).resolve().parent)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

MASK64 = 0xFFFFFFFFFFFFFFFF

# Exact PKFINFO.TBL root record mapping verified byte-for-byte against all 11 .PKB file sizes.
PKFINFO_ROOTS: List[Tuple[int, str]] = [
    (4, "BTLDATA.PKB"),
    (5, "COMMON.PKB"),
    (6, "EFFECT.PKB"),
    (7, "MENU.PKB"),
    (8, "FIELD.PKB"),
    (9, "OPENING.PKB"),
    (10, "SAVELOAD.PKB"),
    (11, "WORLDMAP.PKB"),
    (12, "SHOP.PKB"),
    (13, "SOLOMON.PKB"),
    (14, "LSD.PKB"),
]

# Precomputed PS2 alpha (0..128 -> 0..255) matching runtime/src/png_codec.cpp:
#   scaled = (a * 255 + 64) // 128; min(255, scaled)
PS2_ALPHA_LUT: Tuple[int, ...] = tuple(min(255, (a * 255 + 64) // 128) for a in range(256))


# ---- Exact bit-for-bit Python ports of C++ runtime hash functions ----

def _mix64(a: int, b: int) -> int:
    z = (a ^ ((b + 0x9E3779B97F4A7C15 + ((a << 6) & MASK64) + (a >> 2)) & MASK64)) & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return (z ^ (z >> 31)) & MASK64


def hash_tex_bytes(data: bytes) -> int:
    """Bit-exact match for ps2::gs::gpu::hash_tex_bytes() in runtime/src/gs/gpu/tex_pack.cpp."""
    n = len(data)
    if n == 0:
        return 0
    h = (n * 0x9E3779B97F4A7C15) & MASK64
    step = max(8, (n // 256) & ~7)
    i = 0
    while i + 8 <= n:
        v = struct.unpack_from("<Q", data, i)[0]
        h = (h ^ ((v + 0x9E3779B97F4A7C15 + ((h << 6) & MASK64) + (h >> 2)) & MASK64)) & MASK64
        h = (h * 0xBF58476D1CE4E5B9) & MASK64
        i += step
    tail = min(n, 8)
    tail_bytes = data[n - tail : n] + b"\x00" * (8 - tail)
    v = struct.unpack("<Q", tail_bytes)[0]
    h = (h ^ ((v + 0x9E3779B97F4A7C15 + ((h << 6) & MASK64) + (h >> 2)) & MASK64)) & MASK64
    h = (h * 0xBF58476D1CE4E5B9) & MASK64
    return h if h != 0 else 2


def hash_clut_rgba(entries: Sequence[int]) -> int:
    """Bit-exact match for ps2::gs::gpu::hash_clut_rgba() in runtime/src/gs/gpu/tex_pack.cpp."""
    if not entries:
        return 0
    h = _mix64(0xC107CAFE, len(entries))
    for v in entries:
        h = _mix64(h, v & 0xFFFFFFFF)
    return h if h != 0 else 1


def hash_mpeg2_prefix(data: bytes) -> int:
    """Bit-exact match for ps2::ipu::hash_mpeg2_prefix() in runtime/src/ipu/ipu.cpp (16 KiB prefix)."""
    if not data:
        return 0
    n = min(len(data), 16384)
    h = ((n * 0x9E3779B97F4A7C15) ^ 0xF00DF00D12345678) & MASK64
    i = 0
    while i + 8 <= n:
        v = struct.unpack_from("<Q", data, i)[0]
        h = (h ^ ((v + 0x9E3779B97F4A7C15 + ((h << 6) & MASK64) + (h >> 2)) & MASK64)) & MASK64
        h = (h * 0xBF58476D1CE4E5B9) & MASK64
        i += 8
    if i < n:
        rem = data[i:n] + b"\x00" * (8 - (n - i))
        v = struct.unpack("<Q", rem)[0]
        h = (h ^ ((v + 0x9E3779B97F4A7C15 + ((h << 6) & MASK64) + (h >> 2)) & MASK64)) & MASK64
        h = (h * 0xBF58476D1CE4E5B9) & MASK64
    return h if h != 0 else 1


def csm1_unswizzle_256(words: Sequence[int]) -> List[int]:
    """Unswizzle a 256-entry PS2 CSM1 CLUT (swap [8..15] <-> [16..23] in each 32-entry block)."""
    out = list(words)
    if len(out) < 256:
        out.extend([0] * (256 - len(out)))
    for base in range(0, 256, 32):
        for i in range(8):
            out[base + 8 + i], out[base + 16 + i] = out[base + 16 + i], out[base + 8 + i]
    return out


def next_pow2(v: int) -> int:
    p = 1
    while p < v:
        p <<= 1
    return p


# ---- Fast Zero-Dependency Pure-Python RGBA PNG Encoder ----

def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag)
    crc = zlib.crc32(data, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def encode_rgba_png_bytes(width: int, height: int, raw_scanlines: bytes) -> bytes:
    """
    Encode pre-filtered PNG scanlines (each row prefixed with 1 filter byte, length = height * (1 + width * 4))
    into a valid 8-bit RGBA PNG byte buffer. Uses zlib level 1 for maximum throughput.
    """
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    compressed = zlib.compress(raw_scanlines, 1)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", compressed)
        + _png_chunk(b"IEND", b"")
    )


def _pal_word_to_rgba4(word: int) -> bytes:
    return bytes((
        word & 0xFF,
        (word >> 8) & 0xFF,
        (word >> 16) & 0xFF,
        PS2_ALPHA_LUT[(word >> 24) & 0xFF],
    ))


def _expand_16bit_5551(v16: int) -> bytes:
    r = (v16 & 0x1F) << 3
    g = ((v16 >> 5) & 0x1F) << 3
    b = ((v16 >> 10) & 0x1F) << 3
    a = 255 if (v16 & 0x8000) else 0
    return bytes((r, g, b, a))


def decode_to_pow2_png_bytes(
    w: int,
    h: int,
    bpp: int,
    pix_bytes: bytes,
    clut_words: Optional[Sequence[int]],
) -> Tuple[int, int, bytes]:
    """
    Decode raw linear PS2 pixel bytes + unswizzled RGBA32 CLUT into a power-of-two
    8-bit RGBA PNG (matching runtime GS 1<<tw x 1<<th layout and PS2 alpha expansion).
    """
    pw = next_pow2(w)
    ph = next_pow2(h)
    pad_right = b"\x00" * ((pw - w) * 4) if pw > w else b""
    rows: List[bytes] = []

    if bpp == 8 and clut_words is not None:
        pal_lut = tuple(_pal_word_to_rgba4(clut_words[i] if i < len(clut_words) else 0) for i in range(256))
        lut_get = pal_lut.__getitem__
        mv = memoryview(pix_bytes)
        for y in range(h):
            row = b"".join(map(lut_get, mv[y * w : (y + 1) * w]))
            rows.append(b"\x00" + row + pad_right)
    elif bpp == 4 and clut_words is not None:
        pal16 = tuple(_pal_word_to_rgba4(clut_words[i] if i < len(clut_words) else 0) for i in range(16))
        pair_lut = tuple(pal16[b & 0xF] + pal16[(b >> 4) & 0xF] for b in range(256))
        pair_get = pair_lut.__getitem__
        row_bytes = (w + 1) // 2
        mv = memoryview(pix_bytes)
        trim = w * 4
        for y in range(h):
            off = y * row_bytes
            row = b"".join(map(pair_get, mv[off : off + row_bytes]))
            if len(row) > trim:
                row = row[:trim]
            rows.append(b"\x00" + row + pad_right)
    elif bpp == 32:
        row_bytes = w * 4
        for y in range(h):
            off = y * row_bytes
            src = bytearray(pix_bytes[off : off + row_bytes])
            for i in range(3, len(src), 4):
                src[i] = PS2_ALPHA_LUT[src[i]]
            rows.append(b"\x00" + bytes(src) + pad_right)
    elif bpp == 24:
        row_bytes = w * 3
        for y in range(h):
            off = y * row_bytes
            src = pix_bytes[off : off + row_bytes]
            out = bytearray(w * 4)
            out[0::4] = src[0::3]
            out[1::4] = src[1::3]
            out[2::4] = src[2::3]
            out[3::4] = b"\xff" * w
            rows.append(b"\x00" + bytes(out) + pad_right)
    elif bpp == 16:
        row_bytes = w * 2
        for y in range(h):
            off = y * row_bytes
            vals = struct.unpack_from(f"<{w}H", pix_bytes, off)
            row = b"".join(_expand_16bit_5551(v) for v in vals)
            rows.append(b"\x00" + row + pad_right)
    else:
        raise ValueError(f"Unsupported bpp={bpp}")

    if ph > h:
        empty_row = b"\x00" + (b"\x00" * (pw * 4))
        rows.append(empty_row * (ph - h))

    return pw, ph, encode_rgba_png_bytes(pw, ph, b"".join(rows))


def _write_png_job(job: Tuple[Path, int, int, int, bytes, Optional[Tuple[int, ...]]]) -> bool:
    out_path, w, h, bpp, pix_bytes, clut_words = job
    if out_path.is_file():
        return False
    _, _, png_bytes = decode_to_pow2_png_bytes(w, h, bpp, pix_bytes, clut_words)
    out_path.write_bytes(png_bytes)
    return True


# ---- PKFINFO.TBL & SH2 Container Parsers ----

def parse_pkfinfo_table(tbl_bytes: bytes) -> Dict[str, List[Tuple[int, int]]]:
    """
    Parse Shadow Hearts: Covenant's PKFINFO.TBL into {pkb_filename: [(offset, size), ...]}.
    """
    total_recs = len(tbl_bytes) // 16
    if total_recs < 16:
        return {}

    def get_rec(idx: int) -> Tuple[int, int, int, int]:
        return struct.unpack_from("<4I", tbl_bytes, idx * 16)

    def collect_leaves(dir_start: int, subtree_end: int, base_off: int) -> List[Tuple[int, int]]:
        first_child = subtree_end
        dirs: List[Tuple[int, int]] = []
        leaves: List[Tuple[int, int]] = []
        idx = dir_start
        while idx < first_child and idx < total_recs:
            _, _, w2, w3 = get_rec(idx)
            if w3 & 0x80000000:
                cstart = dir_start + (w3 & 0x7FFFFFFF) // 16
                if cstart <= idx or cstart >= subtree_end:
                    idx += 1
                    continue
                if cstart < first_child:
                    first_child = cstart
                dirs.append((cstart, base_off + w2 * 16))
            else:
                leaves.append((base_off + w2 * 16, w3))
            idx += 1
        dirs.sort(key=lambda x: x[0])
        for di, (cstart, cbase) in enumerate(dirs):
            cend = dirs[di + 1][0] if di + 1 < len(dirs) else subtree_end
            leaves.extend(collect_leaves(cstart, cend, cbase))
        return leaves

    result: Dict[str, List[Tuple[int, int]]] = {}
    for i, (ridx, name) in enumerate(PKFINFO_ROOTS):
        if ridx >= total_recs:
            break
        _, _, _, w3 = get_rec(ridx)
        cstart = 1 + (w3 & 0x7FFFFFFF) // 16
        if i + 1 < len(PKFINFO_ROOTS) and PKFINFO_ROOTS[i + 1][0] < total_recs:
            _, _, _, nw3 = get_rec(PKFINFO_ROOTS[i + 1][0])
            cend = 1 + (nw3 & 0x7FFFFFFF) // 16
        else:
            cend = total_recs
        if 0 < cstart <= cend <= total_recs:
            result[name] = collect_leaves(cstart, cend, 0)
    return result


def is_pkb_subcontainer(buf: bytes) -> bool:
    """Detect Nautilus sub-containers: <count:u32, total_size:u32, (tag4, offset4) * count>."""
    if len(buf) < 16:
        return False
    cnt, totsz = struct.unpack_from("<2I", buf, 0)
    if not (1 <= cnt <= 2048 and 8 + cnt * 8 <= totsz <= len(buf) + 16):
        return False
    prev_off = 8 + cnt * 8
    for i in range(min(cnt, 4)):
        t = buf[8 + i * 8 : 12 + i * 8]
        off = struct.unpack_from("<I", buf, 12 + i * 8)[0]
        if not (32 <= t[0] < 127 and 32 <= t[1] < 127):
            return False
        if off < prev_off or off > totsz:
            return False
        prev_off = off
    return True


def _classify_category(fmt_name: str, pkb_name: str) -> str:
    """Route 2D UI/menus/HUD/portraits to ui_2d/ (anime model) and 3D assets to world_3d/."""
    if fmt_name == "TIHD":
        return "world_3d"
    # Format A / Format B are 2D menus, portraits, Judgment Ring, battle HUD, shop, world map, etc.
    if pkb_name.upper() in (
        "SAVELOAD.PKB", "SHOP.PKB", "WORLDMAP.PKB", "SOLOMON.PKB",
        "MENU.PKB", "COMMON.PKB", "OPENING.PKB", "BTLDATA.PKB",
    ):
        return "ui_2d"
    # FIELD.PKB Format A full-screen 2D backgrounds / pre-rendered art also benefit from ui_2d/
    return "ui_2d"


class _TextureCollector:
    """Collects unique decoded texture descriptors across all .PKB/.BIN files before parallel PNG write."""

    def __init__(self, out_dir: Path) -> None:
        self.out_dir = out_dir
        self.ui_dir = out_dir / "ui_2d"
        self.w3d_dir = out_dir / "world_3d"
        self.ui_dir.mkdir(parents=True, exist_ok=True)
        self.w3d_dir.mkdir(parents=True, exist_ok=True)
        # Deduplicate by (tex_hash, clut_hash, pow2_w, pow2_h)
        self.seen: Dict[Tuple[int, int, int, int], str] = {}
        self.jobs: List[Tuple[Path, int, int, int, bytes, Optional[Tuple[int, ...]]]] = []
        self.raw_hits = 0
        self.fmt_counts: Dict[str, int] = {"FmtA": 0, "FmtB": 0, "TIHD": 0, "TIM2": 0}
        self.cat_counts: Dict[str, int] = {"ui_2d": 0, "world_3d": 0}

    def add(
        self,
        fmt_name: str,
        pkb_name: str,
        w: int,
        h: int,
        bpp: int,
        pix_bytes: bytes,
        clut_groups_raw: Sequence[Sequence[int]],
        csm1_8bpp: bool = True,
    ) -> None:
        if w <= 0 or h <= 0 or w > 2048 or h > 2048 or not pix_bytes:
            return
        self.raw_hits += 1
        self.fmt_counts[fmt_name] = self.fmt_counts.get(fmt_name, 0) + 1
        th = hash_tex_bytes(pix_bytes)
        pw = next_pow2(w)
        ph = next_pow2(h)
        cat = _classify_category(fmt_name, pkb_name)
        sub_dir = self.ui_dir if cat == "ui_2d" else self.w3d_dir

        if not clut_groups_raw:
            # Indexed 4bpp/8bpp textures always require an active CLUT at draw time; skip CLUT-less stubs.
            if bpp in (4, 8):
                return
            key = (th, 0, pw, ph)
            if key not in self.seen:
                self.seen[key] = cat
                self.cat_counts[cat] += 1
                out_path = sub_dir / f"{th:016x}_0000000000000000_{pw}x{ph}.png"
                self.jobs.append((out_path, w, h, bpp, bytes(pix_bytes), None))
            return

        for words in clut_groups_raw:
            if bpp == 8 and csm1_8bpp:
                unswiz = tuple(csm1_unswizzle_256(words))
                ch = hash_clut_rgba(unswiz)
            else:
                unswiz = tuple(words)
                ch = hash_clut_rgba(unswiz)
            key = (th, ch, pw, ph)
            if key not in self.seen:
                self.seen[key] = cat
                self.cat_counts[cat] += 1
                out_path = sub_dir / f"{th:016x}_{ch:016x}_{pw}x{ph}.png"
                self.jobs.append((out_path, w, h, bpp, bytes(pix_bytes), unswiz))


def _parse_tihd_at(buf: bytes, pos: int, pkb_name: str, col: _TextureCollector) -> int:
    if pos + 24 > len(buf) or buf[pos : pos + 4] != b"TIHD":
        return 0
    w, h, bpp_u32, clut_cnt, clut_off, pix_off, tot_sz = struct.unpack_from("<HHIHHII", buf, pos + 4)
    bpp = bpp_u32 & 0xFF
    if bpp not in (4, 8, 16, 24, 32) or not (1 <= w <= 2048 and 1 <= h <= 2048):
        return 0
    pix_sz = (w * h * bpp) // 8
    if pix_sz == 0 or pos + pix_off + pix_sz > len(buf):
        return 0
    pix = buf[pos + pix_off : pos + pix_off + pix_sz]
    cluts: List[Tuple[int, ...]] = []
    if bpp == 8 and clut_cnt > 0:
        n_pal = max(1, clut_cnt // 256)
        for pi in range(n_pal):
            c_start = pos + clut_off + pi * 1024
            avail = min(256, max(0, (len(buf) - c_start) // 4))
            if clut_cnt < 256:
                avail = min(avail, clut_cnt)
            if avail > 0:
                words = list(struct.unpack_from(f"<{avail}I", buf, c_start))
                if len(words) < 256:
                    words.extend([0] * (256 - len(words)))
                cluts.append(tuple(words))
    elif bpp == 4 and clut_cnt >= 16:
        n_pal = clut_cnt // 16
        for pi in range(n_pal):
            c_start = pos + clut_off + pi * 64
            if c_start + 64 <= len(buf):
                cluts.append(struct.unpack_from("<16I", buf, c_start))
    col.add("TIHD", pkb_name, w, h, bpp, pix, cluts, csm1_8bpp=True)
    return tot_sz if tot_sz >= 24 else 24


def _scan_tihds_in_buf(buf: bytes, pkb_name: str, col: _TextureCollector) -> None:
    idx = 0
    while True:
        p = buf.find(b"TIHD", idx)
        if p < 0:
            break
        adv = _parse_tihd_at(buf, p, pkb_name, col)
        idx = p + (adv if adv >= 24 else 4)


def _parse_tex_chunk(chunk: bytes, pkb_name: str, col: _TextureCollector) -> bool:
    if len(chunk) < 16:
        return False
    if chunk[:4] == b"TIHD":
        _scan_tihds_in_buf(chunk, pkb_name, col)
        return True
    # Format A: <HHBBBBII (w, h, bpp, f1, f2, f3, pix_off, clut_off)
    w, h, bpp, f1, f2, f3, po, co = struct.unpack_from("<HHBBBBII", chunk, 0)
    if bpp in (4, 8, 16, 24, 32) and 1 <= w <= 2048 and 1 <= h <= 2048 and 16 <= po < len(chunk):
        pix_sz = (w * h * bpp) // 8
        if pix_sz > 0 and po + pix_sz <= len(chunk):
            pix = chunk[po : po + pix_sz]
            cluts: List[Tuple[int, ...]] = []
            if bpp == 8 and co > 0 and co + 1024 <= len(chunk):
                cluts.append(struct.unpack_from("<256I", chunk, co))
            elif bpp == 4 and co > 0 and co + 64 <= len(chunk):
                rem = len(chunk) - co
                n_pal = max(1, min(16, rem // 64)) if (rem <= 1024 and rem % 64 == 0 and f2 == 0) else 1
                for pi in range(n_pal):
                    cluts.append(struct.unpack_from("<16I", chunk, co + pi * 64))
            col.add("FmtA", pkb_name, w, h, bpp, pix, cluts, csm1_8bpp=True)
            return True
    # Format B: <IHHHHI (0, w, h, fmt, clut_colors, 0)
    z0, bw, bh, bfmt, bclut, z1 = struct.unpack_from("<IHHHHI", chunk, 0)
    if z0 == 0 and bfmt in (0, 1) and 1 <= bw <= 2048 and 1 <= bh <= 2048 and bclut <= 1024:
        bbpp = 4 if bfmt == 0 else 8
        clut_sz = bclut * 4
        pix_sz = (bw * bh * bbpp) // 8
        if pix_sz > 0 and 16 + clut_sz + pix_sz <= len(chunk):
            pix = chunk[16 + clut_sz : 16 + clut_sz + pix_sz]
            cluts = []
            if bbpp == 8 and bclut >= 256:
                for pi in range(bclut // 256):
                    cluts.append(struct.unpack_from("<256I", chunk, 16 + pi * 1024))
            elif bbpp == 4 and bclut >= 16:
                for pi in range(bclut // 16):
                    cluts.append(struct.unpack_from("<16I", chunk, 16 + pi * 64))
            col.add("FmtB", pkb_name, bw, bh, bbpp, pix, cluts, csm1_8bpp=True)
            return True
    return False


def _scan_blob(buf: bytes, pkb_name: str, col: _TextureCollector, depth: int = 0) -> None:
    if len(buf) < 16 or depth > 5:
        return
    if is_pkb_subcontainer(buf):
        cnt, totsz = struct.unpack_from("<2I", buf, 0)
        eff_tot = min(totsz, len(buf))
        for i in range(cnt):
            tag = buf[8 + i * 8 : 12 + i * 8]
            off = struct.unpack_from("<I", buf, 12 + i * 8)[0]
            nxt = struct.unpack_from("<I", buf, 12 + (i + 1) * 8)[0] if i + 1 < cnt else eff_tot
            if off >= eff_tot or nxt <= off:
                continue
            sub = buf[off:nxt]
            if tag == b"tex\x00":
                _parse_tex_chunk(sub, pkb_name, col)
            elif is_pkb_subcontainer(sub):
                _scan_blob(sub, pkb_name, col, depth + 1)
            else:
                _scan_tihds_in_buf(sub, pkb_name, col)
        return
    if buf[:4] == b"TIHD":
        _scan_tihds_in_buf(buf, pkb_name, col)
        return
    # Raw Format A leaf (e.g., MENU.PKB portraits, FIELD.PKB 512x448 / 640x448 backgrounds)
    w, h, bpp, f1, f2, f3, po, co = struct.unpack_from("<HHBBBBII", buf, 0)
    if bpp in (4, 8, 16, 24, 32) and 8 <= w <= 1024 and 8 <= h <= 1024 and po == 16:
        pix_sz = (w * h * bpp) // 8
        clut_sz = 1024 if bpp == 8 else (64 if bpp == 4 else 0)
        if po + pix_sz <= len(buf) and (co == 0 or (po + pix_sz <= co and co + clut_sz <= len(buf) + 16)):
            if _parse_tex_chunk(buf, pkb_name, col):
                return
    _scan_tihds_in_buf(buf, pkb_name, col)


def _scan_tim2_in_blob(blob: bytes, pkb_name: str, col: _TextureCollector) -> None:
    """Fallback scanner for standard PS2 TIM2 ('TIM2') textures if any exist."""
    pos = blob.find(b"TIM2")
    while pos >= 0 and pos + 64 <= len(blob):
        ver = blob[pos + 4]
        pics = struct.unpack_from("<H", blob, pos + 6)[0]
        if ver in (3, 4) and 1 <= pics <= 256:
            pic_off = pos + (128 if blob[pos + 5] == 1 else 16)
            for _ in range(pics):
                if pic_off + 48 > len(blob):
                    break
                total_sz, clut_sz, img_sz, hdr_sz, clut_colors, pict_fmt, mip_cnt, clut_type, img_type, w, h = (
                    struct.unpack_from("<IIIHHBBBBHH", blob, pic_off)
                )
                if total_sz == 0 or pic_off + total_sz > len(blob) or w == 0 or h == 0 or w > 1024 or h > 1024:
                    break
                img_data = blob[pic_off + hdr_sz : pic_off + hdr_sz + img_sz]
                clut_data = blob[pic_off + hdr_sz + img_sz : pic_off + hdr_sz + img_sz + clut_sz]
                if img_type == 3 and len(img_data) >= w * h * 4:
                    col.add("TIM2", pkb_name, w, h, 32, img_data[: w * h * 4], [], csm1_8bpp=False)
                elif img_type == 5 and len(img_data) >= w * h and len(clut_data) >= 256 * 4:
                    cwords = struct.unpack_from("<256I", clut_data, 0)
                    col.add("TIM2", pkb_name, w, h, 8, img_data[: w * h], [cwords], csm1_8bpp=False)
                pic_off += total_sz
        pos = blob.find(b"TIM2", pos + 4)


def _files_quick_equal(p1: Path, p2: Path) -> bool:
    """Fast check whether two disc .PKB files are identical (size + first/mid/last 64 KiB)."""
    if not p1.is_file() or not p2.is_file():
        return False
    s1, s2 = p1.stat().st_size, p2.stat().st_size
    if s1 != s2:
        return False
    if s1 == 0:
        return True
    sample = min(65536, s1)
    with p1.open("rb") as f1, p2.open("rb") as f2:
        for off in (0, max(0, s1 // 2 - sample // 2), max(0, s1 - sample)):
            f1.seek(off)
            f2.seek(off)
            if f1.read(sample) != f2.read(sample):
                return False
    return True


def _clean_dump_dir(out_dir: Path) -> int:
    removed = 0
    for sub in (out_dir, out_dir / "ui_2d", out_dir / "world_3d"):
        if not sub.is_dir():
            continue
        for p in sub.glob("*.png"):
            try:
                p.unlink()
                removed += 1
            except OSError:
                pass
    return removed


def extract_all_textures_from_disc(
    disc_layer0: Path,
    out_dir: Path,
    disc2_layer0: Optional[Path] = None,
    clean: bool = False,
    workers: int = 0,
) -> Tuple[int, int, int]:
    """
    Extract all unique static textures from Shadow Hearts: Covenant's unpacked disc(s)
    into `<out_dir>/ui_2d/` and `<out_dir>/world_3d/`.
    Returns (written_count, skipped_existing_count, total_unique_count).
    """
    t0 = time.perf_counter()
    out_dir.mkdir(parents=True, exist_ok=True)
    if clean:
        removed = _clean_dump_dir(out_dir)
        print(f"[pkb] --clean: removed {removed:,} existing PNG(s) from {out_dir}")

    col = _TextureCollector(out_dir)

    def scan_disc_dir(layer0: Path, label: str, ref_dir: Optional[Path] = None) -> None:
        tbl_path = layer0 / "PKFINFO.TBL"
        tree: Dict[str, List[Tuple[int, int]]] = {}
        if tbl_path.is_file():
            tree = parse_pkfinfo_table(tbl_path.read_bytes())

        for _, pkb_name in PKFINFO_ROOTS:
            if pkb_name.upper() == "LSD.PKB":
                continue
            pkb_path = layer0 / pkb_name
            if not pkb_path.is_file():
                continue
            if ref_dir is not None and _files_quick_equal(ref_dir / pkb_name, pkb_path):
                continue
            before_unique = len(col.seen)
            pkb_sz = pkb_path.stat().st_size
            leaves = tree.get(pkb_name)
            if leaves:
                with pkb_path.open("rb") as f:
                    for off, sz in leaves:
                        if off + sz > pkb_sz or sz < 16:
                            continue
                        f.seek(off)
                        _scan_blob(f.read(sz), pkb_name, col)
            else:
                blob = pkb_path.read_bytes()
                _scan_tihds_in_buf(blob, pkb_name, col)
                _scan_tim2_in_blob(blob, pkb_name, col)
            added = len(col.seen) - before_unique
            print(f"  [pkb] {label}/{pkb_name:14s}: +{added:4,d} unique texture(s)  (total unique: {len(col.seen):,})")

        # Also scan FIELD.BIN / BATTLE.BIN / OPENING.BIN for embedded TIHD textures (e.g., 10 in FIELD.BIN)
        for bin_name in ("FIELD.BIN", "BATTLE.BIN", "OPENING.BIN"):
            bin_path = layer0 / bin_name
            if not bin_path.is_file():
                continue
            if ref_dir is not None and _files_quick_equal(ref_dir / bin_name, bin_path):
                continue
            before_unique = len(col.seen)
            _scan_tihds_in_buf(bin_path.read_bytes(), bin_name, col)
            added = len(col.seen) - before_unique
            if added:
                print(f"  [pkb] {label}/{bin_name:14s}: +{added:4,d} unique texture(s)  (total unique: {len(col.seen):,})")

    print(f"[pkb] Scanning Disc 1 archives in {disc_layer0} ...")
    scan_disc_dir(disc_layer0, "disc1", None)
    if disc2_layer0 is not None and disc2_layer0.is_dir():
        print(f"[pkb] Checking Disc 2 archives in {disc2_layer0} (auto-skipping identical .PKBs) ...")
        scan_disc_dir(disc2_layer0, "disc2", disc_layer0)

    t_scan = time.perf_counter() - t0
    total_unique = len(col.jobs)
    print(
        f"[pkb] Scan complete in {t_scan:.2f}s: {col.raw_hits:,} raw hits -> {total_unique:,} unique textures "
        f"(ui_2d={col.cat_counts['ui_2d']:,}, world_3d={col.cat_counts['world_3d']:,}; "
        f"FmtA={col.fmt_counts['FmtA']:,}, FmtB={col.fmt_counts['FmtB']:,}, TIHD={col.fmt_counts['TIHD']:,})"
    )

    if total_unique == 0:
        return 0, 0, 0

    n_workers = workers if workers > 0 else min(16, max(4, os.cpu_count() or 4))
    print(f"[pkb] Encoding {total_unique:,} PNGs into {out_dir} using {n_workers} worker threads ...")
    t_enc0 = time.perf_counter()
    written = 0
    skipped = 0
    done = 0
    step_report = max(1, total_unique // 10)
    with ThreadPoolExecutor(max_workers=n_workers) as ex:
        for wrote in ex.map(_write_png_job, col.jobs, chunksize=16):
            done += 1
            if wrote:
                written += 1
            else:
                skipped += 1
            if done % step_report == 0 or done == total_unique:
                pct = 100.0 * done / total_unique
                print(f"  [pkb] PNG progress: {done:,}/{total_unique:,} ({pct:5.1f}%)  written={written:,} skipped={skipped:,}", flush=True)

    t_total = time.perf_counter() - t0
    print(
        f"[pkb] Done in {t_total:.2f}s (encode {time.perf_counter() - t_enc0:.2f}s): "
        f"{written:,} written, {skipped:,} already present, {total_unique:,} total unique textures."
    )
    return written, skipped, total_unique


# ---- MPEG-2 / LSD.PKB Stream Inspection ----

def parse_mpeg2_seq_header(seq: bytes) -> Tuple[int, int, float]:
    """Parse width, height, fps from bytes starting at 00 00 01 B3."""
    if len(seq) < 8 or seq[:4] != b"\x00\x00\x01\xb3":
        return 0, 0, 29.97
    w = (seq[4] << 4) | (seq[5] >> 4)
    h = ((seq[5] & 0x0F) << 8) | seq[6]
    fps_table = {
        1: 24000.0 / 1001.0,
        2: 24.0,
        3: 25.0,
        4: 30000.0 / 1001.0,
        5: 30.0,
        6: 50.0,
        7: 60000.0 / 1001.0,
        8: 60.0,
    }
    return w, h, fps_table.get(seq[7] & 0x0F, 30000.0 / 1001.0)


def scan_pkb_subcontainers(pkb_path: Path, max_scan_bytes: int = 8 * 1024 * 1024) -> dict:
    """Inspect a Shadow Hearts .PKB archive for Nautilus sub-archive headers, TIHD, TIM2, and LSDH streams."""
    size = pkb_path.stat().st_size
    info = {
        "name": pkb_path.name,
        "size": size,
        "magic": "",
        "tim2_hits": 0,
        "tihd_hits": 0,
        "tex_tags": 0,
        "lsdh_hits": 0,
    }
    with pkb_path.open("rb") as f:
        head = f.read(min(size, max_scan_bytes))
    if len(head) >= 4:
        if head[:4] == b"LSDH":
            info["magic"] = "LSDH (stream archive)"
        elif is_pkb_subcontainer(head):
            cnt = struct.unpack_from("<I", head, 0)[0]
            tag = head[8:12]
            info["magic"] = f"SH2-PKB (count={cnt}, first={tag.rstrip(b'\\x00').decode('ascii', 'ignore')!r})"
    info["tim2_hits"] = head.count(b"TIM2")
    info["tihd_hits"] = head.count(b"TIHD")
    info["tex_tags"] = head.count(b"tex\x00")
    info["lsdh_hits"] = head.count(b"LSDH")
    return info


def scan_lsd_streams(lsd_path: Path) -> List[Tuple[int, int]]:
    """Scan LSD.PKB on 2048-byte DVD sector boundaries for 'LSDH' stream headers -> list of (offset, size)."""
    offsets: List[int] = []
    file_size = lsd_path.stat().st_size
    sector_chunk = 4 * 1024 * 1024
    with lsd_path.open("rb") as f:
        pos = 0
        while pos < file_size:
            buf = f.read(sector_chunk)
            if not buf:
                break
            for off in range(0, len(buf) - 4, 2048):
                if buf[off : off + 4] == b"LSDH":
                    offsets.append(pos + off)
            pos += len(buf)
    streams: List[Tuple[int, int]] = []
    for i, off in enumerate(offsets):
        nxt = offsets[i + 1] if i + 1 < len(offsets) else file_size
        streams.append((off, nxt - off))
    return streams


def _sanitize_fmv_name(name: str, fallback: str) -> str:
    cleaned = "".join(ch if (ch.isalnum() or ch in ("_", "-")) else "_" for ch in name.strip())
    cleaned = cleaned.strip("_")
    return cleaned if cleaned else fallback


def build_ps2_ads_bytes(adpcm_bytes: bytes, sample_rate: int = 48000, channels: int = 2, interleave: int = 2048) -> bytes:
    """
    Wrap raw PS2 SPU2 VAG/ADPCM interleaved bytes with a 40-byte standard Sony PS2 `.ads`
    (`SShd` + `SSbd`) header so FFmpeg's native C `adpcm_psx` decoder reads it directly.
    """
    block_pair = interleave * max(1, channels)
    trimmed_len = (len(adpcm_bytes) // block_pair) * block_pair
    if trimmed_len <= 0:
        return b""
    sshd = struct.pack(
        "<4s7I",
        b"SShd",
        24,             # header size
        0x10,           # codec = PS2 ADPCM (AV_CODEC_ID_ADPCM_PSX)
        sample_rate,
        channels,
        interleave,
        0xFFFFFFFF,     # loop start (none)
        0xFFFFFFFF,     # loop end (none)
    )
    ssbd = struct.pack("<4sI", b"SSbd", trimmed_len)
    return sshd + ssbd + adpcm_bytes[:trimmed_len]


def parse_lsdh_tracks(head: bytes) -> List[dict]:
    """
    Walk the exact LSDH track descriptor table (starting at hdr_qw*16, n_tracks entries where
    each descriptor is `tqw * 16` bytes) and return one dict per MOVS video track found in this
    container (most containers have 1 MOVS track; hybrid cutscenes can have 2).
    The 0-based descriptor index is the exact `sid` used in CTRL data packets!
    """
    if len(head) < 0x60 or head[:4] != b"LSDH":
        return []
    hdr_qw, n_tracks = struct.unpack_from("<HH", head, 8)
    total_frames = struct.unpack_from("<I", head, 0x10)[0]
    pos = hdr_qw * 16
    if pos < 0x40 or pos >= len(head) or n_tracks == 0 or n_tracks > 2048:
        return []

    movs_tracks: List[dict] = []
    pcms_tracks: List[dict] = []

    for tid in range(n_tracks):
        if pos + 0x20 > len(head):
            break
        tag = head[pos : pos + 4]
        tqw = struct.unpack_from("<H", head, pos + 0x10)[0]
        step = max(1, tqw) * 16
        if pos + step > len(head):
            break
        start_f, end_f = struct.unpack_from("<II", head, pos + 8)
        if tag == b"MOVS" and step >= 0x40:
            w, h = struct.unpack_from("<HH", head, pos + 0x28)
            raw_name = head[pos + 0x38 : pos + 0x40].split(b"\x00", 1)[0].decode("ascii", "ignore")
            movs_tracks.append({
                "movs_sid": tid,
                "width": w,
                "height": h,
                "name": raw_name,
                "start_frame": start_f,
                "end_frame": end_f,
                "frames": (end_f - start_f) if end_f > start_f else total_frames,
                "total_frames": total_frames,
            })
        elif tag == b"PCMS" and step >= 0x30:
            _, _, ch, sr = struct.unpack_from("<HHHH", head, pos + 0x20)
            audio_ch = ch if ch in (1, 2) else 2
            audio_sr = sr if (8000 <= sr <= 96000) else 48000
            audio_ilv = 2048
            blk_pair = struct.unpack_from("<I", head, pos + 0x18)[0]
            if blk_pair >= 32 and (blk_pair % max(1, audio_ch)) == 0:
                ilv = blk_pair // max(1, audio_ch)
                if ilv in (512, 1024, 2048, 4096, 8192):
                    audio_ilv = ilv
            pcms_tracks.append({
                "pcms_sid": tid,
                "start_frame": start_f,
                "end_frame": end_f,
                "audio_ch": audio_ch,
                "audio_sr": audio_sr,
                "audio_interleave": audio_ilv,
            })
        pos += step

    if not movs_tracks:
        return []

    # Pair each MOVS track with its overlapping PCMS track (if present)
    for mt in movs_tracks:
        matched_pcms = None
        if pcms_tracks:
            # Prefer a PCMS track whose frame range overlaps this MOVS track
            for pt in pcms_tracks:
                if pt["start_frame"] <= mt["start_frame"] and (pt["end_frame"] == 0 or pt["end_frame"] >= mt["start_frame"]):
                    matched_pcms = pt
                    break
            if matched_pcms is None:
                matched_pcms = pcms_tracks[0]
        if matched_pcms is not None:
            mt["pcms_sid"] = matched_pcms["pcms_sid"]
            mt["pcms_start_frame"] = matched_pcms["start_frame"]
            mt["audio_ch"] = matched_pcms["audio_ch"]
            mt["audio_sr"] = matched_pcms["audio_sr"]
            mt["audio_interleave"] = matched_pcms["audio_interleave"]
        else:
            mt["pcms_sid"] = -1
            mt["pcms_start_frame"] = 0
            mt["audio_ch"] = 2
            mt["audio_sr"] = 48000
            mt["audio_interleave"] = 2048
    return movs_tracks


def demux_lsdh_tracks(buf: bytes, wanted_sids: Sequence[int]) -> Dict[int, bytes]:
    """
    Demux one or more track streams (`wanted_sids`) in a single linear pass over an LSDH container,
    properly skipping `cflag == 1` sync/index headers so zero chunks (like seq=1) are ever dropped!
    """
    ctrl0 = buf.find(b"CTRL")
    if ctrl0 < 0x0C:
        return {}
    wanted = set(s for s in wanted_sids if s >= 0)
    out: Dict[int, bytearray] = {s: bytearray() for s in wanted}
    n = len(buf)
    pos = ctrl0 - 0x0C

    while pos >= 0 and pos + 0x10 <= n:
        if buf[pos + 0x0C : pos + 0x10] != b"CTRL":
            nxt = buf.find(b"CTRL", pos + 4)
            if nxt < 0x0C:
                break
            pos = nxt - 0x0C
            continue
        _p0, _fr, cflag, _tag = struct.unpack_from("<3I4s", buf, pos)
        pos += 0x10
        if cflag == 1 and pos + 0x10 <= n:
            # Sync/index header: (w0, cnt_plus_1, unk, unk) + (cnt_plus_1 - 1) * 8 bytes rounded up to 16B
            _s0, cnt_plus1, _s2, _s3 = struct.unpack_from("<4I", buf, pos)
            n_items = max(0, cnt_plus1 - 1) if cnt_plus1 < 4096 else 0
            skip_bytes = ((n_items * 8 + 15) // 16) * 16
            pos += 0x10 + skip_bytes

        while pos + 0x10 <= n:
            if buf[pos + 0x0C : pos + 0x10] == b"CTRL":
                break
            sid, _seq, qw, _unk = struct.unpack_from("<4I", buf, pos)
            pos += 0x10
            if sid == 0:
                break
            if (sid >> 16) != 0:
                break
            sz = qw * 16
            if pos + sz > n:
                break
            if sid in wanted:
                out[sid].extend(buf[pos : pos + sz])
            pos += sz

    return {s: bytes(b) for s, b in out.items()}


def trim_mpeg2_stream(vid: bytes) -> bytes:
    """Trim clean MPEG-2 elementary video from sequence start (00 00 01 B3) through sequence end (00 00 01 B7)."""
    b3 = vid.find(b"\x00\x00\x01\xb3")
    if b3 < 0:
        return b""
    end_pos = len(vid)
    b7 = vid.rfind(b"\x00\x00\x01\xb7")
    if b7 > b3:
        end_pos = b7 + 4
    return vid[b3:end_pos]


def extract_fmv_streams_from_disc(
    disc_layer0: Path,
    out_dir: Path,
    disc2_layer0: Optional[Path] = None,
    clean: bool = False,
    auto_setup_ffmpeg: bool = True,
    fmv_filter: str = "",
) -> int:
    """
    Extract all unique FMV cutscenes across Disc 1 (OPENING.PKB + LSD.PKB) and Disc 2 (LSD.PKB)
    into double-click playable H.264/AAC `.mp4` files named `<16hexhash>_<name>.mp4` with exact
    runtime-matching 64-bit hashes and synced 48 kHz stereo audio.
    """
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
            print(f"[pkb] Cleaned {removed} stale file(s) from {out_dir}")

    ffmpeg_exe = ufmv.ensure_ffmpeg(auto_setup=auto_setup_ffmpeg)
    flt = fmv_filter.lower().strip()

    # Collect candidate LSDH containers containing >= 1 MOVS track across Disc 1 & Disc 2
    disc_list: List[Tuple[str, Path]] = [("disc1", disc_layer0)]
    if disc2_layer0 is not None and disc2_layer0.is_dir():
        disc_list.append(("disc2", disc2_layer0))

    containers: List[Tuple[str, Path, int, int, int, List[dict]]] = []
    total_tracks = 0
    for disc_label, layer0 in disc_list:
        tbl_path = layer0 / "PKFINFO.TBL"
        if not tbl_path.is_file():
            continue
        table = parse_pkfinfo_table(tbl_path.read_bytes())
        for pkb_name in ("OPENING.PKB", "LSD.PKB"):
            pkb_path = layer0 / pkb_name
            if not pkb_path.is_file():
                continue
            entries = table.get(pkb_name, [])
            with pkb_path.open("rb") as f:
                for idx, (off, sz) in enumerate(entries):
                    if sz < 4096:
                        continue
                    f.seek(off)
                    head = f.read(min(sz, 131072))
                    tracks = parse_lsdh_tracks(head)
                    if not tracks:
                        continue
                    if flt:
                        tracks = [t for t in tracks if flt in str(t["name"]).lower()]
                    if not tracks:
                        continue
                    containers.append((disc_label, pkb_path, idx, off, sz, tracks))
                    total_tracks += len(tracks)

    print(
        f"[pkb] Discovered {total_tracks} FMV track(s) across {len(containers)} Disc 1 & Disc 2 container(s) "
        f"(deduplicating by MPEG-2 hash)...",
        flush=True,
    )
    t0 = time.perf_counter()
    seen_content: Dict[Tuple[int, int, int], str] = {}
    index_rows: List[str] = []
    written = 0
    skipped = 0
    ti = 0

    with tempfile.TemporaryDirectory(prefix="sh2_fmv_dump_") as td:
        tmp_dir = Path(td)
        for disc_label, pkb_path, idx, off, sz, tracks in containers:
            with pkb_path.open("rb") as f:
                f.seek(off)
                buf = f.read(sz)

            wanted_sids: List[int] = []
            for t in tracks:
                wanted_sids.append(int(t["movs_sid"]))
                if int(t["pcms_sid"]) >= 0:
                    wanted_sids.append(int(t["pcms_sid"]))
            streams = demux_lsdh_tracks(buf, wanted_sids)

            for info in tracks:
                ti += 1
                m2v_bytes = trim_mpeg2_stream(streams.get(int(info["movs_sid"]), b""))
                if len(m2v_bytes) < 64:
                    continue

                fhash = hash_mpeg2_prefix(m2v_bytes)
                # Deduplicate only true byte-identical streams across discs (e.g. d1:DEMO == d2:DEMO)
                tail_crc = zlib.crc32(m2v_bytes[-8192:]) & 0xFFFFFFFF
                content_sig = (fhash, len(m2v_bytes), tail_crc)
                if content_sig in seen_content:
                    continue

                w, h, fps = parse_mpeg2_seq_header(m2v_bytes[:16])
                if w == 0 or h == 0:
                    w, h = int(info["width"]), int(info["height"])
                fps_str = "30000/1001" if abs(fps - 29.97) < 0.05 else f"{fps:.3f}"
                clean_name = _sanitize_fmv_name(str(info["name"]), f"{pkb_path.stem}_{idx:02d}")
                out_name = f"{fhash:016x}_{clean_name}.mp4"
                if out_name in seen_content.values():
                    out_name = f"{fhash:016x}_{clean_name}_{disc_label}.mp4"
                out_mp4 = out_dir / out_name
                seen_content[content_sig] = out_name

                aud_bytes = streams.get(int(info["pcms_sid"]), b"") if int(info["pcms_sid"]) >= 0 else b""
                has_audio = False
                ads_bytes = b""
                if aud_bytes:
                    ads_bytes = build_ps2_ads_bytes(
                        aud_bytes,
                        sample_rate=int(info["audio_sr"]),
                        channels=int(info["audio_ch"]),
                        interleave=int(info["audio_interleave"]),
                    )
                    has_audio = len(ads_bytes) > 40

                index_rows.append(
                    f"{fhash:016x}\t{clean_name}\t{disc_label}\t{w}\t{h}\t{fps:.3f}\t{info['frames']}\t"
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
                    # For hybrid cutscenes where MOVS starts mid-scene, offset audio to match start_frame
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
            f"[pkb] FMV extraction complete in {time.perf_counter() - t0:.1f}s: "
            f"{written} written, {skipped} skipped, {len(index_rows)} unique playable MP4(s) -> {out_dir}",
            flush=True,
        )
    else:
        print("[pkb] No FMV streams matched.")
    return len(index_rows)


# ---- Self-Test ----

def _decode_png_for_test(png_bytes: bytes) -> Tuple[int, int, bytes]:
    assert png_bytes[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = bytearray()
    w = h = 0
    while pos < len(png_bytes):
        ln, typ = struct.unpack_from(">I4s", png_bytes, pos)
        dat = png_bytes[pos + 8 : pos + 8 + ln]
        crc_got = struct.unpack_from(">I", png_bytes, pos + 8 + ln)[0]
        crc_exp = zlib.crc32(dat, zlib.crc32(typ)) & 0xFFFFFFFF
        assert crc_got == crc_exp, f"Bad PNG CRC in {typ!r}"
        if typ == b"IHDR":
            w, h = struct.unpack_from(">II", dat, 0)
        elif typ == b"IDAT":
            idat.extend(dat)
        pos += 12 + ln
    raw = zlib.decompress(bytes(idat))
    out = bytearray()
    stride = w * 4 + 1
    for y in range(h):
        assert raw[y * stride] == 0
        out.extend(raw[y * stride + 1 : (y + 1) * stride])
    return w, h, bytes(out)


def run_self_test() -> int:
    print("[pkb] Running hash parity, format decoder (FmtA/FmtB/TIHD), CSM1, alpha & PNG self-test...")
    sample = bytes((i * 37 + 11) & 0xFF for i in range(4096))
    h1 = hash_tex_bytes(sample)
    h2 = hash_tex_bytes(sample)
    assert h1 == h2 and h1 != 0
    assert hash_tex_bytes(sample[:-1] + bytes([sample[-1] ^ 1])) != h1

    c1 = hash_clut_rgba([0x80112233 + i for i in range(16)])
    c2 = hash_clut_rgba([0x80112233 + i + (1 if i == 5 else 0) for i in range(16)])
    assert c1 != c2 and c1 != 0 and c2 != 0

    # CSM1 round-trip involution check
    clut256 = [((0x80 << 24) | (i << 16) | ((255 - i) << 8) | i) for i in range(256)]
    unswiz = csm1_unswizzle_256(clut256)
    assert unswiz[8] == clut256[16] and unswiz[16] == clut256[8]
    assert csm1_unswizzle_256(unswiz) == clut256

    # PS2 alpha expansion parity with runtime/src/png_codec.cpp
    assert PS2_ALPHA_LUT[0] == 0
    assert PS2_ALPHA_LUT[0x40] == 128
    assert PS2_ALPHA_LUT[0x80] == 255
    assert PS2_ALPHA_LUT[0xFF] == 255

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        col = _TextureCollector(tmp)

        # 1) Synthetic Format A (non-pow2 12x10 8bpp + CSM1 CLUT -> pow2 16x16 PNG)
        pix_a = bytes((i % 256) for i in range(12 * 10))
        clut_a = struct.pack("<256I", *clut256)
        chunk_a = struct.pack("<HHBBBBII", 12, 10, 8, 1, 0, 1, 16, 16 + len(pix_a)) + pix_a + clut_a
        assert _parse_tex_chunk(chunk_a, "SAVELOAD.PKB", col)

        # 2) Synthetic Format B (4bpp 16x8 with 2 palettes = 32 colors -> 2 unique PNGs!)
        pal_b1 = [(0x80 << 24) | (i * 16) for i in range(16)]
        pal_b2 = [(0x40 << 24) | ((i * 16) << 8) for i in range(16)]
        pix_b = bytes(((i & 0xF) | (((i + 1) & 0xF) << 4)) for i in range((16 * 8) // 2))
        chunk_b = struct.pack("<IHHHHI", 0, 16, 8, 0, 32, 0) + struct.pack("<32I", *(pal_b1 + pal_b2)) + pix_b
        assert _parse_tex_chunk(chunk_b, "BTLDATA.PKB", col)

        # 3) Synthetic Format C (TIHD 8x8 4bpp inside SSH1/TXHD-like buffer -> world_3d)
        pix_c = bytes([0x21] * ((8 * 8) // 2))
        clut_c = struct.pack("<16I", *pal_b1)
        tihd_hdr = struct.pack("<4sHHIHHII", b"TIHD", 8, 8, 4, 16, 0x20, 0x60, 0x60 + len(pix_c))
        chunk_c = b"\x00" * 16 + tihd_hdr + b"\x00" * (0x20 - len(tihd_hdr)) + clut_c + pix_c
        _scan_tihds_in_buf(chunk_c, "FIELD.PKB", col)

        assert len(col.jobs) == 4, f"Expected 4 jobs (1 FmtA + 2 FmtB + 1 TIHD), got {len(col.jobs)}"
        assert col.cat_counts == {"ui_2d": 3, "world_3d": 1}, f"Unexpected routing: {col.cat_counts}"

        for job in col.jobs:
            assert _write_png_job(job)
            pw, ph, rgba = _decode_png_for_test(job[0].read_bytes())
            assert pw == next_pow2(job[1]) and ph == next_pow2(job[2])
            assert len(rgba) == pw * ph * 4

        # Verify Format A pixel (0,0) = unswiz[0], pixel (8,0) = unswiz[8] == clut256[16], padded (15,15) = (0,0,0,0)
        fa_w, fa_h, fa_rgba = _decode_png_for_test(col.jobs[0][0].read_bytes())
        assert (fa_w, fa_h) == (16, 16)
        assert fa_rgba[8 * 4 : 8 * 4 + 4] == _pal_word_to_rgba4(clut256[16])
        assert fa_rgba[(15 * 16 + 15) * 4 : (15 * 16 + 15) * 4 + 4] == b"\x00\x00\x00\x00"

    # Synthetic 512x448 @ 29.97 fps MPEG-2 sequence header (> 16 KiB) + LSDH/CTRL demux + PS2 ADS audio test
    seq = b"\x00\x00\x01\xb3\x20\x01\xc0\x24" + bytes(range(256)) * 66 + b"\x00\x00\x01\xb7"
    w, h, fps = parse_mpeg2_seq_header(seq)
    assert w == 512 and h == 448 and abs(fps - 29.97) < 0.02
    fh = hash_mpeg2_prefix(seq)
    assert fh != 0 and fh == hash_mpeg2_prefix(seq + b"\xaa\xbb\xcc")

    # Build a synthetic LSDH block with LSDC (0), LGDT (1), MOVS (2), PCMS (3) + cflag=1 sync block + CTRL sub-packets
    lsdh_hdr = bytearray(0x160)
    lsdh_hdr[0:4] = b"LSDH"
    struct.pack_into("<HH", lsdh_hdr, 8, 4, 4)  # hdr_qw=4 (0x40), n_tracks=4
    struct.pack_into("<I", lsdh_hdr, 0x10, 120)
    # Track 0: LSDC (0x20 bytes @ 0x40)
    lsdh_hdr[0x40:0x44] = b"LSDC"
    struct.pack_into("<H", lsdh_hdr, 0x40 + 0x10, 2)
    # Track 1: LGDT (0x40 bytes @ 0x60)
    lsdh_hdr[0x60:0x64] = b"LGDT"
    struct.pack_into("<H", lsdh_hdr, 0x60 + 0x10, 4)
    # Track 2: MOVS (0x40 bytes @ 0xA0)
    lsdh_hdr[0xA0:0xA4] = b"MOVS"
    struct.pack_into("<II", lsdh_hdr, 0xA0 + 8, 0, 120)
    struct.pack_into("<H", lsdh_hdr, 0xA0 + 0x10, 4)
    struct.pack_into("<HH", lsdh_hdr, 0xA0 + 0x28, 512, 448)
    lsdh_hdr[0xA0 + 0x38 : 0xA0 + 0x3C] = b"TEST"
    # Track 3: PCMS (0x40 bytes @ 0xE0)
    lsdh_hdr[0xE0:0xE4] = b"PCMS"
    struct.pack_into("<II", lsdh_hdr, 0xE0 + 8, 0, 120)
    struct.pack_into("<H", lsdh_hdr, 0xE0 + 0x10, 4)
    struct.pack_into("<I", lsdh_hdr, 0xE0 + 0x18, 4096)
    struct.pack_into("<HHHH", lsdh_hdr, 0xE0 + 0x20, 65, 16, 2, 48000)

    mid = len(seq) // 2
    vid_p1 = seq[:mid]
    vid_p2 = seq[mid:]
    vid_p1_pad = vid_p1 + b"\x00" * ((16 - (len(vid_p1) % 16)) % 16)
    vid_p2_pad = vid_p2 + b"\x00" * ((16 - (len(vid_p2) % 16)) % 16)
    # Make mid divisible by 16 so concatenation matches seq cleanly
    mid = (len(seq) // 32) * 16
    vid_p1 = seq[:mid]
    vid_p2 = seq[mid:] + b"\x00" * ((16 - ((len(seq) - mid) % 16)) % 16)
    aud_chunk = (b"\x0c\x02" + b"\x00" * 14) * 256  # 4096 bytes
    ctrl_pkt = (
        struct.pack("<3I4s", 0, 0, 0, b"CTRL")
        + struct.pack("<4I", 2, 0, len(vid_p1) // 16, 0)
        + vid_p1
        + struct.pack("<4I", 3, 0, len(aud_chunk) // 16, 0)
        + aud_chunk
        + struct.pack("<4I", 0, 0, 0, 0)
        # cflag=1 sync block (4 items = 32B) immediately followed by seq=1 video sub-packet!
        + struct.pack("<3I4s", 0, 0, 1, b"CTRL")
        + struct.pack("<4I", 0x30001, 5, 0, 1)
        + (b"\x00" * 32)
        + struct.pack("<4I", 2, 1, len(vid_p2) // 16, 0)
        + vid_p2
        + struct.pack("<4I", 0, 0, 0, 0)
    )
    syn_lsdh = bytes(lsdh_hdr) + ctrl_pkt
    tracks = parse_lsdh_tracks(syn_lsdh)
    assert len(tracks) == 1 and tracks[0]["name"] == "TEST" and tracks[0]["movs_sid"] == 2 and tracks[0]["pcms_sid"] == 3
    streams = demux_lsdh_tracks(syn_lsdh, [2, 3])
    dv = trim_mpeg2_stream(streams[2])
    da = streams[3]
    assert dv == seq and da == aud_chunk and hash_mpeg2_prefix(dv) == fh
    ads = build_ps2_ads_bytes(da, 48000, 2, 2048)
    assert ads[:4] == b"SShd" and ads[32:36] == b"SSbd" and len(ads) == 40 + 4096

    print(f"[pkb] Self-test PASSED (tex={h1:016x}, clut={c1:016x}, fmv={fh:016x}, formats=FmtA+FmtB+TIHD+PNG+LSDH+ADS).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Covenant — Offline .PKB / LSD.PKB Full-Disc Texture & FMV Extractor")
    ap.add_argument("--disc", default="games/sh2-covenant/disc1_files/layer0", help="Path to unpacked Disc 1 layer0 folder")
    ap.add_argument("--disc2", default="", help="Optional path to unpacked Disc 2 layer0 folder (auto-skips identical .PKBs)")
    ap.add_argument("--info", action="store_true", help="Print archive & stream summary")
    ap.add_argument("--extract-fmv", metavar="DIR", help="Extract all Disc 1 & 2 FMV cutscenes as playable MP4s into DIR")
    ap.add_argument("--fmv-filter", default="", help="Optional case-insensitive cutscene name filter (e.g. DEMO or Movie)")
    ap.add_argument("--upscale-fmv", action="store_true", help="Automatically chain into scripts/upscale_fmvs.py after FMV extraction")
    ap.add_argument("--fmv-output", default="mods/fmv", help="Output HD FMV folder (used with --upscale-fmv)")
    ap.add_argument("--fmv-scale", type=int, default=2, choices=(2, 3, 4), help="FMV AI upscale factor (used with --upscale-fmv)")
    ap.add_argument("--extract-tex", metavar="DIR", help="Extract all static disc textures (FmtA/FmtB/TIHD) into DIR/ui_2d & DIR/world_3d")
    ap.add_argument("--clean", action="store_true", help="Remove existing dumped files in target DIR before extracting")
    ap.add_argument("--workers", type=int, default=0, help="PNG encoding worker threads (0 = auto)")
    ap.add_argument("--upscale", action="store_true", help="Automatically chain into scripts/upscale_textures.py after texture extraction")
    ap.add_argument("--output", default="mods/textures/SH2_HD_Pack", help="Output HD texture pack folder (used with --upscale)")
    ap.add_argument("--scale", type=int, default=4, choices=(2, 4), help="Texture AI upscale factor (used with --upscale)")
    ap.add_argument("--self-test", action="store_true", help="Run built-in hash parity, format, PNG & LSDH/ADS self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    disc = Path(args.disc)
    if not disc.is_dir():
        print(f"[pkb] Unpacked disc folder not found: {disc}")
        return 1

    disc2: Optional[Path] = Path(args.disc2) if args.disc2 else None
    if disc2 is None:
        auto_d2 = disc.parent.parent / "disc2_files" / "layer0"
        if auto_d2.is_dir() and auto_d2.resolve() != disc.resolve():
            disc2 = auto_d2

    pkbs = sorted(disc.glob("*.PKB"))
    if args.info or (not args.extract_fmv and not args.extract_tex):
        tbl = disc / "PKFINFO.TBL"
        print(f"[pkb] Disc folder: {disc}")
        if tbl.is_file():
            print(f"[pkb] PKFINFO.TBL: {tbl.stat().st_size:,} bytes ({tbl.stat().st_size // 16:,} 16-byte records)")
        for pkb in pkbs:
            info = scan_pkb_subcontainers(pkb)
            print(
                f"  {info['name']:14s}  {info['size'] / (1024*1024):7.2f} MiB  "
                f"{info['magic'] or 'PKFINFO archive':34s}  tihd={info['tihd_hits']:5d}  tex_tags={info['tex_tags']:4d}"
            )
        lsd = disc / "LSD.PKB"
        if lsd.is_file():
            streams = scan_lsd_streams(lsd)
            print(f"[pkb] LSD.PKB sector scan: {len(streams)} LSDH stream container(s) detected.")

    if args.extract_fmv:
        out_fmv = Path(args.extract_fmv)
        n_fmvs = extract_fmv_streams_from_disc(
            disc,
            out_fmv,
            disc2_layer0=disc2,
            clean=args.clean,
            auto_setup_ffmpeg=True,
            fmv_filter=args.fmv_filter,
        )
        if args.upscale_fmv and n_fmvs > 0:
            upscale_fmv_script = Path(__file__).resolve().parent / "upscale_fmvs.py"
            cmd = [
                sys.executable,
                str(upscale_fmv_script),
                "--input", str(out_fmv),
                "--output", str(args.fmv_output),
                "--scale", str(args.fmv_scale),
                "--auto-setup",
            ]
            if args.fmv_filter:
                cmd.extend(["--filter", args.fmv_filter])
            print(f"\n[pkb] Launching Vulkan Real-ESRGAN FMV upscaler: {' '.join(cmd)}\n", flush=True)
            rc = subprocess.call(cmd)
            if rc != 0:
                return rc

    if args.extract_tex:
        out_tex = Path(args.extract_tex)
        _, _, total_unique = extract_all_textures_from_disc(
            disc, out_tex, disc2_layer0=disc2, clean=args.clean, workers=args.workers
        )
        if args.upscale and total_unique > 0:
            upscale_script = Path(__file__).resolve().parent / "upscale_textures.py"
            cmd = [
                sys.executable,
                str(upscale_script),
                "--input", str(out_tex),
                "--output", str(args.output),
                "--scale", str(args.scale),
                "--auto-setup",
            ]
            print(f"\n[pkb] Launching Vulkan Real-ESRGAN batch upscaler: {' '.join(cmd)}\n", flush=True)
            rc = subprocess.call(cmd)
            return rc

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
