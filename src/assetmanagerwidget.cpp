/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of the AssetManagerWidget class for managing and visualizing 3D assets.
 * @details This file drives the primary asset library interface for PoseStudio. It acts as a 
 * bridge between the user's physical hard drive (parsing directories for .obj, .duf, etc.) 
 * and the SQLite database (managing virtual Collections and Favorites). It utilizes a 
 * QIdentityProxyModel to seamlessly overlay database-driven metadata (like hit counts and 
 * custom icons) onto standard filesystem trees.
 */

#include "assetmanagerwidget.h"
#include "preferencesmanager.h"
#include "constants.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QEnterEvent>
#include <QFileInfo>
#include <QHash>
#include <QHelpEvent>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProxyStyle>
#include <QSet>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

// =============================================================================
// [ CUSTOM STYLE OVERRIDES ]
// =============================================================================

/**
 * @class AppProxyStyle
 * @brief Intercepts OS-level styling requests to force custom UI behaviors.
 */
class AppProxyStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, 
                  const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override {
        
        if (hint == QStyle::SH_ToolTip_WakeUpDelay) return Constants::TOOLTIP_WAKE_DELAY_MS;
        if (hint == QStyle::SH_ToolTip_FallAsleepDelay) return Constants::TOOLTIP_SLEEP_DELAY_MS;
        
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override {
        if (metric == QStyle::PM_SubMenuOverlap) return -4; 
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    // =========================================================================
    // Globally force all disabled icons to 30% opacity
    // =========================================================================
    QPixmap generatedIconPixmap(QIcon::Mode iconMode, const QPixmap &pixmap, const QStyleOption *opt) const override {
        if (iconMode == QIcon::Disabled) {
            
            // Create a blank, transparent canvas the exact same size as the icon
            QPixmap transparentPixmap(pixmap.size());
            transparentPixmap.fill(Qt::transparent);
            
            // Paint the original icon onto the canvas at exactly 30% opacity
            QPainter painter(&transparentPixmap);
            painter.setOpacity(0.3); 
            painter.drawPixmap(0, 0, pixmap);
            painter.end();
            
            return transparentPixmap;
        }
        
        // Pass all other normal/active icons back to standard Qt behavior
        return QProxyStyle::generatedIconPixmap(iconMode, pixmap, opt);
    }
};

/**
 * @class CustomToolTip
 * @brief An interactive, floating label that replaces standard Qt tooltips.
 */
class CustomToolTip : public QWidget {
    Q_OBJECT
public:
    explicit CustomToolTip(QWidget* parent = nullptr) : QWidget(parent, Qt::ToolTip) {
        setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_ShowWithoutActivating);
        
        // The outer widget becomes the invisible "glass" container
        setAttribute(Qt::WA_TranslucentBackground); 
        setMouseTracking(true); 

        // Create a layout with zero margins so the label fills the entire glass window
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        // The inner label safely renders the CSS without top-level window bugs!
        textLabel = new QLabel(this);
        textLabel->setStyleSheet(
            "QLabel {"
            "  background-color: #1e1f22;"
            "  color: #e0e0e0;"
            "  border: 1px solid #3a3b3c;"
            "  border-radius: 4px;"
            "  padding: 6px 10px;"
            "  font-size: 13px;"
            "}"
        );
        
        layout->addWidget(textLabel);

        hideTimer = new QTimer(this);
        hideTimer->setSingleShot(true);
        connect(hideTimer, &QTimer::timeout, this, &CustomToolTip::hide);
    }

    // Bridge function so the rest of your code can still set the text easily
    void setText(const QString& text) {
        textLabel->setText(text);
    }

    void startHideTimer(int ms) { hideTimer->start(ms); }
    void stopHideTimer() { hideTimer->stop(); }

protected:
    void enterEvent(QEnterEvent* event) override {
        stopHideTimer();
        QWidget::enterEvent(event);
    }
    
    void leaveEvent(QEvent* event) override {
        startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
        QWidget::leaveEvent(event);
    }

private:
    QTimer* hideTimer;
    QLabel* textLabel;
};

// =============================================================================
// [ PROXY MODEL IMPLEMENTATION ]
// =============================================================================

AssetFolderProxyModel::AssetFolderProxyModel(QAbstractItemModel* source, QObject* parent)
    : QIdentityProxyModel(parent) {
    setSourceModel(source);
    connect(source, &QAbstractItemModel::modelReset, this, [this]() {
        hasHitCache.clear();
    });
}

void AssetFolderProxyModel::invalidateAndRefresh(const QString& path) {
    hasHitCache.remove(path);

    // Walk one level deep (root → section roots → collection nodes) to find the matching proxy index
    const int rootRows = sourceModel()->rowCount();
    for (int r = 0; r < rootRows; ++r) {
        const QModelIndex sectionIdx = sourceModel()->index(r, 0);
        const int childRows = sourceModel()->rowCount(sectionIdx);
        for (int c = 0; c < childRows; ++c) {
            const QModelIndex childSrcIdx = sourceModel()->index(c, 0, sectionIdx);
            if (sourceModel()->data(childSrcIdx, Qt::UserRole).toString() == path) {
                const QModelIndex proxyIdx = mapFromSource(childSrcIdx);
                emit dataChanged(proxyIdx, proxyIdx, {Qt::ForegroundRole, Qt::DecorationRole});
                return;
            }
        }
    }
}

bool AssetFolderProxyModel::directFolderHasHit(const QString& folderPath) const {
    static const QStringList imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    const QFileInfoList files = QDir(folderPath).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);

    QSet<QString> images, nonImages;
    for (const QFileInfo& f : files) {
        const QString base = f.baseName();
        if (imageExts.contains(f.suffix().toLower())) {
            images.insert(base);
            if (nonImages.contains(base)) return true;
        } else {
            nonImages.insert(base);
            if (images.contains(base)) return true;
        }
    }
    return false;
}

FolderHitState AssetFolderProxyModel::folderHitState(const QString& folderPath) const {
    auto it = hasHitCache.find(folderPath);
    if (it != hasHitCache.end()) return it.value();

    if (directFolderHasHit(folderPath)) {
        hasHitCache.insert(folderPath, DirectHit);
        return DirectHit;
    }

    const QFileInfoList subdirs = QDir(folderPath).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort);
    for (const QFileInfo& sub : subdirs) {
        if (folderHitState(sub.absoluteFilePath()) != NoHit) {
            hasHitCache.insert(folderPath, IndirectHit);
            return IndirectHit;
        }
    }

    hasHitCache.insert(folderPath, NoHit);
    return NoHit;
}

void AssetFolderProxyModel::processPendingHitCheck() {
    if (m_pendingHitIndexes.isEmpty()) {
        m_hitCheckTimerActive = false;
        return;
    }

    const QPersistentModelIndex pIdx = m_pendingHitIndexes.takeFirst();

    if (pIdx.isValid()) {
        const QModelIndex srcIdx = mapToSource(pIdx);
        const QString path = sourceModel()->data(srcIdx, Qt::UserRole).toString();
        m_pendingHitPathsSet.remove(path);

        if (path.startsWith("COMBINED_DIR_")) {
            const QString relPath = path.mid(13);
            FolderHitState state = NoHit;
            QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
            libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
            while (libQuery.next()) {
                QDir dir(libQuery.value(0).toString());
                if (!dir.cd(relPath)) continue;
                FolderHitState libState = folderHitState(dir.absolutePath());
                if (libState == DirectHit) { state = DirectHit; break; }
                if (libState == IndirectHit) state = IndirectHit;
            }
            hasHitCache.insert(path, state);
        } else {
            folderHitState(path);
        }

        emit dataChanged(QModelIndex(pIdx), QModelIndex(pIdx), {Qt::ForegroundRole, Qt::DecorationRole});
    }

    if (!m_pendingHitIndexes.isEmpty()) {
        QTimer::singleShot(0, this, &AssetFolderProxyModel::processPendingHitCheck);
    } else {
        m_hitCheckTimerActive = false;
    }
}

/**
 * @brief Intercepts data requests from the view to inject dynamic icons and labels.
 */
