/**
 * @file ikrigcontacts.cpp
 * @brief LIVE contact re-detection during a drag (IkRig::updateContacts) and the pin-set
 *        maintenance around it: adding a contact pin mid-drag, removing one, landing a
 *        suspended figure, and rebuilding the active subgraph for the current pin set.
 *
 * The drag-start contacts (ikrigdrag.cpp) are what the figure stands on when the drag begins;
 * a LIVE contact is a joint that reaches the floor while the drag lowers the body — the hands
 * in a deep crouch, a knee coming down. The solve's floor rows stop it there; the contact makes
 * it a SUPPORT (its limb goes active, the support polygon grows around it). A
 * live contact is a UNILATERAL support — it holds the joint where it touched and lets the body
 * lean on it, but standing back up lifts it off again — so it carries no reach leash, never
 * steps, and releases once the body pulls it off its spot. See ikrig.cpp for the TU layout;
 * the constants come from ikrig_constants.h. Qt-free (std + GLM).
 */

#include "ikrig.h"

#include "balancecontroller.h"

#include "ikrig_constants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace pose {

namespace {

// A joint has REACHED the floor when its contact height (its lowest riding part measured
// against that part's own clearance) is within this of it: the solve's floor rows hold a joint
// exactly at its clearance, so a joint resting on the floor reads 0.
constexpr float kLiveContactBand = 0.010f;
// At a drag's START the band is wider: the pose may come from a file, or from a drag that let
// go while a knee still hovered a centimetre up. A live contact holds its joint's PLACE, never
// its height, so a generous band pulls nothing down.
constexpr float kSeedContactBand = 0.020f;
constexpr float kKneelContactBand = 0.05f; // a knee over its planted foot, going down (see DETECT)
// A live contact is UNILATERAL, like a hand on a floor: it resists being pushed through
// (the pin height never yields), it resists being dragged along up to a friction-like strain,
// beyond which it SLIDES, and it lets go when the body LIFTS it (the solve holds it in x/z
// only, so a rising body does lift it):
//  - LIFT-OFF: the limb TAUT (the solved junction farther from the pin than kLiveContactTaut
//    of the chain's rest span — the pelvis rising out of a crouch with the hands planted pulls
//    the arms straight) AND the solved joint above its pin height by kLiveContactLift, for
//    kLiveContactReleaseTicks consecutive ticks, releases the pin; the joint then re-arms only
//    after clearing the floor by kLiveContactRearm. Both halves are needed: an arm that cannot
//    FOLD (a descent the joint limits refuse) also leaves the solver's hand above the pin —
//    the clamped straight arm points beside it — but with the shoulder CLOSER than the span,
//    and that hand must slide, not let go.
//  - SLIDE: a horizontal solved error beyond kLiveContactSlip moves the pin along the floor
//    by the excess, so the pin resists up to that strain and then creeps with the body — a
//    descent the arms cannot absorb (joint limits) slides the hands forward on the floor
//    instead of dropping them (a released hand sank through the floor as an inactive subtree
//    again, and a joint AT the floor could never re-plant).
constexpr float kLiveContactLift = 0.02f;
constexpr float kLiveContactTaut = 0.95f;
constexpr float kLiveContactSlip = 0.025f;
constexpr int   kLiveContactReleaseTicks = 3;
// A released joint re-arms only once it has cleared the floor by this: released AT the floor
// with the arm taut, it would otherwise re-plant on the very next tick.
constexpr float kLiveContactRearm = 0.03f;
// Riding parts within this of the floor join the new pin's footprint (the fingertips, not the
// wrist, are what a hand stands on).
constexpr float kLiveFootprintBand = 0.03f;
// LANDING out of suspension: a lifted (dangling) figure lowered until a FOOT — a foot-class
// joint (isFootClass: the feet and toes) — has its contact height back within the live-contact
// band leaves suspension and RE-PLANTS from the applied pose: every body joint under
// kContactHeight (the Armature's drag-start contact rule, 15cm) is a contact again, pinned
// exactly as at a drag start. Before this the hanging body was simply pushed on into the floor
// (the solver folds the dangling legs up past their limits to keep the feet on the floor plane;
// the applied pose cannot, and the feet went 21cm under).
constexpr float kLandingAirborne = kAirborneHeight; // every foot this far up = the figure hangs (and plantContacts' floating test, the same number)
// A/B probes read at more than one site of this TU (each a `static const` at its site before).
const bool kHeadProps = std::getenv("IK_HEAD_PROPS") != nullptr;       // a face on the floor props the body, as before 2026-09-22
const bool kLiftNoDefer = std::getenv("IK_LIFT_NO_DEFER") != nullptr;  // a knee lifts off mid-roll, as before 2026-10-28
const bool kLiftTautOnly = std::getenv("IK_LIFT_TAUT_ONLY") != nullptr; // the lift-off by the junction's reach alone: the solve's unload not read

// POSESTUDIO_IK_TRACE=1 or IK_CONTACT_TRACE=1: the contact events (seeded, planted, slid, lifted
// off, landed) on stderr, in order with the harness's stdout.
const bool kContactTrace = std::getenv("POSESTUDIO_IK_TRACE") != nullptr || std::getenv("IK_CONTACT_TRACE") != nullptr;

} // namespace

float IkRig::pathLenToChain(int node, const std::vector<int>& chain, const std::vector<float>& chainLen) const {
    float acc = 0.0f;
    for (int cur = node; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
        for (std::size_t a = 0; a < chain.size(); ++a) {
            if (chain[a] == cur) {
                return acc + chainLen[a]; // met the effector's chain: cur is the LCA
            }
        }
        acc += m_edgeRestLen[static_cast<std::size_t>(cur)];
    }
    return 1e30f; // disconnected (the excluded figure-node chain)
}

float IkRig::pathLenToEffector(int node) const {
    return pathLenToChain(node, m_effAncestor, m_effAncestorLen);
}

float IkRig::pathLenToAnyEffector(int node) const {
    float best = pathLenToEffector(node);
    for (std::size_t i = 0; i < m_secondaries.size() && i < m_secAncestor.size(); ++i) {
        best = std::min(best, pathLenToChain(node, m_secAncestor[i], m_secAncestorLen[i]));
    }
    return best;
}

bool IkRig::isEffector(int node) const {
    if (node < 0) {
        return false;
    }
    return node == m_effector || std::find(m_secondaries.begin(), m_secondaries.end(), node) != m_secondaries.end();
}

bool IkRig::pinUnderAnyEffector(std::size_t index) const {
    if (index >= m_pins.size()) {
        return false;
    }
    if (belowEffectorInItsLimb(m_pins[index].node)) {
        return true;
    }
    for (const int sec : m_secondaries) {
        if (belowInItsLimb(m_pins[index].node, sec)) {
            return true;
        }
    }
    return false;
}

bool IkRig::liftedByEffector(int node, int effector, const std::vector<int>& chain, const std::vector<float>& chainLen) const {
    if (pathLenToChain(node, chain, chainLen) > kEffectorLiftReach * m_sizeScale) {
        return false;
    }
    static const bool kKneeLiftsFoot = std::getenv("IK_KNEE_LIFTS_FOOT") != nullptr; // A/B probe
    if (kKneeLiftsFoot || effector < 0 || effector == m_pelvis ||
        isFootClass(effector)) {
        return true; // a foot-class effector lifts what is near it (its own toes)
    }
    return !belowInItsLimb(node, effector); // below a dragged knee: the foot stays planted
}

