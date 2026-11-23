// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanwindow_pose.cpp
 * @brief VulkanWindow's discrete pose edits — utilities, pins, pose files, delete, the Transform
 *        tab's dials and the joint mouse mode — and the unified undo/redo stack every edit of
 *        the scene commits into.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h for the map. Every
 * pose edit here is BRACKETED the same way: refuse while a button-held drag owns the pose
 * (dragInFlight), close every self-completing edit (closeOpenPoseEdits — the release settle,
 * the fall, an open dial edit — each commits its own entry), snapshot m_preEditPose, edit, run the
 * settled-pose hook, commitPoseUndo (a no-op when nothing changed), request a frame. The undo
 * stack is ONE chronological list of four kinds of entry — a pose, the lighting dials, an
 * object's materials, the light objects; each pose entry records the figure it belongs to, so
 * undo re-activates that figure before applying it. (The material and light edits' own
 * brackets are in vulkanwindow_materials.cpp and vulkanwindow_lights.cpp; they push into the
 * stack here, and swapUndoEntry applies every kind.)
 */

#include "vulkanwindow.h"

#include "rendering/vulkanrenderer.h"
#include "scene/model.h"
#include "scene/scene.h"

#include <QCursor>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pose {

namespace {
/// THE JOINT MOUSE MODE's feel (jointModalMove). Where a dial's swing crosses the screen the limb
/// follows the mouse one to one — but never faster than a lever this long would (in pixels, per
/// radian): a finger seen with the whole figure in view is a few pixels long, and followed one to
/// one a pixel of mouse would be several degrees of knuckle. Also the rate of the plain
/// left-right fallback: 150 pixels of mouse a radian, 2.6 pixels a degree.
constexpr double kModalMinLeverPx = 150.0;
/// WHICH WAY the mouse turns a dial is settled when the mode begins, and kept for the gesture: a
/// bend whose swing crosses the screen with a lever of this many pixels or more is FOLLOWED (the
/// limb goes the way the mouse goes); one that goes toward or away from the camera — and every
/// twist, which turns the limb about its own length — is turned by the mouse's left-right, right
/// for more. (Settled afresh at every move it flipped mid-gesture: a thigh flexed in the front
/// view starts straight at the camera and comes up the screen as it rises, and the mouse that
/// had been bending it by going right stopped doing anything.)
constexpr double kModalFollowFromPx = 40.0;
/// ... and a followed swing that turns toward the camera mid-gesture keeps its last direction
/// once its lever on screen is shorter than this (a vector that short points nowhere).
constexpr double kModalMinDirectionPx = 12.0;
constexpr double kDegreesPerRadian = 57.29577951308232;
} // namespace

void VulkanWindow::closeSettlingEdits() {
    finishIkSettle();
    finishGroundFall();
    endJointTransformEdit(); // (the joint mouse mode's too: what it made is kept)
    endMaterialEdit();       // (a material scrub's bracket: what it set is kept)
    endLightEdit();          // (a light scrub's too)
}

void VulkanWindow::closeOpenPoseEdits() {
    closeSettlingEdits();
    if (m_ik.dragging) {
        abortIkDrag(); // a stale drag (or a delete mid-gesture): no settle, the entry is kept
    }
}

void VulkanWindow::commitPoseUndo() {
    if (m_renderer && scene().capturePose() != m_preEditPose) {
        UndoEntry entry;
        entry.kind = UndoEntry::Kind::Pose;
        entry.pose = m_preEditPose;
        entry.figure = scene().activeFigureIndex();
        m_undoStack.push_back(std::move(entry));
        m_redoStack.clear();
        markProjectDirty(); // a committed pose edit is a document change
    }
}

void VulkanWindow::runPoseUtility(PoseUtility what) {
    if (!m_renderer || dragInFlight()) {
        return; // not mid-gesture: the rig captured its pins and pose at drag start
    }
    closeOpenPoseEdits(); // land the previous release / hold first; this edit starts from its result
    m_preEditPose = scene().capturePose();
    switch (what) {
    case PoseUtility::ResetJoint:
        scene().resetSelectedJoint(false);
        break;
    case PoseUtility::ResetLimb:
        scene().resetSelectedJoint(true);
        break;
    case PoseUtility::ResetPose:
        scene().resetPose();
        break;
    case PoseUtility::MirrorPose:
        scene().mirrorPose();
        break;
    case PoseUtility::MirrorLimb:
        scene().mirrorSelectedLimb();
        break;
    }
    scene().finalizePose(); // the settled-pose hook (correctives already follow per frame)
    commitPoseUndo();       // no-op if nothing changed (no selection, already at rest)
    requestUpdate();
}

