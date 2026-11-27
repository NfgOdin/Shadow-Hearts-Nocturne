#!/usr/bin/env python3
"""Shadow Hearts: Covenant — Native Director's Cut Permanent Upgrader.

100% clean-room Nocturne-native upgrader that merges Japanese *Shadow Hearts II:
Director's Cut* (SLPS_732.14, Discs 1 & 2) content directly into an existing
unpacked US *Shadow Hearts: Covenant* virtual-disc installation
(`games/sh2-covenant/disc1_files` & `disc2_files`) WITHOUT copying or rewriting
the 4 GiB `.PKB` archives and WITHOUT requiring the original US ISOs again.

Key capabilities:
  - Smart folder scanner that auto-detects SLPS_732.14 Disc 1 (PVD '0') & Disc 2
    (PVD '1') in any ROMs folder (supports .iso and .chd via native chdunpack).
  - Preserves pristine US baseline metadata (`.us_orig`) so re-running the
    upgrader (e.g., switching Regular <-> UnDub) is always clean and idempotent.
  - Appends new/upgraded assets into sector-aligned `unindexed/dc_shared.bin`
    (NTFS-hardlinked between Disc 1 & Disc 2) and `unindexed/dc_disc.bin`,
    rebuilds `PKFINFO.TBL` cleanly with hierarchical directory insertion, and
    atomically updates `disc_meta.bin` + `disc_manifest.tsv`.
  - Clean-room dynamic MIPS alignment patch for `FIELD.BIN` + `SHOP/TAILOR.DAT`
    enabling the Director's Cut 11th costume ("Beautiful Fool").
  - Imports all 155 Director's Cut-exclusive files (Nahash, Sea of Woods, Deeper
    Neam Ruins, Dog Shrine, Ring Soul quiz maps, BGM/SE banks, `PMOVIE.MSD`,
    and the `SH3_SP.LSD` preview movie), swaps in the DC opening movie
    (`OPENING/DEMO.LSD`), and upgrades Disc 2 `LSD/N125.LSD` ("The Promise") to
    the DC pre-rendered MPEG-2 movie with grafted US English `SBTD` subtitles.
  - Optional `--edition undub`: clean-room lossless UnDub swapping all 202
    Japanese battle voice banks (`BTLDATA/SOUND/SE/*.SSE`), 150 field story
    voice banks (`FIELD/SOUND/SE/*.SSE`), and 161 48 kHz cutscene `PCMS` tracks
    (`LSD/*.LSD`) while keeping 100% of US English subtitles & video.
  - Incrementally rebuilds `games/sh2-covenant/game.dll` (compiling only the
    updated `FIELD.BIN` overlay) and writes `games/sh2-covenant/dc_upgrade.json`.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import difflib
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
SECTOR = 2048
PKF_ALPHABET = ".0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"
DC_UPGRADE_VERSION = "1.0"

sys.path.insert(0, SCRIPT_DIR)
import nocturne_ui as ui  # noqa: E402
import sh2_prepare_overlays  # noqa: E402

FOOL_COSTUME_DESC = (
    "A bold ensemble that trades$practicality for sheer flair.$"
    "Only a true eccentric willing$to brave every stare can$wear this outfit."
)


# ---------------------------------------------------------------------------
# PKFINFO.TBL Base-40 Key Codec & Hierarchical Tree Rebuilder
# ---------------------------------------------------------------------------


def encode_pkf_key(name: str) -> bytes:
    digits = [PKF_ALPHABET.index(ch) + 1 for ch in name.upper() if ch in PKF_ALPHABET][:12]
    digits.extend([0] * (12 - len(digits)))
    hi, lo = [], []
    for g in range(3):
        val = (
            digits[g * 4]
            + digits[g * 4 + 1] * 40
            + digits[g * 4 + 2] * 1600
            + digits[g * 4 + 3] * 64000
        )
        hi.append(val >> 16)
        lo.append(val & 0xFFFF)
    return struct.pack(">4H", hi[0] + hi[1] * 40 + hi[2] * 1600, lo[0], lo[1], lo[2])


def decode_pkf_key(raw8: bytes) -> str:
    w0, l0, l1, l2 = struct.unpack(">4H", raw8)
    h2 = w0 // 1600
    rem = w0 % 1600
    h1 = rem // 40
    h0 = rem % 40
    chars: List[str] = []
    for hi, lo in ((h0, l0), (h1, l1), (h2, l2)):
        val = (hi << 16) | lo
        for _ in range(4):
            c = val % 40
            val //= 40
            if 1 <= c <= len(PKF_ALPHABET):
                chars.append(PKF_ALPHABET[c - 1])
    return "".join(chars)


class PkfNode:
    """One directory or file entry inside `PKFINFO.TBL`."""

    __slots__ = ("name", "key", "is_dir", "offset", "size", "children")

    def __init__(self, name: str, key: bytes, is_dir: bool, offset: int, size: int) -> None:
        self.name = name
        self.key = key
        self.is_dir = is_dir
        self.offset = offset
        self.size = size
        self.children: List[PkfNode] = []


class PkfTable:
    """Clean-room parser & hierarchical rebuilder for Shadow Hearts `PKFINFO.TBL`."""

    def __init__(self, raw: bytes) -> None:
        if len(raw) < 16:
            raise ValueError("PKFINFO.TBL too small")
        self.hdr_tail = raw[8:16]
        count = struct.unpack_from("<H", raw, 0)[0]
        if 16 + count * 16 > len(raw):
            raise ValueError(f"PKFINFO.TBL truncated: count={count}, bytes={len(raw)}")
        ents: List[Tuple[bytes, str, int, int]] = []
        for i in range(count):
            k = raw[16 + i * 16 : 24 + i * 16]
            off, sz = struct.unpack_from("<II", raw, 24 + i * 16)
            ents.append((k, decode_pkf_key(k), off, sz))
        self.root_nodes = self._build_nodes(ents, 0, count)

    def _build_nodes(
        self, ents: List[Tuple[bytes, str, int, int]], start: int, end: int
    ) -> List[PkfNode]:
        own_end = end
        nodes: List[PkfNode] = []
        subdir_refs: List[Tuple[PkfNode, int]] = []
        i = start
        while i < own_end:
            k, nm, off, sz = ents[i]
            if sz & 0x80000000:
                cs = start + ((sz & 0x7FFFFFFF) // 16)
                if cs < own_end:
                    own_end = cs
                node = PkfNode(nm, k, True, off, 0)
                nodes.append(node)
                subdir_refs.append((node, cs))
            else:
                nodes.append(PkfNode(nm, k, False, off, sz))
            i += 1
        subdir_refs.sort(key=lambda x: x[1])
        for idx, (node, cs) in enumerate(subdir_refs):
            next_cs = subdir_refs[idx + 1][1] if idx + 1 < len(subdir_refs) else end
            node.children = self._build_nodes(ents, cs, next_cs)
        return nodes

    def list_files(self) -> Dict[str, Tuple[Optional[str], int, int]]:
        """Return `{path: (root_pkb_or_None, abs_offset_in_16b_units, size_bytes)}`."""
        out: Dict[str, Tuple[Optional[str], int, int]] = {}

        def walk(nodes: List[PkfNode], prefix: str, root_pkb: Optional[str], base_off: int) -> None:
            for nd in nodes:
                path = f"{prefix}/{nd.name}" if prefix else nd.name
                if nd.is_dir:
                    rpkb = (nd.name + ".PKB") if not prefix else root_pkb
                    next_off = (base_off + nd.offset) if prefix else 0
                    walk(nd.children, path, rpkb, next_off)
                else:
                    out[path] = (root_pkb, base_off + nd.offset, nd.size)

        walk(self.root_nodes, "", None, 0)
        return out

    def find_node(self, path: str) -> Tuple[Optional[PkfNode], Optional[str], int]:
        """Return `(node, root_pkb, parent_base_off16)` for a file or directory path."""
        parts = [p.upper() for p in path.split("/") if p]
        cur_list = self.root_nodes
        root_pkb: Optional[str] = None
        base_off = 0
        for idx, comp in enumerate(parts):
            match = next((nd for nd in cur_list if nd.name.upper() == comp), None)
            if match is None:
                return None, root_pkb, base_off
            if idx == len(parts) - 1:
                return match, root_pkb, base_off
            if not match.is_dir:
                return None, root_pkb, base_off
            if idx == 0:
                root_pkb = match.name + ".PKB"
                base_off = 0
            else:
                base_off += match.offset
            cur_list = match.children
        return None, root_pkb, base_off

    def set_or_add_file(self, path: str, abs_off16: int, size: int) -> str:
        """Update an existing file or insert a new file into its parent directory.
        Returns root_pkb name (e.g. 'FIELD.PKB').
        """
        parts = [p.upper() for p in path.split("/") if p]
        if len(parts) < 2:
            raise ValueError(f"Expected PKB sub-path, got {path!r}")
        parent_path = "/".join(parts[:-1])
        fname = parts[-1]
        parent, root_pkb, parent_base = self.find_node(parent_path)
        if parent is None or not parent.is_dir:
            raise KeyError(f"Parent directory not found in PKFINFO.TBL: {parent_path!r}")
        if root_pkb is None:
            # Parent is a top-level PKB root directory (e.g. 'LSD', 'SHOP', 'OPENING')
            root_pkb = parent.name + ".PKB"
            dir_base = 0
        else:
            dir_base = parent_base + parent.offset
        rel_off16 = abs_off16 - dir_base
        if rel_off16 < 0 or rel_off16 > 0xFFFFFFFF:
            raise ValueError(f"Offset out of u32 range for {path}: abs={abs_off16}, dir_base={dir_base}")

        for ch in parent.children:
            if not ch.is_dir and ch.name.upper() == fname:
                ch.offset = rel_off16
                ch.size = size
                return root_pkb
        parent.children.append(PkfNode(fname, encode_pkf_key(fname), False, rel_off16, size))
        return root_pkb

    def serialize(self) -> bytes:
        """Flatten the tree into exact `PKFINFO.TBL` binary format (BFS/preorder directory slices)."""
        flat: List[List[object]] = []  # [key8, offset_u32, size_or_dir_u32]

        def emit_dir(nodes: List[PkfNode]) -> int:
            my_start = len(flat)
            dir_slots: List[Tuple[int, PkfNode]] = []
            for nd in nodes:
                idx = len(flat)
                flat.append([nd.key, nd.offset, 0 if nd.is_dir else nd.size])
                if nd.is_dir:
                    dir_slots.append((idx, nd))
            for idx, nd in dir_slots:
                child_start = emit_dir(nd.children)
                delta_bytes = (child_start - my_start) * 16
                flat[idx][2] = 0x80000000 | delta_bytes
            return my_start

        emit_dir(self.root_nodes)
        n = len(flat)
        hdr = struct.pack("<HHI", n, 16, n * 16) + self.hdr_tail
        body = b"".join(k + struct.pack("<II", off, sz) for k, off, sz in flat)
        return hdr + body


# ---------------------------------------------------------------------------
# Unified ISO9660 & Virtual Disc Folder Reader
# ---------------------------------------------------------------------------


class DiscReader:
    """Reads root files and PKFINFO-indexed files from an ISO or unpacked Nocturne folder."""

    def __init__(self, path: str, prefer_us_orig: bool = False) -> None:
        self.path = os.path.abspath(path)
        self.is_folder = os.path.isdir(self.path)
        self.roots: Dict[str, Tuple[int, int, Optional[str]]] = {}
        self.total_sectors = 0
        self.meta_spans: List[Tuple[int, int, int]] = []
        self._fp = None

        if self.is_folder:
            manifest = os.path.join(self.path, "disc_manifest.tsv")
            orig_manifest = os.path.join(self.path, "disc_manifest.tsv.us_orig")
            use_manifest = orig_manifest if (prefer_us_orig and os.path.isfile(orig_manifest)) else manifest
            if not os.path.isfile(use_manifest):
                raise FileNotFoundError(f"disc_manifest.tsv not found in {self.path}")
            with open(use_manifest, "r", encoding="utf-8") as mf:
                for raw_line in mf:
                    line = raw_line.rstrip("\r\n")
                    if not line or line.startswith("#"):
                        continue
                    cols = line.split("\t")
                    if cols[0] == "TOTAL_SECTORS" and len(cols) >= 2:
                        self.total_sectors = int(cols[1])
                    elif cols[0] == "META" and len(cols) >= 4:
                        self.meta_spans.append((int(cols[1]), int(cols[2]), int(cols[3])))
                    elif cols[0] == "FILE" and len(cols) >= 6:
                        iso_rel = cols[4].replace("\\", "/").lstrip("/").upper()
                        self.roots[iso_rel] = (int(cols[2]), int(cols[3]), os.path.join(self.path, cols[5]))
            pkf_raw = self.read_root("PKFINFO.TBL")
        else:
            self._fp = open(self.path, "rb")
            self._fp.seek(0, 2)
            self.total_sectors = self._fp.tell() // SECTOR
            self._fp.seek(16 * SECTOR)
            pvd = self._fp.read(SECTOR)
            if len(pvd) < SECTOR or pvd[1:6] != b"CD001":
                raise ValueError(f"Not a valid ISO9660 image: {self.path}")
            root_lba = struct.unpack_from("<I", pvd, 158)[0]
            root_sz = struct.unpack_from("<I", pvd, 166)[0]
            self._fp.seek(root_lba * SECTOR)
            raw_dir = self._fp.read(root_sz)
            pos = 0
            while pos < len(raw_dir):
                rec_len = raw_dir[pos]
                if rec_len == 0:
                    pos = ((pos // SECTOR) + 1) * SECTOR
                    continue
                flba = struct.unpack_from("<I", raw_dir, pos + 2)[0]
                fsz = struct.unpack_from("<I", raw_dir, pos + 10)[0]
                nlen = raw_dir[pos + 32]
                nm = raw_dir[pos + 33 : pos + 33 + nlen].decode("ascii", "replace").split(";", 1)[0].upper()
                self.roots[nm] = (flba, fsz, None)
                pos += rec_len
            pkf_raw = self.read_root("PKFINFO.TBL")

        self.pkf_raw = pkf_raw
        self.pkf = PkfTable(pkf_raw)
        self.files = self.pkf.list_files()

    def close(self) -> None:
        if self._fp is not None:
            self._fp.close()
            self._fp = None

    def __enter__(self) -> "DiscReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def root_lba(self, name: str) -> int:
        return self.roots[name.upper()][0]

    def read_root(self, name: str, prefer_us_orig: bool = False) -> bytes:
        lba, sz, fp = self.roots[name.upper()]
        if self.is_folder:
            assert fp is not None
            if prefer_us_orig and os.path.isfile(fp + ".us_orig"):
                with open(fp + ".us_orig", "rb") as f:
                    return f.read()
            with open(fp, "rb") as f:
                return f.read()
        assert self._fp is not None
        self._fp.seek(lba * SECTOR)
        return self._fp.read(sz)

    def read(self, path: str) -> bytes:
        rpkb, off16, sz = self.files[path.upper()]
        if rpkb is None:
            return self.read_root(path)
        lba, _, fp = self.roots[rpkb.upper()]
        byte_off = off16 * 16
        if self.is_folder:
            assert fp is not None
            with open(fp, "rb") as f:
                f.seek(byte_off)
                data = f.read(sz)
            if len(data) != sz:
                raise IOError(f"Short read for {path} from {fp}: {len(data)} != {sz}")
            return data
        assert self._fp is not None
        self._fp.seek(lba * SECTOR + byte_off)
        data = self._fp.read(sz)
        if len(data) != sz:
            raise IOError(f"Short read for {path} from {self.path}: {len(data)} != {sz}")
        return data


# ---------------------------------------------------------------------------
# Smart Folder Scanner for SLPS_732.14 (Director's Cut Discs 1 & 2)
# ---------------------------------------------------------------------------


def probe_dc_iso(path: str) -> Optional[int]:
    """Return 1 or 2 if `path` is Japanese Shadow Hearts II Director's Cut (SLPS_732.14), else None."""
    if not os.path.isfile(path):
        return None
    ext = os.path.splitext(path)[1].lower()
    if ext not in (".iso", ".bin"):
        return None
    try:
        import nocturne_setup as ns  # local import to avoid circularity at module import

        serial = ns.probe_ps2_iso_serial(path)
        if serial != "SLPS_732.14":
            return None
        marker = sh2_prepare_overlays.read_pvd_disc_marker(path)
        if marker == "0":
            return 1
        if marker == "1":
            return 2
        # Fallback if PVD marker stripped
        low = os.path.basename(path).lower()
        return 2 if re.search(r"(disc|disk)\s*2|\(2\)|_2\b", low) else 1
    except Exception:
        return None


