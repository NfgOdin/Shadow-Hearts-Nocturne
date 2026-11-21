#!/usr/bin/env python3
"""One-click ahead-of-time VU1/VU0 microprogram pre-compiler for Shadow Hearts: From the New World (SLUS-21326).

Delegates to sh2_precompile_vu1.py configured for Shadow Hearts 3 (games/sh3-ftnw/SH3.exe).
"""
from __future__ import annotations

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

import sh2_precompile_vu1


if __name__ == "__main__":
    sh2_precompile_vu1.main(default_game="sh3")
