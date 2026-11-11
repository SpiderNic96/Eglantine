// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/pokopia_dex_dialog.h"

#include <algorithm>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtEndian>

#include "common/fs/path_util.h"
#include "yuzu/configuration/pokopia_dex_data.h"

namespace {

constexpr const char* TitleFolder = "0400A3A01EE4C000";
constexpr quint32 Magic = 0x31227687;
constexpr quint32 Layout201 = 0x0200017D; // 2.0.1's common save
constexpr quint32 Format = 0x40001;
constexpr int HeaderSize = 0x130;
constexpr int DataSize = 0x470CED;
// Field offsets in the unpacked data, from the game's save serializer (main+0x5ca320); see
// docs/pokecap/save-layout.md.
constexpr int DexOffset = 0x426EFF;      // m_pokemonDexHistory: a byte per Pokédex slot
constexpr int DexCount = 0x400;
constexpr int HabitatOffset = 0x4256FF;  // m_habitatHistory: 12 bytes per habitat id
constexpr int HabitatCount = 0x1800 / 12;
// m_pokemonHistory: a record per species row (#1066), 129 bytes: the habitats it has been seen in
// (512 bits), the same again, then a flag byte. A habitat page shows "?" for a Pokémon whose
// record lacks that habitat.
constexpr int HistoryOffset = 0x4052FF;
constexpr int HistoryRecord = 129;
constexpr int HistoryCount = 0x20400 / HistoryRecord;
constexpr quint8 Registered = 0x03;      // bits 0 and 1
constexpr quint8 Seen = 0x08;            // bit 3: seen only, a silhouette
constexpr int IdRole = Qt::UserRole;
constexpr int StateRole = Qt::UserRole + 1;

int DexStateOf(quint8 flags) {
    return (flags & Registered) == Registered ? 2 : (flags & Seen) != 0 ? 1 : 0;
}

int HabitatStateOf(quint32 state) {
    return state == 3 ? 2 : state != 0 ? 1 : 0;
}

Qt::CheckState CheckOf(int state) {
    return state == 2 ? Qt::Checked : state == 1 ? Qt::PartiallyChecked : Qt::Unchecked;
}

int StateOf(const QTreeWidgetItem* item) {
    const auto check = item->checkState(0);
    return check == Qt::Checked ? 2 : check == Qt::PartiallyChecked ? 1 : 0;
}

QString HabitatName(int id) {
    for (const auto& h : Pikantine::PokopiaHabitats) {
        if (h.id == id) {
            return QString::fromUtf8(h.name);
        }
    }
    return QObject::tr("Habitat #%1").arg(id);
}

constexpr int AreasRole = Qt::UserRole + 2;
constexpr u8 MainAreas = 0x3f; // every area but Bubbly Basin

QString AreaText(u8 areas) {
    if ((areas & MainAreas) == MainAreas) {
        return QObject::tr("all areas");
    }
    QStringList names;
    for (int i = 0; i < static_cast<int>(Pikantine::PokopiaAreaNames.size()); ++i) {
        if (areas & (1 << i)) {
            names << QString::fromUtf8(Pikantine::PokopiaAreaNames[i]);
        }
    }
    return names.join(QStringLiteral(", "));
}

QString SpawnText(const Pikantine::Spawn& s, const QString& what) {
    QString text = QStringLiteral("%1 (%2").arg(what, AreaText(s.areas));
    if (s.rare) {
        text += QObject::tr(", rare");
    }
    return text + QLatin1Char(')');
}

/// Shows the rows matching the text and, when `area` is not 0, able to appear in that area.
void Filter(QTreeWidget* list, const QString& text, int area = 0) {
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        auto* item = list->topLevelItem(i);
        if (area != 0 && (item->data(0, AreasRole).toInt() & area) == 0) {
            item->setHidden(true);
            continue;
        }
        bool match = text.isEmpty();
        for (int c = 0; !match && c < list->columnCount(); ++c) {
            match = item->text(c).contains(text, Qt::CaseInsensitive);
        }
        item->setHidden(!match);
    }
}

QTreeWidget* MakeList(QWidget* parent, const QStringList& headers) {
    auto* list = new QTreeWidget(parent);
    list->setHeaderLabels(headers);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setColumnWidth(0, 230);
    list->setColumnWidth(1, 50);
    return list;
}

