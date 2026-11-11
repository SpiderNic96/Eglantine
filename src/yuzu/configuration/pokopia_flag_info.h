// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// What is known about Pokopia 2.0.1's 250 "function flags" (m_functionFlag, 32 bytes at 0x220F04
// of the common save; flag n is bit n%8 of byte n/8; 0 is never used).
//
// The game knows these flags only by number. Its name table (container entry #1221, 250 rows)
// ships blank in the retail data: every row is empty but row 0 ("－－－"). Entry #949 gives
// each flag a u32 of settings, and its main check (main+0x113f10) reads them:
//   0x10000  always counts as on (only flag 19)
//   0x20000  also counts as on in one of the game's modes (mode 17)
//   0x40000  in multiplayer, read from the island host's flags instead of your own
// Flags 160..249 have no settings and are set in none of the saves checked: spare ids.
// Names below are PokoHex's (berichan) where it has one; everything else is a hint, not a name.

#pragma once

#include <array>

namespace Pikantine {

struct FlagInfo {
    unsigned char id;
    const char* name;
};

inline constexpr std::array<FlagInfo, 10> PokopiaKnownFlags{{
    {54, "Liquid absorption (Water Gun)"},
    {56, "Surf unlocked"},
    {57, "Glide unlocked"},
    {71, "Leafage food boost"},
    {72, "Cut food boost"},
    {73, "Water Gun food boost"},
    {74, "Rock Smash food boost"},
    {125, "Rock Smash extra food effect"},
    {148, "Dive unlocked"},
    {153, "Surf food boost"},
}};

/// The u32 settings of entry #949, by flag id.
inline constexpr unsigned FlagSettings(int id) {
    switch (id) {
    case 19:
        return 0x10000;
    case 120: case 122: case 130: case 141: case 142: case 143: case 146: case 159:
        return 0x20000;
    case 7: case 61: case 86: case 87: case 88: case 89: case 93: case 149: case 150: case 151:
        return 0x40000;
    case 155:
        return 0x600B7;
    case 3: case 10: case 14: case 15: case 16: case 18: case 22: case 23: case 24: case 25:
    case 26: case 27: case 29: case 30: case 35: case 36: case 37: case 38: case 39: case 40:
    case 43: case 48: case 60: case 62: case 67: case 68: case 69: case 70: case 71: case 72:
    case 73: case 74: case 77: case 78: case 79: case 80: case 81: case 84: case 90: case 91:
    case 92: case 95: case 96: case 97: case 98: case 99: case 100: case 101: case 106:
    case 109: case 115: case 116: case 117: case 118: case 121: case 123: case 124: case 125:
    case 126: case 127: case 128: case 131: case 132: case 133: case 135: case 136: case 137:
    case 138: case 139: case 140: case 144: case 145: case 147: case 152: case 153: case 158:
        return 0x60000;
    default:
        return 0;
    }
}

/// How many places in the game's code ask main+0x113f10 about this flag by its number.
inline constexpr int FlagChecks(int id) {
    switch (id) {
    case 11: case 15: case 16: case 22: case 37: case 38: case 40: case 74: case 92: case 96:
    case 97: case 102: case 103: case 104: case 123: case 124: case 125: case 139: case 142:
        return 1;
    case 61:
        return 2;
    case 14:
        return 3;
    case 18: case 149:
        return 5;
    case 107:
        return 7;
    case 150: case 159:
        return 8;
    case 63: case 64: case 65:
        return 35;
    case 66:
        return 46;
    case 148:
        return 54;
    default:
        return 0;
    }
}

/// Set in a further-on save (Surf, Skylands and Bubbly Basin reached) but not in an earlier one.
inline constexpr bool FlagIsLaterStory(int id) {
    switch (id) {
    case 29: case 48: case 54: case 56: case 65: case 77: case 78: case 79: case 87: case 92:
    case 99: case 104: case 117: case 120: case 123: case 126: case 129: case 132: case 148:
    case 149: case 150: case 151: case 152: case 153: case 155: case 156: case 157: case 159:
        return true;
    default:
        return false;
    }
}

} // namespace Pikantine
