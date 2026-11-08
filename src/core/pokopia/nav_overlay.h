// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Core {
class System;
}

namespace Core::Pokopia {

/// A navigation overlay drawn over the game's picture (common/host_overlay.h) while in play: a
/// north-up radar with the player and a ping, and an arrow at the top of the screen that points
/// to the ping with its distance. G drops a ping where the player stands (or lifts it there).
/// Needs the mouse camera hook, which gives the camera's position.
void InstallNavOverlay(Core::System& system);

} // namespace Core::Pokopia
