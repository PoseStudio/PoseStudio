/**
 * @file ray.h
 * @brief A world-space ray: the picking primitive shared by the camera and the models, and the
 *        ray / box test the model picks begin with.
 *
 * Split out of camera.h so that code which only intersects rays (Model::intersectRay, the
 * scene's model picking) doesn't pull in the whole orbit camera. Pure GLM data — no Vulkan,
 * no Qt.
 */

#ifndef RAY_H
#define RAY_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pose {

/// A world-space ray (origin + normalized direction), e.g. for mouse picking.
struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

/// The slab test: whether the ray @p origin + t · @p dir (t >= 0) meets the axis-aligned box
/// [@p lo, @p hi], and on a hit where it enters — @p tEnter, 0 for an origin inside the box, in
/// the units of @p dir (a distance for a unit direction; an affine map applied to both the ray
/// and the box keeps it). Left as it was on a miss.
inline bool rayHitsBox(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& lo, const glm::vec3& hi,
                       float& tEnter) {
    float tMin = 0.0f; // ignore hits behind the ray origin
    float tMax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1e-8f) {
            // Ray parallel to this slab: miss unless the origin is already within it.
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const float invD = 1.0f / dir[axis];
        float t1 = (lo[axis] - origin[axis]) * invD;
        float t2 = (hi[axis] - origin[axis]) * invD;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) {
            return false;
        }
    }
    tEnter = tMin;
    return true;
}

} // namespace pose

#endif // RAY_H
