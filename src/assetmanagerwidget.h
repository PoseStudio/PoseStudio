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
#include <QHash>
#include <QSet>
#include <QIcon>
#include <QColor>
#include <QStyle>
#include <QAbstractItemModel>
#include <QTreeView>

// Forward declarations drastically improve project compilation times
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

    QLineEdit *searchInput;
    QPushButton *clearSearchButton;
    QPushButton *searchButton;       

    CustomToolTip *customToolTip;       ///< Our floating interactive tooltip
    QListWidgetItem *activeToolTipItem;   ///< Tracks the active item
    
    QStandardItemModel *dirModel;       
    AssetFolderProxyModel *proxyModel;
    
    AssetTreeView *dirTreeView; 
    
    QStandardItem *searchResultsRootItem;
    QStandardItem *collectionsRootItem;

    QList<QPair<int, QString>> m_pendingThumbs;
    QString m_currentFolderPath;
    QString m_currentTitleText;   ///< Plain display name for virtual/collection titles (no breadcrumb)
    QString m_hoveredBreadcrumbLink;
    int m_currentAssetCount = 0;

    // Cached breadcrumb structure for the current physical folder, rebuilt on each displayFolder()
    // call and re-laid-out (without recomputation) whenever the label is resized.
    QString m_breadcrumbLibRoot;
    QString m_breadcrumbLibName;
    QStringList m_breadcrumbSegments;
    QStringList m_breadcrumbPaths;

    void setupUI();
    void processNextThumbnailBatch();
    void runSearch(const QString& query);
    void displayFolder(const QString& folderPath, const QString& title = QString());
    void deselectTree();
    void resolveBreadcrumb(const QString& folderPath);
    void refreshTitleLabel();
    QString buildBreadcrumbHtml(int availableWidth) const;

    QList<AssetHit> parseFolderAssets(const QString& folderPath);
    QList<AssetHit> parseCollectionAssets(int collectionId);

    void navigateToFolderInTree(const QString& folderPath);
    void navigateToCollectionNode(int collectionId, bool enterEditMode = false);
    void collectDirectHits(const QString& folderPath, const QString& libRootPath, QSet<QString>& added, QList<QStandardItem*>& results);
    int  getOrCreateCollection(const QString& name);
    void addAssetToCollection(const QString& filePath, int collectionId);
    void removeAssetFromCollection(const QString& filePath, int collectionId);
    void addFolderToCollection(const QString& folderPath, const QString& displayName, int collectionId, bool saveToDb = true);
    QMenu* buildAddToCollectionMenu(QWidget* parentMenu, const QString& folderPath, const QString& folderName);
    void removeFolderFromCollection(const QString& folderPath, int collectionId);
    void saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths);
    void restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths);
    QModelIndex findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath);

protected:
    /// Handles the title label's resize/context-menu events and the asset grid's custom tooltips.
    bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif // ASSETMANAGERWIDGET_H