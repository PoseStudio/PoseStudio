/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of the AssetManagerWidget class.
 * * This file contains the logic for the side-panel asset library. It handles
 * recursive directory scanning, file pairing (matching 3D assets with their 
 * respective image thumbnails), and the procedural generation of the UI grid.
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
#include <QProgressDialog>
#include <QPainter>

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    setupUI();
}

/**
 * @brief Initializes the UI layout, widgets, and rigid styling constraints.
 */
void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    titleLabel = new QLabel("Asset Library", this);
    titleLabel->setObjectName("AssetManagerTitle"); 
    
    // Subfolder toggle (Defaults to false for faster initial scans)
    subfolderCheckbox = new QCheckBox("Include Subfolders", this);
    subfolderCheckbox->setObjectName("AssetManagerSubfolderCheck");
    subfolderCheckbox->setChecked(false); 
    
    refreshButton = new QPushButton("Scan for Assets", this);
    refreshButton->setObjectName("AssetManagerRefreshBtn");
    connect(refreshButton, &QPushButton::clicked, this, &AssetManagerWidget::onRefreshButtonClicked);

    // Grid View Configuration
    assetListWidget = new QListWidget(this);
    assetListWidget->setObjectName("AssetManagerGrid");
    assetListWidget->setViewMode(QListView::IconMode);
    
    // Icon and Grid sizes are strictly locked. 
    // The grid cage is intentionally larger than the icon to accommodate 
    // procedural canvas padding and CSS margins without breaking the layout.
    assetListWidget->setIconSize(QSize(105, 105));
    assetListWidget->setGridSize(QSize(135, 150)); 
    
    // Text wrapping and truncation
    assetListWidget->setWordWrap(true);
    assetListWidget->setTextElideMode(Qt::ElideRight);
    
    // Disable drag-and-drop movement and enable dynamic wrapping
    assetListWidget->setResizeMode(QListView::Adjust); 
    assetListWidget->setMovement(QListView::Static);

    // Assemble Layout
    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(subfolderCheckbox);
    mainLayout->addWidget(refreshButton);
    mainLayout->addWidget(assetListWidget); 
}

/**
 * @brief Triggered when the Refresh button is clicked. Initiates the scan
 * and populates the QListWidget with generated thumbnails.
 */
void AssetManagerWidget::onRefreshButtonClicked() {
    QString targetFolder = PreferencesManager::instance().getValue("AssetDir", "C:/").toString();
    bool includeSubfolders = subfolderCheckbox->isChecked();
    
    qDebug() << "\n=== ASSET SCAN INITIATED ===";
    qDebug() << "Target Directory:" << targetFolder;
    qDebug() << "Recursive Scan:" << includeSubfolders;
    
    QList<AssetHit> discoveredAssets = scanForPairedAssets(targetFolder, includeSubfolders);

    assetListWidget->clear(); 

    for (const AssetHit& hit : discoveredAssets) {
        QListWidgetItem *item = new QListWidgetItem(assetListWidget);
        
        // Strip file extensions for a cleaner UI presentation
        QString cleanName = QFileInfo(hit.assetFileName).baseName();
        item->setText(cleanName);

        // Process and apply thumbnail if a matching image was found
        if (!hit.matchingImages.isEmpty()) {
            QString imagePath = QDir(hit.folderPath).filePath(hit.matchingImages.first());
            QPixmap rawPixmap(imagePath);
            
            // Apply high-quality downsampling
            QPixmap scaledImage = rawPixmap.scaled(
                QSize(120, 120), 
                Qt::KeepAspectRatio, 
                Qt::SmoothTransformation
            );
            
            // PROCEDURAL CANVAS GENERATION
            // A taller 128px canvas is created to force the text layout down.
            // A linear gradient is baked into the top 120px to act as a background,
            // leaving the bottom 8px fully transparent to prevent CSS layout conflicts.
            QPixmap paddedCanvas(120, 128);
            paddedCanvas.fill(Qt::transparent); 
            
            QPainter painter(&paddedCanvas);
            QLinearGradient gradient(0, 0, 0, 120);
            gradient.setColorAt(0.0, QColor("#2a2d30")); 
            gradient.setColorAt(1.0, QColor("#0d0d0e")); 

            painter.fillRect(0, 0, 120, 120, gradient);

            // Center the image horizontally on the procedural canvas
            int xOffset = (120 - scaledImage.width()) / 2; 
            painter.drawPixmap(xOffset, 0, scaledImage);
            painter.end(); 

            item->setIcon(QIcon(paddedCanvas));
        }

        // Retain original file path in tooltip for user reference
        item->setToolTip(QDir(hit.folderPath).filePath(hit.assetFileName));
    }
    
    // Alphabetize the grid based on the extracted clean names
    assetListWidget->sortItems(Qt::AscendingOrder);
    
    qDebug() << "Scan complete. Populated" << discoveredAssets.size() << "assets.";
    qDebug() << "==========================\n";
}

