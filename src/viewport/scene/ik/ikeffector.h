/**
 * @file ikeffector.h
 * @brief IkEffector: one pin of an IK drag, as the rig keeps it. Qt-free (GLM).
 */

#ifndef IKEFFECTOR_H
#define IKEFFECTOR_H

#include <glm/glm.hpp>

namespace pose {

/// One PIN of the current drag: @p node held at @p target — a planted contact, a user pin, a
/// live contact, the airborne root fallback. The rig owns the list (IkRig::pins) and its
/// policy (targets that heal, step, slide, lift); the Armature's solve turns each into rows.
struct IkEffector {
    int       node = -1;
    glm::vec3 target{0.0f};
    float     weight = 1.0f;       ///< Kept for the rig's bookkeeping and traces (1 = a full pin).
    /// The pin's REACH LEASH: how far the limb's socket may sit from this pin — the socket-to-pin
    /// distance the stance provably held, never more than the limb can span (IkRig::socketLeash;
    /// 1e9 = none: a live contact, a foot mid-step). The lift-off (suspension) test reads it: a
    /// root at the edge of a leash is a body stretched as far as its planted feet allow.
    float     leashRadius = -1.0f;
    /// The leash ball is centered at (target - leashOffset): a limb hangs from its SOCKET (the
    /// root's child on the pin's chain), which sits off the root by this offset.
    glm::vec3 leashOffset{0.0f};
    /// A HARD pin: a user-placed pin, or a knee planted over its still-pinned foot while the body
    /// goes down (IkRig::boundLivePins). It is held in full. A live contact that is NOT hard is
    /// unilateral — held in place on the floor, free to lift off it.
    bool hard = false;
};

} // namespace pose

#endif // IKEFFECTOR_H