def find_dc_isos_in_folder(folder: str, max_depth: int = 4) -> Dict[int, str]:
    """Scan `folder` recursively for JP Shadow Hearts II Director's Cut Disc 1 & Disc 2 ISOs."""
    found: Dict[int, str] = {}
    if not folder or not os.path.isdir(folder):
        return found
    root_depth = folder.rstrip("\\/").count(os.sep)
    for cur, dirs, files in os.walk(folder):
        depth = cur.rstrip("\\/").count(os.sep) - root_depth
        if depth >= max_depth:
            dirs[:] = []
        else:
            dirs[:] = [
                d
                for d in dirs
                if not d.startswith(".")
                and d.lower() not in ("$recycle.bin", "system volume information")
            ]
        for fn in sorted(files):
            if os.path.splitext(fn)[1].lower() not in (".iso", ".bin"):
                continue
            full = os.path.join(cur, fn)
            disc_num = probe_dc_iso(full)
            if disc_num in (1, 2) and disc_num not in found:
                found[disc_num] = full
                if len(found) == 2:
                    return found
    return found


# ---------------------------------------------------------------------------
# Clean-Room Binary Patchers: FIELD.BIN Tailor, TAILOR.DAT, SSE, LSDH
# ---------------------------------------------------------------------------


def _mask_mips_for_alignment(w: int) -> Tuple[int, int]:
    op = w >> 26
    if op in (2, 3):  # j / jal
        return (op, 0)
    if op in (0x01, 0x04, 0x05, 0x06, 0x07):  # branches
        return (op, (w >> 16) & 0x3FF)
    if op in (0x09, 0x0A, 0x0B, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B):
        return (op, (w >> 16) & 0x3FF)
    return (op, w & 0x03FFFFFF)