QString CommonPath(const QString& folder) {
    return QDir(folder).filePath(QStringLiteral("G00/CMNDAT.BIN"));
}

quint32 Le32(const QByteArray& bytes, int offset) {
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}

/// The save's data, or an empty array with `error` set.
QByteArray Unpack(const QByteArray& file, QString* error) {
    if (file.size() < HeaderSize || Le32(file, 0) != Magic) {
        *error = QObject::tr("This is not a Pokopia common save (G00/CMNDAT.BIN).");
        return {};
    }
    if (Le32(file, 4) != Layout201 || Le32(file, 8) != Format) {
        *error = QObject::tr("This save is not from Pokopia 2.0.1; only 2.0.1 saves can be edited.");
        return {};
    }
    const int length = static_cast<int>(Le32(file, 0x10));
    if (length <= HeaderSize || length > file.size()) {
        *error = QObject::tr("The save's length is invalid.");
        return {};
    }
    // qUncompress wants zlib data after the unpacked size (4 bytes, big-endian).
    QByteArray packed(4, '\0');
    qToBigEndian<quint32>(DataSize, packed.data());
    packed.append(file.constData() + HeaderSize, length - HeaderSize);
    QByteArray data = qUncompress(packed);
    if (data.size() != DataSize) {
        *error = QObject::tr("The save could not be unpacked.");
        return {};
    }
    return data;
}

QByteArray Pack(const QByteArray& original, const QByteArray& data) {
    const QByteArray packed = qCompress(data, 9).mid(4); // drop qCompress's size prefix
    const int length = HeaderSize + static_cast<int>(packed.size());
    QByteArray file(std::max<qsizetype>(length, original.size()), '\0');
    std::copy_n(original.constData(), HeaderSize, file.data());
    std::copy(packed.cbegin(), packed.cend(), file.data() + HeaderSize);
    qToLittleEndian<quint32>(static_cast<quint32>(length), file.data() + 0x10);
    return file;
}

bool CopyFolder(const QString& from, const QString& to) {
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    const QDir source(from);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString target = QDir(to).filePath(source.relativeFilePath(path));
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(path, target)) {
            return false;
        }
    }
    return true;
}

} // namespace

