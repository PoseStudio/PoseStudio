/**
 * @file assetmanagerwidget.h
 * @brief Defines the AssetManagerWidget class and supporting data structures.
 * * This file contains the declarations for the side-panel asset library UI, 
 * including the data structure used to pair 3D files with their respective 
 * image thumbnails during directory scans.
 */

#ifndef ASSETMANAGERWIDGET_H
#define ASSETMANAGERWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QList>
#include <QListWidget>
#include <QCheckBox>

/**
 * @struct AssetHit
 * @brief Represents a successfully discovered 3D asset and its paired imagery.
 */
struct AssetHit {
    QString folderPath;          ///< Absolute path to the directory containing the asset.
    QString assetFileName;       ///< The raw file name of the 3D asset (e.g., 'character.obj').
    QStringList matchingImages;  ///< List of image files in the same directory sharing the base name.
};

/**
 * @class AssetManagerWidget
 * @brief A dedicated side-panel widget for browsing and managing 3D assets.
 * * This class handles the rendering of the asset grid, user interaction, and 
 * the background logic required to recursively scan user directories for 
 * valid 3D models and their associated thumbnail graphics.
 */
class AssetManagerWidget : public QWidget {
    Q_OBJECT 

public:
    /**
     * @brief Constructs the AssetManagerWidget.
     * @param parent The parent widget (typically the main window splitter).
     */
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    
    /**
     * @brief Default destructor.
     */
    ~AssetManagerWidget() = default;

private slots:
    /**
     * @brief Slot triggered when the user initiates a directory scan.
     * Reads preferences for the target directory and populates the UI grid.
     */
    void onRefreshButtonClicked();

private:
    // --- UI Components ---
    QVBoxLayout *mainLayout;           ///< The primary vertical layout container.
    QLabel *titleLabel;                ///< Panel header text.
    QCheckBox *subfolderCheckbox;      ///< Toggles recursive directory traversal.
    QPushButton *refreshButton;        ///< Triggers the scanning engine.
    QListWidget *assetListWidget;      ///< The main grid displaying asset thumbnails.

    // --- Internal Methods ---
    /**
     * @brief Initializes the layout, instantiates child widgets, and applies constraints.
     */
    void setupUI();
    
    /**
     * @brief Core engine function to scan directories and pair files.
     * @param rootDirectory The absolute path to begin the search.
     * @param scanSubfolders If true, performs a deep recursive search of all child directories.
     * @return QList<AssetHit> A populated list of paired assets and thumbnails.
     */
    QList<AssetHit> scanForPairedAssets(const QString& rootDirectory, bool scanSubfolders);
};

#endif // ASSETMANAGERWIDGET_H