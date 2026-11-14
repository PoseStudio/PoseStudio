/**
 * @file assetmanagerwidget_menus.cpp
 * @brief AssetManagerWidget, second translation unit: the tree and grid context menus.
 *
 * Every right-click menu the Asset Manager shows is built here, per node kind (empty tree
 * space, the Favorites root, the Collections root, a Collection, a physical folder; a grid
 * folder item, empty grid space, an asset item) plus the shared "Add To Collection" submenu.
 * The menus share a few building blocks so their shape stays consistent: the object name that
 * gives them their QSS, the trailing "Manage Asset Folders / Refresh" pair, the Expand /
 * Collapse entries, and the "Find In Library / Browse Folder" pair for anything shown outside
 * the plain library tree. Icons come from the widget's per-instance cache rather than being
 * constructed from resource paths on every menu open. Split out of assetmanagerwidget.cpp
 * purely for size: it is the same class, with full access to its state.
 */

#include "assetmanagerwidget.h"
#include "assetdb.h"
#include "assetfolderproxymodel.h"
#include "assetgriddragcontroller.h"
#include "assetnodeids.h"
#include "assetstore.h"
#include "assettreeview.h"
#include "constants.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QMenu>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QUrl>
#include <tuple>

// =============================================================================
// [ SHARED BUILDING BLOCKS ]
// =============================================================================

QMenu* AssetManagerWidget::newContextMenu(QWidget* parent) {
    QMenu* menu = new QMenu(parent);
    menu->setObjectName("AssetManagerContextMenu");
    return menu;
}

QPair<QAction*, QAction*> AssetManagerWidget::addManageRefreshActions(QMenu* menu) {
    // The folders are managed in Preferences -> Assets — the registered libraries. A browser
    // with roots of its own (the Materials Library) has none there, so it gets Refresh alone.
    QAction* manage = m_scope.hasOwnRoots()
        ? nullptr
        : menu->addAction(icon(QStringLiteral("preferences")), tr("Manage Asset Folders"));
    QAction* refresh = menu->addAction(icon(QStringLiteral("refresh")), tr("Refresh"));
    return {manage, refresh};
}

AssetManagerWidget::ExpandActions AssetManagerWidget::addExpandCollapseActions(QMenu* menu, const QModelIndex& proxyIndex, bool withBranch) {
    ExpandActions actions;
    const bool isExpanded = dirTreeView->isExpanded(proxyIndex);
    const bool hasChildren = proxyModel->hasChildren(proxyIndex);

    if (!isExpanded) {
        actions.expand = menu->addAction(icon(QStringLiteral("expand")), tr("Expand"));
        actions.expand->setEnabled(hasChildren);
    }
    if (withBranch) {
        actions.expandBranch = menu->addAction(icon(QStringLiteral("expand-branch")), tr("Expand Branch"));
        actions.expandBranch->setEnabled(hasChildren);
    }
    if (isExpanded) {
        actions.collapse = menu->addAction(icon(QStringLiteral("collapse")), tr("Collapse"));
    }
    return actions;
}

bool AssetManagerWidget::handleExpandCollapse(QAction* selected, const ExpandActions& actions, const QModelIndex& proxyIndex) {
    if (!selected) return false;
    if (actions.expand && selected == actions.expand) { dirTreeView->expand(proxyIndex); return true; }
    if (actions.collapse && selected == actions.collapse) { collapseNodeRecursively(proxyIndex); return true; }
    if (actions.expandBranch && selected == actions.expandBranch) { expandNodeRecursively(proxyIndex); return true; }
    return false;
}

QPair<QAction*, QAction*> AssetManagerWidget::addFindAndBrowseActions(QMenu* menu) {
    QAction* find = menu->addAction(icon(QStringLiteral("tree")), tr("Find In Library"));
    QAction* browse = menu->addAction(icon(QStringLiteral("browse-folder")), tr("Browse Folder"));
    return {find, browse};
}

/**
 * @brief Expands a node and all nested subdirectories in a single batched paint pass.
 */
void AssetManagerWidget::expandNodeRecursively(const QModelIndex &proxyIndex) {
    dirTreeView->setUpdatesEnabled(false);
    dirTreeView->expandRecursively(proxyIndex);
    dirTreeView->setUpdatesEnabled(true);
}

/**
 * @brief Recursively collapses a node and resets the expansion state of its active children.
 */
