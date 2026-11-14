/**
 * @file assetmanagerwidget_files.cpp
 * @brief AssetManagerWidget, third translation unit: an asset's FILE OPERATIONS — Rename,
 *        Duplicate, Move To Folder, Delete — and PACKAGES (Export Package, Install Package).
 *
 * The work itself is assets/assetstore.h's (headless, and checked there): the asset, its icon
 * and the texture files of its folder's `textures` store move, are named and go together. This
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
#include "assetnodeids.h"
#include "assetstore.h"
#include "constants.h"
#include "preferencesmanager.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>

#include <cstdio>

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

void AssetManagerWidget::afterFileOperation(bool foldersChanged) {
    if (foldersChanged) {
        refreshAssetManager();
    }
    refreshCurrentView();
    emit assetFilesChanged();
}

// =============================================================================
// [ THE WORK ]
// =============================================================================

AssetStore::Result AssetManagerWidget::renameAssetTo(const QString& assetPath, const QString& name) {
    const AssetStore::Result result = AssetStore::renameAsset(assetPath, name);
    // (Where it is now — even a rename that could not take the icon along has moved the file.)
    if (!result.path.isEmpty() && result.path != assetPath) {
        AssetDb::repathItem(assetPath, result.path);
    }
    afterFileOperation(false);
    return result;
}

AssetStore::Result AssetManagerWidget::duplicateAssetAt(const QString& assetPath) {
    const AssetStore::Result result = AssetStore::duplicateAsset(assetPath);
    afterFileOperation(false);
    return result;
}

AssetStore::Result AssetManagerWidget::moveAssetTo(const QString& assetPath, const QString& folder) {
    const AssetStore::Result result = AssetStore::moveAsset(assetPath, folder);
    if (!result.path.isEmpty() && result.path != assetPath) {
        AssetDb::repathItem(assetPath, result.path);
    }
    afterFileOperation(true);
    return result;
}

AssetStore::Result AssetManagerWidget::deleteAssetAt(const QString& assetPath, bool permanently) {
    const AssetStore::Result result = AssetStore::deleteAsset(assetPath, permanently);
    if (result.ok) {
        AssetDb::forgetItem(assetPath);
        afterFileOperation(false);
    }
    return result;
}

AssetStore::Result AssetManagerWidget::installPackageFrom(const QString& package, const QString& folderPath, bool replaceExisting) {
    const AssetStore::Result result = AssetStore::installPackage(package, folderPath, replaceExisting);
    if (result.ok || result.files > 0) {
        afterFileOperation(true);
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
    box.setInformativeText(tr("Its icon goes with it, and so do the texture files in this folder's \"textures\" folder that no other "
                              "asset here uses."));
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
    }
}

void AssetManagerWidget::exportPackageOf(const QString& path) {
    const QFileInfo     info(path);
    const QString       name = info.isDir() ? info.fileName() : info.completeBaseName();
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
    const AssetStore::Result result = AssetStore::exportPackage({path}, target);
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
