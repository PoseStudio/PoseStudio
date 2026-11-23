// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assettreedelegate.cpp
 * @brief Implementation of AssetTreeDelegate. See the header for what it is for.
 */

#include "assettreedelegate.h"
#include "assetnodeids.h"
#include "constants.h"

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QStyle>
#include <QWidget>

namespace {
// The row's icon edge (the view's decoration size, 16 when unset) and the gap between icon and
// text. The inline rename editor is positioned over the painted text, so both functions must
// agree on the same offset; computing it here keeps them from drifting apart.
int rowIconSize(const QStyleOptionViewItem& option) {
    return option.decorationSize.width() > 0 ? option.decorationSize.width() : 16;
}
constexpr int kIconTextGap = 6;
}

QSize AssetTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    // Visual separators get a fixed slim height rather than a full folder row
    if (index.data(Qt::UserRole).toString() == AssetNode::SEPARATOR) {
        return QSize(option.rect.width(), 12);
    }
    return QStyledItemDelegate::sizeHint(option, index);
}

void AssetTreeDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    Q_UNUSED(index);
    // Shift the inline QLineEdit to perfectly overlay our custom-painted text geometry
    const int textOffset = rowIconSize(option) + kIconTextGap;

    QRect editRect = option.rect;
    editRect.setLeft(option.rect.left() + textOffset - 2); // -2px compensates for native line-edit margins
    editor->setGeometry(editRect);
}

void AssetTreeDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const QString path = index.data(Qt::UserRole).toString();

    if (path == AssetNode::SEPARATOR) {
        painter->save();
        const int y = option.rect.center().y();

        QColor sepColor(Constants::COLOR_SEPARATOR); // Fallback if the .qss property below isn't set
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
        const int iconSize = rowIconSize(opt);
        QRect iconRect = opt.rect;
        iconRect.setWidth(iconSize);

        const QIcon::Mode mode = (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
        folderIcon.paint(painter, iconRect, Qt::AlignLeft | Qt::AlignVCenter, mode, QIcon::Off);

        textOffset = iconSize + kIconTextGap;
    }

    QRect textRect = opt.rect;
    textRect.adjust(textOffset, 0, 0, 0);

    if (opt.state & QStyle::State_Selected) {
        painter->setPen(Qt::white);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, folderText);
    } else {
        // Search result path: grey the ancestor prefix, bright the matched folder name. Gated on
        // the row's role, not on the text: a collection named "Sci-Fi / Props" is one name.
        const int lastSep = index.data(AssetNode::SearchResult).toBool() ? folderText.lastIndexOf(" / ") : -1;
        if (lastSep >= 0) {
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
