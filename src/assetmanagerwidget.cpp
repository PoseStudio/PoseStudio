/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of the AssetManagerWidget class for managing and visualizing 3D assets.
 * * This file contains both the Proxy Model (for filtering and appending asset metadata 
 * to the directory tree) and the primary AssetManagerWidget (for the UI layout and thumbnail grid).
 */

#include "assetmanagerwidget.h"
#include "preferencesmanager.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QSplitter>
#include <QColor>
#include <QSet>
#include <QStyle>

// =============================================================================
// [ PROXY MODEL IMPLEMENTATION ]
// =============================================================================

/**
 * @brief Constructs the proxy model to wrap the standard QFileSystemModel.
 * @param source The underlying QFileSystemModel.
 * @param parent The parent QObject.
 */
AssetFolderProxyModel::AssetFolderProxyModel(QFileSystemModel* source, QObject* parent)
    : QIdentityProxyModel(parent), fsModel(source) {
    setSourceModel(fsModel);
}

/**
 * @brief Calculates the total number of valid asset-thumbnail pairings in a directory.
 * @param folderPath The absolute path to the directory.
 * @return The integer count of valid hits.
 */
int AssetFolderProxyModel::getAssetCount(const QString& folderPath) const {
    QList<AssetHit> hits = parseAssetsInternal(folderPath);
    return hits.size();
}

/**
 * @brief Internal helper that performs the asset parsing and pairing logic.
 * @param folderPath The directory to scan.
 * @return A list of AssetHit structures representing valid pairs.
 * * @note This logic is cached by the proxy to improve performance during tree navigation.
 * // TODO: Deduplicate this logic with AssetManagerWidget::parseFolderAssets in a future PR.
 */
QList<AssetHit> AssetFolderProxyModel::parseAssetsInternal(const QString& folderPath) const {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    QDir dir(folderPath);
    
    // Retrieve files only, ignoring symlinks for performance and safety
    QFileInfoList allFilesInFolder = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);

    // Group files by base name to match 3D assets with their corresponding thumbnails
    QHash<QString, QPair<QStringList, QStringList>> groupedFiles;
    for (const QFileInfo& fileInfo : allFilesInFolder) {
        if (imageExtensions.contains(fileInfo.suffix().toLower()))
            groupedFiles[fileInfo.baseName()].second.append(fileInfo.fileName());
        else
            groupedFiles[fileInfo.baseName()].first.append(fileInfo.fileName());
    }

    // Convert grouped files into a list of valid AssetHit structures
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
 * @brief Intercepts data requests to the underlying file system to inject custom UI elements.
 * @param proxyIndex The model index being queried.
 * @param role The Qt::ItemDataRole being requested.
 * @return The modified QVariant data, or the default data if no modifications are needed.
 */
QVariant AssetFolderProxyModel::data(const QModelIndex &proxyIndex, int role) const {
    
    // 1. DISPLAY ROLE: Append the asset count to the folder name if assets exist
    if (role == Qt::DisplayRole && proxyIndex.column() == 0) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        if (fsModel->isDir(sourceIndex)) {
            QString path = fsModel->filePath(sourceIndex);
            if (!hitCache.contains(path)) hitCache.insert(path, getAssetCount(path));
            int count = hitCache.value(path);
            QString name = fsModel->fileName(sourceIndex);
            
            return (count > 0) ? QString("%1 (%2)").arg(name).arg(count) : name;
        }
    }

    // 2. USER ROLE: Expose the raw integer count for internal UI logic/delegates
    if (role == Qt::UserRole + 1 && proxyIndex.column() == 0) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        QString path = fsModel->filePath(sourceIndex);
        if (!hitCache.contains(path)) hitCache.insert(path, getAssetCount(path));
        
        return hitCache.value(path);
    }

    // 3. DECORATION ROLE: Provide specific folder icons based on asset presence
    if (role == Qt::DecorationRole && proxyIndex.column() == 0) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        if (fsModel->isDir(sourceIndex)) {
            QString path = fsModel->filePath(sourceIndex);
            
            // Ensure cache is populated using the centralized getAssetCount logic
            if (!hitCache.contains(path)) {
                hitCache.insert(path, getAssetCount(path));
            }
            
            // Swap icon dynamically based on the cached integer count
            QString iconPath = (hitCache.value(path) > 0) 
                               ? ":/resources/icons/folder-hit.png" 
                               : ":/resources/icons/folder-empty.png";
            
            return QIcon(iconPath);
        }
    }
    
    // Fallback to default behavior for all other roles and columns
    return QIdentityProxyModel::data(proxyIndex, role);
}

