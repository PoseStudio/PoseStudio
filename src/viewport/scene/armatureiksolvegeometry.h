/**
 * @file armatureiksolvegeometry.h
 * @brief Small pieces of rotation geometry the solve's ROW stages share (the pin rows, the dragged
 *        joint's rows): the frame a per-axis orientation row is measured in, a target turned flat
 *        with its heading kept, an ease that only leads, the hand's bind frame, the palm's lead cap.
 *
 * Each was written out at three or four sites of the rows; here once, with the operations in the
 * order every copy had them (the harness compares the solve's output byte for byte). Private to
 * the solve's row files (armatureiksolvepins.cpp, armatureiksolvecursor.cpp). Qt-free (std + GLM).
 */

#ifndef ARMATURE_IK_SOLVE_GEOMETRY_H
#define ARMATURE_IK_SOLVE_GEOMETRY_H

#include "armatureiksolvetuning.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

/// The frame a per-axis orientation row about a bone's LATERAL axis is measured in: (lateral, the
/// vertical made square to it, their cross) — right-handed like (x, y, z), so the row's weights
/// read (pitch, heading, level).
inline glm::dmat3 frameSquareTo(const glm::dvec3& lateral) {
    const glm::dvec3 up(0.0, 1.0, 0.0);
    glm::dvec3       upSquare = up - lateral * glm::dot(lateral, up);
    upSquare = glm::length(upSquare) > 1.0e-6 ? glm::normalize(upSquare) : glm::dvec3(0.0, 0.0, 1.0);
    return glm::dmat3(lateral, upSquare, glm::cross(lateral, upSquare));
}

/// The FLAT rotation that keeps @p target's heading: how far the bone's lateral axis (@p lateralLocal,
/// in its own frame) has turned about the vertical, as a rotation about the vertical, into @p flat.
/// False where that axis stands too near the vertical to read a heading off.
inline bool flatKeepingHeading(const glm::mat3& target, const glm::vec3& lateralLocal, glm::quat& flat) {
    glm::vec3 lateralNow = target * lateralLocal;
    lateralNow.y = 0.0f;
    if (!(glm::length(lateralNow) > 0.2f)) {
        return false;
    }
    lateralNow = glm::normalize(lateralNow);
    const float yaw = std::atan2(glm::cross(lateralLocal, lateralNow).y, glm::dot(lateralLocal, lateralNow));
    flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return true;
}

/// AN EASE THAT ONLY LEADS, advanced one tick: a target rotation eased from @p fromRot (where the
/// bone was found) toward @p toRot at @p stepDeg a tick, @p ease its share so far (0..1) — and a
/// bone already further along than the ease (read off @p nowRot) is met there, never pulled back.
/// Returns the two ends as quaternions on one hemisphere: the eased target is their slerp at @p ease.
struct LeadingEase {
    glm::quat from;
    glm::quat to;
};
inline LeadingEase advanceLeadingEase(const glm::mat3& fromRot, const glm::mat3& toRot, const glm::mat3& nowRot, float stepDeg,
                                      float& ease) {
    const glm::quat from = glm::normalize(glm::quat_cast(fromRot));
    glm::quat       to = glm::normalize(glm::quat_cast(toRot));
    if (glm::dot(from, to) < 0.0f) {
        to = -to;
    }
    glm::quat now = glm::normalize(glm::quat_cast(nowRot));
    if (glm::dot(now, to) < 0.0f) {
        now = -now;
    }
    const float whole = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f)));
    const float left = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(now, to), -1.0f, 1.0f)));
    const float reached = whole > 1.0e-3f ? glm::clamp(1.0f - left / whole, 0.0f, 1.0f) : 1.0f;
    ease = whole > stepDeg ? std::min(1.0f, std::max(ease, reached) + stepDeg / whole) : 1.0f;
    return {from, to};
}

/// The hand's BIND frame off its fingers' line (wrist to longest fingertip at bind; every bind is
/// translation-only, so the bone's local axes are the world's there): the unit line into @p f and
/// the palm's normal into @p palm — the part of DOWN across the line (palms face down in a T-pose,
/// down and in in an A-pose). False where the fingers point too nearly straight down to say; @p f
/// is set either way. @p fingers must not be zero.
inline bool handBindFrame(const glm::vec3& fingers, glm::vec3& f, glm::vec3& palm) {
    f = fingers / glm::length(fingers);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    palm = down - f * glm::dot(down, f);
    if (!(glm::length(palm) > 0.2f)) {
        return false;
    }
    palm = glm::normalize(palm);
    return true;
}

/// @p target pulled back along the geodesic toward @p now so that it leads the hand's rotation now by at
/// most kPalmLeadDeg (IK_JS_PALMLEAD; 0 = uncapped): the palm eases' lead cap, see kPalmLeadDeg.
inline glm::mat3 capPalmLead(const glm::mat3& now, const glm::mat3& target) {
    static const float kLead = envOr("IK_JS_PALMLEAD", kPalmLeadDeg);
    if (kLead <= 0.0f) {
        return target;
    }
    const glm::quat nowQ = glm::normalize(glm::quat_cast(now));
    glm::quat       to = glm::normalize(glm::quat_cast(target));
    if (glm::dot(nowQ, to) < 0.0f) {
        to = -to;
    }
    const float lead = glm::degrees(2.0f * std::acos(glm::clamp(glm::dot(nowQ, to), -1.0f, 1.0f)));
    if (lead <= kLead) {
        return target;
    }
    return glm::mat3_cast(glm::slerp(nowQ, to, kLead / lead));
}

} // namespace pose

#endif // ARMATURE_IK_SOLVE_GEOMETRY_H
