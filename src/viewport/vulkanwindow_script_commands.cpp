/**
 * @file vulkanwindow_script_commands.cpp
 * @brief The scripted in-app test's COMMANDS, by family: what each line of a script does.
 *
 * One of the three files of the script runner on VulkanWindow (see vulkanwindow_script.cpp, the
 * step driver, and ikscript.h). The driver logs a line and asks each family here in turn; a
 * family answers ScriptFlow::NotMine for a command that is not its own (or has too few words:
 * the driver then logs it as ignored), Next when the line is done, and Wait when the line now
 * waits for the app — a drag's ticks, a release's settle, a shot's frame — whose end calls the
 * driver again.
 *
 * Every command goes through the window's OWN functions — the picker, the press path, the dial
 * bracket, the handlers — never through desktop input, and prints what it did as `[ikscript]`
 * lines: those lines are a baseline compared byte for byte between runs. The commands of a
 * feature that has a translation unit of its own are forwarded there (`render`, `savematerial`,
 * `applymaterial`, `lamp`). tools/ikscripts/README.md is the command reference: a command added
 * here is added there.
 */

#include "vulkanwindow.h"

#include "ikscript.h"
#include "importformats.h" // the scripted `import`'s route by extension
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/ik/ikrig.h"
#include "scene/scene.h"
#include "vulkanwindow_script_detail.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pose {

using namespace scriptdetail;

namespace {

/// The bone a "<bone>[@share]" spec NAMES, as this figure calls it (@p name), and the joint a
/// click on it selects (a face-rig bone's name: the head) — -1 where the figure has no such bone.
/// What `pin` and `press` compare the pick with: the pick is honest, and on another body it is
/// not always the bone the script names.
int namedPosingBone(const Armature* arm, const QString& spec, std::string& name) {
    name = figureBoneName(arm, spec.section(QLatin1Char('@'), 0, 0).toStdString());
    return arm != nullptr ? arm->knownPosingBone(arm->boneIndex(name)) : -1;
}

} // namespace

