// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file scenepicking.cpp
 * @brief Scene's CPU picking and the selections it makes: what a click, a Shift press or a VR
 *        hand is ON — a light's marker, a model's surface, a figure's joint.
 *
 * A translation unit of Scene (no header of its own): scene.cpp records the frame,
 * sceneposing.cpp forwards the posing calls, scenematerials.cpp is the Materials tab's side,
 * and this is the picking. Nothing here touches a Vulkan object — the picks are rays and
 * projections against the models' CPU surfaces (Model::pickSurface / pickExact: groundsampler.h,
 * splatcloud.h) and the joints' world positions (bonepicker.h). Three rules, each stated above
 * its code: a LIGHT is picked by its marker (pickLight); a MODEL by the surface under the
 * cursor, and only off every surface by its box (pickModel); a JOINT by the skin under the
 * cursor, snapped to a joint of the segment hit, and by its 2D projection only for a click just
 * off the silhouette (pickBoneAt, with its two phases snapToSegmentJoint and fallbackPick2D).
 * The selection the joint picks make — the active figure, the selected joint, the IK grab
 * point, the Materials tab's zone — is selectPickedBone.
 */

#include "scene.h"

#include "bonepicker.h"
#include "camera.h"
#include "ikmath.h" // closestSegmentPoints
#include "model.h"
#include "ray.h"

#include <glm/glm.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace pose {

