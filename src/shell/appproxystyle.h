/**
 * @file appproxystyle.h
 * @brief Application-wide proxy style layered over Fusion.
 *
 * Intercepts a few style hints/metrics globally so they apply everywhere without per-widget
 * code: custom tooltip wake/sleep delays, a tighter submenu overlap, and 30%-opacity disabled
 * icons. Installed once in main.cpp via QApplication::setStyle(new AppProxyStyle("Fusion")).
 */

#ifndef APPPROXYSTYLE_H
#define APPPROXYSTYLE_H

#include <QProxyStyle>
#include <QStyle>
#include <QStyleOption>
#include <QPainter>
#include <QPixmap>
#include <QIcon>

#include "constants.h"

/**
 * @class AppProxyStyle
 * @brief Intercepts OS-level styling requests to force custom UI behaviors.
 */
class AppProxyStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override {

        if (hint == QStyle::SH_ToolTip_WakeUpDelay) return Constants::TOOLTIP_WAKE_DELAY_MS;
        if (hint == QStyle::SH_ToolTip_FallAsleepDelay) return Constants::TOOLTIP_SLEEP_DELAY_MS;

        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override {
        if (metric == QStyle::PM_SubMenuOverlap) return -4;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    // Globally force all disabled icons to 30% opacity.
    QPixmap generatedIconPixmap(QIcon::Mode iconMode, const QPixmap &pixmap, const QStyleOption *opt) const override {
        if (iconMode == QIcon::Disabled) {
            // Paint the original icon onto a blank, transparent canvas at exactly 30% opacity.
            QPixmap transparentPixmap(pixmap.size());
            transparentPixmap.fill(Qt::transparent);

            QPainter painter(&transparentPixmap);
            painter.setOpacity(0.3);
            painter.drawPixmap(0, 0, pixmap);
            painter.end();

            return transparentPixmap;
        }

        // Pass all other normal/active icons back to standard Qt behavior.
        return QProxyStyle::generatedIconPixmap(iconMode, pixmap, opt);
    }
};

#endif // APPPROXYSTYLE_H
