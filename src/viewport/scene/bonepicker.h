/**
 * @file bonepicker.h
 * @brief Screen-space joint picking across every figure in the scene — THE FALLBACK since
 *        2026-09-26: Scene::selectBoneAt casts the click against the posed skin first
 *        (Model::pickSurface) and comes here only when the ray misses every figure's surface, a
 *        click just outside the silhouette, and then checks that nothing hides what this found.
 *
 * A click on the viewport is resolved to (figure, bone) here, by projecting each figure's joints
 * with the camera and hit-testing the click against joint ORIGINS and bone BODIES (the segment
 * from a joint to each child joint). Picking is deliberately independent of what the overlay
 * draws: the skeleton is hidden by default and joints are grabbed directly on the figure, so the
 * picker never consults the overlay's visibility. Pure GLM + the models' world joint positions —
 * no Vulkan, no Qt — and free of Scene state, so the selection/active-figure bookkeeping stays in
 * Scene::selectBoneAt while the geometry lives here.
 */

#ifndef BONEPICKER_H
#define BONEPICKER_H

#include <memory>
#include <vector>

namespace pose {

class Camera;
class Model;

/// A pick result: the model (figure) index and the bone index within it; -1/-1 for a miss.
/// A pick of a bone's BODY also says where along it: @p child is the joint the picked segment
/// runs to and @p along the click's place on it (0 at the bone's own joint, 1 at the child's) —
/// the point of the body the user took hold of (Scene::selectBoneAt makes it the IK grab
/// point). A pick of the joint itself leaves child -1.
struct BonePick {
    int   model = -1;
    int   bone = -1;
    int   child = -1;
    float along = 0.0f;
};

/// How near (pixels) a click must be to a joint's projected origin to be right ON the joint:
/// pickBone's first priority, and the radius of the joint snap Scene::pickBoneAt makes after a
/// hit on the skin — one number, so a click that takes the joint in one path takes it in the
/// other.
inline constexpr float kJointSnapPx = 14.0f;

/// Finds the figure joint under the pixel (@p px, @p py) of a @p vpW × @p vpH viewport. Every
/// figure's joints compete and the nearest of ANY figure wins.
///
/// Picking is by BONE, not just by joint origin: a bone's body is the segment from its joint to
/// each child joint, and a click anywhere along it selects that bone — so clicking the middle of
/// a thigh grabs the thigh, where joint-origin picking alone (the thigh's joints sit at the hip
/// and the knee, farther than any sane radius from a mid-thigh click) fell through to a plain
/// model click. A click right ON a joint (kJointSnapPx) still snaps to that joint, so the knee
/// picks the shin bone whose origin it is rather than the thigh segment ending there. A root's
/// own segment is skipped: a figure node sits at the origin with the hip a metre above, and that
/// virtual bone runs straight between the legs. Priority: a joint under the cursor, then the
/// nearest bone body within the click tolerance, then a joint within it (leaf bones — toes,
/// fingertips — have no segment). A miss returns {-1, -1}.
BonePick pickBone(const std::vector<std::unique_ptr<Model>>& models, float px, float py, float vpW,
                  float vpH, const Camera& camera);

} // namespace pose

#endif // BONEPICKER_H
