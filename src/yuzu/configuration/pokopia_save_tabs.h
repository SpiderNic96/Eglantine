// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

/// One tab of Pikantine's Pokopia save editor. It reads the unpacked common save in Load(), keeps
/// its edits to itself, and writes only what changed in Apply(). The layouts are PokoHex's
/// (berichan) for 2.0.1, checked against real saves.
class PokopiaSaveTab : public QWidget {
    Q_OBJECT

public:
    using QWidget::QWidget;

    /// Shows what `data` (the unpacked save) holds and forgets any edits.
    virtual void Load(const QByteArray& data) = 0;
    /// Writes the tab's edits into `data`; returns a line for the summary, or "" if none.
    virtual QString Apply(QByteArray& data) const = 0;
    /// How many entries this tab would change.
    virtual int Pending() const = 0;

signals:
    void Changed();
};

/// Moves (m_pokeSkillHistory: 64 records of 8 bytes at 0x405047, u32 uses then a byte whose
/// bit 0 means learned; then the 8-slot move wheel) and their unlocks among the function flags.
class PokopiaSkillsTab : public PokopiaSaveTab {
    Q_OBJECT

public:
    explicit PokopiaSkillsTab(QWidget* parent = nullptr);
    void Load(const QByteArray& data) override;
    QString Apply(QByteArray& data) const override;
    int Pending() const override;
    void LearnAll();

private:
    QTreeWidget* list{};
    QSet<int> known;       // move ids learned in the save
    QSet<int> known_extra; // extras (see the .cpp) already unlocked
};

/// Reactions: 128 bytes after the move wheel (bit 0 learned), then a 24-slot wheel (2 bytes a
/// slot, the reaction id first).
class PokopiaReactionsTab : public PokopiaSaveTab {
    Q_OBJECT

public:
    explicit PokopiaReactionsTab(QWidget* parent = nullptr);
    void Load(const QByteArray& data) override;
    QString Apply(QByteArray& data) const override;
    int Pending() const override;
    void LearnAll();

private:
    QTreeWidget* list{};
    QSet<int> known;
};

/// The bag: 130 slots of 5 bytes at 0x46ECEA (u32 whose low 13 bits are the item id, then the
/// count); 120 bag slots, then the 10-slot bar.
class PokopiaItemsTab : public PokopiaSaveTab {
    Q_OBJECT

public:
    explicit PokopiaItemsTab(QWidget* parent = nullptr);
    void Load(const QByteArray& data) override;
    QString Apply(QByteArray& data) const override;
    int Pending() const override;

private:
    struct Slot {
        quint32 raw{};
        int count{};
    };
    void ShowSlot(int slot);
    void SetSlot(int slot, int item, int count);
    void Refresh(int slot);

    QTreeWidget* list{};
    QComboBox* item{};
    QSpinBox* count{};
    QLabel* slot_label{};
    QVector<Slot> before;
    QVector<Slot> after;
};

/// Residents: 1050 records of 374 bytes from 0x0, by resident id, and 21-byte homes from
/// 0x79824. A record lives when bytes 1..16 (an id of the save's own) are set.
class PokopiaVillagersTab : public PokopiaSaveTab {
    Q_OBJECT

public:
    explicit PokopiaVillagersTab(QWidget* parent = nullptr);
    void Load(const QByteArray& data) override;
    QString Apply(QByteArray& data) const override;
    int Pending() const override;

private:
    struct Edit {
        int friendship{};
        bool named{};
    };
    struct Arrival {
        int id{};
        int area{};
    };
    void ShowResident();
    void FillAddList();

    QTreeWidget* list{};
    QSpinBox* friendship{};
    QCheckBox* named{};
    QComboBox* add_who{};
    QComboBox* add_area{};
    QPushButton* add_button{};
    QLabel* add_note{};
    QHash<int, Edit> before;
    QHash<int, Edit> after;
    QVector<Arrival> arrivals;
    QByteArray owner; // the 16-byte id the save's residents carry, from one of them
};

/// The 250 function flags (m_functionFlag, 32 bytes at 0x220F04). Labels you type are kept in
/// `labels_path` (JSON) so they survive between saves and builds.
class PokopiaFlagsTab : public PokopiaSaveTab {
    Q_OBJECT

public:
    PokopiaFlagsTab(const QString& labels_path, QWidget* parent = nullptr);
    void Load(const QByteArray& data) override;
    QString Apply(QByteArray& data) const override;
    int Pending() const override;

private:
    void Refilter();
    void SaveLabels();

    QString labels_path;
    QLineEdit* search{};
    QCheckBox* used_only{};
    QTreeWidget* list{};
    std::array<bool, 250> before{};
    bool loading{};
};
