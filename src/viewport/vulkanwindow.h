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

    /// Restores the camera's default framing (the viewport's Home button). No-op before the
    /// renderer exists — the camera is created with that framing, so there'd be nothing to undo.
    void resetView();

    /// Drops the posable figure onto the ground plane (the overlay's ground button): translates it
    /// so the current pose's lowest point rests at y = 0. No-op before the renderer exists.
    void groundFigure();

    /// Loads @p hdrPath as the lighting environment and re-bakes the IBL. Remembered and applied
    /// once the renderer exists if it isn't built yet. Decoding happens in this Qt layer.
    void setEnvironmentFile(const QString& hdrPath);

    /// Applies the live lighting/exposure dials (Environment panel). Remembered and applied once the
    /// renderer exists if it isn't built yet. Never registers undo itself — committed gestures do,
    /// via registerLightingUndo (live scrubbing pushes many intermediate states through here).
    void setLightingSettings(const LightingSettings& settings);

    /// Undoes / redoes the most recent committed edit. Pose changes and Environment-panel lighting
    /// edits share ONE chronological stack, so Ctrl+Z steps back through both kinds in the order
    /// they were made. Reachable via Edit → Undo/Redo (any focus) and Ctrl+Z/Ctrl+Y in the
    /// viewport; no-op while that stack side is empty or the renderer doesn't exist yet.
    void undo();
    void redo();

    /// Pushes a lighting undo entry: @p preEdit is the dial state before a committed Environment-
    /// panel gesture (scrub, type-in, restore button, restore-all). The panel calls this once per
    /// gesture; undoing the entry emits lightingRestored so the panel's widgets follow.
    void registerLightingUndo(const LightingSettings& preEdit);

signals:
    /// Emitted when undo/redo restores a lighting state. The settings are already applied to the
    /// renderer; the Environment panel listens (via ViewportWidget) to sync its widgets.
    void lightingRestored(const LightingSettings& settings);

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

    // One committed, undoable edit. A single stack holds both kinds so undo walks pose and
    // lighting changes together, in the order the user made them.
    struct UndoEntry {
        enum class Kind { Pose, Lighting };
        Kind kind = Kind::Pose;
        PoseSnapshot pose;         // the pre-edit pose  (kind == Pose)
        LightingSettings lighting; // the pre-edit dials (kind == Lighting)
    };

    void initializeVulkan();   // safe to call repeatedly; no-ops once initialised
    /// Decode + bake @p hdrPath off-thread, then swap it in on the GUI thread. @p autoAimKey:
    /// re-aim the key-light dials at the baked environment's dominant light when it lands — true
    /// for user-initiated HDRI switches (the shadow should follow the newly picked environment),
    /// false for the STARTUP bake, so the authored LightingSettings defaults are what a fresh
    /// launch actually shows.
    void beginEnvironmentBake(const QString& hdrPath, bool autoAimKey);
    QString defaultEnvironmentPath() const;            // <appDir>/environment.{hdr,exr} override, else the user library's hdri/
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

    // Undo/redo: each committed edit (a pose drag, or a registered lighting gesture) pushes its
    // pre-edit state. m_preEditPose snapshots the pose at drag start (committed on release).
    PoseSnapshot           m_preEditPose;
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
};

} // namespace pose

#endif // VULKANWINDOW_H
