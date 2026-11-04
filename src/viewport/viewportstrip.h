/**
 * @file viewportstrip.h
 * @brief The control strip floating in the viewport's top-right corner: the shading-mode and
 *        view pickers, the Home / Ground / Skeleton buttons, and the axis-rotate badge.
 *
 * The strip is a TOP-LEVEL window, not a child widget: the viewport is a native child window
 * (QWidget::createWindowContainer), which composites on top of every ordinary sibling widget,
 * so anything that must paint over it has to be its own window positioned over the container
 * in global coordinates (the SplashOverlay rule). It is `Qt::Tool | Qt::FramelessWindowHint`:
 * a plain Qt::Window is a full application window that X11/Wayland list in the taskbar and
 * stack wherever the WM pleases (the strip showed up as a second window BEHIND the main one on
 * Linux); a tool window stays out of the window list and is kept above its owner.
 *
 * The strip knows NOTHING about the Vulkan window: it only emits what the user picked and
 * accepts what state to show. ViewportWidget is the sole wiring point between the two, which
 * is also what keeps teardown safe — the strip can be deleted after the window without a
 * dangling connection firing into it.
 */

#ifndef VIEWPORTSTRIP_H
#define VIEWPORTSTRIP_H

#include "viewpreset.h"

#include <QWidget>

class QAction;
class QPushButton;

namespace pose {

class JointModeBadge;
class MenuPickerButton;

/**
 * @class ViewportStrip
 * @brief The floating top-right control strip of the viewport.
 */
class ViewportStrip : public QWidget {
    Q_OBJECT
public:
    /// @p owner is the widget the strip belongs to (its transient parent for stacking).
    explicit ViewportStrip(QWidget* owner);

    /// Glues the strip to @p container's top-right corner (global coordinates) and remembers
    /// the container, so a change of the strip's own size (the badge row appearing) re-anchors.
    void anchorTo(const QWidget* container);

public slots:
    /// Mirrors the camera's current named view in the View picker's caption (no signal back).
    void setViewPreset(ViewPreset view);
    /// Mirrors the window's shade mode in the shader picker (no signal back — setCurrentIndex
    /// never re-fires currentChanged), the same pattern as setViewPreset.
    void setShadeMode(int mode);
    /// Mirrors the skeleton overlay's state in the Skeleton button (no signal back).
    void setSkeletonChecked(bool on);
    /// Shows the joint-mode badge for @p kind (0/1/2 = Bend / Side-Side / Twist: the dial the
    /// mouse is turning) or hides it (-1), re-anchoring the strip as its row appears/disappears.
    void setJointModeBadge(int kind);
    /// Whether VR Mode is on: the VR button lights up and the view picker's VR Mode entry is
    /// checked. The real state, from the viewport — the controls only ASK (vrModeToggled).
    void setVrActive(bool on);

signals:
    /// The user picked a shading mode: a row of scene/shademode.h's table.
    void shadeModeSelected(int mode);
    /// The user picked a named view (Home included; Free is never picked).
    void viewSelected(ViewPreset view);
    void homeClicked();
    void groundClicked();
    /// The user toggled the Skeleton button (not emitted by setSkeletonChecked).
    void skeletonToggled(bool on);
    /// The user asked to enter (@p on) or leave VR Mode — the VR button or the view picker's
    /// VR Mode entry. The shell decides (a headset-less launch gets an explanation instead); the
    /// outcome comes back through setVrActive.
    void vrModeToggled(bool on);

private:
    MenuPickerButton* m_shaderPicker = nullptr;  // "#ShaderModeButton"
    MenuPickerButton* m_viewPicker = nullptr;    // "#ViewPresetButton"
    QPushButton*      m_homeButton = nullptr;    // resets the camera to default framing
    QPushButton*      m_groundButton = nullptr;  // drops the figure onto the floor plane
    QPushButton*      m_skeletonButton = nullptr; // toggles the skeleton overlay (checkable)
    JointModeBadge*   m_modeBadge = nullptr;     // "the mouse is turning Bend / Side-Side / Twist", under the buttons
    QPushButton*      m_vrButton = nullptr;      // "VR", right of the view picker: enters / leaves VR Mode, lit while on
    QAction*          m_vrMenuAction = nullptr;  // "VR Mode" at the bottom of the view picker's menu
    const QWidget*    m_anchor = nullptr;        // the container anchorTo() last glued us to
};

} // namespace pose

#endif // VIEWPORTSTRIP_H
