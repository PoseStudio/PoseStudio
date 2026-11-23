// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file propertyrows.cpp
 * @brief Implementation of the shared properties-tab builders. See propertyrows.h.
 */

#include "propertyrows.h"

#include "dragnumberbox.h"

#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace pose::propertyrows {

ScrollColumn makeScrollColumn(QWidget* panel, const QString& contentName, int spacing) {
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(0, 0, 0, 0);

    ScrollColumn out;
    out.scroll = new QScrollArea(panel);
    out.scroll->setWidgetResizable(true);
    out.scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(out.scroll);

    out.content = new QWidget;
    out.content->setObjectName(contentName);
    out.column = new QVBoxLayout(out.content);
    out.column->setContentsMargins(12, 12, 12, 12);
    out.column->setSpacing(spacing);
    return out;
}

void ScrollColumn::finish() const {
    scroll->setWidget(content);
}

QFormLayout* addGroup(QVBoxLayout* parent, const QString& title) {
    auto* box = new QGroupBox(title);
    box->setObjectName(QStringLiteral("PropertyGroup"));
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    parent->addWidget(box);
    return form;
}

CollapsibleGroup addCollapsibleGroup(QVBoxLayout* parent, const QString& title, bool expanded) {
    auto* box = new QFrame;
    box->setObjectName(QStringLiteral("PropertyCollapsible"));
    auto* col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* header = new QToolButton;
    header->setObjectName(QStringLiteral("PropertyCollapsibleHeader"));
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(expanded);
    header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setAutoRaise(true);
    header->setCursor(Qt::PointingHandCursor);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    col->addWidget(header);

    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("PropertyCollapsibleBody"));
    auto* form = new QFormLayout(body);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    form->setContentsMargins(8, 8, 8, 8);
    body->setVisible(expanded);
    col->addWidget(body);

    QObject::connect(header, &QToolButton::toggled, body, [header, body](bool on) {
        body->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    parent->addWidget(box);

    CollapsibleGroup group;
    group.form = form;
    group.header = header;
    group.body = body;
    group.box = box;
    group.column = col;
    return group;
}

QToolButton* makeRestoreButton() {
    auto* restore = new QToolButton;
    restore->setObjectName(QStringLiteral("PropertyRowRestore"));
    restore->setIcon(QIcon(QStringLiteral(":/resources/icons/default.png")));
    restore->setIconSize(QSize(14, 14));
    restore->setToolTip(QObject::tr("Restore default"));
    restore->setCursor(Qt::PointingHandCursor);
    restore->setAutoRaise(true);
    return restore;
}

QWidget* rowWithRestore(QWidget* main, QToolButton* restore) {
    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(main, 1);
    row->addWidget(restore, 0);
    return field;
}

ScrubRow makeScrubRow(const QString& label, const QString& tip, double min, double max, double step, int decimals) {
    ScrubRow row;
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.box = new DragNumberBox;
    row.box->setRange(min, max);
    row.box->setSingleStep(step);
    row.box->setDecimals(decimals);
    row.box->setToolTip(tip);
    row.restore = makeRestoreButton();
    row.field = rowWithRestore(row.box, row.restore);
    return row;
}

void stylePickerField(QPushButton* button) {
    button->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    button->setCursor(Qt::PointingHandCursor);
    // (Icon + icon/text spacing come to about a 16 px indent of the caption.)
    QPixmap spacer(10, 10);
    spacer.fill(Qt::transparent);
    button->setIcon(QIcon(spacer));
    button->setIconSize(QSize(10, 10));
}

void alignLabelColumn(const std::vector<QLabel*>& labels) {
    int widest = 0;
    for (QLabel* label : labels) {
        if (label == nullptr) {
            continue;
        }
        label->ensurePolished();
        widest = std::max(widest, label->fontMetrics().horizontalAdvance(label->text()) + 2);
    }
    for (QLabel* label : labels) {
        if (label != nullptr) {
            label->setMinimumWidth(widest);
        }
    }
}

} // namespace pose::propertyrows
