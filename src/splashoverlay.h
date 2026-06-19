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
        layout->addWidget(imageLabel);

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