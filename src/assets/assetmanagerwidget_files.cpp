// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetmanagerwidget_files.cpp
 * @brief AssetManagerWidget, the file operations' translation unit (assetmanagerwidget.h lists
 *        the four): an asset's FILE OPERATIONS — Rename, Duplicate, Move To Folder, Delete —
 *        and PACKAGES (Export Package, Install Package).
 *
 * The work itself is the asset store's (assetstore.h for the standard, assetfileops.h and
 * assetpackage.h for the operations; headless, and checked there): the asset, its icon
 * and the texture files of its folder's `.textures` store move, are named and go together. This
 * file is what the browser adds. Each operation has two halves:
 *
 *   - the DIALOG (renameAssetFile, moveAssetFile, …): what the context menu's entry runs — it
 *     asks for a name, a folder, a confirmation, and shows what went wrong;
 *   - the WORK (renameAssetTo, moveAssetTo, …): the store's operation, then the Favorites and
 *     Collections filings following the file (AssetDb::repathItem / forgetItem), the grid and
 *     tree read again, and the other library browser told (assetFilesChanged). No dialog — so
 *     the scripted in-app test can run it (runFileOperation, the script's `asset` command).
 *
 * The operations are offered for assets PoseStudio itself writes (AssetStore::isOwnAsset: a
 * saved material). Another application's content library is read in place — its files are
 * laid out by that application's rules and are not this one's to rearrange or delete.
 */

#include "assetmanagerwidget.h"

#include "assetdb.h"
#include "assetfileops.h"
#include "assetfolderproxymodel.h"
#include "assetnodeids.h"
#include "assetpackage.h"
#include "assetstore.h"
#include "constants.h"
#include "preferencesmanager.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>

#include <cstdio>

namespace {
/// A folder beyond either of these is asked about before it is packed (exportPackageOf).
constexpr int    kLargePackageFiles = 5000;
constexpr qint64 kLargePackageBytes = qint64(1) << 30;
} // namespace

AssetManagerWidget::FileActions AssetManagerWidget::addFileActions(QMenu* menu, const QString& assetPath) {
    FileActions actions;
    if (!AssetStore::isOwnAsset(assetPath) || !QFileInfo(assetPath).isFile()) {
        return actions;
    }
    menu->addSeparator();
    actions.rename = menu->addAction(icon(QStringLiteral("rename")), tr("Rename..."));
    actions.duplicate = menu->addAction(icon(QStringLiteral("copy")), tr("Duplicate"));
    actions.move = menu->addAction(tr("Move To Folder..."));
    actions.exportPackage = menu->addAction(icon(QStringLiteral("export")), tr("Export Package..."));
    actions.remove = menu->addAction(tr("Delete"));
    return actions;
}

bool AssetManagerWidget::handleFileAction(QAction* selected, const FileActions& actions, const QString& assetPath) {
    if (selected == nullptr) {
        return false;
    }
    if (selected == actions.rename) {
        renameAssetFile(assetPath);
    } else if (selected == actions.duplicate) {
        duplicateAssetFile(assetPath);
    } else if (selected == actions.move) {
        moveAssetFile(assetPath);
    } else if (selected == actions.exportPackage) {
        exportPackageOf(assetPath);
    } else if (selected == actions.remove) {
        deleteAssetFile(assetPath);
    } else {
        return false;
    }
    return true;
}

void AssetManagerWidget::afterFileOperation(const QString& folder, bool foldersChanged) {
    if (foldersChanged) {
        refreshAssetManager(); // (the tree again; it shows the grid's source again itself)
    } else {
        // The folder's files changed, not its subfolders: the tree stays, but what it says of
        // the folder — whether it holds assets, and so whether those above it do — is asked again.
        proxyModel->refreshFolder(folder);
        refreshCurrentView();
    }
    emit assetFilesChanged();
}

// =============================================================================
// [ THE WORK ]
// =============================================================================

AssetStore::Result AssetManagerWidget::renameAssetTo(const QString& assetPath, const QString& name) {
    const AssetStore::Result result = AssetStore::renameAsset(assetPath, name);
    // (A rename that failed has renamed nothing, and names no path.)
    if (!result.path.isEmpty() && result.path != assetPath) {
        AssetDb::repathItem(assetPath, result.path);
    }
    afterFileOperation(QFileInfo(assetPath).absolutePath(), false);
    return result;
}

AssetStore::Result AssetManagerWidget::duplicateAssetAt(const QString& assetPath) {
    const AssetStore::Result result = AssetStore::duplicateAsset(assetPath);
    afterFileOperation(QFileInfo(assetPath).absolutePath(), false);
    return result;
}

AssetStore::Result AssetManagerWidget::moveAssetTo(const QString& assetPath, const QString& folder) {
    const AssetStore::Result result = AssetStore::moveAsset(assetPath, folder);
    if (!result.path.isEmpty() && result.path != assetPath) {
        AssetDb::repathItem(assetPath, result.path);
    }
    afterFileOperation(folder, true);
    return result;
}

AssetStore::Result AssetManagerWidget::deleteAssetAt(const QString& assetPath, bool permanently) {
    const AssetStore::Result result = AssetStore::deleteAsset(assetPath, permanently);
    if (result.ok) {
        AssetDb::forgetItem(assetPath);
        proxyModel->refreshCollections(); // (one that held nothing else has no asset left)
        afterFileOperation(QFileInfo(assetPath).absolutePath(), false);
    }
    return result;
}

AssetStore::Result AssetManagerWidget::installPackageFrom(const QString& package, const QString& folderPath, bool replaceExisting) {
    const AssetStore::Result result = AssetStore::installPackage(package, folderPath, replaceExisting);
    if (result.ok || result.files > 0) {
        afterFileOperation(folderPath, true);
    }
    return result;
}

void AssetManagerWidget::runFileOperation(const QStringList& words) {
    // asset rename <file> <name> | duplicate <file> | move <file> <folder> | delete <file>
    //     | export <file or folder> <zip> | install <zip> <folder> [replace]
    // (The scripted in-app test's door: the paths arrive absolute, and nothing asks.)
    const QString      op = words.value(0).toLower();
    AssetStore::Result result;
    if (op == QLatin1String("rename") && words.size() > 2) {
        result = renameAssetTo(words[1], words.mid(2).join(QLatin1Char(' ')));
    } else if (op == QLatin1String("duplicate") && words.size() > 1) {
        result = duplicateAssetAt(words[1]);
    } else if (op == QLatin1String("move") && words.size() > 2) {
        result = moveAssetTo(words[1], words[2]);
    } else if (op == QLatin1String("delete") && words.size() > 1) {
        result = deleteAssetAt(words[1], /*permanently=*/true); // (a test run fills nobody's trash)
    } else if (op == QLatin1String("export") && words.size() > 2) {
        result = AssetStore::exportPackage({words[1]}, words[2]);
    } else if (op == QLatin1String("install") && words.size() > 2) {
        result = installPackageFrom(words[1], words[2], words.value(3).compare(QLatin1String("replace"), Qt::CaseInsensitive) == 0);
    } else {
        result.error = QStringLiteral("unknown asset operation");
    }
    std::fprintf(stderr, "[ikscript]   asset %s: %s, %d file(s)%s%s\n", qPrintable(op), result.ok ? "ok" : "FAILED", result.files,
                 result.path.isEmpty() ? "" : qPrintable(QStringLiteral(" -> ") + result.path),
                 result.error.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + result.error.simplified() + QLatin1Char(')')));
    std::fflush(stderr);
}

