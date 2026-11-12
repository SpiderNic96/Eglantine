// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <vector>

#include "common/common_types.h"

/// Pikantine: Pokémon Pokopia's button prompts drawn from the player's own controls.
///
/// The game draws every prompt from one 1024x1024 BC7 atlas (and the title screen's "PRESS (A)
/// BUTTON" from the title logo). When those textures are uploaded, each Switch button's glyph is
/// cleared and the tile the frontend drew for whatever that button is bound to is put in its
/// place; only the 4x4 blocks touched are encoded again. The frontend sets the tiles before a
/// game starts (yuzu/pokopia_glyph_plan.cpp); with no tiles the textures are left alone.
namespace VideoCommon::PokopiaGlyphs {

constexpr u64 AtlasHash = 0xabafc68a5cb224c9ULL; // the prompt atlas, 1024x1024 BC7
constexpr u64 TitleHash = 0xeef4c6cb94636c58ULL; // the title logo, 1024x1024 BC7
constexpr u32 TextureSize = 1024;

/// Where a glyph sits in the atlas, from its alpha: x0, y0, x1, y1 (exclusive).
struct Box {
    const char* name;
    int x0, y0, x1, y1;
};

/// The atlas's Switch button glyphs, by the button's name in Settings::NativeButton (and LStick /
/// RStick for the sticks themselves, LStickPress / RStickPress for their clicks).
inline constexpr std::array<Box, 18> AtlasBoxes{{
    {"A", 249, 345, 315, 411},
    {"B", 937, 417, 1003, 483},
    {"X", 713, 605, 779, 671},
    {"Y", 9, 609, 75, 675},
    {"L", 271, 520, 341, 580},
    {"R", 535, 524, 605, 584},
    {"ZL", 95, 612, 165, 672},
    {"ZR", 183, 612, 253, 672},
    {"Plus", 9, 521, 75, 587},
    {"Minus", 777, 517, 843, 583},
    {"DLeft", 361, 517, 427, 583},
    {"DRight", 625, 521, 691, 587},
    {"DUp", 449, 605, 515, 671},
    {"DDown", 425, 429, 491, 495},
    {"LStick", 449, 517, 515, 583},
    {"RStick", 865, 593, 931, 659},
    {"LStickPress", 97, 513, 163, 587},
    {"RStickPress", 185, 513, 251, 587},
}};

/// The A disc of the title logo's "PRESS (A) BUTTON".
inline constexpr Box TitleDisc{"Title", 262, 631, 309, 678};

/// RGBA pixels (straight alpha) to put over a texture: `clear` (x0, y0, x1, y1) is made fully
/// transparent first, then the tile is blended over it with its top left at (x, y).
struct Tile {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::array<int, 4> clear{};
    std::vector<u8> rgba;
};

/// Sets the tiles for the atlas and for the title (either may be empty: that texture is left
/// alone). Called by the frontend before a game starts; takes effect at the next upload.
void SetPlan(std::vector<Tile> atlas, std::vector<Tile> title);

/// True when there are tiles to apply.
bool HasPlan();

/// Applies the tiles to `host` (the texture as uploaded: BC7 blocks, `row_bytes` a row of blocks)
/// when `hash` is the atlas or the title. Returns true when it changed anything.
bool Apply(u64 hash, u32 width, u32 height, std::span<u8> host, std::size_t row_bytes);

} // namespace VideoCommon::PokopiaGlyphs
