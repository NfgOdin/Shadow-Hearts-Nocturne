#!/usr/bin/env python3
"""Shadow Hearts: Covenant — Clean-Room .MSC Script Parser, Dungeon Hooker & US-Font Localizer.

100% original clean-room tooling for Nocturne's Director's Cut upgrade (v1.1):
  - Parses and rebuilds Shadow Hearts: Covenant `.MSC` script containers
    (`header[32] + EVNT + code + data`, 16-byte qword-aligned segments).
  - Hooks the US retail map scripts into the Director's Cut dungeons cleanly
    without clobbering existing script variables or shifting existing code IPs:
      * `FIELD/MPS/CA_02_U.MSC` -> Nahash Undersea Temple (`KS_00_U`) on first
        boat crossing from Cannes to Sainte-Marguerite (via a 3-byte trampoline
        at `0x2f8c` to an appended branch at the end of the code segment).
      * `FIELD/MPS/ND_04_A.MSC` -> Deeper Neam Ruins (`ND_11_U`) portal.
      * `FIELD/MPS/ND_20_U.MSC` -> Deeper Neam Ruins exit (`ND_10_U`).
  - Sanitizes & localizes Shift-JIS strings inside DC-only `.MSC` scripts into
    clean US-font ASCII (preserving pure-ASCII identifiers & `$` newline / `%`
    format tokens) so zero garbled high-bit glyphs render on `SLUS_210.41`.
"""
from __future__ import annotations

import hashlib
import re
import struct
from typing import Dict, List, Optional, Tuple


class MscFile:
    """Clean-room parser & builder for Shadow Hearts: Covenant `.MSC` scripts."""

    def __init__(self, raw: bytes) -> None:
        if len(raw) < 0x30 or raw[0x20:0x24] != b"EVNT":
            raise ValueError("Invalid .MSC header (missing EVNT chunk at 0x20)")
        self.hdr = list(struct.unpack_from("<16H", raw, 0))
        ev_hdr, ev_n1, ev_u2, ev_total = struct.unpack_from("<4H", raw, 0x24)
        ev_end = 0x20 + ev_hdr + ev_total * 8
        self.code0 = (ev_end + 15) & ~15
        code_sz = self.hdr[4] * 16
        data_sz = self.hdr[5] * 16
        data0 = self.code0 + code_sz
        if data0 + data_sz > len(raw):
            raise ValueError(
                f"Truncated .MSC: code0={self.code0:#x} code={code_sz:#x} data={data_sz:#x} len={len(raw):#x}"
            )
        self.evnt_block = bytearray(raw[0x20 : self.code0])
        self.code = bytearray(raw[self.code0 : data0])
        self.data = bytearray(raw[data0 : data0 + data_sz])
        self.tail = raw[data0 + data_sz :]

    def append_data(self, payload: bytes) -> int:
        """Append NUL-terminated bytes to the data segment and return its offset."""
        if not payload.endswith(b"\x00"):
            payload = payload + b"\x00"
        off = len(self.data)
        self.data.extend(payload)
        return off

    def append_code(self, bytecode: bytes) -> int:
        """Append bytecode to the end of the code segment and return its start IP."""
        ip = len(self.code)
        self.code.extend(bytecode)
        return ip

    def serialize(self) -> bytes:
        code_pad = (-len(self.code)) % 16
        data_pad = (-len(self.data)) % 16
        code_out = bytes(self.code) + (b"\x00" * code_pad)
        data_out = bytes(self.data) + (b"\x00" * data_pad)
        hdr = list(self.hdr)
        hdr[4] = len(code_out) // 16
        hdr[5] = len(data_out) // 16
        return struct.pack("<16H", *hdr) + bytes(self.evnt_block) + code_out + data_out + self.tail


# ---------------------------------------------------------------------------
# Clean-Room US -> Director's Cut Dungeon Entry Hooks
# ---------------------------------------------------------------------------


