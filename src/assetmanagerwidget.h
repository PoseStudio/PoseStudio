/**
 * @file assetmanagerwidget.h
 * @brief Defines the AssetManagerWidget class and supporting data structures.
 *
 * This file contains the declarations for the side-panel asset library UI,
 * a high-speed file parser, and a proxy model used to dynamically colorize
 * and format the directory tree based on asset discovery.
 */

#ifndef ASSETMANAGERWIDGET_H
#define ASSETMANAGERWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QList>
#include <QListWidget>
#include <QTreeView>
#include <QFileSystemModel>
#include <QIdentityProxyModel> 
#include <QModelIndex>
#include <QHash>
#include <QSet>

/**
 * @struct AssetHit
 * @brief Represents a successfully discovered 3D asset and its paired imagery.
 */
struct AssetHit {
    QString folderPath;          
    QString assetFileName;       
    QStringList matchingImages;  
};

/**
 * @class AssetFolderProxyModel
 * @brief Intercepts FileSystem data to dynamically style folders based on contents.
 * * This proxy sits between the QFileSystemModel and the QTreeView. It checks if a 
 * directory contains an "Asset Hit" (paired 3D model and image). If it does not, 
 * it overrides the text color to gray. It also intercepts the hasChildren() call 
 * to hide expander arrows on empty directories.
 */
class AssetFolderProxyModel : public QIdentityProxyModel {
    Q_OBJECT
public:
    explicit AssetFolderProxyModel(QFileSystemModel* source, QObject* parent = nullptr);
    QVariant data(const QModelIndex &proxyIndex, int role = Qt::DisplayRole) const override;

    // --- NEW: Intercepts the UI asking if it should draw an arrow ---
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;

private:
    QFileSystemModel* fsModel;
    mutable QHash<QString, bool> hitCache; // Mutable so it can be updated inside const data()

    /**
     * @brief Blazing-fast early-exit algorithm to detect if a folder contains a matched pair.
     */
    bool hasAssetHit(const QString& folderPath) const;
};

/**
 * @class AssetManagerWidget
 * @brief The main side-panel widget managing the directory tree and asset grid.
 */
class AssetManagerWidget : public QWidget {
    Q_OBJECT 

public:
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    ~AssetManagerWidget() = default;

private slots:
    // --- Triggered whenever the user clicks a folder in the top panel ---
    void onFolderSelected(const QModelIndex &index);

private:
    QVBoxLayout *mainLayout;           
    QLabel *titleLabel;                
    QListWidget *assetListWidget;      
    
    QFileSystemModel *dirModel;
    AssetFolderProxyModel *proxyModel; // <-- NEW: The middleman layer
    QTreeView *dirTreeView; // <-- ADD THIS LINE BACK IN!

    void setupUI();
    
    // --- UPDATED: Replaces the old recursive scanner with a lightweight parser ---
    QList<AssetHit> parseFolderAssets(const QString& folderPath);

};

#endif // ASSETMANAGERWIDGET_H