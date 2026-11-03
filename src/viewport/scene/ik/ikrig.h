/**
 * @file ikrig.h
 * @brief The full-body-IK RIG: everything about an IK drag that is POLICY rather than solving —
 *        what is planted and where its pin sits, which joint a grab really drives, when a foot
 *        steps, when the figure lifts off and lands, which joints come down onto the floor, the
 *        body's mass model and support polygon, and the self-collision volumes.
 *
 * Built once per figure from its bind skeleton (the body tree rooted at the PELVIS — rootNode(),
 * the first multi-child descendant of the anatomical root; real figures root at an origin-level
 * figure node whose chain of virtual ancestors is excluded from the rig wholesale: no graph
 * edge, no contact eligibility, no balance mass — the segment masses, the floor clearances, the
 * mesh-fitted body volumes).
 *
 * Per drag: beginDrag() detects which joints are planted on the ground (minus contacts within a
 * limb's length of the effector, by tree path — dragging a foot must lift the foot AND its whole
 * toe subtree), pins each limb's most proximal planted contact at its ground-healed height with
 * a reach LEASH, promotes a token-mass grab (a finger, a toe, a face bone) to the limb's real
 * end joint, and builds the support polygon over all contacts. Each tick the Armature's solve
 * (scene/armatureiksolve.cpp, on scene/ik/jointsolver.h) reads pins(), activeNodes(), the
 * masses, the hull and the volumes, and the rig's per-tick policy runs around it:
 * updateIntent() (down / up intent, the live pins' bounds, the lift-off test) before the solve,
 * updateContacts() (live contacts, the heel-lift intent's pin heights, landings) and
 * updateStepPolicy() (balance- and anchor-driven foot re-planting) after it, all on the solved
 * pose. Positions in/out are model space. Qt-free (std + GLM).
 *
 * The implementation is split across six translation units by phase: ikrig.cpp (build, the
 * shared metric helpers), ikrigvolumes.cpp (the self-collision body volumes), ikrigdrag.cpp
 * (beginDrag / plantContacts), ikrigpolicy.cpp (the per-tick intents, the lift-off test, the step
 * policy's entry), ikrigstepping.cpp (the stepper) and ikrigcontacts.cpp (live contacts, landings,
 * pin-set maintenance); their shared tuning constants live in the private ikrig_constants.h.
 */

#ifndef IKRIG_H
#define IKRIG_H

#include "bodymesh.h"
#include "bodyvolume.h"
#include "rigroles.h"
#include "ikeffector.h"
#include "skeletongraph.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pose {

/// Bind-skeleton description of one bone, as the Armature hands it to IkRig::build().
struct IkRigBone {
    std::string name;
    int         parent = -1;         ///< Anatomical parent index (-1 = skeleton root).
    glm::vec3   bindPos{0.0f};       ///< Bind joint position (model space).
    bool        helper = false;      ///< A twist helper that splits a rigid segment (Armature::isTwistBone): no joint a body rests on — never a contact of its own (see IkRig::helperNode).
};

/**
 * @class IkRig
 * @brief Per-figure FBIK policy state: build once, then per interactive gesture beginDrag ->
 *        (per tick: updateIntent, the Armature's solve, updateContacts, updateStepPolicy) ->
 *        (mouse-up) landPendingStep -> endDrag.
 */
class IkRig {
public:
    /// Joints under this WORLD height are ground contacts when a drag begins (figure-scaled;
    /// the Armature's contact rule at the press) and re-plant when a lifted figure lands.
    static constexpr float kContactHeight = 0.15f;
    /// A FOOT-CLASS joint sits under this height AT BIND (figure-scaled): the ankle's 8cm, the
    /// toes — on the floor because the figure stands on it. Any other joint under the contact
    /// height is there because of the POSE (a kneeling knee, a hand on the floor): plantContacts.
    static constexpr float kFootBindHeight = 0.20f;
    /// The FLESH RADIUS (figure-scaled): the joint-centred clearance every body joint had before the
    /// floor's were fitted to the mesh (fitFloorClearances: never less than this), the body volumes'
    /// default joint clearance, and the floor of a bone's reach along a direction.
    static constexpr float kFleshRadius = 0.015f;
    /// A LIMB's junction this far above the pelvis joint (figure-scaled) is an arm's or the head's:
    /// a leg hangs from the pelvis itself (limbHangsAboveHips).
    static constexpr float kArmJunctionHeight = 0.10f;

    /// Builds the body tree, the segment masses and the clearances from the bind skeleton. With
    /// a body mesh sample (bodymesh.h) the self-collision volumes are fitted to the mesh
    /// (buildBodyVolumes); without one they are sized from the skeleton.
    void build(const std::vector<IkRigBone>& bones, const std::vector<BodyMeshPoint>* mesh = nullptr);
    bool built() const { return !m_parents.empty(); }

    /// Starts an interactive drag of @p effectorNode. @p positions = current model-space joint
    /// positions; @p contactNodes = joints currently planted on the ground (the Armature detects
    /// them by world height). @p groundOffsetY is the MODEL-space height of the world floor
    /// (the Armature's transform may translate the figure vertically — the Ground button does —
    /// and every bind-height ground reference here must shift with it, or pins heal to a floor
    /// that no longer matches the visible one). Picks the anchor root, pins, active subgraph,
    /// and support polygon. @p userPins (optional) are joints the USER pinned: each is held at
    /// its current position as a hard pin — never ground-healed, never released by the drag's
    /// lift reach, never moved by the balance stepper, and kept through suspension (an explicit
    /// pin is intent). A user pin on the effector itself or in its subtree is ignored for that
    /// drag (dragging a pinned hand moves the pin). Returns false if the rig isn't built or the
    /// effector is invalid.
    /// @p secondaries (optional; THE GROUP, 2026-10-27): other joints dragged WITH the effector —
    /// promoted as it is, their limbs active, their own contacts lifted by the drag (liftedByDrag),
    /// user pins in their subtrees sitting out as the effector's do; the Armature holds each on
    /// its own target. The policy — intents, suspension, steps, the limb-specific drags — stays
    /// the effector's.
    bool beginDrag(int effectorNode, const std::vector<glm::vec3>& positions,
                   const std::vector<int>& contactNodes, float groundOffsetY = 0.0f,
                   const std::vector<int>* userPins = nullptr, const std::vector<int>* secondaries = nullptr);

    /// True once, after a suspended (lifted) figure has been LOWERED back onto its feet and
    /// re-planted (see updateContacts): the Armature re-arms its flat-sole hold at the REST
    /// orientation, so the feet that dangled pitched roll flat onto the floor. Cleared by the
    /// call.
    bool takeLanded() {
        const bool landed = m_justLanded;
        m_justLanded = false;
        return landed;
    }

