/**
 * @file assetmanagerwidget.h
 * @brief The Asset Manager side panel: the directory/collections tree over the thumbnail grid.
 *
 * This is the one class the rest of the app sees (main.cpp builds it, MenuManager wires its
 * signals). It composes the module's parts, each in its own file, and owns the STATE that ties
 * them together (which source is displayed, the tree's section roots, the cached library list):
 * - AssetTreeView over a QStandardItemModel wrapped by AssetFolderProxyModel (folder hit icons,
 *   collection drag-reparent), painted by AssetTreeDelegate;
 * - the QListWidget grid painted by AssetGridDelegate, fed by AssetThumbnailLoader, with the
 *   hand-rolled drag in AssetGridDragController and the floating CustomToolTip;
 * - AssetBreadcrumbLabel as the grid's title;
 * - AssetScan (filesystem), AssetDb (collections/favorites SQL) and core/assetlibraries.h
 *   (library folders) as the data layer, with assetnodeids.h as the shared vocabulary.
 * The tree mixes physical library folders with virtual, DB-backed rows (Search Results,
 * Favorites, nested Collections), all told apart by the node id in Qt::UserRole; the grid shows
 * whichever source is selected. Physical folders are lazy-loaded on expand; Favorites and
 * Collections keep a user-defined drag order. The context menus live in a second translation
 * unit of this class (assetmanagerwidget_menus.cpp).
 */

#ifndef ASSETMANAGERWIDGET_H
#define ASSETMANAGERWIDGET_H

#include <QHash>
#include <QIcon>
#include <QList>
#include <QModelIndex>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

// Forward declarations drastically improve project compilation times
class AssetBreadcrumbLabel;
class AssetFolderProxyModel;
class AssetGridDragController;
class AssetThumbnailLoader;
class AssetTreeView;
class CustomToolTip;
class QAction;
class QDir;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPushButton;
class QStandardItem;
class QStandardItemModel;
namespace AssetScan { struct AssetHit; }

/**
 * @class AssetManagerWidget
 * @brief The main side-panel widget managing the directory tree and the asset thumbnail grid.
 */
class AssetManagerWidget : public QWidget {
    Q_OBJECT

public:
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    ~AssetManagerWidget() override;

    /// Clears the directory tree and rebuilds it from the database, preserving the expanded
    /// nodes and the selection where they still exist. Also re-reads the library list.
    void refreshAssetManager();

    /// Selects and displays a top-level library's own root node (e.g. from Preferences'
    /// Assets list). Unlike navigateToFolderInTree, the target is the library root itself,
    /// not a descendant of it.
    void navigateToLibraryRoot(const QString& libraryPath);

    /// Opens a file the way double-clicking it in the grid does: a model, a figure or a splat
    /// scene goes to the viewport through the import*Requested signals, a content-library script
    /// is followed as the link it is (openScriptLink), anything else opens in the operating
    /// system's application for it. Public so a script on the command line takes the same path.
    void openAsset(const QString& path);

signals:
    /// Emitted when the user picks "Manage Asset Folders" from a tree context menu, so
    /// whatever owns the Preferences dialog can open it directly to the Assets tab.
    void manageAssetFoldersRequested();

    /// Emitted when the user double-clicks an importable model asset (e.g. an .obj) in the grid,
    /// so the owner can load it into the 3D viewport. Carries the asset's absolute file path.
    void importModelRequested(const QString& path);

    /// Emitted when the user double-clicks a character-figure asset (`.duf`/`.dsf`) in the grid, so
    /// the owner can import it into the 3D viewport. Carries the asset's absolute file path.
    void importFigureRequested(const QString& path);

    /// Emitted when the user double-clicks a Gaussian splat scene (`.ply` / `.splat`) in the grid.
    void importSplatRequested(const QString& path);

private slots:
    /// A tree row was clicked: section headers toggle their expansion, everything browsable
    /// is shown in the grid.
    void onFolderSelected(const QModelIndex &index);
    /// The tree's current row moved without a click (keyboard navigation): show it in the grid.
    void onTreeCurrentChanged(const QModelIndex &current);
    void onTreeExpanded(const QModelIndex &index);

    void onContextMenuRequested(const QPoint &pos);
    void onGridContextMenuRequested(const QPoint &pos);
    /// Double-click on a grid item: a folder item opens that folder in the grid; a file goes
    /// through openAsset.
    void onGridItemDoubleClicked(QListWidgetItem *item);

    /// A tree item's data changed: for a Collection row, persists the inline rename.
    void onItemChanged(QStandardItem *item);

