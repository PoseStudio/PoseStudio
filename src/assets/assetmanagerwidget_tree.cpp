/**
 * @file assetmanagerwidget_tree.cpp
 * @brief AssetManagerWidget, the TREE's translation unit: building and refreshing the tree
 *        (sections, collections, library roots; what was expanded, selected and shown is kept
 *        across a rebuild), lazy-loading a folder's children on expand, the search, and
 *        navigation — selecting a row for a folder, a library root or a collection.
 *
 * Split out of assetmanagerwidget.cpp for size: it is the same class, with full access to its
 * state. What a selected row SHOWS is the grid's business and stays there (displayFolder,
 * displayTreeIndex).
 */

#include "assetmanagerwidget.h"
#include "assetdb.h"
#include "assetfolderproxymodel.h"
#include "assetlibraries.h"
#include "assetnodeids.h"
#include "assetscan.h"
#include "assettreeview.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QStandardItem>
#include <QStandardItemModel>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
// Tree item for a Collection node. Sorts case-insensitively: QStandardItem's default operator<
// is case-sensitive (uppercase first, "Zebra" before "apple"), which contradicted both the SQL's
// COLLATE NOCASE ordering and every other sort in the widget whenever sortChildren() re-sorted
// after a rename/create/reparent.
class CollectionItem : public QStandardItem {
public:
    using QStandardItem::QStandardItem;
    bool operator<(const QStandardItem& other) const override {
        return text().compare(other.text(), Qt::CaseInsensitive) < 0;
    }
};

// A row of the Search Results that is a note, not a result ("Searching...", "(No results)"): it
// carries no folder and takes no click.
QStandardItem* searchNoteRow(const QString& text) {
    QStandardItem* row = new QStandardItem(text);
    row->setFlags(Qt::NoItemFlags);
    return row;
}
}

// =============================================================================
// [ TREE BUILD / REFRESH ]
// =============================================================================

QModelIndex AssetManagerWidget::searchRootIndex() const {
    return searchResultsRootItem ? proxyModel->mapFromSource(dirModel->indexFromItem(searchResultsRootItem))
                                 : QModelIndex();
}

/**
 * @brief Recursively captures the unique paths of all currently expanded nodes (the branch
 *        under `except`, when given, is passed over).
 */
void AssetManagerWidget::saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths,
                                           const QModelIndex &except) {
    int childCount = proxyModel->rowCount(parentProxyIndex);
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        if (except.isValid() && childProxyIndex == except) continue;

        if (dirTreeView->isExpanded(childProxyIndex)) {
            QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();
            if (!path.isEmpty()) {
                expandedPaths.insert(path);
                // Dive deeper into this expanded branch
                saveExpandedState(childProxyIndex, expandedPaths, except);
            }
        }
    }
}

/**
 * @brief Recursively walks the tree, triggering lazy-loads and re-expanding saved paths (the
 *        branch under `except`, when given, is passed over).
 */
void AssetManagerWidget::restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths,
                                              const QModelIndex &except) {
    int childCount = proxyModel->rowCount(parentProxyIndex);
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        if (except.isValid() && childProxyIndex == except) continue;
        QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();

        if (expandedPaths.contains(path)) {
            // Expanding it synchronously triggers onTreeExpanded, loading its children!
            dirTreeView->expand(childProxyIndex);

            // Now that the children exist, dive deeper
            restoreExpandedState(childProxyIndex, expandedPaths, except);
        }
    }
}

/**
 * @brief Recursively searches the loaded tree under `parentProxyIndex` for the node whose id
 *        (folder path or virtual node id) matches `targetPath` under the module's case policy
 *        (the branch under `except`, when given, is passed over).
 */
QModelIndex AssetManagerWidget::findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath,
                                                     const QModelIndex &except) {
    int childCount = proxyModel->rowCount(parentProxyIndex);

    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        if (except.isValid() && childProxyIndex == except) continue;
        QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();

        if (path.compare(targetPath, AssetScan::kPathCase) == 0) {
            return childProxyIndex; // Found it!
        }

        // If this node has children (and was initialized), search them recursively
        if (proxyModel->hasChildren(childProxyIndex)) {
            QModelIndex result = findProxyIndexByPath(childProxyIndex, targetPath, except);
            if (result.isValid()) return result;
        }
    }

    return QModelIndex(); // Return an invalid index if not found
}

