/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of AssetManagerWidget: setup, tree build/refresh, search, grid display,
 *        navigation, and the collection/favorites operations.
 *
 * The widget is the glue between the tree model, the grid, and the data layer. Two invariants
 * everything here respects: the grid's items are only ever deleted through clearGrid(), which
 * first detaches everything that holds item pointers (the thumbnail loader's jobs, the
 * tooltip's owner item); and every change to a collection's contents goes through a helper
 * that also repaints that collection's tree row (its hit icon is cached by the proxy model).
 * The context menus live in assetmanagerwidget_menus.cpp, a second translation unit of the
 * same class.
 */

#include "assetmanagerwidget.h"
#include "assetbreadcrumblabel.h"
#include "assetdb.h"
#include "assetfolderproxymodel.h"
#include "assetgriddelegate.h"
#include "assetgriddragcontroller.h"
#include "assetlibraries.h"
#include "assetnodeids.h"
#include "assetscan.h"
#include "assetthumbnailloader.h"
#include "assettreedelegate.h"
#include "assettreeview.h"
#include "constants.h"
#include "customtooltip.h"
#include "preferencesmanager.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

using AssetScan::AssetHit;

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

// The name a tree row is shown under in the grid title. Search-result rows display their
// truncated relative path ("a / b / matched"); only for those is the last segment the name.
// A collection the user named "Sci-Fi / Props" keeps its full text.
QString rowDisplayName(const QModelIndex& index) {
    QString name = index.data(Qt::DisplayRole).toString();
    if (index.data(AssetNode::SearchResult).toBool())
        name = name.section(QStringLiteral(" / "), -1);
    return name;
}
}

// =============================================================================
// [ SETUP ]
// =============================================================================

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    // App-level theming (Fusion + AppProxyStyle) and global tooltip effects are configured
    // once in main.cpp, before any widget is built, not here.
    setupUI();
}

AssetManagerWidget::~AssetManagerWidget() = default;

const QIcon& AssetManagerWidget::icon(const QString& name) {
    auto it = m_icons.find(name);
    if (it == m_icons.end())
        it = m_icons.insert(name, QIcon(QStringLiteral(":/resources/icons/%1.png").arg(name)));
    return it.value();
}

/**
 * @brief Initializes the main layout, QSplitters, and primary views.
 */