QVariant AssetFolderProxyModel::data(const QModelIndex &proxyIndex, int role) const {
    if (proxyIndex.column() != 0) return QIdentityProxyModel::data(proxyIndex, role);

    QModelIndex sourceIndex = mapToSource(proxyIndex);
    QString path = sourceModel()->data(sourceIndex, Qt::UserRole).toString();

    // ---------------------------------------------------------
    // 1. Root Node Overrides (Favorites & Collections)
    // ---------------------------------------------------------
    if (path == "FAVORITES_ROOT") {
        if (role == Qt::DecorationRole) return QIcon(":/resources/icons/favorites.png"); 
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == "COLLECTIONS_ROOT") {
        if (role == Qt::DecorationRole) return QIcon(":/resources/icons/collections.png"); 
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == "COMBINED_ROOT") {
        if (role == Qt::DecorationRole) return QIcon(":/resources/icons/collections.png"); 
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    // ---------------------------------------------------------
    // 2. Ignore structural/dummy nodes
    // ---------------------------------------------------------
    if (path.isEmpty() || path == "BROKEN_PATH" || path == "SEPARATOR") {
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    if (role == Qt::DecorationRole || role == Qt::ForegroundRole) {
        QModelIndex parentIndex = sourceIndex.parent();

        if (role == Qt::DecorationRole && parentIndex.isValid() &&
            sourceModel()->data(parentIndex, Qt::UserRole).toString() == "FAVORITES_ROOT") {
            return QIcon(":/resources/icons/favorite-item.png");
        }

        FolderHitState state = NoHit;
        if (path.startsWith("COLLECTION_")) {
            if (!hasHitCache.contains(path)) {
                int collId = path.mid(11).toInt();
                QSqlQuery q(QSqlDatabase::database("db_conn"));
                q.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
                q.bindValue(":id", collId);
                bool found = false;
                if (q.exec()) {
                    while (q.next()) {
                        if (QFileInfo::exists(q.value(0).toString())) { found = true; break; }
                    }
                }
                hasHitCache.insert(path, found ? DirectHit : NoHit);
            }
            state = hasHitCache.value(path);
        } else {
            if (!hasHitCache.contains(path)) {
                if (!m_pendingHitPathsSet.contains(path)) {
                    m_pendingHitPathsSet.insert(path);
                    m_pendingHitIndexes.append(QPersistentModelIndex(proxyIndex));
                    if (!m_hitCheckTimerActive) {
                        m_hitCheckTimerActive = true;
                        QTimer::singleShot(0, const_cast<AssetFolderProxyModel*>(this), &AssetFolderProxyModel::processPendingHitCheck);
                    }
                }
                if (role == Qt::ForegroundRole) return QVariant();
                return QIcon(QStringLiteral(":/resources/icons/folder-hit.png"));
            }
            state = hasHitCache.value(path);
        }

        if (role == Qt::ForegroundRole) {
            return (state == NoHit) ? QColor(110, 110, 110) : QVariant();
        }

        if (state == DirectHit) return QIcon(QStringLiteral(":/resources/icons/folder-full.png"));
        if (state == IndirectHit) return QIcon(QStringLiteral(":/resources/icons/folder-hit.png"));
        return QIcon(QStringLiteral(":/resources/icons/folder-empty.png"));
    }

    return QIdentityProxyModel::data(proxyIndex, role);
}

// =============================================================================
// [ WIDGET IMPLEMENTATION ]
// =============================================================================

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {

    // Disable the OS-level slide and fade animations for all tooltips
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);

    // =========================================================================
    // Apply the custom proxy style GLOBALLY so QIcon can access it!
    // =========================================================================
    static bool globalStyleSet = false;
    if (!globalStyleSet) {
        // By passing nothing to the constructor, it automatically wraps the existing global style
        QApplication::setStyle(new AppProxyStyle()); 
        globalStyleSet = true;
    }

    setupUI();
}

/**
 * @brief Initializes the main layout, QSplitters, and primary views.
 */
void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6); 

    // --- 1. Top Panel: Directory & Collections Tree ---
    dirModel = new QStandardItemModel(this);
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    dirTreeView = new AssetTreeView(splitter);
    dirTreeView->setObjectName("AssetManagerTree");
    dirTreeView->setModel(proxyModel);
    dirTreeView->setItemDelegate(new AssetTreeDelegate(this));
    dirTreeView->setHeaderHidden(true); 
    
    dirTreeView->setEditTriggers(QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    dirTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
    
    connect(dirTreeView, &QTreeView::customContextMenuRequested, this, &AssetManagerWidget::onContextMenuRequested);
    connect(dirModel, &QStandardItemModel::itemChanged, this, &AssetManagerWidget::onItemChanged);

    // --- 2. Bottom Panel: Asset Thumbnail Grid ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0); 

    titleLabel = new QLabel("Select a folder to view assets...", bottomPanel);
    titleLabel->setObjectName("AssetManagerTitle"); 
    titleLabel->setTextFormat(Qt::RichText);

    assetListWidget = new QListWidget(bottomPanel);
    assetListWidget->setObjectName("AssetManagerGrid");
    assetListWidget->setViewMode(QListView::IconMode);
    assetListWidget->setIconSize(QSize(Constants::GRID_ICON_DISPLAY_SIZE, Constants::GRID_ICON_DISPLAY_SIZE));
    assetListWidget->setGridSize(QSize(Constants::GRID_CELL_WIDTH, Constants::GRID_CELL_HEIGHT));
    assetListWidget->setWordWrap(true);
    assetListWidget->setTextElideMode(Qt::ElideRight);
    assetListWidget->setResizeMode(QListView::Adjust); 
    assetListWidget->setMovement(QListView::Static);

    // =====================================================================
    // INITIALIZE INTERACTIVE TOOLTIPS
    // =====================================================================
    customToolTip = new CustomToolTip(this);
    activeToolTipItem = nullptr;
    
    // Force the grid to track mouse movements even when not clicking
    assetListWidget->setMouseTracking(true); 
    // Install the filter so we can intercept the ToolTip spawn requests
    assetListWidget->viewport()->installEventFilter(this);
    // =====================================================================

    assetListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(assetListWidget, &QListWidget::customContextMenuRequested, this, &AssetManagerWidget::onGridContextMenuRequested);
    connect(assetListWidget, &QListWidget::itemDoubleClicked, this, &AssetManagerWidget::onGridItemDoubleClicked);

    bottomLayout->addWidget(titleLabel);
    bottomLayout->addWidget(assetListWidget); 

    splitter->addWidget(dirTreeView);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
    connect(dirTreeView, &QTreeView::expanded, this, &AssetManagerWidget::onTreeExpanded);

    refreshAssetManager();
}

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
 * @brief Recursively searches the visible tree for a node matching the target path.
 */