bool IkRig::liftedByDrag(int node) const {
    // (... by the effector, or by any of THE GROUP's secondaries: a second foot dragged with the
    // first lifts its own toes too, and a second knee keeps its foot planted as the first does.)
    if (liftedByEffector(node, m_effector, m_effAncestor, m_effAncestorLen)) {
        return true;
    }
    for (std::size_t i = 0; i < m_secondaries.size() && i < m_secAncestor.size(); ++i) {
        if (liftedByEffector(node, m_secondaries[i], m_secAncestor[i], m_secAncestorLen[i])) {
            return true;
        }
    }
    return false;
}

bool IkRig::belowInItsLimb(int node, int effector) const {
    // The effector must be a joint OF THE LIMB the node ends (strictly below that limb's junction
    // with the trunk) and above the node: a knee over its foot, an elbow or a shoulder over its
    // hand. A TRUNK joint has the limbs below it too — and a chest drag that held the hands it
    // found on the floor in full could not get a sitting figure up: on the two generations whose
    // arms are short enough to put those hands within the lift reach of the chest she stayed
    // down, 80cm under the cursor, nailed by her hands.
    if (effector < 0 || effector == m_pelvis || node < 0) {
        return false;
    }
    const int junction = limbJunction(node);
    bool effectorInLimb = false;
    for (int cur = m_parents[static_cast<std::size_t>(effector)]; cur >= 0;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        if (cur == junction) {
            effectorInLimb = true;
            break;
        }
    }
    if (!effectorInLimb || effector == junction) {
        return false;
    }
    for (int cur = m_parents[static_cast<std::size_t>(node)]; cur >= 0;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        if (cur == effector) {
            return true;
        }
    }
    return false;
}

bool IkRig::belowEffectorInItsLimb(int node) const {
    return belowInItsLimb(node, m_effector);
}

bool IkRig::pinUnderEffector(std::size_t index) const {
    return index < m_pins.size() && belowEffectorInItsLimb(m_pins[index].node);
}

std::vector<glm::vec2> IkRig::pinFootprintPoints(std::size_t index) const {
    std::vector<glm::vec2> pts;
    if (index < m_pins.size() && index < m_pinFootprint.size()) {
        const glm::vec2 t(m_pins[index].target.x, m_pins[index].target.z);
        for (const glm::vec2& off : m_pinFootprint[index]) {
            pts.push_back(t + off);
        }
    }
    return pts;
}

std::vector<glm::vec2> IkRig::supportPoints(int excludePin) const {
    std::vector<glm::vec2> pts;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (static_cast<int>(p) == excludePin) {
            continue;
        }
        const std::vector<glm::vec2> own = pinFootprintPoints(p);
        pts.insert(pts.end(), own.begin(), own.end());
    }
    return pts;
}

bool IkRig::computeDescendsFromPelvis(int node) const {
    if (node < 0 || node >= static_cast<int>(m_parents.size())) {
        return false;
    }
    for (int cur = m_parents[static_cast<std::size_t>(node)]; cur >= 0;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        if (cur == m_pelvis) {
            return true;
        }
    }
    return false;
}

bool IkRig::restsOnFloor(int node, const std::vector<glm::vec3>& positions) const {
    return node >= 0 && static_cast<std::size_t>(node) < positions.size() &&
           contactHeight(node, positions) <= kSeedContactBand * m_sizeScale;
}

float IkRig::contactHeight(int node, const std::vector<glm::vec3>& positions) const {
    const std::size_t i = static_cast<std::size_t>(node);
    float h = positions[i].y - m_floorClearance[i];
    for (const int d : m_ridingDescendants[i]) {
        const std::size_t j = static_cast<std::size_t>(d);
        h = std::min(h, positions[j].y - m_floorClearance[j]);
    }
    return h - m_groundOffsetY;
}

// (The solve's per-node unload state: 1 = unloaded, 2 = outright, 3 = still easing in — in the low two
// bits — and the 4 bit set where the DESCENT let a trailing hand go: trailReleased.)
bool IkRig::riseUnloaded(int node) const {
    if (kLiftTautOnly || node < 0 || static_cast<std::size_t>(node) >= m_riseUnloaded.size()) {
        return false;
    }
    const int state = m_riseUnloaded[static_cast<std::size_t>(node)] & 3;
    return state == 1 || state == 2;
}

bool IkRig::riseUnloading(int node) const {
    return !kLiftTautOnly && node >= 0 && static_cast<std::size_t>(node) < m_riseUnloaded.size() &&
           (m_riseUnloaded[static_cast<std::size_t>(node)] & 3) == 3;
}

bool IkRig::riseUnloadedOutright(int node) const {
    static const bool kWaits = std::getenv("IK_LIFT_UNLOADED_WAITS") != nullptr; // A/B probe: the 2cm / three ticks as before
    return !kWaits && riseUnloaded(node) && (m_riseUnloaded[static_cast<std::size_t>(node)] & 3) >= 2;
}

bool IkRig::trailReleased(int node) const {
    return !kLiftTautOnly && node >= 0 && static_cast<std::size_t>(node) < m_riseUnloaded.size() &&
           (m_riseUnloaded[static_cast<std::size_t>(node)] & 4) != 0;
}

float IkRig::limbStretch(int node, const glm::vec3& target, float span, const std::vector<glm::vec3>& positions) const {
    constexpr float kStretchChainShare = 0.6f; // a joint answers for the limb's stretch only with this much of it below
    if (node < 0 || span <= 1.0e-4f) {
        return 0.0f;
    }
    const int junction = limbJunction(node);
    float     chain = 0.0f;
    float     best = 0.0f;
    for (int cur = node; cur >= 0 && cur != junction; cur = m_parents[static_cast<std::size_t>(cur)]) {
        const int above = m_parents[static_cast<std::size_t>(cur)];
        if (above < 0) {
            break;
        }
        chain += m_edgeRestLen[static_cast<std::size_t>(cur)];
        if (chain >= kStretchChainShare * span && chain > 1.0e-4f) {
            best = std::max(best, glm::length(positions[static_cast<std::size_t>(above)] - target) / chain);
        }
    }
    return best;
}

float IkRig::liveContactStretch(std::size_t pin, const std::vector<glm::vec3>& positions) const {
    if (pin >= m_pins.size() || pin >= m_liveSpan.size()) {
        return 0.0f;
    }
    return limbStretch(m_pins[pin].node, m_pins[pin].target, m_liveSpan[pin], positions);
}

float IkRig::liveContactRise(std::size_t pin, const std::vector<glm::vec3>& positions) const {
    static const bool kPlantRise = std::getenv("IK_CONTACT_PLANT_RISE") != nullptr; // A/B probe
    if (pin >= m_pins.size()) {
        return 0.0f;
    }
    const int node = m_pins[pin].node;
    // (The SEAT alone: the pelvis is the one contact that ROLLS on its flesh by centimetres. A hand
    // or a knee keeps the planted height — a hand's wrist turns freely over fingers that stay down,
    // and read this way an elbow swung out on all fours jumped 7cm where it had jumped 3.)
    if (kPlantRise || !isGirdleJoint(node) || static_cast<std::size_t>(node) >= m_fleshPoints.size() ||
        m_fleshPoints[static_cast<std::size_t>(node)].empty()) {
        return positions[static_cast<std::size_t>(node)].y - m_pins[pin].target.y;
    }
    return positions[static_cast<std::size_t>(node)].y - m_groundOffsetY - m_floorClearance[static_cast<std::size_t>(node)];
}

