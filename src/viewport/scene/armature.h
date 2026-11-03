/**
 * @file armature.h
 * @brief The runtime skeleton of a posable figure: its bones, the per-joint pose (Euler rotation
 *        + translation), forward kinematics into skinning dual quaternions, the user's joint
 *        pins, and the full-body-IK integration — ONE class over several translation units:
 *        armature.cpp (construction, FK, snapshots, the dump), armaturedials.cpp (the Transform
 *        dials), armatureik.cpp (the drag lifecycle), armatureiksolve.cpp (the per-tick solve
 *        on scene/ik/jointsolver.h, whose stages are armatureiksolvedrag / -posture / -rows.cpp
 *        over armatureiksolvestate.h, tuned by armatureiksolvetuning.h), armatureikpost.cpp
 *        (the post-steps), armatureikdigit.cpp (the bone classes, the digit drag),
 *        armatureikfigure.cpp (the figure move) and armaturelanding.cpp (the landing bounce).
 *
 * An Armature is everything about a figure's pose that is NOT geometry: Model owns one next to
 * its meshes and GPU buffers, uploads the dual quaternions it computes, and forwards every posing
 * call to it. The split is load-bearing: the armature is pure std + GLM — no Vulkan, no Qt — so
 * the IK harness (tools/ikharness/) drives the REAL drag loop on a real figure's dumped
 * skeleton, tick for tick as the viewport does, instead of a mirror copy that drifts. Anything
 * that changes how a pose is composed, clamped, or solved
 * belongs here, never in Model. A static model (an OBJ) still carries an Armature: empty of
 * bones, it holds the model transform and one identity joint for the shared skinned pipeline.
 */

#ifndef ARMATURE_H
#define ARMATURE_H

#include "bodymesh.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pose {

// Pose-snapshot row prefixes (Armature::capturePose/applyPose): a bone's pose TRANSLATION is a
// "@trans:<boneName>" row and the user's joint PINS are "@pin:<boneName>" rows (value unused,
// written 1 0 0). Shared with the .pss project codec, which splits a snapshot into pose / pins
// + root translation — and with the IK harness, which reads the same rows.
inline constexpr char kPoseTranslationPrefix[] = "@trans:";
inline constexpr char kPosePinPrefix[]         = "@pin:";

class IkRig;
class JointSolver;
struct JointSolverDof;

/// One bone of an armature, as the importer (or a skeleton dump) describes it. Rest transforms
/// are TRANSLATION-ONLY (the figure format's convention: a bone's rest frame is axis-aligned with
/// model space; its orientation lives separately in `orientation` and cancels at rest).
struct ArmatureBone {
    std::string name;
    int         parent = -1;                 ///< Index into the bone list; -1 = the skeleton root.
    glm::vec3   localBindTranslation{0.0f};  ///< Rest position relative to the parent joint (model units).
    glm::vec3   orientation{0.0f};           ///< Rest orientation, Euler degrees (XYZ) — the frame pose rotations act in.
    std::string rotationOrder = "XYZ";       ///< The format's rotation_order: the order the channels are APPLIED in (first = innermost, the twist).
    // Per-axis pose-rotation limits (degrees): the joint's anatomical range of motion. Only axes
    // flagged in `rotationLimited` are enforced; the others rotate freely.
    glm::vec3   rotationMin{0.0f};
    glm::vec3   rotationMax{0.0f};
    glm::bvec3  rotationLimited{false, false, false};
    /// A DRIVEN bone (ModelBone::drivenBy): its Euler = driveFactor * the driver's, re-derived at
    /// every pose change (computeSkinMatrices). -1: not driven.
    int         drivenBy = -1;
    glm::vec3   driveFactor{0.0f};
};

/**
 * @class Armature
 * @brief Bones + pose + FK + pins + full-body IK for one figure. Vulkan-free, Qt-free.
 *
 * Pose model: each bone's `poseLocal` = localBind · orient · R(euler, rotationOrder) · orient⁻¹,
 * plus a pose translation in the parent frame (only ever non-zero where FBIK moved the root).
 * poseGlobal chains parent-first; the skin transform is poseGlobal · inverseBind, converted to a
 * unit dual quaternion for the shaders. The world transform of the whole figure (the model
 * matrix) also lives here — IK, picking, and grounding all need it alongside the pose.
 */
struct IkSolveScratch;
struct IkPinRowState; // the per-tick solve's shared stage state (armatureiksolvestate.h)

/// WHAT an IK drag may move (Armature::beginIkDrag). Body: the whole figure — the plain drag, with
/// the feet planted, balance, steps and every policy of the FBIK section. Chain: the grabbed CHAIN
/// alone — Ctrl+drag (2026-09-26): the bones from the grabbed joint up to, not including, where its
/// limb joins the body (IkRig::limbJunction, the mass-based one: an arm to the chest with its
/// collar, a leg to the pelvis, the head and neck to the chest, a spine joint's chain down to the
/// pelvis, a collar by itself) plus what hangs below the grab (a planted foot under a dragged knee),
/// and nothing else: no root translation or rotation, no balance, no step, no lift-off, no arm hang
/// or head righting. A pelvis grab moves the pelvis and only what keeps the planted contacts planted
/// (the legs; a planted hand's arm). The limits, the floor and the body volumes hold as ever.
///
/// Figure (2026-09-28): THE FIGURE, MOVED AS SHE IS POSED — the whole body follows the cursor as one
/// rigid piece (the solve root's pose translation), lifted off the floor or carried across it: no
/// floor contact is planted, nothing balances or steps, and no joint turns. USER PINS HOLD: a
/// pinned limb's chain gives to keep its joint where it is pinned (and when it runs out, the body
/// stops); a pin on the trunk holds the figure where she is. The floor itself still stops her on
/// the way down. See armatureikfigure.cpp.
///
/// Part: what the app's Ctrl+drag ASKS for — resolved at the press by what was grabbed: Figure for
/// a grab of the HIPS (the solve root, which a grab of the pelvis girdle is promoted to; from
/// 2026-09-28 to 2026-10-28 the abdomen and the chest too — Armature::isBodyGrab, which the figure
/// move's "a pin on the trunk holds her" still reads), Chain for anything else (a limb, the head
/// and neck, and a spine joint: the trunk alone, down to the pelvis), and a digit's own drag for a
/// finger or a toe.
enum class IkScope { Body, Chain, Figure, Part };

/// WHAT a bone is to the posing UI (2026-09-28; Armature::boneClass). Body: a joint of the body —
/// selectable, dragged by the whole-body or the scoped solve. Face: a bone of the FACE RIG — every
/// bone below the head (the jaw, the lips, the brows, the eyes, the ears, the tongue): they are for
/// expressions, none of the IK's business, and not selectable — a click on the face selects the
/// HEAD (Armature::posingBone). Digit: a joint of a finger or a toe — dragged, it moves its own
/// digit alone, up to where the digit joins the hand or the foot (the DIGIT DRAG, armatureikdigit.cpp).
/// Carpal (2026-10-28): a bone below a wrist or an ankle that is neither the limb end nor a digit —
/// the carpals, a hand's prop anchor, the mid-foot, the ball, the heel: the hand's or the foot's
/// own flesh, not a joint to pose — a click on one selects the HAND or the FOOT (posingBone), as a
/// click on the face selects the head, and it lights with the selected hand or foot.
enum class BoneClass { Body, Face, Digit, Carpal };

/// The three TRANSFORM DIALS of a joint (the Transform tab; Armature::jointDial): the bend in the
/// joint's natural direction, the bend in its secondary direction, and the twist about its length.
enum class JointDialKind { BendForward = 0, BendSideways = 1, Twist = 2 };
constexpr int kJointDialCount = 3;

/// One dial: the Euler channel it turns and the scale it reads on. 0 is the rest pose, 100 the
/// channel's limit in the dial's own direction (`fullDeg`, signed), and a negative value bends the
/// other way IN THE SAME PROPORTION — value / 100 * fullDeg degrees — until the channel's other
/// limit stops it: a knee authored -11..155 reads -7.1..100. `bone` is -1 for a motion the joint
/// does not have (a locked channel: a hinge elbow's side bend).
struct JointDial {
    int   bone = -1;       ///< The bone whose channel the dial turns (a limb's TWIST may be its twist bone's).
    int   axis = 0;        ///< ... and the channel (0/1/2 = x/y/z).
    float fullDeg = 0.0f;  ///< The channel's angle at dial 100.
    float minValue = 0.0f; ///< The dial's range: where the channel's limits stop it.
    float maxValue = 0.0f;
    bool  valid() const { return bone >= 0; }
};

/// How turning a dial moves the limb it swings — what the viewport's mouse mode (B / S / T) reads
/// to let the limb go the way the mouse goes (Armature::jointDialSweep).
struct JointDialSweep {
    glm::vec3 tip{0.0f};          ///< A world point on the limb the dial swings: its far end, as posed.
    glm::vec3 tipPerDegree{0.0f}; ///< That point's world velocity per degree toward the dial's POSITIVE side.
    float     degreesPerUnit = 0.0f; ///< Degrees of the channel per dial unit (|fullDeg| / 100).
};

class Armature {
public:
    Armature();
    ~Armature();
    Armature(Armature&&) noexcept;
    Armature& operator=(Armature&&) noexcept;
    Armature(const Armature&) = delete;
    Armature& operator=(const Armature&) = delete;

    /// Builds the runtime skeleton from @p bones (parents must precede children — figure
    /// skeletons are listed in hierarchy order) at bind pose. An empty list makes a static
    /// armature: no bones, one identity joint. CONSTRUCT-ONCE contract: build() resets EVERY
    /// member first (pose, selection, pins, transform, and the whole IK state block), so it is
    /// exactly equivalent to building a freshly constructed Armature — which is how it is used
    /// (once per Model, right after construction; the harness does the same).
    void build(const std::vector<ArmatureBone>& bones);

    /// Writes the skeleton (one bone per line — see loadDump for the format) so the IK harness
    /// can run its drag-loop tests against a REAL figure's rig. Returns false if @p path can't
    /// be written. Debug hook (POSESTUDIO_DUMP_SKELETON=<path> in Model's constructor).
    bool dump(const std::string& path) const;

    /// Reads a skeleton written by dump(): `name parent order tx ty tz ox oy oz minx miny minz
    /// maxx maxy maxz lx ly lz` per line (local bind translation, orientation Euler degrees, the
    /// per-axis limits and their enable flags). Returns false (leaving @p out empty) on a read or
    /// parse failure.
    static bool loadDump(const std::string& path, std::vector<ArmatureBone>& out);

    /// The figure's BODY MESH SAMPLE (bodymesh.h): the skinned mesh's bind positions with the
    /// bone each follows. Set once after build() (the Model at import; the harness from the
    /// dump's sidecar); the IK rig fits its self-collision volumes to it when it is built
    /// (IkRig::buildBodyVolumes) — without one the volumes are sized from the skeleton. A rig
    /// already built is dropped so the next use rebuilds it with the mesh. dump() writes the
    /// sample next to the skeleton as `<path>.mesh`.
    void setBodyMesh(std::vector<BodyMeshPoint> mesh);
    const std::vector<BodyMeshPoint>& bodyMesh() const { return m_bodyMesh; }

    // --- Skeleton ---
    bool               hasSkeleton() const { return !m_bones.empty(); }
    std::size_t        boneCount() const { return m_bones.size(); }
    const std::string& boneName(std::size_t i) const { return m_boneNames[i]; }
    int                boneParent(std::size_t i) const { return m_bones[i].parent; }
    /// Index of the bone named @p name, or -1.
    int boneIndex(const std::string& name) const {
        const auto it = m_boneIndex.find(name);
        return it == m_boneIndex.end() ? -1 : it->second;
    }
    /// The number of skinning joints the shaders see: the bone count, or 1 (identity) for a
    /// static armature.
    std::uint32_t jointCount() const {
        return static_cast<std::uint32_t>(m_bones.empty() ? 1 : m_bones.size());
    }