QStringList AssetManagerWidget::currentLibraryRoots() const {
    return m_scope.hasOwnRoots() ? m_scope.roots() : AssetLibraries::enabledPaths();
}

QModelIndex AssetManagerWidget::libraryRootIndex(const QString& libraryPath) const {
    const int rootRows = proxyModel->rowCount(QModelIndex());
    for (int r = 0; r < rootRows; ++r) {
        const QModelIndex candidate = proxyModel->index(r, 0, QModelIndex());
        if (AssetScan::samePath(proxyModel->data(candidate, Qt::UserRole).toString(), libraryPath))
            return candidate;
    }
    return QModelIndex();
}

/**
 * @brief Clears the directory tree and completely rebuilds it from the SQLite database and the
 *        disk — and puts back what the user had on screen: the expanded rows, the selected row,
 *        the source the grid shows (however it was reached) and an active search's results.
 *
 * It runs by itself as well as from the Refresh entries — after a material is saved, after a
 * file operation here or in the other library tab, when the libraries change in Preferences —
 * so it must never cost the user their place.
 */
void AssetManagerWidget::refreshAssetManager() {
    // =========================================================================
    // 1. CAPTURE STATE BEFORE WIPE
    // =========================================================================
    // The Search Results rows are folders the library rows show too, so what is expanded and
    // selected is remembered per branch: looked up by path alone, a row selected in a library
    // would come back as the search result of the same folder.
    const QModelIndex oldSearchRoot = searchRootIndex(); // (invalid before the first build)
    QSet<QString> expandedPaths;
    // We pass an invalid QModelIndex() to represent the invisible root of the entire tree
    saveExpandedState(QModelIndex(), expandedPaths, oldSearchRoot);
    QSet<QString> expandedResults;
    const bool resultsExpanded = oldSearchRoot.isValid() && dirTreeView->isExpanded(oldSearchRoot);
    if (resultsExpanded) saveExpandedState(oldSearchRoot, expandedResults);

    // Capture the currently selected item's path so it can be re-selected after the rebuild
    QString selectedPath;
    bool selectedInResults = false;
    QModelIndex currentIndex = dirTreeView->currentIndex();
    if (currentIndex.isValid()) {
        selectedPath = proxyModel->data(currentIndex, Qt::UserRole).toString();
        selectedInResults = (contextForTreeItem(currentTreeItem()) == BrowseContext::SearchResults);
    }

    // What the grid shows. The selected row brings it back when there is one — but a folder
    // opened from the grid, from the breadcrumb or by a script link has no row selected.
    const QString shownSource = m_currentFolderPath;

    // An active search keeps its results as they are: the rows are taken out of the model before
    // it is cleared and put back below. (Searching again would walk every library once more.)
    const bool searchWasActive = m_searchActive;
    QList<QStandardItem*> keptResults;
    if (searchWasActive && searchResultsRootItem) {
        while (searchResultsRootItem->rowCount() > 0)
            keptResults.append(searchResultsRootItem->takeRow(0).value(0));
    }

    dirModel->clear();
    resetGridView();

    // ---------------------------------------------------------
    // 2. Build Search Results Root (always first in tree)
    // ---------------------------------------------------------
    searchResultsRootItem = new QStandardItem(tr("Search Results"));
    searchResultsRootItem->setData(AssetNode::SEARCH_ROOT, Qt::UserRole);
    searchResultsRootItem->setFlags(searchResultsRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(searchResultsRootItem);

    // Separator between Search Results and Favorites: both stay hidden together until a
    // search is actually active (see updateSearchVisibility), so the panel doesn't waste
    // space on search UI when it isn't in use.
    searchSeparatorItem = new QStandardItem();
    searchSeparatorItem->setData(AssetNode::SEPARATOR, Qt::UserRole);
    searchSeparatorItem->setFlags(Qt::NoItemFlags);
    dirModel->appendRow(searchSeparatorItem);

    // ---------------------------------------------------------
    // 3. Build Favorites Root (a flat container of favorited asset items; no children in the
    //    tree: clicking it lists those items in the grid)
    // ---------------------------------------------------------
    favoritesRootItem = new QStandardItem(tr("Favorites"));
    favoritesRootItem->setData(AssetNode::FAVORITES_ROOT, Qt::UserRole);
    favoritesRootItem->setFlags(favoritesRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(favoritesRootItem);

    // ---------------------------------------------------------
    // 4. Rebuild Collections Root
    // ---------------------------------------------------------
    collectionsRootItem = new QStandardItem(tr("Collections"));
    collectionsRootItem->setData(AssetNode::COLLECTIONS_ROOT, Qt::UserRole);
    collectionsRootItem->setFlags(collectionsRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(collectionsRootItem);

    loadCollectionsTree();

    // ---------------------------------------------------------
    // 5. Build Visual Separator
    // ---------------------------------------------------------
    QStandardItem *separatorItem = new QStandardItem();
    separatorItem->setData(AssetNode::SEPARATOR, Qt::UserRole);
    separatorItem->setFlags(Qt::NoItemFlags);
    dirModel->appendRow(separatorItem);

    // ---------------------------------------------------------
    // 6. Load Physical Asset Libraries
    // ---------------------------------------------------------
    // Already in display order (built-in first, then user libraries by folder NAME; or the
    // scope's own roots), and read once here for every later consumer (search, breadcrumb,
    // Find In Library).
    const QStringList roots = currentLibraryRoots();
    const bool rootsChanged = (roots != m_libraryPaths);
    m_libraryPaths = roots;

    bool anyLibraryAdded = false;
    for (const QString& path : std::as_const(m_libraryPaths)) {
        if (QDir(path).exists()) {
            QStandardItem *rootItem = new QStandardItem(AssetScan::folderDisplayName(path));
            rootItem->setData(path, Qt::UserRole);
            rootItem->setFlags(rootItem->flags() & ~Qt::ItemIsEditable);

            if (AssetScan::hasSubdirectories(path)) {
                rootItem->appendRow(new QStandardItem("..."));
            }
            dirModel->appendRow(rootItem);
            anyLibraryAdded = true;
        } else {
            qWarning() << "Library path does not exist on disk:" << path;
        }
    }
    // "Add Asset Folder" is an offer to register a library: a browser with roots of its own
    // has none to register.
    addLibraryHintLabel->setVisible(!anyLibraryAdded && !m_scope.hasOwnRoots());

    // Search Results (and its separator) stay hidden until a search is actually run; a search
    // that was on show comes back with the results it had, less the folders that have gone or
    // are no longer in a library.
    if (searchWasActive) {
        QList<QStandardItem*> results;
        for (QStandardItem* row : std::as_const(keptResults)) {
            if (!row) continue;
            const QString folder = row->data(Qt::UserRole).toString();
            const bool inLibrary = std::any_of(m_libraryPaths.cbegin(), m_libraryPaths.cend(), [&folder](const QString& root) {
                return AssetScan::folderWithinLibrary(folder, root);
            });
            if (!folder.isEmpty() && inLibrary && QDir(folder).exists()) results.append(row);
            else delete row; // (a note row, or a result that is no longer there)
        }
        if (results.isEmpty()) results.append(searchNoteRow(tr("(No results)")));
        searchResultsRootItem->appendRows(results);
    }
    updateSearchVisibility(searchWasActive);

    // =========================================================================
    // 7. RESTORE STATE AFTER WIPE
    // =========================================================================
    const QModelIndex searchRoot = searchRootIndex();
    if (!expandedPaths.isEmpty()) {
        restoreExpandedState(QModelIndex(), expandedPaths, searchRoot);
    }
    if (resultsExpanded) {
        dirTreeView->expand(searchRoot);
        restoreExpandedState(searchRoot, expandedResults);
    }

    // Re-select whatever was selected before the rebuild, if it still exists (in the branch it
    // was selected in)
    if (!selectedPath.isEmpty()) {
        QModelIndex newSelectedIndex;
        if (selectedPath == AssetNode::SEARCH_ROOT) newSelectedIndex = searchRoot; // the section's own row
        else if (selectedInResults) newSelectedIndex = findProxyIndexByPath(searchRoot, selectedPath);
        else newSelectedIndex = findProxyIndexByPath(QModelIndex(), selectedPath, searchRoot);
        if (newSelectedIndex.isValid()) selectTreeIndex(newSelectedIndex);
    }

    // The grid shows what it showed. The selection has brought it back where a row was selected
    // (selectTreeIndex); otherwise it is shown again as long as it is still there — a folder on
    // disk, the Favorites, a collection that still has its row.
    if (m_currentFolderPath.isEmpty() && !shownSource.isEmpty()) {
        const bool stillThere = AssetNode::isCollection(shownSource)
            ? findCollectionTreeItem(AssetNode::collectionId(shownSource)) != nullptr
            : (shownSource == AssetNode::FAVORITES_ROOT || QDir(shownSource).exists());
        if (stillThere) displayFolder(shownSource);
    }

    // A browser whose whole library is ONE root of its own opens that root: there is nothing
    // else to choose. When the tree is first built, and again if the root itself has moved (the
    // user's library registered somewhere else) — never on an ordinary refresh, which keeps the
    // tree as the user left it. The grid follows only once the tab has been seen (showEvent).
    if (m_scope.hasOwnRoots() && rootsChanged && m_libraryPaths.size() == 1) {
        const QModelIndex root = libraryRootIndex(m_libraryPaths.first());
        if (root.isValid()) {
            dirTreeView->expand(root);
            if (m_shownOnce) selectTreeIndex(root);
        }
    }
}

void AssetManagerWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);

    // Roots of its own are resolved, not registered: they may have moved while the tab was
    // hidden (a library added or removed elsewhere). Rebuild only when they really did.
    if (m_scope.hasOwnRoots() && currentLibraryRoots() != m_libraryPaths) refreshAssetManager();

    if (!m_shownOnce) {
        m_shownOnce = true;
        if (m_scope.hasOwnRoots() && m_libraryPaths.size() == 1 && m_currentFolderPath.isEmpty())
            navigateToLibraryRoot(m_libraryPaths.first());
    }

    // Favorites are one list for every browser: if another one changed it while this one was
    // showing its Favorites, the grid on display is stale.
    if (m_currentFolderPath == AssetNode::FAVORITES_ROOT
        && m_favoritesRevisionShown != AssetDb::favoritesRevision()) {
        refreshCurrentView();
    }
}

void AssetManagerWidget::loadCollectionsTree() {
    // One query for the whole table; nest in memory. Rows arrive name-sorted, so each parent's
    // children are appended in display order without a per-level ORDER BY round trip.
    const QList<AssetDb::CollectionRow> rows = AssetDb::allCollections(m_scope.collections);
    QHash<int, QList<const AssetDb::CollectionRow*>> childrenOf;
    for (const AssetDb::CollectionRow& row : rows) childrenOf[row.parentId].append(&row);

    const std::function<void(QStandardItem*, int)> build = [&](QStandardItem* parentItem, int parentId) {
        for (const AssetDb::CollectionRow* row : childrenOf.value(parentId)) {
            QStandardItem* colItem = makeCollectionItem(row->id, row->name);
            parentItem->appendRow(colItem);
            build(colItem, row->id); // recurse into this collection's own sub-collections
        }
    };
    build(collectionsRootItem, 0);
}

QStandardItem* AssetManagerWidget::makeCollectionItem(int collectionId, const QString& name) const {
    QStandardItem *colItem = new CollectionItem(name);
    colItem->setData(AssetNode::collectionPath(collectionId), Qt::UserRole);
    colItem->setFlags(colItem->flags() | Qt::ItemIsEditable);
    return colItem;
}

QStandardItem* AssetManagerWidget::findCollectionTreeItem(int collectionId) const {
    // Collection rows live only under the Collections root, so the walk starts there rather
    // than at the tree's root (which would also descend every loaded library folder).
    auto* self = const_cast<AssetManagerWidget*>(this);
    const QModelIndex collRootProxy = proxyModel->mapFromSource(dirModel->indexFromItem(collectionsRootItem));
    const QModelIndex found = self->findProxyIndexByPath(collRootProxy, AssetNode::collectionPath(collectionId));
    return found.isValid() ? dirModel->itemFromIndex(proxyModel->mapToSource(found)) : nullptr;
}

/**
 * @brief Returns every collection as (id, full path) pairs, e.g. (7, "Clothing / Shirts"),
 *        sorted by path, so flat pickers can let the user target any nesting depth directly.
 *        Read from the tree, which mirrors the table exactly, rather than re-queried per menu.
 */
QList<QPair<int, QString>> AssetManagerWidget::collectionPathList() const {
    QList<QPair<int, QString>> result;
    const std::function<void(const QStandardItem*, const QString&)> walk =
        [&](const QStandardItem* parent, const QString& prefix) {
            for (int i = 0; i < parent->rowCount(); ++i) {
                const QStandardItem* child = parent->child(i);
                const int id = AssetNode::collectionId(child->data(Qt::UserRole).toString());
                if (id < 0) continue;
                const QString path = prefix.isEmpty() ? child->text() : prefix + QStringLiteral(" / ") + child->text();
                result.append({id, path});
                walk(child, path);
            }
        };
    if (collectionsRootItem) walk(collectionsRootItem, QString());

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second.compare(b.second, Qt::CaseInsensitive) < 0;
    });
    return result;
}