void AssetManagerWidget::collapseNodeRecursively(const QModelIndex &proxyIndex) {
    int childCount = proxyModel->rowCount(proxyIndex);

    for (int i = 0; i < childCount; ++i) {
        QModelIndex childIndex = proxyModel->index(i, 0, proxyIndex);

        if (dirTreeView->isExpanded(childIndex)) {
            collapseNodeRecursively(childIndex);
        }
    }

    dirTreeView->collapse(proxyIndex);
}

/**
 * @brief Builds an "Add To Collection" submenu for a physical folder: a pinned "New Collection"
 *        entry (creates a top-level collection from the folder) followed by every existing
 *        collection (shown by full path, creates a sub-collection under it). Each entry turns the
 *        folder into a populated collection (see addFolderAsCollection). Caller owns the menu.
 */
QMenu* AssetManagerWidget::buildAddToCollectionMenu(QWidget* parentMenu, const QString& folderPath) {
    QMenu *addMenu = newContextMenu(parentMenu);
    addMenu->setTitle(tr("Add To Collection"));
    addMenu->setIcon(icon(QStringLiteral("collections")));

    QAction *newCollAction = addMenu->addAction(tr("New Collection"));
    connect(newCollAction, &QAction::triggered, this, [this, folderPath]() {
        int cid = addFolderAsCollection(folderPath, 0);
        if (cid > 0) navigateToCollectionNode(cid);
    });

    const QList<QPair<int, QString>> collections = collectionPathList();
    if (!collections.isEmpty()) addMenu->addSeparator();

    for (const auto& [colId, colPath] : collections) {
        QAction *addAction = addMenu->addAction(colPath);
        connect(addAction, &QAction::triggered, this, [this, folderPath, colId]() {
            int cid = addFolderAsCollection(folderPath, colId);
            if (cid > 0) navigateToCollectionNode(cid);
        });
    }

    return addMenu;
}

// =============================================================================
// [ TREE CONTEXT MENUS ]
// =============================================================================

/**
 * @brief Constructs and routes right-click context menus based on the data type of the clicked node.
 */