def hook_ca02_nahash(us_msc_bytes: bytes) -> bytes:
    """Hook Nelson's boat in US `CA_02_U.MSC` so the first crossing enters Nahash (`KS_00_U`).

    Appends `"KS_00_U\\0"` to the data segment and appends the DC conditional branch
    after the existing code segment (so zero US script variables or IPs are shifted),
    then installs a 3-byte `jmp` trampoline at `0x2f8c`.
    """
    msc = MscFile(us_msc_bytes)

    # Verify the exact US Sainte-Marguerite boat departure bytecode at 0x2f8c
    sm_off = msc.data.find(b"SM_00_U\x00")
    if sm_off < 0:
        raise ValueError("CA_02_U.MSC: could not find SM_00_U string in data segment")

    expected_us = (
        b"\x0c\x90\x01\x06\x1b\x50\x01\x02"
        + b"\x15"
        + struct.pack("<H", sm_off)
        + b"\x06\x0c\x90\x01\x1b\x45\x06\x03"
        + b"\x90\xb4\x2f"
    )
    hook_ip = msc.code.find(expected_us)
    if hook_ip < 0:
        raise ValueError("CA_02_U.MSC: expected US Sainte-Marguerite boat sequence not found")
    resume_ip = struct.unpack_from("<H", msc.code, hook_ip + len(expected_us) - 2)[0]

    # Append "KS_00_U\0" to data segment (reuse if already present)
    ks_off = msc.data.find(b"KS_00_U\x00")
    if ks_off < 0:
        ks_off = msc.append_data(b"KS_00_U\x00")

    # Build clean DC crossing branch appended at end of code segment:
    #   check flag 0x2a (0a 2a a6 01); if clear (91 <nahash_ip>) -> ChangeMap("KS_00_U")
    #   else -> original US ChangeMap("SM_00_U"); both jump back to resume_ip (0x2fb4).
    branch_start = len(msc.code)
    us_arm = (
        b"\x0c\x90\x01\x06\x1b\x50\x01\x02"
        + b"\x15"
        + struct.pack("<H", sm_off)
        + b"\x06\x0c\x90\x01\x1b\x45\x06\x03"
        + b"\x90"
        + struct.pack("<H", resume_ip)
    )
    nahash_ip = branch_start + 7 + len(us_arm)  # 4 bytes flag check + 3 bytes jz + len(us_arm)
    dc_arm = (
        b"\x0c\xe8\x03\x06\x1b\x50\x01\x02"
        + b"\x15"
        + struct.pack("<H", ks_off)
        + b"\x06\x0c\x90\x01\x1b\x45\x06\x03"
        + b"\x90"
        + struct.pack("<H", resume_ip)
    )
    branch_code = b"\x0a\x2a\xa6\x01\x91" + struct.pack("<H", nahash_ip) + us_arm + dc_arm
    msc.append_code(branch_code)

    # Install 3-byte unconditional jmp trampoline at hook_ip (0x2f8c)
    msc.code[hook_ip : hook_ip + 3] = b"\x90" + struct.pack("<H", branch_start)
    return msc.serialize()


def hook_nd04a_deeper_neam(us_msc_bytes: bytes) -> bytes:
    """Hook Neam Ruins (`ND_04_A.MSC`) portal to enter Deeper Neam Ruins (`ND_11_U`)."""
    msc = MscFile(us_msc_bytes)
    off = msc.data.find(b"ND_20_U\x00")
    if off < 0:
        raise ValueError("ND_04_A.MSC: could not find ND_20_U string")
    ref = b"\x15" + struct.pack("<H", off)
    if msc.code.find(ref) < 0:
        raise ValueError("ND_04_A.MSC: could not verify bytecode reference to ND_20_U")
    msc.data[off : off + 8] = b"ND_11_U\x00"
    return msc.serialize()


def hook_nd20u_deeper_neam_exit(us_msc_bytes: bytes) -> bytes:
    """Hook Deeper Neam Ruins boss chamber (`ND_20_U.MSC`) exit to return to `ND_10_U`."""
    msc = MscFile(us_msc_bytes)
    off = msc.data.find(b"ND_04_A\x00")
    if off < 0:
        raise ValueError("ND_20_U.MSC: could not find ND_04_A string")
    pat = b"\x15" + struct.pack("<H", off) + b"\x08\x19\x0c\x07\x07\x0c\xdc\x05"
    idx = msc.code.find(pat)
    if idx < 0:
        raise ValueError("ND_20_U.MSC: could not verify exit ChangeMap sequence")
    # Patch map name ND_04_A -> ND_10_U (same 7 bytes), entry 08 -> 07, fade 05dc -> 0320
    msc.data[off : off + 8] = b"ND_10_U\x00"
    msc.code[idx + 3] = 0x07
    msc.code[idx + 9 : idx + 11] = b"\x20\x03"
    return msc.serialize()


