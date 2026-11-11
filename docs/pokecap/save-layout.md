# Pokopia 2.0.1 save: Pokédex, Habitat Dex and histories

How the common save (`CMNDAT.BIN`, layout `0x0200017D`) stores the player's progress, as traced
in the game's save code. Check any save with `tools/pikantine/dex_report.py` (read-only).

## How the file is built

- Header `0x130` bytes (magic `0x31227687`), then one zlib stream; decompressed size `0x470CED`.
- The serializer at `main+0x5ca320` writes each field straight after the previous one. Before
  each field it calls `main+0x5a71e0` with the field's name (`m_pokemonDexHistory`…); that check
  only matters when an older save is read. When saving, every current field is written.
- Its order is anchored by offsets PokoHex already proved: the item catalogue at `0x402ED9`
  (`m_itemHistory`, `0x11F8` bytes) and the player abilities at `0x405047` (`m_pokeSkillHistory`
  `0x200`, wheel `8`, emotes `0x80`, emote wheel `0x30`).
- In memory, the same block hangs off `[main+0x3561930]` (Recompia calls it the save root). The
  memory offsets below let a running game be changed; the game then saves the change itself.

| Field | File offset | Size | Memory (root +) | One entry |
|---|---|---|---|---|
| `m_recipeHistory` | `0x3FCB4C` | `0x2328` | `0x6CFCE4` | byte per recipe |
| `m_itemHistory` | `0x402ED9` | `0x11F8` | `0x6D6071` | byte per item, bit 0 obtained |
| `m_pokeSkillHistory` … emote wheel | `0x405047` | `0x2B8` | `0x6D81E4` | PokoHex's abilities |
| `m_pokemonHistory` | `0x4052FF` | `0x20400` | `0x6D849C` | per Pokémon, layout unknown |
| `m_habitatHistory` | `0x4256FF` | `0x1800` | `0x6F889C` | u32 per habitat: 1 discovered, 3 built |
| `m_pokemonDexHistory` | `0x426EFF` | `0x400` | `0x6FA09C` | byte per dex slot, see below |
| `m_pokemonDexReadHistory` | `0x4272FF` | `0x400` | `0x6FA49C` | byte per dex slot: entry opened |
| `m_pokeDexLvFinishHistory` | `0x4276FF` | `0x65` | `0x6FA89C` | byte per dex reward claimed |

## Pokédex byte

- Bits 0 and 1 together: **registered** (45 in SpiderNic96's save, the number the game showed;
  144 in a tester's with 139 residents).
- Bit 3: seen only (a silhouette). Bit 2: unknown yet.
- The slot is the game's own index. The in-game "Numerical order" follows it, but it is **not**
  the printed Pokédex number (Pidgey, No. 010, is slot 13). The slot → species table is in the
  game's data files, not in the save; it is the last piece an editor with names needs.

## Not in these fields

- Which Pokémon live in town: the 1050 villager records at `0x0` (374 bytes each) and their homes
  at `0x79824` (21 bytes each), both known from PokoHex. Each area's placed objects are in its
  own `STGDATnn.BIN`.
- The active-Pokémon budget (30 by default) is a game parameter, not save data:
  `[[main+0x36ca678]]+0xc`. Pikantine raises it while the game runs.
