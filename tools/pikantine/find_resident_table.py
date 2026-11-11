#!/usr/bin/env python3
"""Find the game's resident table and the Pokédex slot of every Pokémon (read-only).

The game registers a Pokémon in the Pokédex by looking its resident id up in a data table and
taking a 2-byte field of that row as the Pokédex slot (main+0xa982c0: table +0x8d0, row +0x28).
This script finds that table in a RomFS dump: for every entry of the data container it tries
every row size and position, and keeps the ones where each Pokémon living in two real towns
(SpiderNic96's 42, a tester's 139) points at a slot that town has registered. Only the resident
ids and registered slots of those two saves are inside this file, no game data.

    python find_resident_table.py "C:\\path\\to\\romfs"

Writes pikantine_resident_table.json next to this script (resident id -> Pokédex slot, numbers
only). Needs Python 3.9+ and numpy (pip install numpy). Takes a while: it prints progress.
"""
import json
import mmap
import os
import struct
import sys
import time
import zlib

import numpy as np

MAX_ENTRY = 64 * 1024 * 1024
MAX_STRIDE = 512
MIN_ID = 30  # villagers below this are not used, so the table may start before its entry
TOWNS = {"nic": {"villagers": [16, 17, 21, 23, 26, 27, 28, 32, 41, 45, 51, 53, 57, 63, 65, 67, 77, 99, 106, 107, 111, 126, 127, 132, 136, 149, 170, 174, 176, 192, 197, 220, 222, 224, 254, 255, 271, 273, 278, 301, 305, 325], "registered": [1, 4, 6, 7, 8, 13, 18, 28, 33, 35, 50, 52, 56, 58, 68, 78, 80, 81, 82, 85, 86, 128, 186, 195, 205, 218, 221, 222, 257, 263, 269, 271, 296, 306, 308, 311, 314, 315, 316, 319, 320, 325, 326, 332, 333, 342]}, "mugs": {"villagers": [15, 16, 17, 18, 19, 20, 21, 24, 26, 27, 28, 29, 32, 40, 41, 44, 45, 49, 50, 51, 52, 54, 57, 58, 60, 63, 64, 65, 67, 68, 69, 70, 72, 74, 77, 86, 90, 93, 94, 97, 99, 106, 107, 111, 113, 119, 120, 122, 123, 125, 126, 127, 130, 132, 133, 134, 135, 136, 139, 140, 143, 145, 146, 148, 149, 150, 156, 159, 160, 164, 165, 166, 170, 174, 175, 177, 179, 180, 181, 184, 185, 192, 197, 198, 199, 200, 210, 211, 212, 218, 220, 222, 225, 226, 227, 228, 229, 232, 233, 234, 235, 236, 240, 241, 245, 250, 252, 254, 255, 256, 258, 259, 260, 264, 271, 273, 274, 276, 278, 287, 288, 289, 295, 301, 302, 303, 304, 311, 312, 313, 314, 315, 321, 324, 325, 326, 513, 514, 566], "registered": [1, 2, 3, 4, 5, 6, 7, 9, 13, 14, 20, 21, 26, 27, 28, 29, 31, 32, 33, 35, 36, 37, 41, 42, 49, 50, 51, 52, 55, 56, 57, 58, 59, 61, 62, 65, 66, 68, 71, 72, 78, 80, 82, 84, 85, 86, 100, 102, 103, 104, 105, 110, 111, 112, 114, 115, 117, 119, 122, 123, 128, 132, 133, 135, 138, 143, 144, 145, 146, 148, 153, 159, 160, 161, 167, 168, 169, 174, 183, 184, 186, 187, 188, 190, 192, 193, 195, 199, 201, 202, 205, 218, 219, 220, 221, 222, 226, 227, 228, 229, 230, 235, 236, 238, 239, 240, 241, 251, 257, 263, 265, 266, 269, 270, 273, 278, 288, 289, 296, 297, 298, 301, 306, 308, 309, 310, 311, 312, 314, 315, 316, 318, 319, 325, 326, 329, 332, 333, 341, 342, 343, 354, 355, 373]}}

def align(x, a):
    return (x + a - 1) & ~(a - 1)


