/**
 * @file rigroles.h
 * @brief THE RIG STANDARD'S ROLES — what each bone of a humanoid skeleton IS to the posing
 *        engine, inferred from the bind skeleton's STRUCTURE (RigRoles::build), so that every
 *        rule of the engine that asks "the head", "the left thigh", "a twist helper of the
 *        forearm" asks one table instead of reading bone names.
 *
 * The vocabulary (RigRole) is the complete set of joints a human rig needs to pose every part of
 * the body: the hips and the pelvis bone, a spine chain of any length, a neck chain of any
 * length, the head and the face rig, the collars, the arms with their twist helpers, the hands
 * with their carpals and fingers, the legs with their twist helpers, the feet with their heels,
 * mid-foot bones, the ball and the toes, the chest riders. A rig conforms to the standard by
 * having each role on a bone of its own (the reference figure generations carry nearly all of
 * them); a rig that lacks a role — no twist helpers, no toes, no neck bone — still poses, with
 * that role empty: the roles are REQUIRED or optional (RigRoles::complete). A figure authored to
 * the standard names its bones by their roles; an imported one keeps its own names, which only
 * the files and the test tools ever see (bonealiases.h resolves the tests' canonical names
 * through the roles when a rig's own names match nothing).
 *
 * The inference reads what the rig already knows of the bind skeleton: the hierarchy, the bind
 * positions, the solve root (IkRig::rootNode — the first multi-child descendant of the figure
 * node), which joints are foot-class (low at bind), which bones are twist helpers
 * (Armature::isTwistBone: two locked channels, the free one along the bone), each subtree's
 * mass, and the body mesh sample (a leaf no skin follows is a helper, not a joint). It reads a
 * bone's NAME for two things only: the digit a finger or a toe is, where the hand's or the
 * foot's geometry is not asked, and the two face bones the tests name (the eyes, the jaw).
 * Qt-free (std + GLM).
 */

