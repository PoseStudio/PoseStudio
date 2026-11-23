// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file customtooltip.cpp
 * @brief Implementation of CustomToolTip. See the header for what it is for.
 */

#include "customtooltip.h"
#include "constants.h"

#include <QEnterEvent>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

CustomToolTip::CustomToolTip(QWidget* parent) : QWidget(parent, Qt::ToolTip) {
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating);

    // The outer top-level widget is a transparent container; the styled label inside does the
    // actual drawing. Rendering the rounded/bordered CSS on a child rather than directly on a
    // top-level translucent window avoids the rectangular-background artifacts Qt otherwise
    // leaves around the rounded corners.
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);

    // Zero margins so the label fills the whole container.
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

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

void CustomToolTip::setText(const QString& text) {
    textLabel->setText(text);
}

void CustomToolTip::startHideTimer(int ms) { hideTimer->start(ms); }
void CustomToolTip::stopHideTimer() { hideTimer->stop(); }

void CustomToolTip::enterEvent(QEnterEvent* event) {
    stopHideTimer();
    QWidget::enterEvent(event);
}

void CustomToolTip::leaveEvent(QEvent* event) {
    startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
    QWidget::leaveEvent(event);
}
