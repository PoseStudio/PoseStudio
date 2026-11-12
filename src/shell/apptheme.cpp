/**
 * @file apptheme.cpp
 * @brief Implementation of AppTheme::install — the app's theming, moved out of main.cpp.
 */

#include "apptheme.h"
#include "appproxystyle.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QIcon>
#include <QMenu>
#include <QPainterPath>
#include <QRegion>
#include <QString>
#include <QStringList>

namespace {
/**
 * @brief Strips the heavy native drop shadow from every popup menu, application-wide.
 *
 * Rounded menus (the border-radius in _menumanager.qss) are *translucent* popup windows, which Windows
 * gives a heavy native drop shadow. Setting Qt::NoDropShadowWindowHint on each menu removes that shadow
 * while keeping the rounded corners. It's applied on the menu's Polish event — before its native window
 * is created, so the shadow never appears — which covers menu-bar dropdowns, their submenus, context
 * menus, and the viewport's shader menu alike, from one place.
 *
 * Installed on the QApplication, so it sees EVERY event in the process: the cheap type test runs
 * first and the qobject_cast only for the two event types it acts on.
 */
class MenuShadowFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type type = event->type();
        if (type != QEvent::Polish && type != QEvent::Resize) {
            return QObject::eventFilter(watched, event);
        }
        auto* menu = qobject_cast<QMenu*>(watched);
        if (!menu) {
            return QObject::eventFilter(watched, event);
        }
        if (type == QEvent::Polish) {
            menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
            menu->setAttribute(Qt::WA_TranslucentBackground, true);
        } else {
            // NoDropShadowWindowHint leaves a 2-3px opaque black speck at each sharp outer corner (the
            // triangle outside the border-radius won't clear to transparent). Clip the popup to its
            // rounded-rect shape so those specks are cut away — the mask radius matches the 4px QSS
            // border-radius, so it removes only the corner triangles and not the visible border.
            QPainterPath path;
            path.addRoundedRect(QRectF(menu->rect()), 4, 4);
            menu->setMask(QRegion(path.toFillPolygon().toPolygon()));
        }
        return QObject::eventFilter(watched, event);
    }
};

/**
 * @brief Concatenates the app's QSS modules into one stylesheet, in cascade order
 *        (later files can override rules from earlier ones).
 */
QString loadStylesheets() {
    QString combinedStyles;

    const QStringList filesToLoad = {
        QStringLiteral(":/resources/styles/global.qss"),
        QStringLiteral(":/resources/styles/_assetmanager.qss"),
        QStringLiteral(":/resources/styles/_menumanager.qss"),
        QStringLiteral(":/resources/styles/_preferences.qss"),
        QStringLiteral(":/resources/styles/_environment.qss"),
        QStringLiteral(":/resources/styles/_help.qss"),
        QStringLiteral(":/resources/styles/_render.qss")
    };

    for (const QString& filePath : filesToLoad) {
        QFile file(filePath);
        if (file.open(QFile::ReadOnly | QFile::Text)) {
            combinedStyles.append(file.readAll()).append(QLatin1Char('\n'));
        } else {
            qWarning() << "[!] Failed to load stylesheet module:" << filePath;
        }
    }
    return combinedStyles;
}
} // namespace

void AppTheme::install(QApplication& app) {
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/icon.png")));

    // Disable the OS-level slide/fade animations for all tooltips (the asset grid uses a
    // custom interactive tooltip and expects instant show/hide).
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);

    // AppProxyStyle layered over Fusion: forces the custom dark theme to render identically on
    // every platform (never the native OS style), and adds app-wide tweaks (tooltip delays,
    // submenu overlap, dimmed disabled icons). The proxy takes ownership of the Fusion base.
    // "Fusion" must be passed explicitly — a bare AppProxyStyle() would wrap the platform
    // DEFAULT style instead, silently defeating the point.
    app.setStyle(new AppProxyStyle(QStringLiteral("Fusion")));
    app.setStyleSheet(loadStylesheets());

    // Strip the native drop shadow from every popup menu app-wide, keeping their rounded corners
    // (see MenuShadowFilter). Parented to the app so it lives for the whole session.
    app.installEventFilter(new MenuShadowFilter(&app));
}
