#!/usr/bin/env python3
"""Writes app/src/main/assets/pokopia.json for the Android save editor from the desktop editor's
tables (src/yuzu/configuration/pokopia_*.h), so both editors name things the same way.

    python make_data.py
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
CFG = os.path.join(ROOT, "src", "yuzu", "configuration")


def read(name):
    with open(os.path.join(CFG, name), encoding="utf-8") as f:
        return f.read()


def c_strings(block):
    return [bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            for s in re.findall(r'"((?:[^"\\]|\\.)*)"', block)]


def array_block(text, name):
    start = text.index(name)
    open_ = text.index("{", start)
    depth = 0
    for i in range(open_, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_ + 1:i]
    raise ValueError(name)


def main():
    dex = read("pokopia_dex_data.h")
    areas = c_strings(array_block(dex, "PokopiaAreaNames{"))
    slots = []
    for m in re.finditer(r'\{(\d+), (\d+), "((?:[^"\\]|\\.)*)", \{\{(.*?)\}\}\}', array_block(dex, "PokopiaDexSlots{{")):
        spawns = [[int(h), int(a), r == "true"] for h, a, r in
                  re.findall(r"\{(\d+), (\d+), (true|false)\}", m.group(4)) if int(h) != 0]
        slots.append({"slot": int(m.group(1)), "row": int(m.group(2)), "name": m.group(3),
                      "spawns": spawns})
    habitats = [[int(i), n] for i, n in
                re.findall(r'\{(\d+), "((?:[^"\\]|\\.)*)"\}', array_block(dex, "PokopiaHabitats{{"))]
    recipes = [[int(i), n] for i, n in
               re.findall(r'\{(\d+), "((?:[^"\\]|\\.)*)"\}', array_block(read("pokopia_recipe_data.h"), "PokopiaRecipes{{"))]
    names = read("pokopia_save_names.h")
    lists = {}
    for key, sym in (("skills", "PokopiaSkillNames"), ("reactions", "PokopiaReactionNames"),
                     ("items", "PokopiaItemNames"), ("residents", "PokopiaVillagerNames"),
                     ("stages", "PokopiaAreaTitles")):
        lists[key] = c_strings(array_block(names, sym + "{"))
    flags = read("pokopia_flag_info.h")
    known = {int(i): n for i, n in re.findall(r'\{(\d+), "([^"]*)"\}', array_block(flags, "PokopiaKnownFlags{{"))}
    out = {"areas": areas, "dex": slots, "habitats": habitats, "recipes": recipes,
           "flags": {str(k): v for k, v in sorted(known.items())}, **lists}
    path = os.path.join(HERE, "..", "app", "src", "main", "assets", "pokopia.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, separators=(",", ":"))
    print("wrote", os.path.normpath(path), {k: len(v) for k, v in out.items()})


if __name__ == "__main__":
    main()
