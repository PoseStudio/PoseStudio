/**
 * @file assetmanagerwidget.cpp
 * @brief Implementation of the AssetManagerWidget class for managing and visualizing 3D assets.
 * @details This file drives the primary asset library interface for PoseStudio. It acts as a
 * bridge between the user's physical hard drive (parsing directories for .obj, .duf, etc.)
 * and the SQLite database (managing virtual Collections). It utilizes a QIdentityProxyModel
 * to seamlessly overlay database-driven metadata (like hit counts and custom icons) onto
 * standard filesystem trees.
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
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QScrollBar>
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
// [ CUSTOM WIDGETS ]
// =============================================================================

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
        folderHitState(path);

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
    // 1. Root Node Overrides (Collections)
    // ---------------------------------------------------------
    if (path == "SEARCH_ROOT") {
        if (role == Qt::DecorationRole || role == Qt::ForegroundRole) {
            const bool hasResults = sourceModel()->rowCount(mapToSource(proxyIndex)) > 0;
            if (role == Qt::DecorationRole)
                return hasResults ? QIcon(":/resources/icons/search.png")
                                  : QIcon(":/resources/icons/search-d.png");
            return hasResults ? QVariant() : QColor(110, 110, 110);
        }
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == "COLLECTIONS_ROOT") {
        if (role == Qt::DecorationRole) return QIcon(":/resources/icons/collections.png");
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == "FAVORITES_ROOT") {
        if (role == Qt::DecorationRole) return QIcon(":/resources/icons/favorite.png");
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    // ---------------------------------------------------------
    // 2. Ignore structural/dummy nodes
    // ---------------------------------------------------------
    if (path.isEmpty() || path == "BROKEN_PATH" || path == "SEPARATOR") {
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    if (role == Qt::DecorationRole || role == Qt::ForegroundRole) {
        FolderHitState state = NoHit;
        const bool isCollection = path.startsWith("COLLECTION_");
        if (isCollection) {
            if (!hasHitCache.contains(path)) {
                int collId = path.mid(11).toInt();
                bool found = false;
                // A collection counts as a "hit" when it holds at least one existing asset item.
                QSqlQuery q(QSqlDatabase::database("db_conn"));
                q.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
                q.bindValue(":id", collId);
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

        if (isCollection) return QIcon(QStringLiteral(":/resources/icons/sub-collection.png"));

        if (state == DirectHit) return QIcon(QStringLiteral(":/resources/icons/folder-full.png"));
        if (state == IndirectHit) return QIcon(QStringLiteral(":/resources/icons/folder-hit.png"));
        return QIcon(QStringLiteral(":/resources/icons/folder-empty.png"));
    }

    return QIdentityProxyModel::data(proxyIndex, role);
}

namespace {
    const QString kCollectionMimeType = QStringLiteral("application/x-posestudio-collection");
}

/**
 * @brief Only Collection rows are draggable; only Collection rows and the Collections root
 *        accept drops. Every other row (physical folders/roots, Search Results, Favorites,
 *        separators) gets neither, overriding QStandardItem's drag/drop-enabled-by-default flags.
 */
Qt::ItemFlags AssetFolderProxyModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QIdentityProxyModel::flags(index) & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    if (!index.isValid() || index.column() != 0) return f;

    const QString path = sourceModel()->data(mapToSource(index), Qt::UserRole).toString();
    if (path.startsWith(QStringLiteral("COLLECTION_"))) {
        f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    } else if (path == QStringLiteral("COLLECTIONS_ROOT")) {
        f |= Qt::ItemIsDropEnabled;
    }
    return f;
}

QStringList AssetFolderProxyModel::mimeTypes() const {
    return { kCollectionMimeType };
}

QMimeData* AssetFolderProxyModel::mimeData(const QModelIndexList &indexes) const {
    if (indexes.isEmpty()) return nullptr;
    const QString path = sourceModel()->data(mapToSource(indexes.first()), Qt::UserRole).toString();
    if (!path.startsWith(QStringLiteral("COLLECTION_"))) return nullptr;

    QMimeData *mime = new QMimeData();
    mime->setData(kCollectionMimeType, path.toUtf8());
    return mime;
}

bool AssetFolderProxyModel::canDropMimeData(const QMimeData *data, Qt::DropAction action,
                                             int row, int column, const QModelIndex &parent) const {
    Q_UNUSED(row);
    Q_UNUSED(column);
    if (action != Qt::MoveAction || !data->hasFormat(kCollectionMimeType)) return false;

    const QString draggedPath = QString::fromUtf8(data->data(kCollectionMimeType));
    // The tree's true invisible root (parent invalid) is never a valid target — only the
    // Collections root or another Collection are.
    if (!parent.isValid()) return false;
    const QString targetPath = sourceModel()->data(mapToSource(parent), Qt::UserRole).toString();
    if (targetPath != QStringLiteral("COLLECTIONS_ROOT") && !targetPath.startsWith(QStringLiteral("COLLECTION_")))
        return false;

    if (targetPath == draggedPath) return false; // dropping onto itself is a no-op

    // Reject dropping into one of its own descendants — would create a cycle.
    for (QModelIndex walk = parent; walk.isValid(); walk = walk.parent()) {
        if (sourceModel()->data(mapToSource(walk), Qt::UserRole).toString() == draggedPath) return false;
    }
    return true;
}

bool AssetFolderProxyModel::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                          int row, int column, const QModelIndex &parent) {
    if (!canDropMimeData(data, action, row, column, parent)) return false;

    const QString draggedPath = QString::fromUtf8(data->data(kCollectionMimeType));
    const int draggedId = draggedPath.mid(QStringLiteral("COLLECTION_").length()).toInt();

    const QString targetPath = sourceModel()->data(mapToSource(parent), Qt::UserRole).toString();
    const int newParentId = (targetPath == QStringLiteral("COLLECTIONS_ROOT"))
        ? 0 : targetPath.mid(QStringLiteral("COLLECTION_").length()).toInt();

    emit collectionReparentRequested(draggedId, newParentId);

    // Return false so Qt's own InternalMove machinery does NOT also remove the "source" row:
    // we move the tree item ourselves in reparentCollection (wired as a queued connection so it
    // runs after this drop event fully unwinds). Returning true here would make the view delete
    // the row we just relocated, so the collection would vanish until the next refresh/restart.
    return false;
}

// =============================================================================
// [ TREE VIEW: CUSTOM DROP-TARGET HIGHLIGHT ]
// =============================================================================
// Built-in drop indicator is turned off (setupUI). We track only the valid drop-enabled node
// under the cursor and paint one clean highlight over it, so there are no stray between-items
// lines or faint edge rects elsewhere.

void AssetTreeView::dragMoveEvent(QDragMoveEvent *event) {
    QTreeView::dragMoveEvent(event); // keeps Qt's edge auto-scroll working

    // Validate against canDropMimeData (rejects self / own-descendant / non-collection targets),
    // not just the drop-enabled flag, so the highlight and the accepted cursor match the rules.
    const QModelIndex idx = indexAt(event->position().toPoint());
    const bool ok = idx.isValid() && model()
        && model()->canDropMimeData(event->mimeData(), Qt::MoveAction, -1, -1, idx);

    const QModelIndex target = ok ? idx : QModelIndex();
    if (QModelIndex(m_dropTarget) != target) {
        m_dropTarget = target;
        viewport()->update();
    }

    // We resolve the drop target ourselves from the cursor position (Qt collapses targeting to
    // the viewport root while its drop indicator is disabled), so set acceptance explicitly.
    if (ok) event->acceptProposedAction();
    else    event->ignore();
}

void AssetTreeView::dragLeaveEvent(QDragLeaveEvent *event) {
    if (m_dropTarget.isValid()) {
        m_dropTarget = QModelIndex();
        viewport()->update();
    }
    QTreeView::dragLeaveEvent(event);
}

void AssetTreeView::dropEvent(QDropEvent *event) {
    // Resolve the target from the cursor and hand it to the model directly — don't defer to
    // QTreeView::dropEvent, which (with the drop indicator disabled) collapses the target to the
    // viewport root and gets rejected. dropMimeData validates and emits the reparent (queued).
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (idx.isValid() && model())
        model()->dropMimeData(event->mimeData(), Qt::MoveAction, -1, -1, idx);

    m_dropTarget = QModelIndex();
    viewport()->update();
    event->ignore(); // reparent performed ourselves; prevent Qt's InternalMove source-row removal
}

void AssetTreeView::paintEvent(QPaintEvent *event) {
    QTreeView::paintEvent(event);
    if (!m_dropTarget.isValid()) return;

    const QRect r = visualRect(m_dropTarget);
    if (!r.isValid()) return;

    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    QColor accent(0x49, 0x7f, 0xd4); // Constants::COLOR_ACCENT_BLUE
    QColor fill = accent;
    fill.setAlpha(55);
    p.setPen(QPen(accent, 1));
    p.setBrush(fill);
    p.drawRoundedRect(r.adjusted(1, 1, -2, -2), 3, 3);
}

// =============================================================================
// [ TREE DELEGATE IMPLEMENTATION ]
// =============================================================================

QSize AssetTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    // Visual separators get a fixed slim height rather than a full folder row
    if (index.data(Qt::UserRole).toString() == "SEPARATOR") {
        return QSize(option.rect.width(), 12);
    }
    return QStyledItemDelegate::sizeHint(option, index);
}

void AssetTreeDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    Q_UNUSED(index);
    // Shift the inline QLineEdit to perfectly overlay our custom-painted text geometry
    const int iconSize = option.decorationSize.width() > 0 ? option.decorationSize.width() : 16;
    const int textOffset = iconSize + 6;

    QRect editRect = option.rect;
    editRect.setLeft(option.rect.left() + textOffset - 2); // -2px compensates for native line-edit margins
    editor->setGeometry(editRect);
}

void AssetTreeDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const QString path = index.data(Qt::UserRole).toString();

    if (path == "SEPARATOR") {
        painter->save();
        const int y = option.rect.center().y();

        QColor sepColor(60, 60, 60); // Fallback if the .qss property below isn't set
        if (const QWidget *widget = option.widget) {
            const QVariant qssColor = widget->property("separatorColor");
            if (qssColor.isValid() && qssColor.canConvert<QColor>()) {
                sepColor = qssColor.value<QColor>();
            }
        }

        painter->setPen(sepColor);
        painter->drawLine(option.rect.left() + 5, y, option.rect.right() - 5, y);
        painter->restore();
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Strip text and icon so the QStyle engine only draws the selection background
    QIcon folderIcon = opt.icon;
    QString folderText = opt.text;
    opt.text = QString();
    opt.icon = QIcon();
    opt.features &= ~QStyleOptionViewItem::HasDisplay;
    opt.features &= ~QStyleOptionViewItem::HasDecoration;

    if (const QWidget *widget = option.widget) {
        widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    }

    painter->save();

    int textOffset = 0;
    if (!folderIcon.isNull()) {
        const int iconSize = opt.decorationSize.width() > 0 ? opt.decorationSize.width() : 16;
        QRect iconRect = opt.rect;
        iconRect.setWidth(iconSize);

        const QIcon::Mode mode = (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
        folderIcon.paint(painter, iconRect, Qt::AlignLeft | Qt::AlignVCenter, mode, QIcon::Off);

        textOffset = iconSize + 6;
    }

    QRect textRect = opt.rect;
    textRect.adjust(textOffset, 0, 0, 0);

    if (opt.state & QStyle::State_Selected) {
        painter->setPen(Qt::white);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, folderText);
    } else {
        const int lastSep = folderText.lastIndexOf(" / ");
        if (lastSep >= 0) {
            // Search result path: grey the ancestor prefix, bright the matched folder name
            const QString prefix  = folderText.left(lastSep + 3);   // "a / b / "
            const QString lastSeg = folderText.mid(lastSep + 3);    // "matched"
            const int prefixW = painter->fontMetrics().horizontalAdvance(prefix);

            QRect prefixRect = textRect;
            prefixRect.setWidth(prefixW);
            QRect lastRect = textRect;
            lastRect.setLeft(textRect.left() + prefixW);

            painter->setPen(QColor(110, 110, 110));
            painter->drawText(prefixRect, Qt::AlignLeft | Qt::AlignVCenter, prefix);

            const QVariant fg = index.data(Qt::ForegroundRole);
            painter->setPen(fg.isValid() ? fg.value<QColor>() : opt.palette.text().color());
            painter->drawText(lastRect, Qt::AlignLeft | Qt::AlignVCenter, lastSeg);
        } else {
            const QVariant fg = index.data(Qt::ForegroundRole);
            painter->setPen(fg.isValid() ? fg.value<QColor>() : opt.palette.text().color());
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, folderText);
        }
    }

    painter->restore();
}

