// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

namespace Core {
class System;
}

class QCheckBox;
class QLabel;
class QSpinBox;

/// Settings -> Pikantine: Pikantine's Pokémon Pokopia options (the minimap overlay and the
/// Pokémon limit) and its Pokédex editor (pokopia_dex_dialog.h), built in code, not a .ui file.
class ConfigurePikantine : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePikantine(const Core::System& system, QWidget* parent = nullptr);
    ~ConfigurePikantine() override;

    void ApplyConfiguration();

private:
    void SetConfiguration();

    const Core::System& system;

    QCheckBox* minimap{};
    QSpinBox* pokemon_cap{};
    QLabel* pokemon_cap_note{};
};
