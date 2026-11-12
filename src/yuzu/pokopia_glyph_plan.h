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
void SetPokopiaGlyphPlan(InputCommon::InputSubsystem* input, bool pokopia);

} // namespace Pikantine
