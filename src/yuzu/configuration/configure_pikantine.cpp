// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_pikantine.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "yuzu/configuration/input_profiles.h"
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

constexpr u64 PokopiaId = 0x0400A3A01EE4C000;
// The add-on folders under <load>/0400A3A01EE4C000 that hold button icons. The first two are
// Pikantine's texture mods (tools/texture_mods/pokopia_kbm_glyphs.py); "Botoes PlayStation" is a
// RomFS mod some players use for the same thing.
constexpr const char* KbmIcons = "Keyboard and mouse glyphs";
constexpr const char* PsIcons = "PlayStation glyphs";
constexpr const char* PsIconsRomfs = "Botoes PlayStation";
constexpr const char* KbmProfile = "Pokopia keyboard and mouse";
constexpr const char* DualSenseProfile = "Pokopia DualSense";

enum Controls : int { NotChosen = 0, KeyboardMouse = 1, DualSense = 2 };

QString ModFolder() {
    return QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / "0400A3A01EE4C000"));
}

bool ModInstalled(const char* name) {
    return QDir(ModFolder()).exists(QString::fromUtf8(name));
}

/// A mod folder unzipped one level too deep (<mods>/<zip name>/<name>): the folder holding it.
QString NestedIn(const char* name) {
    const QDir mods(ModFolder());
    for (const QString& sub : mods.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QDir(mods.filePath(sub)).exists(QString::fromUtf8(name))) {
            return sub;
        }
    }
    return {};
}

void SetAddon(const char* name, bool on) {
    auto& disabled = Settings::values.disabled_addons[PokopiaId];
    const auto it = std::find(disabled.begin(), disabled.end(), std::string(name));
    if (on && it != disabled.end()) {
        disabled.erase(it);
    } else if (!on && it == disabled.end()) {
        disabled.emplace_back(name);
    }
}

} // namespace

ConfigurePikantine::ConfigurePikantine(Core::System& system_, QWidget* parent)
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

    auto* controls_box = new QGroupBox(tr("Controls and button icons"), this);
    auto* controls_layout = new QFormLayout(controls_box);
    controls = new QComboBox(controls_box);
    controls->addItem(tr("Leave my controls as they are"), NotChosen);
    controls->addItem(tr("Keyboard and mouse"), KeyboardMouse);
    controls->addItem(tr("DualSense (as a Pro Controller)"), DualSense);
    controls_layout->addRow(tr("Play Pokopia with"), controls);
    icons = new QComboBox(controls_box);
    icons->addItem(tr("Drawn from Player 1's controls"), 1);
    icons->addItem(tr("The game's own, or an icon mod"), 0);
    controls_layout->addRow(tr("Button icons"), icons);
    connect(icons, &QComboBox::currentIndexChanged, this,
            &ConfigurePikantine::UpdateControlsNote);
    controls_note = new QLabel(controls_box);
    controls_note->setWordWrap(true);
    controls_layout->addRow(controls_note);
    connect(controls, &QComboBox::currentIndexChanged, this,
            &ConfigurePikantine::UpdateControlsNote);
    layout->addWidget(controls_box);

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

void ConfigurePikantine::showEvent(QShowEvent* event) {
    UpdateControlsNote(); // a mod may have been added since the page was built
    QWidget::showEvent(event);
}

void ConfigurePikantine::SetInputSubsystem(InputCommon::InputSubsystem* input_subsystem_) {
    input_subsystem = input_subsystem_;
}

void ConfigurePikantine::SetConfiguration() {
    minimap->setChecked(Settings::values.pokopia_minimap.GetValue());
    pokemon_cap->setValue(Settings::values.pokopia_pokemon_cap.GetValue());
    controls->setCurrentIndex(
        std::max(0, controls->findData(static_cast<int>(Settings::values.pokopia_controls.GetValue()))));
    icons->setCurrentIndex(
        std::max(0, icons->findData(static_cast<int>(Settings::values.pokopia_icons.GetValue()))));
    UpdateControlsNote();
}

void ConfigurePikantine::UpdateControlsNote() {
    const bool drawn = icons->currentData().toInt() == 1;
    const auto mod = [drawn](const char* name) {
        if (drawn) {
            return tr("Prompts show what each button is bound to: keys and mouse buttons, or "
                      "DualSense buttons (other pads keep the Switch icons). Change a bind and "
                      "they follow from the next game start; no icon mod needed");
        }
        const QString folder = QDir::toNativeSeparators(ModFolder());
        if (ModInstalled(name)) {
            return tr("<b>%1</b> is installed").arg(QString::fromUtf8(name));
        }
        if (const QString nested = NestedIn(name); !nested.isEmpty()) {
            return tr("<b>%1</b> is one folder too deep, inside <b>%2</b>: move it up into %3")
                .arg(QString::fromUtf8(name), nested, folder);
        }
        return tr("<b>%1</b> is made the first time Pokopia starts (in %2), from your own copy "
                  "of the game")
            .arg(QString::fromUtf8(name), folder);
    };
    switch (controls->currentData().toInt()) {
    case KeyboardMouse:
        controls_note->setText(
            tr("Player 1 gets the <b>%1</b> profile: keyboard only, as Dual Joycons, so the "
               "Joy-Con mouse mode (middle click) still works. %2.")
                .arg(QString::fromUtf8(KbmProfile), mod(KbmIcons)));
        break;
    case DualSense:
        controls_note->setText(
            tr("Player 1 becomes a Pro Controller on your DualSense, mapped by position (Cross "
               "is B, Circle is A). Connect it before pressing OK. %1.")
                .arg(ModInstalled(PsIcons) || !ModInstalled(PsIconsRomfs) ? mod(PsIcons)
                                                                            : mod(PsIconsRomfs)));
        break;
    default:
        controls_note->setText(
            drawn ? tr("Pick one to set up Player 1. The prompts are drawn from Player 1's "
                       "controls either way.")
                  : tr("Pick one to set up Player 1 and switch the prompts' button icons to "
                       "match. The icon mods go in %1.")
                        .arg(QDir::toNativeSeparators(ModFolder())));
        break;
    }
}

