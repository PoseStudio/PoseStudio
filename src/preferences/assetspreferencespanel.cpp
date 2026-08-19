/**
 * @file assetspreferencespanel.cpp
 * @brief Implements the "Assets" preferences page.
 */

#include "assetspreferencespanel.h"

#include "constants.h"
#include "preferencesmanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QSize>
#include <QAbstractItemView>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QMessageBox>

// QListWidgetItem data roles used in m_libraryList:
//   Qt::UserRole     — AssetLibraryID (int)
//   Qt::UserRole + 1 — raw AssetLibraryPath (QString)
//
// The built-in "Maquettes" library (AssetLibraryIsBuiltIn = 1) is deliberately excluded from
// this list — it's always present and not user-manageable, so it has nothing to add/remove
// here. It still appears in the Asset Manager tree itself.

AssetsPreferencesPanel::AssetsPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("Assets"), parent) {

    addDescription(
        "Asset folders are scanned for 3D models, poses, and other assets. Add the root "
        "folder of each library you want to browse in the Asset Manager. Double-click a "
        "folder to jump to it.");

    m_libraryList = new QListWidget(this);
    m_libraryList->setObjectName(QStringLiteral("AssetLibraryList"));
    m_libraryList->setSelectionMode(QAbstractItemView::SingleSelection);
    contentLayout()->addWidget(m_libraryList);

    auto* buttonRow = new QHBoxLayout();
    auto* addButton = new QPushButton(QStringLiteral("Add Asset Folder..."), this);
    auto* removeButton = new QPushButton(QStringLiteral("Remove Selected"), this);
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

    reloadLibraries();
}

void AssetsPreferencesPanel::reloadLibraries() {
    m_libraryList->clear();

    QSqlQuery q(QSqlDatabase::database(QStringLiteral("db_conn")));
    if (!q.exec(QStringLiteral(
            "SELECT AssetLibraryID, AssetLibraryPath FROM AssetLibraries "
            "WHERE AssetLibraryIsBuiltIn = 0 ORDER BY AssetLibraryPath COLLATE NOCASE"))) {
        // Surface the failure — an empty list from a silent error reads as "my libraries are gone".
        qWarning() << "[!] Failed to load asset libraries:" << q.lastError().text();
        return;
    }
    while (q.next()) {
        const QString path = q.value(1).toString();

        auto* item = new QListWidgetItem(path, m_libraryList);
        item->setData(Qt::UserRole, q.value(0).toInt());
        item->setData(Qt::UserRole + 1, path);
        // Set row height here rather than via QSS ::item top/bottom padding — on the default
        // delegate, vertical QSS padding inflates the selection box past the layout rect (the
        // oversized highlight + hover clipping). A fixed sizeHint keeps the row tight.
        item->setSizeHint(QSize(0, 28));
    }
}

void AssetsPreferencesPanel::promptAddLibrary() {
    // Start the browser in the parent of the last folder added (persisted in Preferences), so
    // adding several sibling libraries doesn't mean re-navigating from home each time.
    QString startDir = PreferencesManager::instance()
                           .getValue(Constants::PREF_LAST_ASSET_FOLDER_PARENT, QDir::homePath())
                           .toString();
    if (startDir.isEmpty() || !QDir(startDir).exists()) {
        startDir = QDir::homePath(); // stored folder was moved/deleted, or nothing saved yet
    }

    const QString folderPath = QFileDialog::getExistingDirectory(
        this, "Select Asset Library Folder", startDir);
    if (folderPath.isEmpty()) return;

    QSqlQuery q(QSqlDatabase::database(QStringLiteral("db_conn")));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO AssetLibraries (AssetLibraryPath) VALUES (:path)"));
    q.bindValue(":path", folderPath);
    if (!q.exec()) {
        qWarning() << "[!] Failed to add asset library:" << q.lastError().text();
        return;
    }

    // INSERT OR IGNORE swallows the UNIQUE(AssetLibraryPath) conflict, so a duplicate add
    // "succeeds" with zero rows. Tell the user instead of silently doing nothing — without
    // feedback a re-add of an existing folder looks like the add simply failed.
    if (q.numRowsAffected() == 0) {
        QMessageBox::information(this, QStringLiteral("Already Added"),
                                 QStringLiteral("That folder is already registered as an asset "
                                                "library:\n%1").arg(folderPath));
        return;
    }

    // Remember this folder's parent as the default location for the next add.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_ASSET_FOLDER_PARENT,
                                            QFileInfo(folderPath).absolutePath());
    reloadLibraries();
    emit librariesChanged();
}

void AssetsPreferencesPanel::removeSelectedLibrary() {
    QListWidgetItem* selected = m_libraryList->currentItem();
    if (!selected) return;

    QSqlQuery q(QSqlDatabase::database(QStringLiteral("db_conn")));
    q.prepare(QStringLiteral("DELETE FROM AssetLibraries WHERE AssetLibraryID = :id"));
    q.bindValue(":id", selected->data(Qt::UserRole).toInt());
    if (!q.exec()) {
        qWarning() << "[!] Failed to remove asset library:" << q.lastError().text();
        return;
    }

    reloadLibraries();
    emit librariesChanged();
}
