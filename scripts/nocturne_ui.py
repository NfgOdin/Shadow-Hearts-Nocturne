#!/usr/bin/env python3
"""Shared console UX, live progress bars, hardware-aware compile presets,
graceful running-app detection/auto-close, native Windows browse dialogs,
portable install-folder config, and shortcut helpers for Shadow Hearts: Nocturne.
"""
from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from typing import Callable, Iterable

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
INSTALL_CONFIG_PATH = os.path.join(REPO_ROOT, "install_config.json")

# Dedicated exit code when the user declines to close a running app and cancels cleanly.
EXIT_USER_CANCELLED = 10

# ANSI styling (enabled cleanly on Windows 10/11 consoles via SetConsoleMode)
_ANSI_READY = False


def enable_ansi() -> bool:
    global _ANSI_READY
    if _ANSI_READY:
        return True
    if os.name != "nt":
        _ANSI_READY = sys.stdout.isatty()
        return _ANSI_READY
    try:
        if not sys.stdout.isatty():
            return False
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
            kernel32.SetConsoleMode(handle, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
            _ANSI_READY = True
            return True
    except Exception:
        pass
    return False


class Style:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    GOLD = "\033[38;5;221m"
    CRIMSON = "\033[38;5;203m"
    GREEN = "\033[38;5;114m"
    CYAN = "\033[38;5;117m"
    YELLOW = "\033[38;5;222m"
    GRAY = "\033[38;5;245m"


def paint(text: str, *codes: str) -> str:
    if not enable_ansi() or not codes:
        return text
    return "".join(codes) + text + Style.RESET


def fmt_time(sec: float) -> str:
    sec = max(0, int(sec))
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h:d}:{m:02d}:{s:02d}"
    return f"{m:02d}:{s:02d}"


def print_banner(title: str, subtitle: str = "") -> None:
    enable_ansi()
    bar = "=" * 78
    print()
    print(paint(bar, Style.GOLD))
    print(paint(f"  {title}", Style.BOLD, Style.GOLD))
    if subtitle:
        print(paint(f"  {subtitle}", Style.GRAY))
    print(paint(bar, Style.GOLD))


def print_section(text: str) -> None:
    print(paint(f"\n{text}", Style.BOLD, Style.CYAN), flush=True)


def wait_for_keypress(prompt: str = "Press any key to close...") -> None:
    print()
    if os.name == "nt" and sys.stdin.isatty():
        try:
            import msvcrt

            print(paint(f"  {prompt}", Style.GRAY), end="", flush=True)
            msvcrt.getch()
            print()
            return
        except Exception:
            pass
    try:
        input(f"  {prompt} (Enter) ")
    except EOFError:
        pass


# ---------------------------------------------------------------------------
# Hardware Detection & Compile-Job Presets
# ---------------------------------------------------------------------------


def detect_system_specs() -> tuple[int, float]:
    """Return (logical_cpu_count, total_ram_gb)."""
    cpus = max(1, os.cpu_count() or 4)
    ram_gb = 8.0
    if os.name == "nt":
        try:
            class MEMORYSTATUSEX(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_uint32),
                    ("dwMemoryLoad", ctypes.c_uint32),
                    ("ullTotalPhys", ctypes.c_uint64),
                    ("ullAvailPhys", ctypes.c_uint64),
                    ("ullTotalPageFile", ctypes.c_uint64),
                    ("ullAvailPageFile", ctypes.c_uint64),
                    ("ullTotalVirtual", ctypes.c_uint64),
                    ("ullAvailVirtual", ctypes.c_uint64),
                    ("ullAvailExtendedVirtual", ctypes.c_uint64),
                ]

            stat = MEMORYSTATUSEX()
            stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat)):
                ram_gb = stat.ullTotalPhys / (1024.0 ** 3)
        except Exception:
            pass
    else:
        try:
            pages = os.sysconf("SC_PHYS_PAGES")
            page_sz = os.sysconf("SC_PAGE_SIZE")
            if pages > 0 and page_sz > 0:
                ram_gb = (pages * page_sz) / (1024.0 ** 3)
        except Exception:
            pass
    return cpus, round(ram_gb, 1)


