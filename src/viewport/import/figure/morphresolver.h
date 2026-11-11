/**
 * @file morphresolver.h
 * @brief Resolves a preset's dialed morph channels — following channel-driver formulas — into concrete morphs.
 *
 * A character preset rarely dials the shape morphs directly. It dials a *control* channel (a
 * "<character>" dial = 1) whose channel-driver formulas drive the real shape morphs — commonly
 * several, one per part: a head, a body, the feet. This walks that graph: starting from the dials
 * the scene sets ON THE FIGURE BEING LOADED (a scene can hold several figures and lists every
 * one's dials side by side — see FigureScope), it evaluates each reached channel's formulas and
 * returns every reached channel that resolves to a file — so the importer can load each one's
 * deltas and apply them at the computed weight. Channels with no deltas (controls, scales) simply
 * contribute nothing when their deltas are looked up.
 *
 * THE RULES (each from real content; tools/figurecheck gates them on synthetic files):
 *  - AN ID IS COMPARED DECODED. A reference spells it percent-encoded ("#Some%20Character"), the
 *    file that defines it plain ("id": "Some Character"). Looked up by the encoded spelling, a
 *    control with a space in its id was never found and its character imported as the base figure.
 *  - A channel's value is (its dial + the sum of its "sum"-stage formulas) x the product of its
 *    "mult"-stage formulas, CLAMPED to the channel's limits where it is limited (the file's, or
 *    the scene's own override). A preset that dials a control at 1 AND the body it drives at 1
 *    gets the body at 1, not 2.
 *  - A formula driven by a JOINT (a rotation) is a pose corrective's and is not evaluated here:
 *    correctivescan finds those and the engine blends them live. What is baked is the shape at
 *    rest.
 *  - spline_tcb / spline_linear are evaluated (a control that shapes one morph below zero and
 *    another above it is written as two splines).
 *
 * NOT DONE: a formula that lives in the DRIVEN morph's own file and is driven by a channel this
 * walk reaches ("this detail follows that head") is only found when something else reaches that
 * file — the format's own loader knows every morph of the figure, this one only the ones named.
 *
 * Pure std + nlohmann (+ UriResolver) — no Qt.
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
struct FigureScope;

/// A morph channel resolved to the file that holds its deltas, and the weight to apply it at.
struct DialedMorph {
    std::string url;    ///< "/path.dsf#channel" — resolvable to the file + fragment.
    float       weight; ///< The channel's value after driver-formula propagation.
    std::string id;     ///< The channel's id, decoded ("Some Character Body").
};

/// Additive rest-position offset for each affected joint, keyed by bone id (node id). A full-body /
/// head morph doesn't only deform the mesh — it also drives each bone's `center_point` so the rig
/// follows the character's new proportions. These offsets (native units, same space as
/// FigureBone::origin) are the summed `center_point/{x,y,z}` driver-formula contributions.
using JointCenterOffsets = std::unordered_map<std::string, glm::vec3>;

/// What a preset's dials come to for one figure.
struct ResolvedShape {
    /// Every reached channel (control, driven, and directly dialed) with a non-zero value and a
    /// loadable file, in the order the walk reached them (the dialed ones first). The caller loads
    /// each and applies its deltas; a control yields none.
    std::vector<DialedMorph> morphs;
    /// The figure's overall uniform scale (1.0 = unchanged) — some controls drive the figure
    /// node's scale/general to set a character's height.
    float figureScale = 1.0f;
    /// The per-bone `center_point` adjustments the same formulas drive (see JointCenterOffsets).
    JointCenterOffsets jointCenterOffsets;
    /// Channels the preset dials or a control drives that could not be read — "<id>: <why>" (its
    /// file is not under any content root, or the file does not define that id). Reported, so a
    /// character that comes out as the base figure says which of its files is missing.
    std::vector<std::string> missing;
};

/// Evaluates the dials @p presetRoot's scene sets on the figure @p scope names through their
/// channel-driver formulas. Never throws: an unreadable file is a `missing` entry.
ResolvedShape resolveDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                                  const std::string& presetDir, const FigureScope& scope);

} // namespace pose

#endif // MORPHRESOLVER_H
