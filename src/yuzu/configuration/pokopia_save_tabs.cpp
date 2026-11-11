// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/pokopia_save_tabs.h"

#include <algorithm>
#include <map>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtEndian>

#include "yuzu/configuration/pokopia_flag_info.h"
#include "yuzu/configuration/pokopia_save_names.h"

namespace {

// Offsets in the unpacked 2.0.1 common save (PokoHex's SaveLayout.Version381).
constexpr int AbilitiesOffset = 0x405047;
constexpr int SkillCount = 64;
constexpr int SkillSize = 8;
constexpr int SkillWheel = AbilitiesOffset + SkillCount * SkillSize; // 8 bytes
constexpr int SkillWheelCount = 8;
constexpr int Reactions = SkillWheel + SkillWheelCount;              // 128 bytes
constexpr int ReactionCount = 128;
constexpr int ReactionWheel = Reactions + ReactionCount;             // 24 x 2 bytes
constexpr int ReactionWheelCount = 24;
constexpr int FlagsOffset = 0x220F04;
constexpr int FlagCount = 250;
constexpr int FirstSpareFlag = 160;
constexpr int InventoryOffset = 0x46ECEA;
constexpr int InventoryCount = 130;
constexpr int BagCount = 120;
constexpr int ItemSize = 5;
constexpr quint32 ItemIdMask = 0x1FFF;
constexpr int MaxStack = 255;
constexpr int VillagerCount = 1050;
constexpr int VillagerSize = 374;
constexpr int HomeOffset = 0x79824;
constexpr int HomeSize = 21;
constexpr int FriendshipMax = 170;

constexpr int IdRole = Qt::UserRole;
constexpr int BeforeRole = Qt::UserRole + 1;

const QBrush& Fresh() {
    static const QBrush brush(QColor(0x2e, 0x7d, 0x32));
    return brush;
}

void Mark(QTreeWidgetItem* item, bool fresh) {
    for (int c = 0; c < item->columnCount(); ++c) {
        item->setForeground(c, fresh ? Fresh() : QBrush());
    }
}

QTreeWidget* MakeTree(QWidget* parent, const QStringList& headers) {
    auto* list = new QTreeWidget(parent);
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setColumnWidth(0, 230);
    return list;
}

QLabel* Note(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    return label;
}

bool Flag(const QByteArray& data, int id) {
    return (static_cast<quint8>(data[FlagsOffset + id / 8]) >> (id % 8)) & 1;
}

void SetFlag(QByteArray& data, int id, bool on) {
    auto& byte = reinterpret_cast<quint8&>(data[FlagsOffset + id / 8]);
    byte = on ? static_cast<quint8>(byte | (1 << (id % 8)))
              : static_cast<quint8>(byte & ~(1 << (id % 8)));
}

QString Name(const char* name, int id, const QString& fallback) {
    return name != nullptr && name[0] != '\0' ? QString::fromUtf8(name) : fallback.arg(id);
}

QString AreaName(int area) {
    if (area > 0 && area < static_cast<int>(Pikantine::PokopiaAreaTitles.size()) &&
        Pikantine::PokopiaAreaTitles[area][0] != '\0') {
        return QString::fromUtf8(Pikantine::PokopiaAreaTitles[area]);
    }
    return area == 0 ? QObject::tr("none") : QObject::tr("Area %1").arg(area);
}

// --- Moves ------------------------------------------------------------------------------------

constexpr std::array<int, 10> PublicSkills{1, 2, 3, 4, 5, 6, 10, 11, 12, 13};

bool SkillOnWheel(int id) {
    return (id >= 1 && id <= 6) || id == 12 || id == 13;
}

/// Surf and Glide also need a permission flag before the game counts them as learned.
int SkillPermission(int id) {
    return id == 10 ? 56 : id == 11 ? 57 : 0;
}

/// Unlocks that hang off a move: function flags, with the move they need (PokoHex's rules).
struct Extra {
    int key;
    const char* name;
    int needs;
    std::array<int, 2> flags;
};
constexpr int ExtraBase = 1000;
constexpr int DiveKey = ExtraBase + 2;
constexpr std::array<Extra, 7> Extras{{
    {ExtraBase + 1, "Liquid absorption (Water Gun)", 3, {54, 0}},
    {DiveKey, "Dive (Surf)", 10, {148, 0}},
    {ExtraBase + 3, "Leafage food boost", 1, {71, 0}},
    {ExtraBase + 4, "Water Gun food boost", 3, {73, 0}},
    {ExtraBase + 5, "Rock Smash food boost and extra effect", 4, {74, 125}},
    {ExtraBase + 6, "Cut food boost", 5, {72, 0}},
    {ExtraBase + 7, "Surf food boost (needs Dive)", 10, {153, 0}},
}};

const Extra* ExtraOf(int key) {
    for (const auto& e : Extras) {
        if (e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

bool SkillLearned(const QByteArray& data, int id) {
    const bool bit = (data[AbilitiesOffset + id * SkillSize + 4] & 1) != 0;
    const int permission = SkillPermission(id);
    return bit && (permission == 0 || Flag(data, permission));
}

bool ExtraUnlocked(const QByteArray& data, const Extra& e) {
    for (const int f : e.flags) {
        if (f != 0 && !Flag(data, f)) {
            return false;
        }
    }
    return true;
}

QTreeWidgetItem* FindRow(QTreeWidget* list, int id) {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        if (list->topLevelItem(i)->data(0, IdRole).toInt() == id) {
            return list->topLevelItem(i);
        }
    }
    return nullptr;
}

bool Ticked(const QTreeWidgetItem* item) {
    return item != nullptr && item->checkState(0) == Qt::Checked;
}

/// Ticked rows that the save does not have yet, by id.
QSet<int> NewlyTicked(QTreeWidget* list) {
    QSet<int> ids;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const auto* item = list->topLevelItem(i);
        if (Ticked(item) && !item->data(0, BeforeRole).toBool()) {
            ids.insert(item->data(0, IdRole).toInt());
        }
    }
    return ids;
}

/// Rows the save already has can't be unticked: the editor only adds.
void AddRow(QTreeWidget* list, int id, const QStringList& text, bool have) {
    auto* item = new QTreeWidgetItem(list, text);
    item->setData(0, IdRole, id);
    item->setData(0, BeforeRole, have);
    item->setCheckState(0, have ? Qt::Checked : Qt::Unchecked);
    if (have) {
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        item->setToolTip(0, QObject::tr("Already in the save"));
    } else {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    }
}

} // namespace

// --- Moves ------------------------------------------------------------------------------------

PokopiaSkillsTab::PokopiaSkillsTab(QWidget* parent) : PokopiaSaveTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(Note(
        this, tr("Tick a move to learn it (it goes on your move wheel if there's room). The rows "
                 "below the moves are their upgrades; ticking one also ticks the move it needs. "
                 "Learned moves stay learned.")));
    list = MakeTree(this, {tr("Move or upgrade"), tr("Id"), tr("Times used")});
    layout->addWidget(list, 1);
    auto* all = new QPushButton(tr("Learn every move and upgrade"), this);
    auto* row = new QHBoxLayout();
    row->addWidget(all);
    row->addStretch(1);
    layout->addLayout(row);
    connect(all, &QPushButton::clicked, this, &PokopiaSkillsTab::LearnAll);
    connect(list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0) {
            return;
        }
        const int id = item->data(0, IdRole).toInt();
        const bool on = Ticked(item);
        Mark(item, on != item->data(0, BeforeRole).toBool());
        if (const Extra* e = ExtraOf(id); e != nullptr) {
            if (on) {
                if (auto* move = FindRow(list, e->needs); move && !Ticked(move)) {
                    move->setCheckState(0, Qt::Checked);
                }
                if (e->key == ExtraBase + 7) {
                    if (auto* dive = FindRow(list, DiveKey); dive && !Ticked(dive)) {
                        dive->setCheckState(0, Qt::Checked);
                    }
                }
            }
        } else if (!on) {
            // Unticking a move unticks the new upgrades that need it.
            for (const auto& e : Extras) {
                auto* extra = FindRow(list, e.key);
                if (extra && e.needs == id && Ticked(extra) &&
                    !extra->data(0, BeforeRole).toBool()) {
                    extra->setCheckState(0, Qt::Unchecked);
                }
            }
        }
        if (id == DiveKey && !on) {
            if (auto* boost = FindRow(list, ExtraBase + 7);
                boost && Ticked(boost) && !boost->data(0, BeforeRole).toBool()) {
                boost->setCheckState(0, Qt::Unchecked);
            }
        }
        emit Changed();
    });
}