int Scene::pickLight(const Camera& camera, float x, float y, float width, float height) const {
    // A light is picked by its MARKER: the point it stands at, within a finger's width on screen.
    constexpr float kPickRadiusPx = 18.0f;
    const glm::mat4 viewProj = camera.viewProjection();
    int             best = -1;
    float           bestDepth = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < m_lights.size(); ++i) {
        const glm::vec4 clip = viewProj * glm::vec4(m_lights[i].position, 1.0f);
        if (clip.w <= 1.0e-5f) {
            continue; // behind the camera
        }
        const float px = (clip.x / clip.w * 0.5f + 0.5f) * width;
        const float py = (clip.y / clip.w * 0.5f + 0.5f) * height;
        const float dx = px - x;
        const float dy = py - y;
        if (dx * dx + dy * dy <= kPickRadiusPx * kPickRadiusPx && clip.w < bestDepth) {
            bestDepth = clip.w;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int Scene::pickModel(const Ray& ray) const {
    static const bool kTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
    static const bool kBoxOnly = std::getenv("POSESTUDIO_PICK_MODEL_BOX") != nullptr; // A/B: the box alone, as before 2026-11-12
    // What the cursor is ON: the nearest surface; a capture fainter than a half there only when
    // no surface is under the cursor.
    int   best = -1;
    int   faint = -1;
    float bestT = std::numeric_limits<float>::max();
    float faintT = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < m_models.size() && !kBoxOnly; ++i) {
        const auto      began = std::chrono::steady_clock::now();
        Model::ModelHit hit;
        const bool      on = m_models[i]->pickExact(ray, hit);
        if (kTrace) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            std::fprintf(stderr, "[pick-model] model %zu: %s (cast in %.2f ms)\n", i,
                         !on ? "not under the cursor" : hit.faint ? "faintly under it" : "its surface under it", ms);
        }
        if (!on) {
            continue;
        }
        if (!hit.faint && hit.t < bestT) {
            bestT = hit.t;
            best = static_cast<int>(i);
        } else if (hit.faint && hit.t < faintT) {
            faintT = hit.t;
            faint = static_cast<int>(i);
        }
    }
    if (best < 0) {
        best = faint;
    }
    if (best >= 0) {
        return best;
    }
    // On no surface: the nearest model whose box the ray enters.
    for (std::size_t i = 0; i < m_models.size(); ++i) {
        float t = 0.0f;
        if (m_models[i]->intersectRay(ray, t) && t < bestT) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    if (kTrace) {
        std::fprintf(stderr, "[pick-model] on no surface: by its box, model %d\n", best);
    }
    return best;
}

bool Scene::otherSurfaceAt(const Ray& ray, float& tOut) const {
    bool  found = false;
    float nearest = std::numeric_limits<float>::max();
    for (const std::unique_ptr<Model>& model : m_models) {
        Model::ModelHit hit;
        if (!model->hasSkeleton() && model->pickExact(ray, hit) && hit.tSolid < nearest) {
            nearest = hit.tSolid;
            found = true;
        }
    }
    tOut = nearest;
    return found;
}

namespace {

/// pickBoneAt's JOINT SNAP, after a hit on the skin of a body bone — @p pickedBone of @p fig,
/// at @p grab: where a joint of that bone's own body projects within kJointSnapPx of the click
/// (@p px, @p py), @p pickedBone and @p grab become that joint and its place; else both stay
/// as the hit has them.
void snapToSegmentJoint(Model& fig, const Ray& ray, float px, float py, float vpW, float vpH, const Camera& camera,
                        int& pickedBone, glm::vec3& grab) {
    const glm::mat4 viewProj = camera.viewProjection();
    const auto screenOf = [&](const glm::vec3& world, glm::vec2& out) {
        const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 1.0e-4f) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = glm::vec2((ndc.x * 0.5f + 0.5f) * vpW, (ndc.y * 0.5f + 0.5f) * vpH);
        return true;
    };
    // The joint snap: a joint of the hit bone's OWN body that projects within kJointSnapPx of
    // the click — the near and far joints of its rigid segment (or its own joint, for a leaf),
    // the joints its CHILDREN hang from (where its flesh ends) and its PARENT's joint. The
    // parent matters for the root: it has no skin of its own (the pelvic flesh is the pelvis
    // bone's), so a click right on the hip's pixel lands on the pelvis bone, and with the hit
    // bone's own joint the only candidate it took the pelvis joint 10px away over the hip at
    // 0px — the drag's first tick then asked the hips 2cm back, to put the pelvis joint where
    // the hip's pixel is, which on all fours popped a foot 165mm (2026-09-26). The CHILDREN's
    // joints must lie no deeper along the ray than the hit bone's own joint (plus
    // kSnapDepthSlack): a trunk bone's children come in lateral PAIRS — the two collars, the
    // two thigh sockets — and from the side the far twin projects onto the near one's pixel.
    // The parent's joint is not depth-checked: it is where the hit bone hangs from, inside the
    // body the cursor is on however deep — the chest joint sits on the spine, 10cm behind the
    // breast's skin, and a click at the chest joint's pixel that lands on a pectoral must
    // still take the chest (depth-checked, it kept the pectoral, and the Home view's chest
    // drag changed) — and the hit bone's own joints are trusted as they are, being the
    // segment the cursor is on. The nearest to the click wins; two at the same pixel go to
    // the nearer to the camera. POSESTUDIO_PICK_TRACE prints every candidate.
    static const bool kSnapTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
    constexpr float kSnapDepthSlack = 0.03f; // metres
    const Armature& arm = fig.armature();
    const int       hitBone = pickedBone;
    struct Candidate {
        int  joint;
        bool depthChecked;
    };
    std::vector<Candidate> candidates;
    const auto             offer = [&](int joint, bool depthChecked) { // (once each: a pectoral's segment near
        for (const Candidate& c : candidates) {                          // joint and its parent are both the chest)
            if (c.joint == joint) {
                return;
            }
        }
        candidates.push_back({joint, depthChecked});
    };
    int nearJoint = -1;
    int farJoint = -1;
    if (arm.segmentJoints(hitBone, nearJoint, farJoint)) {
        offer(nearJoint, false);
        offer(farJoint, false);
    } else {
        offer(hitBone, false);
    }
    if (const int parent = arm.boneParent(static_cast<std::size_t>(hitBone)); parent >= 0) {
        offer(parent, false);
    }
    for (std::size_t c = 0; c < arm.boneCount(); ++c) {
        if (arm.boneParent(c) == hitBone) {
            offer(static_cast<int>(c), true);
        }
    }
    const auto depthOf = [&](int j) { return glm::dot(fig.boneWorldPosition(static_cast<std::size_t>(j)) - ray.origin, ray.direction); };
    const float     ownDepth = depthOf(hitBone);
    const glm::vec2 click(px, py);
    float           bestPx = kJointSnapPx;
    float           bestDepth = std::numeric_limits<float>::max();
    bool            snapped = false;
    for (const Candidate& c : candidates) {
        glm::vec2 at(0.0f);
        if (c.joint < 0 || static_cast<std::size_t>(c.joint) >= fig.boneCount() ||
            fig.boneClass(c.joint) != BoneClass::Body || // (a face-rig bone's or a digit's joint: see pickBoneAt)
            !screenOf(fig.boneWorldPosition(static_cast<std::size_t>(c.joint)), at)) {
            continue;
        }
        const float dPx = glm::length(at - click);
        if (dPx > kJointSnapPx) {
            continue;
        }
        const float depth = depthOf(c.joint);
        const bool  tooDeep = c.depthChecked && depth > ownDepth + kSnapDepthSlack; // the far twin, behind the body the cursor is on
        const bool  nearer = !tooDeep && (!snapped || dPx < bestPx - 0.5f || (dPx <= bestPx + 0.5f && depth < bestDepth));
        if (kSnapTrace) {
            std::fprintf(stderr, "[pick] hit %s: snap candidate %s (%d): %.1f px, depth %+.3f m against the hit bone's joint%s%s\n",
                         arm.boneName(static_cast<std::size_t>(hitBone)).c_str(), arm.boneName(static_cast<std::size_t>(c.joint)).c_str(),
                         c.joint, dPx, depth - ownDepth,
                         tooDeep ? " (too deep)" : "", nearer ? " -> taken" : "");
        }
        if (nearer) {
            snapped = true;
            bestPx = dPx;
            bestDepth = depth;
            pickedBone = c.joint;
            grab = fig.boneWorldPosition(static_cast<std::size_t>(c.joint));
        }
    }
}

/// pickBoneAt's 2D FALLBACK, for a click that misses every figure's skin (just outside the
/// silhouette): the joint or bone body that projects nearest the click (pickBone) and the point
/// of it the cursor is at, in @p pickedModel / @p pickedBone / @p grab — unless something
/// stands between the camera and that point. False on a miss and for a hidden pick.
/// @p pick2DOnly (the A/B probe, the per-frame hover probe) leaves the occlusion cast out.
bool fallbackPick2D(const std::vector<std::unique_ptr<Model>>& models, const Ray& ray, float px, float py, float vpW,
                    float vpH, const Camera& camera, bool pick2DOnly, int& pickedModel, int& pickedBone,
                    glm::vec3& grab) {
    constexpr float kFallbackFleshDepth = 0.10f; // metres: a picked joint sits inside its own flesh
    const BonePick pick = pickBone(models, px, py, vpW, vpH, camera);
    if (pick.bone < 0) {
        return false;
    }
    const Model& fig = *models[static_cast<std::size_t>(pick.model)];
    // The picked point: the joint itself for a pick of the joint, the clicked point of the
    // body otherwise (the point of the picked segment nearest the cursor's ray; the pick's own
    // parameter is a screen-space one, good enough when the ray runs along the bone).
    grab = fig.boneWorldPosition(static_cast<std::size_t>(pick.bone));
    if (pick.child >= 0 && pick.child < static_cast<int>(fig.boneCount())) {
        const glm::vec3& childJoint = fig.boneWorldPosition(static_cast<std::size_t>(pick.child));
        float onRay = 0.0f;
        float onBone = pick.along;
        const glm::vec3 reach = ray.origin + ray.direction * (2.0f * glm::length(childJoint - ray.origin) + 1.0f);
        if (glm::length(childJoint - grab) > 1.0e-5f) {
            closestSegmentPoints(ray.origin, reach, grab, childJoint, onRay, onBone);
        }
        grab = glm::mix(grab, childJoint, glm::clamp(onBone, 0.0f, 1.0f));
    }
    // ... and nothing may stand between the camera and it: cast toward the point through every
    // figure, and a surface met more than the point's own flesh short of it hides it.
    const glm::vec3 toPoint = grab - ray.origin;
    const float     dist = glm::length(toPoint);
    if (dist > 1.0e-4f && !pick2DOnly) {
        const Ray toward{ray.origin, toPoint / dist};
        for (const std::unique_ptr<Model>& other : models) {
            float     t = 0.0f;
            glm::vec3 point(0.0f);
            int       bone = -1;
            if (other->hasSkeleton() && other->pickSurface(toward, t, point, bone) && t < dist - kFallbackFleshDepth) {
                return false; // hidden behind another part: not what the cursor is on
            }
        }
    }
    pickedModel = pick.model;
    pickedBone = pick.bone;
    return true;
}

} // namespace

