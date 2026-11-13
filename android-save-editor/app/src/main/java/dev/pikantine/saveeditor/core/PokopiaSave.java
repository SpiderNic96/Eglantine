// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.pikantine.saveeditor.core;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Calendar;
import java.util.List;
import java.util.zip.DataFormatException;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * Pokémon Pokopia 2.0.1's common save (G00/CMNDAT.BIN): a 0x130-byte header, then one zlib
 * stream. Every edit goes straight into the unpacked data; {@link #pack} writes the file again
 * and checks it reads back the same. The offsets are the desktop editor's
 * (src/yuzu/configuration/pokopia_dex_dialog.cpp and pokopia_save_tabs.cpp, docs/pokecap).
 */
public final class PokopiaSave {
    public static final int MAGIC = 0x31227687;
    public static final int LAYOUT_201 = 0x0200017D;
    public static final int FORMAT = 0x40001;
    public static final int HEADER = 0x130;
    public static final int DATA_SIZE = 0x470CED;

    static final int DEX = 0x426EFF, DEX_COUNT = 0x400;
    static final int REGISTERED = 0x03, SEEN = 0x08;
    static final int HABITATS = 0x4256FF, HABITAT_COUNT = 0x1800 / 12;
    static final int HISTORY = 0x4052FF, HISTORY_RECORD = 129, HISTORY_COUNT = 0x20400 / 129;
    static final int RECIPES = 0x3FCB4C, RECIPE_COUNT = 0x2328 / 6;
    static final int ABILITIES = 0x405047, SKILL_SIZE = 8;
    static final int SKILL_WHEEL = ABILITIES + 64 * SKILL_SIZE, SKILL_WHEEL_COUNT = 8;
    static final int REACTIONS = SKILL_WHEEL + SKILL_WHEEL_COUNT, REACTION_COUNT = 128;
    static final int REACTION_WHEEL = REACTIONS + REACTION_COUNT, REACTION_WHEEL_COUNT = 24;
    static final int FLAGS = 0x220F04, FLAG_COUNT = 250;
    static final int INVENTORY = 0x46ECEA, INVENTORY_COUNT = 130, BAG_COUNT = 120, ITEM_SIZE = 5;
    static final int ITEM_ID_MASK = 0x1FFF;
    static final int VILLAGERS = 0, VILLAGER_COUNT = 1050, VILLAGER_SIZE = 374;
    static final int HOMES = 0x79824, HOME_SIZE = 21;
    static final int PLAYER_NAME = 0x46E0E5, PLAYER_NAME_SIZE = 25;
    public static final int FRIENDSHIP_MAX = 170;

    /** Moves the player can learn (the rest of the 64 are unused or the game's own). */
    public static final int[] PUBLIC_SKILLS = {1, 2, 3, 4, 5, 6, 10, 11, 12, 13};

    private final byte[] original; // the file as read
    private final byte[] before;   // its data
    private final byte[] data;     // the data being edited

    private PokopiaSave(byte[] file, byte[] unpacked) {
        original = file;
        before = unpacked.clone();
        data = unpacked;
    }

    // --- File -----------------------------------------------------------------------------------

    public static PokopiaSave read(byte[] file) {
        if (file.length < HEADER || le32(file, 0) != MAGIC) {
            throw new IllegalArgumentException("This is not a Pokopia common save (G00/CMNDAT.BIN).");
        }
        if (le32(file, 4) != LAYOUT_201 || le32(file, 8) != FORMAT) {
            throw new IllegalArgumentException("This save is not from Pokopia 2.0.1; only 2.0.1 saves can be edited.");
        }
        int length = le32(file, 0x10);
        if (length <= HEADER || length > file.length) {
            throw new IllegalArgumentException("The save's length is invalid.");
        }
        Inflater inflater = new Inflater();
        inflater.setInput(file, HEADER, length - HEADER);
        byte[] out = new byte[DATA_SIZE];
        try {
            int n = 0;
            while (n < out.length && !inflater.finished()) {
                int got = inflater.inflate(out, n, out.length - n);
                if (got == 0 && (inflater.needsInput() || inflater.needsDictionary())) {
                    break;
                }
                n += got;
            }
            if (n != DATA_SIZE || !inflater.finished()) {
                throw new IllegalArgumentException("The save could not be unpacked.");
            }
        } catch (DataFormatException e) {
            throw new IllegalArgumentException("The save could not be unpacked: " + e.getMessage());
        } finally {
            inflater.end();
        }
        return new PokopiaSave(file, out);
    }

    /** The edited file, checked by reading it back. The file never gets shorter than the old one. */
    public byte[] pack() {
        Deflater deflater = new Deflater(9);
        deflater.setInput(data);
        deflater.finish();
        ByteArrayOutputStream z = new ByteArrayOutputStream(original.length);
        byte[] buf = new byte[1 << 16];
        while (!deflater.finished()) {
            int n = deflater.deflate(buf);
            z.write(buf, 0, n);
        }
        deflater.end();
        byte[] packed = z.toByteArray();
        int length = HEADER + packed.length;
        byte[] file = new byte[Math.max(length, original.length)];
        System.arraycopy(original, 0, file, 0, HEADER);
        System.arraycopy(packed, 0, file, HEADER, packed.length);
        put32(file, 0x10, length);
        PokopiaSave check = read(file);
        if (!Arrays.equals(check.data, data)) {
            throw new IllegalStateException("The edited save did not read back the same; nothing was written.");
        }
        return file;
    }

    public boolean isChanged() {
        return !Arrays.equals(data, before);
    }

    public int changedBytes() {
        int n = 0;
        for (int i = 0; i < data.length; i++) {
            if (data[i] != before[i]) {
                n++;
            }
        }
        return n;
    }

    /** Throws away every edit since the save was read. */
    public void revert() {
        System.arraycopy(before, 0, data, 0, data.length);
    }

    public String playerName() {
        for (int i = 0; i < 9; i++) {
            int at = PLAYER_NAME + i * PLAYER_NAME_SIZE;
            if (data[at] != 0) {
                int end = at;
                while (end < at + PLAYER_NAME_SIZE && data[end] != 0) {
                    end++;
                }
                return new String(data, at, end - at, StandardCharsets.UTF_8);
            }
        }
        return "";
    }

    // --- Pokédex, habitats, sightings ----------------------------------------------------------

    /** 0 not yet, 1 seen, 2 registered. */
    public int dexState(int slot) {
        int f = data[DEX + slot] & 0xff;
        return (f & REGISTERED) == REGISTERED ? 2 : (f & SEEN) != 0 ? 1 : 0;
    }

    public int dexStateBefore(int slot) {
        int f = before[DEX + slot] & 0xff;
        return (f & REGISTERED) == REGISTERED ? 2 : (f & SEEN) != 0 ? 1 : 0;
    }

    /**
     * Sets a Pokédex slot. A Pokémon newly registered can also have its habitats marked
     * discovered (`linkHabitats`) and be marked as seen in each of them (`fillSightings`, which
     * fills the "?" on habitat pages).
     */
    public void setDex(GameData.DexSlot p, int state, boolean linkHabitats, boolean fillSightings) {
        int at = DEX + p.slot;
        int f = data[at] & 0xff;
        boolean newly = state == 2 && dexState(p.slot) != 2;
        if (state == 2) {
            f |= REGISTERED;
        } else if (state == 1) {
            f = (f & ~REGISTERED) | SEEN;
        } else {
            f = 0;
        }
        data[at] = (byte) f;
        if (state == 2 && newly && linkHabitats) {
            for (GameData.Spawn s : p.spawns) {
                if (habitatState(s.habitat) == 0) {
                    setHabitat(s.habitat, 1);
                }
            }
        }
        if (state == 2 && fillSightings) {
            fillSightings(p);
        }
    }

    /** Marks a Pokémon as seen in every habitat it appears in. */
    public void fillSightings(GameData.DexSlot p) {
        if (p.row >= HISTORY_COUNT) {
            return;
        }
        int record = HISTORY + p.row * HISTORY_RECORD;
        for (GameData.Spawn s : p.spawns) {
            if (s.habitat <= 0 || s.habitat >= 512) {
                continue;
            }
            for (int copy : new int[] {0, 64}) {
                data[record + copy + s.habitat / 8] |= (byte) (1 << (s.habitat % 8));
            }
        }
    }

    /** 0 not yet, 1 discovered, 2 built. */
    public int habitatState(int id) {
        int v = le32(data, HABITATS + id * 12);
        return v == 3 ? 2 : v != 0 ? 1 : 0;
    }

    public int habitatStateBefore(int id) {
        int v = le32(before, HABITATS + id * 12);
        return v == 3 ? 2 : v != 0 ? 1 : 0;
    }

    public void setHabitat(int id, int state) {
        if (id <= 0 || id >= HABITAT_COUNT) {
            return;
        }
        int at = HABITATS + id * 12;
        if (state == habitatState(id)) {
            return;
        }
        if (state == 2) {
            put32(data, at, 3);
        } else if (state == 1) {
            put32(data, at, 1);
        } else {
            Arrays.fill(data, at, at + 12, (byte) 0);
        }
    }

    // --- Recipes ----------------------------------------------------------------------------------

    public boolean recipeLearned(int index) {
        int at = RECIPES + index * 6;
        for (int i = 0; i < 6; i++) {
            if (data[at + i] != 0) {
                return true;
            }
        }
        return false;
    }

    public boolean recipeLearnedBefore(int index) {
        int at = RECIPES + index * 6;
        for (int i = 0; i < 6; i++) {
            if (before[at + i] != 0) {
                return true;
            }
        }
        return false;
    }

    /** Learns a recipe after the last one learned, marked new. Learned recipes stay as they are. */
    public void learnRecipe(int index) {
        if (index <= 0 || index >= RECIPE_COUNT || recipeLearned(index)) {
            return;
        }
        int order = 0;
        for (int i = 0; i < RECIPE_COUNT; i++) {
            order = Math.max(order, le16(data, RECIPES + i * 6 + 2));
        }
        int at = RECIPES + index * 6;
        put16(data, at, 0);
        put16(data, at + 2, order + 1);
        put16(data, at + 4, 2);
    }

    /** Takes back a recipe learned in this session (one the save had stays). */
    public void unlearnRecipe(int index) {
        if (!recipeLearnedBefore(index)) {
            Arrays.fill(data, RECIPES + index * 6, RECIPES + index * 6 + 6, (byte) 0);
        }
    }

    // --- Moves --------------------------------------------------------------------------------

    /** Surf and Glide also need a permission flag before the game counts them as learned. */
    static int skillPermission(int id) {
        return id == 10 ? 56 : id == 11 ? 57 : 0;
    }

    static boolean skillOnWheel(int id) {
        return (id >= 1 && id <= 6) || id == 12 || id == 13;
    }

    public boolean skillLearned(int id) {
        boolean bit = (data[ABILITIES + id * SKILL_SIZE + 4] & 1) != 0;
        int p = skillPermission(id);
        return bit && (p == 0 || flag(p));
    }

    public long skillUses(int id) {
        return le32(data, ABILITIES + id * SKILL_SIZE) & 0xffffffffL;
    }

    public void learnSkill(int id) {
        int p = skillPermission(id);
        if (p != 0) {
            setFlag(p, true);
        }
        data[ABILITIES + id * SKILL_SIZE + 4] |= 1;
        if (!skillOnWheel(id)) {
            return;
        }
        int empty = -1;
        for (int i = 0; i < SKILL_WHEEL_COUNT; i++) {
            int v = data[SKILL_WHEEL + i] & 0xff;
            if (v == id) {
                return;
            }
            if (v == 0 && empty < 0) {
                empty = i;
            }
        }
        if (empty >= 0) {
            data[SKILL_WHEEL + empty] = (byte) id;
        }
    }

    /** An unlock that hangs off a move: function flags, and the move it needs. */
    public static final class Upgrade {
        public final String name;
        public final int needs;
        public final int[] flags;
        public final boolean needsDive;

        Upgrade(String name, int needs, int[] flags, boolean needsDive) {
            this.name = name;
            this.needs = needs;
            this.flags = flags;
            this.needsDive = needsDive;
        }
    }

    public static final Upgrade[] UPGRADES = {
        new Upgrade("Liquid absorption (Water Gun)", 3, new int[] {54}, false),
        new Upgrade("Dive (Surf)", 10, new int[] {148}, false),
        new Upgrade("Leafage food boost", 1, new int[] {71}, false),
        new Upgrade("Water Gun food boost", 3, new int[] {73}, false),
        new Upgrade("Rock Smash food boost and extra effect", 4, new int[] {74, 125}, false),
        new Upgrade("Cut food boost", 5, new int[] {72}, false),
        new Upgrade("Surf food boost", 10, new int[] {153}, true),
    };

    public boolean upgradeUnlocked(Upgrade u) {
        for (int f : u.flags) {
            if (!flag(f)) {
                return false;
            }
        }
        return true;
    }

    /** Unlocks an upgrade, learning the move (and Dive) it needs first. */
    public void unlockUpgrade(Upgrade u) {
        if (!skillLearned(u.needs)) {
            learnSkill(u.needs);
        }
        if (u.needsDive && !flag(148)) {
            setFlag(148, true);
        }
        for (int f : u.flags) {
            setFlag(f, true);
        }
    }

    // --- Reactions ----------------------------------------------------------------------------

    public boolean reactionLearned(int id) {
        return (data[REACTIONS + id] & 1) != 0;
    }

    public void learnReaction(int id) {
        data[REACTIONS + id] |= 1;
        int empty = -1;
        for (int slot = 0; slot < REACTION_WHEEL_COUNT; slot++) {
            int at = data[REACTION_WHEEL + slot * 2] & 0xff;
            if (at == id) {
                return;
            }
            if (at == 0 && empty < 0) {
                empty = slot;
            }
        }
        if (empty >= 0) {
            data[REACTION_WHEEL + empty * 2] = (byte) id;
            data[REACTION_WHEEL + empty * 2 + 1] = 0;
        }
    }

    // --- Bag ----------------------------------------------------------------------------------

    public int itemId(int slot) {
        return le32(data, INVENTORY + slot * ITEM_SIZE) & ITEM_ID_MASK;
    }

    public int itemCount(int slot) {
        return data[INVENTORY + slot * ITEM_SIZE + 4] & 0xff;
    }

    /** Puts `count` of `item` in a slot; item or count 0 empties it. */
    public void setItem(int slot, int item, int count) {
        int at = INVENTORY + slot * ITEM_SIZE;
        if (item == 0 || count <= 0) {
            Arrays.fill(data, at, at + ITEM_SIZE, (byte) 0);
            return;
        }
        if (itemId(slot) != item) {
            put32(data, at, item & ITEM_ID_MASK); // the other bits belong to the old item
        }
        data[at + 4] = (byte) Math.min(255, count);
    }

    /** The first empty bag slot, or -1. */
    public int firstEmptyBagSlot() {
        for (int s = 0; s < BAG_COUNT; s++) {
            if (itemId(s) == 0) {
                return s;
            }
        }
        return -1;
    }

    public static int inventoryCount() {
        return INVENTORY_COUNT;
    }

    public static int bagCount() {
        return BAG_COUNT;
    }

    // --- Residents ----------------------------------------------------------------------------

    public List<Integer> residents() {
        List<Integer> out = new ArrayList<>();
        for (int id = 0; id < VILLAGER_COUNT; id++) {
            int at = VILLAGERS + id * VILLAGER_SIZE;
            for (int i = 1; i <= 16; i++) {
                if (data[at + i] != 0) {
                    out.add(id);
                    break;
                }
            }
        }
        return out;
    }

    public int friendship(int id) {
        return le16(data, VILLAGERS + id * VILLAGER_SIZE + 0x11);
    }

    public void setFriendship(int id, int value) {
        put16(data, VILLAGERS + id * VILLAGER_SIZE + 0x11, Math.max(0, Math.min(0xffff, value)));
    }

    public boolean namingDone(int id) {
        return data[VILLAGERS + id * VILLAGER_SIZE + 0xC4] != 0;
    }

    public void setNamingDone(int id, boolean done) {
        data[VILLAGERS + id * VILLAGER_SIZE + 0xC4] = (byte) (done ? 1 : 0);
    }

    /** The area (stage) id of a resident's home, 0 when none. */
    public int residentArea(int id) {
        return data[HOMES + id * HOME_SIZE + 3] & 0xff;
    }

    public boolean residentHasHome(int id) {
        return data[HOMES + id * HOME_SIZE] != 0;
    }

    // --- Flags --------------------------------------------------------------------------------

    public boolean flag(int id) {
        return ((data[FLAGS + id / 8] >> (id % 8)) & 1) != 0;
    }

    public boolean flagBefore(int id) {
        return ((before[FLAGS + id / 8] >> (id % 8)) & 1) != 0;
    }

    public void setFlag(int id, boolean on) {
        if (id <= 0 || id >= FLAG_COUNT) {
            return;
        }
        int at = FLAGS + id / 8;
        data[at] = (byte) (on ? (data[at] | (1 << (id % 8))) : (data[at] & ~(1 << (id % 8))));
    }

    public static int flagCount() {
        return FLAG_COUNT;
    }

    // --- Bytes --------------------------------------------------------------------------------

    static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    static int le16(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8;
    }

    static void put32(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
        b[o + 2] = (byte) (v >> 16);
        b[o + 3] = (byte) (v >> 24);
    }

    static void put16(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
    }

    /** "yyyyMMdd-HHmmss" for backup names. */
    public static String stamp() {
        Calendar c = Calendar.getInstance();
        return String.format(java.util.Locale.ROOT, "%04d%02d%02d-%02d%02d%02d", c.get(Calendar.YEAR),
                c.get(Calendar.MONTH) + 1, c.get(Calendar.DAY_OF_MONTH), c.get(Calendar.HOUR_OF_DAY),
                c.get(Calendar.MINUTE), c.get(Calendar.SECOND));
    }
}
