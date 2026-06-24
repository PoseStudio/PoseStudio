/**
 * @file vulkanwindow.h
 * @brief The QWindow that actually owns a Vulkan surface and drives rendering.
 *
 * This is the bridge between Qt's windowing/event world and the Qt-free rendering/
 * subtree. It creates the per-window VkSurfaceKHR (via the app's QVulkanInstance),
 * stands up the VulkanContext + VulkanRenderer once the platform surface exists, pumps
 * one frame per UpdateRequest, and routes mouse/wheel input into the camera.
 *
 * It is never parented into the widget tree directly — ViewportWidget wraps it with
 * QWidget::createWindowContainer(). Lifetime of the Vulkan objects is tied to the
 * platform surface: they are torn down on SurfaceAboutToBeDestroyed, before Qt frees
 * the surface out from under them.
 */

#ifndef VULKANWINDOW_H
#define VULKANWINDOW_H

#include <QWindow>

#include <vulkan/vulkan.h> // for VkExtent2D in the pixelExtent() signature

#include <cstdint>
#include <memory>
#include <vector>

class QVulkanInstance;

namespace pose {

class VulkanContext;
class VulkanRenderer;

/**
 * @class VulkanWindow
 * @brief Hosts the Vulkan surface, owns the renderer, and translates input to camera moves.
 */
class VulkanWindow : public QWindow {
    Q_OBJECT

public:
    /**
     * @param instance   The application-wide QVulkanInstance (owned by ViewportWidget).
     * @param apiVersion The Vulkan API version @p instance was created with (VK_API_VERSION_*).
     * @param shaderDir  Absolute path to the directory holding compiled *.spv files.
     */
    VulkanWindow(QVulkanInstance* instance, uint32_t apiVersion, QString shaderDir,
                 QWindow* parent = nullptr);
    ~VulkanWindow() override;

    /// Imports an OBJ into the scene. If the renderer isn't built yet (the window hasn't been
    /// exposed), the path is queued and loaded once initialisation completes.
    void importObj(const QString& path);

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void initializeVulkan();   // safe to call repeatedly; no-ops once initialised
    void releaseVulkan();      // tears down renderer + context (surface still valid)
    void renderFrame();        // one frame, then schedules the next while exposed
    VkExtent2D pixelExtent() const; // window size in physical pixels

    /// Parses an OBJ, decodes its textures (QImage), and hands the result to the renderer.
    /// Requires m_renderer to exist. Logs and skips on parse/decode failure.
    void loadAndAddModel(const QString& path);

    QVulkanInstance* m_instance = nullptr; // borrowed
    uint32_t         m_apiVersion = 0;
    QString          m_shaderDir;
    bool             m_initialized = false;
    bool             m_deviceFailed = false; // init threw; don't spam retries

    std::unique_ptr<VulkanContext>  m_context;
    std::unique_ptr<VulkanRenderer> m_renderer;

    std::vector<QString> m_pendingModels; // imports requested before the renderer existed

    QPointF          m_lastMousePos;
    // Buttons whose drag actually began with a press in this window. We gate camera moves on
    // this rather than QMouseEvent::buttons() so a modal dialog (e.g. the Import file picker)
    // can't leak a button-held move to us as it closes and snap the camera.
    Qt::MouseButtons m_activeDragButtons = Qt::NoButton;
};

} // namespace pose

#endif // VULKANWINDOW_H