void AssetManagerWidget::setupUI() {
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6);

    // --- 1. Top Panel: Search bar + Directory & Collections Tree ---
    dirModel = new QStandardItemModel(this);
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    QWidget *topPanel = new QWidget(splitter);
    topPanel->setObjectName("AssetManagerTopPanel");
    QVBoxLayout *topLayout = new QVBoxLayout(topPanel);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    QWidget *searchRow = new QWidget(topPanel);
    searchRow->setObjectName("AssetManagerSearchRow");
    QHBoxLayout *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(6, 5, 6, 5);
    searchLayout->setSpacing(4);

    searchInput = new QLineEdit(searchRow);
    searchInput->setObjectName("AssetManagerSearchInput");
    searchInput->setPlaceholderText(tr("Search assets..."));

    clearSearchButton = new QPushButton(searchRow);
    clearSearchButton->setObjectName("AssetManagerClearButton");
    clearSearchButton->setIcon(icon(QStringLiteral("clear")));
    clearSearchButton->setIconSize(QSize(14, 14));
    clearSearchButton->setFixedSize(26, 26);

    searchButton = new QPushButton(searchRow);
    searchButton->setObjectName("AssetManagerSearchButton");
    searchButton->setIcon(icon(QStringLiteral("search")));
    searchButton->setIconSize(QSize(14, 14));
    searchButton->setFixedSize(26, 26);

    searchLayout->addWidget(searchInput);
    searchLayout->addWidget(clearSearchButton);
    searchLayout->addWidget(searchButton);

    QFrame *searchSeparator = new QFrame(topPanel);
    searchSeparator->setObjectName("AssetManagerSearchSeparator");
    searchSeparator->setFrameShape(QFrame::HLine);
    searchSeparator->setFrameShadow(QFrame::Plain);

    dirTreeView = new AssetTreeView(topPanel);
    dirTreeView->setObjectName("AssetManagerTree");
    dirTreeView->setModel(proxyModel);
    dirTreeView->setItemDelegate(new AssetTreeDelegate(this));
    dirTreeView->setHeaderHidden(true);

    dirTreeView->setEditTriggers(QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    dirTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    // Collection drag-and-drop reparenting (see AssetFolderProxyModel's flags/mimeData/
    // canDropMimeData/dropMimeData overrides for what's actually draggable/droppable).
    dirTreeView->setDragEnabled(true);
    dirTreeView->setAcceptDrops(true);
    dirTreeView->setDropIndicatorShown(false); // we paint our own drop highlight (AssetTreeView)
    dirTreeView->setDragDropMode(QAbstractItemView::InternalMove);
    dirTreeView->setDefaultDropAction(Qt::MoveAction);

    connect(dirTreeView, &QTreeView::customContextMenuRequested, this, &AssetManagerWidget::onContextMenuRequested);
    connect(dirModel, &QStandardItemModel::itemChanged, this, &AssetManagerWidget::onItemChanged);
    // Queued so the reparent (which mutates the model via takeRow/appendRow) runs after Qt's
    // drop event has fully unwound, rather than re-entering the model mid-drop.
    connect(proxyModel, &AssetFolderProxyModel::collectionReparentRequested,
            this, &AssetManagerWidget::reparentCollection, Qt::QueuedConnection);

    topLayout->addWidget(searchRow);
    topLayout->addWidget(searchSeparator);
    topLayout->addWidget(dirTreeView);

    // =====================================================================
    // EMPTY-STATE HINT: shown over the directory tree only while no asset library is configured
    // =====================================================================
    addLibraryHintLabel = new QLabel(dirTreeView->viewport());
    addLibraryHintLabel->setObjectName("AssetManagerAddLibraryHint");
    addLibraryHintLabel->setTextFormat(Qt::RichText);
    // Qt's QSS engine doesn't apply selectors to rich-text <a> links inside a QLabel, so the
    // link colour is inline in the HTML.
    addLibraryHintLabel->setText(tr("<a href=\"#\" style=\"color: #ffffff;\">%1</a>")
                                  .arg(tr("Add Asset Folder")));
    addLibraryHintLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    addLibraryHintLabel->setCursor(Qt::PointingHandCursor);
    addLibraryHintLabel->hide();
    connect(addLibraryHintLabel, &QLabel::linkActivated, this, [this](const QString&) {
        promptAddAssetLibrary();
    });

    // A layout on the viewport centers the hint and keeps it centered across resizes,
    // without interfering with the tree's own item painting (not child widgets).
    QVBoxLayout *hintLayout = new QVBoxLayout(dirTreeView->viewport());
    hintLayout->addWidget(addLibraryHintLabel, 0, Qt::AlignCenter);
    // =====================================================================

    // --- 2. Bottom Panel: Asset Thumbnail Grid ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);

    titleLabel = new AssetBreadcrumbLabel(bottomPanel);
    connect(titleLabel, &AssetBreadcrumbLabel::navigateRequested, this, [this](const QString& path) {
        deselectTree();
        displayFolder(path);
    });
    connect(titleLabel, &AssetBreadcrumbLabel::browseRequested, this, [](const QString& path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    assetListWidget = new QListWidget(bottomPanel);
    assetListWidget->setObjectName("AssetManagerGrid");
    assetListWidget->setViewMode(QListView::IconMode);
    assetListWidget->setIconSize(QSize(Constants::GRID_ICON_DISPLAY_SIZE, Constants::GRID_ICON_DISPLAY_SIZE));

    assetListWidget->setResizeMode(QListView::Adjust);
    assetListWidget->setMovement(QListView::Static);
    assetListWidget->setSpacing(6);
    assetListWidget->setItemDelegate(new AssetGridDelegate(assetListWidget));

    m_thumbnails = new AssetThumbnailLoader(assetListWidget, this);

    // =====================================================================
    // INITIALIZE INTERACTIVE TOOLTIPS
    // =====================================================================
    customToolTip = new CustomToolTip(this);
    activeToolTipItem = nullptr;

    // Force the grid to track mouse movements even when not clicking
    assetListWidget->setMouseTracking(true);
    // Install the filter so we can intercept the ToolTip spawn requests (and feed the drag)
    assetListWidget->viewport()->installEventFilter(this);
    // =====================================================================

    // The hand-rolled asset drag: reorder within a sortable view, or drop onto a tree node.
    m_gridDrag = new AssetGridDragController(assetListWidget, dirTreeView,
                                             [this]() { return isSortableView(); }, this);
    connect(m_gridDrag, &AssetGridDragController::dragStarted, this, [this]() { customToolTip->hide(); });
    connect(m_gridDrag, &AssetGridDragController::reorderCommitted, this, &AssetManagerWidget::persistGridOrder);
    connect(m_gridDrag, &AssetGridDragController::droppedOnTree, this, &AssetManagerWidget::dropAssetOnTreeNode);

    assetListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(assetListWidget, &QListWidget::customContextMenuRequested, this, &AssetManagerWidget::onGridContextMenuRequested);
    connect(assetListWidget, &QListWidget::itemDoubleClicked, this, &AssetManagerWidget::onGridItemDoubleClicked);

    infoBarLabel = new QLabel(bottomPanel);
    infoBarLabel->setObjectName("AssetManagerInfoBar");
    infoBarLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    infoBarLabel->hide(); // nothing displayed yet at startup

    bottomLayout->addWidget(titleLabel);
    bottomLayout->addWidget(assetListWidget);
    bottomLayout->addWidget(infoBarLabel);

    splitter->addWidget(topPanel);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
    // Keyboard navigation (arrow keys) moves the current row without a click; follow it too.
    connect(dirTreeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &AssetManagerWidget::onTreeCurrentChanged);
    connect(dirTreeView, &QTreeView::expanded, this, &AssetManagerWidget::onTreeExpanded);

    connect(searchButton, &QPushButton::clicked, this, [this]() {
        runSearch(searchInput->text().trimmed());
    });
    connect(searchInput, &QLineEdit::returnPressed, searchButton, &QPushButton::click);
    connect(clearSearchButton, &QPushButton::clicked, this, [this]() {
        searchInput->clear();
        runSearch(QString());
    });

    refreshAssetManager();
}

/**
 * @brief Prompts the user for a folder via the native file dialog and registers it as a
 *        new Asset Library, then refreshes the tree so it appears immediately.
 */
void AssetManagerWidget::promptAddAssetLibrary() {
    // Start the browser in the parent of the last folder added (persisted in Preferences), so
    // adding several sibling libraries doesn't mean re-navigating from home each time.
    const QString startDir = PreferencesManager::instance().rememberedDirectory(
        Constants::PREF_LAST_ASSET_FOLDER_PARENT, QDir::homePath());

    const QString folderPath = QFileDialog::getExistingDirectory(
        this, tr("Select Asset Library Folder"), startDir);
    if (folderPath.isEmpty()) return;

    if (AssetLibraries::add(folderPath) == AssetLibraries::AddResult::Failed) return; // logged

    // Remember this folder's parent as the default location for the next add.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_ASSET_FOLDER_PARENT,
                                            QFileInfo(folderPath).absolutePath());
    refreshAssetManager();
}

// =============================================================================
// [ TREE BUILD / REFRESH ]
// =============================================================================

/**
 * @brief Recursively captures the unique paths of all currently expanded nodes.
 */
void AssetManagerWidget::saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths) {
    int childCount = proxyModel->rowCount(parentProxyIndex);
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);

        if (dirTreeView->isExpanded(childProxyIndex)) {
            QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();
            if (!path.isEmpty()) {
                expandedPaths.insert(path);
                // Dive deeper into this expanded branch
                saveExpandedState(childProxyIndex, expandedPaths);
            }
        }
    }
}

