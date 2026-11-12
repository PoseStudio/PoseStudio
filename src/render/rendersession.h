/**
 * @file rendersession.h
 * @brief One production render at a time, driven from the GUI thread: starts the engine on a
 *        captured scene, polls it, and keeps the image as it stands — the linear pixels the
 *        engine made, and the picture of them the viewport's display transform gives.
 *
 * The engine renders on its own threads (renderengineapi.h); nothing here blocks. A QTimer
 * polls its status a few times a second and, when the image has changed, copies it out and
 * re-makes the display picture: the engine's linear RGBA through the viewport's own last step
 * (the ACES fit where the scene's tonemap is on, then the sRGB curve), so a render looks like
 * the viewport's PBR mode, path traced. The scene the render was started with is HELD for as
 * long as the engine may read from it — that is this class's part of the interface's promise.
 *
 * The engine library itself is loaded on first use and shared by every session of the process
 * (RenderSession::engine): an application started to pose pays nothing for the renderer.
 */

#ifndef RENDERSESSION_H
#define RENDERSESSION_H

#include "renderengineapi.h"
#include "renderscene.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <memory>
#include <vector>

namespace pose {

/// What the user chooses for a render.
struct RenderOptions {
    int     width = 1920;
    int     height = 1080;
    int     samples = 256;
    QString deviceId;        ///< a RenderDevice::id; empty = the CPU
    bool    denoise = true;
    bool    transparent = false; ///< no backdrop: the image's alpha is the scene's coverage
};

struct RenderDevice {
    QString               id;
    QString               name;
    renderapi::DeviceType type = renderapi::DeviceType::Cpu;
    /// The name with its kind where one device is listed twice ("… (OptiX)", "… (CUDA)").
    QString label() const;
};

class RenderSession : public QObject {
    Q_OBJECT

public:
    explicit RenderSession(QObject* parent = nullptr);
    ~RenderSession() override;

    /// The process's render engine, loaded on first call. Null — with @p error saying why — in
    /// a build without one or when it will not load.
    static renderapi::Engine* engine(QString* error = nullptr);
    /// Whether an engine is installed at all (nothing is loaded).
    static bool engineInstalled();
    /// "Cycles 5.3.0", or "" without an engine.
    static QString engineName();
    /// The engine's devices, the CPU first (empty without an engine).
    static QVector<RenderDevice> devices();
    /// The device a first render uses: the first GPU, preferring a hardware ray-tracing one,
    /// else the CPU.
    static QString defaultDeviceId();

    /// Starts rendering @p scene (held until the render has ended). False with @p error when
    /// there is no engine or it refuses. A render already running is stopped first.
    bool start(std::shared_ptr<RenderSceneHolder> scene, const RenderOptions& options, QString* error = nullptr);
    /// Stops the render (the image so far stays).
    void stop();

    bool                     running() const { return m_running; }
    const renderapi::Status& status() const { return m_status; }
    const RenderOptions&     options() const { return m_options; }

    /// The picture: the image as displayed (8-bit sRGB; alpha kept for a transparent render).
    /// Null before the first pixels.
    const QImage& image() const { return m_image; }
    bool          hasImage() const { return !m_image.isNull(); }

    /// Saves the image: `.exr` the engine's LINEAR pixels (32-bit float, before the tonemap —
    /// for compositing and grading), anything else the picture as displayed (PNG, JPEG, …).
    bool save(const QString& path, QString* error = nullptr) const;

signals:
    /// The status changed (progress, the engine's text, the state).
    void statusChanged();
    /// The image changed.
    void imageChanged();
    /// The render ended: finished, cancelled or failed (status().state says which).
    void ended();

private:
    void poll();
    void refreshImage();
    void finishRun();

    QTimer                             m_timer;
    std::shared_ptr<RenderSceneHolder> m_scene; // held while the engine may read it
    RenderOptions                      m_options;
    QByteArray                         m_deviceId; // (the UTF-8 the engine is handed)
    renderapi::Status                  m_status;
    bool                               m_running = false;
    bool                               m_tonemap = true;
    quint64                            m_shownVersion = 0;
    std::vector<float>                 m_linear; // the engine's pixels: RGBA, row 0 = top
    QImage                             m_image;
};

} // namespace pose

#endif // RENDERSESSION_H
