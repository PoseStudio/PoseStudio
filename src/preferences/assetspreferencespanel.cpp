/**
 * @file assetspreferencespanel.cpp
 * @brief Implements the "Assets" preferences page.
 */

#include "assetspreferencespanel.h"

#include "assetlibraries.h"
#include "constants.h"
#include "librarypaths.h"
#include "preferencesmanager.h"

#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QSize>
#include <QAbstractItemView>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>

// QListWidgetItem data roles used in m_libraryList:
//   Qt::UserRole     — AssetLibraryID (int)
//   Qt::UserRole + 1 — raw AssetLibraryPath (QString)
//
// The built-in "Maquettes" library (AssetLibraryIsBuiltIn = 1) is deliberately excluded from
// this list — it's always present and not user-manageable, so it has nothing to add/remove
// here. It still appears in the Asset Manager tree itself. Every table access goes through
// the AssetLibraries repository (core/assetlibraries.h), never hand-written SQL.

AssetsPreferencesPanel::AssetsPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("Assets"), parent) {

    addDescription(
        tr("Asset folders are scanned for 3D models, poses, and other assets. Add the root "
           "folder of each library you want to browse in the Asset Manager. Double-click a "
           "folder to jump to it."));

    m_libraryList = new QListWidget(this);
    m_libraryList->setObjectName(QStringLiteral("AssetLibraryList"));
    m_libraryList->setSelectionMode(QAbstractItemView::SingleSelection);
    contentLayout()->addWidget(m_libraryList);

    auto* buttonRow = new QHBoxLayout();
    auto* addButton = new QPushButton(tr("Add Asset Folder..."), this);
    auto* removeButton = new QPushButton(tr("Remove Selected"), this);
    removeButton->setEnabled(false);
    buttonRow->addWidget(addButton);
    buttonRow->addWidget(removeButton);
    buttonRow->addStretch(1);
    contentLayout()->addLayout(buttonRow);

    connect(m_libraryList, &QListWidget::itemSelectionChanged, this, [this, removeButton]() {
        removeButton->setEnabled(!m_libraryList->selectedItems().isEmpty());
    });
    connect(m_libraryList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit navigateToLibraryRequested(item->data(Qt::UserRole + 1).toString());
    });
    connect(addButton, &QPushButton::clicked, this, &AssetsPreferencesPanel::promptAddLibrary);
    connect(removeButton, &QPushButton::clicked, this, &AssetsPreferencesPanel::removeSelectedLibrary);

    // WHERE A SAVED MATERIAL'S TEXTURES GO (assets/assetstore.h): copied beside it by default, so
    // that a material's folder is all the material needs; off, the files are named where they are.
    contentLayout()->addSpacing(12);
    auto* copyTextures = new QCheckBox(tr("Copy texture files when saving a material"), this);
    copyTextures->setProperty("testId", QStringLiteral("assets.copytextures"));
    copyTextures->setChecked(PreferencesManager::instance().getValue(Constants::PREF_MATERIAL_COPY_TEXTURES, true).toBool());
    contentLayout()->addWidget(copyTextures);
    addDescription(
        tr("When on, saving a material copies the texture files it uses into a \"textures\" folder beside it "
           "(a file that is already there is not copied twice), so the material keeps working when its folder is "
           "moved, shared or opened on another computer. When off, the material refers to the files where they "
           "are, and stops finding them if they move."));
    connect(copyTextures, &QCheckBox::toggled, this, [](bool enabled) {
        PreferencesManager::instance().setValue(Constants::PREF_MATERIAL_COPY_TEXTURES, enabled ? "1" : "0");
    });

    reloadLibraries();
}

void AssetsPreferencesPanel::reloadLibraries() {
    m_libraryList->clear();

    // userManaged() logs a query failure itself; the list simply stays empty.
    const QList<AssetLibraries::Library> libraries = AssetLibraries::userManaged();
    for (const AssetLibraries::Library& lib : libraries) {
        auto* item = new QListWidgetItem(lib.path, m_libraryList);
        item->setData(Qt::UserRole, lib.id);
        item->setData(Qt::UserRole + 1, lib.path);
        // Set row height here rather than via QSS ::item top/bottom padding — on the default
        // delegate, vertical QSS padding inflates the selection box past the layout rect (the
        // oversized highlight + hover clipping). A fixed sizeHint keeps the row tight. (The
        // Preferences nav list applies the same fix — PreferencesDialog::addPanel.)
        item->setSizeHint(QSize(0, 28));
    }
}

void AssetsPreferencesPanel::promptAddLibrary() {
    // Start the browser in the parent of the last folder added (persisted in Preferences), so
    // adding several sibling libraries doesn't mean re-navigating from home each time; a stored
    // folder that was since moved/deleted falls back to home.
    const QString startDir = PreferencesManager::instance().rememberedDirectory(
        Constants::PREF_LAST_ASSET_FOLDER_PARENT, QDir::homePath());

    const QString folderPath = QFileDialog::getExistingDirectory(
        this, tr("Select Asset Library Folder"), startDir);
    if (folderPath.isEmpty()) return;

    switch (AssetLibraries::add(folderPath)) {
    case AssetLibraries::AddResult::Failed:
        return; // logged by the repository
    case AssetLibraries::AddResult::AlreadyRegistered:
        // Tell the user instead of silently doing nothing — without feedback a re-add of an
        // existing folder looks like the add simply failed.
        QMessageBox::information(this, tr("Already Added"),
                                 tr("That folder is already registered as an asset "
                                    "library:\n%1").arg(folderPath));
        return;
    case AssetLibraries::AddResult::Added:
        break;
    }

    // Remember this folder's parent as the default location for the next add.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_ASSET_FOLDER_PARENT,
                                            QFileInfo(folderPath).absolutePath());
    LibraryPaths::invalidateCache(); // the new folder may BE "My PoseStudio Library"
    reloadLibraries();
    emit librariesChanged();
}

void AssetsPreferencesPanel::removeSelectedLibrary() {
    QListWidgetItem* selected = m_libraryList->currentItem();
    if (!selected) return;

    if (!AssetLibraries::remove(selected->data(Qt::UserRole).toInt())) {
        return; // logged by the repository
    }

    LibraryPaths::invalidateCache(); // the removed folder may have been the resolved user library
    reloadLibraries();
    emit librariesChanged();
}