    // --- World transform (the model matrix) ---
    const glm::mat4& transform() const { return m_transform; }
    /// Translates the figure by @p dy along world Y (the animated ground drop applies its
    /// per-frame fall increments through this) and refreshes the bone world positions.
    void translateY(float dy);
    /// Moves the figure by @p dy along world Y IN HER POSE: the solve root's pose translation —
    /// the figure's place, what a hip walk and the Ctrl+drag figure move write and a pose file
    /// carries as its `@trans:` row. The Ground button's drop goes here (2026-09-28), so it is a
    /// pose edit like any other: undoable, saved with the pose, and taken back by Reset Pose
    /// together with everything else. (It went into the model transform until then, which no pose
    /// operation touches: a figure lifted in her pose, grounded, and reset stood below the floor
    /// by the lift.) A boneless model has no pose: there it is translateY().
    void shiftPoseY(float dy);

    /// Replaces the model's world transform (the .pss load path restores a saved TRS here)
    /// and refreshes the transform-dependent bone world positions, like translateY().
    void setTransform(const glm::mat4& transform);

    // --- Pose state ---
    /// Current world-space position of each joint (updated whenever the pose changes).
    const glm::vec3& boneWorldPosition(std::size_t i) const { return m_boneWorldPos[i]; }
    /// The accumulated pose rotation of bone @p i (Euler degrees, its rotation order).
    const glm::vec3& boneEuler(std::size_t i) const { return m_boneEuler[i]; }
    /// The pose translation of bone @p i (parent frame; non-zero only where FBIK moved the root).
    const glm::vec3& boneTranslation(std::size_t i) const { return m_boneTranslation[i]; }
    /// Bone @p i's posed model-space transform (poseGlobal) as of the last pose update.
    const glm::mat4& poseGlobal(std::size_t i) const { return m_poseGlobal[i]; }
    /// A HAND's palm normal as posed (world, unit), or zero for a bone that is not a wrist (see
    /// ensureHandMaps): the bind's DOWN across the fingers' line — palms face down in a T-pose,
    /// down and in in an A-pose — carried by the hand's rotation. Where a laid hand faces: the
    /// harness's hand-lay gates and the in-app scripts' `palm.<bone>.<axis>` read it.
    glm::vec3 handPalmNormal(int wrist) const;
    /// Bone @p i's inverse bind transform (the skin transform is poseGlobal · inverseBind).
    const glm::mat4& inverseBind(std::size_t i) const { return m_bones[i].inverseBind; }
    /// The skinning DUAL QUATERNIONS — per joint two vec4s: real = rotation as (x,y,z,w), dual =
    /// 0.5·(0,t)·real carrying the translation — recomputed by every pose update. The shaders
    /// blend THESE, not matrices: the figure format authors its weights (and every pose
    /// corrective) against dual-quaternion skinning, and linear matrix blending collapses deep
    /// bends (a 155° knee folded into a shapeless blob that the pose correctives made worse).
    const std::vector<glm::vec4>& skinDualQuats() const { return m_skinDualQuats; }
    /// Bumped by every pose update; the GPU layer uploads per frame in flight when it changes.
    std::uint64_t skinVersion() const { return m_skinVersion; }

    /// Recomputes every bone's poseGlobal from its poseLocal, the world positions, and the skin
    /// dual quaternions. Every pose path ends here; callers that edit several bones' Euler
    /// through the primitives below call it once afterwards.
    void computeSkinMatrices();

    // --- Selection (the posing UI's current joint) ---
    int  selectedBone() const { return m_selectedBone; }
    /// The joint the selection HIGHLIGHT shows (the tinted flesh, the skeleton overlay's hot
    /// segments): the selected joint — the USER's through a landing bounce, which drags the hips
    /// by the solve's own machinery and must not light them up for its half second.
    int highlightBone() const { return m_landing.active ? m_landing.selected : m_selectedBone; }
    /// Selects bone @p index (-1: nothing) — or, for a bone of the face rig, the HEAD (posingBone).
    void setSelectedBone(int index) {
        m_selectedBone = posingBone(index);
        m_ikGrabBone = -1; // (a grab point is the pick's that made the selection: Scene::selectBoneAt sets it after)
        clearGroup();      // (a plain selection is of ONE joint: THE GROUP, below)
    }
    /// Selects the bone named @p name (diagnostics / the IK benchmark; a face-rig bone's name
    /// selects the head); the index of the bone NAMED, or -1.
    int selectBoneByName(const std::string& name) {
        const int index = boneIndex(name);
        if (index >= 0) {
            m_selectedBone = posingBone(index);
            m_ikGrabBone = -1;
            clearGroup();
        }
        return index;
    }
    /// What @p bone is to the posing UI (see BoneClass). Found by STRUCTURE with the IK rig, which
    /// this builds on first use: the face rig is what hangs below the rig's head; a digit's joint
    /// hangs below a wrist (ensureHandMaps) or below an ankle (the most proximal joint of a foot),
    /// BENDS (its widest limited channel spans kDigitMinRangeDeg or more: a carpal's does not),
    /// stands OUT in its hand or foot (kDigitMinReachShare of the limb end's reach: a heel bone or
    /// a mid-foot bone at the ankle does not) and carries ONE digit at most (the ball of the foot
    /// the toes fan from is the foot's, a carpal with two fingers the hand's). Body for a boneless
    /// model. POSESTUDIO_POSE_ALL_BONES makes every bone Body (the A/B probe: the face rig
    /// selectable and a digit grab the limb's, as before 2026-09-28); POSESTUDIO_BONE_CLASS_TRACE
    /// prints what was found.
    BoneClass boneClass(int bone);
    /// The bone a selection of @p bone selects: the HEAD for a bone of the face rig, the hand or
    /// the foot (the limb end, m_boneLimbEnd) for a Carpal, else itself.
    int posingBone(int bone);
    /// posingBone() as the classes stand, without building the rig (@p bone itself until it is
    /// built): for read-only callers — the scripted test asks what a name's selection IS.
    int knownPosingBone(int bone) const;
    /// True when @p bone's flesh is LIT with the selection though the bone is neither the selected
    /// joint nor its twin: a bone of the FACE RIG while the head is selected — the face is the
    /// head's, and lit by the head bone's own skin weights alone a click on a cheek lit the scalp —
    /// or a CARPAL while its hand or foot is (the palm is the carpals' flesh, 2026-10-28).
    /// (Reads the classes as they stand: false until the rig has been built.)
    bool highlightedWithSelection(int bone) const;
    /// The highlighted joint's twin (see m_highlightTwin), or -1 without a selection or twin.
    int selectedHighlightTwin() const {
        const int bone = highlightBone();
        return (bone >= 0 && bone < static_cast<int>(m_highlightTwin.size())) ? m_highlightTwin[static_cast<std::size_t>(bone)]
                                                                              : -1;
    }

    // --- THE GROUP (2026-10-27): several joints taken hold of at once. Shift+click adds a joint
    // to the selection or takes it out; Shift+drag of any member pulls them all. The PRIMARY is the
    // selected joint — the pressed one: its grab point, its drag plane and every policy of a plain
    // drag are its — and the others are SECONDARIES riding the cursor's displacement with it
    // (beginIkDrag, updateGroupTargets). A group drag is a whole-body drag: the body answers as it
    // answers one joint — balance, steps, the hips yielding to a push, a tiptoe or a lift-off under
    // a pull beyond reach — with every grabbed limb freed and priced as the dragged one.
    /// Adds @p bone to the group as a secondary, or removes it when it is one; a bone of the face
    /// rig stands for the head, and a finger's or a toe's joint — posed alone (the digit drag) —
    /// or the primary itself is refused. Returns true when the bone is a member afterwards.
    bool toggleGroupBone(int bone);
    /// True when @p bone is a secondary of the group.
    bool inGroup(int bone) const;
    /// The secondaries, in the order they were added (never the primary).
    const std::vector<int>& groupBones() const { return m_groupBones; }
    void clearGroup();
    /// Makes @p bone the PRIMARY — a Shift+press on a member drags the group through that joint,
    /// its grab point the drag plane's: the previous primary joins the secondaries, @p bone leaves
    /// them; a bone that was no member is added this way. A face-rig bone stands for the head; a
    /// digit's joint is refused (false).
    bool makeGroupPrimary(int bone);
    /// Removes @p bone from the group — a Shift+CLICK on a member: a secondary simply leaves; the
    /// primary hands the selection to the first secondary or, alone, is deselected. False for a
    /// non-member.
    bool removeFromGroup(int bone);
    /// Bumped by every change of the group (the GPU's lit-joint upload keys on it).
    std::uint32_t groupVersion() const { return m_groupVersion; }
    /// Diagnostics (the scripted test, whose pixel lookup selects a bone BY NAME): puts the
    /// selection back — @p primary selected (-1: nothing) with @p secondaries as the group.
    void restoreSelection(int primary, const std::vector<int>& secondaries) {
        m_selectedBone = primary;
        m_ikGrabBone = -1;
        m_groupBones = secondaries;
        ++m_groupVersion;
    }