// =============================================================================
// [ THE DIALOGS ]
// =============================================================================

void AssetManagerWidget::renameAssetFile(const QString& assetPath) {
    const QString oldName = QFileInfo(assetPath).completeBaseName();
    bool          accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal, oldName, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name == oldName) {
        return;
    }
    const AssetStore::Result result = renameAssetTo(assetPath, name);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Rename"), result.error);
    }
}

void AssetManagerWidget::duplicateAssetFile(const QString& assetPath) {
    const AssetStore::Result result = duplicateAssetAt(assetPath);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Duplicate"), result.error);
    }
}

void AssetManagerWidget::moveAssetFile(const QString& assetPath) {
    const QFileInfo info(assetPath);
    const QString   folder = QFileDialog::getExistingDirectory(this, tr("Move \"%1\" To Folder").arg(info.completeBaseName()), info.absolutePath());
    if (folder.isEmpty()) {
        return;
    }
    const AssetStore::Result result = moveAssetTo(assetPath, folder);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Move To Folder"), result.error);
    }
}

void AssetManagerWidget::deleteAssetFile(const QString& assetPath) {
    const QString name = QFileInfo(assetPath).completeBaseName();
    QMessageBox   box(QMessageBox::Question, tr("Delete"), tr("Move \"%1\" to the trash?").arg(name), QMessageBox::NoButton, this);
    box.setInformativeText(tr("Its icon goes with it, and so do the texture files it uses in this folder's \"%1\" folder that no other "
                              "asset here uses.")
                               .arg(AssetStore::texturesFolderName()));
    QPushButton* trash = box.addButton(tr("Move To Trash"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != trash) {
        return;
    }
    const AssetStore::Result result = deleteAssetAt(assetPath, /*permanently=*/false);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Delete"), result.error);
    } else if (!result.kept.isEmpty()) {
        QMessageBox::information(this, tr("Delete"),
                                 tr("\"%1\" was deleted. %n texture file(s) it used were left in this folder's \"%2\" folder: "
                                    "another asset here could not be read, and may use them too.",
                                    nullptr, static_cast<int>(result.kept.size()))
                                     .arg(name, AssetStore::texturesFolderName()));
    }
}

