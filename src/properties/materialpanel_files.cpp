/**
 * @file materialpanel_files.cpp
 * @brief MaterialPanel's material FILES: Save (the `.psm` and its icon), Import, and a saved or
 *        MaterialX material applied to the shown zone — the dialogs and messages around the
 *        viewport's own save / apply. See materialpanel.h.
 */

#include "materialpanel.h"

#include "assetscan.h"      // pathsWithin (is the remembered save folder still in the library)
#include "constants.h"
#include "importformats.h"  // the material file's extension
#include "librarypaths.h"   // the Materials library folder
#include "preferencesmanager.h"
#include "viewportwidget.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>

namespace pose {

void MaterialPanel::saveMaterial() {
    if (!m_viewport || !m_view.valid || m_view.zone < 0) {
        return;
    }
    // Where: the Materials library — the folder of it saved into last, while that is still
    // inside the library (a category is saved into again and again), else its root.
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_MATERIAL_DIR);
    const QString       library = LibraryPaths::ensureMaterialsDirectory();
    QString             start = prefs.rememberedDirectory(key, library);
    if (AssetScan::pathsWithin({start}, {library}).isEmpty()) {
        start = library;
    }
    // The name offered: the zone's, less what a file name cannot hold.
    QString suggested = m_view.zone < static_cast<int>(m_view.zones.size())
                            ? QString::fromStdString(m_view.zones[static_cast<std::size_t>(m_view.zone)])
                            : tr("Material");
    static const QString kForbidden = QStringLiteral("\\/:*?\"<>|");
    for (QChar& c : suggested) {
        if (kForbidden.contains(c)) {
            c = QLatin1Char('-');
        }
    }
    const QString extension = ImportFormats::materialFileExtension();
    QString       path = QFileDialog::getSaveFileName(this, tr("Save Material"), QDir(start).filePath(suggested + extension),
                                                      tr("PoseStudio Material (*%1)").arg(extension));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(extension, Qt::CaseInsensitive)) {
        path += extension;
    }
    const QFileInfo file(path);
    prefs.setValue(key, file.absolutePath());

    // The icon lies beside the file under its name. (The file dialog has already asked about
    // replacing the FILE.) An icon that is there is the user's to keep or to have made again.
    const QString iconPath = file.absoluteDir().filePath(file.completeBaseName() + QStringLiteral(".png"));
    bool          makeIcon = true;
    if (QFileInfo::exists(iconPath)) {
        const IconChoice choice = askIconOverwrite(file.completeBaseName());
        if (choice == IconChoice::Cancel) {
            return; // nothing is saved
        }
        makeIcon = choice == IconChoice::Recreate;
    }

    // The textures go WITH the material — copied into a `.textures` folder beside it — unless
    // Preferences > Assets says to name them where they are (assets/assetstore.h).
    const bool         copyTextures = prefs.getValue(QString::fromLatin1(Constants::PREF_MATERIAL_COPY_TEXTURES), true).toBool();
    QString            error;
    MaterialSaveReport report;
    if (!m_viewport->saveMaterialFile(path, copyTextures, &error, &report)) {
        QMessageBox::warning(this, tr("Save Material"), error);
        return;
    }
    bool iconFailed = false;
    if (makeIcon) {
        // A render engine traces the ball, which takes a moment (the first time on a graphics
        // card, several seconds): the window waits behind a note, not a frozen button.
        QProgressDialog busy(tr("Rendering the material's icon..."), QString(), 0, 0, this);
        busy.setWindowTitle(tr("Save Material"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);
        busy.setCancelButton(nullptr);
        busy.show();
        QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const QImage icon = m_viewport->renderMaterialIcon(Constants::MATERIAL_ICON_SIZE);
        busy.close();
        iconFailed = icon.isNull() || !icon.save(iconPath, "PNG");
    }
    emit materialSaved(path);

    QStringList notes;
    if (iconFailed) {
        notes << tr("The icon could not be written, so the material will not be listed in the Materials Library until it has one "
                    "(an image named like the file, beside it).");
    }
    if (report.notCopied > 0) {
        notes << tr("%n texture file(s) could not be copied into the material's \".textures\" folder. The material refers to "
                    "them where they are, and will not find them on another computer.", nullptr, report.notCopied);
    }
    if (report.leftOut > 0) {
        notes << (copyTextures
                      ? tr("%n map(s) of this zone could not be written out as files: the saved material does not include them.",
                           nullptr, report.leftOut)
                      : tr("%n map(s) of this zone exist only inside the model's file and have no file of their own to refer to: "
                           "the saved material does not include them. (With \"Copy texture files when saving a material\" on, in "
                           "Preferences > Assets, they are written out beside the material.)", nullptr, report.leftOut));
    }
    if (!notes.isEmpty()) {
        QMessageBox::information(this, tr("Save Material"),
                                 tr("The material was saved as \"%1\".").arg(file.fileName()) + QStringLiteral("\n\n") +
                                     notes.join(QStringLiteral("\n\n")));
    }
}