/**
 * @brief Walks a tree item's ancestor chain to determine whether it lives under the Search
 *        Results, Favorites or Collections branch (Collection rows have no folder children
 *        since 0.1.3, so a tree FOLDER row is never in the Collection context; grid items of
 *        a displayed collection are).
 */
AssetManagerWidget::BrowseContext AssetManagerWidget::contextForTreeItem(QStandardItem* item) const {
    while (item) {
        if (item == collectionsRootItem) return BrowseContext::Collection;
        if (item == searchResultsRootItem) return BrowseContext::SearchResults;
        if (item == favoritesRootItem) return BrowseContext::Favorites;
        item = item->parent();
    }
    return BrowseContext::Library;
}

QStandardItem* AssetManagerWidget::currentTreeItem() const {
    const QModelIndex cur = dirTreeView->currentIndex();
    return cur.isValid() ? dirModel->itemFromIndex(proxyModel->mapToSource(cur)) : nullptr;
}

void AssetManagerWidget::refreshCollectionNode(int collectionId) {
    if (QStandardItem* item = findCollectionTreeItem(collectionId))
        proxyModel->refreshNode(proxyModel->mapFromSource(dirModel->indexFromItem(item)));
}

/**
 * @brief Handles lazy-loading of physical directories to keep memory footprint low.
 */