    /// Moves a collection (and its entire subtree) to a new parent in response to a validated
    /// drag-drop from AssetFolderProxyModel. newParentId is 0 for the Collections root.
    void reparentCollection(int collectionId, int newParentId);

private:
    /// Identifies which branch of the directory tree a node lives under, regardless of nesting
    /// depth, so context menus can tell a "real" library folder apart from a Search Results /
    /// Favorites / Collection view showing the same physical path.
    enum class BrowseContext { Library, Collection, SearchResults, Favorites };

    // --- Widgets ---
    AssetBreadcrumbLabel *titleLabel;
    QListWidget *assetListWidget;
    QLabel *infoBarLabel;        ///< Footer below the grid: "Assets: X   Folders: X   Sortable"
    QLabel *addLibraryHintLabel; ///< "Add Asset Folder" link shown over the directory tree when no library exists yet

    QLineEdit *searchInput;
    QPushButton *clearSearchButton;
    QPushButton *searchButton;

    CustomToolTip *customToolTip;       ///< Our floating interactive tooltip
    /// The grid item the visible tooltip belongs to. Nulled by clearGrid(), the ONE place the
    /// grid's items are deleted: it is compared against live item pointers in the grid's
    /// MouseMove handling.
    QListWidgetItem *activeToolTipItem = nullptr;

    QStandardItemModel *dirModel;
    AssetFolderProxyModel *proxyModel;
    AssetTreeView *dirTreeView;

    AssetThumbnailLoader *m_thumbnails;   ///< Off-thread thumbnail decodes; cleared by clearGrid()
    AssetGridDragController *m_gridDrag;  ///< The hand-rolled grid drag (reorder / drop onto the tree)

    QStandardItem *searchResultsRootItem;
    QStandardItem *searchSeparatorItem; ///< Separator between Search Results and Favorites; hidden together with searchResultsRootItem when no search is active
    QStandardItem *favoritesRootItem;
    QStandardItem *collectionsRootItem;

    // --- State ---
    /// The enabled library paths in display order, read once per refreshAssetManager(): the
    /// search, the breadcrumb and "Find In Library" all consult it instead of re-querying.
    QStringList m_libraryPaths;

    QString m_currentFolderPath;  ///< Node id of what the grid shows: a folder path, FAVORITES_ROOT or COLLECTION_<id>
    int m_currentAssetCount = 0;
    int m_currentFolderCount = 0; ///< Subfolder count for the currently displayed physical folder (0 for virtual sources)

    /// True while the widget itself sets the tree's current index (refresh re-select,
    /// navigate*, reparent): onTreeCurrentChanged must not also populate the grid then.
    bool m_selectingProgrammatically = false;

    /// Context-menu icons, constructed once per widget rather than per menu open.
    QHash<QString, QIcon> m_icons;
    const QIcon& icon(const QString& name);

    // --- Setup ---
    void setupUI();
    void promptAddAssetLibrary();