// =============================================================================
// [ WIDGET IMPLEMENTATION ]
// =============================================================================

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    // App-level theming (Fusion + AppProxyStyle) and global tooltip effects are configured
    // once in main.cpp, before any widget is built — not here.
    setupUI();
}

void AssetGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QIcon   icon = opt.icon;
    QString txt  = opt.text;

    // Strip text/icon so the style engine only draws the background/selection
    opt.text     = {};
    opt.icon     = {};
    opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
    const QWidget *widget = opt.widget;
    // During a grid drag (Favorites/Collections) we show a between-items drop line instead of a
    // hover highlight, so suppress the per-item hover background while the drag is in progress.
    if (widget && widget->property("gridDragging").toBool())
        opt.state &= ~QStyle::State_MouseOver;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    // Hard backstop: never let icon/text bleed past the cell's selection box,
    // regardless of how tall the actual font metrics turn out to be at runtime.
    painter->setClipRect(opt.rect);

    // Icon — centred horizontally, small top margin
    const int iconSz = Constants::GRID_ICON_DISPLAY_SIZE;

    if (!icon.isNull()) {
        QRect ir(opt.rect.left() + (opt.rect.width() - iconSz) / 2,
                 opt.rect.top() + Constants::GRID_ICON_TOP_MARGIN,
                 iconSz, iconSz);
        icon.paint(painter, ir, Qt::AlignCenter,
                   (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal);
    }

    // Text — up to 2 word-wrapped lines; line 2 is elided with "..." if overflow.
    // Anchored right after the icon + gap (matching the gridSize computed in setupUI),
    // clamped so it can never get pushed past the cell bottom and clipped.
    if (!txt.isEmpty()) {
        const QFontMetrics fm(painter->font());
        const int lineH = fm.height();
        const int textBlockH = 2 * lineH + 2;

        int textTop = opt.rect.top() + Constants::GRID_ICON_TOP_MARGIN + iconSz + Constants::GRID_ICON_TEXT_GAP;
        const int maxTextTop = opt.rect.bottom() - Constants::GRID_TEXT_BOTTOM_MARGIN - textBlockH;
        if (textTop > maxTextTop) textTop = maxTextTop;

        QRect tr = opt.rect.adjusted(4, 0, -4, 0);
        tr.setTop(textTop);
        tr.setBottom(opt.rect.bottom() - Constants::GRID_TEXT_BOTTOM_MARGIN);

        // Respect explicit ForegroundRole (e.g. greyed items), else use theme defaults
        const QVariant fgData = index.data(Qt::ForegroundRole);
        QColor fg = (opt.state & QStyle::State_Selected)
            ? Qt::white
            : (fgData.isValid() ? fgData.value<QColor>() : QColor(0xaa, 0xaa, 0xaa));
        painter->setPen(fg);

        const int availW = tr.width();

        if (fm.horizontalAdvance(txt) <= availW) {
            // Whole label fits on one line
            painter->drawText(tr, Qt::AlignHCenter | Qt::AlignTop, txt);
        } else {
            // Build line 1 word-by-word, put remainder on line 2
            const QStringList words = txt.split(' ', Qt::SkipEmptyParts);
            QString line1;
            int wi = 0;
            for (; wi < words.size(); ++wi) {
                const QString candidate = line1.isEmpty() ? words[wi] : line1 + ' ' + words[wi];
                if (fm.horizontalAdvance(candidate) > availW) break;
                line1 = candidate;
            }
            if (line1.isEmpty()) {
                // First word alone is wider than the cell — just elide the whole string
                painter->drawText(tr, Qt::AlignHCenter | Qt::AlignTop,
                                  fm.elidedText(txt, Qt::ElideRight, availW));
            } else {
                const QString rest  = words.mid(wi).join(' ');
                const QString line2 = fm.elidedText(rest, Qt::ElideRight, availW);
                const QRect r1(tr.left(), tr.top(),          tr.width(), lineH);
                const QRect r2(tr.left(), tr.top() + lineH,  tr.width(), lineH);
                painter->drawText(r1, Qt::AlignHCenter | Qt::AlignVCenter, line1);
                if (!line2.isEmpty())
                    painter->drawText(r2, Qt::AlignHCenter | Qt::AlignVCenter, line2);
            }
        }
    }

    painter->restore();
}

QSize AssetGridDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    // This is the value Qt's QListView actually uses to lay out each cell —
    // setGridSize() alone does not control row height when a custom delegate is installed.
    // The "2 *" here must match the text block height paint() computes below, otherwise
    // cells get sized too short and paint()'s overflow clamp kicks in on every 2-line label.
    const QFontMetrics fm(option.font);
    const int textBlockH = 2 * fm.height() + 2;
    const int height = Constants::GRID_ICON_TOP_MARGIN + Constants::GRID_ICON_DISPLAY_SIZE
                      + Constants::GRID_ICON_TEXT_GAP + textBlockH
                      + Constants::GRID_TEXT_BOTTOM_MARGIN;
    return QSize(Constants::GRID_CELL_WIDTH, height);
}

/**
 * @brief Initializes the main layout, QSplitters, and primary views.
 */
void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6); 

    // --- 1. Top Panel: Search bar + Directory & Collections Tree ---
    dirModel = new QStandardItemModel(this);
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    QWidget *topPanel = new QWidget(splitter);
    topPanel->setObjectName("AssetManagerTopPanel");
    QVBoxLayout *topLayout = new QVBoxLayout(topPanel);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    QWidget *searchRow = new QWidget(topPanel);
    searchRow->setObjectName("AssetManagerSearchRow");
    QHBoxLayout *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(6, 5, 6, 5);
    searchLayout->setSpacing(4);

    searchInput = new QLineEdit(searchRow);
    searchInput->setObjectName("AssetManagerSearchInput");
    searchInput->setPlaceholderText("Search assets...");

    clearSearchButton = new QPushButton(searchRow);
    clearSearchButton->setObjectName("AssetManagerClearButton");
    clearSearchButton->setIcon(QIcon(":/resources/icons/clear.png"));
    clearSearchButton->setIconSize(QSize(14, 14));
    clearSearchButton->setFixedSize(26, 26);

    searchButton = new QPushButton(searchRow);
    searchButton->setObjectName("AssetManagerSearchButton");
    searchButton->setIcon(QIcon(":/resources/icons/search.png"));
    searchButton->setIconSize(QSize(14, 14));
    searchButton->setFixedSize(26, 26);

    searchLayout->addWidget(searchInput);
    searchLayout->addWidget(clearSearchButton);
    searchLayout->addWidget(searchButton);

    QFrame *searchSeparator = new QFrame(topPanel);
    searchSeparator->setObjectName("AssetManagerSearchSeparator");
    searchSeparator->setFrameShape(QFrame::HLine);
    searchSeparator->setFrameShadow(QFrame::Plain);

    dirTreeView = new AssetTreeView(topPanel);
    dirTreeView->setObjectName("AssetManagerTree");
    dirTreeView->setModel(proxyModel);
    dirTreeView->setItemDelegate(new AssetTreeDelegate(this));
    dirTreeView->setHeaderHidden(true);

    dirTreeView->setEditTriggers(QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    dirTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    // Collection drag-and-drop reparenting (see AssetFolderProxyModel's flags/mimeData/
    // canDropMimeData/dropMimeData overrides for what's actually draggable/droppable).
    dirTreeView->setDragEnabled(true);
    dirTreeView->setAcceptDrops(true);
    dirTreeView->setDropIndicatorShown(false); // we paint our own drop highlight (AssetTreeView)
    dirTreeView->setDragDropMode(QAbstractItemView::InternalMove);
    dirTreeView->setDefaultDropAction(Qt::MoveAction);

    connect(dirTreeView, &QTreeView::customContextMenuRequested, this, &AssetManagerWidget::onContextMenuRequested);
    connect(dirModel, &QStandardItemModel::itemChanged, this, &AssetManagerWidget::onItemChanged);
    // Queued so the reparent (which mutates the model via takeRow/appendRow) runs after Qt's
    // drop event has fully unwound, rather than re-entering the model mid-drop.
    connect(proxyModel, &AssetFolderProxyModel::collectionReparentRequested,
            this, &AssetManagerWidget::reparentCollection, Qt::QueuedConnection);

    topLayout->addWidget(searchRow);
    topLayout->addWidget(searchSeparator);
    topLayout->addWidget(dirTreeView);

    // =====================================================================
    // EMPTY-STATE HINT: shown over the directory tree only while no asset library is configured
    // =====================================================================
    addLibraryHintLabel = new QLabel(dirTreeView->viewport());
    addLibraryHintLabel->setObjectName("AssetManagerAddLibraryHint");
    addLibraryHintLabel->setTextFormat(Qt::RichText);
    addLibraryHintLabel->setText(QStringLiteral("<a href=\"#\" style=\"color: #ffffff;\">Add Asset Folder</a>"));
    addLibraryHintLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    addLibraryHintLabel->setCursor(Qt::PointingHandCursor);
    addLibraryHintLabel->hide();
    connect(addLibraryHintLabel, &QLabel::linkActivated, this, [this](const QString&) {
        promptAddAssetLibrary();
    });

    // A layout on the viewport centers the hint and keeps it centered across resizes,
    // without interfering with the tree's own item painting (not child widgets).
    QVBoxLayout *hintLayout = new QVBoxLayout(dirTreeView->viewport());
    hintLayout->addWidget(addLibraryHintLabel, 0, Qt::AlignCenter);
    // =====================================================================

    // --- 2. Bottom Panel: Asset Thumbnail Grid ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0); 

    titleLabel = new QLabel("Select a folder to view assets...", bottomPanel);
    titleLabel->setObjectName("AssetManagerTitle");
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(titleLabel, &QLabel::linkActivated, this, [this](const QString& link) {
        deselectTree();
        displayFolder(link);
    });
    connect(titleLabel, &QLabel::linkHovered, this, [this](const QString& link) {
        m_hoveredBreadcrumbLink = link;
        refreshTitleLabel();
    });
    titleLabel->installEventFilter(this);

    assetListWidget = new QListWidget(bottomPanel);
    assetListWidget->setObjectName("AssetManagerGrid");
    assetListWidget->setViewMode(QListView::IconMode);
    assetListWidget->setIconSize(QSize(Constants::GRID_ICON_DISPLAY_SIZE, Constants::GRID_ICON_DISPLAY_SIZE));

    assetListWidget->setResizeMode(QListView::Adjust);
    assetListWidget->setMovement(QListView::Static);
    assetListWidget->setSpacing(6);
    assetListWidget->setItemDelegate(new AssetGridDelegate(assetListWidget));

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

    infoBarLabel = new QLabel(bottomPanel);
    infoBarLabel->setObjectName("AssetManagerInfoBar");
    infoBarLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    infoBarLabel->hide(); // nothing displayed yet at startup

    bottomLayout->addWidget(titleLabel);
    bottomLayout->addWidget(assetListWidget);
    bottomLayout->addWidget(infoBarLabel);

    splitter->addWidget(topPanel);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
    connect(dirTreeView, &QTreeView::expanded, this, &AssetManagerWidget::onTreeExpanded);

    connect(searchButton, &QPushButton::clicked, this, [this]() {
        runSearch(searchInput->text().trimmed());
    });
    connect(searchInput, &QLineEdit::returnPressed, searchButton, &QPushButton::click);
    connect(clearSearchButton, &QPushButton::clicked, this, [this]() {
        searchInput->clear();
        runSearch(QString());
    });

    // Edge auto-scroll while drag-reordering Favorites: a repeating timer scrolls the grid (and
    // re-places the drop line) whenever the cursor sits past the top/bottom edge during a drag.
    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setInterval(20);
    connect(m_scrollTimer, &QTimer::timeout, this, [this]() {
        if (m_scrollDir == 0) return;
        QScrollBar *sb = assetListWidget->verticalScrollBar();
        sb->setValue(sb->value() + m_scrollDir * 14);
        updateGridDropIndicator(m_dragLastPos);
    });

    refreshAssetManager();
}

