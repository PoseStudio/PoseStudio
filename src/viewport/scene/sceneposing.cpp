/**
 * @file sceneposing.cpp
 * @brief Scene's posing API: thin forwarders to the ACTIVE figure (Scene::figureModel) for
 *        FK/IK posing, pins, the pose utilities, grounding, and the pose snapshot + file.
 *
 * A second translation unit of Scene (no header of its own) so scene.cpp stays the rendering
 * orchestration and this file stays the one-liners. Every function here has the same shape —
 * find the figure, forward, no-op without one — and none of them knows anything the Model /
 * Armature don't; new posing behaviour belongs in the armature, never here (see armature.h).
 */

#include "scene.h"

#include "ikmath.h"
#include "model.h"
#include "posefile.h"

namespace pose {

void Scene::nudgeSelectedBone(const glm::vec3& deltaEulerDegrees) {
    if (Model* fig = figureModel()) {
        fig->nudgeSelectedBone(deltaEulerDegrees);
    }
}

namespace {

// A STATIC model's transform as the Transform tab's rows read it: translation, (turn, tip,
// tilt) by the figure's own convention (Armature::figureRotationDeg: "YXZ", the tilt shown as the
// negative roll), and the uniform scale.
void decomposeStaticTransform(const glm::mat4& m, glm::vec3& translation, glm::vec3& turnTipTilt, float& scale) {
    translation = glm::vec3(m[3]);
    glm::mat3 rot(m);
    scale = glm::length(rot[0]);
    if (scale < 1e-8f) {
        scale = 1.0f;
    }
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(rot[c]);
        if (len > 1e-8f) {
            rot[c] /= len;
        }
    }
    const glm::vec3 e = eulerFromMatrix(rot, "YXZ"); // (e.x pitch, e.y yaw, e.z roll)
    turnTipTilt = glm::vec3(e.y, e.x, -e.z);
}

glm::mat4 composeStaticTransform(const glm::vec3& translation, const glm::vec3& turnTipTilt, float scale) {
    glm::mat4 m(eulerMatrix(glm::vec3(turnTipTilt.y, turnTipTilt.x, -turnTipTilt.z), "YXZ"));
    for (int c = 0; c < 3; ++c) {
        m[c] *= scale;
    }
    m[3] = glm::vec4(translation, 1.0f);
    return m;
}

} // namespace

Model* Scene::selectedStaticModel() {
    if (figureModel() != nullptr) {
        return nullptr;
    }
    const int sel = selectedModelIndex();
    Model*    model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    return (model != nullptr && !model->hasSkeleton()) ? model : nullptr;
}

const Model* Scene::selectedStaticModel() const {
    return const_cast<Scene*>(this)->selectedStaticModel();
}

bool Scene::setFigureScalePct(float pct) {
    Model* model = selectedStaticModel();
    if (model == nullptr) {
        return false;
    }
    glm::vec3 t, rot;
    float     scale;
    decomposeStaticTransform(model->transform(), t, rot, scale);
    model->setTransform(composeStaticTransform(t, rot, std::max(0.01f, pct / 100.0f)));
    return true;
}

JointTransform Scene::selectedJointTransform() const {
    JointTransform out;
    const Model*   fig = figureModel();
    if (fig == nullptr) {
        // A STATIC model selected (an OBJ, a splat scene): its place, orientation and scale off
        // its model transform, the rows the Figure group shows for a figure.
        if (const Model* model = selectedStaticModel()) {
            glm::vec3 t, rot;
            float     scale;
            decomposeStaticTransform(model->transform(), t, rot, scale);
            out.figure = {true, t.x * 100.0f, t.y * 100.0f, t.z * 100.0f, rot.x, rot.y, rot.z, scale * 100.0f, true};
        }
        return out;
    }
    const Armature& arm = fig->armature();
    if (arm.figureRoot() >= 0) {
        // The figure's place and heading: valid with any posable figure, a joint selected or not.
        const glm::vec3 at = arm.figurePosition() * 100.0f;
        const glm::vec3 rot = arm.figureRotationDeg();
        out.figure = {true, at.x, at.y, at.z, rot.x, rot.y, rot.z};
    }
    const int joint = arm.dialJoint(arm.highlightBone());
    if (joint < 0) {
        return out;
    }
    out.valid = true;
    out.name = arm.boneName(static_cast<std::size_t>(joint));
    for (int k = 0; k < kJointDialCount; ++k) {
        const JointDial dial = arm.jointDial(joint, static_cast<JointDialKind>(k));
        if (dial.valid()) {
            out.dials[k] = {true, arm.jointDialValue(dial), dial.minValue, dial.maxValue};
        }
    }
    return out;
}

