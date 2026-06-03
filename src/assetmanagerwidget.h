/**
 * @file assetmanagerwidget.h
 * @brief Declarations for the AssetManagerWidget and its associated models and delegates.
 * * This file defines the core UI components and data models required to parse, 
 * display, and interact with the PoseStudio 3D asset library and its directory structure.
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
#include <QIcon>
#include <QStyledItemDelegate>
#include <QPainter>

/**
 * @struct AssetHit
 * @brief Represents a successfully discovered 3D asset and its paired thumbnail imagery.
 */
struct AssetHit {
    QString folderPath;          ///< Absolute path to the directory containing the asset
    QString assetFileName;       ///< Filename of the 3D asset (e.g., model.obj, .dsf)
    QStringList matchingImages;  ///< List of filenames for matching thumbnails (e.g., render.png)
};

/**
 * @class AssetFolderProxyModel
 * @brief Intercepts FileSystem data to dynamically style folders based on contents.
 * * Wraps a standard QFileSystemModel to calculate if directories contain valid 3D assets.
 * It modifies the DisplayRole (to show asset counts) and DecorationRole (to show custom icons).
 */
class AssetFolderProxyModel : public QIdentityProxyModel {
    Q_OBJECT
public:
    explicit AssetFolderProxyModel(QFileSystemModel* source, QObject* parent = nullptr);
    QVariant data(const QModelIndex &proxyIndex, int role = Qt::DisplayRole) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;

private:
    QFileSystemModel* fsModel;
    mutable QHash<QString, int> hitCache; ///< Caches asset counts to prevent redundant disk reads

    int getAssetCount(const QString& folderPath) const;
    QList<AssetHit> parseAssetsInternal(const QString& folderPath) const;
    bool hasAssetHit(const QString& folderPath) const;
    
    QIcon folderIcon = QIcon(QStringLiteral(":/resources/icons/folder.png"));
};

/**
 * @class AssetTreeDelegate
 * @brief Custom item delegate to style the directory tree, highlighting asset counts in blue.
 */
class AssetTreeDelegate : public QStyledItemDelegate {
public:
    explicit AssetTreeDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    /**
     * @brief Custom paint routine to handle drawing the folder name and the blue asset count.
     * Highly optimized to ensure smooth 60fps scrolling in the QTreeView.
     */
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();

        // 1. Draw the background highlight across the entire row rect
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
            painter->setPen(option.palette.highlightedText().color());
        } else {
            painter->setPen(option.palette.text().color());
        }

        // 2. Prepare the text
        const QString text = index.data(Qt::DisplayRole).toString();
        
        // Single-pass check to locate the count parentheses (faster than text.contains)
        const int openParen = text.lastIndexOf(QLatin1Char('('));

        if (openParen == -1) {
            // No count found, render standard text
            painter->drawText(option.rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        } else {
            // Split text into Name and Count segments
            const QString name = text.left(openParen);
            const QString count = text.mid(openParen);

            // Draw Folder Name
            painter->drawText(option.rect, Qt::AlignLeft | Qt::AlignVCenter, name);
            
            // Offset the count text using pre-existing fontMetrics to save CPU cycles
            const int nameWidth = option.fontMetrics.horizontalAdvance(name);
            QRect countRect = option.rect;
            countRect.setX(option.rect.x() + nameWidth + 5);

            // Keep the count blue if not selected, white/contrast if selected
            if (!(option.state & QStyle::State_Selected)) {
                // Use raw RGB integers instead of hex strings to bypass parsing overhead
                painter->setPen(QColor(77, 166, 255)); // Equivalent to #4da6ff
            }
            
            // Apply smaller, bold font to the count
            QFont countFont = painter->font();
            countFont.setPixelSize(10);
            countFont.setWeight(QFont::Bold);
            painter->setFont(countFont);
            
            painter->drawText(countRect, Qt::AlignLeft | Qt::AlignVCenter, count);
        }

        painter->restore();
    }
};

/**
 * @class AssetManagerWidget
 * @brief The main side-panel widget managing the directory tree and the asset thumbnail grid.
 */
class AssetManagerWidget : public QWidget {
    Q_OBJECT 

public:
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    ~AssetManagerWidget() override = default;

private slots:
    void onFolderSelected(const QModelIndex &index);

private:
    QVBoxLayout *mainLayout;            
    QLabel *titleLabel;                 
    QListWidget *assetListWidget;       
    
    QFileSystemModel *dirModel;
    AssetFolderProxyModel *proxyModel;
    QTreeView *dirTreeView;

    void setupUI();
    QList<AssetHit> parseFolderAssets(const QString& folderPath);
};

#endif // ASSETMANAGERWIDGET_H