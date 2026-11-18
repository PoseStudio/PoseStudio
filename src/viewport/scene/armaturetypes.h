/**
 * @file armaturetypes.h
 * @brief The Armature's nested STATE TYPES (namespace pose::armaturestate): a runtime bone, an arm
 *        that hangs and its turn of one tick, the landing bounce in flight, a hanging foot being
 *        landed, a pin the figure move holds. Plain data; the class names them as its own (`using`,
 *        in armature.h), so the Armature's code reads Bone, HangArm ... as it always did. The soft
 *        body's state types are in armaturesoftbody.h. Qt-free (std + GLM).
 */

#ifndef ARMATURE_TYPES_H
#define ARMATURE_TYPES_H

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace pose::armaturestate {
// One runtime skeleton joint. localBind is its rest transform relative to its parent;
// poseLocal folds in the current animated rotation (== localBind at bind pose).
// `orient`/`invOrient` frame that rotation in the joint's true orientation so bends are
// anatomically correct.
struct Bone {
    int         parent = -1;
    glm::mat4   inverseBind{1.0f};
    glm::mat4   localBind{1.0f};
    glm::mat4   poseLocal{1.0f};
    glm::mat4   orient{1.0f};
    glm::mat4   invOrient{1.0f};
    glm::vec3   orientationDeg{0.0f}; ///< The authored orientation (dump round-trip).
    std::string rotationOrder = "XYZ"; ///< REVERSED from the format's (see build): eulerMatrix composes leftmost outermost.
    // Per-axis pose-rotation limits (degrees) enforced by clampBoneEuler(); only axes flagged
    // in rotLimited are constrained (the figure's anatomical range of motion).
    glm::vec3   rotMin{0.0f};
    glm::vec3   rotMax{0.0f};
    glm::bvec3  rotLimited{false, false, false};
    int         drivenBy = -1;        ///< See ArmatureBone::drivenBy.
    glm::vec3   driveFactor{0.0f};
    std::string roleHint;             ///< See ArmatureBone::roleHint (carried to the rig, written to the dump).
};

/// An arm that hung at the press (hangIdleArms): its shoulder socket, its elbow, and the upper
/// arm's direction then (socket to elbow, model space, unit).
struct HangArm {
    int       socket = -1;
    int       elbow = -1;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    /// Where it WANTS to point (world, at the heading the drag found): `direction` for an arm
    /// that hung at the press; straight down by her side for one that did not and is the
    /// hang's all the same (a seated figure's trunk drag: hangIdleArms).
    glm::vec3 wants{0.0f, -1.0f, 0.0f};
    /// How far inside a body volume the arm stood when the drag began (the hang's stop lets
    /// it settle a little deeper than that, never more: hangIdleArms).
    float depthAtPress = 0.0f;
    /// Its hand came down on the floor DURING this drag: the solve has the arm from then on,
    /// and balance stays blind to it (balancePositions).
    bool landed = false;
    /// Its hand's contact LIFTED during this drag (held, then not): the arm is the hang's, held off
    /// the floor by it. It keeps its floor rows in the solve; under THE PRONE its bones join the
    /// unknowns so those rows bend the ARM and not the body (ikTrunkPolicy's hangArmDof) — the
    /// heavy male's released arms rested along his body with the thumbs on the floor, and the
    /// thumbs' rows held his chest up (2026-10-29).
    bool released = false;
    /// Its hand was ON THE FLOOR when the drag began (a contact): once that contact is unloaded
    /// or lifted the wrist RELAXES toward neutral (hangIdleArms) — riding its posture reference,
    /// a hand lifted off all fours stayed bent back 80 degrees, flat, held out like a paw.
    bool handLandedAtPress = false;
    /// The elbow's fold channel and the way it flexes (+1 / -1), and the arm's hand (-1: none found).
    int   foldAxis = 0;
    float foldSign = 1.0f;
    int   hand = -1;
    /// THE STRIDE SWING (hangIdleArms): whether a two-footed stance could be read at the press.
    /// The swing itself is ABSOLUTE — the stance's split now, faded by the hips' drop — applied to
    /// the arm's BASE hang (`wants`), found at each press by undoing the swing the arm CARRIES
    /// (m_hangSwingDeg, remembered across drags): a turn the stop or a limit cut short does not
    /// corrupt the next drag's home.
    bool hasSwing = false;
    /// IDLE ELBOWS SOFTEN (hangIdleArms): the elbow is the hang's to bend -- at the bind at the
    /// press, or where the hang itself left it (m_hangElbowDeg). A posed elbow keeps its bend.
    bool elbowOurs = false;
};

/// One hanging arm's turn this tick (hangIdleArms' parts: found, stopped, applied in that order).
struct HangTurn {
    const HangArm* arm = nullptr;
    std::size_t    s;
    glm::vec3      before;
    glm::vec3      delta;
    glm::vec3      wanted;
    glm::vec3      out{0.0f}; // the arm's outward axis (levelled lateral, toward its side), or zero
    glm::vec3      unswung{0.0f}; // the wanted direction before THE STRIDE SWING's tilt (see m_hangSwingDeg)
    glm::vec3      fore{0.0f};    // the trunk's fore axis now, or zero
    float          swingAsked = 0.0f; // the swing the tilt asked (degrees, + forward)
    float          depthBefore = 0.0f;
    float          floorDepthBefore = 0.0f; // the arm's depth under the floor alone (negative = clear)
    float          fraction = 1.0f;
};

/// The bounce in flight: its depth and how long it lasts, where the hips stood when she landed
/// (world), the pose she landed in (what the bounce blends back to and ends in), and the
/// user's selection and grab point, which the bounce's pelvis drag borrows the selection from.
struct LandingBounce {
    bool                   active = false;
    float                  dip = 0.0f;
    float                  seconds = 0.0f;
    glm::vec3              hipStart{0.0f};
    std::vector<glm::vec3> euler;
    std::vector<glm::vec3> translation;
    int                    selected = -1;
    int                    grabBone = -1;
    glm::vec3              grabLocal{0.0f};
    /// THE ARMS' BOUNCE (2026-11-04): each hanging arm swings forward over the dip and back —
    /// its shoulder's forward channel (the Bend dial's) from the landing pose's value.
    struct ArmSwing {
        int   bone = -1;
        int   axis = 0;
        float sign = 1.0f;
        float base = 0.0f;
        int   elbowBone = -1; ///< The arm's elbow, bending with the forward swing (none where the arm has no fold dial).
        int   elbowAxis = 0;
        float elbowSign = 1.0f;
        float elbowBase = 0.0f;
    };
    std::vector<ArmSwing>  arms;
    float                  armSwingDeg = 0.0f; ///< The swing's amplitude, sized by the impact like the dip.
    float                  lastSeconds = 0.0f; ///< The previous tick's time (the soft body's dt).
    bool                   tail = false;       ///< The dip is over; the breasts still settle (the pose is home).
};

/// One foot of a landing in flight: its ankle; the bones the landing turns — the ankle first,
/// then the bones below it that the lift had turned, parents first — each one's rotation as
/// the landing found it and flat (the ankle's flat value is re-read under its shin whenever it
/// is posed); and every bone of the foot with its parent's slot (-1: the shin), for the floor.
struct HangFoot {
    int                    ankle = -1;
    std::vector<int>       bones;
    std::vector<glm::vec3> found;
    std::vector<glm::vec3> flat;
    std::vector<int>       subtree;
    std::vector<int>       parentSlot;
    std::vector<int>       boneSlot; ///< per subtree entry: its index in `bones`, or -1 (it keeps its pose)
};

/// A user pin the FIGURE MOVE holds (armatureikfigure.cpp): the joint, and its place and rotation
/// at the press (model space).
struct HeldPin {
    int       bone = -1;
    glm::vec3 place{0.0f};
    glm::mat3 rotation{1.0f};
};

} // namespace pose::armaturestate

#endif // ARMATURE_TYPES_H
