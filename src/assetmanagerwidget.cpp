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
}

int AssetFolderProxyModel::getAssetCount(const QString& folderPath) const {
    return parseAssetsInternal(folderPath).size();
}

/**
 * @brief Internal helper to scan a directory and group 3D assets with their thumbnail images.
 * @param folderPath The absolute path to scan on the local disk.
 * @return A list of AssetHit structures containing the paired data.
 */
QList<AssetHit> AssetFolderProxyModel::parseAssetsInternal(const QString& folderPath) const {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    QDir dir(folderPath);
    
    QFileInfoList allFilesInFolder = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);
    
    struct FileGroup {
        QStringList nonImages;
        QString bestImage;
        qint64 maxBytes = -1;
        QStringList otherImages;
    };
    QHash<QString, FileGroup> groupedFiles;

    for (const QFileInfo& fileInfo : allFilesInFolder) {
        QString baseName = fileInfo.baseName(); 
        if (imageExtensions.contains(fileInfo.suffix().toLower())) {
            qint64 fileSize = fileInfo.size();
            if (fileSize > groupedFiles[baseName].maxBytes) {
                if (!groupedFiles[baseName].bestImage.isEmpty()) {
                    groupedFiles[baseName].otherImages.append(groupedFiles[baseName].bestImage);
                }
                groupedFiles[baseName].bestImage = fileInfo.fileName();
                groupedFiles[baseName].maxBytes = fileSize;
            } else {
                groupedFiles[baseName].otherImages.append(fileInfo.fileName());
            }
        } else {
            groupedFiles[baseName].nonImages.append(fileInfo.fileName());
        }
    }

    for (auto it = groupedFiles.begin(); it != groupedFiles.end(); ++it) {
        if (!it.value().bestImage.isEmpty() && !it.value().nonImages.isEmpty()) {
            for (const QString& nonImage : it.value().nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImage;
                hit.matchingImages.append(it.value().bestImage);
                hit.matchingImages.append(it.value().otherImages);
                finalHits.append(hit);
            }
        }
    }
    return finalHits;
}

/**
 * @brief Intercepts data requests from the view to inject dynamic icons, labels, and hit counts.
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

    // ---------------------------------------------------------
    // 3. Asset Hit Cache & DB Query Logic
    // ---------------------------------------------------------
    if (!hitCache.contains(path)) {
        if (path.startsWith("COLLECTION_")) {
            int count = 0;
            int collId = path.mid(11).toInt(); 
            
            QSqlQuery query(QSqlDatabase::database("db_conn"));
            query.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
            query.bindValue(":id", collId);
            
            if (query.exec()) {
                while (query.next()) {
                    if (QFileInfo::exists(query.value(0).toString())) {
                        count++;
                    }
                }
            }
            hitCache.insert(path, count);
        } 
        else if (path.startsWith("COMBINED_DIR_")) {
            QString relPath = path.mid(13); 
            int count = 0;
            QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
            libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
            
            while (libQuery.next()) {
                QDir dir(libQuery.value(0).toString());
                if (dir.cd(relPath)) {
                    count += getAssetCount(dir.absolutePath());
                }
            }
            hitCache.insert(path, count);
        } 
        else {
            hitCache.insert(path, getAssetCount(path)); 
        }
    }
    
    int count = hitCache.value(path);

    // ---------------------------------------------------------
    // 4. Apply UI Roles based on cache results
    // ---------------------------------------------------------
    if (role == Qt::DisplayRole) {
        QString name = sourceModel()->data(sourceIndex, Qt::DisplayRole).toString();
        return (count > 0) ? QString("%1 (%2)").arg(name).arg(count) : name;
    }

    if (role == Qt::UserRole + 1) return count; 

    if (role == Qt::DecorationRole) {
        QModelIndex parentIndex = sourceIndex.parent();
        
        if (parentIndex.isValid() && sourceModel()->data(parentIndex, Qt::UserRole).toString() == "FAVORITES_ROOT") {
            return QIcon(":/resources/icons/favorite-item.png");
        }
        
        QString iconPath = (count > 0) ? QStringLiteral(":/resources/icons/folder-hit.png") 
                                       : QStringLiteral(":/resources/icons/folder-empty.png");
        return QIcon(iconPath);
    }
    
    return QIdentityProxyModel::data(proxyIndex, role);
}

bool AssetFolderProxyModel::hasAssetHit(const QString& folderPath) const {
    QDir dir(folderPath);
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);

    QSet<QString> images;
    QSet<QString> assets;
    QStringList imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    for (const QFileInfo& file : files) {
        QString baseName = file.baseName(); 
        if (imageExts.contains(file.suffix().toLower())) {
            images.insert(baseName);
            if (assets.contains(baseName)) return true;
        } else {
            assets.insert(baseName);
            if (images.contains(baseName)) return true;
        }
    }
    return false; 
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
 * @brief Clears the directory tree and completely rebuilds it from the SQLite database.
 */
