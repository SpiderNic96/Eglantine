// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.pikantine.saveeditor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.AutoCompleteTextView;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import dev.pikantine.saveeditor.core.GameData;
import dev.pikantine.saveeditor.core.PokopiaSave;

/**
 * Pikantine's Pokopia save editor for phones: pick the save folder, change things tab by tab,
 * then Save. The old save is copied to the app's own folder first and can be put back.
 */
public final class MainActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final int PICK_FILE = 2;
    private static final String[] TABS = {"Pokédex", "Habitats", "Recipes", "Moves", "Reactions",
                                          "Bag", "Residents", "Flags"};
    private static final int GREEN = Color.rgb(0x4c, 0xaf, 0x50);
    private static final int FIRST_SPARE_FLAG = 160;

    private GameData game;
    private PokopiaSave save;
    private Uri saveUri;
    private String savePath = "";
    private byte[] saveFile;

    private int tab = 0;
    private int areaMask = 0;
    private String query = "";
    private boolean linkHabitats = true;
    private boolean fillSightings = true;
    private boolean showSpareFlags = false;

    private TextView status;
    private final Button[] tabButtons = new Button[TABS.length];
    private EditText search;
    private Spinner areaSpinner;
    private LinearLayout optionsRow;
    private Button tickAll;
    private Button saveButton;
    private ListView list;
    private final RowAdapter adapter = new RowAdapter();
    private final List<Row> rows = new ArrayList<>();
    private final List<Row> shown = new ArrayList<>();

    // --- Rows -----------------------------------------------------------------------------------

    private abstract static class Row {
        abstract String title();

        String sub() {
            return "";
        }

        abstract String badge();

        boolean changed() {
            return false;
        }

        boolean enabled() {
            return true;
        }

        int areas() {
            return -1;
        }

        abstract void tap();

        void tick() {}
    }

    private final class RowAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return shown.size();
        }

        @Override
        public Object getItem(int i) {
            return shown.get(i);
        }

        @Override
        public long getItemId(int i) {
            return i;
        }

        @Override
        public View getView(int i, View view, ViewGroup parent) {
            LinearLayout line;
            TextView badge, title, sub;
            if (view == null) {
                line = new LinearLayout(MainActivity.this);
                line.setOrientation(LinearLayout.HORIZONTAL);
                line.setGravity(Gravity.CENTER_VERTICAL);
                line.setPadding(dp(12), dp(8), dp(12), dp(8));
                badge = new TextView(MainActivity.this);
                badge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
                badge.setMinWidth(dp(40));
                badge.setGravity(Gravity.CENTER);
                LinearLayout texts = new LinearLayout(MainActivity.this);
                texts.setOrientation(LinearLayout.VERTICAL);
                texts.setPadding(dp(10), 0, 0, 0);
                title = new TextView(MainActivity.this);
                title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
                sub = new TextView(MainActivity.this);
                sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                sub.setAlpha(0.7f);
                texts.addView(title);
                texts.addView(sub);
                line.addView(badge);
                line.addView(texts, new LinearLayout.LayoutParams(0,
                        ViewGroup.LayoutParams.WRAP_CONTENT, 1));
                line.setTag(new TextView[] {badge, title, sub});
            } else {
                line = (LinearLayout) view;
            }
            TextView[] v = (TextView[]) line.getTag();
            Row r = shown.get(i);
            v[0].setText(r.badge());
            v[1].setText(r.title());
            String s = r.sub();
            v[2].setText(s);
            v[2].setVisibility(s.isEmpty() ? View.GONE : View.VISIBLE);
            int colour = r.changed() ? GREEN : plainText;
            v[0].setTextColor(colour);
            v[1].setTextColor(colour);
            line.setAlpha(r.enabled() ? 1f : 0.55f);
            return line;
        }
    }

    // Theme text colour, for rows that are not changed.
    private int plainText = Color.WHITE;

    // --- Activity -------------------------------------------------------------------------------

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            game = GameData.parse(readAsset("pokopia.json"));
        } catch (IOException | RuntimeException e) {
            Toast.makeText(this, "Could not read the game data: " + e.getMessage(),
                    Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        plainText = new TextView(this).getCurrentTextColor();
        setContentView(buildUi());
        refreshTabs();
        SharedPreferences prefs = getSharedPreferences("editor", MODE_PRIVATE);
        String last = prefs.getString("tree", null);
        if (last != null) {
            openTree(Uri.parse(last), false);
        } else {
            status.setText("Close the game first. Then tap \"Open save folder\" and pick the "
                    + "Pokopia save folder (0400A3A01EE4C000, or the G00 inside it), or tap "
                    + "\"Open save file\" and pick CMNDAT.BIN itself.");
        }
    }

    private String readAsset(String name) throws IOException {
        try (InputStream in = getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(8), dp(8), dp(8), dp(8));
        // Android 15 draws apps under the status and navigation bars: keep everything clear of them.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int l, t, r, b;
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                l = i.left; t = i.top; r = i.right; b = i.bottom;
            } else {
                l = insets.getSystemWindowInsetLeft(); t = insets.getSystemWindowInsetTop();
                r = insets.getSystemWindowInsetRight(); b = insets.getSystemWindowInsetBottom();
            }
            v.setPadding(dp(8) + l, dp(8) + t, dp(8) + r, dp(8) + b);
            return insets;
        });

        TextView heading = new TextView(this);
        heading.setText("Pikantine Save Editor");
        heading.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        heading.setPadding(dp(4), 0, dp(4), dp(4));
        root.addView(heading);

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        Button open = new Button(this);
        open.setText("Open save folder");
        open.setAllCaps(false);
        open.setOnClickListener(v -> pickFolder());
        top.addView(open, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button file = new Button(this);
        file.setText("Open save file");
        file.setAllCaps(false);
        file.setOnClickListener(v -> pickFile());
        top.addView(file, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button restore = new Button(this);
        restore.setText("Backups");
        restore.setAllCaps(false);
        restore.setOnClickListener(v -> showBackups());
        top.addView(restore);
        root.addView(top);

        status = new TextView(this);
        status.setPadding(dp(4), dp(4), dp(4), dp(4));
        root.addView(status);

        HorizontalScrollView scroll = new HorizontalScrollView(this);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < TABS.length; i++) {
            final int index = i;
            Button b = new Button(this);
            b.setText(TABS[i]);
            b.setAllCaps(false);
            b.setOnClickListener(v -> {
                tab = index;
                refreshTabs();
            });
            tabButtons[i] = b;
            tabs.addView(b);
        }
        scroll.addView(tabs);
        root.addView(scroll);

        LinearLayout filters = new LinearLayout(this);
        filters.setOrientation(LinearLayout.HORIZONTAL);
        search = new EditText(this);
        search.setHint("Search");
        search.setSingleLine(true);
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable e) {
                query = e.toString().trim().toLowerCase(Locale.ROOT);
                applyFilter();
            }
        });
        filters.addView(search, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        areaSpinner = new Spinner(this);
        List<String> areaNames = new ArrayList<>();
        areaNames.add("Any area");
        areaNames.addAll(game.areas);
        ArrayAdapter<String> areaAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, areaNames);
        areaAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        areaSpinner.setAdapter(areaAdapter);
        areaSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                areaMask = pos == 0 ? 0 : 1 << (pos - 1);
                applyFilter();
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        filters.addView(areaSpinner);
        root.addView(filters);

        optionsRow = new LinearLayout(this);
        optionsRow.setOrientation(LinearLayout.VERTICAL);
        root.addView(optionsRow);

        tickAll = new Button(this);
        tickAll.setAllCaps(false);
        tickAll.setOnClickListener(v -> {
            for (Row r : new ArrayList<>(shown)) {
                if (r.enabled()) {
                    r.tick();
                }
            }
            changed();
        });
        root.addView(tickAll);

        list = new ListView(this);
        list.setAdapter(adapter);
        list.setOnItemClickListener((p, v, pos, id) -> {
            Row r = shown.get(pos);
            if (r.enabled()) {
                r.tap();
                changed();
            }
        });
        root.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        Button undo = new Button(this);
        undo.setText("Undo all");
        undo.setOnClickListener(v -> {
            if (save != null) {
                save.revert();
                rebuildRows();
            }
        });
        bottom.addView(undo, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        saveButton = new Button(this);
        saveButton.setText("Save");
        saveButton.setTypeface(Typeface.DEFAULT_BOLD);
        saveButton.setOnClickListener(v -> confirmSave());
        bottom.addView(saveButton, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        root.addView(bottom);
        return root;
    }

    // --- Picking the save -----------------------------------------------------------------------

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, PICK_FOLDER);
    }

    private void pickFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, PICK_FILE);
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri,
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                return c.getString(0);
            }
        } catch (RuntimeException e) {
            // fall through
        }
        return uri.getLastPathSegment();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent intent) {
        super.onActivityResult(request, result, intent);
        if (request == PICK_FILE && result == RESULT_OK && intent != null && intent.getData() != null) {
            Uri file = intent.getData();
            try {
                getContentResolver().takePersistableUriPermission(file,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException e) {
                // works for this session only
            }
            loadSave(new SaveFolder.Found(file, displayName(file)));
            return;
        }
        if (request != PICK_FOLDER || result != RESULT_OK || intent == null || intent.getData() == null) {
            return;
        }
        Uri tree = intent.getData();
        try {
            getContentResolver().takePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (SecurityException e) {
            // works for this session only
        }
        getSharedPreferences("editor", MODE_PRIVATE).edit().putString("tree", tree.toString()).apply();
        openTree(tree, true);
    }

    private void openTree(Uri tree, boolean fromPicker) {
        status.setText("Looking for the save…");
        new Thread(() -> {
            List<SaveFolder.Found> found = SaveFolder.find(getContentResolver(), tree);
            runOnUiThread(() -> {
                if (found.isEmpty()) {
                    status.setText(fromPicker
                            ? "No G00/CMNDAT.BIN in that folder. Pick the Pokopia save folder "
                              + "(0400A3A01EE4C000) or the G00 folder inside it."
                            : "Tap \"Open save folder\" to pick your Pokopia save.");
                    return;
                }
                if (found.size() == 1) {
                    loadSave(found.get(0));
                    return;
                }
                String[] names = new String[found.size()];
                for (int i = 0; i < names.length; i++) {
                    names[i] = found.get(i).path;
                }
                new AlertDialog.Builder(this).setTitle("Which save?")
                        .setItems(names, (d, w) -> loadSave(found.get(w))).show();
            });
        }).start();
    }

    private void loadSave(SaveFolder.Found f) {
        status.setText("Reading " + f.path + "…");
        new Thread(() -> {
            try {
                byte[] bytes = SaveFolder.read(getContentResolver(), f.file);
                PokopiaSave s = PokopiaSave.read(bytes);
                runOnUiThread(() -> {
                    save = s;
                    saveFile = bytes;
                    saveUri = f.file;
                    savePath = f.path;
                    rebuildRows();
                });
            } catch (IOException | RuntimeException e) {
                runOnUiThread(() -> status.setText("Could not read the save: " + e.getMessage()));
            }
        }).start();
    }

    // --- Tabs and rows --------------------------------------------------------------------------

    private void refreshTabs() {
        for (int i = 0; i < tabButtons.length; i++) {
            tabButtons[i].setTypeface(i == tab ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            tabButtons[i].setAlpha(i == tab ? 1f : 0.6f);
        }
        areaSpinner.setVisibility(tab <= 1 ? View.VISIBLE : View.GONE);
        optionsRow.removeAllViews();
        if (tab == 0) {
            CheckBox link = new CheckBox(this);
            link.setText("Registering also marks its habitats discovered");
            link.setChecked(linkHabitats);
            link.setOnCheckedChangeListener((b, on) -> linkHabitats = on);
            CheckBox fill = new CheckBox(this);
            fill.setText("Mark registered Pokémon as seen in their habitats (fills the \"?\")");
            fill.setChecked(fillSightings);
            fill.setOnCheckedChangeListener((b, on) -> fillSightings = on);
            optionsRow.addView(link);
            optionsRow.addView(fill);
        } else if (tab == 7) {
            CheckBox spare = new CheckBox(this);
            spare.setText("Show the spare flags (160–249)");
            spare.setChecked(showSpareFlags);
            spare.setOnCheckedChangeListener((b, on) -> {
                showSpareFlags = on;
                rebuildRows();
            });
            TextView warn = new TextView(this);
            warn.setText("The game knows these only by number. Story flags can break your "
                    + "progress: change one at a time and keep the backup.");
            warn.setAlpha(0.7f);
            optionsRow.addView(warn);
            optionsRow.addView(spare);
        } else if (tab == 5) {
            Button add = new Button(this);
            add.setAllCaps(false);
            add.setText("Put an item in the first empty slot");
            add.setOnClickListener(v -> {
                if (save == null) {
                    return;
                }
                int slot = save.firstEmptyBagSlot();
                if (slot < 0) {
                    Toast.makeText(this, "The bag is full.", Toast.LENGTH_SHORT).show();
                    return;
                }
                editItem(slot);
            });
            optionsRow.addView(add);
        }
        String[] tick = {"Register all shown", "Build all shown", "Learn all shown",
                         "Learn every move and upgrade", "Learn all shown", "", "", ""};
        tickAll.setText(tick[tab]);
        tickAll.setVisibility(tick[tab].isEmpty() ? View.GONE : View.VISIBLE);
        rebuildRows();
    }

    private void rebuildRows() {
        rows.clear();
        if (save != null) {
            switch (tab) {
            case 0: dexRows(); break;
            case 1: habitatRows(); break;
            case 2: recipeRows(); break;
            case 3: skillRows(); break;
            case 4: reactionRows(); break;
            case 5: bagRows(); break;
            case 6: residentRows(); break;
            case 7: flagRows(); break;
            default: break;
            }
        }
        applyFilter();
        changed();
    }

    private void applyFilter() {
        shown.clear();
        for (Row r : rows) {
            if (areaMask != 0 && tab <= 1 && (r.areas() & areaMask) == 0) {
                continue;
            }
            if (!query.isEmpty() && !(r.title() + " " + r.sub()).toLowerCase(Locale.ROOT).contains(query)) {
                continue;
            }
            shown.add(r);
        }
        adapter.notifyDataSetChanged();
    }

    private void changed() {
        adapter.notifyDataSetChanged();
        if (save == null) {
            saveButton.setEnabled(false);
            return;
        }
        int n = save.changedBytes();
        saveButton.setEnabled(n > 0);
        saveButton.setText(n > 0 ? "Save (" + n + " bytes changed)" : "Save");
        int reg = 0, built = 0;
        for (GameData.DexSlot p : game.dex) {
            reg += save.dexState(p.slot) == 2 ? 1 : 0;
        }
        for (GameData.Named h : game.habitats) {
            built += save.habitatState(h.id) == 2 ? 1 : 0;
        }
        status.setText(String.format(Locale.ROOT, "%s — %s\nPokédex %d/%d registered, habitats %d/%d built",
                save.playerName(), savePath, reg, game.dex.size(), built, game.habitats.size()));
    }

    private static final String[] TRI = {"○", "◐", "●"};
    private static final String[] DEX_STATES = {"Not yet", "Seen", "Registered"};
    private static final String[] HABITAT_STATES = {"Not yet", "Discovered", "Built"};

    private String spawnText(GameData.Spawn s, String what) {
        return what + " (" + game.areaText(s.areas) + (s.rare ? ", rare)" : ")");
    }

    private void dexRows() {
        for (GameData.DexSlot p : game.dex) {
            StringBuilder where = new StringBuilder();
            int mask = 0;
            for (GameData.Spawn s : p.spawns) {
                if (where.length() > 0) {
                    where.append("; ");
                }
                where.append(spawnText(s, game.habitatName(s.habitat)));
                mask |= s.areas;
            }
            final int areas = mask;
            final String sub = "#" + p.slot + " · "
                    + (where.length() == 0 ? "special: story, event or not listed yet" : where);
            rows.add(new Row() {
                String title() {
                    return p.name + " — " + DEX_STATES[save.dexState(p.slot)];
                }
                String sub() {
                    return sub;
                }
                String badge() {
                    return TRI[save.dexState(p.slot)];
                }
                boolean changed() {
                    return save.dexState(p.slot) != save.dexStateBefore(p.slot);
                }
                int areas() {
                    return areas;
                }
                void tap() {
                    save.setDex(p, (save.dexState(p.slot) + 1) % 3, linkHabitats, fillSightings);
                }
                void tick() {
                    save.setDex(p, 2, linkHabitats, fillSightings);
                }
            });
        }
    }

    private void habitatRows() {
        Map<Integer, StringBuilder> draws = new HashMap<>();
        Map<Integer, Integer> masks = new HashMap<>();
        for (GameData.DexSlot p : game.dex) {
            for (GameData.Spawn s : p.spawns) {
                StringBuilder b = draws.get(s.habitat);
                if (b == null) {
                    b = new StringBuilder();
                    draws.put(s.habitat, b);
                }
                if (b.length() > 0) {
                    b.append("; ");
                }
                b.append(spawnText(s, p.name));
                Integer m = masks.get(s.habitat);
                masks.put(s.habitat, (m == null ? 0 : m) | s.areas);
            }
        }
        for (GameData.Named h : game.habitats) {
            StringBuilder d = draws.get(h.id);
            Integer m = masks.get(h.id);
            final String sub = "#" + h.id + (d == null ? "" : " · " + d);
            final int areas = m == null ? 0 : m;
            rows.add(new Row() {
                String title() {
                    return h.name + " — " + HABITAT_STATES[save.habitatState(h.id)];
                }
                String sub() {
                    return sub;
                }
                String badge() {
                    return TRI[save.habitatState(h.id)];
                }
                boolean changed() {
                    return save.habitatState(h.id) != save.habitatStateBefore(h.id);
                }
                int areas() {
                    return areas;
                }
                void tap() {
                    save.setHabitat(h.id, (save.habitatState(h.id) + 1) % 3);
                }
                void tick() {
                    save.setHabitat(h.id, 2);
                }
            });
        }
    }

    private void recipeRows() {
        for (GameData.Named r : game.recipes) {
            rows.add(new Row() {
                String title() {
                    return r.name;
                }
                String sub() {
                    return save.recipeLearnedBefore(r.id) ? "Already learned" : "#" + r.id;
                }
                String badge() {
                    return save.recipeLearned(r.id) ? "☑" : "☐";
                }
                boolean changed() {
                    return save.recipeLearned(r.id) != save.recipeLearnedBefore(r.id);
                }
                boolean enabled() {
                    return !save.recipeLearnedBefore(r.id);
                }
                void tap() {
                    if (save.recipeLearned(r.id)) {
                        save.unlearnRecipe(r.id);
                    } else {
                        save.learnRecipe(r.id);
                    }
                }
                void tick() {
                    save.learnRecipe(r.id);
                }
            });
        }
    }

    private void skillRows() {
        final PokopiaSave start = PokopiaSave.read(saveFile);
        for (int id : PokopiaSave.PUBLIC_SKILLS) {
            final String name = GameData.nameOr(game.skills, id, "Move");
            final boolean had = start.skillLearned(id);
            rows.add(new Row() {
                String title() {
                    return name;
                }
                String sub() {
                    return had ? "Learned · used " + save.skillUses(id) + " times" : "Not learned";
                }
                String badge() {
                    return save.skillLearned(id) ? "☑" : "☐";
                }
                boolean changed() {
                    return save.skillLearned(id) != had;
                }
                boolean enabled() {
                    return !save.skillLearned(id);
                }
                void tap() {
                    save.learnSkill(id);
                }
                void tick() {
                    save.learnSkill(id);
                }
            });
        }
        for (PokopiaSave.Upgrade u : PokopiaSave.UPGRADES) {
            final boolean had = start.upgradeUnlocked(u);
            rows.add(new Row() {
                String title() {
                    return "  " + u.name;
                }
                String sub() {
                    return "Upgrade · needs " + GameData.nameOr(game.skills, u.needs, "move")
                            + (u.needsDive ? " and Dive" : "");
                }
                String badge() {
                    return save.upgradeUnlocked(u) ? "☑" : "☐";
                }
                boolean changed() {
                    return save.upgradeUnlocked(u) != had;
                }
                boolean enabled() {
                    return !save.upgradeUnlocked(u);
                }
                void tap() {
                    save.unlockUpgrade(u);
                }
                void tick() {
                    save.unlockUpgrade(u);
                }
            });
        }
    }

    private void reactionRows() {
        final PokopiaSave start = PokopiaSave.read(saveFile);
        for (int id = 1; id <= 100 && id < game.reactions.size(); id++) {
            if (game.reactions.get(id).isEmpty()) {
                continue;
            }
            final int rid = id;
            final boolean had = start.reactionLearned(id);
            rows.add(new Row() {
                String title() {
                    return game.reactions.get(rid);
                }
                String badge() {
                    return save.reactionLearned(rid) ? "☑" : "☐";
                }
                boolean changed() {
                    return save.reactionLearned(rid) != had;
                }
                boolean enabled() {
                    return !save.reactionLearned(rid);
                }
                void tap() {
                    save.learnReaction(rid);
                }
                void tick() {
                    save.learnReaction(rid);
                }
            });
        }
    }

    private String itemName(int id) {
        return GameData.nameOr(game.items, id, "Item");
    }

    private void bagRows() {
        final PokopiaSave start = PokopiaSave.read(saveFile);
        for (int slot = 0; slot < PokopiaSave.inventoryCount(); slot++) {
            final int s = slot;
            final String label = slot < PokopiaSave.bagCount() ? "Bag " + (slot + 1)
                                                                : "Bar " + (slot - PokopiaSave.bagCount() + 1);
            rows.add(new Row() {
                String title() {
                    int id = save.itemId(s);
                    return id == 0 ? label + ": empty" : label + ": " + itemName(id) + " ×" + save.itemCount(s);
                }
                String badge() {
                    return save.itemId(s) == 0 ? "·" : "▣";
                }
                boolean changed() {
                    return save.itemId(s) != start.itemId(s) || save.itemCount(s) != start.itemCount(s);
                }
                void tap() {
                    editItem(s);
                }
            });
        }
    }

    private void editItem(int slot) {
        List<String> names = new ArrayList<>();
        for (int id = 1; id < game.items.size(); id++) {
            if (!game.items.get(id).isEmpty()) {
                names.add(game.items.get(id) + " (" + id + ")");
            }
        }
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(8), dp(16), 0);
        AutoCompleteTextView item = new AutoCompleteTextView(this);
        item.setHint("Item (type to search)");
        item.setThreshold(1);
        item.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, names));
        int current = save.itemId(slot);
        if (current != 0) {
            item.setText(itemName(current) + " (" + current + ")");
        }
        EditText count = new EditText(this);
        count.setInputType(InputType.TYPE_CLASS_NUMBER);
        count.setHint("Count (1–255; real stacks go to 99)");
        count.setText(String.valueOf(current == 0 ? 1 : save.itemCount(slot)));
        box.addView(item);
        box.addView(count);
        new AlertDialog.Builder(this)
                .setTitle(slot < PokopiaSave.bagCount() ? "Bag " + (slot + 1) : "Bar " + (slot - PokopiaSave.bagCount() + 1))
                .setView(box)
                .setPositiveButton("Set", (d, w) -> {
                    int id = idInParens(item.getText().toString());
                    int n = parse(count.getText().toString(), 1);
                    if (id <= 0) {
                        Toast.makeText(this, "Pick an item from the list.", Toast.LENGTH_SHORT).show();
                        return;
                    }
                    save.setItem(slot, id, Math.max(1, Math.min(255, n)));
                    changed();
                })
                .setNeutralButton("Empty slot", (d, w) -> {
                    save.setItem(slot, 0, 0);
                    changed();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static int idInParens(String text) {
        int open = text.lastIndexOf('('), close = text.lastIndexOf(')');
        if (open < 0 || close < open) {
            return -1;
        }
        return parse(text.substring(open + 1, close), -1);
    }

    private static int parse(String s, int fallback) {
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private void residentRows() {
        final PokopiaSave start = PokopiaSave.read(saveFile);
        for (int id : save.residents()) {
            final String name = GameData.nameOr(game.residents, id, "Resident");
            rows.add(new Row() {
                String title() {
                    return name + " — friendship " + save.friendship(id);
                }
                String sub() {
                    int area = save.residentArea(id);
                    return GameData.nameOr(game.stages, area, "Area")
                            + (save.residentHasHome(id) ? " · has a home" : " · no home")
                            + (save.namingDone(id) ? " · naming chat done" : "");
                }
                String badge() {
                    return "♥";
                }
                boolean changed() {
                    return save.friendship(id) != start.friendship(id)
                            || save.namingDone(id) != start.namingDone(id);
                }
                void tap() {
                    editResident(id, name);
                }
            });
        }
    }

    private void editResident(int id, String name) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(8), dp(16), 0);
        EditText friendship = new EditText(this);
        friendship.setInputType(InputType.TYPE_CLASS_NUMBER);
        friendship.setHint("Friendship (0–" + PokopiaSave.FRIENDSHIP_MAX + ")");
        friendship.setText(String.valueOf(save.friendship(id)));
        CheckBox naming = new CheckBox(this);
        naming.setText("Naming chat done");
        naming.setChecked(save.namingDone(id));
        box.addView(friendship);
        box.addView(naming);
        new AlertDialog.Builder(this).setTitle(name).setView(box)
                .setPositiveButton("Set", (d, w) -> {
                    int max = Math.max(PokopiaSave.FRIENDSHIP_MAX, save.friendship(id));
                    save.setFriendship(id, Math.max(0, Math.min(max, parse(friendship.getText().toString(), save.friendship(id)))));
                    save.setNamingDone(id, naming.isChecked());
                    changed();
                })
                .setNegativeButton("Cancel", null).show();
    }

    private void flagRows() {
        for (int id = 1; id < PokopiaSave.flagCount(); id++) {
            final int f = id;
            if (f >= FIRST_SPARE_FLAG && !showSpareFlags && !save.flag(f)) {
                continue;
            }
            final String known = game.flags.get(f);
            rows.add(new Row() {
                String title() {
                    return "Flag " + f + (known != null ? " — " + known : "");
                }
                String sub() {
                    return f >= FIRST_SPARE_FLAG ? "spare" : "";
                }
                String badge() {
                    return save.flag(f) ? "☑" : "☐";
                }
                boolean changed() {
                    return save.flag(f) != save.flagBefore(f);
                }
                void tap() {
                    save.setFlag(f, !save.flag(f));
                }
            });
        }
    }

    // --- Saving and backups ---------------------------------------------------------------------

    private File backupRoot() {
        File ext = getExternalFilesDir(null);
        File dir = new File(ext != null ? ext : getFilesDir(), "backups");
        if (!dir.exists() && !dir.mkdirs()) {
            dir = new File(getFilesDir(), "backups");
            dir.mkdirs();
        }
        return dir;
    }

    private void confirmSave() {
        if (save == null || !save.isChanged()) {
            return;
        }
        new AlertDialog.Builder(this).setTitle("Write to the save?")
                .setMessage(save.changedBytes() + " bytes of " + savePath + " change. Make sure "
                        + "the game is closed. The old save is backed up first (Backups puts it "
                        + "back).")
                .setPositiveButton("Save", (d, w) -> doSave())
                .setNegativeButton("Cancel", null).show();
    }

    private void doSave() {
        final PokopiaSave s = save;
        final byte[] old = saveFile;
        final Uri uri = saveUri;
        final String path = savePath;
        saveButton.setEnabled(false);
        status.setText("Saving…");
        new Thread(() -> {
            String message;
            try {
                File dir = new File(backupRoot(), PokopiaSave.stamp());
                if (!dir.mkdirs() && !dir.isDirectory()) {
                    throw new IOException("Could not make the backup folder.");
                }
                try (FileOutputStream out = new FileOutputStream(new File(dir, "CMNDAT.BIN"))) {
                    out.write(old);
                }
                try (FileOutputStream out = new FileOutputStream(new File(dir, "from.txt"))) {
                    out.write((path + "\n" + uri + "\n").getBytes(StandardCharsets.UTF_8));
                }
                byte[] packed = s.pack();
                SaveFolder.write(getContentResolver(), uri, packed);
                byte[] back = SaveFolder.read(getContentResolver(), uri);
                if (!Arrays.equals(Arrays.copyOf(back, packed.length), packed)) {
                    throw new IOException("The save did not read back the same. Restore the "
                            + "backup from " + dir.getName() + ".");
                }
                PokopiaSave fresh = PokopiaSave.read(back);
                runOnUiThread(() -> {
                    save = fresh;
                    saveFile = back;
                    rebuildRows();
                });
                message = "Saved. The old save is in Backups → " + dir.getName() + ".";
            } catch (IOException | RuntimeException e) {
                message = "Not saved: " + e.getMessage();
            }
            final String m = message;
            runOnUiThread(() -> {
                Toast.makeText(this, m, Toast.LENGTH_LONG).show();
                changed();
            });
        }).start();
    }

    private void showBackups() {
        File[] dirs = backupRoot().listFiles(File::isDirectory);
        if (dirs == null || dirs.length == 0) {
            Toast.makeText(this, "No backups yet: one is made each time you save.", Toast.LENGTH_LONG).show();
            return;
        }
        Arrays.sort(dirs, (a, b) -> b.getName().compareTo(a.getName()));
        String[] names = new String[dirs.length];
        for (int i = 0; i < dirs.length; i++) {
            names[i] = dirs[i].getName();
        }
        new AlertDialog.Builder(this).setTitle("Put a backup back into the open save?")
                .setItems(names, (d, w) -> restore(dirs[w]))
                .setNegativeButton("Cancel", null).show();
    }

    private void restore(File dir) {
        if (saveUri == null) {
            Toast.makeText(this, "Open the save folder first.", Toast.LENGTH_LONG).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle("Restore " + dir.getName() + "?")
                .setMessage("This writes that backup over " + savePath + ". Make sure the game is closed.")
                .setPositiveButton("Restore", (d, w) -> new Thread(() -> {
                    String m;
                    try {
                        File f = new File(dir, "CMNDAT.BIN");
                        byte[] bytes = new byte[(int) f.length()];
                        try (FileInputStream in = new FileInputStream(f)) {
                            int off = 0;
                            while (off < bytes.length) {
                                int n = in.read(bytes, off, bytes.length - off);
                                if (n < 0) {
                                    break;
                                }
                                off += n;
                            }
                        }
                        PokopiaSave check = PokopiaSave.read(bytes);
                        SaveFolder.write(getContentResolver(), saveUri, bytes);
                        byte[] back = SaveFolder.read(getContentResolver(), saveUri);
                        PokopiaSave fresh = PokopiaSave.read(back);
                        runOnUiThread(() -> {
                            save = fresh;
                            saveFile = back;
                            rebuildRows();
                        });
                        m = "Restored " + dir.getName() + " (" + check.playerName() + ").";
                    } catch (IOException | RuntimeException e) {
                        m = "Not restored: " + e.getMessage();
                    }
                    final String msg = m;
                    runOnUiThread(() -> Toast.makeText(this, msg, Toast.LENGTH_LONG).show());
                }).start())
                .setNegativeButton("Cancel", null).show();
    }
}