/**
 * @brief Determines if a folder node has children, avoiding deep directory scans for performance.
 */
bool AssetFolderProxyModel::hasChildren(const QModelIndex &parent) const {
    QModelIndex sourceParent = mapToSource(parent);
    if (!fsModel->hasChildren(sourceParent)) return false;
    
    QString dirPath = fsModel->filePath(sourceParent);
    if (dirPath.isEmpty()) return true;
    
    QDirIterator it(dirPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    return it.hasNext();
}

/**
 * @brief Fast, single-pass algorithm to detect if at least one asset/thumbnail pair exists.
 * @note This function exits immediately upon finding the first valid pair.
 */
bool AssetFolderProxyModel::hasAssetHit(const QString& folderPath) const {
    QDir dir(folderPath);
    // Ignore symlinks and prevent OS-level sorting to maximize read speed
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);

    QSet<QString> images;
    QSet<QString> assets;
    QStringList imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    // Iterate through files once. The moment a pair is found, return true.
    for (const QFileInfo& file : files) {
        QString baseName = file.baseName(); 
        
        if (imageExts.contains(file.suffix().toLower())) {
            images.insert(baseName);
            if (assets.contains(baseName)) return true; // Match found!
        } else {
            assets.insert(baseName);
            if (images.contains(baseName)) return true; // Match found!
        }
    }
    return false; // Loop finished, no pairs exist
}

// =============================================================================
// [ WIDGET IMPLEMENTATION ]
// =============================================================================

/**
 * @brief Constructor for the AssetManagerWidget.
 */
AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    setupUI();
}

/**
 * @brief Constructs the UI layout, including the directory tree and the asset thumbnail grid.
 */
void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    // Main layout uses a vertical splitter to allow user resizing between tree and grid
    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6); 

    // --- 1. TOP PANEL: THE DIRECTORY TREE ---
    dirModel = new QFileSystemModel(this);
    dirModel->setFilter(QDir::NoDotAndDotDot | QDir::AllDirs); 
    
    // Fetch user's preferred starting directory from the DB
    QString startFolder = PreferencesManager::instance().getValue("AssetDir", "C:/").toString();
    dirModel->setRootPath(startFolder);

    // Instantiate and inject the custom Proxy Model
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    dirTreeView = new QTreeView(splitter);
    dirTreeView->setObjectName("AssetManagerTree");
    
    // Hook the TreeView to the Proxy, NOT the raw file system
    dirTreeView->setModel(proxyModel);
    dirTreeView->setItemDelegate(new AssetTreeDelegate(this));
    
    // Map the raw file system root index through the proxy to establish the view
    QModelIndex sourceRoot = dirModel->index(startFolder);
    dirTreeView->setRootIndex(proxyModel->mapFromSource(sourceRoot));

    // Hide file details (size, date modified) to maintain a clean folder tree
    for (int i = 1; i < proxyModel->columnCount(); ++i) {
        dirTreeView->hideColumn(i);
    }
    dirTreeView->setHeaderHidden(true); 
    dirTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // --- 2. BOTTOM PANEL (Asset Library Grid) ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0); 

    // The Title will dynamically update based on the folder clicked
    titleLabel = new QLabel("Select a folder to view assets...", bottomPanel);
    titleLabel->setObjectName("AssetManagerTitle"); 
    titleLabel->setTextFormat(Qt::RichText);

    // Setup the icon grid for thumbnails
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

    // =========================================================================
    // [ ASSEMBLE & CONNECT ]
    // =========================================================================
    
    splitter->addWidget(dirTreeView);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    // Whenever a folder is clicked in the tree, fire the onFolderSelected function
    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
}

/**
 * @brief Fired when a user clicks a folder. Parses assets and dynamically populates the Grid View.
 * @param proxyIndex The index of the clicked folder in the tree view.
 */
