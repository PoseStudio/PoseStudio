/**
 * @file namerules.h
 * @brief THE ONE READING OF A BONE NAME: the few things the posing engine still learns from a
 *        name — which SIDE it says (its left/right token), the DIGIT a finger or a toe is, whether
 *        it says TWIST, which limb SEGMENT word it carries — and the two lowercased forms the
 *        token readers compare. Every rule that reads a name reads it through here; until the IK
 *        refactoring round of 2026-11-03 four files each kept a copy (the armature's mirror
 *        pairing, the balance masses' side, the roles' digit tokens, the conformer's twist and
 *        segment words), and the two side rules had drifted apart in their boundaries.
 *
 * What a name is NOT read for: a bone's role (rigroles.h infers that from the structure), its
 * limits or its frame (rigconformer.h gives those by role). Qt-free.
 */

#ifndef NAMERULES_H
#define NAMERULES_H

#include <cstddef>
#include <string>

namespace pose {
namespace namerules {

/// The name's letters alone, lowercased (`lShldrBend` -> `lshldrbend`, `thigh_twist_01_l` ->
/// `thightwistl`): the form the token readers compare — a token is found whatever the separators.
std::string lettersLower(const std::string& name);
/// Every character of the name lowercased, separators kept (`Up_Leg` -> `up_leg`): the form the
/// segment words are read in, some of which carry a separator.
std::string lower(const std::string& name);
/// The token occurs in the (lowercased) name.
bool hasToken(const std::string& normalized, const char* token);

/// The digit a finger's (or, with @p toe, a toe's) name says it is, 1..5, or 0 where the name
/// says nothing: thumb/big = 1, index/second = 2, mid/third = 3, ring/fourth = 4,
/// pinky/little/fifth = 5. "small" is deliberately none: one generation numbers its four small
/// toes 1-4, which only their places across the foot can order.
int digitByToken(const std::string& normalized, bool toe);

/// The name says the bone is a twist helper ("twist" or "roll", any case).
bool saysTwist(const std::string& name);
/// The limb SEGMENT a bone's name says it belongs to: 0 none, 1 thigh, 2 shin, 3 upper arm, 4
/// forearm — the words every rigging convention uses for them. A bone that repeats its PARENT's
/// segment word is the second piece of one segment (a thigh cut in two, "thigh1" and "thigh2"),
/// which the posing engine knows as a twist helper: it turns about the bone and never bends.
int segmentWord(const std::string& name);

/// A bone name's SIDE TOKEN: where it stands, how long it is, and the side it says ('l', 'r', or
/// 0 for a name with none — a centre bone, or one whose side the name does not carry).
struct SideToken {
    std::size_t pos = 0;
    std::size_t len = 0;
    char        side = 0;
};
/// Finds the side token. The rules, in order (the first that fits wins): the PREFIX letter —
/// `l`/`r` before an uppercase letter or an underscore (`lShin`, `l_thigh`: the reference
/// generations' convention; a lowercase continuation, `lowerJaw`, is no side); the PREFIX word
/// `Left`/`Right`/`left`/`right` with anything after it; a LONE letter `L`/`R`/`l`/`r` anywhere,
/// bounded by `_ . - :`, a space or the name's ends (`CC_Base_L_Thigh`, `DEF-thigh.L`, `thigh_l`,
/// a biped's `Bip001 L Thigh`); a WORD `Left`/`Right` (any case) anywhere, bounded likewise, by a
/// change of case (`rig:LeftUpLeg`, `armLeft`), by a digit after it, or by an uppercase letter
/// after an all-caps word. (Until 2026-11-03 two rules read this: the mirror pairing's, which
/// knew the space, and the balance masses', which knew the digit; each is this one's now.)
SideToken sideToken(const std::string& name);
/// The side the name says, by its token: 'l', 'r' or 0.
inline char sideOf(const std::string& name) { return sideToken(name).side; }
/// The other side's name — the side token swapped, its case kept (`lShin` -> `rShin`, `thigh_l`
/// -> `thigh_r`, `LeftArm` -> `RightArm`) — or "" for a name with no side token.
std::string mirroredName(const std::string& name);

} // namespace namerules
} // namespace pose

#endif // NAMERULES_H