// --- The Transform tab's dials -------------------------------------------------------------------

JointTransform VulkanWindow::jointTransform() const {
    return m_renderer ? scene().selectedJointTransform() : JointTransform{};
}

bool VulkanWindow::beginJointTransformEdit() {
    if (m_transformEdit) {
        return true; // already open (a set opened it, or a nested begin)
    }
    if (!m_renderer || dragInFlight()) {
        return false; // not mid-gesture: the drag owns the pose until its release
    }
    closeOpenPoseEdits(); // land a release settle / fall first; this edit starts from its result
    m_preEditPose = scene().capturePose();
    m_transformEdit = true;
    for (bool& valid : m_dialAskedValid) {
        valid = false; // (a new bracket: the first ask of each dial is measured against the pose)
    }
    // (A static model's rows: its transform as the bracket finds it — see m_editStaticModel.)
    m_editStaticModel = -1;
    if (jointTransform().figure.isStatic) {
        const int sel = scene().selectedModelIndex(); // (the rows are the selected model's)
        if (const Model* model = sel >= 0 ? scene().modelAt(static_cast<std::size_t>(sel)) : nullptr) {
            m_editStaticModel = sel;
            m_preEditStaticTransform = model->transform();
        }
    }
    return true;
}

void VulkanWindow::setJointTransformDial(int dial, double value, bool absolute) {
    if (!beginJointTransformEdit()) {
        return;
    }
    // (Limits clamp and the body volumes stop it inside; correctives follow live on the GPU.)
    const bool  inRange = dial >= 0 && dial < 3;
    const bool  knownAsk = inRange && m_dialAskedValid[dial];
    const float previous = knownAsk ? m_dialAsked[dial] : 0.0f;
    if (scene().setSelectedJointDial(dial, static_cast<float>(value), absolute, knownAsk ? &previous : nullptr)) {
        requestUpdate();
    }
    if (inRange) {
        m_dialAsked[dial] = static_cast<float>(value);
        m_dialAskedValid[dial] = true;
    }
}

void VulkanWindow::setFigurePosition(int axis, double cm) {
    if (!beginJointTransformEdit()) {
        return;
    }
    if (scene().setFigurePosition(axis, static_cast<float>(cm))) {
        requestUpdate();
    }
}

void VulkanWindow::setFigureRotation(int axis, double degrees) {
    if (!beginJointTransformEdit()) {
        return;
    }
    if (scene().setFigureRotationDeg(axis, static_cast<float>(degrees))) {
        requestUpdate();
    }
}

void VulkanWindow::setFigureScale(double pct) {
    if (!beginJointTransformEdit()) {
        return;
    }
    if (scene().setFigureScalePct(static_cast<float>(pct))) {
        requestUpdate();
    }
}

void VulkanWindow::endJointTransformEdit() {
    if (!m_transformEdit) {
        return;
    }
    m_transformEdit = false;
    const bool modal = m_modal.kind >= 0; // the joint mouse mode is this bracket: it ends with it
    m_modal.kind = -1;
    const int staticModel = m_editStaticModel;
    m_editStaticModel = -1;
    if (m_renderer) {
        scene().finalizePose(); // the settled-pose hook (correctives already followed the dial live)
        commitPoseUndo();       // no-op if the dial never moved the pose
        // A STATIC model's placement is no pose (no undo entry), but it is saved with the project:
        // moved, turned or scaled, the document has changed.
        const Model* model = (staticModel >= 0 && static_cast<std::size_t>(staticModel) < scene().modelCount())
                                 ? scene().modelAt(static_cast<std::size_t>(staticModel))
                                 : nullptr;
        if (model != nullptr && model->transform() != m_preEditStaticTransform) {
            markProjectDirty();
        }
        requestUpdate();
    }
    if (modal) {
        emit jointModalChanged(-1);
    }
}

