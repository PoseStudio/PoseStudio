// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file customtooltip.h
 * @brief An interactive, floating label that replaces Qt's native tooltips in the asset grid.
 *
 * Qt's built-in tooltip vanishes the instant the cursor moves and can't be styled as a rounded,
 * translucent panel. The asset grid wants a rich-text card (type, size, modified date, folder)
 * that stays up while the cursor moves within the owning item and only fades after a short
 * grace period once it leaves, so this is a top-level Qt::ToolTip window with its own hide
 * timer, driven by AssetManagerWidget's event filter on the grid viewport (it decides WHEN to
 * show/hide; this class only knows HOW).
 */

#ifndef CUSTOMTOOLTIP_H
#define CUSTOMTOOLTIP_H

#include <QWidget>

class QEnterEvent;
class QLabel;
class QTimer;

/**
 * @class CustomToolTip
 * @brief An interactive, floating label that replaces standard Qt tooltips.
 */
class CustomToolTip : public QWidget {
    Q_OBJECT
public:
    explicit CustomToolTip(QWidget* parent = nullptr);

    /// Sets the tooltip's rich-text content.
    void setText(const QString& text);

    void startHideTimer(int ms);
    void stopHideTimer();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QTimer* hideTimer;
    QLabel* textLabel;
};

#endif // CUSTOMTOOLTIP_H