void PokopiaSkillsTab::Load(const QByteArray& data) {
    const QSignalBlocker blocker(list);
    list->clear();
    known.clear();
    known_extra.clear();
    for (const int id : PublicSkills) {
        const bool have = SkillLearned(data, id);
        if (have) {
            known.insert(id);
        }
        const quint32 uses = qFromLittleEndian<quint32>(data.constData() + AbilitiesOffset +
                                                        id * SkillSize);
        AddRow(list, id,
               {Name(Pikantine::PokopiaSkillNames[id], id, tr("Move %1")), QString::number(id),
                QString::number(uses)},
               have);
    }
    for (const auto& e : Extras) {
        const bool have = ExtraUnlocked(data, e);
        if (have) {
            known_extra.insert(e.key);
        }
        QStringList flags;
        for (const int f : e.flags) {
            if (f != 0) {
                flags << QString::number(f);
            }
        }
        AddRow(list, e.key,
               {QStringLiteral("    ") + QString::fromUtf8(e.name),
                tr("flag %1").arg(flags.join(QStringLiteral(", "))), QString()},
               have);
    }
}

void PokopiaSkillsTab::LearnAll() {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        if (item->flags() & Qt::ItemIsUserCheckable) {
            item->setCheckState(0, Qt::Checked);
        }
    }
}