bool Scene::setSelectedJointDial(int dial, float value) {
    Model* fig = figureModel();
    if (fig == nullptr || dial < 0 || dial >= kJointDialCount) {
        return false;
    }
    const Armature& arm = fig->armature();
    return fig->setJointDial(arm.jointDial(arm.highlightBone(), static_cast<JointDialKind>(dial)), value);
}

bool Scene::setFigurePosition(int axis, float cm) {
    if (axis < 0 || axis > 2) {
        return false;
    }
    if (Model* model = selectedStaticModel()) {
        glm::vec3 t, rot;
        float     scale;
        decomposeStaticTransform(model->transform(), t, rot, scale);
        t[axis] = cm / 100.0f;
        model->setTransform(composeStaticTransform(t, rot, scale));
        return true;
    }
    Model* fig = figureModel();
    if (fig == nullptr || fig->armature().figureRoot() < 0) {
        return false;
    }
    glm::vec3 at = fig->armature().figurePosition();
    at[axis] = cm / 100.0f;
    fig->setFigurePosition(at);
    return true;
}

bool Scene::setFigureRotationDeg(int axis, float degrees) {
    if (axis < 0 || axis > 2) {
        return false;
    }
    if (Model* model = selectedStaticModel()) {
        glm::vec3 t, rot;
        float     scale;
        decomposeStaticTransform(model->transform(), t, rot, scale);
        rot[axis] = degrees;
        model->setTransform(composeStaticTransform(t, rot, scale));
        return true;
    }
    Model* fig = figureModel();
    if (fig == nullptr || fig->armature().figureRoot() < 0) {
        return false;
    }
    glm::vec3 rot = fig->armature().figureRotationDeg();
    rot[axis] = degrees;
    fig->setFigureRotationDeg(rot);
    return true;
}

bool Scene::selectedJointDialSweep(int dial, JointDialSweep& sweep) const {
    const Model* fig = figureModel();
    if (fig == nullptr || dial < 0 || dial >= kJointDialCount) {
        return false;
    }
    const Armature& arm = fig->armature();
    return arm.jointDialSweep(arm.jointDial(arm.highlightBone(), static_cast<JointDialKind>(dial)), sweep);
}

void Scene::finalizePose() {
    if (Model* fig = figureModel()) {
        fig->refreshCorrectives();
    }
}

bool Scene::beginBoneIkDrag(IkScope scope) {
    Model* fig = figureModel();
    return fig != nullptr && fig->beginIkDrag(scope);
}

bool Scene::dragBoneIkTo(const glm::vec3& targetWorld) {
    Model* fig = figureModel();
    return fig && fig->dragIkTo(targetWorld);
}

bool Scene::settleBoneIkTick() {
    Model* fig = figureModel();
    return fig && fig->settleIkTick();
}

void Scene::endBoneIkDrag() {
    if (Model* fig = figureModel()) {
        fig->endIkDrag();
    }
}

const Armature* Scene::figureArmature() const {
    const Model* fig = figureModel();
    return fig != nullptr ? &fig->armature() : nullptr;
}

bool Scene::ikGrabPointWorld(glm::vec3& out) const {
    const Model* fig = figureModel();
    if (!fig || fig->selectedBone() < 0 ||
        fig->selectedBone() >= static_cast<int>(fig->boneCount())) {
        return false;
    }
    out = fig->ikGrabPointWorld();
    return true;
}

void Scene::setIkGrabOnSegment(float share) {
    if (Model* fig = figureModel()) {
        fig->setIkGrabOnSegment(fig->selectedBone(), share);
    }
}

bool Scene::selectedBoneWorldPosition(glm::vec3& out) const {
    const Model* fig = figureModel();
    if (!fig || fig->selectedBone() < 0 ||
        fig->selectedBone() >= static_cast<int>(fig->boneCount())) {
        return false;
    }
    out = fig->boneWorldPosition(static_cast<std::size_t>(fig->selectedBone()));
    return true;
}

bool Scene::figureGroundGap(float& lowestY) const {
    const Model* fig = figureModel();
    return fig != nullptr && fig->groundGap(lowestY);
}

