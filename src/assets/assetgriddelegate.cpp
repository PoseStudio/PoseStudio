// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetgriddelegate.cpp
 * @brief Implementation of AssetGridDelegate. See the header for what it is for.
 */

#include "assetgriddelegate.h"
#include "assetnodeids.h"
#include "constants.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QStyle>
#include <QWidget>

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
    if (widget && widget->property(kGridDraggingProperty).toBool())
        opt.state &= ~QStyle::State_MouseOver;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    // Hard backstop: never let icon/text bleed past the cell's selection box,
    // regardless of how tall the actual font metrics turn out to be at runtime.
    painter->setClipRect(opt.rect);

    // Icon: centred horizontally, small top margin
    const int iconSz = Constants::GRID_ICON_DISPLAY_SIZE;

    if (!icon.isNull()) {
        QRect ir(opt.rect.left() + (opt.rect.width() - iconSz) / 2,
                 opt.rect.top() + Constants::GRID_ICON_TOP_MARGIN,
                 iconSz, iconSz);
        icon.paint(painter, ir, Qt::AlignCenter,
                   (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal);
    }

    // Text: up to 2 word-wrapped lines; line 2 is elided with "..." if overflow.
    // Anchored right after the icon + gap (the same Constants::GRID_* geometry sizeHint() uses
    // to size the cell), clamped so it can never get pushed past the cell bottom and clipped.
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
                // First word alone is wider than the cell: just elide the whole string
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
    Q_UNUSED(index);
    // This is the value Qt's QListView actually uses to lay out each cell:
    // setGridSize() alone does not control row height when a custom delegate is installed.
    // The "2 *" here must match the text block height paint() (above) computes, otherwise
    // cells get sized too short and paint()'s overflow clamp kicks in on every 2-line label.
    const QFontMetrics fm(option.font);
    const int textBlockH = 2 * fm.height() + 2;
    const int height = Constants::GRID_ICON_TOP_MARGIN + Constants::GRID_ICON_DISPLAY_SIZE
                      + Constants::GRID_ICON_TEXT_GAP + textBlockH
                      + Constants::GRID_TEXT_BOTTOM_MARGIN;
    return QSize(Constants::GRID_CELL_WIDTH, height);
}