int PokopiaSkillsTab::Pending() const {
    return static_cast<int>(NewlyTicked(list).size());
}

QString PokopiaSkillsTab::Apply(QByteArray& data) const {
    const QSet<int> ticked = NewlyTicked(list);
    int moves = 0, extras = 0;
    for (const int id : PublicSkills) {
        if (!ticked.contains(id)) {
            continue;
        }
        if (const int permission = SkillPermission(id); permission != 0) {
            SetFlag(data, permission, true);
        }
        auto& learned = reinterpret_cast<quint8&>(data[AbilitiesOffset + id * SkillSize + 4]);
        learned |= 1;
        ++moves;
        if (!SkillOnWheel(id)) {
            continue;
        }
        char* wheel = data.data() + SkillWheel;
        if (std::find(wheel, wheel + SkillWheelCount, static_cast<char>(id)) !=
            wheel + SkillWheelCount) {
            continue;
        }
        if (char* empty = std::find(wheel, wheel + SkillWheelCount, '\0');
            empty != wheel + SkillWheelCount) {
            *empty = static_cast<char>(id);
        }
    }
    for (const auto& e : Extras) {
        if (!ticked.contains(e.key) || !SkillLearned(data, e.needs)) {
            continue;
        }
        if (e.key == ExtraBase + 7 && !Flag(data, 148)) {
            continue;
        }
        for (const int f : e.flags) {
            if (f != 0) {
                SetFlag(data, f, true);
            }
        }
        ++extras;
    }
    if (moves == 0 && extras == 0) {
        return {};
    }
    return tr("Moves: %1 learned, %2 upgrades unlocked.").arg(moves).arg(extras);
}

// --- Reactions --------------------------------------------------------------------------------

PokopiaReactionsTab::PokopiaReactionsTab(QWidget* parent) : PokopiaSaveTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(Note(this, tr("Tick a reaction to learn it. New ones go on the reaction "
                                    "wheel while it has empty slots (24).")));
    list = MakeTree(this, {tr("Reaction"), tr("Id")});
    layout->addWidget(list, 1);
    auto* all = new QPushButton(tr("Learn every reaction"), this);
    auto* row = new QHBoxLayout();
    row->addWidget(all);
    row->addStretch(1);
    layout->addLayout(row);
    connect(all, &QPushButton::clicked, this, &PokopiaReactionsTab::LearnAll);
    connect(list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column == 0) {
            Mark(item, Ticked(item) != item->data(0, BeforeRole).toBool());
            emit Changed();
        }
    });
}

void PokopiaReactionsTab::Load(const QByteArray& data) {
    const QSignalBlocker blocker(list);
    list->clear();
    known.clear();
    for (int id = 1; id <= 100; ++id) {
        if (Pikantine::PokopiaReactionNames[id][0] == '\0') {
            continue;
        }
        const bool have = (data[Reactions + id] & 1) != 0;
        if (have) {
            known.insert(id);
        }
        AddRow(list, id,
               {QString::fromUtf8(Pikantine::PokopiaReactionNames[id]), QString::number(id)},
               have);
    }
}

void PokopiaReactionsTab::LearnAll() {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        if (item->flags() & Qt::ItemIsUserCheckable) {
            item->setCheckState(0, Qt::Checked);
        }
    }
}

int PokopiaReactionsTab::Pending() const {
    return static_cast<int>(NewlyTicked(list).size());
}