void IkRig::addLivePin(int node, const glm::vec3& target,
                       const std::vector<glm::vec3>& positions) {
    // No leash: the sentinel must be effectively INFINITE, not merely unset (a non-positive
    // radius makes the solver compute its path-length fallback, which would leash the root to
    // a hand on the floor — and standing up must be able to lift that hand off).
    IkEffector pin{node, target, 1.0f, 1e9f};
    m_pins.push_back(pin);
    m_pinUser.push_back(0);
    m_pinLive.push_back(1);
    m_liveErrTicks.push_back(0);
    m_pinSteppable.push_back(0);
    m_pinStanceOffset.push_back(glm::vec2(0.0f));
    // The limb's rest span (see m_liveSpan): its chain's rest offsets summed as vectors.
    {
        const int junction = limbJunction(node);
        glm::vec3 span(0.0f);
        for (int cur = node; cur >= 0 && cur != junction;
             cur = m_parents[static_cast<std::size_t>(cur)]) {
            span += m_edgeRestDir[static_cast<std::size_t>(cur)] *
                    m_edgeRestLen[static_cast<std::size_t>(cur)];
        }
        m_liveSpan.push_back(glm::length(span));
    }
    m_pinFootprint.push_back(footprintOf(node, target, positions));
}

std::vector<glm::vec2> IkRig::footprintOf(int node, const glm::vec3& target, const std::vector<glm::vec3>& positions) const {
    std::vector<glm::vec2> footprint{glm::vec2(0.0f)};
    for (const int d : m_ridingDescendants[static_cast<std::size_t>(node)]) {
        const std::size_t j = static_cast<std::size_t>(d);
        if (positions[j].y - m_floorClearance[j] - m_groundOffsetY <
            kLiveFootprintBand * m_sizeScale) {
            footprint.emplace_back(positions[j].x - target.x, positions[j].z - target.z);
        }
    }
    return footprint;
}

bool IkRig::computeIsLimbTip(int node) const {
    // A real-mass joint with no real-mass joint below it: a hand (its fingers are token), a
    // foot, a carpal on the generations whose carpals are real joints, the head.
    const int n = static_cast<int>(m_parents.size());
    for (int d = 0; d < n; ++d) {
        const std::size_t di = static_cast<std::size_t>(d);
        if (d == node || (m_masses[di] < kTokenBoneMass && m_subtreeMass[di] < kTokenLimbMass)) {
            continue;
        }
        for (int cur = m_parents[di]; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur == node) {
                return false;
            }
        }
    }
    return true;
}

bool IkRig::servedByHandBelow(int c, const std::vector<glm::vec3>& positions) const {
    // A HAND on the floor is its arm's contact; an ELBOW resting on the floor beside it is
    // incidental and no contact of its own (2026-09-23; IK_ELBOW_CONTACTS). Lying prone with
    // the arms folded under the chest the elbows rest on the floor beside the torso and were
    // planted as contacts: unilateral, held in their place until the arm read taut, they
    // slid along the floor as the body was pushed up off its belly and let go in one tick when
    // the arm straightened — the forearm 110mm, the hand 200. For LEGS the rule stays the
    // other way (a knee over its planted foot is a second support: a kneel); an arm's fold
    // joint over a hand on the floor is not.
    static const bool kElbowContacts = std::getenv("IK_ELBOW_CONTACTS") != nullptr; // A/B probe
    if (kElbowContacts || isLimbTip(c) || isFootClass(c)) {
        return false;
    }
    const int n = static_cast<int>(m_parents.size());
    for (int d = 0; d < n; ++d) {
        const std::size_t di = static_cast<std::size_t>(d);
        if (d == c || !m_bodyNode[di] || isFootClass(static_cast<int>(di)) ||
            (m_masses[di] < kTokenBoneMass && m_subtreeMass[di] < kTokenLimbMass) || !isLimbTip(d)) {
            continue; // (a HAND: a real-mass tip that is not foot-class)
        }
        bool below = false;
        for (int cur = m_parents[di]; cur >= 0 && !below; cur = m_parents[static_cast<std::size_t>(cur)]) {
            below = cur == c;
        }
        if (below && contactHeight(d, positions) <= kLiveContactBand * m_sizeScale) {
            return true;
        }
    }
    return false;
}

bool IkRig::helperNode(int node) const {
    static const bool kHelperContacts = std::getenv("IK_HELPER_CONTACTS") != nullptr; // A/B probe: twist helpers as contacts, as before
    if (kHelperContacts || node < 0 || static_cast<std::size_t>(node) >= m_helperNode.size()) {
        return false;
    }
    // A twist helper (IkRigBone::helper), or a bone the rig standard reads as a HELPER — an anchor,
    // a control, a skinless leaf (rigroles.h): no joint a body rests on either.
    return m_helperNode[static_cast<std::size_t>(node)] != 0 || m_roles.role(node) == RigRole::Helper;
}

bool IkRig::servedByPinAbove(int c) const {
    // A joint under a pin on its own path is SERVED by it — a planted foot's toes, a planted
    // hand's carpals — and is no contact of its own ... unless it is the LIMB'S TIP under a
    // pin that is not (2026-09-23; IK_ELBOW_SERVES_HAND): a HAND under an ELBOW resting on the
    // floor. Lying prone with the forearms on the floor the elbows come first in the node
    // order and were the arms' only contacts; the hands, 'served', were none — and the elbow's
    // contact is taut the moment the shoulder rises an upper arm's length, so a body pushed up
    // off its belly lost its arms in the first centimetres and came up onto its knees with both
    // hands folded at the chest, 40cm in the air. The hand is the contact that bears a push-up;
    // it stays down while its arm has length to give (the slack hand's ceiling, solveIk).
    static const bool kElbowServesHand = std::getenv("IK_ELBOW_SERVES_HAND") != nullptr; // A/B probe
    // ... and A SEAT SERVES NO KNEE (2026-10-28; IK_SEAT_SERVES_LEGS): a pin on a joint of the
    // pelvis girdle — the pelvis bone resting on the floor, a thigh socket whose flesh does — is
    // above every leg joint on its path, and while a heavy body's belly rested on the floor in
    // her prone no knee of hers could be a contact under a hand drag: the hands slid forward from
    // under the chest, the sphinx arched the trunk, the knees — held by nothing — rose 3cm, and
    // the hips-up that followed found none within the seed band and planked her. (The base
    // figure's belly rides clear of the floor in its prone, so its knees were always its own.)
    // The seat and the knees are separate supports; a girdle pin serves nothing below it.
    static const bool kSeatServesLegs = std::getenv("IK_SEAT_SERVES_LEGS") != nullptr; // A/B probe
    for (const IkEffector& pin : m_pins) {
        if (!kSeatServesLegs && pin.node != c && isGirdleJoint(pin.node)) {
            continue;
        }
        for (int cur = c; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur != pin.node) {
                continue;
            }
            if (kElbowServesHand || cur == c || !isLimbTip(c) || isLimbTip(pin.node) ||
                isFootClass(pin.node)) {
                return true;
            }
            break; // (a tip under a pin that is neither a tip nor a foot: its own contact)
        }
    }
    return false;
}

