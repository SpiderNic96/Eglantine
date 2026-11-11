#!/usr/bin/env python3
"""Find the game's Pokédex slot -> species table in a Pokopia 2.0.1 RomFS dump (read-only).

The save keeps one byte per Pokédex *slot* (see docs/pokecap/save-layout.md), and the slot is not
the printed Pokédex number. This script looks for the table that says which Pokémon each slot is.

It reads every entry of the game's data container (LI_C0_R.IDX + LD_C0_R.BIN, format from
eduardosmulero-tech/pokopia-tools, GPL-3.0) and every other small file in the dump, and searches
each one for rows where the known slots line up:
    slot 1 Bulbasaur, 4 Charmander, 6 Charizard, 7 Squirtle, 8 Wartortle, 13 Pidgey, 18 Oddish
(read off SpiderNic96's save and his Pokédex screen). It tries 1-, 2- and 4-byte fields, any row
size up to 256 bytes, and both the National Dex number and the Pokopia Pokédex number.

    python find_dex_table.py "C:\\path\\to\\romfs"

Get the RomFS in Eden/Eglantine: right-click Pokopia -> Dump RomFS (with the 2.0.1 update
installed). Nothing is written except pikantine_dex_slots.json next to this script, which holds
what was found (numbers and the English names below, no game data). Send that file back.
Needs Python 3.9+ and numpy (pip install numpy).
"""
import json
import mmap
import os
import struct
import sys
import zlib

import numpy as np

# Known slots: (National Dex number, Pokopia Pokédex number)
KNOWN = {1: (1, 1), 4: (4, 4), 6: (6, 6), 7: (7, 7), 8: (8, 8), 13: (16, 10), 18: (43, 13)}
MAX_STRIDE = 256
MAX_SLOTS = 1024
MAX_ENTRY = 64 * 1024 * 1024  # bigger entries are textures/models, not tables