PokopiaDexDialog::PokopiaDexDialog(bool game_running_, QWidget* parent)
    : QDialog(parent), game_running{game_running_} {
    setWindowTitle(tr("Pikantine: Pokopia Pokédex and habitats"));
    resize(760, 700);
    auto* layout = new QVBoxLayout(this);

    auto* note = new QLabel(
        tr("Filter by area to see what can appear on each island. Click a box to cycle it: empty = not yet, <b>half</b> = seen (Pokédex) or discovered "
           "(habitat), <b>ticked</b> = registered or built. Changes show "
           "<span style='color:#2e7d32'>green</span>; nothing is written until <b>Save</b>, and "
           "your save folder is backed up first. Dex rewards (recipes) are left for the game to "
           "hand out. For Pokopia <b>2.0.1</b> saves; keep your own backup too."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* row = new QHBoxLayout();
    folders = new QComboBox(this);
    auto* browse = new QPushButton(tr("Browse…"), this);
    row->addWidget(folders, 1);
    row->addWidget(browse);
    layout->addLayout(row);

    status = new QLabel(this);
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* tabs = new QTabWidget(this);
    auto* dex_page = new QWidget(tabs);
    auto* dex_layout = new QVBoxLayout(dex_page);
    dex_search = new QLineEdit(dex_page);
    dex_search->setPlaceholderText(tr("Search Pokémon or habitat"));
    dex_search->setClearButtonEnabled(true);
    dex_area = new QComboBox(dex_page);
    dex_area->addItem(tr("Any area"), 0);
    for (int i = 0; i < static_cast<int>(Pikantine::PokopiaAreaNames.size()); ++i) {
        dex_area->addItem(QString::fromUtf8(Pikantine::PokopiaAreaNames[i]), 1 << i);
    }
    dex_list = MakeList(dex_page, {tr("Pokémon"), tr("Slot"), tr("Appears in (habitat, areas)")});
    auto* dex_filters = new QHBoxLayout();
    dex_filters->addWidget(dex_search, 1);
    dex_filters->addWidget(dex_area);
    dex_layout->addLayout(dex_filters);
    dex_layout->addWidget(dex_list, 1);
    tabs->addTab(dex_page, tr("Pokédex"));

    auto* habitat_page = new QWidget(tabs);
    auto* habitat_layout = new QVBoxLayout(habitat_page);
    habitat_search = new QLineEdit(habitat_page);
    habitat_search->setPlaceholderText(tr("Search habitat or Pokémon"));
    habitat_search->setClearButtonEnabled(true);
    habitat_area = new QComboBox(habitat_page);
    for (int i = 0; i < dex_area->count(); ++i) {
        habitat_area->addItem(dex_area->itemText(i), dex_area->itemData(i));
    }
    habitat_list = MakeList(habitat_page, {tr("Habitat"), tr("Id"), tr("Draws (Pokémon, areas)")});
    auto* habitat_filters = new QHBoxLayout();
    habitat_filters->addWidget(habitat_search, 1);
    habitat_filters->addWidget(habitat_area);
    habitat_layout->addLayout(habitat_filters);
    habitat_layout->addWidget(habitat_list, 1);
    tabs->addTab(habitat_page, tr("Habitats"));
    layout->addWidget(tabs, 1);

    link_habitats = new QCheckBox(
        tr("When a Pokémon gets registered, also mark its habitats discovered"), this);
    link_habitats->setChecked(true);
    layout->addWidget(link_habitats);
    fill_sightings = new QCheckBox(
        tr("Mark each registered Pokémon as seen in all its habitats (fills the \"?\" on habitat "
           "pages)"),
        this);
    fill_sightings->setChecked(true);
    layout->addWidget(fill_sightings);

    auto* actions = new QHBoxLayout();
    auto* unlock_all = new QPushButton(tr("Unlock everything"), this);
    unlock_all->setToolTip(tr("Every Pokémon registered and every habitat built."));
    auto* tick_shown = new QPushButton(tr("Tick the ones shown"), this);
    tick_shown->setToolTip(tr("Registers or builds every entry the search shows, on this tab."));
    auto* reset = new QPushButton(tr("Back to the save"), this);
    actions->addWidget(unlock_all);
    actions->addWidget(tick_shown);
    actions->addWidget(reset);
    actions->addStretch(1);
    layout->addLayout(actions);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    save_button = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    if (game_running) {
        save_button->setEnabled(false);
        save_button->setToolTip(tr("Stop the game first: it would write its own save over this."));
    }

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString picked = QFileDialog::getExistingDirectory(
            this, tr("The Pokopia save folder (the one holding G00)"), folder);
        if (!picked.isEmpty()) {
            folders->addItem(picked, picked);
            folders->setCurrentIndex(folders->count() - 1);
        }
    });
    connect(folders, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) {
            LoadSave(folders->itemData(index).toString());
        }
    });
    connect(dex_search, &QLineEdit::textChanged, this, &PokopiaDexDialog::Refilter);
    connect(habitat_search, &QLineEdit::textChanged, this, &PokopiaDexDialog::Refilter);
    connect(dex_area, &QComboBox::currentIndexChanged, this, &PokopiaDexDialog::Refilter);
    connect(habitat_area, &QComboBox::currentIndexChanged, this, &PokopiaDexDialog::Refilter);
    const auto changed = [this](QTreeWidgetItem* item, int column) {
        if (populating || column != 0) {
            return;
        }
        const bool fresh = StateOf(item) != item->data(0, StateRole).toInt();
        const QBrush brush = fresh ? QBrush(QColor(0x2e, 0x7d, 0x32)) : QBrush();
        for (int c = 0; c < item->columnCount(); ++c) {
            item->setForeground(c, brush);
        }
        UpdateCount();
    };
    connect(dex_list, &QTreeWidget::itemChanged, this, changed);
    connect(habitat_list, &QTreeWidget::itemChanged, this, changed);
    connect(unlock_all, &QPushButton::clicked, this, [this] {
        for (auto* list : {dex_list, habitat_list}) {
            for (int i = 0; i < list->topLevelItemCount(); ++i) {
                list->topLevelItem(i)->setCheckState(0, Qt::Checked);
            }
        }
    });
    connect(tick_shown, &QPushButton::clicked, this, [this, tabs] {
        auto* list = tabs->currentIndex() == 0 ? dex_list : habitat_list;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            if (!list->topLevelItem(i)->isHidden()) {
                list->topLevelItem(i)->setCheckState(0, Qt::Checked);
            }
        }
    });
    connect(reset, &QPushButton::clicked, this, &PokopiaDexDialog::Populate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(save_button, &QPushButton::clicked, this, &PokopiaDexDialog::Save);

    FindSaves();
}

