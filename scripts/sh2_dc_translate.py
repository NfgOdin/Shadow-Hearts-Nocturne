#!/usr/bin/env python3
"""Shadow Hearts: Covenant — Clean-Room Hash-Keyed English Translation & 40x3 Wrapper Engine.

Zero copyrighted game text is stored in this repository. Translations are keyed
strictly by the 16-hex MD5 digest of the source Shift-JIS byte string:

  Priority order for every Shift-JIS string encountered in DC-only `.MSC` scripts:
    1. Curated hash-keyed English packs in `assets/dc_translations/*.json`
    2. Persistent local cache in `<game_dir>/dc_en_cache.json` (instant on re-runs)
    3. Automatic batched JP -> EN translation on the user's machine (stdlib-only,
       preserving all Shadow Hearts control codes like `%i32`, `%03d`, `%d`, `%s`,
       `$` line breaks, and character/place proper nouns)
    4. Clean offline US-font ASCII sanitizer fallback if no network is available
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import glob
import hashlib
import json
import os
import re
import time
import urllib.parse
import urllib.request
from typing import Dict, Iterable, List, Optional, Tuple

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
CURATED_PACKS_DIR = os.path.join(REPO_ROOT, "assets", "dc_translations")

# Shadow Hearts: Covenant official US proper-noun normalization
_SH2_PROPER_NOUNS: List[Tuple[str, str]] = [
    ("シャドウハーツ", "Shadow Hearts"),
    ("ディレクターズカット", "Director's Cut"),
    ("ジャッジメントリング", "Judgment Ring"),
    ("リング魂", "Ring Soul"),
    ("アナスタシア", "Anastasia"),
    ("ベロニカ", "Veronica"),
    ("ヨアヒム", "Joachim"),
    ("ゲパルト", "Gepetto"),
    ("ゼペット", "Gepetto"),
    ("ブランカ", "Blanca"),
    ("ニコライ", "Nicolai"),
    ("ルチア", "Lucia"),
    ("カレン", "Karin"),
    ("レニー", "Lenny"),
    ("ウル", "Yuri"),
    ("蔵人", "Kurando"),
    ("ナハシュ", "Nahash"),
    ("樹海", "Sea of Woods"),
    ("ネアム", "Neam"),
    ("サント・マルグリット", "Sainte-Marguerite"),
    ("カンヌ", "Cannes"),
    ("フィレンツェ", "Florence"),
    ("パリ", "Paris"),
    ("犬神", "Inugami"),
]

# Engine control tokens that MUST survive translation verbatim
_CTRL_RE = re.compile(r"(%[0-9\-\+]*[a-zA-Z][0-9]*|\n)")

_ASCII_NORM = str.maketrans({
    "\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"',
    "\u2013": "-", "\u2014": "-", "\u2026": "...", "\u00a0": " ",
    "\u3000": " ", "、": ", ", "。": ". ", "！": "!", "？": "?",
    "「": '"', "」": '"', "『": "'", "』": "'", "（": "(", "）": ")",
    "【": "[", "】": "]", "…": "...", "・": " ", "～": "~", "：": ":",
})


def string_hash(raw_bytes: bytes) -> str:
    return hashlib.md5(raw_bytes).hexdigest()[:16]


def load_curated_packs(packs_dir: str = CURATED_PACKS_DIR) -> Dict[str, str]:
    """Load any curated hash-keyed English JSON packs from `assets/dc_translations/*.json`."""
    merged: Dict[str, str] = {}
    if not os.path.isdir(packs_dir):
        return merged
    for path in sorted(glob.glob(os.path.join(packs_dir, "*.json"))):
        try:
            with open(path, "r", encoding="utf-8") as f:
                obj = json.load(f)
            if isinstance(obj, dict):
                entries = obj.get("strings", obj) if isinstance(obj.get("strings"), dict) else obj
                for k, v in entries.items():
                    if isinstance(k, str) and isinstance(v, str) and len(k) == 16:
                        merged[k.lower()] = v
        except Exception:
            pass
    return merged


def load_local_cache(cache_path: str) -> Dict[str, str]:
    if not cache_path or not os.path.isfile(cache_path):
        return {}
    try:
        with open(cache_path, "r", encoding="utf-8") as f:
            obj = json.load(f)
        if isinstance(obj, dict):
            return {str(k).lower(): str(v) for k, v in obj.items() if isinstance(v, str)}
    except Exception:
        pass
    return {}


def save_local_cache(cache_path: str, cache: Dict[str, str]) -> None:
    if not cache_path:
        return
    parent = os.path.dirname(cache_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp = cache_path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cache, f, indent=2, sort_keys=True, ensure_ascii=True)
        f.write("\n")
    os.replace(tmp, cache_path)


def to_us_font_ascii(text: str) -> str:
    """Normalize any Unicode English text into clean printable US-font ASCII."""
    s = text.translate(_ASCII_NORM)
    out = []
    for ch in s:
        o = ord(ch)
        if 0x20 <= o <= 0x7E or ch == "\n":
            out.append(ch)
    res = "".join(out)
    res = re.sub(r"[ \t]+", " ", res)
    res = re.sub(r" +([,.\!\?:;])", r"\1", res)
    return res.strip()


def wrap_sh2_dialogue(text: str, max_cols: int = 40) -> str:
    """Word-wrap English text to Covenant's 40-column dialogue box using `$` line breaks.

    Preserves explicit `$` breaks when lines are already <= max_cols (e.g. menu choices
    or speaker headers), and wraps long sentences cleanly on word boundaries.
    """
    text = to_us_font_ascii(text)
    if not text:
        return "..."

    # Preserve raw `\n` lines separately from `$` dialogue lines
    if "\n" in text and "$" not in text:
        return "\n".join(wrap_sh2_dialogue(part, max_cols) for part in text.split("\n"))

    # If the source had short distinct `$`-separated lines (like speaker name / Yes$No choices),
    # keep those explicit breaks and only wrap individual segments exceeding max_cols.
    segments = text.split("$")
    # If there are multiple long wrapped JP prose lines joined by `$`, re-flow them cleanly
    if len(segments) >= 2 and all(len(seg.strip()) >= 14 for seg in segments[:-1]):
        joined = " ".join(seg.strip() for seg in segments if seg.strip())
        segments = [joined]

    out_lines: List[str] = []
    for seg in segments:
        seg = seg.strip()
        if not seg:
            if out_lines:
                out_lines.append("")
            continue
        if len(seg) <= max_cols:
            out_lines.append(seg)
            continue
        words = seg.split(" ")
        cur = ""
        for w in words:
            if not cur:
                cur = w
            elif len(cur) + 1 + len(w) <= max_cols:
                cur += " " + w
            else:
                out_lines.append(cur)
                cur = w
        if cur:
            out_lines.append(cur)

    return "$".join(out_lines) if out_lines else "..."


def _protect_tokens(jp_text: str) -> Tuple[str, List[str]]:
    """Replace engine control tokens & Shadow Hearts proper nouns with safe placeholders."""
    tokens: List[str] = []

    def _sub_ctrl(m: re.Match) -> str:
        idx = len(tokens)
        tokens.append(m.group(0))
        return f" XCTRL{idx}X "

    # Preserve explicit `$` choice/line boundaries as soft spaces during prose translation,
    # unless it's a very short 2-line choice (e.g. Yes$No).
    work = jp_text
    if "$" in work:
        parts = work.split("$")
        if len(parts) <= 3 and all(len(p.strip()) <= 10 for p in parts):
            work = work.replace("$", " XBRK ")
        else:
            work = "".join(p.strip() for p in parts)

    work = _CTRL_RE.sub(_sub_ctrl, work)
    for jp, en in _SH2_PROPER_NOUNS:
        if jp in work:
            idx = len(tokens)
            tokens.append(en)
            work = work.replace(jp, f" XNAME{idx}X ")
    return work.strip(), tokens


def _restore_tokens(en_text: str, tokens: List[str]) -> str:
    out = en_text
    out = re.sub(r"\s*XBRK\s*", "$", out, flags=re.IGNORECASE)
    for idx, orig in enumerate(tokens):
        out = re.sub(rf"\bX(?:CTRL|NAME){idx}X\b", lambda _m, v=orig: v, out, flags=re.IGNORECASE)
    return out


def _call_translate_endpoint(text: str, timeout: float = 12.0) -> Optional[str]:
    """Translate a single Japanese text block to English using the public HTTP translation endpoint."""
    q = urllib.parse.urlencode({"client": "gtx", "sl": "ja", "tl": "en", "dt": "t", "q": text})
    url = f"https://translate.googleapis.com/translate_a/single?{q}"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8", errors="replace"))
    if not isinstance(data, list) or not data or not isinstance(data[0], list):
        return None
    return "".join(seg[0] for seg in data[0] if isinstance(seg, list) and seg and isinstance(seg[0], str))


def _translate_chunk(items: List[Tuple[str, bytes]]) -> Dict[str, str]:
    """Translate a small batch of `(hash, raw_sjis)` pairs in one HTTP call with per-item fallback."""
    if not items:
        return {}
    prepared: List[Tuple[str, str, List[str]]] = []
    for h, raw in items:
        jp = raw.decode("cp932", errors="ignore")
        prot, toks = _protect_tokens(jp)
        prepared.append((h, prot, toks))

    # Join batch with numbered markers that survive translation reliably
    joined = "\n".join(f"[{i}] {prot}" for i, (_, prot, _) in enumerate(prepared))
    result: Dict[str, str] = {}
    for attempt in range(3):
        try:
            translated = _call_translate_endpoint(joined)
            if not translated:
                continue
            # Split back on [i] markers
            parts = re.split(r"(?:^|\n)\s*\[(\d+)\]\s*", translated)
            mapped: Dict[int, str] = {}
            for k in range(1, len(parts) - 1, 2):
                idx = int(parts[k])
                mapped[idx] = parts[k + 1].strip()
            if len(mapped) == len(prepared) and all(i in mapped for i in range(len(prepared))):
                for i, (h, _, toks) in enumerate(prepared):
                    restored = _restore_tokens(mapped[i], toks)
                    result[h] = wrap_sh2_dialogue(restored, max_cols=40)
                return result
        except Exception:
            time.sleep(0.4 * (attempt + 1))

    # Fallback: translate individually if batch marker alignment differed
    for h, prot, toks in prepared:
        for attempt in range(2):
            try:
                tr = _call_translate_endpoint(prot)
                if tr:
                    result[h] = wrap_sh2_dialogue(_restore_tokens(tr, toks), max_cols=40)
                    break
            except Exception:
                time.sleep(0.3 * (attempt + 1))
    return result


def build_translation_map(
    raw_sjis_strings: Iterable[bytes],
    cache_path: str,
    packs_dir: str = CURATED_PACKS_DIR,
    allow_online: bool = True,
    max_workers: int = 6,
) -> Dict[str, str]:
    """Resolve English translations for all unique Shift-JIS byte strings.

    Returns `{hash16: english_ascii_40col}`. Curated packs always override cached/auto entries.
    """
    unique: Dict[str, bytes] = {}
    for raw in raw_sjis_strings:
        if raw and any(b >= 0x80 for b in raw):
            h = string_hash(raw)
            if h not in unique:
                unique[h] = raw

    curated = load_curated_packs(packs_dir)
    cache = load_local_cache(cache_path)
    final_map: Dict[str, str] = {}

    missing: List[Tuple[str, bytes]] = []
    for h, raw in unique.items():
        if h in curated:
            final_map[h] = wrap_sh2_dialogue(curated[h], max_cols=40)
        elif h in cache:
            final_map[h] = cache[h]
        else:
            missing.append((h, raw))

    if missing and allow_online:
        # Quick connectivity check on first item so offline users never stall
        try:
            probe = _translate_chunk(missing[:1])
        except Exception:
            probe = {}
        if probe:
            for h, en in probe.items():
                cache[h] = en
                final_map[h] = en
            remaining = missing[1:]
            # Pack into ~12-string batches (~900 chars each)
            batches: List[List[Tuple[str, bytes]]] = []
            cur_batch: List[Tuple[str, bytes]] = []
            cur_chars = 0
            for item in remaining:
                est = len(item[1]) // 2 + 12
                if cur_batch and (len(cur_batch) >= 12 or cur_chars + est > 900):
                    batches.append(cur_batch)
                    cur_batch = []
                    cur_chars = 0
                cur_batch.append(item)
                cur_chars += est
            if cur_batch:
                batches.append(cur_batch)

            total_new = len(missing)
            done_new = len(probe)
            print(
                f"        Translating {total_new:,} unique DC dialogue strings to English "
                f"({len(batches)} batches, cached in dc_en_cache.json)..."
            )
            last_save = time.monotonic()
            with ThreadPoolExecutor(max_workers=max_workers) as ex:
                futs = [ex.submit(_translate_chunk, b) for b in batches]
                for fu in as_completed(futs):
                    try:
                        chunk_res = fu.result()
                    except Exception:
                        chunk_res = {}
                    for h, en in chunk_res.items():
                        cache[h] = en
                        final_map[h] = en
                    done_new += len(chunk_res)
                    if time.monotonic() - last_save >= 4.0:
                        save_local_cache(cache_path, cache)
                        print(f"        [Translate] {done_new:,} / {total_new:,} unique strings translated...", flush=True)
                        last_save = time.monotonic()
            save_local_cache(cache_path, cache)
            print(f"        [Translate] Done: {done_new:,} / {total_new:,} unique strings translated & cached!")
        else:
            print("        [Translate] Offline or translation endpoint unavailable — using US-font sanitizer fallback.")

    return final_map
