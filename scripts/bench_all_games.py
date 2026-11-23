#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — All-4-Games Headless Performance Benchmark.

Measures real frame-time / throughput across all 4 installed games:
  - Koudelka (PS1)
  - Shadow Hearts (1913, PS2)
  - Shadow Hearts: Covenant (1915, PS2)
  - Shadow Hearts: From the New World (1929, PS2)

Usage:
  tools/portable/python/python.exe scripts/bench_all_games.py --frames 300
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)

GAMES = [
    ("koudelka", "Koudelka (PS1)", "psx"),
    ("sh1", "Shadow Hearts (1913)", "ps2"),
    ("sh2-covenant", "Shadow Hearts: Covenant (1915)", "ps2"),
    ("sh3-ftnw", "Shadow Hearts: From the New World (1929)", "ps2"),
]


def find_engine_exe(kind: str) -> str | None:
    if kind == "psx":
        cands = [
            os.path.join(REPO_ROOT, "engine", "nocturne_psx.exe"),
            os.path.join(REPO_ROOT, "build", "native-koudelka", "psxrun.exe"),
        ]
    else:
        cands = [
            os.path.join(REPO_ROOT, "engine", "nocturne_ps2.exe"),
            os.path.join(REPO_ROOT, "build", "native-sh2-covenant", "nocturne_ps2.exe"),
        ]
    for p in cands:
        if os.path.isfile(p):
            return p
    return None


def bench_game(slug: str, title: str, kind: str, frames: int, timeout_s: int) -> dict:
    gdir = os.path.join(REPO_ROOT, "games", slug)
    if not os.path.isdir(gdir):
        return {"slug": slug, "title": title, "status": "SKIP (not installed)"}
    exe = find_engine_exe(kind)
    if not exe:
        return {"slug": slug, "title": title, "status": "SKIP (engine binary missing)"}

    env = os.environ.copy()
    env["PS2_SCALE"] = "1"
    if kind == "psx":
        cmd = [exe, "--game-dir", gdir, "--headless", "--frames", str(frames)]
    else:
        cmd = [exe, "--game", slug, "--game-dir", gdir, "--frames", str(frames)]

    t0 = time.perf_counter()
    try:
        res = subprocess.run(cmd, cwd=REPO_ROOT, env=env, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return {"slug": slug, "title": title, "status": f"TIMEOUT (>{timeout_s}s)"}
    elapsed = max(1e-6, time.perf_counter() - t0)
    fps = frames / elapsed
    ms_per_frame = (elapsed * 1000.0) / frames
    return {
        "slug": slug,
        "title": title,
        "status": "OK" if res.returncode == 0 else f"EXIT {res.returncode}",
        "elapsed_s": elapsed,
        "fps": fps,
        "ms_per_frame": ms_per_frame,
        "frames": frames,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Benchmark all 4 Shadow Hearts: Nocturne games headlessly")
    ap.add_argument("--frames", type=int, default=300, help="Frames to run per game (default: 300)")
    ap.add_argument("--timeout", type=int, default=60, help="Timeout seconds per game (default: 60)")
    ap.add_argument("--game", default="all", help="Single game slug or 'all'")
    args = ap.parse_args(argv)

    print(f"=== Shadow Hearts: Nocturne — All-4-Games Performance Benchmark ({args.frames} frames @ 1x stock) ===")
    print(f"{'Game':42s} | {'Status':10s} | {'Elapsed':8s} | {'Avg FPS':9s} | {'ms/frame':8s}")
    print("-" * 88)
    for slug, title, kind in GAMES:
        if args.game != "all" and args.game != slug:
            continue
        r = bench_game(slug, title, kind, args.frames, args.timeout)
        if "fps" in r:
            print(f"{title:42s} | {r['status']:10s} | {r['elapsed_s']:7.2f}s | {r['fps']:9.1f} | {r['ms_per_frame']:7.2f}ms")
        else:
            print(f"{title:42s} | {r['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