    /// BODY VOLUMES for self-collision (bodyvolume.h), fitted to the figure's mesh sample where
    /// there is one (buildBodyVolumes): the torso as a banded profile along the spine (a centre
    /// and two side capsules per spine bone), the head, and each thigh, shin, upper arm and
    /// forearm as tapered capsules. Each applies to the OTHER limbs' joints, segments and finger
    /// riders — never to its own joints or the trunk's, never to a limb's root joint (the socket
    /// sits at the torso's edge), and never to a joint or segment that already overlaps it at
    /// BIND (the volume is the approximation, not the rig). Hands are not tested against legs
    /// (they rest on knees and thighs). This is the set as BUILT (every pair; the FK collision
    /// stop reads it with its own hysteresis); dragVolumes() is the set a drag tests.
    const std::vector<BodyVolume>& bodyVolumes() const { return m_bodyVolumes; }
    /// The body volumes for the CURRENT drag: the built set with every pair that already
    /// overlapped when the drag began masked out (beginDrag). A pose the user made with the
    /// limbs touching or crossed must not explode on the next drag: with no notion of which
    /// way is "uncrossed", two crossed hands pushed each other out of each other's forearm
    /// capsule until both arms folded up and the all-fours' hands never came down. Such a
    /// pair is left alone for the drag (and the settle); the limb can be uncrossed by
    /// dragging it. The rig's full set until a drag has begun.
    const std::vector<BodyVolume>& dragVolumes() const {
        return m_dragVolumes.empty() ? m_bodyVolumes : m_dragVolumes;
    }
    /// The flesh radius (m) of the limb segment ending at @p node — the capsule whose axis
    /// runs through it (0 = none): the volume rules keep the whole segment out with it.
    float volumeSegmentRadius(int node) const {
        return (node >= 0 && node < static_cast<int>(m_volumeSegmentRadius.size()))
                   ? m_volumeSegmentRadius[static_cast<std::size_t>(node)]
                   : 0.0f;
    }
    /// The hand a rider joint (a finger, a thumb, a carpal — kVolumeAppliesRider) belongs to,
    /// or -1: a rider inside a volume is the hand's business (the volume rules, the FK stop).
    int riderHand(int node) const {
        return (node >= 0 && node < static_cast<int>(m_riderHand.size()))
                   ? m_riderHand[static_cast<std::size_t>(node)]
                   : -1;
    }
    /// A rider's flesh clearance outside a body volume (kRiderClearance, figure-scaled).
    float riderClearance() const { return m_riderClearance; }
    /// A joint's flesh clearance outside a body volume (see bodyVolumes()): 1.5cm, 3cm for a
    /// hand or a foot (their fingers and toes ride outside the joint).
    float volumeClearance(int node) const {
        return (node >= 0 && node < static_cast<int>(m_volumeClearance.size()))
                   ? m_volumeClearance[static_cast<std::size_t>(node)]
                   : 0.0f;
    }
    /// @p target pushed out of every body volume that applies to @p node, the volumes taken at
    /// @p positions — the Armature clamps the drag goal with it, as it does against the floor.
    glm::vec3 clampOutOfVolumes(int node, glm::vec3 target,
                                const std::vector<glm::vec3>& positions) const;

    /// True if pin @p index (into pins()) is a USER pin rather than a ground contact.
    bool pinIsUser(std::size_t index) const {
        return index < m_pinUser.size() && m_pinUser[index] != 0;
    }

    /// LIVE CONTACT RE-DETECTION, the heel-lift intent's pin heights and LANDINGS — call once
    /// per drag tick with the solved model-space @p pose. A real-mass joint whose lowest riding
    /// part — itself or a token-mass descendant that rides it rigidly: the fingers under a hand,
    /// the face under the head, the toes under a foot — has come down to the floor becomes a
    /// contact PIN at that height: the hands in a deep crouch, a knee coming down. Its limb
    /// chain goes active, the support polygon grows around it, and the body can lean on it.
    /// Unlike the drag-start contacts a live one is a UNILATERAL support: no reach leash
    /// (standing back up must be able to lift a hand off the floor), never stepped, sliding
    /// along the floor when the body drags it, and RELEASED again once the body lifts it off
    /// its spot — after which the joint must clear the floor by a margin before it can
    /// re-plant. A suspended figure whose foot comes back down to the floor LANDS (every
    /// contact re-planted; takeLanded()). Returns true when the pin set changed.
    bool updateContacts(const std::vector<glm::vec3>& pose);

    /// True if pin @p index hangs BELOW the dragged joint on its own limb (a knee dragged over its
    /// planted foot, an elbow over its hand; never under a trunk drag, which has the limbs below
    /// it). Such a foot is the user's: it is never stepped, and Armature::solveIk lets it go when
    /// the knee is pulled UP; such a hand on the floor is held in full.
    bool pinUnderEffector(std::size_t index) const;

    /// The support's raw points — every pin's footprint at its current target — without pin
    /// @p excludePin's (-1 = all of them).
    std::vector<glm::vec2> supportPoints(int excludePin) const;
    /// Pin @p index's own footprint points, at its current target.
    std::vector<glm::vec2> pinFootprintPoints(std::size_t index) const;

    /// True if pin @p index (into pins()) is a LIVE contact (see updateContacts).
    bool pinIsLive(std::size_t index) const {
        return index < m_pinLive.size() && m_pinLive[index] != 0;
    }

    /// A step still in flight when the button comes up lands in TARGET terms (the pin goes to
    /// its spot; the Armature then brings the foot down onto it at the pin-easing pace).
    void landPendingStep(const std::vector<glm::vec3>& positions);

    void endDrag() {
        m_effector = -1;
        m_secondaries.clear();
        m_secAncestor.clear();
        m_secAncestorLen.clear();
        m_imbalanceTicks = 0; // a mid-confirm step trigger dies with the drag
        m_stepPin = -1;       // steppingPin() stays callable after the drag: no stale index
        m_suspended = false;
        m_scoped = false;
    }
    bool dragActive() const { return m_effector >= 0; }

    /// True when the current drag's effector carries real body mass below it (its subtree over
    /// ~5% of the body: the hip, the spine, the chest, the head; a thigh) — a TRUNK gesture that
    /// moves the body, as opposed to a LIMB gesture placing a hand or a foot. The release settle's
    /// pose-hold bound (the released joint stays within 2cm of where it was let go) applies to
    /// limb effectors only: planting the feet after a trunk drag necessarily moves the trunk,
    /// and the absolute bound left the figure standing 4cm in the air after a stepping chest drag.
    bool effectorIsTrunk() const {
        return m_effector >= 0 && static_cast<std::size_t>(m_effector) < m_subtreeMass.size() &&
               m_subtreeMass[static_cast<std::size_t>(m_effector)] > 0.05f;
    }

    /// The figure's size relative to the adult reference the rig's world-space tuning constants
    /// were calibrated on (pelvis bind height / ~1m, clamped). Every GEOMETRIC threshold —
    /// contact release reach, ground-heal bands, step geometry, suspension strain — scales by
    /// it, so a 0.54-scale child crouches and walks instead of losing its pins to adult-sized
    /// thresholds.
    float sizeScale() const { return m_sizeScale; }