def patch_field_bin_tailor(us_fb: bytes, dc_fb: bytes) -> Tuple[bytes, Dict[str, int]]:
    """Clean-room MIPS alignment patcher porting the 11-costume Tailor support from DC FIELD.BIN."""
    magic_u, _, ub = struct.unpack_from("<3I", us_fb, 0)
    magic_d, _, db = struct.unpack_from("<3I", dc_fb, 0)
    if magic_u != 0x336F574D or magic_d != 0x336F574D:
        raise ValueError("Expected MWo3 header on US & DC FIELD.BIN")

    ua0, ua1 = 0x533630, 0x53B400
    da0, da1 = 0x5354E0, 0x53D400
    if ua1 - ub > len(us_fb) or da1 - db > len(dc_fb):
        raise ValueError("FIELD.BIN size smaller than expected Tailor code window")

    uwords = [struct.unpack_from("<I", us_fb, a - ub)[0] for a in range(ua0, ua1, 4)]
    dwords = [struct.unpack_from("<I", dc_fb, a - db)[0] for a in range(da0, da1, 4)]
    sm = difflib.SequenceMatcher(
        None,
        [_mask_mips_for_alignment(w) for w in uwords],
        [_mask_mips_for_alignment(w) for w in dwords],
        autojunk=False,
    )

    out = bytearray(us_fb)
    stats = {"field_offset_plus4": 0, "tailor_index_plus1": 0, "dress_loop_10_to_11": 0}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            continue
        for k in range(i2 - i1):
            uw = uwords[i1 + k]
            dw = dwords[j1 + k]
            if uw == dw or (uw & 0xFFFF0000) != (dw & 0xFFFF0000):
                continue
            addr = ua0 + (i1 + k) * 4
            uimm = uw & 0xFFFF
            dimm = dw & 0xFFFF
            rs = (uw >> 21) & 31
            op = uw >> 26
            if dimm == uimm + 4 and 0x14C <= uimm <= 0x1B0 and rs != 29:
                struct.pack_into("<I", out, addr - ub, dw)
                stats["field_offset_plus4"] += 1
            elif (uw >> 16) == 0x2405 and dimm == uimm + 1 and 25 <= uimm <= 30:
                struct.pack_into("<I", out, addr - ub, dw)
                stats["tailor_index_plus1"] += 1
            elif op in (0x0A, 0x0B) and uimm == 10 and dimm == 11:
                struct.pack_into("<I", out, addr - ub, dw)
                stats["dress_loop_10_to_11"] += 1

    if stats["field_offset_plus4"] < 120 or stats["tailor_index_plus1"] < 5 or stats["dress_loop_10_to_11"] != 2:
        raise RuntimeError(f"Unexpected Tailor alignment stats on FIELD.BIN: {stats}")
    return bytes(out), stats