void AssetManagerWidget::exportPackageOf(const QString& path) {
    const QFileInfo     info(path);
    const QString       name = info.isDir() ? info.fileName() : info.completeBaseName();

    // A folder is packed with everything in it, and the entry is on every folder of every library
    // — a content library's root as well, where "everything" is tens of gigabytes read and
    // compressed with the window not responding. So a folder is counted first, and a large one
    // asked about. (An asset is itself, its icon and its own textures: never large.)
    if (info.isDir()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const AssetStore::PackageSize size = AssetStore::packageSize({path});
        QApplication::restoreOverrideCursor();
        if (size.files > kLargePackageFiles || size.bytes > kLargePackageBytes) {
            QMessageBox box(QMessageBox::Question, tr("Export Package"),
                            tr("\"%1\" holds %n file(s), %2 in all.", nullptr, size.files).arg(name, QLocale().formattedDataSize(size.bytes)),
                            QMessageBox::NoButton, this);
            box.setInformativeText(tr("Packing a folder this large takes a while, and PoseStudio does not respond until it is done. "
                                      "Pack the whole folder?"));
            QPushButton* pack = box.addButton(tr("Pack Folder"), QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != pack) {
                return;
            }
        }
    }

    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_PACKAGE_DIR);
    const QString       start = prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    QString             target = QFileDialog::getSaveFileName(this, tr("Export Package"), QDir(start).filePath(name + QStringLiteral(".zip")),
                                                              tr("Package (*.zip)"));
    if (target.isEmpty()) {
        return;
    }
    if (!target.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) {
        target += QStringLiteral(".zip");
    }
    prefs.setValue(key, QFileInfo(target).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const AssetStore::Result result = AssetStore::exportPackage({path}, target);
    QApplication::restoreOverrideCursor();
    if (!result.ok) {
        QMessageBox::warning(this, tr("Export Package"), result.error);
        return;
    }
    QMessageBox::information(this, tr("Export Package"),
                             tr("%n file(s) were packed into \"%1\".", nullptr, result.files).arg(QFileInfo(target).fileName()) +
                                 QStringLiteral("\n\n") +
                                 tr("To install the package on another computer, use Install Package in a library folder's menu there, "
                                    "or unzip it into a library folder."));
}

void AssetManagerWidget::installPackageInto(const QString& folderPath) {
    if (!QFileInfo(folderPath).isDir()) {
        return;
    }
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_PACKAGE_DIR);
    const QString       start = prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    const QString       package = QFileDialog::getOpenFileName(this, tr("Install Package"), start, tr("Package (*.zip)"));
    if (package.isEmpty()) {
        return;
    }
    prefs.setValue(key, QFileInfo(package).absolutePath());

    // Files that are there already with OTHER content are the user's to keep or to replace.
    bool      replace = false;
    const int conflicts = AssetStore::packageConflicts(package, folderPath);
    if (conflicts > 0) {
        QMessageBox box(QMessageBox::Question, tr("Install Package"),
                        tr("%n file(s) of this package are already in the folder, with different content.", nullptr, conflicts), QMessageBox::NoButton,
                        this);
        box.setInformativeText(tr("Replace them with the package's files, or keep the ones that are there?"));
        QPushButton* replaceButton = box.addButton(tr("Replace"), QMessageBox::YesRole);
        QPushButton* keepButton = box.addButton(tr("Keep Existing"), QMessageBox::NoRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(keepButton);
        box.exec();
        if (box.clickedButton() != replaceButton && box.clickedButton() != keepButton) {
            return;
        }
        replace = box.clickedButton() == replaceButton;
    }
    const AssetStore::Result result = installPackageFrom(package, folderPath, replace);
    if (!result.ok) {
        QMessageBox::warning(this, tr("Install Package"), result.error);
        return;
    }
    QString text = result.files > 0 ? tr("%n file(s) were installed into \"%1\".", nullptr, result.files).arg(QDir(folderPath).dirName())
                                    : tr("Everything in this package is in \"%1\" already.").arg(QDir(folderPath).dirName());
    if (!result.kept.isEmpty()) {
        text += QStringLiteral("\n\n") + tr("%n file(s) that were already there were kept.", nullptr, static_cast<int>(result.kept.size()));
    }
    QMessageBox::information(this, tr("Install Package"), text);
}
