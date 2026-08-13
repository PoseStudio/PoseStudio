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

#include "scene/lightingsettings.h" // stored by value; applied to the renderer once it exists

#include <QWindow>

#include <vulkan/vulkan.h> // for VkExtent2D in the pixelExtent() signature

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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

    /// Imports a native figure (`.duf`/`.dsf`) into the scene. Same queue-until-ready behaviour as
    /// importObj; routed to the FigureImportService.
    void importFigure(const QString& path);

    /// Whether the scene holds a posable figure (gates the pose save/load actions).
    bool hasPosableFigure() const;
    /// Saves / loads the posed figure's joint rotations to/from @p path. Returns false if there's
    /// no posable figure or the file can't be read/written.
    bool savePose(const QString& path);
    bool loadPose(const QString& path);

    /// Sets the viewport shade mode (index into the shader picker's mode list; see mesh.frag).
    /// Remembered and applied once the renderer exists if it isn't built yet.
    void setShadeMode(int mode);

    /// Loads @p hdrPath as the lighting environment and re-bakes the IBL. Remembered and applied
    /// once the renderer exists if it isn't built yet. Decoding happens in this Qt layer.
    void setEnvironmentFile(const QString& hdrPath);

    /// Applies the live lighting/exposure dials (Environment panel). Remembered and applied once the
    /// renderer exists if it isn't built yet.
    void setLightingSettings(const LightingSettings& settings);

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    using PoseSnapshot = std::vector<std::pair<std::string, glm::vec3>>;
    void undoPose(); // Ctrl+Z: revert the last committed pose edit
    void redoPose(); // Ctrl+Y: reapply an undone edit

    void initializeVulkan();   // safe to call repeatedly; no-ops once initialised
    void beginEnvironmentBake(const QString& hdrPath); // decode + bake off-thread, then swap in on the GUI thread
    QString defaultEnvironmentPath() const;            // <appDir>/environment.hdr override, else bundled HDRI
    void releaseVulkan();      // tears down renderer + context (surface still valid)
    void renderFrame();        // one frame, then schedules the next while exposed
    VkExtent2D pixelExtent() const; // window size in physical pixels

    /// Picks the object under @p localPos (window pixels) and, if one is hit, pops up the
    /// object context menu at @p globalPos (currently just "Delete"). No-op on empty space.
    void showObjectContextMenu(const QPointF& localPos, const QPoint& globalPos);

    QVulkanInstance* m_instance = nullptr; // borrowed
    uint32_t         m_apiVersion = 0;
    QString          m_shaderDir;
    bool             m_initialized = false;
    bool             m_deviceFailed = false; // init threw; don't spam retries

    std::unique_ptr<VulkanContext>  m_context;
    std::unique_ptr<VulkanRenderer> m_renderer;

    std::vector<QString> m_pendingModels;  // OBJ imports requested before the renderer existed
    std::vector<QString> m_pendingFigures; // figure imports requested before the renderer existed
    QString              m_pendingPose;    // pose file to apply once the queued figure is loaded
    int                  m_shadeMode = 1;  // viewport shade mode (1 = PBR/IBL); applied to the renderer once it exists
    QString              m_environmentPath;         // chosen HDRI (empty = the default at init); applied once the renderer exists
    quint64              m_environmentRequestId = 0; // ++ per HDRI request; a slower earlier bake with a stale id is discarded
    LightingSettings     m_lighting;                // live lighting dials; applied to the renderer once it exists

    QPointF          m_lastMousePos;
    // Buttons whose drag actually began with a press in this window. We gate camera moves on
    // this rather than QMouseEvent::buttons() so a modal dialog (e.g. the Import file picker)
    // can't leak a button-held move to us as it closes and snap the camera.
    Qt::MouseButtons m_activeDragButtons = Qt::NoButton;
    // True when the current left-drag began on a figure joint, so it rotates that joint instead of
    // orbiting the camera (posing). Set on press (near a joint), cleared on release.
    bool             m_posingBone = false;
    // Which rotate-gizmo ring the current left-drag grabbed (0=X,1=Y,2=Z), or -1 if not a gizmo drag.
    int              m_gizmoAxis = -1;

    // Pose undo/redo: each committed pose edit (gizmo or free-drag) pushes the pre-edit pose. m_preEdit
    // snapshots that pose at drag start.
    PoseSnapshot              m_preEditPose;
    std::vector<PoseSnapshot> m_undoStack;
    std::vector<PoseSnapshot> m_redoStack;
};

} // namespace pose

#endif // VULKANWINDOW_H
