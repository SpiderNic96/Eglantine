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

#include <fmt/format.h>

#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "yuzu/configuration/pokopia_dex_dialog.h"

namespace {

/// The console's user profiles, with the folder names their saves can sit under.
QVector<PokopiaProfile> Profiles(const Core::System& system) {
    QVector<PokopiaProfile> out;
    const auto& manager = system.GetProfileManager();
    const auto current = manager.GetUser(Settings::values.current_user.GetValue());
    for (std::size_t index = 0; index < Service::Account::MAX_USERS; ++index) {
        const auto user = manager.GetUser(index);
        if (!user.has_value()) {
            continue;
        }
        const Common::UUID uuid = *user;
        Service::Account::ProfileBase base{};
        PokopiaProfile p;
        if (manager.GetProfileBase(uuid, base)) {
            p.name = QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
                reinterpret_cast<const char*>(base.username.data()), base.username.size()));
        }
        const auto id = uuid.AsU128();
        p.keys << QString::fromStdString(fmt::format("{:016X}{:016X}", id[1], id[0]))
               << QString::fromStdString(uuid.RawString());
        if (p.name.isEmpty()) {
            p.name = p.keys.front();
        }
        p.current = current.has_value() && *current == uuid;
        out.push_back(p);
    }
    return out;
}

} // namespace

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
        tr("Up to 50 residents, with the 64-entry object pool keeping up to 14 additional slots "
           "for transient visitors and item-triggered spawns. The game places Pokémon while an "
           "area loads, so a higher number makes entering an area slower (around 30 stays quick). "
           "Changes apply from the next "
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

    auto* dex_box = new QGroupBox(tr("Save editor"), this);
    auto* dex_layout = new QVBoxLayout(dex_box);
    auto* dex_note = new QLabel(
        tr("Pokédex, habitats and recipes; moves, reactions, the bag, residents and the game's "
           "feature flags. Opens your current profile's 2.0.1 save (or asks whose), edits it while "
           "the game is stopped, and backs it up first."),
        dex_box);
    dex_note->setWordWrap(true);
    auto* dex_button = new QPushButton(tr("Open the save editor…"), dex_box);
    dex_button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(dex_button, &QPushButton::clicked, this, [this] {
        PokopiaDexDialog dialog(system.IsPoweredOn(), Profiles(system), this);
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
