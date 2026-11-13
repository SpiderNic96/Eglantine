// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package dev.pikantine.saveeditor.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** A small JSON reader (objects, arrays, strings, numbers, booleans), so the core runs anywhere. */
final class Json {
    private final String s;
    private int i;

    private Json(String text) {
        s = text;
    }

    static Object parse(String text) {
        Json j = new Json(text);
        Object v = j.value();
        j.space();
        if (j.i != j.s.length()) {
            throw new IllegalArgumentException("JSON: text after the value at " + j.i);
        }
        return v;
    }

    private void space() {
        while (i < s.length() && Character.isWhitespace(s.charAt(i))) {
            i++;
        }
    }

    private Object value() {
        space();
        char c = s.charAt(i);
        switch (c) {
        case '{': {
            i++;
            Map<String, Object> m = new LinkedHashMap<>();
            space();
            if (s.charAt(i) == '}') {
                i++;
                return m;
            }
            while (true) {
                space();
                String k = string();
                space();
                expect(':');
                m.put(k, value());
                space();
                if (s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                expect('}');
                return m;
            }
        }
        case '[': {
            i++;
            List<Object> l = new ArrayList<>();
            space();
            if (s.charAt(i) == ']') {
                i++;
                return l;
            }
            while (true) {
                l.add(value());
                space();
                if (s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                expect(']');
                return l;
            }
        }
        case '"':
            return string();
        case 't':
            i += 4;
            return Boolean.TRUE;
        case 'f':
            i += 5;
            return Boolean.FALSE;
        case 'n':
            i += 4;
            return null;
        default: {
            int start = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) {
                i++;
            }
            String num = s.substring(start, i);
            if (num.contains(".") || num.contains("e") || num.contains("E")) {
                return Double.parseDouble(num);
            }
            return Long.parseLong(num);
        }
        }
    }

    private void expect(char c) {
        if (s.charAt(i) != c) {
            throw new IllegalArgumentException("JSON: expected " + c + " at " + i);
        }
        i++;
    }

    private String string() {
        expect('"');
        StringBuilder b = new StringBuilder();
        while (true) {
            char c = s.charAt(i++);
            if (c == '"') {
                return b.toString();
            }
            if (c != '\\') {
                b.append(c);
                continue;
            }
            char e = s.charAt(i++);
            switch (e) {
            case 'n': b.append('\n'); break;
            case 't': b.append('\t'); break;
            case 'r': b.append('\r'); break;
            case 'b': b.append('\b'); break;
            case 'f': b.append('\f'); break;
            case 'u':
                b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                i += 4;
                break;
            default: b.append(e); break;
            }
        }
    }
}