// --- The camera: view, orbit, frame, lookat, closeup, shade ----------------------------------
ScriptFlow VulkanWindow::scriptCameraCommand(const QString& cmd, const QStringList& w, const QString& /*line*/) {
    const WordNumber num{w};
    if (cmd == QLatin1String("view") && w.size() > 1) {
        const QString v = w[1].toLower();
        if (v == QLatin1String("home")) {
            resetView();
        } else {
            const AxisView axis = v == QLatin1String("back")     ? AxisView::Back
                                  : v == QLatin1String("left")   ? AxisView::Left
                                  : v == QLatin1String("right")  ? AxisView::Right
                                  : v == QLatin1String("top")    ? AxisView::Top
                                  : v == QLatin1String("bottom") ? AxisView::Bottom
                                                                 : AxisView::Front;
            setAxisView(axis);
        }
    } else if (cmd == QLatin1String("orbit")) {
        // orbit <yawDeg> <pitchDeg>: what an orbit drag does (and, like one, back to perspective)
        m_renderer->camera().orbit(glm::radians(num(1)), glm::radians(num(2))); // (degrees in, radians to the camera)
        requestUpdate();
    } else if (cmd == QLatin1String("frame")) {
        frameSelected();
    } else if (cmd == QLatin1String("lookat") && w.size() > 4) {
        // lookat <x> <y> <z> <radius m>: the camera framed on a WORLD point — a splat scene's
        // face or hands, which no bone names (closeup frames a joint).
        m_renderer->camera().frame(glm::vec3(num(1), num(2), num(3)), std::max(0.02f, num(4)));
        requestUpdate();
    } else if (cmd == QLatin1String("closeup") && w.size() > 2) {
        // closeup <bone> <radius m>: the camera framed on that joint — a hand, a foot, the face
        // (what a user does with the wheel and a pan before posing a finger).
        const Armature* arm = scene().figureArmature();
        const int       bone = scriptBone(arm, w[1].toStdString());
        if (bone >= 0) {
            m_renderer->camera().frame(arm->boneWorldPosition(static_cast<std::size_t>(bone)), std::max(0.02f, num(2)));
            requestUpdate();
        } else {
            say("[ikscript]   unknown bone\n");
        }
    } else if (cmd == QLatin1String("shade") && w.size() > 1) {
        setShadeMode(w[1].toInt());
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- Pose edits: pose, reset, undo, redo, ground, lift, fk, the Figure rows (move, scale, turn /
//     tip / tilt), dial, and the joint mouse mode (modal, app, modalmove, modalkeep / modaldrop) ---
ScriptFlow VulkanWindow::scriptPoseCommand(const QString& cmd, const QStringList& w, const QString& line) {
    const WordNumber num{w};
    if (cmd == QLatin1String("pose") && w.size() > 1) {
        if (!loadPose(line.section(QLatin1Char(' '), 1).trimmed())) {
            say("[ikscript]   pose not loaded\n");
        }
    } else if (cmd == QLatin1String("reset")) {
        runPoseUtility(PoseUtility::ResetPose);
    } else if (cmd == QLatin1String("undo")) {
        undo();
    } else if (cmd == QLatin1String("redo")) {
        redo();
    } else if (cmd == QLatin1String("ground")) {
        groundFigure();
    } else if (cmd == QLatin1String("lift") && w.size() > 1) {
        // lift <metres>: the active figure raised that far off the floor, as posed — what a pose
        // that bends the knees, or a pose file, leaves a figure at. `ground` then drops her.
        closeOpenPoseEdits();
        scene().translateModelY(scene().activeFigureIndex(), num(1));
        requestUpdate();
    } else if (cmd == QLatin1String("fk") && w.size() >= 5) {
        // fk <bone> <dx> <dy> <dz>: the joint turned by that many degrees about its own
        // channels, as a Transform dial turns one (through the limits and the FK collision
        // stop), as one undoable edit.
        if (scene().selectBoneByName(figureBoneName(scene().figureArmature(), w[1].toStdString())) >= 0) {
            closeOpenPoseEdits();
            m_preEditPose = scene().capturePose();
            scene().nudgeSelectedBone(glm::vec3(num(2), num(3), num(4)));
            scene().finalizePose();
            commitPoseUndo();
            requestUpdate();
        } else {
            say("[ikscript]   unknown bone\n");
        }
    } else if (cmd == QLatin1String("move") && w.size() > 3) {
        // move <dx> <dy> <dz>: the whole figure moved by that, in world centimetres (the
        // Transform tab's Figure group: her left, up, forward), one undoable edit.
        if (beginJointTransformEdit()) {
            const JointTransform::Figure fig = jointTransform().figure;
            setFigurePosition(0, fig.x + num(1));
            setFigurePosition(1, fig.y + num(2));
            setFigurePosition(2, fig.z + num(3));
            endJointTransformEdit();
        } else {
            say("[ikscript]   move refused (a drag owns the pose)\n");
        }
    } else if (cmd == QLatin1String("scale") && w.size() > 1) {
        // scale <percent>: a STATIC model's uniform scale (the Figure group's Scale row; a
        // figure keeps her own size).
        if (beginJointTransformEdit()) {
            setFigureScale(num(1));
            endJointTransformEdit();
        } else {
            say("[ikscript]   scale refused (a drag owns the pose)\n");
        }
    } else if ((cmd == QLatin1String("turn") || cmd == QLatin1String("tip") || cmd == QLatin1String("tilt")) && w.size() > 1) {
        // turn / tip / tilt <degrees>: the whole figure's rotation about the vertical, about
        // her own lateral axis (forward positive), about her own forward axis (her left down
        // positive) — the Figure group's three rows.
        if (beginJointTransformEdit()) {
            setFigureRotation(cmd == QLatin1String("turn") ? 0 : cmd == QLatin1String("tip") ? 1 : 2, num(1));
            endJointTransformEdit();
        } else {
            say("[ikscript]   %s refused (a drag owns the pose)\n", qPrintable(cmd));
        }
    } else if (cmd == QLatin1String("dial") && w.size() > 2) {
        // dial <forward|sideways|twist> <value> [abs]: the selected joint's Transform-tab dial set
        // to that value, the way the tab does it (one bracketed, undoable edit); with a group the
        // others turn by the same change — or, `abs`, to the value itself (the restore buttons).
        const QString which = w[1].toLower();
        const int     dial = which == QLatin1String("forward") ? 0 : which == QLatin1String("sideways") ? 1 : 2;
        if (beginJointTransformEdit()) {
            setJointTransformDial(dial, w[2].toDouble(), w.size() > 3 && w[3].toLower() == QLatin1String("abs"));
            endJointTransformEdit();
        } else {
            say("[ikscript]   dial refused (a drag owns the pose)\n");
        }
    } else if (cmd == QLatin1String("modal") && w.size() > 1) {
        // modal <bend|side|twist>: the B / S / T key — a key event handed to the window's own
        // keyPressEvent, so the routing is the real one (the same key again drops the mode,
        // another of the three switches). The mode reads the desktop's cursor at the key; the
        // script's stands at the middle of the view instead: only its moves count.
        const QString which = w[1].toLower();
        const int     key = (which == QLatin1String("bend") || which == QLatin1String("forward")) ? Qt::Key_B
                            : (which == QLatin1String("side") || which == QLatin1String("sideways")) ? Qt::Key_S
                                                                                                      : Qt::Key_T;
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        keyPressEvent(&press);
        m_modal.cursor = QPointF(width() * 0.5, height() * 0.5);
        m_pointer.lastPos = m_modal.cursor;
        say("[ikscript]   joint mouse mode %s\n", m_modal.kind >= 0 ? "on" : "off");
    } else if (cmd == QLatin1String("app") && w.size() > 1) {
        // app <b|s|t|esc|enter|click|rclick> / app move <dxPx> <dyPx> [steps]: the same input
        // arriving at the APPLICATION with the viewport not focused — the Edit menu's action
        // for the key, and key and mouse events delivered to the main window, where the mode's
        // application-wide filter (JointModalInput) must pick them up. ViewportWidget makes them.
        // (The menu's actions are enabled by the joint the Transform state last ANNOUNCED, which
        // follows the rendered frame; the script does not wait for one: bring it up to date.)
        notifyJointTransform();
        const QString what = w[1].toLower();
        const bool    move = what == QLatin1String("move") && w.size() > 3;
        emit appInputRequested(what, move ? QPointF(num(2), num(3)) : QPointF(),
                               move ? std::max(1, w.size() > 4 ? w[4].toInt() : 20) : 1);
        say("[ikscript]   joint mouse mode %s\n", m_modal.kind >= 0 ? "on" : "off");
    } else if (cmd == QLatin1String("modalmove") && w.size() > 2) {
        // modalmove <dxPx> <dyPx> [steps]: the mouse moved that far (+y down) with no button
        // held, in that many move events (20 by default) through the window's mouseMoveEvent.
        const int     steps = std::max(1, w.size() > 3 ? w[3].toInt() : 20);
        const QPointF step(num(1) / steps, num(2) / steps);
        for (int k = 0; k < steps; ++k) {
            const QPointF at = m_pointer.lastPos + step;
            QMouseEvent   move(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            mouseMoveEvent(&move);
        }
    } else if (cmd == QLatin1String("modalkeep") || cmd == QLatin1String("modaldrop")) {
        // modalkeep [enter] / modaldrop [esc]: the left click (or Enter) that keeps the result /
        // the right click (or Esc) that puts the joint back — press AND release through the
        // window's handlers, so a click that leaked through the mode would show as a changed
        // selection, a drag, or the context menu it must not open.
        const bool keep = cmd == QLatin1String("modalkeep");
        if (w.size() > 1) {
            QKeyEvent press(QEvent::KeyPress, keep ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
            keyPressEvent(&press);
        } else {
            const Qt::MouseButton button = keep ? Qt::LeftButton : Qt::RightButton;
            const QPointF         at = m_pointer.lastPos;
            QMouseEvent down(QEvent::MouseButtonPress, at, mapToGlobal(at), button, button, Qt::NoModifier);
            mousePressEvent(&down);
            QMouseEvent up(QEvent::MouseButtonRelease, at, mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
            mouseReleaseEvent(&up);
        }
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- The mouse on the figure: pin, sclick, unpinall, pick2d, clickat / clickpx, click, press /
//     cpress / spress, drag / dragw / dragv, wheel, release ------------------------------------
ScriptFlow VulkanWindow::scriptGestureCommand(const QString& cmd, const QStringList& w, const QString& /*line*/) {
    IkScript&        s = *m_script;
    const WordNumber num{w};
    if (cmd == QLatin1String("pin") && w.size() > 1) {
        // The joint a CLICK there finds — and, since the pick is the SKIN's (2026-09-26), the named
        // bone by name where the click lands on another part of the body (a hanging hand covers
        // the hip from the side: the click pinned a finger, and a "seated, hips pinned" lean had no
        // seat), as press does.
        QPointF px;
        if (scriptPixelOf(w[1], px)) {
            const int picked = boneAt(px);
            const Armature* found = scene().figureArmature();
            std::string     name;
            const int       wanted = namedPosingBone(found, w[1], name);
            if (wanted >= 0 && (picked < 0 || !found->sameRigidSegment(picked, wanted))) {
                if (picked >= 0) {
                    say("[ikscript]   the pick found %s there, not %s: pinned by name instead\n",
                        found->boneName(static_cast<std::size_t>(picked)).c_str(), name.c_str());
                }
                scene().selectBoneByName(name);
            }
            if (scene().hasSelectedBone()) {
                togglePinSelectedJoint();
            }
        }
    } else if (cmd == QLatin1String("sclick") && w.size() > 1) {
        // sclick <bone>: the Shift+CLICK at the bone's pixel — adds the joint to THE GROUP (as its
        // primary) or removes a member; prints the group afterwards.
        const Armature*        before = scene().figureArmature();
        const int              prevPrimary = before != nullptr ? before->selectedBone() : -1;
        const std::vector<int> prevGroup = scene().groupBones();
        QPointF px;
        if (scriptPixelOf(w[1], px)) {
            scene().restoreSelection(prevPrimary, prevGroup); // (the lookup selected the bone by name: see spress)
            closeOpenPoseEdits();
            const bool began = beginGroupGesture(px);
            if (began) {
                const int clicked = m_ik.groupClickBone;
                abortIkDrag();
                if (clicked >= 0) {
                    scene().removeGroupBone(clicked);
                    m_ik.groupClickBone = -1;
                }
            }
            const Armature* arm = scene().figureArmature();
            std::string     members = arm && arm->selectedBone() >= 0
                                          ? arm->boneName(static_cast<std::size_t>(arm->selectedBone()))
                                          : std::string("nothing");
            for (const int member : scene().groupBones()) {
                members += " + " + (arm ? arm->boneName(static_cast<std::size_t>(member)) : std::string("?"));
            }
            say("[ikscript]   shift-click at px(%.0f %.0f): the group is %s\n", px.x(), px.y(), members.c_str());
            requestUpdate();
        }
    } else if (cmd == QLatin1String("unpinall")) {
        unpinAllJoints();
    } else if (cmd == QLatin1String("pick2d") && w.size() > 1) {
        scene().setPick2D(w[1] == QLatin1String("on")); // (the A/B probe: the 2D pick alone, as before 2026-09-26)
    } else if ((cmd == QLatin1String("clickat") && w.size() > 3) ||
               (cmd == QLatin1String("clickpx") && w.size() > 2)) {
        // clickat <x> <y> <z>: a real LEFT CLICK — a press and its release, no move between —
        // at the pixel a WORLD point (metres) is seen at, through the window's own handlers:
        // exactly what a user's click there does (a joint taken and let go, the model under
        // the cursor selected, or the selection cleared). `selection` then reads the model.
        // clickpx <x%> <y%>: the same at a point of the VIEW, in percent of its width and
        // height from the top left (so a script does not depend on the window's size).
        QPointF    at;
        const bool byPixel = cmd == QLatin1String("clickpx");
        if (byPixel) {
            at = QPointF(num(1) * 0.01 * width(), num(2) * 0.01 * height());
        }
        if (m_renderer && (byPixel || projectToScreen(glm::vec3(num(1), num(2), num(3)), at))) {
            QMouseEvent down(QEvent::MouseButtonPress, at, mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
            mousePressEvent(&down);
            QMouseEvent up(QEvent::MouseButtonRelease, at, mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                           Qt::NoModifier);
            mouseReleaseEvent(&up);
            say("[ikscript]   click at px(%.0f %.0f): model %d is selected\n", at.x(), at.y(),
                scene().selectedModelIndex());
        } else {
            say("[ikscript]   clickat: the point is not in view\n");
        }
        requestUpdate();
    } else if (cmd == QLatin1String("click") && w.size() > 1) {
        // click <bone>[@share] [dxPx dyPx]: the pick at the bone's pixel — moved by that many
        // pixels where given (+y down): the tip of a toe lies beyond its last joint's pixel.
        QPointF px;
        if (scriptPixelOf(w[1], px)) {
            px += QPointF(num(2), num(3));
            const int picked = boneAt(px);
            const Armature* arm = scene().figureArmature();
            say("[ikscript]   click at px(%.0f %.0f): picked %s\n", px.x(), px.y(),
                picked >= 0 && arm ? arm->boneName(static_cast<std::size_t>(picked)).c_str() : "nothing");
            requestUpdate();
        }
    } else if ((cmd == QLatin1String("press") || cmd == QLatin1String("cpress") || cmd == QLatin1String("spress")) &&
               w.size() > 1) {
        // The real press: the picker at the pixel, then the press path (the IK drag, which takes its undo snapshot).
        // cpress is the Ctrl press (IkScope::Part): the SCOPED drag of the grabbed chain alone for
        // a limb, the head or the spine, the FIGURE MOVE for a grab of the hips.
        const bool scoped = cmd == QLatin1String("cpress");
        // spress is the SHIFT press (THE GROUP): the pressed joint joins the selection as its primary
        // and the whole group is dragged through it.
        const bool shift = cmd == QLatin1String("spress");
        // (scriptPixelOf selects the named bone to find its pixel, which a plain press relies on;
        // the Shift press must find the selection AS IT WAS — a member pressed again is the
        // primary, a selected joint not pressed becomes a secondary — so it is put back.)
        const Armature*        before = scene().figureArmature();
        const int              prevPrimary = before != nullptr ? before->selectedBone() : -1;
        const std::vector<int> prevGroup = scene().groupBones();
        QPointF px;
        if (!scriptPixelOf(w[1], px)) {
            return ScriptFlow::Next;
        }
        if (shift) {
            scene().restoreSelection(prevPrimary, prevGroup);
        }
        closeOpenPoseEdits();
        bool began = (shift ? beginGroupGesture(px) : beginJointGesture(px, scoped)) && m_ik.dragging;
        // The pick is honest: what it finds is what is under that pixel. On another body that is
        // not always the bone the script names — a broad character's hanging arm covers her chest
        // from the side, one hand covers the other — and the gesture that follows is then some
        // other gesture (a whole gallery of "chest" drags once hauled an upper arm about). When
        // the pick lands on ANOTHER PART of the body — the rig would drag a different joint than
        // the one named — the bone is grabbed by name instead, at the same point, and the log says
        // so. (A small bone OF the named one, a pectoral for the chest, is what a user's click
        // finds too, and stands: the rig drags the named joint.)
        {
            const Armature* found = scene().figureArmature();
            std::string     name;
            const int       wanted = namedPosingBone(found, w[1], name);
            const QString   share = w[1].section(QLatin1Char('@'), 1, 1);
            // (A DIGIT drag has no rig effector: what it drags is the selected digit's joint.)
            const int         dragged = found && found->ikDigitDrag() ? found->selectedBone()
                                        : found && found->ikRig() ? found->ikRig()->dragEffector() : -1;
            if (!shift && began && found && wanted >= 0 && !found->sameRigidSegment(found->selectedBone(), wanted) &&
                found->ikRig() && dragged != wanted) {
                const std::string other = found->boneName(static_cast<std::size_t>(found->selectedBone()));
                abortIkDrag();
                scene().selectBoneByName(name);
                if (!share.isEmpty()) {
                    scene().setIkGrabOnSegment(share.toFloat());
                }
                began = beginIkDrag(scoped);
                say("[ikscript]   the pick found %s there, not %s: grabbed by name instead\n", other.c_str(), name.c_str());
            }
        }
        if (!began) {
            say("[ikscript]   press at px(%.0f %.0f) began no drag\n", px.x(), px.y());
            return ScriptFlow::Next;
        }
        const Armature* arm = scene().figureArmature();
        s.dragging = true;
        s.cursorPx = px;
        s.cursorWorld = m_ik.planePoint;
        s.jumpMax = 0.0f;
        s.jumpBone = -1;
        s.jumpTick = s.dragTicks = 0;
        s.pressPositions = s.lastPositions = jointPositions(arm);
        say("[ikscript]   pressed %s at px(%.0f %.0f), grab point (%.3f %.3f %.3f)\n",
            arm && arm->selectedBone() >= 0 ? arm->boneName(static_cast<std::size_t>(arm->selectedBone())).c_str() : "?",
            px.x(), px.y(), m_ik.planePoint.x, m_ik.planePoint.y, m_ik.planePoint.z);
        requestUpdate();
    } else if ((cmd == QLatin1String("drag") || cmd == QLatin1String("dragw") || cmd == QLatin1String("dragv")) &&
               s.dragging) {
        // drag <dxPx> <dyPx> <moveTicks> <holdTicks>: the cursor moves in SCREEN space (logical
        // pixels, +y DOWN, from where it is) — through the real drag plane, as the mouse does.
        // dragw <dx> <dy> <dz> <moveTicks> <holdTicks>: the cursor's WORLD target moves (metres)
        // — the harness's gestures, for parity.
        // dragv <right> <up> <moveTicks> <holdTicks>: a SCREEN-space drag too, given in metres
        // along the view's right and up axes at the grab's depth ("10cm to screen-right").
        s.worldMove = cmd == QLatin1String("dragw");
        if (cmd == QLatin1String("dragv")) {
            const glm::mat4 view = m_renderer->camera().view();
            const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
            const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
            QPointF         to = s.cursorPx;
            projectToScreen(s.cursorWorld + right * num(1) + up * num(2), to);
            s.fromPx = s.cursorPx;
            s.toPx = to;
            s.moveTicks = std::max(1, static_cast<int>(num(3)));
            s.holdTicks = std::max(0, static_cast<int>(num(4)));
        } else if (s.worldMove) {
            s.fromWorld = s.cursorWorld;
            s.toWorld = s.cursorWorld + glm::vec3(num(1), num(2), num(3));
            s.moveTicks = std::max(1, static_cast<int>(num(4)));
            s.holdTicks = std::max(0, static_cast<int>(num(5)));
        } else {
            s.fromPx = s.cursorPx;
            s.toPx = s.cursorPx + QPointF(num(1), num(2));
            s.moveTicks = std::max(1, static_cast<int>(num(3)));
            s.holdTicks = std::max(0, static_cast<int>(num(4)));
        }
        s.tick = 0;
        s.moving = true;
        return ScriptFlow::Wait; // the 60 Hz tick drives it (scriptDragTick), and comes back here when done
    } else if (cmd == QLatin1String("wheel") && s.dragging) {
        stepIkDepth(num(1), s.cursorPx);
    } else if (cmd == QLatin1String("release") && s.dragging) {
        scriptReport("release");
        s.dragging = false;
        if (m_ik.poseChanged) {
            beginIkRelease();
            s.awaitingSettle = true;
            requestUpdate();
            return ScriptFlow::Wait; // onIkTick calls scriptSettled()
        }
        abortIkDrag(); // a press that never moved: a click
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- The Materials tab: select, zone, material, matlink, matreset, texture, overlay,
//     savematerial, applymaterial -------------------------------------------------------------
ScriptFlow VulkanWindow::scriptMaterialCommand(const QString& cmd, const QStringList& w, const QString& /*line*/) {
    IkScript&        s = *m_script;
    const WordNumber num{w};
    if (cmd == QLatin1String("select") && w.size() > 1) {
        // select <index>: the model selected, as a click on it does (-1 = none) — the way a
        // script reaches a STATIC model's materials (it has no joint to click).
        scene().setSelectedModel(static_cast<int>(num(1)));
        requestUpdate();
    } else if (cmd == QLatin1String("zone") && w.size() > 1) {
        // zone <index|name>: the Materials tab's zone picker.
        bool ok = false;
        int  index = w[1].toInt(&ok);
        if (!ok) {
            const MaterialView view = materialView();
            index = -1;
            for (std::size_t i = 0; i < view.zones.size(); ++i) {
                if (QString::fromStdString(view.zones[i]).compare(w[1], Qt::CaseInsensitive) == 0) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0) {
                say("[ikscript]   zone '%s' unknown\n", qPrintable(w[1]));
            }
        }
        if (index >= 0) {
            setMaterialZone(index);
        }
    } else if (cmd == QLatin1String("material") && w.size() > 2) {
        // material <field> <value> | material <color> <r> <g> <b> (linear; `color` the base
        // color, else a materialColorName — specularcolor, coatcolor, sheencolor,
        // transmissioncolor, emissioncolor, subsurfacecolor, scattercolor): the selected zone's
        // material set the way the Materials tab does it (one bracketed, undoable edit; the
        // fields and their display units are the tab's — materialparams.h), reaching the zones
        // that share the imported material while `matlink` is on.
        MaterialParams p = materialView().params;
        bool           known = true;
        MaterialColor  which = MaterialColor::Base;
        if (w.size() > 4 && (w[1].compare(QLatin1String("color"), Qt::CaseInsensitive) == 0 ||
                             materialColorByName(w[1].toLower().toStdString(), which))) {
            materialColorRef(p, which) =
                glm::vec3(static_cast<float>(num(2)), static_cast<float>(num(3)), static_cast<float>(num(4)));
        } else {
            MaterialField field = MaterialField::Roughness;
            if (materialFieldByName(w[1].toLower().toStdString(), field)) {
                setMaterialField(p, field, static_cast<float>(num(2)));
            } else {
                known = false;
                say("[ikscript]   material field '%s' unknown\n", qPrintable(w[1]));
            }
        }
        if (known && beginMaterialEdit()) {
            setSelectedMaterial(p, s.materialLinked);
            endMaterialEdit();
        }
    } else if (cmd == QLatin1String("matlink") && w.size() > 1) {
        // matlink on|off: the tab's "Edit matching zones together".
        s.materialLinked = w[1].compare(QLatin1String("off"), Qt::CaseInsensitive) != 0;
    } else if (cmd == QLatin1String("matreset")) {
        // matreset [all]: the tab's Reset Zone / Reset All Zones.
        resetSelectedMaterial(s.materialLinked, w.size() > 1 && w[1].compare(QLatin1String("all"), Qt::CaseInsensitive) == 0);
    } else if (cmd == QLatin1String("texture") && w.size() > 2) {
        // texture <slot> <path|none|imported>: the tab's Textures group — the selected zone's
        // slot replaced by an image file (no spaces in the path: the line is split on them),
        // cleared, or put back to the imported map; one undoable edit. The slot names are
        // materialMapName's (basecolor, normal, bump, roughness, metalness, specmask,
        // translucency, emission, pore, cutout).
        MaterialMap map = MaterialMap::BaseColor;
        if (!materialMapByName(w[1].toLower().toStdString(), map)) {
            say("[ikscript]   texture slot '%s' unknown\n", qPrintable(w[1]));
        } else {
            const QString what = w[2];
            QString       source;
            if (what.compare(QLatin1String("imported"), Qt::CaseInsensitive) == 0) {
                source = QString::fromLatin1(kImportedTexture);
            } else if (what.compare(QLatin1String("none"), Qt::CaseInsensitive) != 0) {
                // A relative path is a test asset's: against POSESTUDIO_IK_SCRIPT_ASSETS (the
                // folder tools/materials/make_test_assets.py wrote), else the output folder.
                source = s.assetPath(what);
            }
            if (!setMaterialTexture(map, source)) {
                say("[ikscript]   texture '%s' could not be applied\n", qPrintable(what));
            }
        }
    } else if (cmd == QLatin1String("overlay") && w.size() > 1) {
        // THE OVERLAYS (materialoverlay.h), the way the tab's Overlays section edits the
        // selected zone's stack — every form one undoable edit, reaching every zone painted
        // on the zone's color map:
        //   overlay add [image]                     a new overlay on top: EMPTY, as the tab's
        //                                           + Add Overlay makes it, or with its picture
        //   overlay remove <n> | overlay move <n> <to>
        //   overlay <n> <field> <value>             enabled, opacity, brightness, contrast, gamma,
        //                                           hue, saturation, gloss, metallic,
        //                                           bumpstrength, normalstrength,
        //                                           displacementstrength, offsetx, offsety,
        //                                           rotation, scale (the tab's units),
        //                                           blend <name|index>
        //   overlay <n> tint <r> <g> <b>            the card's Base Color: display (sRGB) values 0..1
        //   overlay <n> name <text…>                its name (a new one is "Overlay N")
        //   overlay <n> map <color|opacity|metallic|bump|normal|displacement> <image|none>
        // (`map opacity` is the Opacity row's map: the overlay's mask.)
        // <n> counts from 0, the lowest layer. A relative image path is a test asset's (as
        // `texture`'s: POSESTUDIO_IK_SCRIPT_ASSETS, else the output folder).
        std::vector<MaterialOverlay> stack = materialView().params.overlays;
        const QString                what = w[1].toLower();
        const auto                   imagePath = [&](const QString& given) -> std::string {
            if (given.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) {
                return std::string();
            }
            return s.assetPath(given).toStdString();
        };
        bool changed = false;
        if (what == QLatin1String("add")) {
            MaterialOverlay overlay;
            overlay.name = "Overlay " + std::to_string(nextOverlayNumber(stack, "Overlay")); // (as the tab names one)
            if (w.size() > 2) {
                overlay.color = imagePath(w[2]);
            }
            stack.push_back(std::move(overlay));
            changed = true;
        } else if (w.size() > 2) {
            const bool indexFirst = what != QLatin1String("remove") && what != QLatin1String("move");
            bool       ok = false;
            const int  n = (indexFirst ? w[1] : w[2]).toInt(&ok);
            if (!ok || n < 0 || static_cast<std::size_t>(n) >= stack.size()) {
                say("[ikscript]   overlay %s: no such overlay (the zone has %d)\n", qPrintable(indexFirst ? w[1] : w[2]),
                    static_cast<int>(stack.size()));
            } else if (what == QLatin1String("remove")) {
                stack.erase(stack.begin() + n);
                changed = true;
            } else if (what == QLatin1String("move") && w.size() > 3) {
                const int       to = std::clamp(w[3].toInt(), 0, static_cast<int>(stack.size()) - 1);
                MaterialOverlay moved = std::move(stack[static_cast<std::size_t>(n)]);
                stack.erase(stack.begin() + n);
                stack.insert(stack.begin() + to, std::move(moved));
                changed = true;
            } else if (indexFirst && w.size() > 3 && w[2].compare(QLatin1String("name"), Qt::CaseInsensitive) == 0) {
                stack[static_cast<std::size_t>(n)].name = w.mid(3).join(QLatin1Char(' ')).toStdString();
                changed = true;
            } else if (indexFirst && w.size() > 5 && w[2].compare(QLatin1String("tint"), Qt::CaseInsensitive) == 0) {
                stack[static_cast<std::size_t>(n)].tint = glm::clamp(glm::vec3(num(3), num(4), num(5)), glm::vec3(0.0f), glm::vec3(1.0f));
                changed = true;
            } else if (indexFirst && w.size() > 4 && w[2].compare(QLatin1String("map"), Qt::CaseInsensitive) == 0) {
                OverlayElement element = OverlayElement::Color;
                if (overlayElementByName(w[3].toLower().toStdString(), element)) {
                    overlayElementSource(stack[static_cast<std::size_t>(n)], element) = imagePath(w[4]);
                    changed = true;
                } else {
                    say("[ikscript]   overlay element '%s' unknown\n", qPrintable(w[3]));
                }
            } else if (indexFirst && w.size() > 3) {
                OverlayField field = OverlayField::Opacity;
                OverlayBlend blend = OverlayBlend::Multiply;
                if (!overlayFieldByName(w[2].toLower().toStdString(), field)) {
                    say("[ikscript]   overlay field '%s' unknown\n", qPrintable(w[2]));
                } else if (field == OverlayField::Blend && overlayBlendByName(w[3].toLower().toStdString(), blend)) {
                    stack[static_cast<std::size_t>(n)].blend = blend;
                    changed = true;
                } else {
                    setOverlayField(stack[static_cast<std::size_t>(n)], field, num(3));
                    changed = true;
                }
            }
        }
        if (changed && !setMaterialOverlays(stack)) {
            say("[ikscript]   overlay: no zone selected\n");
        }
    } else if (cmd == QLatin1String("savematerial") && w.size() > 1) {
        // savematerial <name.psm> [noicon] [link] [<size>]: the Materials tab's Save — the
        // selected zone's material written into the run's folder (<name> may carry folders),
        // its textures collected beside it (`link`: named where they are) and its ball icon
        // rendered as <name>.png (metrics: matfile.ok, matfile.copied / .reused / .written /
        // .linked / .embedded, maticon.ok, maticon.engine, maticon.size,
        // maticon.<r|g|b|a>.<x%>.<y%>).
        scriptSaveMaterial(w);
    } else if (cmd == QLatin1String("applymaterial") && w.size() > 1) {
        // applymaterial <name.psm>: a saved material put on the selected zone, as a
        // double-click in the Materials Library does (metrics: matfile.ok, matfile.missing,
        // matfile.relocated). applymaterial <file.mtlx> [<material>]: a material of a
        // MaterialX document (the first, or the one named) the same way (matfile.materials:
        // how many the file holds; matfile.notes: what it could not carry, counted).
        scriptApplyMaterial(w);
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- The document: saveproject, openproject, new, import, delete -----------------------------
ScriptFlow VulkanWindow::scriptDocumentCommand(const QString& cmd, const QStringList& w, const QString& line) {
    IkScript& s = *m_script;
    if (cmd == QLatin1String("saveproject") && w.size() > 1) {
        // saveproject <name.pss>: File -> Save As into the run's folder (the .pss document).
        const QString path = QDir(s.outDir).filePath(w[1]);
        const bool    saved = saveProjectFile(path);
        if (saved) {
            setProjectPath(path); // as the menu's Save As does: the document's path, and clean
            setProjectClean();
        }
        say("[ikscript]   project %s: %s\n", saved ? "saved" : "NOT saved", qPrintable(path));
    } else if (cmd == QLatin1String("openproject") && w.size() > 1) {
        // openproject <name.pss>: File -> Open from the run's folder — the scene reset and every
        // figure re-imported and restored (the recovery dialog's re-pointing is not driven here).
        std::vector<std::string> missing;
        std::string              error;
        const int status = loadProjectFile(QDir(s.outDir).filePath(w[1]), {}, missing, error);
        say("[ikscript]   project %s%s%s\n", status == 0 ? "opened" : status == 1 ? "has missing sources" : "NOT opened: ",
            status < 0 ? error.c_str() : "", status == 1 ? (" " + missing.front()).c_str() : "");
    } else if (cmd == QLatin1String("new")) {
        resetToEmptyScene(); // File -> New's scene reset (the menu's prompt and panel reset are not driven here)
    } else if (cmd == QLatin1String("import") && w.size() > 1) {
        // import <file>: File → Import, without its dialogs — a model, a figure or a Gaussian
        // splat scene by its extension, added to the scene as it stands (a relative path
        // resolves like `texture`'s: POSESTUDIO_IK_SCRIPT_ASSETS, else the output folder).
        const QString file = s.assetPath(line.section(QLatin1Char(' '), 1).trimmed());
        PendingImport import;
        import.path = file;
        import.kind = ImportFormats::isSplatFile(file)   ? PendingImport::Kind::Splat
                      : ImportFormats::isModelFile(file) ? PendingImport::Kind::Obj
                                                         : PendingImport::Kind::Figure;
        closeSettlingEdits();
        if (!runImport(import, /*showProgress=*/false)) {
            say("[ikscript]   %s was NOT imported\n", qPrintable(file));
        }
        requestUpdate();
    } else if (cmd == QLatin1String("delete")) {
        // delete: the Delete key — the selected light goes (one undo entry), else the selected
        // model (which clears the undo history).
        deleteSelectedObject();
        requestUpdate();
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- The application around the viewport: ui, outlinestyle / jointstyle, prefs, renderwindow,
//     windowsize, uishot, asset ---------------------------------------------------------------
ScriptFlow VulkanWindow::scriptUiCommand(const QString& cmd, const QStringList& w, const QString& /*line*/) {
    IkScript&        s = *m_script;
    const WordNumber num{w};
    if (cmd == QLatin1String("ui") && w.size() > 2) {
        // ui click <id> [n] | ui set <id> <n> <value> | ui check <id> <n> <0|1> | ui pick <id> <n> <k>:
        // one of the application's widgets driven in-process — the n-th (from 0, in creation
        // order) carrying the dynamic property `testId` == <id> — through its own slot, as a
        // user's click, scrub or pick would run it; no desktop input. What a panel's `overlay`
        // / `material` commands cannot show: that ITS buttons are wired to the right thing.
        // (The tabs follow the rendered frame, which the script does not wait for: bring them
        // up to date first.)
        notifyJointTransform();
        notifyMaterialView();
        // (Also: ui call <id> <n> <slot> [int] — one of the widget's own slots or invokables
        // taking an int (ui post: the same from the event loop, for a slot that opens a modal
        // dialog); ui type <id> <n> <text…> — a line edit's text; ui key <id> <n>
        // <enter|esc> — that key to the widget. What opens, fills and closes an inline editor.
        // And: ui scrub <id> <n> <% of its width> [shift] — a press, a drag that far and a
        // release on a scrub field, through its own mouse handlers; `shift` holds Shift (the
        // fine scrub).)
        emit uiActionRequested(w[1].toLower(), w[2], w.size() > 3 ? w[3].toInt() : 0, w.size() > 4 ? w[4].toDouble() : 0.0,
                               w.size() > 4 ? w.mid(4).join(QLatin1Char(' ')) : QString());
    } else if ((cmd == QLatin1String("outlinestyle") || cmd == QLatin1String("jointstyle")) && w.size() > 2) {
        // outlinestyle <rrggbb> <opacity%> / jointstyle <rrggbb> <opacity%>: the selection
        // outline's / the joint highlight's color (hex, no #: a # begins a comment) and opacity,
        // as Preferences → Selection hands them to the viewport — nothing is stored.
        const QColor color = QColor::fromString(QLatin1Char('#') + w[1]);
        if (!color.isValid()) {
            say("[ikscript]   %s: '%s' is no rrggbb color\n", qPrintable(cmd), qPrintable(w[1]));
        } else if (cmd == QLatin1String("outlinestyle")) {
            setSelectionOutline(color, static_cast<float>(num(2) / 100.0));
        } else {
            setJointHighlight(color, static_cast<float>(num(2) / 100.0));
        }
    } else if (cmd == QLatin1String("prefs") && w.size() > 1) {
        // prefs <tab>: the Preferences dialog opened on that tab, as Edit → Preferences opens it
        // (from the event loop: give it a `wait` before driving its widgets with `ui`; picture
        // it with `uishot`, close it with `ui click preferences.close`).
        emit preferencesRequested(w.mid(1).join(QLatin1Char(' ')));
    } else if (cmd == QLatin1String("renderwindow")) {
        // renderwindow [start]: the Render window, as Render → Show Render Window opens it —
        // with `start`, as Render → Render Image does (from the event loop: `wait` before
        // driving it with `ui`; `uienabled.render.stop.0` reads 1 while its render runs).
        emit renderWindowRequested(w.size() > 1 && w[1].compare(QLatin1String("start"), Qt::CaseInsensitive) == 0);
    } else if (cmd == QLatin1String("windowsize") && w.size() > 2) {
        // windowsize <width> <height>: the main window resized, as the user resizes it (logical
        // pixels). The viewport follows through its own resize events: `wait` a few ticks.
        emit scriptWindowSizeRequested(std::max(400, w[1].toInt()), std::max(300, w[2].toInt()));
    } else if (cmd == QLatin1String("uishot") && w.size() > 1) {
        // uishot <name>: ui/<name>.png of the application's widgets — the side tabs (start on
        // one with POSESTUDIO_TAB=<title>); in a folder of its own, beside the viewport's shots
        // and out of their contact sheet. The Transform tab is brought up to date first: it
        // follows the rendered frame, and the script does not wait for one here.
        notifyJointTransform();
        notifyMaterialView();
        notifyLights();
        QDir(s.outDir).mkpath(QStringLiteral("ui"));
        emit uiShotRequested(QDir(s.outDir).filePath(QStringLiteral("ui/") + w[1] + QStringLiteral(".png")));
    } else if (cmd == QLatin1String("asset") && w.size() > 2) {
        // asset rename <file> <name> | duplicate <file> | move <file> <folder> | delete <file>
        //     | export <file or folder> <zip> | install <zip> <folder> [replace]:
        // the library browser's file operations on files of the run's folder (what its
        // context menu's entries do, without their dialogs; a delete is for good, not to
        // the trash). Every path is relative to the run's folder; a rename's <name> is a
        // name, and the file of a duplicate or a delete is the REST OF THE LINE (a duplicate's
        // name has a space in it). Read the outcome with `exists.<path>` / `count.<glob>`.
        QStringList words = w.mid(1);
        const QString op = words[0].toLower();
        if (op == QLatin1String("duplicate") || op == QLatin1String("delete")) {
            words = QStringList{op, words.mid(1).join(QLatin1Char(' '))};
        }
        const auto inRun = [&s](const QString& path) { return QDir::cleanPath(QDir(s.outDir).absoluteFilePath(path)); };
        words[1] = inRun(words[1]);
        if (words.size() > 2 && op != QLatin1String("rename")) {
            words[2] = inRun(words[2]);
        }
        if (op == QLatin1String("install") || op == QLatin1String("move")) {
            QDir().mkpath(words.value(2));
        } else if (op == QLatin1String("export")) {
            QDir().mkpath(QFileInfo(words.value(2)).absolutePath()); // (a save dialog names a folder that is there)
        }
        emit libraryOperationRequested(words);
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- Pictures: shotref / renderref, shotbase / renderbase, probe, render, shot ----------------
ScriptFlow VulkanWindow::scriptPictureCommand(const QString& cmd, const QStringList& w, const QString& /*line*/) {
    IkScript& s = *m_script;
    if (cmd == QLatin1String("shotref")) {
        // shotref / renderref: the last `shot` (the last `render`) is kept as THE REFERENCE —
        // `shot.fromref` (`render.fromref`) then reads how far a later one is from it: the
        // mean difference of their channels, 0-255. For "this picture is nearer to that one
        // than to the other" — which of two readings of a normal map shows the relief a
        // height map of the same relief shows.
        s.shotReference = s.shotImage;
    } else if (cmd == QLatin1String("renderref")) {
        s.renderReference = s.renderImage;
    } else if (cmd == QLatin1String("shotbase")) {
        // shotbase / renderbase: the last shot (render) is kept as THE BASE — the view WITHOUT
        // the thing being compared. `shot.likeref` (`render.likeref`) is then how alike the
        // last picture's and the reference's CHANGES from the base are: +1 the same relief lit
        // the same way, -1 the relief turned over, whatever their strengths.
        s.shotBase = s.shotImage;
    } else if (cmd == QLatin1String("renderbase")) {
        s.renderBase = s.renderImage;
    } else if (cmd == QLatin1String("probe") && w.size() > 3) {
        // probe <x> <y> <z>: a WORLD point (metres) — `render.probe.<r|g|b|a|lum>` then reads the
        // last render's picture where that point is seen (through the viewport's camera as
        // it stands: a render of the viewport's own size frames what the viewport frames).
        s.probeSet = true;
        s.probe[0] = w[1].toFloat();
        s.probe[1] = w[2].toFloat();
        s.probe[2] = w[3].toFloat();
    } else if (cmd == QLatin1String("render") && w.size() > 1) {
        // render <name> [<width> <height>] [<samples>] [<device text>|cpu] [nodenoise] [transparent]:
        // a PRODUCTION RENDER of the scene as it stands, through the render engine, run to its
        // end and saved as render/<name>.png (metrics: render.ok, render.seconds, render.mean,
        // render.covered).
        scriptRender(w);
    } else if (cmd == QLatin1String("shot") && w.size() > 1) {
        s.shotPath = QDir(s.outDir).filePath(w[1] + QStringLiteral(".png"));
        s.awaitingShot = true;
        m_renderer->requestCapture();
        requestUpdate();
        // A window that is MINIMIZED (or hidden) draws no frame, and the shot would wait for
        // good — a run started on a desktop in use, its window put away by the user, hung at
        // its next `shot` with nothing in the log. After 20 s the shot is given up as a
        // FAILURE (the exit code counts it) and the script goes on.
        const int serial = ++s.shotSerial;
        QTimer::singleShot(20000, this, [this, serial]() {
            if (m_script && m_script->awaitingShot && m_script->shotSerial == serial) {
                m_script->awaitingShot = false;
                ++m_script->failures;
                say("[ikscript]   SHOT FAILED: no frame was drawn in 20 s (is the window minimized or hidden?) - %s\n",
                    qPrintable(m_script->shotPath));
                scriptStep();
            }
        });
        return ScriptFlow::Wait; // renderFrame calls scriptFrameRendered()
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

// --- Lighting: light, lamp, environment ------------------------------------------------------
ScriptFlow VulkanWindow::scriptLightingCommand(const QString& cmd, const QStringList& w, const QString& line) {
    IkScript& s = *m_script;
    if (cmd == QLatin1String("light") && w.size() > 2) {
        // light <dial> <value>: one of the Environment tab's dials, as the tab sets it (no undo
        // entry; the tab's own widgets are not told).
        LightingSettings lighting = m_deferred.lighting;
        const QString    dial = w[1].toLower();
        const float      value = w[2].toFloat();
        const QHash<QString, float LightingSettings::*> dials = {
            {QStringLiteral("exposure"), &LightingSettings::exposure},
            {QStringLiteral("diffuse"), &LightingSettings::diffuseIntensity},
            {QStringLiteral("specular"), &LightingSettings::specularIntensity},
            {QStringLiteral("ambient"), &LightingSettings::ambientFill},
            {QStringLiteral("key"), &LightingSettings::keyIntensity},
            {QStringLiteral("rotation"), &LightingSettings::environmentRotationDeg},
            {QStringLiteral("azimuth"), &LightingSettings::keyAzimuthDeg},
            {QStringLiteral("elevation"), &LightingSettings::keyElevationDeg},
            {QStringLiteral("subsurface"), &LightingSettings::subsurface},
            {QStringLiteral("rim"), &LightingSettings::rimIntensity},
            {QStringLiteral("backdropbrightness"), &LightingSettings::backdropBrightness},
            {QStringLiteral("backdropblur"), &LightingSettings::backdropBlur},
            {QStringLiteral("shadowintensity"), &LightingSettings::shadowIntensity},
            {QStringLiteral("shadowsoftness"), &LightingSettings::shadowSoftness},
        };
        if (const auto it = dials.constFind(dial); it != dials.constEnd()) {
            lighting.*(it.value()) = value;
        } else if (dial == QLatin1String("tonemap")) {
            lighting.tonemap = value != 0.0f;
        } else if (dial == QLatin1String("shadows")) {
            lighting.shadowsEnabled = value != 0.0f;
        } else if (dial == QLatin1String("backdrop")) {
            lighting.backdropMode = static_cast<int>(value);
        } else {
            say("[ikscript]   unknown light dial '%s'\n", qPrintable(w[1]));
        }
        setLightingSettings(lighting);
    } else if (cmd == QLatin1String("lamp") && w.size() > 1) {
        // lamp add <kind> | remove <n> | select <n> | aim <n> | click <n> | drag <n> <dx%> <dy%> |
        // <n> <field> <value>: the LIGHT OBJECTS (the Lights tab), each as its control or the
        // mouse does it — see scriptLamp.
        scriptLamp(w);
    } else if (cmd == QLatin1String("environment") && w.size() > 1) {
        // environment <file.hdr|.exr>: the Environment tab's panorama pick (a relative path
        // resolves like `texture`'s). The bake runs off-thread: `wait` for it.
        const QString file = s.assetPath(line.section(QLatin1Char(' '), 1).trimmed());
        setEnvironmentFile(file);
    } else {
        return ScriptFlow::NotMine;
    }
    return ScriptFlow::Next;
}

} // namespace pose