bool Scene::pickBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, int& outModel, int& outBone,
                       glm::vec3& outGrab) {
    // A CLICK PICKS WHAT THE CURSOR IS ON (2026-09-26): the click is cast against every figure's
    // posed SKIN (Model::pickSurface) and the FIRST surface along the ray wins — its bone the joint
    // that weighs most on the skin at the hit, its point the grab point (Armature::setIkGrabPoint).
    // Until then the pick was a 2D one alone — every joint and bone body projected to the screen,
    // the nearest within 32px taken (pickBone) — and it saw THROUGH the body: a click on the chest
    // found the shoulder or the arm behind it as often as the chest (the user: "no body part
    // should ever get selected by clicking through another body part"). A joint of the hit bone's
    // OWN rigid segment (Armature::segmentJoints) that projects within kJointSnapPx of the click
    // is still snapped to — a click on the knee grabs the knee, not a point of the shin's skin
    // beside it — and cannot be hidden, being the segment the cursor is on. The 2D pick remains
    // for a click that MISSES the skin (just outside the silhouette), and only when nothing stands
    // between the camera and the point it picked (a second cast toward that point; a bone's own
    // flesh, kFallbackFleshDepth deep, is not "something").
    // Every figure competes: the nearest surface (or, in the fallback, the nearest joint) of ANY
    // figure wins, and its figure becomes the active one (the posing target). On a miss return -1
    // (so the caller orbits / click-selects) but keep the current selection — the figure can be
    // orbited while a joint is selected.
    const Ray ray = camera.screenPointToRay(px, py, vpW, vpH);
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    static const bool kPick2DEnv = std::getenv("POSESTUDIO_PICK_2D") != nullptr; // A/B probe: the 2D pick alone, as before 2026-09-26
    const bool kPick2D = kPick2DEnv || m_pick2D;
    float hitT = std::numeric_limits<float>::max();
    {
        m_pickedZone = -1;
        for (std::size_t m = 0; m < m_models.size() && !kPick2D; ++m) {
            const Model* fig = m_models[m].get();
            float     t = 0.0f;
            glm::vec3 point(0.0f);
            int       bone = -1;
            int       zone = -1;
            if (fig->hasSkeleton() && fig->pickSurface(ray, t, point, bone, &zone) && t < hitT) {
                hitT = t;
                pickedModel = static_cast<int>(m);
                pickedBone = bone;
                grab = point;
                m_pickedZone = zone; // (the material zone under the click, for the Materials tab)
            }
        }
    }
    // WHAT ELSE THE CURSOR IS ON (2026-11-12): a prop's opaque surface or a Gaussian splat scene
    // that covers the pixel. A click there is a click on THAT — it takes no joint: not the skin it
    // hides (a capture standing in front of the figure), and not, through the 2D fallback below,
    // a joint that merely projects within its radius of the click (a capture standing close
    // behind her: the user clicked the capture beside her arm and grabbed the arm, and the
    // capture could not be selected at all). The caller's click then selects the model the
    // cursor is on (pickModel). Not in the 2D-only mode (the per-frame hover probe's, and the
    // A/B's): this casts every prop and capture.
    {
        static const bool kNoGate = std::getenv("POSESTUDIO_PICK_THROUGH") != nullptr; // A/B: as before 2026-11-12
        float otherT = 0.0f;
        if (!kPick2D && !kNoGate && otherSurfaceAt(ray, otherT) && (pickedBone < 0 || otherT < hitT)) {
            m_pickedZone = -1;
            return false;
        }
    }
    // WHAT the hit bone is to the posing UI (Armature::boneClass, 2026-09-28). A bone of the FACE RIG
    // is for expressions and is not posed: the click is on the HEAD (the selection below maps it),
    // at the point it hit. A finger's or a toe's joint is dragged by the point the cursor is ON —
    // its drag moves the digit alone, and a point AT a joint has no lever on the bone it belongs
    // to. Neither snaps to a joint, and no joint of either is a snap candidate for a hit beside it
    // (a click on the back of the hand is the hand's, not the knuckle's 10px away). A CARPAL — the
    // hand's or the foot's own flesh below the wrist or the ankle (2026-10-28) — is the HAND's or
    // the FOOT's click at the point it hit, like the face's the head's: it never snaps and is never
    // a snap candidate; the selection below maps it.
    if (pickedBone >= 0 && m_models[static_cast<std::size_t>(pickedModel)]->boneClass(pickedBone) == BoneClass::Body) {
        snapToSegmentJoint(*m_models[static_cast<std::size_t>(pickedModel)], ray, px, py, vpW, vpH, camera, pickedBone, grab);
    } else if (pickedBone < 0) {
        if (!fallbackPick2D(m_models, ray, px, py, vpW, vpH, camera, kPick2D, pickedModel, pickedBone, grab)) {
            return false;
        }
    }
    outModel = pickedModel;
    outBone = pickedBone;
    outGrab = grab;
    return true;
}