/**
 * @brief Recursively walks the tree, triggering lazy-loads and re-expanding saved paths.
 */
void AssetManagerWidget::restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths) {
    int childCount = proxyModel->rowCount(parentProxyIndex);
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();

        if (expandedPaths.contains(path)) {
            // Expanding it synchronously triggers onTreeExpanded, loading its children!
            dirTreeView->expand(childProxyIndex);

            // Now that the children exist, dive deeper
            restoreExpandedState(childProxyIndex, expandedPaths);
        }
    }
}

/**
 * @brief Recursively searches the loaded tree under `parentProxyIndex` for the node whose id
 *        (folder path or virtual node id) matches `targetPath` under the module's case policy.
 */
QModelIndex AssetManagerWidget::findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath) {
    int childCount = proxyModel->rowCount(parentProxyIndex);

    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();

        if (path.compare(targetPath, AssetScan::kPathCase) == 0) {
            return childProxyIndex; // Found it!
        }

        // If this node has children (and was initialized), search them recursively
        if (proxyModel->hasChildren(childProxyIndex)) {
            QModelIndex result = findProxyIndexByPath(childProxyIndex, targetPath);
            if (result.isValid()) return result;
        }
    }

    return QModelIndex(); // Return an invalid index if not found
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
 * @brief Clears the directory tree and completely rebuilds it from the SQLite database.
 */