MaterialPanel::IconChoice MaterialPanel::askIconOverwrite(const QString& name) {
    QMessageBox box(QMessageBox::Question, tr("Save Material"), tr("\"%1\" already has an icon.").arg(name), QMessageBox::NoButton, this);
    box.setInformativeText(tr("Recreate the icon from the material as it is now, and overwrite the old one?"));
    QPushButton* recreate = box.addButton(tr("Recreate Icon"), QMessageBox::YesRole);
    QPushButton* keep = box.addButton(tr("Keep Existing Icon"), QMessageBox::NoRole);
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    for (QPushButton* button : {recreate, keep, cancel}) {
        button->setProperty("testId", QStringLiteral("materials.iconquestion")); // (0 recreate, 1 keep, 2 cancel)
    }
    box.setDefaultButton(recreate);
    box.exec();
    if (box.clickedButton() == recreate) {
        return IconChoice::Recreate;
    }
    return box.clickedButton() == keep ? IconChoice::Keep : IconChoice::Cancel; // (closed by Esc or its corner: nothing is saved)
}

void MaterialPanel::previewIconQuestion(int) {
    askIconOverwrite(m_view.valid && m_view.zone >= 0 && m_view.zone < static_cast<int>(m_view.zones.size())
                         ? QString::fromStdString(m_view.zones[static_cast<std::size_t>(m_view.zone)])
                         : tr("Material"));
}

void MaterialPanel::importMaterial() {
    if (!m_viewport || !m_view.valid || m_view.zone < 0) {
        return;
    }
    // (The folder a material was imported from last: material packs are downloaded and unpacked
    // into one place, and imported from there one after another.)
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_MATERIALX_DIR);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Material"), prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)),
        tr("Material Files (*.mtlx *.psm);;MaterialX Files (*.mtlx);;PoseStudio Materials (*.psm)"));
    if (path.isEmpty()) {
        return; // cancelled
    }
    prefs.setValue(key, QFileInfo(path).absolutePath());
    applyMaterialFile(path);
}

void MaterialPanel::applyMaterialFile(const QString& path) {
    if (!m_viewport) {
        return;
    }
    // A MATERIALX document may hold several materials: which one is the user's to say.
    QString material;
    QString error;
    if (pose::ImportFormats::isMaterialXFile(path)) {
        const QStringList names = m_viewport->materialNamesIn(path, &error);
        if (names.isEmpty()) {
            QMessageBox::information(this, tr("Apply Material"), error);
            return;
        }
        material = names.first();
        if (names.size() > 1) {
            bool chosen = false;
            material = QInputDialog::getItem(this, tr("Apply Material"),
                                             tr("\"%1\" holds %n material(s). Apply which one?", nullptr, static_cast<int>(names.size()))
                                                 .arg(QFileInfo(path).fileName()),
                                             names, 0, /*editable=*/false, &chosen);
            if (!chosen) {
                return;
            }
        }
    }
    int         missing = 0;
    QStringList notes;
    if (!m_viewport->applyMaterialFile(path, &error, &missing, nullptr, material, &notes)) {
        QMessageBox::information(this, tr("Apply Material"), error);
        return;
    }
    refresh();
    QStringList said;
    if (missing > 0) {
        // (`missing` counts a file that was not found AND one that was found and is no readable image.)
        said << tr("%n texture file(s) of \"%1\" could not be used: not found where the material says, nor by name "
                   "beside it or anywhere in its library — or found, and not readable as an image. Those slots were "
                   "left empty.", nullptr, missing)
                    .arg(QFileInfo(path).fileName());
    }
    if (!notes.isEmpty()) {
        // What a MaterialX document describes that a PoseStudio material cannot hold.
        said << tr("Not everything in this material could be carried over:") + QStringLiteral("\n\n") + notes.join(QStringLiteral("\n\n"));
    }
    if (!said.isEmpty()) {
        QMessageBox box(missing > 0 ? QMessageBox::Warning : QMessageBox::Information, tr("Apply Material"), said.join(QStringLiteral("\n\n")),
                        QMessageBox::Ok, this);
        box.exec();
    }
}

} // namespace pose
