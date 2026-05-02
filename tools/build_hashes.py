"""Build resources/data/hashes.bin from a wordlist.

Format: sorted ascending list of little-endian uint32 values, where each
value is the first 4 bytes of SHA-256(word_lowercase_as_UTF-16-LE_bytes).
Matches the lookup convention used by the original Fitbit Letterfall
implementation and by main.c::word_is_valid().
"""

from __future__ import annotations

import hashlib
import re
import struct
import sys
from pathlib import Path

WORD_RE = re.compile(r"^[A-Za-z]{3,}$")


def iter_words(source: Path):
    seen: set[str] = set()
    with source.open("r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            word = line.strip().lower()
            if not WORD_RE.fullmatch(word):
                continue
            if word in seen:
                continue
            seen.add(word)
            yield word


def hash_prefix(word: str) -> int:
    payload = word.encode("utf-16-le")
    digest = hashlib.sha256(payload).digest()
    return int.from_bytes(digest[:4], "little")


def main() -> None:
    source = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/dict/words")
    target = Path(__file__).resolve().parents[1] / "resources" / "data" / "hashes.bin"

    hashes = sorted({hash_prefix(w) for w in iter_words(source)})
    with target.open("wb") as fh:
        for h in hashes:
            fh.write(struct.pack("<I", h))

    size_kb = target.stat().st_size / 1024
    print(f"wrote {len(hashes)} entries -> {target} ({size_kb:.1f} KB)")


if __name__ == "__main__":
    main()
