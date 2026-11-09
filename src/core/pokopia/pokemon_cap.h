// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Pokopia {
class HookInstaller;
}

namespace Core::Pokopia {

/// Raises how many Pokémon can live in the world at once (the game's own is 25).
///
/// The limit is a game parameter, not code: main+0x1c9abc returns it ([[main+0x36ca678]+0x20]
/// +0x4bc), and the spawner (main+0x28c6c0) sends a Pokémon away to make room once that many
/// live. A second parameter next to it (+0x4c0) is the hard limit on all Pokémon objects. Both
/// are rewritten here while the game runs; the object pool behind them has 64 slots, so the
/// limit stays well under that.
///
/// The game brings Pokémon in up to the limit while its loading screen is up, all at once, so a
/// big raise made loading take minutes: with `hooks` (and the camera hook) the game keeps its own
/// limit while loading, and in play the limit climbs a couple at a time, each step bringing that
/// many in. EDEN_POKOPIA_POKEMON_STAGED=0 raises it at once instead.
///
/// EDEN_POKOPIA_POKEMON_CAP sets the limit (default 30, at most 50); 0 leaves the game's own.
void InstallPokemonCap(Core::System& system, HookInstaller* hooks, u64 main_base);

} // namespace Core::Pokopia
