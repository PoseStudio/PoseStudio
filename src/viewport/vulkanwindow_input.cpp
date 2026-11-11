/**
 * @file vulkanwindow_input.cpp
 * @brief VulkanWindow's mouse, wheel and keyboard handlers, the object context menu, and the
 *        cursor→world helpers.
 *
 * One of the seven translation units of VulkanWindow — see vulkanwindow.h for the map. The
 * handlers here are ROUTING: they decide which gesture a press/move/key belongs to and hand it
 * to the gesture's own code (the IK/fall/axis-hold machinery in vulkanwindow_ik.cpp, the pose
 * edits in vulkanwindow_pose.cpp, the camera in vulkanwindow_camera.cpp). The one rule every
 * branch obeys: a camera move acts only on buttons in m_activeDragButtons — pressed IN this
 * window — because a modal dialog closing over the native child window can leak a button-held
 * move to us and snap the camera (the "event-leak class" of bugs).
 */

#include "vulkanwindow.h"

#include "rendering/vulkanrenderer.h"
#include "scene/scene.h"

#include <QAction>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QStyleHints>
#include <QWheelEvent>

namespace pose {

namespace {
// Input tuning. Kept local for now; promote to Constants / a preference once the
// navigation-preferences panel grows a "viewport" section.
constexpr float kOrbitRadiansPerPixel = 0.008f;
constexpr float kPanPerPixel = 0.0015f;
constexpr float kDollyPerWheelStep = 0.12f; // per 120-unit wheel notch
} // namespace

Ray VulkanWindow::cursorRay(const QPointF& localPos) const {
    return m_renderer->camera().screenPointToRay(
        static_cast<float>(localPos.x()), static_cast<float>(localPos.y()),
        static_cast<float>(width()), static_cast<float>(height()));
}

int VulkanWindow::boneAt(const QPointF& localPos) {
    return scene().selectBoneAt(static_cast<float>(localPos.x()), static_cast<float>(localPos.y()),
                                static_cast<float>(width()), static_cast<float>(height()),
                                m_renderer->camera());
}

void VulkanWindow::mousePressEvent(QMouseEvent* event) {
    // THE JOINT MOUSE MODE owns the mouse while it is on (B / S / T: beginJointModal): a left
    // click keeps what it made, a right click puts the joint back — and neither is a press on
    // the scene: no selection, no drag, no context menu (the matching release is the mode's too).
    // Any other button does nothing until the mode is over.
    if (m_modalKind >= 0) {
        if (event->button() == Qt::LeftButton) {
            confirmJointModal();
        } else if (event->button() == Qt::RightButton) {
            cancelJointModal();
        }
        m_modalSwallow |= event->button();
        m_lastMousePos = event->position();
        return;
    }
    // A new interaction must not overlap a still-animating release settle, a figure still
    // falling (a drag captures the floor at its start), or an open dial edit (its pose snapshot
    // would be overwritten below). A live button-held drag is left alone here: a right press
    // while the left button drags is the context-menu-mid-drag case, handled by the menu's actions.
    closeSettlingEdits();
    m_lastMousePos = event->position();
    m_activeDragButtons |= event->button(); // a drag with this button started in the viewport

    // Left-press priority: (0) a joint -> select it + full-body-IK drag of it (the body follows:
    // feet pinned, auto-balanced) — THE posing gesture, no modifier; (1) Ctrl + a joint -> select
    // it + a SCOPED IK drag of it (the grabbed chain alone moves: a limb up to the body, the head
    // and neck, the spine over a still pelvis); (2) empty space -> orbit the camera — or, if
    // released without dragging, a CLICK that selects the model under the cursor / clears the
    // selection (see the release).
    if (event->button() == Qt::LeftButton && m_renderer) {
        m_leftClickCandidate = false;
        m_leftPressPos = event->position();
        // A left press while a left-button gesture is still flagged means its release never
        // reached this window (a modal/shortcut swallowed the mouse-up — the m_activeDragButtons
        // event-leak class): close it out, or the 60 Hz timer runs forever and this press hijacks
        // into IK-dragging the old joint.
        if (m_ik.dragging) {
            abortIkDrag();
        }
        m_ik.groupClickBone = -1;
        // (Shift: THE GROUP — the press extends the selection and drags the whole group; see
        // beginGroupGesture.)
        const bool began = event->modifiers().testFlag(Qt::ShiftModifier)
                               ? beginGroupGesture(event->position())
                               : beginJointGesture(event->position(),
                                                   event->modifiers().testFlag(Qt::ControlModifier));
        if (!began) {
            m_leftClickCandidate = true; // the orbit gesture, until it moves
        }
        requestUpdate(); // reflect the new selection (outline / skeleton highlight)
    }
}

bool VulkanWindow::hasSelection() const {
    return m_renderer && m_renderer->scene().selectedModelIndex() >= 0;
}

void VulkanWindow::clearSelection() {
    if (!m_renderer || m_ik.dragging || scene().selectedModelIndex() < 0) {
        return;
    }
    // (A release settle, the landing bounce, an open dial edit: landed and committed first, as a
    // press does — the bounce BORROWS the selection, and with it taken from under the bounce its
    // drag lost its effector and gave the user's joint back on a model no longer selected.)
    closeSettlingEdits();
    scene().setSelectedModel(-1); // and the joint selection with it
    requestUpdate();              // the outline goes away
}

void VulkanWindow::selectModelAtClick(const QPointF& localPos) {
    // Box-level pick (Model::intersectRay): the nearest model whose bounds the cursor's ray
    // enters, else -1 = clear the selection. Selecting a figure also makes it the posing target;
    // deselecting drops its joint selection with it (Scene::setSelectedModel).
    const Ray ray = cursorRay(localPos);
    const int picked = scene().pickModel(ray);
    if (picked != scene().selectedModelIndex()) {
        scene().setSelectedModel(picked);
        requestUpdate(); // the outline moved / went away
    }
    // The material zone under the click, for the Materials tab (a figure's joint pick records
    // it the same way; a static model's surface is cast here).
    if (picked >= 0 && scene().selectZoneAt(ray)) {
        requestUpdate(); // (the frame's notify tells the tab)
    }
}

void VulkanWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (m_modalSwallow & event->button()) {
        m_modalSwallow &= ~event->button(); // the click that ended the joint mouse mode: all of it
        return;
    }
    m_activeDragButtons &= ~event->button();
    if (event->button() == Qt::LeftButton) {
        if (dragInFlight() && m_renderer) {
            if (m_ik.dragging && !m_ik.poseChanged) {
                // A CLICK on a joint: it was selected but no solve ever moved the pose — end the
                // drag with no settle and no undo (the settle would still walk a hovering
                // figure onto its ground-healed pins, turning a mere selection into an edit).
                abortIkDrag();
                if (m_ik.groupClickBone >= 0) {
                    // A Shift+CLICK on a member of THE GROUP removes it (the press had made it the
                    // primary, for the drag that never came).
                    scene().removeGroupBone(m_ik.groupClickBone);
                    m_ik.groupClickBone = -1;
                }
            } else if (m_ik.dragging) {
                // IK release: hand off to the ANIMATED settle — the timer keeps ticking, each
                // tick relaxing the body onto its pins so hovering feet visibly land instead of
                // popping in one frame. The settled-pose hook + the undo commit run when it
                // finishes (finishIkSettle).
                beginIkRelease();
            }
            requestUpdate();
        } else if (m_leftClickCandidate && m_renderer) {
            // The orbit gesture that never moved: a CLICK. Within the platform's drag distance
            // it selects what's under the cursor (or clears the selection on empty space); a
            // real orbit — however short — leaves the selection alone, so orbiting around a
            // figure can't deselect it.
            const bool moved = (event->position() - m_leftPressPos).manhattanLength() >=
                               QGuiApplication::styleHints()->startDragDistance();
            if (!moved) {
                selectModelAtClick(event->position());
            }
        }
        m_leftClickCandidate = false;
        m_ik.dragging = false;
    }
    // Right-click (on release, the desktop convention) opens the object context menu. The right
    // button drives no camera motion, so there's nothing to disambiguate from a drag here.
    if (event->button() == Qt::RightButton) {
        showObjectContextMenu(event->position(), event->globalPosition().toPoint());
    }
}