#ifndef RIGROLES_H
#define RIGROLES_H

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace pose {

struct BodyMeshPoint;

/// The roles of the standard. A bone has exactly one.
enum class RigRole : unsigned char {
    None = 0,      ///< Not a bone of the body: the figure node above the hips, a follower's scene node, a mesh node.
    Hips,          ///< The pelvis JOINT the whole body hangs from — the solve root (IkRig::rootNode).
    Pelvis,        ///< The pelvis BONE both legs hang from, on a rig that has one beside the hips (IkRig::isPelvisBone).
    Spine,         ///< A link of the spine chain: index 1 just above the hips .. N, the chest top the collars leave.
    Neck,          ///< A link of the neck chain: index 1 on the chest top .. N, under the head.
    Head,          ///< The head joint.
    Jaw,           ///< The lower jaw (of the face rig).
    Eye,           ///< An eye (of the face rig), by side.
    Face,          ///< Any other bone of the face rig: everything under the head.
    Pectoral,      ///< A chest rider by side (a breast bone): token mass, no limb hangs from it.
    Collar,        ///< The clavicle, by side: the bone the arm's socket hangs from.
    UpperArm,      ///< The shoulder joint's bone, by side.
    UpperArmTwist, ///< A twist helper of the upper arm, by side, index 1.. — on the chain or a leaf beside it.
    Forearm,       ///< The elbow's bone, by side.
    ForearmTwist,  ///< A twist helper of the forearm, by side, index 1..
    Hand,          ///< The wrist's bone, by side.
    Carpal,        ///< A bone of the hand that carries a finger or two — a carpal, a metacarpal — by side, index 1..
    Finger,        ///< A finger joint, by side: index = the digit (1 the thumb .. 5 the little finger; 0 unknown), joint 1.. from the hand.
    Thigh,         ///< The hip socket's bone, by side.
    ThighTwist,    ///< A twist helper of the thigh, by side, index 1..
    Shin,          ///< The knee's bone, by side.
    ShinTwist,     ///< A twist helper of the shin, by side, index 1..
    Foot,          ///< The ankle's bone, by side: the leg's most proximal foot-class joint.
    Heel,          ///< A heel bone under the ankle, by side.
    Metatarsal,    ///< A mid-foot bone between the ankle and the ball, or beside the ankle, by side.
    Ball,          ///< The ball of the foot the toes fan from, by side.
    Toe,           ///< A toe joint, by side: index = the digit (1 the big toe .. 5 the little toe; 0 unknown), joint 1.. from the ball.
    Helper,        ///< A rig helper with no joint of its own: an anchor, a control bone, a skinless leaf.
    Other,         ///< A bone of the body the standard has no role for (listed in the report).
    Count
};

/// Which side of the body a bone is on. The figure faces +z; her LEFT is +x at bind.
enum class RigSide : unsigned char { Centre = 0, Left = 1, Right = 2 };

/// One bone's role, with its side and, for the chains and the digits, its place in them.
struct RigRoleTag {
    RigRole       role = RigRole::None;
    RigSide       side = RigSide::Centre;
    unsigned char index = 0; ///< The chain link (Spine, Neck), the helper's number (the twists), the carpal's, the digit (Finger, Toe).
    unsigned char joint = 0; ///< A digit's joint from its base, 1..; 0 elsewhere.
};

/**
 * @class RigRoles
 * @brief The per-bone role table of one skeleton (see the file comment), built once with the rig.
 */
class RigRoles {
public:
    /// What the inference reads — constants of the bind skeleton the rig has when it builds.
    struct Input {
        const std::vector<int>*              parents = nullptr;     ///< Per bone: the anatomical parent (-1 = none).
        const std::vector<std::vector<int>>* children = nullptr;    ///< Per bone: its children, ascending.
        const std::vector<glm::vec3>*        bindPos = nullptr;     ///< Per bone: the bind joint position (model space).
        const std::vector<std::string>*      names = nullptr;       ///< Per bone: its name (the digits' and the face's tokens; the report).
        const std::vector<char>*             bodyNode = nullptr;    ///< Per bone: of the body (not the figure-node chain). Null = every bone.
        const std::vector<float>*            subtreeMass = nullptr; ///< Per bone: its own and its descendants' balance mass.
        const std::vector<char>*             helper = nullptr;      ///< Per bone: a twist helper (Armature::isTwistBone). Null = none.
        const std::vector<char>*             footClass = nullptr;   ///< Per bone: foot-class, low at bind (IkRig::isFootClass).
        const std::vector<BodyMeshPoint>*    mesh = nullptr;        ///< The body mesh sample, or null: a leaf no skin follows is a helper.
        int                                  root = -1;             ///< The solve root (IkRig::rootNode): the hips.
        float                                sizeScale = 1.0f;      ///< The figure's size against the reference (IkRig::sizeScale).
    };

    /// Infers every bone's role. Safe on an empty or degenerate skeleton: every bone is then
    /// None and complete() is false.
    void build(const Input& in);
    bool built() const { return !m_tags.empty(); }

    /// The bone's tag (a None tag outside the table).
    const RigRoleTag& tag(int bone) const;
    RigRole           role(int bone) const { return tag(bone).role; }
    RigSide           side(int bone) const { return tag(bone).side; }

    /// The bone with @p role on @p side, or -1. An @p index of 0 takes the first in bone order
    /// whatever its index; a @p joint of 0 likewise.
    int find(RigRole role, RigSide side = RigSide::Centre, int index = 0, int joint = 0) const;
    /// Every bone with @p role on any side, in bone order.
    std::vector<int> all(RigRole role) const;
    /// Every bone with @p role on @p side, in bone order.
    std::vector<int> all(RigRole role, RigSide side) const;

    /// The chains: the number of spine links (the chest top is link spineLinks()) and of neck links.
    int spineLinks() const { return m_spineLinks; }
    int neckLinks() const { return m_neckLinks; }
    /// The chest top: the last spine link, the one the collars and the neck leave (-1 if none).
    int spineTop() const { return m_spineLinks > 0 ? find(RigRole::Spine, RigSide::Centre, m_spineLinks) : -1; }

    /// Every REQUIRED role was found: the hips, a spine, the head, and on both sides the upper
    /// arm, the forearm, the hand, the thigh, the shin and the foot. missing() names the rest.
    bool                            complete() const { return built() && m_missing.empty(); }
    const std::vector<std::string>& missing() const { return m_missing; }

    /// The conformance report: the chain lengths, the missing roles, then one line per bone of the
    /// body (name, role, side, index, joint). What POSESTUDIO_RIG_ROLES_TRACE prints at build.
    std::string report() const;

    /// The role's name in the report ("thigh", "upper arm twist").
    static const char* roleName(RigRole role);
    /// "L" / "R" / "" for the report.
    static const char* sideName(RigSide side);
    /// The other side (Centre stays).
    static RigSide mirrored(RigSide side) {
        return side == RigSide::Left ? RigSide::Right : side == RigSide::Right ? RigSide::Left : RigSide::Centre;
    }

    /// THE STANDARD'S NAME of a tag — what a figure authored to the standard names the bone:
    /// lowercase words, the part first, its number next, the side last as `_l` / `_r`. `hips`,
    /// `pelvis`, `spine1`..`spineN`, `neck1`..`neckN`, `head`, `jaw`, `eye_l`, `pectoral_l`,
    /// `collar_l`, `upper_arm_l`, `upper_arm_twist1_l`, `forearm_l`, `forearm_twist1_l`, `hand_l`,
    /// `carpal1_l`, `thumb1_l`..`thumb3_l`, `index1_l`, `middle1_l`, `ring1_l`, `pinky1_l`,
    /// `thigh_l`, `thigh_twist1_l`, `shin_l`, `shin_twist1_l`, `foot_l`, `heel_l`, `metatarsal1_l`,
    /// `ball_l`, `bigtoe1_l`, `indextoe1_l`, `middletoe1_l`, `ringtoe1_l`, `pinkytoe1_l`. Empty for
    /// a face bone, a helper, Other and None: those keep the names their rig gave them.
    static std::string standardName(const RigRoleTag& tag);

    /// The tag a canonical name denotes, or a None tag for a name that is no role. Two grammars
    /// are read: THE STANDARD'S NAMES (standardName, above — a figure authored to the standard
    /// conforms by its names alone) and THE REFERENCE GENERATION'S NAMES, the first naming
    /// convention the standard knew and the one the test tools speak (`lThigh`, `chest_2`,
    /// `lIndex3`, `rSmallToe2_2`): there `chest` denotes the spine link below the top on a rig with
    /// four links or more and the top itself otherwise, `chest_2` the top, `neck` the first neck
    /// link. See bonealiases.cpp.
    RigRoleTag canonicalTag(const std::string& canonical) const;

private:
    std::vector<RigRoleTag>  m_tags;
    std::vector<std::string> m_names;
    std::vector<std::string> m_missing;
    int                      m_spineLinks = 0;
    int                      m_neckLinks = 0;
};

} // namespace pose

#endif // RIGROLES_H