QModelIndex AssetManagerWidget::findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath) {
    int childCount = proxyModel->rowCount(parentProxyIndex);
    
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childProxyIndex = proxyModel->index(i, 0, parentProxyIndex);
        QString path = proxyModel->data(childProxyIndex, Qt::UserRole).toString();

        if (path == targetPath) {
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

    // THE FIX: Capture the currently selected item's path
    QString selectedPath;
    QModelIndex currentIndex = dirTreeView->currentIndex();
    if (currentIndex.isValid()) {
        selectedPath = proxyModel->data(currentIndex, Qt::UserRole).toString();
    }

    dirModel->clear();
    
    if (assetListWidget) assetListWidget->clear();
    if (titleLabel) titleLabel->setText("Select a folder to view assets...");

    // ---------------------------------------------------------
    // 2. Rebuild Favorites Root
    // ---------------------------------------------------------
    favoritesRootItem = new QStandardItem(Constants::TERM_FAV_PLURAL);
    favoritesRootItem->setData("FAVORITES_ROOT", Qt::UserRole); 
    favoritesRootItem->setIcon(QIcon(":/resources/icons/favorites.png")); 
    
    favoritesRootItem->setFlags(favoritesRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(favoritesRootItem);

    QSqlQuery favQuery(QSqlDatabase::database("db_conn"));
    favQuery.exec("SELECT AssetFavoritePath, AssetFavoriteName FROM AssetFavorites");
    while (favQuery.next()) {
        QString savedPath = favQuery.value(0).toString();
        QString savedName = favQuery.value(1).toString();
        addFavoriteFolder(savedPath, savedName, false); 
    }

    // ---------------------------------------------------------
    // 3. Rebuild Collections Root
    // ---------------------------------------------------------
    collectionsRootItem = new QStandardItem(Constants::TERM_COL_PLURAL); 
    collectionsRootItem->setData("COLLECTIONS_ROOT", Qt::UserRole); 
    collectionsRootItem->setFlags(collectionsRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(collectionsRootItem);

    QSqlQuery collQuery(QSqlDatabase::database("db_conn"));
    collQuery.exec("SELECT AssetCollectionID, AssetCollectionName FROM AssetCollections");
    
    while (collQuery.next()) {
        QString colId = collQuery.value(0).toString();
        QString colName = collQuery.value(1).toString();

        QStandardItem *colItem = new QStandardItem(colName);
        colItem->setData("COLLECTION_" + colId, Qt::UserRole); 
        
        colItem->setFlags(colItem->flags() | Qt::ItemIsEditable);
        collectionsRootItem->appendRow(colItem);
    }

    // ---------------------------------------------------------
    // 4. Count Active Libraries
    // ---------------------------------------------------------
    int activeLibraryCount = 0;
    QSqlQuery countQuery(QSqlDatabase::database("db_conn"));
    countQuery.exec("SELECT COUNT(*) FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    if (countQuery.next()) {
        activeLibraryCount = countQuery.value(0).toInt();
    }

    // ---------------------------------------------------------
    // 5. Build Combined View Root (Only if 2+ libraries exist)
    // ---------------------------------------------------------
    combinedRootItem = nullptr; 
    
    if (activeLibraryCount > 1) {
        combinedRootItem = new QStandardItem("Combined View");
        combinedRootItem->setData("COMBINED_ROOT", Qt::UserRole); 
        combinedRootItem->setFlags(combinedRootItem->flags() & ~Qt::ItemIsEditable);
        
        combinedRootItem->appendRow(new QStandardItem("..."));
        dirModel->appendRow(combinedRootItem);
    }

    // ---------------------------------------------------------
    // 6. Build Visual Separator
    // ---------------------------------------------------------
    QStandardItem *separatorItem = new QStandardItem();
    separatorItem->setData("SEPARATOR", Qt::UserRole);
    separatorItem->setFlags(Qt::NoItemFlags); 
    dirModel->appendRow(separatorItem);

    // ---------------------------------------------------------
    // 7. Load Physical Asset Libraries
    // ---------------------------------------------------------
    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    
    while (libQuery.next()) {
        QString path = libQuery.value(0).toString();
        QDir dir(path);
        
        if (dir.exists()) {
            QStandardItem *rootItem = new QStandardItem(dir.dirName());
            rootItem->setData(path, Qt::UserRole); 
            rootItem->setFlags(rootItem->flags() & ~Qt::ItemIsEditable);
            
            QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
            if (it.hasNext()) {
                rootItem->appendRow(new QStandardItem("..."));
            }
            dirModel->appendRow(rootItem);
        } else {
            qWarning() << "Library path does not exist on disk:" << path;
        }
    }

    // =========================================================================
    // 8. RESTORE STATE AFTER WIPE
    // =========================================================================
    if (!expandedPaths.isEmpty()) {
        restoreExpandedState(QModelIndex(), expandedPaths);
    }

    // THE FIX: Find the previously selected item and re-trigger it!
    if (!selectedPath.isEmpty()) {
        QModelIndex newSelectedIndex = findProxyIndexByPath(QModelIndex(), selectedPath);
        
        if (newSelectedIndex.isValid()) {
            dirTreeView->setCurrentIndex(newSelectedIndex); // Highlights it in the tree
            dirTreeView->scrollTo(newSelectedIndex);        // Ensures it didn't scroll off-screen
            onFolderSelected(newSelectedIndex);             // Repopulates the lower asset grid
        }
    }
}

/**
 * @brief Handles lazy-loading of physical directories to keep memory footprint low.
 */
void AssetManagerWidget::onTreeExpanded(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QStandardItem *item = dirModel->itemFromIndex(sourceIndex);

    if (!item || !item->hasChildren()) return;
    if (item == favoritesRootItem || item == collectionsRootItem) return;

    const QString parentData = item->data(Qt::UserRole).toString();

    // =========================================================================
    // COMBINED VIEW: Aggregate subdirectories across all libraries
    // =========================================================================
    if (parentData == "COMBINED_ROOT" || parentData.startsWith("COMBINED_DIR_")) {
        QStandardItem *firstChild = item->child(0);
        if (!firstChild || firstChild->text() != "...") return;
        item->removeRow(0);

        const QString relPath = (parentData == "COMBINED_ROOT") ? "" : parentData.mid(13);

        QStringList activeLibraries;
        QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
        libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
        while (libQuery.next()) activeLibraries.append(libQuery.value(0).toString());

        QSet<QString> uniqueDirs;
        for (const QString& libPath : activeLibraries) {
            QDir dir(libPath);
            if (!relPath.isEmpty() && !dir.cd(relPath)) continue;
            for (const QFileInfo& info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase))
                uniqueDirs.insert(info.fileName());
        }

        QStringList sortedDirs = uniqueDirs.values();
        sortedDirs.sort(Qt::CaseInsensitive);

        QList<QStandardItem*> children;
        children.reserve(sortedDirs.size());

        for (const QString& dirName : sortedDirs) {
            const QString nextRelPath = relPath.isEmpty() ? dirName : relPath + "/" + dirName;

            QStandardItem* child = new QStandardItem(dirName);
            child->setData("COMBINED_DIR_" + nextRelPath, Qt::UserRole);
            child->setFlags(child->flags() & ~Qt::ItemIsEditable);

            for (const QString& libPath : activeLibraries) {
                QDir subDir(libPath);
                if (!subDir.cd(nextRelPath)) continue;
                QDirIterator it(subDir.absolutePath(), QDir::Dirs | QDir::NoDotAndDotDot);
                if (it.hasNext()) { child->appendRow(new QStandardItem("...")); break; }
            }

            children.append(child);
        }

        dirTreeView->setUpdatesEnabled(false);
        if (!children.isEmpty()) item->appendRows(children);
        dirTreeView->setUpdatesEnabled(true);
        return;
    }

    // =========================================================================
    // STANDARD PHYSICAL FOLDER LOGIC
    // =========================================================================
    QStandardItem *firstChild = item->child(0);
    if (!firstChild || !firstChild->data(Qt::UserRole).toString().isEmpty() || firstChild->text() != "...") return;
    item->removeRow(0);

    const QString parentPath = item->data(Qt::UserRole).toString();
    const QFileInfoList list = QDir(parentPath).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);

    QList<QStandardItem*> children;
    children.reserve(list.size());

    for (const QFileInfo& info : list) {
        QStandardItem* child = new QStandardItem(info.fileName());
        child->setData(info.absoluteFilePath(), Qt::UserRole);
        child->setFlags(child->flags() & ~Qt::ItemIsEditable);

        QDirIterator it(info.absoluteFilePath(), QDir::Dirs | QDir::NoDotAndDotDot);
        if (it.hasNext()) child->appendRow(new QStandardItem("..."));

        children.append(child);
    }

    dirTreeView->setUpdatesEnabled(false);
    if (!children.isEmpty()) item->appendRows(children);
    dirTreeView->setUpdatesEnabled(true);
}

/**
 * @brief Adds a folder (or virtual combined directory) to the Shortcuts database and visually updates the tree.
 */
void AssetManagerWidget::addFavoriteFolder(const QString& folderPath, const QString& displayName, bool saveToDb) {
    // 1. Prevent duplicate entries during current session
    for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
        if (favoritesRootItem->child(i)->data(Qt::UserRole).toString() == folderPath) {
            return; 
        }
    }

    // 2. Check if this is a virtual directory or a physical hard drive path
    bool isVirtualNode = folderPath.startsWith("COMBINED_DIR_");

    // 3. Determine the default name if none was provided
    QString defaultName;
    if (isVirtualNode) {
        // Extract just the final folder name from "COMBINED_DIR_path/to/folder"
        QString relPath = folderPath.mid(13);
        defaultName = relPath.section('/', -1); 
    } else {
        defaultName = QDir(folderPath).dirName();
    }
    QString nameToSave = displayName.isEmpty() ? defaultName : displayName;

    // 4. Save to SQLite
    if (saveToDb) {
        QSqlQuery query(QSqlDatabase::database("db_conn"));
        query.prepare("INSERT OR IGNORE INTO AssetFavorites (AssetFavoritePath, AssetFavoriteName) VALUES (:path, :name)");
        query.bindValue(":path", folderPath);
        query.bindValue(":name", nameToSave);
        if (!query.exec()) {
            qWarning() << "[!] Failed to save shortcut to DB:" << query.lastError().text();
        }
    }

    // 5. Build the visual UI Item
    QStandardItem *favItem = new QStandardItem(nameToSave);
    favItem->setFlags(favItem->flags() | Qt::ItemIsEditable); 

    // 6. Validation & Child Assignment
    if (isVirtualNode) {
        favItem->setData(folderPath, Qt::UserRole);
        
        // THE FIX: Check if this virtual folder actually has subdirectories before adding the dummy node
        QString relPath = folderPath.mid(13);
        bool hasSubDirs = false;
        
        QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
        libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
        
        while (libQuery.next()) {
            QDir dir(libQuery.value(0).toString());
            if (dir.cd(relPath)) {
                QDirIterator it(dir.absolutePath(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
                if (it.hasNext()) {
                    hasSubDirs = true;
                    break; // As soon as we find one subfolder across any drive, we know it needs an arrow!
                }
            }
        }
        
        // Only add the dummy node (which draws the arrow) if there are actual subfolders
        if (hasSubDirs) {
            favItem->appendRow(new QStandardItem("..."));
        }
        
    } else {
        QDir dir(folderPath);
        if (!dir.exists()) {
            favItem->setData("BROKEN_PATH", Qt::UserRole);
            favItem->setText(nameToSave + " (Not Found)");
            favoritesRootItem->appendRow(favItem);
            return;
        }

        favItem->setData(folderPath, Qt::UserRole);
        QDirIterator it(folderPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
        if (it.hasNext()) {
            favItem->appendRow(new QStandardItem("..."));
        }
    }

    favoritesRootItem->appendRow(favItem);
}

/**
 * @brief Removes a folder from the Favorites database and visually removes it from the tree.
 */
void AssetManagerWidget::removeFavoriteFolder(const QString& folderPath) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("DELETE FROM AssetFavorites WHERE AssetFavoritePath = :path");
    query.bindValue(":path", folderPath);
    
    if (!query.exec()) {
        qWarning() << "Failed to remove favorite from DB:" << query.lastError().text();
    }

    // Visually remove from the UI Tree
    for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
        if (favoritesRootItem->child(i)->data(Qt::UserRole).toString() == folderPath) {
            favoritesRootItem->removeRow(i);
            break; 
        }
    }
}

/**
 * @brief Triggered when a user clicks a node in the directory tree. Processes the click and populates the grid.
 */
void AssetManagerWidget::onFolderSelected(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QString folderPath = dirModel->data(sourceIndex, Qt::UserRole).toString();
    QString folderName = dirModel->data(sourceIndex, Qt::DisplayRole).toString();

    // ---------------------------------------------------------
    // 1. Handle Structural Node Clicks
    // ---------------------------------------------------------
    if (folderPath == "FAVORITES_ROOT" || folderPath == "COLLECTIONS_ROOT" || folderPath == "COMBINED_ROOT") {
        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        dirTreeView->setExpanded(proxyIndex, !isExpanded);
        return; 
    }

    if (folderPath.isEmpty() || folderPath == "BROKEN_PATH" || folderPath == "SEPARATOR") return;

    // ---------------------------------------------------------
    // 2. Fetch Assets (Branch between DB Collection vs Hard Drive)
    // ---------------------------------------------------------
    QList<AssetHit> discoveredAssets;

    if (folderPath.startsWith("COLLECTION_")) {
        int collId = folderPath.mid(11).toInt();
        discoveredAssets = parseCollectionAssets(collId);
    } else if (folderPath.startsWith("COMBINED_DIR_")) {
        QString relPath = folderPath.mid(13);
        discoveredAssets = parseCombinedAssets(relPath);
    } else {
        discoveredAssets = parseFolderAssets(folderPath);
    }

    // ---------------------------------------------------------
    // 3. Update Title Labels
    // ---------------------------------------------------------
    const int assetCount = discoveredAssets.size();
    if (assetCount > 0) {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span>"
                                    "<span style='color: %2; font-size: 14px; font-weight: bold;'>&nbsp;&nbsp;(%3)</span>")
                            .arg(folderName.toHtmlEscaped(), Constants::COLOR_ACCENT_BLUE, QString::number(assetCount)));
    } else {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span>").arg(folderName.toHtmlEscaped()));
    }
    
    // ---------------------------------------------------------
    // 4. Render Asset Grid — Phase 1: items + metadata, no thumbnails yet
    // ---------------------------------------------------------
    std::sort(discoveredAssets.begin(), discoveredAssets.end(), [](const AssetHit& a, const AssetHit& b) {
        const QString& na = a.displayName.isEmpty() ? a.assetFileName : a.displayName;
        const QString& nb = b.displayName.isEmpty() ? b.assetFileName : b.displayName;
        return na.compare(nb, Qt::CaseInsensitive) < 0;
    });

    m_pendingThumbs.clear();
    m_pendingThumbs.reserve(discoveredAssets.size());

    assetListWidget->setUpdatesEnabled(false);
    assetListWidget->clear();

    for (int i = 0; i < discoveredAssets.size(); ++i) {
        const AssetHit& hit = discoveredAssets[i];

        const QString cleanName = hit.displayName.isEmpty()
            ? QFileInfo(hit.assetFileName).baseName()
            : hit.displayName;
        const QString fullPath = QDir(hit.folderPath).filePath(hit.assetFileName);

        QListWidgetItem* item = new QListWidgetItem();
        item->setText(cleanName);
        item->setData(Qt::UserRole, fullPath);

        if (!hit.matchingImages.isEmpty())
            m_pendingThumbs.append({i, QDir(hit.folderPath).filePath(hit.matchingImages.first())});

        // Tooltip
        const QFileInfo info(fullPath);
        const QString ext = info.suffix().toUpper();
        const double sz = info.size();
        item->setData(Qt::UserRole + 1, QString(
            "<div style='white-space: pre-wrap;'>"
            "<span style='font-weight: bold; color: %1;'>%2</span><br/>"
            "Size: %3<br/>"
            "Modified: %4<br/><br/>"
            "<span style='color: %5; font-size: 11px;'>%6</span>"
            "</div>"
        ).arg(Constants::COLOR_TOOLTIP_ACCENT,
              ext.isEmpty() ? QStringLiteral("File") : QString(".%1 File").arg(ext),
              sz > (1024 * 1024) ? QString::number(sz / (1024.0 * 1024.0), 'f', 2) + " MB"
                                 : QString::number(sz / 1024.0, 'f', 2) + " KB",
              info.lastModified().toString("MM/dd/yyyy h:mm ap"),
              Constants::COLOR_TOOLTIP_MUTED,
              QDir::toNativeSeparators(hit.folderPath).toHtmlEscaped()));

        assetListWidget->addItem(item);
    }

    assetListWidget->setUpdatesEnabled(true);
    QMetaObject::invokeMethod(assetListWidget, "doItemsLayout");

    // Phase 2: thumbnails load in batches so the grid is visible immediately
    if (!m_pendingThumbs.isEmpty())
        QTimer::singleShot(0, this, &AssetManagerWidget::processNextThumbnailBatch);
}