# ---------------------------------------------------------------------------
# Clean-Room US-Font Shift-JIS -> ASCII Localizer for DC-Only .MSC Scripts
# ---------------------------------------------------------------------------

_JP_TERM_MAP: List[Tuple[str, str]] = [
    ("シャドウハーツ", "Shadow Hearts"),
    ("ディレクターズカット", "Director's Cut"),
    ("アナスタシア", "Anastasia"),
    ("カレン", "Karin"),
    ("ゲパルト", "Gepetto"),
    ("ブランカ", "Blanca"),
    ("ヨアヒム", "Joachim"),
    ("ルチア", "Lucia"),
    ("ベロニカ", "Veronica"),
    ("ニコライ", "Nicolai"),
    ("レニー", "Lenny"),
    ("ゼペット", "Gepetto"),
    ("ウル", "Yuri"),
    ("蔵人", "Kurando"),
    ("ナハシュ", "Nahash"),
    ("樹海", "Sea of Woods"),
    ("ネアム", "Neam"),
    ("カンヌ", "Cannes"),
    ("パリ", "Paris"),
    ("フィレンツェ", "Florence"),
    ("犬神", "Inugami"),
    ("クイズ", "Quiz"),
    ("ジャッジメントリング", "Judgment Ring"),
    ("リング魂", "Ring Soul"),
    ("宝箱", "Treasure Chest"),
    ("セーブ", "Save"),
    ("ロード", "Load"),
    ("はい", "Yes"),
    ("いいえ", "No"),
    ("調べる", "Examine"),
    ("開ける", "Open"),
    ("閉じる", "Close"),
    ("戻る", "Back"),
    ("進む", "Proceed"),
    ("やめる", "Cancel"),
    ("を手に入れた", " obtained!"),
    ("手に入れた", "Obtained"),
    ("鍵", "Key"),
    ("扉", "Door"),
    ("封印", "Seal"),
    ("紋章", "Crest"),
    ("魂", "Soul"),
    ("記憶", "Memory"),
    ("船", "Boat"),
    ("島", "Island"),
    ("神殿", "Temple"),
    ("遺跡", "Ruins"),
    ("森", "Forest"),
    ("犬", "Wolf"),
    ("戦い", "Battle"),
    ("勝利", "Victory"),
    ("正解", "Correct!"),
    ("不正解", "Wrong!"),
]

_PUNCT_MAP = str.maketrans({
    "\u3000": " ", "、": ", ", "。": ". ", "！": "!", "？": "?",
    "「": '"', "」": '"', "『": "'", "』": "'", "（": "(", "）": ")",
    "［": "[", "］": "]", "【": "[", "】": "]", "…": "...", "・": " ",
    "ー": "-", "～": "~", "－": "-", "：": ":", "；": ";", "＋": "+",
    "０": "0", "１": "1", "２": "2", "３": "3", "４": "4",
    "５": "5", "６": "6", "７": "7", "８": "8", "９": "9",
    "Ａ": "A", "Ｂ": "B", "Ｃ": "C", "Ｄ": "D", "Ｅ": "E", "Ｆ": "F",
    "Ｇ": "G", "Ｈ": "H", "Ｉ": "I", "Ｊ": "J", "Ｋ": "K", "Ｌ": "L",
    "Ｍ": "M", "Ｎ": "N", "Ｏ": "O", "Ｐ": "P", "Ｑ": "Q", "Ｒ": "R",
    "Ｓ": "S", "Ｔ": "T", "Ｕ": "U", "Ｖ": "V", "Ｗ": "W", "Ｘ": "X",
    "Ｙ": "Y", "Ｚ": "Z",
    "ａ": "a", "ｂ": "b", "ｃ": "c", "ｄ": "d", "ｅ": "e", "ｆ": "f",
    "ｇ": "g", "ｈ": "h", "ｉ": "i", "ｊ": "j", "ｋ": "k", "ｌ": "l",
    "ｍ": "m", "ｎ": "n", "ｏ": "o", "ｐ": "p", "ｑ": "q", "ｒ": "r",
    "ｓ": "s", "ｔ": "t", "ｕ": "u", "ｖ": "v", "ｗ": "w", "ｘ": "x",
    "ｙ": "y", "ｚ": "z",
})