void IkRig::seedPoseContacts(const std::vector<glm::vec3>& positions) {
    const int n = m_graph.nodeCount();
    if (static_cast<int>(positions.size()) != n || m_pinLive.size() != m_pins.size()) {
        return;
    }
    static const bool kOff = std::getenv("IK_NO_POSE_CONTACTS") != nullptr; // A/B probe
    if (kOff) {
        return;
    }
    const bool kTrace = kContactTrace;
    const float scale = m_sizeScale;
    bool changed = false;
    // A figure SITTING ON THE FLOOR whose trunk is dragged (her seat a candidate below): a hand
    // that merely HANGS to the floor beside her — the arm straight, pointing down, or one the
    // Armature saw come down by itself while its arm hung (setIncidentalContacts) — is no support
    // of hers, it is where a hanging arm ends on the longer-armed generations. Seeded, it was a
    // prop: laid down on her back by the chest, the planted hand under a shoulder on its way to
    // the floor folded the arm one way and the other, 10-20cm a tick. (The Armature's hang keeps
    // such an arm off the floor while the seat rolls; a hand PLACED on the floor — the arm bent,
    // or reaching out — is a contact as ever.)
    static const bool kSeedHanging = std::getenv("IK_SEED_HANGING_HANDS") != nullptr; // A/B probe
    bool seated = false;
    const bool upperBodyDrag = m_effector >= 0 && m_effector != m_pelvis && effectorIsTrunk() &&
                               m_bindPos[static_cast<std::size_t>(m_effector)].y >
                                   m_bindPos[static_cast<std::size_t>(m_pelvis)].y + 0.05f * scale;
    if (!kSeedHanging && upperBodyDrag) {
        for (int c = 0; c < n && !seated; ++c) {
            seated = m_bodyNode[static_cast<std::size_t>(c)] && isGirdleJoint(c) && c != m_effector &&
                     contactHeight(c, positions) <= kSeedContactBand * scale;
        }
    }
    m_seatedTrunkDrag = seated;
    const auto inArm = [&](int c) { return limbHangsAboveHips(c); };
    const auto hangsToTheFloor = [&](int c) {
        const int junction = limbJunction(c);
        int   socket = c;
        float path = 0.0f;
        while (m_parents[static_cast<std::size_t>(socket)] >= 0 && m_parents[static_cast<std::size_t>(socket)] != junction) {
            const int up = m_parents[static_cast<std::size_t>(socket)];
            path += glm::length(positions[static_cast<std::size_t>(socket)] - positions[static_cast<std::size_t>(up)]);
            socket = up;
        }
        const glm::vec3 span = positions[static_cast<std::size_t>(c)] - positions[static_cast<std::size_t>(socket)];
        const float     straight = glm::length(span);
        if (kTrace && socket != c) {
            std::fprintf(stderr, "[ikrig]   node=%d limb from %d (junction %d): straight %.3f of path %.3f, down share %.2f, seated %d", c, socket,
                         junction, straight, path, straight > 1.0e-4f ? -span.y / straight : 0.0f, seated ? 1 : 0);
            std::fputc(10, stderr);
        }
        return socket != c && path > 0.20f * scale && straight > 0.95f * path && -span.y > 0.5f * straight;
    };
    // updateContacts' DETECT rule, for the joints plantContacts left out: real mass, not
    // foot-class, not the drag's own limb (except under a pelvis drag), not under a pin.
    for (int c = 0; c < n; ++c) {
        const std::size_t ci = static_cast<std::size_t>(c);
        const bool realMass =
            m_masses[ci] >= kTokenBoneMass || m_subtreeMass[ci] >= kTokenLimbMass;
        const bool rootSeat = c == m_pelvis && m_rootIsSeat; // (the root descends from nothing: it is the seat by itself)
        if (!m_bodyNode[ci] || (c == m_pelvis && !rootSeat) || isEffector(c) || !realMass ||
            (!descendsFromPelvis(c) && !rootSeat) || isFootClass(static_cast<int>(ci)) || helperNode(c)) {
            continue; // (helperNode: a twist helper is no contact — the segment's far joint is; see ikrig.h; m_rootIsSeat: a rig with no pelvis bone sits on its root)
        }
        const float h = contactHeight(c, positions);
        if (kTrace && h < 0.08f * scale) {
            std::fprintf(stderr, "[ikrig]   seed candidate node=%d h=%.4f clearance=%.3f y=%.3f\n", c, h, m_floorClearance[ci],
                         positions[ci].y);
        }
        if (h > kSeedContactBand * scale) {
            continue;
        }
        // (liftedByDrag, not the bare reach: a hand on the floor BELOW a dragged elbow or
        // shoulder stays a contact, as a foot below a dragged knee stays planted.)
        // (... but THE SEAT is a contact whatever is dragged: a pelvis that rests on the floor is
        // what a sitting figure is held up by, and the chest, 35cm up the spine, has it inside
        // its "own limb's" reach. Unseeded, a sitting figure stood on her feet alone to the rig,
        // 40cm out of balance — and reclined by the chest, her arms were flung out as
        // counterweights the tick her hands reached the floor: 70cm at the fingertips.)
        if (m_effector != m_pelvis && liftedByDrag(c) && !isGirdleJoint(c)) {
            continue;
        }
        if (m_effector == m_pelvis && isGirdleJoint(c)) {
            continue; // (the pelvis's own girdle is no contact of a pelvis drag: see updateContacts)
        }
        // (... nor a SPINE joint, 2026-10-29: a chest that comes down onto the floor under a prone
        // descent rests and slides as the hips are taken on — pinned in place it stretched the trunk
        // between itself and the moving hips, and the head whipped 90mm a tick over it. The floor's
        // rows keep the trunk on the floor; a trunk is no support under its own pelvis's drag.)
        if (m_effector == m_pelvis && isSpineJoint(c)) {
            continue;
        }
        // (... nor is her head, or her neck — see updateContacts: a face on the floor rests, it props
        // nothing, and seeded here under a hip drag up off her belly it held her head down.)
        if (!kHeadProps && m_effector == m_pelvis && m_headNode >= 0 && m_neckBase >= 0) {
            bool headChain = false;
            for (int cur = c; cur >= 0 && !headChain; cur = m_parents[static_cast<std::size_t>(cur)]) {
                headChain = cur == m_neckBase;
            }
            if (headChain) {
                continue;
            }
        }
        // ... and so is ANY joint of an arm, while her trunk is dragged: rolled back onto her back
        // or brought up again, a hand held to its spot on the floor is a prop under a shoulder on
        // its way down — gripping and letting go on alternate ticks, 10cm at the fingertips. The
        // Armature carries her idle arms clear of the floor meanwhile (hangIdleArms); a hand that
        // must STAY is pinned (a user pin is no live contact, and holds).
        const bool incidental = ci < m_incidentalContact.size() && m_incidentalContact[ci];
        if (!isGirdleJoint(c) && seated && (incidental || hangsToTheFloor(c) || inArm(c))) {
            if (kTrace) {
                std::fprintf(stderr, "[ikrig]   node=%d hangs to the floor beside a seated figure: no contact\n", c);
            }
            continue;
        }
        if (servedByPinAbove(c) || servedByHandBelow(c, positions)) {
            continue;
        }
        glm::vec3 target = positions[ci];
        target.y -= std::min(h, 0.0f); // never below its clearance; a hover keeps its height
        if (kTrace) {
            std::fprintf(stderr, "[ikrig] CONTACT from the pose node=%d h=%.4f at(%.3f %.3f %.3f)\n", c, h,
                         target.x, target.y, target.z);
        }
        addLivePin(c, target, positions);
        // A contact BELOW the dragged joint (a hand on the floor under a dragged elbow) is HARD:
        // held in full, as the knee over its planted foot is. A live contact is unilateral — its
        // hold on its place goes as it leaves the floor — and nothing is cheaper for a solve
        // asked to swing the elbow above it than to lift the hand two centimetres and carry it
        // along: on all fours an elbow swung out 6cm took its hand 5cm with it. (It is the
        // joint the elbow swivels ABOUT; the body's own rise still lifts it off through the rig's
        // lift-off rule, which reads the limb's tautness, not this flag.)
        if (pinUnderEffector(m_pins.size() - 1)) {
            m_pins.back().hard = true;
        }
        changed = true;
    }
    if (changed) {
        rebuildSupportHull(-1);
    }
}

bool IkRig::pinIsShadow(std::size_t pin) const {
    return pin < m_pins.size() && pin < m_pinLive.size() && m_pinLive[pin] && pin < m_liveErrTicks.size() &&
           m_liveErrTicks[pin] >= kLiveContactReleaseTicks;
}

float IkRig::kneelSeatHeight() const {
    return kneelSeat().y;
}

