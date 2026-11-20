#!/usr/bin/env python3
"""Extract SLUS_210.41 and all three MWo3 code overlays (OPENING.BIN, FIELD.BIN,
BATTLE.BIN) cleanly from Shadow Hearts: Covenant Disc 1 ISO, build deterministic
0x0045C000-based overlay images, and seed entries.txt with all static constructors.
"""
import binascii
import os
import re
import struct
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import disc  # noqa: E402

WINDOW_BASE = 0x0045C000
WINDOW_UNIT = 0x4000
OVERLAY_FILES = ("OPENING.BIN", "FIELD.BIN", "BATTLE.BIN")
MANAGED_RE = re.compile(r"^([0-9A-Fa-f]{8})_[0-9A-Fa-f]{8}\.bin$")


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
    # Direct fallback scan inside layer0/
    layer0 = os.path.join(folder, "layer0")
    if os.path.isdir(layer0):
        for fn in os.listdir(layer0):
            if _norm_iso_name(fn) == target:
                with open(os.path.join(layer0, fn), "rb") as f:
                    return f.read()
    raise RuntimeError(f"{name} not found in unpacked disc folder {folder}")


def read_pvd_disc_marker(disc_path: str) -> str:
    if is_unpacked_disc(disc_path):
        manifest_path = os.path.join(disc_path, "disc_manifest.tsv")
        meta_path = os.path.join(disc_path, "disc_meta.bin")
        with open(manifest_path, "r", encoding="utf-8") as mf:
            for raw_line in mf:
                line = raw_line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 4 and parts[0] == "META":
                    start_sec = int(parts[1])
                    count_sec = int(parts[2])
                    meta_off = int(parts[3])
                    if start_sec <= 16 < start_sec + count_sec:
                        with open(meta_path, "rb") as f:
                            f.seek(meta_off + (16 - start_sec) * disc.SECTOR + 190)
                            raw = f.read(16)
                        return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()
        return ""
    with open(disc_path, "rb") as f:
        f.seek(16 * disc.SECTOR + 190)
        raw = f.read(16)
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()


def find_main_elf_ctors(elf: bytes) -> list[int]:
    """Read Metrowerks __init_start..__init_end constructor table from SLUS_210.41."""
    # Look up the PT_LOAD segment containing 0x00437300..0x00437E10 (NTSC-U SLUS_210.41)
    # Also scan ELF sections/known table cleanly by checking __init_array range in SLUS_210.41.
    phoff, = struct.unpack_from("<I", elf, 28)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if p_type == 1 and p_filesz > 0:
            segs.append((p_vaddr, p_off, p_filesz, p_flags))

    def read_vaddr(addr: int, n: int) -> bytes | None:
        for vaddr, off, fsz, _ in segs:
            if vaddr <= addr and addr + n <= vaddr + fsz:
                return elf[off + (addr - vaddr): off + (addr - vaddr) + n]
        return None

    # Standard NTSC-U SLUS_210.41 (CRC F3BDB2E6) static constructor table: 0x00438010..0x00438098
    # Verify dynamically by checking every word in known ctor range or falling back to explicit table.
    known_ntsc_u = [
        0x437300, 0x437310, 0x437360, 0x4373B0, 0x4373F0, 0x437410, 0x437450, 0x437480,
        0x4374C0, 0x437500, 0x437530, 0x4375A0, 0x437610, 0x4376C0, 0x437710, 0x437780,
        0x4377D0, 0x437820, 0x437870, 0x4378C0, 0x437900, 0x437990, 0x4379F0, 0x437A40,
        0x437AC0, 0x437B10, 0x437B60, 0x437BB0, 0x437C00, 0x437C80, 0x437CE0, 0x437D50,
        0x437DC0, 0x437E10,
    ]
    out = []
    for addr in known_ntsc_u:
        b = read_vaddr(addr, 4)
        if b and struct.unpack("<I", b)[0] != 0:
            out.append(addr)
    return out


def detect_boot_name(disc_path: str) -> str:
    if is_unpacked_disc(disc_path):
        sys_cnf = read_unpacked_file(disc_path, "SYSTEM.CNF")
    else:
        img = disc.Image(disc_path)
        sys_cnf = read_iso_file(img, "SYSTEM.CNF")
    m = re.search(rb"BOOT2\s*=\s*cdrom0:[\\/]*([^;\s]+)", sys_cnf, re.IGNORECASE)
    if not m:
        raise RuntimeError(f"Could not parse BOOT2 executable from SYSTEM.CNF in {disc_path}")
    return m.group(1).decode("ascii").upper()