PokopiaDexDialog::~PokopiaDexDialog() = default;

void PokopiaDexDialog::Refilter() {
    Filter(dex_list, dex_search->text(), dex_area->currentData().toInt());
    Filter(habitat_list, habitat_search->text(), habitat_area->currentData().toInt());
}

void PokopiaDexDialog::FindSaves() {
    const QString root = QString::fromStdString(
        Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir)));
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (it.fileName().compare(QLatin1String(TitleFolder), Qt::CaseInsensitive) == 0 &&
            QFile::exists(CommonPath(path))) {
            folders->addItem(QDir::toNativeSeparators(path), path);
        }
    }
    if (folders->count() == 0) {
        status->setText(tr("No Pokopia save found; use Browse to pick its folder (the one "
                           "holding G00)."));
        save_button->setEnabled(false);
    }
}

void PokopiaDexDialog::LoadSave(const QString& path) {
    folder = path;
    dex_list->clear();
    habitat_list->clear();
    dex_state.clear();
    habitat_state.clear();
    data.clear();
    QFile in(CommonPath(path));
    if (!in.open(QIODevice::ReadOnly)) {
        status->setText(tr("Could not open %1.").arg(QDir::toNativeSeparators(in.fileName())));
        save_button->setEnabled(false);
        return;
    }
    file = in.readAll();
    QString error;
    data = Unpack(file, &error);
    if (data.isEmpty()) {
        status->setText(error);
        save_button->setEnabled(false);
        return;
    }
    for (int slot = 0; slot < DexCount; ++slot) {
        dex_state[slot] = DexStateOf(static_cast<quint8>(data[DexOffset + slot]));
    }
    for (int id = 0; id < HabitatCount; ++id) {
        habitat_state[id] = HabitatStateOf(Le32(data, HabitatOffset + id * 12));
    }
    save_button->setEnabled(!game_running);
    Populate();
}

void PokopiaDexDialog::Populate() {
    if (data.isEmpty()) {
        return;
    }
    populating = true;
    dex_list->clear();
    habitat_list->clear();

    QHash<int, QStringList> draws; // habitat id -> Pokémon that appear in it
    QHash<int, int> habitat_areas; // habitat id -> areas any of them appear in
    QSet<int> named_slots;
    for (const auto& p : Pikantine::PokopiaDexSlots) {
        named_slots.insert(p.slot);
        QStringList where;
        int areas = 0;
        for (const auto& spawn : p.spawns) {
            if (spawn.habitat == 0) {
                continue;
            }
            where << SpawnText(spawn, HabitatName(spawn.habitat));
            draws[spawn.habitat] << SpawnText(spawn, QString::fromUtf8(p.name));
            habitat_areas[spawn.habitat] |= spawn.areas;
            areas |= spawn.areas;
        }
        auto* item = new QTreeWidgetItem(dex_list);
        item->setText(0, QString::fromUtf8(p.name));
        item->setText(1, QString::number(p.slot));
        item->setText(2, where.isEmpty() ? tr("(special: story, event or not listed yet)")
                                         : where.join(QStringLiteral("; ")));
        item->setToolTip(2, item->text(2));
        item->setData(0, IdRole, p.slot);
        item->setData(0, AreasRole, areas);
    }
    // Slots the save uses that the table does not name still show, by number.
    for (int slot = 1; slot < DexCount; ++slot) {
        if (dex_state.value(slot) != 0 && !named_slots.contains(slot)) {
            auto* item = new QTreeWidgetItem(dex_list);
            item->setText(0, tr("Slot %1").arg(slot));
            item->setText(1, QString::number(slot));
            item->setData(0, IdRole, slot);
        }
    }

    QSet<int> named_habitats;
    for (const auto& h : Pikantine::PokopiaHabitats) {
        named_habitats.insert(h.id);
        auto* item = new QTreeWidgetItem(habitat_list);
        item->setText(0, QString::fromUtf8(h.name));
        item->setText(1, QString::number(h.id));
        item->setText(2, draws.value(h.id).join(QStringLiteral("; ")));
        item->setToolTip(2, item->text(2));
        item->setData(0, IdRole, static_cast<int>(h.id));
        item->setData(0, AreasRole, habitat_areas.value(h.id));
    }
    for (int id = 1; id < HabitatCount; ++id) {
        if (habitat_state.value(id) != 0 && !named_habitats.contains(id)) {
            auto* item = new QTreeWidgetItem(habitat_list);
            item->setText(0, HabitatName(id));
            item->setText(1, QString::number(id));
            item->setData(0, IdRole, id);
        }
    }

    for (auto* list : {dex_list, habitat_list}) {
        const auto& states = list == dex_list ? dex_state : habitat_state;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto* item = list->topLevelItem(i);
            const int state = states.value(item->data(0, IdRole).toInt());
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsUserTristate);
            item->setData(0, StateRole, state);
            item->setCheckState(0, CheckOf(state));
        }
    }
    populating = false;
    Refilter();
    UpdateCount();
}