void AssetManagerWidget::onTreeExpanded(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QStandardItem *item = dirModel->itemFromIndex(sourceIndex);

    if (!item || !item->hasChildren()) return;
    if (item == collectionsRootItem || item == searchResultsRootItem) return;

    // =========================================================================
    // STANDARD PHYSICAL FOLDER LOGIC
    // =========================================================================
    QStandardItem *firstChild = item->child(0);
    if (!firstChild || !firstChild->data(Qt::UserRole).toString().isEmpty() || firstChild->text() != "...") return;
    item->removeRow(0);

    const QString parentPath = item->data(Qt::UserRole).toString();
    const QFileInfoList list = AssetScan::listSubdirectories(parentPath);

    QList<QStandardItem*> children;
    children.reserve(list.size());

    for (const QFileInfo& info : list) {
        QStandardItem* child = new QStandardItem(info.fileName());
        child->setData(info.absoluteFilePath(), Qt::UserRole);
        child->setFlags(child->flags() & ~Qt::ItemIsEditable);

        if (AssetScan::hasSubdirectories(info.absoluteFilePath())) child->appendRow(new QStandardItem("..."));

        children.append(child);
    }

    dirTreeView->setUpdatesEnabled(false);
    if (!children.isEmpty()) item->appendRows(children);
    dirTreeView->setUpdatesEnabled(true);
}