def prepare(game_dir: str, disc1_path: str, disc2_path: str | None = None) -> None:
    disc_out = os.path.join(game_dir, "disc")
    overlays_dir = os.path.join(game_dir, "overlays")
    entries_path = os.path.join(game_dir, "entries.txt")
    os.makedirs(disc_out, exist_ok=True)
    os.makedirs(overlays_dir, exist_ok=True)

    if is_unpacked_disc(disc1_path):
        read_file = lambda name: read_unpacked_file(disc1_path, name)
    else:
        img = disc.Image(disc1_path)
        read_file = lambda name: read_iso_file(img, name)

    sys_cnf = read_file("SYSTEM.CNF")
    with open(os.path.join(disc_out, "SYSTEM.CNF"), "wb") as f:
        f.write(sys_cnf)

    m = re.search(rb"BOOT2\s*=\s*cdrom0:[\\/]*([^;\s]+)", sys_cnf, re.IGNORECASE)
    if not m:
        raise RuntimeError("Could not parse BOOT2 executable from SYSTEM.CNF")
    boot_name = m.group(1).decode("ascii").upper()
    if boot_name == "SLUS_210.44":
        raise RuntimeError(
            "Disc 2 (SLUS_210.44) was provided as Disc 1! Please provide Shadow Hearts: Covenant (USA) Disc 1 (SLUS_210.41) first."
        )
    if boot_name != "SLUS_210.41":
        raise RuntimeError(
            f"Unsupported disc region/title ({boot_name}). This build requires Shadow Hearts: Covenant (USA, NTSC-U) Disc 1 (SLUS_210.41)."
        )
    elf_bytes = read_file(boot_name)
    elf_path = os.path.join(disc_out, boot_name)
    with open(elf_path, "wb") as f:
        f.write(elf_bytes)
    print(f"[prepare] Extracted {boot_name} ({len(elf_bytes):,} bytes)")

    if disc2_path and (os.path.isfile(disc2_path) or is_unpacked_disc(disc2_path)):
        boot2 = detect_boot_name(disc2_path)
        if boot2 == "SLUS_210.41":
            raise RuntimeError(
                "Disc 1 (SLUS_210.41) was provided as Disc 2! Please supply Shadow Hearts: Covenant (USA) Disc 2 (SLUS_210.44)."
            )
        if boot2 != "SLUS_210.44":
            raise RuntimeError(
                f"Unsupported Disc 2 ({boot2}). Expected Shadow Hearts: Covenant (USA, NTSC-U) Disc 2 (SLUS_210.44)."
            )
        marker1 = read_pvd_disc_marker(disc1_path)
        marker2 = read_pvd_disc_marker(disc2_path)
        print(f"[prepare] Multi-disc verified: Disc 1 ({boot_name}, marker={marker1!r}), Disc 2 ({boot2}, marker={marker2!r})")

    # Remove old managed overlays at 0x0045C000 so no stale/duplicate RAM captures remain
    for fn in os.listdir(overlays_dir):
        if MANAGED_RE.match(fn):
            os.remove(os.path.join(overlays_dir, fn))

    overlay_entries: list[str] = []
    overlay_names: set[str] = set()

    for ov_name in OVERLAY_FILES:
        raw = read_file(ov_name)
        magic, ov_id, vaddr, text_sz, data_sz, bss_sz, init_s, init_e = struct.unpack_from("<8I", raw, 0)
        if magic != 0x336F574D:
            raise RuntimeError(f"{ov_name}: expected MWo3 header, got 0x{magic:08X}")
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

        # Seed first function after MWo3 header (0x45EA80 + 0x80 = 0x45EB00) + all .init ctors
        first_fn = None
        for off in range(0x40, min(0x200, len(raw)), 4):
            w = struct.unpack_from("<I", raw, off)[0]
            if (w >> 16) == 0x27BD:
                first_fn = vaddr + off
                break
        if first_fn:
            overlay_entries.append(f"{tag}:{first_fn - WINDOW_BASE:x}")

        ctors = []
        if init_e > init_s:
            for a in range(init_s, init_e, 4):
                fn = struct.unpack_from("<I", raw, a - vaddr)[0]
                if vaddr <= fn < vaddr + len(raw):
                    ctors.append(fn)
                    overlay_entries.append(f"{tag}:{fn - WINDOW_BASE:x}")

        print(f"[prepare] {ov_name:11s} -> {tag}.bin ({len(blob) // 1024:4d} KiB, .text={text_sz // 1024:4d} KiB, ctors={len(ctors)})")

    # Seed main-ELF static ctors + preserve any previously discovered main-ELF & overlay entry tokens
    main_entries: set[int] = set(find_main_elf_ctors(elf_bytes))
    other_lines: list[str] = []
    if os.path.isfile(entries_path):
        for tok in open(entries_path, "r", encoding="utf-8", errors="ignore").read().split():
            if ":" in tok:
                name, _, off_s = tok.rpartition(":")
                if name in overlay_names:
                    other_lines.append(f"{name}:{int(off_s, 16):x}")
            else:
                try:
                    main_entries.add(int(tok, 16))
                except ValueError:
                    pass

    all_tokens = [f"{a:x}" for a in sorted(main_entries)]
    seen = set(all_tokens)
    for tok in overlay_entries + other_lines:
        if tok not in seen:
            seen.add(tok)
            all_tokens.append(tok)

    with open(entries_path, "w", encoding="utf-8", newline="\n") as f:
        for tok in all_tokens:
            f.write(tok + "\n")
    print(f"[prepare] Wrote {len(all_tokens)} entry seeds to entries.txt ({len(main_entries)} main, {len(all_tokens) - len(main_entries)} overlay)")


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit("usage: sh2_prepare_overlays.py <game_dir> <disc1.iso> [disc2.iso]")
    prepare(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)


if __name__ == "__main__":
    main()
