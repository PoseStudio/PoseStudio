/**
 * @file viewportwidget.h
 * @brief The 3D viewport as a plain QWidget — the ONLY thing the rest of the app touches.
 *
 * Owns the application's QVulkanInstance and embeds a VulkanWindow via
 * QWidget::createWindowContainer(), so callers (e.g. main.cpp) can drop it into a
 * layout like any other widget and stay completely unaware of Vulkan. If the Vulkan
 * instance can't be created (no driver / no GPU), it degrades to an inline message
 * rather than taking the whole app down.
 *
 * It is also the SOLE wiring point between the window and the floating control strip
 * (ViewportStrip): the strip emits what the user picked, the window reports the state it is in,
 * and neither knows the other — every facade method here forwards to the window, and every
 * strip signal lands on one of those facade methods, so the menus, the keys and the strip all
 * take the same path.
 */

#ifndef VIEWPORTWIDGET_H
#define VIEWPORTWIDGET_H

#include "scene/camera.h" // AxisView (the View menu's camera entries)

#include <QPointF>
#include <QStringList>
#include <QWidget>

#include <map>
#include <memory>
#include <string>
#include <vector>

class QVulkanInstance;
class QShowEvent;
class QHideEvent;
class QResizeEvent;
class QMoveEvent;

namespace pose {

class OpenXrSystem;
class ViewportStrip;
class VulkanWindow;
struct JointTransform;
class JointModalInput;
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

    /// Saves the whole scene to @p path as a .pss project file (File → Save / Save As). False on
    /// failure (no renderer, or the file can't be written).
    bool saveProjectFile(const QString& path);
    /// Loads a .pss project (File → Open): atomic — the scene is only touched once the whole
    /// document has parsed and every figure's source exists. @p recovered maps a missing source
    /// to its re-pointed replacement (link recovery). Returns 0 on success, 1 when sources are
    /// still missing (@p missing receives them), -1 on failure with a human-readable @p error.
    int loadProjectFile(const QString& path, const std::map<std::string, std::string>& recovered,
                        std::vector<std::string>& missing, std::string& error);
    /// The .pss document this viewport last saved/opened ("" = unsaved) and whether the scene has
    /// changed since (the close prompt and Save's no-op hint read it).
    QString projectPath() const;
    void setProjectPath(const QString& path);
    bool isProjectDirty() const;
    void setProjectClean();
    /// Marks the scene as having unsaved changes (a scene restored across a restart is unsaved
    /// work again — main.cpp's --restore-scene).
    void markProjectDirty();
    /// Sets the viewport shade mode by index into the picker's table (scene/shademode.h — the
    /// order shaderModeNames() lists). No-op if the viewport degraded.
    void setShadeMode(int mode);

    /// Deletes the selected (outlined) object — Edit → Delete; the viewport's own Delete key
    /// handling reaches the same place. No-op without a selection or if the viewport degraded.
    void deleteSelectedObject();

    /// Resets the scene to a fresh launch (File → New, and the base of Open): models, camera,
    /// lighting / HDRI, shade mode, skeleton and history back to startup defaults. No-op if the
    /// viewport degraded.
    void resetToEmptyScene();

    /// Pose utilities (Edit menu), all undoable pose edits on the active figure; no-ops without
    /// a figure/selection or if the viewport degraded. "Limb" = the selected joint and
    /// everything below it; Mirror Pose swaps the body's sides, Mirror Limb copies the selected
    /// limb, mirrored, onto the other side.
    void resetSelectedJoint();
    void resetSelectedLimb();
    void resetPose();
    void mirrorPose();
    void mirrorSelectedLimb();

    /// The Transform tab's view of the selected joint — its name and its three dials (Bend
    /// Forward, Bend Sideways, Twist: see VulkanWindow::jointTransform) — and the dial edit, one
    /// undo entry per begin..end however many values are set between. Invalid / no-ops if the
    /// viewport degraded; begin is false while a viewport drag owns the pose.
    JointTransform jointTransform() const;
    bool beginJointTransformEdit();
    void setJointTransformDial(int dial, double value);
    void endJointTransformEdit();
    /// The B / S / T key, wherever the keyboard focus is (the Edit menu's actions carry the keys
    /// application-wide): the joint mouse mode on dial @p kind (0/1/2 = Bend / Side-Side /
    /// Twist) of the selected joint — on, off again, or switched to it
    /// (VulkanWindow::toggleJointModal). While it is on, the mouse and Esc / Enter are the
    /// mode's anywhere in the application (JointModalInput).
    void toggleJointModal(int kind);

    /// Returns the camera to the default perspective framing (the strip's Home button, the 5
    /// key). No-op if the viewport degraded.
    void resetView();