glm::vec3 IkRig::kneelSeat() const {
    // The point the hips sit at kneeling upright: the root's bind offset from the knee, applied at
    // each planted knee's contact (the thighs vertical over the knees) — the mean across the knees
    // for x/z, the highest for y (the one kneelSeatHeight always gave).
    float seat = -1.0f;
    glm::vec3 across(0.0f);
    int       knees = 0;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        const int node = m_pins[p].node;
        if (p >= m_pinLive.size() || !m_pinLive[p]) {
            continue;
        }
        if (isLegJointAboveFoot(node)) { // (a planted KNEE, or a thigh: a leg joint over its foot)
            const glm::vec3 over = m_pins[p].target + (m_bindPos[static_cast<std::size_t>(m_pelvis)] - m_bindPos[static_cast<std::size_t>(node)]);
            seat = std::max(seat, over.y);
            across += over;
            ++knees;
        }
    }
    if (knees == 0) {
        return glm::vec3(0.0f, -1.0f, 0.0f);
    }
    across /= static_cast<float>(knees);
    return glm::vec3(across.x, seat, across.z);
}

bool IkRig::computeIsLegJointAboveFoot(int node) const {
    // A leg joint with a foot-class joint below it — a knee, a thigh (onKnees' test for one contact).
    const int n = static_cast<int>(m_parents.size());
    if (node < 0 || node == m_pelvis || isGirdleJoint(node) || !descendsFromPelvis(node) ||
        isFootClass(node)) {
        return false;
    }
    for (int i = node + 1; i < n; ++i) {
        if (!isFootClass(i)) {
            continue;
        }
        for (int cur = m_parents[static_cast<std::size_t>(i)]; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur == node) {
                return true;
            }
        }
    }
    return false;
}

int IkRig::computeLegOf(int node) const {
    // The leg a joint belongs to: its ancestor that is a child of the pelvis girdle with a foot
    // below it (the thigh socket), or -1 for a joint of no leg.
    int cur = node;
    while (cur >= 0) {
        const int par = m_parents[static_cast<std::size_t>(cur)];
        if (par >= 0 && (par == m_pelvis || isGirdleJoint(par)) && isLegJointAboveFoot(cur)) {
            return cur;
        }
        cur = par;
    }
    return -1;
}

bool IkRig::onKnees() const {
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        // (A LIVE contact on a leg joint above its foot: a knee. Not the SEAT — isLegJointAboveFoot
        // excludes the girdle: the pelvis bone rests on the floor in a sit with both legs below it.)
        if (p < m_pinLive.size() && m_pinLive[p] && isLegJointAboveFoot(m_pins[p].node)) {
            return true;
        }
    }
    return false;
}

bool IkRig::kneeOver(int joint, int pinned) const {
    // The joint the pinned one hangs from: its nearest ancestor with a real length between them
    // (leaf helpers and zero-length links aside).
    static const bool kAnyAncestor = std::getenv("IK_KNEE_ANY_ANCESTOR") != nullptr; // A/B probe
    if (joint < 0 || pinned < 0 || joint == m_pelvis) {
        return false;
    }
    for (int cur = m_parents[static_cast<std::size_t>(pinned)]; cur >= 0 && cur != m_pelvis;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        if (cur == joint) {
            return true;
        }
        if (!kAnyAncestor &&
            glm::length(m_bindPos[static_cast<std::size_t>(cur)] - m_bindPos[static_cast<std::size_t>(pinned)]) > 0.05f * m_sizeScale) {
            return false; // (a real joint above the pinned one, and it is not this one)
        }
    }
    return false;
}

void IkRig::reseatPin(std::size_t pin, float x, float z) {
    if (pin >= m_pins.size() || (pin < m_pinUser.size() && m_pinUser[pin])) {
        return;
    }
    m_pins[pin].target.x = x;
    m_pins[pin].target.z = z;
    rebuildSupportHull(-1);
}

void IkRig::reseatLivePin(std::size_t pin, float x, float z) {
    if (pin >= m_pins.size() || pin >= m_pinLive.size() || !m_pinLive[pin]) {
        return;
    }
    m_pins[pin].target.x = x;
    m_pins[pin].target.z = z;
    rebuildSupportHull(-1);
}

void IkRig::boundLivePins(const std::vector<glm::vec3>& positions) {
    if (m_pinLive.size() != m_pins.size() || static_cast<int>(positions.size()) != m_graph.nodeCount()) {
        return;
    }
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (!m_pinLive[p]) {
            continue;
        }
        IkEffector& pin = m_pins[p];
        if (pinUnderEffector(p)) {
            pin.hard = true; // what a dragged elbow swivels about (see seedPoseContacts)
            continue;
        }
        // ... and THE PLANTED KNEE UNDER A KNEE DRAG (2026-09-23; IK_KNEE_DRAG_KNEES_SLIDE): a
        // kneeling figure's other knee is the whole body's support while one knee is drawn up,
        // and a person's does not slide. Soft — unilateral, sliding once dragged past 2.5cm — it
        // was where the drawn knee's unmet pull went once the lifted thigh could no longer roll
        // toward a goal the cursor put out of its reach: the planted knee slid 38-68mm along the
        // floor on four custom characters (22 on the male rig), the hips with it.
        static const bool kKneesSlide = std::getenv("IK_KNEE_DRAG_KNEES_SLIDE") != nullptr; // A/B probe
        static const bool kBoundTrace = std::getenv("IK_BOUND_TRACE") != nullptr;
        if (kBoundTrace) {
            std::fprintf(stderr, "[bound] pin node=%d legAboveFoot=%d effector=%d trunk=%d legOfEff=%d legOfPin=%d down=%d\n", pin.node,
                         isLegJointAboveFoot(pin.node) ? 1 : 0, m_effector, effectorIsTrunk() ? 1 : 0, legOf(m_effector), legOf(pin.node), m_lastDownIntent ? 1 : 0);
        }
        // (By the LEG the effector belongs to, not effectorIsTrunk: a shin's subtree — shin,
        // foot, toes — is over 5% of the body, so a dragged knee reads as a trunk grab there.)
        if (!kKneesSlide && isLegJointAboveFoot(pin.node) && !pinIsShadow(p) && m_effector >= 0 && m_effector != m_graph.root() &&
            legOf(m_effector) >= 0 && legOf(m_effector) != legOf(pin.node)) { // (not a SHADOW: a lifted knee is not nailed in the air)
            pin.hard = true;
            continue;
        }
        if (!m_lastDownIntent) {
            pin.leashRadius = 1e9f;
            pin.leashOffset = glm::vec3(0.0f);
            pin.hard = false;
            continue;
        }
        pin.leashRadius = socketLeash(pin.node, pin.target, positions, pin.leashOffset);
        // HARD when another pin sits RIGHT below it on its own limb — a KNEE over its planted
        // foot: the joint the pinned one hangs from, not any joint further up. (It was any
        // ancestor: sitting down out of a deep squat, the MID-THIGH joint — whose flesh is what
        // reaches the floor there — was taken for a landing knee, pinned hard on the side that
        // crossed the band first, and the pose went lopsided: a 12cm pop at the other shin.)
        bool pinBelow = false;
        for (std::size_t q = 0; q < m_pins.size() && !pinBelow; ++q) {
            pinBelow = q != p && !m_pinLive[q] && kneeOver(pin.node, m_pins[q].node);
        }
        pin.hard = pinBelow;
    }
}

