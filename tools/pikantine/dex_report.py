#!/usr/bin/env python3
"""Read-only report of the Pokédex, Habitat Dex and related histories in a Pokopia 2.0.1 CMNDAT.BIN.

Offsets come from the game's own save serializer (main+0x5ca320), which writes its fields one after
another; the order is anchored by PokoHex's known offsets (item catalogue 0x402ED9, abilities 0x405047).

    python dex_report.py path/to/CMNDAT.BIN [more saves...]

Nothing is written. Index = the game's internal Pokédex slot (the in-game "Numerical order" follows it,
but it is NOT the printed Pokédex number). dex_tables.py names the slots and habitats from a RomFS dump.
"""
import struct
import sys
import zlib

MAGIC = 0x31227687
LAYOUT_201 = 0x0200017D
HEADER = 0x130

# name: (offset in the decompressed payload, size, what one entry is)
FIELDS = {
    "recipeHistory": (0x3FCB4C, 0x2328, "byte per recipe"),
    "itemHistory": (0x402ED9, 0x11F8, "byte per item (bit 0: obtained)"),
    "pokemonHistory": (0x4052FF, 0x20400, "per-Pokémon history (layout unknown)"),
    "habitatHistory": (0x4256FF, 0x1800, "12 bytes per habitat id: u32 state (1 discovered, 3 built), 2 x u32"),
    "pokemonDexHistory": (0x426EFF, 0x400, "byte per dex slot: bits 0+1 registered, bit 3 seen, bit 2 ?"),
    "pokemonDexReadHistory": (0x4272FF, 0x400, "byte per dex slot: entry opened (not 'new')"),
    "pokeDexLvFinishHistory": (0x4276FF, 0x65, "byte per dex reward level claimed"),
}


def payload(path):
    data = open(path, "rb").read()
    magic, layout = struct.unpack_from("<II", data, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: not a Pokopia CMNDAT.BIN")
    if layout != LAYOUT_201:
        raise SystemExit(f"{path}: save layout {layout:#x}, only 2.0.1 ({LAYOUT_201:#x}) is mapped")
    end = struct.unpack_from("<i", data, 0x10)[0]
    return zlib.decompress(data[HEADER:end])


def report(path):
    raw = payload(path)
    dex = raw[0x426EFF:0x426EFF + 0x400]
    hab = raw[0x4256FF:0x4256FF + 0x1800]
    registered = [i for i, v in enumerate(dex) if v & 3 == 3]
    seen = [i for i, v in enumerate(dex) if v & 8 and v & 3 != 3]
    habitats = [struct.unpack_from("<I", hab, i * 12)[0] for i in range(0x1800 // 12)]
    print(f"== {path}")
    print(f"  Pokédex: {len(registered)} registered, {len(seen)} seen only")
    print(f"    registered slots: {registered}")
    print(f"  Habitat Dex: {sum(1 for v in habitats if v == 3)} built, "
          f"{sum(1 for v in habitats if v and v != 3)} discovered only")
    print(f"  Dex rewards claimed: {sum(1 for v in raw[0x4276FF:0x4276FF + 0x65] if v)}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for p in sys.argv[1:]:
        report(p)