    // --- Forward-kinematic posing ---
    /// Poses the joint @p boneName by an Euler rotation (degrees) applied in its oriented frame
    /// (clamped to its anatomical limits), then recomputes the skin data. Returns false (no-op)
    /// if there is no such bone. @p eulerDegrees of 0 restores the bone's rest pose.
    bool setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees);
    /// Adds @p deltaEulerDegrees to the selected bone's accumulated rotation and re-poses it.
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees);

    // --- THE TRANSFORM DIALS (armaturedials.cpp): a joint's rotation as three 0-100 dials ---
    /// The joint a selection of @p bone presents: the bone itself, or — for a TWIST bone, which is
    /// a piece of its limb's rigid segment and no joint of its own — the bend bone it belongs to
    /// (a click on the lower thigh finds the thigh's twist bone: the joint is the thigh). -1 for
    /// a bad index.
    int dialJoint(int bone) const;
    /// Dial @p kind of the joint @p bone presents (dialJoint), found by STRUCTURE:
    ///   - the TWIST channel is the bone's length axis — the rotation order's first axis, the
    ///     format's own statement of it. Where the bend bone's is locked (a limb whose twist is
    ///     cut out into a twist bone), the dial turns that twist bone's;
    ///   - of the two others, BEND FORWARD is the joint's natural direction — the channel with
    ///     the wider range (a knee's or an elbow's fold, a wrist's flexion), or, where the two
    ///     are near equal (a shoulder, a spine joint), the one that carries the limb fore and aft
    ///     — and BEND SIDEWAYS the other;
    ///   - each dial's POSITIVE direction: a one-sided channel's long side (a knee flexes, an
    ///     elbow folds, a collar shrugs); else, for the forward bend, the way that takes what the
    ///     joint carries FORWARD, or where it moves up and down, DOWN (a wrist flexes, a finger
    ///     and a toe curl, a foot points); for the side bend and the twist the channel's longer
    ///     side, and on an even channel outward / toward her left — mirrored left and right, so
    ///     the same value on a left and a right joint gives mirrored poses.
    /// Reads the bone classes as they stand (the head is the bone the face rig hangs from): the
    /// same answer before and after the rig is built for every bone but the head, whose selection
    /// builds it anyway. An invalid dial for a bad index or a locked channel.
    JointDial jointDial(int bone, JointDialKind kind) const;
    /// The dial's reading of the pose as it stands.
    float jointDialValue(const JointDial& dial) const;
    /// Turns the dial's channel to @p value (clamped to the dial's range) — an FK rotation
    /// through the limits and the FK collision stop, so the pose may rest short of the value
    /// asked (read it back). False when nothing changed or the dial is invalid.
    bool setJointDial(const JointDial& dial, float value);
    /// How turning @p dial moves the limb it swings, in the pose as it stands: the limb's far end
    /// (the joint plus the way the bone lies, limbDirection, over the length of its rigid segment)
    /// and that point's velocity per degree of the dial's positive direction — the channel's
    /// axis as it stands (the rotation-order prefix applied, as the solver's Jacobian reads it)
    /// crossed with the lever. A TWIST turns the limb about its own length: its far end moves
    /// little or not at all (nil on a twist bone and on a straight limb) — the caller has no
    /// direction to follow there. False for an invalid dial.
    bool jointDialSweep(const JointDial& dial, JointDialSweep& sweep) const;

    /// Captures the current pose as (bone name, Euler degrees) for each non-rest joint, plus
    /// "@trans:<bone>" rows for pose translations and "@pin:<bone>" rows for user pins (see
    /// the pin notes below) — the snapshot the undo stack and the .pose file carry.
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const;
    /// Resets to bind pose, then applies @p pose (bone name -> Euler degrees, @trans:/@pin:
    /// rows honoured, unknown names ignored) and recomputes the skin data.
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);

    // --- Pose utilities: reset and mirror (the Edit menu / joint context menu). Pins are left
    // untouched by all of them — they are constraints, not shape (Unpin All exists for that).
    /// The bone on the OTHER side of the body: names pair by prefix — `l`/`r` followed by an
    /// uppercase letter or underscore (`lShin`/`rShin`, `l_thigh`/`r_thigh`), or `Left`/`Right`
    /// — and a centre bone (or an unpaired name) maps to itself.
    int mirrorBone(std::size_t index) const { return m_mirrorBone[index]; }
    /// Returns bone @p index to rest (rotation and pose translation zero); with @p subtree,
    /// every descendant too. False without such a bone.
    bool resetBone(int index, bool subtree);
    /// Returns every bone to the bind pose (rotations + translations).
    void resetPose();
    /// Mirrors the WHOLE pose across the sagittal plane: left and right swap, centre bones flip.
    /// Per Euler channel the mirror is (x, -y, -z) and the pose translation negates x — exact
    /// for a reflection because the figure's left/right orientation frames are themselves
    /// mirrored ((a, b, c) <-> (a, -b, -c)), so the reflected world rotation lands in the other
    /// side's frame with exactly those channel values (verified geometrically in the harness).
    void mirrorPose();
    /// Copies bone @p index's pose AND its subtree's to the opposite side, mirrored (the
    /// matching bones there take the mirrored values; centre bones inside the subtree flip in
    /// place, so mirroring from the chest mirrors both arms and the head). False without such a
    /// bone.
    bool mirrorSubtreeToOpposite(int index);

    // --- The IK GRAB POINT: where on the selected bone the user took hold ---
    /// A click picks a bone by its BODY as readily as by its joint, and what the user has hold of
    /// is the point under the cursor — not the bone's joint, which may be a forearm's length
    /// away. The drag plane passes through this point and the targets are where IT should go;
    /// what the solve drags is the nearest REAL joint of the rigid limb segment the point lies
    /// on (twist bones are part of their segment: a thigh is one piece from socket to knee), the
    /// point kept under the cursor as that segment turns. Before this the JOINT was asked to the
    /// cursor: a mid-thigh click hauled the hip socket — the pelvis — 20cm down to the pointer.
    /// @p worldPoint is taken as rigid with @p bone; cleared by any other selection.
    void setIkGrabPoint(int bone, const glm::vec3& worldPoint);
    /// The same by segment share (the harness): @p share of the way from the rigid segment's
    /// near joint to its far one, the segment being the one @p bone belongs to.
    void setIkGrabOnSegment(int bone, float share);
    /// The grab point as the pose has it now (world): the selected joint without one.
    glm::vec3 ikGrabPointWorld() const;

    // --- Full-body IK (scene/ik/): drag a joint, the whole body follows anatomically ---
    /// Begins an FBIK drag of the SELECTED joint: detects which joints are planted on the ground,
    /// pins them (feet for a standing figure — never the hip), and builds the balance support
    /// polygon. Builds the IK rig from the skeleton on first use. Returns false without a
    /// skeleton or selection. @p scope: the whole body (the plain drag), or the grabbed chain
    /// alone (Ctrl+drag) — see IkScope.
    bool beginIkDrag(IkScope scope = IkScope::Body);
    /// True while the drag in flight is a DIGIT drag (beginIkDrag on a finger's or a toe's joint):
    /// the digit alone moves, whatever the scope asked for, and the rig has no drag of its own.
    bool ikDigitDrag() const { return m_digitDrag; }
    /// One FBIK drag tick: the whole body is SOLVED, to convergence, so the selected joint sits
    /// on @p targetWorld with every pin, the floor, the body volumes and balance honoured, directly
    /// in the pose's own unknowns (the Euler channels inside their authored limits + the root's
    /// translation — scene/ik/jointsolver.h), and the result is applied in full. Returns true if
    /// the pose changed (or is still easing). Correctives are the Model's business.
    bool dragIkTo(const glm::vec3& targetWorld);
    /// One release tick after the button comes up. The pose already satisfies its pins, so there
    /// is nothing to land — except a pin target still being approached (a hovering figure
    /// healing onto the floor, a step that was in flight): that finishes here, with a placed hand
    /// or foot held where it was let go. Call at the drag tick rate until it returns false.
    bool settleIkTick();
    /// Ends the FBIK drag (the solved pose stays).
    void endIkDrag();
    /// The rig (null until built: by prepareIkRig, the first IK drag or the first pick) —
    /// diagnostics and the harness.
    const IkRig* ikRig() const { return m_ikRig.get(); }
    /// Builds the rig now (the roles of the rig standard with it): a figure's import does, so that
    /// the test tools' bone-name fallback (bonealiases.h) can resolve a name through the roles from
    /// the first command, and the first click pays nothing. A static model (no bones) builds none.
    void prepareIkRig() { ensureIkRig(); }
    /// True when the two bones are pieces of ONE rigid limb segment (a bend bone and its twist
    /// bones: a thigh from socket to knee), or the same bone. The scripted in-app test asks it:
    /// a pick that lands on a thigh's twist bone HAS found the thigh.
    bool sameRigidSegment(int a, int b) const;
    /// The rigid segment @p bone is a piece of — its NEAR joint (a twist bone's non-twist parent)
    /// and its FAR joint (the next real joint down the main chain): the joints a click on that
    /// body may snap to (Scene::selectBoneAt). False for a leaf with no segment.
    bool segmentJoints(int bone, int& nearJoint, int& farJoint) const { return rigidSegment(bone, nearJoint, farJoint); }
    /// The solve's DAMPING level (0 = none ... 3 = strong; process-wide, initial value
    /// IK_JS_DAMPING or kIkDampingDefault = slight): a converged solve is instantaneous, which
    /// reads as a touch too snappy, so (1) the POSTURE eases toward its solution over a few ticks
    /// INSIDE the solve — each channel's posture term also pulls toward the previous tick's
    /// value, which leaves every constraint row (the cursor, the pins, the floor, the body
    /// volumes) exact on every tick and the resting pose unchanged; what eases is only the
    /// body's redundant follow-through — and (2) the cursor target runs through a critically
    /// damped follower about a tick long, which rounds the starts and stops of the grabbed joint
    /// itself. (Blending whole poses in joint space, the obvious damping, slides planted feet.)
    static int ikDamping();
    static constexpr int kIkDampingDefault = 1;
    static constexpr int kIkDampingLevels = 4;
    /// A channel whose limits span less than this is LOCKED (a twist bone's swing axes, a hinge's
    /// other channels): it has no dial and is never an unknown of a solve (isTwistBone's measure).
    static constexpr float kLockedRangeDeg = 2.0f;
    /// A FOLD channel (a knee's, an elbow's bend): at least kFoldRangeDeg of range, all but
    /// kFoldNarrowFraction of it to one side of straight; and a fold joint is a HINGE — its other
    /// channel, the twist excepted, spans no more than kFoldHingeOtherDeg (a knee's side channel
    /// spans 10 degrees, an elbow's is locked; a thigh's abduction, -85..15 on two generations,
    /// is no fold). isFoldChannel / foldAxisOf read the first two; the solve's posture model
    /// and the digit drag add the hinge test.
    static constexpr float kFoldRangeDeg = 100.0f;
    static constexpr float kFoldNarrowFraction = 0.2f;
    static constexpr float kFoldHingeOtherDeg = 60.0f;

    // --- User joint pins (see IkRig::beginDrag's userPins): a pinned joint is held exactly where
    // it is through every later IK drag of OTHER joints, until unpinned. Dragging a pinned joint
    // itself moves the pin. Pins are part of the POSE SNAPSHOT (capturePose/applyPose carry them
    // as "@pin:<bone>" rows), so a pin toggle is undoable, undoing a drag restores the pins of
    // that moment, and a .pose file reproduces its pins on load (a file without pin rows — an
    // older one — loads with none).
    /// Toggles the pin on the selected joint; returns the new pinned state (false with no selection).
    bool togglePinSelectedBone();
    bool isBonePinned(std::size_t index) const {
        return index < m_bonePinned.size() && m_bonePinned[index] != 0;
    }
    bool selectedBonePinned() const {
        return m_selectedBone >= 0 && isBonePinned(static_cast<std::size_t>(m_selectedBone));
    }
    bool hasPinnedBones() const; ///< Any user pin set (the context menu's "Unpin All").
    void unpinAllBones();        ///< Clears every user pin (an undoable pose edit at the window).
    /// The rig's CONTACT pins (ground-detected, not user pins) while an IK drag is active — for
    /// the overlay's "which feet are planted" markers. Empty outside a drag.
    std::vector<int> activeContactPins() const;
    // --- THE LANDING BOUNCE (armaturelanding.cpp): the Ground button's fall, landed on her feet ---
    /// Begins the landing's absorption for a figure that has just come down at @p impactSpeed (m/s,
    /// the fall's speed at the floor): the hips will dip over the planted feet, the legs folding
    /// under them, and come back up — a pelvis drag made by the solve's own machinery (the scoped
    /// one: the pelvis and the legs alone, no balance, no step, nothing re-posed), its depth the
    /// speed's (kLandingDip* in the tuning header). True when a bounce began. False — and nothing
    /// changed — when she did NOT land on her feet (a knee, a hand, the seat or the head on the
    /// floor; no foot on it; a pinned pelvis), when the dip would be imperceptible, without a
    /// skeleton, or while a drag owns the pose. PURELY AESTHETIC: the pose the bounce ends in is
    /// the pose it began in, exactly (endLandingBounce), so it is no pose edit and no undo entry.
    bool beginLandingBounce(float impactSpeed);
    /// The bounce @p seconds after the landing: the pose for that moment. False once it is over
    /// (or none is in flight): the caller then calls endLandingBounce().
    bool landingBounceTick(float seconds);
    /// Ends the bounce, wherever it is: the pose she landed in, exactly, and the user's selection.
    void endLandingBounce();
    bool landingBounceActive() const { return m_landing.active; }
    /// The dip the bounce in flight was given (m; 0 without one): the harness and the scripts.
    float landingBounceDip() const { return m_landing.active ? m_landing.dip : 0.0f; }
    /// True for a joint whose floor clearance stands down because its foot LIES on its top behind a
    /// planted knee (the ankle and the mid-foot: their clearances are the standing foot's bind
    /// heights, and a lying ankle sits at the shin's radius) — the harness's penetration measure
    /// reads it, as the solve's floor rows do. See A KNEELING FOOT LIES FLAT.
    bool footLyingExempt(int node) const;

