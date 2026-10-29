/**
 * @file ikmath.h
 * @brief Small shared rotation math for the IK module and the Armature's pose composition: the
 *        axis letters, rotation vectors, segment distances, and the Euler composition and
 *        decomposition for the figure format's arbitrary per-joint rotation orders.
 *
 * eulerMatrix composes the figure format's per-joint Euler pose (M = R(order[0]) · R(order[1]) ·
 * R(order[2]), each a right-handed rotation about a principal axis) and eulerFromMatrix is its
 * exact inverse — what turns a solved IK world rotation back into the per-channel Euler degrees
 * the engine's pose model (and its per-axis anatomical limits) are expressed in. The pair lives
 * together here so the composition the Armature poses with and its decomposition can never
 * drift apart. Qt-free (std + GLM).
 */

#ifndef IKMATH_H
#define IKMATH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>

namespace pose {

/// Pi and the degree/radian conversions the IK module computes in (double: the solver's
/// arithmetic; a float caller converts the result).
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

/// The channel index of an axis letter of a rotation order ('X'/'x' = 0, 'Y' = 1, 'Z' = 2), -1 otherwise.
inline int axisIndexOf(char c) {
    switch (c) {
    case 'X': case 'x': return 0;
    case 'Y': case 'y': return 1;
    case 'Z': case 'z': return 2;
    default: return -1;
    }
}

/// The rotation vector (axis x angle, radians) of rotation matrix @p m, in float (the Armature's
/// post-steps) or double (the solver): the small-angle branch below @p small is 2v.
template <typename T, glm::qualifier Q>
glm::vec<3, T, Q> rotationVectorOf(const glm::mat<3, 3, T, Q>& m, T small) {
    glm::qua<T, Q> q = glm::normalize(glm::quat_cast(m));
    if (q.w < T(0)) {
        q = -q;
    }
    const glm::vec<3, T, Q> v(q.x, q.y, q.z);
    const T                 s = glm::length(v);
    return s < small ? T(2) * v : v * (T(2) * std::atan2(s, q.w) / s);
}
inline glm::vec3 rotationVector(const glm::mat3& m) { return rotationVectorOf(m, 1.0e-6f); }
inline glm::dvec3 rotationVector(const glm::dmat3& m) { return rotationVectorOf(m, 1e-12); }

/// The closest points between the segments p1-q1 and p2-q2: returns their distance and writes
/// the parameters @p s (on p1-q1) and @p t (on p2-q2), both in [0, 1]. The body volumes test
/// a LIMB SEGMENT against a capsule with it (two limbs cross at their middles, where no joint
/// sits).
float closestSegmentPoints(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2,
                           const glm::vec3& q2, float& s, float& t);

/// Builds the rotation matrix of Euler angles @p degrees (per world channel: .x about X, .y about
/// Y, .z about Z) composed in @p order (e.g. "YZX", the figure format's per-joint rotation_order):
/// each listed axis rotation is applied in turn, M = R(order[0]) · R(order[1]) · R(order[2]).
/// Unknown characters in @p order are skipped.
glm::mat4 eulerMatrix(const glm::vec3& degrees, const std::string& order);

/// Decomposes rotation @p m into Euler angles (DEGREES, per world channel: .x about X, .y about Y,
/// .z about Z) such that composing them in @p order (e.g. "YZX", the figure format's per-joint
/// rotation_order) reproduces @p m — the exact inverse of the engine's eulerMatrix. Near gimbal
/// lock the third-applied angle is folded into the first (the standard degenerate-case choice).
glm::vec3 eulerFromMatrix(const glm::mat3& m, const std::string& order);

} // namespace pose

#endif // IKMATH_H