_KANA_DIGRAPHS: Dict[str, str] = {
    "きゃ": "kya", "きゅ": "kyu", "きょ": "kyo", "しゃ": "sha", "しゅ": "shu", "しょ": "sho",
    "ちゃ": "cha", "ちゅ": "chu", "ちょ": "cho", "にゃ": "nya", "にゅ": "nyu", "にょ": "nyo",
    "ひゃ": "hya", "ひゅ": "hyu", "ひょ": "hyo", "みゃ": "mya", "みゅ": "myu", "みょ": "myo",
    "りゃ": "rya", "りゅ": "ryu", "りょ": "ryo", "ぎゃ": "gya", "ぎゅ": "gyu", "ぎょ": "gyo",
    "じゃ": "ja", "じゅ": "ju", "じょ": "jo", "びゃ": "bya", "びゅ": "byu", "びょ": "byo",
    "ぴゃ": "pya", "ぴゅ": "pyu", "ぴょ": "pyo", "ふぁ": "fa", "ふぃ": "fi", "ふぇ": "fe",
    "ふぉ": "fo", "てぃ": "ti", "でぃ": "di", "ヴぁ": "va", "ヴぃ": "vi", "ヴぇ": "ve", "ヴぉ": "vo",
}

_KANA_SINGLE: Dict[str, str] = {
    "あ": "a", "い": "i", "う": "u", "え": "e", "お": "o",
    "か": "ka", "き": "ki", "く": "ku", "け": "ke", "こ": "ko",
    "さ": "sa", "し": "shi", "す": "su", "せ": "se", "そ": "so",
    "た": "ta", "ち": "chi", "つ": "tsu", "て": "te", "と": "to",
    "な": "na", "に": "ni", "ぬ": "nu", "ね": "ne", "の": "no",
    "は": "ha", "ひ": "hi", "ふ": "fu", "へ": "he", "ほ": "ho",
    "ま": "ma", "み": "mi", "む": "mu", "め": "me", "も": "mo",
    "や": "ya", "ゆ": "yu", "よ": "yo",
    "ら": "ra", "り": "ri", "る": "ru", "れ": "re", "ろ": "ro",
    "わ": "wa", "を": "o", "ん": "n",
    "が": "ga", "ぎ": "gi", "ぐ": "gu", "げ": "ge", "ご": "go",
    "ざ": "za", "じ": "ji", "ず": "zu", "ぜ": "ze", "ぞ": "zo",
    "だ": "da", "ぢ": "ji", "づ": "zu", "で": "de", "ど": "do",
    "ば": "ba", "び": "bi", "ぶ": "bu", "べ": "be", "ぼ": "bo",
    "ぱ": "pa", "ぴ": "pi", "ぷ": "pu", "ぺ": "pe", "ぽ": "po",
    "ぁ": "a", "ぃ": "i", "ぅ": "u", "ぇ": "e", "ぉ": "o",
    "ゃ": "ya", "ゅ": "yu", "ょ": "yo", "っ": "", "ヴ": "vu",
}


def _katakana_to_hiragana(s: str) -> str:
    out = []
    for ch in s:
        cp = ord(ch)
        if 0x30A1 <= cp <= 0x30F6:
            out.append(chr(cp - 0x60))
        else:
            out.append(ch)
    return "".join(out)


def _jp_to_us_ascii(text: str) -> str:
    """Convert Shift-JIS decoded text into clean US-font printable ASCII (preserving `$` / `%`)."""
    s = text.translate(_PUNCT_MAP)
    for jp, en in _JP_TERM_MAP:
        if jp in s:
            s = s.replace(jp, f" {en} ")
    s = _katakana_to_hiragana(s)

    out: List[str] = []
    i = 0
    n = len(s)
    kanji_run = False
    while i < n:
        ch = s[i]
        if ord(ch) < 0x80:
            if 0x20 <= ord(ch) <= 0x7E or ch in ("\n", "\r", "\t"):
                out.append(ch)
            kanji_run = False
            i += 1
            continue
        if i + 1 < n and s[i : i + 2] in _KANA_DIGRAPHS:
            out.append(_KANA_DIGRAPHS[s[i : i + 2]])
            kanji_run = False
            i += 2
            continue
        if ch == "っ" and i + 1 < n:
            nxt = _KANA_DIGRAPHS.get(s[i + 1 : i + 3], _KANA_SINGLE.get(s[i + 1], ""))
            if nxt and nxt[0].isalpha():
                out.append(nxt[0])
            kanji_run = False
            i += 1
            continue
        if ch in _KANA_SINGLE:
            out.append(_KANA_SINGLE[ch])
            kanji_run = False
            i += 1
            continue
        # Remaining CJK kanji without glossary match -> clean ASCII ellipsis/placeholder (no garbled glyphs)
        if not kanji_run:
            out.append("...")
            kanji_run = True
        i += 1

    res = re.sub(r"[ \t]+", " ", "".join(out)).strip()
    res = re.sub(r" *\$ *", "$", res)
    return res or "..."