int Scene::selectBoneAt(float px, float py, float vpW, float vpH, const Camera& camera) {
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    if (!pickBoneAt(px, py, vpW, vpH, camera, pickedModel, pickedBone, grab)) {
        return -1;
    }
    return selectPickedBone(pickedModel, pickedBone, grab, m_pickedZone);
}

int Scene::selectPickedBone(int model, int bone, const glm::vec3& grab, int zone) {
    if (model != activeFigureIndex()) {
        // Switching figures: the previous one's selection goes away.
        if (Model* previous = figureModel()) {
            previous->setSelectedBone(-1);
        }
    }
    setActiveFigure(model);
    Model& figure = *m_models[static_cast<std::size_t>(model)];
    figure.setSelectedBone(bone); // (a bone of the face rig selects the HEAD: Armature::posingBone)
    figure.setIkGrabPoint(bone, grab);
    if (zone >= 0) {
        m_selectedZone = zone; // the zone the click landed on: what the Materials tab shows
    }
    return figure.selectedBone();
}

int Scene::selectNearestBone(const glm::vec3& point, float radius) {
    int   pickedModel = -1;
    int   pickedBone = -1;
    float best = radius;
    for (std::size_t m = 0; m < m_models.size(); ++m) {
        const Model* fig = m_models[m].get();
        if (!fig->hasSkeleton()) {
            continue;
        }
        const Armature& arm = fig->armature();
        for (std::size_t i = 0; i < fig->boneCount(); ++i) {
            const float distance = glm::length(arm.boneWorldPosition(i) - point);
            if (distance < best) {
                best = distance;
                pickedModel = static_cast<int>(m);
                pickedBone = static_cast<int>(i);
            }
        }
    }
    if (pickedBone < 0) {
        return -1;
    }
    // (A face-rig bone selects the head, as for a click; the grab point is the joint itself.)
    const glm::vec3 at = m_models[static_cast<std::size_t>(pickedModel)]->armature().boneWorldPosition(static_cast<std::size_t>(pickedBone));
    return selectPickedBone(pickedModel, pickedBone, at);
}