/**
 * @brief Loads thumbnails in batches of 20 per event-loop tick so the grid stays responsive.
 */
void AssetManagerWidget::processNextThumbnailBatch() {
    constexpr int BATCH = 20;
    int count = 0;

    assetListWidget->setUpdatesEnabled(false);

    while (!m_pendingThumbs.isEmpty() && count < BATCH) {
        const QPair<int, QString> job = m_pendingThumbs.takeFirst();
        QListWidgetItem* item = assetListWidget->item(job.first);
        if (item) {
            QPixmap raw(job.second);
            if (!raw.isNull()) {
                const QPixmap scaled = raw.scaled(
                    QSize(Constants::THUMB_RENDER_SIZE, Constants::THUMB_RENDER_SIZE),
                    Qt::KeepAspectRatio, Qt::SmoothTransformation);

                QPixmap canvas(Constants::THUMB_RENDER_SIZE, Constants::THUMB_CANVAS_HEIGHT);
                canvas.fill(Qt::transparent);

                QPainter p(&canvas);
                QLinearGradient grad(0, 0, 0, Constants::THUMB_RENDER_SIZE);
                grad.setColorAt(0.0, QColor(Constants::COLOR_THUMB_BG_START));
                grad.setColorAt(1.0, QColor(Constants::COLOR_THUMB_BG_END));
                p.fillRect(0, 0, Constants::THUMB_RENDER_SIZE, Constants::THUMB_RENDER_SIZE, grad);
                p.drawPixmap((Constants::THUMB_RENDER_SIZE - scaled.width()) / 2, 0, scaled);
                p.end();

                QIcon icon;
                icon.addPixmap(canvas, QIcon::Normal);
                icon.addPixmap(canvas, QIcon::Selected);
                item->setIcon(icon);
            }
        }
        ++count;
    }

    assetListWidget->setUpdatesEnabled(true);
    QMetaObject::invokeMethod(assetListWidget, "doItemsLayout");

    if (!m_pendingThumbs.isEmpty())
        QTimer::singleShot(0, this, &AssetManagerWidget::processNextThumbnailBatch);
}

