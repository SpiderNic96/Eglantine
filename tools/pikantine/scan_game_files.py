#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Look through a dumped Pokémon Pokopia (romfs, exefs, update) for its Pokédex table.

The in-game Pokédex starts Bulbasaur, Charmander, Charizard, Squirtle, ..., Pidgey, Oddish. In the
save those Pokémon are residents 32, 51, 16, 17 (... 273, 149) and their portraits are 2, 5, 7, 8
(... 11, 28). A table that lists them in that order, as 16- or 32-bit numbers in records of any
size, is very likely the Pokédex: the script finds every such table, in plain files and inside
the zlib streams the game packs files in, and writes what it finds to scan_report.txt.

It also writes an inventory of every file (size, magic) and lists files mentioning dex words.

    pip install numpy
    python scan_game_files.py "<folder with the dumped game>" [report file]
"""
import sys
import zlib
from pathlib import Path

import numpy as np

SEQUENCES = {
    "resident ids (Bulbasaur, Charmander, Charizard, Squirtle)": [32, 51, 16, 17],
    "portrait ids (Bulbasaur, Charmander, Charizard, Squirtle)": [2, 5, 7, 8],
    "National Dex (Bulbasaur, Charmander, Charizard, Squirtle)": [1, 4, 6, 7],
}
FOLLOW = {  # one silhouette after Squirtle, then Pidgey and Oddish
    "resident ids (Bulbasaur, Charmander, Charizard, Squirtle)": (273, 149),
    "portrait ids (Bulbasaur, Charmander, Charizard, Squirtle)": (11, 28),
    "National Dex (Bulbasaur, Charmander, Charizard, Squirtle)": (16, 43),
}
WORDS = [b"dex", b"Dex", b"DEX", b"zukan", b"Zukan", b"ZUKAN", b"register", b"Register"]
MAX_STRIDE = 256
ZLIB_HEADERS = (b"\x78\x01", b"\x78\x5e", b"\x78\x9c", b"\x78\xda")


def blobs(data: bytes):
    """The file itself, then every zlib stream inside it that inflates to something sizeable."""
    yield "raw", data
    seen = 0
    for header in ZLIB_HEADERS:
        start = 0
        while True:
            at = data.find(header, start)
            if at < 0 or seen > 4000:
                break
            start = at + 1
            try:
                d = zlib.decompressobj()
                out = d.decompress(data[at:at + 64 * 1024 * 1024], 256 * 1024 * 1024)
            except zlib.error:
                continue
            if len(out) >= 64:
                seen += 1
                yield f"zlib@{at:#x}", out


def find_tables(blob: bytes):
    hits = []
    for width, dtype in ((2, "<u2"), (4, "<u4")):
        for align in range(width):
            n = (len(blob) - align) // width
            if n < 8:
                continue
            vals = np.frombuffer(blob, dtype=dtype, count=n, offset=align).astype(np.int64)
            for label, seq in SEQUENCES.items():
                first = np.nonzero(vals == seq[0])[0]
                if len(first) == 0 or len(first) > 2_000_000:
                    continue
                for stride in range(1, MAX_STRIDE // width + 1):
                    ok = first[first + 3 * stride < n]
                    for k, v in enumerate(seq[1:], 1):
                        ok = ok[vals[ok + k * stride] == v]
                        if len(ok) == 0:
                            break
                    for p in ok[:20]:
                        a, b = FOLLOW[label]
                        nxt = [int(vals[p + k * stride]) for k in range(4, 10) if p + k * stride < n]
                        strong = a in nxt and b in nxt
                        hits.append((strong, label, width, stride * width, align + p * width, nxt))
    return hits


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        return
    root = Path(sys.argv[1])
    report = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("scan_report.txt")
    lines, inventory, worded = [], [], []
    files = sorted(p for p in root.rglob("*") if p.is_file())
    print(f"{len(files)} files under {root}")
    for i, path in enumerate(files, 1):
        rel = path.relative_to(root)
        data = path.read_bytes()
        inventory.append(f"{len(data):>12}  {data[:4].hex()}  {rel}")
        if any(w in data for w in WORDS):
            worded.append(str(rel))
        for where, blob in blobs(data):
            for strong, label, width, stride, offset, nxt in find_tables(blob):
                tag = "LIKELY " if strong else "maybe  "
                lines.append(f"{tag}{rel} [{where}] +{offset:#x}: {label}, {width * 8}-bit, "
                             f"record {stride} bytes; next values {nxt}")
        if i % 50 == 0:
            print(f"  {i}/{len(files)} files, {len(lines)} candidate tables")
    lines.sort(key=lambda l: not l.startswith("LIKELY"))
    text = ["# Pokédex table candidates (LIKELY = Pidgey and Oddish follow too)", *lines,
            "", "# Files mentioning dex / zukan / register", *worded,
            "", "# Inventory (size, first 4 bytes, path)", *inventory]
    report.write_text("\n".join(text), encoding="utf-8")
    print(f"Done: {len(lines)} candidates ({sum(l.startswith('LIKELY') for l in lines)} likely). "
          f"Report: {report.resolve()}")


if __name__ == "__main__":
    main()
