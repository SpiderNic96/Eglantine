// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_pikantine.h"

#include <cstdlib>

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/pokopia_dex_dialog.h"

ConfigurePikantine::ConfigurePikantine(const Core::System& system_, QWidget* parent)
    : QWidget(parent), system{system_} {
    setAccessibleName(tr("Pikantine"));
    auto* layout = new QVBoxLayout(this);

    auto* map_box = new QGroupBox(tr("Pokémon Pokopia minimap"), this);
    auto* map_layout = new QVBoxLayout(map_box);
    minimap = new QCheckBox(tr("Show the minimap and the ping arrow while playing"), map_box);
    auto* map_note = new QLabel(
        tr("Show or hide it in game with the <b>Toggle Pokopia Minimap</b> hotkey: <b>F7</b> on the "
           "keyboard, <b>Home + D-pad Left</b> on a controller. Change those under General → "
           "Hotkeys.<br>Pings: <b>G</b> or <b>L3 + R3</b> where you stand, or on the pause menu's "
           "map a mouse click, or the right stick then <b>R3</b>."),
        map_box);
    map_note->setWordWrap(true);
    map_layout->addWidget(minimap);
    map_layout->addWidget(map_note);
    layout->addWidget(map_box);

    auto* cap_box = new QGroupBox(tr("Pokémon limit"), this);
    auto* cap_layout = new QFormLayout(cap_box);
    pokemon_cap = new QSpinBox(cap_box);
    pokemon_cap->setRange(0, 50);
    pokemon_cap->setSpecialValueText(tr("Game's own (25)"));
    cap_layout->addRow(tr("Pokémon living in your world"), pokemon_cap);
    pokemon_cap_note = new QLabel(
        tr("Up to 50, per area. The game places Pokémon while an area loads, so a higher number "
           "makes entering an area slower (around 30 stays quick). Changes apply from the next "
           "area you enter.<br><b>Back up your save first:</b> going back to a lower limit may have the "
           "game send the extra Pokémon away."),
        cap_box);
    pokemon_cap_note->setWordWrap(true);
    cap_layout->addRow(pokemon_cap_note);
    if (std::getenv("EDEN_POKOPIA_POKEMON_CAP") != nullptr) {
        pokemon_cap->setEnabled(false);
        pokemon_cap_note->setText(
            tr("Set by the EDEN_POKOPIA_POKEMON_CAP environment variable, which overrides this."));
    }
    layout->addWidget(cap_box);

    auto* dex_box = new QGroupBox(tr("Pokédex"), this);
    auto* dex_layout = new QVBoxLayout(dex_box);
    auto* dex_note = new QLabel(
        tr("<b>Off for now:</b> the save flag the editor changed turned out not to be the "
           "Pokédex, so ticked Pokémon did not register. If you saved with it, put your save back "
           "from the <b>pikantine_backups</b> folder in Eglantine's folder (Pikantine build 34)."),
        dex_box);
    dex_note->setWordWrap(true);
    auto* dex_button = new QPushButton(tr("Open the Pokédex editor…"), dex_box);
    dex_button->setEnabled(false); // until the Pokédex's own save data is found
    dex_button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(dex_button, &QPushButton::clicked, this, [this] {
        PokopiaDexDialog dialog(system.IsPoweredOn(), this);
        dialog.exec();
    });
    dex_layout->addWidget(dex_note);
    dex_layout->addWidget(dex_button);
    layout->addWidget(dex_box);

    layout->addStretch(1);
    SetConfiguration();
}

ConfigurePikantine::~ConfigurePikantine() = default;

void ConfigurePikantine::SetConfiguration() {
    minimap->setChecked(Settings::values.pokopia_minimap.GetValue());
    pokemon_cap->setValue(Settings::values.pokopia_pokemon_cap.GetValue());
}

void ConfigurePikantine::ApplyConfiguration() {
    Settings::values.pokopia_minimap = minimap->isChecked();
    Settings::values.pokopia_pokemon_cap = static_cast<u8>(pokemon_cap->value());
}
