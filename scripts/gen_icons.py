#!/usr/bin/env python3
"""Generate crisp multi-resolution Gothic Windows .ico icons (PNG-in-ICO) for:
  - assets/icons/nocturne_launcher.ico (Shadow Hearts Nocturne.exe — Gilded Judgment Ring)
  - assets/icons/covenant_game.ico     (Shadow Hearts Covenant.exe — Crimson Eclipse Seal)
Uses only Python stdlib (math, struct, zlib).
"""
import math
import os
import pathlib
import struct
import zlib


def clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return lo if v < lo else (hi if v > hi else v)


def smoothstep(e0: float, e1: float, x: float) -> float:
    t = clamp((x - e0) / (e1 - e0)) if e1 != e0 else (1.0 if x >= e0 else 0.0)
    return t * t * (3.0 - 2.0 * t)


def blend(dst, src):
    dr, dg, db, da = dst
    sr, sg, sb, sa = src
    if sa <= 0.0:
        return dst
    out_a = sa + da * (1.0 - sa)
    if out_a <= 1e-6:
        return (0.0, 0.0, 0.0, 0.0)
    inv = 1.0 - sa
    return (
        (sr * sa + dr * da * inv) / out_a,
        (sg * sa + dg * da * inv) / out_a,
        (sb * sa + db * da * inv) / out_a,
        out_a,
    )


def angle_in_arc(ang: float, a0: float, a1: float) -> float:
    """Distance outside [a0, a1] in radians (0 inside)."""
    two_pi = 2.0 * math.pi
    a = ang % two_pi
    lo = a0 % two_pi
    hi = a1 % two_pi
    if lo <= hi:
        if lo <= a <= hi:
            return 0.0
        return min(abs(a - lo), abs(a - hi), two_pi - abs(a - lo), two_pi - abs(a - hi))
    if a >= lo or a <= hi:
        return 0.0
    return min(abs(a - lo), abs(a - hi))


def sample_launcher_icon(nx: float, ny: float):
    """Gilded Brass & Crimson Judgment Ring Crest (normalized coords in [-1, 1])."""
    r = math.hypot(nx, ny)
    if r > 1.0:
        return (0.0, 0.0, 0.0, 0.0)
    ang = math.atan2(ny, nx)
    c = (0.0, 0.0, 0.0, 0.0)

    # Soft outer rim AA
    disc_a = 1.0 - smoothstep(0.95, 0.995, r)
    # Deep obsidian-indigo radial base
    bg_t = clamp(r / 0.96)
    bg_r = 0.08 + 0.06 * (1.0 - bg_t)
    bg_g = 0.06 + 0.04 * (1.0 - bg_t)
    bg_b = 0.14 + 0.08 * (1.0 - bg_t)
    # Subtle warm brass glow near center
    glow = math.exp(-r * r * 5.0)
    c = blend(c, (bg_r + 0.18 * glow, bg_g + 0.08 * glow, bg_b + 0.04 * glow, disc_a))

    # Outer gilded brass ring (r in [0.80, 0.94])
    outer_ring = smoothstep(0.79, 0.82, r) * (1.0 - smoothstep(0.92, 0.955, r))
    if outer_ring > 0.0:
        sheen = 0.65 + 0.35 * math.cos(ang + 0.8)
        c = blend(c, (0.94 * sheen, 0.74 * sheen, 0.34 * sheen, outer_ring))

    # Recessed dark Judgment Ring track (r in [0.54, 0.78])
    track = smoothstep(0.52, 0.55, r) * (1.0 - smoothstep(0.76, 0.79, r))
    if track > 0.0:
        c = blend(c, (0.05, 0.04, 0.08, track * 0.92))

    # Glowing Crimson Hit Zone & Gold Perfect Strike Zones on the Judgment Ring!
    if track > 0.0:
        for (a0, a1, col) in [
            (-0.65, 0.30, (0.76, 0.16, 0.24)),    # Wide Crimson Hit Zone
            (0.14, 0.30, (1.00, 0.82, 0.30)),     # Narrow Golden Perfect Strike Zone
            (1.60, 2.30, (0.76, 0.16, 0.24)),     # Second Hit Zone
            (2.16, 2.30, (1.00, 0.82, 0.30)),     # Second Perfect Zone
            (3.75, 4.35, (0.68, 0.14, 0.24)),     # Third Hit Zone
            (4.22, 4.35, (1.00, 0.82, 0.30)),     # Third Perfect Zone
        ]:
            d = angle_in_arc(ang, a0, a1)
            if d < 0.06:
                arc_a = (1.0 - smoothstep(0.0, 0.05, d)) * track
                c = blend(c, (col[0], col[1], col[2], arc_a * 0.95))

    # Inner brass bezel ring (r in [0.47, 0.54])
    inner_ring = smoothstep(0.46, 0.49, r) * (1.0 - smoothstep(0.52, 0.55, r))
    if inner_ring > 0.0:
        c = blend(c, (0.90, 0.72, 0.36, inner_ring))

    # 8 radial clockwork compass ticks
    for k in range(8):
        ka = k * math.pi * 0.25
        d_ang = abs(((ang - ka + math.pi) % (2 * math.pi)) - math.pi)
        perp = r * d_ang
        if 0.76 <= r <= 0.94 and perp < 0.032:
            ta = 1.0 - smoothstep(0.014, 0.032, perp)
            c = blend(c, (1.0, 0.90, 0.62, ta * 0.9))

    # Rotating Judgment Needle pointing toward upper-right Strike Zone (-0.35 rad)
    needle_ang = -0.22
    ca, sa_n = math.cos(needle_ang), math.sin(needle_ang)
    along = nx * ca + ny * sa_n
    across = abs(-nx * sa_n + ny * ca)
    if -0.18 <= along <= 0.86:
        half_w = 0.042 * (1.0 - max(0.0, along) * 0.55)
        if across < half_w + 0.025:
            na = 1.0 - smoothstep(half_w * 0.6, half_w + 0.025, across)
            c = blend(c, (1.0, 0.95, 0.82, na))

    # Central crimson-gold faceted diamond jewel
    dm = abs(nx) + abs(ny)
    if dm < 0.30:
        ja = 1.0 - smoothstep(0.25, 0.30, dm)
        c = blend(c, (0.95, 0.75, 0.32, ja))
    if dm < 0.18:
        ja2 = 1.0 - smoothstep(0.14, 0.18, dm)
        c = blend(c, (0.85, 0.16, 0.28, ja2))
    if dm < 0.08:
        ja3 = 1.0 - smoothstep(0.05, 0.08, dm)
        c = blend(c, (1.0, 0.94, 0.78, ja3))

    return c


