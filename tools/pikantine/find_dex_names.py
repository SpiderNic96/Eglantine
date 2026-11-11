#!/usr/bin/env python3
"""Find the Pokédex slot order in the game's text: a list of Pokémon names in slot order (read-only).

Second try after find_dex_table.py (which found no table of plain numbers). This one looks through
every entry of the game's data container (LI_C0_R.IDX + LD_C0_R.BIN) and every small file for lists
of UTF-8 strings that contain the English names of the slots known from SpiderNic96's save:
    slot 1 Bulbasaur, 4 Charmander, 6 Charizard, 7 Squirtle, 8 Wartortle, 13 Pidgey, 18 Oddish
For each list that holds them, it checks whether their positions follow the slot numbers (any
fixed number of strings per slot, from any start), and writes what it found to
pikantine_dex_names.json next to this script: the matching lists' names in order (Pokémon names
only, short strings), so the Pokédex editor can show them. Send that file back.

    python find_dex_names.py "C:\\path\\to\\romfs"

Needs Python 3.9+ (no other packages). Nothing in the dump is changed.
"""
import json
import mmap
import os
import struct
import sys
import zlib

KNOWN = {1: "Bulbasaur", 4: "Charmander", 6: "Charizard", 7: "Squirtle", 8: "Wartortle",
         13: "Pidgey", 18: "Oddish"}
EXTRA = ["Pawmi", "Onix", "Scyther", "Heracross", "Ditto", "Tangrowth", "Mew"]
MAX_ENTRY = 64 * 1024 * 1024
MAX_KEEP = 4000  # strings kept per matching list

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



def strings(blob):
    """NUL-separated UTF-8 strings of a blob, with their byte offsets (empty ones included)."""
    out = []
    pos = 0
    n = len(blob)
    while pos < n:
        end = blob.find(b"\0", pos)
        if end < 0:
            end = n
        out.append((pos, blob[pos:end]))
        pos = end + 1
    return out


def check(blob):
    if b"Bulbasaur" not in blob or b"Pidgey" not in blob or b"Oddish" not in blob:
        return None
    strs = strings(blob)
    # positions of the known names (exact strings)
    where = {}
    for i, (_, s) in enumerate(strs):
        try:
            t = s.decode("utf-8")
        except UnicodeDecodeError:
            continue
        for slot, name in KNOWN.items():
            if t == name:
                where.setdefault(slot, []).append(i)
    if len(where) < 5:
        return {"found": {KNOWN[s]: v[:5] for s, v in where.items()}, "fits": [], "strs": strs}
    fits = []
    for per in range(1, 13):
        for b in where.get(1, []):
            base = b - per
            if all(any(p == base + s * per for p in where.get(s, [])) for s in KNOWN if s in where):
                fits.append({"strings_per_slot": per, "slot0_string": base})
    return {"found": {KNOWN[s]: v[:5] for s, v in where.items()}, "fits": fits, "strs": strs}


def names_list(strs, start, per):
    out = {}
    for slot in range(0, 1024):
        i = start + slot * per
        if i < 0:
            continue
        if i >= len(strs) or len(out) > MAX_KEEP:
            break
        try:
            t = strs[i][1].decode("utf-8")
        except UnicodeDecodeError:
            t = ""
        if len(t) <= 40:
            out[slot] = t
    return out


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit(__doc__)
    root = sys.argv[1]
    results = []
    pairs, others = [], []
    for d, _, files in os.walk(root):
        upper = {f.upper(): f for f in files}
        if "LI_C0_R.IDX" in upper and "LD_C0_R.BIN" in upper:
            pairs.append((os.path.join(d, upper["LI_C0_R.IDX"]), os.path.join(d, upper["LD_C0_R.BIN"])))
        for f in files:
            p = os.path.join(d, f)
            if f.upper() in ("LI_C0_R.IDX", "LD_C0_R.BIN") or f.lower().endswith(".fdata"):
                continue
            if os.path.getsize(p) <= MAX_ENTRY:
                others.append(p)

    def look(label, blob):
        r = check(blob)
        if r is None:
            return
        item = {"source": label, "size": len(blob), "found": r["found"], "fits": r["fits"]}
        if r["fits"]:
            f = r["fits"][0]
            item["names"] = names_list(r["strs"], f["slot0_string"], f["strings_per_slot"])
        elif len(r["found"]) >= 3:
            # no fit: keep a window of the short strings around Bulbasaur to study the layout
            first = min(min(v) for v in r["found"].values())
            win = r["strs"][max(0, first - 40):first + 1200]
            item["window_from"] = max(0, first - 40)
            item["window"] = [s.decode("utf-8", "replace") for _, s in win if len(s) <= 40]
        results.append(item)
        print(f"  {label}: names {sorted(r['found'])}, fits {r['fits'][:3]}", flush=True)

    if not pairs:
        print("No LI_C0_R.IDX / LD_C0_R.BIN found under that folder: is it the RomFS dump?")
    for idx_path, bin_path in pairs:
        print(f"Container: {bin_path}")
        for n, blob in container_entries(idx_path, bin_path):
            if n % 1000 == 0:
                print(f"  entry {n}...", flush=True)
            look(f"container entry {n}", blob)
    print(f"Other files: {len(others)}")
    for p in others:
        look(os.path.relpath(p, root), open(p, "rb").read())
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pikantine_dex_names.json")
    json.dump(results, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"\n{len(results)} list(s) with Pokémon names, "
          f"{sum(1 for r in results if r['fits'])} in slot order; written to {out}")


if __name__ == "__main__":
    main()
