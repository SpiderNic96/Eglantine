// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace InputCommon {
class InputSubsystem;
}

/// Pikantine: Player 1's controls for Pokémon Pokopia, as Settings -> Pikantine chooses them.
///
/// Each choice is an input profile in the input profile folder: "Pokopia keyboard and mouse"
/// (shipped inside Eglantine, keyboard only as Dual Joycons so the Joy-Con mouse works) and
/// "Pokopia DualSense" (made from the connected pad the first time, a Pro Controller). The chosen
/// one is loaded into Player 1 each time Pokopia starts, and binds changed under Controls are
/// written back into it, so custom binds stay.
namespace Pikantine::PokopiaControls {

inline constexpr int NotChosen = 0;
inline constexpr int KeyboardMouse = 1;
inline constexpr int DualSense = 2;

inline constexpr const char* KbmProfile = "Pokopia keyboard and mouse";
inline constexpr const char* DualSenseProfile = "Pokopia DualSense";

/// Puts the keyboard and mouse profile in the input profile folder if it is not there.
void InstallProfiles();

/// Loads `choice` into Player 1 (Settings only; the caller reloads the controller). For the
/// DualSense, the saved profile is loaded, or one is made from the connected pad (Cross on B,
/// Circle on A, or swapped when `swap`). Returns what it did, for the player to read.
QString Load(InputCommon::InputSubsystem* input, int choice, bool swap);

/// Swaps Player 1's A and B binds (Settings only).
void SwapConfirmBack();

/// Writes Player 1's binds into the profile of `choice`, if Player 1 is on that kind of input
/// (keys for keyboard and mouse, a pad for the DualSense).
void SaveBack(int choice);

/// At game start: loads the chosen profile into Player 1 (Settings only; the caller reloads the
/// controllers). Returns true if it loaded one.
bool ApplyAtBoot(InputCommon::InputSubsystem* input);

} // namespace Pikantine::PokopiaControls