def recommended_compile_jobs(cpus: int | None = None, ram_gb: float | None = None) -> tuple[int, str]:
    """Compute RAM-safe yet high-performance Ninja job count + preset label.

    - <= 10 GB RAM: 3 jobs (Safe / Low-RAM — prevents clang++ OOM on 8 GB hosts)
    - 11-20 GB RAM: up to 6 jobs (Balanced)
    - 21-36 GB RAM: up to 12 jobs (Fast)
    - > 36 GB RAM (e.g. 64 GB DDR5): full CPU parallelism capped at ram_gb // 2 (Turbo)
    """
    if cpus is None or ram_gb is None:
        cpus, ram_gb = detect_system_specs()
    if ram_gb <= 10.5:
        return min(3, cpus), "Safe (Low-RAM)"
    if ram_gb <= 20.5:
        return max(3, min(6, cpus)), "Balanced"
    if ram_gb <= 36.5:
        return max(4, min(12, cpus)), "Fast"
    turbo = max(6, min(cpus, int(ram_gb // 2)))
    return turbo, "Turbo (High-End)"


def build_job_presets(cpus: int | None = None, ram_gb: float | None = None) -> list[tuple[str, int, str]]:
    """Return selectable presets: [(label, jobs, description), ...]."""
    if cpus is None or ram_gb is None:
        cpus, ram_gb = detect_system_specs()
    rec_jobs, rec_name = recommended_compile_jobs(cpus, ram_gb)
    balanced_jobs = max(3, min(6, cpus))
    turbo_jobs = max(4, min(cpus, max(6, int(ram_gb // 2))))
    return [
        (f"Auto-Recommended ({rec_name})", rec_jobs, f"{rec_jobs} threads — matched to {cpus} CPU threads / {ram_gb:.0f} GB RAM"),
        ("Safe / Low-RAM", min(3, cpus), f"{min(3, cpus)} threads — best for 8 GB RAM or background multitasking"),
        ("Balanced", balanced_jobs, f"{balanced_jobs} threads — solid speed for 16 GB RAM PCs"),
        ("High-End / Turbo", turbo_jobs, f"{turbo_jobs} threads — maximum speed for 32-64+ GB RAM rigs"),
    ]


# ---------------------------------------------------------------------------
# Live Visual Progress Bars & Animated Spinner Commands
# ---------------------------------------------------------------------------


def format_bar(ratio: float, width: int = 24) -> str:
    ratio = max(0.0, min(1.0, ratio))
    filled = int(round(ratio * width))
    if enable_ansi():
        return "[" + paint("█" * filled, Style.GOLD) + paint("░" * (width - filled), Style.GRAY) + "]"
    return "[" + ("#" * filled) + ("-" * (width - filled)) + "]"


class ProgressBar:
    """Single-line updating progress bar (falls back cleanly to periodic lines when piped)."""

    def __init__(self, label: str, total: int = 100, width: int = 24):
        self.label = label
        self.total = max(1, total)
        self.width = width
        self.current = 0
        self.t0 = time.monotonic()
        self._last_draw = 0.0
        self._tty = sys.stdout.isatty()
        self._last_len = 0
        self._finished = False

    def update(self, current: int, detail: str = "", total: int | None = None, force: bool = False) -> None:
        if total is not None and total > 0:
            self.total = total
        self.current = max(0, min(self.total, current))
        now = time.monotonic()
        if not force and self.current < self.total and (now - self._last_draw) < 0.12:
            return
        self._last_draw = now
        ratio = self.current / max(1, self.total)
        pct = ratio * 100.0
        elapsed = now - self.t0
        eta_str = fmt_time((elapsed * (self.total - self.current)) / self.current) if self.current > 0 else "--:--"
        short_detail = detail if len(detail) <= 34 else ("..." + detail[-31:])
        bar = format_bar(ratio, self.width)
        line = f"  [{self.label}] {bar} {pct:5.1f}% ({self.current}/{self.total}) {fmt_time(elapsed)} ETA {eta_str}"
        if short_detail:
            line += f" | {short_detail}"
        if self._tty:
            pad = max(0, self._last_len - len(line))
            sys.stdout.write("\r" + line + (" " * pad))
            sys.stdout.flush()
            self._last_len = len(line)
        else:
            print(line, flush=True)

    def finish(self, summary: str = "") -> None:
        if self._finished:
            return
        self._finished = True
        elapsed = time.monotonic() - self.t0
        bar = format_bar(1.0, self.width)
        tail = f" — {summary}" if summary else ""
        line = f"  [{self.label}] {bar} 100.0% ({self.total}/{self.total}) Done in {fmt_time(elapsed)}{tail}"
        if self._tty:
            pad = max(0, self._last_len - len(line))
            sys.stdout.write("\r" + line + (" " * pad) + "\n")
            sys.stdout.flush()
        else:
            print(line, flush=True)


class Spinner:
    """Live animated heartbeat + elapsed clock for indeterminate stages (CMake, recomp, etc.)."""

    FRAMES_UTF = ("|", "/", "-", "\\")

    def __init__(self, label: str, detail: str = ""):
        self.label = label
        self.detail = detail
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._t0 = time.monotonic()
        self._tty = sys.stdout.isatty()
        self._last_len = 0

    def set_detail(self, detail: str) -> None:
        self.detail = detail

    def start(self) -> "Spinner":
        self._t0 = time.monotonic()
        if not self._tty:
            msg = f"  [{self.label}] {self.detail or 'Working...'}"
            print(msg, flush=True)
            return self
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _run(self) -> None:
        idx = 0
        while not self._stop.is_set():
            ch = self.FRAMES_UTF[idx % len(self.FRAMES_UTF)]
            idx += 1
            elapsed = fmt_time(time.monotonic() - self._t0)
            det = self.detail if len(self.detail) <= 50 else ("..." + self.detail[-47:])
            line = f"  [{self.label}] {paint(ch, Style.GOLD)} {det} ({elapsed})"
            pad = max(0, self._last_len - len(line))
            sys.stdout.write("\r" + line + (" " * pad))
            sys.stdout.flush()
            self._last_len = len(line)
            self._stop.wait(0.15)

    def finish(self, ok: bool = True, summary: str = "") -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None
        elapsed = fmt_time(time.monotonic() - self._t0)
        mark = paint("OK", Style.GREEN) if ok else paint("FAIL", Style.CRIMSON)
        msg = summary or self.detail or ("Complete" if ok else "Failed")
        line = f"  [{self.label}] [{mark}] {msg} ({elapsed})"
        if self._tty:
            pad = max(0, self._last_len - len(line))
            sys.stdout.write("\r" + line + (" " * pad) + "\n")
            sys.stdout.flush()
        else:
            print(line, flush=True)

    def __enter__(self) -> "Spinner":
        return self.start()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.finish(ok=(exc_type is None))


def run_spinner_cmd(
    cmd: list[str],
    label: str,
    detail: str = "Working...",
    cwd: str = REPO_ROOT,
    env: dict[str, str] | None = None,
    echo_stdout: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run a command with a live animated spinner + elapsed timer; show full logs on failure."""
    sp = Spinner(label, detail)
    sp.start()
    try:
        res = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except Exception as exc:
        sp.finish(ok=False, summary=str(exc))
        raise
    if res.returncode != 0:
        sp.finish(ok=False, summary=f"exited with code {res.returncode}")
        if res.stdout:
            print("\n--- Command output ---", file=sys.stderr)
            lines = res.stdout.strip().splitlines()
            for ln in lines[-40:]:
                print("  " + ln, file=sys.stderr)
        raise RuntimeError(f"{label} failed (exit code {res.returncode}): {' '.join(cmd)}")
    sp.finish(ok=True, summary=detail)
    if echo_stdout and res.stdout:
        for ln in res.stdout.strip().splitlines():
            if ln.strip():
                print("    " + ln)
    return res


def run_ninja_with_progress(
    cmd: list[str],
    label: str,
    cwd: str = REPO_ROOT,
    env: dict[str, str] | None = None,
) -> None:
    """Run Ninja / `cmake --build` with a live updating visual progress bar + ETA."""
    t0 = time.monotonic()
    step_re = re.compile(r"^\[(\d+)/(\d+)\]\s+(.*)$")
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdout is not None

    bar: ProgressBar | None = None
    spinner: Spinner | None = Spinner(label, "Checking build graph...").start()
    output_tail: list[str] = []
    errors: list[str] = []

    for raw in proc.stdout:
        line = raw.rstrip("\r\n")
        output_tail.append(line)
        if len(output_tail) > 60:
            output_tail.pop(0)
        m = step_re.match(line)
        if m:
            cur, total = int(m.group(1)), int(m.group(2))
            desc = m.group(3)
            if spinner is not None:
                spinner._stop.set()
                if spinner._thread is not None:
                    spinner._thread.join(timeout=0.5)
                    spinner._thread = None
                spinner = None
            if bar is None:
                bar = ProgressBar(label, total=max(1, total))
                bar.t0 = t0
            bar.update(cur, detail=desc, total=max(1, total), force=(cur == total or cur == 1))
        elif "no work to do" in line.lower():
            if spinner is not None:
                spinner.finish(ok=True, summary="Up to date (no work to do)")
                spinner = None
        elif any(tok in line for tok in ("error:", "fatal error", "FAILED:", "lld-link:", "ld.lld:")):
            errors.append(line)

    rc = proc.wait()
    if rc != 0:
        if spinner is not None:
            spinner.finish(ok=False, summary=f"failed (exit {rc})")
        elif bar is not None and sys.stdout.isatty():
            sys.stdout.write("\n")
            sys.stdout.flush()
        print(paint(f"\n--- {label} failed (exit code {rc}) ---", Style.CRIMSON), file=sys.stderr)
        for ln in (errors[-20:] if errors else output_tail[-30:]):
            print("  " + ln, file=sys.stderr)
        raise RuntimeError(f"{label} failed with exit code {rc}")

    if bar is not None:
        bar.finish("Complete")
    elif spinner is not None:
        spinner.finish(ok=True, summary="Complete")


# ---------------------------------------------------------------------------
# Graceful Running-App Detection & Auto-Close
# ---------------------------------------------------------------------------

ALL_NOCTURNE_EXES = [
    "Shadow Hearts Nocturne.exe",
    "sh2launcher.exe",
    "nocturne_ps2.exe",
    "nocturne_psx.exe",
    "Koudelka.exe",
    "SH1.exe",
    "SH2C.exe",
    "SH3.exe",
]

EXE_LABELS = {
    "Shadow Hearts Nocturne.exe": "Shadow Hearts Nocturne Launcher",
    "sh2launcher.exe": "Shadow Hearts Nocturne Launcher",
    "nocturne_ps2.exe": "Shadow Hearts PS2 Engine",
    "nocturne_psx.exe": "Koudelka PS1 Engine",
    "Koudelka.exe": "Koudelka (PS1)",
    "SH1.exe": "Shadow Hearts (1913)",
    "SH2C.exe": "Shadow Hearts: Covenant (1915)",
    "SH3.exe": "Shadow Hearts: From the New World (1929)",
}


def find_running_exes(exe_names: Iterable[str]) -> list[str]:
    """Return the subset of exe_names currently running on Windows."""
    if os.name != "nt":
        return []
    wanted = {name.lower(): name for name in exe_names}
    if not wanted:
        return []
    try:
        res = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
    except Exception:
        return []
    found: list[str] = []
    seen: set[str] = set()
    for line in res.stdout.splitlines():
        line = line.strip()
        if not line.startswith('"'):
            continue
        end = line.find('"', 1)
        if end <= 1:
            continue
        proc_name = line[1:end]
        key = proc_name.lower()
        if key in wanted and key not in seen:
            seen.add(key)
            found.append(wanted[key])
    return found


def close_exes_gracefully(exe_names: list[str], wait_sec: float = 3.5) -> list[str]:
    """Send WM_CLOSE first so saves flush cleanly, then force-close only if still open.

    Returns any exe names that somehow remained running after the timeout.
    """
    if os.name != "nt" or not exe_names:
        return []
    for name in exe_names:
        subprocess.run(["taskkill", "/IM", name], capture_output=True)
    deadline = time.monotonic() + wait_sec
    remaining = list(exe_names)
    while time.monotonic() < deadline:
        remaining = find_running_exes(exe_names)
        if not remaining:
            time.sleep(0.35)  # brief settle so Windows releases the EXE file lock
            return []
        time.sleep(0.25)
    # Fallback force close for any app that ignored WM_CLOSE
    for name in remaining:
        subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
    time.sleep(0.6)
    return find_running_exes(exe_names)


def ensure_apps_closed(
    exe_names: Iterable[str] = ALL_NOCTURNE_EXES,
    interactive: bool = True,
    pause_on_cancel: bool = True,
) -> bool:
    """Check if the Launcher or game EXEs are open.

    If running:
      - In interactive mode: politely explains that the app(s) must be closed to continue,
        asks Y/N. On Yes, auto-closes cleanly and continues. On No, cancels cleanly,
        tells the user to re-run Setup.cmd when finished, pauses for keypress, and returns False.
      - In non-interactive mode: returns False with a clear message.
    """
    running = find_running_exes(exe_names)
    if not running:
        return True

    print()
    print(paint("  " + "-" * 74, Style.YELLOW))
    print(paint("  APP CURRENTLY OPEN — MUST BE CLOSED TO CONTINUE", Style.BOLD, Style.YELLOW))
    print(paint("  " + "-" * 74, Style.YELLOW))
    print("  The following Shadow Hearts: Nocturne app(s) are currently running:")
    for exe in running:
        friendly = EXE_LABELS.get(exe, exe)
        print(f"    • {paint(friendly, Style.BOLD)} ({exe})")
    print()
    print("  They must be closed before installing or updating executables.")

    if not interactive:
        print("  Re-run Setup after closing the app(s) above.")
        return False

    while True:
        try:
            ans = input("\n  Close the running app(s) automatically and continue? [Y/N]: ").strip().lower()
        except EOFError:
            ans = "n"
        if ans in ("y", "yes"):
            print("  Closing app(s) cleanly...")
            still = close_exes_gracefully(running)
            if still:
                print(paint(f"  Could not close: {', '.join(still)}. Please close manually and re-run Setup.", Style.CRIMSON))
                if pause_on_cancel:
                    wait_for_keypress("Press any key to close...")
                return False
            print(paint("  Closed cleanly. Continuing installation...", Style.GREEN))
            return True
        if ans in ("n", "no"):
            print()
            print(paint("  Installation cancelled.", Style.YELLOW))
            print("  Please re-run 'Shadow Hearts Nocturne Setup.cmd' once you are finished with the app.")
            if pause_on_cancel:
                wait_for_keypress("Press any key to close...")
            return False
        print("  Please enter Y (yes) or N (no).")


# ---------------------------------------------------------------------------
# Native Windows Folder / File Browse Dialogs (via PowerShell WinForms)
# ---------------------------------------------------------------------------


def pick_folder_dialog(title: str = "Select Folder", initial_dir: str = "") -> str:
    """Open a topmost native Windows folder picker. Returns selected path or ''."""
    if os.name != "nt":
        return ""
    init = initial_dir.replace("'", "''") if initial_dir and os.path.isdir(initial_dir) else ""
    safe_title = title.replace("'", "''")
    ps = (
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$owner = New-Object System.Windows.Forms.Form; "
        "$owner.TopMost = $true; $owner.StartPosition = 'CenterScreen'; "
        "$owner.Width = 1; $owner.Height = 1; $owner.ShowInTaskbar = $false; "
        "$dlg = New-Object System.Windows.Forms.FolderBrowserDialog; "
        f"$dlg.Description = '{safe_title}'; "
        "$dlg.UseDescriptionForTitle = $true; "
        "$dlg.ShowNewFolderButton = $true; "
        + (f"$dlg.SelectedPath = '{init}'; " if init else "")
        + "if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dlg.SelectedPath }; "
        "$owner.Dispose()"
    )
    try:
        res = subprocess.run(
            ["powershell.exe", "-NoLogo", "-NoProfile", "-STA", "-Command", ps],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return res.stdout.strip()
    except Exception:
        return ""


def pick_file_dialog(
    title: str = "Select Disc Image",
    file_filter: str = "Disc Images (*.iso;*.chd;*.cso;*.cue;*.bin)|*.iso;*.chd;*.cso;*.cue;*.bin|All Files (*.*)|*.*",
    initial_dir: str = "",
) -> str:
    """Open a topmost native Windows file picker. Returns selected file path or ''."""
    if os.name != "nt":
        return ""
    init = initial_dir.replace("'", "''") if initial_dir and os.path.isdir(initial_dir) else ""
    safe_title = title.replace("'", "''")
    safe_filter = file_filter.replace("'", "''")
    ps = (
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$owner = New-Object System.Windows.Forms.Form; "
        "$owner.TopMost = $true; $owner.StartPosition = 'CenterScreen'; "
        "$owner.Width = 1; $owner.Height = 1; $owner.ShowInTaskbar = $false; "
        "$dlg = New-Object System.Windows.Forms.OpenFileDialog; "
        f"$dlg.Title = '{safe_title}'; "
        f"$dlg.Filter = '{safe_filter}'; "
        + (f"$dlg.InitialDirectory = '{init}'; " if init else "")
        + "if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $dlg.FileName }; "
        "$owner.Dispose()"
    )
    try:
        res = subprocess.run(
            ["powershell.exe", "-NoLogo", "-NoProfile", "-STA", "-Command", ps],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return res.stdout.strip()
    except Exception:
        return ""


# ---------------------------------------------------------------------------
# Install Config Persistence & Portable Standalone Staging Helpers
# ---------------------------------------------------------------------------


def load_install_config(path: str = INSTALL_CONFIG_PATH) -> dict:
    """Load persisted user install preferences (install_dir, jobs, shortcuts, etc.)."""
    rec_jobs, _ = recommended_compile_jobs()
    defaults = {
        "install_dir": REPO_ROOT,
        "jobs": rec_jobs,
        "create_desktop_shortcut": True,
        "create_startmenu_shortcut": False,
        "delete_source_images": False,
        "last_rom_dir": "",
    }
    if not os.path.isfile(path):
        return defaults
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            defaults.update({k: v for k, v in data.items() if v is not None})
    except Exception:
        pass
    if not defaults.get("install_dir"):
        defaults["install_dir"] = REPO_ROOT
    return defaults


def save_install_config(cfg: dict, path: str = INSTALL_CONFIG_PATH) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def resolve_install_dir(cli_install_dir: str | None = None) -> str:
    if cli_install_dir:
        return os.path.abspath(os.path.expanduser(cli_install_dir.strip().strip('"')))
    cfg = load_install_config()
    return os.path.abspath(cfg.get("install_dir") or REPO_ROOT)


def copy_dll_if_needed(src: str, dst: str) -> None:
    if not os.path.isfile(src):
        return
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    if os.path.isfile(dst) and os.path.getsize(src) == os.path.getsize(dst):
        return
    try:
        shutil.copy2(src, dst)
    except OSError as exc:
        print(f"  [warn] Skipped locked DLL {os.path.basename(dst)} ({exc})")


def sync_launcher_source_repo(target_root: str, repo_root: str = REPO_ROOT) -> None:
    """Record source_repo in <target_root>/launcher.ini when installing to a standalone folder
    so the Launcher's Tools/Updates/Setup buttons always locate scripts and Setup.cmd.
    """
    target_abs = os.path.abspath(target_root)
    repo_abs = os.path.abspath(repo_root)
    if target_abs.lower() == repo_abs.lower():
        return
    os.makedirs(target_abs, exist_ok=True)
    ini_path = os.path.join(target_abs, "launcher.ini")
    lines: list[str] = []
    if os.path.isfile(ini_path):
        try:
            with open(ini_path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            lines = []
    if not lines:
        lines = ["# Shadow Hearts: Nocturne — Trilogy Launcher Configuration"]

    def upsert(key: str, val: str) -> None:
        lk = key.lower()
        for i, ln in enumerate(lines):
            stripped = ln.strip()
            if not stripped or stripped.startswith("#") or "=" not in stripped:
                continue
            k = stripped.split("=", 1)[0].strip().lower()
            if k == lk:
                lines[i] = f"{key} = {val}"
                return
        lines.append(f"{key} = {val}")

    upsert("source_repo", repo_abs)
    cfg = load_install_config()
    ch = str(cfg.get("update_channel") or "").strip().lower()
    if ch in ("main", "nightly"):
        upsert("update_channel", ch)

    tmp = ini_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, ini_path)


def stage_shared_runtime_dlls(target_root: str, build_dir: str = "") -> None:
    """Copy required shared DLLs (SDL2, DXC, llvm-mingw C++/pthread/unwind) into target_root."""
    os.makedirs(target_root, exist_ok=True)
    sync_launcher_source_repo(target_root)
    dxc_dir = os.path.join(REPO_ROOT, "third_party", "dxc")
    for dll in ("dxcompiler.dll", "dxil.dll"):
        copy_dll_if_needed(os.path.join(dxc_dir, dll), os.path.join(target_root, dll))

    sdl_cands = []
    if build_dir:
        sdl_cands.append(os.path.join(build_dir, "SDL2.dll"))
    sdl_cands.append(os.path.join(REPO_ROOT, "third_party", "sdl2", "bin", "SDL2.dll"))
    for cand in sdl_cands:
        if os.path.isfile(cand):
            copy_dll_if_needed(cand, os.path.join(target_root, "SDL2.dll"))
            break

    mingw_bins = [
        os.path.join(REPO_ROOT, "tools", "portable", "llvm-mingw", "bin"),
        os.path.join(REPO_ROOT, "tools", "portable", "llvm-mingw", "x86_64-w64-mingw32", "bin"),
    ]
    for dll in ("libwinpthread-1.dll", "libc++.dll", "libunwind.dll"):
        found = None
        for bdir in mingw_bins:
            cand = os.path.join(bdir, dll)
            if os.path.isfile(cand):
                found = cand
                break
        if not found:
            w = shutil.which(dll)
            if w and os.path.isfile(w):
                found = w
        if found:
            copy_dll_if_needed(found, os.path.join(target_root, dll))


def create_windows_shortcut(
    target_exe: str,
    shortcut_path: str,
    description: str = "Shadow Hearts: Nocturne — Collection Launcher",
    working_dir: str = "",
) -> bool:
    """Create a Windows .lnk shortcut pointing to target_exe with its embedded icon."""
    if os.name != "nt" or not os.path.isfile(target_exe):
        return False
    os.makedirs(os.path.dirname(os.path.abspath(shortcut_path)), exist_ok=True)
    wd = working_dir or os.path.dirname(os.path.abspath(target_exe))
    esc = lambda s: s.replace("'", "''")
    ps = (
        "$ws = New-Object -ComObject WScript.Shell; "
        f"$sc = $ws.CreateShortcut('{esc(shortcut_path)}'); "
        f"$sc.TargetPath = '{esc(target_exe)}'; "
        f"$sc.WorkingDirectory = '{esc(wd)}'; "
        f"$sc.IconLocation = '{esc(target_exe)},0'; "
        f"$sc.Description = '{esc(description)}'; "
        "$sc.Save()"
    )
    try:
        res = subprocess.run(
            ["powershell.exe", "-NoLogo", "-NoProfile", "-Command", ps],
            capture_output=True,
            text=True,
        )
        return res.returncode == 0 and os.path.isfile(shortcut_path)
    except Exception:
        return False


def create_launcher_shortcuts(install_dir: str, desktop: bool = False, start_menu: bool = False) -> list[str]:
    """Create Desktop and/or Start Menu shortcuts for Shadow Hearts Nocturne.exe."""
    created: list[str] = []
    if os.name != "nt":
        return created
    launcher_exe = os.path.join(install_dir, "Shadow Hearts Nocturne.exe")
    if not os.path.isfile(launcher_exe):
        return created
    if desktop:
        desk_dir = os.path.join(os.environ.get("USERPROFILE", ""), "Desktop")
        if os.path.isdir(desk_dir):
            lnk = os.path.join(desk_dir, "Shadow Hearts Nocturne.lnk")
            if create_windows_shortcut(launcher_exe, lnk, working_dir=install_dir):
                created.append(lnk)
    if start_menu:
        appdata = os.environ.get("APPDATA", "")
        if appdata:
            sm_dir = os.path.join(appdata, "Microsoft", "Windows", "Start Menu", "Programs", "Shadow Hearts Nocturne")
            lnk = os.path.join(sm_dir, "Shadow Hearts Nocturne.lnk")
            if create_windows_shortcut(launcher_exe, lnk, working_dir=install_dir):
                created.append(lnk)
    return created


def any_game_installed(install_dir: str) -> bool:
    """Return True if at least one of the 4 games is staged (game.dll or legacy EXE) under install_dir/games/."""
    candidates = (
        os.path.join(install_dir, "games", "koudelka", "game.dll"),
        os.path.join(install_dir, "games", "sh1", "game.dll"),
        os.path.join(install_dir, "games", "sh2-covenant", "game.dll"),
        os.path.join(install_dir, "games", "sh3-ftnw", "game.dll"),
        os.path.join(install_dir, "games", "koudelka", "Koudelka.exe"),
        os.path.join(install_dir, "games", "sh1", "SH1.exe"),
        os.path.join(install_dir, "games", "sh2-covenant", "SH2C.exe"),
        os.path.join(install_dir, "games", "sh3-ftnw", "SH3.exe"),
    )
    return any(os.path.isfile(p) for p in candidates)


def probe_engine_abi(exe_path: str, install_dir: str | None = None) -> dict | None:
    """Query `nocturne_ps2.exe --print-abi` or `nocturne_psx.exe --print-abi` -> {engine, abi, layout}."""
    if not os.path.isfile(exe_path):
        return None
    env = os.environ.copy()
    if install_dir:
        env["PATH"] = install_dir + os.pathsep + env.get("PATH", "")
    try:
        res = subprocess.run(
            [exe_path, "--print-abi"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
            timeout=10,
        )
        if res.returncode == 0 and res.stdout.strip():
            return json.loads(res.stdout.strip().splitlines()[-1])
    except Exception:
        pass
    return None


def probe_game_dll_abi(dll_path: str, engine_kind: str) -> dict | None:
    """Probe a game.dll's exported ABI & layout in a short-lived subprocess (so the DLL is never locked)."""
    if not os.path.isfile(dll_path):
        return None
    prefix = "nocturne_psx_game" if engine_kind == "psx" else "nocturne_ps2_game"
    code = (
        "import ctypes, json, sys; "
        "lib = ctypes.CDLL(sys.argv[1]); "
        f"abi_fn = getattr(lib, '{prefix}_abi'); abi_fn.restype = ctypes.c_uint32; "
        f"lay_fn = getattr(lib, '{prefix}_layout'); lay_fn.restype = ctypes.c_uint64; "
        "print(json.dumps({'abi': int(abi_fn()), 'layout': f'0x{int(lay_fn()):016X}'}))"
    )
    try:
        res = subprocess.run(
            [sys.executable, "-c", code, dll_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        if res.returncode == 0 and res.stdout.strip():
            return json.loads(res.stdout.strip().splitlines()[-1])
    except Exception:
        pass
    return None


def build_and_stage_shared_engine(
    install_dir: str,
    jobs: int,
    targets: list[str] | None = None,
    env: dict[str, str] | None = None,
) -> None:
    """Configure & incrementally build the shared engine (build/engine) and stage EXEs into install_dir."""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    engine_build = os.path.join(repo_root, "build", "engine")
    want = list(targets or ["sh2launcher", "nocturne_ps2", "nocturne_psx"])

    run_env = dict(env or os.environ)
    portable = os.path.join(repo_root, "tools", "portable")
    bins = [
        os.path.join(portable, "llvm-mingw", "bin"),
        os.path.join(portable, "cmake", "bin"),
        os.path.join(portable, "ninja"),
        os.path.join(portable, "python"),
        os.path.join(portable, "mingit", "cmd"),
    ]
    existing = [p for p in bins if os.path.isdir(p)]
    if existing:
        run_env["PATH"] = os.pathsep.join(existing) + os.pathsep + run_env.get("PATH", "")

    cmake_exe = shutil.which("cmake", path=run_env["PATH"]) or "cmake"
    ninja_file = os.path.join(engine_build, "build.ninja")
    need_configure = not os.path.isfile(ninja_file)
    if not need_configure and "nocturne_ps2" in want:
        # If build/engine was first configured during a PS1-only install (before wgpu-native was fetched),
        # re-configure now that wgpu-native is present so nocturne_ps2 links full Vulkan/DX12 GPU support.
        wgpu_lib = os.path.join(repo_root, "third_party", "wgpu-native", "lib", "libwgpu_native.a")
        if os.path.isfile(wgpu_lib):
            try:
                with open(ninja_file, "r", encoding="utf-8", errors="ignore") as nf:
                    if "wgpu_native" not in nf.read():
                        need_configure = True
            except OSError:
                need_configure = True
    if need_configure:
        cmake_cmd = [
            cmake_exe, "-S", repo_root, "-B", engine_build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DPS2_WGPU_DIR={os.path.join(repo_root, 'third_party', 'wgpu-native')}",
            f"-DCMAKE_PREFIX_PATH={os.path.join(repo_root, 'third_party', 'sdl2')}",
        ]
        run_spinner_cmd(cmake_cmd, "CMake (Engine)", "Configuring shared Nocturne engine...", env=run_env)

    build_cmd = [cmake_exe, "--build", engine_build, "--target"] + want + ["--", f"-j{jobs}"]
    run_ninja_with_progress(build_cmd, "Shared Engine", env=run_env)

    os.makedirs(install_dir, exist_ok=True)
    eng_dir = os.path.join(install_dir, "engine")
    os.makedirs(eng_dir, exist_ok=True)

    staging_map = {
        "sh2launcher": (os.path.join(engine_build, "sh2launcher.exe"), os.path.join(install_dir, "Shadow Hearts Nocturne.exe")),
        "nocturne_ps2": (os.path.join(engine_build, "nocturne_ps2.exe"), os.path.join(eng_dir, "nocturne_ps2.exe")),
        "nocturne_psx": (os.path.join(engine_build, "nocturne_psx.exe"), os.path.join(eng_dir, "nocturne_psx.exe")),
    }
    for tgt in want:
        pair = staging_map.get(tgt)
        if not pair:
            continue
        src, dst = pair
        if os.path.isfile(src):
            tmp_dst = dst + ".upd_tmp"
            shutil.copy2(src, tmp_dst)
            for attempt in range(12):
                try:
                    os.replace(tmp_dst, dst)
                    break
                except OSError:
                    if attempt == 11:
                        try:
                            os.remove(tmp_dst)
                        except OSError:
                            pass
                        raise
                    time.sleep(0.25)
            print(f"  Staged {os.path.basename(dst)} -> {dst}")

    stage_shared_runtime_dlls(install_dir, build_dir=engine_build)