QString PokopiaReactionsTab::Apply(QByteArray& data) const {
    QList<int> ticked = NewlyTicked(list).values();
    std::sort(ticked.begin(), ticked.end());
    int learned = 0, wheeled = 0;
    for (const int id : ticked) {
        auto& byte = reinterpret_cast<quint8&>(data[Reactions + id]);
        byte |= 1;
        ++learned;
        int empty = -1;
        bool there = false;
        for (int slot = 0; slot < ReactionWheelCount; ++slot) {
            const int at = static_cast<quint8>(data[ReactionWheel + slot * 2]);
            there = there || at == id;
            if (at == 0 && empty < 0) {
                empty = slot;
            }
        }
        if (!there && empty >= 0) {
            data[ReactionWheel + empty * 2] = static_cast<char>(id);
            data[ReactionWheel + empty * 2 + 1] = 0;
            ++wheeled;
        }
    }
    if (learned == 0) {
        return {};
    }
    return tr("Reactions: %1 learned (%2 put on the wheel).").arg(learned).arg(wheeled);
}

// --- Items ------------------------------------------------------------------------------------

PokopiaItemsTab::PokopiaItemsTab(QWidget* parent) : PokopiaSaveTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(Note(
        this, tr("Pick a slot, then choose an item and how many. Count 0 or \"(empty)\" clears the "
                 "slot. Stacks in real saves go up to 99; the save can hold up to 255.")));
    list = MakeTree(this, {tr("Slot"), tr("Item"), tr("Count")});
    list->setColumnWidth(0, 90);
    list->setColumnWidth(1, 260);
    layout->addWidget(list, 1);

    slot_label = new QLabel(this);
    item = new QComboBox(this);
    item->setEditable(true);
    item->setInsertPolicy(QComboBox::NoInsert);
    item->addItem(tr("(empty)"), 0);
    std::multimap<QString, int> sorted;
    for (int id = 1; id < static_cast<int>(Pikantine::PokopiaItemNames.size()); ++id) {
        if (Pikantine::PokopiaItemNames[id][0] != '\0') {
            sorted.emplace(QString::fromUtf8(Pikantine::PokopiaItemNames[id]).toLower(), id);
        }
    }
    for (const auto& [key, id] : sorted) {
        item->addItem(QStringLiteral("%1 (%2)")
                          .arg(QString::fromUtf8(Pikantine::PokopiaItemNames[id]))
                          .arg(id),
                      id);
    }
    item->completer()->setFilterMode(Qt::MatchContains);
    item->completer()->setCompletionMode(QCompleter::PopupCompletion);
    count = new QSpinBox(this);
    count->setRange(0, MaxStack);
    auto* set = new QPushButton(tr("Set slot"), this);
    auto* clear = new QPushButton(tr("Empty slot"), this);
    auto* add = new QPushButton(tr("Put in first empty slot"), this);
    auto* row = new QHBoxLayout();
    row->addWidget(slot_label);
    row->addWidget(item, 1);
    row->addWidget(count);
    row->addWidget(set);
    row->addWidget(clear);
    layout->addLayout(row);
    auto* row2 = new QHBoxLayout();
    row2->addWidget(add);
    row2->addStretch(1);
    layout->addLayout(row2);

    connect(list, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                if (current != nullptr) {
                    ShowSlot(current->data(0, IdRole).toInt());
                }
            });
    const auto picked = [this] {
        const int index = item->findText(item->currentText(), Qt::MatchFixedString);
        return index >= 0 ? item->itemData(index).toInt() : -1;
    };
    connect(item, &QComboBox::currentIndexChanged, this, [this](int) {
        if (item->currentData().toInt() != 0 && count->value() == 0) {
            count->setValue(1);
        }
    });
    connect(set, &QPushButton::clicked, this, [this, picked] {
        auto* current = list->currentItem();
        const int id = picked();
        if (current == nullptr || id < 0) {
            return;
        }
        SetSlot(current->data(0, IdRole).toInt(), id, count->value());
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (auto* current = list->currentItem(); current != nullptr) {
            SetSlot(current->data(0, IdRole).toInt(), 0, 0);
        }
    });
    connect(add, &QPushButton::clicked, this, [this, picked] {
        const int id = picked();
        if (id <= 0 || count->value() == 0) {
            return;
        }
        for (int slot = 0; slot < BagCount; ++slot) {
            if ((after[slot].raw & ItemIdMask) == 0) {
                SetSlot(slot, id, count->value());
                list->setCurrentItem(list->topLevelItem(slot));
                return;
            }
        }
        slot_label->setText(tr("The bag is full."));
    });
}