QString ConfigurePikantine::SwitchControls(int choice) {
    QStringList done;
    bool mapped_pad = false;
    // Icons: the renderer loads whichever of Pikantine's two icon mods these settings want
    // (texture_mods.cpp), so both stay on in the add-on list. A RomFS icon mod changes the
    // texture the prompts are drawn into, so it is only on for the DualSense without our own.
    const bool ps = choice == DualSense;
    const bool drawn = icons->currentData().toInt() == 1;
    SetAddon(KbmIcons, true);
    SetAddon(PsIcons, true);
    SetAddon(PsIconsRomfs, !drawn && ps && !ModInstalled(PsIcons));

    auto* p1 = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (choice == KeyboardMouse) {
        // The profile ships inside Eglantine; a copy you have edited is kept.
        const QString dir = QString::fromStdString(Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "input"));
        const QString file = QDir(dir).filePath(QString::fromUtf8(KbmProfile) +
                                                QStringLiteral(".ini"));
        if (!QFile::exists(file)) {
            QDir().mkpath(dir);
            QFile::copy(QStringLiteral(":/pikantine/kbm_profile.ini"), file);
            QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser |
                                            QFile::WriteUser);
        }
        InputProfiles profiles;
        if (profiles.LoadProfile(KbmProfile, 0)) {
            done << tr("Player 1 now uses the \"%1\" profile.").arg(QString::fromUtf8(KbmProfile));
        } else {
            done << tr("Could not load the \"%1\" profile; Player 1 is unchanged.")
                        .arg(QString::fromUtf8(KbmProfile));
        }
    } else if (choice == DualSense) {
        Common::ParamPackage pad;
        if (input_subsystem != nullptr) {
            for (const auto& device : input_subsystem->GetInputDevices()) {
                if (device.Get("engine", "") != "sdl") {
                    continue;
                }
                const std::string name = device.Get("display", "");
                const bool dualsense = name.find("DualSense") != std::string::npos ||
                                       name.find("PS5") != std::string::npos;
                if (dualsense || !pad.Has("engine")) {
                    pad = device;
                }
                if (dualsense) {
                    break;
                }
            }
        }
        if (!pad.Has("engine")) {
            done << tr("No controller found, so Player 1 is unchanged: connect your DualSense, "
                       "then pick Keyboard and mouse and DualSense again.");
        } else {
            auto& player = Settings::values.players.GetValue()[0];
            player.buttons = {};
            player.analogs = {};
            player.motions = {};
            for (const auto& [index, param] : input_subsystem->GetButtonMappingForDevice(pad)) {
                player.buttons[index] = param.Serialize();
            }
            for (const auto& [index, param] : input_subsystem->GetAnalogMappingForDevice(pad)) {
                player.analogs[index] = param.Serialize();
            }
            for (const auto& [index, param] : input_subsystem->GetMotionMappingForDevice(pad)) {
                player.motions[index] = param.Serialize();
            }
            player.controller_type = Settings::ControllerType::ProController;
            player.connected = true;
            player.profile_name.clear();
            mapped_pad = true;
            done << tr("Player 1 is now a Pro Controller on %1.")
                        .arg(QString::fromStdString(pad.Get("display", "your controller")));
        }
    }
    // The controller reads its bindings back, as the Controls page does after loading a
    // profile, then keeps them the way that page's Apply does.
    p1->ReloadFromSettings();
    p1->DisableConfiguration();
    p1->SaveCurrentConfig();
    p1->EnableConfiguration();
    if (mapped_pad) {
        // Kept as an input profile too, to load again from Controls -> Profile.
        InputProfiles profiles;
        if (profiles.CreateProfile(DualSenseProfile, 0) ||
            profiles.SaveProfile(DualSenseProfile, 0)) {
            done << tr("Saved as the \"%1\" input profile.").arg(QString::fromUtf8(DualSenseProfile));
        }
    }
    return done.join(QLatin1Char(' '));
}

void ConfigurePikantine::ApplyConfiguration() {
    Settings::values.pokopia_minimap = minimap->isChecked();
    Settings::values.pokopia_pokemon_cap = static_cast<u8>(pokemon_cap->value());
    const int choice = controls->currentData().toInt();
    if (choice != NotChosen && choice != Settings::values.pokopia_controls.GetValue()) {
        const QString done = SwitchControls(choice);
        QMessageBox::information(
            this, tr("Pikantine"),
            tr("%1\n\nThe button icons change the next time Pokopia starts.").arg(done));
    }
    const int drawn = icons->currentData().toInt();
    if (drawn == 1 && Settings::values.pokopia_icons.GetValue() != 1) {
        SetAddon(PsIconsRomfs, false);
    }
    Settings::values.pokopia_controls = static_cast<u8>(choice);
    Settings::values.pokopia_icons = static_cast<u8>(drawn);
}
