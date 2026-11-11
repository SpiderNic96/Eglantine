// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class PokopiaSaveTab;

/// A console user profile, so saves can be listed by whose they are. `keys` are the folder names
/// the emulator may keep that user's saves under (the user id as 32 hex digits, two spellings).
struct PokopiaProfile {
    QString name;
    QStringList keys;
    bool current{};
};

/// Pikantine's save editor for Pokémon Pokopia 2.0.1: Pokédex, habitats and recipes here, and the
/// moves, reactions, bag, residents and function flags tabs of pokopia_save_tabs.h.
///
/// The common save (G00/CMNDAT.BIN: a 0x130-byte header, then zlib) keeps a byte per Pokédex
/// slot at 0x426EFF of its data (bits 0+1 registered, bit 3 seen) and 12 bytes per habitat id at
/// 0x4256FF (u32 state: 1 discovered, 3 built). The slot and habitat names come from the game's
/// own tables (pokopia_dex_data.h). The dialog backs the save folder up first and checks what it
/// wrote by reading it back. Dex rewards are left alone, so the game hands them out itself.
class PokopiaDexDialog : public QDialog {
    Q_OBJECT

public:
    /// Lists the saves found, by profile; opens the current profile's, or asks whose to edit.
    PokopiaDexDialog(bool game_running, QVector<PokopiaProfile> profiles,
                     QWidget* parent = nullptr);
    ~PokopiaDexDialog() override;

private:
    void FindSaves();
    void LoadSave(const QString& folder);
    void Populate();
    void UpdateCount();
    void Save();
    void Refilter();
    void PickStartingSave();
    QString OwnerOf(const QString& folder) const;

    QComboBox* folders{};
    QLabel* status{};
    QLineEdit* dex_search{};
    QLineEdit* habitat_search{};
    QLineEdit* recipe_search{};
    QComboBox* dex_area{};
    QComboBox* habitat_area{};
    QTreeWidget* dex_list{};
    QTreeWidget* habitat_list{};
    QTreeWidget* recipe_list{};
    QCheckBox* link_habitats{};
    QCheckBox* fill_sightings{};
    QPushButton* save_button{};
    QVector<PokopiaSaveTab*> extra_tabs; // applied in this order (flags first)
    QVector<PokopiaProfile> profiles;

    bool game_running{};
    QString folder;   // the save folder (holds G00)
    QByteArray file;  // G00/CMNDAT.BIN as read
    QByteArray data;  // its data, unpacked
    QHash<int, int> dex_state;     // slot -> 0 none, 1 seen, 2 registered (as in the save)
    QHash<int, int> habitat_state; // id -> 0 none, 1 discovered, 2 built (as in the save)
    QSet<int> learned;             // recipe indices the save has
    bool populating{};
};