    // --- Tree build/refresh ---
    void saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths);
    void restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths);
    QModelIndex findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath);
    /// The top-level tree row of the library registered at `libraryPath`, or an invalid index.
    QModelIndex libraryRootIndex(const QString& libraryPath) const;
    /// Builds the Collections subtree from one query of the whole AssetCollections table.
    void loadCollectionsTree();
    /// A fresh, editable Collection tree item tagged "COLLECTION_<id>".
    QStandardItem* makeCollectionItem(int collectionId, const QString& name) const;
    /// The tree item of a collection (any depth under the Collections root), or nullptr.
    QStandardItem* findCollectionTreeItem(int collectionId) const;
    /// Every collection as (id, "Parent / Child" path), sorted by path, derived from the tree.
    QList<QPair<int, QString>> collectionPathList() const;
    /// Which branch of the tree `item` lives under (see BrowseContext).
    BrowseContext contextForTreeItem(QStandardItem* item) const;
    /// The tree's current item, or nullptr.
    QStandardItem* currentTreeItem() const;
    /// Repaints a Collection row's hit icon after its items changed.
    void refreshCollectionNode(int collectionId);

    // --- Search ---
    void runSearch(const QString& query);
    void updateSearchVisibility(bool active);
    void collectDirectHits(const QString& folderPath, const QDir& libRootDir, QSet<QString>& added, QList<QStandardItem*>& results);

    // --- Grid display ---
    /// Shows a source in the grid: a physical folder (subfolders + assets, alphabetical) or a
    /// virtual node id (Favorites / a Collection, in the user's manual order). `title` is the
    /// virtual source's display name; when empty it is resolved from the tree.
    void displayFolder(const QString& folderPath, const QString& title = QString());
    /// Shows the tree row at `proxyIndex` in the grid, if it is browsable.
    void displayTreeIndex(const QModelIndex& proxyIndex);
    /// Re-displays whatever is currently shown (after its contents changed).
    void refreshCurrentView();
    /// Empties the grid and everything that points at its items (thumbnail jobs, tooltip).
    /// THE only place grid items are deleted.
    void clearGrid();
    /// Back to the idle state: empty grid, idle title, no current source, no info bar.
    void resetGridView();
    /// Rebuilds the "Assets: X   Folders: X   Sortable" footer, hiding any segment that isn't
    /// relevant to what's currently displayed (zero count, or a non-sortable view).
    void refreshInfoBar();
    /// The tooltip card for an asset item, built on demand from the item's roles.
    static QString assetTooltipHtml(const QListWidgetItem* item);
    /// True for any view with a manual drag order (Favorites or a Collection). Gates the
    /// drag-reorder handling and the "Sortable" info-bar label.
    bool isSortableView() const;
    /// Writes the current visual order of the grid back to FavoriteSortOrder or
    /// AssetCollectionItemSortOrder (whichever the current view backs onto), so a
    /// drag-reorder survives navigation and restarts.
    void persistGridOrder();

    // --- Navigation ---
    void deselectTree();
    /// Makes `proxyIndex` current, scrolls it into view and shows it in the grid.
    void selectTreeIndex(const QModelIndex& proxyIndex);
    void navigateToFolderInTree(const QString& folderPath);
    /// Selects a collection's tree row (expanding the Collections root), shows it in the grid,
    /// and optionally opens the inline rename editor on it (a freshly created collection).
    void navigateToCollectionNode(int collectionId, bool enterEditMode = false);
    void navigateToCollectionAssetItem(int collectionId, const QString& assetFullPath);

    // --- Script links (assetscriptlink.h) ---
    /// Follows a content-library script to what it links to: a folder is shown (in the tree when
    /// it lies in a library) and the character in it imported when it is the folder's only one;
    /// a file opens through openAsset; a web page opens in the browser. A script that leads
    /// nowhere PoseStudio can go is explained in a message, which offers the operating system's
    /// application instead — PoseStudio never runs a script.
    void openScriptLink(const QString& scriptPath);
    /// The folders a script's content-relative path may lie under besides its own ancestors:
    /// the asset libraries and the figure content roots set in Preferences.
    QStringList scriptLinkRoots() const;

    // --- Collections & favorites ---
    /// Creates a collection in the database and its tree row. Returns the new id, or -1.
    int  createCollection(const QString& name, int parentCollectionId = 0);
    /// Adds the tree row for a collection the database already holds.
    void insertCollectionTreeItem(int collectionId, const QString& name, int parentCollectionId);
    /// Creates a new sub-collection named after `folderPath` under `parentCollectionId` (0 = top
    /// level) and fills it with the folder's own assets, ignoring subfolders. Returns the new id.
    int addFolderAsCollection(const QString& folderPath, int parentCollectionId);
    /// Files an asset into a collection and repaints the collection's row. False on failure.
    bool addAssetToCollection(const QString& filePath, int collectionId);
    bool removeAssetFromCollection(const QString& filePath, int collectionId);
    /// Handles a grid asset dropped onto a Collection/Favorites tree node: ADDs it when the asset
    /// comes from a library/search view, or MOVEs it (remove-from-source + add-to-target, one
    /// transaction) when the current view is itself a Favorites/Collection. No-op when the
    /// target is the current source view.
    void dropAssetOnTreeNode(QListWidgetItem* item, const QString& targetNodeId);

    // --- Context menus (assetmanagerwidget_menus.cpp) ---
    QMenu* buildAddToCollectionMenu(QWidget* parentMenu, const QString& folderPath);
    /// A QMenu carrying the Asset Manager's context-menu object name (for its QSS).
    QMenu* newContextMenu(QWidget* parent);
    /// The trailing "Manage Asset Folders" / "Refresh" pair every tree menu ends with.
    QPair<QAction*, QAction*> addManageRefreshActions(QMenu* menu);
    struct ExpandActions {
        QAction* expand = nullptr;
        QAction* expandBranch = nullptr;
        QAction* collapse = nullptr;
    };
    /// Expand / Expand Branch / Collapse entries for a tree row, enabled per its state.
    ExpandActions addExpandCollapseActions(QMenu* menu, const QModelIndex& proxyIndex, bool withBranch);
    /// Runs an Expand/Collapse choice. Returns true if `selected` was one of them.
    bool handleExpandCollapse(QAction* selected, const ExpandActions& actions, const QModelIndex& proxyIndex);
    /// "Find In Library" + "Browse Folder" for something shown outside the plain library tree.
    QPair<QAction*, QAction*> addFindAndBrowseActions(QMenu* menu);
    void expandNodeRecursively(const QModelIndex &proxyIndex);
    void collapseNodeRecursively(const QModelIndex &proxyIndex);

protected:
    /// Watches the grid viewport: forwards mouse events to the drag controller first, then
    /// manages the custom tooltip's show/hide grace period.
    bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif // ASSETMANAGERWIDGET_H
