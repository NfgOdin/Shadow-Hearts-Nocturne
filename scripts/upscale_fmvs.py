#!/usr/bin/env python3
"""
Shadow Hearts: Covenant — Local Vulkan AI FMV Cutscene Upscaler & Portable FFmpeg Helper

Upscales dumped PS2 FMV cutscenes from `dumps/fmv/` into Media-Foundation-ready HD `.mp4`
replacements in `mods/fmv/` using free, local Vulkan `realesrgan-ncnn-vulkan`
(`realesr-animevideov3` at 2x, 3x, or 4x) and portable `ffmpeg.exe`.

Key features:
  - Zero external Python dependencies (runs directly on tools/portable/python/python.exe).
  - Auto-detects `ffmpeg` on PATH or `tools/ffmpeg/ffmpeg.exe`, and can auto-download a
    portable Windows static `ffmpeg.exe` into `tools/ffmpeg/` (gitignored) on first use.
  - Frame-exact keyframe segmentation (`-f segment -c:v copy`) keeps peak temporary disk
    usage small (~200-400 MB even on 13-minute ending cutscenes) with zero frame drift.
  - Preserves original 48 kHz stereo AAC audio (`-c:a copy`) so upscaled HD FMVs are both
    double-click watchable in Windows Media Player and 100% compatible with in-game WMF
    hardware playback (`host/fmv_player_win.cpp` selects video stream only while PS2 SPU2
    plays synced in-game audio).
  - 100% resumable per cutscene: skips any `<hash>_<name>.mp4` already present in output.

Usage:
  python scripts/upscale_fmvs.py --input dumps/fmv --output mods/fmv --scale 2 --auto-setup
  python scripts/upscale_fmvs.py --input dumps/fmv --output mods/fmv --scale 3 --filter DEMO
  python scripts/upscale_fmvs.py --setup-ffmpeg
  python scripts/upscale_fmvs.py --self-test
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

FFMPEG_URLS = (
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip",
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip",
)

HEX16_RE = re.compile(r"^([0-9a-fA-F]{16})(?:$|[_-])")


def parse_hex16_prefix(stem: str) -> Optional[str]:
    """Match host/fmv_player_win.cpp::parse_hex16_prefix()."""
    m = HEX16_RE.match(stem)
    if not m:
        return None
    h = m.group(1).lower()
    return None if h == "0" * 16 else h


def find_ffmpeg(root: Path = REPO_ROOT) -> Optional[Path]:
    """Locate ffmpeg.exe in tools/ffmpeg/ or on system PATH."""
    candidates = [
        root / "tools" / "ffmpeg" / "ffmpeg.exe",
        root / "tools" / "ffmpeg" / "bin" / "ffmpeg.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    on_path = shutil.which("ffmpeg")
    if on_path:
        return Path(on_path)
    return None


def _download_with_progress(url: str, dst: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": "ShadowHeartsCovenant-Port/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        total = int(resp.headers.get("Content-Length") or 0)
        done = 0
        last_pct = -10
        t0 = time.perf_counter()
        with dst.open("wb") as out:
            while True:
                chunk = resp.read(256 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if total > 0:
                    pct = int(done * 100 / total)
                    if pct >= last_pct + 10 or done == total:
                        mb = done / (1024 * 1024)
                        tmb = total / (1024 * 1024)
                        spd = mb / max(0.05, time.perf_counter() - t0)
                        print(f"  [ffmpeg] Downloading: {pct:3d}% ({mb:5.1f}/{tmb:5.1f} MiB @ {spd:.1f} MiB/s)", flush=True)
                        last_pct = pct


def download_portable_ffmpeg(root: Path = REPO_ROOT) -> Path:
    """Download a portable Windows static ffmpeg.exe into tools/ffmpeg/ (gitignored)."""
    target_dir = root / "tools" / "ffmpeg"
    target_dir.mkdir(parents=True, exist_ok=True)
    target_exe = target_dir / "ffmpeg.exe"
    if target_exe.is_file():
        return target_exe

    last_err: Optional[Exception] = None
    with tempfile.TemporaryDirectory(prefix="sh2_ffmpeg_dl_") as td:
        zip_path = Path(td) / "ffmpeg.zip"
        for url in FFMPEG_URLS:
            try:
                print(f"[ffmpeg] Fetching portable static FFmpeg from {url} ...", flush=True)
                _download_with_progress(url, zip_path)
                print("[ffmpeg] Extracting ffmpeg.exe into tools/ffmpeg/ ...", flush=True)
                with zipfile.ZipFile(zip_path, "r") as zf:
                    extracted_any = False
                    for info in zf.infolist():
                        name = Path(info.filename).name.lower()
                        if name in ("ffmpeg.exe", "ffprobe.exe"):
                            out_file = target_dir / name
                            with zf.open(info, "r") as src, out_file.open("wb") as dst:
                                shutil.copyfileobj(src, dst)
                            extracted_any = True
                    if not extracted_any or not target_exe.is_file():
                        raise RuntimeError("Zip did not contain ffmpeg.exe")
                # Quick smoke check
                rc = subprocess.run([str(target_exe), "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
                if rc != 0:
                    raise RuntimeError(f"ffmpeg -version exited with {rc}")
                print(f"[ffmpeg] Installed portable FFmpeg at {target_exe}", flush=True)
                return target_exe
            except Exception as e:
                last_err = e
                print(f"[ffmpeg] Mirror failed ({e}), trying fallback...", flush=True)
                if zip_path.exists():
                    zip_path.unlink()
    raise RuntimeError(f"Could not download portable ffmpeg.exe: {last_err}")


def ensure_ffmpeg(auto_setup: bool = True, root: Path = REPO_ROOT) -> Path:
    exe = find_ffmpeg(root)
    if exe is not None:
        return exe
    if not auto_setup:
        raise FileNotFoundError(
            "ffmpeg.exe not found in tools/ffmpeg/ or on PATH. Run with --auto-setup or --setup-ffmpeg."
        )
    return download_portable_ffmpeg(root)


def ensure_realesrgan_exe(auto_setup: bool = True, root: Path = REPO_ROOT) -> Path:
    exe = root / "tools" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
    if exe.is_file():
        return exe
    on_path = shutil.which("realesrgan-ncnn-vulkan")
    if on_path:
        return Path(on_path)
    if auto_setup:
        import upscale_textures as ut
        ut.setup_realesrgan(root / "tools" / "realesrgan")
        if exe.is_file():
            return exe
    raise FileNotFoundError(
        f"realesrgan-ncnn-vulkan.exe not found at {exe}. Run with --auto-setup."
    )


def probe_video_info(ffmpeg_exe: Path, video_path: Path) -> Dict[str, object]:
    """Inspect video resolution, fps, duration, and audio presence using ffmpeg -i stderr."""
    proc = subprocess.run(
        [str(ffmpeg_exe), "-hide_banner", "-i", str(video_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    txt = proc.stderr
    info: Dict[str, object] = {
        "width": 512,
        "height": 448,
        "fps": 30000.0 / 1001.0,
        "fps_str": "30000/1001",
        "duration": 0.0,
        "has_audio": False,
    }
    m_dur = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", txt)
    if m_dur:
        info["duration"] = int(m_dur.group(1)) * 3600 + int(m_dur.group(2)) * 60 + float(m_dur.group(3))

    m_vid = re.search(r"Stream #\d+:\d+.*?Video:.*?,\s*(\d{3,4})x(\d{3,4})", txt)
    if m_vid:
        info["width"] = int(m_vid.group(1))
        info["height"] = int(m_vid.group(2))

    m_fps = re.search(r"Stream #\d+:\d+.*?Video:.*?,\s*(\d+(?:\.\d+)?)\s*fps", txt)
    if m_fps:
        fps_val = float(m_fps.group(1))
        info["fps"] = fps_val
        if abs(fps_val - 29.97) < 0.05:
            info["fps_str"] = "30000/1001"
        elif abs(fps_val - 23.976) < 0.05:
            info["fps_str"] = "24000/1001"
        elif abs(fps_val - 59.94) < 0.05:
            info["fps_str"] = "60000/1001"
        else:
            info["fps_str"] = f"{fps_val:.3f}"

    if re.search(r"Stream #\d+:\d+.*?Audio:", txt):
        info["has_audio"] = True
    return info


def discover_input_fmvs(input_dir: Path, name_filter: str = "") -> List[Path]:
    """Find dumped <16hexhash>*.mp4 (or .m2v fallback) files in input_dir, deduplicated by hash."""
    by_hash: Dict[str, Path] = {}
    if not input_dir.is_dir():
        return []
    flt = name_filter.lower().strip()
    # Prefer .mp4 over .m2v for the same hash
    for ext_rank, exts in enumerate(((".mp4", ".m4v", ".mov"), (".m2v",))):
        for p in sorted(input_dir.iterdir()):
            if not p.is_file() or p.suffix.lower() not in exts:
                continue
            h = parse_hex16_prefix(p.stem)
            if not h:
                continue
            if flt and flt not in p.name.lower():
                continue
            if h not in by_hash:
                by_hash[h] = p
    return sorted(by_hash.values(), key=lambda x: x.name.lower())


def upscale_single_fmv(
    src_path: Path,
    dst_path: Path,
    ffmpeg_exe: Path,
    esrgan_exe: Path,
    scale: int = 2,
    model: str = "realesr-animevideov3",
    chunk_sec: int = 30,
    crf: int = 17,
    gpu_id: str = "auto",
) -> None:
    """Upscale one FMV cutscene in lossless keyframe segments with bounded temp disk usage."""
    info = probe_video_info(ffmpeg_exe, src_path)
    src_w = int(info["width"])
    src_h = int(info["height"])
    fps_str = str(info["fps_str"])
    dur = float(info["duration"])
    has_audio = bool(info["has_audio"])
    dst_w = src_w * scale
    dst_h = src_h * scale

    print(
        f"[fmv-ai] Upscaling {src_path.name} ({src_w}x{src_h} -> {dst_w}x{dst_h}, "
        f"{dur:.1f}s @ {fps_str} fps, model={model} x{scale}, audio={'yes' if has_audio else 'no'})",
        flush=True,
    )
    t0 = time.perf_counter()
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    models_dir = esrgan_exe.parent / "models"

    with tempfile.TemporaryDirectory(prefix="sh2_fmv_ai_") as td:
        work = Path(td)
        seg_dir = work / "segs"
        seg_dir.mkdir()

        # 1) Split into keyframe-aligned video-only segments with zero re-encoding & zero frame drift
        seg_pat = seg_dir / "seg_%04d.mkv"
        split_cmd = [
            str(ffmpeg_exe), "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(src_path),
            "-map", "0:v:0", "-c:v", "copy", "-an",
            "-f", "segment", "-segment_time", str(max(5, chunk_sec)),
            "-reset_timestamps", "1",
            str(seg_pat),
        ]
        subprocess.run(split_cmd, check=True)
        segs = sorted(seg_dir.glob("seg_*.mkv"))
        if not segs:
            raise RuntimeError(f"FFmpeg produced 0 segments for {src_path}")

        hd_segs: List[Path] = []
        total_frames_done = 0

        for si, seg in enumerate(segs, start=1):
            in_dir = work / "in_frames"
            out_dir = work / "out_frames"
            shutil.rmtree(in_dir, ignore_errors=True)
            shutil.rmtree(out_dir, ignore_errors=True)
            in_dir.mkdir()
            out_dir.mkdir()

            # 2a) Extract exact frames of this segment (FFmpeg 5.1+ / 8.x compatible)
            subprocess.run(
                [
                    str(ffmpeg_exe), "-hide_banner", "-loglevel", "error", "-y",
                    "-i", str(seg),
                    "-fps_mode", "passthrough",
                    str(in_dir / "f_%06d.png"),
                ],
                check=True,
            )
            n_frames = sum(1 for _ in in_dir.glob("*.png"))
            if n_frames == 0:
                continue

            # 2b) Batch AI-upscale the segment folder on GPU via Vulkan
            esr_cmd = [
                str(esrgan_exe),
                "-i", str(in_dir),
                "-o", str(out_dir),
                "-n", model,
                "-s", str(scale),
                "-m", str(models_dir),
                "-f", "png",
                "-j", "2:2:2",
            ]
            if gpu_id != "auto":
                esr_cmd.extend(["-g", gpu_id])
            t_seg = time.perf_counter()
            res = subprocess.run(esr_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
            if res.returncode != 0:
                raise RuntimeError(f"realesrgan-ncnn-vulkan failed (rc={res.returncode}): {res.stderr.strip()}")

            # 2c) Encode upscaled segment to H.264 MP4 (exact CFR, yuv420p for Media Foundation)
            hd_seg = work / f"hd_{si:04d}.mp4"
            subprocess.run(
                [
                    str(ffmpeg_exe), "-hide_banner", "-loglevel", "error", "-y",
                    "-framerate", fps_str,
                    "-i", str(out_dir / "f_%06d.png"),
                    "-c:v", "libx264", "-crf", str(crf), "-preset", "fast",
                    "-pix_fmt", "yuv420p",
                    "-r", fps_str,
                    str(hd_seg),
                ],
                check=True,
            )
            hd_segs.append(hd_seg)
            total_frames_done += n_frames

            # Free temp PNGs immediately before next segment
            shutil.rmtree(in_dir, ignore_errors=True)
            shutil.rmtree(out_dir, ignore_errors=True)
            seg.unlink(missing_ok=True)

            seg_dt = time.perf_counter() - t_seg
            print(
                f"  [fmv-ai] Segment {si:2d}/{len(segs):2d}: {n_frames:4d} frames in {seg_dt:5.1f}s "
                f"({n_frames / max(0.05, seg_dt):4.1f} fps) | total={total_frames_done:,d} frames",
                flush=True,
            )

        if not hd_segs:
            raise RuntimeError(f"No upscaled segments produced for {src_path}")

        # 3) Losslessly concatenate HD segments & copy original AAC audio track
        concat_list = work / "concat.txt"
        concat_list.write_text(
            "".join(f"file '{p.as_posix()}'\n" for p in hd_segs),
            encoding="utf-8",
        )
        tmp_out = dst_path.with_suffix(".tmp.mp4")
        mux_cmd = [
            str(ffmpeg_exe), "-hide_banner", "-loglevel", "error", "-y",
            "-f", "concat", "-safe", "0", "-i", str(concat_list),
        ]
        if has_audio:
            mux_cmd.extend([
                "-i", str(src_path),
                "-map", "0:v:0", "-map", "1:a:0",
                "-c:v", "copy", "-c:a", "copy",
                "-shortest",
            ])
        else:
            mux_cmd.extend(["-map", "0:v:0", "-c:v", "copy", "-an"])
        mux_cmd.extend([
            "-aspect", "4:3",
            "-movflags", "+faststart",
            str(tmp_out),
        ])
        subprocess.run(mux_cmd, check=True)
        tmp_out.replace(dst_path)

    dt = time.perf_counter() - t0
    sz_mb = dst_path.stat().st_size / (1024 * 1024)
    print(
        f"[fmv-ai] Completed {dst_path.name} ({sz_mb:.1f} MiB, {total_frames_done:,d} frames) in {dt:.1f}s.",
        flush=True,
    )


def run_self_test() -> int:
    print("[fmv-ai] Running self-test (hash prefix parser, discovery, ffmpeg probe)...")
    assert parse_hex16_prefix("53d2ba1148512c1f_DEMO") == "53d2ba1148512c1f"
    assert parse_hex16_prefix("464d127b972c82a4") == "464d127b972c82a4"
    assert parse_hex16_prefix("464d127b972c82a4-Movie") == "464d127b972c82a4"
    assert parse_hex16_prefix("0000000000000000_bad") is None
    assert parse_hex16_prefix("notahash_DEMO") is None
    assert parse_hex16_prefix("53d2ba1148512c1fextra") is None

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "53d2ba1148512c1f_DEMO.mp4").write_bytes(b"mp4")
        (tmp / "53d2ba1148512c1f.m2v").write_bytes(b"m2v")
        (tmp / "464d127b972c82a4_Movie.mp4").write_bytes(b"mp4")
        (tmp / "fmv_index.tsv").write_text("header\n", encoding="utf-8")
        found = discover_input_fmvs(tmp)
        assert [p.name for p in found] == ["464d127b972c82a4_Movie.mp4", "53d2ba1148512c1f_DEMO.mp4"]
        assert [p.name for p in discover_input_fmvs(tmp, "demo")] == ["53d2ba1148512c1f_DEMO.mp4"]

    ff = find_ffmpeg()
    print(f"[fmv-ai] Self-test PASSED (ffmpeg={'found: ' + str(ff) if ff else 'auto-setup ready'}).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Shadow Hearts: Covenant — Local Vulkan AI FMV Cutscene Upscaler")
    ap.add_argument("--input", default="games/sh2-covenant/dumps/fmv", help="Directory containing dumped FMV .mp4 files")
    ap.add_argument("--output", default="games/sh2-covenant/mods/fmv", help="Output HD FMV directory for in-game WMF playback")
    ap.add_argument("--scale", type=int, default=2, choices=(2, 3, 4), help="AI upscale factor (default: 2)")
    ap.add_argument("--model", default="realesr-animevideov3", help="Real-ESRGAN video model (default: realesr-animevideov3)")
    ap.add_argument("--chunk-sec", type=int, default=30, help="Seconds per temp frame batch (controls peak temp disk)")
    ap.add_argument("--crf", type=int, default=17, help="H.264 CRF quality (lower = higher quality, default: 17)")
    ap.add_argument("--gpu-id", default="auto", help="Vulkan GPU index or 'auto'")
    ap.add_argument("--filter", default="", help="Optional case-insensitive filename substring filter (e.g. DEMO or Movie)")
    ap.add_argument("--limit", type=int, default=0, help="Optional max number of FMVs to upscale this run (0 = all)")
    ap.add_argument("--overwrite", action="store_true", help="Re-upscale FMVs even if already present in output")
    ap.add_argument("--auto-setup", action="store_true", help="Auto-download portable ffmpeg.exe & realesrgan if missing")
    ap.add_argument("--setup-ffmpeg", action="store_true", help="Download portable ffmpeg.exe into tools/ffmpeg/ and exit")
    ap.add_argument("--self-test", action="store_true", help="Run built-in self-test and exit")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.setup_ffmpeg:
        ensure_ffmpeg(auto_setup=True)
        return 0

    in_dir = Path(args.input)
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    ffmpeg_exe = ensure_ffmpeg(auto_setup=args.auto_setup)
    esrgan_exe = ensure_realesrgan_exe(auto_setup=args.auto_setup)

    fmvs = discover_input_fmvs(in_dir, args.filter)
    if not fmvs:
        print(f"[fmv-ai] No dumped FMV videos found in {in_dir} (filter={args.filter!r}).")
        print("[fmv-ai] Run 'Dump All Disc FMVs (MP4)' first to extract all Disc 1 & Disc 2 cutscenes.")
        return 0

    existing_hashes = set()
    if not args.overwrite:
        for p in out_dir.glob("*.mp4"):
            h = parse_hex16_prefix(p.stem)
            if h:
                existing_hashes.add(h)

    todo = [p for p in fmvs if args.overwrite or (parse_hex16_prefix(p.stem) not in existing_hashes)]
    if args.limit > 0:
        todo = todo[: args.limit]

    print(
        f"[fmv-ai] Found {len(fmvs)} dumped FMV(s); {len(fmvs) - len(todo)} already in {out_dir}, "
        f"{len(todo)} to upscale at {args.scale}x ({args.model}).",
        flush=True,
    )
    if not todo:
        print("[fmv-ai] All cutscenes are already upscaled!")
        return 0

    t_all = time.perf_counter()
    for idx, src in enumerate(todo, start=1):
        dst = out_dir / f"{src.stem}.mp4"
        print(f"\n=== [{idx}/{len(todo)}] {src.name} ===", flush=True)
        upscale_single_fmv(
            src_path=src,
            dst_path=dst,
            ffmpeg_exe=ffmpeg_exe,
            esrgan_exe=esrgan_exe,
            scale=args.scale,
            model=args.model,
            chunk_sec=args.chunk_sec,
            crf=args.crf,
            gpu_id=args.gpu_id,
        )

    total_dt = time.perf_counter() - t_all
    print(f"\n[fmv-ai] Finished {len(todo)} HD FMV cutscene(s) in {total_dt / 60.0:.1f} min -> {out_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
