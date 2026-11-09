// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDialog>
#include <QSet>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/// Pikantine's Pokédex editor for Pokémon Pokopia 2.0.1 saves.
///
/// The common save (G00/CMNDAT.BIN: a 0x130-byte header, then zlib) holds a byte per resident
/// (0..1049) at 0x221758 of its data; bit 0 is "registered in the Pokédex" (the game's own
/// "No. Registered" counts them). The dialog lists the residents by their portrait order (the
/// Pokédex's), shows which are registered, and sets bit 0 on the ones picked. It backs the save
/// folder up first and checks what it wrote by reading it back.
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
    void Filter(const QString& text);
    void Save();

    QComboBox* folders{};
    QLabel* status{};
    QLineEdit* search{};
    QListWidget* list{};
    QPushButton* save_button{};

    bool game_running{};
    QString folder;      // the save folder (holds G00)
    QByteArray file;     // G00/CMNDAT.BIN as read
    QByteArray data;     // its data, unpacked
    QSet<int> registered; // as in the save
    bool populating{};
};