NATDEX = {1: 'Bulbasaur', 2: 'Ivysaur', 3: 'Venusaur', 4: 'Charmander', 5: 'Charmeleon', 6: 'Charizard', 7: 'Squirtle', 8: 'Wartortle', 9: 'Blastoise', 16: 'Pidgey', 17: 'Pidgeotto', 18: 'Pidgeot', 23: 'Ekans', 24: 'Arbok', 25: 'Pikachu', 26: 'Raichu', 35: 'Clefairy', 36: 'Clefable', 37: 'Vulpix', 38: 'Ninetales', 39: 'Jigglypuff', 40: 'Wigglytuff', 41: 'Zubat', 42: 'Golbat', 43: 'Oddish', 44: 'Gloom', 45: 'Vileplume', 46: 'Paras', 47: 'Parasect', 48: 'Venonat', 49: 'Venomoth', 50: 'Diglett', 51: 'Dugtrio', 52: 'Meowth', 53: 'Persian', 54: 'Psyduck', 55: 'Golduck', 58: 'Growlithe', 59: 'Arcanine', 60: 'Poliwag', 61: 'Poliwhirl', 62: 'Poliwrath', 63: 'Abra', 64: 'Kadabra', 65: 'Alakazam', 66: 'Machop', 67: 'Machoke', 68: 'Machamp', 69: 'Bellsprout', 70: 'Weepinbell', 71: 'Victreebel', 74: 'Geodude', 75: 'Graveler', 76: 'Golem', 79: 'Slowpoke', 80: 'Slowbro', 81: 'Magnemite', 82: 'Magneton', 83: "Farfetch'd", 88: 'Grimer', 89: 'Muk', 91: 'Cloyster', 92: 'Gastly', 93: 'Haunter', 94: 'Gengar', 95: 'Onix', 100: 'Voltorb', 101: 'Electrode', 102: 'Exeggcute', 103: 'Exeggutor', 104: 'Cubone', 105: 'Marowak', 106: 'Hitmonlee', 107: 'Hitmonchan', 109: 'Koffing', 110: 'Weezing', 113: 'Chansey', 114: 'Tangela', 116: 'Horsea', 117: 'Seadra', 120: 'Staryu', 121: 'Starmie', 122: 'Mr. Mime', 123: 'Scyther', 125: 'Electabuzz', 126: 'Magmar', 127: 'Pinsir', 129: 'Magikarp', 130: 'Gyarados', 131: 'Lapras', 132: 'Ditto', 133: 'Eevee', 134: 'Vaporeon', 135: 'Jolteon', 136: 'Flareon', 137: 'Porygon', 142: 'Aerodactyl', 143: 'Snorlax', 144: 'Articuno', 145: 'Zapdos', 146: 'Moltres', 147: 'Dratini', 148: 'Dragonair', 149: 'Dragonite', 150: 'Mewtwo', 151: 'Mew', 155: 'Cyndaquil', 156: 'Quilava', 157: 'Typhlosion', 158: 'Totodile', 159: 'Croconaw', 160: 'Feraligatr', 163: 'Hoothoot', 164: 'Noctowl', 167: 'Spinarak', 168: 'Ariados', 169: 'Crobat', 170: 'Chinchou', 171: 'Lanturn', 172: 'Pichu', 173: 'Cleffa', 174: 'Igglybuff', 179: 'Mareep', 180: 'Flaaffy', 181: 'Ampharos', 182: 'Bellossom', 183: 'Marill', 184: 'Azumarill', 185: 'Sudowoodo', 186: 'Politoed', 187: 'Hopip', 188: 'Skiploom', 189: 'Jumpluff', 196: 'Espeon', 197: 'Umbreon', 198: 'Murkrow', 199: 'Slowking', 200: 'Misdreavus', 203: 'Girafarig', 208: 'Steelix', 212: 'Scizor', 214: 'Heracross', 222: 'Corsola', 230: 'Kingdra', 233: 'Porygon2', 236: 'Tyrogue', 237: 'Hitmontop', 239: 'Elekid', 240: 'Magby', 242: 'Blissey', 243: 'Raikou', 244: 'Entei', 245: 'Suicune', 246: 'Larvitar', 247: 'Pupitar', 248: 'Tyranitar', 249: 'Lugia', 250: 'Ho-Oh', 255: 'Torchic', 256: 'Combusken', 257: 'Blaziken', 258: 'Mudkip', 259: 'Marshtomp', 260: 'Swampert', 270: 'Lotad', 271: 'Lombre', 272: 'Ludicolo', 278: 'Wingull', 279: 'Pelipper', 280: 'Ralts', 281: 'Kirlia', 282: 'Gardevoir', 296: 'Makuhita', 297: 'Hariyama', 298: 'Azurill', 302: 'Sableye', 303: 'Mawile', 311: 'Plusle', 312: 'Minun', 313: 'Volbeat', 314: 'Illumise', 316: 'Gulpin', 317: 'Swalot', 318: 'Carvanha', 319: 'Sharpedo', 324: 'Torkoal', 328: 'Trapinch', 329: 'Vibrava', 330: 'Flygon', 331: 'Cacnea', 332: 'Cacturne', 333: 'Swablu', 334: 'Altaria', 339: 'Barboach', 340: 'Whiscash', 341: 'Corphish', 342: 'Crawdaunt', 349: 'Shellder', 355: 'Duskull', 356: 'Dusclops', 359: 'Absol', 370: 'Luvdisc', 374: 'Beldum', 375: 'Metang', 376: 'Metagross', 382: 'Kyogre', 385: 'Jirachi', 393: 'Piplup', 394: 'Prinplup', 395: 'Empoleon', 401: 'Kricketot', 402: 'Kricketune', 408: 'Cranidos', 409: 'Rampardos', 410: 'Shieldon', 411: 'Bastiodon', 416: 'Vespiquen', 418: 'Buizel', 419: 'Floatzel', 422: '西', 423: '西', 425: 'Drifloon', 426: 'Drifblim', 429: 'Mismagius', 430: 'Honchkrow', 438: 'Bonsly', 439: 'Mime Jr.', 440: 'Happiny', 441: 'Chatot', 446: 'Munchlax', 447: 'Riolu', 448: 'Lucario', 456: 'Finneon', 457: 'Lumineon', 462: 'Magnezone', 465: 'Professor Tangrowth', 466: 'Electivire', 467: 'Magmortar', 470: 'Leafeon', 471: 'Glaceon', 474: 'Porygon-Z', 475: 'Gallade', 477: 'Dusknoir', 489: 'Phione', 490: 'Manaphy', 495: 'Snivy', 496: 'Servine', 497: 'Serperior', 529: 'Drilbur', 530: 'Excadrill', 531: 'Audino', 532: 'Timburr', 533: 'Gurdurr', 534: 'Conkeldurr', 568: 'Trubbish', 569: 'Garbodor', 570: 'Zorua', 571: 'Zoroark', 572: 'Minccino', 573: 'Cinccino', 592: '雄性', 593: '雄性', 594: 'Alomomola', 607: 'Litwick', 608: 'Lampent', 609: 'Chandelure', 610: 'Axew', 611: 'Fraxure', 612: 'Haxorus', 618: 'Stunfisk', 636: 'Larvesta', 637: 'Volcarona', 656: 'Froakie', 657: 'Frogadier', 658: 'Greninja', 686: 'Inkay', 687: 'Malamar', 696: 'Tyrunt', 697: 'Tyrantrum', 698: 'Amaura', 699: 'Aurorus', 700: 'Sylveon', 702: 'Dedenne', 704: 'Goomy', 705: 'Sliggoo', 706: 'Goodra', 714: 'Noibat', 715: 'Noivern', 721: 'Volcanion', 722: 'Rowlet', 723: 'Dartrix', 724: 'Decidueye', 728: 'Popplio', 729: 'Brionne', 730: 'Primarina', 736: 'Grubbin', 737: 'Charjabug', 738: 'Vikavolt', 747: 'Mareanie', 748: 'Toxapex', 767: 'Wimpod', 768: 'Golisopod', 778: 'Mimikyu', 779: 'Bruxish', 781: 'Dhelmise', 813: 'Scorbunny', 814: 'Raboot', 815: 'Cinderace', 819: 'Skwovet', 821: 'Rookidee', 822: 'Corvisquire', 823: 'Corviknight', 833: 'Chewtle', 834: 'Drednaw', 837: 'Rolycoly', 838: 'Carkol', 839: 'Coalossal', 845: 'Cramorant', 848: 'Toxel', 849: '低調形態', 871: 'Pincurchin', 885: 'Dreepy', 886: 'Drakloak', 887: 'Dragapult', 906: 'Sprigatito', 907: 'Floragato', 908: 'Meowscarada', 921: 'Pawmi', 922: 'Pawmo', 923: 'Pawmot', 926: 'Fidough', 927: 'Dachsbun', 935: 'Charcadet', 936: 'Armarouge', 937: 'Ceruledge', 940: 'Wattrel', 941: 'Kilowattrel', 957: 'Tinkatink', 958: 'Tinkatuff', 960: 'Wiglett', 961: 'Wugtrio', 969: 'Glimmet', 970: 'Glimmora', 976: 'Veluza', 978: '上弓姿勢', 980: 'Clodsire', 981: 'Farigiraf', 999: 'Gimmighoul', 1000: 'Gholdengo'}
POKOPIA_NO = {1: 'Bulbasaur', 2: 'Ivysaur', 3: 'Venusaur', 4: 'Charmander', 5: 'Charmeleon', 6: 'Charizard', 7: 'Squirtle', 8: 'Wartortle', 9: 'Blastoise', 10: 'Pidgey', 11: 'Pidgeotto', 12: 'Pidgeot', 13: 'Oddish', 14: 'Gloom', 15: 'Vileplume', 16: 'Bellossom', 17: 'Paras', 18: 'Parasect', 19: 'Venonat', 20: 'Venomoth', 21: 'Bellsprout', 22: 'Weepinbell', 23: 'Victreebel', 24: 'Slowpoke', 25: 'Slowbro', 26: 'Slowking', 27: 'Magnemite', 28: 'Magneton', 29: 'Magnezone', 30: 'Onix', 31: 'Steelix', 32: 'Cubone', 33: 'Marowak', 34: 'Tyrogue', 35: 'Hitmonlee', 36: 'Hitmonchan', 37: 'Hitmontop', 38: 'Koffing', 39: 'Weezing', 40: 'Tangela', 41: 'Professor Tangrowth', 42: 'Scyther', 43: 'Scizor', 44: 'Pinsir', 45: 'Magikarp', 46: 'Gyarados', 47: 'Ditto', 48: 'Hoothoot', 49: 'Noctowl', 50: 'Heracross', 51: 'Volbeat', 52: 'Illumise', 53: 'Gulpin', 54: 'Swalot', 55: 'Cacnea', 56: 'Cacturne', 57: 'Combee', 58: 'Vespiquen', 59: '西', 60: '西', 61: 'Drifloon', 62: 'Drifblim', 63: 'Drilbur', 64: 'Excadrill', 65: 'Timburr', 66: 'Gurdurr', 67: 'Conkeldurr', 68: 'Litwick', 69: 'Lampent', 70: 'Chandelure', 71: 'Axew', 72: 'Fraxure', 73: 'Haxorus', 74: 'Goomy', 75: 'Sliggoo', 76: 'Goodra', 77: 'Cramorant', 78: 'Pichu', 79: 'Peakychu', 80: 'Raichu', 81: 'Zubat', 82: 'Golbat', 83: 'Crobat', 84: 'Meowth', 85: 'Persian', 86: 'Psyduck', 87: 'Golduck', 88: 'Growlithe', 89: 'Arcanine', 90: "Farfetch'd", 91: 'Grimer', 92: 'Muk', 93: 'Gastly', 94: 'Haunter', 95: 'Gengar', 96: 'Voltorb', 97: 'Electrode', 98: 'Exeggcute', 99: 'Exeggutor', 100: 'Happiny', 101: 'Chansey', 102: 'Blissey', 103: 'Elekid', 104: 'Electabuzz', 105: 'Electivire', 106: 'Lapras', 107: 'Munchlax', 108: 'Mosslax', 109: 'Spinarak', 110: 'Ariados', 111: 'Mareep', 112: 'Flaaffy', 113: 'Ampharos', 114: 'Azurill', 115: 'Marill', 116: 'Azumarill', 117: 'Paldean Wooper', 118: 'Clodsire', 119: 'Smeargle', 120: 'Torchic', 121: 'Combusken', 122: 'Blaziken', 123: 'Wingull', 124: 'Pelipper', 125: 'Makuhita', 126: 'Hariyama', 127: 'Absol', 128: 'Piplup', 129: 'Prinplup', 130: 'Empoleon', 131: 'Audino', 132: 'Trubbish', 133: 'Garbodor', 134: 'Zorua', 135: 'Zoroark', 136: 'Minccino', 137: 'Cinccino', 138: 'Grubbin', 139: 'Charjabug', 140: 'Vikavolt', 141: 'Mimikyu', 142: 'Pawmi', 143: 'Pawmo', 144: 'Pawmot', 145: '上弓姿勢', 146: 'Ekans', 147: 'Arbok', 148: 'Cleffa', 149: 'Clefairy', 150: 'Clefable', 151: 'Igglybuff', 152: 'Jigglypuff', 153: 'Wigglytuff', 154: 'Diglett', 155: 'Dugtrio', 156: 'Machop', 157: 'Machoke', 158: 'Machamp', 159: 'Geodude', 160: 'Graveler', 161: 'Golem', 162: 'Magby', 163: 'Magmar', 164: 'Magmortar', 165: 'Bonsly', 166: 'Sudowoodo', 167: 'Murkrow', 168: 'Honchkrow', 169: 'Larvitar', 170: 'Pupitar', 171: 'Tyranitar', 172: 'Lotad', 173: 'Lombre', 174: 'Ludicolo', 175: 'Mawile', 176: 'Torkoal', 177: 'Kricketot', 178: 'Kricketune', 179: 'Chatot', 180: 'Riolu', 181: 'Lucario', 182: '音響形態', 183: 'Larvesta', 184: 'Volcarona', 185: 'Rowlet', 186: 'Dartrix', 187: 'Decidueye', 188: 'Scorbunny', 189: 'Raboot', 190: 'Cinderace', 191: 'Skwovet', 192: 'Greedent', 193: 'Rolycoly', 194: 'Carkol', 195: 'Coalossal', 196: 'Toxel', 197: '高調形態', 198: 'Fidough', 199: 'Dachsbun', 200: 'Charcadet', 201: 'Armarouge', 202: 'Ceruledge', 203: 'Glimmet', 204: 'Glimmora', 205: 'Gimmighoul', 206: 'Gholdengo', 207: 'Vulpix', 208: 'Ninetales', 209: 'Poliwag', 210: 'Poliwhirl', 211: 'Poliwrath', 212: 'Politoed', 213: 'Abra', 214: 'Kadabra', 215: 'Alakazam', 216: 'Mime Jr.', 217: 'Mr. Mime', 218: 'Porygon', 219: 'Porygon2', 220: 'Porygon-Z', 221: 'Dratini', 222: 'Dragonair', 223: 'Dragonite', 224: 'Cyndaquil', 225: 'Quilava', 226: 'Typhlosion', 227: 'Misdreavus', 228: 'Mismagius', 229: 'Girafarig', 230: 'Farigiraf', 231: 'Ralts', 232: 'Kirlia', 233: 'Gardevoir', 234: 'Gallade', 235: 'Plusle', 236: 'Minun', 237: 'Trapinch', 238: 'Vibrava', 239: 'Flygon', 240: 'Swablu', 241: 'Altaria', 242: 'Duskull', 243: 'Dusclops', 244: 'Dusknoir', 245: 'Beldum', 246: 'Metang', 247: 'Metagross', 248: 'Snivy', 249: 'Servine', 250: 'Serperior', 251: 'Froakie', 252: 'Frogadier', 253: 'Greninja', 254: 'Dedenne', 255: 'Noibat', 256: 'Noivern', 257: 'Rookidee', 258: 'Corvisquire', 259: 'Corviknight', 260: 'Dreepy', 261: 'Drakloak', 262: 'Dragapult', 263: 'Sprigatito', 264: 'Floragato', 265: 'Meowscarada', 266: 'Wattrel', 267: 'Kilowattrel', 268: 'Tinkatink', 269: 'Tinkatuff', 270: 'Tinkaton', 271: 'Aerodactyl', 272: 'Cranidos', 273: 'Rampardos', 274: 'Shieldon', 275: 'Bastiodon', 276: 'Tyrunt', 277: 'Tyrantrum', 278: 'Amaura', 279: 'Aurorus', 280: 'Eevee', 281: 'Vaporeon', 282: 'Jolteon', 283: 'Flareon', 284: 'Espeon', 285: 'Umbreon', 286: 'Leafeon', 287: 'Glaceon', 288: 'Sylveon', 289: 'Kyogre', 290: 'Raikou', 291: 'Entei', 292: 'Suicune', 293: 'Volcanion', 294: 'Articuno', 295: 'Zapdos', 296: 'Moltres', 297: 'Lugia', 298: 'Ho-Oh', 299: 'Mewtwo', 300: 'Mew'}


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