void PokopiaDexDialog::UpdateCount() {
    if (data.isEmpty()) {
        return;
    }
    const auto count = [](QTreeWidget* list, int* full, int* half, int* changed) {
        *full = *half = *changed = 0;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            const auto* item = list->topLevelItem(i);
            const int state = StateOf(item);
            *full += state == 2 ? 1 : 0;
            *half += state == 1 ? 1 : 0;
            *changed += state != item->data(0, StateRole).toInt() ? 1 : 0;
        }
    };
    int registered, seen, dex_changed, built, discovered, habitat_changed;
    count(dex_list, &registered, &seen, &dex_changed);
    count(habitat_list, &built, &discovered, &habitat_changed);
    status->setText(tr("Pokédex: %1 registered, %2 seen (of %3). Habitats: %4 built, %5 "
                       "discovered (of %6). Changes not saved yet: %7.")
                        .arg(registered)
                        .arg(seen)
                        .arg(dex_list->topLevelItemCount())
                        .arg(built)
                        .arg(discovered)
                        .arg(habitat_list->topLevelItemCount())
                        .arg(dex_changed + habitat_changed));
}

void PokopiaDexDialog::Save() {
    if (data.isEmpty() || game_running) {
        return;
    }
    QByteArray edited = data;
    QHash<int, int> habitats; // id -> wanted state
    for (int i = 0; i < habitat_list->topLevelItemCount(); ++i) {
        const auto* item = habitat_list->topLevelItem(i);
        habitats[item->data(0, IdRole).toInt()] = StateOf(item);
    }

    int registered = 0, seen = 0, cleared = 0, linked = 0;
    for (int i = 0; i < dex_list->topLevelItemCount(); ++i) {
        const auto* item = dex_list->topLevelItem(i);
        const int slot = item->data(0, IdRole).toInt();
        const int want = StateOf(item);
        if (slot <= 0 || slot >= DexCount) {
            continue;
        }
        if (want == 2 && dex_state.value(slot) != 2 && link_habitats->isChecked()) {
            for (const auto& p : Pikantine::PokopiaDexSlots) {
                if (p.slot != slot) {
                    continue;
                }
                for (const auto& spawn : p.spawns) {
                    const int h = spawn.habitat;
                    if (h != 0 && habitats.value(h) == 0) {
                        habitats[h] = 1;
                        ++linked;
                    }
                }
            }
        }
        if (want == dex_state.value(slot)) {
            continue;
        }
        auto& flags = reinterpret_cast<quint8&>(edited[DexOffset + slot]);
        if (want == 2) {
            flags |= Registered;
            ++registered;
        } else if (want == 1) {
            flags = static_cast<quint8>((flags & ~Registered) | Seen);
            ++seen;
        } else {
            flags = 0;
            ++cleared;
        }
    }

    // Sightings: every Pokémon registered after this save, seen in each habitat it appears in.
    int sighted = 0;
    if (fill_sightings->isChecked()) {
        QHash<int, int> wanted; // slot -> wanted state
        for (int i = 0; i < dex_list->topLevelItemCount(); ++i) {
            const auto* item = dex_list->topLevelItem(i);
            wanted[item->data(0, IdRole).toInt()] = StateOf(item);
        }
        for (const auto& p : Pikantine::PokopiaDexSlots) {
            if (wanted.value(p.slot) != 2 || p.row >= HistoryCount) {
                continue;
            }
            char* record = edited.data() + HistoryOffset + p.row * HistoryRecord;
            bool any = false;
            for (const auto& spawn : p.spawns) {
                if (spawn.habitat == 0 || spawn.habitat >= 512) {
                    continue;
                }
                const int byte = spawn.habitat / 8;
                const char bit = static_cast<char>(1 << (spawn.habitat % 8));
                for (const int copy : {0, 64}) {
                    if ((record[copy + byte] & bit) == 0) {
                        record[copy + byte] = static_cast<char>(record[copy + byte] | bit);
                        any = true;
                    }
                }
            }
            sighted += any ? 1 : 0;
        }
    }

    int built = 0, discovered = 0, removed = 0;
    for (auto it = habitats.cbegin(); it != habitats.cend(); ++it) {
        const int id = it.key();
        if (id <= 0 || id >= HabitatCount || it.value() == habitat_state.value(id)) {
            continue;
        }
        char* record = edited.data() + HabitatOffset + id * 12;
        if (it.value() == 2) {
            qToLittleEndian<quint32>(3, record);
            ++built;
        } else if (it.value() == 1) {
            qToLittleEndian<quint32>(1, record);
            ++discovered;
        } else {
            std::fill_n(record, 12, '\0');
            ++removed;
        }
    }

    if (edited == data) {
        QMessageBox::information(this, windowTitle(), tr("Nothing to change."));
        return;
    }
    QString summary = tr("Pokédex: %1 registered, %2 set to seen, %3 cleared.\nHabitats: %4 "
                         "built, %5 discovered, %6 cleared.")
                          .arg(registered)
                          .arg(seen)
                          .arg(cleared)
                          .arg(built)
                          .arg(discovered)
                          .arg(removed);
    if (sighted > 0) {
        summary += tr("\nSightings: %1 Pokémon marked as seen in more of their habitats.")
                       .arg(sighted);
    }
    if (linked > 0) {
        summary += tr("\n(%1 of those habitats come from the Pokémon you registered.)").arg(linked);
    }
    if (QMessageBox::question(this, windowTitle(),
                              tr("%1\n\nWrite this to the save? The save folder is backed up "
                                 "first.")
                                  .arg(summary)) != QMessageBox::Yes) {
        return;
    }

    const QString backups = QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "pikantine_backups"));
    // A new folder for every save, even two in the same second.
    const QString stamp = QStringLiteral("%1-%2").arg(
        QLatin1String(TitleFolder),
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString backup = QDir(backups).filePath(stamp);
    for (int n = 2; QFileInfo::exists(backup); ++n) {
        backup = QDir(backups).filePath(QStringLiteral("%1-%2").arg(stamp).arg(n));
    }
    if (!CopyFolder(folder, backup)) {
        QMessageBox::critical(this, windowTitle(),
                              tr("Could not back the save up to %1; nothing was changed.")
                                  .arg(QDir::toNativeSeparators(backup)));
        return;
    }

    const QByteArray out = Pack(file, edited);
    QString error;
    if (Unpack(out, &error) != edited) {
        QMessageBox::critical(this, windowTitle(),
                              tr("The edited save did not read back the same; nothing was "
                                 "changed."));
        return;
    }
    QFile target(CommonPath(folder));
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        target.write(out) != out.size()) {
        QMessageBox::critical(this, windowTitle(),
                              tr("Could not write the save. Your backup is in %1.")
                                  .arg(QDir::toNativeSeparators(backup)));
        return;
    }
    target.close();
    QMessageBox::information(this, windowTitle(),
                             tr("Saved. The old save is backed up in %1.")
                                 .arg(QDir::toNativeSeparators(backup)));
    LoadSave(folder);
}
