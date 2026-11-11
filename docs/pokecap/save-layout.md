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
| `m_recipeHistory` | `0x3FCB4C` | `0x2328` | `0x6CFCE4` | 6 bytes per recipe, see below |
| `m_itemHistory` | `0x402ED9` | `0x11F8` | `0x6D6071` | byte per item, bit 0 obtained |
| `m_pokeSkillHistory` … emote wheel | `0x405047` | `0x2B8` | `0x6D81E4` | PokoHex's abilities |
| `m_pokemonHistory` | `0x4052FF` | `0x20400` | `0x6D849C` | 129 bytes per species row, see below |
| `m_habitatHistory` | `0x4256FF` | `0x1800` | `0x6F889C` | 12 bytes per habitat id, see below |
| `m_pokemonDexHistory` | `0x426EFF` | `0x400` | `0x6FA09C` | byte per dex slot, see below |
| `m_pokemonDexReadHistory` | `0x4272FF` | `0x400` | `0x6FA49C` | byte per dex slot: entry opened |
| `m_pokeDexLvFinishHistory` | `0x4276FF` | `0x65` | `0x6FA89C` | byte per dex reward claimed |

## Pokédex byte

- Bits 0 and 1 together: **registered** (45 in SpiderNic96's save, the number the game showed;
  144 in a tester's with 139 residents).
- Bit 3: seen only (a silhouette). Bit 2: unknown yet.
- The slot is the game's own Pokédex id (1–999), **not** the printed Pokédex number (Pidgey,
  No. 010, is slot 13). The code reads it as `[main+0x3561930] + 0x6FA1A4 + id` (getters such as
  `main+0x5bd458`; the save root is 0x108 below the serializer's own base).
- To register a Pokémon the game looks it up in data table `+0x8d0` (slot 0x11a of the table
  manager at `[main+0x35618f0]`) and takes the `u16` at row `+0x28` as the Pokédex id
  (`main+0xa982c0` sets bit 0, `main+0xa983d4` bit 1; both accept ids 1–999).
- That table is container entry **#1066** of `LD_C0_R.BIN` (1024 rows × 104 B, row index at
  `+0x18`). Its rows follow the name list of entry #4934 (English; #4933–#4941 are the nine
  languages). 374 slots are used, from 1 to 412; the save array has room for 1024.
- Some species have two slots: the special residents (Pikachu 18 / Peakychu 309, Snorlax 91 /
  Mosslax 312, Professor Tangrowth 308 / Tangrowth 337) and forms (Shellos, Gastrodon, Toxtricity,
  Tatsugiri, Frillish, Jellicent).
- Checked on two saves: every named resident (40 in SpiderNic96's town, 134 in a tester's) points
  at a slot that save has registered. `tools/pikantine/dex_tables.py` writes the full table from
  a RomFS dump and names what a save has.

## Recipe record

- `m_recipeHistory` is 1500 records of 6 bytes, indexed by recipe (table #493, whose `u16` at
  `+18` is the item made; its name via item table #983 `+34` and names #4646): `u16` 0, `u16` the
  order it was learned in (1, 2, 3…), `u16` flags (2 learned, still new; 10 and 26 seen/made).
- 921 of the 1500 rows make a named item.

## Sightings record

- `m_pokemonHistory` is 1024 records of 129 bytes, indexed by **species row** (entry #1066's
  order, not the Pokédex slot): 64 bytes of habitat bits (bit h = habitat id h), the same 64
  bytes again, then a flag byte (0, 1 or 2; meaning unknown, not the Pokédex state).
- A habitat's page in the Habitat Dex shows "?" for a Pokémon it draws whose bits lack that
  habitat. In three real saves every set bit is one of that Pokémon's known habitats.

## Habitat record

- `m_habitatHistory` is 512 records of 12 bytes, indexed by habitat id: `u32` state (1 discovered,
  3 built) and two more `u32`; the first of them holds a small number (1–8) in a few records.
- Habitat names are entry **#4538** (English; #4537–#4545 by language): 501 rows of 12 bytes, each
  a pointer relative to itself to the name and to an "in the …" phrase. Id 3 is the first habitat,
  Tall grass. Descriptions are #4546–#4554, one string per row.
- Which Pokémon each habitat draws is not in these tables. The public Pokopia database
  (cliffchang11/Pokopia_Cliff) lists two habitats per Pokémon; its 252 habitats match game ids
  through the zh-TW names (#4543).

## Resident record

From PokoHex, then compared across two real saves (42 and 139 residents):

- 1050 records of 374 bytes from `0x0`, indexed by resident id. Bytes 1–16 are a 16-byte id that is
  the same in every resident of a save. It is repeated at `0x22`, and its being set is what makes
  a record a resident. `0x11` u16 friendship (170 per tier); `0x13` u16 thank points; `0x15` the
  present level (1 in every record the game has touched); `0x16` waiting for a present;
  `0x17` float −1.0; `0x1B` a u32 that differs per resident; `0xC2` 1; `0xC4` naming chat done;
  `0xC5` 1; `0x16A` six u16, the date and time it arrived.
- Some records have no id but a home area. Professor Tangrowth, Smearguru and Mosslax have one.
- Homes: 21 bytes per resident from `0x79824`. Byte 0 is the home type (0 none, 1–3), `1` u16 home
  id, `3` the area (stage) id, `6`/`8`/`10` i16 the last home block, `0x0E` float −1.0, `0x14` 1
  when housed.
- The editor's **Add resident** writes this shape with no home. It is untested in game.

## Function flags

- `m_functionFlag`: 32 bytes at `0x220F04`, flag n = bit n%8 of byte n/8, ids 1–249.
- The game's name table for them, entry **#1221** (250 rows), ships blank: every row is empty
  except row 0, "－－－". So the game knows the flags only by number.
- Entry **#949** gives each flag a u32 of settings. The flag check at `main+0x113f10` reads them:
  `0x10000` always on (flag 19), `0x20000` also on in game mode 17, `0x40000` read from the island
  host's flags in multiplayer. Flags 160–249 have none and are set in no save checked.
- Known from PokoHex: 54 liquid absorption, 56 Surf, 57 Glide, 71–74 the Leafage, Cut, Water Gun
  and Rock Smash food boosts, 125 Rock Smash's extra food effect, 148 Dive, 153 Surf's food boost.

## Not in these fields

- Each area's placed objects are in its own `STGDATnn.BIN`.
- The active-Pokémon budget (30 by default) is a game parameter, not save data:
  `[[main+0x36ca678]]+0xc`. Pikantine raises it while the game runs.
