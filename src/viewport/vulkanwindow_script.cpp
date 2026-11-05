/**
 * @file vulkanwindow_script.cpp
 * @brief The scripted in-app posing test (POSESTUDIO_IK_SCRIPT=<file>): IkScript's loader plus
 *        the runner on VulkanWindow.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h and ikscript.h. Where the
 * IK harness (tools/ikharness) drives the Armature alone, this drives the APP: a `press` is the
 * real picker at a pixel followed by the real press path, a `drag` moves a scripted cursor in
 * SCREEN space through the real camera-parallel drag plane, the 60 Hz timer solves, the release
 * settles, and `shot` saves the frame the renderer produced. Nothing is injected into the OS —
 * the window's own gesture functions are called — so it runs on a desktop that is in use, and
 * the frames are read back from the GPU, so the window need not even be visible.
 *
 * tools/ikscripts/README.md is the command reference.
 */

#include "vulkanwindow.h"

#include "ikscript.h"
#include "rendering/vulkanrenderer.h"
#include "scene/armature.h"
#include "scene/ik/bonealiases.h"
#include "scene/ik/ikrig.h"
#include "scene/scene.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace pose {

std::unique_ptr<IkScript> IkScript::fromEnvironment() {
    const QString path = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT");
    if (path.isEmpty()) {
        return nullptr;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // (Out, not on: a run that names a script the app cannot read must not sit there as an
        // ordinary app while its runner waits for a "done" that never comes - it did, on screen,
        // until someone noticed. The exit code is the runner's own "did not finish".)
        std::fprintf(stderr, "[ikscript] cannot read %s\n", qPrintable(path));
        std::fflush(stderr);
        std::_Exit(101);
    }
    auto script = std::make_unique<IkScript>();
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            line.truncate(hash);
        }
        line = line.trimmed();
        if (!line.isEmpty()) {
            script->lines.push_back(line);
        }
    }
    script->outDir = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_OUT");
    if (script->outDir.isEmpty()) {
        script->outDir = QFileInfo(path).absolutePath();
    }
    QDir().mkpath(script->outDir);
    return script;
}

namespace {

void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

/// A script names bones the way the reference figure does; the other generations call the same
/// joints something else: the alias table the IK harness shares (scene/ik/bonealiases.h). The
/// name as given if the figure has it, else the first alternative it has, else the name.
std::string figureBoneName(const Armature* arm, const std::string& canonical) {
    return arm != nullptr ? aliasedBoneName(*arm, canonical) : canonical;
}
std::vector<glm::vec3> jointPositions(const Armature* arm) {
    std::vector<glm::vec3> out;
    if (arm != nullptr) {
        out.resize(arm->boneCount());
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = arm->boneWorldPosition(i);
        }
    }
    return out;
}

/// How far @p bone has moved since the press (mm), 0 for a bone the press did not record.
double movedSincePress(const Armature& arm, const IkScript& s, int bone) {
    return bone >= 0 && static_cast<std::size_t>(bone) < s.pressPositions.size()
               ? glm::length(arm.boneWorldPosition(static_cast<std::size_t>(bone)) -
                             s.pressPositions[static_cast<std::size_t>(bone)]) * 1000.0
               : 0.0;
}

} // namespace

/// The script's first step, once a posable figure exists (polled: a command-line figure imports
/// through the progress dialog, whose event loop fires this timer mid-import).
void VulkanWindow::startScript() {
    IkScript& s = *m_script;
    // (As the bench: a command-line figure imports through the progress dialog, whose event loop
    // fires this timer mid-import. Poll until a posable figure exists.)
    if ((!m_renderer || !scene().hasPosableFigure()) && s.retries++ < 120) {
        QTimer::singleShot(500, this, &VulkanWindow::startScript);
        return;
    }
    if (!m_renderer || !scene().hasPosableFigure()) {
        say("[ikscript] no posable figure: give one on the command line\n");
        std::_Exit(2);
    }
    say("[ikscript] %d commands, output in %s\n", static_cast<int>(s.lines.size()), qPrintable(s.outDir));
    scriptStep();
}

/// Resolves "<bone>[@share]" to the pixel a user would click: the joint itself, or the point that
/// share of the way along the bone's rigid limb segment. False when the bone is unknown or the
/// point is behind the camera.
bool VulkanWindow::scriptPixelOf(const QString& spec, QPointF& px) {
    const std::string bone = figureBoneName(scene().figureArmature(), spec.section(QLatin1Char('@'), 0, 0).toStdString());
    const QString     share = spec.section(QLatin1Char('@'), 1, 1);
    const int named = scene().selectBoneByName(bone);
    if (named < 0) {
        say("[ikscript] unknown bone '%s'\n", bone.c_str());
        return false;
    }
    if (share.isEmpty()) {
        // (The NAMED bone's own joint — not the selection's: a bone of the face rig selects the head,
        // and a click "at the eye" is a click at the eye's pixel.)
        const Armature* arm = scene().figureArmature();
        return arm != nullptr && projectToScreen(arm->boneWorldPosition(static_cast<std::size_t>(named)), px);
    }
    scene().setIkGrabOnSegment(share.toFloat());
    glm::vec3 point(0.0f);
    return scene().ikGrabPointWorld(point) && projectToScreen(point, px);
}