/**
 * @brief Parses physical folder directories for 3D assets and paired thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseFolderAssets(const QString& folderPath) {
    static const QSet<QString> imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    const QFileInfoList files = QDir(folderPath).entryInfoList(QDir::Files | QDir::NoSymLinks);

    struct FileGroup {
        QStringList nonImages;
        QString bestImage;
        qint64 maxBytes = -1;
        QStringList otherImages;
    };
    QHash<QString, FileGroup> groups;
    groups.reserve(files.size());

    for (const QFileInfo& fi : files) {
        const QString base = fi.baseName();
        if (imageExts.contains(fi.suffix().toLower())) {
            const qint64 sz = fi.size();
            auto& g = groups[base];
            if (sz > g.maxBytes) {
                if (!g.bestImage.isEmpty()) g.otherImages.prepend(g.bestImage);
                g.bestImage = fi.fileName();
                g.maxBytes = sz;
            } else {
                g.otherImages.append(fi.fileName());
            }
        } else {
            groups[base].nonImages.append(fi.fileName());
        }
    }

    QList<AssetHit> finalHits;
    finalHits.reserve(groups.size());
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const FileGroup& g = it.value();
        if (!g.bestImage.isEmpty() && !g.nonImages.isEmpty()) {
            for (const QString& nonImg : g.nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImg;
                hit.matchingImages.reserve(1 + g.otherImages.size());
                hit.matchingImages.append(g.bestImage);
                hit.matchingImages.append(g.otherImages);
                finalHits.append(std::move(hit));
            }
        }
    }
    return finalHits;
}

/**
 * @brief Queries the SQLite database for virtual collection items and locates their physical thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseCollectionAssets(int collectionId) {
    static const QSet<QString> imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    QList<AssetHit> finalHits;
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
    query.bindValue(":id", collectionId);
    if (!query.exec()) {
        qWarning() << "Failed to load collection items:" << query.lastError().text();
        return finalHits;
    }

    // Group valid asset paths by folder so each folder is scanned only once
    QHash<QString, QStringList> folderToAssets;
    while (query.next()) {
        const QString fullPath = query.value(0).toString();
        if (QFileInfo::exists(fullPath)) {
            const QFileInfo fi(fullPath);
            folderToAssets[fi.absolutePath()].append(fi.fileName());
        }
    }

    for (auto folderIt = folderToAssets.cbegin(); folderIt != folderToAssets.cend(); ++folderIt) {
        const QString& folderPath = folderIt.key();
        const QStringList& assetFiles = folderIt.value();

        // Build set of relevant basenames so the directory scan stays focused
        QSet<QString> relevantBases;
        relevantBases.reserve(assetFiles.size());
        for (const QString& af : assetFiles) relevantBases.insert(QFileInfo(af).baseName());

        // Single scan of this folder to find images for all assets in this batch
        struct ImageGroup { QString bestImage; qint64 maxBytes = -1; QStringList others; };
        QHash<QString, ImageGroup> imagesByBase;
        const QFileInfoList allFiles = QDir(folderPath).entryInfoList(QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo& fi : allFiles) {
            const QString base = fi.baseName();
            if (!relevantBases.contains(base) || !imageExts.contains(fi.suffix().toLower())) continue;
            const qint64 sz = fi.size();
            auto& ig = imagesByBase[base];
            if (sz > ig.maxBytes) {
                if (!ig.bestImage.isEmpty()) ig.others.prepend(ig.bestImage);
                ig.bestImage = fi.fileName();
                ig.maxBytes = sz;
            } else {
                ig.others.append(fi.fileName());
            }
        }

        for (const QString& assetFile : assetFiles) {
            AssetHit hit;
            hit.folderPath = folderPath;
            hit.assetFileName = assetFile;
            const auto igIt = imagesByBase.constFind(QFileInfo(assetFile).baseName());
            if (igIt != imagesByBase.cend()) {
                hit.matchingImages.reserve(1 + igIt->others.size());
                hit.matchingImages.append(igIt->bestImage);
                hit.matchingImages.append(igIt->others);
            }
            finalHits.append(std::move(hit));
        }
    }

    return finalHits;
}

/**
 * @brief Constructs and routes right-click context menus based on the data type of the clicked node.
 */
