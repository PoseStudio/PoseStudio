/**
 * @file assetmanagerwidget.h
 * @brief Declarations for the AssetManagerWidget and its associated models and delegates.
 * @details This file defines the core UI components and data models required to parse, 
 * display, and interact with the PoseStudio 3D asset library, physical directories, 
 * and virtual database collections.
 */

#ifndef ASSETMANAGERWIDGET_H
#define ASSETMANAGERWIDGET_H

#include <QWidget>
#include <QIdentityProxyModel> 
#include <QStyledItemDelegate>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <QList>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QPoint>
#include <QHash>
#include <QSet>
#include <QIcon>
#include <QColor>
#include <QStyle>
#include <QAbstractItemModel>
#include <QTreeView>

// Forward declarations drastically improve project compilation times
class QDir;
class QVBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;
class QStandardItemModel;
class QStandardItem;
class QMenu;
class CustomToolTip;

/**
 * @struct AssetHit
 * @brief Represents a successfully discovered 3D asset and its paired thumbnail imagery.
 */
struct AssetHit {
    QString folderPath;          ///< Absolute path to the directory containing the asset
    QString assetFileName;       ///< Filename of the 3D asset (e.g., model.obj, .dsf)
    QStringList matchingImages;  ///< List of filenames for matching thumbnails (e.g., render.png)
};

enum FolderHitState { NoHit = 0, IndirectHit = 1, DirectHit = 2 };

/// Identifies which branch of the directory tree a node lives under, regardless of nesting
/// depth, so context menus can tell a "real" library folder apart from a Collections/Search
/// Results/Favorites shortcut pointing at the same physical path.
enum class BrowseContext { Library, Collection, SearchResults, Favorites };

/**
 * @class AssetFolderProxyModel
 * @brief Intercepts data requests to dynamically style folders with custom icons and labels.
 */
class AssetFolderProxyModel : public QIdentityProxyModel {
    Q_OBJECT
public:
    explicit AssetFolderProxyModel(QAbstractItemModel* source, QObject* parent = nullptr);
    QVariant data(const QModelIndex &proxyIndex, int role = Qt::DisplayRole) const override;

    void invalidateAndRefresh(const QString& path);
    bool hasHit(const QString& folderPath)      const { return folderHitState(folderPath) != NoHit; }
    bool isDirectHit(const QString& folderPath) const { return folderHitState(folderPath) == DirectHit; }

private slots:
    void processPendingHitCheck();

private:
    mutable QHash<QString, FolderHitState> hasHitCache;
    mutable QList<QPersistentModelIndex> m_pendingHitIndexes;
    mutable QSet<QString> m_pendingHitPathsSet;
    mutable bool m_hitCheckTimerActive = false;

    bool directFolderHasHit(const QString& folderPath) const;
    FolderHitState folderHitState(const QString& folderPath) const;
};

/**
 * @class AssetTreeDelegate
 * @brief Custom item delegate that completely overrides native Qt painting for the directory tree.
 * @details Responsible for rendering folder names, custom icons, and enforcing the geometry of the inline rename editor.
 */
class AssetTreeDelegate : public QStyledItemDelegate {
public:
    explicit AssetTreeDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

/**
 * @class AssetGridDelegate
 * @brief Custom delegate for the asset grid that word-wraps labels to 2 lines and elides on line 2.
 */
class AssetGridDelegate : public QStyledItemDelegate {
public:
    explicit AssetGridDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

/**
 * @class AssetTreeView
 * @brief A customized QTreeView that exposes custom properties to the Qt Style Engine.
 * @details Allows the C++ delegate to pull variables directly from external .qss files.
 */
class AssetTreeView : public QTreeView {
    Q_OBJECT
    Q_PROPERTY(QColor separatorColor READ separatorColor WRITE setSeparatorColor)

public:
    explicit AssetTreeView(QWidget *parent = nullptr) : QTreeView(parent), m_separatorColor(60, 60, 60) {}

