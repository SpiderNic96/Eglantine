// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/pokopia_controls.h"

#include <string>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "input_common/main.h"
#include "yuzu/configuration/input_profiles.h"

namespace Pikantine::PokopiaControls {

namespace {

QString ProfileFile(const char* name) {
    return QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "input" /
        (std::string(name) + ".ini")));
}

Settings::PlayerInput& Player1() {
    return Settings::values.players.GetValue()[0];
}

std::string EngineOfA() {
    return Common::ParamPackage(Player1().buttons[Settings::NativeButton::A]).Get("engine", "");
}

/// The connected DualSense, else the first SDL pad; an empty package if there is none.
Common::ParamPackage FindPad(InputCommon::InputSubsystem* input) {
    Common::ParamPackage pad;
    if (input == nullptr) {
        return pad;
    }
    for (const auto& device : input->GetInputDevices()) {
        if (device.Get("engine", "") != "sdl") {
            continue;
        }
        const std::string name = device.Get("display", "");
        const bool dualsense =
            name.find("DualSense") != std::string::npos || name.find("PS5") != std::string::npos;
        if (dualsense || !pad.Has("engine")) {
            pad = device;
        }
        if (dualsense) {
            break;
        }
    }
    return pad;
}

} // namespace

void InstallProfiles() {
    const QString file = ProfileFile(KbmProfile);
    if (QFile::exists(file)) {
        return;
    }
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (QFile::copy(QStringLiteral(":/pikantine/kbm_profile.ini"), file)) {
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser |
                                        QFile::WriteUser);
    }
}

void SwapConfirmBack() {
    auto& p = Player1();
    std::swap(p.buttons[Settings::NativeButton::A], p.buttons[Settings::NativeButton::B]);
}

QString Load(InputCommon::InputSubsystem* input, int choice, bool swap) {
    if (choice == KeyboardMouse) {
        InstallProfiles();
        InputProfiles profiles;
        return profiles.LoadProfile(KbmProfile, 0)
                   ? QObject::tr("Player 1 uses the \"%1\" profile.").arg(QString::fromUtf8(KbmProfile))
                   : QObject::tr("Could not load the \"%1\" profile; Player 1 is unchanged.")
                         .arg(QString::fromUtf8(KbmProfile));
    }
    if (choice != DualSense) {
        return {};
    }
    if (QFile::exists(ProfileFile(DualSenseProfile))) {
        InputProfiles profiles;
        if (profiles.LoadProfile(DualSenseProfile, 0)) {
            return QObject::tr("Player 1 uses the \"%1\" profile.")
                .arg(QString::fromUtf8(DualSenseProfile));
        }
    }
    const Common::ParamPackage pad = FindPad(input);
    if (!pad.Has("engine")) {
        return QObject::tr("No controller found, so Player 1 is unchanged: connect your "
                           "DualSense, then choose it again.");
    }
    auto& player = Player1();
    player.buttons = {};
    player.analogs = {};
    player.motions = {};
    for (const auto& [index, param] : input->GetButtonMappingForDevice(pad)) {
        player.buttons[index] = param.Serialize();
    }
    for (const auto& [index, param] : input->GetAnalogMappingForDevice(pad)) {
        player.analogs[index] = param.Serialize();
    }
    for (const auto& [index, param] : input->GetMotionMappingForDevice(pad)) {
        player.motions[index] = param.Serialize();
    }
    player.controller_type = Settings::ControllerType::ProController;
    player.connected = true;
    player.profile_name.clear();
    if (swap) {
        SwapConfirmBack();
    }
    InputProfiles profiles;
    if (!profiles.CreateProfile(DualSenseProfile, 0)) {
        profiles.SaveProfile(DualSenseProfile, 0);
    }
    return QObject::tr("Player 1 is a Pro Controller on %1, saved as the \"%2\" profile.")
        .arg(QString::fromStdString(pad.Get("display", "your controller")),
             QString::fromUtf8(DualSenseProfile));
}

void SaveBack(int choice) {
    const std::string engine = EngineOfA();
    const char* name = nullptr;
    if (choice == KeyboardMouse && engine == "keyboard") {
        name = KbmProfile;
    } else if (choice == DualSense && engine == "sdl") {
        name = DualSenseProfile;
    }
    if (name == nullptr) {
        return;
    }
    InputProfiles profiles;
    if (!profiles.SaveProfile(name, 0)) {
        profiles.CreateProfile(name, 0);
    }
}

bool ApplyAtBoot(InputCommon::InputSubsystem* input) {
    const int choice = Settings::values.pokopia_controls.GetValue();
    if (choice == NotChosen) {
        return false;
    }
    const QString what = Load(input, choice, Settings::values.pokopia_swap_confirm.GetValue());
    LOG_INFO(Frontend, "Pokopia controls: {}", what.toStdString());
    return true;
}

} // namespace Pikantine::PokopiaControls
