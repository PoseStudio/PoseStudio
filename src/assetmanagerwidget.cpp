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
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QSet>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

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
    QHash<QString, QPair<QStringList, QStringList>> groupedFiles;

    for (const QFileInfo& fileInfo : allFilesInFolder) {
        if (imageExtensions.contains(fileInfo.suffix().toLower()))
            groupedFiles[fileInfo.baseName()].second.append(fileInfo.fileName());
        else
            groupedFiles[fileInfo.baseName()].first.append(fileInfo.fileName());
    }

    for (auto it = groupedFiles.begin(); it != groupedFiles.end(); ++it) {
        if (!it.value().second.isEmpty() && !it.value().first.isEmpty()) {
            for (const QString& nonImage : it.value().first) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImage;
                hit.matchingImages = it.value().second;
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
            // Extract the numeric ID from the string (e.g., "COLLECTION_3" -> 3)
            int count = 0;
            int collId = path.mid(11).toInt(); 
            
            QSqlQuery query(QSqlDatabase::database("db_conn"));
            query.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
            query.bindValue(":id", collId);
            
            if (query.exec()) {
                while (query.next()) {
                    // Verify the physical file hasn't been deleted outside the application
                    if (QFileInfo::exists(query.value(0).toString())) {
                        count++;
                    }
                }
            }
            hitCache.insert(path, count);
        } else {
            // Standard hard-drive folders
            hitCache.insert(path, getAssetCount(path)); 
        }
    }
    
    int count = hitCache.value(path);

    // ---------------------------------------------------------
    // 4. Apply UI Roles based on cache results
    // ---------------------------------------------------------
    if (role == Qt::DisplayRole) {
        QString name = sourceModel()->data(sourceIndex, Qt::DisplayRole).toString();
        // Append the blue hit count next to the folder name if assets exist
        return (count > 0) ? QString("%1 (%2)").arg(name).arg(count) : name;
    }

    // Qt::UserRole + 1 stores the raw integer count for delegate access
    if (role == Qt::UserRole + 1) return count; 

    if (role == Qt::DecorationRole) {
        QModelIndex parentIndex = sourceIndex.parent();
        
        // Items directly under the Favorites root get a distinct star icon
        if (parentIndex.isValid() && sourceModel()->data(parentIndex, Qt::UserRole).toString() == "FAVORITES_ROOT") {
            return QIcon(":/resources/icons/favorite-item.png");
        }
        
        // Standard folders dynamically swap between empty/hit icons
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
    
    // Enable F2 and slow double-click renaming
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
    assetListWidget->setIconSize(QSize(105, 105));
    assetListWidget->setGridSize(QSize(135, 150)); 
    assetListWidget->setWordWrap(true);
    assetListWidget->setTextElideMode(Qt::ElideRight);
    assetListWidget->setResizeMode(QListView::Adjust); 
    assetListWidget->setMovement(QListView::Static);

    bottomLayout->addWidget(titleLabel);
    bottomLayout->addWidget(assetListWidget); 

    splitter->addWidget(dirTreeView);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
    connect(dirTreeView, &QTreeView::expanded, this, &AssetManagerWidget::onTreeExpanded);

    // Populate the tree data from the database
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
    favoritesRootItem = new QStandardItem(Constants::TERM_FAV_PLURAL); // THE FIX: Uses UI Constant
    favoritesRootItem->setData("FAVORITES_ROOT", Qt::UserRole); 
    favoritesRootItem->setIcon(QIcon(":/resources/icons/favorites.png")); 
    
    // Prevent the root node from being renamed via UI triggers
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
    collectionsRootItem = new QStandardItem(Constants::TERM_COL_PLURAL); // THE FIX: Uses UI Constant
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
        
        // Explicitly allow custom collections to be renamed
        colItem->setFlags(colItem->flags() | Qt::ItemIsEditable);
        collectionsRootItem->appendRow(colItem);
    }

    // ---------------------------------------------------------
    // 3. Build Visual Separator
    // ---------------------------------------------------------
    QStandardItem *separatorItem = new QStandardItem();
    separatorItem->setData("SEPARATOR", Qt::UserRole);
    separatorItem->setFlags(Qt::NoItemFlags); 
    dirModel->appendRow(separatorItem);

    // ---------------------------------------------------------
    // 4. Load Physical Asset Libraries
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
            
            // Append a dummy node if children exist to enable lazy-loading
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
    if (item == favoritesRootItem) return;

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

            // Standard hard-drive folders should not be renamed via the UI
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
 * @brief Adds a folder to the Favorites database and visually updates the tree.
 */