private:
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
    };

    /// Re-poses bone @p index from its accumulated Euler (m_boneEuler) in its oriented frame, then
    /// recomputes the skin data. Shared by setBoneRotation() and nudgeSelectedBone().
    void applyBoneEuler(int index);
    /// Adds @p deltaEulerDegrees to bone @p bone's rotation through the FK COLLISION STOP (see
    /// fkVolumeDepth) — the body of nudgeSelectedBone, and of a transform dial, which may turn a
    /// bone other than the selected one (a limb's twist bone).
    void nudgeBone(int bone, const glm::vec3& deltaEulerDegrees);
    /// The way bone @p bone lies, distally, at bind (model space, unit): its own length axis — the
    /// rotation order's first axis in its orientation frame — pointing at what the joint carries.
    glm::vec3 limbDirection(int bone) const;
    /// POSESTUDIO_ORDER_AS_WRITTEN: compose the pose channels in the rotation order AS WRITTEN
    /// (leftmost outermost), the composition before 2026-10-27 — the A/B probe (see build).
    static const bool kOrderAsWritten;
    /// The format's own rotation_order string for @p b (the stored one is reversed; see build).
    static std::string formatRotationOrder(const Bone& b);
    /// The letter of the bone's LENGTH axis — the format's first channel, the twist.
    static char lengthAxisLetter(const Bone& b);
    /// Builds the IK rig (graph, constraints, masses, body volumes) from the bind skeleton on
    /// first use — an IK drag or an FK rotation, which checks the body volumes. False for a
    /// boneless model.
    bool ensureIkRig();
    /// The FK COLLISION STOP's measure: the deepest penetration (m) of any tested joint into
    /// any body volume (IkRig::bodyVolumes) among the pairs that MOVE relative to each other
    /// when @p rotatedBone turns — a joint in its subtree against a volume outside it, or the
    /// reverse. nudgeSelectedBone bisects a rotation back to where this stops growing.
    float fkVolumeDepth(int rotatedBone) const;

    /// Recomposes bone @p index's poseLocal from its current Euler + pose translation:
    /// localBind · orient · R(euler, order) · orient⁻¹, translation added in the parent frame.
    /// The single composition point every pose path (FK, pose load, the IK solve) shares.
    void recomposePoseLocal(std::size_t index);

    /// Appends bone @p index and every descendant to @p out (anatomical hierarchy).
    void collectSubtree(int index, std::vector<int>& out) const;
    /// Re-poses every bone from its (limit-clamped) Euler + translation, then the skin data once.
    void reposeAll();

    /// Clamps m_boneEuler[index] in place to the bone's per-axis rotation limits (a no-op on axes
    /// the figure leaves unconstrained). The single enforcement point every posing path funnels
    /// through.
    void clampBoneEuler(int index);

    std::vector<Bone>                    m_bones;
    std::unordered_map<std::string, int> m_boneIndex; // bone name -> index into m_bones
    std::vector<std::string>             m_boneNames;  // parallel to m_bones (for the posing UI)
    std::vector<BodyMeshPoint>           m_bodyMesh;   // see setBodyMesh()
    glm::mat4                            m_transform{1.0f};
    std::vector<glm::vec3>               m_boneWorldPos; // current world position per bone (overlay/pick)
    // Per bone: its highlight TWIN (-1 if none). Figures split each limb segment into a bend
    // bone and a TWIST child (isTwistBone: two locked axes, the free one along the bone) — the
    // mid-limb bone that spreads axial twist across the skin. The selection highlight covers the
    // pair (bend -> its twist child, twist -> its bend parent), so grabbing the upper-arm joint
    // lights the whole upper arm rather than the half the bend bone's own weights cover. A HINGE
    // (a finger's or a toe's middle joint, an elbow with its other channels locked) is no twist
    // bone and has no twin: every joint of a digit lights its own phalanx.
    std::vector<int>                     m_highlightTwin;
    std::vector<std::vector<int>>        m_children;   // anatomical children per bone (subtree walks, the foot contact model's ball search)
    std::vector<int>                     m_mirrorBone; // the other side's bone per bone (self for centre)
    std::vector<glm::mat4>               m_poseGlobal;  // scratch for computeSkinMatrices (it runs per drag-move; no per-call allocation)
    std::vector<glm::vec3>               m_boneEuler;  // accumulated pose rotation per bone (degrees)
    // Pose translation per bone (parent-frame offset added to poseLocal). Rotations alone can't
    // move the skeleton root, so FBIK with pinned feet writes the hip's solved displacement here
    // (a crouch drops the pelvis). Round-trips through capture/applyPose as "@trans:<bone>" rows.
    std::vector<glm::vec3>               m_boneTranslation;
    int                                  m_selectedBone = -1;
    /// THE GROUP's secondaries (see toggleGroupBone): the other joints a Shift+drag pulls with the
    /// selected one. Never the primary, a face-rig bone (the head stands for it) or a digit.
    std::vector<int>                     m_groupBones;
    std::uint32_t                        m_groupVersion = 0;
    // Skinning data (see skinDualQuats()), recomputed whenever the pose changes; the GPU layer
    // uploads it lazily per frame in flight when m_skinVersion moves.
    std::vector<glm::vec4>               m_skinDualQuats;
    std::uint64_t                        m_skinVersion = 0;

    // Full-body IK: the rig (the drag's policy: body tree, masses, volumes, contacts — built
    // lazily on first use), the joint solver's skeleton, and the model-space bind joint positions
    // (the foot contact model finds the ball of a foot by bind height).
    std::unique_ptr<IkRig>        m_ikRig;
    std::vector<glm::vec3>        m_ikBindPos;
    // HELD ORIENTATIONS: each pin's drag-start world rotation — a planted foot keeps its sole
    // flat on the floor as the body moves above it, a user pin is held in all six degrees.
    std::vector<int>              m_ikFlatNodes;
    std::vector<glm::mat3>        m_ikFlatRot;
    // Parallel to m_ikFlatNodes: the pin each entry belongs to. A CONTACT pin's whole subtree
    // (a foot's metatarsals and toes) is listed after the pin itself with the pin as owner: the
    // solve holds the BALL of the foot's orientation from it, so when a deep crouch pitches
    // the sole past the ankle's range the toes bend back and stay flat on the floor.
    std::vector<int>              m_ikFlatOwner;
    /// Captures the flat-sole hold (m_ikFlatNodes/Rot/Owner) from the rig's pins: each pin and,
    /// for a contact pin, its whole subtree, at their CURRENT world orientation (a drag start)
    /// or at the REST orientation (a landing out of suspension: the feet that dangled pitched
    /// roll flat onto the floor at the hold's capped pace). A user pin's own node always keeps
    /// its current orientation (the pin is held 6-DoF).
    void captureFlatHold(bool restOrientation);
    /// beginIkDrag's parts (armatureik.cpp), in the order it calls them: the drag's state reset in
    /// two halves around the grab offset's capture, then the two drag-start readings.
    void resetContactState();                                                              ///< the seat and kneel readings, the contacts' and the flat-foot model's state
    void captureDragStart(const std::vector<glm::vec3>& positions, float groundOffsetY);  ///< the posture reference, every stage's state, the root's upright
    void findWeightShift();                                                                ///< THE WEIGHT SHIFT: a two-footed stance's lift takes the hips over the standing foot
    void resetFootAndHandState();                                                          ///< the balls, the eased targets, the hand slide and lay
    void findTiptoeDrag();                                                                 ///< THE TIPTOE: whether this drag is one that can lift the heels
    // User joint pins (per bone, see togglePinSelectedBone): persistent until unpinned.
    std::vector<char>             m_bonePinned;
    // The drag-start Euler pose: the solve's POSTURE REFERENCE (every channel is pulled back
    // toward it — what makes the solved pose a function of the targets).
    std::vector<glm::vec3>        m_ikStartEuler;
    // Grab offset (model space) for a PROMOTED drag: the rig solves the limb's real end joint
    // (a finger grab drives the HAND — see IkRig::dragEffector), so the window's targets, which
    // track the grabbed joint, are shifted by (grabbed - solved) captured at drag start.
    glm::vec3                     m_ikGrabOffset{0.0f};
    // The grab point (setIkGrabPoint): the bone it is rigid with (-1: none, the selected joint
    // itself) and its place in that bone's frame; and, for the drag it began, the rigid limb
    // segment it lies on — near joint, far joint, the point's share of the way — and whether the
    // FAR joint is what the solve drags (see beginIkDrag / dragIkTo).
    int                           m_ikGrabBone = -1;
    glm::vec3                     m_ikGrabLocal{0.0f};
    int                           m_ikSegmentNear = -1;
    int                           m_ikSegmentFar = -1;
    float                         m_ikSegmentShare = 0.0f;
    bool                          m_ikGrabFarJoint = false;
    bool                          m_ikGrabMeasured = false; ///< the grab's offset from the effector is read off the pose each tick
    glm::vec3                     m_ikSegmentPrevNear{0.0f}; ///< the segment's joints at the last tick, and from them
    glm::vec3                     m_ikSegmentPrevFar{0.0f};
    bool                          m_ikSegmentPrevValid = false;
    float                         m_ikSegmentTranslating = 0.0f; ///< ... how much of the far joint's travel is the limb translating (see dragIkTo)
    /// A TWIST bone: two locked swing channels and the free one ALONG the bone (a thigh's or a
    /// forearm's twist helper) — part of its limb's rigid segment, never a joint of its own.
    bool isTwistBone(int bone) const;
    /// The mirror twins the names and the places left unpaired, by the rig standard's roles
    /// (armatureik.cpp; after the rig is built).
    void pairMirrorsByRole();
    /// True when channel @p axis of bone @p b is a FOLD channel (kFoldRangeDeg, kFoldNarrowFraction).
    bool isFoldChannel(int b, int axis) const;
    /// The first fold channel of bone @p b, or -1: a knee's or an elbow's bend.
    int  foldAxisOf(int b) const;
    /// How many of bone @p b's channels are unlocked (kLockedRangeDeg).
    int  freeChannelCount(int b) const;
    /// True when @p node is @p ancestor or hangs below it (the solver's O(1) ancestry once the rig
    /// is built; a parent walk before). False for -1 either way.
    bool boneUnder(int node, int ancestor) const;
    /// The bind length of the chain from @p from up to, not including, @p upTo (or the root): each
    /// bone's local bind offset, summed child-first.
    float chainLength(int from, int upTo) const;
    /// The upper arm's joint of @p hand's arm — the bone below the collar on the hand's path to its limb
    /// junction (the shoulder socket), or the hand's parent on a rig with no such chain.
    int   armSocketOf(int hand) const;
    /// The child a limb continues through: the one with the biggest subtree (-1 for a leaf).
    int  mainChild(int bone) const;
    /// The rigid limb segment @p bone belongs to: its near joint (up past any twist bones) and
    /// its far joint (down the main child past them); false for a leaf.
    bool rigidSegment(int bone, int& nearJoint, int& farJoint) const;
    // The solved effector's world rotation at drag start: the grab offset is carried through
    // the effector's rotation since (offset_now = R_now * R_start^-1 * offset), so a finger
    // grab keeps tracking the FINGER when the hand twists.
    glm::mat3                     m_ikGrabRotStart{1.0f};
    // The model matrix's inverse, cached at drag start: the transform is fixed for a drag's
    // duration (a press completes any ground fall first, and the Ground button is ignored
    // mid-drag), and dragIkTo maps every tick's world-space target through it.
    glm::mat4                     m_ikInvTransform{1.0f};
    int                           m_ikSettleTicks = 0; ///< Animated release-settle tick budget.
    // --- The per-tick solve (armatureiksolve.cpp) ---
    std::unique_ptr<JointSolver>  m_jointSolver;          ///< Built with the rig.
    std::vector<int>              m_jsTwistAxis;          ///< Per bone: its twist channel, or -1.
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
        /// the floor by it, and keeps NO floor rows in the solve (ikPlaneRows) — rows only the body
        /// could answer, and a prop: the heavy male's released arms rested along his body with the
        /// thumbs on the floor, and the thumbs' rows held his chest up (2026-10-29).
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
        /// the arm's BASE hang (`wants`), which is remembered across drags (m_hangBase): a turn the
        /// stop or a limit cut short does not corrupt the next drag's home.
        bool hasSwing = false;
        /// IDLE ELBOWS SOFTEN (hangIdleArms): the elbow is the hang's to bend -- at the bind at the
        /// press, or where the hang itself left it (m_hangElbowDeg). A posed elbow keeps its bend.
        bool elbowOurs = false;
    };
    std::vector<HangArm>          m_jsHangArms;
    bool                          m_jsHangArmsFound = false;
    /// THE STRIDE SWING the arm CARRIES, per socket (degrees, + forward): measured at the end of
    /// every hang tick as the arm's fore-aft angle against the hang's own un-swung target — what the
    /// stance's swing actually turned it by, a turn the stop or a limit cut short included — and
    /// undone at the next press to find the arm's un-swung base. Without it the next drag read a
    /// cut-short swing as the arm's home and a stance closed again left the arm 8 degrees forward;
    /// a remembered base DIRECTION was tried instead and broke the un-bow (the hang moves an arm
    /// between drags for other reasons too: plumb under a fold). Forgotten for an arm found POSED
    /// at a press, and by a pose reset or load (clearHangBases); unknown, the found pose is taken
    /// to carry its stance's swing (a loaded stride pose keeps its arms until the stance changes).
    std::vector<float>            m_hangSwingDeg;
    std::vector<char>             m_hangBaseValid;
    /// Per socket: the elbow's fold channel where the hang LEFT it (IDLE ELBOWS SOFTEN), and whether
    /// it did -- an elbow found there at the next press is still the hang's to bend; forgotten with
    /// the swing bases (clearHangBases) and for an arm found posed.
    std::vector<float>            m_hangElbowDeg;
    std::vector<char>             m_hangElbowValid;
    void clearHangBases() {
        m_hangBaseValid.assign(m_hangBaseValid.size(), 0);
        m_hangElbowValid.assign(m_hangElbowValid.size(), 0);
    }
    /// Per bone: where a POST-STEP (the arms' hang, the head's righting) last left it, and whether
    /// one has turned it this drag. When such a bone becomes one of the SOLVE's unknowns — a hanging
    /// hand comes down on the floor and is a contact, the body is lifted and every segment is the
    /// solve's — its posture reference is THIS, not the drag-start value the post-step turned it
    /// away from (see solveIk).
    std::vector<glm::vec3>        m_jsPostStepEuler;
    std::vector<char>             m_jsPostStepTurned;
    /// THE HEAD STAYS UP (rightIdleHead): the neck's bones, parent first, ending in the head; the
    /// bone they hang from (the chest); and the righting the drag FOUND there, in the chest's frame
    /// — what is left of the head's pose without it is the user's, over and above the righting.
    std::vector<int>              m_jsHeadChain;
    /// ... and the bones of it the righting HAS, this tick: those the solve does not (under a head
    /// drag the solve bends the neck and the righting has the head bone alone). Balance is blind
    /// to these, and to these only.
    std::vector<int>              m_jsHeadFree;
    std::vector<char>             m_jsNeckLift;           ///< Per bone: the neck chain the solve has for THE FACE LIFT this tick (the righting leaves those)
    int                           m_jsHeadBase = -1;
    glm::mat3                     m_jsHeadIntrinsic{1.0f}; ///< The drag-start righting, in the chest's frame (see rightIdleHead).
    float                         m_jsHeadIntrinsicHang = 0.0f; ///< How far the righting was faded at the press (0-1): that share of the intrinsic relaxes with the rise.
    float                         m_jsTrunkHomeShare = 0.0f;    ///< THE TRUNK COMES UP WITH THE RISE: how far the spine's references have turned home (ikPostureModel; kTrunkHomeStep).
    bool                          m_jsHandsPlanted = false;     ///< This tick: a live, non-shadow wrist contact stands (ikTrunkPolicy) — the trunk waits for it to go before it comes up.
    bool                          m_jsHeadFound = false;
    /// How far the pelvis's pitch is an unknown of this trunk drag (solveIk: the bow), 0-1.
    float                         m_jsTrunkHinge = 0.0f;
    std::vector<int>              m_jsSpineChain;         ///< -1, then the spine's bones from the root's spine child to the chest junction (lazily; empty = not searched)
    int                           m_jsSpineChainChest = -1; ///< ... the chest junction the chain ENDED at, or -1 when it ran out first (ensureSpineChain)
    std::vector<glm::vec3>        m_jsStartTranslation;   ///< Drag-start pose translations.
    /// Per bone: the pin target the solve is USING — the rig's, approached at a capped pace
    /// whenever the rig moves one (ground healing, a heel lift, a landing).
    std::vector<glm::vec3>        m_jsPinTarget;
    std::vector<char>             m_jsPinTargetValid;
    bool                          m_jsPinsEasing = false; ///< A pin target is still en route.
    bool                          m_jsPostStepMoving = false; ///< The last tick's post-steps (the arms' hang, the head's righting) turned something: the settle goes on (settleIkSolveTick).
    /// The release SETTLE is the drag continued under a still cursor (settleIkSolveTick): the
    /// followed target the last drag tick solved to, and the flag that keeps the rig from
    /// reading new intents or beginning new steps meanwhile.
    glm::vec3                     m_jsSettleTarget{0.0f};
    bool                          m_jsSettling = false;
    /// Per bone: the BALL of the foot under a standing contact pin at that bone (-1 = none,
    /// -2 = not looked for yet this drag), and the ball's offset from the ankle when the pin
    /// was first seen — the foot contact model of solveIk.
    std::vector<int>              m_jsBall;
    std::vector<std::vector<int>> m_jsToeMates;  ///< Per pin node: the ball's sibling toe bases on a rig whose toes fan from the mid-foot (unknowns, held flat like the ball).
    std::vector<glm::vec3>        m_jsBallOffset;
    /// The ball's own eased target (see solveIk): it starts where the ball is and approaches the
    /// pin target + m_jsBallOffset at the pins' pace. The offset is relative to the pin's PLANTED
    /// target with the ball at ITS OWN floor height — a drag that begins with the heels lifted
    /// (a deep crouch, a kneel) heals the ankle's pin down to standing height, and a ball
    /// carried rigidly below the ankle was driven that far under the floor.
    std::vector<glm::vec3>        m_jsBallTarget;
    glm::vec3                     m_jsKneeUnderEased = glm::vec3(0.0f); ///< a dragged knee's foot target (kneeHangUnder), approached at the pins' pace
    char                          m_jsKneeUnderValid = 0;
    float                         m_jsLyingHangY = 0.0f;   ///< THE LYING FOOT'S HANG HEIGHT, eased at the pins' pace (ikPinRowsEnd; a drawn-up knee's lying foot)
    char                          m_jsLyingHangYValid = 0;
    std::vector<float>            m_jsFootAllow;          ///< a letting-go foot's joints' depth through the floor at the release's first tick (m): forgiven by its floor rows
    char                          m_jsFootAllowValid = 0;
    /// The ankle's offset from the ball as the drag found the foot (see the heel row in solveIk).
    std::vector<glm::vec3>        m_jsHeelOffset;
    std::vector<char>             m_jsBallTargetValid;
    /// A KNEELING FOOT LIES FLAT (kKneelFlat* in the tuning header): per foot pin node, the eased
    /// share of "this foot lies on its top behind a planted knee" (persists across drags: a kneel
    /// let go of is still a kneel), the ankle's pitch channel that turns it and the eased plantar
    /// reference for it, the lying foot's ball and heel offsets (the standing model's twins), the
    /// knee it kneels on, and the drag-local flags (seen this drag, was flat this drag, flat at
    /// the press).
    std::vector<float>            m_jsFootFlat;
    std::vector<int>              m_jsFootFlatAxis;   ///< -2 = not looked for yet; -1 = none
    std::vector<float>            m_jsFootFlatLimit;  ///< the plantar limit of that channel (deg)
    std::vector<float>            m_jsFootFlatTuck;   ///< ... and its other limit: the tucked foot's (the unroll's goal)
    std::vector<glm::vec3>        m_jsFootFlatRef;    ///< the eased lying references of all three channels: the pitch's toward its plantar limit, the others' toward 0 (in line with the shin)
    std::vector<char>             m_jsFootFlatRefValid;
    std::vector<glm::vec3>        m_jsFlatBallOffset;
    std::vector<glm::vec3>        m_jsFlatHeelOffset;
    std::vector<int>              m_jsFootKnee;
    std::vector<float>            m_jsFlatLyingY;     ///< the lying ankle's height: the knee's planted height (the shin's radius)
    std::vector<float>            m_jsFlatRollStartY; ///< the ankle's height when the roll began (kept through its first half: the foot rolls in the air)
    std::vector<float>            m_jsFlatRollStartRef; ///< ... and the pitch channel's value then (the roll's progress is measured from it)
    /// ... and the TOES' and mid-foot's lying references (per bone of the ankle-to-ball chain below
    /// the foot bone): straight, eased from where they stand at the same pace, at the flat
    /// stiffness over the foot's share — so the foot rolls as one lever and lies straight.
    std::vector<glm::vec3>        m_jsToeFlatRef;
    std::vector<char>             m_jsToeFlatValid;
    std::vector<float>            m_jsToeFlatShare;
    std::vector<char>             m_jsFootFlatSeen;
    std::vector<char>             m_jsFootWasFlat;
    std::vector<char>             m_jsFootFlatAtStart;
    std::vector<float>            m_jsFootUnrollProgress; ///< how far a lying foot has rolled back onto its toes (0 lying, 1 handed over or not a lying foot): the knee's ceiling above it
    std::vector<char>             m_jsKneeHeldDown;       ///< per bone, this tick: a knee held down by its ceiling (kneeBelowHips) — its lying foot stays lying
    std::vector<char>             m_jsKneeHeldDownPrev;   ///< ... and last tick's (the feet's rows come before the knees' in the pin loop)
    std::vector<float>            m_jsKneeHold;           ///< Per knee the hips were LOW over at the press: its hold's share, a function of the target's rise (ikPinRows; kept through the settle).
    std::vector<float>            m_jsKneeUnrollBaseY;    ///< Per lying foot: the height its knee was planted at, for the unroll's ceiling (ikPinRowsEnd) ...
    std::vector<char>             m_jsKneeUnrollBaseValid; ///< ... and whether it has one.
    std::vector<float>            m_jsKneeUnrollExtra;    ///< ... and the extra lift the ceiling has gained since the handover (kKneeUnrollExtra).
    std::vector<char>             m_jsFootUnrolled;   ///< the foot rolled back to standing this drag: its posture references are the standing ones from then
    std::vector<char>             m_jsBallLanded;     ///< ... and its ball's target has met its spot since the handover: the gentle landing pace is over (ikPinRows)
    std::vector<float>            m_jsFlatRiseBase;   ///< the rise's progress at that handover: the heel's and the soles' turn to standing is measured from it
    /// The balance requirement's RAMP (JointSolver::Problem::balanceSlack): how far outside
    /// the support polygon the centre of mass may still sit. Set to the current distance
    /// whenever the support changes (the drag's start, a foot lifted, a contact planted) and
    /// taken back a centimetre a tick, so the body shifts over its support instead of jumping.
    float                         m_jsBalanceSlack = 0.0f;
    float                         m_jsHullKey = -1.0f;    ///< The support polygon it was set for.
    std::vector<glm::vec2>        m_jsLastSupport;        ///< The solve's support polygon, last tick (whole).
    std::vector<glm::vec2>        m_jsGrowFrom;           ///< The polygon a grown support is growing out of,
    float                         m_jsGrown = 0.0f;       ///< ... and how far it has grown, metres.
    float                         m_jsBalanceBase = -1.0f; ///< The drag-start imbalance: never asked back.
    /// THE WEIGHT SHIFT (ikPostureModel): a foot lifted off a two-footed stance takes the hips over
    /// the STANDING foot. From the centre of mass as the drag found it to the standing foot's
    /// footprint centre (model xz); zero when the drag did not begin on exactly two planted feet
    /// with one of them liftable by the drag (beginIkDrag).
    glm::vec2                     m_jsWeightShift{0.0f};
    float                         m_jsWeightLiftStart = -1.0f; ///< The lift share the drag BEGAN with (-1: not read yet): a foot already in the air shifts nothing.
    float                         m_jsWeightLift = 0.0f;       ///< The shift's share applied this tick (0-1): the traces, the harness.
    glm::vec2                     m_jsWeightEased{0.0f};       ///< The shift the root's home carries this tick, approached at the pins' pace (kPinEaseStep).
    bool                          m_jsWeightEasing = false;    ///< ... and still on its way (the settle waits for it, as for a pin).
    /// THE STRIDE TURNS THE HIPS (ikPosturePolicy): the stance's fore-aft split as the drag FOUND it
    /// (the left foot ahead of the right, metres; read at the first tick), the yaw's baseline — a pose
    /// found striding is taken to carry its stance's turn, as the arm swing's base is. Unread below -1e8.
    float                         m_jsStrideSplitStart = -1.0e9f;
    glm::vec3                     m_jsHold{0.0f};         ///< The released joint's hold (settle).
    /// The damped follower's state (see ikDamping): the target the solve is given and
    /// its velocity per second; seeded at the first tick of a drag from the raw target.
    glm::vec3                     m_jsFollowPos{0.0f};
    glm::vec3                     m_jsFollowVel{0.0f};
    bool                          m_jsFollowValid = false;
    // THE GROUP's drag (beginIkDrag / updateGroupTargets): each secondary effector's position at
    // the press and its target this tick (model space; index-aligned with the rig's
    // secondaryEffectors()), the primary's grab point at the press, the raw cursor of the last
    // drag tick, and the group's own damped follower on it — the primary's twin, so the
    // secondaries trail the cursor exactly as the primary does.
    std::vector<glm::vec3>        m_jsGroupStartPos;
    std::vector<glm::vec3>        m_jsGroupTargets;
    std::vector<char>             m_jsGroupTargetValid;
    glm::vec3                     m_jsGroupCursorStart{0.0f};
    glm::vec3                     m_jsGroupCursorRaw{0.0f};
    bool                          m_jsGroupCursorValid = false;
    glm::vec3                     m_jsGroupFollowPos{0.0f};
    glm::vec3                     m_jsGroupFollowVel{0.0f};
    bool                          m_jsGroupFollowValid = false;
    /// The drag's first target (the grab point) and the pelvis's VERTICAL YIELD derived from how
    /// far, and how vertically, the target has gone below it — or above it, when the drag began
    /// with height left in the legs (0-1; see kRootYieldStiffness).
    glm::vec3                     m_jsStartTarget{0.0f};
    /// True for a drag that BEGAN with the pelvis resting on the floor (IkRig::floorSeat as the
    /// rig found the pose): the seat rocks and ROLLS under a chest or head drag (solveIk).
    bool                          m_jsFloorSeatStart = false;
    /// The kneeling seat's height (IkRig::kneelSeatHeight) as the drag found it, for a drag that
    /// began on her knees: the trunk's rise is measured about it (dragIkTick). Negative otherwise.
    float                         m_jsKneelSeatY = -1.0f;
    glm::vec3                     m_jsKneelSeat = glm::vec3(0.0f, -1.0f, 0.0f); ///< ... and the seat POINT (IkRig::kneelSeat), a constant of the drag
    int                           m_jsSeatNode = -1; ///< ... and the joint she sat on (IkRig::floorSeatNode)
    /// True for a drag that BEGAN ON HER KNEES (IkRig::onKnees as the rig found the pose): the
    /// trunk folds at the hips over them under a chest or head drag, as it does standing (solveIk).
    bool                          m_jsKneelStart = false;
    /// A HAND ON THE FLOOR holds its place at its FINGERTIP (solveIk): per bone, a hand's longest
    /// rider by bind distance (-1: not a hand; found once per rig), where that tip is held, and
    /// whether it is being held.
    mutable std::vector<int>      m_jsHandTip;    ///< per WRIST (see m_jsWristOf; mutable: found lazily, read by the const handPalmNormal)
    /// Per bone: the WRIST of the hand it is a part of (the wrist itself included), -1 elsewhere.
    /// Found by structure (ensureHandMaps): below an elbow, down the main children past the
    /// forearm's twist bones, the first bone with two free channels. The generations cut a hand
    /// differently - on some the CARPALS are real joints and the fingers ride them, and a hand
    /// lands on those: whichever joint of it touches, it is ONE hand, held once.
    mutable std::vector<int>      m_jsWristOf;
    void                          ensureHandMaps() const;
    std::vector<glm::vec3>        m_jsHandTipTarget;
    std::vector<char>             m_jsHandTipValid;
    std::vector<glm::vec3>        m_jsHandHeading; ///< ... and the way its fingers point along the floor, as it landed
    std::vector<glm::mat3>        m_jsPalmFrom;    ///< ... the hand's rotation as it landed, and how far the palm's
    std::vector<float>            m_jsPalmEase;    ///<     target has turned from it toward flat (0-1)
    /// Per bone, ACROSS drags: a floor contact that formed by itself on an arm that hung (the
    /// hang's arm came down with the body) — not one the user placed. Cleared for an arm when a
    /// joint of it is grabbed. IkRig::setIncidentalContacts has what it is for.
    std::vector<char>             m_incidentalContact;
    /// Per bone: how far GONE a live contact's hold was last tick (0..1; solveIk's fade, eased).
    std::vector<float>            m_jsContactGone;
    /// Per bone: the body's rise term (m_jsRootRise / m_jsContactRise) when the live contact FORMED,
    /// -1 while the bone is no live contact. The rise unloads a contact by the rise SINCE it formed:
    /// a hand that touches down under a body that has already risen (2026-10-27: a prone-up's right
    /// hand, left a centimetre off the floor by the hand drag before it, landed at tick 84 of the
    /// hips' rise) was planted and unloaded in the same tick — a pin that held nothing, bringing
    /// the arm into the solve with the palm's rows, and the arm flipped basins, 219mm at a thumb.
    /// (IK_JS_RISE_UNLOAD_ABSOLUTE: the drag's whole rise, as before.)
    std::vector<float>            m_jsContactRiseBase;
    /// Per node, this tick: a live contact whose hold the solve has faded out entirely (the rise, a
    /// kneel-up). Its pin stands until the rig's lift-off, but it holds nothing — and its arm is the
    /// hang's (hangArmHeld), not a rider's: unloaded under a kneel-up the hands rode the rising
    /// trunk forward-down and lay on the floor ahead of her, arms straight, until the pins went.
    // Per-tick BUFFERS, kept across ticks for their capacity (each assigned before it is read):
    std::vector<glm::mat3>        m_jsRotationsBuf;   ///< syncRigRotations: every bone's world rotation
    std::vector<glm::vec3>        m_jsPositionsBuf;   ///< dragIkTick: every joint's position
    std::vector<char>             m_jsHeldBeforeBuf;  ///< dragIkTick: per hang arm, held before the contacts updated
    std::vector<char>             m_jsDeferredBuf;    ///< dragIkTick: per node, its lift-off deferred (IkRig::setLiftOffDeferred)
    std::vector<char>             m_jsHangHeld;       ///< hangIdleArms: per hang arm, hangArmHeld() cached through the parts ...
    bool                          m_jsHangHeldCached = false; ///< ... while this is set (the inputs do not change meanwhile)
    std::vector<glm::vec3>        m_jsHeadNudgeBuf;   ///< rightIdleHead: the head chain's Euler with one channel nudged (its Jacobian by differences)
    mutable std::vector<char>     m_fkInSubBuf;       ///< fkVolumeDepth: the rotated bone's subtree mask
    std::vector<char>             m_jsContactUnloaded;
    /// Per node, this tick: a HAND contact the body's RISE has unloaded because its arm has no length
    /// to give (ikPinRows: !armSlack under m_jsRootRise / m_jsContactRise at 1). Handed to the rig
    /// (IkRig::setRiseUnloadedContacts), which lets such a hand lift off like a taut one — the solve
    /// reads the arm's own stretch as well as the junction's reach, the rig's lift-off only the reach,
    /// and a straight arm on a shrugged collar fell between the two: weightless on the floor, then
    /// 17cm in the air with its pin standing, and swung 144mm in a tick by the torso's volume rows.
    std::vector<char>             m_jsContactRiseUnloaded;
    /// Per node: a hand contact whose ARM is being re-based where the rise's unload finds it (the
    /// fade block of ikPinRows, rebaseContactArm: every tick of the unload); cleared when the node
    /// is no live contact. Without it the fading hold let the arm snap back to its drag-start fold.
    std::vector<char>             m_jsContactRebased;
    /// Per node: a hand contact's unload by the rise, EASED in time (kHandUnloadStep a tick; the fade
    /// block of ikPinRows). The ceiling rises with it and the rig's lift-off flags read it.
    std::vector<float>            m_jsHandUnload;
    /// Per bone: a hard live contact (a knee over its planted foot) that is still COMING DOWN — it
    /// is re-seated where it lands (dragIkTick).
    std::vector<char>             m_jsContactLanding;
    /// The pelvis's tilt price scale on planted knees, as the last tick had it (eased: solveIk).
    double                        m_jsKneelTilt = 1.0;
    /// True while the seat ROLLS (solveIk: a floor sit's chest or head drag): the idle arms keep
    /// off the floor instead of landing on it (hangIdleArms).
    bool                          m_jsSeatRolls = false;
    float                         m_jsRootYield = 0.0f;
    /// REACHING DOWN (ikTrunkPolicy): how far, and how vertically, a dragged HAND's target has gone
    /// below where the drag began (the push-down's yieldFor, 0-1) — the hinge share of a hand
    /// pushed down, and what scales the hips' vertical yield under it (kReachSquatShare).
    float                         m_jsReachDown = 0.0f;
    float                         m_jsReachHinge = 0.0f; ///< ... applied: a hand drag from a standing start, not suspended, no seat.
    float                         m_jsReachTravel = 0.0f; ///< ... and the hand's downward travel itself (m), which sizes the fold.
    /// The target's DOWNWARD travel since the drag began, as a share (kHandCeilingDownFrom..Full):
    /// a body coming down stands the slack hands' ceilings down (solveIk).
    float                         m_jsTargetDown = 0.0f;
    /// The RISING part of it alone (0-1): it also yields the pelvis's HORIZONTAL price — a body
    /// getting up out of a kneel or a sit must carry its hips back over its feet.
    float                         m_jsRootRise = 0.0f;
    /// ... and the body's rise over the LOWEST its target has been in THIS drag (m_jsLowestTargetY;
    /// the same reductions as m_jsRootRise, no room factor): what UNLOADS the live contacts a
    /// drag made on its way down when it comes back up. Held by their own rise alone, a figure
    /// taken down onto all fours and back up in one drag kept both hands on the floor - the trunk
    /// folding at the hips to leave them there - on the five rigs whose arms could reach.
    float                         m_jsContactRise = 0.0f;
    float                         m_jsProneLie = 0.0f;     ///< THE PRONE: a kneel-start pelvis drag's target ahead of a planted knee and down toward the belly's lying height (ikChooseUnknowns; a function of the target, kept through the settle)
    float                         m_jsProneShare = 0.0f;   ///< a pelvis drag's hips AHEAD of a planted knee (solveIk): the pelvis's tilt price and the feet's rows fade with it
    float                         m_jsKneelUpRise = 0.0f;  ///< a kneel-start TRUNK drag's upward travel (smoothstep 5-20cm): it unloads the HAND contacts, whose arms cannot always make the rise that would (dragIkTick)
    float                         m_jsKneelHold = 0.0f;    ///< a kneel-start TRUNK drag's hold of the knees DOWN (a ceiling): 1 while the trunk can give what is asked, or for the whole drag when it began folded (dragIkTick)
    glm::vec3                     m_jsKneelSeatShift = glm::vec3(0.0f); ///< a kneel-up's hips' home over where the drag found them: to the kneeling seat over the planted knees, by the target's uprightness (dragIkTick)
    float                         m_jsKneelUpright = 0.0f; ///< ... and the uprightness that shift is by (0-1): the horizontal price yields to it
    /// THE TIPTOE (dragIkTick, ikPostureModel, ikPinRows): a hand pulled UP beyond the arm's reach
    /// lifts the heels — the body rises onto the balls of the feet, up to what kTiptoeDeg of
    /// plantar flexion about the ball gives, before the lift-off takes her off the floor.
    bool                          m_jsTiptoeDrag = false;  ///< The drag can (beginIkDrag): a limb's joint dragged from a stance of exactly two planted feet — no live contact, no user pin, not on her knees or her seat, not suspended.
    IkScope                       m_ikScope = IkScope::Body; ///< The drag in flight's scope (beginIkDrag); Body between drags.
    float                         m_jsTiptoeRoom = -1.0f;  ///< The heel lift the ankles give (model m): -1 until the balls are known (the first solve finds them), 0 = none.
    float                         m_jsTiptoeD = 0.0f;      ///< The feet at bind: the ankle's horizontal distance behind the ball and its height over it (means over the standing feet).
    float                         m_jsTiptoeH0 = 0.0f;
    float                         m_jsTiptoe = 0.0f;       ///< This tick's heel lift (model m): the root's home rises by it, each heel's target with it. A function of the target.
    float                         m_jsTiptoeTheta = 0.0f;  ///< ... and the pitch about the ball that gives it (rad).
    float                         m_jsKneelYield = 0.0f;   ///< a kneel-start drag's push DOWN, by its downward travel alone (no share gate): both pelvis prices yield to it under a trunk drag
    float                         m_jsLowestTargetY = 0.0f;
    /// How far along the rise is (0-1): the height regained over the height the legs had left.
    float                         m_jsRiseProgress = 0.0f;
    /// The solve root's Euler pose with its drag-start HEADING and no pitch or roll: what its
    /// rotation's posture reference turns to as the body rises (see rootTurns in solveIk).
    glm::vec3                     m_jsRootUprightEuler{0.0f};
    /// The root's heading (its yaw about the vertical, radians) and the solved effector's
    /// position from the root, both as the drag began: the hips are drawn under a trunk joint
    /// that is pulled up out of a low pose (see trunkRise in solveIk).
    float                         m_jsRootHeading = 0.0f;
    glm::vec3                     m_jsStartGrabFromRoot{0.0f};
    /// The solved effector's world rotation and position as the drag began: a dragged FOOT's sole
    /// is held to it while the foot is on the floor (see the slide in solveIk).
    std::vector<glm::vec3>        m_jsStartPos;  ///< every joint's model-space position as the drag began
    std::vector<glm::mat3>        m_jsStartRot;  ///< ... and its world rotation
    bool                          m_jsTrunkInSolve = false; ///< a pelvis drag's spine chain, once among the unknowns
    /// How far a dragged trunk joint is taking the HIPS across the floor (model xz; see solveIk's
    /// trunk follow), and whether this drag has ever been suspended (a lifted body's travel is
    /// not a walk's: no follow after it).
    glm::vec2                     m_jsTrunkFollow{0.0f};
    glm::vec2                     m_jsUnfoldShift = glm::vec2(0.0f); ///< ... and the hips' home under a trunk that comes MORE UPRIGHT than the drag found it (x/z; solveIk)
    float                         m_jsTrunkCounter = 0.0f; ///< how far the pelvis's horizontal price yields to a lean that has run out of balance room (a stance that cannot walk)
    float                         m_jsTrunkWalk = 0.0f; ///< how far the stance can WALK after a followed trunk drag (0: it keeps its balance)
    bool                          m_jsWasSuspended = false;
    float                         m_jsLeanMoment = -1.0f; ///< the upper body's mass moment over the hips / the body's mass (per drag, lazily)
    glm::vec2                     m_jsStartCom{0.0f};     ///< the centre of mass at the press (model xz), with it
    std::vector<int>              m_jsStartStanceNodes;   ///< ... and the LIVE contacts the lean found when first measured: the lean's room is the stance's and theirs, never a contact the drag itself makes (solveIk)
    int                           m_jsLimbSocket = -1;     ///< a dragged LIMB's socket joint and its reach from it (the rise: dragIkTick); -1 = not yet found
    float                         m_jsLimbReach = 0.0f;
    bool                          m_jsLiftEasing = false;  ///< a lift is being eased in (dragIkTick)
    glm::vec3                     m_jsLiftDeficit{0.0f};   ///< ... what of the lift's first miss is still owed
    glm::vec3                     m_jsHangDeficit{0.0f};   ///< ... and how far the root still is from hanging under the hand
    bool                          m_jsWasSwinging = false; ///< a step was in flight at the last solve (the balance ramp's key)
    /// The hip hinge's release slack once a pelvis drag has stepped (see solveIk's pelvisBalance).
    float                         m_jsHingeRelease = 0.0f;
    glm::vec3                     m_jsPrevGoal{0.0f}; ///< the last tick's solve target (the continuation walks its MOVE)
    glm::vec3                     m_jsSolveGoal{0.0f}; ///< the target this tick's solve worked to (a knee's, an elbow's: on their reach)
    bool                          m_jsPrevGoalValid = false;
    glm::mat3                     m_jsEffectorStartRot{1.0f};
    glm::vec3                     m_jsEffectorStartPos{0.0f};
    float                         m_jsHandSlideEase = 0.0f;   ///< A dragged hand's palm-flat hold, eased in from the rotation the drag found it at (solveIk).
    /// A dragged hand LAID ON THE BODY (ikCursorRows): its palm's hold against the body volume its
    /// target rests on, eased from the rotation the hand had when the target came within the
    /// lay's band (re-captured whenever it leaves and returns).
    glm::mat3                     m_jsHandLayFrom{1.0f};
    float                         m_jsHandLayEase = 0.0f;
    int                           m_jsAkimboBone = -1;    ///< AKIMBO (the hand lay): the shoulder whose abduction channel is referred OUT while a hand lies on the hip
    int                           m_jsAkimboAxis = 0;
    float                         m_jsAkimboRefDeg = 0.0f; ///< the channel value that stands the upper arm kHandLayElbowOutDeg out
    float                         m_jsAkimboShare = 0.0f;  ///< the lay x the surface's lateral share x the hips' band x the ease (0 = off)
    float                         m_jsAkimboEase = 0.0f;   ///< the time ease (kHandLayElbowEaseStep a tick, decaying when the lay lets go)
    float                         m_jsDangle = 0.0f;       ///< THE DANGLE: how far the suspended body has gone over to hanging (kDangleStep a tick while suspended, back on landing)
    std::vector<glm::vec3>        m_jsDangleRef;           ///< per bone, the hanging references (the thighs plumb, the knees soft, the feet pointed) — ikPostureDofs
    std::vector<unsigned char>    m_jsDangleValid;         ///< per bone, a bit per channel that has one
    bool                          m_jsHandLayValid = false;
    /// ... and which SIDE of the hand lies on it: +1 the palm, -1 its BACK — whichever needed the
    /// least turn where the target entered the band (a hand behind the back rests on its back: the
    /// forearm cannot supinate that far, and palm-in it flexed the wrist to its limit to comply).
    float                         m_jsHandLaySide = 1.0f;
    /// The drag target BEFORE the drag clamp pushed it out of the body volumes (model space; dragIkTo):
    /// a cursor inside the body is the lay's surest sign that the hand is on it, however far the
    /// clamp holds the wrist off (the riders' clearance keeps a fingers-first hand a hand's length out).
    glm::vec3                     m_jsRawTarget{0.0f};
    bool                          m_jsRawTargetValid = false;
    /// A sliding foot's SOLE hold, eased from the rotation the slide FOUND the foot at (the tick
    /// the target came within the slide's band) toward its flat target at kSoleTurnStepDeg a tick
    /// (ikCursorRows); a foot carried on a taut leg rides its shin 50 degrees from flat, and the
    /// hold, coming in over the foot's weak posture, flipped it in two ticks (85mm at a toe).
    glm::mat3                     m_jsSoleEaseFrom{1.0f};
    float                         m_jsSoleEase = 1.0f;
    bool                          m_jsSoleEaseValid = false;
    /// The height the legs had left to give when the drag began (model metres): the solve
    /// root's standing height over the floor less its height then. ~0 standing, the crouch's
    /// depth crouched, negative hovering.
    float                         m_jsRootRiseRoom = 0.0f;
    bool                          m_jsHoldValid = false;
    // --- THE LANDING BOUNCE (armaturelanding.cpp) ---
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
    };
    LandingBounce                 m_landing;
    // --- The bone classes and THE DIGIT DRAG (armatureikdigit.cpp) ---
    /// Per bone: its BoneClass (classifyBones, with the rig: empty until then).
    std::vector<char>             m_boneClass;
    /// Per bone: the limb end a Carpal hangs below — its wrist or its ankle (classifyBones), else -1.
    std::vector<int>              m_boneLimbEnd;
    void classifyBones();
    /// The digit drag in flight: the grabbed bone and the grabbed point in its frame (the joint
    /// itself without a grab point), and the bones whose channels answer the cursor — the digit's,
    /// parents first: the grabbed bone (when the point is off its joint: its own channels turn it)
    /// and the digit's bones above it, up to where the digit joins the hand or the foot.
    bool                          m_digitDrag = false;
    int                           m_digitBone = -1;
    glm::vec3                     m_digitLocal{0.0f};
    std::vector<int>              m_digitChain;
    bool beginDigitDrag();
    bool dragDigitTo(const glm::vec3& targetWorld);
    /// The unknowns a LOCAL solve (the digit drag, the figure move's pinned limbs) has in @p bone:
    /// its unlocked channels, each pulled toward its drag-start value at 1 / range^2 (the limb's
    /// price), a fold channel bounded at straight — the body solve's own posture model
    /// (ikPostureModel), for the few bones there are. Appended to @p out.
    void localChannelDofs(int bone, std::vector<JointSolverDof>& out) const;
    // --- THE FIGURE MOVE (armatureikfigure.cpp; IkScope::Figure) ---
    /// True when a drag that has hold of @p effector (the rig's, after its promotions) has hold of
    /// the BODY itself: the solve root (a pelvis-girdle grab is promoted to it) or a bone of the
    /// spine — not a leg's, an arm's, the neck's or the head's.
    bool isBodyGrab(int effector) const;
    /// The move in flight: the grabbed bone and the grabbed point in its frame; the user pins it
    /// holds (the joint, its place and rotation at the press, model space) and the bones whose
    /// channels give for them; whether a pin on the trunk holds the whole figure where she is;
    /// and, per joint that rides the body, the height it may come down to (its floor clearance —
    /// or where it stood at the press, when that was lower: a pose's own penetration is not pushed
    /// out by a move) — 1e9 for a joint of a pinned limb, which goes where its pin holds it.
    struct HeldPin {
        int       bone = -1;
        glm::vec3 place{0.0f};
        glm::mat3 rotation{1.0f};
    };
    bool                          m_figureMove = false;
    int                           m_moveBone = -1;
    glm::vec3                     m_moveLocal{0.0f};
    std::vector<HeldPin>          m_movePins;
    std::vector<int>              m_moveLimbs;
    bool                          m_moveHeld = false;
    std::vector<float>            m_moveFloor;
    bool beginFigureMove();
    bool dragFigureTo(const glm::vec3& targetWorld);
    /// The damped FOLLOWER (see ikDamping): advances the followed target one tick toward @p raw
    /// and returns it (m_jsFollowPos); a drag's first tick seeds it ON the target, and records
    /// the drag's first target (m_jsStartTarget).
    glm::vec3 followIkTarget(const glm::vec3& raw);
    /// THE GROUP's targets for this tick (dragIkTick): each secondary effector's position at the
    /// press carried by the cursor's displacement since — through the group's own follower —
    /// bounded by the floor and the body volumes as the primary's target is (dragIkTo).
    void updateGroupTargets();
    void buildJointSolver();
    /// One solve of the current drag's tasks, applied in full: toward @p dragTarget (a drag
    /// tick), or with the released joint held at @p holdTarget / free (the release). Returns
    /// whether the pose changed.
    bool solveIk(const glm::vec3* dragTarget, const glm::vec3* holdTarget);
    // The solve's STAGES, in the order solveIk runs them: each is a member function over the
    // tick's shared IkSolveScratch (armatureiksolvestate.h), and begins by naming the members it
    // reads and writes. armatureiksolve.cpp's file comment is the map.
    // --- armatureiksolvedrag.cpp — what the drag IS and which channels answer it ---
    /// The UNKNOWNS: which bones' channels the solve owns (the active paths, the pins, the toes), the
    /// foot contact model's balls, and whether the root rotates.
    void ikChooseUnknowns(IkSolveScratch& s);
    /// The TRUNK's policy for this tick: the trunk follow and the unfold, the hip sway, the hip hinge's
    /// balance, and whether the spine chain is among the unknowns.
    void ikTrunkPolicy(IkSolveScratch& s);
    /// The STIFFNESS CLASSES: which bones are the dragged limb, which serve a pin (and carry the limb
    /// twist price), a landed hand's arm, the pinned joints, the pelvis bone, and what goes home as the
    /// body rises.
    void ikStiffnessClasses(IkSolveScratch& s);
    /// A FOOT drag: the SLIDE (how much a dragged foot is on the floor), which is what reshapes the
    /// stance.
    void ikFootDrag(IkSolveScratch& s);
    /// A KNEE drag: the knee's goal over its planted foot, or the foot letting go as the knee lifts
    /// (`plant`), the lateral reach clamp and the goal's raise.
    void ikKneeDrag(IkSolveScratch& s);
    /// An ELBOW drag: the elbow's goal on the upper arm's reach, the hand that tends to stay, a pin
    /// below the elbow that holds.
    void ikElbowDrag(IkSolveScratch& s);
    // --- armatureiksolveposture.cpp — the posture model that prices them ---
    /// The POSTURE MODEL: every unknown's stiffness and reference (the trunk drag, the seat roll, the
    /// sagittal lock, the kneel tilt, the rise home, the root's home).
    void ikPostureModel(IkSolveScratch& s);
    void ensureSpineChain(); ///< The spine chain once per skeleton (m_jsSpineChain, m_jsSpineChainChest): read by ikChooseUnknowns and ikPosturePolicy
    void ikPosturePolicy(IkSolveScratch& s); ///< ... its policy: the thigh boxes, the spine chain, the weight lift, contrapposto, the trunk home, the seat roll, the sagittal lock, the kneel tilt
    void ikPostureDofs(IkSolveScratch& s);   ///< ... every channel priced and referred
    void ikPostureRoot(IkSolveScratch& s);   ///< ... the root's home and translation unknowns
    /// The spine's COUPLING rows (the neck as its top, the pelvis's hinge as its base) and the posture
    /// EASING.
    void ikSpineCoupling(IkSolveScratch& s);
    // --- armatureiksolverows.cpp — the rows the solver satisfies ---
    /// The PIN ROWS, part one: the per-drag state the pins' rows share (the eased pin targets, the palm
    /// and fingertip rows, the ceilings, the rise positions).
    void ikPinRowsBegin(IkSolveScratch& s);
    /// The PIN ROWS of ONE pin: a user pin's hold, a standing foot's contact model (ball, heel, sole,
    /// toes), a live contact's fading unilateral hold, a landed hand's palm and fingertip, the knee-
    /// drag foot's release.
    void ikPinRows(IkSolveScratch& s, std::size_t p);
    /// ... in parts over the pin's IkPinRowState (armatureiksolvestate.h), in this order:
    void ikPinRowFlatFoot(IkSolveScratch& s, IkPinRowState& st);    ///< A KNEELING FOOT LIES FLAT: the lying model's share, references, roll and unroll
    void ikPinRowLiveContact(IkSolveScratch& s, IkPinRowState& st); ///< a live contact's unilateral hold, the ceilings, the landed hand's palm, the fade
    void ikPinRowFootFree(IkSolveScratch& s, IkPinRowState& st);    ///< the prone release, and the place row pushed
    void ikPinRowFootModel(IkSolveScratch& s, IkPinRowState& st);   ///< a standing foot's rows: ball, heel, sole, toes
    void ikPinRowHold(IkSolveScratch& s, IkPinRowState& st);        ///< a user pin's hold; the seat's rock
    /// The heading a foot was found at (its lateral axis's turn about the vertical) and the standing
    /// ball offset turned to it: the flat-foot model's and the standing foot's readings.
    bool      footYawFound(std::size_t node, int ballBone, float& yaw) const;
    glm::vec3 standingBallOffset(std::size_t node, int ballBone) const;
    /// The PIN ROWS, part three: the rows that need every pin known — the letting-go foot's spring and
    /// hang, the elbow drag's hand hold, the knee's hanging foot.
    void ikPinRowsEnd(IkSolveScratch& s);
    /// The CURSOR's row and its company: the hips drawn under a rising trunk, the sway's level chest, a
    /// slid foot's sole, a hand slid along the floor, a hand laid on the body, a suspended body's hang.
    void ikCursorRows(IkSolveScratch& s);
    void ikHandSlideRows(IkSolveScratch& s); ///< ... its part: a dragged hand at the floor's height lies on its palm
    void ikHandLayRows(IkSolveScratch& s);   ///< ... its part: a dragged hand resting against a body volume lies on its palm, or its back
    /// The BALANCE row: the support polygon (grown into, a stepping foot kept in it), the slack ramp,
    /// the hinge's and the follow's release, the seat that keeps no balance.
    void ikBalanceRow(IkSolveScratch& s);
    /// The FLOOR's and the BODY VOLUMES' one-sided rows: the exemptions, a letting-go foot's scale and
    /// allowance, the ceilings, and the plane source the solver regenerates at every linearization.
    void ikPlaneRows(IkSolveScratch& s);
    // --- armatureiksolve.cpp — the solve and the pose applied ---
    /// The SOLVE: the pose as it stands, the settings, and the continuation over a cursor jump.
    void ikSolve(IkSolveScratch& s);
    /// APPLY the solved pose in full: every channel and the root's translation, then the skin matrices.
    void ikApply(IkSolveScratch& s);
    /// The solve's TRACES (POSESTUDIO_IK_TRACE, IK_JS_JACOBIAN_CHECK, IK_JS_FAR_ROWS, IK_JS_COST_TRACE,
    /// IK_JS_POSE_TRACE): probes only, no effect on the pose.
    void ikSolveTraces(IkSolveScratch& s);
    /// A dragged knee's hanging foot: where it hangs from the knee's goal, and its eased target.
    glm::vec3 ikKneeHangOffset(const IkSolveScratch& s) const;
    glm::vec3 ikKneeHangUnder(IkSolveScratch& s);
    /// A drag tick toward the clamped model-space target: the damped follower, the rig's intent,
    /// the solve, then live contacts / landings and the step policy on the solved pose.
    bool dragIkTick(const glm::vec3& rawTarget);
    /// Hands the rig the bones' world rotations (IkRig::setBoneRotations: the floor clearances).
    void syncRigRotations();
    /// IDLE ARMS HANG (armatureikpost.cpp): after a tick's solve, turns the shoulder of every arm
    /// that hung at the press — and that nothing has hold of — so it keeps hanging under the trunk
    /// as it now is. True when it moved one.
    bool hangIdleArms();
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
    /// ... IDLE ARMS HANG in parts (armatureikpost.cpp), in the order hangIdleArms calls them:
    void               findHangArms();                                          ///< the once-per-drag search for the arms that hang
    std::vector<float> hangArmSwings() const;                                   ///< THE STRIDE SWING per arm this tick
    bool               softenHangElbows(const std::vector<float>& swings);      ///< the elbow softens near the floor, bends with the swing
    bool               relaxLiftedWrists();                                     ///< a lifted hand's wrist eases to neutral
    bool               relaxIdleFingers();                                      ///< IDLE HANDS RELAX; a hand coming down opens
    void               findHangTurns(const std::vector<float>& swings, std::vector<HangTurn>& turns);       ///< each arm's wanted direction and turn
    void               stopHangTurns(std::vector<HangTurn>& turns);             ///< THE STOP: at the volumes and the floor, order-independent, outward where blocked
    bool               applyHangTurns(const std::vector<HangTurn>& turns);      ///< the turns applied; the carried swing remembered
    bool               hangStanceSplit(float armSide, const glm::vec3& fore, float& split) const; ///< the stance's fore-aft split for an arm's side
    float              hangSwingDegOf(float split) const;                       ///< the swing a split gives (degrees, + forward)
    bool               hangHandLifted(const HangArm& arm) const;                ///< no pin but a shadow stands in the arm
    bool hangTurnFor(const HangArm& arm, const glm::vec3& wanted, HangTurn& turn, float stepDeg = -1.0f) const; // (stepDeg <= 0: the hang's pace, kArmHangStepDeg; a large value = the whole turn) ///< the least swing that points the arm at `wanted`
    float              hangTurnDepth(const HangTurn& t) const;                  ///< how deep the arm stands in the volumes / under the floor
    float              hangTurnFloorDepth(const HangTurn& t) const;             ///< ... under the floor alone (negative = clear by that much)
    void               hangTurnTo(const HangTurn& t, float fraction);           ///< the shoulder turned that far along the turn
    static float       hangTurnAllow(const HangTurn& t);                        ///< how deep a turn may leave its arm
    void               hangStopFraction(HangTurn& t);                           ///< one turn stopped at the volumes and the floor
    /// True when a pin or a floor contact of the drag sits in the arm: the solve has it, not the hang.
    bool hangArmHeld(const HangArm& arm) const;
    /// THE HEAD STAYS UP (armatureikpost.cpp): after a tick's solve, turns the neck and the head —
    /// when nothing has hold of them — so the head keeps most of its uprightness as the trunk
    /// tilts. True when it moved them.
    bool rightIdleHead();
    /// True while a post-step (the arms' hang, the head's righting) has the pose away from where
    /// the limbs would ride: balancePositions() then differs from the pose's joints.
    bool balanceIsBlind() const;
    /// The joint positions BALANCE reads: the pose's, with every hung arm and the righted head put
    /// back where they would RIDE (their joints at the rotations the drag found). Balance is blind
    /// to both — see hangIdleArms.
    std::vector<glm::vec3> balancePositions() const;
    bool settleIkSolveTick();
};

} // namespace pose

#endif // ARMATURE_H