def patch_tailor_dat(us_td: bytes, dc_td: bytes) -> bytes:
    """Splice DC's 11th costume sketchbook model (entry 25) + English description into US SHOP/TAILOR.DAT."""

    def unpack_entries(blob: bytes) -> List[Tuple[bytes, bytes]]:
        cnt = struct.unpack_from("<I", blob, 0)[0]
        offs = [struct.unpack_from("<I", blob, 12 + 8 * i)[0] for i in range(cnt)] + [len(blob)]
        return [(blob[8 + 8 * i : 12 + 8 * i], blob[offs[i] : offs[i + 1]]) for i in range(cnt)]

    ue = unpack_entries(us_td)
    de = unpack_entries(dc_td)
    if len(ue) != 31 or len(de) != 32 or de[25][0] != b"mdl\x00":
        raise ValueError(f"Unexpected TAILOR.DAT layout (US={len(ue)}, DC={len(de)})")

    merged = ue[:25] + [de[25]] + ue[25:]
    # Append English description for dress 10 (slot 21 in the 22-entry Change Dress msg block)
    last_tag, last_msg = merged[-1]
    if last_tag == b"msg\x00":
        m = bytearray(last_msg)
        msg_cnt = struct.unpack_from("<H", m, 0)[0]
        if msg_cnt == 22:
            pool_base = 2 + 2 * msg_cnt
            off21 = struct.unpack_from("<H", m, 2 + 2 * 21)[0]
            if pool_base + off21 < len(m) and m[pool_base + off21] == 0:
                struct.pack_into("<H", m, 2 + 2 * 21, len(m) - pool_base)
                m += FOOL_COSTUME_DESC.encode("ascii") + b"\x00"
                merged[-1] = (last_tag, bytes(m))

    header_end = (8 + 8 * len(merged) + 0xF) & ~0xF
    body = bytearray()
    offsets: List[int] = []
    for _, payload in merged:
        offsets.append(header_end + len(body))
        body += payload
        body += bytes(-len(body) % 16)

    hdr = bytearray(header_end)
    struct.pack_into("<II", hdr, 0, len(merged), header_end + len(body))
    for i, (tag, _) in enumerate(merged):
        hdr[8 + 8 * i : 12 + 8 * i] = tag
        struct.pack_into("<I", hdr, 12 + 8 * i, offsets[i])
    return bytes(hdr) + bytes(body)


def patch_sse_voice_bank(us_sse: bytes, dc_sse: bytes) -> bytes:
    """Port a JP DC `.SSE` sound bank to US while preserving US SDRH wave-bank IDs."""
    if len(us_sse) < 48 or len(dc_sse) < 48:
        return dc_sse
    if us_sse[:4] != b"SDRH" or dc_sse[:4] != b"SDRH":
        return dc_sse
    us_bank = struct.unpack_from("<I", us_sse, 0x0C)[0]
    dc_bank = struct.unpack_from("<I", dc_sse, 0x0C)[0]
    if us_bank == dc_bank:
        return dc_sse
    out = bytearray(dc_sse)
    struct.pack_into("<I", out, 0x0C, us_bank)
    # High 16 bits at +0x28 store the matching wave-bank ID
    us_w10_hi = struct.unpack_from("<H", us_sse, 0x2A)[0]
    struct.pack_into("<H", out, 0x2A, us_w10_hi)
    return bytes(out)


def _parse_lsd_tracks(blob: bytes) -> Tuple[List[Tuple[int, int, str, int, int]], int]:
    """Return `(tracks, stream_start)` where each track is `(idx, offset, tag, byte_len, flags)`."""
    if len(blob) < 0x40 or blob[:4] != b"LSDH":
        raise ValueError("Not an LSDH stream")
    pos = 0x40
    tracks: List[Tuple[int, int, str, int, int]] = []
    while pos + 16 <= len(blob):
        tag_b = blob[pos : pos + 4]
        if not all(32 <= b < 127 for b in tag_b):
            break
        qw, flags = struct.unpack_from("<HH", blob, pos + 0x10)
        if qw == 0:
            break
        tracks.append((len(tracks), pos, tag_b.decode("ascii"), qw * 16, flags))
        pos += qw * 16
    return tracks, pos


def swap_lsd_pcms_audio(us_lsd: bytes, dc_lsd: bytes) -> Tuple[bytes, str]:
    """Losslessly swap 48 kHz JP DC `PCMS` packets into a US `.LSD` cutscene container.

    Preserves 100% of US video, 3D animations, camera tracks, and English `SBTD` subtitles.
    Returns `(patched_bytes, status)` where status is `'swapped'`, `'skipped_rate'`, or `'skipped_no_pcms'`.
    """
    utracks, ustart = _parse_lsd_tracks(us_lsd)
    dtracks, dstart = _parse_lsd_tracks(dc_lsd)
    u_pcms = next((t for t in utracks if t[2] == "PCMS"), None)
    d_pcms = next((t for t in dtracks if t[2] == "PCMS"), None)
    if u_pcms is None or d_pcms is None:
        return us_lsd, "skipped_no_pcms"

    u_ch_rate, u_per_ch = struct.unpack_from("<II", us_lsd, u_pcms[1] + 0x24)
    d_ch_rate, d_per_ch = struct.unpack_from("<II", dc_lsd, d_pcms[1] + 0x24)
    if u_ch_rate != d_ch_rate:
        # e.g. N024 / N104 (44.1 kHz in DC vs 48 kHz in US): keep US audio cleanly
        return us_lsd, "skipped_rate"

    # Extract JP DC PCMS payload stream (each packet is interleaved L/R SPU-ADPCM chunks)
    d_tid = d_pcms[0]
    dc_chunks: List[bytes] = []
    pos = dstart
    while pos + 16 <= len(dc_lsd):
        tid, _frm, qsz, _fl = struct.unpack_from("<4I", dc_lsd, pos)
        if tid == 0xFFFFFFFF:
            break
        plen = qsz * 16
        if tid == d_tid:
            dc_chunks.append(dc_lsd[pos + 16 : pos + 16 + plen])
        pos += 16 + plen
    dc_stream = b"".join(dc_chunks)

    # Splice DC SPU-ADPCM bytes cleanly into US PCMS packet slots (exact same packet sizes & frame sync)
    out = bytearray(us_lsd)
    u_tid = u_pcms[0]
    pos = ustart
    cursor = 0
    dc_len = len(dc_stream)
    while pos + 16 <= len(out):
        tid, _frm, qsz, _fl = struct.unpack_from("<4I", out, pos)
        if tid == 0xFFFFFFFF:
            break
        plen = qsz * 16
        if tid == u_tid and plen > 0:
            take = min(plen, max(0, dc_len - cursor))
            if take > 0:
                out[pos + 16 : pos + 16 + take] = dc_stream[cursor : cursor + take]
                cursor += take
            if take < plen:
                # Pad remaining tail with silent valid 16-byte SPU-ADPCM blocks (all zeros)
                out[pos + 16 + take : pos + 16 + plen] = b"\x00" * (plen - take)
        pos += 16 + plen

    # Keep per-channel byte count clamped to US capacity so engine streaming buffer never over-reads
    struct.pack_into("<I", out, u_pcms[1] + 0x28, min(u_per_ch, d_per_ch))
    return bytes(out), "swapped"


