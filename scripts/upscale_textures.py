#!/usr/bin/env python3
"""
Shadow Hearts: Covenant — Local AI Batch Texture Upscaler Helper (Phase 2)

Wraps free, local Vulkan `realesrgan-ncnn-vulkan` (runs on AMD, NVIDIA, and Intel GPUs
with zero PyTorch/CUDA setup) to upscale dumped PS2 textures from `dumps/textures/` into
a drop-in HD texture pack under `mods/textures/<PackName>/`.

Key features:
  - Category-aware model selection:
      ui_2d/    -> realesrgan-x4plus-anime (crisp UI, fonts, Judgement Ring, portraits)
      world_3d/ -> realesrgan-x4plus       (rich 3D character, cloth, stone, world detail)
  - Alpha-safe PS2 pipeline:
      Separates RGB and Alpha, bleeds opaque RGB colors outward into alpha==0 pixels
      to eliminate dark/bright edge fringes on cutouts/hair/foliage, upscales cleanly,
      and recombines with exact power-of-two (2x or 4x) dimensions required by gs.wgsl.
  - Resumable: skips textures already present in the output pack so new chapters can
    be upscaled incrementally in seconds.
  - Zero copyrighted assets or AI model weights are shipped in the repository.

Usage:
  python scripts/upscale_textures.py --input dumps/textures --output mods/textures/SH2_HD_Pack --scale 4
  python scripts/upscale_textures.py --setup        # Download realesrgan-ncnn-vulkan into tools/realesrgan/
  python scripts/upscale_textures.py --self-test    # Run automated pipeline verification
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
import zlib
from pathlib import Path
from typing import Optional, Tuple

try:
    from PIL import Image  # type: ignore
    HAS_PIL = True
except Exception:
    HAS_PIL = False

REALESRGAN_WIN_ZIP_URL = (
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
    "realesrgan-ncnn-vulkan-20220424-windows.zip"
)


# ---- Pure-Python minimal RGBA PNG fallback (used if Pillow is not installed) ----

def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)


def write_png_rgba_pure(path: Path, w: int, h: int, rgba: bytearray) -> None:
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    data = (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + _png_chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)


def read_png_rgba_pure(path: Path) -> Tuple[int, int, bytearray]:
    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Not a PNG file: {path}")
    pos = 8
    w = h = bd = ct = 0
    idat = bytearray()
    while pos + 12 <= len(raw):
        clen = struct.unpack_from(">I", raw, pos)[0]
        ctype = raw[pos + 4 : pos + 8]
        cdata = raw[pos + 8 : pos + 8 + clen]
        pos += 12 + clen
        if ctype == b"IHDR":
            w, h, bd, ct = struct.unpack_from(">IIBB", cdata, 0)
        elif ctype == b"IDAT":
            idat.extend(cdata)
        elif ctype == b"IEND":
            break
    if bd != 8 or ct not in (2, 6):
        raise ValueError(f"Unsupported PNG format (bd={bd}, ct={ct}) in pure fallback; install Pillow for full support")
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    buf = zlib.decompress(bytes(idat))
    recon = bytearray(h * stride)
    for y in range(h):
        ft = buf[y * (1 + stride)]
        src = buf[y * (1 + stride) + 1 : (y + 1) * (1 + stride)]
        dst_off = y * stride
        for i in range(stride):
            a = recon[dst_off + i - bpp] if i >= bpp else 0
            b = recon[dst_off - stride + i] if y > 0 else 0
            c = recon[dst_off - stride + i - bpp] if (y > 0 and i >= bpp) else 0
            if ft == 0:
                pred = 0
            elif ft == 1:
                pred = a
            elif ft == 2:
                pred = b
            elif ft == 3:
                pred = (a + b) >> 1
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                pred = 0
            recon[dst_off + i] = (src[i] + pred) & 0xFF
    if ct == 6:
        return w, h, recon
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        rgba[i * 4 + 0] = recon[i * 3 + 0]
        rgba[i * 4 + 1] = recon[i * 3 + 1]
        rgba[i * 4 + 2] = recon[i * 3 + 2]
        rgba[i * 4 + 3] = 255
    return w, h, rgba


def load_rgba(path: Path) -> Tuple[int, int, bytearray]:
    if HAS_PIL:
        with Image.open(path) as im:
            im = im.convert("RGBA")
            return im.width, im.height, bytearray(im.tobytes())
    return read_png_rgba_pure(path)


def save_rgba(path: Path, w: int, h: int, rgba: bytearray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if HAS_PIL:
        im = Image.frombytes("RGBA", (w, h), bytes(rgba))
        tmp = path.with_suffix(path.suffix + ".tmp")
        im.save(tmp, format="PNG", optimize=True)
        tmp.replace(path)
    else:
        write_png_rgba_pure(path, w, h, rgba)


def bleed_transparent_rgb(w: int, h: int, rgba: bytearray, passes: int = 3) -> bytearray:
    """
    Propagate RGB color from opaque/semi-opaque neighbors into fully transparent (alpha==0) pixels.
    Prevents dark or white fringes when AI upscalers or bilinear filtering sample across cutout borders.
    """
    out = bytearray(rgba)
    valid = bytearray(w * h)
    has_transparent = False
    has_opaque = False
    for i in range(w * h):
        if out[i * 4 + 3] > 0:
            valid[i] = 1
            has_opaque = True
        else:
            has_transparent = True
    if not has_transparent or not has_opaque:
        return out

    neighbors = ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1))
    for _ in range(passes):
        next_valid = bytearray(valid)
        changed = False
        for y in range(h):
            row = y * w
            for x in range(w):
                idx = row + x
                if valid[idx]:
                    continue
                sr = sg = sb = cnt = 0
                for dx, dy in neighbors:
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        nidx = ny * w + nx
                        if valid[nidx]:
                            sr += out[nidx * 4 + 0]
                            sg += out[nidx * 4 + 1]
                            sb += out[nidx * 4 + 2]
                            cnt += 1
                if cnt > 0:
                    out[idx * 4 + 0] = sr // cnt
                    out[idx * 4 + 1] = sg // cnt
                    out[idx * 4 + 2] = sb // cnt
                    next_valid[idx] = 1
                    changed = True
        valid = next_valid
        if not changed:
            break
    return out


def resize_rgba_nearest_or_lanczos(w: int, h: int, rgba: bytearray, dst_w: int, dst_h: int) -> bytearray:
    if w == dst_w and h == dst_h:
        return rgba
    if HAS_PIL:
        im = Image.frombytes("RGBA", (w, h), bytes(rgba))
        im = im.resize((dst_w, dst_h), Image.Resampling.LANCZOS)
        return bytearray(im.tobytes())
    # Pure-Python bilinear fallback
    out = bytearray(dst_w * dst_h * 4)
    sx = w / float(dst_w)
    sy = h / float(dst_h)
    for y in range(dst_h):
        fy = (y + 0.5) * sy - 0.5
        y0 = max(0, min(h - 1, int(fy)))
        y1 = min(h - 1, y0 + 1)
        wy = max(0.0, min(1.0, fy - y0))
        for x in range(dst_w):
            fx = (x + 0.5) * sx - 0.5
            x0 = max(0, min(w - 1, int(fx)))
            x1 = min(w - 1, x0 + 1)
            wx = max(0.0, min(1.0, fx - x0))
            i00 = (y0 * w + x0) * 4
            i10 = (y0 * w + x1) * 4
            i01 = (y1 * w + x0) * 4
            i11 = (y1 * w + x1) * 4
            di = (y * dst_w + x) * 4
            for c in range(4):
                top = rgba[i00 + c] * (1.0 - wx) + rgba[i10 + c] * wx
                bot = rgba[i01 + c] * (1.0 - wx) + rgba[i11 + c] * wx
                out[di + c] = max(0, min(255, int(top * (1.0 - wy) + bot * wy + 0.5)))
    return out


def is_pow2(n: int) -> bool:
    return n > 0 and (n & (n - 1)) == 0


def find_realesrgan_exe(root_dir: Path) -> Optional[Path]:
    exe_name = "realesrgan-ncnn-vulkan.exe" if os.name == "nt" else "realesrgan-ncnn-vulkan"
    candidates = [
        root_dir / "tools" / "realesrgan" / exe_name,
        Path("tools/realesrgan") / exe_name,
    ]
    for c in candidates:
        if c.is_file():
            return c.resolve()
    on_path = shutil.which(exe_name)
    return Path(on_path) if on_path else None


def setup_realesrgan(root_dir: Path) -> Path:
    dest = root_dir / "tools" / "realesrgan"
    exe = find_realesrgan_exe(root_dir)
    if exe:
        print(f"[upscale] Real-ESRGAN Vulkan already installed at: {exe}")
        return exe
    if os.name != "nt":
        raise RuntimeError(
            "Automatic --setup currently targets Windows x64. On Linux/macOS, install "
            "realesrgan-ncnn-vulkan into tools/realesrgan/ or PATH."
        )
    dest.mkdir(parents=True, exist_ok=True)
    zip_path = dest / "realesrgan-ncnn-vulkan-windows.zip"
    print(f"[upscale] Downloading official MIT-licensed realesrgan-ncnn-vulkan to {dest} ...")
    urllib.request.urlretrieve(REALESRGAN_WIN_ZIP_URL, zip_path)
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest)
    zip_path.unlink(missing_ok=True)
    exe = find_realesrgan_exe(root_dir)
    if not exe:
        raise RuntimeError(f"Extraction finished, but realesrgan-ncnn-vulkan.exe was not found under {dest}")
    print(f"[upscale] Installed successfully: {exe}")
    return exe


def upscale_batch_for_model(
    items: list[Tuple[Path, Path]],
    scale: int,
    exe: Path,
    model_name: str,
    batch_size: int = 128,
) -> Tuple[int, int]:
    """
    Upscale a list of (src_png, dst_png) pairs in directory batch mode so Vulkan + 64 MB weights
    initialize ONCE per chunk instead of once per texture (10x-20x faster!).
    Returns (done_count, failed_count).
    """
    done = 0
    failed = 0
    total = len(items)
    for start in range(0, total, batch_size):
        chunk = items[start : start + batch_size]
        with tempfile.TemporaryDirectory() as td:
            tdir = Path(td)
            in_dir = tdir / "in"
            out_dir = tdir / "out"
            in_dir.mkdir(parents=True, exist_ok=True)
            out_dir.mkdir(parents=True, exist_ok=True)

            prepared: list[Tuple[str, Path, int, int, bytearray, bool]] = []
            for idx, (src_png, dst_png) in enumerate(chunk):
                try:
                    w, h, rgba = load_rgba(src_png)
                except Exception as exc:
                    print(f"  [skip] could not decode {src_png.name}: {exc}")
                    failed += 1
                    continue
                if not is_pow2(w) or not is_pow2(h):
                    print(f"  [skip] non-power-of-two texture {src_png.name} ({w}x{h})")
                    failed += 1
                    continue
                bled = bleed_transparent_rgb(w, h, rgba, passes=3)
                rgb_opaque = bytearray(bled)
                has_alpha = False
                for i in range(w * h):
                    if rgba[i * 4 + 3] < 255:
                        has_alpha = True
                    rgb_opaque[i * 4 + 3] = 255
                stem = f"{idx:04d}.png"
                save_rgba(in_dir / stem, w, h, rgb_opaque)
                prepared.append((stem, dst_png, w, h, rgba, has_alpha))

            if not prepared:
                continue

            print(
                f"  [batch] {model_name}: upscaling {len(prepared)} texture(s) "
                f"({start + 1}-{start + len(chunk)} of {total})..."
            )
            cmd = [str(exe), "-i", str(in_dir), "-o", str(out_dir), "-n", model_name, "-s", "4", "-f", "png"]
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if proc.returncode != 0:
                print(f"  [err] Real-ESRGAN batch failed ({model_name}): {proc.stderr.strip()}")
                failed += len(prepared)
                continue

            for stem, dst_png, w, h, orig_rgba, has_alpha in prepared:
                up_path = out_dir / stem
                if not up_path.is_file():
                    print(f"  [err] Missing Real-ESRGAN output for {dst_png.name}")
                    failed += 1
                    continue
                target_w, target_h = w * scale, h * scale
                uw, uh, up_rgba = load_rgba(up_path)
                if uw != target_w or uh != target_h:
                    up_rgba = resize_rgba_nearest_or_lanczos(uw, uh, up_rgba, target_w, target_h)
                if has_alpha:
                    alpha_img = bytearray(w * h * 4)
                    for i in range(w * h):
                        a = orig_rgba[i * 4 + 3]
                        alpha_img[i * 4 + 0] = a
                        alpha_img[i * 4 + 1] = a
                        alpha_img[i * 4 + 2] = a
                        alpha_img[i * 4 + 3] = 255
                    up_alpha = resize_rgba_nearest_or_lanczos(w, h, alpha_img, target_w, target_h)
                    for i in range(target_w * target_h):
                        up_rgba[i * 4 + 3] = up_alpha[i * 4 + 0]
                else:
                    for i in range(target_w * target_h):
                        up_rgba[i * 4 + 3] = 255
                save_rgba(dst_png, target_w, target_h, up_rgba)
                done += 1
    return done, failed


def run_self_test() -> int:
    print("[upscale] Running self-test (alpha bleeding, pure/PIL PNG codec, power-of-two resize)...")
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        w, h = 16, 16
        src = bytearray(w * h * 4)
        # Red opaque square in center (4..11), black transparent border (0..3, 12..15)
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 4
                if 4 <= x < 12 and 4 <= y < 12:
                    src[idx + 0] = 220
                    src[idx + 1] = 40
                    src[idx + 2] = 60
                    src[idx + 3] = 255
                else:
                    src[idx + 0] = 0
                    src[idx + 1] = 0
                    src[idx + 2] = 0
                    src[idx + 3] = 0
        in_p = tdir / "deadbeef01234567_0000000000000000_16x16.png"
        save_rgba(in_p, w, h, src)
        rw, rh, rr = load_rgba(in_p)
        assert (rw, rh) == (16, 16) and rr == src, "PNG round-trip mismatch"

        bled = bleed_transparent_rgb(w, h, rr, passes=2)
        # Pixel (3, 4) was transparent black (0,0,0,0), now must inherit red neighbor (220,40,60) with alpha still 0
        edge = (4 * w + 3) * 4
        assert bled[edge + 0] == 220 and bled[edge + 1] == 40 and bled[edge + 2] == 60 and bled[edge + 3] == 0, (
            f"Alpha bleed failed at edge pixel: {list(bled[edge:edge+4])}"
        )

        up = resize_rgba_nearest_or_lanczos(w, h, bled, 64, 64)
        assert len(up) == 64 * 64 * 4 and is_pow2(64)
        out_p = tdir / "out_64x64.png"
        save_rgba(out_p, 64, 64, up)
        ow, oh, _ = load_rgba(out_p)
        assert (ow, oh) == (64, 64)
    print("[upscale] Self-test PASSED (0 failures).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Covenant — Local AI Batch Texture Upscaler")
    ap.add_argument("--input", default="dumps/textures", help="Input folder of dumped PS2 textures")
    ap.add_argument("--output", default="mods/textures/SH2_HD_Pack", help="Output HD texture pack folder")
    ap.add_argument("--scale", type=int, choices=(2, 4), default=4, help="Target upscale factor (2 or 4)")
    ap.add_argument("--ui-model", default="realesrgan-x4plus-anime", help="Real-ESRGAN model for ui_2d/ textures")
    ap.add_argument("--world-model", default="realesrgan-x4plus", help="Real-ESRGAN model for world_3d/ textures")
    ap.add_argument("--overwrite", action="store_true", help="Re-upscale files that already exist in output")
    ap.add_argument("--setup", action="store_true", help="Download official realesrgan-ncnn-vulkan into tools/realesrgan/")
    ap.add_argument("--auto-setup", action="store_true", help="Automatically download realesrgan-ncnn-vulkan if missing")
    ap.add_argument("--self-test", action="store_true", help="Run built-in verification self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    root_dir = Path(__file__).resolve().parent.parent
    if args.setup:
        setup_realesrgan(root_dir)
        return 0

    exe = find_realesrgan_exe(root_dir)
    if not exe:
        print("[upscale] realesrgan-ncnn-vulkan not found in tools/realesrgan/ or PATH.")
        if args.auto_setup:
            exe = setup_realesrgan(root_dir)
        elif os.name == "nt" and sys.stdin and sys.stdin.isatty():
            try:
                ans = input("[upscale] Download official MIT-licensed realesrgan-ncnn-vulkan now? [Y/n]: ").strip().lower()
            except EOFError:
                ans = "n"
            if ans in ("", "y", "yes"):
                exe = setup_realesrgan(root_dir)
        if not exe:
            print("[upscale] Run:  python scripts/upscale_textures.py --setup")
            print(f"[upscale] Or download manually from: {REALESRGAN_WIN_ZIP_URL}")
            return 1

    in_dir = Path(args.input)
    out_dir = Path(args.output)
    if not in_dir.is_dir():
        print(f"[upscale] Input folder does not exist yet: {in_dir}")
        print("[upscale] Enable 'Runtime GPU Texture Dumping' in the Launcher or F1 Quick Menu, play a bit, then re-run.")
        return 1

    pngs = sorted(p for p in in_dir.rglob("*.png") if p.is_file())
    if not pngs:
        print(f"[upscale] No .png textures found under {in_dir}.")
        return 0

    print(f"[upscale] Found {len(pngs)} texture(s) in {in_dir}")
    print(f"[upscale] Output pack: {out_dir} ({args.scale}x) | Engine: {exe.name} | PIL={'yes' if HAS_PIL else 'fallback'}")

    ui_jobs: list[Tuple[Path, Path]] = []
    world_jobs: list[Tuple[Path, Path]] = []
    skipped = 0
    for src in pngs:
        rel = src.relative_to(in_dir)
        dst = out_dir / rel
        if dst.is_file() and not args.overwrite:
            skipped += 1
            continue
        is_ui = any(part.lower() == "ui_2d" for part in rel.parts)
        if is_ui:
            ui_jobs.append((src, dst))
        else:
            world_jobs.append((src, dst))

    done = failed = 0
    if ui_jobs:
        d, f = upscale_batch_for_model(ui_jobs, args.scale, exe, args.ui_model)
        done += d
        failed += f
    if world_jobs:
        d, f = upscale_batch_for_model(world_jobs, args.scale, exe, args.world_model)
        done += d
        failed += f

    print(f"[upscale] Complete: {done} upscaled, {skipped} already existed, {failed} skipped/failed.")
    print(f"[upscale] Your HD pack is ready in: {out_dir}")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
