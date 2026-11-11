// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/saves_window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <JlCompress.h>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"

namespace FritchiiUi {
namespace {
namespace fs = std::filesystem;

/// A profile folder's saves: <nand>/user/save/0000000000000000 (one folder per user profile id,
/// one per game inside).
struct Place {
    QString name;
    fs::path root;
};

fs::path SaveRoot(const fs::path& nand) {
    return nand / "user" / "save" / "0000000000000000";
}

std::vector<Place> Places() {
    using namespace Common::FS;
    std::vector<Place> places{
        {QStringLiteral("Fritchii"), SaveRoot(GetEdenPath(EdenPath::NANDDir))}};
    constexpr std::array<std::pair<EmuPath, const char*>, 2> others{
        {{LegacyEdenDir, "Eden"}, {LichiiDir, "Lichii"}}};
    for (const auto& [path, name] : others) {
        if (fs::is_directory(GetLegacyPath(path) / NAND_DIR)) {
            places.push_back({QString::fromLatin1(name), SaveRoot(GetLegacyPath(path) / NAND_DIR)});
        }
    }
    return places;
}

QString Text(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

/// The user profile folders (32 hex digits) in a save root.
std::vector<fs::path> Profiles(const fs::path& root) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec) && name.size() == 32 &&
            std::ranges::all_of(name, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
            result.push_back(entry.path());
        }
    }
    return result;
}

/// Copies `root` to <nand>/user/save-backups/<time> before it changes. Empty when there was
/// nothing to keep.
fs::path Backup(const fs::path& root) {
    if (Profiles(root).empty()) {
        return {};
    }
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const auto target = root.parent_path().parent_path() / "save-backups" / stamp.toStdString();
    fs::create_directories(target.parent_path());
    fs::copy(root, target, fs::copy_options::recursive);
    return target;
}

/// Each game's saves from `from` into `to`, replacing that game's saves there. When both sides
/// have one user profile, the profile ids may differ (each install makes its own): the saves go
/// into the target's profile. Returns how many games were copied.
int CopySaves(const fs::path& from, const fs::path& to) {
    const auto sources = Profiles(from);
    const auto targets = Profiles(to);
    int games = 0;
    for (const auto& source : sources) {
        const fs::path profile =
            sources.size() == 1 && targets.size() == 1 ? targets[0] : to / source.filename();
        fs::create_directories(profile);
        for (const auto& game : fs::directory_iterator(source)) {
            if (!game.is_directory()) {
                continue;
            }
            const auto target = profile / game.path().filename();
            fs::remove_all(target);
            fs::copy(game.path(), target, fs::copy_options::recursive);
            ++games;
        }
    }
    return games;
}

/// Tests only: EDEN_NX2_SAVES_TEST_FILE answers the export/import file dialog (the sandbox tests
/// can't drive Windows' own dialog reliably).
QString TestFile() {
    return qEnvironmentVariable("EDEN_NX2_SAVES_TEST_FILE");
}

void Done(QWidget* parent, int games, const fs::path& backup) {
    QString text = games == 1 ? QObject::tr("Done: the saves of 1 game were copied.")
                              : QObject::tr("Done: the saves of %1 games were copied.").arg(games);
    if (!backup.empty()) {
        text += QObject::tr("\n\nThe saves that were there before are in:\n%1").arg(Text(backup));
    }
    QMessageBox box(QMessageBox::Information, QObject::tr("Saves"), text, QMessageBox::Ok, parent);
    QAbstractButton* open = nullptr;
    if (!backup.empty()) {
        open = box.addButton(QObject::tr("Open backup"), QMessageBox::ActionRole);
    }
    box.exec();
    if (open && box.clickedButton() == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Text(backup)));
    }
}

} // namespace

