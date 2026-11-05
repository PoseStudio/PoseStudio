/**
 * @file vulkanwindow.h
 * @brief The QWindow that owns the Vulkan surface and the renderer, and turns every viewport
 *        gesture — camera, posing, pins, undo — into calls on the Qt-free engine.
 *
 * This is the bridge between Qt's windowing/event world and the Qt-free rendering/ + scene/
 * subtrees: it creates the per-window VkSurfaceKHR (via the app's QVulkanInstance), stands up
 * the VulkanContext + VulkanRenderer once the platform surface exists, pumps one frame per
 * UpdateRequest, and owns every interaction state machine the viewport has. It is never
 * parented into the widget tree directly — ViewportWidget wraps it with
 * QWidget::createWindowContainer(). The Vulkan objects' lifetime is tied to the platform
 * surface: they are torn down on SurfaceAboutToBeDestroyed, before Qt frees the surface.
 *
 * ONE class, SEVEN translation units (the armatureik.cpp pattern): input, IK, pose/undo and the
 * camera all mutate the same private gesture flags and must all call requestUpdate(), so they
 * stay one class with this header as the single contract, split by concern:
 *   - vulkanwindow.cpp             lifecycle: construction, Vulkan init/teardown/device loss,
 *                                  the remembered scene state and the import queue, expose /
 *                                  resize / the event-driven frame.
 *   - vulkanwindow_environment.cpp the HDRI: default path, the off-thread IBL bake, lighting dials.
 *   - vulkanwindow_input.cpp       mouse/wheel/key handlers — pure ROUTING into the gestures below,
 *                                  plus the object context menu and cursor→ray helpers.
 *   - vulkanwindow_camera.cpp      named views, the Blender-convention view hotkeys, projection.
 *   - vulkanwindow_ik.cpp          the full-body-IK drag loop (the 60 Hz tick, the release settle,
 *                                  wheel depth), the animated ground fall.
 *   - vulkanwindow_pose.cpp        pose edits as undo entries: commit, utilities, pins, save/load,
 *                                  delete, the Transform tab's dials and the joint mouse mode
 *                                  (B / S / T), the unified undo/redo stack.
 *   - vulkanwindow_bench.cpp       the POSESTUDIO_IK_PERF / POSESTUDIO_IK_BENCH diagnostics.
 *   - vulkanwindow_vr.cpp          VR Mode: the OpenXR session (vr/) — its state machine, the
 *                                  frame-wait thread, and the per-frame stereo render + mirror.
 * The private declarations below follow the same order.
 *
 * Gesture contracts (each learned from a real failure; see CLAUDE.md's viewport section):
 * rendering is EVENT-DRIVEN — every visual mutation calls requestUpdate(); camera drags gate on
 * m_activeDragButtons (buttons whose press was seen HERE), never on QMouseEvent::buttons(); a
 * plain left drag on a joint is the full-body-IK gesture and Ctrl+drag the single-joint FK
 * rotate; the 60 Hz drag timer is the SOLE IK solve driver (mouse moves only record the target);
 * a left press released within the platform drag distance is a CLICK (model select); a release
 * hands off to an ANIMATED settle, a ground drop to an animated fall, and every other pose edit
 * closes those first (closeOpenPoseEdits) while a button-held drag REFUSES them (dragInFlight).
 */

#ifndef VULKANWINDOW_H
#define VULKANWINDOW_H

#include "scene/camera.h"           // AxisView (the view hotkeys), Ray
#include "scene/ik/cursorfilter.h"  // IkCursorFilter (the drag target low-pass)
#include "scene/jointtransform.h"   // JointTransform (the Transform tab's view of the selected joint)
#include "scene/lightingsettings.h" // stored by value; applied to the renderer once it exists
#include "scene/projectfile.h"      // ProjectDocument (the .pss save/load state)
#include "scene/shademode.h"        // kDefaultShadeMode
#include "projectstate.h"           // ProjectState (the .pss document's runtime state)
#include "viewpreset.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QWindow>

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QSize;
class QTimer;
class QVulkanInstance;

