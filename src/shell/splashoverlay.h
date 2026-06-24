/**
 * @file splashoverlay.h
 * @brief A full-window, click-to-dismiss branding overlay (boot splash and "About" screen).
 */

#ifndef SPLASHOVERLAY_H
#define SPLASHOVERLAY_H

#include <QWidget>
#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QShowEvent>
#include <QEvent>
#include <QPixmap>
#include <QGraphicsDropShadowEffect>
#include <QColor>
#include <QRect>
#include <QPoint>
#include "constants.h"

/**
 * @class SplashOverlay
 * @brief Covers its parent window with the PoseStudio branding image until clicked.
 *
 * Tracks the parent's geometry via an event filter so it always fills the window, and
 * deletes itself on the next click anywhere within it.
 *
 * It is a frameless, always-on-top, translucent *top-level window* (not a child widget)
 * deliberately: the 3D viewport is hosted in a native child window
 * (QWidget::createWindowContainer), and native windows are composited on top of ordinary
 * overlay widgets — a child-widget overlay would be hidden behind the viewport. A top-level
 * window owned by the parent is composited above the viewport surface instead.
 */
class SplashOverlay : public QWidget {
public:
    /// @param parent The window this overlay should cover (usually the QMainWindow).
    SplashOverlay(QWidget *parent)
        : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        // Qt::Window (the window *type*, not just the hints) is what actually promotes this
        // parented widget into a top-level window — without it the hints are ignored and it
        // stays a child widget hidden behind the native viewport.
        setAttribute(Qt::WA_StyledBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true); // see-through except the artwork
        setStyleSheet("background-color: rgba(0, 0, 0, 0);");

        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *imageLabel = new QLabel(this);

        constexpr int targetWidth = 740;
        constexpr int targetHeight = 555;

        // Downscale from the source resolution here (once) rather than letting Qt
        // stretch the raw pixmap at paint time, which would look blurry.
        QPixmap originalPixmap(":/resources/splash_screen.png");
        QPixmap scaledPixmap = originalPixmap.scaled(
            targetWidth, targetHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        imageLabel->setPixmap(scaledPixmap);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setStyleSheet("background-color: transparent;");
        // Fixed-size + centered (rather than letting the layout stretch it) so the pixmap's
        // top-left corner is always at a known offset within imageLabel — required for the
        // version label below to be positioned in the artwork's own coordinate space.
        imageLabel->setFixedSize(scaledPixmap.size());
        layout->addWidget(imageLabel, 0, Qt::AlignCenter);

        // Live version number, overlaid just to the left of "BLUEPRINT" baked into the
        // artwork, so it doesn't need to be re-baked into the image on every release.
        // Coordinates were measured against splash_screen.png's native art (BLUEPRINT
        // spans roughly x:1042-1147, y:90-119 out of 1200x900) and scaled to match here.
        const qreal artScale = scaledPixmap.width() / qreal(originalPixmap.width());
        QLabel *versionLabel = new QLabel(QString::fromLatin1(Constants::APP_VERSION), imageLabel);
        versionLabel->setObjectName("SplashVersionLabel");
        versionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // An ID selector, not a bare property list: global.qss's "QLabel { color: #ffffff; ... }"
        // otherwise wins the cascade over an instance-level style that only sets a few properties.
        versionLabel->setStyleSheet(
            "#SplashVersionLabel { background-color: transparent; color: #d6d6d6; font-size: 14px; font-weight: 800; padding: 0; }");
        constexpr int blueprintLeftOrig = 1040;
        constexpr int blueprintCenterYOrig = 110;
        constexpr int versionLabelWidth = 70;
        constexpr int versionLabelHeight = 20;
        constexpr int gapBeforeBlueprint = 6;
        const int blueprintLeftScaled = qRound(blueprintLeftOrig * artScale);
        const int blueprintCenterYScaled = qRound(blueprintCenterYOrig * artScale);
        versionLabel->setGeometry(
            blueprintLeftScaled - gapBeforeBlueprint - versionLabelWidth,
            blueprintCenterYScaled - versionLabelHeight / 2,
            versionLabelWidth, versionLabelHeight);

        // Drop shadow so the splash image lifts off the dark overlay behind it. The overlay
        // itself is near-black, so the shadow needs to be fully opaque and fairly large to
        // read against it at all.
        QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(imageLabel);
        shadow->setBlurRadius(24);
        shadow->setOffset(6, 6);
        shadow->setColor(QColor(0, 0, 0, 120));
        imageLabel->setGraphicsEffect(shadow);

        if (parent) {
            syncGeometryToParent();
        }
        // Dismiss on the next mouse press ANYWHERE in the app, not just on the splash window.
        // As a translucent top-level window, clicks on its fully-transparent regions fall
        // through to the app beneath it, so watching only this widget would miss them — filter
        // the whole application instead. (The parent's resize/move events also arrive through
        // this same app-wide filter, which keeps the overlay aligned over the window.)
        if (qApp) {
            qApp->installEventFilter(this);
        }
    }

protected:
    void mousePressEvent(QMouseEvent *) override {
        dismiss(); // fallback; in practice the app-wide filter intercepts the press first
    }

    void showEvent(QShowEvent *event) override {
        // Parent geometry may have shifted between construction and show(); re-align and
        // make sure we're stacked above the (native) viewport.
        syncGeometryToParent();
        raise();
        QWidget::showEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        const QEvent::Type type = event->type();
        // Any mouse press anywhere in the app dismisses the splash, and is consumed so the
        // dismissing click doesn't also act on whatever sits underneath it.
        if (type == QEvent::MouseButtonPress) {
            dismiss();
            return true;
        }
        // As a top-level window we don't move with the parent automatically, so follow both
        // its resizes and moves to stay aligned over the window.
        if (watched == parent() && (type == QEvent::Resize || type == QEvent::Move)) {
            syncGeometryToParent();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    bool m_dismissed = false;

    /// Tear down the splash exactly once. deleteLater() (not 'delete this') because the click
    /// event may still be dispatching; the app-wide filter is removed immediately so no
    /// further events are intercepted while we wait to be deleted.
    void dismiss() {
        if (m_dismissed) {
            return;
        }
        m_dismissed = true;
        if (qApp) {
            qApp->removeEventFilter(this);
        }
        deleteLater();
    }

    /// Cover the parent window's client area, in global (screen) coordinates — required now
    /// that this is a top-level window rather than a child positioned in parent-local coords.
    void syncGeometryToParent() {
        if (QWidget *p = parentWidget()) {
            setGeometry(QRect(p->mapToGlobal(QPoint(0, 0)), p->size()));
        }
    }
};

#endif // SPLASHOVERLAY_H