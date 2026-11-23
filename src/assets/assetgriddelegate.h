// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetgriddelegate.h
 * @brief The item delegate that paints the Asset Manager's thumbnail grid.
 *
 * Like the tree delegate it takes over Qt's item painting entirely: the style engine draws the
 * selection/hover box, then the delegate centres the thumbnail and word-wraps the label to two
 * lines (eliding the second). Its sizeHint is THE cell size Qt lays the grid out with — a
 * custom delegate's sizeHint wins over QListView::setGridSize — and both sizeHint and paint read
 * the same Constants::GRID_* geometry so the two can never drift apart. During a hand-rolled
 * grid drag it also suppresses the hover highlight (the kGridDraggingProperty flag) so the
 * between-items drop line is the only drop feedback.
 */

#ifndef ASSETGRIDDELEGATE_H
#define ASSETGRIDDELEGATE_H

#include <QStyledItemDelegate>

/**
 * @class AssetGridDelegate
 * @brief Custom delegate for the asset grid that word-wraps labels to 2 lines and elides on line 2.
 */
class AssetGridDelegate : public QStyledItemDelegate {
public:
    explicit AssetGridDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // ASSETGRIDDELEGATE_H
