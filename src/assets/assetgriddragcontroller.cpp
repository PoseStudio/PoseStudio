// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetgriddragcontroller.cpp
 * @brief Implementation of AssetGridDragController. See the header for the design.
 */

#include "assetgriddragcontroller.h"
#include "assetnodeids.h"
#include "assettreeview.h"
#include "constants.h"

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QTimer>

AssetGridDragController::AssetGridDragController(QListWidget* grid, AssetTreeView* tree,
                                                 std::function<bool()> sortable, QObject* parent)
    : QObject(parent), m_grid(grid), m_tree(tree), m_sortable(std::move(sortable)) {
    // Edge auto-scroll while drag-reordering a sortable grid (Favorites or a Collection): a
    // repeating timer scrolls the grid (and re-places the drop line) whenever the cursor sits
    // past the top/bottom edge during a drag.
    m_scrollTimer = new QTimer(this);
    m_scrollTimer->setInterval(20);
    connect(m_scrollTimer, &QTimer::timeout, this, [this]() {
        if (m_scrollDir == 0) return;
        QScrollBar* sb = m_grid->verticalScrollBar();
        sb->setValue(sb->value() + m_scrollDir * 14);
        updateDropIndicator(m_dragLastPos);
    });
}

AssetGridDragController::~AssetGridDragController() {
    // The ghost is an unparented top-level window: nothing else owns it, so a controller that
    // dies mid-drag (widget teardown) must take it down itself, and synchronously (deleteLater
    // needs an event loop that may already be gone).
    if (m_scrollTimer) m_scrollTimer->stop();
    delete m_dragPreview.data();
    delete m_dropLine.data(); // (null already when the grid went first)
}

bool AssetGridDragController::handleMouseEvent(QMouseEvent* me) {
    // Two destinations share one hand-rolled gesture (we avoid Qt's QDrag/InternalMove path: in
    // IconMode it free-positions icons rather than reordering rows): while the cursor stays
    // inside a *sortable* grid (Favorites/Collections) it reorders rows; once it leaves the grid
    // (any view) it instead drops the asset onto a Collection/Favorites tree node, highlighting
    // that node like a node-move. Either way, past the drag threshold we float a translucent
    // ghost of the thumbnail under the cursor; in reorder mode a line marks the gap where it
    // will land. The grid is left untouched until release; moving items mid-drag would reflow
    // the layout under the cursor.
    switch (me->type()) {
    case QEvent::MouseButtonPress: {
        // ANY button pressed during a live drag ends it first: a right-click mid-drag otherwise
        // opens the context menu over a drag that never gets its left release (see the header).
        if (m_dragging) cancel();
        if (me->button() == Qt::LeftButton) {
            QListWidgetItem* it = m_grid->itemAt(me->pos());
            // Only asset items can be dragged: folder shortcuts are not assets.
            m_dragItem = (it && it->data(AssetGridRole::ItemKind).toString() != kFolderItemKind) ? it : nullptr;
            m_dragStartPos = me->pos();
            m_dragging = false;
            m_dropTargetNodeId.clear();
        }
        return false;
    }
    case QEvent::MouseButtonRelease: {
        if (me->button() != Qt::LeftButton) {
            // Only the button that started the drag may finish it: a mid-drag right-click
            // release would otherwise commit the drop at the right-click position AND leave
            // the context menu opening over the just-committed drop.
            return false;
        }
        if (m_dragging && m_dragItem) {
            if (!m_dropTargetNodeId.isEmpty()) {
                // Dropped onto a Collection/Favorites node: the owner adds (from a library/search
                // view) or moves (from another Favorites/Collection view) the asset there.
                emit droppedOnTree(m_dragItem, m_dropTargetNodeId);
            } else if (m_sortable() && m_grid->viewport()->rect().contains(me->pos())) {
                // Released inside a sortable grid: commit the reorder.
                const int fromRow = m_grid->row(m_dragItem);
                int insertIdx = insertIndexAt(me->pos());
                if (fromRow < insertIdx) --insertIdx; // removal shifts everything after fromRow up
                if (insertIdx != fromRow) {
                    QListWidgetItem* moved = m_grid->takeItem(fromRow);
                    m_grid->insertItem(insertIdx, moved);
                    m_grid->setCurrentItem(moved);
                }
                emit reorderCommitted();
            }
        }
        m_tree->clearAssetDropHighlight();
        endDrag();
        return false;
    }
    case QEvent::MouseMove: {
        // A move with the left button UP while a drag is live: its release never reached us (a
        // modal dialog opened over the drag, a popup's grab, the user switched windows). The
        // drag is over: left alone, the ghost stayed on screen, the grid's hover stayed off and
        // the edge auto-scroll ran on until the next press.
        if (m_dragging && !(me->buttons() & Qt::LeftButton)) {
            cancel();
            return false;
        }
        if (!((me->buttons() & Qt::LeftButton) && m_dragItem)) return false;
        if (!m_dragging &&
            (me->pos() - m_dragStartPos).manhattanLength() >= QApplication::startDragDistance()) {
            beginDrag();
        }
        if (!m_dragging) return false;

        m_dragLastPos = me->pos();
        if (m_dragPreview) {
            const QPoint g = me->globalPosition().toPoint();
            m_dragPreview->move(g - QPoint(m_dragPreview->width() / 2, m_dragPreview->height() / 2));
        }

        const bool insideGrid = m_grid->viewport()->rect().contains(me->pos());
        if (insideGrid && m_sortable()) {
            // Reorder mode: drop-line indicator + edge auto-scroll, no tree target.
            m_tree->clearAssetDropHighlight();
            m_dropTargetNodeId.clear();
            updateDropIndicator(me->pos());

            // Auto-scroll when the cursor is in (or past) the top/bottom edge zone.
            const int edge = 30;
            const int h = m_grid->viewport()->height();
            const int y = me->pos().y();
            m_scrollDir = (y < edge) ? -1 : (y > h - edge) ? 1 : 0;
            if (m_scrollDir != 0) {
                if (!m_scrollTimer->isActive()) m_scrollTimer->start();
            } else {
                m_scrollTimer->stop();
            }
        } else {
            // Outside the grid (or a non-sortable view): the reorder "move" is suspended
            // while out here; instead try to drop onto a Collection/Favorites tree node.
            if (m_dropLine) m_dropLine->hide();
            m_scrollTimer->stop();
            m_scrollDir = 0;
            m_dropTargetNodeId = m_tree->updateAssetDropHighlight(me->globalPosition().toPoint());
        }
        return true; // consume during an active drag (also suppresses tooltips)
    }
    default:
        return false;
    }
}