void PokopiaItemsTab::Load(const QByteArray& data) {
    before = QVector<Slot>(InventoryCount);
    for (int slot = 0; slot < InventoryCount; ++slot) {
        const char* at = data.constData() + InventoryOffset + slot * ItemSize;
        before[slot] = {qFromLittleEndian<quint32>(at), static_cast<quint8>(at[4])};
    }
    after = before;
    const QSignalBlocker blocker(list);
    list->clear();
    for (int slot = 0; slot < InventoryCount; ++slot) {
        auto* row = new QTreeWidgetItem(list);
        row->setText(0, slot < BagCount ? tr("Bag %1").arg(slot + 1)
                                        : tr("Bar %1").arg(slot - BagCount + 1));
        row->setData(0, IdRole, slot);
        Refresh(slot);
    }
    list->setCurrentItem(list->topLevelItem(0));
    ShowSlot(0);
}

void PokopiaItemsTab::ShowSlot(int slot) {
    if (slot < 0 || slot >= after.size()) {
        return;
    }
    slot_label->setText(slot < BagCount ? tr("Bag %1:").arg(slot + 1)
                                        : tr("Bar %1:").arg(slot - BagCount + 1));
    const int id = static_cast<int>(after[slot].raw & ItemIdMask);
    const int index = item->findData(id);
    const QSignalBlocker blocker(item);
    if (index >= 0) {
        item->setCurrentIndex(index);
    } else {
        item->setEditText(tr("Item %1").arg(id));
    }
    count->setValue(after[slot].count);
}

void PokopiaItemsTab::SetSlot(int slot, int id, int amount) {
    auto& s = after[slot];
    if (id == 0 || amount == 0) {
        s = {};
    } else {
        if (static_cast<int>(s.raw & ItemIdMask) != id) {
            // The other bits belong to the old item.
            s.raw = static_cast<quint32>(id);
        }
        s.count = amount;
    }
    if ((s.raw & ItemIdMask) == (before[slot].raw & ItemIdMask) && s.count == before[slot].count) {
        s = before[slot];
    }
    Refresh(slot);
    emit Changed();
}

void PokopiaItemsTab::Refresh(int slot) {
    auto* row = list->topLevelItem(slot);
    if (row == nullptr) {
        return;
    }
    const int id = static_cast<int>(after[slot].raw & ItemIdMask);
    if (id == 0) {
        row->setText(1, QString());
        row->setText(2, QString());
    } else {
        row->setText(1, id < static_cast<int>(Pikantine::PokopiaItemNames.size())
                            ? Name(Pikantine::PokopiaItemNames[id], id, tr("Item %1"))
                            : tr("Item %1").arg(id));
        row->setText(2, QString::number(after[slot].count));
    }
    Mark(row, after[slot].raw != before[slot].raw || after[slot].count != before[slot].count);
}

int PokopiaItemsTab::Pending() const {
    int n = 0;
    for (int slot = 0; slot < after.size(); ++slot) {
        n += after[slot].raw != before[slot].raw || after[slot].count != before[slot].count;
    }
    return n;
}

QString PokopiaItemsTab::Apply(QByteArray& data) const {
    int changed = 0;
    for (int slot = 0; slot < after.size(); ++slot) {
        if (after[slot].raw == before[slot].raw && after[slot].count == before[slot].count) {
            continue;
        }
        char* at = data.data() + InventoryOffset + slot * ItemSize;
        qToLittleEndian<quint32>(after[slot].raw, at);
        at[4] = static_cast<char>(after[slot].count);
        ++changed;
    }
    return changed == 0 ? QString() : tr("Bag: %1 slots changed.").arg(changed);
}

// --- Residents --------------------------------------------------------------------------------

namespace {

bool Lives(const QByteArray& data, int id) {
    const char* r = data.constData() + id * VillagerSize;
    return std::any_of(r + 1, r + 17, [](char c) { return c != 0; });
}

QString ResidentName(int id) {
    return Name(Pikantine::PokopiaVillagerNames[id], id, QObject::tr("Resident %1"));
}

} // namespace

