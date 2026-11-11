#!/usr/bin/env python3
"""Names every Pokédex slot and habitat id of Pokopia 2.0.1, from your own RomFS dump (read-only).

    python dex_tables.py "C:\\path\\to\\romfs"                     writes pikantine_dex_tables.json
    python dex_tables.py "C:\\path\\to\\romfs" CMNDAT.BIN [...]    also names what each save has

What the game does (main+0xa982c0 / 0xa983d4): it looks a Pokémon up in the 1024-row table of
container entry #1066 (104-byte rows, row index at +0x18) and takes the u16 at row +0x28 as the
Pokédex slot, 1..999. The save keeps one byte per slot (m_pokemonDexHistory, 0x426EFF). The rows
follow the name list of entry #4934 (English; #4933..#4941 are the nine languages). Habitat ids
index entry #4538 (English habitat names, 12-byte rows: name, "in the <name>"); the save keeps 12
bytes per habitat (m_habitatHistory, 0x4256FF): u32 state (1 discovered, 3 built) and two more u32.

Checked on two real saves: every named resident of both towns (40 and 134) points at a slot its
save has registered. Entry numbers are for update build 655360 (2.0.1a); the script checks each
table's shape and stops if a different build moved them.
"""
import json
import mmap
import os
import struct
import sys
import zlib

SLOT_TABLE, SLOT_NAMES, HABITAT_NAMES = 1066, 4934, 4538
DEX, DEX_SIZE = 0x426EFF, 0x400
HABITATS, HABITAT_COUNT = 0x4256FF, 512
MAGIC, LAYOUT_201, HEADER = 0x31227687, 0x0200017D, 0x130


def align(x, a):
    return (x + a - 1) & ~(a - 1)


def read_entries(romfs, wanted):
    """Returns {index: payload} for the wanted entries of LI_C0_R.IDX / LD_C0_R.BIN."""
    for d, _, files in os.walk(romfs):
        upper = {f.upper(): f for f in files}
        if "LI_C0_R.IDX" in upper and "LD_C0_R.BIN" in upper:
            idx = open(os.path.join(d, upper["LI_C0_R.IDX"]), "rb").read()
            bin_path = os.path.join(d, upper["LD_C0_R.BIN"])
            break
    else:
        raise SystemExit("No LI_C0_R.IDX / LD_C0_R.BIN under that folder: is it the RomFS dump?")
    out = {}
    with open(bin_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for n in wanted:
            off, raw, _packed, flag = struct.unpack_from("<qqqq", idx, n * 32)
            if flag == 0:
                out[n] = bytes(mm[off:off + raw])
                continue
            block, nblocks, _raw2 = struct.unpack_from("<III", mm, off)
            slots = struct.unpack_from(f"<{nblocks}I", mm, off + 12)
            pos, data = align(12 + 4 * nblocks, 128), bytearray()
            for b, size in enumerate(slots):
                chunk = mm[off + pos:off + pos + size]
                if size >= 4 and struct.unpack_from("<I", chunk, 0)[0] == size - 4:
                    chunk = zlib.decompress(chunk[4:])
                data += chunk
                pos = align(pos + size, 128)
            out[n] = bytes(data[:raw])
        mm.close()
    return out


def texts(blob, record):
    """Strings of a text table: each row starts with a pointer relative to itself."""
    count = struct.unpack_from("<I", blob, 0)[0]
    rows = []
    for r in range(count):
        p = 0x40 + record * r
        rel = struct.unpack_from("<I", blob, p)[0]
        rows.append(blob[p + rel:blob.index(b"\0", p + rel)].decode("utf-8", "replace") if rel else "")
    return rows


def build(romfs):
    e = read_entries(romfs, [SLOT_TABLE, SLOT_NAMES, HABITAT_NAMES])
    table = e[SLOT_TABLE]
    count = struct.unpack_from("<I", table, 0)[0]
    if count != 1024 or len(table) != 0x40 + count * 104:
        raise SystemExit(f"Entry #{SLOT_TABLE} is not the 1024 x 104 B table: a different game build?")
    names = texts(e[SLOT_NAMES], 8)
    slots = {}
    for row in range(count):
        base = 0x40 + row * 104
        slot = struct.unpack_from("<H", table, base + 0x28)[0]
        if struct.unpack_from("<H", table, base + 0x18)[0] != row or not slot:
            continue
        slots[slot] = {"name": names[row], "row": row}
    if slots.get(1, {}).get("name") != "Bulbasaur":
        raise SystemExit("Slot 1 is not Bulbasaur: a different game build or language order?")
    # Species with more than one slot: label the later ones so the editor can tell them apart.
    seen = {}
    for slot in sorted(slots):
        n = slots[slot]["name"]
        seen[n] = seen.get(n, 0) + 1
        slots[slot]["label"] = n if seen[n] == 1 else f"{n} ({seen[n]})"
    habitat_names = texts(e[HABITAT_NAMES], 12)
    habitats = {i: n[:1].upper() + n[1:] for i, n in enumerate(habitat_names) if n}
    return {
        "build": "2.0.1a (update 655360)",
        "dex_slots": {str(s): slots[s] for s in sorted(slots)},
        "habitats": {str(i): habitats[i] for i in sorted(habitats)},
    }


def save_report(path, tables):
    data = open(path, "rb").read()
    magic, layout = struct.unpack_from("<II", data, 0)
    if magic != MAGIC or layout != LAYOUT_201:
        print(f"== {path}: not a Pokopia 2.0.1 CMNDAT.BIN, skipped")
        return
    raw = zlib.decompress(data[HEADER:struct.unpack_from("<i", data, 0x10)[0]])
    dex = raw[DEX:DEX + DEX_SIZE]
    slots, habs = tables["dex_slots"], tables["habitats"]
    label = lambda s: slots.get(str(s), {}).get("label", f"slot {s}")
    reg = [s for s, v in enumerate(dex) if v & 3 == 3]
    seen = [s for s, v in enumerate(dex) if v & 8 and v & 3 != 3]
    missing = [int(s) for s in slots if dex[int(s)] & 3 != 3]
    state = [struct.unpack_from("<I", raw, HABITATS + 12 * i)[0] for i in range(HABITAT_COUNT)]
    built = [i for i, v in enumerate(state) if v == 3]
    found = [i for i, v in enumerate(state) if v and v != 3]
    print(f"== {path}")
    print(f"  Pokédex: {len(reg)} of {len(slots)} registered, {len(seen)} seen only")
    print(f"    registered: {', '.join(label(s) for s in reg)}")
    if seen:
        print(f"    seen only: {', '.join(label(s) for s in seen)}")
    print(f"    not yet: {len(missing)} ({', '.join(label(s) for s in missing[:12])}{' ...' if len(missing) > 12 else ''})")
    print(f"  Habitats: {len(built)} built, {len(found)} discovered only (of {len(habs)})")
    print(f"    built: {', '.join(habs.get(str(i), f'#{i}') for i in built)}")
    if found:
        print(f"    discovered only: {', '.join(habs.get(str(i), f'#{i}') for i in found)}")


def main():
    if len(sys.argv) < 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit(__doc__)
    tables = build(sys.argv[1])
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pikantine_dex_tables.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(tables, f, ensure_ascii=False, indent=1)
    print(f"{len(tables['dex_slots'])} Pokédex slots, {len(tables['habitats'])} habitats -> {out}")
    for path in sys.argv[2:]:
        save_report(path, tables)


if __name__ == "__main__":
    main()