/**
 * @brief Triggered when a user clicks a node in the directory tree. Processes the click and populates the grid.
 */
void AssetManagerWidget::onFolderSelected(const QModelIndex &proxyIndex) {
    const QString folderPath = proxyIndex.data(Qt::UserRole).toString();

    // Section headers toggle on click instead of showing anything.
    if (folderPath == AssetNode::COLLECTIONS_ROOT || folderPath == AssetNode::SEARCH_ROOT) {
        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        dirTreeView->setExpanded(proxyIndex, !isExpanded);
        return;
    }

    displayTreeIndex(proxyIndex);
}

void AssetManagerWidget::onTreeCurrentChanged(const QModelIndex &current) {
    if (!current.isValid() || m_selectingProgrammatically) return;
    // A mouse press moves the current row before `clicked` fires on release (which populates
    // the grid); only follow current-changes that arrive with no button down, i.e. keyboard
    // navigation. Section headers are left alone: an arrow key must not toggle them.
    if (QApplication::mouseButtons() != Qt::NoButton) return;
    displayTreeIndex(current);
}

// =============================================================================
// [ SEARCH ]
// =============================================================================

/**
 * @brief Shows or hides the Search Results root and its separator together, so the panel
 *        doesn't reserve space for search UI while no search is active.
 */
