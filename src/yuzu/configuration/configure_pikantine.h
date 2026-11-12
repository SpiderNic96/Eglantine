// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

/// Settings -> Pikantine: Pikantine's Pokémon Pokopia options (the minimap overlay, the Pokémon
/// limit, the controls and their button icons) and its save editor (pokopia_dex_dialog.h), built
/// in code, not a .ui file.
class ConfigurePikantine : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePikantine(Core::System& system, QWidget* parent = nullptr);
    ~ConfigurePikantine() override;

    void ApplyConfiguration();
    /// The input subsystem, for finding a DualSense; without it only the icons switch.
    void SetInputSubsystem(InputCommon::InputSubsystem* input_subsystem_);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void SetConfiguration();
    void UpdateControlsNote();
    /// Switches the button-icon add-ons and Player 1's controls; returns what it did.
    QString SwitchControls(int controls, bool swap);

    Core::System& system;
    InputCommon::InputSubsystem* input_subsystem{};

    QCheckBox* minimap{};
    QSpinBox* pokemon_cap{};
    QLabel* pokemon_cap_note{};
    QComboBox* controls{};
    QComboBox* icons{};
    QCheckBox* swap_confirm{};
    QLabel* controls_note{};
};