    /// True for a FOOT-CLASS joint (kFootBindHeight): the ONE reading of the rule, for the rig's
    /// own policy and the Armature's solve alike (both read the same bind positions).
    bool isFootClass(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_bindPos.size() &&
               m_bindPos[static_cast<std::size_t>(node)].y < kFootBindHeight * m_sizeScale;
    }
    /// Per bone: its anatomical parent (-1 at the root) — the Armature's own, as the rig was built.
    const std::vector<int>& parents() const { return m_parents; }
    /// @p node rests ON THE FLOOR in @p positions: its lowest riding part within the seed band of its
    /// clearance — what seedPoseContacts plants at a press, read off the pose whether or not it has.
    bool restsOnFloor(int node, const std::vector<glm::vec3>& positions) const;

    /// Which nodes the current drag's solve moves (paths between effector, pins, pelvis, and the
    /// anchor root); everything else keeps its local pose and rides along.
    const std::vector<char>& activeNodes() const { return m_active; }

    /// The current drag's pins (planted contacts, user pins, live contacts, the airborne root
    /// fallback): the Armature's solve holds each as rows, approaching a target the rig MOVES
    /// (ground healing, a step's swing, a slide, a landing) at a capped pace.
    const std::vector<IkEffector>& pins() const { return m_pins; }

    /// A joint's floor clearance: its rest height above the floor for a ground contact (the
    /// foot-class joints), else how far the figure's own FLESH reaches from the joint straight
    /// DOWN, as the bone is turned now (setBoneRotations; fitFloorClearances). The Armature clamps
    /// the drag target with it so the cursor cannot ask for a joint below the floor either.
    float floorClearance(int node) const {
        return (node >= 0 && node < static_cast<int>(m_floorClearance.size()))
                   ? m_floorClearance[static_cast<std::size_t>(node)]
                   : 0.0f;
    }

    /// The FBIK solve root (the "pelvis": first multi-child descendant of the anatomical root —
    /// real figures root at an origin-level FIGURE NODE with the hip as its lone child). The
    /// Armature absorbs the solved root displacement into THIS bone's pose translation.
    int rootNode() const { return m_pelvis; }
    /// A live contact's limb: its chain's rest span from the joint up to its junction (0 if unknown).
    float liveSpan(std::size_t pin) const { return pin < m_liveSpan.size() ? m_liveSpan[pin] : 0.0f; }

    /// The node the current drag actually solves for. Usually the grabbed joint passed to
    /// beginDrag(), but a token-mass grab (a finger, a toe, a face bone) is PROMOTED to the
    /// limb's first real-mass joint (the hand, the foot, the head) — a finger pull is an ARM
    /// gesture. The Armature offsets its drag targets by the grab offset when these differ.
    int dragEffector() const { return m_effector; }
    /// THE GROUP's other dragged joints (beginDrag's secondaries, promoted as the effector is):
    /// empty for a single joint's drag.
    const std::vector<int>& secondaryEffectors() const { return m_secondaries; }
    /// True for the effector or any secondary.
    bool isEffector(int node) const;
    /// pinUnderEffector for the effector OR any secondary — the stepper's test: such a foot is
    /// the user's (the limb-specific drags keep reading the effector's own).
    bool pinUnderAnyEffector(std::size_t index) const;
    /// pinUnderEffector with @p joint in the effector's place: pin @p index hangs below @p joint on
    /// its own limb (a secondary knee's planted foot: Armature::updateGroupTargets).
    bool pinUnderJoint(std::size_t index, int joint) const {
        return index < m_pins.size() && belowInItsLimb(m_pins[index].node, joint);
    }
    /// True when the grab was a joint of the pelvis girdle (the pelvis bone, the first two spine
    /// bones, a thigh socket) PROMOTED to the solve root: a pelvis drag that keeps the grabbed
    /// joint — not the root — on the cursor (Armature::dragIkTo measures the offset per tick).
    bool girdleGrab() const { return m_girdleGrab; }
    /// True for the solve root and for a joint of the pelvis girdle (see girdleGrab()): a joint
    /// that goes where the pelvis goes and nowhere else.
    bool isGirdleJoint(int node) const;
    /// True for a joint of the SPINE: above the solve root, its limb junction the root itself, and
    /// no foot-class joint anywhere below it (the pelvis bone and the thigh sockets have the feet).
    bool isSpineJoint(int node) const;
    /// A leg joint with a foot-class joint below it — a knee, or a thigh's twist bone (never a girdle
    /// joint, never a foot): the joint a kneel plants, what the solve prices as a knee.
    bool isLegJointAboveFoot(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_legAboveFoot.size() && m_legAboveFoot[static_cast<std::size_t>(node)] != 0;
    }
    /// The thigh socket a joint hangs from, or -1 for no leg.
    int legOf(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_legOfNode.size() ? m_legOfNode[static_cast<std::size_t>(node)] : -1;
    }
    /// The PELVIS BONE: the joint of the girdle, other than the solve root, that BOTH LEGS hang
    /// from (two children or more with a foot-class joint below them); none on a rig whose legs
    /// hang off the root itself. NOT "a girdle joint with two children": one generation's thighs
    /// carry their twist helpers as children beside the shin, and read as pelvis bones they were
    /// priced like one - on planted knees hip flexion cost what tipping the pelvis costs, and a
    /// kneeling figure sat back onto her heels reclined 41 degrees instead of folding at the hips.
    bool isPelvisBone(int node) const;
    /// THE SEAT: the index of a USER pin on the pelvis girdle in this drag, or -1. A pinned pelvis
    /// is a body that something else holds up — a chair, a stool, a ledge the app knows nothing
    /// of: it keeps no balance over its feet and takes no step (see Armature::solveIk).
    int seatPin() const { return m_seatPin; }
    /// True while the pelvis RESTS ON THE FLOOR: a live contact on a joint of the pelvis girdle (a
    /// sit on the floor, made by a drag or found in the pose). She cannot fall from there — what
    /// balance guards against — so the solve's balance row stands down, as under a pinned seat.
    bool floorSeat() const { return floorSeatNode() >= 0; }
    /// True while she is ON HER KNEES: a live floor contact on a joint of a leg ABOVE its foot (a
    /// knee, a shin: not foot-class itself, a foot-class joint below it). Read at a drag's start it
    /// says the drag BEGAN kneeling, which is as good a base to fold the trunk over as standing is.
    bool onKnees() const;
    /// A live contact that has LIFTED and is kept only as a SHADOW of a support (updateContacts:
    /// its footprint follows the joint while the body still leans on it). It holds NOTHING — the
    /// solve fades its rows out whatever the rise says, and the idle-arm hang has its arm.
    bool pinIsShadow(std::size_t pin) const;
    /// The height the HIPS sit at over planted knees with the thighs upright — the solve root's
    /// bind height over the highest such knee's, from that knee's contact height: the kneeling
    /// figure's seat (the rise of a kneel-start trunk drag is measured about it). Negative when
    /// onKnees() is false.
    float kneelSeatHeight() const;
    /// ... and the whole point: over the planted knees (their mean), at that height. y < 0 = no knee planted.
    glm::vec3 kneelSeat() const;
    /// Joints NOT to seed as contacts from the pose when the next drag is a seated figure's trunk
    /// drag (seedPoseContacts): the hands of arms that HUNG and came down on the floor by
    /// themselves in an earlier drag — no supports of hers, and props if they are held. Set by
    /// the Armature before beginDrag.
    void setIncidentalContacts(const std::vector<char>& nodes) { m_incidentalContact = nodes; }
    /// A TWIST HELPER IS NO CONTACT (2026-10-28; IK_HELPER_CONTACTS is the A/B): a bone that splits
    /// a rigid segment — the main family's mid-thigh and mid-forearm twist bones — has no joint a
    /// body rests on; the segment's far joint (the knee, the hand) is the contact. The contact rules
    /// took the FIRST real-mass joint whose flesh reached the floor, in node order, and on a
    /// character whose prone lies flatter than the base figure's the thigh twist's flesh reached it
    /// before the knee's: planted 12cm up and held DOWN by the belly's knee ceiling, with the knee
    /// itself free, the hips dragged up off the belly lifted the thigh about its middle — 106mm of
    /// 250, the trunk hinged into a cobra — and the lying foot's model asked the ankle 7cm UP to
    /// the "knee's" height. No reference rig ever planted one (their thighs slope from a higher hip).
    bool helperNode(int node) const;
    /// Per bone, this tick: a HAND contact whose hold the solve's RISE has faded out entirely because
    /// its arm has no length to give (Armature::m_jsContactRiseUnloaded). Such a contact is TAUT for
    /// the lift-off's purposes (updateContacts; IK_LIFT_TAUT_ONLY): the solve reads an arm's stretch
    /// as well as the junction's reach, the lift-off only the reach, and between the two a straight
    /// arm on a shrugged collar left its hand weightless on the floor and then in the air with its
    /// pin standing. Set by the Armature before each updateContacts().
    void setRiseUnloadedContacts(const std::vector<char>& nodes) { m_riseUnloaded = nodes; } // (assigned, not moved: the buffer keeps its capacity across ticks)
    /// Per node, this tick: a live contact whose LIFT-OFF the solve defers — a knee whose lying foot is
    /// still rolling back onto its toes (the flat kneel's unroll, Armature::dragIkTick). With the pin
    /// went the foot's soft hold in place: the foot slid 10cm and the thigh splayed to its box in one
    /// tick, 122mm at the knee, standing up from a kneel sat back on the heels (2026-10-28).
    void setLiftOffDeferred(const std::vector<char>& nodes) { m_liftOffDeferred = nodes; }
    bool liftOffDeferred(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_liftOffDeferred.size() && m_liftOffDeferred[static_cast<std::size_t>(node)] != 0;
    }
    /// True for a node setRiseUnloadedContacts() marked this tick.
    bool riseUnloaded(int node) const;
    /// ... and OUTRIGHT: the solve's rise term at 1, the hold's weight zero. Such a contact lifts
    /// off at once, whatever its height (IK_LIFT_UNLOADED_WAITS keeps the 2cm / three ticks).
    bool riseUnloadedOutright(int node) const;
    /// A hand contact whose unload by the rise is still EASING IN (the solve's kHandUnloadStep): not yet
    /// taut for the lift-off, whatever the junction's reach reads.
    bool riseUnloading(int node) const;
    /// ... and a hand the solve has released UNDER A DESCENT — the trailing hand of a prone, a leash the
    /// body slid forward over (the live-contact block, 2026-10-29): the no-lift-off-under-a-descent rule
    /// stands down for it.
    bool trailReleased(int node) const;
    /// A SCOPED drag (Armature's IkScope::Chain — the app's Ctrl+drag): the rig reads no intent,
    /// tests no suspension and begins no step; the chain alone answers the cursor. Set by the
    /// Armature BEFORE beginDrag() — whose user-pin rule reads it: a pin below a scoped TRUNK
    /// drag holds, where a plain trunk drag leaves it out — and cleared by endDrag().
    void setScoped(bool scoped) { m_scoped = scoped; }
    bool scoped() const { return m_scoped; }
    /// Moves live contact @p pin's place on the floor to (x, z) — a knee still COMING DOWN is
    /// planted where it lands, not where it hovered when the rig first saw it (Armature::dragIkTick).
    void reseatLivePin(std::size_t pin, float x, float z);
    /// Moves ANY contact pin's target in the floor's plane (a user pin never) and rebuilds the
    /// support polygon: a foot rolled back to standing off a flat kneel stands a foot's length
    /// behind where its ankle lay, and the rig's pin — the balance polygon, the steps — must stand
    /// with it (see A KNEELING FOOT LIES FLAT).
    void reseatPin(std::size_t pin, float x, float z);
    /// True for a joint whose floor clearance is read off a NEIGHBOUR's flesh (fleshPoints()).
    bool fleshBorrowed(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_fleshBorrowed.size() && m_fleshBorrowed[static_cast<std::size_t>(node)];
    }
    /// ... and the joint she sits on: the node of that contact, or -1.
    int floorSeatNode() const {
        for (std::size_t p = 0; p < m_pins.size(); ++p) {
            if (pinIsLive(p) && isGirdleJoint(m_pins[p].node)) {
                return m_pins[p].node;
            }
        }
        return -1;
    }