void AssetManagerWidget::onContextMenuRequested(const QPoint &pos) {
    QModelIndex proxyIndex = dirTreeView->indexAt(pos);
    
    // =========================================================================
    // DEDICATED MENU: EMPTY SPACE (BACKGROUND CLICK IN TREE)
    // =========================================================================
    if (!proxyIndex.isValid()) {
        QMenu emptyMenu(this);
        emptyMenu.setObjectName("AssetManagerContextMenu");
        
        QAction *refreshAction = emptyMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        QAction *selectedAction = emptyMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));
        
        if (selectedAction == refreshAction) {
            refreshAssetManager();
        }
        return; 
    }

    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QString folderPath = dirModel->data(sourceIndex, Qt::UserRole).toString();
    QString folderName = dirModel->data(sourceIndex, Qt::DisplayRole).toString();

    // Prevent context menus on non-interactive structural nodes
    if (folderPath.isEmpty() || folderPath == "BROKEN_PATH" || folderPath == "SEPARATOR") {
        return; 
    }

    // =========================================================================
    // DEDICATED MENU: "FAVORITES" ROOT NODE
    // =========================================================================
    if (folderPath == "FAVORITES_ROOT") {
        QMenu rootMenu(this);
        rootMenu.setObjectName("AssetManagerContextMenu");
        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        bool hasChildren = proxyModel->hasChildren(proxyIndex); 
        
        QAction *expandAction = nullptr;
        QAction *collapseAction = nullptr;
        
        if (isExpanded) {
            collapseAction = rootMenu.addAction(QIcon(":/resources/icons/collapse.png"), "Collapse");
        } else {
            expandAction = rootMenu.addAction(QIcon(":/resources/icons/expand.png"), "Expand");
            expandAction->setEnabled(hasChildren); 
        }
        
        rootMenu.addSeparator();
        QAction *refreshAction = rootMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        
        QAction *selectedAction = rootMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));
        
        if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
        else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex); 
        else if (selectedAction == refreshAction) refreshAssetManager();
        
        return; 
    }

    // =========================================================================
    // DEDICATED MENU: "COLLECTIONS" ROOT NODE
    // =========================================================================
    if (folderPath == "COLLECTIONS_ROOT") {
        QMenu rootMenu(this);
        rootMenu.setObjectName("AssetManagerContextMenu");

        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        bool hasChildren = proxyModel->hasChildren(proxyIndex); 

        QAction *expandAction = nullptr;
        QAction *collapseAction = nullptr;

        if (isExpanded) {
            collapseAction = rootMenu.addAction(QIcon(":/resources/icons/collapse.png"), "Collapse");
        } else {
            expandAction = rootMenu.addAction(QIcon(":/resources/icons/expand.png"), "Expand");
            expandAction->setEnabled(hasChildren); 
        }

        rootMenu.addSeparator();
        QAction *refreshAction = rootMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        QAction *selectedAction = rootMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
        else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex); 
        else if (selectedAction == refreshAction) refreshAssetManager();
        
        return; 
    }

    // =========================================================================
    // DEDICATED MENU: COMBINED VIEW VIRTUAL NODES
    // =========================================================================
    if (folderPath == "COMBINED_ROOT" || folderPath.startsWith("COMBINED_DIR_")) {
        QMenu combinedMenu(this);
        combinedMenu.setObjectName("AssetManagerContextMenu");
        
        // 1. Setup Actions
        QAction *shortcutAction = nullptr;
        QAction *removeShortcutAction = nullptr;
        QAction *browseAction = nullptr;
        QString singlePhysicalPath;

        if (folderPath.startsWith("COMBINED_DIR_")) {
            
            // A. Shortcut Management (POSITION 1)
            bool isAlreadyShortcut = false;
            for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
                if (favoritesRootItem->child(i)->data(Qt::UserRole).toString() == folderPath) {
                    isAlreadyShortcut = true;
                    break;
                }
            }

            if (!isAlreadyShortcut) {
                shortcutAction = combinedMenu.addAction(QIcon(":/resources/icons/favorites.png"), 
                                                       QStringLiteral("Add to %1").arg(Constants::TERM_FAV_PLURAL));
            } else {
                removeShortcutAction = combinedMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), 
                                                             QStringLiteral("Remove %1").arg(Constants::TERM_FAV_SINGULAR));
            }
            
            // B. Browse Folder (POSITION 2)
            QString relPath = folderPath.mid(13);
            QStringList existingPaths;
            QStringList pathsWithFiles;
            
            QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
            libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
            while (libQuery.next()) {
                QDir dir(libQuery.value(0).toString());
                if (dir.exists(relPath)) {
                    QString absPath = dir.absoluteFilePath(relPath);
                    existingPaths.append(absPath);
                    
                    // Ultra-fast peek to see if this physical folder actually contains any files
                    QDirIterator it(absPath, QDir::Files | QDir::NoSymLinks, QDirIterator::NoIteratorFlags);
                    if (it.hasNext()) {
                        pathsWithFiles.append(absPath);
                    }
                }
            }
            
            // Unconditionally add the action to maintain menu consistency
            browseAction = combinedMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
            
            // Determine if we can safely route to a single physical directory
            if (existingPaths.size() == 1) {
                singlePhysicalPath = existingPaths.first();
            } else if (pathsWithFiles.size() == 1) {
                singlePhysicalPath = pathsWithFiles.first();
            } else {
                // The folder spans multiple drives (or is entirely empty), disable the action!
                browseAction->setEnabled(false);
            }

            combinedMenu.addSeparator();
        }

        // 2. Tree Navigation
        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        bool hasChildren = proxyModel->hasChildren(proxyIndex); 
        
        QAction *expandAction = nullptr;
        QAction *expandBranchAction = nullptr;
        QAction *collapseAction = nullptr;

        if (!isExpanded) {
            expandAction = combinedMenu.addAction(QIcon(":/resources/icons/expand.png"), "Expand");
            expandAction->setEnabled(hasChildren); 
        }
        
        expandBranchAction = combinedMenu.addAction(QIcon(":/resources/icons/expand-branch.png"), "Expand Branch");
        expandBranchAction->setEnabled(hasChildren); 

        if (isExpanded) {
            collapseAction = combinedMenu.addAction(QIcon(":/resources/icons/collapse.png"), "Collapse");
        }
        
        combinedMenu.addSeparator();
        QAction *refreshAction = combinedMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        // 3. Execution
        QAction *selectedAction = combinedMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (shortcutAction && selectedAction == shortcutAction) addFavoriteFolder(folderPath, folderName);
        else if (removeShortcutAction && selectedAction == removeShortcutAction) removeFavoriteFolder(folderPath);
        else if (browseAction && selectedAction == browseAction) QDesktopServices::openUrl(QUrl::fromLocalFile(singlePhysicalPath));
        else if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
        else if (expandBranchAction && selectedAction == expandBranchAction) expandNodeRecursively(proxyIndex);
        else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex); 
        else if (selectedAction == refreshAction) refreshAssetManager();
        
        return; 
    }

    // =========================================================================
    // DEDICATED MENU: INDIVIDUAL DB COLLECTION ITEMS
    // =========================================================================
    if (folderPath.startsWith("COLLECTION_")) {
        QMenu collMenu(this);
        collMenu.setObjectName("AssetManagerContextMenu");

        QAction *renameAction = collMenu.addAction(QIcon(":/resources/icons/rename.png"), 
                                                   QStringLiteral("Rename %1").arg(Constants::TERM_COL_SINGULAR));
        collMenu.addSeparator();

        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        bool hasChildren = proxyModel->hasChildren(proxyIndex); 
        QAction *expandAction = nullptr;
        QAction *collapseAction = nullptr;

        if (isExpanded) {
            collapseAction = collMenu.addAction(QIcon(":/resources/icons/collapse.png"), "Collapse");
        } else {
            expandAction = collMenu.addAction(QIcon(":/resources/icons/expand.png"), "Expand");
            expandAction->setEnabled(hasChildren); 
        }
        
        collMenu.addSeparator();
        QAction *refreshAction = collMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        QAction *selectedAction = collMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (renameAction && selectedAction == renameAction) dirTreeView->edit(proxyIndex);
        else if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
        else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex); 
        else if (selectedAction == refreshAction) refreshAssetManager();
        
        return;
    }

    // =========================================================================
    // STANDARD MENU: PHYSICAL HARD DRIVE DIRECTORIES
    // =========================================================================
    
    bool isAlreadyFavorite = false;
    bool isChildOfFavorite = false;
    
    QString targetPath = QDir::fromNativeSeparators(folderPath);
    if (!targetPath.endsWith('/')) targetPath += '/';

    for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
        QString favPath = favoritesRootItem->child(i)->data(Qt::UserRole).toString();
        QString normalizedFav = QDir::fromNativeSeparators(favPath);
        if (!normalizedFav.endsWith('/')) normalizedFav += '/';

        if (targetPath == normalizedFav) {
            isAlreadyFavorite = true;
            break;
        } 
        else if (targetPath.startsWith(normalizedFav)) {
            isChildOfFavorite = true;
        }
    }

    QStandardItem *clickedItem = dirModel->itemFromIndex(sourceIndex);
    bool isTopLevelFavorite = (clickedItem && clickedItem->parent() == favoritesRootItem);

    QMenu contextMenu(this);
    contextMenu.setObjectName("AssetManagerContextMenu");

    QAction *favoriteAction = nullptr;
    QAction *renameAction = nullptr;

    if (!isAlreadyFavorite && !isChildOfFavorite) {
        favoriteAction = contextMenu.addAction(QIcon(":/resources/icons/favorites.png"), 
                                               QStringLiteral("Add to %1").arg(Constants::TERM_FAV_PLURAL));
    }
    
    if (isAlreadyFavorite && isTopLevelFavorite) {
        renameAction = contextMenu.addAction(QIcon(":/resources/icons/rename.png"), 
                                             QStringLiteral("Rename %1").arg(Constants::TERM_FAV_SINGULAR));
    }

    QAction *browseAction = contextMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
    contextMenu.addSeparator();

    bool isExpanded = dirTreeView->isExpanded(proxyIndex);
    bool hasChildren = proxyModel->hasChildren(proxyIndex); 

    QAction *expandAction = nullptr;
    QAction *expandBranchAction = nullptr;
    QAction *collapseAction = nullptr;

    if (!isExpanded) {
        expandAction = contextMenu.addAction(QIcon(":/resources/icons/expand.png"), "Expand");
        expandAction->setEnabled(hasChildren); 
    }
    
    expandBranchAction = contextMenu.addAction(QIcon(":/resources/icons/expand-branch.png"), "Expand Branch");
    expandBranchAction->setEnabled(hasChildren); 

    if (isExpanded) {
        collapseAction = contextMenu.addAction(QIcon(":/resources/icons/collapse.png"), "Collapse");
    }
    contextMenu.addSeparator();

    QAction *removeFavoriteAction = nullptr;
    
    if (isAlreadyFavorite) {
        removeFavoriteAction = contextMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), 
                                                     QStringLiteral("Remove From %1").arg(Constants::TERM_FAV_PLURAL));
    }

    QAction *refreshAction = contextMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

    QAction *selectedAction = contextMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

    if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
    else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex);
    else if (selectedAction == expandBranchAction) expandNodeRecursively(proxyIndex);
    else if (selectedAction == browseAction) QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    else if (favoriteAction && selectedAction == favoriteAction) addFavoriteFolder(folderPath, folderName);
    else if (removeFavoriteAction && selectedAction == removeFavoriteAction) removeFavoriteFolder(folderPath);
    else if (renameAction && selectedAction == renameAction) dirTreeView->edit(proxyIndex);
    else if (selectedAction == refreshAction) refreshAssetManager();
}