void IkRig::landAt(const std::vector<glm::vec3>& applied) {
    const int n = m_graph.nodeCount();
    const float scale = m_sizeScale;
    std::vector<int> contacts;
    for (int i = 0; i < n; ++i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        if (m_bodyNode[ui] && applied[ui].y - m_groundOffsetY < kContactHeight * scale) {
            contacts.push_back(i);
        }
    }
    std::vector<int> users;
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (p < m_pinUser.size() && m_pinUser[p]) {
            users.push_back(m_pins[p].node);
        }
    }
    const std::vector<glm::vec3> priorBefore = m_startPose;
    plantContacts(contacts, applied, &users);
    // The prior after a landing is the DRAG-START pose moved horizontally under the landed
    // body — a figure that has just come down onto its feet stands back up where it landed
    // (see landAt): the landing pose itself as the prior kept the knees bent as they touched
    // down (the hip 5-7cm low for the rest of the drag on two rigs) and pulled the dragged arm
    // back toward the overhead stretch it landed with.
    if (priorBefore.size() == m_startPose.size() && m_pelvis >= 0) {
        const glm::vec3& rootNow = applied[static_cast<std::size_t>(m_pelvis)];
        const glm::vec3& rootWas = priorBefore[static_cast<std::size_t>(m_pelvis)];
        const glm::vec3 shift(rootNow.x - rootWas.x, 0.0f, rootNow.z - rootWas.z);
        for (std::size_t i = 0; i < m_startPose.size(); ++i) {
            m_startPose[i] = priorBefore[i] + shift;
        }
        m_stanceShift += shift; // the solve's root-translation reference lands with the body
    }
    m_justLanded = true;
    const bool kTrace = kContactTrace;
    if (kTrace) {
        std::fprintf(stderr, "[ikrig] LANDED: %zu contacts, %zu pins\n", contacts.size(),
                     m_pins.size());
    }
}

void IkRig::removePin(std::size_t index) {
    const auto erase = [index](auto& v) {
        if (index < v.size()) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(index));
        }
    };
    erase(m_pins);
    erase(m_pinUser);
    erase(m_pinLive);
    erase(m_liveErrTicks);
    erase(m_liveSpan);
    erase(m_pinSteppable);
    erase(m_pinStanceOffset);
    erase(m_pinFootprint);
    // (The pin indices the rig keeps: the foot in flight and the seat. A removed pin below them
    // shifts them down one; a removed pin that IS one clears it.)
    for (int* kept : {&m_stepPin, &m_seatPin}) {
        if (*kept == static_cast<int>(index)) {
            *kept = -1;
        } else if (*kept > static_cast<int>(index)) {
            --*kept;
        }
    }
}

void IkRig::rebuildActiveSet() {
    // Active subgraph: the paths joining effector and pins to the root. Everything else —
    // fingers during an arm drag, the face — rides along rigidly.
    std::vector<int> targets{m_effector};
    targets.insert(targets.end(), m_secondaries.begin(), m_secondaries.end()); // (THE GROUP's limbs are the solve's too)
    for (const IkEffector& pin : m_pins) {
        targets.push_back(pin.node);
    }
    m_active = m_graph.markActivePaths(targets);
}

