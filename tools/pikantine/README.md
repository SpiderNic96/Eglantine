# Pikantine research tools (Pokémon Pokopia)

Python 3.9+. Both only read the game and your saves; they never change them.

## save_watch.py: catch exactly what the game writes

```
python save_watch.py "<save folder holding G00>"
```

Leave it running while you play. Every time the game saves, the whole save folder is copied to
`pokopia_snapshots/<time>/` and `changes.txt` there lists what changed since the last save.
Register one new Pokémon, Save & Quit, and the snapshot made then holds the Pokédex change.
Compare any two snapshots with `python save_watch.py --diff <A> <B>`.

## scan_game_files.py: find the Pokédex table in the game files

```
pip install numpy
python scan_game_files.py "<folder with the dumped game>"
```

Looks through every file (and the zlib streams inside them) for a table listing Bulbasaur,
Charmander, Charizard, Squirtle, …, Pidgey, Oddish in the in-game Pokédex's order. `LIKELY`
lines in `scan_report.txt` are the ones to send back.

## dex_report.py

Read-only: prints how many Pokémon a `CMNDAT.BIN` has registered and seen, how many habitats are
built and discovered, and the dex rewards claimed. The layout behind it is in
[docs/pokecap/save-layout.md](../../docs/pokecap/save-layout.md).

    python dex_report.py "%APPDATA%\eglantine\nand\user\save\...\G00\CMNDAT.BIN"

## find_dex_table.py

Read-only: searches a RomFS dump for the table that says which Pokémon each Pokédex slot is (the
save stores slots, not Pokédex numbers). Writes `pikantine_dex_slots.json` next to itself; send it
back so the Pokédex editor can show names. Needs `pip install numpy`.

    python find_dex_table.py "C:\path\to\romfs"

## find_dex_names.py

Second try at the Pokédex slot order, from the game's text: finds lists of Pokémon names whose
positions follow the known slots. Writes `pikantine_dex_names.json` next to itself. No packages
needed.

    python find_dex_names.py "C:\path\to\romfs"