// --- The joint mouse mode (B / S / T) ------------------------------------------------------------

bool VulkanWindow::beginJointModal(int kind, const QPointF& cursor) {
    if (!m_renderer || kind < 0 || kind >= kJointDialCount) {
        return false;
    }
    if (m_modal.kind >= 0) {
        // The same key again drops the mode; another of the three drops it and begins that one.
        const bool same = m_modal.kind == kind;
        cancelJointModal();
        if (same) {
            return false;
        }
    }
    if (dragInFlight()) {
        return false; // the drag owns the pose (and the mouse) until its release
    }
    // Nor while a button pressed in the viewport is still DOWN (an orbit, a pan, a click not yet
    // released): that button's release is not the mode's — begun here, the mode would have had a
    // click's release select another model, or clear the selection, under its open edit. (Asked
    // of the mouse as it is: a button whose release never reached this window blocks nothing.)
    if (m_pointer.activeDragButtons & QGuiApplication::mouseButtons()) {
        return false;
    }
    closeSettlingEdits(); // a release settle, a fall, the tab's own open edit: this one starts from their result
    const JointTransform now = jointTransform();
    if (!now.valid || !now.dials[kind].enabled) {
        return false; // nothing selected, or a motion the joint does not have (a hinge elbow's side bend)
    }
    if (!beginJointTransformEdit()) {
        return false;
    }
    m_modal.kind = kind;
    m_modal.cursor = cursor;
    QPointF direction;
    double  leverPx = 0.0;
    m_modal.follow = kind != static_cast<int>(JointDialKind::Twist) && jointModalSwing(direction, leverPx) &&
                    leverPx >= kModalFollowFromPx;
    m_modal.direction = m_modal.follow ? direction : QPointF(1.0, 0.0);
    emit jointModalChanged(kind);
    return true;
}

bool VulkanWindow::toggleJointModal(int kind) {
    // (A scripted test's cursor stands in the middle of the view: the desktop's is somebody else's.)
    const QPointF cursor = m_script ? QPointF(width() * 0.5, height() * 0.5) : QPointF(mapFromGlobal(QCursor::pos()));
    return beginJointModal(kind, cursor);
}

void VulkanWindow::moveJointModalTo(const QPointF& globalPos) {
    jointModalMove(mapFromGlobal(globalPos)); // the viewport's own frame, as its mouseMoveEvent reads it
}

bool VulkanWindow::jointModalSwing(QPointF& direction, double& leverPx) const {
    JointDialSweep sweep;
    QPointF        from, to;
    if (m_modal.kind < 0 || !m_renderer || !scene().selectedJointDialSweep(m_modal.kind, sweep) ||
        !projectToScreen(sweep.tip, from) || !projectToScreen(sweep.tip + sweep.tipPerDegree, to)) {
        return false;
    }
    const QPointF perDegree = to - from;
    const double  length = std::hypot(perDegree.x(), perDegree.y());
    leverPx = length * kDegreesPerRadian;
    if (length <= 0.0) {
        return false;
    }
    direction = perDegree / length;
    return true;
}

void VulkanWindow::jointModalMove(const QPointF& cursor) {
    if (m_modal.kind < 0 || !m_renderer) {
        return;
    }
    const QPointF move = cursor - m_modal.cursor;
    m_modal.cursor = cursor;
    if (move.isNull()) {
        return;
    }
    const JointTransform now = jointTransform();
    JointDialSweep       sweep;
    if (!now.valid || !now.dials[m_modal.kind].enabled || !scene().selectedJointDialSweep(m_modal.kind, sweep) ||
        sweep.degreesPerUnit <= 0.0f) {
        confirmJointModal(); // the joint went away under the mode (another figure, a cleared selection)
        return;
    }
    // THE LIMB GOES THE WAY THE MOUSE GOES: the dial's swing, projected — where the limb's far end
    // moves on screen per degree — is the direction that counts, and along it the end follows the
    // mouse one to one (a lever of so many pixels a radian). Read afresh at every move, so a
    // mouse carried round the joint carries the limb round with it. A dial that was not followed
    // when the mode began (see kModalFollowFromPx) is turned by left-right throughout.
    double leverPx = 0.0;
    if (m_modal.follow) {
        QPointF direction;
        if (jointModalSwing(direction, leverPx) && leverPx >= kModalMinDirectionPx) {
            m_modal.direction = direction;
        } else {
            leverPx = 0.0; // (turned toward the camera: the last direction, at the floor's pace)
        }
    }
    const QPointF direction = m_modal.direction;
    const double  along = move.x() * direction.x() + move.y() * direction.y();
    const double degrees = along / std::max(leverPx, kModalMinLeverPx) * kDegreesPerRadian;
    // (From the value the pose HAS: the limits and the body stop a dial short of what it is
    // asked, and the mouse turned back must move the joint at once, not first undo an overshoot.)
    // … and THE GROUP's other joints turn by that same change — the mouse's own. The dial bracket
    // moves them by the change between two ASKS (the panel's rule: it re-asks absolute values),
    // and an ask made from the read-back is not the last ask plus the mouse's move once the
    // primary has been stopped short: turned back off a limit, the others went back by the move
    // AND the shortfall. Forgetting the last ask makes the set measure against the pose instead.
    m_dialAskedValid[m_modal.kind] = false;
    setJointTransformDial(m_modal.kind, now.dials[m_modal.kind].value + degrees / sweep.degreesPerUnit);
}

