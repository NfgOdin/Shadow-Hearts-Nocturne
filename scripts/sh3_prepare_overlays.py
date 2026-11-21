#!/usr/bin/env python3
"""Extract SLUS_213.26 and decompress all three Nautilus 'cx'-packed MWo3 code overlays
(OPENING.CPX, FIELD.CPX, BATTLE.CPX) from Shadow Hearts: From the New World (SLUS-21326),
build deterministic 0x00400000-based overlay images, and seed entries.txt with overlay
entry points and static constructors.
"""
from __future__ import annotations

import binascii
import os
import re
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import disc  # noqa: E402

WINDOW_BASE = 0x00400000
WINDOW_UNIT = 0x4000
OVERLAY_FILES = ("OPENING.CPX", "FIELD.CPX", "BATTLE.CPX")
MANAGED_RE = re.compile(r"^00400000_[0-9A-Fa-f]{8}\.bin$")


def is_unpacked_disc(path: str | None) -> bool:
    return bool(path and os.path.isdir(path) and os.path.isfile(os.path.join(path, "disc_manifest.tsv")))


def _norm_iso_name(name: str) -> str:
    n = name.replace("\\", "/").lstrip("/")
    if ";" in n:
        n = n.split(";", 1)[0]
    return n.upper()


def read_iso_file(img: disc.Image, name: str) -> bytes:
    entry = img.find(name)
    if not entry:
        raise RuntimeError(f"{name} not found in {img.path}")
    _, _, sector, size = entry
    img.file.seek(sector * disc.SECTOR)
    data = img.file.read(size)
    if len(data) != size:
        raise RuntimeError(f"Short read for {name}: got {len(data)} of {size} bytes")
    return data


def read_unpacked_file(folder: str, name: str) -> bytes:
    target = _norm_iso_name(name)
    manifest_path = os.path.join(folder, "disc_manifest.tsv")
    fallback_rel: str | None = None
    with open(manifest_path, "r", encoding="utf-8") as mf:
        for raw_line in mf:
            line = raw_line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) >= 6 and parts[0] == "FILE":
                layer = parts[1]
                iso_rel = _norm_iso_name(parts[4])
                local_rel = parts[5]
                if iso_rel == target:
                    if layer == "0":
                        with open(os.path.join(folder, local_rel), "rb") as f:
                            return f.read()
                    if fallback_rel is None:
                        fallback_rel = local_rel
    if fallback_rel is not None:
        with open(os.path.join(folder, fallback_rel), "rb") as f:
            return f.read()
    layer0 = os.path.join(folder, "layer0")
    if os.path.isdir(layer0):
        for fn in os.listdir(layer0):
            if _norm_iso_name(fn) == target:
                with open(os.path.join(layer0, fn), "rb") as f:
                    return f.read()
    raise RuntimeError(f"{name} not found in unpacked disc folder {folder}")


