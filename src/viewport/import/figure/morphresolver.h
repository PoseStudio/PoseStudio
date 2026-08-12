/**
 * @file morphresolver.h
 * @brief Resolves a preset's dialed morph channels — following channel-driver formulas — into concrete morphs.
 *
 * A character preset rarely dials the shape morphs directly. It dials a *control* channel (e.g. a
 * "CTRL<character>" = 1) whose channel-driver formulas drive the real head/body shape morphs ("FHM…"/"FBM…").
 * This walks that graph: starting from the scene's dialed channel values, it evaluates each channel's
 * formulas (a small push/mult/add stack machine) to propagate values onto the channels they drive,
 * and returns every reached channel that resolves to a file — so the importer can load each one's
 * deltas and apply them at the computed weight. Channels with no deltas (controls, scales) simply
 * contribute nothing when their deltas are looked up. Pure std + nlohmann (+ UriResolver) — no Qt.
 */

#ifndef MORPHRESOLVER_H
#define MORPHRESOLVER_H

#include <nlohmann/json_fwd.hpp>

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

class UriResolver;

/// A morph channel resolved to the file that holds its deltas, and the weight to apply it at.
struct DialedMorph {
    std::string url;    ///< "/path.dsf#channel" — resolvable to the file + fragment.
    float       weight; ///< The channel's value after driver-formula propagation.
};

/// Additive rest-position offset for each affected joint, keyed by bone id (node id). A full-body /
/// head morph doesn't only deform the mesh — it also drives each bone's `center_point` so the rig
/// follows the character's new proportions. These offsets (native units, same space as
/// FigureBone::origin) are the summed `center_point/{x,y,z}` driver-formula contributions.
using JointCenterOffsets = std::unordered_map<std::string, glm::vec3>;

/// Evaluates @p presetRoot's dialed channels through their channel-driver formulas and returns every reached
/// channel (control, driven, and directly-dialed) with a non-zero weight and a loadable file. The
/// caller loads each and applies its deltas (controls/scales yield no deltas, so they no-op).
/// @p outFigureScale receives the figure's overall uniform scale (1.0 = unchanged) — some controls
/// drive the figure node's scale/general to set character height.
/// @p outJointCenterOffsets receives the per-bone `center_point` adjustments the same formulas drive,
/// so the caller can shift each joint's rest origin to match the morphed shape (see JointCenterOffsets).
std::vector<DialedMorph> resolveDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                                             const std::string& presetDir, float& outFigureScale,
                                             JointCenterOffsets& outJointCenterOffsets);

} // namespace pose

#endif // MORPHRESOLVER_H