void VulkanWindow::showObjectContextMenu(const QPointF& localPos, const QPoint& globalPos) {
    if (!m_renderer) {
        return;
    }
    // A joint under the cursor gets the pin actions (selecting it, so the skeleton overlay —
    // when shown — highlights which joint the menu acts on); the model under the cursor gets
    // Delete. Both can apply. Not while a pose edit owns the joints (a right-click mid-drag).
    const int joint = poseEditInFlight() ? -1 : boneAt(localPos);
    const int picked = scene().pickModel(cursorRay(localPos));
    if (joint < 0 && picked < 0) {
        return; // empty space — no menu (for now)
    }

    QMenu menu;
    QAction* pinAction = nullptr;
    QAction* unpinAllAction = nullptr;
    QAction* resetJointAction = nullptr;
    QAction* resetLimbAction = nullptr;
    QAction* mirrorLimbAction = nullptr;
    if (joint >= 0) {
        pinAction = menu.addAction(scene().selectedBonePinned() ? tr("Unpin Joint\tP")
                                                                : tr("Pin Joint\tP"));
        if (scene().hasPinnedBones()) {
            unpinAllAction = menu.addAction(tr("Unpin All Joints"));
        }
        // Pose utilities on the clicked joint (boneAt selected it): "limb" = the joint and
        // everything below it. The Edit menu carries the same plus the whole-pose variants.
        menu.addSeparator();
        resetJointAction = menu.addAction(tr("Reset Joint"));
        resetLimbAction = menu.addAction(tr("Reset Limb"));
        mirrorLimbAction = menu.addAction(tr("Mirror Limb to Other Side"));
        if (picked >= 0) {
            menu.addSeparator();
        }
    }
    QAction* deleteAction = picked >= 0 ? menu.addAction(tr("Delete")) : nullptr;
    QAction* chosen = menu.exec(globalPos);
    if (chosen != nullptr && chosen == resetJointAction) {
        runPoseUtility(PoseUtility::ResetJoint);
    } else if (chosen != nullptr && chosen == resetLimbAction) {
        runPoseUtility(PoseUtility::ResetLimb);
    } else if (chosen != nullptr && chosen == mirrorLimbAction) {
        runPoseUtility(PoseUtility::MirrorLimb);
    } else if (chosen != nullptr && chosen == pinAction) {
        togglePinSelectedJoint();
    } else if (chosen != nullptr && chosen == unpinAllAction) {
        unpinAllJoints();
    } else if (chosen != nullptr && chosen == deleteAction) {
        deleteModel(picked);
    }
    requestUpdate(); // selection highlight / pin markers changed even if nothing was chosen
}

void VulkanWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!m_renderer) {
        return;
    }
    const QPointF delta = event->position() - m_lastMousePos;
    m_lastMousePos = event->position();
    if (m_modalKind >= 0) {
        // The joint mouse mode: every move turns the selected joint's dial, button or no button
        // (a QWindow gets move events with none held). It requests its own frame when the pose moves.
        jointModalMove(event->position());
        return;
    }

    // Only act on buttons that are BOTH held now AND were pressed inside this window. Using
    // event->buttons() alone would let a leaked move (e.g. as the modal Import dialog closes
    // over us with a button still logically down) snap the camera from a stale last-position.
    const Qt::MouseButtons active = m_activeDragButtons & event->buttons();

    Camera& camera = m_renderer->camera();
    if ((active & Qt::LeftButton) && m_ik.dragging) {
        // Full-body IK: the grabbed joint tracks the cursor within the camera-parallel plane
        // through its grab point. The plane's depth changes ONLY by explicit wheel steps
        // (wheelEvent), never from the solve, so it can't feed back.
        glm::vec3 target;
        if (dragPlaneHit(event->position(), target)) {
            setIkTarget(target);
            // Deliberately NO solve here: the 60 Hz timer is the SOLE caller of
            // issueIkTarget(). Solving per mouse event stacked extra solves on top of the
            // timer's — with a high-polling-rate mouse the damped-motion dynamics (all tuned
            // in per-tick units) ran several times faster than designed.
        }
    } else if (active & Qt::LeftButton) {
        // Drag right -> orbit right; drag up -> tilt up. Negated to feel like grabbing the scene.
        camera.orbit(-static_cast<float>(delta.x()) * kOrbitRadiansPerPixel,
                     -static_cast<float>(delta.y()) * kOrbitRadiansPerPixel);
        if (!delta.isNull()) {
            noteView(ViewPreset::Free); // orbited away from whatever named view this was
            noteCameraChange();
        }
    } else if (active & Qt::MiddleButton) {
        camera.pan(static_cast<float>(delta.x()) * kPanPerPixel,
                   static_cast<float>(delta.y()) * kPanPerPixel);
        noteCameraChange();
    } else {
        // Plain hover (QWindow gets move events with no buttons held): nothing changed, so don't
        // schedule a frame — an unconditional requestUpdate() here redraws the whole scene at
        // mouse-move rate, exactly the idle churn event-driven rendering exists to avoid.
        return;
    }
    requestUpdate();
}

