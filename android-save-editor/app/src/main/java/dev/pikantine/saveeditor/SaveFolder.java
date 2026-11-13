// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.pikantine.saveeditor;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Finding and writing G00/CMNDAT.BIN in a folder the player picked (Storage Access Framework).
 * The emulators' own file providers (Eden, Eglantine) show their save folders in the picker.
 */
final class SaveFolder {
    /** A CMNDAT.BIN found under the picked folder, with the path that leads to it. */
    static final class Found {
        final Uri file;
        final String path;

        Found(Uri file, String path) {
            this.file = file;
            this.path = path;
        }
    }

    private SaveFolder() {}

    private static final class Child {
        final String id;
        final String name;
        final boolean dir;

        Child(String id, String name, boolean dir) {
            this.id = id;
            this.name = name;
            this.dir = dir;
        }
    }

    private static List<Child> children(ContentResolver cr, Uri tree, String parentId) {
        List<Child> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                         DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = cr.query(uri, cols, null, null, null)) {
            if (c == null) {
                return out;
            }
            while (c.moveToNext()) {
                String mime = c.getString(2);
                out.add(new Child(c.getString(0), c.getString(1),
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)));
            }
        } catch (RuntimeException e) {
            // an unreadable folder is skipped
        }
        return out;
    }

    /** Every G00/CMNDAT.BIN up to six folders below the picked one (or the picked G00 itself). */
    static List<Found> find(ContentResolver cr, Uri tree) {
        List<Found> out = new ArrayList<>();
        String root = DocumentsContract.getTreeDocumentId(tree);
        walk(cr, tree, root, "", 0, out);
        return out;
    }

    private static void walk(ContentResolver cr, Uri tree, String id, String path, int depth,
                             List<Found> out) {
        List<Child> kids = children(cr, tree, id);
        boolean isG00 = path.endsWith("G00") || (path.isEmpty() && treeNameIs(tree, "G00"));
        for (Child k : kids) {
            if (!k.dir && k.name.equalsIgnoreCase("CMNDAT.BIN") && isG00) {
                out.add(new Found(DocumentsContract.buildDocumentUriUsingTree(tree, k.id),
                        path.isEmpty() ? "CMNDAT.BIN" : path + "/CMNDAT.BIN"));
            }
        }
        if (depth >= 6) {
            return;
        }
        for (Child k : kids) {
            if (k.dir && !k.name.equalsIgnoreCase("B00")) {
                walk(cr, tree, k.id, path.isEmpty() ? k.name : path + "/" + k.name, depth + 1, out);
            }
        }
    }

    private static boolean treeNameIs(Uri tree, String name) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        return id != null && (id.endsWith("/" + name) || id.endsWith(":" + name) || id.equals(name));
    }

    static byte[] read(ContentResolver cr, Uri file) throws IOException {
        try (InputStream in = cr.openInputStream(file)) {
            if (in == null) {
                throw new IOException("Could not open the save.");
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream(1 << 20);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    static void write(ContentResolver cr, Uri file, byte[] bytes) throws IOException {
        OutputStream out;
        try {
            out = cr.openOutputStream(file, "wt");
        } catch (IllegalArgumentException | SecurityException e) {
            out = cr.openOutputStream(file, "w");
        }
        if (out == null) {
            throw new IOException("Could not open the save for writing.");
        }
        try (OutputStream o = out) {
            o.write(bytes);
            o.flush();
        }
    }
}
