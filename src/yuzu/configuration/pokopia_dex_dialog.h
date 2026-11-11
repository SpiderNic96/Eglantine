// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDialog>
#include <QHash>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/// Pikantine's Pokédex and Habitat Dex editor for Pokémon Pokopia 2.0.1 saves.
///
/// The common save (G00/CMNDAT.BIN: a 0x130-byte header, then zlib) keeps a byte per Pokédex
/// slot at 0x426EFF of its data (bits 0+1 registered, bit 3 seen) and 12 bytes per habitat id at
/// 0x4256FF (u32 state: 1 discovered, 3 built). The slot and habitat names come from the game's
/// own tables (pokopia_dex_data.h). The dialog backs the save folder up first and checks what it
/// wrote by reading it back. Dex rewards are left alone, so the game hands them out itself.
class PokopiaDexDialog : public QDialog {
    Q_OBJECT

public:
    explicit PokopiaDexDialog(bool game_running, QWidget* parent = nullptr);
    ~PokopiaDexDialog() override;

private:
    void FindSaves();
    void LoadSave(const QString& folder);
    void Populate();
    void UpdateCount();
    void Save();
    void Refilter();

    QComboBox* folders{};
    QLabel* status{};
    QLineEdit* dex_search{};
    QLineEdit* habitat_search{};
    QComboBox* dex_area{};
    QComboBox* habitat_area{};
    QTreeWidget* dex_list{};
    QTreeWidget* habitat_list{};
    QCheckBox* link_habitats{};
    QCheckBox* fill_sightings{};
    QPushButton* save_button{};

    bool game_running{};
    QString folder;   // the save folder (holds G00)
    QByteArray file;  // G00/CMNDAT.BIN as read
    QByteArray data;  // its data, unpacked
    QHash<int, int> dex_state;     // slot -> 0 none, 1 seen, 2 registered (as in the save)
    QHash<int, int> habitat_state; // id -> 0 none, 1 discovered, 2 built (as in the save)
    bool populating{};
};
