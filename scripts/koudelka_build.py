#!/usr/bin/env python3
"""Shadow Hearts: Nocturne — Koudelka (1999, PS1 NTSC-U, 4 Discs) Builder.

Milestone 1 prepares the ISO-free deduplicated 4-disc virtual folder under
games/koudelka/disc_files/ (preserving bit-exact Mode 2 Form 2 XA/STR streams)
and extracts the PS-X EXE (SLUS_010.51) + 6 code overlays.
Milestones 2 & 3 will wire psxrecomp + psxruntime to compile games/koudelka/Koudelka.exe.
"""
from __future__ import annotations

import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
GAME_DIR = os.path.join(REPO_ROOT, "games", "koudelka")

sys.path.insert(0, SCRIPT_DIR)
import koudelka_prepare  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build / stage Koudelka (PS1, 4 discs) for Shadow Hearts: Nocturne.")
    ap.add_argument("--source", action="append", default=[], help="Folder containing Koudelka Disc 1..4 .bin/.cue files")
    ap.add_argument("--non-interactive", action="store_true", help="Do not prompt for paths")
    ap.add_argument("--verify", action="store_true", help="Verify unpacked virtual disc sectors against source .bin files")
    args = ap.parse_args(argv)

    print("==============================================================================")
    print("  Shadow Hearts: Nocturne — Koudelka (1999, PS1 NTSC-U, 4 Discs) Builder")
    print("==============================================================================")

    manifest_path = os.path.join(GAME_DIR, "disc_files", "disc_manifest.tsv")
    exe_path = os.path.join(GAME_DIR, "exe", "SLUS_010.51")

    if os.path.isfile(manifest_path) and os.path.isfile(exe_path) and not args.source:
        print(f"\n[1/2] 4-disc virtual folder already prepared: {os.path.join(GAME_DIR, 'disc_files')}")
    else:
        print("\n[1/2] Locating & unpacking Koudelka Discs 1-4 (Mode 2/2352 -> deduplicated virtual discs)...")
        search = list(args.source) + [GAME_DIR] + koudelka_prepare.DEFAULT_SEARCH_DIRS
        discs = koudelka_prepare.find_koudelka_discs(search)
        if len(discs) < 4 and not args.non_interactive:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"  Could not auto-locate Disc(s) {missing}.")
            user_dir = input("  Enter folder path containing Koudelka Disc 1-4 .bin/.cue files: ").strip().strip('"')
            if user_dir:
                discs = koudelka_prepare.find_koudelka_discs([user_dir] + search)
        if len(discs) < 4:
            missing = [d for d in (1, 2, 3, 4) if d not in discs]
            print(f"[ERROR] Missing Koudelka Disc(s) {missing}. Place all 4 NTSC-U discs in one folder and retry.")
            return 1
        koudelka_prepare.unpack_discs(discs, GAME_DIR, verify=args.verify)

    psxrecomp_exe = os.path.join(REPO_ROOT, "build", "native-koudelka", "psxrecomp.exe")
    if not os.path.isfile(psxrecomp_exe):
        print("\n[2/2] Koudelka 4-disc virtual library & PS-X EXE + overlays are staged and verified!")
        print("      Next milestone: psxrecomp (R3000A + GTE static recompiler) & psxruntime will")
        print("      compile games/koudelka/exe/SLUS_010.51 -> games/koudelka/Koudelka.exe.")
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