void AssetManagerWidget::refreshAssetManager() {
    // =========================================================================
    // 1. CAPTURE STATE BEFORE WIPE
    // =========================================================================
    QSet<QString> expandedPaths;
    // We pass an invalid QModelIndex() to represent the invisible root of the entire tree
    saveExpandedState(QModelIndex(), expandedPaths);

    // Capture the currently selected item's path so it can be re-selected after the rebuild
    QString selectedPath;
    QModelIndex currentIndex = dirTreeView->currentIndex();
    if (currentIndex.isValid()) {
        selectedPath = proxyModel->data(currentIndex, Qt::UserRole).toString();
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
    // Already in display order (built-in first, then user libraries by folder NAME), and read
    // once here for every later consumer (search, breadcrumb, Find In Library).
    m_libraryPaths = AssetLibraries::enabledPaths();

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
    addLibraryHintLabel->setVisible(!anyLibraryAdded);

    // Search Results (and its separator) stay hidden until a search is actually run:
    // a freshly rebuilt tree has no active search, regardless of stale text left in the box.
    updateSearchVisibility(false);

    // =========================================================================
    // 7. RESTORE STATE AFTER WIPE
    // =========================================================================
    if (!expandedPaths.isEmpty()) {
        restoreExpandedState(QModelIndex(), expandedPaths);
    }

    // Re-select whatever was selected before the rebuild, if it still exists
    if (!selectedPath.isEmpty()) {
        QModelIndex newSelectedIndex = findProxyIndexByPath(QModelIndex(), selectedPath);
        if (newSelectedIndex.isValid()) selectTreeIndex(newSelectedIndex);
    }
}

void AssetManagerWidget::loadCollectionsTree() {
    // One query for the whole table; nest in memory. Rows arrive name-sorted, so each parent's
    // children are appended in display order without a per-level ORDER BY round trip.
    const QList<AssetDb::CollectionRow> rows = AssetDb::allCollections();
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

void AssetManagerWidget::displayTreeIndex(const QModelIndex& proxyIndex) {
    const QString folderPath = proxyIndex.data(Qt::UserRole).toString();
    if (AssetNode::isStructural(folderPath)
        || folderPath == AssetNode::COLLECTIONS_ROOT || folderPath == AssetNode::SEARCH_ROOT) return;
    displayFolder(folderPath, rowDisplayName(proxyIndex));
}

// =============================================================================
// [ SEARCH ]
// =============================================================================

/**
 * @brief Shows or hides the Search Results root and its separator together, so the panel
 *        doesn't reserve space for search UI while no search is active.
 */
void AssetManagerWidget::updateSearchVisibility(bool active) {
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
    QStandardItem *loadingItem = new QStandardItem(tr("Searching..."));
    loadingItem->setFlags(Qt::NoItemFlags);
    searchResultsRootItem->appendRow(loadingItem);
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

        QDirIterator it(libPath,
                        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            if (!fi.fileName().contains(query, Qt::CaseInsensitive)) continue;

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
        QStandardItem *noResult = new QStandardItem(tr("(No results)"));
        noResult->setFlags(Qt::NoItemFlags);
        searchResultsRootItem->appendRow(noResult);
    } else {
        searchResultsRootItem->appendRows(resultNodes);
    }

    QApplication::restoreOverrideCursor();
    searchButton->setEnabled(true);
    clearSearchButton->setEnabled(true);
    searchInput->setEnabled(true);

    // The root's icon/colour reflect whether there are results now.
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
        QStandardItem *item = new QStandardItem(AssetScan::truncateSearchPath(libRootDir.relativeFilePath(folderPath)));
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
// [ GRID DISPLAY ]
// =============================================================================

/**
 * @brief Rebuilds the footer info bar ("Assets: X   Folders: X   Sortable"), omitting any
 *        segment that isn't relevant to the currently displayed source.
 */
void AssetManagerWidget::refreshInfoBar() {
    if (m_currentFolderPath.isEmpty()) {
        infoBarLabel->hide();
        return;
    }

    QStringList countSegments;
    if (m_currentAssetCount > 0)
        countSegments << tr("Assets: %1").arg(m_currentAssetCount);
    if (m_currentFolderCount > 0)
        countSegments << tr("Folders: %1").arg(m_currentFolderCount);

    const bool isSortable = isSortableView();

    if (countSegments.isEmpty() && !isSortable) {
        infoBarLabel->hide();
        return;
    }

    // An extra space sets "Sortable" apart from the Assets/Folders counts a bit more.
    QString text = countSegments.join(QStringLiteral("   "));
    if (isSortable) {
        if (!text.isEmpty()) text += QStringLiteral("    ");
        text += tr("Sortable");
    }

    infoBarLabel->setText(text);
    infoBarLabel->show();
}

void AssetManagerWidget::clearGrid() {
    m_thumbnails->clear();       // queued/in-flight decodes point at the items about to die
    activeToolTipItem = nullptr; // ditto for the tooltip's item
    m_gridDrag->cancel();        // a drag can't outlive its item either
    assetListWidget->clear();
}

void AssetManagerWidget::resetGridView() {
    clearGrid();
    titleLabel->clearTitle();
    m_currentFolderPath.clear();
    m_currentAssetCount = 0;
    m_currentFolderCount = 0;
    infoBarLabel->hide();
}

void AssetManagerWidget::refreshCurrentView() {
    if (m_currentFolderPath.isEmpty()) refreshAssetManager();
    else displayFolder(m_currentFolderPath);
}

/**
 * @brief Parses and displays assets for a given source. Called by the tree selection, by
 *        breadcrumb link clicks in the title label, and by grid folder items.
 */
void AssetManagerWidget::displayFolder(const QString& folderPath, const QString& title) {
    // "Virtual" sources (Collections, Favorites) hold a DB-backed list of asset items rather than
    // a physical directory: they get a plain title (not a breadcrumb) and no subfolder listing.
    const bool isFavorites = (folderPath == AssetNode::FAVORITES_ROOT);
    const bool isCollection = AssetNode::isCollection(folderPath);
    const bool isVirtual = isFavorites || isCollection;

    QList<AssetHit> discoveredAssets;
    if (isFavorites) {
        discoveredAssets = AssetScan::resolveAssetHits(AssetDb::favoritePaths());
    } else if (isCollection) {
        discoveredAssets = AssetScan::resolveAssetHits(AssetDb::collectionItemPaths(AssetNode::collectionId(folderPath)));
    } else {
        discoveredAssets = AssetScan::scanFolderAssets(folderPath);
    }

    m_currentFolderPath = folderPath;
    m_currentAssetCount = discoveredAssets.size();

    if (isVirtual) {
        QString titleText = title;
        if (titleText.isEmpty()) {
            // No name handed in (a refresh, a drop-move): read it off the tree row.
            if (isFavorites) titleText = tr("Favorites");
            else if (QStandardItem* item = findCollectionTreeItem(AssetNode::collectionId(folderPath)))
                titleText = item->text();
            else titleText = tr("Collection");
        }
        titleLabel->setPlainTitle(titleText);
    } else {
        titleLabel->setFolder(folderPath, m_libraryPaths);
    }

    // Favorites and Collections keep the user's manual drag order (the DB query returned it);
    // everything else is shown alphabetically.
    if (!isVirtual) {
        std::sort(discoveredAssets.begin(), discoveredAssets.end(), [](const AssetHit& a, const AssetHit& b) {
            return a.assetFileName.compare(b.assetFileName, Qt::CaseInsensitive) < 0;
        });
    }

    clearGrid();
    assetListWidget->setUpdatesEnabled(false);

    // --- Subfolder items (physical paths only) ---
    int folderCount = 0;
    if (!isVirtual) {
        const QFileInfoList subdirs = AssetScan::listSubdirectories(folderPath);
        if (!subdirs.isEmpty()) {
            const QIcon folderIcon = m_thumbnails->folderIcon();
            for (const QFileInfo& di : subdirs) {
                QListWidgetItem *folderItem = new QListWidgetItem();
                folderItem->setText(di.fileName());
                folderItem->setIcon(folderIcon);
                folderItem->setData(AssetGridRole::FilePath, di.absoluteFilePath());
                folderItem->setData(AssetGridRole::ItemKind, kFolderItemKind);
                assetListWidget->addItem(folderItem);
                ++folderCount;
            }
        }
    }

    m_currentFolderCount = folderCount;
    refreshInfoBar();

    for (const AssetHit& hit : std::as_const(discoveredAssets)) {
        const QString cleanName = QFileInfo(hit.assetFileName).baseName();
        const QString fullPath = QDir(hit.folderPath).filePath(hit.assetFileName);

        QListWidgetItem *item = new QListWidgetItem();
        item->setText(cleanName);
        item->setData(AssetGridRole::FilePath, fullPath);
        // The tooltip is built on demand from these (assetTooltipHtml), not stored per item.
        item->setData(AssetGridRole::FileSize, hit.sizeBytes);
        item->setData(AssetGridRole::LastModified, hit.lastModified);

        assetListWidget->addItem(item);

        if (!hit.bestImage.isEmpty())
            m_thumbnails->enqueue(item, QDir(hit.folderPath).filePath(hit.bestImage));
    }

    assetListWidget->setUpdatesEnabled(true);
    // Lay the new items out now: navigateToCollectionAssetItem scrolls to one of them right
    // after this returns, which needs their geometry.
    assetListWidget->doItemsLayout();
}

QString AssetManagerWidget::assetTooltipHtml(const QListWidgetItem* item) {
    if (!item || item->data(AssetGridRole::ItemKind).toString() == kFolderItemKind) return {};
    const QString fullPath = item->data(AssetGridRole::FilePath).toString();
    if (fullPath.isEmpty()) return {};

    const QFileInfo info(fullPath); // string ops only: no stat below
    const QString ext = info.suffix().toUpper();
    const double sz = static_cast<double>(item->data(AssetGridRole::FileSize).toLongLong());
    const QDateTime modified = item->data(AssetGridRole::LastModified).toDateTime();
    return QString(
        "<div style='white-space: pre-wrap;'>"
        "<span style='font-weight: bold; color: %1;'>%2</span><br/>"
        "Size: %3<br/>"
        "Modified: %4<br/><br/>"
        "<span style='color: %5; font-size: 11px;'>%6</span>"
        "</div>"
    ).arg(Constants::COLOR_ACCENT,
          ext.isEmpty() ? tr("File") : tr(".%1 File").arg(ext),
          sz > (1024 * 1024) ? QString::number(sz / (1024.0 * 1024.0), 'f', 2) + " MB"
                             : QString::number(sz / 1024.0, 'f', 2) + " KB",
          modified.toString("MM/dd/yyyy h:mm ap"),
          Constants::COLOR_TOOLTIP_MUTED,
          QDir::toNativeSeparators(info.absolutePath()).toHtmlEscaped());
}

/**
 * @brief Handles double-clicking an item in the asset grid.
 */
void AssetManagerWidget::onGridItemDoubleClicked(QListWidgetItem *item) {
    if (!item) return;

    const QString path = item->data(AssetGridRole::FilePath).toString();
    if (path.isEmpty()) return;

    if (item->data(AssetGridRole::ItemKind).toString() == kFolderItemKind) {
        deselectTree();
        displayFolder(path);
    } else if (path.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive)) {
        // Importable model: load it into the 3D viewport rather than the OS default app.
        emit importModelRequested(path);
    } else if (path.endsWith(QStringLiteral(".duf"), Qt::CaseInsensitive) ||
               path.endsWith(QStringLiteral(".dsf"), Qt::CaseInsensitive) ||
               path.endsWith(QStringLiteral(".fbx"), Qt::CaseInsensitive)) {
        // Character figure (native, or a rigged FBX): import it into the 3D viewport (same path as
        // File -> Import / open-with).
        emit importFigureRequested(path);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

/**
 * @brief Intercepts events on the asset grid viewport: the drag controller sees every mouse
 *        event first (a live drag consumes moves), then the custom tooltip's show/hide logic.
 */
bool AssetManagerWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == assetListWidget->viewport()) {
        // 0. The hand-rolled asset drag (reorder / drop onto a tree node).
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseMove) {
            if (m_gridDrag->handleMouseEvent(static_cast<QMouseEvent *>(event)))
                return true; // consumed during an active drag (also suppresses tooltips)
        }

        // 1. Intercept the exact moment Qt tries to spawn a native tooltip
        if (event->type() == QEvent::ToolTip) {
            QHelpEvent *helpEvent = static_cast<QHelpEvent *>(event);
            QListWidgetItem *item = assetListWidget->itemAt(helpEvent->pos());

            if (item) {
                const QString html = assetTooltipHtml(item);
                if (!html.isEmpty()) {
                    customToolTip->stopHideTimer();
                    customToolTip->setText(html);

                    // Offset the box slightly right/down so the cursor doesn't instantly block it
                    customToolTip->move(helpEvent->globalPos() + QPoint(15, 15));
                    customToolTip->show();

                    // Only update the "owner" when a new tooltip successfully spawns
                    activeToolTipItem = item;
                    return true; // fully suppress the native OS tooltip
                }
            } else {
                customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
            }
        }
        // 2. Track mouse movements to manage the closing grace period
        else if (event->type() == QEvent::MouseMove) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            QListWidgetItem *item = assetListWidget->itemAt(mouseEvent->pos());

            if (customToolTip->isVisible()) {
                if (item == activeToolTipItem) {
                    // The mouse is still moving safely inside the item that OWNS the tooltip. Keep it alive.
                    customToolTip->stopHideTimer();
                } else {
                    // The mouse left the owning item (to empty space OR a neighboring item). Start the countdown!
                    customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
                }
            }
        }
        // 3. The mouse left the grid entirely
        else if (event->type() == QEvent::Leave) {
            if (customToolTip->isVisible()) {
                customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
            }
        }
    }

    // Pass all other normal events back to the application
    return QWidget::eventFilter(watched, event);
}