    /// The head joint and the first neck bone (the rig standard's roles, roles(); -1 on a rig
    /// without them). The Armature's head righting turns the chain between them.
    int headNode() const { return m_headNode; }
    int neckBase() const { return m_neckBase; }
    /// THE RIG STANDARD'S ROLES of this skeleton (rigroles.h): what each bone is — the hips, the
    /// spine links, the collars, the limbs' joints and twist helpers, the hands' and feet's parts,
    /// the face — inferred from the bind skeleton's structure at build. Every rule of the rig that
    /// used to read a bone's NAME for its part (the body volumes' limb roots, the head and the neck,
    /// the helpers) reads this; a rig whose names match nothing conforms the same way.
    const RigRoles& roles() const { return m_roles; }
    /// The node of the foot currently mid-STEP (its pin target swinging to its new spot), or -1.
    int steppingPin() const {
        return m_stepPin >= 0 ? m_pins[static_cast<std::size_t>(m_stepPin)].node : -1;
    }

    /// True while the figure is SUSPENDED (lifted off its feet by a sustained beyond-reach
    /// pull: the contact pins released, the body hanging from the grabbed joint), and how far
    /// below the grab point the root hangs (the grab-to-root chain length).
    bool suspended() const { return m_suspended; }
    float suspendHang() const { return m_suspendHang; }

    /// Balance steps completed during the current drag (diagnostics / tests).
    int stepsTaken() const { return m_stepsTaken; }
    /// How many of the pins may STEP (standing feet that are neither user pins nor under the
    /// effector): a stance walks only with two of them (the last one never steps).
    int steppablePins() const {
        int count = 0;
        for (const char flag : m_pinSteppable) {
            count += flag ? 1 : 0;
        }
        return count;
    }