def graft_lsd_sbtd_subtitles(dc_lsd: bytes, us_lsd: bytes) -> bytes:
    """Graft US English `SBTD` subtitle pack into a DC pre-rendered `.LSD` (e.g. `N125.LSD`)."""
    utracks, ustart = _parse_lsd_tracks(us_lsd)
    dtracks, dstart = _parse_lsd_tracks(dc_lsd)
    u_sbtd = next((t for t in utracks if t[2] == "SBTD"), None)
    d_sbtd = next((t for t in dtracks if t[2] == "SBTD"), None)
    if u_sbtd is None or d_sbtd is None:
        return dc_lsd

    # Locate US SBTD packet payload
    us_payload: Optional[bytes] = None
    pos = ustart
    while pos + 16 <= len(us_lsd):
        tid, _frm, qsz, _fl = struct.unpack_from("<4I", us_lsd, pos)
        if tid == 0xFFFFFFFF:
            break
        plen = qsz * 16
        if tid == u_sbtd[0]:
            us_payload = us_lsd[pos + 16 : pos + 16 + plen]
            break
        pos += 16 + plen
    if us_payload is None:
        return dc_lsd

    us_qsz = (len(us_payload) + 15) // 16
    us_padded = us_payload + bytes(-len(us_payload) % 16)

    # Rebuild DC LSD with US SBTD packet swapped cleanly
    out = bytearray(dc_lsd[:dstart])
    # Update SBTD track header's payload qword count (+0x18)
    struct.pack_into("<I", out, d_sbtd[1] + 0x18, us_qsz)

    pos = dstart
    replaced = False
    while pos + 16 <= len(dc_lsd):
        tid, frm, qsz, fl = struct.unpack_from("<4I", dc_lsd, pos)
        if tid == 0xFFFFFFFF:
            out += dc_lsd[pos : pos + 16]
            pos += 16
            break
        plen = qsz * 16
        if tid == d_sbtd[0] and not replaced:
            out += struct.pack("<4I", tid, frm, us_qsz, fl) + us_padded
            replaced = True
        else:
            out += dc_lsd[pos : pos + 16 + plen]
        pos += 16 + plen
    if pos < len(dc_lsd):
        out += dc_lsd[pos:]
    return bytes(out)


# ---------------------------------------------------------------------------
# Virtual Disc Atomic Patcher (disc_meta.bin + disc_manifest.tsv + unindexed/)
# ---------------------------------------------------------------------------


def _patch_iso_dir_and_pvd(
    meta_bytes: bytes,
    meta_spans: List[Tuple[int, int, int]],
    old_tbl_lba: int,
    old_tbl_size: int,
    new_tbl_lba: int,
    new_tbl_size: int,
    new_total_sectors: int,
) -> bytes:
    """Update `PKFINFO.TBL` directory record and PVD Volume Space Size inside `disc_meta.bin`."""
    buf = bytearray(meta_bytes)

    # 1. Patch PVD sector 16 Volume Space Size (offset 80..88: LE + BE u32)
    for start_sec, count_sec, meta_off in meta_spans:
        if start_sec <= 16 < start_sec + count_sec:
            pvd_off = meta_off + (16 - start_sec) * SECTOR
            if pvd_off + SECTOR <= len(buf) and buf[pvd_off + 1 : pvd_off + 6] == b"CD001":
                struct.pack_into("<I", buf, pvd_off + 80, new_total_sectors)
                struct.pack_into(">I", buf, pvd_off + 84, new_total_sectors)
            break

    # 2. Patch ISO9660 directory record for PKFINFO.TBL
    needle = (
        struct.pack("<I", old_tbl_lba)
        + struct.pack(">I", old_tbl_lba)
        + struct.pack("<I", old_tbl_size)
        + struct.pack(">I", old_tbl_size)
    )
    repl = (
        struct.pack("<I", new_tbl_lba)
        + struct.pack(">I", new_tbl_lba)
        + struct.pack("<I", new_tbl_size)
        + struct.pack(">I", new_tbl_size)
    )
    patched = False
    search = 0
    while True:
        idx = buf.find(needle, search)
        if idx < 0:
            break
        rec = idx - 2
        if rec >= 0 and rec + 34 <= len(buf):
            nlen = buf[rec + 32]
            if rec + 33 + nlen <= len(buf):
                nm = buf[rec + 33 : rec + 33 + nlen].decode("latin1", errors="ignore").split(";", 1)[0].upper()
                if nm == "PKFINFO.TBL":
                    buf[idx : idx + 16] = repl
                    patched = True
        search = idx + 1

    if not patched:
        raise RuntimeError("Could not locate PKFINFO.TBL directory record in disc_meta.bin")
    return bytes(buf)


def ensure_us_baseline_backups(disc_dir: str) -> None:
    """Snapshot pristine US metadata files on first upgrade so future re-runs are idempotent."""
    for rel in ("disc_manifest.tsv", "disc_meta.bin", os.path.join("layer0", "FIELD.BIN")):
        src = os.path.join(disc_dir, rel)
        bak = src + ".us_orig"
        if os.path.isfile(src) and not os.path.isfile(bak):
            shutil.copy2(src, bak)


def _hardlink_or_copy(src: str, dst: str) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    try:
        os.link(src, tmp)
        os.replace(tmp, dst)
    except OSError:
        if os.path.exists(tmp):
            os.remove(tmp)
        shutil.copy2(src, tmp)
        os.replace(tmp, dst)


