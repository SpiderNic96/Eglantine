// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/pokopia_dex_dialog.h"

#include <algorithm>
#include <vector>

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
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtEndian>

#include "common/fs/path_util.h"
#include "yuzu/configuration/pokopia_residents.h"

namespace {

constexpr const char* TitleFolder = "0400A3A01EE4C000";
constexpr quint32 Magic = 0x31227687;
constexpr quint32 Layout201 = 0x0200017D; // 2.0.1's common save
constexpr quint32 Format = 0x40001;
constexpr int HeaderSize = 0x130;
constexpr int DataSize = 0x470CED;
constexpr int DexOffset = 0x221758; // a byte per resident; bit 0: registered
constexpr int DexCount = 1050;
constexpr int IdRole = Qt::UserRole;

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
    setWindowTitle(tr("Pikantine: Pokopia Pokédex editor"));
    resize(560, 640);
    auto* layout = new QVBoxLayout(this);

    auto* note = new QLabel(
        tr("Tick the Pokémon to register in your Pokédex, then <b>Save</b>. Ticked ones that are "
           "<span style='color:#2e7d32'>green</span> are new. Your save folder is backed up "
           "first. For Pokopia <b>2.0.1</b> saves; experimental, so keep your own backup too."),
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
    layout->addWidget(status);

    search = new QLineEdit(this);
    search->setPlaceholderText(tr("Search"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);

    list = new QListWidget(this);
    layout->addWidget(list, 1);

    auto* actions = new QHBoxLayout();
    auto* tick_all = new QPushButton(tr("Tick all"), this);
    auto* reset = new QPushButton(tr("Back to the save"), this);
    actions->addWidget(tick_all);
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
    connect(search, &QLineEdit::textChanged, this, &PokopiaDexDialog::Filter);
    connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (populating) {
            return;
        }
        const bool fresh = item->checkState() == Qt::Checked &&
                           !registered.contains(item->data(IdRole).toInt());
        item->setForeground(fresh ? QColor(0x2e, 0x7d, 0x32) : palette().color(QPalette::Text));
        UpdateCount();
    });
    connect(tick_all, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden()) {
                list->item(i)->setCheckState(Qt::Checked);
            }
        }
    });
    connect(reset, &QPushButton::clicked, this, &PokopiaDexDialog::Populate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(save_button, &QPushButton::clicked, this, &PokopiaDexDialog::Save);

    FindSaves();
}

PokopiaDexDialog::~PokopiaDexDialog() = default;

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
    list->clear();
    registered.clear();
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
    for (int id = 0; id < DexCount; ++id) {
        if (static_cast<quint8>(data[DexOffset + id]) & 1) {
            registered.insert(id);
        }
    }
    save_button->setEnabled(!game_running);
    Populate();
}

void PokopiaDexDialog::Populate() {
    populating = true;
    list->clear();
    std::vector<Pikantine::Resident> rows(Pikantine::PokopiaResidents.begin(),
                                          Pikantine::PokopiaResidents.end());
    // Registered residents the name list does not know still show, by their id.
    for (const int id : registered) {
        const bool named = std::any_of(rows.begin(), rows.end(),
                                       [id](const auto& r) { return r.id == id; });
        if (!named) {
            rows.push_back({static_cast<u16>(id), 0xffff, nullptr});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.portrait != b.portrait ? a.portrait < b.portrait : a.id < b.id;
    });
    for (const auto& r : rows) {
        const QString name = r.name != nullptr ? QString::fromUtf8(r.name)
                                               : tr("Resident %1").arg(r.id, 4, 10, QLatin1Char('0'));
        auto* item = new QListWidgetItem(QStringLiteral("%1  (#%2)").arg(name).arg(r.id), list);
        item->setData(IdRole, static_cast<int>(r.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(registered.contains(r.id) ? Qt::Checked : Qt::Unchecked);
    }
    populating = false;
    Filter(search->text());
    UpdateCount();
}

void PokopiaDexDialog::UpdateCount() {
    if (data.isEmpty()) {
        return;
    }
    int ticked = 0, fresh = 0;
    for (int i = 0; i < list->count(); ++i) {
        const auto* item = list->item(i);
        if (item->checkState() == Qt::Checked) {
            ++ticked;
            fresh += registered.contains(item->data(IdRole).toInt()) ? 0 : 1;
        }
    }
    status->setText(tr("Registered in this save: %1. Ticked: %2 (%3 new).")
                        .arg(registered.size())
                        .arg(ticked)
                        .arg(fresh));
}

void PokopiaDexDialog::Filter(const QString& text) {
    for (int i = 0; i < list->count(); ++i) {
        auto* item = list->item(i);
        item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
    }
}

void PokopiaDexDialog::Save() {
    if (data.isEmpty() || game_running) {
        return;
    }
    QByteArray edited = data;
    int added = 0, removed = 0;
    for (int i = 0; i < list->count(); ++i) {
        const auto* item = list->item(i);
        const int id = item->data(IdRole).toInt();
        auto& flags = reinterpret_cast<quint8&>(edited[DexOffset + id]);
        const bool want = item->checkState() == Qt::Checked;
        if (want && !(flags & 1)) {
            flags |= 1;
            ++added;
        } else if (!want && (flags & 1)) {
            flags &= ~1;
            ++removed;
        }
    }
    if (added == 0 && removed == 0) {
        QMessageBox::information(this, windowTitle(), tr("Nothing to change."));
        return;
    }
    if (QMessageBox::question(this, windowTitle(),
                              tr("Register %1 and unregister %2 Pokémon in this save?\n\nThe save "
                                 "folder is backed up first.")
                                  .arg(added)
                                  .arg(removed)) != QMessageBox::Yes) {
        return;
    }

    const QString backups = QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "pikantine_backups"));
    const QString backup = QDir(backups).filePath(
        QStringLiteral("%1-%2").arg(QLatin1String(TitleFolder),
                                    QDateTime::currentDateTime().toString(
                                        QStringLiteral("yyyyMMdd-HHmmss"))));
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
