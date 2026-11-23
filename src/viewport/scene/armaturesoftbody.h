// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file armaturesoftbody.h
 * @brief The Armature's SOFT-BODY state types (namespace pose::armaturestate): a breast's mass (on
 *        a pectoral bone, or boneless), a boneless breast's delta field, a cheek's mass, and the
 *        acceleration reader the masses share. Plain data; the class names them as its own
 *        (`using`, in armature.h). The model is armaturebreasts.cpp's, armaturebreastfield.cpp's
 *        and armaturebuttocks.cpp's; the common entry points are armaturesoftbody.cpp's.
 *        Qt-free (std + GLM).
 */

#ifndef ARMATURE_SOFT_BODY_H
#define ARMATURE_SOFT_BODY_H

#include <glm/glm.hpp>

namespace pose::armaturestate {

/// A BREAST's mass (armaturebreasts.cpp): what carries it, how full it is, where it hangs from,
/// and the spring's state.
struct Breast {
    int       bone = -1;         ///< The pectoral that carries the breast; -1 for a BONELESS breast (`field`).
    int       field = -1;        ///< A boneless breast's delta field (0 left, 1 right: m_breastFields); -1 where a bone carries it.
    int       parent = -1;       ///< The chest: the bone whose frame the mass moves in (the pectoral's parent, or the field's spine link).
    float     size = 0.0f;       ///< Its fullness: 0 (a flat chest: no motion) .. 1 (the base female) .. 3, off the skin's profile (setupBreasts) or, boneless, the chest's silhouette (BreastField::size).
    glm::vec3 pivotBind{0.0f};   ///< The breast's base on the chest wall behind the nipple (model space at bind).
    glm::vec3 pivotTopBind{0.0f}; ///< Its upper attachment on the chest wall, kBreastLever over the nipple: the swing's pivot.
    float     protrusion = 0.0f; ///< The nipple's distance forward of the base (m).
    float     lever = 0.05f;     ///< The nipple's distance from the swing's pivot (m).
    float     side = 1.0f;       ///< +1 the left breast (outward is +x), -1 the right.
    glm::vec3 disp{0.0f};        ///< The mass's displacement from the sculpt (m, chest frame: sideways, along the chest, out along the tether).
    glm::vec3 vel{0.0f};
};

/// THE ACCELERATION READER the soft body's masses share (armaturebreasts.cpp, armaturebuttocks.cpp):
/// a joint's velocity SMOOTHED before it is differenced and the acceleration smoothed after, so
/// a millimetre of tick-to-tick tremble (a g and a half raw) reads as nothing while a jolt keeps
/// its impulse; then the dead band and the cap.
struct SoftBodyAccelReader {
    glm::vec3 pos{0.0f}, vel{0.0f}, accel{0.0f}; ///< The last position and smoothed velocity (world), the smoothed acceleration (g).
    int       samples = 0;
    void      reset() { pos = vel = accel = glm::vec3(0.0f); samples = 0; }
    /// One tick: the acceleration the mass feels (g, world) after the band and the cap, @p magOut its magnitude.
    glm::vec3 sample(const glm::vec3& p, float dt, float velSmooth, float accelSmooth, float deadG, float maxG, float& magOut);
};

/// A BONELESS breast's delta field (armaturebreastfield.cpp): where the breast stands on the
/// chest's own skin, how full it is, and how far the field reaches.
struct BreastField {
    bool      valid = false;       ///< This side's breast is a field: its apex is carried by no pectoral bone, and it is full enough to move.
    int       chest = -1;          ///< The spine link that owns the skin at the apex: the frame the mass moves in.
    float     size = 0.0f;         ///< Its fullness, on Breast::size's scale (0 flat .. 1 the base female .. 3).
    glm::vec3 apexBind{0.0f};      ///< The breast's furthest point forward at bind (model space): the field's centre.
    float     wallZ = 0.0f;        ///< The ribs under the fold (bind z): the chest wall behind the breast.
    float     radiusIn = 0.07f;    ///< The field's reach from the apex toward the sternum (m) ...
    float     radiusOut = 0.08f;   ///< ... toward the armpit ...
    float     radiusUp = 0.09f;    ///< ... up the chest ...
    float     radiusDown = 0.05f;  ///< ... and down to the fold.
};

/// A CHEEK's mass (armaturebuttocks.cpp).
struct Buttock {
    float     side = 1.0f;     ///< +1 the left cheek (outward is +x), -1 the right.
    glm::vec3 apexBind{0.0f};  ///< The cheek's fullest point at bind (model space): the field's centre.
    glm::vec3 disp{0.0f};      ///< The mass's displacement (m, hips frame: sideways, along the trunk; z unused).
    glm::vec3 vel{0.0f};
};

} // namespace pose::armaturestate

#endif // ARMATURE_SOFT_BODY_H