void VulkanWindow::confirmJointModal() {
    if (m_modal.kind >= 0) {
        endJointTransformEdit(); // the undo entry; ends the mode with the bracket
    }
}

void VulkanWindow::cancelJointModal() {
    if (m_modal.kind < 0) {
        return;
    }
    m_modal.kind = -1;
    m_transformEdit = false; // the bracket is dropped, not committed
    m_editStaticModel = -1;
    if (m_renderer) {
        scene().applyPose(m_preEditPose); // as it was at the key
        scene().finalizePose();
        requestUpdate();
    }
    emit jointModalChanged(-1);
}

void VulkanWindow::notifyJointTransform() {
    JointTransform now = jointTransform();
    if (now != m_announcedJointTransform) {
        m_announcedJointTransform = std::move(now);
        emit jointTransformChanged();
    }
}

void VulkanWindow::resetSelectedJoint() { runPoseUtility(PoseUtility::ResetJoint); }
void VulkanWindow::resetSelectedLimb() { runPoseUtility(PoseUtility::ResetLimb); }
void VulkanWindow::resetPose() { runPoseUtility(PoseUtility::ResetPose); }
void VulkanWindow::mirrorPose() { runPoseUtility(PoseUtility::MirrorPose); }
void VulkanWindow::mirrorSelectedLimb() { runPoseUtility(PoseUtility::MirrorLimb); }

void VulkanWindow::togglePinSelectedJoint() {
    if (!m_renderer || dragInFlight()) {
        return; // the rig captured its pins at drag start
    }
    closeOpenPoseEdits();
    m_preEditPose = scene().capturePose(); // a pin toggle is an undoable pose edit
    scene().togglePinSelectedBone();
    commitPoseUndo();
    requestUpdate();
}

void VulkanWindow::unpinAllJoints() {
    if (!m_renderer || dragInFlight()) {
        return;
    }
    closeOpenPoseEdits();
    m_preEditPose = scene().capturePose();
    scene().unpinAllBones();
    commitPoseUndo();
    requestUpdate();
}

void VulkanWindow::deleteSelectedObject() {
    if (!m_renderer || m_lightDrag.active) {
        return; // (not mid-gesture: a light being dragged is the selection, and the drag holds its index)
    }
    if (scene().selectedLight() >= 0) {
        removeLight(scene().selectedLight()); // the selection is a light: it goes (one undo entry)
        return;
    }
    deleteModel(scene().selectedModelIndex());
}

void VulkanWindow::deleteModel(int index) {
    if (!m_renderer || index < 0) {
        return;
    }
    // Deleting the dragged figure mid-gesture (left button still held while the context menu
    // opened), or with a release settle / fall still animating or a dial edit open: end them cleanly
    // first — a settle or fall would otherwise keep ticking past the delete and retarget to
    // whatever figure remains.
    closeOpenPoseEdits();
    m_renderer->deleteModel(static_cast<std::size_t>(index));
    // Model indices shift and the deleted figure's poses are meaningless: the pose history goes
    // with it (lighting entries too — one chronological stack).
    m_undoStack.clear();
    m_redoStack.clear();
    requestUpdate();
    markProjectDirty(); // the scene's contents changed
}