void AssetManagerWidget::addFavoriteFolder(const QString& folderPath, const QString& displayName, bool saveToDb) {
    // Prevent duplicate entries during current session
    for (int i = 0; i < favoritesRootItem->rowCount(); ++i) {
        if (favoritesRootItem->child(i)->data(Qt::UserRole).toString() == folderPath) {
            return; 
        }
    }

    if (saveToDb) {
        QString nameToSave = displayName.isEmpty() ? QDir(folderPath).dirName() : displayName;
        QSqlQuery query(QSqlDatabase::database("db_conn"));
        
        query.prepare("INSERT OR IGNORE INTO AssetFavorites (AssetFavoritePath, AssetFavoriteName) VALUES (:path, :name)");
        query.bindValue(":path", folderPath);
        query.bindValue(":name", nameToSave);
        if (!query.exec()) {
            qWarning() << "Failed to save favorite to DB:" << query.lastError().text();
        }
    }

    QDir dir(folderPath);
    QString name = displayName.isEmpty() ? dir.dirName() : displayName;
    QStandardItem *favItem = new QStandardItem(name);

    // Favorites require editing privileges for the rename functionality
    favItem->setFlags(favItem->flags() | Qt::ItemIsEditable);

    // Handle missing drives/moved folders gracefully
    if (!dir.exists()) {
        favItem->setData("BROKEN_PATH", Qt::UserRole);
        favItem->setText(name + " (Not Found)");
        favoritesRootItem->appendRow(favItem);
        return;
    }

    favItem->setData(folderPath, Qt::UserRole);

    QDirIterator it(folderPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    if (it.hasNext()) {
        favItem->appendRow(new QStandardItem("..."));
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
    if (folderPath == "FAVORITES_ROOT" || folderPath == "COLLECTIONS_ROOT") {
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
    } else {
        hits = proxyModel->data(proxyIndex, Qt::UserRole + 1).toInt();
        discoveredAssets = parseFolderAssets(folderPath);
    }

    // ---------------------------------------------------------
    // 3. Update Title Labels
    // ---------------------------------------------------------
    if (hits > 0) {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span><span style='color: #1d84c7; font-size: 14px; font-weight: bold;'>&nbsp;&nbsp;(%2)</span>")
                            .arg(folderName.toHtmlEscaped())
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
        
        QString cleanName = QFileInfo(hit.assetFileName).baseName();
        item->setText(cleanName);

        // Apply visual styling to thumbnail containers
        if (!hit.matchingImages.isEmpty()) {
            QString imagePath = QDir(hit.folderPath).filePath(hit.matchingImages.first());
            QPixmap rawPixmap(imagePath);
            
            QPixmap scaledImage = rawPixmap.scaled(QSize(120, 120), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap paddedCanvas(120, 128);
            paddedCanvas.fill(Qt::transparent); 
            
            QPainter painter(&paddedCanvas);
            QLinearGradient gradient(0, 0, 0, 120);
            gradient.setColorAt(0.0, QColor(42, 45, 48)); 
            gradient.setColorAt(1.0, QColor(13, 13, 14)); 

            painter.fillRect(0, 0, 120, 120, gradient);
            int xOffset = (120 - scaledImage.width()) / 2; 
            painter.drawPixmap(xOffset, 0, scaledImage);
            painter.end(); 

            item->setIcon(QIcon(paddedCanvas));
        }
        item->setToolTip(QDir(hit.folderPath).filePath(hit.assetFileName));
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
        QStringList images;
    };
    QHash<QString, FileGroup> groupedFiles;

    for (const QFileInfo& fileInfo : allFilesInFolder) {
        QString baseName = fileInfo.baseName(); 
        QString finalExtension = fileInfo.suffix().toLower(); 
        if (imageExtensions.contains(finalExtension)) {
            groupedFiles[baseName].images.append(fileInfo.fileName());
        } else {
            groupedFiles[baseName].nonImages.append(fileInfo.fileName());
        }
    }

    for (auto it = groupedFiles.begin(); it != groupedFiles.end(); ++it) {
        const FileGroup& group = it.value();
        if (!group.images.isEmpty() && !group.nonImages.isEmpty()) {
            for (const QString& nonImageFile : group.nonImages) {
                AssetHit hit;
                hit.folderPath = folderPath;
                hit.assetFileName = nonImageFile;
                hit.matchingImages = group.images;
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

        // Skip assets that have been moved or deleted from the physical hard drive
        if (!fileInfo.exists()) continue; 

        QString baseName = fileInfo.baseName();
        QString folderPath = fileInfo.absolutePath();

        AssetHit hit;
        hit.folderPath = folderPath;
        hit.assetFileName = fileInfo.fileName();

        // Check the local hard drive for matching thumbnails using a wildcard filter
        QDir dir(folderPath);
        QFileInfoList relatedFiles = dir.entryInfoList(QStringList() << baseName + ".*", QDir::Files);
        
        for (const QFileInfo& related : relatedFiles) {
            if (imageExtensions.contains(related.suffix().toLower())) {
                hit.matchingImages.append(related.fileName());
            }
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
    if (!proxyIndex.isValid()) return;

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
    // DEDICATED MENU: INDIVIDUAL DB COLLECTION ITEMS
    // =========================================================================
    if (folderPath.startsWith("COLLECTION_")) {
        QMenu collMenu(this);
        collMenu.setObjectName("AssetManagerContextMenu");

        // THE FIX: Dynamic UI Constant
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
    
    // Prevent duplicate favoriting by checking if the path is already nested in a Favorite
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

    // --- Action Group 1: Manage & Browse ---
    QAction *favoriteAction = nullptr;
    QAction *renameAction = nullptr;

    if (!isAlreadyFavorite && !isChildOfFavorite) {
        // THE FIX: Dynamic UI Constant
        favoriteAction = contextMenu.addAction(QIcon(":/resources/icons/favorites.png"), 
                                               QStringLiteral("Add to %1").arg(Constants::TERM_FAV_PLURAL));
    }
    
    if (isAlreadyFavorite && isTopLevelFavorite) {
        // THE FIX: Dynamic UI Constant
        renameAction = contextMenu.addAction(QIcon(":/resources/icons/rename.png"), 
                                             QStringLiteral("Rename %1").arg(Constants::TERM_FAV_SINGULAR));
    }

    QAction *browseAction = contextMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse in Explorer");
    contextMenu.addSeparator();

    // --- Action Group 2: Tree Navigation ---
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

    // --- Action Group 3: Danger & Refresh ---
    QAction *removeFavoriteAction = nullptr;
    
    if (isAlreadyFavorite) {
        // THE FIX: Dynamic UI Constant
        removeFavoriteAction = contextMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), 
                                                     QStringLiteral("Remove from %1").arg(Constants::TERM_FAV_PLURAL));
    }

    QAction *refreshAction = contextMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

    // --- Event Execution ---
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
            // Strip the text prefix to isolate the primary key database ID
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