    QColor separatorColor() const { return m_separatorColor; }
    void setSeparatorColor(const QColor &color) {
        m_separatorColor = color;
        viewport()->update();
    }

protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override {
        QVariant fg = index.data(Qt::ForegroundRole);
        if (fg.isValid()) {
            painter->save();
            painter->setOpacity(0.35);
            QTreeView::drawBranches(painter, rect, index);
            painter->restore();
        } else {
            QTreeView::drawBranches(painter, rect, index);
        }
    }

private:
    QColor m_separatorColor;
};

// =============================================================================
// [ PRIMARY WIDGET ]
// =============================================================================

/**
 * @class AssetManagerWidget
 * @brief The main side-panel widget managing the directory tree and the asset thumbnail grid.
 */
class AssetManagerWidget : public QWidget {
    Q_OBJECT 

public:
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    ~AssetManagerWidget() override = default;

    void expandNodeRecursively(const QModelIndex &proxyIndex);
    void collapseNodeRecursively(const QModelIndex &proxyIndex);
    void refreshAssetManager();

    /// Selects and displays a top-level library's own root node (e.g. from Preferences'
    /// Assets list) — unlike navigateToFolderInTree, the target is the library root itself,
    /// not a descendant of it.
    void navigateToLibraryRoot(const QString& libraryPath);

signals:
    /// Emitted when the user picks "Manage Asset Folders" from a tree context menu, so
    /// whatever owns the Preferences dialog can open it directly to the Assets tab.
    void manageAssetFoldersRequested();

private slots:
    void onFolderSelected(const QModelIndex &index);
    void onTreeExpanded(const QModelIndex &index); 

    void onContextMenuRequested(const QPoint &pos);
    void onGridContextMenuRequested(const QPoint &pos); 
    void onGridItemDoubleClicked(QListWidgetItem *item); ///< Opens an asset, or navigates into a folder item

    void onItemChanged(QStandardItem *item);

private:
    QVBoxLayout *mainLayout;
    QLabel *titleLabel;
    QListWidget *assetListWidget;
    QLabel *infoBarLabel;        ///< Footer below the grid: "Assets: X   Folders: X   Sortable"
    QLabel *addLibraryHintLabel; ///< "Add Asset Folder" link shown over the grid when no library exists yet

    QLineEdit *searchInput;
    QPushButton *clearSearchButton;
    QPushButton *searchButton;       

    CustomToolTip *customToolTip;       ///< Our floating interactive tooltip
    QListWidgetItem *activeToolTipItem;   ///< Tracks the active item
    
    QStandardItemModel *dirModel;       
    AssetFolderProxyModel *proxyModel;
    
    AssetTreeView *dirTreeView; 
    
    QStandardItem *searchResultsRootItem;
    QStandardItem *favoritesRootItem;
    QStandardItem *collectionsRootItem;

    QList<QPair<int, QString>> m_pendingThumbs;

    // Manual drag-reorder state for sortable grids (Favorites and Collections — see eventFilter).
    QListWidgetItem *m_dragItem = nullptr;
    QPoint m_dragStartPos;
    bool m_dragging = false;
    QLabel *m_dragPreview = nullptr;  ///< Floating ghost thumbnail that follows the cursor mid-drag
    QWidget *m_dropLine = nullptr;    ///< Vertical line marking where the dragged item will drop
    QTimer *m_scrollTimer = nullptr;  ///< Drives edge auto-scroll while reordering
    int m_scrollDir = 0;              ///< -1 = scroll up, +1 = scroll down, 0 = idle
    QPoint m_dragLastPos;             ///< Last cursor pos (viewport coords) during a drag

    QString m_currentFolderPath;
    QString m_currentTitleText;   ///< Plain display name for virtual/collection titles (no breadcrumb)
    QString m_hoveredBreadcrumbLink;
    int m_currentAssetCount = 0;
    int m_currentFolderCount = 0; ///< Subfolder count for the currently displayed physical folder (0 for virtual sources)