void ShowSavesWindow(QWidget* parent, const std::function<bool()>& emulation_running) {
    const auto places = Places();
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Saves"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QFont big = dialog.font();
    big.setPointSizeF(big.pointSizeF() * 1.15);
    dialog.setFont(big);
    auto* layout = new QGridLayout(&dialog);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(14);

    const auto blocked = [&] {
        if (emulation_running && emulation_running()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("Close the game first."));
            return true;
        }
        return false;
    };
    const auto failed = [&](const std::exception& error) {
        QMessageBox::warning(&dialog, QObject::tr("Saves"),
                             QObject::tr("That didn't work:\n%1")
                                 .arg(QString::fromUtf8(error.what())));
    };
    const fs::path fritchii = places.front().root;

    auto* export_button = new QPushButton(QObject::tr("Export..."), &dialog);
    layout->addWidget(new QLabel(QObject::tr("Your saves to a .zip file"), &dialog), 0, 0);
    layout->addWidget(export_button, 0, 1);
    QObject::connect(export_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        if (Profiles(fritchii).empty()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("Fritchii has no saves yet."));
            return;
        }
        const QString name =
            QObject::tr("Fritchii saves %1.zip")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")));
        const QString file = !TestFile().isEmpty() ? TestFile() : QFileDialog::getSaveFileName(
            &dialog, QObject::tr("Export saves"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') +
                name,
            QObject::tr("Zip files (*.zip)"));
        if (file.isEmpty()) {
            return;
        }
        if (!JlCompress::compressDir(file, Text(fritchii), true)) {
            QMessageBox::warning(&dialog, QObject::tr("Saves"),
                                 QObject::tr("Could not write %1").arg(file));
            return;
        }
        QMessageBox::information(&dialog, QObject::tr("Saves"),
                                 QObject::tr("Saves exported to:\n%1").arg(file));
    });

    auto* import_button = new QPushButton(QObject::tr("Import..."), &dialog);
    layout->addWidget(new QLabel(QObject::tr("Saves from a .zip file"), &dialog), 1, 0);
    layout->addWidget(import_button, 1, 1);
    QObject::connect(import_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const QString file = !TestFile().isEmpty() ? TestFile() : QFileDialog::getOpenFileName(
            &dialog, QObject::tr("Import saves"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            QObject::tr("Zip files (*.zip)"));
        if (file.isEmpty()) {
            return;
        }
        const fs::path temp = fritchii.parent_path().parent_path() / "save-import";
        try {
            fs::remove_all(temp);
            fs::create_directories(temp);
            if (JlCompress::extractDir(file, Text(temp)).isEmpty()) {
                throw std::runtime_error("the zip file is empty or can't be read");
            }
            // A Fritchii export holds the profile folders; a zip of the whole save folder has
            // 0000000000000000 above them.
            fs::path root = temp;
            if (Profiles(root).empty() && !Profiles(temp / "0000000000000000").empty()) {
                root = temp / "0000000000000000";
            }
            if (Profiles(root).empty()) {
                throw std::runtime_error("no saves in that zip file");
            }
            const auto backup = Backup(fritchii);
            const int games = CopySaves(root, fritchii);
            fs::remove_all(temp);
            Done(&dialog, games, backup);
        } catch (const std::exception& error) {
            std::error_code ec;
            fs::remove_all(temp, ec);
            failed(error);
        }
    });

    auto* from = new QComboBox(&dialog);
    auto* to = new QComboBox(&dialog);
    for (const auto& place : places) {
        from->addItem(place.name);
        to->addItem(place.name);
    }
    from->setCurrentIndex(places.size() > 1 ? 1 : 0);
    to->setCurrentIndex(0);
    auto* copy_button = new QPushButton(QObject::tr("Copy"), &dialog);
    auto* copy_row = new QHBoxLayout;
    copy_row->addWidget(new QLabel(QObject::tr("Copy saves from"), &dialog));
    copy_row->addWidget(from);
    copy_row->addWidget(new QLabel(QObject::tr("to"), &dialog));
    copy_row->addWidget(to);
    copy_row->addStretch(1);
    layout->addLayout(copy_row, 2, 0);
    layout->addWidget(copy_button, 2, 1);
    const auto update = [=] {
        copy_button->setEnabled(places.size() > 1 && from->currentIndex() != to->currentIndex());
    };
    QObject::connect(from, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(to, &QComboBox::currentIndexChanged, &dialog, update);
    update();
    QObject::connect(copy_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const Place& source = places[from->currentIndex()];
        const Place& target = places[to->currentIndex()];
        if (Profiles(source.root).empty()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("%1 has no saves.").arg(source.name));
            return;
        }
        QString question = QObject::tr("Copy the saves from %1 to %2? The games' saves in %2 "
                                       "are replaced; a backup of them is kept.")
                               .arg(source.name, target.name);
        if (target.name != QStringLiteral("Fritchii")) {
            question += QObject::tr("\n\nClose %1 first.").arg(target.name);
        }
        if (QMessageBox::question(&dialog, QObject::tr("Saves"), question) != QMessageBox::Yes) {
            return;
        }
        try {
            const auto backup = Backup(target.root);
            const int games = CopySaves(source.root, target.root);
            Done(&dialog, games, backup);
        } catch (const std::exception& error) {
            failed(error);
        }
    });

    auto* close = new QPushButton(QObject::tr("Close"), &dialog);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, 3, 1);
    dialog.exec();
}

} // namespace FritchiiUi