    // --- What the Armature's solve (Armature::solveIk) reads besides pins() and activeNodes() ---
    /// Per bone segment masses (BalanceController::assignMasses; 0 outside the body).
    const std::vector<float>& masses() const { return m_masses; }
    /// The current support polygon (XZ, counter-clockwise; empty when nothing is planted).
    const std::vector<glm::vec2>& supportHull() const { return m_supportHull; }
    /// The balance inset of the support polygon (figure-scaled).
    float balanceMargin() const;
    /// The drag INTENT for this tick, read BEFORE the solve on the current pose (down: a pelvis
    /// drag below its start height, a taut limb pushed well below itself; up: a limb pulled
    /// above itself): the live pins' bounds (boundLivePins) and the step policy depend on it,
    /// and for a non-pelvis drag it also runs the LIFT-OFF test (updateSuspension).
    void updateIntent(const glm::vec3& target, const std::vector<glm::vec3>& positions);
    /// The STEPPING policy for this tick on the solved pose @p positions: an in-flight step
    /// advances (its pin target swings), a new one triggers from the balance effort (the centre
    /// of mass the solve could not keep over the support) or, for a pelvis drag and a trunk
    /// drag whose target the lean cannot serve, from a foot far from its stance spot under
    /// where the user is taking the body. (There is no pin-strain trigger: the solve holds its
    /// pins exactly, so a foot is never dragged off its plant.)
    /// @p bodyAnchor (model xz), when given, is where a TRUNK drag's solve is taking the hips
    /// (Armature's trunk follow): the steps' anchor, as the target is a pelvis drag's.
    void updateStepPolicy(const glm::vec3& target, const std::vector<glm::vec3>& positions,
                          const glm::vec2* bodyAnchor = nullptr);
    /// How far the stance has WALKED since the drag began (the landed steps' displacement
    /// shared over the standing feet, plus a landing's horizontal re-base; XZ in x/z): the
    /// Armature's solve moves its root-translation posture reference by it, so the pelvis comes
    /// over the new stance instead of being held where the drag began.
    const glm::vec3& stanceShift() const { return m_stanceShift; }

    /// The node where @p node's limb joins the AXIAL skeleton — the first ancestor whose
    /// off-path descendants carry real body mass (the upper chest for anything on an arm, the
    /// pelvis for a leg or a spine bone). The solve's stiffness classes are cut here (the
    /// dragged limb, a pin's limb, the trunk), and a limb's down intent measures its reach from
    /// it. Mass (m_subtreeMass), not branching, identifies the junction: a finger's first
    /// branching ancestor is the HAND, and the token-mass pectoral bones branch the chest.
    int limbJunction(int node) const;

private:
    // Built once per figure:
    SkeletonGraph                m_graph;
    std::vector<int>             m_parents;        ///< Anatomical hierarchy.
    std::vector<std::vector<int>> m_children;      ///< Per bone: anatomical children, ascending.
    std::vector<glm::vec3>       m_bindPos;        ///< Bind joint positions (model space).
    std::vector<glm::vec3>       m_edgeRestDir;    ///< Per bone: rest direction parent -> bone.
    std::vector<float>           m_edgeRestLen;    ///< Per bone: rest length of that edge.
    std::vector<float>           m_masses;         ///< Per bone: segment mass (balance).
    std::vector<float>           m_subtreeMass;    ///< Per bone: own + descendants' mass.
    std::vector<float>           m_floorClearance; ///< Per bone: see floorClearance().
    /// Per bone: the EXTREME points of the skin it dominates, about its joint (bind frame, metres) —
    /// the support of its flesh; empty for a bone without a fit (foot-class, token, too little skin).
    std::vector<std::vector<glm::vec3>> m_fleshPoints;
    std::vector<char>                   m_fleshBorrowed; ///< Per bone: its flesh points are a neighbour's (see fleshPoints()).
    std::vector<char>                   m_incidentalContact; ///< Per bone: see setIncidentalContacts().
    std::vector<char>                   m_riseUnloaded;      ///< Per bone: see setRiseUnloadedContacts().
    std::vector<char>                   m_liftOffDeferred;   ///< Per bone: see setLiftOffDeferred().
    std::vector<char>                   m_fleshRows;         ///< Per bone: see fleshPoints().
    // Per-node STRUCTURE the policy asks for every tick, cached at build (constants of the skeleton):
    std::vector<char>      m_underPelvis;     ///< See descendsFromPelvis().
    std::vector<char>      m_footBelow;       ///< See hasFootBelow().
    std::vector<char>      m_legAboveFoot;    ///< See isLegJointAboveFoot().
    std::vector<char>      m_limbTip;         ///< See isLimbTip().
    std::vector<int>       m_legOfNode;       ///< See legOf().
    std::vector<glm::vec3> m_lastAppliedPrev; ///< updateContacts: the tick before m_lastApplied (swapped, never reallocated).
    /// True for a drag of a figure SEATED ON THE FLOOR by a joint of her upper body (set where the
    /// pose's contacts are seeded): her arms are no supports in it (seedPoseContacts).
    bool                                m_seatedTrunkDrag = false;
    std::vector<char>            m_helperNode;     ///< Per bone: IkRigBone::helper — a twist helper, never a contact (helperNode()).
    bool                         m_rootIsSeat = false; ///< The rig has no pelvis bone: the root's own flesh is the seat, and the root a contact candidate (see the contact rules).
    bool                         m_standardNamed = false; ///< Every bone the standard names carries that name (a conformed FBX rig, or one authored to the standard): massed by role.
    std::vector<char>            m_bodyNode;       ///< False for the figure-node chain above the
                                                   ///< pelvis: no edges, contacts, or mass.
    /// Per bone: its token-mass descendants — the parts that ride it rigidly whenever it is not
    /// itself on an effector path (a hand's fingers, the head's face bones, a foot's toes). The
    /// live contact detection reads a joint's height through them: a hand has "reached the
    /// floor" when its lowest fingertip has.
    std::vector<std::vector<int>> m_ridingDescendants;
    int                          m_pelvis = -1;    ///< The FBIK root (see rootNode()).
    float                        m_sizeScale = 1.0f; ///< See sizeScale().
    float                        m_groundOffsetY = 0.0f; ///< World-floor height in model space (per drag).