PokopiaVillagersTab::PokopiaVillagersTab(QWidget* parent) : PokopiaSaveTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(Note(this, tr("Your residents. Pick one to change its friendship or whether "
                                    "its naming chat is done.")));
    list = MakeTree(this, {tr("Resident"), tr("Id"), tr("Area"), tr("Home"), tr("Friendship")});
    list->setColumnWidth(0, 180);
    list->setColumnWidth(1, 50);
    list->setColumnWidth(2, 140);
    layout->addWidget(list, 1);

    friendship = new QSpinBox(this);
    friendship->setRange(0, FriendshipMax);
    named = new QCheckBox(tr("Naming chat done"), this);
    auto* edit_row = new QHBoxLayout();
    edit_row->addWidget(new QLabel(tr("Friendship:"), this));
    edit_row->addWidget(friendship);
    edit_row->addWidget(named);
    edit_row->addStretch(1);
    layout->addLayout(edit_row);

    add_note = Note(this, tr("<b>Experimental:</b> adds a resident the way the game's own records "
                             "look, with no home yet, in the area you pick. Untested in game; "
                             "try it on a copy of your save first."));
    layout->addWidget(add_note);
    add_who = new QComboBox(this);
    add_who->setEditable(true);
    add_who->setInsertPolicy(QComboBox::NoInsert);
    add_who->completer()->setFilterMode(Qt::MatchContains);
    add_who->completer()->setCompletionMode(QCompleter::PopupCompletion);
    add_area = new QComboBox(this);
    for (int area = 1; area < static_cast<int>(Pikantine::PokopiaAreaTitles.size()); ++area) {
        if (Pikantine::PokopiaAreaTitles[area][0] != '\0') {
            add_area->addItem(QStringLiteral("%1 (%2)").arg(AreaName(area)).arg(area), area);
        }
    }
    add_button = new QPushButton(tr("Add resident"), this);
    auto* add_row = new QHBoxLayout();
    add_row->addWidget(add_who, 1);
    add_row->addWidget(add_area);
    add_row->addWidget(add_button);
    layout->addLayout(add_row);

    connect(list, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { ShowResident(); });
    const auto edited = [this] {
        auto* row = list->currentItem();
        if (row == nullptr) {
            return;
        }
        const int id = row->data(0, IdRole).toInt();
        if (!after.contains(id)) {
            return; // a new arrival: nothing to edit yet
        }
        after[id] = {friendship->value(), named->isChecked()};
        row->setText(4, QString::number(friendship->value()));
        Mark(row, after[id].friendship != before[id].friendship ||
                      after[id].named != before[id].named);
        emit Changed();
    };
    connect(friendship, &QSpinBox::valueChanged, this, edited);
    connect(named, &QCheckBox::toggled, this, edited);
    connect(add_button, &QPushButton::clicked, this, [this] {
        const int index = add_who->findText(add_who->currentText(), Qt::MatchFixedString);
        if (index < 0 || owner.isEmpty()) {
            return;
        }
        const int id = add_who->itemData(index).toInt();
        const int area = add_area->currentData().toInt();
        arrivals.push_back({id, area});
        auto* row = new QTreeWidgetItem(list);
        row->setText(0, ResidentName(id));
        row->setText(1, QString::number(id));
        row->setText(2, AreaName(area));
        row->setText(3, tr("none yet"));
        row->setText(4, QStringLiteral("0"));
        row->setData(0, IdRole, id);
        Mark(row, true);
        FillAddList();
        emit Changed();
    });
}

void PokopiaVillagersTab::Load(const QByteArray& data) {
    before.clear();
    arrivals.clear();
    owner.clear();
    const QSignalBlocker blocker(list);
    list->clear();
    QHash<int, int> area_count;
    for (int id = 0; id < VillagerCount; ++id) {
        if (!Lives(data, id)) {
            continue;
        }
        const char* r = data.constData() + id * VillagerSize;
        const char* home = data.constData() + HomeOffset + id * HomeSize;
        if (owner.isEmpty()) {
            owner = QByteArray(r + 1, 16);
        }
        const int area = static_cast<quint8>(home[3]);
        ++area_count[area];
        Edit e{qFromLittleEndian<quint16>(r + 0x11), r[0xC4] != 0};
        before[id] = e;
        auto* row = new QTreeWidgetItem(list);
        row->setText(0, ResidentName(id));
        row->setText(1, QString::number(id));
        row->setText(2, AreaName(area));
        row->setText(3, home[0] == 0 ? tr("none")
                                     : tr("#%1").arg(qFromLittleEndian<quint16>(home + 1)));
        row->setText(4, QString::number(e.friendship));
        row->setData(0, IdRole, id);
    }
    after = before;
    list->sortItems(0, Qt::AscendingOrder);
    // Default to the area most residents live in.
    int best = 0;
    for (auto it = area_count.cbegin(); it != area_count.cend(); ++it) {
        if (it.key() != 0 && it.value() > area_count.value(best)) {
            best = it.key();
        }
    }
    if (const int index = add_area->findData(best); index >= 0) {
        add_area->setCurrentIndex(index);
    }
    add_button->setEnabled(!owner.isEmpty());
    FillAddList();
    ShowResident();
}