void AssetManagerWidget::refreshAssetManager() {
    dirModel->clear();
    
    if (assetListWidget) assetListWidget->clear();
    if (titleLabel) titleLabel->setText("Select a folder to view assets...");

    // ---------------------------------------------------------
    // 1. Rebuild Favorites Root
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
    // 2. Rebuild Collections Root
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
    // 3. Count Active Libraries
    // ---------------------------------------------------------
    int activeLibraryCount = 0;
    QSqlQuery countQuery(QSqlDatabase::database("db_conn"));
    countQuery.exec("SELECT COUNT(*) FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    if (countQuery.next()) {
        activeLibraryCount = countQuery.value(0).toInt();
    }

    // ---------------------------------------------------------
    // 4. Build Combined View Root (Only if 2+ libraries exist)
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
    // 5. Build Visual Separator
    // ---------------------------------------------------------
    QStandardItem *separatorItem = new QStandardItem();
    separatorItem->setData("SEPARATOR", Qt::UserRole);
    separatorItem->setFlags(Qt::NoItemFlags); 
    dirModel->appendRow(separatorItem);

    // ---------------------------------------------------------
    // 6. Load Physical Asset Libraries
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
}

/**
 * @brief Handles lazy-loading of physical directories to keep memory footprint low.
 */
void AssetManagerWidget::onTreeExpanded(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QStandardItem *item = dirModel->itemFromIndex(sourceIndex);

    if (!item || !item->hasChildren()) return;
    if (item == favoritesRootItem || item == collectionsRootItem) return;

    QString parentData = item->data(Qt::UserRole).toString();

    // =========================================================================
    // THE NEW MERGE LOGIC: Aggregate subdirectories across all libraries
    // =========================================================================
    if (parentData == "COMBINED_ROOT" || parentData.startsWith("COMBINED_DIR_")) {
        QStandardItem *firstChild = item->child(0);
        if (firstChild && firstChild->text() == "...") {
            item->removeRow(0); // Erase dummy node

            QString relPath = (parentData == "COMBINED_ROOT") ? "" : parentData.mid(13);
            QSet<QString> uniqueDirs;
            
            // OPTIMIZATION: Query the database ONCE and cache the paths in memory
            QStringList activeLibraries;
            QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
            libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
            while (libQuery.next()) {
                activeLibraries.append(libQuery.value(0).toString());
            }
            
            // 1. Find all unique folder names across all active libraries
            for (const QString& libPath : activeLibraries) {
                QDir dir(libPath);
                // If we are deep in the tree, ensure this specific library actually has this subfolder
                if (!relPath.isEmpty() && !dir.cd(relPath)) continue; 
                
                QFileInfoList list = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
                for (const QFileInfo &info : list) {
                    uniqueDirs.insert(info.fileName());
                }
            }
            
            // 2. Sort the unified list alphabetically
            QStringList sortedDirs = uniqueDirs.values();
            sortedDirs.sort(Qt::CaseInsensitive);
            
            // 3. Build the virtual tree nodes
            for (const QString &dirName : sortedDirs) {
                QStandardItem *child = new QStandardItem(dirName);
                QString nextRelPath = relPath.isEmpty() ? dirName : relPath + "/" + dirName;
                
                child->setData("COMBINED_DIR_" + nextRelPath, Qt::UserRole);
                child->setFlags(child->flags() & ~Qt::ItemIsEditable);
                
                // Check if this virtual folder actually has subdirectories in ANY library
                bool hasSubDirs = false;
                for (const QString& libPath : activeLibraries) {
                    QDir subDir(libPath);
                    if (subDir.cd(nextRelPath)) {
                        QDirIterator it(subDir.absolutePath(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
                        if (it.hasNext()) {
                            hasSubDirs = true;
                            break; 
                        }
                    }
                }
                
                // Only append the dummy node if there is actually content to lazy-load
                if (hasSubDirs) {
                    child->appendRow(new QStandardItem("..."));
                }
                
                item->appendRow(child);
            }
        }
        return; 
    }

    // =========================================================================
    // STANDARD PHYSICAL FOLDER LOGIC
    // =========================================================================
    QStandardItem *firstChild = item->child(0);
    if (firstChild && firstChild->data(Qt::UserRole).toString().isEmpty() && firstChild->text() == "...") {
        // Erase dummy node
        item->removeRow(0); 

        QString parentPath = item->data(Qt::UserRole).toString();
        QDir dir(parentPath);
        QFileInfoList list = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        
        for (const QFileInfo &info : list) {
            QStandardItem *child = new QStandardItem(info.fileName());
            child->setData(info.absoluteFilePath(), Qt::UserRole);

            child->setFlags(child->flags() & ~Qt::ItemIsEditable);

            QDirIterator it(info.absoluteFilePath(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
            if (it.hasNext()) {
                child->appendRow(new QStandardItem("..."));
            }
            item->appendRow(child);
        }
    }
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
        favItem->appendRow(new QStandardItem("..."));
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
    int hits = 0;

    if (folderPath.startsWith("COLLECTION_")) {
        int collId = folderPath.mid(11).toInt(); 
        discoveredAssets = parseCollectionAssets(collId);
        hits = discoveredAssets.size();
    } else if (folderPath.startsWith("COMBINED_DIR_")) {
        QString relPath = folderPath.mid(13); 
        discoveredAssets = parseCombinedAssets(relPath);
        hits = discoveredAssets.size();
    } else {
        discoveredAssets = parseFolderAssets(folderPath);
        hits = discoveredAssets.size();
    }

    // ---------------------------------------------------------
    // 3. Update Title Labels
    // ---------------------------------------------------------
    if (hits > 0) {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span><span style='color: %2; font-size: 14px; font-weight: bold;'>&nbsp;&nbsp;(%3)</span>")
                            .arg(folderName.toHtmlEscaped())
                            .arg(Constants::COLOR_ACCENT_BLUE)
                            .arg(hits));
    } else {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span>").arg(folderName.toHtmlEscaped()));
    }
    
    // ---------------------------------------------------------
    // 4. Render Asset Grid
    // ---------------------------------------------------------
    assetListWidget->clear(); 

    for (const AssetHit& hit : discoveredAssets) {
        QListWidgetItem *item = new QListWidgetItem(assetListWidget);

        QString cleanName = hit.displayName.isEmpty() ? QFileInfo(hit.assetFileName).baseName() : hit.displayName;
        item->setText(cleanName);

        if (!hit.matchingImages.isEmpty()) {
            QString imagePath = QDir(hit.folderPath).filePath(hit.matchingImages.first());
            QPixmap rawPixmap(imagePath);
            
            QPixmap scaledImage = rawPixmap.scaled(QSize(Constants::THUMB_RENDER_SIZE, Constants::THUMB_RENDER_SIZE), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap paddedCanvas(Constants::THUMB_RENDER_SIZE, Constants::THUMB_CANVAS_HEIGHT);
            paddedCanvas.fill(Qt::transparent); 
            
            QPainter painter(&paddedCanvas);
            QLinearGradient gradient(0, 0, 0, Constants::THUMB_RENDER_SIZE);
            
            gradient.setColorAt(0.0, QColor(Constants::COLOR_THUMB_BG_START)); 
            gradient.setColorAt(1.0, QColor(Constants::COLOR_THUMB_BG_END)); 

            painter.fillRect(0, 0, Constants::THUMB_RENDER_SIZE, Constants::THUMB_RENDER_SIZE, gradient);
            int xOffset = (Constants::THUMB_RENDER_SIZE - scaledImage.width()) / 2;
            painter.drawPixmap(xOffset, 0, scaledImage);
            painter.end(); 

            // =================================================================
            // Prevent Qt from auto-tinting the image on selection
            // =================================================================
            QIcon thumbIcon;
            thumbIcon.addPixmap(paddedCanvas, QIcon::Normal);
            thumbIcon.addPixmap(paddedCanvas, QIcon::Selected); // Force it to stay unmodified!
            
            item->setIcon(thumbIcon);
        }

        // =====================================================================
        // RICH TEXT TOOLTIP GENERATION WITH CONSTANT COLORS
        // =====================================================================
        QString fullPath = QDir(hit.folderPath).filePath(hit.assetFileName);
        QFileInfo info(fullPath);

        // 1. Format the Extension (e.g., ".DUF File")
        QString ext = info.suffix().toUpper();
        QString typeStr = ext.isEmpty() ? "File" : QString(".%1 File").arg(ext);

        // 2. Format the Size dynamically (KB vs MB)
        double sizeInBytes = info.size();
        QString sizeStr;
        if (sizeInBytes > (1024 * 1024)) {
            sizeStr = QString::number(sizeInBytes / (1024.0 * 1024.0), 'f', 2) + " MB";
        } else {
            sizeStr = QString::number(sizeInBytes / 1024.0, 'f', 2) + " KB";
        }

        // 3. Format Date to match "12/22/2025 6:30pm"
        QString modStr = info.lastModified().toString("MM/dd/yyyy h:mm ap");

        // 4. Convert Unix paths to native Windows paths for a cleaner display
        QString nativeFolder = QDir::toNativeSeparators(hit.folderPath);

        // 5. Construct the HTML Tooltip using Global Constants
        QString tooltipHtml = QString(
            "<div style='white-space: pre-wrap;'>"
            "<span style='font-weight: bold; color: %1;'>%2</span><br/>"
            "Size: %3<br/>"
            "Modified: %4<br/>"
            "<br/>"
            "<span style='color: %5; font-size: 11px;'>%6</span>"
            "</div>"
        ).arg(Constants::COLOR_TOOLTIP_ACCENT, 
              typeStr, 
              sizeStr, 
              modStr, 
              Constants::COLOR_TOOLTIP_MUTED, 
              nativeFolder.toHtmlEscaped());

        item->setData(Qt::UserRole + 1, tooltipHtml);
        item->setData(Qt::UserRole, fullPath);
    }
    
    assetListWidget->sortItems(Qt::AscendingOrder);
}

/**
 * @brief Parses physical folder directories for 3D assets and paired thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseFolderAssets(const QString& folderPath) {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    QDir dir(folderPath);
    QFileInfoList allFilesInFolder = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);

    struct FileGroup {
        QStringList nonImages;
        QString bestImage;
        qint64 maxBytes = -1;
        QStringList otherImages;
    };
    QHash<QString, FileGroup> groupedFiles;

    for (const QFileInfo& fileInfo : allFilesInFolder) {
        QString baseName = fileInfo.baseName(); 
        QString finalExtension = fileInfo.suffix().toLower(); 
        if (imageExtensions.contains(finalExtension)) {
            qint64 fileSize = fileInfo.size();
            if (fileSize > groupedFiles[baseName].maxBytes) {
                if (!groupedFiles[baseName].bestImage.isEmpty()) {
                    groupedFiles[baseName].otherImages.append(groupedFiles[baseName].bestImage);
                }
                groupedFiles[baseName].bestImage = fileInfo.fileName();
                groupedFiles[baseName].maxBytes = fileSize;
            } else {
                groupedFiles[baseName].otherImages.append(fileInfo.fileName());
            }
        } else {
            groupedFiles[baseName].nonImages.append(fileInfo.fileName());
        }
    }

    for (auto it = groupedFiles.begin(); it != groupedFiles.end(); ++it) {
        const FileGroup& group = it.value();
        if (!group.bestImage.isEmpty() && !group.nonImages.isEmpty()) {
            for (const QString& nonImageFile : group.nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImageFile;
                hit.matchingImages.append(group.bestImage);
                hit.matchingImages.append(group.otherImages);
                finalHits.append(hit);
            }
        }
    }
    return finalHits;
}

/**
 * @brief Queries the SQLite database for virtual collection items and locates their physical thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseCollectionAssets(int collectionId) {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
    query.bindValue(":id", collectionId);
    
    if (!query.exec()) {
        qWarning() << "Failed to load collection items:" << query.lastError().text();
        return finalHits;
    }

    while (query.next()) {
        QString fullPath = query.value(0).toString();
        QFileInfo fileInfo(fullPath);

        if (!fileInfo.exists()) continue; 

        QString baseName = fileInfo.baseName();
        QString folderPath = fileInfo.absolutePath();

        AssetHit hit;
        hit.folderPath = folderPath;
        hit.assetFileName = fileInfo.fileName();

        QDir dir(folderPath);
        QFileInfoList relatedFiles = dir.entryInfoList(QStringList() << baseName + ".*", QDir::Files);
        
        QString bestImage;
        qint64 maxBytes = -1;
        QStringList otherImages;

        for (const QFileInfo& related : relatedFiles) {
            if (imageExtensions.contains(related.suffix().toLower())) {
                qint64 fileSize = related.size();
                if (fileSize > maxBytes) {
                    if (!bestImage.isEmpty()) otherImages.append(bestImage);
                    bestImage = related.fileName();
                    maxBytes = fileSize;
                } else {
                    otherImages.append(related.fileName());
                }
            }
        }

        if (!bestImage.isEmpty()) {
            hit.matchingImages.append(bestImage);
            hit.matchingImages.append(otherImages);
        }

        finalHits.append(hit);
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
    
    itemMenu.addSeparator();

    // Action 2: Add to Collection (Sub-menu)
    QMenu *collectionMenu = new QMenu("Add To Collection", &itemMenu);
    collectionMenu->setIcon(QIcon(":/resources/icons/collections.png"));
    
    // Explicitly name the menu so your global.qss catches it
    collectionMenu->setObjectName("AssetManagerContextMenu");
    collectionMenu->setAttribute(Qt::WA_TranslucentBackground);
    collectionMenu->setStyleSheet("QMenu { margin: 0px 4px; }"); // Aligned exactly with -4 offset from AppProxyStyle

    // Dynamically query collections from SQLite
    QSqlQuery collQuery(QSqlDatabase::database("db_conn"));
    collQuery.exec("SELECT AssetCollectionID, AssetCollectionName FROM AssetCollections");
    
    bool hasCollections = false;
    while (collQuery.next()) {
        hasCollections = true;
        int colId = collQuery.value(0).toInt();
        QString colName = collQuery.value(1).toString();

        QAction *colAction = collectionMenu->addAction(colName);
        // Use a Lambda to capture the specific Collection ID when clicked
        connect(colAction, &QAction::triggered, [this, colId, fullPath]() {
            addAssetToCollection(fullPath, colId);
        });
    }

    if (!hasCollections) {
        collectionMenu->setEnabled(false);
    }
    
    // Assign the menu to the action
    itemMenu.addMenu(collectionMenu);
    
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
 * @brief Expands a node and forces initialization of all nested lazy-loaded subdirectories.
 */
void AssetManagerWidget::expandNodeRecursively(const QModelIndex &proxyIndex) {
    dirTreeView->expand(proxyIndex);

    int childCount = proxyModel->rowCount(proxyIndex);
    for (int i = 0; i < childCount; ++i) {
        QModelIndex childIndex = proxyModel->index(i, 0, proxyIndex);
        
        if (proxyModel->hasChildren(childIndex)) {
            expandNodeRecursively(childIndex);
        }
    }
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
        qDebug() << "Successfully added" << filePath << "to collection" << collectionId;
        // Optionally force a refresh or visual feedback here
    }
}

#include "assetmanagerwidget.moc"