    // Per-drag state. Pin targets are captured at DRAG START and only ever moved by policy (a
    // step, a slide, a heel lift, a landing) — never re-read from the pose, which would let any
    // residual relocate the stance tick by tick.
    int                     m_effector = -1; ///< The solved effector — see dragEffector().
    int                     m_grabbed = -1;  ///< The joint beginDrag() was given, before any promotion.
    bool                    m_girdleGrab = false; ///< See girdleGrab().
    int                     m_seatPin = -1;       ///< See seatPin().
    bool                    m_scoped = false;     ///< See setScoped().
    RigRoles                m_roles;              ///< See roles().
    int                     m_headNode = -1;      ///< See headNode().
    mutable int             m_pelvisBoneNode = -2; ///< See isPelvisBone(): -2 = not looked for yet, -1 = none.
    int                     m_neckBase = -1;      ///< See neckBase().
    float                   m_effectorChainLen = 0.0f; ///< pathLenToRoot(m_effector): drag-invariant.
    /// The effector's ancestor chain (effector first) with the cumulative rest length to each:
    /// the tree-path metric of pathLenToEffector(), captured once per drag after the promotion.
    std::vector<int>        m_effAncestor;
    std::vector<float>      m_effAncestorLen;
    /// THE GROUP's secondary effectors (see secondaryEffectors()), each with its ancestor chain
    /// and cumulative lengths as the effector's: pathLenToAnyEffector reads them all.
    std::vector<int>                m_secondaries;
    std::vector<std::vector<int>>   m_secAncestor;
    std::vector<std::vector<float>> m_secAncestorLen;
    std::vector<IkEffector> m_pins;        ///< Planted contacts (or the airborne root fallback).
    std::vector<char>       m_active;      ///< Per node: on a path joining the effector or a pin to the root (rebuildActiveSet).
    std::vector<glm::vec2>  m_supportHull; ///< XZ support polygon of the planted contacts.
    /// The DRAG-START joint positions (ground-healed; moved with the stance by every landed
    /// step and landing): the reference the intents measure against (a pelvis drag below its
    /// start height is a crouch).
    std::vector<glm::vec3>  m_startPose;
    /// The balance EFFORT (XZ), low-passed across ticks: how far the solved pose's centre of
    /// mass sits outside its support — the step policy's balance trigger.
    glm::vec2               m_balanceCorrection{0.0f};
    /// How far outside its support the centre of mass stood when the drag BEGAN, metres. The
    /// drag-start pose is the contract — the solve's balance row never asks that imbalance back
    /// (Armature: m_jsBalanceBase), and the step policy's effort is measured OVER it: a bow let go
    /// of, a loaded pose that leans, stood 6cm out by the row's measure, and grabbing her HAND —
    /// the cursor still — walked her three steps, her head 29cm. Gone once a step has landed (the
    /// new stance is the rig's own, centred under the weight).
    float                   m_startImbalance = 0.0f;
    // SUSPENSION (lift-off): a sustained, mostly-vertical pull beyond the leashed body's reach
    // means the user is deliberately lifting the figure — the contact pins release and the
    // Armature's solve hangs the root below the grab point (suspended() / suspendHang()).
    bool                    m_suspended = false;
    int                     m_suspendTicks = 0;
    bool                    m_lastUpIntent = false; ///< A limb pulled above itself: no new steps.
    /// The last solve ran under DOWNWARD intent — a crouching root drag (target below the
    /// drag-start pelvis) or a decisive downward pull on any other joint. The heel lift adds
    /// lift only then: heels rise when the body goes DOWN past the ankle's range, never for the
    /// few degrees a lean or a pull-up rolls a planted sole (that roll is the flat-sole hold's
    /// deliberate softness, and answering it lifted a heel a centimetre mid head-pull).
    bool                    m_lastDownIntent = false;
    /// The lowest a pelvis drag's target has been in this drag (model space): "going down" ends
    /// once the target is brought back up from it (updateIntent).
    float                   m_lowestPelvisTarget = 1.0e9f;
    /// The applied joint positions updateContacts saw last tick (is a knee COMING DOWN?).
    std::vector<glm::vec3>  m_lastApplied;
    float                   m_suspendHang = 0.0f; ///< Grab-point→root chain length (root hangs here).
    /// Builds m_bodyVolumes / m_volumeClearance from the bone names and the bind pose (build()),
    /// fitted to the body mesh sample when there is one (ikrigvolumes.cpp).
    void buildBodyVolumes(const std::vector<std::string>& names, const std::vector<BodyMeshPoint>* mesh);
    struct VolumeFitter; ///< The fit's working state and parts (ikrigvolumes.cpp).
    /// The balance masses of the bones the name table left at the token mass, by their roles (see ikrig.cpp).
    void fillMassesByRole();
    /// The balance masses of a STANDARD-NAMED rig, every real bone by its role (see ikrig.cpp).
    void assignMassesByRole();
    /// The floor clearances of the joints above the feet, from the figure's own skin: each bone's
    /// flesh, as its extreme points (see the definition).
    void fitFloorClearances(const std::vector<BodyMeshPoint>& mesh);

public:
    /// The bones' world rotations as the pose stands (every bind is translation-only, so these are
    /// the rotations away from the bind): re-reads each fitted joint's floor clearance along the
    /// world's DOWN in the bone's own frame. Called by the Armature before a drag begins and at
    /// the top of every tick; the clearances are constants of the tick.
    void setBoneRotations(const std::vector<glm::mat3>& worldRotations);
    /// A joint's floor clearance were the world's DOWN this direction in the bone's own (bind)
    /// frame — "a kneeling shin lies with its front down: (0, 0, 1)". The harness sizes its floor
    /// scenarios with it. The joint's current clearance when it has no fit.
    float floorClearanceAlong(int node, const glm::vec3& downInBoneFrame) const;
    /// The extreme points of the skin @p node's bone answers for, about its joint in the bone's
    /// (bind) frame — empty for a joint without a fit. The SOLVE's floor rows are on these points
    /// themselves (Armature::solveIk): a clearance is a constant of the tick, and a bone that
    /// turns within the tick — a hand on the floor under a forearm coming down — has another.
    /// (REAL-MASS bones only — the pelvis, the trunk, the head, the limbs' segments, the hands: a
    /// finger's or a toe's phalanx keeps its joint's clearance row. A dozen little bones each with
    /// rows of its own under one planted hand jittered it, 4cm at a fingertip under a held sit.)
    const std::vector<glm::vec3>& fleshPoints(int node) const {
        static const std::vector<glm::vec3> kNone;
        const std::size_t at = static_cast<std::size_t>(node);
        return node >= 0 && at < m_fleshPoints.size() && at < m_fleshRows.size() && m_fleshRows[at] ? m_fleshPoints[at] : kNone;
    }

private:
    std::vector<float>      m_volumeSegmentRadius;      ///< See volumeSegmentRadius().
    std::vector<int>        m_riderHand;                ///< See riderHand().
    float                   m_riderClearance = kRiderClearance; ///< See riderClearance().
    std::vector<BodyVolume> m_dragVolumes;              ///< See dragVolumes().
    /// Rebuilds m_dragVolumes from the pose the drag starts in (see dragVolumes()).
    void maskOverlappingVolumes(const std::vector<glm::vec3>& positions);
    std::vector<BodyVolume> m_bodyVolumes;     ///< See bodyVolumes().
    std::vector<float>      m_volumeClearance; ///< See volumeClearance().
    /// True when this drag began with ground-HEALED (snapped) pins — a hovering figure being
    /// pulled back onto the floor (beginDrag). No step is triggered while it heals: the feet are
    /// still on their way down to the floor.
    bool                    m_healing = false;
    bool                    m_justLanded = false; ///< See takeLanded().
    /// Suspension only: true once every foot has cleared the floor (the figure really hangs),
    /// which a LANDING requires — at the moment a lift confirms, the feet are still on the
    /// floor, and landing on them re-planted the figure before it ever rose.
    bool                    m_airborne = false;
    // BALANCE-DRIVEN STEPPING (foot re-planting): when a sustained drag holds the CoM outside
    // the support polygon beyond what leaning can absorb, the figure re-plants a foot at its
    // BALANCED stance position — the drag-start stance shape re-centered on the current CoM —
    // with an animated swing (eased glide + lift arc on the pin target). Captured at drag
    // start: which pins are steppable standing feet, each pin's XZ offset from the stance
    // center (the stance SHAPE), and each pin's footprint (its limb's planted-contact offsets,
    // so the support polygon can be rebuilt around a moved pin).
    std::vector<std::vector<glm::vec2>> m_pinFootprint;
    std::vector<glm::vec2>              m_pinStanceOffset;
    /// The stance center's drag-start XZ offset from the ROOT: an explicit (pelvis-drag)
    /// anchor is offset by it, so the walk re-creates the stance the figure actually held
    /// under its hip (the feet naturally sit a few centimetres behind it) rather than one
    /// centered under the hip. Balance-driven steps stay centered on the CoM itself — that IS
    /// the re-centering they exist for (offsetting them cost two of a chest drag's four steps).
    glm::vec2                           m_stanceRootOffset{0.0f};
    std::vector<char>                   m_pinSteppable;
    std::vector<char>                   m_pinUser; ///< Parallel to m_pins: 1 = user pin.
    std::vector<char>                   m_pinLive; ///< Parallel to m_pins: 1 = live contact.
    std::vector<int>                    m_liveErrTicks; ///< Parallel: ticks over the release error.
    /// Parallel: a live pin's limb REST SPAN — the straight-line length of its chain's rest
    /// offsets from the pin up to its junction (a hanging arm's shoulder-to-hand distance):
    /// the socket farther from the pin than this is a limb being pulled taut.
    std::vector<float>                  m_liveSpan;
    /// Per node: 1 while a released live contact must clear the floor before re-planting.
    std::vector<char>                   m_contactBlocked;
    int       m_stepPin = -1;      ///< Index into m_pins of the foot in flight (-1 = none).
    glm::vec3 m_stepFrom{0.0f};    ///< The step's start (the pin target it left) ...
    glm::vec3 m_stepTo{0.0f};      ///< ... and its landing spot (model space).
    float     m_stepHeight = 0.0f; ///< The swing's lift arc height (m).
    int       m_stepTick = 0;      ///< Ticks into the swing ...
    int       m_stepTotal = 0;     ///< ... of this many (kStepSpeed pacing).
    int       m_imbalanceTicks = 0; ///< Consecutive ticks of above-threshold balance need.
    int       m_stepsTaken = 0;
    glm::vec3 m_stanceShift{0.0f}; ///< See stanceShift().

