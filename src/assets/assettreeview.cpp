// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assettreeview.cpp
 * @brief Implementation of AssetTreeView. See the header for what it is for.
 */

#include "assettreeview.h"
#include "assetnodeids.h"
#include "constants.h"

#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

AssetTreeView::AssetTreeView(QWidget *parent)
    : QTreeView(parent), m_separatorColor(Constants::COLOR_SEPARATOR) {}

void AssetTreeView::setSeparatorColor(const QColor &color) {
    m_separatorColor = color;
    viewport()->update();
}

void AssetTreeView::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const {
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

// =============================================================================
// [ CUSTOM DROP-TARGET HIGHLIGHT ]
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
    // Resolve the target from the cursor and hand it to the model directly: don't defer to
    // QTreeView::dropEvent, which (with the drop indicator disabled) collapses the target to the
    // viewport root and gets rejected. dropMimeData validates and emits the reparent (queued).
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (idx.isValid() && model())
        model()->dropMimeData(event->mimeData(), Qt::MoveAction, -1, -1, idx);

    m_dropTarget = QModelIndex();
    viewport()->update();
    event->ignore(); // reparent performed ourselves; prevent Qt's InternalMove source-row removal
}

QString AssetTreeView::updateAssetDropHighlight(const QPoint &globalPos) {
    const QPoint vp = viewport()->mapFromGlobal(globalPos);
    QModelIndex target;
    QString result;

    if (viewport()->rect().contains(vp)) {
        const QModelIndex idx = indexAt(vp);
        if (idx.isValid()) {
            // A Collection node, the Collections header (drops there spawn a new top-level
            // collection, same as "New Collection" from the asset context menu) or the Favorites
            // root can receive an asset. Search Results, separators and physical folders never do.
            const QString role = idx.data(Qt::UserRole).toString();
            if (role == AssetNode::FAVORITES_ROOT || role == AssetNode::COLLECTIONS_ROOT
                || AssetNode::isCollection(role)) {
                target = idx;
                result = role;
            }
        }
    }

    if (QModelIndex(m_dropTarget) != target) {
        m_dropTarget = target;
        viewport()->update();
    }

    // Spring-load: dwell over a collapsed node (any expandable collection-tree node, including the
    // Collections header, not just valid drop targets) auto-expands it so the user can reach a
    // child. The node under the cursor drives this even when it isn't itself a droppable target.
    const QModelIndex hovered = viewport()->rect().contains(vp) ? indexAt(vp) : QModelIndex();
    scheduleAutoExpand(hovered);

    return result;
}

void AssetTreeView::scheduleAutoExpand(const QModelIndex &index) {
    // Spring-loading is scoped to the Collections subtree (where sub-collections, the only nodes
    // worth drilling toward for an asset drop, live) plus the Collections header. Physical library
    // folders, Search Results and Favorites are never asset destinations, so auto-expanding them
    // mid-drag would just churn the tree (and needlessly lazy-load folder children).
    const QString role = index.isValid() ? index.data(Qt::UserRole).toString() : QString();
    const bool inCollectionsTree = (role == AssetNode::COLLECTIONS_ROOT || AssetNode::isCollection(role));
    const bool expandable = inCollectionsTree && model() && model()->hasChildren(index)
                            && !isExpanded(index);

    if (!expandable) {
        if (m_autoExpandTimer) m_autoExpandTimer->stop();
        m_autoExpandTarget = QModelIndex();
        return;
    }

    if (QModelIndex(m_autoExpandTarget) == index)
        return; // already counting down for this node; let the dwell continue

    m_autoExpandTarget = index;
    if (!m_autoExpandTimer) {
        m_autoExpandTimer = new QTimer(this);
        m_autoExpandTimer->setSingleShot(true);
        connect(m_autoExpandTimer, &QTimer::timeout, this, [this]() {
            const QModelIndex idx = m_autoExpandTarget;
            if (idx.isValid() && !isExpanded(idx)) {
                expand(idx);
                scrollTo(idx); // keep the freshly revealed children in view
            }
            m_autoExpandTarget = QModelIndex();
        });
    }
    m_autoExpandTimer->start(Constants::DRAG_AUTO_EXPAND_HOVER_MS);
}

void AssetTreeView::clearAssetDropHighlight() {
    if (m_autoExpandTimer) m_autoExpandTimer->stop();
    m_autoExpandTarget = QModelIndex();
    if (m_dropTarget.isValid()) {
        m_dropTarget = QModelIndex();
        viewport()->update();
    }
}

// Paints the view normally, then one rounded, translucent accent-blue box over the current drop
// target. COLOR_ACCENT_BLUE (not the app accent) because the highlight has to read over the
// selection fill, which the softer accent doesn't.
void AssetTreeView::paintEvent(QPaintEvent *event) {
    QTreeView::paintEvent(event);
    if (!m_dropTarget.isValid()) return;

    const QRect r = visualRect(m_dropTarget);
    if (!r.isValid()) return;

    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    QColor accent(Constants::COLOR_ACCENT_BLUE);
    QColor fill = accent;
    fill.setAlpha(55);
    p.setPen(QPen(accent, 1));
    p.setBrush(fill);
    p.drawRoundedRect(r.adjusted(1, 1, -2, -2), 3, 3);
}