/**
 * @brief Constructs and routes right-click context menus for the asset grid.
 */
void AssetManagerWidget::onGridContextMenuRequested(const QPoint &pos) {
    QListWidgetItem *item = assetListWidget->itemAt(pos);

    // =========================================================================
    // DEDICATED MENU: EMPTY SPACE (BACKGROUND CLICK IN GRID)
    // =========================================================================
    if (!item) {
        QMenu emptyMenu(this);
        emptyMenu.setObjectName("AssetManagerContextMenu");

        QAction *refreshAction = emptyMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        QAction *selectedAction = emptyMenu.exec(assetListWidget->viewport()->mapToGlobal(pos));

        if (selectedAction == refreshAction) {
            QModelIndex currentIndex = dirTreeView->currentIndex();
            if (currentIndex.isValid()) {
                onFolderSelected(currentIndex); 
            } else {
                refreshAssetManager(); 
            }
        }
        return; 
    }

    // =========================================================================
    // DEDICATED MENU: SPECIFIC ASSET ITEM
    // =========================================================================
    // Extract the raw path we hid inside the Qt::UserRole
    QString fullPath = item->data(Qt::UserRole).toString();
    QString folderPath = QFileInfo(fullPath).absolutePath();

    QMenu itemMenu(this);
    itemMenu.setObjectName("AssetManagerContextMenu");

    // Action 1: Open
    QAction *openAction = itemMenu.addAction(QIcon(":/resources/icons/open-item.png"), "Open");

    // Detect collection context and add collection-specific actions immediately after Open
    QAction *removeFromColAction = nullptr;
    QAction *goToFolderAction = nullptr;
    int currentCollectionId = -1;
    QModelIndex currentTreeIndex = dirTreeView->currentIndex();

    if (currentTreeIndex.isValid()) {
        QModelIndex sourceIndex = proxyModel->mapToSource(currentTreeIndex);
        QString currentTreeData = dirModel->data(sourceIndex, Qt::UserRole).toString();

        if (currentTreeData.startsWith("COLLECTION_")) {
            currentCollectionId = currentTreeData.mid(11).toInt();
            goToFolderAction = itemMenu.addAction(QIcon(":/resources/icons/folder-hit.png"), "Go to Folder");
        }
    }

    itemMenu.addSeparator();

    if (currentCollectionId != -1)
        removeFromColAction = itemMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), "Remove From Collection");




// =========================================================================
    // Action 2: Contextual Collection Management (Add vs Move/Copy)
    // =========================================================================
    QMenu *addMenu = nullptr;
    QMenu *moveMenu = nullptr;
    QMenu *copyMenu = nullptr;

    // If we are NOT in a collection, build the standard "Add" menu
    if (currentCollectionId == -1) {
        addMenu = new QMenu("Add To Collection", &itemMenu);
        addMenu->setIcon(QIcon(":/resources/icons/collections.png"));
        addMenu->setObjectName("AssetManagerContextMenu");
        addMenu->setAttribute(Qt::WA_TranslucentBackground);
        addMenu->setStyleSheet("QMenu { margin: 0px 4px; }");
    } 
    // If we ARE in a collection, build the "Move" and "Copy" menus
    else {
        moveMenu = new QMenu("Move To Collection", &itemMenu);
        moveMenu->setIcon(QIcon(":/resources/icons/collections.png")); // Or a dedicated move icon if you have one
        moveMenu->setObjectName("AssetManagerContextMenu");
        moveMenu->setAttribute(Qt::WA_TranslucentBackground);
        moveMenu->setStyleSheet("QMenu { margin: 0px 4px; }");

        copyMenu = new QMenu("Copy To Collection", &itemMenu);
        copyMenu->setIcon(QIcon(":/resources/icons/collections.png")); // Or a dedicated copy icon
        copyMenu->setObjectName("AssetManagerContextMenu");
        copyMenu->setAttribute(Qt::WA_TranslucentBackground);
        copyMenu->setStyleSheet("QMenu { margin: 0px 4px; }");
    }

    // Dynamically query collections from SQLite
    QSqlQuery collQuery(QSqlDatabase::database("db_conn"));
    collQuery.exec("SELECT AssetCollectionID, AssetCollectionName FROM AssetCollections");
    
    bool hasCollections = false;
    bool hasOtherCollections = false; // Tracks if there are valid destinations to move/copy to
    
    while (collQuery.next()) {
        hasCollections = true;
        int colId = collQuery.value(0).toInt();
        QString colName = collQuery.value(1).toString();

        if (currentCollectionId == -1) {
            // STANDARD ADD
            QAction *addAction = addMenu->addAction(colName);
            connect(addAction, &QAction::triggered, [this, colId, fullPath]() {
                addAssetToCollection(fullPath, colId);
            });
        } else {
            // MOVE & COPY (Exclude the collection the user is currently standing in!)
            if (colId != currentCollectionId) {
                hasOtherCollections = true;

                // Move Lambda: Remove from current, Add to new, Refresh Grid
                QAction *moveAction = moveMenu->addAction(colName);
                connect(moveAction, &QAction::triggered, [this, fullPath, currentCollectionId, colId, currentTreeIndex]() {
                    removeAssetFromCollection(fullPath, currentCollectionId);
                    addAssetToCollection(fullPath, colId);
                    onFolderSelected(currentTreeIndex); // Instantly removes it from the current view
                });

                // Copy Lambda: Just Add to new (No refresh needed, it stays in the current view)
                QAction *copyAction = copyMenu->addAction(colName);
                connect(copyAction, &QAction::triggered, [this, fullPath, colId]() {
                    addAssetToCollection(fullPath, colId);
                });
            }
        }
    }

    // Attach the appropriate menus to the context list and grey them out if empty
    if (currentCollectionId == -1) {
        if (!hasCollections) addMenu->setEnabled(false);
        itemMenu.addMenu(addMenu);
    } else {
        if (!hasOtherCollections) {
            moveMenu->setEnabled(false);
            copyMenu->setEnabled(false);
        }
        itemMenu.addMenu(moveMenu);
        itemMenu.addMenu(copyMenu);
    }




    
    // Action 3: Browse Folder
    QAction *browseAction = itemMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");

    itemMenu.addSeparator();

    // Action 4: Refresh
    QAction *refreshAction = itemMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

    // --- Execute Menu & Handle Clicks ---
    QAction *selectedAction = itemMenu.exec(assetListWidget->viewport()->mapToGlobal(pos));

    if (selectedAction == openAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
    } 
    else if (removeFromColAction && selectedAction == removeFromColAction) {
        removeAssetFromCollection(fullPath, currentCollectionId);
        onFolderSelected(currentTreeIndex);
    }
    else if (goToFolderAction && selectedAction == goToFolderAction) {
        navigateToFolderInTree(folderPath);
    }
    else if (selectedAction == browseAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    } 
    else if (selectedAction == refreshAction) {
        QModelIndex currentIndex = dirTreeView->currentIndex();
        if (currentIndex.isValid()) {
            onFolderSelected(currentIndex); 
        } else {
            refreshAssetManager(); 
        }
    }
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
 * @brief Saves a renamed virtual item (Favorite or Collection) back to the SQLite database.
 */
void AssetManagerWidget::onItemChanged(QStandardItem *item) {
    if (!item) return;

    // ---------------------------------------------------------
    // Handle Favorites Renaming
    // ---------------------------------------------------------
    if (item->parent() == favoritesRootItem) {
        QString newName = item->text();
        QString path = item->data(Qt::UserRole).toString();

        if (path.isEmpty() || path == "BROKEN_PATH") return;

        QSqlQuery query(QSqlDatabase::database("db_conn"));
        query.prepare("UPDATE AssetFavorites SET AssetFavoriteName = :name WHERE AssetFavoritePath = :path");
        query.bindValue(":name", newName);
        query.bindValue(":path", path);

        if (!query.exec()) {
            qWarning() << "[!] Failed to update favorite name in DB:" << query.lastError().text();
        }
    } 
    // ---------------------------------------------------------
    // Handle Collection Renaming
    // ---------------------------------------------------------
    else if (item->parent() == collectionsRootItem) {
        QString newName = item->text();
        QString dataStr = item->data(Qt::UserRole).toString();

        if (dataStr.startsWith("COLLECTION_")) {
            int collId = dataStr.mid(11).toInt(); 

            QSqlQuery query(QSqlDatabase::database("db_conn"));
            query.prepare("UPDATE AssetCollections SET AssetCollectionName = :name WHERE AssetCollectionID = :id");
            query.bindValue(":name", newName);
            query.bindValue(":id", collId);

            if (!query.exec()) {
                qWarning() << "[!] Failed to update collection name in DB:" << query.lastError().text();
            }
        }
    }
}