/**
 * @brief Prompts the user for a folder via the native file dialog and registers it as a
 *        new Asset Library, then refreshes the tree so it appears immediately.
 */
void AssetManagerWidget::promptAddAssetLibrary() {
    const QString folderPath = QFileDialog::getExistingDirectory(
        this, "Select Asset Library Folder", QDir::homePath());
    if (folderPath.isEmpty()) return;

    QSqlQuery q(QSqlDatabase::database("db_conn"));
    q.prepare("INSERT OR IGNORE INTO AssetLibraries (AssetLibraryPath) VALUES (:path)");
    q.bindValue(":path", folderPath);
    if (!q.exec()) {
        qWarning() << "[!] Failed to add asset library:" << q.lastError().text();
        return;
    }

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

    // Capture the currently selected item's path so it can be re-selected after the rebuild
    QString selectedPath;
    QModelIndex currentIndex = dirTreeView->currentIndex();
    if (currentIndex.isValid()) {
        selectedPath = proxyModel->data(currentIndex, Qt::UserRole).toString();
    }

    dirModel->clear();

    if (assetListWidget) assetListWidget->clear();
    if (titleLabel) titleLabel->setText("Select a folder to view assets...");
    m_currentFolderPath.clear();
    if (infoBarLabel) infoBarLabel->hide();

    // ---------------------------------------------------------
    // 2. Build Search Results Root (always first in tree)
    // ---------------------------------------------------------
    searchResultsRootItem = new QStandardItem("Search Results");
    searchResultsRootItem->setData("SEARCH_ROOT", Qt::UserRole);
    searchResultsRootItem->setFlags(searchResultsRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(searchResultsRootItem);

    // Separator between Search Results and Favorites — both stay hidden together until a
    // search is actually active (see updateSearchVisibility), so the panel doesn't waste
    // space on search UI when it isn't in use.
    searchSeparatorItem = new QStandardItem();
    searchSeparatorItem->setData("SEPARATOR", Qt::UserRole);
    searchSeparatorItem->setFlags(Qt::NoItemFlags);
    dirModel->appendRow(searchSeparatorItem);

    // ---------------------------------------------------------
    // 3. Build Favorites Root (a flat container of favorited asset items; no children in the
    //    tree — clicking it lists those items in the grid)
    // ---------------------------------------------------------
    favoritesRootItem = new QStandardItem("Favorites");
    favoritesRootItem->setData("FAVORITES_ROOT", Qt::UserRole);
    favoritesRootItem->setFlags(favoritesRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(favoritesRootItem);

    // ---------------------------------------------------------
    // 4. Rebuild Collections Root
    // ---------------------------------------------------------
    collectionsRootItem = new QStandardItem(Constants::TERM_COL_PLURAL); 
    collectionsRootItem->setData("COLLECTIONS_ROOT", Qt::UserRole); 
    collectionsRootItem->setFlags(collectionsRootItem->flags() & ~Qt::ItemIsEditable);
    dirModel->appendRow(collectionsRootItem);

    loadCollectionsInto(collectionsRootItem, 0);

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
    libQuery.exec("SELECT AssetLibraryPath, AssetLibraryIsBuiltIn FROM AssetLibraries WHERE AssetLibraryEnabled = 1");

    // Built-in libraries (currently just "Maquettes") always sort first; everything else is
    // alphabetized by folder NAME — not full path, which could misorder libraries that live
    // under different parent folders/drives despite their own names being in order.
    QStringList builtInPaths, regularPaths;
    while (libQuery.next()) {
        const QString path = libQuery.value(0).toString();
        if (libQuery.value(1).toBool()) builtInPaths << path;
        else regularPaths << path;
    }
    std::sort(regularPaths.begin(), regularPaths.end(), [](const QString& a, const QString& b) {
        return QDir(a).dirName().compare(QDir(b).dirName(), Qt::CaseInsensitive) < 0;
    });

    bool anyLibraryAdded = false;
    for (const QString& path : builtInPaths + regularPaths) {
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
            anyLibraryAdded = true;
        } else {
            qWarning() << "Library path does not exist on disk:" << path;
        }
    }
    addLibraryHintLabel->setVisible(!anyLibraryAdded);

    // Search Results (and its separator) stay hidden until a search is actually run —
    // a freshly rebuilt tree has no active search, regardless of stale text left in the box.
    updateSearchVisibility(false);

    // =========================================================================
    // 7. RESTORE STATE AFTER WIPE
    // =========================================================================
    if (!expandedPaths.isEmpty()) {
        restoreExpandedState(QModelIndex(), expandedPaths);
    }

    // Re-select whatever was selected before the rebuild, if it still exists
    if (!selectedPath.isEmpty()) {
        QModelIndex newSelectedIndex = findProxyIndexByPath(QModelIndex(), selectedPath);
        
        if (newSelectedIndex.isValid()) {
            dirTreeView->setCurrentIndex(newSelectedIndex); // Highlights it in the tree
            dirTreeView->scrollTo(newSelectedIndex, QAbstractItemView::PositionAtCenter);
            onFolderSelected(newSelectedIndex);             // Repopulates the lower asset grid
        }
    }
}

// True if any ancestor directory of `path` is present in `set`. Used both to skip processing a
// folder's subtree once an ancestor is already a result, and to drop any result that slipped
// through nested under another — in either case it's reachable by expanding that ancestor.
static bool hasAncestorIn(const QString& path, const QSet<QString>& set) {
    QString ancestor = path;
    int slash;
    while ((slash = ancestor.lastIndexOf('/')) > 0) {
        ancestor.truncate(slash);
        if (set.contains(ancestor)) return true;
    }
    return false;
}

/**
 * @brief Shows or hides the Search Results root and its separator together, so the panel
 *        doesn't reserve space for search UI while no search is active.
 */
void AssetManagerWidget::updateSearchVisibility(bool active) {
    const auto setHidden = [this](QStandardItem* item, bool hidden) {
        if (!item) return;
        const QModelIndex proxyIdx = proxyModel->mapFromSource(dirModel->indexFromItem(item));
        dirTreeView->setRowHidden(proxyIdx.row(), proxyIdx.parent(), hidden);
    };
    setHidden(searchResultsRootItem, !active);
    setHidden(searchSeparatorItem, !active);
}

/**
 * @brief Searches every enabled asset library for files/folders matching the query and lists
 *        the results as a flat set of paths under the Search Results root node. Folders that
 *        only have matches in a subfolder (no direct hit) are skipped in favor of that subfolder,
 *        and any result that's itself nested under another result is dropped as redundant.
 */
void AssetManagerWidget::runSearch(const QString& query) {
    searchResultsRootItem->removeRows(0, searchResultsRootItem->rowCount());
    updateSearchVisibility(!query.isEmpty());

    const QModelIndex searchProxyIdx = proxyModel->mapFromSource(
        dirModel->indexFromItem(searchResultsRootItem));

    if (query.isEmpty()) {
        if (searchProxyIdx.isValid()) dirTreeView->collapse(searchProxyIdx);
        return;
    }

    // Show "Searching..." immediately and let Qt repaint before the blocking scan
    QStandardItem *loadingItem = new QStandardItem("Searching...");
    loadingItem->setFlags(Qt::NoItemFlags);
    searchResultsRootItem->appendRow(loadingItem);
    if (searchProxyIdx.isValid()) {
        dirTreeView->expand(searchProxyIdx);
        dirTreeView->scrollTo(searchProxyIdx, QAbstractItemView::PositionAtTop);
    }
    searchButton->setEnabled(false);
    clearSearchButton->setEnabled(false);
    searchInput->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // Collect results into a flat list before touching the model
    QList<QStandardItem*> resultNodes;
    QSet<QString> addedFolders;   // direct-hit leaf folders already emitted as results (dedup)
    QSet<QString> seenFolders;    // match folders already expanded — avoids re-walking on sibling matches

    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");

    while (libQuery.next()) {
        const QString libPath = libQuery.value(0).toString();
        const QDir libDir(libPath);
        if (!libDir.exists()) continue;

        QDirIterator it(libPath,
                        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            if (!fi.fileName().contains(query, Qt::CaseInsensitive)) continue;

            const QString folderMatch = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

            // A folder with many matching files (or a matching folder name) only needs to be
            // expanded once; skip it on every subsequent hit. Indirect-hit folders never land
            // in addedFolders (only their direct-hit leaves do), so this is the guard that
            // actually prevents the redundant re-walk.
            if (seenFolders.contains(folderMatch)) continue;
            seenFolders.insert(folderMatch);

            // If an ancestor of this folder was already emitted as a direct-hit result, this folder
            // and everything under it is already reachable by expanding that ancestor in the tree —
            // skip processing its subtree entirely instead of collecting hits only to discard them
            // in the dedup pass below. (That dedup remains the order-independent safety net.)
            if (hasAncestorIn(folderMatch, addedFolders)) continue;

            // collectDirectHits classifies folderMatch (direct/indirect/no hit) and recurses
            // only where needed, so a separate hasHit() pre-check would just walk the subtree twice.
            collectDirectHits(folderMatch, libDir, addedFolders, resultNodes);
        }
    }

    // Drop any result that is itself a subfolder of another result already in the list —
    // it's already reachable by expanding that ancestor in the tree. Walk each result's
    // ancestor chain and test membership in a set: O(results * depth) with O(1) lookups,
    // versus the previous O(results^2) pairwise prefix compare.
    {
        QSet<QString> pathSet;
        pathSet.reserve(resultNodes.size());
        for (QStandardItem* item : resultNodes)
            pathSet.insert(item->data(Qt::UserRole).toString());

        for (int i = resultNodes.size() - 1; i >= 0; --i) {
            if (hasAncestorIn(resultNodes[i]->data(Qt::UserRole).toString(), pathSet))
                delete resultNodes.takeAt(i);
        }
    }

    // Replace "Searching..." with real results
    searchResultsRootItem->removeRows(0, searchResultsRootItem->rowCount());

    if (resultNodes.isEmpty()) {
        QStandardItem *noResult = new QStandardItem("(No results)");
        noResult->setFlags(Qt::NoItemFlags);
        searchResultsRootItem->appendRow(noResult);
    } else {
        searchResultsRootItem->appendRows(resultNodes);
    }

    QApplication::restoreOverrideCursor();
    searchButton->setEnabled(true);
    clearSearchButton->setEnabled(true);
    searchInput->setEnabled(true);

    proxyModel->invalidateAndRefresh("SEARCH_ROOT");

    if (searchProxyIdx.isValid()) {
        dirTreeView->expand(searchProxyIdx);
        dirTreeView->scrollTo(searchProxyIdx, QAbstractItemView::PositionAtTop);
    }
}

/**
 * @brief Handles lazy-loading of physical directories to keep memory footprint low.
 */
void AssetManagerWidget::onTreeExpanded(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QStandardItem *item = dirModel->itemFromIndex(sourceIndex);

    if (!item || !item->hasChildren()) return;
    if (item == collectionsRootItem || item == searchResultsRootItem) return;

    const QString parentData = item->data(Qt::UserRole).toString();

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
 * @brief Triggered when a user clicks a node in the directory tree. Processes the click and populates the grid.
 */
void AssetManagerWidget::onFolderSelected(const QModelIndex &proxyIndex) {
    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QString folderPath = dirModel->data(sourceIndex, Qt::UserRole).toString();
    QString folderName = dirModel->data(sourceIndex, Qt::DisplayRole).toString();

    // ---------------------------------------------------------
    // 1. Handle Structural Node Clicks
    // ---------------------------------------------------------
    if (folderPath == "COLLECTIONS_ROOT" || folderPath == "SEARCH_ROOT") {
        bool isExpanded = dirTreeView->isExpanded(proxyIndex);
        dirTreeView->setExpanded(proxyIndex, !isExpanded);
        return;
    }

    if (folderPath.isEmpty() || folderPath == "BROKEN_PATH" || folderPath == "SEPARATOR") return;

    // Normalize display name: search result items encode the full relative path with " / " separators;
    // strip everything before the last separator so we get just the folder name.
    if (folderName.contains(" / "))
        folderName = folderName.section(" / ", -1);

    displayFolder(folderPath, folderName);
}

/**
 * @brief Resolves which library a physical folder belongs to and caches its breadcrumb
 *        segments (names + each segment's full clickable path) for use by buildBreadcrumbHtml.
 *        For virtual/collection paths, clears the breadcrumb cache instead.
 */
void AssetManagerWidget::resolveBreadcrumb(const QString& folderPath) {
    m_breadcrumbLibRoot.clear();
    m_breadcrumbLibName.clear();
    m_breadcrumbSegments.clear();
    m_breadcrumbPaths.clear();

    if (folderPath.startsWith("COLLECTION_")) return;

    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    while (libQuery.next()) {
        const QString lib = libQuery.value(0).toString();
        if (folderPath == lib || folderPath.startsWith(lib + "/") || folderPath.startsWith(lib + "\\")) {
            m_breadcrumbLibRoot = lib;
            break;
        }
    }

    if (m_breadcrumbLibRoot.isEmpty()) {
        m_breadcrumbLibName = QDir(folderPath).dirName();
        return;
    }

    m_breadcrumbLibName = QDir(m_breadcrumbLibRoot).dirName();
    const QString relPath = QDir(m_breadcrumbLibRoot).relativeFilePath(folderPath);
    m_breadcrumbSegments = (relPath == "." || relPath.isEmpty()) ? QStringList() : relPath.split('/');

    QString cumPath = m_breadcrumbLibRoot;
    m_breadcrumbPaths.reserve(m_breadcrumbSegments.size());
    for (const QString& seg : m_breadcrumbSegments) {
        cumPath = QDir::cleanPath(cumPath + "/" + seg);
        m_breadcrumbPaths.append(cumPath);
    }
}

/**
 * @brief Builds the breadcrumb's HTML, fitting as many TRAILING segments as actually fit
 *        within availableWidth and rendering them as real clickable links. Whatever doesn't
 *        fit (the leading library name and/or earliest ancestors) collapses into a
 *        non-clickable "...". The current (last) segment is always shown, bright, non-link.
 */
QString AssetManagerWidget::buildBreadcrumbHtml(int availableWidth) const {
    QFont linkFont = titleLabel->font();
    linkFont.setBold(true);
    linkFont.setPixelSize(14);
    const QFontMetrics linkFm(linkFont);

    QFont curFont = titleLabel->font();
    curFont.setBold(true);
    curFont.setPixelSize(16);
    const QFontMetrics curFm(curFont);

    auto linkStyle = [&](const QString& path) -> QString {
        const bool hovered = (!m_hoveredBreadcrumbLink.isEmpty() && path == m_hoveredBreadcrumbLink);
        return hovered
            ? "font-size:14px; font-weight:bold; color:#aaaaaa; text-decoration:underline;"
            : "font-size:14px; font-weight:bold; color:#6e6e6e; text-decoration:none;";
    };
    const QString greySep = "<span style='color:#6e6e6e; font-weight:bold;'> / </span>";
    const int sepWidth = linkFm.horizontalAdvance(QStringLiteral(" / "));

    // No segments — the library root itself is the current folder.
    if (m_breadcrumbSegments.isEmpty()) {
        const QString elidedName = curFm.elidedText(m_breadcrumbLibName, Qt::ElideLeft, availableWidth);
        return QString("&nbsp;<span style='font-size:16px; font-weight:bold; color:#cccccc;'>%1</span>")
                    .arg(elidedName.toHtmlEscaped());
    }

    // The last segment (current folder) is always shown in full, bright, non-link.
    const QString& lastSeg = m_breadcrumbSegments.last();
    const QString elidedLast = curFm.elidedText(lastSeg, Qt::ElideRight, qMax(availableWidth, 0));
    int usedWidth = curFm.horizontalAdvance(elidedLast);

    // Walk backward through the remaining segments, keeping as many as fit.
    int startIdx = m_breadcrumbSegments.size() - 1;
    QList<int> shownIndices;
    for (int i = m_breadcrumbSegments.size() - 2; i >= 0; --i) {
        const int segW = linkFm.horizontalAdvance(m_breadcrumbSegments[i]) + sepWidth;
        if (usedWidth + segW > availableWidth) break;
        usedWidth += segW;
        shownIndices.prepend(i);
        startIdx = i;
    }

    // If every segment fit, see if the library name itself also fits.
    bool showLibName = false;
    if (startIdx == 0) {
        const int libW = linkFm.horizontalAdvance(m_breadcrumbLibName) + sepWidth;
        if (usedWidth + libW <= availableWidth) showLibName = true;
    }

    QString html = "&nbsp;";
    if (showLibName) {
        html += QString("<a href='%1' style='%2'>%3</a>")
                    .arg(m_breadcrumbLibRoot.toHtmlEscaped(), linkStyle(m_breadcrumbLibRoot),
                         m_breadcrumbLibName.toHtmlEscaped());
    } else {
        html += "<span style='font-size:14px; font-weight:bold; color:#6e6e6e;'>...</span>";
    }

    for (int idx : shownIndices) {
        html += greySep;
        html += QString("<a href='%1' style='%2'>%3</a>")
                    .arg(m_breadcrumbPaths[idx].toHtmlEscaped(), linkStyle(m_breadcrumbPaths[idx]),
                         m_breadcrumbSegments[idx].toHtmlEscaped());
    }

    html += greySep;
    html += QString("<span style='font-size:16px; font-weight:bold; color:#cccccc;'>%1</span>")
                .arg(elidedLast.toHtmlEscaped());

    return html;
}

/**
 * @brief Rebuilds the title label from the cached breadcrumb/title state.
 *        Called whenever the folder selection changes or the label is resized (e.g. splitter drag).
 */
void AssetManagerWidget::refreshTitleLabel() {
    if (m_currentFolderPath.isEmpty()) return;
    // Virtual sources (Collections, Favorites) show a plain title; physical folders show a
    // clickable breadcrumb. Must match the isVirtual logic in displayFolder().
    const bool isVirtual = m_currentFolderPath.startsWith("COLLECTION_")
                        || m_currentFolderPath == "FAVORITES_ROOT";
    if (isVirtual && m_currentTitleText.isEmpty()) return;

    // Reserve room for the leading "&nbsp;" and the QSS padding on #AssetManagerTitle.
    constexpr int kMargin = 30;
    const int availableWidth = qMax(0, titleLabel->width() - kMargin);

    QString html;
    if (isVirtual) {
        QFont pathFont = titleLabel->font();
        pathFont.setBold(true);
        pathFont.setPixelSize(16);
        const QFontMetrics pathFm(pathFont);
        const QString elided = pathFm.elidedText(m_currentTitleText, Qt::ElideLeft, availableWidth);
        html = QString("&nbsp;<span style='font-size:16px; font-weight:bold;'>%1</span>")
                    .arg(elided.toHtmlEscaped());
    } else {
        html = buildBreadcrumbHtml(availableWidth);
    }

    titleLabel->setText(html);
}

/**
 * @brief Rebuilds the footer info bar ("Assets: X   Folders: X   Sortable"), omitting any
 *        segment that isn't relevant to the currently displayed source.
 */
void AssetManagerWidget::refreshInfoBar() {
    if (m_currentFolderPath.isEmpty()) {
        infoBarLabel->hide();
        return;
    }

    QStringList countSegments;
    if (m_currentAssetCount > 0)
        countSegments << QStringLiteral("Assets: %1").arg(m_currentAssetCount);
    if (m_currentFolderCount > 0)
        countSegments << QStringLiteral("Folders: %1").arg(m_currentFolderCount);

    const bool isSortable = isSortableView();

    if (countSegments.isEmpty() && !isSortable) {
        infoBarLabel->hide();
        return;
    }

    // An extra space sets "Sortable" apart from the Assets/Folders counts a bit more.
    QString text = countSegments.join(QStringLiteral("   "));
    if (isSortable) {
        if (!text.isEmpty()) text += QStringLiteral("    ");
        text += QStringLiteral("Sortable");
    }

    infoBarLabel->setText(text);
    infoBarLabel->show();
}

/**
 * @brief Clears the tree's selection and current-index so no stale folder appears
 *        highlighted after navigating away from it by some means other than a tree click
 *        (breadcrumb link, grid folder double-click/Open, etc.).
 */
void AssetManagerWidget::deselectTree() {
    dirTreeView->clearSelection();
    dirTreeView->setCurrentIndex(QModelIndex());
}

/**
 * @brief Parses and displays assets for a given folder path. Called by onFolderSelected
 *        and by breadcrumb link clicks in the title label.
 */
void AssetManagerWidget::displayFolder(const QString& folderPath, const QString& title) {
    // "Virtual" sources (Collections, Favorites) hold a DB-backed list of asset items rather than
    // a physical directory: they get a plain title (not a breadcrumb) and no subfolder listing.
    const bool isFavorites = (folderPath == "FAVORITES_ROOT");
    const bool isCollection = folderPath.startsWith("COLLECTION_");
    const bool isVirtual = isFavorites || isCollection;

    QList<AssetHit> discoveredAssets;
    if (isFavorites) {
        discoveredAssets = parseFavorites();
    } else if (isCollection) {
        discoveredAssets = parseCollectionAssets(folderPath.mid(11).toInt());
    } else {
        discoveredAssets = parseFolderAssets(folderPath);
    }

    const int assetCount = discoveredAssets.size();

    // Cache for resize-triggered relayout
    m_currentFolderPath = folderPath;
    m_currentAssetCount = assetCount;
    m_hoveredBreadcrumbLink.clear(); // stale hover from the previous folder shouldn't carry over
    if (isVirtual) {
        m_currentTitleText = !title.isEmpty() ? title
                           : (isFavorites ? QStringLiteral("Favorites") : QDir(folderPath).dirName());
    } else {
        m_currentTitleText.clear();
        resolveBreadcrumb(folderPath);
    }

    refreshTitleLabel();

    // Favorites and Collections keep the user's manual drag order (already applied by
    // parseFavorites/parseCollectionAssets); everything else is shown alphabetically.
    if (!isFavorites && !isCollection) {
        std::sort(discoveredAssets.begin(), discoveredAssets.end(), [](const AssetHit& a, const AssetHit& b) {
            return a.assetFileName.compare(b.assetFileName, Qt::CaseInsensitive) < 0;
        });
    }

    m_pendingThumbs.clear();
    m_pendingThumbs.reserve(discoveredAssets.size());

    assetListWidget->setUpdatesEnabled(false);
    assetListWidget->clear();

    // --- Subfolder items (physical paths only) ---
    int folderCount = 0;
    if (!isVirtual) {
        QFileInfoList subdirs = QDir(folderPath).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);

        if (!subdirs.isEmpty()) {
            QPixmap folderPx(QStringLiteral(":/resources/icons/folder.png"));
            QPixmap canvas(Constants::THUMB_RENDER_SIZE, Constants::THUMB_CANVAS_HEIGHT);
            canvas.fill(Qt::transparent);
            {
                QPainter p(&canvas);
                const int iconSz = Constants::THUMB_RENDER_SIZE * 82 / 100;
                const QPixmap scaled = folderPx.scaled(iconSz, iconSz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p.drawPixmap((Constants::THUMB_RENDER_SIZE - scaled.width()) / 2,
                             (Constants::THUMB_RENDER_SIZE - scaled.height()) / 2, scaled);
            }
            const QIcon folderIcon(canvas);

            for (const QFileInfo& di : subdirs) {
                QListWidgetItem *folderItem = new QListWidgetItem();
                folderItem->setText(di.fileName());
                folderItem->setIcon(folderIcon);
                folderItem->setData(Qt::UserRole,     di.absoluteFilePath());
                folderItem->setData(Qt::UserRole + 2, QStringLiteral("FOLDER"));
                assetListWidget->addItem(folderItem);
                ++folderCount;
            }
        }
    }

    m_currentFolderCount = folderCount;
    refreshInfoBar();

    for (int i = 0; i < discoveredAssets.size(); ++i) {
        const AssetHit& hit = discoveredAssets[i];

        const QString cleanName = QFileInfo(hit.assetFileName).baseName();
        const QString fullPath = QDir(hit.folderPath).filePath(hit.assetFileName);

        QListWidgetItem *item = new QListWidgetItem();
        item->setText(cleanName);
        item->setData(Qt::UserRole, fullPath);

        if (!hit.matchingImages.isEmpty())
            m_pendingThumbs.append({folderCount + i, QDir(hit.folderPath).filePath(hit.matchingImages.first())});

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
 * @brief Queries the SQLite database for a virtual collection's items and locates their thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseCollectionAssets(int collectionId) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    // Preserve the user's manual drag order (AssetCollectionItemSortOrder), falling back to
    // insertion order.
    query.prepare("SELECT AssetCollectionItemPath FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id "
                  "ORDER BY AssetCollectionItemSortOrder ASC, AssetCollectionItemID ASC");
    query.bindValue(":id", collectionId);
    if (!query.exec()) {
        qWarning() << "Failed to load collection items:" << query.lastError().text();
        return {};
    }

    QStringList paths;
    while (query.next()) paths.append(query.value(0).toString());
    return buildAssetHits(paths);
}

/**
 * @brief Loads every favorited asset path from the database and locates their thumbnails.
 */
QList<AssetHit> AssetManagerWidget::parseFavorites() {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    // Preserve the user's manual drag order (FavoriteSortOrder), falling back to insertion order.
    if (!query.exec("SELECT FavoritePath FROM Favorites ORDER BY FavoriteSortOrder ASC, FavoriteID ASC")) {
        qWarning() << "Failed to load favorites:" << query.lastError().text();
        return {};
    }

    QStringList paths;
    while (query.next()) paths.append(query.value(0).toString());
    return buildAssetHits(paths);
}

/**
 * @brief Resolves a flat list of asset file paths into AssetHits with thumbnails. Folders are
 *        scanned once each (grouped internally for efficiency), but the result is returned in the
 *        caller's input order — Favorites rely on this to preserve the user's manual drag order.
 */
QList<AssetHit> AssetManagerWidget::buildAssetHits(const QStringList& assetPaths) {
    static const QSet<QString> imageExts = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tif", "tiff"};

    // Group the (existing) paths by folder so each folder's images are scanned only once. Keep
    // the original full paths as the keys so we can re-emit in input order at the end.
    QHash<QString, QStringList> folderToPaths;
    for (const QString& fullPath : assetPaths) {
        if (QFileInfo::exists(fullPath))
            folderToPaths[QFileInfo(fullPath).absolutePath()].append(fullPath);
    }

    QHash<QString, AssetHit> hitByPath;
    for (auto folderIt = folderToPaths.cbegin(); folderIt != folderToPaths.cend(); ++folderIt) {
        const QString& folderPath = folderIt.key();
        const QStringList& folderPaths = folderIt.value();

        // Build set of relevant basenames so the directory scan stays focused
        QSet<QString> relevantBases;
        relevantBases.reserve(folderPaths.size());
        for (const QString& p : folderPaths) relevantBases.insert(QFileInfo(p).baseName());

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

        for (const QString& p : folderPaths) {
            const QFileInfo fi(p);
            AssetHit hit;
            hit.folderPath = folderPath;
            hit.assetFileName = fi.fileName();
            const auto igIt = imagesByBase.constFind(fi.baseName());
            if (igIt != imagesByBase.cend()) {
                hit.matchingImages.reserve(1 + igIt->others.size());
                hit.matchingImages.append(igIt->bestImage);
                hit.matchingImages.append(igIt->others);
            }
            hitByPath.insert(p, std::move(hit));
        }
    }

    // Re-emit in the caller's original input order.
    QList<AssetHit> finalHits;
    finalHits.reserve(hitByPath.size());
    for (const QString& fullPath : assetPaths) {
        const auto it = hitByPath.constFind(fullPath);
        if (it != hitByPath.cend()) finalHits.append(it.value());
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
        
        QAction *manageFoldersAction = emptyMenu.addAction(QIcon(":/resources/icons/preferences.png"), "Manage Asset Folders");
        QAction *refreshAction = emptyMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        QAction *selectedAction = emptyMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (selectedAction == manageFoldersAction) {
            emit manageAssetFoldersRequested();
        } else if (selectedAction == refreshAction) {
            refreshAssetManager();
        }
        return;
    }

    QModelIndex sourceIndex = proxyModel->mapToSource(proxyIndex);
    QString folderPath = dirModel->data(sourceIndex, Qt::UserRole).toString();
    QString folderName = dirModel->data(sourceIndex, Qt::DisplayRole).toString();

    // Normalize display name: search result items encode the full relative path with " / " separators;
    // strip everything before the last separator so we get just the folder name.
    if (folderName.contains(" / "))
        folderName = folderName.section(" / ", -1);

    // Prevent context menus on non-interactive structural nodes
    if (folderPath.isEmpty() || folderPath == "BROKEN_PATH" || folderPath == "SEPARATOR") {
        return;
    }

    // =========================================================================
    // DEDICATED MENU: "FAVORITES" ROOT NODE (just Refresh — it's a flat container)
    // =========================================================================
    if (folderPath == "FAVORITES_ROOT") {
        QMenu favMenu(this);
        favMenu.setObjectName("AssetManagerContextMenu");
        QAction *refreshAction = favMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        if (favMenu.exec(dirTreeView->viewport()->mapToGlobal(pos)) == refreshAction)
            refreshAssetManager();
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
        QAction *newCollAction = rootMenu.addAction(QIcon(":/resources/icons/add-col.png"), "New Collection");

        rootMenu.addSeparator();
        QAction *manageFoldersAction = rootMenu.addAction(QIcon(":/resources/icons/preferences.png"), "Manage Asset Folders");
        QAction *refreshAction = rootMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        QAction *selectedAction = rootMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
        else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex);
        else if (selectedAction == newCollAction) {
            int cid = getOrCreateCollection(uniqueCollectionName("New Collection", 0), 0);
            if (cid > 0) navigateToCollectionNode(cid, true);
        }
        else if (selectedAction == manageFoldersAction) emit manageAssetFoldersRequested();
        else if (selectedAction == refreshAction) refreshAssetManager();

        return;
    }

    // =========================================================================
    // DEDICATED MENU: INDIVIDUAL DB COLLECTION ITEMS
    // =========================================================================
    if (folderPath.startsWith("COLLECTION_")) {
        const int collId = folderPath.mid(11).toInt();

        // Deletable only when both asset items and sub-collections are absent
        bool collIsEmpty = true;
        {
            QSqlQuery chk(QSqlDatabase::database("db_conn"));
            chk.prepare("SELECT COUNT(*) FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id");
            chk.bindValue(":id", collId);
            if (chk.exec() && chk.next() && chk.value(0).toInt() > 0) collIsEmpty = false;
        }
        if (collIsEmpty) {
            QSqlQuery chk(QSqlDatabase::database("db_conn"));
            chk.prepare("SELECT COUNT(*) FROM AssetCollections WHERE AssetCollectionParentID = :id");
            chk.bindValue(":id", collId);
            if (chk.exec() && chk.next() && chk.value(0).toInt() > 0) collIsEmpty = false;
        }

        QMenu collMenu(this);
        collMenu.setObjectName("AssetManagerContextMenu");

        QAction *renameAction = collMenu.addAction(QIcon(":/resources/icons/rename.png"),
                                                   QStringLiteral("Rename %1").arg(Constants::TERM_COL_SINGULAR));
        QAction *deleteAction = collMenu.addAction(QStringLiteral("Delete %1").arg(Constants::TERM_COL_SINGULAR));
        deleteAction->setEnabled(collIsEmpty);
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
        QAction *newSubCollAction = collMenu.addAction(QIcon(":/resources/icons/add-col.png"), "New Sub-Collection");

        collMenu.addSeparator();
        QAction *manageFoldersAction = collMenu.addAction(QIcon(":/resources/icons/preferences.png"), "Manage Asset Folders");
        QAction *refreshAction = collMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        QAction *selectedAction = collMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

        if (renameAction && selectedAction == renameAction) {
            dirTreeView->edit(proxyIndex);
        } else if (deleteAction && selectedAction == deleteAction) {
            QSqlDatabase db = QSqlDatabase::database("db_conn");
            QSqlQuery q(db);  q.prepare("DELETE FROM AssetCollections WHERE AssetCollectionID = :id");         q.bindValue(":id", collId); q.exec();
            QSqlQuery q2(db); q2.prepare("DELETE FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id"); q2.bindValue(":id", collId); q2.exec();
            QModelIndex srcIdx = proxyModel->mapToSource(proxyIndex);
            QStandardItem *collItem = dirModel->itemFromIndex(srcIdx);
            if (collItem && collItem->parent()) collItem->parent()->removeRow(collItem->row());
            titleLabel->setText("Select a folder to view assets...");
            assetListWidget->clear();
            m_currentFolderPath.clear();
            infoBarLabel->hide();
        } else if (expandAction && selectedAction == expandAction) {
            dirTreeView->expand(proxyIndex);
        } else if (collapseAction && selectedAction == collapseAction) {
            collapseNodeRecursively(proxyIndex);
        } else if (selectedAction == newSubCollAction) {
            int cid = getOrCreateCollection(uniqueCollectionName("New Collection", collId), collId);
            if (cid > 0) navigateToCollectionNode(cid, true);
        } else if (selectedAction == manageFoldersAction) {
            emit manageAssetFoldersRequested();
        } else if (selectedAction == refreshAction) {
            refreshAssetManager();
        }

        return;
    }

    // =========================================================================
    // STANDARD MENU: PHYSICAL HARD DRIVE DIRECTORIES
    // =========================================================================
    QStandardItem *clickedItem = dirModel->itemFromIndex(sourceIndex);
    const bool inCollectionOrSearch = (contextForTreeItem(clickedItem) != BrowseContext::Library);

    QMenu contextMenu(this);
    contextMenu.setObjectName("AssetManagerContextMenu");

    QAction *findInLibraryAction = nullptr;
    QAction *browseAction = nullptr;

    if (inCollectionOrSearch) {
        findInLibraryAction = contextMenu.addAction(QIcon(":/resources/icons/tree.png"), "Find In Library");
        browseAction = contextMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
        contextMenu.addSeparator();
    }

    QMenu *addToCollMenu = buildAddToCollectionMenu(&contextMenu, folderPath);
    contextMenu.addAction(addToCollMenu->menuAction());
    contextMenu.addSeparator();

    if (!inCollectionOrSearch) {
        browseAction = contextMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
        contextMenu.addSeparator();
    }

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
    QAction *manageFoldersAction = contextMenu.addAction(QIcon(":/resources/icons/preferences.png"), "Manage Asset Folders");
    QAction *refreshAction = contextMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

    QAction *selectedAction = contextMenu.exec(dirTreeView->viewport()->mapToGlobal(pos));

    if (findInLibraryAction && selectedAction == findInLibraryAction)
        navigateToFolderInTree(folderPath);
    else if (expandAction && selectedAction == expandAction) dirTreeView->expand(proxyIndex);
    else if (collapseAction && selectedAction == collapseAction) collapseNodeRecursively(proxyIndex);
    else if (selectedAction == expandBranchAction) expandNodeRecursively(proxyIndex);
    else if (selectedAction == browseAction) QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
    else if (selectedAction == manageFoldersAction) emit manageAssetFoldersRequested();
    else if (selectedAction == refreshAction) refreshAssetManager();
}

/**
 * @brief Constructs and routes right-click context menus for the asset grid.
 */
void AssetManagerWidget::onGridContextMenuRequested(const QPoint &pos) {
    QListWidgetItem *item = assetListWidget->itemAt(pos);

    // =========================================================================
    // DEDICATED MENU: FOLDER ITEM
    // =========================================================================
    if (item && item->data(Qt::UserRole + 2).toString() == QStringLiteral("FOLDER")) {
        const QString folderPath = item->data(Qt::UserRole).toString();

        QStandardItem *curTreeItem = nullptr;
        QModelIndex curTreeIndex = dirTreeView->currentIndex();
        if (curTreeIndex.isValid())
            curTreeItem = dirModel->itemFromIndex(proxyModel->mapToSource(curTreeIndex));
        const bool inCollectionOrSearch = (contextForTreeItem(curTreeItem) != BrowseContext::Library);

        QMenu folderMenu(this);
        folderMenu.setObjectName("AssetManagerContextMenu");

        QAction *openAction = folderMenu.addAction(QIcon(":/resources/icons/open-item.png"), "Open");

        QAction *findInLibraryAction = nullptr;
        QAction *browseAction = nullptr;
        if (inCollectionOrSearch) {
            findInLibraryAction = folderMenu.addAction(QIcon(":/resources/icons/tree.png"), "Find In Library");
            browseAction = folderMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
        }
        folderMenu.addSeparator();

        QMenu *addMenu = buildAddToCollectionMenu(&folderMenu, folderPath);
        folderMenu.addMenu(addMenu);

        if (!inCollectionOrSearch)
            browseAction = folderMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
        folderMenu.addSeparator();
        QAction *refreshAction = folderMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");

        QAction *selected = folderMenu.exec(assetListWidget->viewport()->mapToGlobal(pos));

        if (selected == openAction) {
            deselectTree();
            displayFolder(folderPath);
        } else if (findInLibraryAction && selected == findInLibraryAction) {
            navigateToFolderInTree(folderPath);
        } else if (selected == browseAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
        } else if (selected == refreshAction) {
            QModelIndex currentIndex = dirTreeView->currentIndex();
            if (currentIndex.isValid()) onFolderSelected(currentIndex);
            else refreshAssetManager();
        }
        return;
    }

    // =========================================================================
    // DEDICATED MENU: EMPTY SPACE (BACKGROUND CLICK IN GRID)
    // =========================================================================
    if (!item) {
        QMenu emptyMenu(this);
        emptyMenu.setObjectName("AssetManagerContextMenu");

        // Browse the physical folder currently shown in the grid. Disabled when nothing is
        // displayed, or when the grid is showing a virtual Collection (COLLECTION_<id>, which
        // has no single on-disk folder to open).
        QAction *browseAction = emptyMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
        const bool hasPhysicalFolder = !m_currentFolderPath.isEmpty()
            && !m_currentFolderPath.startsWith("COLLECTION_")
            && QDir(m_currentFolderPath).exists();
        browseAction->setEnabled(hasPhysicalFolder);

        emptyMenu.addSeparator();
        QAction *refreshAction = emptyMenu.addAction(QIcon(":/resources/icons/refresh.png"), "Refresh");
        QAction *selectedAction = emptyMenu.exec(assetListWidget->viewport()->mapToGlobal(pos));

        if (selectedAction == browseAction) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_currentFolderPath));
        } else if (selectedAction == refreshAction) {
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
    QAction *findInLibraryAction = nullptr;
    QAction *browseAction = nullptr;
    int currentCollectionId = -1;
    QModelIndex currentTreeIndex = dirTreeView->currentIndex();
    QStandardItem *curTreeItem = nullptr;

    if (currentTreeIndex.isValid()) {
        QModelIndex sourceIndex = proxyModel->mapToSource(currentTreeIndex);
        QString currentTreeData = dirModel->data(sourceIndex, Qt::UserRole).toString();
        curTreeItem = dirModel->itemFromIndex(sourceIndex);

        if (currentTreeData.startsWith("COLLECTION_"))
            currentCollectionId = currentTreeData.mid(11).toInt();
    }

    const BrowseContext browseContext = contextForTreeItem(curTreeItem);
    const bool inFavoritesView = (browseContext == BrowseContext::Favorites);

    // Find In Library / Browse Folder apply to any asset shown outside the plain Library tree
    // (Collections, Search Results, Favorites), not just ones added directly to a collection.
    const bool inCollectionOrSearch = (browseContext != BrowseContext::Library);
    if (inCollectionOrSearch) {
        findInLibraryAction = itemMenu.addAction(QIcon(":/resources/icons/tree.png"), "Find In Library");
        browseAction = itemMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
    }

    itemMenu.addSeparator();

    // Favorites toggle, sitting directly above the collection submenu. Available for asset items
    // everywhere they appear; reads "Remove From Favorites" while browsing the Favorites view.
    QAction *addToFavAction = nullptr;
    if (!inFavoritesView)
        addToFavAction = itemMenu.addAction("Add To Favorites");

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
    }
    // If we ARE in a collection, build the "Move" and "Copy" menus
    else {
        moveMenu = new QMenu("Move To Collection", &itemMenu);
        moveMenu->setIcon(QIcon(":/resources/icons/collections.png")); // Or a dedicated move icon if you have one
        moveMenu->setObjectName("AssetManagerContextMenu");

        copyMenu = new QMenu("Copy To Collection", &itemMenu);
        copyMenu->setIcon(QIcon(":/resources/icons/collections.png")); // Or a dedicated copy icon
        copyMenu->setObjectName("AssetManagerContextMenu");
    }

    const QList<QPair<int, QString>> collections = collectionPathList();
    bool hasOtherCollections = false; // Tracks if there are valid destinations to move/copy to

    // "New Collection" always at the top of the Add menu
    if (currentCollectionId == -1) {
        QAction *newCollAction = addMenu->addAction("New Collection");
        connect(newCollAction, &QAction::triggered, [this, fullPath]() {
            int cid = getOrCreateCollection(uniqueCollectionName("New Collection", 0), 0);
            if (cid > 0) { addAssetToCollection(fullPath, cid); navigateToCollectionNode(cid, true); }
        });
    }

    bool hasExistingForAdd = false;
    for (const auto& [colId, colPath] : collections) {
        if (currentCollectionId == -1) {
            if (!hasExistingForAdd) { addMenu->addSeparator(); hasExistingForAdd = true; }
            // STANDARD ADD
            QAction *addAction = addMenu->addAction(colPath);
            connect(addAction, &QAction::triggered, [this, colId, fullPath]() {
                addAssetToCollection(fullPath, colId); navigateToCollectionAssetItem(colId, fullPath);
            });
        } else {
            // MOVE & COPY (Exclude the collection the user is currently standing in!)
            if (colId != currentCollectionId) {
                hasOtherCollections = true;

                // Move Lambda: Remove from current, Add to new, Refresh Grid
                QAction *moveAction = moveMenu->addAction(colPath);
                connect(moveAction, &QAction::triggered, [this, fullPath, currentCollectionId, colId, currentTreeIndex]() {
                    removeAssetFromCollection(fullPath, currentCollectionId);
                    addAssetToCollection(fullPath, colId);
                    onFolderSelected(currentTreeIndex); // Instantly removes it from the current view
                });

                // Copy Lambda: Just Add to new (No refresh needed, it stays in the current view)
                QAction *copyAction = copyMenu->addAction(colPath);
                connect(copyAction, &QAction::triggered, [this, fullPath, colId]() {
                    addAssetToCollection(fullPath, colId);
                });
            }
        }
    }

    // Attach the appropriate menus to the context list and grey them out if empty
    if (currentCollectionId == -1) {
        itemMenu.addMenu(addMenu);
    } else {
        if (!hasOtherCollections) {
            moveMenu->setEnabled(false);
            copyMenu->setEnabled(false);
        }
        itemMenu.addMenu(moveMenu);
        itemMenu.addMenu(copyMenu);
    }

    // Action 3: Browse Folder (already added at the top when inside Collections/Search Results)
    if (!inCollectionOrSearch)
        browseAction = itemMenu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");

    if (currentCollectionId != -1)
        removeFromColAction = itemMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), "Remove From Collection");

    // Favorites toggle, sitting directly above the collection submenu. Available for asset items
    // everywhere they appear; reads "Remove From Favorites" while browsing the Favorites view.
    QAction *removeFromFavAction = nullptr;
    if (inFavoritesView)
        removeFromFavAction = itemMenu.addAction(QIcon(":/resources/icons/unfavorite.png"), "Remove From Favorites");

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
    else if (addToFavAction && selectedAction == addToFavAction) {
        addAssetToFavorites(fullPath);
    }
    else if (removeFromFavAction && selectedAction == removeFromFavAction) {
        removeAssetFromFavorites(fullPath);
        onFolderSelected(currentTreeIndex); // instantly drop it from the Favorites grid
    }
    else if (findInLibraryAction && selectedAction == findInLibraryAction) {
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
 * @brief Persists a renamed Collection back to the SQLite database.
 */
void AssetManagerWidget::onItemChanged(QStandardItem *item) {
    if (!item) return;

    // ---------------------------------------------------------
    // Handle Collection Renaming (at any nesting depth, not just top-level collections)
    // ---------------------------------------------------------
    QString dataStr = item->data(Qt::UserRole).toString();
    if (dataStr.startsWith("COLLECTION_")) {
        QString newName = item->text();
        int collId = dataStr.mid(11).toInt();

        QSqlQuery query(QSqlDatabase::database("db_conn"));
        query.prepare("UPDATE AssetCollections SET AssetCollectionName = :name WHERE AssetCollectionID = :id");
        query.bindValue(":name", newName);
        query.bindValue(":id", collId);

        if (!query.exec()) {
            qWarning() << "[!] Failed to update collection name in DB:" << query.lastError().text();
        } else if (item->parent()) {
            item->parent()->sortChildren(0, Qt::AscendingOrder);
        } else {
            collectionsRootItem->sortChildren(0, Qt::AscendingOrder);
        }
    }
}

/**
 * @brief Handles double-clicking an item in the asset grid to open it.
 */
void AssetManagerWidget::onGridItemDoubleClicked(QListWidgetItem *item) {
    if (!item) return;

    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;

    if (item->data(Qt::UserRole + 2).toString() == QStringLiteral("FOLDER")) {
        deselectTree();
        displayFolder(path);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

/**
 * @brief Intercepts events on the asset grid viewport to manage interactive custom tooltips.
 */
bool AssetManagerWidget::eventFilter(QObject *watched, QEvent *event) {
    // The title label re-fits its breadcrumb whenever resized, and right-click targets
    // whichever ancestor link is currently hovered (falling back to the current folder
    // when right-clicking the bright, non-link current-folder segment).
    if (watched == titleLabel) {
        if (event->type() == QEvent::Resize) {
            refreshTitleLabel();
        } else if (event->type() == QEvent::ContextMenu) {
            const QString targetPath = !m_hoveredBreadcrumbLink.isEmpty()
                ? m_hoveredBreadcrumbLink : m_currentFolderPath;
            // Only physical folders get an Open/Browse menu — not the virtual Collections/Favorites titles.
            if (!targetPath.isEmpty() && !targetPath.startsWith("COLLECTION_") && targetPath != "FAVORITES_ROOT") {
                QMenu menu(this);
                menu.setObjectName("AssetManagerContextMenu");
                QAction *openAction   = menu.addAction(QIcon(":/resources/icons/open-item.png"), "Open");
                menu.addSeparator();
                QAction *browseAction = menu.addAction(QIcon(":/resources/icons/browse-folder.png"), "Browse Folder");
                QAction *selected = menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
                if (selected == openAction) {
                    deselectTree();
                    displayFolder(targetPath);
                } else if (selected == browseAction)
                    QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
            }
            return true; // always suppress Qt's built-in "Copy Link" menu
        }
        return false;
    }

    if (watched == assetListWidget->viewport()) {

        // 0. Manual drag-reorder for sortable grids (Favorites and Collections). We avoid Qt's
        //    QDrag/InternalMove path (in IconMode it free-positions icons rather than reordering
        //    rows). Once the pointer crosses the drag threshold we float a translucent ghost of
        //    the thumbnail under the cursor and show a line at the gap where it will land. The
        //    grid is left untouched until release — moving items mid-drag would reflow the layout
        //    and throw off where the cursor is pointing — then the item drops at the indicated
        //    gap and the order persists.
        if (isSortableView()) {
            if (event->type() == QEvent::MouseButtonPress) {
                QMouseEvent *me = static_cast<QMouseEvent *>(event);
                if (me->button() == Qt::LeftButton) {
                    m_dragItem = assetListWidget->itemAt(me->pos());
                    m_dragStartPos = me->pos();
                    m_dragging = false;
                }
            } else if (event->type() == QEvent::MouseButtonRelease) {
                if (m_dragging && m_dragItem) {
                    QMouseEvent *me = static_cast<QMouseEvent *>(event);
                    const int fromRow = assetListWidget->row(m_dragItem);
                    int insertIdx = gridInsertIndex(me->pos());
                    if (fromRow < insertIdx) --insertIdx; // removal shifts everything after fromRow up
                    if (insertIdx != fromRow) {
                        QListWidgetItem *moved = assetListWidget->takeItem(fromRow);
                        assetListWidget->insertItem(insertIdx, moved);
                        assetListWidget->setCurrentItem(moved);
                    }
                    persistGridOrder();
                }
                endGridDrag();
            } else if (event->type() == QEvent::MouseMove) {
                QMouseEvent *me = static_cast<QMouseEvent *>(event);
                if ((me->buttons() & Qt::LeftButton) && m_dragItem) {
                    if (!m_dragging &&
                        (me->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
                        beginGridDrag();
                    }
                    if (m_dragging) {
                        m_dragLastPos = me->pos();
                        if (m_dragPreview) {
                            const QPoint g = me->globalPosition().toPoint();
                            m_dragPreview->move(g - QPoint(m_dragPreview->width() / 2,
                                                              m_dragPreview->height() / 2));
                        }
                        updateGridDropIndicator(me->pos());

                        // Auto-scroll when the cursor is in (or past) the top/bottom edge zone.
                        const int edge = 30;
                        const int h = assetListWidget->viewport()->height();
                        const int y = me->pos().y();
                        m_scrollDir = (y < edge) ? -1 : (y > h - edge) ? 1 : 0;
                        if (m_scrollDir != 0) {
                            if (!m_scrollTimer->isActive()) m_scrollTimer->start();
                        } else {
                            m_scrollTimer->stop();
                        }

                        return true; // consume during an active drag (also suppresses tooltips)
                    }
                }
            }
        }

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
 * @details Walks down the tree expanding lazy-loaded nodes segment by segment,
 *          starting from the physical library root that contains the target path.
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

    QSqlQuery libQuery(QSqlDatabase::database("db_conn"));
    libQuery.exec("SELECT AssetLibraryPath FROM AssetLibraries WHERE AssetLibraryEnabled = 1");
    while (libQuery.next()) {
        const QString libPath = libQuery.value(0).toString();
        const QString normLib = QDir::cleanPath(QDir::fromNativeSeparators(libPath));
        // Match assets in a subfolder of the library AND assets that live directly in the
        // library root itself (e.g. built-in Maquettes), where normTarget == normLib.
        const bool isLibraryRoot = (normTarget.compare(normLib, Qt::CaseInsensitive) == 0);
        if (!isLibraryRoot && !normTarget.startsWith(normLib + '/', Qt::CaseInsensitive)) continue;

        // Find this library's top-level root item in the tree
        QModelIndex libRootProxy;
        const int rootRows = proxyModel->rowCount(QModelIndex());
        for (int r = 0; r < rootRows; ++r) {
            const QModelIndex candidate = proxyModel->index(r, 0, QModelIndex());
            if (proxyModel->data(candidate, Qt::UserRole).toString() == libPath) {
                libRootProxy = candidate;
                break;
            }
        }
        if (!libRootProxy.isValid()) continue;

        const QStringList segments = normTarget.mid(normLib.length() + 1).split('/', Qt::SkipEmptyParts);
        if (segments.isEmpty()) { selectNode(libRootProxy); return; }

        if (!dirTreeView->isExpanded(libRootProxy))
            dirTreeView->expand(libRootProxy);

        const QModelIndex result = walkSegments(libRootProxy, segments);
        if (result.isValid()) { selectNode(result); return; }
    }
}

void AssetManagerWidget::navigateToLibraryRoot(const QString& libraryPath) {
    const int rootRows = proxyModel->rowCount(QModelIndex());
    for (int r = 0; r < rootRows; ++r) {
        const QModelIndex candidate = proxyModel->index(r, 0, QModelIndex());
        if (proxyModel->data(candidate, Qt::UserRole).toString() != libraryPath) continue;

        dirTreeView->setCurrentIndex(candidate);
        dirTreeView->scrollTo(candidate, QAbstractItemView::PositionAtCenter);
        onFolderSelected(candidate);
        return;
    }
}

/**
 * @brief Walks a tree item's ancestor chain to determine whether it lives under the
 *        Collections or Search Results branch, no matter how deeply it's nested (e.g. a
 *        subfolder several levels under a collection's folder shortcut still counts).
 */
BrowseContext AssetManagerWidget::contextForTreeItem(QStandardItem* item) const {
    while (item) {
        if (item == collectionsRootItem) return BrowseContext::Collection;
        if (item == searchResultsRootItem) return BrowseContext::SearchResults;
        if (item == favoritesRootItem) return BrowseContext::Favorites;
        item = item->parent();
    }
    return BrowseContext::Library;
}

// Caps a search result's displayed path to the last N segments, prefixing "..." when truncated.
static QString truncateSearchPath(const QString& relPath, int maxSegments = 4) {
    QStringList segments = relPath.split('/', Qt::SkipEmptyParts);
    if (segments.size() > maxSegments) {
        segments = segments.mid(segments.size() - maxSegments);
        return QStringLiteral("... / ") + segments.join(" / ");
    }
    return segments.join(" / ");
}

// Adds folderPath as a result if it is a DirectHit.
// If it is an IndirectHit, recursively descends into its children until DirectHit leaves are found.
void AssetManagerWidget::collectDirectHits(const QString& folderPath, const QDir& libRootDir,
                                            QSet<QString>& added, QList<QStandardItem*>& results) {
    if (added.contains(folderPath)) return;

    if (proxyModel->isDirectHit(folderPath)) {
        added.insert(folderPath);
        QStandardItem *item = new QStandardItem(truncateSearchPath(libRootDir.relativeFilePath(folderPath)));
        item->setData(folderPath, Qt::UserRole);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        QDirIterator subIt(folderPath, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        if (subIt.hasNext()) item->appendRow(new QStandardItem("..."));
        results.append(item);
    } else if (proxyModel->hasHit(folderPath)) {
        // IndirectHit — don't add this folder; recurse into children that have hits
        const QFileInfoList children = QDir(folderPath).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& child : children)
            collectDirectHits(child.absoluteFilePath(), libRootDir, added, results);
    }
}

void AssetManagerWidget::navigateToCollectionNode(int collectionId, bool enterEditMode) {
    const QString targetPath = QString("COLLECTION_%1").arg(collectionId);

    QModelIndex collRootProxy = proxyModel->mapFromSource(dirModel->indexFromItem(collectionsRootItem));
    if (collRootProxy.isValid() && !dirTreeView->isExpanded(collRootProxy))
        dirTreeView->expand(collRootProxy);

    QModelIndex colProxy = findProxyIndexByPath(collRootProxy, targetPath);
    if (!colProxy.isValid()) return;

    dirTreeView->setCurrentIndex(colProxy);
    dirTreeView->scrollTo(colProxy, QAbstractItemView::PositionAtCenter);
    onFolderSelected(colProxy);

    if (enterEditMode)
        dirTreeView->edit(colProxy);
}

/**
 * @brief Jumps to an asset that was just added to a Collection: selects the collection (which
 *        populates the asset grid below) then finds and highlights the matching grid item.
 */
void AssetManagerWidget::navigateToCollectionAssetItem(int collectionId, const QString& assetFullPath) {
    navigateToCollectionNode(collectionId);

    for (int i = 0; i < assetListWidget->count(); ++i) {
        QListWidgetItem *item = assetListWidget->item(i);
        if (item->data(Qt::UserRole).toString() == assetFullPath) {
            assetListWidget->setCurrentItem(item);
            assetListWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}

/**
 * @brief Recursively searches the entire Collections subtree (not just direct children) for the
 *        tree item tagged "COLLECTION_<collectionId>", so callers can find a node at any nesting depth.
 */
QStandardItem* AssetManagerWidget::findCollectionTreeItem(QStandardItem* parent, int collectionId) const {
    if (!parent) return nullptr;
    const QString target = QString("COLLECTION_%1").arg(collectionId);

    for (int i = 0; i < parent->rowCount(); ++i) {
        QStandardItem *child = parent->child(i);
        if (child->data(Qt::UserRole).toString() == target) return child;
        if (QStandardItem *found = findCollectionTreeItem(child, collectionId)) return found;
    }
    return nullptr;
}

/**
 * @brief Recursively populates a Collections tree node with its child collections, so the
 *        Collections branch can be nested arbitrarily deep. (Asset items live in the DB and are
 *        shown in the grid when a collection is selected — they are not tree nodes.)
 * @param parentItem The tree node to populate (collectionsRootItem for the top level).
 * @param parentCollectionId The DB collection ID whose children to load (0 = top-level).
 */
void AssetManagerWidget::loadCollectionsInto(QStandardItem* parentItem, int parentCollectionId) {
    QSqlQuery collQuery(QSqlDatabase::database("db_conn"));
    collQuery.prepare("SELECT AssetCollectionID, AssetCollectionName FROM AssetCollections "
                       "WHERE AssetCollectionParentID = :pid ORDER BY AssetCollectionName COLLATE NOCASE");
    collQuery.bindValue(":pid", parentCollectionId);
    if (!collQuery.exec()) return;

    while (collQuery.next()) {
        const int colId = collQuery.value(0).toInt();
        const QString colName = collQuery.value(1).toString();

        QStandardItem *colItem = new QStandardItem(colName);
        colItem->setData(QString("COLLECTION_%1").arg(colId), Qt::UserRole);
        colItem->setFlags(colItem->flags() | Qt::ItemIsEditable);
        parentItem->appendRow(colItem);

        // Recurse into this collection's own sub-collections
        loadCollectionsInto(colItem, colId);
    }

    parentItem->sortChildren(0, Qt::AscendingOrder);
}

/**
 * @brief Returns every collection as (id, full path) pairs, e.g. (7, "Clothing / Shirts"),
 *        sorted by path, so flat pickers can let the user target any nesting depth directly.
 */
QList<QPair<int, QString>> AssetManagerWidget::collectionPathList() const {
    QHash<int, QPair<QString, int>> info; // id -> (name, parentId)
    QSqlQuery q(QSqlDatabase::database("db_conn"));
    q.exec("SELECT AssetCollectionID, AssetCollectionName, AssetCollectionParentID FROM AssetCollections");
    while (q.next())
        info.insert(q.value(0).toInt(), {q.value(1).toString(), q.value(2).toInt()});

    QList<QPair<int, QString>> result;
    result.reserve(info.size());
    for (auto it = info.cbegin(); it != info.cend(); ++it) {
        QStringList parts;
        int cur = it.key();
        QSet<int> seen; // guards against an accidental parent cycle
        while (info.contains(cur) && !seen.contains(cur)) {
            seen.insert(cur);
            parts.prepend(info[cur].first);
            cur = info[cur].second;
            if (cur == 0) break;
        }
        result.append({it.key(), parts.join(" / ")});
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second.compare(b.second, Qt::CaseInsensitive) < 0;
    });
    return result;
}

/**
 * @brief Builds an "Add To Collection" submenu for a physical folder: a pinned "New Collection"
 *        entry (creates a top-level collection from the folder) followed by every existing
 *        collection (shown by full path, creates a sub-collection under it). Each entry turns the
 *        folder into a populated collection — see addFolderAsCollection. Caller owns the menu.
 */
QMenu* AssetManagerWidget::buildAddToCollectionMenu(QWidget* parentMenu, const QString& folderPath) {
    QMenu *addMenu = new QMenu("Add To Collection", parentMenu);
    addMenu->setIcon(QIcon(":/resources/icons/collections.png"));
    addMenu->setObjectName("AssetManagerContextMenu");

    QAction *newCollAction = addMenu->addAction("New Collection");
    connect(newCollAction, &QAction::triggered, this, [this, folderPath]() {
        int cid = addFolderAsCollection(folderPath, 0);
        if (cid > 0) navigateToCollectionNode(cid);
    });

    const QList<QPair<int, QString>> collections = collectionPathList();
    if (!collections.isEmpty()) addMenu->addSeparator();

    for (const auto& [colId, colPath] : collections) {
        QAction *addAction = addMenu->addAction(colPath);
        connect(addAction, &QAction::triggered, this, [this, folderPath, colId]() {
            int cid = addFolderAsCollection(folderPath, colId);
            if (cid > 0) navigateToCollectionNode(cid);
        });
    }

    return addMenu;
}

/**
 * @brief Turns a physical folder into a collection: creates a new (uniquely named) sub-collection
 *        named after the folder under parentCollectionId, then adds the folder's own assets as
 *        collection items — subfolders and their contents are ignored. Returns the new id, or -1.
 */
int AssetManagerWidget::addFolderAsCollection(const QString& folderPath, int parentCollectionId) {
    const QString folderName = QDir(folderPath).dirName();
    const int newColId = getOrCreateCollection(
        uniqueCollectionName(folderName, parentCollectionId), parentCollectionId);
    if (newColId <= 0) return -1;

    // parseFolderAssets returns only this folder's own (non-recursive) thumbnailed assets.
    const QList<AssetHit> assets = parseFolderAssets(folderPath);

    QSqlDatabase db = QSqlDatabase::database("db_conn");
    db.transaction();
    QSqlQuery q(db);
    q.prepare("INSERT INTO AssetCollectionItems (AssetCollectionItemPath, AssetCollectionItemCol) VALUES (:path, :id)");
    for (const AssetHit& hit : assets) {
        q.bindValue(":path", QDir(hit.folderPath).filePath(hit.assetFileName));
        q.bindValue(":id", newColId);
        q.exec();
    }
    db.commit();

    proxyModel->invalidateAndRefresh(QString("COLLECTION_%1").arg(newColId));
    return newColId;
}

/**
 * @brief Returns baseName if no sibling under parentCollectionId already has it, otherwise
 *        "baseName (2)", "baseName (3)", etc. — lets repeated "New Collection" clicks under the
 *        same parent each create a distinct collection instead of colliding on name.
 */
QString AssetManagerWidget::uniqueCollectionName(const QString& baseName, int parentCollectionId) const {
    QSqlQuery q(QSqlDatabase::database("db_conn"));
    q.prepare("SELECT AssetCollectionName FROM AssetCollections WHERE AssetCollectionParentID = :pid");
    q.bindValue(":pid", parentCollectionId);
    q.exec();

    QSet<QString> existingNames;
    while (q.next()) existingNames.insert(q.value(0).toString());

    if (!existingNames.contains(baseName)) return baseName;

    int suffix = 2;
    QString candidate;
    do {
        candidate = QStringLiteral("%1 (%2)").arg(baseName).arg(suffix++);
    } while (existingNames.contains(candidate));

    return candidate;
}

int AssetManagerWidget::getOrCreateCollection(const QString& name, int parentCollectionId) {
    QSqlQuery q(QSqlDatabase::database("db_conn"));
    q.prepare("SELECT AssetCollectionID FROM AssetCollections WHERE AssetCollectionName = :name AND AssetCollectionParentID = :pid");
    q.bindValue(":name", name);
    q.bindValue(":pid", parentCollectionId);
    if (q.exec() && q.next()) return q.value(0).toInt();

    QSqlQuery ins(QSqlDatabase::database("db_conn"));
    ins.prepare("INSERT INTO AssetCollections (AssetCollectionName, AssetCollectionParentID) VALUES (:name, :pid)");
    ins.bindValue(":name", name);
    ins.bindValue(":pid", parentCollectionId);
    if (!ins.exec()) return -1;
    int newId = ins.lastInsertId().toInt();

    QStandardItem *parentItem = (parentCollectionId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(collectionsRootItem, parentCollectionId);
    if (!parentItem) return newId;

    QStandardItem *colItem = new QStandardItem(name);
    colItem->setData("COLLECTION_" + QString::number(newId), Qt::UserRole);
    colItem->setFlags(colItem->flags() | Qt::ItemIsEditable);
    parentItem->appendRow(colItem);
    parentItem->sortChildren(0, Qt::AscendingOrder);

    return newId;
}

/**
 * @brief Moves a collection (and its entire subtree, untouched) under a new parent, in response
 *        to a validated drag-drop from AssetFolderProxyModel. Only the dragged collection's own
 *        AssetCollectionParentID changes in the DB — descendants keep referencing its same ID, so
 *        they come along structurally without any writes of their own.
 */
void AssetManagerWidget::reparentCollection(int collectionId, int newParentId) {
    QStandardItem *collItem = findCollectionTreeItem(collectionsRootItem, collectionId);
    if (!collItem) return;
    QStandardItem *oldParentItem = collItem->parent();
    if (!oldParentItem) return;

    QStandardItem *newParentItem = (newParentId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(collectionsRootItem, newParentId);
    if (!newParentItem || oldParentItem == newParentItem) return;

    QSqlQuery q(QSqlDatabase::database("db_conn"));
    q.prepare("UPDATE AssetCollections SET AssetCollectionParentID = :pid WHERE AssetCollectionID = :id");
    q.bindValue(":pid", newParentId);
    q.bindValue(":id", collectionId);
    if (!q.exec()) {
        qWarning() << "[!] Failed to reparent collection:" << q.lastError().text();
        return;
    }

    const QList<QStandardItem*> takenRow = oldParentItem->takeRow(collItem->row());
    newParentItem->appendRow(takenRow);
    newParentItem->sortChildren(0, Qt::AscendingOrder);

    const QModelIndex newProxyIdx = proxyModel->mapFromSource(dirModel->indexFromItem(collItem));
    if (newProxyIdx.isValid()) {
        dirTreeView->setCurrentIndex(newProxyIdx);
        dirTreeView->scrollTo(newProxyIdx);
    }
}

void AssetManagerWidget::addAssetToCollection(const QString& filePath, int collectionId) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    // New items append to the end of the collection's manual order (max existing order + 1).
    query.prepare("INSERT INTO AssetCollectionItems (AssetCollectionItemPath, AssetCollectionItemCol, AssetCollectionItemSortOrder) "
                  "VALUES (:path, :id, (SELECT COALESCE(MAX(AssetCollectionItemSortOrder), -1) + 1 FROM AssetCollectionItems WHERE AssetCollectionItemCol = :id2))");
    query.bindValue(":path", filePath);
    query.bindValue(":id", collectionId);
    query.bindValue(":id2", collectionId);

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

/**
 * @brief Adds an asset file path to the flat Favorites list (no-op if already favorited).
 */
void AssetManagerWidget::addAssetToFavorites(const QString& filePath) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    // New favorites append to the end of the user's order (max existing order + 1).
    query.prepare("INSERT OR IGNORE INTO Favorites (FavoritePath, FavoriteSortOrder) "
                  "VALUES (:path, (SELECT COALESCE(MAX(FavoriteSortOrder), -1) + 1 FROM Favorites))");
    query.bindValue(":path", filePath);
    if (!query.exec())
        qWarning() << "Failed to add asset to favorites:" << query.lastError().text();
}

/**
 * @brief Removes an asset file path from the Favorites list.
 */
void AssetManagerWidget::removeAssetFromFavorites(const QString& filePath) {
    QSqlQuery query(QSqlDatabase::database("db_conn"));
    query.prepare("DELETE FROM Favorites WHERE FavoritePath = :path");
    query.bindValue(":path", filePath);
    if (!query.exec())
        qWarning() << "Failed to remove asset from favorites:" << query.lastError().text();
}

/**
 * @brief Starts a Favorites drag: floats a translucent ghost of the thumbnail that follows the
 *        cursor, suppresses the grid's hover highlight, and prepares the drop-line indicator.
 */
void AssetManagerWidget::beginGridDrag() {
    m_dragging = true;
    customToolTip->hide();
    if (!m_dragItem) return;

    const QPixmap icon = m_dragItem->icon().pixmap(
        QSize(Constants::THUMB_RENDER_SIZE, Constants::THUMB_RENDER_SIZE));
    if (!icon.isNull()) {
        QPixmap ghost(icon.size());
        ghost.fill(Qt::transparent);
        QPainter gp(&ghost);
        gp.setOpacity(0.75);
        gp.drawPixmap(0, 0, icon);
        gp.end();

        m_dragPreview = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        m_dragPreview->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dragPreview->setAttribute(Qt::WA_TranslucentBackground);
        m_dragPreview->setStyleSheet("background: transparent;");
        m_dragPreview->setPixmap(ghost);
        m_dragPreview->resize(ghost.size());
        m_dragPreview->show();
    }

    // Between-items drop indicator, parented to the grid's viewport.
    m_dropLine = new QWidget(assetListWidget->viewport());
    m_dropLine->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 1px;")
                                     .arg(Constants::COLOR_ACCENT_BLUE));
    m_dropLine->hide();

    // Switch the grid from hover-highlight to drop-line mode (see AssetGridDelegate::paint).
    assetListWidget->setProperty("gridDragging", true);
    assetListWidget->viewport()->update();
}

/**
 * @brief Tears down the floating drag ghost and drop line, and clears Favorites drag state.
 */
void AssetManagerWidget::endGridDrag() {
    if (m_scrollTimer) m_scrollTimer->stop();
    m_scrollDir = 0;
    if (m_dragPreview) {
        m_dragPreview->deleteLater();
        m_dragPreview = nullptr;
    }
    if (m_dropLine) {
        m_dropLine->deleteLater();
        m_dropLine = nullptr;
    }
    if (assetListWidget->property("gridDragging").toBool()) {
        assetListWidget->setProperty("gridDragging", false);
        assetListWidget->viewport()->update();
    }
    m_dragItem = nullptr;
    m_dragging = false;
}

/**
 * @brief Returns the insertion index (0..count) the cursor points at, walking items in reading
 *        order: an earlier row counts as "before"; within a row, the item's horizontal centre
 *        decides before/after.
 */
int AssetManagerWidget::gridInsertIndex(const QPoint& viewportPos) const {
    const int n = assetListWidget->count();
    for (int i = 0; i < n; ++i) {
        const QRect r = assetListWidget->visualItemRect(assetListWidget->item(i));
        bool before;
        if (viewportPos.y() < r.top())         before = true;
        else if (viewportPos.y() > r.bottom()) before = false;
        else                                   before = (viewportPos.x() < r.center().x());
        if (before) return i;
    }
    return n;
}

/**
 * @brief Positions the drop-line indicator at the gap the cursor points at.
 */
void AssetManagerWidget::updateGridDropIndicator(const QPoint& viewportPos) {
    if (!m_dropLine) return;
    const int n = assetListWidget->count();
    if (n == 0) { m_dropLine->hide(); return; }

    const int idx = gridInsertIndex(viewportPos);
    const int gap = assetListWidget->spacing();
    QRect r;
    int x;
    if (idx < n) {
        r = assetListWidget->visualItemRect(assetListWidget->item(idx));
        x = r.left() - gap / 2;          // gap before the item it will sit in front of
    } else {
        r = assetListWidget->visualItemRect(assetListWidget->item(n - 1));
        x = r.right() + gap / 2;         // after the last item
    }
    m_dropLine->setGeometry(x - 1, r.top(), 3, r.height());
    m_dropLine->show();
    m_dropLine->raise();
}

/**
 * @brief True for any view with a manual drag order — Favorites, or a Collection.
 */
bool AssetManagerWidget::isSortableView() const {
    return m_currentFolderPath == "FAVORITES_ROOT" || m_currentFolderPath.startsWith("COLLECTION_");
}

/**
 * @brief Persists the grid's current item order into FavoriteSortOrder or
 *        AssetCollectionItemSortOrder, whichever view is currently displayed, after a drag-reorder.
 */
void AssetManagerWidget::persistGridOrder() {
    QSqlDatabase db = QSqlDatabase::database("db_conn");
    db.transaction();
    QSqlQuery query(db);
    if (m_currentFolderPath == "FAVORITES_ROOT") {
        query.prepare("UPDATE Favorites SET FavoriteSortOrder = :ord WHERE FavoritePath = :path");
        for (int i = 0; i < assetListWidget->count(); ++i) {
            const QListWidgetItem* it = assetListWidget->item(i);
            query.bindValue(":ord", i);
            query.bindValue(":path", it->data(Qt::UserRole).toString());
            query.exec();
        }
    } else if (m_currentFolderPath.startsWith("COLLECTION_")) {
        const int collectionId = m_currentFolderPath.mid(11).toInt();
        query.prepare("UPDATE AssetCollectionItems SET AssetCollectionItemSortOrder = :ord "
                      "WHERE AssetCollectionItemPath = :path AND AssetCollectionItemCol = :id");
        for (int i = 0; i < assetListWidget->count(); ++i) {
            const QListWidgetItem* it = assetListWidget->item(i);
            query.bindValue(":ord", i);
            query.bindValue(":path", it->data(Qt::UserRole).toString());
            query.bindValue(":id", collectionId);
            query.exec();
        }
    }
    db.commit();
}

#include "assetmanagerwidget.moc"