void PokopiaVillagersTab::FillAddList() {
    QSet<int> taken;
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        taken.insert(it.key());
    }
    for (const auto& a : arrivals) {
        taken.insert(a.id);
    }
    std::multimap<QString, int> sorted;
    for (int id = 1; id < VillagerCount; ++id) {
        if (!taken.contains(id) && Pikantine::PokopiaVillagerNames[id][0] != '\0') {
            sorted.emplace(QString::fromUtf8(Pikantine::PokopiaVillagerNames[id]), id);
        }
    }
    add_who->clear();
    for (const auto& [name, id] : sorted) {
        add_who->addItem(QStringLiteral("%1 (%2)").arg(name).arg(id), id);
    }
}

void PokopiaVillagersTab::ShowResident() {
    auto* row = list->currentItem();
    const int id = row != nullptr ? row->data(0, IdRole).toInt() : -1;
    const bool editable = after.contains(id);
    friendship->setEnabled(editable);
    named->setEnabled(editable);
    if (!editable) {
        return;
    }
    const QSignalBlocker b1(friendship);
    const QSignalBlocker b2(named);
    friendship->setMaximum(std::max(FriendshipMax, after[id].friendship));
    friendship->setValue(after[id].friendship);
    named->setChecked(after[id].named);
}

int PokopiaVillagersTab::Pending() const {
    int n = static_cast<int>(arrivals.size());
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto& b = before[it.key()];
        n += it->friendship != b.friendship || it->named != b.named;
    }
    return n;
}

QString PokopiaVillagersTab::Apply(QByteArray& data) const {
    int edited = 0;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto& b = before[it.key()];
        if (it->friendship == b.friendship && it->named == b.named) {
            continue;
        }
        char* r = data.data() + it.key() * VillagerSize;
        qToLittleEndian<quint16>(static_cast<quint16>(it->friendship), r + 0x11);
        r[0xC4] = it->named ? 1 : 0;
        ++edited;
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (const auto& a : arrivals) {
        char* r = data.data() + a.id * VillagerSize;
        std::fill_n(r, VillagerSize, '\0');
        std::copy(owner.cbegin(), owner.cend(), r + 1);
        std::copy(owner.cbegin(), owner.cend(), r + 0x22);
        r[0x15] = 1;                                // as in every record the game made
        qToLittleEndian<float>(-1.0f, r + 0x17);    // ditto
        qToLittleEndian<quint32>(QRandomGenerator::global()->generate(), r + 0x1B);
        r[0xC2] = 1;
        r[0xC5] = 1;
        const quint16 when[6] = {
            static_cast<quint16>(now.date().year()), static_cast<quint16>(now.date().month()),
            static_cast<quint16>(now.date().day()),  static_cast<quint16>(now.time().hour()),
            static_cast<quint16>(now.time().minute()), static_cast<quint16>(now.time().second())};
        for (int i = 0; i < 6; ++i) {
            qToLittleEndian<quint16>(when[i], r + 0x16A + i * 2);
        }
        char* home = data.data() + HomeOffset + a.id * HomeSize;
        std::fill_n(home, HomeSize, '\0');
        home[3] = static_cast<char>(a.area);
        qToLittleEndian<float>(-1.0f, home + 0x0E); // as in the game's homeless records
    }
    if (edited == 0 && arrivals.isEmpty()) {
        return {};
    }
    QString line = tr("Residents: %1 changed").arg(edited);
    if (!arrivals.isEmpty()) {
        line += tr(", %1 added (experimental)").arg(arrivals.size());
    }
    return line + QLatin1Char('.');
}

// --- Flags ------------------------------------------------------------------------------------