    /// Rebuilds m_supportHull from the pins' footprints at their CURRENT targets, excluding
    /// @p excludePin's contribution (the foot in flight supports nothing).
    void rebuildSupportHull(int excludePin);

    /// Summed bone rest lengths from @p node up to the pelvis (the one metric distance used by
    /// leashes, reach clamps, and suspension alike).
    float pathLenToRoot(int node) const;

    /// True when the drag itself lifts contact @p node: it is within kEffectorLiftReach of the
    /// effector by tree path — UNLESS it hangs below an effector that is not itself foot-class.
    /// Dragging a foot (or a toe, promoted to it) lifts the foot and its toes; dragging the KNEE
    /// leaves the foot below it planted, which is the gesture: the knee swung out, in or forward
    /// over a standing foot. (Released, the leg below a dragged knee rode it rigidly: a knee
    /// taken 15cm forward was a goose step with the STANDING knee bent 21 degrees to pay for it,
    /// 10cm inward ran the thigh's twist to its 75-degree limit, and a knee raise stopped 17cm
    /// short of the cursor.)
    bool liftedByDrag(int node) const;
    /// True when @p node hangs below the drag effector AND the effector is a joint of the limb the
    /// node ends (a knee over its foot, an elbow over its hand) — never a trunk joint, which has
    /// every limb below it.
    bool belowEffectorInItsLimb(int node) const;

    /// Summed bone rest lengths along the tree path from @p node to the drag effector (up one
    /// side to their lowest common ancestor, down the other — see m_effAncestor); a huge value
    /// for a node outside the body tree. The contact-release reach is measured with it.
    float pathLenToEffector(int node) const;
    /// pathLenToEffector against an effector's ancestor @p chain with its cumulative @p chainLen.
    float pathLenToChain(int node, const std::vector<int>& chain, const std::vector<float>& chainLen) const;
    /// The least path length from @p node to the effector or any secondary.
    float pathLenToAnyEffector(int node) const;
    /// belowEffectorInItsLimb with @p effector in the effector's place.
    bool belowInItsLimb(int node, int effector) const;
    /// liftedByDrag for ONE effector (@p effector, with its ancestor chain).
    bool liftedByEffector(int node, int effector, const std::vector<int>& chain, const std::vector<float>& chainLen) const;

    /// True when @p node hangs below the pelvis in the anatomical tree: what a ground contact
    /// must be (a merged follower's scene node sits beside the pelvis at the origin and once
    /// read as "planted").
    bool descendsFromPelvis(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_underPelvis.size() && m_underPelvis[static_cast<std::size_t>(node)] != 0;
    }
    /// isPelvisBone()'s test for one node (uncached).
    bool hangsBothLegs(int node) const;
    /// True when a foot-class joint (low at bind) hangs anywhere below @p node: a leg's joint, the
    /// pelvis bone — never a bone of the spine.
    bool hasFootBelow(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_footBelow.size() && m_footBelow[static_cast<std::size_t>(node)] != 0;
    }

    /// A joint's height above the floor through its lowest riding part (see
    /// m_ridingDescendants), each part measured against its own floor clearance: 0 = touching.
    float contactHeight(int node, const std::vector<glm::vec3>& positions) const;
    /// True when @p joint is the KNEE over @p pinned: the real joint the pinned one hangs from.
    bool kneeOver(int joint, int pinned) const;
    /// A real-mass joint with no real-mass joint below it (a hand, a foot, the head).
    bool isLimbTip(int node) const {
        return node >= 0 && static_cast<std::size_t>(node) < m_limbTip.size() ? m_limbTip[static_cast<std::size_t>(node)] != 0
                                                                                : computeIsLimbTip(node);
    }
    /// The per-node structure above, computed (build caches them: constants of the skeleton that the
    /// policy asked for every tick, each an O(n x depth) walk).
    bool computeDescendsFromPelvis(int node) const;
    /// A joint of an ARM (or the head): its limb hangs from a junction above the hips, where a
    /// leg's is the pelvis. (The sit's rule — a floor sitter's arms are no supports under a trunk
    /// drag — in seedPoseContacts and updateContacts.)
    bool limbHangsAboveHips(int node) const {
        return m_bindPos[static_cast<std::size_t>(limbJunction(node))].y >
               m_bindPos[static_cast<std::size_t>(m_pelvis)].y + kArmJunctionHeight * m_sizeScale;
    }
    bool computeHasFootBelow(int node) const;
    bool computeIsLegJointAboveFoot(int node) const;
    bool computeIsLimbTip(int node) const;
    int  computeLegOf(int node) const;
    bool servedByPinAbove(int node) const;    ///< A pin at or above the joint on its path makes it no contact of its own — a hand under an elbow excepted.
    bool servedByHandBelow(int node, const std::vector<glm::vec3>& positions) const; ///< A hand on the floor below an arm joint makes the joint (an elbow) no contact of its own.

public:
    /// How far live contact @p pin's joint has RISEN off the floor, in @p positions: over the
    /// height it was planted at — or, for a joint whose clearance is fitted to its skin, over
    /// where its flesh rests AS THE BONE IS TURNED NOW (a pelvis rolled back from a sit goes over
    /// the buttocks, its joint 3.5cm higher on the way, resting all the while).
    float liveContactRise(std::size_t pin, const std::vector<glm::vec3>& positions) const;
    /// How STRETCHED a live contact's limb is toward its target: over the joints of the path from
    /// the contact up to its junction — those with at least kStretchChainShare of the limb's span
    /// of chain below them — the best of (the joint's distance from the target) / (the rest length
    /// of the chain between them): 1 for a dead-straight limb, less as it folds. The solve's
    /// slack-arm rule reads it (a hand contact held down, a ceiling on it, while its arm has length
    /// to give): on the junction's distance over the whole chain's sum, which is 1 only for a
    /// COLLINEAR chain, the two oldest generations' straight arms read 0.85 — their collars run
    /// sideways from the chest — and a hand under a chest lifted off all fours never let go: the
    /// trunk stayed down, 23cm short. (The LIFT-OFF keeps the junction's measure: it asks whether
    /// the body has left the limb's reach, not whether the limb is straight — a hand landing from a
    /// hanging arm is straight too.)
    float liveContactStretch(std::size_t pin, const std::vector<glm::vec3>& positions) const;

private:

    /// Live contacts' bounds while the drag takes the body DOWN: each planted knee or hand
    /// keeps the root within its limb's reach (the socket-centered leash every standing foot
    /// has), and a knee planted ABOVE its still-pinned foot — a leg held at both ends, with
    /// nothing left to give — becomes a HARD pin (held in full by the solve, not unilaterally),
    /// so a dragged root stops where the leg runs out instead of the leg buckling into a
    /// compromise. Under any other intent both are lifted again
    /// (the 1e9 leash sentinel, soft): standing up must be able to lift a hand or a knee off,
    /// which the lift-off rule then decides. Hands stay soft: an arm folds.
    void boundLivePins(const std::vector<glm::vec3>& positions);
    /// The SUSPENSION (lift-off) test, run from updateIntent: a sustained (~0.25s), mostly
    /// vertical pull on a target GEOMETRICALLY beyond the leash-bound body's reach — farther
    /// from the root than the fully extended grab-to-root chain plus a margin, never the
    /// instantaneous drag error — confirms over kSuspendConfirmTicks and then releases the
    /// contact pins (user pins stay), clears the support polygon and makes every real-mass
    /// segment active (token-mass bones — the face, the fingers — keep their pose and ride).
    void updateSuspension(const glm::vec3& target, const std::vector<glm::vec3>& positions);
    /// Whether a LIMB drag toward @p target reads as a decisive DOWNWARD push: the target 15cm
    /// below the effector with the limb TAUT toward it (its junction-to-effector reach past 90%
    /// of its rest span — a hand far below its cursor with the arm still folded is not pushing
    /// the body down); trunk effectors are always "taut".
    bool limbDownIntent(const glm::vec3& target, const std::vector<glm::vec3>& positions) const;
    /// The drag-start contact capture, reusable for a LANDING: @p contactNodes (joints the
    /// caller found on the floor) become the planted set — minus the effector's own lift reach
    /// and non-body nodes — the most proximal planted contact of each limb a ground-healed,
    /// leashed pin, @p userPins hard pins (replacing a coinciding contact), the airborne root
    /// fallback when nothing is planted, the live-contact arrays reset, the support polygon,
    /// the stepping state (footprints, steppable feet, the stance shape) and the pose prior
    /// re-anchored at @p positions (healed by the ground snap), the active set and the
    /// stiffness rebuilt. Assumes m_effector / m_effAncestor are set (beginDrag).
    /// @p dragStart: a foot the user lifted (kFootLiftedHeight) is left lifted; a landing out
    /// of a lift (false) plants whatever has come down.
    void plantContacts(const std::vector<int>& contactNodes,
                       const std::vector<glm::vec3>& positions,
                       const std::vector<int>* userPins, bool dragStart = false);
    /// A LANDING mid-drag (out of suspension, or a foot touching down): every body joint under
    /// the contact height of @p applied is re-planted through plantContacts (the surviving
    /// user pins kept), and the drag-start reference moves horizontally under the landed body
    /// (m_startPose, stanceShift()) — the figure stands back up where it landed, not in the
    /// pose it touched down in. Sets the flag takeLanded() reports.
    void landAt(const std::vector<glm::vec3>& applied);

    /// Adds a LIVE contact pin at @p node held at @p target (see updateContacts) with its
    /// footprint from the riding parts near the floor; the caller rebuilds the derived state.
    void addLivePin(int node, const glm::vec3& target, const std::vector<glm::vec3>& positions);
    /// A contact's FOOTPRINT: the joint itself and its riding descendants that stand within
    /// kLiveFootprintBand of the floor, as offsets from the pin's target in the floor's plane.
    std::vector<glm::vec2> footprintOf(int node, const glm::vec3& target, const std::vector<glm::vec3>& positions) const;
    /// The floor contacts the POSE made — a kneeling knee, a hand on the floor: any real-mass
    /// joint that is not foot-class and sits on the floor in @p positions — become LIVE pins, as
    /// if the drag had brought them down itself (plantContacts calls this; see there).
    void seedPoseContacts(const std::vector<glm::vec3>& positions);

    /// Removes pin @p index from m_pins and every array parallel to it.
    void removePin(std::size_t index);

    /// m_active = the paths joining the effector and every pin to the root.
    void rebuildActiveSet();

    /// Socket-centered reach leash for a pin at @p node held at @p target, in the pose
    /// @p positions: the limb hangs from its SOCKET (the root's child on the pin's chain), so
    /// the ball confines the socket — radius = the socket-to-pin distance in this pose (a
    /// stance the figure provably holds) with fractional headroom for a bent start — and is
    /// expressed on the root through the root->socket offset written to @p offsetOut (see
    /// IkEffector::leashOffset). A root-centered ball let the root sit on its far side with the
    /// socket genuinely out of reach: a planted foot 8cm short under a hard lean.
    float socketLeash(int node, const glm::vec3& target, const std::vector<glm::vec3>& positions,
                      glm::vec3& offsetOut) const;

    /// Lands the in-flight step: pin onto m_stepTo, leash recomputed against the current root,
    /// support hull rebuilt, step + trigger state cleared. Shared by the swing's final tick and
    /// landPendingStep's instant completion at mouse-up.
    void landStep(const std::vector<glm::vec3>& positions);

    /// Advances an in-flight step / evaluates the step trigger (see the .cpp): the sustained
    /// balance EFFORT, or a steppable foot far from its anchored stance spot. @p anchorXZ, when
    /// non-null, is the EXPLICIT stance anchor — a pelvis drag's target, where the user is
    /// taking the body — that steps aim at and are reach-clamped against; null anchors on the
    /// CoM (upper-body drags, where balance is the intent signal).
    void updateStepping(const std::vector<glm::vec3>& positions, bool allowTrigger,
                        const glm::vec2* anchorXZ);
};

} // namespace pose

#endif // IKRIG_H