bool VulkanWindow::savePose(const QString& path) {
    closeOpenPoseEdits(); // serialize the LANDED pose, not a transient mid-settle/mid-fall frame
    return m_renderer && scene().savePose(path.toStdString());
}

bool VulkanWindow::loadPose(const QString& path) {
    if (!m_renderer) {
        m_deferred.pendingPose = path; // applied after the queued figure is drained (open-with a .pose)
        return true;
    }
    if (dragInFlight()) {
        return false; // the drag owns the pose until its release
    }
    // An undoable pose edit like any other: the file replaces the whole snapshot (rotations,
    // translations, pins), and Ctrl+Z brings the previous pose back.
    closeOpenPoseEdits(); // don't let a still-animating release settle fight the loaded pose
    m_preEditPose = scene().capturePose();
    if (!scene().loadPose(path.toStdString())) {
        return false;
    }
    scene().finalizePose();
    commitPoseUndo();
    requestUpdate(); // repaint with the restored pose
    return true;
}

void VulkanWindow::registerLightingUndo(const LightingSettings& preEdit) {
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::Lighting;
    entry.lighting = preEdit;
    m_undoStack.push_back(std::move(entry));
    m_redoStack.clear(); // a fresh edit invalidates the redo branch, same as a pose edit
    markProjectDirty(); // the environment settings are part of the document
}

void VulkanWindow::swapUndoEntry(std::vector<UndoEntry>& from, std::vector<UndoEntry>& to) {
    if (dragInFlight()) {
        return; // mid-gesture (Ctrl+Z with the button held): the drag owns the pose right now
    }
    if (m_lightDrag.active) {
        // A light marker's drag holds an INDEX into the list of lights, which an undone add or
        // removal would leave pointing at another light: with the button held the drag owns the
        // list, as a posing drag owns the pose; a stale one (its release never reached this
        // window) ends here.
        if (m_pointer.activeDragButtons.testFlag(Qt::LeftButton)) {
            return;
        }
        endLightGesture();
    }
    closeOpenPoseEdits(); // a pending release settle / open dial edit must commit its own entry first
    if (!m_renderer || from.empty()) {
        return;
    }
    UndoEntry entry = std::move(from.back());
    from.pop_back();
    UndoEntry counter; // the state this entry replaces — what the opposite direction restores
    counter.kind = entry.kind;
    if (entry.kind == UndoEntry::Kind::Pose) {
        scene().setActiveFigure(entry.figure); // the snapshot belongs to that figure
        counter.figure = entry.figure;
        counter.pose = scene().capturePose();
        scene().applyPose(entry.pose);
        scene().finalizePose(); // re-runs correctives, like any pose change
    } else if (entry.kind == UndoEntry::Kind::Material) {
        // The zones' materials of that object back to the snapshot — the object selected, so the
        // Materials tab shows what changed (the frame's notify tells it).
        counter.figure = entry.figure;
        counter.materials = scene().captureMaterials(entry.figure);
        applyMaterialTextures(entry.figure, entry.materials); // the slots first: the values keep the mesh's maps
        scene().applyMaterials(entry.figure, entry.materials);
        syncChosenMapFills(entry.figure); // (the values just applied: a chosen map's gutter fill follows the zone's Tiling)
        syncOverlayBakes(entry.figure);   // (… and an overlay's metal is baked for the zone's Metallic)
        scene().setSelectedModel(entry.figure);
    } else if (entry.kind == UndoEntry::Kind::Lights) {
        // The light objects back to the snapshot (the frame's notify tells the Lights tab).
        counter.lights = m_deferred.lights;
        applyLights(entry.lights);
    } else {
        counter.lighting = m_deferred.lighting; // the current dials
        setLightingSettings(entry.lighting);
        emit lightingRestored(entry.lighting); // the Environment panel syncs its widgets
    }
    to.push_back(std::move(counter));
    requestUpdate();
    markProjectDirty(); // undo/redo changes the scene
}

void VulkanWindow::undo() {
    swapUndoEntry(m_undoStack, m_redoStack);
}

void VulkanWindow::redo() {
    swapUndoEntry(m_redoStack, m_undoStack);
}

} // namespace pose