def apply_dc_upgrade_to_virtual_discs(
    game_dir: str,
    dc1_iso: str,
    dc2_iso: Optional[str],
    edition: str,
    workers: int = 0,
) -> Dict[str, object]:
    """Perform the clean-room Director's Cut upgrade on `disc1_files` (and `disc2_files`)."""
    t0 = time.monotonic()
    d1_dir = os.path.join(game_dir, "disc1_files")
    d2_dir = os.path.join(game_dir, "disc2_files")
    has_d2 = bool(dc2_iso and os.path.isdir(d2_dir) and os.path.isfile(os.path.join(d2_dir, "disc_manifest.tsv")))
    if not os.path.isfile(os.path.join(d1_dir, "disc_manifest.tsv")):
        raise RuntimeError(f"Unpacked US Disc 1 not found in {d1_dir}. Build Covenant first!")

    ensure_us_baseline_backups(d1_dir)
    if has_d2:
        ensure_us_baseline_backups(d2_dir)

    print(f"  [1/5] Opening US baseline virtual discs & JP Director's Cut ISOs ({edition})...")
    us1 = DiscReader(d1_dir, prefer_us_orig=True)
    dc1 = DiscReader(dc1_iso, prefer_us_orig=False)
    us2 = DiscReader(d2_dir, prefer_us_orig=True) if has_d2 else None
    dc2 = DiscReader(dc2_iso, prefer_us_orig=False) if (has_d2 and dc2_iso) else None

    try:
        # 1. Patch FIELD.BIN (Tailor 11th costume) cleanly via MIPS instruction alignment
        print("  [2/5] Patching FIELD.BIN (Tailor 11th costume) & SHOP/TAILOR.DAT...")
        us_fb = us1.read_root("FIELD.BIN", prefer_us_orig=True)
        dc_fb = dc1.read_root("FIELD.BIN")
        patched_fb, tailor_stats = patch_field_bin_tailor(us_fb, dc_fb)
        patched_tailor = patch_tailor_dat(us1.read("SHOP/TAILOR.DAT"), dc1.read("SHOP/TAILOR.DAT"))
        print(
            f"        FIELD.BIN Tailor patches: +{tailor_stats['field_offset_plus4']} struct offsets, "
            f"+{tailor_stats['tailor_index_plus1']} TAILOR.DAT indices, "
            f"+{tailor_stats['dress_loop_10_to_11']} loop bounds (10 -> 11)"
        )

        # 2. Collect SHARED assets (identical on Disc 1 and Disc 2 -> written once & hardlinked!)
        print("  [3/5] Building shared Director's Cut virtual archive (dc_shared.bin)...")
        shared_items: List[Tuple[str, bytes]] = []
        dc1_only = sorted(set(dc1.files) - set(us1.files))
        shared_dc_only = [p for p in dc1_only if p != "LSD/SH3_SP.LSD"]
        for p in shared_dc_only:
            shared_items.append((p, dc1.read(p)))
        # DC Opening movie & Tailor data
        shared_items.append(("OPENING/DEMO.LSD", dc1.read("OPENING/DEMO.LSD")))
        shared_items.append(("SHOP/TAILOR.DAT", patched_tailor))

        undub_btl_count = 0
        undub_fld_count = 0
        if edition == "undub":
            for prefix in ("BTLDATA/SOUND/SE/", "FIELD/SOUND/SE/"):
                for p in sorted(k for k in us1.files if k.startswith(prefix) and k.endswith(".SSE") and k in dc1.files):
                    ub = us1.read(p)
                    db = dc1.read(p)
                    if ub != db:
                        shared_items.append((p, patch_sse_voice_bank(ub, db)))
                        if prefix.startswith("BTLDATA"):
                            undub_btl_count += 1
                        else:
                            undub_fld_count += 1
            print(f"        UnDub voice banks queued: {undub_btl_count} battle SE + {undub_fld_count} field SE")

        # Write dc_shared.bin once inside disc1_files/unindexed/ and record relative sector offsets
        d1_unindexed = os.path.join(d1_dir, "unindexed")
        os.makedirs(d1_unindexed, exist_ok=True)
        shared_bin_d1 = os.path.join(d1_unindexed, "dc_shared.bin")
        shared_tmp = shared_bin_d1 + ".tmp"
        shared_layout: List[Tuple[str, int, int]] = []  # (pkf_path, rel_sector, byte_size)
        cur_sec = 0
        with open(shared_tmp, "wb") as sf:
            for p, data in shared_items:
                shared_layout.append((p, cur_sec, len(data)))
                sf.write(data)
                pad = (-len(data)) % SECTOR
                if pad:
                    sf.write(b"\x00" * pad)
                cur_sec += (len(data) + SECTOR - 1) // SECTOR
        os.replace(shared_tmp, shared_bin_d1)
        shared_sectors = cur_sec
        shared_bytes = shared_sectors * SECTOR
        print(
            f"        Wrote dc_shared.bin: {len(shared_items)} files ({shared_bytes / (1 << 20):.1f} MiB, "
            f"{shared_sectors} sectors)"
        )

        # Hardlink dc_shared.bin to disc2_files so Disc 1 & Disc 2 share 0 duplicate bytes
        if has_d2:
            d2_unindexed = os.path.join(d2_dir, "unindexed")
            os.makedirs(d2_unindexed, exist_ok=True)
            _hardlink_or_copy(shared_bin_d1, os.path.join(d2_unindexed, "dc_shared.bin"))

        # Helper to build per-disc dc_disc.bin + PKFINFO.TBL + disc_meta.bin + disc_manifest.tsv
        n_workers = workers if workers > 0 else min(8, max(2, os.cpu_count() or 4))
        undub_lsd_swapped = 0
        undub_lsd_skipped = 0

        def finalize_disc(
            disc_num: int,
            disc_dir: str,
            us_reader: DiscReader,
            dc_reader: DiscReader,
            dc_iso_path: str,
        ) -> Tuple[int, int]:
            nonlocal undub_lsd_swapped, undub_lsd_skipped
            base_sec = us_reader.total_sectors
            shared_start_lba = base_sec
            disc_bin_start_lba = shared_start_lba + shared_sectors

            # Build per-disc items
            disc_items: List[Tuple[str, bytes]] = []
            if disc_num == 1 and "LSD/SH3_SP.LSD" in dc_reader.files:
                disc_items.append(("LSD/SH3_SP.LSD", dc_reader.read("LSD/SH3_SP.LSD")))
            if disc_num == 2 and "LSD/N125.LSD" in dc_reader.files:
                print("        Upgrading Disc 2 LSD/N125.LSD ('The Promise') -> DC MPEG-2 + English SBTD...")
                disc_items.append(
                    ("LSD/N125.LSD", graft_lsd_sbtd_subtitles(dc_reader.read("LSD/N125.LSD"), us_reader.read("LSD/N125.LSD")))
                )

            # UnDub cutscene PCMS stream swaps (parallelized across CPU threads)
            if edition == "undub":
                lsd_keys = sorted(
                    k
                    for k in us_reader.files
                    if k.startswith("LSD/") and k in dc_reader.files and not (disc_num == 2 and k == "LSD/N125.LSD")
                )
                print(f"        Swapping JP cutscene voices on Disc {disc_num} ({len(lsd_keys)} cutscenes)...")

                def _swap_one(k: str) -> Tuple[str, Optional[bytes], str]:
                    with DiscReader(disc_dir, prefer_us_orig=True) as u_loc, DiscReader(dc_iso_path, prefer_us_orig=False) as d_loc:
                        ub = u_loc.read(k)
                        db = d_loc.read(k)
                    res, st = swap_lsd_pcms_audio(ub, db)
                    return k, (res if st == "swapped" else None), st

                with ThreadPoolExecutor(max_workers=n_workers) as ex:
                    for k, payload, st in ex.map(_swap_one, lsd_keys):
                        if st == "swapped" and payload is not None:
                            disc_items.append((k, payload))
                            undub_lsd_swapped += 1
                        else:
                            undub_lsd_skipped += 1

            unindexed_dir = os.path.join(disc_dir, "unindexed")
            os.makedirs(unindexed_dir, exist_ok=True)
            disc_bin_path = os.path.join(unindexed_dir, "dc_disc.bin")
            disc_bin_tmp = disc_bin_path + ".tmp"
            disc_layout: List[Tuple[str, int, int]] = []
            dcur = 0
            with open(disc_bin_tmp, "wb") as df:
                for p, data in disc_items:
                    disc_layout.append((p, dcur, len(data)))
                    df.write(data)
                    pad = (-len(data)) % SECTOR
                    if pad:
                        df.write(b"\x00" * pad)
                    dcur += (len(data) + SECTOR - 1) // SECTOR
            os.replace(disc_bin_tmp, disc_bin_path)
            disc_sectors = dcur

            # Rebuild PKFINFO.TBL cleanly with all shared + disc-specific entries
            tbl = PkfTable(us_reader.pkf_raw)
            for p, rel_sec, sz in shared_layout:
                rpkb = p.split("/", 1)[0] + ".PKB"
                abs_lba = shared_start_lba + rel_sec
                off16 = (abs_lba - us_reader.root_lba(rpkb)) << 7
                tbl.set_or_add_file(p, off16, sz)
            for p, rel_sec, sz in disc_layout:
                rpkb = p.split("/", 1)[0] + ".PKB"
                abs_lba = disc_bin_start_lba + rel_sec
                off16 = (abs_lba - us_reader.root_lba(rpkb)) << 7
                tbl.set_or_add_file(p, off16, sz)

            new_pkf_bytes = tbl.serialize()
            # Self-verify serialized PKFINFO.TBL round-trip!
            verify_tbl = PkfTable(new_pkf_bytes)
            verify_files = verify_tbl.list_files()
            for p, _, sz in shared_layout + disc_layout:
                if p.upper() not in verify_files or verify_files[p.upper()][2] != sz:
                    raise RuntimeError(f"PKFINFO.TBL round-trip verification failed for {p}")

            pkf_lba = disc_bin_start_lba + disc_sectors
            pkf_sectors = (len(new_pkf_bytes) + SECTOR - 1) // SECTOR
            new_total_sectors = pkf_lba + pkf_sectors

            pkf_path = os.path.join(unindexed_dir, "dc_pkfinfo.tbl")
            pkf_tmp = pkf_path + ".tmp"
            with open(pkf_tmp, "wb") as pf:
                pf.write(new_pkf_bytes)
            os.replace(pkf_tmp, pkf_path)

            # Patch disc_meta.bin from pristine .us_orig backup
            old_tbl_lba, old_tbl_sz, _ = us_reader.roots["PKFINFO.TBL"]
            orig_meta = open(os.path.join(disc_dir, "disc_meta.bin.us_orig"), "rb").read()
            new_meta = _patch_iso_dir_and_pvd(
                orig_meta,
                us_reader.meta_spans,
                old_tbl_lba,
                old_tbl_sz,
                pkf_lba,
                len(new_pkf_bytes),
                new_total_sectors,
            )
            meta_tmp = os.path.join(disc_dir, "disc_meta.bin.tmp")
            with open(meta_tmp, "wb") as mf:
                mf.write(new_meta)
            os.replace(meta_tmp, os.path.join(disc_dir, "disc_meta.bin"))

            # Atomically write updated disc_manifest.tsv (from pristine .us_orig)
            orig_manifest_lines = open(os.path.join(disc_dir, "disc_manifest.tsv.us_orig"), "r", encoding="utf-8").readlines()
            new_lines: List[str] = []
            for raw_line in orig_manifest_lines:
                s = raw_line.rstrip("\r\n")
                if s.startswith("TOTAL_SECTORS\t"):
                    cols = s.split("\t")
                    l1 = cols[3] if len(cols) >= 4 else "0"
                    new_lines.append(f"TOTAL_SECTORS\t{new_total_sectors}\tLAYER1_START\t{l1}\n")
                    continue
                if s.startswith("FILE\t"):
                    cols = s.split("\t")
                    if len(cols) >= 6 and cols[4].replace("\\", "/").lstrip("/").upper() == "PKFINFO.TBL":
                        # Replaced by unindexed/dc_pkfinfo.tbl at pkf_lba
                        continue
                new_lines.append(raw_line)
            if shared_sectors > 0:
                new_lines.append(
                    f"FILE\t0\t{shared_start_lba}\t{shared_sectors * SECTOR}\t__DC_SHARED__\tunindexed/dc_shared.bin\n"
                )
            if disc_sectors > 0:
                new_lines.append(
                    f"FILE\t0\t{disc_bin_start_lba}\t{disc_sectors * SECTOR}\t__DC_DISC__\tunindexed/dc_disc.bin\n"
                )
            new_lines.append(f"FILE\t0\t{pkf_lba}\t{len(new_pkf_bytes)}\tPKFINFO.TBL\tunindexed/dc_pkfinfo.tbl\n")

            man_tmp = os.path.join(disc_dir, "disc_manifest.tsv.tmp")
            with open(man_tmp, "w", encoding="utf-8", newline="\n") as outf:
                outf.writelines(new_lines)
            os.replace(man_tmp, os.path.join(disc_dir, "disc_manifest.tsv"))

            # Write patched FIELD.BIN into layer0/FIELD.BIN
            fb_dst = os.path.join(disc_dir, "layer0", "FIELD.BIN")
            if disc_num == 1:
                fb_tmp = fb_dst + ".tmp"
                with open(fb_tmp, "wb") as fbf:
                    fbf.write(patched_fb)
                os.replace(fb_tmp, fb_dst)
            else:
                _hardlink_or_copy(os.path.join(d1_dir, "layer0", "FIELD.BIN"), fb_dst)

            return len(disc_items), new_total_sectors

        print("  [4/5] Applying virtual-disc Director's Cut upgrade atomically...")
        d1_extra, d1_secs = finalize_disc(1, d1_dir, us1, dc1, dc1_iso)
        print(f"        Disc 1 upgraded: {d1_extra} disc-specific file(s), {d1_secs:,} virtual sectors")
        discs_done = [1]
        d2_extra = 0
        if has_d2 and us2 is not None and dc2 is not None and dc2_iso is not None:
            d2_extra, d2_secs = finalize_disc(2, d2_dir, us2, dc2, dc2_iso)
            print(f"        Disc 2 upgraded: {d2_extra} disc-specific file(s), {d2_secs:,} virtual sectors")
            discs_done.append(2)

        meta = {
            "installed": True,
            "version": DC_UPGRADE_VERSION,
            "edition": edition,
            "discs_upgraded": discs_done,
            "upgraded_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "dc_only_files_added": len(dc1_only),
            "tailor_patches": tailor_stats,
            "undub_battle_banks": undub_btl_count,
            "undub_field_banks": undub_fld_count,
            "undub_cutscenes_swapped": undub_lsd_swapped,
            "undub_cutscenes_kept_en": undub_lsd_skipped,
            "elapsed_sec": round(time.monotonic() - t0, 2),
        }
        marker_path = os.path.join(game_dir, "dc_upgrade.json")
        marker_tmp = marker_path + ".tmp"
        with open(marker_tmp, "w", encoding="utf-8", newline="\n") as jf:
            json.dump(meta, jf, indent=2)
            jf.write("\n")
        os.replace(marker_tmp, marker_path)
        return meta
    finally:
        us1.close()
        dc1.close()
        if us2 is not None:
            us2.close()
        if dc2 is not None:
            dc2.close()


