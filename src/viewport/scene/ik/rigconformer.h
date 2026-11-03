/**
 * @file rigconformer.h
 * @brief THE RIG STANDARD'S CONFORMER: gives a skeleton that authors no frames and no limits — a
 *        character read from an FBX file — the frames, rotation orders and limits of the standard,
 *        by the ROLE of each bone (rigroles.h), so that the posing engine poses it as it poses the
 *        reference figures.
 *
 * The engine's model of a joint is the reference generation's: a bone's pose channels act in an
 * ORIENTATION FRAME whose first-applied axis lies along the bone (the twist), the other two across
 * it (the bends), and the authored LIMITS say what the joint does — a knee is a fold because its
 * range is 166 degrees on one side, a twist helper is one because two of its channels are locked.
 * A rig from another tool has none of that: its local frames are its authoring tool's (a bone's
 * length along X in one, Y in another), and it carries no limits at all. The conformer:
 *   1. builds a PROVISIONAL armature of the raw skeleton — every frame aligned to its bone, the
 *      twist helpers (named so) locked across — so that the rig standard's inference can read its
 *      structure, and reads the ROLE of every bone;
 *   2. gives each bone the standard's frame for its role (the reference generation's convention:
 *      the trunk and the legs along Y, the arms and the hands along X, the feet along Z — the
 *      least rotation that takes that world axis onto the bone, the right side mirrored), its
 *      rotation order, and its limits (the reference generation's anatomical ranges per role,
 *      the right side mirrored; a twist helper's two cross channels locked; a segment whose rig has
 *      no twist helper keeps the twist on the bone itself).
 * The mesh is untouched: the bind stays translation-only, so the figure renders exactly as read;
 * only what the pose channels MEAN changes. Qt-free (std + GLM).
 */

#ifndef RIGCONFORMER_H
#define RIGCONFORMER_H

#include "modeldata.h"

#include <string>
#include <vector>

namespace pose {

/// What the conformer found: the report of the rig standard's roles on this skeleton.
struct RigConformReport {
    bool                     conformed = false; ///< Frames and limits were written (false: no skeleton, or no rig could be built).
    bool                     complete = false;  ///< Every required role was found.
    int                      bones = 0;
    int                      bodyBones = 0;     ///< Bones under the hips.
    int                      twistHelpers = 0;
    int                      fingerJoints = 0;
    int                      toeJoints = 0;
    int                      unplaced = 0;      ///< Bones of the body the standard has no role for.
    int                      drivenHelpers = 0; ///< Helper bones given a driver (half a joint's bend, a share of a segment's twist).
    int                      renamed = 0;       ///< Bones given the standard's name (the face rig and the helpers keep their file's).
    int                      spineLinks = 0;
    int                      neckLinks = 0;
    std::vector<std::string> missing;           ///< The required roles not found.
    std::string              roles;             ///< The full conformance report (RigRoles::report).
};

/// Conforms @p data's skeleton to the rig standard (see the file comment), in place. A model with
/// no bones is left alone. The body mesh sample for the inference is taken from @p data's meshes.
RigConformReport conformRigToStandard(ModelData& data);

} // namespace pose

#endif // RIGCONFORMER_H