void AssetGridDragController::cancel() {
    m_tree->clearAssetDropHighlight();
    endDrag();
}

/**
 * @brief Starts a grid drag (sortable Favorites/Collection reorder, or a drop onto a tree node):
 *        floats a translucent ghost of the thumbnail that follows the cursor, suppresses the
 *        grid's hover highlight, and prepares the drop-line indicator.
 */
void AssetGridDragController::beginDrag() {
    m_dragging = true;
    emit dragStarted();
    if (!m_dragItem) return;

    // The thumbnails carry the grid's device pixel ratio; ask for the ghost at the same ratio so
    // it is as crisp as the cell it was lifted from.
    const qreal dpr = m_grid->devicePixelRatioF();
    const QPixmap icon = m_dragItem->icon().pixmap(
        QSize(Constants::GRID_ICON_DISPLAY_SIZE, Constants::GRID_ICON_DISPLAY_SIZE), dpr);
    if (!icon.isNull()) {
        QPixmap ghost(icon.size());
        ghost.setDevicePixelRatio(icon.devicePixelRatio());
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
        m_dragPreview->resize(ghost.deviceIndependentSize().toSize());
        m_dragPreview->show();
    }

    // Between-items drop indicator, parented to the grid's viewport.
    m_dropLine = new QWidget(m_grid->viewport());
    m_dropLine->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 1px;")
                                     .arg(Constants::COLOR_ACCENT_BLUE));
    m_dropLine->hide();

    // Switch the grid from hover-highlight to drop-line mode (see AssetGridDelegate::paint).
    m_grid->setProperty(kGridDraggingProperty, true);
    m_grid->viewport()->update();
}

void AssetGridDragController::endDrag() {
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
    if (m_grid->property(kGridDraggingProperty).toBool()) {
        m_grid->setProperty(kGridDraggingProperty, false);
        m_grid->viewport()->update();
    }
    m_dragItem = nullptr;
    m_dragging = false;
    m_dropTargetNodeId.clear();
}

/**
 * @brief Returns the insertion index (0..count) the cursor points at, in the grid's reading
 *        order: an earlier row counts as "before"; within a row, the item's horizontal centre
 *        decides before/after. Rows are uniform in height and items are laid out in index
 *        order, so the cursor's row is found by binary search on the item rects (O(log n))
 *        and only that row's items are inspected; the previous linear walk over every item's
 *        visualItemRect ran on every mouse move and every 20 ms auto-scroll tick.
 */
int AssetGridDragController::insertIndexAt(const QPoint& viewportPos) const {
    const int n = m_grid->count();
    if (n == 0) return 0;
    const auto rectOf = [this](int i) { return m_grid->visualItemRect(m_grid->item(i)); };

    // First item whose row bottom is at or below the cursor: the cursor's row, or the row
    // right after a gap the cursor sits in.
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (rectOf(mid).bottom() < viewportPos.y()) lo = mid + 1;
        else hi = mid;
    }
    if (lo == n) return n; // below every row

    const QRect first = rectOf(lo);
    if (viewportPos.y() < first.top()) return lo; // in the gap above this row: before its first item

    // Within the row: walk its items (same top) until one's centre is right of the cursor.
    int i = lo;
    for (; i < n; ++i) {
        const QRect r = rectOf(i);
        if (r.top() != first.top()) break;
        if (viewportPos.x() < r.center().x()) return i;
    }
    return i; // after the row's last item
}

void AssetGridDragController::updateDropIndicator(const QPoint& viewportPos) {
    if (!m_dropLine) return;
    const int n = m_grid->count();
    if (n == 0) { m_dropLine->hide(); return; }

    const int idx = insertIndexAt(viewportPos);
    const int gap = m_grid->spacing();
    QRect r;
    int x;
    if (idx < n) {
        r = m_grid->visualItemRect(m_grid->item(idx));
        x = r.left() - gap / 2;          // gap before the item it will sit in front of
    } else {
        r = m_grid->visualItemRect(m_grid->item(n - 1));
        x = r.right() + gap / 2;         // after the last item
    }
    m_dropLine->setGeometry(x - 1, r.top(), 3, r.height());
    m_dropLine->show();
    m_dropLine->raise();
}
