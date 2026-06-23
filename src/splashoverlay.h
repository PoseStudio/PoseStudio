/**
 * @file splashoverlay.h
 * @brief A full-window, click-to-dismiss branding overlay (boot splash and "About" screen).
 */

#ifndef SPLASHOVERLAY_H
#define SPLASHOVERLAY_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QEvent>
#include <QPixmap>
#include <QGraphicsDropShadowEffect>
#include <QColor>
#include "constants.h"

/**
 * @class SplashOverlay
 * @brief Covers its parent window with the PoseStudio branding image until clicked.
 *
 * Tracks the parent's size via an event filter so it always fills the window, and
 * deletes itself on the next click anywhere within it.
 */
class SplashOverlay : public QWidget {
public:
    /// @param parent The window this overlay should cover (usually the QMainWindow).
    SplashOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, true);
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
            parent->installEventFilter(this);
            resize(parent->size());
        }
    }

protected:
    void mousePressEvent(QMouseEvent *) override {
        // deleteLater(), not 'delete this': the click event is still being dispatched,
        // so the widget must survive until Qt finishes processing it this iteration.
        deleteLater();
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parent() && event->type() == QEvent::Resize) {
            resize(static_cast<QWidget*>(parent())->size());
        }
        return QWidget::eventFilter(watched, event);
    }
};

#endif // SPLASHOVERLAY_H