/// Runs script lines until one has to WAIT for the app — a drag's ticks (`drag`/`dragw`/`dragv`),
/// a release's settle, a shot's frame, a `wait` — and returns; the event that ends the wait
/// (scriptDragTick's last tick, scriptSettled, scriptFrameRendered, the wait's timer) schedules
/// the next call. At the last line it prints the verdict and exits with the failure count.
void VulkanWindow::scriptStep() {
    IkScript& s = *m_script;
    while (s.next < s.lines.size()) {
        const QString     line = s.lines[s.next++];
        const QStringList w = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString     cmd = w[0].toLower();
        const auto        num = [&](int i) { return i < w.size() ? w[i].toFloat() : 0.0f; };
        say("[ikscript] > %s\n", qPrintable(line));

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
        } else if (cmd == QLatin1String("closeup") && w.size() > 2) {
            // closeup <bone> <radius m>: the camera framed on that joint — a hand, a foot, the face
            // (what a user does with the wheel and a pan before posing a finger).
            const Armature* arm = scene().figureArmature();
            const int       bone = arm ? arm->boneIndex(figureBoneName(arm, w[1].toStdString())) : -1;
            if (bone >= 0) {
                m_renderer->camera().frame(arm->boneWorldPosition(static_cast<std::size_t>(bone)), std::max(0.02f, num(2)));
                requestUpdate();
            } else {
                say("[ikscript]   unknown bone\n");
            }
        } else if (cmd == QLatin1String("shade") && w.size() > 1) {
            setShadeMode(w[1].toInt());
        } else if (cmd == QLatin1String("pose") && w.size() > 1) {
            if (!loadPose(line.section(QLatin1Char(' '), 1).trimmed())) {
                say("[ikscript]   pose not loaded\n");
            }
        } else if (cmd == QLatin1String("reset")) {
            runPoseUtility(PoseUtility::ResetPose);
        } else if (cmd == QLatin1String("undo")) {
            undo();
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
            // dial <forward|sideways|twist> <value>: the selected joint's Transform-tab dial set to
            // that value, the way the tab does it (one bracketed, undoable edit).
            const QString which = w[1].toLower();
            const int     dial = which == QLatin1String("forward") ? 0 : which == QLatin1String("sideways") ? 1 : 2;
            if (beginJointTransformEdit()) {
                setJointTransformDial(dial, w[2].toDouble());
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
            m_modalCursor = QPointF(width() * 0.5, height() * 0.5);
            m_lastMousePos = m_modalCursor;
            say("[ikscript]   joint mouse mode %s\n", m_modalKind >= 0 ? "on" : "off");
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
            say("[ikscript]   joint mouse mode %s\n", m_modalKind >= 0 ? "on" : "off");
        } else if (cmd == QLatin1String("modalmove") && w.size() > 2) {
            // modalmove <dxPx> <dyPx> [steps]: the mouse moved that far (+y down) with no button
            // held, in that many move events (20 by default) through the window's mouseMoveEvent.
            const int     steps = std::max(1, w.size() > 3 ? w[3].toInt() : 20);
            const QPointF step(num(1) / steps, num(2) / steps);
            for (int k = 0; k < steps; ++k) {
                const QPointF at = m_lastMousePos + step;
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
                const QPointF         at = m_lastMousePos;
                QMouseEvent down(QEvent::MouseButtonPress, at, mapToGlobal(at), button, button, Qt::NoModifier);
                mousePressEvent(&down);
                QMouseEvent up(QEvent::MouseButtonRelease, at, mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
                mouseReleaseEvent(&up);
            }
        } else if (cmd == QLatin1String("saveproject") && w.size() > 1) {
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
        } else if (cmd == QLatin1String("uishot") && w.size() > 1) {
            // uishot <name>: ui/<name>.png of the application's widgets — the side tabs (start on
            // one with POSESTUDIO_TAB=<title>); in a folder of its own, beside the viewport's shots
            // and out of their contact sheet. The Transform tab is brought up to date first: it
            // follows the rendered frame, and the script does not wait for one here.
            notifyJointTransform();
            QDir(s.outDir).mkpath(QStringLiteral("ui"));
            emit uiShotRequested(QDir(s.outDir).filePath(QStringLiteral("ui/") + w[1] + QStringLiteral(".png")));
        } else if (cmd == QLatin1String("pin") && w.size() > 1) {
            // The joint a CLICK there finds — and, since the pick is the SKIN's (2026-09-26), the named
            // bone by name where the click lands on another part of the body (a hanging hand covers
            // the hip from the side: the click pinned a finger, and a "seated, hips pinned" lean had no
            // seat), as press does.
            QPointF px;
            if (scriptPixelOf(w[1], px)) {
                const int picked = boneAt(px);
                const Armature* found = scene().figureArmature();
                const std::string name = figureBoneName(found, w[1].section(QLatin1Char('@'), 0, 0).toStdString());
                const int wanted = found ? found->knownPosingBone(found->boneIndex(name)) : -1; // (a face-rig bone's name: the head)
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
            // The real press: the picker at the pixel, then the press path (IK drag + undo snapshot).
            // cpress is the Ctrl press (IkScope::Part): the SCOPED drag of the grabbed chain alone for
            // a limb or the head, the FIGURE MOVE for a grab of the body itself.
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
                continue;
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
                const Armature*   found = scene().figureArmature();
                const std::string name = figureBoneName(found, w[1].section(QLatin1Char('@'), 0, 0).toStdString());
                const QString     share = w[1].section(QLatin1Char('@'), 1, 1);
                const int         wanted = found ? found->knownPosingBone(found->boneIndex(name)) : -1; // (a face-rig bone's name: the head)
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
                    if (began) {
                        m_preEditPose = scene().capturePose();
                    }
                    say("[ikscript]   the pick found %s there, not %s: grabbed by name instead\n", other.c_str(), name.c_str());
                }
            }
            if (!began) {
                say("[ikscript]   press at px(%.0f %.0f) began no drag\n", px.x(), px.y());
                continue;
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
            return; // the 60 Hz tick drives it (scriptDragTick), and comes back here when done
        } else if (cmd == QLatin1String("wheel") && s.dragging) {
            stepIkDepth(num(1), s.cursorPx);
        } else if (cmd == QLatin1String("release") && s.dragging) {
            scriptReport("release");
            s.dragging = false;
            if (m_ik.poseChanged) {
                beginIkRelease();
                s.awaitingSettle = true;
                requestUpdate();
                return; // onIkTick calls scriptSettled()
            }
            abortIkDrag(); // a press that never moved: a click
        } else if (cmd == QLatin1String("wait")) {
            QTimer::singleShot(std::max(1, static_cast<int>(num(1))) * 16, this, &VulkanWindow::scriptStep);
            return;
        } else if (cmd == QLatin1String("expect") && w.size() >= 4) {
            // expect <metric> <op> <value>: the script as a TEST. (Metrics: scriptMetric.)
            double      value = 0.0;
            const bool  known = scriptMetric(w[1], value);
            const double want = w[3].toDouble();
            const QString op = w[2];
            const bool ok = known && (op == QLatin1String("<")    ? value < want
                                      : op == QLatin1String("<=") ? value <= want
                                      : op == QLatin1String(">")  ? value > want
                                      : op == QLatin1String(">=") ? value >= want
                                      : op == QLatin1String("==") ? std::abs(value - want) < 1.0e-3
                                                                  : false);
            ++s.expectations;
            s.failures += ok ? 0 : 1;
            if (known) {
                say("[ikscript]   EXPECT %s: %s = %.3f, wanted %s %s\n", ok ? "ok" : "FAILED", qPrintable(w[1]), value,
                    qPrintable(op), qPrintable(w[3]));
            } else {
                say("[ikscript]   EXPECT FAILED: unknown metric '%s'\n", qPrintable(w[1]));
            }
        } else if (cmd == QLatin1String("report")) {
            scriptReport(w.size() > 1 ? qPrintable(line.section(QLatin1Char(' '), 1)) : "report");
        } else if (cmd == QLatin1String("shot") && w.size() > 1) {
            s.shotPath = QDir(s.outDir).filePath(w[1] + QStringLiteral(".png"));
            s.awaitingShot = true;
            m_renderer->requestCapture();
            requestUpdate();
            return; // renderFrame calls scriptFrameRendered()
        } else if (cmd == QLatin1String("quit")) {
            break;
        } else {
            say("[ikscript]   ignored (unknown command, or no drag in flight)\n");
        }
    }
    say("[ikscript] done: %d expectation(s), %d FAILED\n", s.expectations, s.failures);
    std::_Exit(std::min(s.failures, 100)); // diagnostic mode: no teardown; the exit code is the failures
}

/// One 60 Hz tick of a scripted move (called from the IK tick before its solve): advances the
/// cursor along the move, feeds it to the drag exactly as a mouse move would, records the
/// worst single-tick joint move (a POP over 10cm is logged), and resumes the script when the
/// move's ticks and hold are up.
void VulkanWindow::scriptDragTick() {
    IkScript& s = *m_script;
    if (!s.moving) {
        return;
    }
    const int   t = ++s.tick;
    ++s.dragTicks;
    const float f = std::min(1.0f, static_cast<float>(t) / static_cast<float>(s.moveTicks));
    if (s.worldMove) {
        s.cursorWorld = glm::mix(s.fromWorld, s.toWorld, f);
        setIkTarget(s.cursorWorld);
        projectToScreen(s.cursorWorld, s.cursorPx);
    } else {
        s.cursorPx = s.fromPx + (s.toPx - s.fromPx) * static_cast<qreal>(f);
        glm::vec3 target;
        if (dragPlaneHit(s.cursorPx, target)) { // exactly mouseMoveEvent's IK branch
            setIkTarget(target);
            s.cursorWorld = target;
        }
    }
    // What the last tick's solve did (this runs at the top of the tick, before the next solve).
    const Armature* arm = scene().figureArmature();
    glm::vec3       grab(0.0f);
    if (arm != nullptr && t > 1 && scene().ikGrabPointWorld(grab)) {
        const std::vector<glm::vec3> now = jointPositions(arm);
        float       tickWorst = 0.0f;
        std::size_t tickBone = 0;
        for (std::size_t i = 0; i < now.size() && i < s.lastPositions.size(); ++i) {
            const float step = glm::length(now[i] - s.lastPositions[i]);
            if (step > tickWorst) {
                tickWorst = step;
                tickBone = i;
            }
            if (step > s.jumpMax) {
                s.jumpMax = step;
                s.jumpBone = static_cast<int>(i);
                s.jumpTick = s.dragTicks;
            }
        }
        if (tickWorst > 0.10f) { // a POP: more than 10cm in one tick is never a drag's pace
            say("[ikscript]   POP at tick %d: %s moved %.0f mm in one tick\n", s.dragTicks, arm->boneName(tickBone).c_str(),
                tickWorst * 1000.0f);
        }
        // The per-tick trace (POSESTUDIO_IK_SCRIPT_TICKS, POSESTUDIO_IK_SCRIPT_EULERS): the worst joint's
        // move every tick, and named bones' Euler channels — the app's twin of the harness's
        // IK_HARNESS_POSE_TRACE / IK_HARNESS_EULER_TICKS, for a pop only a scripted click makes.
        static const bool  kTickTrace = std::getenv("POSESTUDIO_IK_SCRIPT_TICKS") != nullptr;
        static const char* kEulerBones = std::getenv("POSESTUDIO_IK_SCRIPT_EULERS");
        if (kTickTrace) {
            say("[ikscript]   tick %d: worst %s %.1f mm | grab (%.3f %.3f %.3f) cursor (%.3f %.3f %.3f)\n", s.dragTicks,
                arm->boneName(tickBone).c_str(), tickWorst * 1000.0f, grab.x, grab.y, grab.z, s.cursorWorld.x, s.cursorWorld.y,
                s.cursorWorld.z);
        }
        if (kEulerBones != nullptr) {
            const QStringList names = QString::fromUtf8(kEulerBones).split(QLatin1Char(','), Qt::SkipEmptyParts);
            QString           row;
            for (const QString& nm : names) {
                const int b = arm->boneIndex(figureBoneName(arm, nm.trimmed().toStdString()));
                if (b >= 0) {
                    const glm::vec3 e = arm->boneEuler(static_cast<std::size_t>(b));
                    row += QStringLiteral(" %1(%2 %3 %4)").arg(nm.trimmed()).arg(e.x, 0, 'f', 1).arg(e.y, 0, 'f', 1).arg(e.z, 0, 'f', 1);
                }
            }
            say("[ikscript]   tick %d eulers:%s\n", s.dragTicks, row.toUtf8().constData());
        }
        // (... and POSESTUDIO_IK_SCRIPT_POS: named bones' world positions every tick, mm — which joint a
        // pop is in when no channel of its chain moved.)
        static const char* kPosBones = std::getenv("POSESTUDIO_IK_SCRIPT_POS");
        if (kPosBones != nullptr) {
            const QStringList names = QString::fromUtf8(kPosBones).split(QLatin1Char(','), Qt::SkipEmptyParts);
            QString           row;
            for (const QString& nm : names) {
                const int b = arm->boneIndex(figureBoneName(arm, nm.trimmed().toStdString()));
                if (b >= 0 && static_cast<std::size_t>(b) < now.size()) {
                    const glm::vec3 w = now[static_cast<std::size_t>(b)] * 1000.0f;
                    row += QStringLiteral(" %1(%2 %3 %4)").arg(nm.trimmed()).arg(w.x, 0, 'f', 0).arg(w.y, 0, 'f', 0).arg(w.z, 0, 'f', 0);
                }
            }
            say("[ikscript]   tick %d positions (mm):%s\n", s.dragTicks, row.toUtf8().constData());
        }
        s.lastPositions = now;
    }
    if (t >= s.moveTicks + s.holdTicks) {
        s.moving = false;
        QTimer::singleShot(0, this, &VulkanWindow::scriptStep); // after this tick's solve
    }
}

/// The release settle finished (finishIkSettle): a `release` line's wait is over.
void VulkanWindow::scriptSettled() {
    IkScript& s = *m_script;
    if (s.awaitingSettle) {
        s.awaitingSettle = false;
        QTimer::singleShot(0, this, &VulkanWindow::scriptStep);
    }
}

/// A frame was presented after a `shot` line: takes the GPU capture, saves it as a PNG
/// (re-requesting one when a swapchain rebuild skipped it) and resumes the script.
void VulkanWindow::scriptFrameRendered() {
    IkScript& s = *m_script;
    if (!s.awaitingShot) {
        return;
    }
    VulkanRenderer::CapturedFrame frame;
    if (!m_renderer->takeCapture(frame)) {
        m_renderer->requestCapture(); // (the frame was skipped by a swapchain rebuild: the next one)
        requestUpdate();
        return;
    }
    s.awaitingShot = false;
    QImage image(frame.pixels.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                 static_cast<int>(frame.width) * 4,
                 frame.bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    image = image.convertToFormat(QImage::Format_RGB888); // (drops the alpha the composite leaves)
    const bool saved = image.save(s.shotPath);
    say("[ikscript]   shot %s (%ux%u)%s\n", qPrintable(s.shotPath), frame.width, frame.height,
        saved ? "" : " — NOT SAVED");
    QTimer::singleShot(0, this, &VulkanWindow::scriptStep);
}

/// A number a script can `expect` on — what `report` prints, and a few per-bone readings:
///   grab            grab point to cursor, mm (during a drag)   grabpx  the same on screen, pixels
///   hips, head      moved since the last press, mm          steps   steps taken in the drag
///   jump            worst single-tick move of any joint, mm  lowest  the lowest joint's height, mm
///   meshlow         the posed MESH's lowest point over the floor, mm (negative = flesh through it)
///   gaze            the head's look direction over the horizon, degrees (negative = down)
///   headroll        the head's sideways tilt, degrees
///   tilt.<bone>     a bone's tilt from upright, degrees      y.<bone>     its height, mm
///   hang.<bone>     a bone's direction (origin to its longest child) from straight DOWN, degrees
///   moved.<bone>    moved since the last press, mm           euler.<bone>.<x|y|z>  a posed channel
///   movedxz.<bone>  moved along the FLOOR since the last press, mm (a planted foot's place, heel lift aside)
///   selected.<bone> 1 if the selected joint is that bone or a bone of its rigid segment (a click's pick), else 0
///   lit.<bone>      1 if the selection highlight tints that bone's flesh (the selected joint, or its twist twin), else 0
///   palm.<bone>.<x|y|z>  a HAND's palm normal, that world component (where a laid hand faces)
///   figure.<x|y|z|turn|tip|tilt>  the figure's place (cm, world) and rotation (deg): the Transform tab's Figure group
///   dial.<forward|sideways|twist>  the selected joint's Transform-tab dial, as it reads now
///   dialmin.<kind>, dialmax.<kind>  that dial's range     dialjoint.<bone>  1 if the tab shows that joint
///   modal                  the dial the joint mouse mode turns (0/1/2 = bend / side / twist), -1 when off
///   figures                models in the scene          dirty  1 if the .pss document has unsaved changes
bool VulkanWindow::scriptMetric(const QString& name, double& value) {
    IkScript&       s = *m_script;
    // (The two document metrics need no figure: an empty scene is what they are asked about.)
    if (name.compare(QLatin1String("figures"), Qt::CaseInsensitive) == 0) {
        value = m_renderer ? static_cast<double>(scene().modelCount()) : 0.0;
        return true;
    }
    if (name.compare(QLatin1String("dirty"), Qt::CaseInsensitive) == 0) {
        value = isProjectDirty() ? 1.0 : 0.0;
        return true;
    }
    if (name.startsWith(QLatin1String("figure."), Qt::CaseInsensitive)) {
        // (The Figure group's rows: a posable figure's, or a selected STATIC model's — a splat
        // scene, an OBJ — which has no armature.)
        const JointTransform::Figure fig = jointTransform().figure;
        const QString                which = name.mid(7).toLower();
        value = which == QLatin1String("x") ? fig.x : which == QLatin1String("y") ? fig.y : which == QLatin1String("z") ? fig.z
                : which == QLatin1String("tip") ? fig.tipDeg : which == QLatin1String("tilt") ? fig.tiltDeg
                : which == QLatin1String("scale") ? fig.scalePct : which == QLatin1String("static") ? (fig.isStatic ? 1.0f : 0.0f)
                                                                                                   : fig.turnDeg;
        return true;
    }
    const Armature* arm = scene().figureArmature();
    if (arm == nullptr) {
        return false;
    }
    const auto moved = [&](int bone) { return movedSincePress(*arm, s, bone); };
    const auto axisOf = [&](int bone, const glm::vec3& local) {
        return glm::normalize(glm::mat3(arm->poseGlobal(static_cast<std::size_t>(bone))) * local);
    };
    const QStringList part = name.split(QLatin1Char('.'));
    const QString     key = part[0].toLower();
    const int         bone = part.size() > 1 ? arm->boneIndex(figureBoneName(arm, part[1].toStdString())) : -1;
    const int         head = arm->boneIndex(figureBoneName(arm, "head")); // (by role on a rig in another convention)
    if (key == QLatin1String("grab")) {
        glm::vec3 grab(0.0f);
        if (!s.dragging || !scene().ikGrabPointWorld(grab)) {
            return false;
        }
        value = glm::length(grab - s.cursorWorld) * 1000.0;
    } else if ((key == QLatin1String("dial") || key == QLatin1String("dialmin") || key == QLatin1String("dialmax")) &&
               part.size() > 1) {
        const JointTransform transform = jointTransform();
        const QString        which = part[1].toLower();
        const int            dial = which == QLatin1String("forward") ? 0 : which == QLatin1String("sideways") ? 1 : 2;
        if (!transform.valid || !transform.dials[dial].enabled) {
            return false;
        }
        value = key == QLatin1String("dial") ? transform.dials[dial].value
                : key == QLatin1String("dialmin") ? transform.dials[dial].minValue : transform.dials[dial].maxValue;
    } else if (key == QLatin1String("modal")) {
        value = m_modalKind; // the dial the mouse turns (0/1/2), -1 with the mode off
    } else if (key == QLatin1String("dialjoint") && bone >= 0) {
        const JointTransform transform = jointTransform();
        value = transform.valid && transform.name == arm->boneName(static_cast<std::size_t>(bone)) ? 1.0 : 0.0;
    } else if (key == QLatin1String("palm") && bone >= 0 && part.size() > 2) {
        const glm::vec3 pn = arm->handPalmNormal(bone);
        if (glm::length(pn) < 0.5f) {
            return false;
        }
        const QString axis = part[2].toLower();
        value = axis == QLatin1String("x") ? pn.x : axis == QLatin1String("y") ? pn.y : pn.z;
    } else if (key == QLatin1String("grabpx")) {
        // ... on SCREEN, in pixels: what the user sees. (A knee pulled up in a front view is taken
        // FORWARD onto its thigh's reach: under the pointer, and 40cm from the cursor's plane.)
        glm::vec3 grab(0.0f);
        QPointF   at;
        if (!s.dragging || !scene().ikGrabPointWorld(grab) || !projectToScreen(grab, at)) {
            return false;
        }
        value = std::hypot(at.x() - s.cursorPx.x(), at.y() - s.cursorPx.y());
    } else if (key == QLatin1String("hips")) {
        value = moved(arm->boneIndex(figureBoneName(arm, "hip"))); // (by the alias layer: any rig's hips)
    } else if (key == QLatin1String("head") && part.size() == 1) {
        value = moved(head);
    } else if (key == QLatin1String("group")) {
        value = static_cast<double>(scene().groupBones().size()) + (arm->selectedBone() >= 0 ? 1.0 : 0.0); // the selected joints
    } else if (key == QLatin1String("ingroup") && bone >= 0) {
        const int posed = arm->knownPosingBone(bone);
        value = (posed == arm->selectedBone() || arm->inGroup(posed)) ? 1.0 : 0.0;
    } else if (key == QLatin1String("steps")) {
        value = arm->ikRig() != nullptr ? arm->ikRig()->stepsTaken() : 0;
    } else if (key == QLatin1String("jump")) {
        value = s.jumpMax * 1000.0;
    } else if (key == QLatin1String("breastl") || key == QLatin1String("breastr")) {
        // THE SOFT BREASTS: the left / right breast's swing from the sculpt, degrees (0 with the
        // Posing preference off, or on a figure without pectoral bones).
        value = static_cast<double>(scene().breastSwingDeg(key == QLatin1String("breastl") ? 0 : 1));
    } else if (key == QLatin1String("breasthangl") || key == QLatin1String("breasthangr")) {
        // THE SOFT BREASTS: how far the left / right breast is drawn out along its tether, mm (0
        // standing, where the tether is level; a level-chested bow reads 30 on a full breast).
        value = static_cast<double>(scene().breastHangMm(key == QLatin1String("breasthangl") ? 0 : 1));
    } else if (key == QLatin1String("breastsidel") || key == QLatin1String("breastsider")) {
        // THE SOFT BREASTS: how far the left / right breast has moved sideways, mm, + outward toward the armpit
        // (on her back the spread, on her side the fall toward the lower side).
        value = static_cast<double>(scene().breastSideMm(key == QLatin1String("breastsidel") ? 0 : 1));
    } else if (key == QLatin1String("buttl") || key == QLatin1String("buttr")) {
        // THE SOFT BUTTOCKS: how far the left / right cheek has moved from the sculpt, mm (0 at
        // rest in any pose — they never hang — and while the Posing preference is off).
        value = static_cast<double>(scene().buttockMoveMm(key == QLatin1String("buttl") ? 0 : 1));
    } else if (key == QLatin1String("lowest")) {
        // (Not the figure node and its like: parentless, or hanging straight off a parentless node
        // at the origin — the hip's own parent.)
        const int hip = arm->boneIndex(figureBoneName(arm, "hip"));
        value = 1.0e9;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            bool body = false;
            for (int cur = static_cast<int>(i); cur >= 0 && !body; cur = arm->boneParent(static_cast<std::size_t>(cur))) {
                body = cur == hip;
            }
            if (body) {
                value = std::min(value, static_cast<double>(arm->boneWorldPosition(i).y) * 1000.0);
            }
        }
    } else if (key == QLatin1String("meshlow")) {
        // The posed MESH's lowest point over the floor, mm (the Ground button's measure: the skin,
        // CPU-skinned): negative is flesh through the floor, which no joint reading shows.
        float lowestY = 0.0f;
        if (!scene().figureGroundGap(lowestY)) {
            return false;
        }
        value = static_cast<double>(lowestY) * 1000.0;
    } else if (key == QLatin1String("gaze") && head >= 0) {
        value = glm::degrees(std::asin(glm::clamp(axisOf(head, glm::vec3(0.0f, 0.0f, 1.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("headroll") && head >= 0) {
        value = glm::degrees(std::asin(glm::clamp(axisOf(head, glm::vec3(1.0f, 0.0f, 0.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("tilt") && bone >= 0) {
        value = glm::degrees(std::acos(glm::clamp(axisOf(bone, glm::vec3(0.0f, 1.0f, 0.0f)).y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("hang") && bone >= 0) {
        // (An upper arm hanging by the side reads 15-25; one held out in front 60 and more.)
        int   child = -1;
        float longest = 0.0f;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            if (arm->boneParent(i) != bone) {
                continue;
            }
            const float len = glm::length(arm->boneWorldPosition(i) - arm->boneWorldPosition(static_cast<std::size_t>(bone)));
            if (len > longest) {
                longest = len;
                child = static_cast<int>(i);
            }
        }
        if (child < 0 || longest < 1.0e-4f) {
            return false;
        }
        const glm::vec3 dir = (arm->boneWorldPosition(static_cast<std::size_t>(child)) - arm->boneWorldPosition(static_cast<std::size_t>(bone))) / longest;
        value = glm::degrees(std::acos(glm::clamp(-dir.y, -1.0f, 1.0f)));
    } else if (key == QLatin1String("swing") && bone >= 0) {
        // The bone's fore-aft angle from straight down, degrees, + forward of the figure (the hips'
        // heading): an upper arm swung forward reads +20, one hanging back -20.
        int   child = -1;
        float longest = 0.0f;
        for (std::size_t i = 0; i < arm->boneCount(); ++i) {
            if (arm->boneParent(i) != bone) {
                continue;
            }
            const float len = glm::length(arm->boneWorldPosition(i) - arm->boneWorldPosition(static_cast<std::size_t>(bone)));
            if (len > longest) {
                longest = len;
                child = static_cast<int>(i);
            }
        }
        const int hip = arm->boneIndex(figureBoneName(arm, "hip"));
        if (child < 0 || longest < 1.0e-4f || hip < 0) {
            return false;
        }
        const glm::vec3 dir = (arm->boneWorldPosition(static_cast<std::size_t>(child)) - arm->boneWorldPosition(static_cast<std::size_t>(bone))) / longest;
        glm::vec3       fore = glm::mat3(arm->poseGlobal(static_cast<std::size_t>(hip))) * glm::vec3(0.0f, 0.0f, 1.0f);
        fore.y = 0.0f;
        fore = glm::length(fore) > 1.0e-4f ? glm::normalize(fore) : glm::vec3(0.0f, 0.0f, 1.0f);
        value = glm::degrees(std::atan2(glm::dot(dir, fore), -dir.y));
    } else if (key == QLatin1String("y") && bone >= 0) {
        value = arm->boneWorldPosition(static_cast<std::size_t>(bone)).y * 1000.0;
    } else if (key == QLatin1String("moved") && bone >= 0) {
        value = moved(bone);
    } else if (key == QLatin1String("selected") && bone >= 0) {
        value = arm->selectedBone() >= 0 && arm->sameRigidSegment(arm->selectedBone(), bone) ? 1.0 : 0.0;
    } else if (key == QLatin1String("lit") && bone >= 0) {
        // (What the mesh shader is told to tint: the highlighted joint and its twin — and what is lit
        // WITH the selection: the face rig under a selected head, a carpal under a selected hand.)
        value = arm->highlightBone() >= 0 && (bone == arm->highlightBone() || bone == arm->selectedHighlightTwin() ||
                                              arm->highlightedWithSelection(bone))
                    ? 1.0
                    : 0.0;
    } else if (key == QLatin1String("movedxz") && bone >= 0) {
        const std::size_t b = static_cast<std::size_t>(bone);
        if (b >= s.pressPositions.size()) {
            return false;
        }
        const glm::vec3 d = arm->boneWorldPosition(b) - s.pressPositions[b];
        value = static_cast<double>(glm::length(glm::vec2(d.x, d.z))) * 1000.0;
    } else if (key == QLatin1String("euler") && bone >= 0 && part.size() > 2) {
        const int axis = part[2].toLower() == QLatin1String("x") ? 0 : part[2].toLower() == QLatin1String("y") ? 1 : 2;
        value = arm->boneEuler(static_cast<std::size_t>(bone))[axis];
    } else {
        return false;
    }
    return true;
}

/// What a user would see, as numbers: where the grabbed point is against the cursor, what the
/// body did since the press, the feet, the steps.
void VulkanWindow::scriptReport(const char* label) {
    IkScript&       s = *m_script;
    const Armature* arm = scene().figureArmature();
    if (arm == nullptr) {
        return;
    }
    const auto index = [&](const char* name) { return arm->boneIndex(name); };
    const auto moved = [&](int bone) { return static_cast<float>(movedSincePress(*arm, s, bone)); };
    glm::vec3 grab(0.0f);
    const bool hasGrab = s.dragging && scene().ikGrabPointWorld(grab);
    const int  hip = index("hip");
    const int  head = index("head");
    const IkRig* rig = arm->ikRig();
    double gaze = 0.0;
    double lowest = 0.0;
    scriptMetric(QStringLiteral("gaze"), gaze);
    double meshLow = 0.0;
    scriptMetric(QStringLiteral("lowest"), lowest);
    scriptMetric(QStringLiteral("meshlow"), meshLow);
    say("[ikscript]   [%s] gaze %.1f deg | lowest joint %.0f mm | lowest skin %.0f mm | worst move: %s at tick %d\n", label, gaze,
        lowest, meshLow, s.jumpBone >= 0 ? arm->boneName(static_cast<std::size_t>(s.jumpBone)).c_str() : "-", s.jumpTick);
    say("[ikscript]   [%s] grab-to-cursor %.1f mm | hips moved %.1f mm, head %.1f mm | pelvis euler (%.1f %.1f %.1f) | "
        "steps %d | worst single-tick joint move %.1f mm\n",
        label, hasGrab ? glm::length(grab - s.cursorWorld) * 1000.0f : 0.0f, moved(hip), moved(head),
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).x : 0.0f,
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).y : 0.0f,
        hip >= 0 ? arm->boneEuler(static_cast<std::size_t>(hip)).z : 0.0f,
        rig != nullptr ? rig->stepsTaken() : 0, s.jumpMax * 1000.0f);
    // The posed channels, for a record of what the pose IS (bones turned more than half a degree).
    QString posed;
    int     listed = 0;
    for (std::size_t i = 0; i < arm->boneCount() && listed < 40; ++i) {
        const glm::vec3& e = arm->boneEuler(i);
        if (std::max({std::abs(e.x), std::abs(e.y), std::abs(e.z)}) > 0.5f && arm->boneWorldPosition(i).y > -1.0f) {
            const std::string& name = arm->boneName(i);
            if (name.find("Toe") != std::string::npos || name.find("Carpal") != std::string::npos ||
                name.find("Index") != std::string::npos || name.find("Mid") != std::string::npos ||
                name.find("Ring") != std::string::npos || name.find("Pinky") != std::string::npos ||
                name.find("Thumb") != std::string::npos) {
                continue; // (digits: noise here)
            }
            posed += QStringLiteral(" %1(%2 %3 %4)").arg(QString::fromStdString(name)).arg(e.x, 0, 'f', 0).arg(e.y, 0, 'f', 0).arg(e.z, 0, 'f', 0);
            ++listed;
        }
    }
    say("[ikscript]   [%s] posed:%s\n", label, qPrintable(posed));
}

} // namespace pose