def decompress_cpx(data: bytes) -> bytes:
    """Decompress a Nautilus 'cx' block-LZSS archive (Shadow Hearts: From the New World).

    Header:
      [0..1]  "cx" magic (0x63, 0x78)
      [2]     header flags (bit 5: +8 bytes; bits 4..2: u16 extra length + payload)
      [3]     block count
      [4..7]  uint32 uncompressed size
    Blocks (repeating until flag bit 7 is set):
      [0]     block flag (bit 7 = last block, bits 6..3 = mode: 0 raw, nonzero LZSS)
      [1]     reserved
      [2..3]  uint16 (compressed payload bytes - 1)
      [4..5]  uint16 (uncompressed bytes - 1)
      [6..]   compressed bitstream (LSB-first: 1 + 8-bit literal, or 0 + 8-bit len-3 + 14-bit dist-1)
    """
    if len(data) < 8 or data[:2] != b"cx":
        raise RuntimeError(f"Expected 'cx' CPX magic, got {data[:4]!r}")
    hdr_flags = data[2]
    total_usz, = struct.unpack_from("<I", data, 4)
    pos = 8
    if hdr_flags & 0x20:
        pos += 8
    if hdr_flags & 0x1C:
        extra, = struct.unpack_from("<H", data, pos)
        pos += 2 + extra

    out = bytearray()
    n = len(data)
    while pos < n:
        if pos + 6 > n:
            raise RuntimeError(f"Truncated CPX block header at offset 0x{pos:X}")
        bflag = data[pos]
        is_last = (bflag & 0x80) != 0
        mode = (bflag >> 3) & 0x0F
        comp_m1, uncomp_m1 = struct.unpack_from("<HH", data, pos + 2)
        comp_len = comp_m1 + 1
        uncomp_len = uncomp_m1 + 1
        s = pos + 6
        if mode == 0:
            # Nautilus 'cx' raw blocks (mode == 0) are XOR-obfuscated per block by
            # SLUS_213.26:0x002DFAB0: byte[i] ^= (((i + 1) * 0x0B) ^ 0xDD) & 0xFF.
            raw = data[s: s + uncomp_len]
            out.extend(b ^ ((((i + 1) * 0x0B) ^ 0xDD) & 0xFF) for i, b in enumerate(raw))
        else:
            bitbuf = 0
            nbits = 0
            rem = uncomp_len
            i = s
            while rem > 0:
                if nbits == 0:
                    bitbuf = data[i]
                    i += 1
                    nbits = 8
                flag = bitbuf & 1
                bitbuf >>= 1
                nbits -= 1
                if flag:
                    while nbits < 8:
                        bitbuf |= data[i] << nbits
                        i += 1
                        nbits += 8
                    out.append(bitbuf & 0xFF)
                    bitbuf >>= 8
                    nbits -= 8
                    rem -= 1
                else:
                    while nbits < 22:
                        bitbuf |= data[i] << nbits
                        i += 1
                        nbits += 8
                    v = bitbuf & 0x3FFFFF
                    bitbuf >>= 22
                    nbits -= 22
                    length = (v & 0xFF) + 3
                    dist = (v >> 8) + 1
                    rem -= length
                    start = len(out) - dist
                    for k in range(length):
                        out.append(out[start + k])
        pos += 6 + comp_len
        if is_last:
            break

    if len(out) != total_usz:
        raise RuntimeError(f"CPX decompressed size mismatch: got {len(out)}, expected {total_usz}")
    return bytes(out)


def find_main_elf_ctors(elf: bytes) -> list[int]:
    """Read Metrowerks __init_start..__init_end static constructor table (0x003C2A70..0x003C2AF0) from SLUS_213.26."""
    phoff, = struct.unpack_from("<I", elf, 28)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if p_type == 1 and p_filesz > 0:
            segs.append((p_vaddr, p_off, p_filesz))

    def read_u32(addr: int) -> int | None:
        for vaddr, off, fsz in segs:
            if vaddr <= addr and addr + 4 <= vaddr + fsz:
                return struct.unpack_from("<I", elf, off + (addr - vaddr))[0]
        return None

    out: list[int] = []
    for a in range(0x003C2A70, 0x003C2AF0, 4):
        fn = read_u32(a)
        if fn and 0x00200000 <= fn < 0x003C8400 and (fn & 3) == 0:
            out.append(fn)
    return out


