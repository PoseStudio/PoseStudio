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

    /// Loads @p hdrPath as the lighting environment (re-bakes the IBL). No-op if the viewport degraded.
    void setEnvironment(const QString& hdrPath);
    /// Applies the live lighting/exposure dials (Environment panel). No-op if the viewport degraded.
    void setLightingSettings(const LightingSettings& settings);

    /// The user-facing shade-mode names, in the shader's mode order (index == shade mode). The single
    /// source of truth the floating shader dropdown is populated from.
    static QStringList shaderModeNames();

protected:
    // The shader dropdown floats as a *top-level* window over the native viewport (a child widget would
    // be composited behind it), so it has to be repositioned as the viewport moves/resizes.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void createShaderOverlay();   // builds the floating top-right shader dropdown
    void syncOverlayPosition();   // glues it to the viewport's top-right corner (global coords)

    std::unique_ptr<QVulkanInstance> m_instance;           // owns the VkInstance
    VulkanWindow*                    m_window = nullptr;    // owned by m_container
    QWidget*                         m_container = nullptr; // the createWindowContainer wrapper
    QWidget*                         m_overlay = nullptr;   // top-level frameless host for the dropdown
    QPushButton*                     m_shaderButton = nullptr;   // opens the shader-mode QMenu
    QWidget*                         m_filteredWindow = nullptr; // top-level we filter for move/resize
};

} // namespace pose

#endif // VIEWPORTWIDGET_H
