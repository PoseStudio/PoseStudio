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
class QListWidget;
class QListWidgetItem;
class QStandardItemModel;
class QStandardItem;
class CustomToolTip;

/**
 * @struct AssetHit
 * @brief Represents a successfully discovered 3D asset and its paired thumbnail imagery.
 */
struct AssetHit {
    QString folderPath;          ///< Absolute path to the directory containing the asset
    QString assetFileName;       ///< Filename of the 3D asset (e.g., model.obj, .dsf)
    QStringList matchingImages;  ///< List of filenames for matching thumbnails (e.g., render.png)
    QString displayName;         ///< Custom UI text for handling virtual folder collisions
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

    // =========================================================================
    // [ CUSTOM RENDER HOOKS ]
    // =========================================================================
    
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        // Enforce a strict 12px height for visual separators rather than a full folder height
        if (index.data(Qt::UserRole).toString() == "SEPARATOR") {
            return QSize(option.rect.width(), 12); 
        }
        return QStyledItemDelegate::sizeHint(option, index);
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        // Shift the inline QLineEdit to perfectly overlay our custom-painted text geometry
        int iconSize = option.decorationSize.width() > 0 ? option.decorationSize.width() : 16;
        int textOffset = iconSize + 6;

        QRect editRect = option.rect;
        editRect.setLeft(option.rect.left() + textOffset - 2); // -2px compensates for native line-edit margins
        editor->setGeometry(editRect);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QString path = index.data(Qt::UserRole).toString();

        // --- Custom Separator Paint ---
        if (path == "SEPARATOR") {
            painter->save();
            int y = option.rect.center().y();
            
            QColor sepColor(60, 60, 60); // Default fallback

            // Extract the dynamic qproperty-separatorColor set in the .qss file
            if (const QWidget *widget = option.widget) {
                QVariant qssColor = widget->property("separatorColor");
                if (qssColor.isValid() && qssColor.canConvert<QColor>()) {
                    sepColor = qssColor.value<QColor>(); 
                }
            }

            painter->setPen(sepColor);
            painter->drawLine(option.rect.left() + 5, y, option.rect.right() - 5, y);
            painter->restore();
            return; 
        }

        // --- Custom Node Paint ---
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        // Strip text and icons so the QStyle engine only draws the selection background
        QIcon folderIcon = opt.icon;
        QString folderText = opt.text;

        opt.text = QString();
        opt.icon = QIcon();
        opt.features &= ~QStyleOptionViewItem::HasDisplay;
        opt.features &= ~QStyleOptionViewItem::HasDecoration;

        if (const QWidget *widget = option.widget) {
            QStyle *style = widget->style();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        }

        painter->save();

        int textOffset = 0;
        
        // Draw the custom folder icon
        if (!folderIcon.isNull()) {
            const int iconSize = opt.decorationSize.width() > 0 ? opt.decorationSize.width() : 16;
            QRect iconRect = opt.rect;
            iconRect.setWidth(iconSize);
            
            QIcon::Mode mode = (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
            folderIcon.paint(painter, iconRect, Qt::AlignLeft | Qt::AlignVCenter, mode, QIcon::Off);
            
            textOffset = iconSize + 6;
        }

        QRect textRect = opt.rect;
        textRect.adjust(textOffset, 0, 0, 0);

        if (opt.state & QStyle::State_Selected) {
            painter->setPen(Qt::white);
        } else {
            QVariant fg = index.data(Qt::ForegroundRole);
            painter->setPen(fg.isValid() ? fg.value<QColor>() : opt.palette.text().color());
        }

        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, folderText);

        painter->restore();
    }
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

    void addFavoriteFolder(const QString& folderPath, const QString& displayName = QString(), bool saveToDb = true);
    void removeFavoriteFolder(const QString& folderPath);

    void expandNodeRecursively(const QModelIndex &proxyIndex);
    void collapseNodeRecursively(const QModelIndex &proxyIndex);
    void refreshAssetManager();

private slots:
    void onFolderSelected(const QModelIndex &index);
    void onTreeExpanded(const QModelIndex &index); 

    void onContextMenuRequested(const QPoint &pos);
    void onGridContextMenuRequested(const QPoint &pos); 
    void onGridItemDoubleClicked(QListWidgetItem *item); ///< ADDED: Handles double-clicking an asset

    void onItemChanged(QStandardItem *item);

private:
    QVBoxLayout *mainLayout;            
    QLabel *titleLabel;                 
    QListWidget *assetListWidget;       

    CustomToolTip *customToolTip;       ///< Our floating interactive tooltip
    QListWidgetItem *activeToolTipItem;   ///< Tracks the active item
    
    QStandardItemModel *dirModel;       
    AssetFolderProxyModel *proxyModel;
    
    AssetTreeView *dirTreeView; 
    
    QStandardItem *favoritesRootItem;
    QStandardItem *collectionsRootItem; 
    QStandardItem *combinedRootItem;    

    QList<QPair<int, QString>> m_pendingThumbs;

    void setupUI();
    void processNextThumbnailBatch();

    QList<AssetHit> parseFolderAssets(const QString& folderPath);
    QList<AssetHit> parseCollectionAssets(int collectionId);
    QList<AssetHit> parseCombinedAssets(const QString& relativePath);

    void navigateToFolderInTree(const QString& folderPath);
    void addAssetToCollection(const QString& filePath, int collectionId);
    void removeAssetFromCollection(const QString& filePath, int collectionId);
    void saveExpandedState(const QModelIndex &parentProxyIndex, QSet<QString> &expandedPaths);
    void restoreExpandedState(const QModelIndex &parentProxyIndex, const QSet<QString> &expandedPaths);
    QModelIndex findProxyIndexByPath(const QModelIndex &parentProxyIndex, const QString &targetPath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override; ///< Intercepts mouse events
};

#endif // ASSETMANAGERWIDGET_H