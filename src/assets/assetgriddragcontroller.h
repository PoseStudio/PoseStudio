// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetgriddragcontroller.h
 * @brief The hand-rolled asset drag out of the thumbnail grid: reorder within a sortable view,
 *        or drop onto a Collection/Favorites tree node.
 *
 * This is deliberately NOT Qt's QDrag/InternalMove: in IconMode, QListWidget's internal move
 * free-positions the icon instead of reordering rows, and QDrag runs through the OS drag loop.
 * The controller tracks raw mouse events from the grid viewport (AssetManagerWidget's event
 * filter forwards them to handleMouseEvent), floats a translucent ghost QLabel of the thumbnail
 * under the cursor, shows a between-items drop-line while the cursor is inside a SORTABLE grid
 * (Favorites/Collection), or lights the Collection/Favorites tree node under the cursor once it
 * leaves the grid, and auto-scrolls via a QTimer when the cursor sits in the top/bottom edge
 * zone. The grid is left untouched until release (moving items mid-drag would reflow the
 * layout under the cursor); on release it either reorders the rows and asks the owner to
 * persist the order, or hands the item + target node to the owner. The kGridDraggingProperty
 * flag tells AssetGridDelegate to suppress the hover highlight while a drag is live.
 *
 * Any button press while a drag is live cancels it first. Windows opens the context menu on
 * the right button's RELEASE, so a right-click mid-drag used to leave the drag stuck: the
 * left release was swallowed by the menu's grab, the (unparented, top-level) ghost stayed on
 * screen for the session and the grid never hover-highlighted again. A release can be lost in
 * other ways too (a modal dialog opening over the drag, a switch to another window), so a
 * mouse move that arrives with the left button up while a drag is live cancels it as well.
 */

#ifndef ASSETGRIDDRAGCONTROLLER_H
#define ASSETGRIDDRAGCONTROLLER_H

#include <QLabel>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <functional>

class AssetTreeView;
class QListWidget;
class QListWidgetItem;
class QMouseEvent;
class QTimer;

/**
 * @class AssetGridDragController
 * @brief Owns the state and visuals of a grid drag; reports its outcome through signals.
 */
class AssetGridDragController : public QObject {
    Q_OBJECT
public:
    /**
     * @param grid     The asset grid whose viewport's mouse events are forwarded here.
     * @param tree     The tree that highlights the drop-target node while the cursor is over it.
     * @param sortable Answers "does the CURRENT view keep a manual order?" (Favorites or a
     *                 Collection): only then does a drag inside the grid reorder rows.
     */
    AssetGridDragController(QListWidget* grid, AssetTreeView* tree,
                            std::function<bool()> sortable, QObject* parent = nullptr);
    ~AssetGridDragController() override;

    /// Feed a MouseButtonPress / MouseMove / MouseButtonRelease from the grid's viewport.
    /// Returns true when the event was consumed (a move during a live drag): the caller must
    /// then skip its own handling (tooltips) for that event.
    bool handleMouseEvent(QMouseEvent* event);

    /// True from the moment the cursor passed the drag threshold until release/cancel.
    bool dragging() const { return m_dragging; }

    /// Ends a live drag without committing anything (tears down the ghost, drop line, tree
    /// highlight, and the gridDragging flag). Safe to call when no drag is live.
    void cancel();

signals:
    /// The cursor passed the drag threshold: the drag is now live (the owner hides its tooltip).
    void dragStarted();
    /// The rows were reordered in place; the owner persists the new order.
    void reorderCommitted();
    /// The item was released over a Collection/Favorites tree node (`targetNodeId` is its
    /// UserRole id, see assetnodeids.h); the owner files the asset there.
    void droppedOnTree(QListWidgetItem* item, const QString& targetNodeId);

private:
    void beginDrag();               ///< Floats the ghost thumbnail + drop line, sets the grid flag.
    void endDrag();                 ///< Tears down the ghost/drop line and clears the drag state.
    int  insertIndexAt(const QPoint& viewportPos) const; ///< Insertion index (0..count) the cursor points at.
    void updateDropIndicator(const QPoint& viewportPos); ///< Positions the drop line at the gap the cursor points at.

    QListWidget* m_grid;
    AssetTreeView* m_tree;
    std::function<bool()> m_sortable;

    QListWidgetItem* m_dragItem = nullptr;
    QPoint m_dragStartPos;
    bool m_dragging = false;
    // Guarded pointers: the drop line is a child of the grid's viewport, and at teardown the
    // grid dies BEFORE this controller (the widget's children go in creation order), taking the
    // line with it; the destructor must not delete it a second time.
    QPointer<QLabel> m_dragPreview;   ///< Floating ghost thumbnail that follows the cursor mid-drag (top-level, unparented)
    QPointer<QWidget> m_dropLine;     ///< Vertical line marking where the dragged item will drop (a child of the grid's viewport)
    QTimer* m_scrollTimer = nullptr;  ///< Drives edge auto-scroll while reordering
    int m_scrollDir = 0;              ///< -1 = scroll up, +1 = scroll down, 0 = idle
    QPoint m_dragLastPos;             ///< Last cursor pos (viewport coords) during a drag

    /// Tree node ("FAVORITES_ROOT"/"COLLECTIONS_ROOT"/"COLLECTION_<id>") currently highlighted as
    /// the drop target while the asset is over the tree; empty when there's no valid target.
    QString m_dropTargetNodeId;
};

#endif // ASSETGRIDDRAGCONTROLLER_H
