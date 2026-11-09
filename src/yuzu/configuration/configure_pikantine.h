// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QSpinBox;

/// Settings -> Pikantine: Pikantine's Pokémon Pokopia options (the minimap overlay and the
/// Pokémon limit), built in code rather than from a .ui file.
class ConfigurePikantine : public QWidget {
    Q_OBJECT

public:
    explicit ConfigurePikantine(QWidget* parent = nullptr);
    ~ConfigurePikantine() override;

    void ApplyConfiguration();

private:
    void SetConfiguration();

    QCheckBox* minimap{};
    QSpinBox* pokemon_cap{};
    QLabel* pokemon_cap_note{};
};
