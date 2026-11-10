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
/// are rewritten here while the game runs; the hard limit is kept within the verified 64-slot
/// object pool, with 14 entries left beyond the maximum 50 residents for transient visitors and
/// item-triggered spawns.
///
/// The game brings Pokémon in up to the limit while an area loads, all at once (it places Pokémon
/// only then), so a high limit makes entering an area slower. Each area starts from an empty pool
/// of 64. EDEN_POKOPIA_POKEMON_STAGED=1 tries filling a couple at a time in play instead
/// (experimental: in a test the game placed none of them).
///
/// The limit is Settings -> Pikantine's (pokopia_pokemon_cap, default 30, at most 50, 0 the game's own),
/// and can change while the game runs; EDEN_POKOPIA_POKEMON_CAP overrides it.
void InstallPokemonCap(Core::System& system, HookInstaller* hooks, u64 main_base);

} // namespace Core::Pokopia