void AssetManagerWidget::updateSearchVisibility(bool active) {
    m_searchActive = active;
    const auto setHidden = [this](QStandardItem* item, bool hidden) {
        if (!item) return;
        const QModelIndex proxyIdx = proxyModel->mapFromSource(dirModel->indexFromItem(item));
        dirTreeView->setRowHidden(proxyIdx.row(), proxyIdx.parent(), hidden);
    };
    setHidden(searchResultsRootItem, !active);
    setHidden(searchSeparatorItem, !active);
}

/**
 * @brief Searches every enabled asset library for files/folders matching the query and lists
 *        the results as a flat set of paths under the Search Results root node. Folders that
 *        only have matches in a subfolder (no direct hit) are skipped in favor of that subfolder,
 *        and any result that's itself nested under another result is dropped as redundant.
 *        Synchronous by design (the results are classified as they are found); the
 *        classification itself lists each folder once (AssetScan::classifyFolder).
 */
void AssetManagerWidget::runSearch(const QString& query) {
    searchResultsRootItem->removeRows(0, searchResultsRootItem->rowCount());
    updateSearchVisibility(!query.isEmpty());

    const QModelIndex searchProxyIdx = proxyModel->mapFromSource(
        dirModel->indexFromItem(searchResultsRootItem));

    if (query.isEmpty()) {
        if (searchProxyIdx.isValid()) dirTreeView->collapse(searchProxyIdx);
        return;
    }

    // Show "Searching..." immediately and let Qt repaint before the blocking scan
    searchResultsRootItem->appendRow(searchNoteRow(tr("Searching...")));
    if (searchProxyIdx.isValid()) {
        dirTreeView->expand(searchProxyIdx);
        dirTreeView->scrollTo(searchProxyIdx, QAbstractItemView::PositionAtTop);
    }
    searchButton->setEnabled(false);
    clearSearchButton->setEnabled(false);
    searchInput->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // Collect results into a flat list before touching the model
    QList<QStandardItem*> resultNodes;
    QSet<QString> addedFolders;   // direct-hit leaf folders already emitted as results (dedup)
    QSet<QString> seenFolders;    // match folders already expanded; avoids re-walking on sibling matches

    for (const QString& libPath : std::as_const(m_libraryPaths)) {
        const QDir libDir(libPath);
        if (!libDir.exists()) continue;

        QDirIterator it(libPath, AssetScan::kEntryFilter, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            if (!fi.fileName().contains(query, Qt::CaseInsensitive)) continue;
            // (What is hidden — a dot name, or anything inside a folder with one — is not found:
            // core/hiddennames.h. An asset's texture store, `.textures`, is such a folder.)
            if (HiddenNames::inHiddenPath(fi.absoluteFilePath(), libPath)) continue;

            const QString folderMatch = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

            // A folder with many matching files (or a matching folder name) only needs to be
            // expanded once; skip it on every subsequent hit. Indirect-hit folders never land
            // in addedFolders (only their direct-hit leaves do), so this is the guard that
            // actually prevents the redundant re-walk.
            if (seenFolders.contains(folderMatch)) continue;
            seenFolders.insert(folderMatch);

            // If an ancestor of this folder was already emitted as a direct-hit result, this folder
            // and everything under it is already reachable by expanding that ancestor in the tree:
            // skip processing its subtree entirely instead of collecting hits only to discard them
            // in the dedup pass below. (That dedup remains the order-independent safety net.)
            if (AssetScan::hasAncestorIn(folderMatch, addedFolders)) continue;

            // collectDirectHits classifies folderMatch (direct/indirect/no hit) and recurses
            // only where needed, so a separate hasHit() pre-check would just walk the subtree twice.
            collectDirectHits(folderMatch, libDir, addedFolders, resultNodes);
        }
    }

    // Drop any result that is itself a subfolder of another result already in the list:
    // it's already reachable by expanding that ancestor in the tree. Walk each result's
    // ancestor chain and test membership in a set: O(results * depth) with O(1) lookups,
    // versus the previous O(results^2) pairwise prefix compare.
    {
        QSet<QString> pathSet;
        pathSet.reserve(resultNodes.size());
        for (QStandardItem* item : resultNodes)
            pathSet.insert(item->data(Qt::UserRole).toString());

        for (int i = resultNodes.size() - 1; i >= 0; --i) {
            if (AssetScan::hasAncestorIn(resultNodes[i]->data(Qt::UserRole).toString(), pathSet))
                delete resultNodes.takeAt(i);
        }
    }

    // Replace "Searching..." with real results
    searchResultsRootItem->removeRows(0, searchResultsRootItem->rowCount());

    if (resultNodes.isEmpty()) {
        searchResultsRootItem->appendRow(searchNoteRow(tr("(No results)")));
    } else {
        searchResultsRootItem->appendRows(resultNodes);
    }

    QApplication::restoreOverrideCursor();
    searchButton->setEnabled(true);
    clearSearchButton->setEnabled(true);
    searchInput->setEnabled(true);

    // The root's icon/color reflect whether there are results now.
    proxyModel->refreshNode(searchProxyIdx);

    if (searchProxyIdx.isValid()) {
        dirTreeView->expand(searchProxyIdx);
        dirTreeView->scrollTo(searchProxyIdx, QAbstractItemView::PositionAtTop);
    }
}