    // Cached breadcrumb structure for the current physical folder, rebuilt on each displayFolder()
    // call and re-laid-out (without recomputation) whenever the label is resized.
    QString m_breadcrumbLibRoot;
    QString m_breadcrumbLibName;
    QStringList m_breadcrumbSegments;
    QStringList m_breadcrumbPaths;

    void setupUI();
    void promptAddAssetLibrary();
    void processNextThumbnailBatch();
    void runSearch(const QString& query);
    void displayFolder(const QString& folderPath, const QString& title = QString());
    void deselectTree();
    void resolveBreadcrumb(const QString& folderPath);
    void refreshTitleLabel();
    /// Rebuilds the "Assets: X   Folders: X   Sortable" footer, hiding any segment that isn't
    /// relevant to what's currently displayed (zero count, or a non-sortable view).
    void refreshInfoBar();
    QString buildBreadcrumbHtml(int availableWidth) const;

    QList<AssetHit> parseFolderAssets(const QString& folderPath);
    QList<AssetHit> parseCollectionAssets(int collectionId);
    QList<AssetHit> parseFavorites();
    /// Shared by parseCollectionAssets/parseFavorites: groups a flat list of asset file paths by
    /// folder and resolves each asset's best thumbnail. Skips paths that no longer exist on disk.
    QList<AssetHit> buildAssetHits(const QStringList& assetPaths);

    void navigateToFolderInTree(const QString& folderPath);
    void navigateToCollectionNode(int collectionId, bool enterEditMode = false);
    void navigateToCollectionAssetItem(int collectionId, const QString& assetFullPath);
    void collectDirectHits(const QString& folderPath, const QDir& libRootDir, QSet<QString>& added, QList<QStandardItem*>& results);
    int  getOrCreateCollection(const QString& name, int parentCollectionId = 0);
    void addAssetToCollection(const QString& filePath, int collectionId);
    void removeAssetFromCollection(const QString& filePath, int collectionId);
    void addAssetToFavorites(const QString& filePath);
    void removeAssetFromFavorites(const QString& filePath);
    /// True for any view with a manual drag order (Favorites or a Collection) — gates the
    /// drag-reorder handling in eventFilter and the "Sortable" info-bar label.
    bool isSortableView() const;
    /// Writes the current visual order of the grid back to FavoriteSortOrder or
    /// AssetCollectionItemSortOrder (whichever the current view backs onto), so a
    /// drag-reorder survives navigation and restarts.
    void persistGridOrder();
    void beginGridDrag(); ///< Starts a reorder: floats a ghost thumbnail + drop line.
    void endGridDrag();   ///< Tears down the ghost/drop line and clears drag state.
    /// Insertion index (0..count) the cursor points at, in the grid's reading order.
    int  gridInsertIndex(const QPoint& viewportPos) const;
    /// Positions the drop-line indicator at the gap the cursor points at.
    void updateGridDropIndicator(const QPoint& viewportPos);
    /// Creates a new sub-collection named after `folderPath` under `parentCollectionId` (0 = top
    /// level) and fills it with the folder's own assets, ignoring subfolders. Returns the new id.
    int addFolderAsCollection(const QString& folderPath, int parentCollectionId);
    QMenu* buildAddToCollectionMenu(QWidget* parentMenu, const QString& folderPath);
    void saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths);
    void restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths);
    QModelIndex findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath);
    BrowseContext contextForTreeItem(QStandardItem* item) const;

    void loadCollectionsInto(QStandardItem* parentItem, int parentCollectionId);
    QStandardItem* findCollectionTreeItem(QStandardItem* parent, int collectionId) const;
    QList<QPair<int, QString>> collectionPathList() const;
    QString uniqueCollectionName(const QString& baseName, int parentCollectionId) const;

protected:
    /// Handles the title label's resize/context-menu events and the asset grid's custom tooltips.
    bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif // ASSETMANAGERWIDGET_H