def sample_game_icon(nx: float, ny: float):
    """Crimson Eclipse & 8-Ray Golden Covenant Seal (normalized coords in [-1, 1])."""
    r = math.hypot(nx, ny)
    if r > 1.0:
        return (0.0, 0.0, 0.0, 0.0)
    ang = math.atan2(ny, nx)
    c = (0.0, 0.0, 0.0, 0.0)

    disc_a = 1.0 - smoothstep(0.95, 0.995, r)
    # Deep nocturnal crimson-plum backdrop
    t = clamp(r / 0.96)
    c = blend(c, (0.26 * (1 - t) + 0.07 * t, 0.05 * (1 - t) + 0.03 * t, 0.14 * (1 - t) + 0.10 * t, disc_a))

    # Outer ornate metallic gold border
    ring = smoothstep(0.84, 0.875, r) * (1.0 - smoothstep(0.93, 0.965, r))
    if ring > 0.0:
        sh = 0.70 + 0.30 * math.cos(ang - 0.7)
        c = blend(c, (0.98 * sh, 0.80 * sh, 0.38 * sh, ring))

    # Crimson lunar eclipse halo
    halo = math.exp(-((r - 0.50) ** 2) * 32.0)
    c = blend(c, (0.94, 0.22, 0.32, halo * 0.85 * disc_a))

    # Dark eclipsed moon offset slightly to create a luminous crimson-gold crescent
    moon_r = math.hypot(nx - 0.12, ny + 0.10)
    crescent_outer = 1.0 - smoothstep(0.52, 0.56, r)
    crescent_cut = smoothstep(0.42, 0.46, moon_r)
    cres = crescent_outer * crescent_cut
    if cres > 0.0:
        c = blend(c, (0.98, 0.78, 0.36, cres * 0.95))

    # 8-ray Covenant Star centered inside the crest
    for k in range(8):
        ka = k * math.pi * 0.25
        ca, sa_k = math.cos(ka), math.sin(ka)
        along = nx * ca + ny * sa_k
        across = abs(-nx * sa_k + ny * ca)
        max_len = 0.76 if (k % 2 == 0) else 0.52
        if 0.0 <= along <= max_len:
            w = (0.065 if k % 2 == 0 else 0.042) * (1.0 - along / max_len)
            if across < w + 0.02:
                ra = 1.0 - smoothstep(w * 0.4, w + 0.02, across)
                c = blend(c, (1.0, 0.90, 0.58, ra * 0.96))

    # Bright white-gold core
    core = 1.0 - smoothstep(0.11, 0.18, r)
    if core > 0.0:
        c = blend(c, (1.0, 0.97, 0.86, core))
    return c


def render_png(size: int, sampler) -> bytes:
    ss = 2  # 2x2 supersampling for silky anti-aliasing
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # PNG filter None
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    nx = ((x + (sx + 0.5) / ss) / size) * 2.0 - 1.0
                    ny = ((y + (sy + 0.5) / ss) / size) * 2.0 - 1.0
                    sr, sg, sb, sa = sampler(nx, ny)
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            a /= (ss * ss)
            if a > 1e-6:
                r = clamp(r / (ss * ss * a))
                g = clamp(g / (ss * ss * a))
                b = clamp(b / (ss * ss * a))
            else:
                r = g = b = 0.0
            rows.extend((
                int(r * 255 + 0.5),
                int(g * 255 + 0.5),
                int(b * 255 + 0.5),
                int(clamp(a) * 255 + 0.5),
            ))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(bytes(rows), 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def write_ico(path: pathlib.Path, sampler, sizes=(16, 32, 48, 256)) -> None:
    pngs = [render_png(s, sampler) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = bytearray()
    for s, data in zip(sizes, pngs):
        dim = 0 if s >= 256 else s
        entries.extend(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes(entries) + b"".join(pngs))


def main() -> None:
    root = pathlib.Path(__file__).resolve().parent.parent
    out_dir = root / "assets" / "icons"
    write_ico(out_dir / "nocturne_launcher.ico", sample_launcher_icon)
    write_ico(out_dir / "covenant_game.ico", sample_game_icon)
    print(f"Generated {out_dir / 'nocturne_launcher.ico'} and {out_dir / 'covenant_game.ico'}")


if __name__ == "__main__":
    main()