def search(blob):
    """Returns (encoding, width, stride_bytes, base_byte) for every place the known slots line up."""
    hits = []
    for width, dtype in ((1, "u1"), (2, "<u2"), (4, "<u4")):
        for start in range(width):
            n = (len(blob) - start) // width
            if n < 32:
                continue
            a = np.frombuffer(blob, dtype, n, start)
            for which, label in ((0, "national"), (1, "pokopia")):
                want = {s: v[which] for s, v in KNOWN.items()}
                cand = np.nonzero(a == want[18])[0]
                if cand.size == 0:
                    continue
                for stride in range(1, MAX_STRIDE // width + 1):
                    base = cand - 18 * stride  # element index of slot 0
                    ok = base + stride >= 0
                    for s, v in want.items():
                        if s == 18 or not ok.any():
                            continue
                        idx = base + s * stride
                        good = ok & (idx >= 0)
                        ok = good & (a[np.where(good, idx, 0)] == v)
                    for b in base[ok][:4]:
                        hits.append((label, width, stride * width, start + int(b) * width))
    return hits


def read_table(blob, width, stride, base):
    fmt = {1: "<B", 2: "<H", 4: "<I"}[width]
    rows = {}
    for slot in range(MAX_SLOTS):
        at = base + slot * stride
        if at < 0:
            continue
        if at + width > len(blob):
            break
        rows[slot] = struct.unpack_from(fmt, blob, at)[0]
    return rows


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        raise SystemExit(__doc__)
    root = sys.argv[1]
    found = []
    pairs = []
    others = []
    for d, _, files in os.walk(root):
        names = {f.upper(): f for f in files}
        if "LI_C0_R.IDX" in names and "LD_C0_R.BIN" in names:
            pairs.append((os.path.join(d, names["LI_C0_R.IDX"]), os.path.join(d, names["LD_C0_R.BIN"])))
        for f in files:
            p = os.path.join(d, f)
            if f.upper() in ("LI_C0_R.IDX", "LD_C0_R.BIN") or f.lower().endswith(".fdata"):
                continue
            if os.path.getsize(p) <= MAX_ENTRY:
                others.append(p)
    if not pairs:
        print("No LI_C0_R.IDX / LD_C0_R.BIN found under that folder: is it the RomFS dump?")
    for idx_path, bin_path in pairs:
        print(f"Container: {bin_path}")
        for n, blob in container_entries(idx_path, bin_path):
            if n % 500 == 0:
                print(f"  entry {n}...", flush=True)
            for hit in search(blob):
                found.append({"source": f"container entry {n}", "size": len(blob), "hit": hit,
                              "table": read_table(blob, hit[1], hit[2], hit[3]),
                              "head": blob[max(0, hit[3]):hit[3] + hit[2] * 3].hex()})
    print(f"Other files: {len(others)}")
    for p in others:
        blob = open(p, "rb").read()
        for hit in search(blob):
            found.append({"source": os.path.relpath(p, root), "size": len(blob), "hit": hit,
                          "table": read_table(blob, hit[1], hit[2], hit[3]),
                          "head": blob[max(0, hit[3]):hit[3] + hit[2] * 3].hex()})
    for f in found:
        label = f["hit"][0]
        names = NATDEX if label == "national" else POKOPIA_NO
        f["named"] = {s: names.get(v, "") for s, v in f["table"].items() if v in names}
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pikantine_dex_slots.json")
    json.dump(found, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"\n{len(found)} candidate table(s); written to {out}")
    for f in found[:10]:
        label, width, stride, base = f["hit"]
        sample = ", ".join(f"{s}:{n}" for s, n in list(f["named"].items())[:12])
        print(f"- {f['source']}: {label} number, {width}-byte field, row {stride} B, at {base:#x}; "
              f"{len(f['named'])} slots named: {sample} ...")


if __name__ == "__main__":
    main()