def collect_sjis_strings(dc_msc_bytes: bytes) -> List[bytes]:
    """Return all non-empty Shift-JIS byte strings in a `.MSC` data segment."""
    msc = MscFile(dc_msc_bytes)
    out: List[bytes] = []
    pos = 0
    end_lim = len(msc.data)
    while pos < end_lim:
        end = msc.data.find(b"\x00", pos)
        if end < 0:
            break
        raw = bytes(msc.data[pos:end])
        if raw and any(b >= 0x80 for b in raw):
            out.append(raw)
        pos = end + 1
    return out


def sanitize_dc_msc_for_us_font(
    dc_msc_bytes: bytes,
    translations: Optional[Dict[str, str]] = None,
) -> Tuple[bytes, int]:
    """Localize all Shift-JIS strings inside a DC-only `.MSC` so it renders cleanly on US `SLUS_210.41`.

    Uses `translations` (`{md5_16: english_ascii}`) when provided, falling back to the
    offline US-font ASCII sanitizer for any unmapped string. Preserves 100% of pure-ASCII
    identifiers (map names, SE/BGM/model strings, format codes).
    Replaces strings in-place when they fit (NUL-padded, zero bytecode changes), or
    appends longer replacements to `data` and repoints exact `0x15 <u16>` pushes.
    Returns `(patched_bytes, localized_count)`.
    """
    msc = MscFile(dc_msc_bytes)
    orig_data_len = len(msc.data)
    localized = 0
    pos = 0
    while pos < orig_data_len:
        end = msc.data.find(b"\x00", pos)
        if end < 0:
            break
        raw = bytes(msc.data[pos:end])
        if raw and any(b >= 0x80 for b in raw):
            h16 = hashlib.md5(raw).hexdigest()[:16]
            if translations and h16 in translations:
                clean_ascii = translations[h16].encode("ascii", errors="ignore")
            else:
                try:
                    decoded = raw.decode("cp932", errors="ignore")
                except Exception:
                    decoded = ""
                clean_ascii = _jp_to_us_ascii(decoded).encode("ascii", errors="ignore")
            if not clean_ascii:
                clean_ascii = b"..."
            if len(clean_ascii) <= len(raw):
                # Fits in-place: zero bytecode modifications needed!
                msc.data[pos : pos + len(clean_ascii)] = clean_ascii
                msc.data[pos + len(clean_ascii) : end] = b"\x00" * (len(raw) - len(clean_ascii))
            else:
                # Append to data segment and repoint exact 0x15 <u16 pos> pushes
                new_off = msc.append_data(clean_ascii + b"\x00")
                if new_off <= 0xFFFF:
                    old_pat = b"\x15" + struct.pack("<H", pos)
                    new_pat = b"\x15" + struct.pack("<H", new_off)
                    cpos = 0
                    while True:
                        hit = msc.code.find(old_pat, cpos)
                        if hit < 0:
                            break
                        msc.code[hit : hit + 3] = new_pat
                        cpos = hit + 3
                    # Also write truncated ASCII fallback into the old slot so no indirect ref sees Shift-JIS
                    fb = clean_ascii[: len(raw)]
                    msc.data[pos : pos + len(fb)] = fb
                    msc.data[pos + len(fb) : end] = b"\x00" * (len(raw) - len(fb))
                else:
                    # Fallback truncate to fit in-place if data exceeds 64 KiB
                    trunc = clean_ascii[: len(raw)]
                    msc.data[pos : pos + len(trunc)] = trunc
                    msc.data[pos + len(trunc) : end] = b"\x00" * (len(raw) - len(trunc))
            localized += 1
        pos = end + 1

    return (msc.serialize(), localized) if localized else (dc_msc_bytes, 0)
