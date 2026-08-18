/**
 * @file viewportwidget.h
 * @brief The 3D viewport as a plain QWidget — the ONLY thing the rest of the app touches.
 *
 * Owns the application's QVulkanInstance and embeds a VulkanWindow via
 * QWidget::createWindowContainer(), so callers (e.g. main.cpp) can drop it into a
 * layout like any other widget and stay completely unaware of Vulkan. If the Vulkan
 * instance can't be created (no driver / no GPU), it degrades to an inline message
 * rather than taking the whole app down.
 */

#ifndef VIEWPORTWIDGET_H
#define VIEWPORTWIDGET_H

#include <QWidget>
#include <QStringList>

#include <memory>

class QVulkanInstance;
class QPushButton;
class QShowEvent;
class QHideEvent;
class QResizeEvent;
class QMoveEvent;

namespace pose {

class VulkanWindow;
struct LightingSettings;

/**
 * @class ViewportWidget
 * @brief Self-contained 3D viewport widget backed by Vulkan.
 */
class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override; // out-of-line: completes unique_ptr<QVulkanInstance>

    /// Imports an OBJ file into the 3D scene. No-op if the viewport degraded to the
    /// Vulkan-unavailable message (no window to load into).
    void importObj(const QString& path);

    /// Imports a native figure file (`.duf`/`.dsf`) into the 3D scene. No-op if the viewport
    /// degraded to the Vulkan-unavailable message.
    void importFigure(const QString& path);

    /// Whether the scene holds a posable figure (gates the pose save/load menu actions).
    bool hasPosableFigure() const;
    /// Saves / loads the posed figure's joint rotations to/from @p path. Returns false on failure
    /// (no figure, degraded viewport, or unreadable/unwritable file).
    bool savePose(const QString& path);
    bool loadPose(const QString& path);

    /// Sets the viewport shade mode by index (see shaderModeNames()). No-op if the viewport degraded.
    void setShadeMode(int mode);

    /// Returns the camera to the default perspective framing (the overlay's Home button).
    /// No-op if the viewport degraded.
    void resetView();

    /// Drops the posable figure onto the ground plane (the overlay's ground button): moves it so
    /// the current pose's lowest point rests at y = 0. No-op if the viewport degraded.
    void groundFigure();

    /// Loads @p hdrPath as the lighting environment (re-bakes the IBL). No-op if the viewport degraded.
    void setEnvironment(const QString& hdrPath);
    /// Applies the live lighting/exposure dials (Environment panel). No-op if the viewport degraded.
    void setLightingSettings(const LightingSettings& settings);

    /// Undo/redo the last committed viewport edit — pose changes and Environment-panel lighting
    /// gestures share one chronological stack. Driven by Edit → Undo/Redo (whose shortcuts make
    /// Ctrl+Z/Ctrl+Y work app-wide) and the viewport's own key handling. No-op if degraded.
    void undo();
    void redo();

    /// Registers the pre-edit dial state of a committed Environment-panel gesture on the undo
    /// stack (see VulkanWindow::registerLightingUndo). No-op if the viewport degraded.
    void registerLightingUndo(const LightingSettings& preEdit);

    /// The user-facing shade-mode names, in the shader's mode order (index == shade mode). The single
    /// source of truth the floating shader dropdown is populated from.
    static QStringList shaderModeNames();

signals:
    /// Re-emitted from the viewport when undo/redo restores a lighting state, so the Environment
    /// panel can sync its widgets. The settings are already applied renderer-side.
    void lightingRestored(const LightingSettings& settings);

protected:
    // The shader dropdown floats as a *top-level* window over the native viewport (a child widget would
    // be composited behind it), so it has to be repositioned as the viewport moves/resizes.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createShaderOverlay();   // builds the floating top-right shader dropdown + home button
    void syncOverlayPosition();   // glues it to the viewport's top-right corner (global coords)

    std::unique_ptr<QVulkanInstance> m_instance;           // owns the VkInstance
    VulkanWindow*                    m_window = nullptr;    // owned by m_container
    QWidget*                         m_container = nullptr; // the createWindowContainer wrapper
    QWidget*                         m_overlay = nullptr;   // top-level frameless host for the dropdown
    QPushButton*                     m_shaderButton = nullptr;   // opens the shader-mode QMenu
    QPushButton*                     m_homeButton = nullptr;     // resets the camera to default framing
    QPushButton*                     m_groundButton = nullptr;   // drops the figure onto the floor plane
    QWidget*                         m_filteredWindow = nullptr; // top-level we filter for move/resize
};

} // namespace pose

#endif // VIEWPORTWIDGET_H