// Adds folderPath as a result if it is a DirectHit.
// If it is an IndirectHit, recursively descends into its children until DirectHit leaves are found.
void AssetManagerWidget::collectDirectHits(const QString& folderPath, const QDir& libRootDir,
                                            QSet<QString>& added, QList<QStandardItem*>& results) {
    if (added.contains(folderPath)) return;

    if (proxyModel->isDirectHit(folderPath)) {
        added.insert(folderPath);
        // (A hit in the library's ROOT folder itself has the relative path "." — it is shown by
        // the root's name, as the tree shows it.)
        const QString relativePath = libRootDir.relativeFilePath(folderPath);
        QStandardItem *item = new QStandardItem(relativePath == QLatin1String(".") ? AssetScan::folderDisplayName(folderPath)
                                                                                    : AssetScan::truncateSearchPath(relativePath));
        item->setData(folderPath, Qt::UserRole);
        item->setData(true, AssetNode::SearchResult); // the delegate greys the path prefix
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (AssetScan::hasSubdirectories(folderPath)) item->appendRow(new QStandardItem("..."));
        results.append(item);
    } else if (proxyModel->hasHit(folderPath)) {
        // IndirectHit: don't add this folder; recurse into children that have hits
        const QFileInfoList children = AssetScan::listSubdirectories(folderPath);
        for (const QFileInfo& child : children)
            collectDirectHits(child.absoluteFilePath(), libRootDir, added, results);
    }
}

// =============================================================================
// [ NAVIGATION ]
// =============================================================================

/**
 * @brief Clears the tree's selection and current-index so no stale folder appears
 *        highlighted after navigating away from it by some means other than a tree click
 *        (breadcrumb link, grid folder double-click/Open, etc.).
 */
void AssetManagerWidget::deselectTree() {
    m_selectingProgrammatically = true;
    dirTreeView->clearSelection();
    dirTreeView->setCurrentIndex(QModelIndex());
    m_selectingProgrammatically = false;
}

void AssetManagerWidget::selectTreeIndex(const QModelIndex& proxyIndex) {
    if (!proxyIndex.isValid()) return;
    m_selectingProgrammatically = true;
    dirTreeView->setCurrentIndex(proxyIndex); // highlights it in the tree
    dirTreeView->scrollTo(proxyIndex, QAbstractItemView::PositionAtCenter);
    m_selectingProgrammatically = false;
    displayTreeIndex(proxyIndex);             // populates the grid below
}