    /// Camera views in Blender's numpad convention. The View menu's QActions carry the keys
    /// APP-WIDE (1/3/7 + Ctrl for the opposite side, 9 = flip, 5 = Home, "." = frame selected,
    /// number row and keypad alike); the window handles the same keys itself only as the
    /// fallback for a platform that hands the native window the key first. No-ops if the
    /// viewport degraded.
    void setAxisView(AxisView view);
    void flipView();
    void frameSelected();

    /// Drops the posable figure onto the ground plane (the strip's Ground button): moves it so
    /// the current pose's lowest point rests at y = 0. No-op if the viewport degraded.
    void groundFigure();

    /// Toggles the skeleton overlay (the joint→parent bone lines drawn over the figure). Off by
    /// default — joints are grabbed directly on the figure — but joints stay clickable either way.
    /// Driven by the strip's Skeleton button and the View menu (kept in sync via the
    /// skeletonVisibilityChanged signal). No-op if the viewport degraded.
    void setShowSkeleton(bool on);
    bool showSkeleton() const;

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

    /// The user-facing shade-mode names in picker order — the names of scene/shademode.h's table,
    /// which is the single source of truth (each row also says how the scene draws in that mode).
    static QStringList shaderModeNames();

    /// VR Mode (View → VR Mode): the scene in the headset found at startup, live, with the window
    /// mirroring the headset's left eye. Returns whether VR is on afterwards; a start that fails
    /// explains itself through vrError. Whether it CAN start is VrSettings::ready() — the shell
    /// consults that before calling, so a headset-less launch gets the explanation instead.
    /// No-op if the viewport degraded.
    bool setVrMode(bool on);
    bool vrModeActive() const;
    /// Puts the user back in front of the figure, facing it (View → Recenter VR View).
    void recenterVr();

signals:
    /// Re-emitted from the viewport when VR Mode starts or ends (by the user or because the
    /// headset session was lost); the View menu's checkable entry follows it.
    void vrModeChanged(bool on);
    /// Re-emitted from the viewport with a user-readable message when VR Mode could not start or
    /// was ended by the runtime. The shell shows it.
    void vrError(const QString& message);
    /// Re-emitted from the viewport when undo/redo restores a lighting state, so the Environment
    /// panel can sync its widgets. The settings are already applied renderer-side.
    void lightingRestored(const LightingSettings& settings);
    /// Emitted whenever the skeleton overlay's visibility changes (from the strip's Skeleton
    /// button or the View menu), so the two controls stay in sync. The renderer state is already
    /// updated by the time this fires.
    void skeletonVisibilityChanged(bool visible);
    /// Re-emitted from the viewport when the selected joint's transform changed — another joint
    /// selected, or its pose moved by any means (VulkanWindow::jointTransformChanged). The
    /// Transform tab re-reads jointTransform() on it.
    void jointTransformChanged();
    /// Re-emitted from the viewport when the .pss document's state changes (path adopted, dirty
    /// flag transitioned), so the main window can keep its title in sync. The window-title update
    /// listens to this; the state itself is read back through projectPath() / isProjectDirty().
    void documentChanged();
    /// Re-emitted from the viewport when a loaded project put a panorama up (File → Open): the
    /// environment now in use, for the Environment tab's HDRI caption (VulkanWindow::environmentRestored).
    void environmentRestored(const QString& path);

protected:
    // The control strip is a *top-level* window floating over the native viewport (a child widget
    // would be composited behind it), so it has to be re-anchored as the viewport moves/resizes.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Builds the strip and wires it to the window — the one place the two meet.
    void createStrip();
    /// Glues the strip to the container's top-right corner (no-op while hidden / degraded).
    void anchorStrip();
    /// Applies @p f to the window — the body of every facade forwarder. No-op when the viewport
    /// degraded (no window).
    template <class F>
    void withWindow(F&& f) {
        if (m_window) {
            f(*m_window);
        }
    }

    std::unique_ptr<OpenXrSystem>    m_xr;                     // the headset found at startup (VR enabled in Preferences); null = none
    std::unique_ptr<QVulkanInstance> m_instance;               // owns the VkInstance
    VulkanWindow*                    m_window = nullptr;        // owned by m_container
    JointModalInput*                 m_modalInput = nullptr;    // the joint mouse mode's app-wide input (a child)
    QPointF                          m_scriptCursor;            // the scripted test's `app` cursor, global
    QWidget*                         m_container = nullptr;     // the createWindowContainer wrapper
    ViewportStrip*                   m_strip = nullptr;         // the floating top-right control strip
    QWidget*                         m_filteredWindow = nullptr; // top-level we filter for move/resize
};

} // namespace pose

#endif // VIEWPORTWIDGET_H
