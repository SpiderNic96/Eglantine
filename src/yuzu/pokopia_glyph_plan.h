// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace InputCommon {
class InputSubsystem;
}

namespace Pikantine {

/// Draws Pokémon Pokopia's button prompts for Player 1's current controls (keys, mouse buttons,
/// DualSense buttons) and hands them to the renderer (video_core/texture_cache/pokopia_glyphs.h).
/// Buttons bound to anything else keep the game's own Switch glyph. Call before a game starts;
/// with `pokopia` false, or Settings -> Pikantine -> Button icons set to the game's own, it
/// clears the tiles.
/// Also has the renderer write the "Keyboard and mouse glyphs" and "PlayStation glyphs" icon
/// mods into the game's mod folder, from this copy of the game, the first time its textures load
/// (which one loads follows Settings -> Pikantine, see texture_mods.cpp).
void SetPokopiaGlyphPlan(InputCommon::InputSubsystem* input, bool pokopia);

/// Puts the "Pokopia keyboard and mouse" input profile (shipped inside Eglantine) in the input
/// profile folder if it is not there. Called as Eglantine opens.
void InstallPokopiaProfiles();

} // namespace Pikantine
