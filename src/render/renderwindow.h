/**
 * @file renderwindow.h
 * @brief The Render window (Render → Render Image, F12): a production render of the scene as
 *        the viewport shows it, appearing as it renders, with what a render is chosen by — size,
 *        samples, device, denoising — and Save Image.
 *
 * One per process (RenderWindow::open), a window of its own owned by the main window, like the
 * User Manual: the viewport stays free, and posing goes on while a render runs — the render is
 * of the scene as it stood when Render was pressed (RenderSession holds that snapshot). The
 * window knows nothing of the viewport: it is given a function that captures the scene
 * (setSceneSource — main.cpp hands it ViewportWidget::captureRenderScene).
 */

#ifndef RENDERWINDOW_H
#define RENDERWINDOW_H

#include "rendersession.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace pose {

class DragNumberBox;
class MenuPickerButton;
class RenderImageView;

class RenderWindow : public QWidget {
    Q_OBJECT

public:
    using SceneSource = std::function<std::shared_ptr<RenderSceneHolder>()>;

    /// The process's Render window, created on first call (owned by @p owner), shown and raised.
    static RenderWindow* open(QWidget* owner);
    /// The window if it exists (it may be hidden), else null.
    static RenderWindow* existing();

    /// Where a render's scene comes from. Set once, before the first render.
    void setSceneSource(SceneSource source) { m_source = std::move(source); }

    RenderSession* session() { return &m_session; }

public slots:
    /// Captures the scene and starts a render with the window's settings (a running render is
    /// replaced). Tells the user why when it cannot.
    void startRender();
    void stopRender();
    void saveImage();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    explicit RenderWindow(QWidget* owner);

    void buildDevices();
    void loadSettings();
    void storeSettings();
    void syncControls();
    void showStatus();
    /// The size a render is made at: the chosen preset, or the viewport's own (@p view).
    QSize renderSize(const RenderSceneHolder::View& view) const;

    SceneSource   m_source;
    RenderSession m_session;

    QPushButton*      m_render = nullptr;
    QPushButton*      m_stop = nullptr;
    QPushButton*      m_save = nullptr;
    MenuPickerButton* m_size = nullptr;
    MenuPickerButton* m_device = nullptr;
    DragNumberBox*    m_samples = nullptr;
    QCheckBox*        m_denoise = nullptr;
    QCheckBox*        m_transparent = nullptr;
    RenderImageView*  m_view = nullptr;
    QLabel*           m_statusText = nullptr;
    QLabel*           m_engineText = nullptr;
    QProgressBar*     m_progress = nullptr;

    QVector<RenderDevice> m_devices;
    int                   m_skippedSplats = 0;
};

} // namespace pose

#endif // RENDERWINDOW_H