void Scene::translateModelY(int index, float dy) {
    if (index >= 0 && static_cast<std::size_t>(index) < m_models.size()) {
        m_models[static_cast<std::size_t>(index)]->translateY(dy);
    }
}

bool Scene::beginLandingBounce(int index, float impactSpeed) {
    return index >= 0 && static_cast<std::size_t>(index) < m_models.size() &&
           m_models[static_cast<std::size_t>(index)]->hasSkeleton() &&
           m_models[static_cast<std::size_t>(index)]->beginLandingBounce(impactSpeed);
}

bool Scene::landingBounceTick(int index, float seconds) {
    return index >= 0 && static_cast<std::size_t>(index) < m_models.size() &&
           m_models[static_cast<std::size_t>(index)]->landingBounceTick(seconds);
}

bool Scene::tickSoftBody(int index, float dt) {
    return index >= 0 && static_cast<std::size_t>(index) < m_models.size() && m_models[static_cast<std::size_t>(index)]->hasSkeleton() &&
           m_models[static_cast<std::size_t>(index)]->tickSoftBody(dt);
}

bool Scene::softBodyMoving(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < m_models.size() && m_models[static_cast<std::size_t>(index)]->softBodyMoving();
}

void Scene::settleSoftBody(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < m_models.size()) {
        m_models[static_cast<std::size_t>(index)]->settleSoftBody();
    }
}

void Scene::applySoftBodySettings() {
    for (auto& model : m_models) {
        if (model->hasSkeleton()) {
            model->applySoftBodySettings();
        }
    }
}

float Scene::breastSwingDeg(int side) const {
    const Model* fig = figureModel();
    return fig ? fig->breastSwingDeg(side) : 0.0f;
}

float Scene::breastHangMm(int side) const {
    const Model* fig = figureModel();
    return fig ? fig->breastHangMm(side) : 0.0f;
}

float Scene::breastSideMm(int side) const {
    const Model* fig = figureModel();
    return fig ? fig->breastSideMm(side) : 0.0f;
}

float Scene::buttockMoveMm(int side) const {
    const Model* fig = figureModel();
    return fig ? fig->buttockMoveMm(side) : 0.0f;
}

void Scene::endLandingBounce(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < m_models.size()) {
        m_models[static_cast<std::size_t>(index)]->endLandingBounce();
    }
}

bool Scene::togglePinSelectedBone() {
    Model* fig = figureModel();
    return fig && fig->togglePinSelectedBone();
}

bool Scene::selectedBonePinned() const {
    const Model* fig = figureModel();
    return fig && fig->selectedBonePinned();
}

bool Scene::hasPinnedBones() const {
    const Model* fig = figureModel();
    return fig && fig->hasPinnedBones();
}

void Scene::unpinAllBones() {
    if (Model* fig = figureModel()) {
        fig->unpinAllBones();
    }
}

bool Scene::resetSelectedJoint(bool subtree) {
    Model* fig = figureModel();
    return fig && fig->resetSelectedBone(subtree);
}

void Scene::resetPose() {
    if (Model* fig = figureModel()) {
        fig->resetPose();
    }
}

void Scene::mirrorPose() {
    if (Model* fig = figureModel()) {
        fig->mirrorPose();
    }
}

bool Scene::mirrorSelectedLimb() {
    Model* fig = figureModel();
    return fig && fig->mirrorSelectedLimb();
}

std::vector<std::pair<std::string, glm::vec3>> Scene::capturePose() const {
    const Model* fig = figureModel();
    return fig ? fig->capturePose() : std::vector<std::pair<std::string, glm::vec3>>{};
}

void Scene::applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose) {
    if (Model* fig = figureModel()) {
        fig->applyPose(pose);
    }
}

bool Scene::savePose(const std::string& path) const {
    // The whole snapshot goes to the file — rotations plus the @trans:/@pin: rows (posefile.h).
    const Model* fig = figureModel();
    return fig != nullptr && writePoseFile(path, fig->capturePose());
}

bool Scene::loadPose(const std::string& path) {
    Model* fig = figureModel();
    if (!fig) {
        return false;
    }
    PoseRows pose;
    if (!readPoseFile(path, pose)) {
        return false; // unreadable or malformed: the figure's pose is left untouched
    }
    fig->applyPose(pose);
    return true;
}

} // namespace pose