void VulkanWindow::wheelEvent(QWheelEvent* event) {
    if (!m_renderer) {
        return;
    }
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (m_ik.dragging && (m_activeDragButtons & event->buttons() & Qt::LeftButton)) {
        // DEPTH during a full-body-IK drag. The cursor only ever places the grabbed joint within
        // the camera-parallel drag plane, so without this every "bring the hand forward" needed a
        // release, an orbit, and a second drag. Each notch moves the drag plane along the view
        // direction — scroll up (the dolly-in direction) pushes the joint AWAY from the camera,
        // scroll down pulls it toward it — by a fraction of the plane's distance from the camera,
        // so the step matches the view's scale, and the raw target is re-derived from the
        // CURRENT cursor through the moved plane so the joint stays under the pointer while its
        // depth changes. The 60 Hz timer picks the new target up like any cursor motion (same
        // filter, same damped follower), so a notch reads as a smooth push rather than a jump, and the
        // Armature's floor clamp on the drag target still applies. The plane is kept at least
        // 10cm in front of the camera: pulled through it, the cursor ray could no longer hit it.
        stepIkDepth(steps, event->position());
        requestUpdate(); // the drag timer issues the moved target on its next tick
        return;
    }
    m_renderer->camera().dolly(steps * kDollyPerWheelStep);
    noteCameraChange();
    requestUpdate();
}

void VulkanWindow::focusOutEvent(QFocusEvent* event) {
    // The clicks and releases will never reach us: don't leave the joint mouse mode on (what it
    // made is kept, as a click would keep it — nothing springs back behind the user's back), and
    // don't leave the button mask set (it would keep blocking the view hotkeys). A live IK drag is
    // left to the stale close-out at the next press: its timer keeps re-issuing the last target,
    // which is harmless, and the mouse-up may still arrive.
    confirmJointModal();
    m_modalSwallow = Qt::NoButton;
    m_activeDragButtons = Qt::NoButton;
    m_leftClickCandidate = false;
    QWindow::focusOutEvent(event);
}

void VulkanWindow::keyPressEvent(QKeyEvent* event) {
    // Fallback path: the Edit-menu actions carry the app-wide Ctrl+Z/Ctrl+Y shortcuts, but keep
    // handling the raw keys here too in case a platform delivers them to the native window
    // without the shortcut map consuming them first.
    if (m_renderer && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Z) {
            undo();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Y) {
            redo();
            event->accept();
            return;
        }
    }
    // THE JOINT MOUSE MODE (see beginJointModal): B / S / T with a joint selected turns on its
    // Bend / Side-Side / Twist dial — moving the mouse then turns it, a left click (or Enter)
    // keeps the result, Esc, a right click or the same key again puts the joint back; the strip
    // shows the mode's badge. The Edit menu's actions carry these keys APPLICATION-WIDE (they
    // must work whichever panel has the focus); this is the in-viewport fallback, like Ctrl+Z
    // and the view keys, for a key the shortcut map did not take. No modifiers (Ctrl+S and the
    // like stay free), and the key's auto-repeats are swallowed: a key held a moment too long
    // must not toggle the mode off again. Refused while a drag owns the pose; a settle or fall
    // still animating is landed first.
    if (m_renderer && event->modifiers() == Qt::NoModifier &&
        (event->key() == Qt::Key_B || event->key() == Qt::Key_S || event->key() == Qt::Key_T)) {
        if (!event->isAutoRepeat()) {
            toggleJointModal(event->key() == Qt::Key_B ? 0 : event->key() == Qt::Key_S ? 1 : 2);
        }
        event->accept();
        return;
    }
    if (m_modalKind >= 0 && event->key() == Qt::Key_Escape) {
        cancelJointModal();
        event->accept();
        return;
    }
    if (m_modalKind >= 0 && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        confirmJointModal();
        event->accept();
        return;
    }
    // Camera views (Blender's numpad convention; the View menu's QAction shortcuts carry these
    // app-wide, this is the in-viewport fallback like Ctrl+Z). Not while a button drag owns the
    // camera or the pose.
    if (m_renderer && !dragInFlight() &&
        !(m_activeDragButtons & (Qt::LeftButton | Qt::MiddleButton)) && handleViewHotkey(event)) {
        event->accept();
        return;
    }
    // Delete — or Backspace, the key labelled Delete on Mac keyboards — removes the SELECTED
    // object, like the context menu's Delete. Handled here (viewport focus) rather than as a
    // window-level shortcut, which would hijack Delete from the Asset Manager's tree and grid.
    // Not mid-gesture: a drag owns the figure until it's released.
    if (m_renderer && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        event->modifiers() == Qt::NoModifier && !dragInFlight()) {
        deleteSelectedObject();
        event->accept();
        return;
    }
    // P toggles the pin on the selected joint (the joint is then held in place through IK drags
    // of other joints). Not mid-gesture: the rig captured its pins at drag start.
    if (m_renderer && event->key() == Qt::Key_P && event->modifiers() == Qt::NoModifier &&
        !dragInFlight() && scene().hasSelectedBone()) {
        togglePinSelectedJoint();
        event->accept();
        return;
    }
    QWindow::keyPressEvent(event);
}

} // namespace pose