PokopiaFlagsTab::PokopiaFlagsTab(const QString& labels_path_, QWidget* parent)
    : PokopiaSaveTab(parent), labels_path{labels_path_} {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(Note(
        this, tr("The game unlocks features with 250 numbered flags. It ships without their "
                 "names, so only a few are known; the notes say what the game does with each. "
                 "Double-click <b>Your label</b> to name one as you find out (kept between saves). "
                 "Turning story flags on or off can break your progress: change one at a time and "
                 "keep the backup.")));
    search = new QLineEdit(this);
    search->setPlaceholderText(tr("Search number, name, note or label"));
    search->setClearButtonEnabled(true);
    used_only = new QCheckBox(tr("Hide the spare flags (160-249)"), this);
    used_only->setChecked(true);
    auto* row = new QHBoxLayout();
    row->addWidget(search, 1);
    row->addWidget(used_only);
    layout->addLayout(row);
    list = MakeTree(this, {tr("Flag"), tr("Known as"), tr("Notes"), tr("Your label")});
    list->setColumnWidth(0, 70);
    list->setColumnWidth(1, 190);
    list->setColumnWidth(2, 300);
    layout->addWidget(list, 1);

    QJsonObject labels;
    if (QFile f(labels_path); f.open(QIODevice::ReadOnly)) {
        labels = QJsonDocument::fromJson(f.readAll()).object();
    }
    for (int id = 1; id < FlagCount; ++id) {
        auto* item = new QTreeWidgetItem(list);
        item->setText(0, QString::number(id));
        item->setData(0, IdRole, id);
        for (const auto& known : Pikantine::PokopiaKnownFlags) {
            if (known.id == id) {
                item->setText(1, QString::fromUtf8(known.name));
            }
        }
        QStringList notes;
        const unsigned settings = Pikantine::FlagSettings(id);
        if (settings & 0x10000) {
            notes << tr("always on");
        }
        if (settings & 0x40000) {
            notes << tr("host's in multiplayer");
        }
        if (settings & 0x20000) {
            notes << tr("also on in game mode 17");
        }
        if (const int checks = Pikantine::FlagChecks(id); checks > 0) {
            notes << tr("checked in %n place(s)", nullptr, checks);
        }
        if (Pikantine::FlagIsLaterStory(id)) {
            notes << tr("later story (after Surf)");
        }
        if (id >= FirstSpareFlag) {
            notes << tr("spare");
        }
        item->setText(2, notes.join(QStringLiteral("; ")));
        item->setToolTip(2, item->text(2));
        item->setText(3, labels.value(QString::number(id)).toString());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);
    }

    connect(list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == 3) {
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            list->editItem(item, 3);
        }
    });
    connect(list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (loading) {
            return;
        }
        if (column == 3) {
            SaveLabels();
            return;
        }
        if (column == 0) {
            const int id = item->data(0, IdRole).toInt();
            loading = true; // Mark() repaints every column, which reports them all as changed
            Mark(item, Ticked(item) != before[id]);
            loading = false;
            emit Changed();
        }
    });
    connect(search, &QLineEdit::textChanged, this, &PokopiaFlagsTab::Refilter);
    connect(used_only, &QCheckBox::toggled, this, &PokopiaFlagsTab::Refilter);
    Refilter();
}

void PokopiaFlagsTab::Load(const QByteArray& data) {
    loading = true;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        const int id = item->data(0, IdRole).toInt();
        before[id] = Flag(data, id);
        item->setCheckState(0, before[id] ? Qt::Checked : Qt::Unchecked);
        Mark(item, false);
    }
    loading = false;
    Refilter();
}

void PokopiaFlagsTab::Refilter() {
    const QString text = search->text();
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        const int id = item->data(0, IdRole).toInt();
        bool show = !(used_only->isChecked() && id >= FirstSpareFlag && !before[id]);
        if (show && !text.isEmpty()) {
            show = false;
            for (int c = 0; c < item->columnCount() && !show; ++c) {
                show = item->text(c).contains(text, Qt::CaseInsensitive);
            }
        }
        item->setHidden(!show);
    }
}

void PokopiaFlagsTab::SaveLabels() {
    QJsonObject labels;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const auto* item = list->topLevelItem(i);
        if (!item->text(3).trimmed().isEmpty()) {
            labels.insert(item->text(0), item->text(3).trimmed());
        }
    }
    if (QFile f(labels_path); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(labels).toJson());
    }
}

int PokopiaFlagsTab::Pending() const {
    int n = 0;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const auto* item = list->topLevelItem(i);
        n += Ticked(item) != before[item->data(0, IdRole).toInt()];
    }
    return n;
}

QString PokopiaFlagsTab::Apply(QByteArray& data) const {
    int on = 0, off = 0;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        const auto* item = list->topLevelItem(i);
        const int id = item->data(0, IdRole).toInt();
        const bool want = Ticked(item);
        if (want == before[id]) {
            continue;
        }
        SetFlag(data, id, want);
        ++(want ? on : off);
    }
    return on + off == 0 ? QString() : tr("Flags: %1 turned on, %2 off.").arg(on).arg(off);
}
