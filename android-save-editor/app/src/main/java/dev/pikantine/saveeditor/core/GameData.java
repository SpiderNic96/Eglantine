// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.pikantine.saveeditor.core;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Names and spawn data for Pokémon Pokopia 2.0.1, from assets/pokopia.json (made by
 * tools/make_data.py from the desktop editor's tables).
 */
public final class GameData {
    /** A habitat a Pokémon appears in: the habitat id, the areas (bit i = areas[i]), rare. */
    public static final class Spawn {
        public final int habitat;
        public final int areas;
        public final boolean rare;

        Spawn(int habitat, int areas, boolean rare) {
            this.habitat = habitat;
            this.areas = areas;
            this.rare = rare;
        }
    }

    public static final class DexSlot {
        public final int slot;
        public final int row; // the species row, which indexes the sightings
        public final String name;
        public final List<Spawn> spawns;

        DexSlot(int slot, int row, String name, List<Spawn> spawns) {
            this.slot = slot;
            this.row = row;
            this.name = name;
            this.spawns = spawns;
        }
    }

    public static final class Named {
        public final int id;
        public final String name;

        Named(int id, String name) {
            this.id = id;
            this.name = name;
        }
    }

    public final List<String> areas = new ArrayList<>();
    public final List<DexSlot> dex = new ArrayList<>();
    public final List<Named> habitats = new ArrayList<>();
    public final Map<Integer, String> habitatNames = new HashMap<>();
    public final List<Named> recipes = new ArrayList<>();
    public final List<String> skills = new ArrayList<>();
    public final List<String> reactions = new ArrayList<>();
    public final List<String> items = new ArrayList<>();
    public final List<String> residents = new ArrayList<>();
    public final List<String> stages = new ArrayList<>();
    public final Map<Integer, String> flags = new HashMap<>();

    @SuppressWarnings("unchecked")
    public static GameData parse(String json) {
        Map<String, Object> root = (Map<String, Object>) Json.parse(json);
        GameData d = new GameData();
        for (Object o : (List<Object>) root.get("areas")) {
            d.areas.add((String) o);
        }
        for (Object o : (List<Object>) root.get("dex")) {
            Map<String, Object> m = (Map<String, Object>) o;
            List<Spawn> spawns = new ArrayList<>();
            for (Object so : (List<Object>) m.get("spawns")) {
                List<Object> s = (List<Object>) so;
                spawns.add(new Spawn(num(s.get(0)), num(s.get(1)), (Boolean) s.get(2)));
            }
            d.dex.add(new DexSlot(num(m.get("slot")), num(m.get("row")), (String) m.get("name"),
                    spawns));
        }
        for (Object o : (List<Object>) root.get("habitats")) {
            List<Object> h = (List<Object>) o;
            Named n = new Named(num(h.get(0)), (String) h.get(1));
            d.habitats.add(n);
            d.habitatNames.put(n.id, n.name);
        }
        for (Object o : (List<Object>) root.get("recipes")) {
            List<Object> r = (List<Object>) o;
            d.recipes.add(new Named(num(r.get(0)), (String) r.get(1)));
        }
        strings(root.get("skills"), d.skills);
        strings(root.get("reactions"), d.reactions);
        strings(root.get("items"), d.items);
        strings(root.get("residents"), d.residents);
        strings(root.get("stages"), d.stages);
        for (Map.Entry<String, Object> e : ((Map<String, Object>) root.get("flags")).entrySet()) {
            d.flags.put(Integer.parseInt(e.getKey()), (String) e.getValue());
        }
        return d;
    }

    @SuppressWarnings("unchecked")
    private static void strings(Object o, List<String> out) {
        for (Object s : (List<Object>) o) {
            out.add((String) s);
        }
    }

    private static int num(Object o) {
        return ((Number) o).intValue();
    }

    public String habitatName(int id) {
        String n = habitatNames.get(id);
        return n != null ? n : "Habitat #" + id;
    }

    public static String nameOr(List<String> list, int id, String fallback) {
        if (id >= 0 && id < list.size() && !list.get(id).isEmpty()) {
            return list.get(id);
        }
        return fallback + " " + id;
    }

    /** "Bleak Beach, Rocky Ridges" or "all areas" for an area mask. */
    public String areaText(int mask) {
        if ((mask & 0x3f) == 0x3f) {
            return "all areas";
        }
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < areas.size(); i++) {
            if ((mask & (1 << i)) != 0) {
                if (b.length() > 0) {
                    b.append(", ");
                }
                b.append(areas.get(i));
            }
        }
        return b.toString();
    }
}