/**
 * @brief Scans directories to locate 3D assets and match them with companion images.
 * * @param rootDirectory The starting directory path.
 * @param scanSubfolders If true, recursively searches all child directories.
 * @return QList<AssetHit> A structured list of discovered assets and their thumbnails.
 */
QList<AssetHit> AssetManagerWidget::scanForPairedAssets(const QString& rootDirectory, bool scanSubfolders) {
    QList<AssetHit> finalHits;
    QStringList imageExtensions = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};
    
    // Phase 1: Build the directory tree
    QStringList foldersToScan;
    foldersToScan << rootDirectory; 
    
    if (scanSubfolders) {
        qDebug() << "Engine: Subfolder scan triggered. Building directory tree...";
        QDirIterator dirIt(rootDirectory, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirIt.hasNext()) {
            foldersToScan << dirIt.next();
        }
    } else {
        qDebug() << "Engine: Subfolder scan bypassed. Only evaluating root directory.";
    }

    int totalFolders = foldersToScan.size();
    qDebug() << "Engine: Total directories to process:" << totalFolders;

    // Initialize non-blocking UI progress dialog
    QProgressDialog progressDialog("Initializing scanner...", "Cancel Scan", 0, totalFolders, this);
    progressDialog.setWindowTitle("Scanning Assets");
    progressDialog.setWindowModality(Qt::WindowModal); 
    progressDialog.setMinimumDuration(100); 
    progressDialog.setFixedWidth(450); 
    progressDialog.setFixedHeight(150); 
    progressDialog.setValue(0);

    int currentStep = 0;

    // Phase 2: Traverse and parse directories
    for (const QString& currentFolder : foldersToScan) {
        
        // Handle user abort
        if (progressDialog.wasCanceled()) {
            qWarning() << "Scan aborted by user!";
            break; 
        }
        
        // Elide text to prevent horizontal dialog stretching during deep path scans
        QFontMetrics metrics(progressDialog.font());
        QString safeText = metrics.elidedText(currentFolder, Qt::ElideRight, 400);

        progressDialog.setLabelText(QString("Scanning:\n%1").arg(safeText));
        progressDialog.setValue(currentStep);
        
        // Pump the Qt event loop to keep the UI responsive
        QCoreApplication::processEvents(); 

        QDir dir(currentFolder);
        QFileInfoList allFilesInFolder = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);

        // Group files by base name to identify pairs (e.g., 'model.obj' + 'model.png')
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

        // Assemble final hits for valid pairs
        for (auto it = groupedFiles.begin(); it != groupedFiles.end(); ++it) {
            const FileGroup& group = it.value();
            if (!group.images.isEmpty() && !group.nonImages.isEmpty()) {
                for (const QString& nonImageFile : group.nonImages) {
                    AssetHit hit;
                    hit.folderPath = currentFolder;
                    hit.assetFileName = nonImageFile;
                    hit.matchingImages = group.images;
                    finalHits.append(hit);
                }
            }
        }
        
        currentStep++; 
    }
    
    progressDialog.setValue(totalFolders); 
    return finalHits;
}