def prepare(game_dir: str, disc_path: str) -> None:
    disc_out = os.path.join(game_dir, "disc")
    overlays_dir = os.path.join(game_dir, "overlays")
    entries_path = os.path.join(game_dir, "entries.txt")
    os.makedirs(disc_out, exist_ok=True)
    os.makedirs(overlays_dir, exist_ok=True)

    if is_unpacked_disc(disc_path):
        read_file = lambda name: read_unpacked_file(disc_path, name)
    else:
        img = disc.Image(disc_path)
        read_file = lambda name: read_iso_file(img, name)

    sys_cnf = read_file("SYSTEM.CNF")
    with open(os.path.join(disc_out, "SYSTEM.CNF"), "wb") as f:
        f.write(sys_cnf)

    m = re.search(rb"BOOT2\s*=\s*cdrom0:[\\/]*([^;\s]+)", sys_cnf, re.IGNORECASE)
    if not m:
        raise RuntimeError("Could not parse BOOT2 executable from SYSTEM.CNF")
    boot_name = m.group(1).decode("ascii").upper()
    if boot_name != "SLUS_213.26":
        raise RuntimeError(
            f"Unsupported disc region/title ({boot_name}). Expected Shadow Hearts: From the New World (USA, SLUS_213.26)."
        )
    elf_bytes = read_file(boot_name)
    elf_path = os.path.join(disc_out, boot_name)
    with open(elf_path, "wb") as f:
        f.write(elf_bytes)
    print(f"[prepare] Extracted {boot_name} ({len(elf_bytes):,} bytes)")

    # Remove old managed overlays at 0x00400000 so no stale captures remain
    for fn in os.listdir(overlays_dir):
        if MANAGED_RE.match(fn):
            os.remove(os.path.join(overlays_dir, fn))

    overlay_entries: list[str] = []
    overlay_names: set[str] = set()

    for ov_name in OVERLAY_FILES:
        packed = read_file(ov_name)
        raw = decompress_cpx(packed)
        magic, ov_id, vaddr, text_sz, data_sz, bss_sz, init_s, init_e = struct.unpack_from("<8I", raw, 0)
        if magic != 0x336F574D:
            raise RuntimeError(f"{ov_name}: expected MWo3 header after CPX decompression, got 0x{magic:08X}")
        if vaddr < WINDOW_BASE:
            raise RuntimeError(f"{ov_name}: unexpected vaddr 0x{vaddr:08X} < 0x{WINDOW_BASE:08X}")
        pad_head = vaddr - WINDOW_BASE
        total = ((pad_head + len(raw) + WINDOW_UNIT - 1) // WINDOW_UNIT) * WINDOW_UNIT
        blob = bytearray(total)
        blob[pad_head: pad_head + len(raw)] = raw
        crc = binascii.crc32(blob) & 0xFFFFFFFF
        tag = f"{WINDOW_BASE:08X}_{crc:08X}"
        overlay_names.add(tag)
        out_path = os.path.join(overlays_dir, f"{tag}.bin")
        with open(out_path, "wb") as f:
            f.write(blob)

        # Seed first function after MWo3 header + all .init static constructors
        first_fn = None
        for off in range(0x40, min(0x400, len(raw)), 4):
            w = struct.unpack_from("<I", raw, off)[0]
            if (w >> 16) == 0x27BD:
                first_fn = vaddr + off
                break
        if first_fn:
            overlay_entries.append(f"{tag}:{first_fn - WINDOW_BASE:x}")

        ctors = []
        if init_e > init_s:
            for a in range(init_s, init_e, 4):
                off = a - vaddr
                if 0 <= off <= len(raw) - 4:
                    fn = struct.unpack_from("<I", raw, off)[0]
                    if vaddr <= fn < vaddr + len(raw):
                        ctors.append(fn)
                        overlay_entries.append(f"{tag}:{fn - WINDOW_BASE:x}")

        print(
            f"[prepare] {ov_name:11s} -> {tag}.bin "
            f"({len(blob) // 1024:4d} KiB, .text={text_sz // 1024:4d} KiB, ctors={len(ctors)})"
        )

    # Seed main-ELF static ctors + preserve any previously discovered main-ELF or live overlay entry tokens
    main_entries: set[int] = set(find_main_elf_ctors(elf_bytes))
    other_lines: list[str] = []
    if os.path.isfile(entries_path):
        for tok in open(entries_path, "r", encoding="utf-8", errors="ignore").read().split():
            if ":" in tok:
                mod, _ = tok.split(":", 1)
                if mod in overlay_names:
                    overlay_entries.append(tok)
                elif not MANAGED_RE.match(f"{mod}.bin"):
                    other_lines.append(tok)
            else:
                try:
                    main_entries.add(int(tok, 16))
                except ValueError:
                    pass

    seen_ov: set[str] = set()
    unique_ov: list[str] = []
    for tok in overlay_entries:
        if tok not in seen_ov:
            seen_ov.add(tok)
            unique_ov.append(tok)

    lines = [f"{a:x}" for a in sorted(main_entries)] + unique_ov + sorted(set(other_lines))
    with open(entries_path, "w", encoding="utf-8", newline="\n") as f:
        for ln in lines:
            f.write(ln + "\n")
    print(f"[prepare] Updated {entries_path} ({len(main_entries)} main entries, {len(unique_ov)} overlay entries)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <game_dir> <disc_path>", file=sys.stderr)
        sys.exit(2)
    prepare(sys.argv[1], sys.argv[2])