def container_entries(idx_path, bin_path):
    """Yields (index, payload) for every entry of LI_C0_R.IDX / LD_C0_R.BIN."""
    idx = open(idx_path, "rb").read()
    if len(idx) % 32:
        print(f"  {idx_path}: size not a multiple of 32, skipped")
        return
    with open(bin_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for n in range(len(idx) // 32):
            off, raw, packed, flag = struct.unpack_from("<qqqq", idx, n * 32)
            if raw == 0 or raw > MAX_ENTRY:
                continue
            try:
                if flag == 0:
                    yield n, bytes(mm[off:off + raw])
                elif flag == 1:
                    block, nblocks, raw2 = struct.unpack_from("<III", mm, off)
                    table = struct.unpack_from(f"<{nblocks}I", mm, off + 12)
                    pos = align(12 + 4 * nblocks, 128)
                    out = bytearray()
                    for b, slot in enumerate(table):
                        want = min(block, raw - b * block)
                        chunk = mm[off + pos:off + pos + slot]
                        if slot >= 4 and struct.unpack_from("<I", chunk, 0)[0] == slot - 4:
                            data = zlib.decompress(chunk[4:])
                        else:
                            data = chunk
                        if len(data) != want:
                            raise ValueError("block size")
                        out += data
                        pos = align(pos + slot, 128)
                    yield n, bytes(out)
            except Exception as exc:  # a malformed entry: report and go on
                print(f"  entry {n}: {exc}")
        mm.close()



def setup():
    towns = []
    for t in TOWNS.values():
        reg = np.zeros(65536, bool)
        reg[t["registered"]] = True
        ids = sorted((r for r in t["villagers"] if r >= MIN_ID), reverse=True)
        towns.append((reg, ids))
    # most constraining first: the largest ids (shortest slices), then the rest
    order = []
    for reg, ids in towns:
        order += [(reg, r) for r in ids]
    order.sort(key=lambda x: -x[1])
    return order, max(r for _, r in order)


def search(blob, order, top):
    n = len(blob)
    if n < top * 2 + 4:
        return []
    b = np.frombuffer(blob, np.uint8)
    pad = MIN_ID * MAX_STRIDE
    v = np.zeros(pad + n, np.uint16)
    v[pad:pad + n - 1] = b[:-1].astype(np.uint16) | (b[1:].astype(np.uint16) << 8)
    hits = []
    for s in range(2, min(MAX_STRIDE, (n + pad) // (top + 1)) + 1):
        # p = position of row 0's field (may be negative, down to -MIN_ID*s)
        lo = pad - MIN_ID * s
        count = pad + n - 1 - top * s - lo
        if count <= 0:
            break
        mask = None
        cand = None
        for reg, r in order:
            start = lo + r * s
            if cand is None:
                ok = reg[v[start:start + count]]
                mask = ok if mask is None else (mask & ok)
                if mask.sum() < 4096:
                    cand = np.nonzero(mask)[0]
                    if cand.size == 0:
                        break
            else:
                cand = cand[reg[v[start + cand]]]
                if cand.size == 0:
                    break
        if cand is None:
            cand = np.nonzero(mask)[0]
        for c in cand[:8]:
            p0 = int(c) + lo - pad
            hits.append((s, p0))
    return hits


def table(blob, s, p0, rows=1050):
    out = {}
    for r in range(rows):
        at = p0 + r * s
        if 0 <= at and at + 2 <= len(blob):
            out[r] = struct.unpack_from("<H", blob, at)[0]
    return out


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit(__doc__)
    order, top = setup()
    pairs = []
    for d, _, files in os.walk(sys.argv[1]):
        upper = {f.upper(): f for f in files}
        if "LI_C0_R.IDX" in upper and "LD_C0_R.BIN" in upper:
            pairs.append((os.path.join(d, upper["LI_C0_R.IDX"]), os.path.join(d, upper["LD_C0_R.BIN"])))
    if not pairs:
        raise SystemExit("No LI_C0_R.IDX / LD_C0_R.BIN found under that folder: is it the RomFS dump?")
    found = []
    t0 = time.time()
    for idx_path, bin_path in pairs:
        print(f"Container: {bin_path}")
        for n, blob in container_entries(idx_path, bin_path):
            if n % 250 == 0:
                print(f"  entry {n} ({time.time() - t0:.0f} s, {len(found)} found)", flush=True)
            for s, p0 in search(blob, order, top):
                found.append({"source": f"container entry {n}", "size": len(blob), "row_size": s,
                              "row0_field_at": p0, "slot_of_resident": table(blob, s, p0)})
                print(f"  FOUND: entry {n}, row {s} B, field of row 0 at {p0}", flush=True)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pikantine_resident_table.json")
    json.dump(found, open(out, "w"), indent=1)
    print(f"\n{len(found)} candidate(s); written to {out}")


if __name__ == "__main__":
    main()
