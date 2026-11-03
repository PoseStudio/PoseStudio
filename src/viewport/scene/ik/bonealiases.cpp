/**
 * @file bonealiases.cpp
 * @brief The bone-alias table (see bonealiases.h): reference-generation names -> the other
 *        generations' names, tried in order after the name itself.
 */

#include "bonealiases.h"

#include "armature.h"
#include "ikrig.h"
#include "rigroles.h"

#include <cstddef>

namespace pose {

namespace {

struct BoneAlias {
    const char* canonical;
    const char* alternates[4];
};

const BoneAlias kBoneAliases[] = {
    {"lHand", {"l_hand"}},
    {"rHand", {"r_hand"}},
    {"lFoot", {"l_foot"}},
    {"rFoot", {"r_foot"}},
    {"lShin", {"l_shin"}},
    {"rShin", {"r_shin"}},
    {"lThigh", {"l_thigh"}},
    {"rThigh", {"r_thigh"}},
    // The newest generation's thigh twist bones hang OFF the chain (siblings of the shin); the
    // oldest generations have none — the harness's leg diagnostics then skip the twist metric.
    {"lThighTwist", {"l_thightwist1"}},
    {"rThighTwist", {"r_thightwist1"}},
    {"lForeArm", {"lForearmBend", "l_forearm"}},
    {"rForeArm", {"rForearmBend", "r_forearm"}},
    {"lShldr", {"lShldrBend", "l_upperarm"}},
    {"rShldr", {"rShldrBend", "r_upperarm"}},
    {"lCollar", {"l_shoulder"}},
    {"rCollar", {"r_shoulder"}},
    {"lIndex3", {"l_index3"}},
    {"lMid3", {"l_mid3"}},
    {"lThumb3", {"l_thumb3"}},
    {"lBigToe", {"l_bigtoe1"}},
    {"lEye", {"l_eye"}},
    {"lEar", {"l_ear"}},
    {"lowerJaw", {"lowerjaw"}},
    {"head", {"head"}},
    {"neck", {"neckLower", "neck1"}},
    {"hip", {"hip"}},
    {"abdomenLower", {"abdomen", "spine1"}},
    {"chest", {"chestLower", "spine3"}},
    {"chest_2", {"chestUpper", "spine4", "chest"}}, // the two-bone-chest generations: the top
};

} // namespace

int aliasedBoneIndex(const Armature& arm, const std::string& canonical) {
    int idx = arm.boneIndex(canonical);
    if (idx < 0) {
        for (const BoneAlias& alias : kBoneAliases) {
            if (canonical != alias.canonical) {
                continue;
            }
            for (const char* alt : alias.alternates) {
                if (alt != nullptr && (idx = arm.boneIndex(alt)) >= 0) {
                    break;
                }
            }
            if (idx >= 0) {
                break;
            }
        }
    }
    // THE ROLE the canonical name denotes (rigroles.h — the reference generation's names are the
    // standard's first convention), on a rig whose roles have been built: the answer where
    // neither the name nor an alias fits (a rig that names its bones in a convention this table
    // never saw resolves the same canonical names the same way) — and, since 2026-11-03, for ONE
    // collision of conventions where the name does fit: a game-engine export calls its HIPS
    // "pelvis" and keeps the pelvis bone as a leaf beside them, so "pelvis" read by name took the
    // hips there and every pelvis-bone gesture was a hips drag. Where the canonical denotes the
    // pelvis bone and the named bone is the rig's hips, the Pelvis-role bone answers. Only that
    // pair: letting the role win over ANY name whose bone carries another role reached the
    // reference rigs — the third generation's big toe is its Ball by role, and "lBigToe" then
    // grabbed another toe (r79 against r76: 36-94 rows moved on every rig). The names are the
    // standard's first convention; the roles are an inference, and where the two disagree on a
    // reference rig the NAME is right. (A conformed rig carries the standard's names since the
    // same day — its hips ARE `hips`, its pelvis leaf `pelvis` — so the swap serves the dumps of
    // un-renamed rigs alone; it stays for them.)
    const IkRig* rig = arm.ikRig();
    if (idx < 0 && rig == nullptr && arm.hasSkeleton()) {
        // The rig is a cache of the bind skeleton (built at the first drag, pick or import): a
        // name lookup that neither the names nor the aliases answer builds it, as reading any
        // cache does — and ONLY such a lookup (2026-11-03): built on every lookup for a day, the
        // harness's probes on the reference rigs, whose names all resolve, carried rigs they
        // never had, built before their body mesh (skeleton-sized floor clearances), and the
        // floor scenarios sized off those put the seat 25mm over the floor instead of 110 —
        // every prone and all-fours phase moved on the eight rigs (r79/r80 against r76). A
        // rig the lookup did not need is not built; one that exists is read.
        const_cast<Armature&>(arm).prepareIkRig();
        rig = arm.ikRig();
    }
    if (rig != nullptr && rig->roles().built()) {
        const RigRoleTag tag = rig->roles().canonicalTag(canonical);
        if (tag.role != RigRole::None) {
            const int byRole = rig->roles().find(tag.role, tag.side, tag.index, tag.joint);
            if (idx < 0) {
                return byRole;
            }
            if (byRole >= 0 && byRole != idx && tag.role == RigRole::Pelvis &&
                rig->roles().role(idx) == RigRole::Hips) {
                return byRole;
            }
        }
    }
    return idx;
}

std::string aliasedBoneName(const Armature& arm, const std::string& canonical) {
    const int idx = aliasedBoneIndex(arm, canonical);
    return idx >= 0 ? arm.boneName(static_cast<std::size_t>(idx)) : canonical;
}

} // namespace pose