bool AssetManagerWidget::isSortableView() const {
    return AssetNode::isVirtual(m_currentFolderPath);
}

void AssetManagerWidget::persistGridOrder() {
    QStringList pathsInOrder;
    pathsInOrder.reserve(assetListWidget->count());
    for (int i = 0; i < assetListWidget->count(); ++i)
        pathsInOrder.append(assetListWidget->item(i)->data(AssetGridRole::FilePath).toString());

    if (m_currentFolderPath == AssetNode::FAVORITES_ROOT)
        AssetDb::setFavoriteOrder(pathsInOrder);
    else if (AssetNode::isCollection(m_currentFolderPath))
        AssetDb::setCollectionOrder(AssetNode::collectionId(m_currentFolderPath), pathsInOrder);
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
        // A drive-root library keeps its trailing slash after cleanPath ("D:/"), so build the
        // child prefix without doubling it: `normLib + '/'` would be "D://" and never match.
        const bool isLibraryRoot = (normTarget.compare(normLib, AssetScan::kPathCase) == 0);
        const QString libPrefix =
            normLib.endsWith(QLatin1Char('/')) ? normLib : normLib + QLatin1Char('/');
        if (!isLibraryRoot && !normTarget.startsWith(libPrefix, AssetScan::kPathCase)) continue;

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

// =============================================================================
// [ COLLECTIONS & FAVORITES ]
// =============================================================================

/**
 * @brief Persists a renamed Collection back to the SQLite database. Empty/whitespace names are
 *        refused: the row reverts to its stored name.
 */
void AssetManagerWidget::onItemChanged(QStandardItem *item) {
    if (!item) return;

    // Handle Collection renaming (at any nesting depth, not just top-level collections)
    const int collId = AssetNode::collectionId(item->data(Qt::UserRole).toString());
    if (collId < 0) return;

    const QString newName = item->text().trimmed();
    const auto revertTo = [&](const QString& name) {
        // setText re-enters this slot; the block keeps it from becoming a second rename.
        const QSignalBlocker blocker(dirModel);
        item->setText(name);
    };

    if (newName.isEmpty()) {
        revertTo(AssetDb::collectionName(collId));
        return;
    }
    if (newName != item->text()) revertTo(newName); // store what we display: the trimmed name

    if (!AssetDb::renameCollection(collId, newName)) {
        revertTo(AssetDb::collectionName(collId));
        return;
    }
    QStandardItem* parent = item->parent() ? item->parent() : collectionsRootItem;
    parent->sortChildren(0, Qt::AscendingOrder);
}

int AssetManagerWidget::createCollection(const QString& name, int parentCollectionId) {
    const int newId = AssetDb::createCollection(name, parentCollectionId);
    if (newId > 0) insertCollectionTreeItem(newId, name, parentCollectionId);
    return newId;
}

void AssetManagerWidget::insertCollectionTreeItem(int collectionId, const QString& name, int parentCollectionId) {
    QStandardItem *parentItem = (parentCollectionId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(parentCollectionId);
    if (!parentItem) return;

    parentItem->appendRow(makeCollectionItem(collectionId, name));
    parentItem->sortChildren(0, Qt::AscendingOrder);
}

/**
 * @brief Turns a physical folder into a collection: creates a new (uniquely named) sub-collection
 *        named after the folder under parentCollectionId, then adds the folder's own assets as
 *        collection items (subfolders and their contents are ignored). Returns the new id, or -1.
 */
int AssetManagerWidget::addFolderAsCollection(const QString& folderPath, int parentCollectionId) {
    const QString folderName = QDir(folderPath).dirName();
    const int newColId = createCollection(
        AssetDb::uniqueCollectionName(folderName, parentCollectionId), parentCollectionId);
    if (newColId <= 0) return -1;

    // scanFolderAssets returns only this folder's own (non-recursive) thumbnailed assets.
    const QList<AssetHit> assets = AssetScan::scanFolderAssets(folderPath);
    QStringList paths;
    paths.reserve(assets.size());
    for (const AssetHit& hit : assets)
        paths.append(QDir(hit.folderPath).filePath(hit.assetFileName));
    AssetDb::addCollectionItems(newColId, paths);

    refreshCollectionNode(newColId);
    return newColId;
}

/**
 * @brief Moves a collection (and its entire subtree, untouched) under a new parent, in response
 *        to a validated drag-drop from AssetFolderProxyModel. Only the dragged collection's own
 *        AssetCollectionParentID changes in the DB; descendants keep referencing its same ID, so
 *        they come along structurally without any writes of their own.
 */
void AssetManagerWidget::reparentCollection(int collectionId, int newParentId) {
    QStandardItem *collItem = findCollectionTreeItem(collectionId);
    if (!collItem) return;
    QStandardItem *oldParentItem = collItem->parent();
    if (!oldParentItem) return;

    QStandardItem *newParentItem = (newParentId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(newParentId);
    if (!newParentItem || oldParentItem == newParentItem) return;

    if (!AssetDb::reparentCollection(collectionId, newParentId)) return; // logged

    const QList<QStandardItem*> takenRow = oldParentItem->takeRow(collItem->row());
    newParentItem->appendRow(takenRow);
    newParentItem->sortChildren(0, Qt::AscendingOrder);

    const QModelIndex newProxyIdx = proxyModel->mapFromSource(dirModel->indexFromItem(collItem));
    if (newProxyIdx.isValid()) {
        m_selectingProgrammatically = true;
        dirTreeView->setCurrentIndex(newProxyIdx);
        dirTreeView->scrollTo(newProxyIdx);
        m_selectingProgrammatically = false;
    }
}

bool AssetManagerWidget::addAssetToCollection(const QString& filePath, int collectionId) {
    if (!AssetDb::addCollectionItem(filePath, collectionId)) return false;
    refreshCollectionNode(collectionId);
    return true;
}

bool AssetManagerWidget::removeAssetFromCollection(const QString& filePath, int collectionId) {
    if (!AssetDb::removeCollectionItem(filePath, collectionId)) return false;
    refreshCollectionNode(collectionId);
    return true;
}

void AssetManagerWidget::dropAssetOnTreeNode(QListWidgetItem* item, const QString& targetNodeId) {
    if (!item || targetNodeId.isEmpty()) return;
    const QString fullPath = item->data(AssetGridRole::FilePath).toString();
    if (fullPath.isEmpty()) return;

    // Dropping onto the very node the asset already lives in is a no-op.
    if (targetNodeId == m_currentFolderPath) return;

    // The current view decides the gesture: a Favorites/Collection source means the asset is
    // already filed somewhere, so the drag MOVES it; a library/search source just ADDS a copy
    // (`from` stays empty, and AssetDb::moveItem then only adds).
    AssetDb::Filing from;
    if (m_currentFolderPath == AssetNode::FAVORITES_ROOT) from.favorites = true;
    else if (AssetNode::isCollection(m_currentFolderPath)) from.collectionId = AssetNode::collectionId(m_currentFolderPath);
    const bool isMove = from.favorites || from.collectionId > 0;

    // Dropping on the Collections header spawns a brand-new top-level collection, same as
    // picking "New Collection" from the asset's context menu, and files the asset into it.
    AssetDb::Filing to;
    int newCollectionId = -1;
    if (targetNodeId == AssetNode::FAVORITES_ROOT) {
        to.favorites = true;
    } else if (targetNodeId == AssetNode::COLLECTIONS_ROOT) {
        newCollectionId = createCollection(AssetDb::uniqueCollectionName(tr("New Collection"), 0), 0);
        if (newCollectionId <= 0) return; // logged; nothing was moved
        to.collectionId = newCollectionId;
    } else {
        to.collectionId = AssetNode::collectionId(targetNodeId);
    }

    // Add + remove in ONE transaction: a failed add can never delete the asset's only filing.
    // (Both inserts are UNIQUE/INSERT-OR-IGNORE, so re-filing is harmless.)
    if (!AssetDb::moveItem(fullPath, from, to)) return; // logged; nothing changed
    if (to.collectionId > 0) refreshCollectionNode(to.collectionId);
    if (from.collectionId > 0) refreshCollectionNode(from.collectionId);

    if (newCollectionId > 0) {
        // Jump to the freshly created collection and drop it straight into rename edit mode,
        // matching the "New Collection" context-menu action.
        navigateToCollectionNode(newCollectionId, true);
    } else if (isMove) {
        // A move leaves the current grid; refresh it so the asset visibly disappears from this view.
        refreshCurrentView();
    }
}
