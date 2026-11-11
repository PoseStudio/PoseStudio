/**
 * @file assettreeview.h
 * @brief The Asset Manager's directory/collections tree view.
 *
 * A QTreeView with three additions the stock view can't provide: (1) a `separatorColor`
 * Q_PROPERTY so _assetmanager.qss can drive the color AssetTreeDelegate paints separator rows
 * with (QSS can't reach C++ painting any other way); (2) its own single drop-target highlight,
 * painted over exactly the node a drag may land on, because Qt's built-in drop indicator (lines
 * between rows plus stray edge rectangles) reads as confusing ghost highlights mid-drag, so
 * setupUI turns it off; and (3) spring-loaded expansion: a drag dwelling over a collapsed
 * Collections node auto-expands it so the user can drill toward a sub-collection without
 * dropping. Two gestures share the highlight: Qt's own QDrag for collection-reparenting (the
 * dragMove/dragLeave/drop overrides) and the Asset Manager's hand-rolled grid drag, which has no
 * QDrag and feeds cursor positions through updateAssetDropHighlight() instead.
 */

#ifndef ASSETTREEVIEW_H
#define ASSETTREEVIEW_H

#include <QColor>
#include <QPersistentModelIndex>
#include <QTreeView>

class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QPaintEvent;
class QTimer;

/**
 * @class AssetTreeView
 * @brief A customized QTreeView that exposes custom properties to the Qt Style Engine and owns
 *        the drop-target highlight + spring-loaded expansion for both drag gestures.
 * @details Allows the C++ delegate to pull variables directly from external .qss files.
 */
class AssetTreeView : public QTreeView {
    Q_OBJECT
    Q_PROPERTY(QColor separatorColor READ separatorColor WRITE setSeparatorColor)

public:
    explicit AssetTreeView(QWidget *parent = nullptr);

    QColor separatorColor() const { return m_separatorColor; }
    void setSeparatorColor(const QColor &color);

    // --- Asset drag-onto-node highlight: driven by AssetManagerWidget's hand-rolled grid drag
    // (not a Qt QDrag), so these reuse the same single drop-target highlight the collection-reparent
    // drag paints. Given a global cursor position they light up the Collection/Favorites node under
    // it (or clear if none), reusing AssetTreeView's paintEvent highlight for visual parity with the
    // node-move gesture.
    /// Highlights the droppable Collection/Favorites node at globalPos and returns its UserRole
    /// id-string ("FAVORITES_ROOT" or "COLLECTION_<id>"), or an empty string if there's no valid
    /// target under the cursor.
    QString updateAssetDropHighlight(const QPoint &globalPos);
    void clearAssetDropHighlight();

protected:
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;

    // Qt's built-in drop indicator (between-items lines + stray edge rects) reads as confusing
    // ghost highlights mid-drag. It's disabled in setupUI; instead we track the valid drop target
    // under the cursor and paint a single, definitive highlight over just that node.
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    /// Paints the view, then the one rounded accent-blue highlight over m_dropTarget (if any).
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_separatorColor;
    QPersistentModelIndex m_dropTarget; ///< Drop-enabled node currently under the cursor during a drag

    // Spring-loaded expansion: while an asset is hovered over a collapsed node with children, a
    // short dwell auto-expands it so the user can drill toward a child node without dropping.
    QTimer *m_autoExpandTimer = nullptr;
    QPersistentModelIndex m_autoExpandTarget; ///< Collapsed node the dwell timer is currently counting down for
    void scheduleAutoExpand(const QModelIndex &index); ///< (re)arms or cancels the dwell timer for `index`
};

#endif // ASSETTREEVIEW_H
