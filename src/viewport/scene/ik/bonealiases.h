/**
 * @file bonealiases.h
 * @brief The ONE bone-alias table the two posing test tools share: the reference figure
 *        generation's bone names mapped to the other generations' names for the same joints.
 *
 * The IK harness's phases (tools/ikharness) and the in-app scripted tests (tools/ikscripts, run
 * by vulkanwindow_script.cpp) name bones the way the reference generation does — `lHand`,
 * `chest_2`, `lThighTwist` — and are rig-agnostic gestures; every other generation calls the same
 * joints something else (`l_hand` / `spine4` / `l_thightwist1` on the newest, `chestUpper` on the
 * previous). Each tool used to carry its own copy of this table, and the copies drifted. Qt-free.
 */

#ifndef BONE_ALIASES_H
#define BONE_ALIASES_H

#include <string>

namespace pose {

class Armature;

/// The index on @p arm of the bone the tests call @p canonical: the name itself when the rig has
/// it, else the first alias it has, else the bone of the ROLE the name denotes in the rig standard
/// (rigroles.h), else -1. SIDE EFFECT: when neither the name nor an alias fits and @p arm has no
/// IK rig yet, the rig is BUILT to read its roles (Armature::prepareIkRig, through a const_cast:
/// the rig is a cache of the bind skeleton, as the first drag would have built it); a lookup the
/// names answer builds nothing. One collision of conventions is settled by role although the
/// name fits: "pelvis" on a rig that calls its HIPS so is the Pelvis-role bone (see the .cpp).
int aliasedBoneIndex(const Armature& arm, const std::string& canonical);

/// The rig's own name for @p canonical (see aliasedBoneIndex), or @p canonical when no bone fits
/// — a name a caller can pass on to a by-name API and let IT report the miss.
std::string aliasedBoneName(const Armature& arm, const std::string& canonical);

} // namespace pose

#endif // BONE_ALIASES_H