bool IkRig::updateContacts(const std::vector<glm::vec3>& pose) {
    // (The rules below were written against two poses — the one the user sees and the solver's
    // own — when those differed; the solve is applied in full now, so they are the same.)
    const std::vector<glm::vec3>& applied = pose;
    m_lastAppliedPrev.swap(m_lastApplied); // (last tick's: is a knee coming down? — swapped, never reallocated)
    m_lastApplied = pose;
    const std::vector<glm::vec3>& previous = m_lastAppliedPrev;
    const std::vector<glm::vec3>& solved = pose;
    const int n = m_graph.nodeCount();
    if (!dragActive() || static_cast<int>(applied.size()) != n || m_pinLive.size() != m_pins.size() ||
        m_liveSpan.size() != m_pins.size() || m_contactBlocked.size() != static_cast<std::size_t>(n)) {
        return false;
    }
    const bool kTrace = kContactTrace;
    // IK_CONTACT_TRACE=2: every candidate joint within 5cm of the floor, per tick.
    static const bool kTraceNear = std::getenv("IK_CONTACT_TRACE") != nullptr &&
                                   std::getenv("IK_CONTACT_TRACE")[0] == '2';
    const float scale = m_sizeScale;
    // 0. LANDING (see isFootClass): a suspended figure whose foot has come back down to
    //    the floor stands again — every contact re-planted from where the body is now.
    if (m_suspended) {
        float lowestFoot = 1e9f;
        for (int i = 0; i < n; ++i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            if (m_bodyNode[ui] && !isEffector(i) && descendsFromPelvis(i) &&
                isFootClass(static_cast<int>(ui))) {
                lowestFoot = std::min(lowestFoot, contactHeight(i, applied));
            }
        }
        if (lowestFoot > kLandingAirborne * scale) {
            m_airborne = true; // hanging clear of the floor: a later touch-down is a landing
        }
        if (!m_airborne || lowestFoot > kLiveContactBand * scale) {
            return false;
        }
        landAt(applied);
        return true;
    }
    bool changed = false;   // pins added or removed: the active set and stiffness follow
    bool hullDirty = false; // a pin slid: the support polygon follows
    // 1. LIFT-OFF and SLIDE (see the constants): a live contact the solver holds above its pin
    //    (sustained) is let go, and its joint must clear the floor before it can plant again;
    //    one the solver holds beyond the slip strain along the floor creeps with the body.
    // (The contacts LIFTING this tick, judged TOGETHER: the shadow test below asks whether the
    // body still leans on a lifting contact, with the support drawn WITHOUT it — and read pin
    // by pin, the first hand of a pair to lift saw the other still in the support and let go,
    // and the second, checked against a support with no hand in it, was kept as a shadow: one
    // hand hanging, the other nailed to the floor, 341mm lopsided at the fingertips on a
    // symmetric rig. Both are now judged against the support without EITHER.)
    // NO LIFT-OFF UNDER A DESCENT (2026-09-23; IK_LIFT_OFF_DESCENDING): a body the user is
    // taking DOWN by the hips (m_lastDownIntent) does not let go of the hands and knees it comes
    // down onto. Two custom characters and the newest generation, whose arms stand as struts
    // under a prone descent, reared their trunk up on them — the hands rising 14cm while still
    // contacts, their holds faded with the rise — and the moment the arms read taut the hands
    // lifted off and the body came down 27-41cm in a tick, 38mm through the floor. Kept as
    // contacts they stall as the third generation does (the hips up on straight arms: a hand
    // walk this design has no gesture for), which is a pose and not a fall.
    static const bool kLiftOffDescending = std::getenv("IK_LIFT_OFF_DESCENDING") != nullptr; // A/B probe
    const bool descending = !kLiftOffDescending && m_effector == m_pelvis && m_lastDownIntent;
    std::vector<char> lifting(m_pins.size(), 0);
    for (std::size_t p = 0; p < m_pins.size(); ++p) {
        if (p >= m_pinLive.size() || !m_pinLive[p] || p >= m_liveSpan.size() || p >= m_liveErrTicks.size()) {
            continue;
        }
        const int   node = m_pins[p].node;
        const float reach = glm::length(solved[static_cast<std::size_t>(limbJunction(node))] - m_pins[p].target);
        // (A hand whose unload is still EASING IN — riseUnloading, 2026-10-27 — is not taut by its
        // reach yet: released on the reach alone with its hold at 1e5, the trunk it had held down
        // sprang 83mm at the brow in the tick the pin went. It is taut once the solve says so.)
        // (... and the descent's rule stands down for a TRAILING hand the solve is letting go of —
        // trailReleased, 2026-10-29: a leash the body slid forward over, not a support it comes down onto.)
        lifting[p] = (!descending || trailReleased(node)) && (kLiftNoDefer || !liftOffDeferred(node)) &&
                     ((((reach > kLiveContactTaut * m_liveSpan[p] && !riseUnloading(node)) || riseUnloaded(node)) &&
                       liveContactRise(p, solved) > kLiveContactLift * scale && m_liveErrTicks[p] + 1 >= kLiveContactReleaseTicks) ||
                      // (A hold the rise has taken OUTRIGHT holds nothing: let go of at once, whatever
                      // the height — see riseUnloadedOutright. A prone-up's hand pressed to the floor
                      // under a rising chest never rose the 2cm, and the weightless arm was thrown.)
                      riseUnloadedOutright(node));
    }
    for (std::size_t p = 0; p < m_pins.size();) {
        if (!m_pinLive[p]) {
            ++p;
            continue;
        }
        const int node = m_pins[p].node;
        const glm::vec3& at = solved[static_cast<std::size_t>(node)];
        IkEffector& pin = m_pins[p];
        // A contact SETTLES: once its JOINT rests on the floor, that is the height it is planted at.
        // A hand comes down fingers first, its wrist a hand's length up, and then lies down on its
        // palm, 13cm lower — and its rise, for the fade of its hold and for the lift-off below, was
        // still read over the height it TOUCHED at: taken all the way back up in the same drag, a
        // figure on all fours kept both hands on the floor, the wrists back up on their fingertips
        // but never 2cm over where they had landed, and her trunk folded 19 degrees further at the
        // hips, the lower spine at its limit, to leave them there. (Only AT REST on the floor: a
        // planted height that followed the wrist all the way down read every centimetre the wrist
        // bobbed back up on its way as a rise — the hold and the palm's rows let go mid-roll, an arm
        // swung 38cm up and came down again, 24-36cm in a tick.)
        static const bool kNoSettle = std::getenv("IK_CONTACT_NO_SETTLE") != nullptr; // A/B probe
        const float restsAt = m_groundOffsetY + m_floorClearance[static_cast<std::size_t>(node)];
        if (!kNoSettle && !pin.hard && at.y < pin.target.y && at.y - restsAt < kLiveContactBand * scale) {
            pin.target.y = std::min(pin.target.y, std::max(at.y, restsAt));
        }
        // (The JUNCTION's distance from the target over the chain's rest span — 1 only once the
        // body has left the limb's reach altogether, whatever the limb's own straightness: a
        // hand that lands from a hanging arm is dead straight too, and on liveContactStretch —
        // 1 for a straight limb — the male rig's landing hands lifted off as they landed.)
        // (... OR a hand the solve's RISE has UNLOADED (setRiseUnloadedContacts; 2026-09-26): the solve
        // lets a hand's hold go with the body's rise when its arm has no length to give by EITHER
        // measure — the arm's own stretch or this reach — and a straight arm on a collar shrugged
        // forward reads taut by the stretch (0.98) and slack by the reach (0.81), so its hand lay
        // weightless on the floor, then rode the rising chest 17cm into the air with its pin
        // standing — neither the solve's nor the hang's — until the torso's volume rows swung the
        // free arm 144mm in one tick (a heavy newest-generation character getting up off her
        // belly). Unloaded, it is taut for the lift-off's purposes; a landing hand is never one.)
        const int   junction = limbJunction(node);
        const float reach = glm::length(solved[static_cast<std::size_t>(junction)] - pin.target);
        const bool  taut = (p < m_liveSpan.size() && reach > kLiveContactTaut * m_liveSpan[p] && !riseUnloading(node)) || riseUnloaded(node);
        const bool  outright = riseUnloadedOutright(node); // (a hold the rise took outright: let go of now — see lifting[] above)
        if ((!descending || trailReleased(node)) && (kLiftNoDefer || !liftOffDeferred(node)) && (outright || (taut && liveContactRise(p, solved) > kLiveContactLift * scale))) {
            if (++m_liveErrTicks[p] >= kLiveContactReleaseTicks || outright) {
                // In the air. Is the body still LEANING on it? Removed the tick it lifts, a
                // kneeling figure's support went from knees-and-feet to the feet alone in one
                // tick with her weight a third of a metre ahead of them, and the balance row
                // hauled the hips back over the feet at 2.4cm a tick — faster, and higher, than
                // the chest the user held: the spine was crushed between the two, folded to its
                // limits and twisted sideways, and a fingertip jumped 56cm in a tick. A lifted
                // contact now stays a SHADOW of a support — holding nothing (its hold has
                // faded with its rise), its footprint following the joint — until the centre of
                // mass stands inside the support without it; a rising knee comes over its own
                // foot, so the support narrows as she stands and the shadow goes unnoticed.
                // Not under a pelvis drag (the solve has no balance row there).
                // (... and a shadow EXPIRES once its joint is kShadowMaxRise off the floor: nothing
                // leans on a knee 10cm in the air. Kept while the body would be unbalanced without
                // it, the knees of a figure pulled up by the chest out of a FLAT kneel — her feet a
                // foot's length further back than a tucked kneel's, her weight 27cm ahead of them —
                // stayed supports half a metre up: the balance need read zero, no step ever came,
                // and she stood leaning on straight legs on tiptoe. IK_SHADOW_NO_EXPIRY.)
                // (The whole PIN goes with the expiry, not its footprint alone: kept as a shadow of
                // nothing while the body leaned, a lifted knee still counted as a knee on the floor to
                // the solve's kneeling-foot rules — since guarded by pinIsShadow — and a figure rising
                // by the chest off a kneel jumped 97-105mm where she jumps 52 with the pin gone.)
                static const bool kNoShadow = std::getenv("IK_NO_SHADOW_SUPPORT") != nullptr; // A/B probe
                static const bool kNoExpiry = std::getenv("IK_SHADOW_NO_EXPIRY") != nullptr;  // A/B probe
                const bool expired = !kNoExpiry && liveContactRise(p, solved) > kShadowMaxRise * scale;
                if (!kNoShadow && !expired && m_effector != m_pelvis) {
                    std::vector<glm::vec2> without;
                    for (std::size_t q = 0; q < m_pins.size(); ++q) {
                        static const bool kLiftOneByOne = std::getenv("IK_LIFT_ONE_BY_ONE") != nullptr; // A/B probe
                        if (q == p || static_cast<int>(q) == m_stepPin || (!kLiftOneByOne && q < lifting.size() && lifting[q])) {
                            continue;
                        }
                        const glm::vec2 t(m_pins[q].target.x, m_pins[q].target.z);
                        for (const glm::vec2& off : m_pinFootprint[q]) {
                            without.push_back(t + off);
                        }
                    }
                    glm::vec2 need(0.0f);
                    const std::vector<glm::vec2> hull = BalanceController::supportPolygon(std::move(without));
                    if (!hull.empty()) {
                        BalanceController::balanceCorrection(solved, m_parents, m_masses, hull,
                                                             kBalanceMargin * scale, need);
                    }
                    if (hull.empty() || glm::length(need) > 1.0e-4f) {
                        pin.target.x = at.x;
                        pin.target.z = at.z;
                        hullDirty = true;
                        ++p;
                        continue;
                    }
                }
                // (An expired shadow's pin goes at ONCE. Two gentler expiries were tried and taken
                // out the same day, each measured on the eight rigs: the footprint alone withdrawn
                // with the pin left standing, and the footprint drawn in toward the support without
                // it at a centimetre a tick with the pin going once it had arrived — both left the
                // base rig's and the male's kneel-up by the chest jumping 97-105mm where the plain
                // removal jumps 52, and neither moved the third generation's 175.)
                if (kTrace) {
                    std::fprintf(stderr, "[ikrig] CONTACT lift-off node=%d dy=%.4f reach=%.3f span=%.3f\n",
                                 node, at.y - pin.target.y, reach, m_liveSpan[p]);
                }
                m_contactBlocked[static_cast<std::size_t>(node)] = 1;
                removePin(p);
                changed = true;
                continue;
            }
        } else {
            if (m_liveErrTicks[p] >= kLiveContactReleaseTicks) {
                // A shadow support (above) whose joint came back down: it plants where it IS.
                pin.target.x = at.x;
                pin.target.z = at.z;
                hullDirty = true;
            }
            m_liveErrTicks[p] = 0;
        }
        const glm::vec2 slip(at.x - pin.target.x, at.z - pin.target.z);
        const float slipLen = glm::length(slip);
        if (slipLen > kLiveContactSlip * scale) {
            const glm::vec2 move = slip * (1.0f - kLiveContactSlip * scale / slipLen);
            pin.target.x += move.x;
            pin.target.z += move.y;
            hullDirty = true;
            if (kTrace) {
                std::fprintf(stderr, "[ikrig] CONTACT slide node=%d by=%.4f to(%.3f %.3f)\n", node,
                             glm::length(move), pin.target.x, pin.target.z);
            }
        }
        ++p;
    }
    // 2. DETECT: a real-mass body joint (token-mass parts — fingers, toes, face bones — count
    //    through the joint they ride, never on their own) that is not the effector, not within
    //    the drag's own lift reach of it (the same rule that frees a dragged foot's toes at
    //    drag start), not already served by a pin at or above it (a planted foot's toes), and
    //    whose contact height has reached the floor. A pin BELOW it does not exclude it: a knee
    //    coming down onto the floor with its foot still planted is a second support on that
    //    leg (a kneel), and the solver restores the foot's chain through the knee pin.
    for (int c = 0; c < n; ++c) {
        const std::size_t ci = static_cast<std::size_t>(c);
        // A real-mass joint by its own segment OR by what hangs off it (a hand whose class
        // share a helper bone halved still carries its fingers) — the promotion rule's pair.
        const bool realMass =
            m_masses[ci] >= kTokenBoneMass || m_subtreeMass[ci] >= kTokenLimbMass;
        const bool rootSeat = c == m_pelvis && m_rootIsSeat; // (the root descends from nothing: it is the seat by itself)
        if (!m_bodyNode[ci] || (c == m_pelvis && !rootSeat) || isEffector(c) || !realMass ||
            (!descendsFromPelvis(c) && !rootSeat) || helperNode(c)) {
            continue; // (helperNode: a twist helper is no contact — the segment's far joint is; see ikrig.h; m_rootIsSeat: a rig with no pelvis bone sits on its root)
        }
        const float h = contactHeight(c, applied);
        if (kTraceNear && h < 0.05f * scale) {
            std::fprintf(stderr, "[ikrig]   near node=%d h=%.4f blocked=%d reach=%.3f\n", c, h,
                         m_contactBlocked[ci] ? 1 : 0, pathLenToEffector(c));
        }
        if (m_contactBlocked[ci]) {
            if (h > kLiveContactRearm * scale) {
                m_contactBlocked[ci] = 0;
            }
            continue;
        }
        // The contact band. A joint ABOVE a still-planted pin — a knee over its pinned foot —
        // plants from kKneelContactBand while the drag takes the body DOWN: it becomes a HARD
        // pin there (boundLivePins), held in full, so the solve brings it onto the floor (a
        // kneel). Its geometry otherwise leaves a knee hovering 3-4cm up: the hip's target is
        // reachable on the balls of the feet without the knees ever touching.
        float band = kLiveContactBand * scale;
        // ... a knee over a planted FOOT (a drag-start pin, not another live contact: a knee that
        // had just landed made a "knee" of the mid-thigh joint above it), and one that is COMING
        // DOWN: sitting back out of a deep squat the knees hover within the band and RISE, and one
        // of them was pulled down onto the floor all the same — the other shin 12cm in one tick.
        if (m_lastDownIntent) {
            const bool comingDown = ci < previous.size() && applied[ci].y < previous[ci].y - 2.0e-4f * scale;
            for (std::size_t q = 0; q < m_pins.size(); ++q) {
                if (!m_pinLive[q] && comingDown && kneeOver(c, m_pins[q].node)) {
                    band = kKneelContactBand * scale;
                }
            }
        }
        if (h > band) {
            continue;
        }
        // The drag's own limb never plants itself (the lift-reach rule of beginDrag) — except
        // under a ROOT drag, whose "limb" is the whole body: the knees are within that reach
        // of the pelvis, and a kneel is exactly a hip drag bringing them down.
        if (m_effector != m_pelvis && pathLenToAnyEffector(c) <= kEffectorLiftReach * scale && !isGirdleJoint(c)) {
            continue; // (the SEAT is a contact whatever is dragged: see seedPoseContacts)
        }
        // ... but the PELVIS ITSELF is no contact of its own drag: the root and the joints of its
        // girdle (the pelvis bone, the first spine bones, the thigh sockets — isGirdleJoint) go
        // where the cursor takes them. With floor clearances fitted to the skin they do reach the
        // floor in a sit — that is the point: the floor rows stop them there — and planted, a
        // thigh socket's hold in the floor's plane fought the cursor that had the hips: on the
        // oldest generation a sit tipped the pelvis 72 degrees back, 7cm off its cursor.
        if (m_effector == m_pelvis && (c == m_pelvis || isGirdleJoint(c))) {
            continue;
        }
        // ... nor is her HEAD, or her neck: a body lowered forward off its knees by the hips puts its
        // face on the floor before its belly, and planted there — a hard hold of the head's place
        // with the hips still 30cm up — it stood on its head, a tripod of knees and face, the hips
        // frozen 40cm from their cursor. A head on the floor rests; it props nothing.
        if (!kHeadProps && m_effector == m_pelvis && m_headNode >= 0 && m_neckBase >= 0) {
            bool headChain = false;
            for (int cur = c; cur >= 0 && !headChain; cur = m_parents[static_cast<std::size_t>(cur)]) {
                headChain = cur == m_neckBase;
            }
            if (headChain) {
                continue;
            }
        }
        if (m_effector == m_pelvis && isSpineJoint(c)) {
            continue; // (nor a SPINE joint under a pelvis drag: a chest lying down rests and slides — see seedPoseContacts)
        }
        // (A figure seated on the floor, her trunk dragged: her arms are no supports — see
        // seedPoseContacts.)
        if (m_seatedTrunkDrag && !isGirdleJoint(c) && limbHangsAboveHips(c)) {
            continue;
        }
        if (servedByPinAbove(c) || servedByHandBelow(c, applied)) {
            continue;
        }
        // A FOOT touching down while unpinned — the second foot of a landing out of
        // suspension (the feet dangle at different heights, and the landing re-plants only
        // what is within the contact height) — STANDS again: the whole contact set is
        // re-planted as at a drag start (ground-healed, leashed, steppable, the flat-sole hold
        // re-armed), not held as a unilateral live contact. Only while no live contact
        // exists: a re-plant would turn a planted hand into a standing-type pin.
        if (isFootClass(static_cast<int>(ci))) {
            bool anyLive = false;
            for (const char live : m_pinLive) {
                anyLive = anyLive || live != 0;
            }
            if (!anyLive) {
                landAt(applied);
                return true;
            }
        }
        glm::vec3 target = applied[ci];
        target.y -= h; // the lowest riding part exactly at its clearance (h <= band: a lift)
        if (kTrace) {
            std::fprintf(stderr, "[ikrig] CONTACT plant node=%d h=%.4f at(%.3f %.3f %.3f)\n", c,
                         h, target.x, target.y, target.z);
        }
        addLivePin(c, target, applied);
        changed = true;
    }
    if (changed) {
        rebuildActiveSet();
    }
    if (changed || hullDirty) {
        rebuildSupportHull(m_stepPin);
    }
    return changed;
}

} // namespace pose