/**
 * @brief Parses identical relative paths across all physical libraries and handles filename collisions.
 */
QList<AssetHit> AssetManagerWidget::parseCombinedAssets(const QString& relativePath) {
    QList<AssetHit> rawHits;
    
    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    
    // 1. Gather all assets from all libraries sharing this folder path
    while (libQuery.next()) {
        QDir dir(libQuery.value(0).toString());
        if (dir.cd(relativePath)) {
            rawHits.append(parseFolderAssets(dir.absolutePath()));
        }
    }
    
    // 2. Map indices by base name to detect collisions safely
    QHash<QString, QList<int>> hitMap;
    for (int i = 0; i < rawHits.size(); ++i) {
        QString base = QFileInfo(rawHits[i].assetFileName).baseName();
        hitMap[base].append(i);
    }
    
    // 3. Resolve collisions with numbers (e.g., Aisling (1), Aisling (2))
    for (auto it = hitMap.begin(); it != hitMap.end(); ++it) {
        const QList<int>& indices = it.value();
        if (indices.size() > 1) {
            for (int j = 0; j < indices.size(); ++j) {
                rawHits[indices[j]].displayName = QString("%1 (%2)").arg(it.key()).arg(j + 1);
            }
        } else {
            rawHits[indices.first()].displayName = it.key();
        }
    }
    
    return rawHits;
}

/**
 * @brief Handles double-clicking an item in the asset grid to open it.
 */
void AssetManagerWidget::onGridItemDoubleClicked(QListWidgetItem *item) {
    if (!item) return;

    // Extract the raw path we hid inside the Qt::UserRole
    QString fullPath = item->data(Qt::UserRole).toString();

    if (!fullPath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
    }
}

/**
 * @brief Intercepts events on the asset grid viewport to manage interactive custom tooltips.
 */
bool AssetManagerWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == assetListWidget->viewport()) {
        
        // 1. Intercept the exact moment Qt tries to spawn a native tooltip
        if (event->type() == QEvent::ToolTip) {
            QHelpEvent *helpEvent = static_cast<QHelpEvent *>(event);
            QListWidgetItem *item = assetListWidget->itemAt(helpEvent->pos());

            if (item) {
                QString html = item->data(Qt::UserRole + 1).toString();
                if (!html.isEmpty()) {
                    customToolTip->stopHideTimer();
                    customToolTip->setText(html);
                    
                    // Offset the box slightly right/down so the cursor doesn't instantly block it
                    customToolTip->move(helpEvent->globalPos() + QPoint(15, 15));
                    customToolTip->show();
                    
                    // Only update the "owner" when a new tooltip successfully spawns
                    activeToolTipItem = item; 
                    return true; // Return TRUE to completely block the native OS tooltip!
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

/**
 * @brief Navigates the tree to the physical folder that contains a given asset.
 * @details Walks down the tree expanding lazy-loaded nodes segment by segment.
 *          Searches favorites first, then falls back to the Combined library view.
 */
void AssetManagerWidget::navigateToFolderInTree(const QString& folderPath) {
    const QString normTarget = QDir::cleanPath(QDir::fromNativeSeparators(folderPath));

    // Shared helper: expand startIdx then walk each segment, returning the final index.
    // Returns an invalid index if any segment is not found.
    auto walkSegments = [this](QModelIndex startIdx, const QStringList& segments) -> QModelIndex {
        QModelIndex cur = startIdx;
        for (const QString& seg : segments) {
            if (!dirTreeView->isExpanded(cur))
                dirTreeView->expand(cur); // fires onTreeExpanded → populates lazy children

            bool found = false;
            const int n = proxyModel->rowCount(cur);
            for (int c = 0; c < n; ++c) {
                const QModelIndex child = proxyModel->index(c, 0, cur);
                const QString name = dirModel->data(proxyModel->mapToSource(child),
                                                     Qt::DisplayRole).toString();
                if (name.compare(seg, Qt::CaseInsensitive) == 0) {
                    cur = child;
                    found = true;
                    break;
                }
            }
            if (!found) return QModelIndex();
        }
        return cur;
    };

    auto selectNode = [this](const QModelIndex& idx) {
        dirTreeView->setCurrentIndex(idx);
        dirTreeView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        onFolderSelected(idx);
    };

    // --- Pass 1: search Favorites ---
    for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
        QStandardItem* favItem = favoritesRootItem->child(i);
        const QString itemPath = favItem->data(Qt::UserRole).toString();
        if (itemPath == "BROKEN_PATH" || itemPath.startsWith("COMBINED_DIR_")) continue;

        const QString normFav = QDir::cleanPath(QDir::fromNativeSeparators(itemPath));
        if (normTarget.compare(normFav, Qt::CaseInsensitive) != 0 &&
            !normTarget.startsWith(normFav + '/', Qt::CaseInsensitive)) continue;

        // Ensure the Favorites section is expanded so the child is accessible
        QModelIndex favRootProxy = proxyModel->mapFromSource(dirModel->indexFromItem(favoritesRootItem));
        if (!dirTreeView->isExpanded(favRootProxy))
            dirTreeView->expand(favRootProxy);

        QModelIndex favProxy = proxyModel->mapFromSource(dirModel->indexFromItem(favItem));

        if (normTarget.compare(normFav, Qt::CaseInsensitive) == 0) {
            selectNode(favProxy);
            return;
        }

        const QStringList segments = normTarget.mid(normFav.length() + 1).split('/', Qt::SkipEmptyParts);
        const QModelIndex result = walkSegments(favProxy, segments);
        if (result.isValid()) { selectNode(result); return; }
    }

    // --- Pass 2: search Combined library view ---
    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    while (libQuery.next()) {
        const QString normLib = QDir::cleanPath(QDir::fromNativeSeparators(libQuery.value(0).toString()));
        if (!normTarget.startsWith(normLib + '/', Qt::CaseInsensitive)) continue;

        const QStringList segments = normTarget.mid(normLib.length() + 1).split('/', Qt::SkipEmptyParts);
        if (segments.isEmpty()) continue;

        QModelIndex combinedRootProxy = proxyModel->mapFromSource(dirModel->indexFromItem(combinedRootItem));
        if (!dirTreeView->isExpanded(combinedRootProxy))
            dirTreeView->expand(combinedRootProxy);

        const QModelIndex result = walkSegments(combinedRootProxy, segments);
        if (result.isValid()) { selectNode(result); return; }
    }
}

/**
 * @brief Links an asset file path to a specific Collection ID in the database.
 */
void AssetManagerWidget::addAssetToCollection(const QString& filePath, int collectionId) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("INSERT INTO AssetCollectionItems (AssetCollectionItemPath, AssetCollectionItemCol) VALUES (:path, :id)");
    query.bindValue(":path", filePath);
    query.bindValue(":id", collectionId);
    
    if (!query.exec()) {
        qWarning() << "Failed to add asset to collection:" << query.lastError().text();
    } else {
        proxyModel->invalidateAndRefresh(QString("COLLECTION_%1").arg(collectionId));
    }
}

/**
 * @brief Removes a specific asset file path from a Collection ID in the database.
 */
void AssetManagerWidget::removeAssetFromCollection(const QString& filePath, int collectionId) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("DELETE FROM AssetCollectionItems WHERE AssetCollectionItemPath = :path AND AssetCollectionItemCol = :id");
    query.bindValue(":path", filePath);
    query.bindValue(":id", collectionId);
    
    if (!query.exec()) {
        qWarning() << "Failed to remove asset from collection:" << query.lastError().text();
    } else {
        proxyModel->invalidateAndRefresh(QString("COLLECTION_%1").arg(collectionId));
    }
}

#include "assetmanagerwidget.moc"