int Scene::shiftPressBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, bool& wasMember) {
    wasMember = false;
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    if (!pickBoneAt(px, py, vpW, vpH, camera, pickedModel, pickedBone, grab)) {
        return -1;
    }
    Model& figure = *m_models[static_cast<std::size_t>(pickedModel)];
    // Another figure than the active one, a digit (posed alone), or nothing selected yet: a plain
    // selection, as without Shift — of the pick already made.
    if (pickedModel != activeFigureIndex() || figure.selectedBone() < 0 ||
        figure.boneClass(pickedBone) == BoneClass::Digit) {
        return selectPickedBone(pickedModel, pickedBone, grab, m_pickedZone);
    }
    const int primary = figure.armature().knownPosingBone(pickedBone); // (a face-rig bone: the head)
    wasMember = primary == figure.selectedBone() || figure.inGroup(primary);
    static const bool kShiftTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
    if (kShiftTrace) {
        std::fprintf(stderr, "[pick] shift press: picked %s (model %d), selected before %d, group %zu, member %d\n",
                     figure.armature().boneName(static_cast<std::size_t>(pickedBone)).c_str(), pickedModel, figure.selectedBone(),
                     figure.groupBones().size(), wasMember ? 1 : 0);
    }
    if (!figure.makeGroupPrimary(pickedBone)) {
        return selectPickedBone(pickedModel, pickedBone, grab, m_pickedZone);
    }
    figure.setIkGrabPoint(pickedBone, grab);
    return figure.selectedBone();
}

} // namespace pose