void AssetManagerWidget::onContextMenuRequested(const QPoint &pos) {
    QModelIndex proxyIndex = dirTreeView->indexAt(pos);
    const QPoint globalPos = dirTreeView->viewport()->mapToGlobal(pos);

    // =========================================================================
    // DEDICATED MENU: EMPTY SPACE (BACKGROUND CLICK IN TREE)
    // =========================================================================
    if (!proxyIndex.isValid()) {
        QMenu* emptyMenu = newContextMenu(this);
        const auto [manageFoldersAction, refreshAction] = addManageRefreshActions(emptyMenu);
        QAction *selectedAction = emptyMenu->exec(globalPos);
        emptyMenu->deleteLater();

        if (manageFoldersAction && selectedAction == manageFoldersAction) {
            emit manageAssetFoldersRequested();
        } else if (selectedAction == refreshAction) {
            refreshAssetManager();
        }
        return;
    }

    const QString folderPath = proxyIndex.data(Qt::UserRole).toString();

    // Prevent context menus on non-interactive structural nodes
    if (AssetNode::isStructural(folderPath)) {
        return;
    }

    // =========================================================================
    // DEDICATED MENU: "FAVORITES" ROOT NODE (just Refresh: it's a flat container)
    // =========================================================================
    if (folderPath == AssetNode::FAVORITES_ROOT) {
        QMenu* favMenu = newContextMenu(this);
        QAction *refreshAction = favMenu->addAction(icon(QStringLiteral("refresh")), tr("Refresh"));
        const bool refresh = (favMenu->exec(globalPos) == refreshAction);
        favMenu->deleteLater();
        if (refresh) refreshAssetManager();
        return;
    }

    // =========================================================================
    // DEDICATED MENU: "COLLECTIONS" ROOT NODE
    // =========================================================================
    if (folderPath == AssetNode::COLLECTIONS_ROOT) {
        QMenu* rootMenu = newContextMenu(this);

        const ExpandActions expandActions = addExpandCollapseActions(rootMenu, proxyIndex, false);

        rootMenu->addSeparator();
        QAction *newCollAction = rootMenu->addAction(icon(QStringLiteral("add-col")), tr("New Collection"));

        rootMenu->addSeparator();
        const auto [manageFoldersAction, refreshAction] = addManageRefreshActions(rootMenu);

        QAction *selectedAction = rootMenu->exec(globalPos);
        rootMenu->deleteLater();

        if (handleExpandCollapse(selectedAction, expandActions, proxyIndex)) return;
        if (selectedAction == newCollAction) {
            int cid = createCollection(newCollectionName(tr("New Collection"), 0), 0);
            if (cid > 0) navigateToCollectionNode(cid, true);
        }
        else if (manageFoldersAction && selectedAction == manageFoldersAction) emit manageAssetFoldersRequested();
        else if (selectedAction == refreshAction) refreshAssetManager();

        return;
    }

    // =========================================================================
    // DEDICATED MENU: INDIVIDUAL DB COLLECTION ITEMS
    // =========================================================================
    if (AssetNode::isCollection(folderPath)) {
        const int collId = AssetNode::collectionId(folderPath);

        QMenu* collMenu = newContextMenu(this);

        QAction *renameAction = collMenu->addAction(icon(QStringLiteral("rename")),
                                                    tr("Rename %1").arg(tr("Collection")));
        QAction *deleteAction = collMenu->addAction(tr("Delete %1").arg(tr("Collection")));
        // Deletable only when both asset items and sub-collections are absent
        deleteAction->setEnabled(AssetDb::collectionIsEmpty(collId));
        collMenu->addSeparator();

        const ExpandActions expandActions = addExpandCollapseActions(collMenu, proxyIndex, false);

        collMenu->addSeparator();
        QAction *newSubCollAction = collMenu->addAction(icon(QStringLiteral("add-col")), tr("New Sub-Collection"));

        collMenu->addSeparator();
        const auto [manageFoldersAction, refreshAction] = addManageRefreshActions(collMenu);

        QAction *selectedAction = collMenu->exec(globalPos);
        collMenu->deleteLater();

        if (renameAction && selectedAction == renameAction) {
            dirTreeView->edit(proxyIndex);
        } else if (deleteAction && selectedAction == deleteAction) {
            // The DB delete is one transaction; the tree row only goes once it succeeded, and
            // the grid is only reset when it was showing THIS collection: deleting an empty
            // collection while browsing a library folder must leave that folder on screen.
            if (!AssetDb::deleteCollection(collId)) return; // logged
            QStandardItem *collItem = dirModel->itemFromIndex(proxyModel->mapToSource(proxyIndex));
            if (collItem && collItem->parent()) collItem->parent()->removeRow(collItem->row());
            if (m_currentFolderPath == folderPath) resetGridView();
        } else if (handleExpandCollapse(selectedAction, expandActions, proxyIndex)) {
            return;
        } else if (selectedAction == newSubCollAction) {
            int cid = createCollection(newCollectionName(tr("New Collection"), collId), collId);
            if (cid > 0) navigateToCollectionNode(cid, true);
        } else if (manageFoldersAction && selectedAction == manageFoldersAction) {
            emit manageAssetFoldersRequested();
        } else if (selectedAction == refreshAction) {
            refreshAssetManager();
        }

        return;
    }

    // =========================================================================
    // STANDARD MENU: PHYSICAL HARD DRIVE DIRECTORIES
    // =========================================================================
    // A folder row lives either under a library root or under Search Results (Collections
    // hold no folder rows), so "outside the library tree" here means a search result.
    QStandardItem *clickedItem = dirModel->itemFromIndex(proxyModel->mapToSource(proxyIndex));
    const bool inCollectionOrSearch = (contextForTreeItem(clickedItem) != BrowseContext::Library);

    QMenu* contextMenu = newContextMenu(this);

    QAction *findInLibraryAction = nullptr;
    QAction *browseAction = nullptr;

    if (inCollectionOrSearch) {
        std::tie(findInLibraryAction, browseAction) = addFindAndBrowseActions(contextMenu);
        contextMenu->addSeparator();
    }

    QMenu *addToCollMenu = buildAddToCollectionMenu(contextMenu, folderPath);
    contextMenu->addAction(addToCollMenu->menuAction());
    contextMenu->addSeparator();

    if (!inCollectionOrSearch) {
        browseAction = contextMenu->addAction(icon(QStringLiteral("browse-folder")), tr("Browse Folder"));
        contextMenu->addSeparator();
    }

    // Packages (assetstore.h): this folder zipped with everything in it, or a package unpacked into it.
    QAction *exportPackageAction = contextMenu->addAction(icon(QStringLiteral("export")), tr("Export Package..."));
    QAction *installPackageAction = contextMenu->addAction(icon(QStringLiteral("import")), tr("Install Package..."));
    contextMenu->addSeparator();

    const ExpandActions expandActions = addExpandCollapseActions(contextMenu, proxyIndex, true);
    contextMenu->addSeparator();
    const auto [manageFoldersAction, refreshAction] = addManageRefreshActions(contextMenu);

    QAction *selectedAction = contextMenu->exec(globalPos);
    contextMenu->deleteLater();

    if (findInLibraryAction && selectedAction == findInLibraryAction)
        navigateToFolderInTree(folderPath);
    else if (handleExpandCollapse(selectedAction, expandActions, proxyIndex)) return;
    else if (selectedAction == browseAction) QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    else if (selectedAction == exportPackageAction) exportPackageOf(folderPath);
    else if (selectedAction == installPackageAction) installPackageInto(folderPath);
    else if (manageFoldersAction && selectedAction == manageFoldersAction) emit manageAssetFoldersRequested();
    else if (selectedAction == refreshAction) refreshAssetManager();
}

