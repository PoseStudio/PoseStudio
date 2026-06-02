/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of the AssetManagerWidget class.
 *
 * This file contains the logic for the side-panel asset library. It utilizes a 
 * custom proxy model to intercept file system data, allowing for ultra-fast 
 * folder coloring and expander-arrow toggling without freezing the UI thread.
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

// =============================================================================
// [ PROXY MODEL IMPLEMENTATION ]
// =============================================================================

AssetFolderProxyModel::AssetFolderProxyModel(QFileSystemModel* source, QObject* parent)
    : QIdentityProxyModel(parent), fsModel(source) {
    setSourceModel(fsModel);
}

/**
 * @brief Intercepts UI requests to dynamically swap folder icons.
 */
QVariant AssetFolderProxyModel::data(const QModelIndex &proxyIndex, int role) const {
    
    // Only intercept the Icon Request (DecorationRole) on the primary column (0)
    if (role == Qt::DecorationRole && proxyIndex.column() == 0) {
        QModelIndex sourceIndex = mapToSource(proxyIndex);
        
        // Only apply logic to folders, not files
        if (fsModel->isDir(sourceIndex)) {
            QString folderPath = fsModel->filePath(sourceIndex);

            // 1. Check cache. Evaluate and store if new.
            if (!hitCache.contains(folderPath)) {
                bool hit = hasAssetHit(folderPath);
                hitCache.insert(folderPath, hit);
                
                // --- DEBUG LOGGING ---
                // This will print to your terminal so you can verify the backend 
                // is actually finding your files successfully!
                if (hit) {
                    qDebug() << "[ASSET HIT] Found pair in:" << folderPath;
                }
            }

            // 2. Safely extract the native OS folder icon directly from the File System provider
            QIcon defaultIcon = fsModel->fileIcon(sourceIndex);

            // 3. If the folder is empty (No Hit), force it to be grayscale!
            if (!hitCache.value(folderPath)) {
                
                // Convert the icon to an image so we can manipulate the raw pixels
                QImage img = defaultIcon.pixmap(16, 16).toImage();
                
                // Iterate through the 16x16 grid (super fast)
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        QColor c = img.pixelColor(x, y);
                        
                        // If the pixel isn't invisible (preserves the transparent background)
                        if (c.alpha() > 0) {
                            // Calculate the mathematical grayscale equivalent of the color
                            int gray = qGray(c.rgb()); 
                            
                            // Re-apply the gray color, and cut the opacity in half for a faded look
                            img.setPixelColor(x, y, QColor(gray, gray, gray, c.alpha() / 2));
                        }
                    }
                }
                
                // Repackage the manipulated image back into an icon for the UI
                return QIcon(QPixmap::fromImage(img));
                
            } else {
                // Return the bright, full-color default icon for hits!
                return defaultIcon;
            }
        }
    }
    
    // For all other roles (text name, font size, etc.), pass it through normally
    return QIdentityProxyModel::data(proxyIndex, role);
}

/**
 * @brief Determines if a folder actually contains subdirectories.
 * Overrides the default lazy-loading to hide expander arrows on empty directories.
 */
bool AssetFolderProxyModel::hasChildren(const QModelIndex &parent) const {
    QModelIndex sourceParent = mapToSource(parent);

    // If the base model explicitly knows there are no children, trust it.
    if (!fsModel->hasChildren(sourceParent)) {
        return false;
    }

    QString dirPath = fsModel->filePath(sourceParent);
    
    if (dirPath.isEmpty()) {
        return true; 
    }

    // Fast Peek: Grab the very first directory it finds and exit.
    QDirIterator it(dirPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    return it.hasNext();
}

/**
 * @brief An ultra-fast, single-pass algorithm to detect file pairs.
 */
bool AssetFolderProxyModel::hasAssetHit(const QString& folderPath) const {
    QDir dir(folderPath);
    // Ignore symlinks and prevent sorting to maximize read speed
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);

    QSet<QString> images;
    QSet<QString> assets;
    QStringList imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    // Iterate through files once. The moment a pair is found, exit immediately!
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

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    setupUI();
}

void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6); 

    // --- 1. TOP PANEL: THE DIRECTORY TREE ---
    dirModel = new QFileSystemModel(this);
    dirModel->setFilter(QDir::NoDotAndDotDot | QDir::AllDirs); 
    
    QString startFolder = PreferencesManager::instance().getValue("AssetDir", "C:/").toString();
    dirModel->setRootPath(startFolder);

    // Instantiate and inject the Proxy Model
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    dirTreeView = new QTreeView(splitter);
    dirTreeView->setObjectName("AssetManagerTree");
    
    // Hook the TreeView to the Proxy, NOT the raw file system
    dirTreeView->setModel(proxyModel);
    
    // Map the raw file system root index through the proxy
    QModelIndex sourceRoot = dirModel->index(startFolder);
    dirTreeView->setRootIndex(proxyModel->mapFromSource(sourceRoot));

    for (int i = 1; i < proxyModel->columnCount(); ++i) {
        dirTreeView->hideColumn(i);
    }
    dirTreeView->setHeaderHidden(true); 
    dirTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);


    // --- 2. BOTTOM PANEL (Asset Library) ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0); 

    // The Title will dynamically update based on the folder clicked
    titleLabel = new QLabel("Select a folder to view assets...", bottomPanel);
    titleLabel->setObjectName("AssetManagerTitle"); 

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
 * @brief Slot triggered dynamically when a user clicks a folder in the QTreeView.
 */
void AssetManagerWidget::onFolderSelected(const QModelIndex &proxyIndex) {
    // 1. Map the clicked Proxy index back to the underlying FileSystem index
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    
    // 2. Extract the absolute hard drive path
    QString folderPath = dirModel->filePath(sourceIndex);

    // 3. Update the UI Title to show the user exactly where they are
    QString folderName = dirModel->fileName(sourceIndex);
    titleLabel->setText(QString("Viewing: %1").arg(folderName));
    
    // 4. Parse the folder
    QList<AssetHit> discoveredAssets = parseFolderAssets(folderPath);

    // 5. Populate the Grid
    assetListWidget->clear(); 

    for (const AssetHit& hit : discoveredAssets) {
        QListWidgetItem *item = new QListWidgetItem(assetListWidget);
        
        QString cleanName = QFileInfo(hit.assetFileName).baseName();
        item->setText(cleanName);

        if (!hit.matchingImages.isEmpty()) {
            QString imagePath = QDir(hit.folderPath).filePath(hit.matchingImages.first());
            QPixmap rawPixmap(imagePath);
            
            QPixmap scaledImage = rawPixmap.scaled(
                QSize(120, 120), 
                Qt::KeepAspectRatio, 
                Qt::SmoothTransformation
            );
            
            QPixmap paddedCanvas(120, 128);
            paddedCanvas.fill(Qt::transparent); 
            
            QPainter painter(&paddedCanvas);
            QLinearGradient gradient(0, 0, 0, 120);
            gradient.setColorAt(0.0, QColor("#2a2d30")); 
            gradient.setColorAt(1.0, QColor("#0d0d0e")); 

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
 * @brief A highly optimized, single-directory parser to match 3D files with thumbnails.
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