# ---------------------------------------------------------------------------
# CLI & Interactive Launcher Flow
# ---------------------------------------------------------------------------


def main(argv: Optional[List[str]] = None) -> int:
    rec_jobs, _ = ui.recommended_compile_jobs()
    ap = argparse.ArgumentParser(description="Shadow Hearts: Covenant — Native Director's Cut Permanent Upgrader")
    ap.add_argument("--install-dir", default=None, help="Nocturne install folder")
    ap.add_argument("--dc-dir", default=None, help="Folder containing JP Director's Cut ISOs (SLPS_732.14)")
    ap.add_argument("--dc1", default=None, help="Explicit path to JP Director's Cut Disc 1 ISO")
    ap.add_argument("--dc2", default=None, help="Explicit path to JP Director's Cut Disc 2 ISO")
    ap.add_argument("--edition", choices=("regular", "undub"), default=None, help="regular (EN voices) or undub (JP voices)")
    ap.add_argument("--jobs", type=int, default=rec_jobs, help=f"Parallel compile jobs (default: {rec_jobs})")
    ap.add_argument("--skip-dll-rebuild", action="store_true", help="Patch virtual discs only without recompiling game.dll")
    ap.add_argument("--skip-app-check", action="store_true", help="Skip running-app check")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively")
    ap.add_argument("--pause", action="store_true", help="Wait for keypress on exit (for Launcher console windows)")
    args = ap.parse_args(argv)

    interactive = not args.non_interactive
    cfg = ui.load_install_config()
    install_dir = ui.resolve_install_dir(args.install_dir)
    game_dir = os.path.join(install_dir, "games", "sh2-covenant")

    ui.print_banner(
        "SHADOW HEARTS: COVENANT — DIRECTOR'S CUT PERMANENT UPGRADE",
        f"Install: {install_dir}  |  100% Nocturne-Native Virtual-Disc Upgrade (No US ISOs Needed)",
    )

    if not os.path.isfile(os.path.join(game_dir, "disc1_files", "disc_manifest.tsv")):
        print(ui.paint(f"  [ERROR] Unpacked US Covenant not found in {game_dir}\\disc1_files\\.", ui.Style.CRIMSON))
        print("  Build Shadow Hearts: Covenant first, then run this Director's Cut upgrader.")
        if args.pause:
            ui.wait_for_keypress("Press any key to close...")
        return 1

    # 1. Resolve JP Director's Cut ISOs (explicit -> --dc-dir -> saved last_rom_dir -> interactive folder picker)
    dc1 = args.dc1
    dc2 = args.dc2
    search_dirs: List[str] = []
    if args.dc_dir:
        search_dirs.append(args.dc_dir)
    if cfg.get("last_rom_dir"):
        search_dirs.append(cfg["last_rom_dir"])

    for sdir in search_dirs:
        if dc1 and dc2:
            break
        found = find_dc_isos_in_folder(sdir)
        if not dc1 and 1 in found:
            dc1 = found[1]
        if not dc2 and 2 in found:
            dc2 = found[2]

    while not dc1 and interactive:
        print("\n  Locate the folder containing your Japanese Director's Cut ISOs (SLPS_732.14, Discs 1 & 2).")
        raw = input("  Press [B] to browse folder, paste folder/ISO path, or [Q] to cancel: ").strip()
        if raw.lower() in ("q", "quit", "cancel", ""):
            print("  Upgrade cancelled.")
            return ui.EXIT_USER_CANCELLED
        if raw.lower() == "b":
            chosen = ui.pick_folder_dialog(
                "Select Folder Containing Japanese Shadow Hearts II Director's Cut ISOs",
                initial_dir=cfg.get("last_rom_dir") or "",
            )
            if not chosen:
                continue
            raw = chosen
        cleaned = raw.strip('"').strip("'")
        if os.path.isdir(cleaned):
            found = find_dc_isos_in_folder(cleaned)
            dc1 = dc1 or found.get(1)
            dc2 = dc2 or found.get(2)
            if dc1:
                cfg["last_rom_dir"] = cleaned
                ui.save_install_config(cfg)
            else:
                print(ui.paint("  No SLPS_732.14 Director's Cut ISO found in that folder.", ui.Style.YELLOW))
        elif os.path.isfile(cleaned):
            dn = probe_dc_iso(cleaned)
            if dn == 1:
                dc1 = cleaned
                siblings = find_dc_isos_in_folder(os.path.dirname(cleaned))
                dc2 = dc2 or siblings.get(2)
            elif dn == 2:
                dc2 = cleaned
                siblings = find_dc_isos_in_folder(os.path.dirname(cleaned))
                dc1 = dc1 or siblings.get(1)
            else:
                print(ui.paint("  That file is not SLPS_732.14 (JP Shadow Hearts II Director's Cut).", ui.Style.YELLOW))
        else:
            print(ui.paint(f"  Path not found: {cleaned}", ui.Style.CRIMSON))

    if not dc1:
        print(ui.paint("  [ERROR] Could not find Japanese Director's Cut Disc 1 ISO (SLPS_732.14).", ui.Style.CRIMSON))
        if args.pause:
            ui.wait_for_keypress("Press any key to close...")
        return 1

    print(f"  {ui.paint('[OK]', ui.Style.GREEN)} JP Director's Cut Disc 1 : {dc1}")
    if dc2:
        print(f"  {ui.paint('[OK]', ui.Style.GREEN)} JP Director's Cut Disc 2 : {dc2}")
    else:
        print(ui.paint("  [WARN] JP Director's Cut Disc 2 not found — only Disc 1 will be upgraded.", ui.Style.YELLOW))

    # 2. Choose Edition (Regular English Dub vs UnDub JP Voices / EN Subs)
    edition = args.edition
    if not edition:
        if not interactive:
            edition = "regular"
        else:
            print("\n  Choose Director's Cut Audio Edition:")
            print(f"    [{ui.paint('1', ui.Style.GOLD)}] Regular  — English dub voices + Director's Cut content (Recommended)")
            print(f"    [{ui.paint('2', ui.Style.GOLD)}] UnDub    — Japanese voices (cutscenes, battles & field) + English subtitles")
            while True:
                ans = input("\n  Select edition [1/2, default 1]: ").strip().lower()
                if ans in ("", "1", "r", "regular"):
                    edition = "regular"
                    break
                if ans in ("2", "u", "undub"):
                    edition = "undub"
                    break
                print("  Please enter 1 or 2.")

    if not args.skip_app_check:
        if not ui.ensure_apps_closed(["nocturne_ps2.exe", "SH2C.exe"], interactive=interactive):
            return ui.EXIT_USER_CANCELLED

    # 3. Apply virtual-disc upgrade
    meta = apply_dc_upgrade_to_virtual_discs(game_dir, dc1, dc2, edition=edition)

    # 4. Fast incremental game.dll rebuild (compiles only the updated FIELD.BIN overlay!)
    if not args.skip_dll_rebuild:
        print("  [5/5] Updating games/sh2-covenant/game.dll (incremental FIELD.BIN overlay compile)...")
        cmd = [
            sys.executable,
            "-u",
            os.path.join(SCRIPT_DIR, "sh2_build.py"),
            "--install-dir",
            install_dir,
            "--jobs",
            str(args.jobs),
            "--non-interactive",
            "--skip-app-check",
            "--game-dll-only",
        ]
        res = subprocess.run(cmd, cwd=REPO_ROOT)
        if res.returncode != 0:
            print(ui.paint(f"  [ERROR] game.dll rebuild failed (exit code {res.returncode}).", ui.Style.CRIMSON))
            if args.pause:
                ui.wait_for_keypress("Press any key to close...")
            return res.returncode

    ui.print_banner(
        f"DIRECTOR'S CUT ({edition.upper()}) UPGRADE COMPLETE in {meta['elapsed_sec']}s!",
        "Your saves, trophies, mods, and keys.ini are preserved. Launch Covenant from Shadow Hearts Nocturne.exe!",
    )
    if args.pause:
        ui.wait_for_keypress("Press any key to close...")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nUpgrade cancelled by user.")
        raise SystemExit(ui.EXIT_USER_CANCELLED)
    except Exception as exc:
        print(f"\n[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