// =============================================================================
// [ GRID CONTEXT MENUS ]
// =============================================================================

/**
 * @brief Constructs and routes right-click context menus for the asset grid.
 */
void AssetManagerWidget::onGridContextMenuRequested(const QPoint &pos) {
    // A right-click mid-drag already cancelled the drag (AssetGridDragController); should a
    // platform deliver the menu request first, don't open a menu over a live drag.
    if (m_gridDrag->dragging()) return;

    QListWidgetItem *item = assetListWidget->itemAt(pos);
    const QPoint globalPos = assetListWidget->viewport()->mapToGlobal(pos);

    // =========================================================================
    // DEDICATED MENU: FOLDER ITEM
    // =========================================================================
    if (item && item->data(AssetGridRole::ItemKind).toString() == kFolderItemKind) {
        const QString folderPath = item->data(AssetGridRole::FilePath).toString();
        const bool inCollectionOrSearch = (contextForTreeItem(currentTreeItem()) != BrowseContext::Library);

        QMenu* folderMenu = newContextMenu(this);

        QAction *openAction = folderMenu->addAction(icon(QStringLiteral("open-item")), tr("Open"));

        QAction *findInLibraryAction = nullptr;
        QAction *browseAction = nullptr;
        if (inCollectionOrSearch)
            std::tie(findInLibraryAction, browseAction) = addFindAndBrowseActions(folderMenu);
        folderMenu->addSeparator();

        QMenu *addMenu = buildAddToCollectionMenu(folderMenu, folderPath);
        folderMenu->addMenu(addMenu);

        if (!inCollectionOrSearch)
            browseAction = folderMenu->addAction(icon(QStringLiteral("browse-folder")), tr("Browse Folder"));
        folderMenu->addSeparator();
        QAction *exportPackageAction = folderMenu->addAction(icon(QStringLiteral("export")), tr("Export Package..."));
        folderMenu->addSeparator();
        QAction *refreshAction = folderMenu->addAction(icon(QStringLiteral("refresh")), tr("Refresh"));

        QAction *selected = folderMenu->exec(globalPos);
        folderMenu->deleteLater();
        if (selected != nullptr && selected == exportPackageAction) {
            exportPackageOf(folderPath);
            return;
        }

        if (selected == openAction) {
            deselectTree();
            displayFolder(folderPath);
        } else if (findInLibraryAction && selected == findInLibraryAction) {
            navigateToFolderInTree(folderPath);
        } else if (selected == browseAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
        } else if (selected == refreshAction) {
            refreshCurrentView();
        }
        return;
    }

    // =========================================================================
    // DEDICATED MENU: EMPTY SPACE (BACKGROUND CLICK IN GRID)
    // =========================================================================
    if (!item) {
        QMenu* emptyMenu = newContextMenu(this);

        // Browse the physical folder currently shown in the grid. Disabled when nothing is
        // displayed, or when the grid is showing a virtual source (Favorites / a Collection,
        // which has no single on-disk folder to open).
        QAction *browseAction = emptyMenu->addAction(icon(QStringLiteral("browse-folder")), tr("Browse Folder"));
        const bool hasPhysicalFolder = !m_currentFolderPath.isEmpty()
            && !AssetNode::isVirtual(m_currentFolderPath)
            && QDir(m_currentFolderPath).exists();
        browseAction->setEnabled(hasPhysicalFolder);

        // Packages: the folder shown, zipped; or a package unpacked into it.
        emptyMenu->addSeparator();
        QAction *exportPackageAction = emptyMenu->addAction(icon(QStringLiteral("export")), tr("Export Package..."));
        QAction *installPackageAction = emptyMenu->addAction(icon(QStringLiteral("import")), tr("Install Package..."));
        exportPackageAction->setEnabled(hasPhysicalFolder);
        installPackageAction->setEnabled(hasPhysicalFolder);

        emptyMenu->addSeparator();
        QAction *refreshAction = emptyMenu->addAction(icon(QStringLiteral("refresh")), tr("Refresh"));
        QAction *selectedAction = emptyMenu->exec(globalPos);
        emptyMenu->deleteLater();

        if (selectedAction == browseAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_currentFolderPath));
        } else if (selectedAction == exportPackageAction) {
            exportPackageOf(m_currentFolderPath);
        } else if (selectedAction == installPackageAction) {
            installPackageInto(m_currentFolderPath);
        } else if (selectedAction == refreshAction) {
            refreshCurrentView();
        }
        return;
    }

    // =========================================================================
    // DEDICATED MENU: SPECIFIC ASSET ITEM
    // =========================================================================
    const QString fullPath = item->data(AssetGridRole::FilePath).toString();
    const QString folderPath = QFileInfo(fullPath).absolutePath();

    QMenu* itemMenu = newContextMenu(this);

    // Action 1: Open
    QAction *openAction = itemMenu->addAction(icon(QStringLiteral("open-item")), tr("Open"));

    // Detect collection context and add collection-specific actions immediately after Open
    QAction *removeFromColAction = nullptr;
    QAction *findInLibraryAction = nullptr;
    QAction *browseAction = nullptr;
    const int currentCollectionId = AssetNode::collectionId(m_currentFolderPath); // -1 outside a collection

    const BrowseContext browseContext = contextForTreeItem(currentTreeItem());
    const bool inFavoritesView = (browseContext == BrowseContext::Favorites);

    // Find In Library / Browse Folder apply to any asset shown outside the plain Library tree
    // (Collections, Search Results, Favorites), not just ones added directly to a collection.
    const bool inCollectionOrSearch = (browseContext != BrowseContext::Library);
    if (inCollectionOrSearch)
        std::tie(findInLibraryAction, browseAction) = addFindAndBrowseActions(itemMenu);

    itemMenu->addSeparator();

    // "Add To Favorites" sits directly above the collection submenu(s) wherever the asset
    // appears outside the Favorites view; the Favorites view gets "Remove From Favorites"
    // further down instead, after the collection entries.
    QAction *addToFavAction = nullptr;
    if (!inFavoritesView)
        addToFavAction = itemMenu->addAction(tr("Add To Favorites"));

    // =========================================================================
    // Action 2: Contextual Collection Management (Add vs Move/Copy)
    // =========================================================================
    QMenu *addMenu = nullptr;
    QMenu *moveMenu = nullptr;
    QMenu *copyMenu = nullptr;

    // If we are NOT in a collection, build the standard "Add" menu
    if (currentCollectionId == -1) {
        addMenu = newContextMenu(itemMenu);
        addMenu->setTitle(tr("Add To Collection"));
        addMenu->setIcon(icon(QStringLiteral("collections")));
    }
    // If we ARE in a collection, build the "Move" and "Copy" menus
    else {
        moveMenu = newContextMenu(itemMenu);
        moveMenu->setTitle(tr("Move To Collection"));
        moveMenu->setIcon(icon(QStringLiteral("collections")));

        copyMenu = newContextMenu(itemMenu);
        copyMenu->setTitle(tr("Copy To Collection"));
        copyMenu->setIcon(icon(QStringLiteral("collections")));
    }

    const QList<QPair<int, QString>> collections = collectionPathList();
    bool hasOtherCollections = false; // Tracks if there are valid destinations to move/copy to

    // "New Collection" always at the top of the Add menu
    if (currentCollectionId == -1) {
        QAction *newCollAction = addMenu->addAction(tr("New Collection"));
        connect(newCollAction, &QAction::triggered, this, [this, fullPath]() {
            int cid = createCollection(newCollectionName(tr("New Collection"), 0), 0);
            if (cid > 0 && addAssetToCollection(fullPath, cid)) navigateToCollectionNode(cid, true);
        });
    }

    bool hasExistingForAdd = false;
    for (const auto& [colId, colPath] : collections) {
        if (currentCollectionId == -1) {
            if (!hasExistingForAdd) { addMenu->addSeparator(); hasExistingForAdd = true; }
            // STANDARD ADD
            QAction *addAction = addMenu->addAction(colPath);
            connect(addAction, &QAction::triggered, this, [this, colId, fullPath]() {
                if (addAssetToCollection(fullPath, colId)) navigateToCollectionAssetItem(colId, fullPath);
            });
        } else {
            // MOVE & COPY (Exclude the collection the user is currently standing in!)
            if (colId != currentCollectionId) {
                hasOtherCollections = true;

                // Move: remove from the current collection + add to the new one in ONE
                // transaction, then refresh the grid so it visibly leaves the current view.
                QAction *moveAction = moveMenu->addAction(colPath);
                connect(moveAction, &QAction::triggered, this, [this, fullPath, currentCollectionId, colId]() {
                    AssetDb::Filing from; from.collectionId = currentCollectionId;
                    AssetDb::Filing to;   to.collectionId = colId;
                    if (!AssetDb::moveItem(fullPath, from, to)) return; // logged; nothing changed
                    refreshCollectionNode(currentCollectionId);
                    refreshCollectionNode(colId);
                    refreshCurrentView();
                });

                // Copy: just add to the new one (no refresh needed, it stays in the current view)
                QAction *copyAction = copyMenu->addAction(colPath);
                connect(copyAction, &QAction::triggered, this, [this, fullPath, colId]() {
                    addAssetToCollection(fullPath, colId);
                });
            }
        }
    }

    // Attach the appropriate menus to the context list and grey them out if empty
    if (currentCollectionId == -1) {
        itemMenu->addMenu(addMenu);
    } else {
        if (!hasOtherCollections) {
            moveMenu->setEnabled(false);
            copyMenu->setEnabled(false);
        }
        itemMenu->addMenu(moveMenu);
        itemMenu->addMenu(copyMenu);
    }

    // Action 3: Browse Folder (already added at the top when inside Collections/Search Results)
    if (!inCollectionOrSearch)
        browseAction = itemMenu->addAction(icon(QStringLiteral("browse-folder")), tr("Browse Folder"));

    if (currentCollectionId != -1)
        removeFromColAction = itemMenu->addAction(icon(QStringLiteral("unfavorite")), tr("Remove From Collection"));

    // The Favorites view's counterpart of "Add To Favorites" above: removal, placed after the
    // collection entries like "Remove From Collection".
    QAction *removeFromFavAction = nullptr;
    if (inFavoritesView)
        removeFromFavAction = itemMenu->addAction(icon(QStringLiteral("unfavorite")), tr("Remove From Favorites"));

    // An asset of PoseStudio's own (a saved material): its file operations (assetstore.h).
    const FileActions fileActions = addFileActions(itemMenu, fullPath);

    itemMenu->addSeparator();

    // Action 4: Refresh
    QAction *refreshAction = itemMenu->addAction(icon(QStringLiteral("refresh")), tr("Refresh"));

    // --- Execute Menu & Handle Clicks ---
    QAction *selectedAction = itemMenu->exec(globalPos);
    itemMenu->deleteLater();

    if (handleFileAction(selectedAction, fileActions, fullPath)) {
        return;
    }
    if (selectedAction == openAction) {
        // What a double-click does for an asset PoseStudio opens itself (a saved material is
        // applied); anything else goes to the application that owns it.
        if (AssetStore::isOwnAsset(fullPath)) openAsset(fullPath);
        else QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
    }
    else if (removeFromColAction && selectedAction == removeFromColAction) {
        if (removeAssetFromCollection(fullPath, currentCollectionId)) refreshCurrentView();
    }
    else if (addToFavAction && selectedAction == addToFavAction) {
        AssetDb::addFavorite(fullPath);
    }
    else if (removeFromFavAction && selectedAction == removeFromFavAction) {
        if (AssetDb::removeFavorite(fullPath)) refreshCurrentView(); // instantly drop it from the Favorites grid
    }
    else if (findInLibraryAction && selectedAction == findInLibraryAction) {
        navigateToFolderInTree(folderPath);
    }
    else if (selectedAction == browseAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    }
    else if (selectedAction == refreshAction) {
        refreshCurrentView();
    }
}
