#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Snapshot a Pokémon Pokopia save folder every time the game saves, and show what changed.

Run it, then play normally. Each time G00/CMNDAT.BIN changes, the whole save folder is copied to
<out>/<time>/ and the differences in the common save's data are listed (offset ranges, before and
after bytes). Register one new Pokémon, note the time, and the snapshot pair either side of it
holds the Pokédex change.

    python save_watch.py "<save folder holding G00>" [out folder]
    python save_watch.py --diff <snapshot A> <snapshot B>

Needs only Python 3.9+.
"""
import shutil
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

HEADER = 0x130  # common save; the area saves (STGDAT) use 0x124
DEX_GUESS = 0x221758  # a byte per resident; bit 0 matched "No. Registered" but is not enough


def unpack(path: Path) -> bytes:
    header = 0x124 if path.name.upper().startswith("STGDAT") else HEADER
    data = path.read_bytes()
    if struct.unpack_from("<I", data, 0)[0] != 0x31227687:
        raise ValueError(f"{path} is not a Pokopia save file")
    length = struct.unpack_from("<i", data, 0x10)[0]
    return zlib.decompress(data[header:length])


def ranges(a: bytes, b: bytes, gap: int = 16):
    out, start, last = [], None, None
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            if start is None:
                start = i
            elif i - last > gap:
                out.append((start, last))
                start = i
            last = i
    if start is not None:
        out.append((start, last))
    return out


def diff(old: Path, new: Path, report=print) -> None:
    for name in ("CMNDAT.BIN", "STGDAT01.BIN", "STGDAT02.BIN", "STGDAT03.BIN", "STGDAT12.BIN"):
        pa, pb = old / "G00" / name, new / "G00" / name
        if not (pa.exists() and pb.exists()):
            continue
        a, b = unpack(pa), unpack(pb)
        rs = ranges(a, b)
        report(f"== {name}: {len(rs)} changed ranges")
        for s, e in rs[:400]:
            n = e - s + 1
            report(f"  {s:#09x}..{e:#09x} ({n:4} bytes)  "
                   f"{a[s:s + min(n, 24)].hex()} -> {b[s:s + min(n, 24)].hex()}")
        if name == "CMNDAT.BIN":
            da = sum(x & 1 for x in a[DEX_GUESS:DEX_GUESS + 1050])
            db = sum(x & 1 for x in b[DEX_GUESS:DEX_GUESS + 1050])
            report(f"  (resident flags with bit 0: {da} -> {db})")


def watch(folder: Path, out: Path) -> None:
    common = folder / "G00" / "CMNDAT.BIN"
    if not common.exists():
        sys.exit(f"No G00/CMNDAT.BIN in {folder}: pass the folder that holds G00.")
    out.mkdir(parents=True, exist_ok=True)
    last_stamp, last_snap = None, None
    print(f"Watching {common}\nSnapshots go to {out}\nCtrl+C to stop.")
    while True:
        try:
            stamp = (common.stat().st_mtime, common.stat().st_size)
        except FileNotFoundError:
            stamp = None
        if stamp and stamp != last_stamp:
            time.sleep(3)  # let the game finish writing every file
            snap = out / datetime.now().strftime("%Y%m%d-%H%M%S")
            shutil.copytree(folder, snap)
            print(f"\n[{snap.name}] snapshot")
            if last_snap is not None:
                lines = []
                diff(last_snap, snap, lines.append)
                (snap / "changes.txt").write_text("\n".join(lines), encoding="utf-8")
                print("\n".join(lines[:40]))
            last_stamp, last_snap = (common.stat().st_mtime, common.stat().st_size), snap
        time.sleep(2)


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--diff":
        diff(Path(sys.argv[2]), Path(sys.argv[3]))
    elif len(sys.argv) >= 2:
        watch(Path(sys.argv[1]), Path(sys.argv[2]) if len(sys.argv) > 2 else Path("pokopia_snapshots"))
    else:
        print(__doc__)