void AssetManagerWidget::onFolderSelected(const QModelIndex &proxyIndex) {
    // 1. Map the clicked Proxy index back to the underlying FileSystem index
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    
    // 2. Extract the absolute hard drive path and name
    QString folderPath = dirModel->filePath(sourceIndex);
    QString folderName = dirModel->fileName(sourceIndex);
    
    // 3. Fetch the pre-calculated hit count from our Proxy Model
    int hits = proxyModel->data(proxyIndex, Qt::UserRole + 1).toInt();

    // 4. Update the UI Title with hit count badge (Styled via RichText/HTML)
    if (hits > 0) {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span><span style='color: #1d84c7; font-size: 14px; font-weight: bold;'>&nbsp;&nbsp;(%2)</span>")
                            .arg(folderName.toHtmlEscaped())
                            .arg(hits));
    } else {
        titleLabel->setText(QString("<span style='font-size: 16px; font-weight: bold;'>&nbsp;%1</span>").arg(folderName.toHtmlEscaped()));
    }
    
    // 5. Parse the folder for assets
    QList<AssetHit> discoveredAssets = parseFolderAssets(folderPath);

    // 6. Populate the Grid
    assetListWidget->clear(); 

    for (const AssetHit& hit : discoveredAssets) {
        QListWidgetItem *item = new QListWidgetItem(assetListWidget);
        
        QString cleanName = QFileInfo(hit.assetFileName).baseName();
        item->setText(cleanName);

        // Generate and apply customized thumbnail icons with gradients
        if (!hit.matchingImages.isEmpty()) {
            QString imagePath = QDir(hit.folderPath).filePath(hit.matchingImages.first());
            QPixmap rawPixmap(imagePath);
            
            // Smoothly scale the image to fit the container
            QPixmap scaledImage = rawPixmap.scaled(
                QSize(120, 120), 
                Qt::KeepAspectRatio, 
                Qt::SmoothTransformation
            );
            
            // Create a slightly taller padded canvas to accommodate the background
            QPixmap paddedCanvas(120, 128);
            paddedCanvas.fill(Qt::transparent); 
            
            // Draw a subtle linear gradient background behind the thumbnail
            QPainter painter(&paddedCanvas);
            QLinearGradient gradient(0, 0, 0, 120);
            gradient.setColorAt(0.0, QColor("#2a2d30")); 
            gradient.setColorAt(1.0, QColor("#0d0d0e")); 

            painter.fillRect(0, 0, 120, 120, gradient);
            
            // Center the image horizontally on the canvas
            int xOffset = (120 - scaledImage.width()) / 2; 
            painter.drawPixmap(xOffset, 0, scaledImage);
            painter.end(); 

            item->setIcon(QIcon(paddedCanvas));
        }
        
        // Tooltip shows the absolute path to the actual 3D asset file
        item->setToolTip(QDir(hit.folderPath).filePath(hit.assetFileName));
    }
    
    assetListWidget->sortItems(Qt::AscendingOrder);
}

/**
 * @brief A highly optimized, single-directory parser to match 3D files with thumbnails.
 * @param folderPath The directory to scan.
 * @return A list of valid AssetHits.
 * * // TODO: This is currently duplicating proxyModel->parseAssetsInternal. Refactor to unify parsing logic.
 */
QList<AssetHit> AssetManagerWidget::parseFolderAssets(const QString& folderPath) {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    
    QDir dir(folderPath);
    // Only grab files (ignore subdirectories) to keep it lightning fast
    QFileInfoList allFilesInFolder = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);

    struct FileGroup {
        QStringList nonImages;
        QStringList images;
    };
    QHash<QString, FileGroup> groupedFiles;

    // Separate files into images (thumbnails) and non-images (assets)
    for (const QFileInfo& fileInfo : allFilesInFolder) {
        QString baseName = fileInfo.baseName(); 
        QString finalExtension = fileInfo.suffix().toLower(); 

        if (imageExtensions.contains(finalExtension)) {
            groupedFiles[baseName].images.append(fileInfo.fileName());
        } else {
            groupedFiles[baseName].nonImages.append(fileInfo.fileName());
        }
    }

    // Connect assets to their respective thumbnails
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