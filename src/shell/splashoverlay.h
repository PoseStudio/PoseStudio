/**
 * @file splashoverlay.h
 * @brief A full-window, click-to-dismiss branding overlay (boot splash and "About" screen).
 */

#ifndef SPLASHOVERLAY_H
#define SPLASHOVERLAY_H

#include <QWidget>

class QEvent;
class QMouseEvent;
class QShowEvent;

/**
 * @class SplashOverlay
 * @brief Covers its parent window with the PoseStudio branding image until clicked.
 *
 * Tracks the parent's geometry via an event filter so it always fills the window, and
 * deletes itself on the next click anywhere within it.
 *
 * It is a frameless, translucent *top-level window* (not a child widget) deliberately: the
 * 3D viewport is hosted in a native child window (QWidget::createWindowContainer), and native
 * windows are composited on top of ordinary overlay widgets — a child-widget overlay would be
 * hidden behind the viewport. A top-level window *owned by the parent* (the parent pointer plus
 * the Qt::Tool type) is composited above the viewport surface instead.
 * Crucially it is NOT a global always-on-top window (no Qt::WindowStaysOnTopHint): because it's
 * an *owned* window, the OS keeps it above PoseStudio's own window/viewport, but it still yields
 * to other applications when they take focus. An always-on-top splash would float over and block
 * every other app — even ones the user switched to — which is a bad experience.
 */
class SplashOverlay : public QWidget {
    Q_OBJECT

public:
    /// @param parent The window this overlay should cover (usually the QMainWindow).
    explicit SplashOverlay(QWidget *parent);

signals:
    /// The overlay has been dismissed (the user's first click): what waits for the branding to
    /// clear — the What's New window on the first start of a new version — goes now.
    void dismissed();

protected:
    void mousePressEvent(QMouseEvent *) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool m_dismissed = false;

    /// Tear down the splash exactly once. deleteLater() (not 'delete this') because the click
    /// event may still be dispatching; the app-wide filter is removed immediately so no
    /// further events are intercepted while we wait to be deleted.
    void dismiss();

    /// Cover the parent window's client area, in global (screen) coordinates — required now
    /// that this is a top-level window rather than a child positioned in parent-local coords.
    void syncGeometryToParent();
};

#endif // SPLASHOVERLAY_H