namespace pose {

class OpenXrSystem;
struct VrInput;
class Scene;
class VulkanContext;
class VulkanError;
class VulkanRenderer;
struct IkDiagnostics;
struct IkScript;

/**
 * @class VulkanWindow
 * @brief Hosts the Vulkan surface, owns the renderer, and runs every viewport interaction.
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
    /// exposed), the path is queued and loaded once initialisation completes. An interactive
    /// import closes every open pose edit first (an import selects the new model, and a settle
    /// still ticking would retarget to it).
    void importObj(const QString& path);

    /// Imports a native figure (`.duf`/`.dsf`) into the scene. Same queue-until-ready behaviour as
    /// importObj; routed to the FigureImportService.
    void importFigure(const QString& path);

    /// Whether the scene holds a posable figure (gates the pose save/load actions).
    bool hasPosableFigure() const;
    /// Saves / loads the posed figure's joint rotations to/from @p path. Returns false if there's
    /// no posable figure or the file can't be read/written. A load is an undoable pose edit
    /// (refused while a button-held drag owns the pose); before the renderer exists the path is
    /// queued and applied after the queued figure (open-with a .pose).
    bool savePose(const QString& path);
    bool loadPose(const QString& path);

    /// Gathers the live scene state into a .pss project document (File → Save): every model's
    /// pose snapshot + world transform, the lighting dials + selected HDRI path, and the camera
    /// framing. False without a renderer.
    bool captureProjectDocument(ProjectDocument& out) const;

    /// Writes the live scene state to @p path as a .pss project file (File → Save / Save As).
    /// False without a renderer or if the file can't be written.
    bool saveProjectFile(const QString& path);

    /// Loads a .pss project file (File → Open): validates it atomically — the scene is only
    /// touched once the whole document has parsed — then resets the scene, re-imports every
    /// figure by its source path and restores pose + transform in document order, and applies
    /// environment + camera. @p recovered maps a missing source path to the caller's re-pointed
    /// replacement (link recovery); any source still absent lands in @p missing and the load is
    /// refused with the scene untouched. Returns 0 on success, 1 when sources are missing,
    /// -1 on failure with a human-readable @p error.
    int loadProjectFile(const QString& path, const std::map<std::string, std::string>& recovered,
                        std::vector<std::string>& missing, std::string& error);

    /// (loadProjectFile: a source the caller maps to "" in @p recovered is SKIPPED — left out of
    /// the load — which is what the recovery dialog's Skip means.)
    /// The .pss document this window last saved/opened ("" = unsaved).
    QString projectPath() const;
    void setProjectPath(const QString& path);

    /// Whether the scene has changed since the last save/open. Set by every scene-mutating
    /// gesture, cleared by a successful save (the close prompt and Save's no-op hint read it).
    bool isProjectDirty() const;
    void markProjectDirty();
    void setProjectClean();

    /// Sets the viewport shade mode (an index into the picker's table, scene/shademode.h).
    /// Remembered and applied once the renderer exists if it isn't built yet.
    void setShadeMode(int mode);

    /// Deletes the SELECTED object (the outlined one) — the Delete key and Edit → Delete. No-op
    /// without a selection or before the renderer exists; a drag in flight is ended first, and
    /// the undo history is cleared (model indices shift; the deleted figure's poses are moot).
    void deleteSelectedObject();

    /// Resets the scene to a FRESH LAUNCH (File → New; the base of Open): models, camera,
    /// shade mode / skeleton / lighting / HDRI back to startup defaults, history cleared —
    /// Ctrl+Z does not undo a New. Open pose edits are closed first (no settle/fall ticking at
    /// deleted models). No-op before the renderer exists.
    void resetToEmptyScene();

    /// Pose utilities (Edit menu / the joint context menu), each ONE undoable pose edit on the
    /// active figure — see runPoseUtility. "Limb" = the selected joint and everything below it.
    void resetSelectedJoint();
    void resetSelectedLimb();
    void resetPose();
    void mirrorPose();
    void mirrorSelectedLimb();

    /// THE TRANSFORM TAB's side of the selected joint (Armature::jointDial): its name and its
    /// three dials — Bend Forward, Bend Sideways, Twist — as the pose stands. Invalid without a
    /// renderer, a posable figure or a selected joint. jointTransformChanged says when to re-read.
    JointTransform jointTransform() const;
    /// A dial edit, as ONE undoable pose edit however many values it passes through (a scrub):
    /// begin — refused (false) while a button-held drag owns the pose; every self-completing edit
    /// is closed first and the pose snapshotted — then any number of setJointTransformDial, then
    /// end (the settled-pose hook and the undo entry; a no-op when nothing changed). A set without
    /// a begin opens the bracket itself; any other pose edit closes an open one (closeSettlingEdits),
    /// after which the panel's own end is a no-op.
    bool beginJointTransformEdit();
    /// Turns dial @p dial (0/1/2 = Bend Forward / Bend Sideways / Twist) of the selected joint to
    /// @p value on its 0-100 scale: an FK rotation through the limits and the collision stop, so
    /// the pose may rest short of the value — re-read jointTransform().
    void setJointTransformDial(int dial, double value);
    void endJointTransformEdit();
    /// THE FIGURE'S PLACE AND HEADING (the Transform tab's Figure group): the same bracket as the
    /// dials — one undo entry per scrub. `axis` 0/1/2 = x/y/z in world centimetres; the turn in
    /// degrees (0 = facing +z, positive toward her left).
    void setFigurePosition(int axis, double cm);
    void setFigureTurn(double degrees);

    /// THE JOINT MOUSE MODE, as the rest of the application drives it (the mode itself is
    /// described with its members below). toggleJointModal is the B / S / T key (@p kind 0/1/2 =
    /// Bend / Side-Side / Twist, a JointDialKind) WHEREVER THE KEYBOARD FOCUS IS — the Edit menu's
    /// actions carry the keys application-wide: it turns the mode on for that dial of the
    /// selected joint, off again if it is the running one, or switches to it; true if the mode
    /// is on afterwards. moveJointModalTo is a mouse move that arrived at another window of the
    /// application (JointModalInput), in GLOBAL coordinates; confirm keeps the result (one undo
    /// entry), cancel puts the joint back as it was. All no-ops with the mode off.
    bool toggleJointModal(int kind);
    void moveJointModalTo(const QPointF& globalPos);
    void confirmJointModal();
    void cancelJointModal();
    /// The dial the mouse is turning (a JointDialKind), or -1 with the mode off.
    int jointModal() const { return m_modalKind; }
    /// A scripted test owns the input (POSESTUDIO_IK_SCRIPT): real input is ignored meanwhile.
    bool scriptRunning() const { return m_script != nullptr; }

    /// Toggles the skeleton overlay (the joint→parent bone lines drawn over the figure). Off by
    /// default — joints are grabbed directly on the figure — but they stay grabbable either way.
    /// Remembered and applied once the renderer exists if it isn't built yet.
    void setShowSkeleton(bool on);
    bool showSkeleton() const;

    /// Restores the camera's default framing (the viewport's Home button). No-op before the
    /// renderer exists — the camera is created with that framing, so there'd be nothing to undo.
    void resetView();

    /// Camera hotkeys in Blender's numpad convention (View menu + the keys in keyPressEvent):
    /// snap to an axis-aligned view, flip to the opposite side (180° about the up axis), or
    /// frame the selected object (everything, with no selection). No-ops before the renderer
    /// exists.
    void setAxisView(AxisView view);
    void flipView();
    void frameSelected();

    /// Which named view the camera is in (see ViewPreset) — the View picker's label.
    ViewPreset currentView() const { return m_viewPreset; }

    /// Drops the posable figure onto the ground plane (the overlay's ground button): translates it
    /// so the current pose's lowest point rests at y = 0. A figure ABOVE the floor FALLS there —
    /// free fall from rest at 9.81 m/s² on real elapsed time (world units are metres, so a 1m drop
    /// lands in ~0.45s), animated on the fall timer; a figure sunk below the floor is lifted
    /// instantly (nothing falls upward). No-op before the renderer exists or mid-drag.
    void groundFigure();
    /// The Posing preferences changed (the soft breasts): every figure re-posed with them.
    void applySoftBodySettings();

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

    // --- VR Mode (vulkanwindow_vr.cpp) ----------------------------------------------------------
    /// Registers the headset found at startup (an OpenXrSystem owned by ViewportWidget, which
    /// created the Vulkan instance to its requirements). Must precede the renderer's creation
    /// (the first expose): the device is created on the headset's physical device with its
    /// extensions. Null = no VR this session. Whether VR Mode can start is published through
    /// VrSettings::setReady once the device exists.
    void setOpenXrSystem(OpenXrSystem* system);
    /// Starts / stops VR Mode (View → VR Mode). Returns whether VR is on afterwards; a failure
    /// to start, or a session the runtime ends, is announced through vrError. No-op before the
    /// renderer exists or without a headset.
    bool setVrMode(bool on);
    bool vrModeActive() const { return m_vr != nullptr; }
    /// Puts the user back in front of the figure, facing it (View → Recenter VR View) — applied
    /// at the next tracked frame. VR Mode also does this on its own as it starts.
    void recenterVr();

signals:
    /// Emitted when VR Mode starts or ends — by the user, or because the headset session was
    /// lost. The View menu's checkable entry follows it.
    void vrModeChanged(bool on);
    /// Emitted with a user-readable message when VR Mode could not start or was ended by the
    /// runtime. The shell shows it.
    void vrError(const QString& message);
    /// Emitted when undo/redo restores a lighting state. The settings are already applied to the
    /// renderer; the Environment panel listens (via ViewportWidget) to sync its widgets.
    void lightingRestored(const LightingSettings& settings);
    /// Emitted whenever the camera enters or leaves a named view (an axis view, Home, a flip, or
    /// an orbit drag away from one) — the View picker's button follows it.
    void viewPresetChanged(ViewPreset view);
    /// Emitted when the joint mouse mode turns on (@p kind 0/1/2 = Bend / Side-Side / Twist, a
    /// JointDialKind: moving the mouse turns that dial of the selected joint) and when it ends
    /// (-1). The viewport strip shows a badge for the duration.
    void jointModalChanged(int kind);
    /// Emitted when what jointTransform() returns has changed — another joint (or none) selected,
    /// or the selected joint's pose moved by ANY means: a drag, the mouse mode, undo, a pose load,
    /// a reset, a dial. The Transform tab re-reads on it.
    void jointTransformChanged();
    /// The scripted test's `uishot`: a picture of the application's WIDGETS (the side tabs — the
    /// native viewport is not in it) is wanted at @p path. ViewportWidget takes it.
    void uiShotRequested(const QString& path);
    /// The scripted test's `app`: input as it arrives at the APPLICATION with the viewport not
    /// focused — @p what is b / s / t (the Edit menu's action with that shortcut), esc / enter
    /// (a key to the main window), click / rclick (a press and release there) or move (@p delta
    /// pixels in @p steps mouse moves there). ViewportWidget makes the events.
    void appInputRequested(const QString& what, const QPointF& delta, int steps);
    /// Emitted when the shade mode changes (a picker-table index, scene/shademode.h) — only a
    /// real change is announced (setShadeMode de-duplicates against m_announcedShadeMode). The
    /// viewport strip mirrors it into its shader picker.
    void shadeModeChanged(int mode);
    /// Emitted when the .pss document's state changes (a path was adopted, or the dirty flag
    /// transitioned) — ProjectState de-duplicates, so a pose edit on an already-dirty scene is
    /// not re-announced. ViewportWidget re-emits it for the window-title update.
    void documentChanged();
    /// Emitted when a loaded project put a panorama up (File → Open): @p path is the environment
    /// now in use (the default's when the document named none). The Environment tab's HDRI
    /// caption follows it — a caption, not a pick, so nothing re-bakes.
    void environmentRestored(const QString& path);

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    using PoseSnapshot = std::vector<std::pair<std::string, glm::vec3>>;

    // =========================================================================================
    // Lifecycle (vulkanwindow.cpp)
    // =========================================================================================

    /// Builds the context + renderer on the platform surface; safe to call repeatedly (no-ops
    /// once initialised or after a device failure). Applies the remembered scene state, kicks
    /// off the startup environment bake, and drains the import queue.
    void initializeVulkan();
    /// Tears down renderer + context (the surface stays valid) and resets every interaction
    /// state machine — a drag, settle, fall, or joint mouse mode dies with the figure it acted on.
    /// Idempotent.
    void releaseVulkan();
    /// The ONE device-loss path (init, the environment upload, a frame): releases the Vulkan
    /// objects, marks the device failed so nothing retries every expose, and logs @p stage.
    void failDevice(const char* stage, const VulkanError& error);
    /// One frame, then schedules another only when the renderer says the swapchain was rebuilt
    /// (event-driven rendering — see renderFrame's comment).
    void renderFrame();
    /// Window size in physical pixels (the swapchain extent).
    QSize pixelExtent() const;
    /// Hands the renderer the current physical size + device pixel ratio and requests a frame
    /// (expose and resize both do exactly this).
    void syncRendererExtent();
    /// The renderer's scene — every posing/selection call goes through it. Requires m_renderer.
    Scene& scene() const;
    /// Emits jointTransformChanged if the selected joint's transform differs from the one last
    /// announced. Called once per rendered frame: rendering is event-driven and EVERY pose or
    /// selection change requests a frame, so the frame is the one place that sees them all —
    /// no gesture has to remember the Transform tab.
    void notifyJointTransform();

    /// An import requested before the renderer existed, replayed in request order once it does
    /// (command-line "open with" hands several in a row — the order must survive).
    struct PendingImport {
        enum class Kind { Obj, Figure };
        Kind    kind = Kind::Obj;
        QString path;
    };
    /// The one import body: the interactive path (progress dialog) and the startup drain (no
    /// dialog — it runs inside the expose/init path, where a modal dialog's processEvents() could
    /// re-enter exposeEvent()/initializeVulkan()) share it. True if a model was added.
    bool runImport(const PendingImport& import, bool showProgress);
    /// Queues or runs an import depending on whether the renderer exists yet.
    void importOrQueue(PendingImport import);

    /// Scene state this window remembers on the renderer's behalf: chosen before the renderer
    /// exists (the picker/panel can be driven while the viewport is still hidden) and re-applied
    /// after a device loss. applyTo() pushes the live settings; the queues are drained by
    /// initializeVulkan().
    struct DeferredSceneState {
        int              shadeMode = kDefaultShadeMode; // picker-table index
        bool             showSkeleton = false;
        LightingSettings lighting;        // the live dials (also what undo restores from)
        QString          environmentPath; // chosen HDRI (empty = the default at init)
        std::vector<PendingImport> imports; // in request order
        QString          pendingPose;     // pose file to apply once the queued figure is loaded
        /// Applies shade mode, skeleton overlay and lighting dials to a (fresh) renderer.
        void applyTo(VulkanRenderer& renderer) const;
    };

    /// Applies @p f to the renderer and requests a frame — the body of every "remember, apply,
    /// redraw" setter. No-op before the renderer exists (the remembered value applies at init).
    template <class F>
    void withRenderer(F&& f) {
        if (m_renderer) {
            f(*m_renderer);
            requestUpdate();
        }
    }

    QVulkanInstance* m_instance = nullptr; // borrowed
    uint32_t         m_apiVersion = 0;
    QString          m_shaderDir;
    bool             m_initialized = false;
    bool             m_deviceFailed = false; // init/frame/upload threw; don't spam retries

    std::unique_ptr<VulkanContext>  m_context;
    std::unique_ptr<VulkanRenderer> m_renderer;
    DeferredSceneState              m_deferred;
    int                            m_announcedShadeMode = kDefaultShadeMode; // last value announced via shadeModeChanged

    ProjectState m_project;      // the .pss document's runtime state (path + dirty, change signal)

    // =========================================================================================
    // Environment (vulkanwindow_environment.cpp)
    // =========================================================================================

    /// Decode + bake @p hdrPath off-thread, then swap it in on the GUI thread. @p autoAimKey:
    /// re-aim the key-light dials at the baked environment's dominant light when it lands — true
    /// for user-initiated HDRI switches (the shadow should follow the newly picked environment),
    /// false for the STARTUP bake, so the authored LightingSettings defaults are what a fresh
    /// launch actually shows.
    void beginEnvironmentBake(const QString& hdrPath, bool autoAimKey);
    /// The panorama to load when none was chosen (or the chosen one no longer exists):
    /// LibraryPaths::defaultHdri(). Empty = keep the procedural studio.
    QString defaultEnvironmentPath() const;

    quint64 m_environmentRequestId = 0; // ++ per HDRI request; a slower earlier bake with a stale id is discarded

    // =========================================================================================
    // Input (vulkanwindow_input.cpp) — the handlers ROUTE; the gestures live in the sections
    // below.
    // =========================================================================================

    /// The world-space picking ray through @p localPos (window pixels).
    Ray cursorRay(const QPointF& localPos) const;
    /// Selects and returns the figure joint under @p localPos (Scene::selectBoneAt — picks by
    /// bone body, independent of the overlay's visibility), or -1 on a miss (the selection is
    /// kept).
    int boneAt(const QPointF& localPos);
    /// Picks the object under @p localPos (window pixels) and, if a joint or model is hit, pops
    /// up the object context menu at @p globalPos: Pin/Unpin Joint (+ Unpin All), Reset Joint /
    /// Reset Limb / Mirror Limb, and Delete. No-op on empty space.
    void showObjectContextMenu(const QPointF& localPos, const QPoint& globalPos);
    /// A left CLICK (press + release without dragging) at @p localPos: selects the model under
    /// the cursor — the one the viewport outlines — or clears the selection on empty space.
    void selectModelAtClick(const QPointF& localPos);

    QPointF m_lastMousePos;
    // Buttons whose drag actually began with a press in this window. We gate camera moves on
    // this rather than QMouseEvent::buttons() so a modal dialog (e.g. the Import file picker)
    // can't leak a button-held move to us as it closes and snap the camera.
    Qt::MouseButtons m_activeDragButtons = Qt::NoButton;
    // A left press that hit no joint — the orbit gesture — may still turn out to be a CLICK:
    // released within the platform's drag distance of the press, it selects the model under the
    // cursor (box-level pick) or, on empty space, clears the selection. The selected model is
    // the one the viewport outlines.
    bool    m_leftClickCandidate = false;
    QPointF m_leftPressPos;

    // =========================================================================================
    // Camera (vulkanwindow_camera.cpp)
    // =========================================================================================

    /// Records the named view the camera is now in and emits viewPresetChanged if it changed.
    void noteView(ViewPreset view);
    /// Marks the project dirty for a camera move — ONLY when the "camera changes count as unsaved"
    /// preference is on (default off: framing is saved into the document, but fiddling with the
    /// view must not trip the close/New prompt). Every camera gesture funnels through this.
    void noteCameraChange();
    /// The in-viewport fallback for the View menu's app-wide shortcuts (Blender's numpad
    /// convention, number row + keypad with either NumLock state): true if @p event was a view
    /// key and was handled.
    bool handleViewHotkey(const QKeyEvent* event);
    /// Projects a world point to window coordinates (false if behind the camera).
    bool projectToScreen(const glm::vec3& world, QPointF& out) const;

    ViewPreset m_viewPreset = ViewPreset::Home; // the camera starts at its default framing

    // =========================================================================================
    // Full-body IK drag, ground fall (vulkanwindow_ik.cpp)
    // =========================================================================================

    /// Wheel during a full-body-IK drag: how far the drag plane moves along the view direction
    /// per 120-unit notch, as a FRACTION of the plane's distance from the camera (~5cm framing a
    /// whole figure at the default 2.6m, millimetres in a close-up on a hand). Shared with the
    /// benchmark's ":depth" variant, which checks the geometry against it.
    static constexpr float kIkDepthPerWheelNotch = 0.02f;

    /// The 60 Hz drag timer's tick — the SOLE IK solve driver: re-issues the last (filtered)
    /// target while the button is held, so catch-up continues and settles even when the mouse
    /// stops moving (mouse-move events stop with it), and steps the animated release settle.
    void onIkTick();
    /// Issues the current (smoothed) IK target to the scene. Returns true if the solve actually
    /// changed the pose (the caller only requests a frame then). ONLY onIkTick calls this —
    /// solving per mouse event made the damped dynamics mouse-polling-rate-dependent.
    bool issueIkTarget();
    /// A left press on the joint under @p localPos: selects it and begins the gesture — a plain
    /// press the full-body-IK drag, @p scoped (Ctrl) the Ctrl+drag (IkScope::Part, resolved by
    /// the armature: the SCOPED drag of the grabbed chain alone for a limb or the head and neck,
    /// the FIGURE MOVE — the whole figure as she is posed, off the floor's contacts — for a grab of
    /// the body itself, the digit's own drag for a finger or a toe) — and snapshots the pose for
    /// undo. Returns false when no joint is under the cursor (the press is then the orbit/click
    /// gesture).
    bool beginJointGesture(const QPointF& localPos, bool scoped);
    /// A SHIFT + left press on the joint under @p localPos (THE GROUP, 2026-10-27): the joint joins
    /// the selection as its primary — the previous selection becoming the group's secondaries, a
    /// member pressed again becoming the primary — and a whole-body IK drag of the whole group
    /// begins through it (Armature::beginIkDrag). Released without a move it is a Shift+CLICK:
    /// a new joint stays added, a member is removed (m_ik.groupClickBone). Returns false when no
    /// joint is under the cursor.
    bool beginGroupGesture(const QPointF& localPos);
    /// Begins the IK drag of the selected joint (the drag plane anchored at its grab point) and
    /// starts the tick — the whole body's, or @p scoped the grabbed chain's alone. Returns false
    /// without a figure/selection.
    bool beginIkDrag(bool scoped = false);
    /// Mouse-up of an IK drag that moved the pose: hands off to the ANIMATED settle — the timer
    /// keeps ticking, each tick relaxing the body onto its pins (finishIkSettle then commits).
    void beginIkRelease();
    /// Ends an IK drag WITHOUT a settle: a click on a joint (nothing moved), a stale drag whose
    /// release never reached this window, a delete mid-gesture. Commits the undo entry if the
    /// drag did move the pose.
    void abortIkDrag();
    /// Completes the post-release IK settle NOW (ends the drag, runs the settled-pose hook,
    /// commits the undo entry). Called by the timer when the animated settle lands, and by any
    /// interaction that must not overlap it (a new press, undo/redo) to cut it short cleanly.
    void finishIkSettle();
    /// Intersects the cursor ray through @p localPos with the camera-parallel drag plane through
    /// m_ik.planePoint. True (with @p outWorld) on a hit in front of the camera.
    bool dragPlaneHit(const QPointF& localPos, glm::vec3& outWorld) const;
    /// Records @p target as the raw drag target (seeding the cursor filter at the grab point on
    /// the first one). The tick consumes it.
    void setIkTarget(const glm::vec3& target);
    /// The IK drag's DEPTH control: moves the drag plane (m_ik.planePoint) along the view
    /// direction by @p notches wheel notches (+ = away from the camera) and re-derives the raw
    /// drag target from @p cursorPos (window coords) through the moved plane, so the grabbed joint
    /// stays under the pointer while its depth changes. Shared by wheelEvent and the scripted
    /// bench (POSESTUDIO_IK_BENCH=<bone>:depth), which exercises the same geometry with no
    /// desktop input. Returns false if the cursor ray missed the plane (target left unchanged).
    bool stepIkDepth(float notches, const QPointF& cursorPos);

    /// The full-body-IK drag's state (see the class comment for the gesture).
    struct IkDragState {
        // True while a PLAIN left-drag is full-body-IK-dragging the grabbed joint: the joint
        // follows the cursor in a camera-parallel plane through its grab point (planePoint,
        // world space — the mouse WHEEL moves that plane along the view direction during the
        // drag, the gesture's depth control), the body following via the FBIK solve (feet
        // pinned, auto-balanced).
        bool dragging = false;
        // True between IK-drag mouse RELEASE and the end of the ANIMATED release settle: the
        // timer keeps ticking, each tick relaxing the body one capped round onto its ground pins
        // (the old one-shot settle applied the whole landing in a single frame — a visible pose
        // pop at mouse-up). The settled-pose hook + the undo commit wait until it lands.
        bool settling = false;
        // A raw target has been recorded since the press (the first move or wheel seeds the
        // filter at the grab point).
        bool hasTarget = false;
        // True once a drag's solve has actually CHANGED the pose. A CLICK on a joint (select, no
        // motion) must be a no-op: without this gate the release still ran the settle onto the
        // drag's ground-healed pins, visibly shifting a hovering figure and committing an undo
        // entry for a gesture the user perceived as a click.
        bool      poseChanged = false;
        // THE GROUP: a Shift press on a joint that was ALREADY a member (or the primary) — released
        // as a CLICK, with no solve having moved the pose, it REMOVES that joint from the group
        // (beginGroupGesture / mouseReleaseEvent); -1 otherwise.
        int       groupClickBone = -1;
        glm::vec3 planePoint{0.0f};
        glm::vec3 lastTarget{0.0f};
        // The adaptive low-pass the raw cursor target goes through before each solve tick (see
        // cursorfilter.h): raw cursor positions carry pixel noise even when "held still". Shared
        // with the IK harness so the app and the tests run the identical loop.
        IkCursorFilter filter;
    };
    IkDragState m_ik;
    QTimer*     m_ikTimer = nullptr;

    /// Completes an in-flight ground fall NOW (applies the remaining drop, stops the timer):
    /// any interaction that reads the figure's transform (a press, a pose save) calls this first.
    void finishGroundFall();
    /// The fall timer's tick: advances the free-fall curve on real elapsed time.
    void onFallTick();
    /// The ground drop as a pose edit: the settled-pose hook and its one undo entry, once she has
    /// landed (and her landing's bounce is over) or the fall was completed at once.
    void commitGroundDrop();

    /// The animated ground drop (groundFigure): WHICH model falls (the active figure at the
    /// button press — undo can switch the active figure mid-fall, and the remaining drop must
    /// still land on the one that started falling), the height still to fall, how much has
    /// fallen so far, and the real-time clock the free-fall curve is evaluated against.
    /// ... and THE LANDING BOUNCE that follows a fall onto her feet (Armature::beginLandingBounce:
    /// the legs absorb the landing, deeper the faster she came down): on the same timer and
    /// clock, from the moment of the landing; finishGroundFall ends it like the fall.
    struct GroundFall {
        QTimer*       timer = nullptr;
        QElapsedTimer clock;
        float         height = 0.0f;
        float         dropped = 0.0f;
        int           figure = -1;
        bool          bouncing = false;  ///< The fall has landed and the bounce is in flight.
        float         landedAt = 0.0f;   ///< When it landed, seconds on the clock.
        bool          softTail = false;  ///< Landed with no bounce; the soft body (the breasts) still settles on this timer.
        float         lastT = 0.0f;      ///< The previous tick's time (the soft body's dt).
    };
    GroundFall m_fall;

    // =========================================================================================
    // Pose edits and undo (vulkanwindow_pose.cpp)
    // =========================================================================================

    /// A mouse-button-held pose gesture owns the pose right now (an IK drag, or a Ctrl FK drag):
    /// every other pose edit — undo/redo, utilities, pins, a pose load, the view/delete keys —
    /// is REFUSED until its release. An IK drag in particular solves against pins captured at
    /// drag start, and re-posing underneath it would leave the solve fighting a stale stance.
    bool dragInFlight() const { return m_ik.dragging; }
    /// Any pose edit in flight: a held drag, the animated release settle, or an open dial edit
    /// (the Transform tab's, or the joint mouse mode's). The self-completing ones can be closed
    /// early by closeOpenPoseEdits().
    bool poseEditInFlight() const { return dragInFlight() || m_ik.settling || m_transformEdit; }
    /// Lands / closes every edit that completes on its own — the release settle, the ground
    /// fall, an open dial edit (the Transform tab's or the joint mouse mode's, which is KEPT) —
    /// so the caller acts on a settled pose (each one commits its own undo entry). Leaves a live
    /// button-held drag alone.
    void closeSettlingEdits();
    /// closeSettlingEdits() plus the button-held drags: a STALE IK drag (its release never
    /// reached this window) is aborted, an FK drag ended. Every discrete pose edit (undo/redo,
    /// utilities, pins, pose load/save, delete, import) starts here so m_preEditPose can't be
    /// overwritten under an open bracket — the cross-figure/mis-bracketed undo entries of old.
    void closeOpenPoseEdits();

    /// Commits an undo entry for the pose edit bracketed by m_preEditPose (no-op if unchanged).
    void commitPoseUndo();
    /// Runs one reset/mirror utility as an undoable pose edit: refused while a drag owns the
    /// pose (the rig captured its pins and pose at drag start), every open edit is closed
    /// first, then the edit is bracketed with m_preEditPose/commitPoseUndo (a no-op when
    /// nothing changed — no selection, already at rest), correctives re-evaluate, and a frame is
    /// requested.
    enum class PoseUtility { ResetJoint, ResetLimb, ResetPose, MirrorPose, MirrorLimb };
    void runPoseUtility(PoseUtility what);
    /// Pin / unpin the selected joint, or drop every pin — each ONE undoable pose edit (pins
    /// ride in the pose snapshot as "@pin:" rows). Refused while a drag owns the pose.
    void togglePinSelectedJoint();
    void unpinAllJoints();
    /// Removes model @p index from the scene (the context menu's Delete and deleteSelectedObject
    /// share it): every open edit is closed first, then the undo history is cleared.
    void deleteModel(int index);

    // One committed, undoable edit. A single stack holds both kinds so undo walks pose and
    // lighting changes together, in the order the user made them. Joint PIN toggles are pose
    // edits too: the snapshot carries the pins ("@pin:" rows), so undoing a pin or a drag
    // restores the pins of that moment.
    struct UndoEntry {
        enum class Kind { Pose, Lighting };
        Kind             kind = Kind::Pose;
        PoseSnapshot     pose;        // the pre-edit pose  (kind == Pose)
        int              figure = -1; // which figure the pose belongs to (kind == Pose)
        LightingSettings lighting;    // the pre-edit dials (kind == Lighting)
    };
    /// The one undo/redo body: pops @p from, re-applies it, and pushes the state it replaced
    /// onto @p to. undo() and redo() are mirror images of each other through it.
    void swapUndoEntry(std::vector<UndoEntry>& from, std::vector<UndoEntry>& to);

    // Undo/redo: each committed edit (a pose drag, or a registered lighting gesture) pushes its
    // pre-edit state. m_preEditPose snapshots the pose at the start of every bracketed edit.
    PoseSnapshot           m_preEditPose;
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;

    // The Transform tab's dial edit (beginJointTransformEdit .. endJointTransformEdit): open while
    // true, its pre-edit pose in m_preEditPose like every bracketed edit's. And the joint
    // transform last announced through jointTransformChanged (notifyJointTransform).
    bool           m_transformEdit = false;
    JointTransform m_announcedJointTransform;

    // THE JOINT MOUSE MODE (Blender's modal transforms): with a joint selected, B / S / T turns
    // on its Bend / Side-Side / Twist dial and MOVING THE MOUSE turns it — the limb goes the way
    // the mouse goes (the dial's sweep projected to the screen: Armature::jointDialSweep), or,
    // for a twist and for a bend that goes toward or away from the camera as the mode begins,
    // right is more and left is less. A LEFT CLICK (or Enter) keeps the result: one undo entry. Esc, a
    // RIGHT CLICK or the same key again puts the joint back as it was; another of the three keys
    // drops the running mode and begins that one. It is an open dial edit (m_transformEdit) with
    // the mouse as its scrub, so everything that closes one — a press elsewhere, undo, a pose
    // utility, a lost focus — keeps the result like a click. Refused while a drag owns the pose.
    // The keys and the mouse are the mode's WHEREVER they land in the application, not only over
    // the viewport: the Edit menu's actions carry B / S / T (toggleJointModal), and while the mode
    // is on JointModalInput hands this window the moves, clicks and Esc / Enter that arrive at
    // the application's other windows.
    /// B / S / T (@p kind a JointDialKind), with the cursor at @p cursor: toggles or switches the
    /// mode as described above. True if the mode is on afterwards.
    bool beginJointModal(int kind, const QPointF& cursor);
    /// The cursor moved to @p cursor with the mode on: turns the dial by the move.
    void jointModalMove(const QPointF& cursor);
    /// Where the mode's dial swings its limb's far end on screen, as the pose stands: the unit
    /// @p direction of a positive turn and the lever in pixels per radian. False with no swing
    /// to read (the mode off, the point behind the camera, a limb that does not move).
    bool jointModalSwing(QPointF& direction, double& leverPx) const;
    int              m_modalKind = -1;               // the dial the mouse turns, or -1
    QPointF          m_modalCursor;                  // the cursor at the last move
    bool             m_modalFollow = false;          // the limb follows the mouse (else left-right turns the dial)
    QPointF          m_modalDirection{1.0, 0.0};     // the screen direction of "more", as last read
    Qt::MouseButtons m_modalSwallow = Qt::NoButton;  // presses the mode consumed: their releases are its too

    // =========================================================================================
    // Diagnostics (vulkanwindow_bench.cpp; see ikdiagnostics.h)
    // =========================================================================================

    // IK loop diagnostics (POSESTUDIO_IK_PERF=1) and the scripted IK benchmark
    // (POSESTUDIO_IK_BENCH=<bone>[:depth], e.g. lHand): the benchmark runs a plain (full-body-IK)
    // drag of that joint along a fixed cursor path with NO desktop input — the real timer, solve,
    // render, and present chain — and prints per-phase tick/frame intervals and the grabbed
    // joint's distance from the (virtual) cursor, then exits. This is how the loop's real rate
    // and the user-perceived lag are measured on the actual machine (the harness assumes 60 Hz
    // ticks). m_diag is null unless one of the env vars is set — the hot paths test only it.
    void startBench();
    void benchAdvance();
    void benchDepthNotch(int notch);
    void benchFinish();
    std::unique_ptr<IkDiagnostics> m_diag;

    // =========================================================================================
    // The scripted in-app posing test (vulkanwindow_script.cpp; POSESTUDIO_IK_SCRIPT=<file>, a
    // figure on the command line): camera views, REAL picks and presses at pixels, cursor drags
    // in screen space through the real drag plane, releases, pins, FK turns — each through the
    // window's own gesture functions, with NO desktop input — plus `report` (what a user would
    // see, as numbers) and `shot` (the rendered frame, read back from the GPU, as a PNG). Then
    // it exits. m_script is null unless the env var is set; tools/ikscripts/README.md is the reference.
    void startScript();          ///< The first step, once a posable figure exists (polled).
    void scriptStep();           ///< Runs lines until one must wait for the app; the wait's end re-calls it.
    bool scriptPixelOf(const QString& spec, QPointF& px); ///< "<bone>[@share]" -> the pixel a user would click.
    void scriptDragTick();       ///< One tick of a scripted move (from the IK tick, before its solve).
    void scriptSettled();        ///< A release's settle finished: the `release` line's wait is over.
    void scriptFrameRendered();  ///< A frame was presented after `shot`: save the capture, resume.
    void scriptReport(const char* label); ///< `report`: what a user would see, as numbers.
    bool scriptMetric(const QString& name, double& value); ///< A number `expect` can assert on.
    std::unique_ptr<IkScript> m_script;

    // =========================================================================================
    // VR Mode (vulkanwindow_vr.cpp)
    // =========================================================================================

    /// The session's poll timer tick: runs the OpenXR state machine (begin on Ready, end on
    /// Stopping, tear down on Gone) and starts/stops the frame-wait thread with it.
    void onVrPoll();
    /// The GUI-thread half of a VR frame: the wait thread has a predicted display time —
    /// beginFrame, locate the eyes, render both into the headset's images, endFrame, then
    /// request the desktop mirror. Strictly one frame at a time.
    void onVrFrameReady();
    /// Starts / stops the thread that blocks in xrWaitFrame (the ONE OpenXR call off the GUI
    /// thread, so the headset's pacing never stalls the UI).
    void startVrWaiter();
    void stopVrWaiter();
    /// Ends VR Mode: stops the wait thread, releases the eye targets and the session, emits
    /// vrModeChanged(false) and, with a non-empty @p error, vrError. Idempotent.
    void stopVr(const QString& error);
    /// Where the user should stand to look at the scene: in front of the framed bounds.
    glm::vec3 vrStandPoint() const;
    struct VrState; // complete only in vulkanwindow_vr.cpp (see m_vr below)
    /// THE CONTROLLERS (one frame's input, VrInput): locomotion on the play-space transform —
    /// the left stick walks head-relative, the right stick turns smoothly about the head — then each
    /// hand's pointer ray: a hover pick colours it and ends it on the joint, the trigger is the
    /// LEFT MOUSE BUTTON (a press on a joint selects it and begins the full-body-IK drag of it,
    /// the grabbed point then carried on the ray at its grab distance while the trigger is held,
    /// the right stick's Y pushing/pulling it; a press on empty space click-selects the model
    /// under the ray or clears the selection), the face button pins / unpins the selected joint,
    /// the menu button recentres. Called per VR frame before the eyes are located, so the frame
    /// shows the move.
    void applyVrInput(VrState& vr, const VrInput& input, float dt, int64_t time);
    /// Ends a controller-driven IK drag (as the mouse's release would), if one is in flight.
    void endVrDrag();

    // VrState is complete only in vulkanwindow_vr.cpp (it holds the OpenXR session and the wait
    // thread); the deleter is defined there too, so the other TUs of this class can hold the
    // pointer without knowing the type.
    struct VrStateDeleter {
        void operator()(VrState* state) const;
    };
    std::unique_ptr<VrState, VrStateDeleter> m_vr; // null = VR Mode off
    OpenXrSystem*            m_xr = nullptr;   // borrowed (ViewportWidget); null = no headset
    uint64_t                 m_vrGeneration = 0; // ++ per start/stop: stale queued frames are ignored
};

} // namespace pose

#endif // VULKANWINDOW_H