/**
 * @brief Navigates the tree to the physical folder that contains a given asset.
 * @details Walks down the tree expanding lazy-loaded nodes segment by segment,
 *          starting from the physical library root that contains the target path.
 */
void AssetManagerWidget::navigateToFolderInTree(const QString& folderPath) {
    const QString normTarget = AssetScan::normalizePath(folderPath);

    // Shared helper: expand startIdx then walk each segment, returning the final index.
    // Returns an invalid index if any segment is not found.
    auto walkSegments = [this](QModelIndex startIdx, const QStringList& segments) -> QModelIndex {
        QModelIndex cur = startIdx;
        for (const QString& seg : segments) {
            if (!dirTreeView->isExpanded(cur))
                dirTreeView->expand(cur); // fires onTreeExpanded -> populates lazy children

            bool found = false;
            const int n = proxyModel->rowCount(cur);
            for (int c = 0; c < n; ++c) {
                const QModelIndex child = proxyModel->index(c, 0, cur);
                const QString name = child.data(Qt::DisplayRole).toString();
                if (name.compare(seg, AssetScan::kPathCase) == 0) {
                    cur = child;
                    found = true;
                    break;
                }
            }
            if (!found) return QModelIndex();
        }
        return cur;
    };

    for (const QString& libPath : std::as_const(m_libraryPaths)) {
        const QString normLib = AssetScan::normalizePath(libPath);
        // Match assets in a subfolder of the library AND assets that live directly in the
        // library root itself (e.g. the built-in library), where normTarget == normLib.
        if (!AssetScan::folderWithinLibrary(normTarget, normLib)) continue;
        // What lies below the root is walked segment by segment. A drive-root library keeps its
        // trailing slash after cleanPath ("D:/"): the prefix must not double it.
        const QString libPrefix =
            normLib.endsWith(QLatin1Char('/')) ? normLib : normLib + QLatin1Char('/');

        // Find this library's top-level root item in the tree
        const QModelIndex libRootProxy = libraryRootIndex(libPath);
        if (!libRootProxy.isValid()) continue;

        const QStringList segments = normTarget.mid(libPrefix.length()).split('/', Qt::SkipEmptyParts);
        if (segments.isEmpty()) { selectTreeIndex(libRootProxy); return; }

        if (!dirTreeView->isExpanded(libRootProxy))
            dirTreeView->expand(libRootProxy);

        const QModelIndex result = walkSegments(libRootProxy, segments);
        if (result.isValid()) { selectTreeIndex(result); return; }
    }
}

// Selects a library's own top-level row (Preferences -> Assets -> the library's "show" action):
// a no-op when the path isn't a registered, existing library.
void AssetManagerWidget::navigateToLibraryRoot(const QString& libraryPath) {
    selectTreeIndex(libraryRootIndex(libraryPath));
}

void AssetManagerWidget::navigateToCollectionNode(int collectionId, bool enterEditMode) {
    // Expand the Collections root first: a collapsed root's descendants have no visual rows to
    // scroll to or edit.
    const QModelIndex collRootProxy = proxyModel->mapFromSource(dirModel->indexFromItem(collectionsRootItem));
    if (collRootProxy.isValid() && !dirTreeView->isExpanded(collRootProxy))
        dirTreeView->expand(collRootProxy);

    const QModelIndex colProxy = findProxyIndexByPath(collRootProxy, AssetNode::collectionPath(collectionId));
    if (!colProxy.isValid()) return;

    selectTreeIndex(colProxy);

    if (enterEditMode)
        dirTreeView->edit(colProxy);
}

/**
 * @brief Jumps to an asset that was just added to a Collection: selects the collection (which
 *        populates the asset grid below) then finds and highlights the matching grid item.
 */
void AssetManagerWidget::navigateToCollectionAssetItem(int collectionId, const QString& assetFullPath) {
    navigateToCollectionNode(collectionId);

    for (int i = 0; i < assetListWidget->count(); ++i) {
        QListWidgetItem *item = assetListWidget->item(i);
        if (item->data(AssetGridRole::FilePath).toString() == assetFullPath) {
            assetListWidget->setCurrentItem(item);
            assetListWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}
