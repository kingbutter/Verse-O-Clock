#!/usr/bin/env python3
"""
VerseOClock v3 builder (flat binary assets, Unishox2 compression, no SQLite).

Outputs (to ./verseoclock_v3_unishox2_assets):
  - books.bin   : book names (utf-8, length-prefixed)
  - toc.bin     : 1357 records: (uint32 entry_offset, uint16 count)
  - entries.bin : N records: (u16 book_id, u16 chapter, u16 verse, u32 text_off, u16 text_c_len, u16 text_len)
  - texts.bin   : concatenated Unishox2-compressed verse texts

Time slots follow your project convention:
  hour = chapter  (1..23)
  minute = verse  (1..59)
  slot_index = (hour-1)*59 + (minute-1)  => SLOT_COUNT = 23*59 = 1357
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys
import time
import urllib.request
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple, Any, Optional

BOOKS_URL = "https://raw.githubusercontent.com/aruljohn/Bible-KJV/master/Books.json"
DATA_BASE = "https://raw.githubusercontent.com/aruljohn/Bible-KJV/master/"
OUT_DIR = Path("data")
SUMMARY_PATH = Path("verseoclock_v3_unishox2_summary.txt")

HOURS = list(range(1, 24))      # 01..23
MINUTES = list(range(1, 60))    # 01..59
SLOT_COUNT = len(HOURS) * len(MINUTES)  # 1357


# --------------------------
# Standalone scoring + backup pool
# --------------------------

PRIMARY_PER_SLOT = 10
MIN_SCORE_KEEP = 60.0
MIN_SCORE_BACKUP = 75.0
MIN_BACKUP_POOL = 250  # keep a decent pool for deterministic fills

BAD_PREFIXES = (
    "the children of", "and the children of",
    "the sons of", "and the sons of",
    "the daughters of", "the daughter of",
    "these are", "now these are",
    "the number of", "and the number of",
    "the names of", "and the names of",
)

CONTINUATION_PREFIXES = (
    "and ", "but ", "for ", "therefore ", "wherefore ",
    "then ", "also ", "moreover ", "nevertheless ",
    "now ", "behold ",
)

# Keep this small to start; expand later if you want.
NOTABLE_REFS = [
    ("Genesis", 1, 1),
    ("Psalms", 23, 1),
    ("Psalms", 23, 4),
    ("Psalms", 46, 10),
    ("Proverbs", 3, 5),
    ("Isaiah", 40, 31),
    ("Jeremiah", 29, 11),
    ("Micah", 6, 8),
    ("Matthew", 11, 28),
    ("John", 3, 16),
    ("John", 14, 6),
    ("Romans", 8, 28),
    ("Philippians", 4, 13),
    ("Revelation", 21, 4),
]

def _norm_book(s: str) -> str:
    s = s.lower()
    s = re.sub(r"[^a-z0-9]+", "", s)
    # repo uses "Psalms"
    return s

def _find_book_id_by_name(books: list, wanted: str) -> int:
    wn = _norm_book(wanted)
    for i, b in enumerate(books, start=1):  # your script uses book_id starting at 1
        if _norm_book(b) == wn:
            return i
    # fallback: substring-ish match
    for i, b in enumerate(books, start=1):
        bn = _norm_book(b)
        if wn in bn or bn in wn:
            return i
    return -1

def deterministic_pick(n: int, key: str) -> int:
    h = hashlib.md5(key.encode("utf-8")).hexdigest()
    return int(h[:8], 16) % n

def standalone_score(text: str) -> float:
    t = text.strip()
    tl = t.lower()

    if len(t) < 25:
        return 0.0
    if tl.startswith(BAD_PREFIXES):
        return 0.0

    score = 50.0

    wc = len(t.split())
    if wc >= 10: score += 10
    if wc >= 14: score += 6
    if wc >= 18: score += 4
    if wc < 7: score -= 25

    if tl.startswith(CONTINUATION_PREFIXES):
        score -= 12
        if len(t) >= 90:
            score += 6

    punct = t.count(",") + t.count(";") + t.count(":")
    ofs = tl.count(" of ")
    if punct >= 4: score -= 10
    if punct >= 7: score -= 10
    if ofs >= 4: score -= 10

    if sum(c.isdigit() for c in t) >= 4:
        score -= 10

    if any(p in t for p in [".", "!", "?", "—"]):
        score += 6

    if any(v in tl for v in (" is ", " are ", " was ", " were ", " hath ", " has ", " have ", " shall ", " will ", " said ", " saith ")):
        score += 8

    if score < 0: score = 0.0
    if score > 100: score = 100.0
    return score


def slot_index(h: int, m: int) -> int:
    return (h - 1) * 59 + (m - 1)


def hhmm_from_slot(si: int) -> str:
    h = (si // 59) + 1
    m = (si % 59) + 1
    return f"{h:02d}:{m:02d}"


def http_get_bytes(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "VerseOClockBuilder/3"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def fetch_json(url: str) -> Any:
    return json.loads(http_get_bytes(url).decode("utf-8"))


def normalize_spaces(s: str) -> str:
    return re.sub(r"\s+", " ", s.strip())


def slug_basic(book_name: str) -> str:
    # Keep letters/numbers, turn separators into underscores
    s = normalize_spaces(book_name)
    s = re.sub(r"[^A-Za-z0-9]+", "_", s)
    return s.strip("_")


def candidate_book_filenames(book_name: str) -> List[str]:
    """
    Try a handful of filename patterns that appear in the aruljohn/Bible-KJV repo.
    """
    base = slug_basic(book_name)
    candidates = []
    # Common patterns
    candidates.append(f"{base}.json")
    candidates.append(f"{base}_json.json")  # just in case
    candidates.append(f"{base.lower()}.json")
    candidates.append(f"{base.replace('_', '')}.json")
    candidates.append(f"{base.lower().replace('_', '')}.json")
    # Also try removing spaces only
    nospace = re.sub(r"\s+", "", normalize_spaces(book_name))
    candidates.append(f"{nospace}.json")
    candidates.append(f"{nospace.lower()}.json")
    # de-dupe preserving order
    seen = set()
    out = []
    for c in candidates:
        if c not in seen:
            out.append(c)
            seen.add(c)
    return out


def resolve_book_url(book_name: str) -> str:
    """
    Probe candidate URLs until one returns JSON.
    """
    for fn in candidate_book_filenames(book_name):
        url = DATA_BASE + fn
        try:
            b = http_get_bytes(url, timeout=15)
            # basic sanity check
            if b and b.lstrip().startswith(b"{"):
                return url
        except Exception:
            continue
    raise FileNotFoundError(f"Could not resolve URL for book {book_name!r}")


def extract_text(verse_obj: Any) -> str:
    """
    aruljohn/Bible-KJV structure commonly uses: {"verse": "In the beginning..."}
    """
    # Supported shapes seen in aruljohn/Bible-kjv:
    #   1) {"1": "In the beginning..."}
    #   2) {"verse": 1, "text": "In the beginning..."}
    #   3) {"verse": "In the beginning..."}  (older/simple variants)
    #   4) "In the beginning..." (string)
    if isinstance(verse_obj, dict):
        # shape (2)
        t = verse_obj.get("text")
        if isinstance(t, str) and t.strip():
            return normalize_spaces(t)

        # shape (3)
        v = verse_obj.get("verse")
        if isinstance(v, str) and v.strip():
            return normalize_spaces(v)

        # shape (1): single-key dict mapping verse number to text
        if len(verse_obj) == 1:
            only_val = next(iter(verse_obj.values()))
            if isinstance(only_val, str) and only_val.strip():
                return normalize_spaces(only_val)

        # as a last resort, find first string value
        for val in verse_obj.values():
            if isinstance(val, str) and val.strip():
                return normalize_spaces(val)

    elif isinstance(verse_obj, str):
        return normalize_spaces(verse_obj)

    return ""


def extract_verse_num(verse_obj: Any, fallback_index: int) -> int:
    """Return verse number for a verse object; fallback to the list index (1-based)."""
    if isinstance(verse_obj, dict):
        # shape (2)
        v = verse_obj.get("verse")
        if isinstance(v, int):
            return v
        if isinstance(v, str) and v.isdigit():
            return int(v)
        # shape (1): single key
        if len(verse_obj) == 1:
            k = next(iter(verse_obj.keys()))
            if isinstance(k, str) and k.isdigit():
                return int(k)
            if isinstance(k, int):
                return k
    return fallback_index


class Unishox2Codec:
    """
    Wrapper around a Python Unishox2 implementation.

    Supports:
      - unishox2-py3 (pip install unishox2-py3) which typically exposes module `unishox2`
        with functions compress(text)->bytes and decompress(bytes)->str
    """
    def __init__(self, mod):
        self.mod = mod

    @staticmethod
    def load() -> Optional["Unishox2Codec"]:
        try:
            import unishox2  # from unishox2-py3
        except Exception:
            return None
        return Unishox2Codec(unishox2)

    def compress(self, text: str) -> bytes:
        # Try common APIs
        m = self.mod
        if hasattr(m, "compress"):
            c = m.compress(text)
            # some variants might return (bytes, origlen)
            if isinstance(c, tuple) and len(c) >= 1:
                c = c[0]
            if isinstance(c, (bytes, bytearray)):
                return bytes(c)
        # Some libs expose a class Unishox2
        if hasattr(m, "Unishox2"):
            inst = m.Unishox2()
            c = inst.compress(text)
            if isinstance(c, tuple) and len(c) >= 1:
                c = c[0]
            if isinstance(c, (bytes, bytearray)):
                return bytes(c)
        raise RuntimeError("Unishox2 module loaded but compress() API not recognized.")

    def decompress(self, data: bytes) -> str:
        m = self.mod
        if hasattr(m, "decompress"):
            s = m.decompress(data)
            if isinstance(s, str):
                return s
        if hasattr(m, "Unishox2"):
            inst = m.Unishox2()
            s = inst.decompress(data)
            if isinstance(s, str):
                return s
        raise RuntimeError("Unishox2 module loaded but decompress() API not recognized.")


@dataclass
class VerseEntry:
    book_id: int
    chapter: int
    verse: int
    text_c_off: int
    text_c_len: int
    text_len: int  # original UTF-8 length (+1 for null)


def write_books_bin(book_names: List[str], out_path: Path) -> None:
    """
    Format:
      uint16 count
      repeated:
        uint16 utf8_len
        utf8 bytes
    """
    with out_path.open("wb") as f:
        f.write(struct.pack("<H", len(book_names)))
        for name in book_names:
            b = name.encode("utf-8")
            f.write(struct.pack("<H", len(b)))
            f.write(b)


def main() -> int:
    t0 = time.time()
    print("Downloading Books.json ...")
    books = fetch_json(BOOKS_URL)
    if not isinstance(books, list) or not all(isinstance(b, str) for b in books):
        print("ERROR: Books.json not a list of strings.")
        return 2

    codec = Unishox2Codec.load()
    if codec is None:
        print("ERROR: Python module 'unishox2' not found. Install with: pip install unishox2-py3")
        return 3

    # slot_entries[slot] -> list of (book_id, chapter, verse, text)
    slot_entries: List[List[Tuple[int, int, int, str]]] = [[] for _ in range(SLOT_COUNT)]

    scanned = 0
    candidates = 0
    resolved = 0

    for book_id, book_name in enumerate(books, start=1):
        try:
            url = resolve_book_url(book_name)
            resolved += 1
        except Exception as e:
            print(f"[warn] skip book {book_name!r}: {e}")
            continue

        data = fetch_json(url)
        if not isinstance(data, dict):
            continue

        chapters = data.get("chapters")
        if not isinstance(chapters, list):
            continue

        # aruljohn/Bible-kjv commonly uses:
        #   chapters: [ {"chapter":"1", "verses":[{"1":"..."}, ...]}, ... ]
        # but we also support a simpler shape:
        #   chapters: [ ["verse1", "verse2", ...], ... ]
        for ci_idx, chapter_obj in enumerate(chapters, start=1):
            # Determine chapter number + verse list for either shape
            if isinstance(chapter_obj, dict) and "verses" in chapter_obj:
                ch_raw = chapter_obj.get("chapter", ci_idx)
                try:
                    ch = int(ch_raw)
                except Exception:
                    ch = ci_idx
                verses_list = chapter_obj.get("verses")
            else:
                ch = ci_idx
                verses_list = chapter_obj

            if ch not in HOURS:
                continue
            if not isinstance(verses_list, list):
                continue

            for vi_idx, verse_obj in enumerate(verses_list, start=1):
                scanned += 1
                vi = extract_verse_num(verse_obj, vi_idx)
                if vi not in MINUTES:
                    continue
                text = extract_text(verse_obj)
                if not text:
                    continue
                candidates += 1
                si = slot_index(ch, vi)
                slot_entries[si].append((book_id, ch, vi, text))

    # --------------------------
    # Score & pick best per slot + backup fallback
    # --------------------------

    # Build notable verse map from scanned candidates (ref -> entry)
    # ref is (book_id, chapter, verse)
    notable_found = {}
    for si in range(SLOT_COUNT):
        for (bid, ch, vs, text) in slot_entries[si]:
            notable_found.setdefault((bid, ch, vs), (bid, ch, vs, text))

    # Build backup pool: notable verses first, then high-scoring verses from anywhere
    backup_pool = []

    # Notable seeds (if present in your scanned set)
    for (bname, ch, vs) in NOTABLE_REFS:
        bid = _find_book_id_by_name(books, bname)
        if bid != -1:
            e = notable_found.get((bid, ch, vs))
            if e:
                s = standalone_score(e[3])
                if s >= 50.0:
                    backup_pool.append((s, e))

    # High-quality extras
    for si in range(SLOT_COUNT):
        for e in slot_entries[si]:
            s = standalone_score(e[3])
            if s >= MIN_SCORE_BACKUP:
                backup_pool.append((s, e))

    # Deduplicate backup pool by verse ref
    backup_pool.sort(key=lambda x: x[0], reverse=True)
    seen = set()
    deduped = []
    for s, e in backup_pool:
        ref = (e[0], e[1], e[2])
        if ref in seen:
            continue
        seen.add(ref)
        deduped.append((s, e))
    backup_pool = deduped[:max(MIN_BACKUP_POOL, len(deduped))]

    print(f"[backup] pool size: {len(backup_pool)}")

    # Now score slots and keep best N; then fill empty slots from backup_pool
    new_slot_entries = [[] for _ in range(SLOT_COUNT)]
    emptied = 0

    for si in range(SLOT_COUNT):
        scored = [(standalone_score(e[3]), e) for e in slot_entries[si]]
        scored.sort(key=lambda x: x[0], reverse=True)
        kept = [e for (s, e) in scored if s >= MIN_SCORE_KEEP][:PRIMARY_PER_SLOT]
        new_slot_entries[si] = kept

    for si in range(SLOT_COUNT):
        if new_slot_entries[si]:
            continue
        emptied += 1
        if backup_pool:
            idx = deterministic_pick(len(backup_pool), f"slot:{si}")
            new_slot_entries[si] = [backup_pool[idx][1]]

    slot_entries = new_slot_entries
    print(f"[filter] filled empty slots from backup: {emptied}")


    OUT_DIR.mkdir(parents=True, exist_ok=True)

    books_path = OUT_DIR / "books.bin"
    toc_path = OUT_DIR / "toc.bin"
    entries_path = OUT_DIR / "entries.bin"
    texts_path = OUT_DIR / "texts.bin"

    print("Writing books.bin ...")
    write_books_bin(books, books_path)

    print("Compressing texts and writing entries/texts ...")
    entry_records: List[VerseEntry] = []
    texts_blob = bytearray()

    toc: List[Tuple[int, int]] = []
    for si in range(SLOT_COUNT):
        entries_here = slot_entries[si]
        entry_off = len(entry_records)
        for (book_id, ch, vs, text) in entries_here:
            # Store UTF-8 length (+1 for null terminator for C string)
            text_utf8 = text.encode("utf-8")
            orig_len = len(text_utf8) + 1

            c = codec.compress(text)
            off = len(texts_blob)
            texts_blob.extend(c)

            entry_records.append(
                VerseEntry(
                    book_id=book_id,
                    chapter=ch,
                    verse=vs,
                    text_c_off=off,
                    text_c_len=len(c),
                    text_len=orig_len,
                )
            )
        toc.append((entry_off, len(entries_here)))

    with texts_path.open("wb") as f:
        f.write(texts_blob)

    # toc: SLOT_COUNT records, each: uint32 entry_offset, uint16 count
    with toc_path.open("wb") as f:
        for off, cnt in toc:
            f.write(struct.pack("<IH", off, cnt))

    # entries: each record:
    #   uint16 book_id, uint16 chapter, uint16 verse,
    #   uint32 text_c_off, uint16 text_c_len, uint16 text_len
    with entries_path.open("wb") as f:
        for e in entry_records:
            f.write(
                struct.pack(
                    "<HHHIHH",
                    e.book_id,
                    e.chapter,
                    e.verse,
                    e.text_c_off,
                    e.text_c_len,
                    e.text_len,
                )
            )

    filled = sum(1 for _, cnt in toc if cnt > 0)
    missing = [hhmm_from_slot(i) for i, (_, cnt) in enumerate(toc) if cnt == 0]

    with SUMMARY_PATH.open("w", encoding="utf-8") as f:
        f.write("VerseOClock v3 (flat files + Unishox2) summary\n")
        f.write("=========================================\n")
        f.write(f"[books] resolved: {resolved} / {len(books)}\n")
        f.write(f"[scan] total verses scanned (chapters/verses limited by time rules): {scanned}\n")
        f.write(f"[scan] candidate matches seen: {candidates}\n")
        f.write(f"[scan] unique times filled: {filled} / {SLOT_COUNT}\n")
        f.write(f"[scan] missing times: {len(missing)}\n")
        if missing:
            f.write("[scan] first 50 missing: " + ", ".join(missing[:50]) + "\n")
        f.write("\n")
        f.write(f"[out] books.bin:   {books_path.stat().st_size} bytes\n")
        f.write(f"[out] toc.bin:     {toc_path.stat().st_size} bytes\n")
        f.write(f"[out] entries.bin: {entries_path.stat().st_size} bytes\n")
        f.write(f"[out] texts.bin:   {texts_path.stat().st_size} bytes\n")
        f.write(f"[time] elapsed: {time.time()-t0:.1f}s\n")

    print("DONE")
    print(f"  books.bin:   {books_path.stat().st_size} bytes")
    print(f"  toc.bin:     {toc_path.stat().st_size} bytes")
    print(f"  entries.bin: {entries_path.stat().st_size} bytes")
    print(f"  texts.bin:   {texts_path.stat().st_size} bytes")
    print(f"  summary:     {SUMMARY_PATH}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
