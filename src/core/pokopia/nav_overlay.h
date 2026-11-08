// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Pokopia {

class HookInstaller;

/// A navigation overlay drawn over the game's picture (common/host_overlay.h) while in play: a
/// north-up radar with the player and a ping, and an arrow at the top of the screen that points
/// to the ping with its distance. G drops a ping where the player stands (or lifts it there).
/// Needs the mouse camera hook, which gives the camera's position.
///
/// The radar shows the game's own minimap texture ("tex_minimap", rendered for the pause menu's
/// map) live: the overlay asks the game to redraw it whenever it changed, and hooks the drawing to
/// learn which part of the world it covers.
void InstallNavOverlay(Core::System& system, HookInstaller& hooks, u64 main_base);

} // namespace Core::Pokopia
