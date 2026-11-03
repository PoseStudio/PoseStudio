/**
 * @file ikrigdrag.cpp
 * @brief IkRig::beginDrag — everything decided ONCE at the start of an interactive drag: the
 *        token-limb promotion, the planted-contact pins with their ground-healed targets and
 *        socket leashes, the user's hard pins, the support polygon and stepping footprints, the
 *        active subgraph, the soft pose prior with its per-node stiffness model, and the trunk
 *        chain.
 *
 * Pin targets and the pose prior are captured here and never re-read from the pose during the
 * drag: re-reading each tick would let any per-tick residual relocate the stance and ratchet
 * the figure across the floor. The order of the decisions matters and is documented inline — in
 * particular the effector is PROMOTED (a finger grab drives the hand, an eye grab the head) before
 * anything that classifies the gesture reads it. See ikrig.cpp for the TU layout; the constants
 * come from ikrig_constants.h. Qt-free (std + GLM).
 */

#include "ikrig.h"

#include "balancecontroller.h"
#include "ikrig_constants.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace pose {

bool IkRig::isGirdleJoint(int node) const {
    if (node < 0 || node >= static_cast<int>(m_parents.size())) {
        return false;
    }
    if (node == m_pelvis) {
        return true;
    }
    int depth = 0; // (within two bones of the solve root: the walk stops at two)
    int cur = node;
    while (cur >= 0 && cur != m_pelvis && depth < 2) {
        cur = m_parents[static_cast<std::size_t>(cur)];
        ++depth;
    }
    return cur == m_pelvis && m_subtreeMass[static_cast<std::size_t>(node)] > 0.05f &&
           pathLenToRoot(node) < kGirdleGrabReach * m_sizeScale;
}

bool IkRig::isSpineJoint(int node) const {
    return node >= 0 && node < static_cast<int>(m_parents.size()) && node != m_pelvis &&
           m_bodyNode[static_cast<std::size_t>(node)] && limbJunction(node) == m_pelvis && !hasFootBelow(node);
}

bool IkRig::computeHasFootBelow(int node) const {
    const int n = static_cast<int>(m_parents.size());
    if (node < 0 || node >= n) {
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

bool IkRig::isPelvisBone(int node) const {
    if (m_pelvisBoneNode == -2) {
        m_pelvisBoneNode = -1; // (found once per rig: the solve asks for every bone, every tick)
        for (int candidate = 0; candidate < static_cast<int>(m_parents.size()); ++candidate) {
            if (hangsBothLegs(candidate)) {
                m_pelvisBoneNode = candidate;
                break;
            }
        }
    }
    return node >= 0 && node == m_pelvisBoneNode;
}

bool IkRig::hangsBothLegs(int node) const {
    if (node == m_pelvis || !isGirdleJoint(node)) {
        return false;
    }
    const int n = static_cast<int>(m_parents.size());
    int legs = 0;
    for (int child = 0; child < n; ++child) {
        if (m_parents[static_cast<std::size_t>(child)] != node) {
            continue;
        }
        bool foot = false;
        for (int i = child; i < n && !foot; ++i) {
            if (!isFootClass(i)) {
                continue;
            }
            for (int cur = i; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
                if (cur == child) {
                    foot = true;
                    break;
                }
            }
        }
        legs += foot ? 1 : 0;
    }
    return legs >= 2;
}

bool IkRig::beginDrag(int effectorNode, const std::vector<glm::vec3>& positions,
                      const std::vector<int>& contactNodes, float groundOffsetY,
                      const std::vector<int>* userPins, const std::vector<int>* secondaries) {
    const int n = m_graph.nodeCount();
    if (!built() || m_pelvis < 0 || effectorNode < 0 || effectorNode >= n ||
        static_cast<int>(positions.size()) != n ||
        !m_bodyNode[static_cast<std::size_t>(effectorNode)]) {
        m_effector = -1;
        return false;
    }
    m_effector = effectorNode;
    m_grabbed = effectorNode;
    m_girdleGrab = false;
    m_seatPin = -1;
    m_groundOffsetY = groundOffsetY;

    // TOKEN-LIMB PROMOTION: when the grabbed joint sits in a token-mass extremity (clicking near
    // a hand almost always picks a FINGER; a face grab picks a nose/brow bone), the drag
    // EFFECTOR is promoted to the limb's first real-mass joint (the hand, the foot, the head) —
    // the Armature compensates the target by the grab offset, so the drag still tracks where the
    // user grabbed. This is what makes a finger pull behave as an ARM gesture, the way a person
    // raises an arm when tugged by one finger; the digits, no longer on the effector path, ride
    // along rigidly like any inactive subtree, and fingers stay posable by FK (plain drag /
    // gizmo). Solving with the FINGERTIP as the effector was tried in two forms and both left
    // the hand far short of a reachable overhead target: free finger joints absorbed the goal
    // (the knuckle hyperextended to its +50° limit while the arm stayed near-horizontal), and
    // per-drag RIGIDIFIED finger edges still equilibrated ~25cm short — the fingertip goal
    // under-recruited the collar (14° vs the hand-drag's 35° shrug), and no stiffness or prior
    // variant closed that gap. Driving the limb's real end joint IS the verified arm-raise path.
    // (Since 2026-09-28 a finger's or a toe's grab never reaches the rig — it is the Armature's
    // DIGIT DRAG, the digit alone — and a bone of the face rig selects the head: what is promoted
    // here is what is left, the small bones that are the limb's own flesh — a carpal, a mid-foot
    // bone, the ball of the foot, a pectoral on the chest. See Armature::boneClass.)
    while (m_effector != m_pelvis) {
        const int p = m_parents[static_cast<std::size_t>(m_effector)];
        if (p < 0 || m_masses[static_cast<std::size_t>(m_effector)] >= kTokenBoneMass ||
            m_subtreeMass[static_cast<std::size_t>(m_effector)] >= kTokenLimbMass) { // (real mass: the contact rule's test)
            break;
        }
        m_effector = p;
    }
    // A COLLAR under a SCOPED drag is the TRUNK's (2026-10-28; IK_SCOPED_COLLAR_STAYS is the A/B):
    // the shoulder girdle's bone hangs off a spine joint and is its own chain's whole length — its
    // limb junction is its parent — so Ctrl+dragging it turned the collar about its own joint and
    // moved nothing a user could see ("ctrl+click drag on chest_2 does nothing"), and the collars'
    // skin is the whole band of the upper chest above the breast line, in every view (a pixel scan
    // over the chest: a click there picks a collar as often as the chest). Promoted to the spine
    // joint it hangs from, as a pectoral is by its token mass, a Ctrl grab of the upper chest bends
    // the trunk down to the pelvis wherever it lands; the Armature measures the grab's offset off
    // the pose each tick, as for a pectoral. AND THE NECK'S LOWEST JOINT IS THE SAME CASE (the
    // user's third report the same night: the lit upper chest runs up the throat, a click there
    // picks the neck's base, and its chain — that one bone — moved nothing: head 0.0mm): it is
    // promoted too. The upper neck and the head are not: their chains run down through the neck
    // base, which bends, and Ctrl on the head or the upper neck bends the neck alone, by design.
    static const bool kScopedCollarStays = std::getenv("IK_SCOPED_COLLAR_STAYS") != nullptr; // A/B probe
    if (!kScopedCollarStays && m_scoped && m_effector != m_pelvis) {
        const int par = m_parents[static_cast<std::size_t>(m_effector)];
        if (par >= 0 && par != m_pelvis && limbJunction(m_effector) == par && isSpineJoint(par)) {
            m_effector = par;
        }
    }
    // PELVIS-GIRDLE PROMOTION: a joint within two bones and kGirdleGrabReach of the solve root —
    // the pelvis bone and the first spine bone (their origins sit 2cm from the root's on every
    // generation), the thigh sockets and the second spine bone — goes where the pelvis goes and
    // nowhere else: nothing between it and the root has leverage worth the name. Grabbing it IS
    // grabbing the pelvis, and it was not treated so: as an ordinary trunk drag it met the root's
    // horizontal price (4500/m^2 against the cursor's bounded pull: 2.2cm of hips for a 10cm
    // drag, the grab 5-8cm behind the cursor), with no sway, no hinge, no walk. And these are
    // the joints a click on the hips FINDS: the root's own body is two 2cm segments, the pelvic
    // flesh belongs to the pelvis bone's — in the app "dragging the hip" did next to nothing
    // sideways or fore and aft. The Armature keeps the GRABBED joint on the cursor (it measures
    // the grab's offset from the root on every tick: see Armature::dragIkTo).
    // (... but NOT a joint of the SPINE under a SCOPED drag — Ctrl on the lower abdomen, 2026-10-28:
    // the first and second spine bones are girdle grabs, and promoted to the pelvis they were the
    // FIGURE MOVE under Ctrl where the user wants the trunk bent down to the pelvis. The effector
    // stays the spine joint, and its scoped chain is the bones from it down to, not including,
    // the root. The pelvis bone and the thigh sockets — a foot below them — go to the pelvis as
    // ever: a Ctrl grab of the hips moves the figure. m_scoped is set before beginDrag for this.)
    if (m_effector != m_pelvis) {
        static const bool kNoGirdle = std::getenv("IK_NO_GIRDLE_GRAB") != nullptr; // A/B probe
        static const bool kScopedSpinePromotes = std::getenv("IK_SCOPED_SPINE_GIRDLE_PROMOTES") != nullptr; // A/B probe: as before 2026-10-28
        const bool spineUnderScope = !kScopedSpinePromotes && m_scoped && !hasFootBelow(m_effector);
        if (!kNoGirdle && isGirdleJoint(m_effector) && !spineUnderScope) {
            m_effector = m_pelvis;
            m_girdleGrab = true;
        }
    }
    // The grab point's chain length to the root is drag-invariant now that the effector is
    // fixed; the suspension gate reads it every tick.
    m_effectorChainLen = pathLenToRoot(m_effector);

    // Planted contacts: everything grounded, minus what the drag itself is lifting (contacts
    // within kEffectorLiftReach of the effector, measured as tree path length — see the constant
    // above), and minus non-body nodes (the origin-level figure node reads as "on the floor"
    // every frame). The path length is computed via the effector's ancestor chain (LCA): the
    // skeleton is a tree, so the effector->contact path runs up one side and down the other.
    // The chain is kept for the drag (pathLenToEffector): the live contact detection applies
    // the same reach rule every tick.
    m_effAncestor.clear();    // effector, its parent, ... up to the anatomical top
    m_effAncestorLen.clear(); // cumulative edge length from the effector to each
    {
        float acc = 0.0f;
        for (int cur = m_effector; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            m_effAncestor.push_back(cur);
            m_effAncestorLen.push_back(acc);
            if (m_parents[static_cast<std::size_t>(cur)] >= 0) {
                acc += m_edgeRestLen[static_cast<std::size_t>(cur)];
            }
        }
    }
    // THE GROUP's secondaries (2026-10-27): promoted as the effector is — a token-mass grab to its
    // limb's real-mass joint, a girdle joint to the root — one of each, never the effector itself;
    // each with its ancestor chain, for the lift reach (pathLenToAnyEffector, liftedByDrag) and the
    // active set.
    m_secondaries.clear();
    m_secAncestor.clear();
    m_secAncestorLen.clear();
    if (secondaries != nullptr) {
        for (int sec : *secondaries) {
            if (sec < 0 || sec >= n || !m_bodyNode[static_cast<std::size_t>(sec)]) {
                continue;
            }
            while (sec != m_pelvis) {
                const int p = m_parents[static_cast<std::size_t>(sec)];
                if (p < 0 || m_masses[static_cast<std::size_t>(sec)] >= kTokenBoneMass ||
                    m_subtreeMass[static_cast<std::size_t>(sec)] >= kTokenLimbMass) {
                    break;
                }
                sec = p;
            }
            if (sec != m_pelvis && isGirdleJoint(sec)) {
                sec = m_pelvis;
            }
            if (sec == m_effector || std::find(m_secondaries.begin(), m_secondaries.end(), sec) != m_secondaries.end()) {
                continue;
            }
            std::vector<int>   chain;
            std::vector<float> chainLen;
            float              acc = 0.0f;
            for (int cur = sec; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
                chain.push_back(cur);
                chainLen.push_back(acc);
                if (m_parents[static_cast<std::size_t>(cur)] >= 0) {
                    acc += m_edgeRestLen[static_cast<std::size_t>(cur)];
                }
            }
            m_secondaries.push_back(sec);
            m_secAncestor.push_back(std::move(chain));
            m_secAncestorLen.push_back(std::move(chainLen));
        }
    }
    // Ground contacts -> pins, the support polygon, the stepping state and the prior (shared
    // with a LANDING out of suspension — see plantContacts).
    plantContacts(contactNodes, positions, userPins, /*dragStart=*/true);
    maskOverlappingVolumes(positions);
    return true;
}

void IkRig::plantContacts(const std::vector<int>& contactNodes,
                          const std::vector<glm::vec3>& positions,
                          const std::vector<int>* userPins, bool dragStart) {
    const int n = m_graph.nodeCount();
    // A contact must DESCEND FROM THE PELVIS (the anatomical body tree). Merged follower
    // figures' scene nodes are appended as SIBLINGS of the pelvis under the figure node and sit
    // at the ORIGIN (y=0) — an eyelash follower's scene node read as "planted" and was pinned
    // there, the round-8 phantom-anchor bug returning through the addon path (its pin stalled
    // every drag and anchored the figure to the origin). Structure, not mass, is the test: the
    // individual toe bones split the "toe" mass share to near-token values, and a mass gate that
    // caught the follower node also dropped the toes from the support polygon (a smaller
    // footprint destabilized balance during ordinary arm drags).
    //
    // And it must be FOOT-CLASS (kFootBindHeight): a joint that is low because the figure stands
    // on it. A joint that is low because of the POSE — a kneeling knee, a hand on the floor —
    // is not an anchor of this kind: planted like a foot (held in full, leashed, steppable, and
    // as its limb's MOST PROXIMAL contact the only pin its limb got, the foot below it riding),
    // a figure left kneeling by one drag could not be stood up by the next. Pulled up by the
    // chest, the knees stayed nailed to the floor until the pull read as beyond the leashes'
    // reach and the whole figure was LIFTED off the floor, dangling; dragged up by the hip,
    // the stepper "stepped" the knee pins and she arrived 4cm short with a toe 4cm through the
    // floor. Within ONE drag a kneel had always ended differently: the feet pinned, the knees
    // LIVE contacts over them (unilateral: they lift off when the body rises). A drag now
    // starts in the state the last one ended in — seedPoseContacts, below.
    std::vector<int> planted;
    for (const int c : contactNodes) {
        if (c >= 0 && c < n && !isEffector(c) && m_bodyNode[static_cast<std::size_t>(c)] &&
            descendsFromPelvis(c) &&
            isFootClass(c) && !helperNode(c) && // (a helper by the rig standard is no foot)
            !liftedByDrag(c)) {
            planted.push_back(c);
        }
    }
    // A foot the user LIFTED stays lifted. The contact band is 15cm deep and the ground healing
    // below reaches 12 — made for a figure left hovering by millimetres of residue — so a foot
    // raised 5 or 10cm by one drag (a step being posed, a heel kicked up) was planted by the
    // NEXT drag of anything, a hand included, and hauled back down to the floor at the pin
    // easing's pace. A foot is LIFTED when its lowest joint stands more than kFootLiftedHeight
    // over its own floor height (a foot on its toes or its heel has a joint down: standing)
    // while another foot of the figure STANDS; with no foot standing the whole figure hovers,
    // and healing it is the point. At a drag's start only: a landing out of a lift plants what
    // comes down.
    if (dragStart && !planted.empty()) {
        const auto footOf = [&](int node) {
            int top = node;
            for (int cur = m_parents[static_cast<std::size_t>(node)];
                 cur >= 0 && m_bodyNode[static_cast<std::size_t>(cur)] && descendsFromPelvis(cur) &&
                 isFootClass(cur);
                 cur = m_parents[static_cast<std::size_t>(cur)]) {
                top = cur;
            }
            return top;
        };
        std::vector<int> foot(planted.size());
        std::vector<int> feet;
        std::vector<float> lift;
        const auto footIndex = [&feet](int f) { // (the foot's slot among the feet found so far)
            std::size_t k = 0;
            for (; k < feet.size() && feet[k] != f; ++k) {
            }
            return k;
        };
        for (std::size_t i = 0; i < planted.size(); ++i) {
            foot[i] = footOf(planted[i]);
            const float over = positions[static_cast<std::size_t>(planted[i])].y -
                               (m_bindPos[static_cast<std::size_t>(planted[i])].y + m_groundOffsetY);
            const std::size_t k = footIndex(foot[i]);
            if (k == feet.size()) {
                feet.push_back(foot[i]);
                lift.push_back(over);
            } else {
                lift[k] = std::min(lift[k], over);
            }
        }
        bool standing = false;
        for (const float over : lift) {
            standing = standing || over < kFootLiftedHeight * m_sizeScale;
        }
        static const bool kPlantLifted = std::getenv("IK_PLANT_LIFTED_FEET") != nullptr; // A/B probe
        if (standing && !kPlantLifted) {
            std::vector<int> kept;
            for (std::size_t i = 0; i < planted.size(); ++i) {
                if (lift[footIndex(foot[i])] < kFootLiftedHeight * m_sizeScale) {
                    kept.push_back(planted[i]);
                }
            }
            planted.swap(kept);
        }
    }

    // The solve traverses the ANATOMICAL hierarchy (rooted at the pelvis) so every constraint is
    // evaluated in its owning parent-side frame; ground anchoring is the pins' job. The pelvis
    // root FLOATS, held by the planted limbs — its solved displacement becomes the Armature's root
    // pose translation — except when nothing is planted (an airborne figure): then the root is
    // pinned at its current position, the classic anchored-pelvis fallback.
    m_graph.setRoot(m_pelvis);

    // Only the MOST PROXIMAL planted contact of each limb becomes a pin (the ankle — a contact
    // none of whose ancestors are themselves planted): pinning every toe/metatarsal joint
    // individually made the solver BEND the foot and toe joints to hold their pins whenever the
    // ankle shifted a few millimeters — visible toe curling and tiptoeing on gentle drags. With
    // only the ankle pinned, the foot and toes stay off the active set and ride along rigidly,
    // keeping their shape. The support polygon still spans ALL contacts (toes give the real
    // footprint). Each pin carries its reach-leash radius (see IkEffector::leashRadius): the DRAG-START
    // root-to-pin distance — a stance the figure provably held — with fractional path-length
    // headroom so a bent-leg start may still extend.
    m_pins.clear();
    std::vector<glm::vec2> support;
    support.reserve(planted.size());
    float snapSum = 0.0f;
    int snapCount = 0;
    for (const int c : planted) {
        // GROUND-HEALING pin targets: a contact hovering NEAR its natural floor height is pinned
        // AT that height (bind y), not where it currently hangs — every drag then re-plants
        // slightly-lifted feet instead of preserving (and compounding) the hover: pins that
        // anchored "wherever the foot is" let per-drag residuals stack until the figure floated.
        // Bounded: a joint far from its bind height (a kneeling knee, a deliberately airborne
        // pose) keeps its current height.
        glm::vec3 target = positions[static_cast<std::size_t>(c)];
        const float floorY = m_bindPos[static_cast<std::size_t>(c)].y + m_groundOffsetY;
        if (std::abs(target.y - floorY) < kGroundHealBand * m_sizeScale) {
            snapSum += floorY - target.y;
            ++snapCount;
            target.y = floorY;
        }
        support.emplace_back(target.x, target.z);
        bool ancestorPlanted = false;
        for (int cur = m_parents[static_cast<std::size_t>(c)]; cur >= 0 && !ancestorPlanted;
             cur = m_parents[static_cast<std::size_t>(cur)]) {
            for (const int other : planted) {
                if (other == cur) {
                    ancestorPlanted = true;
                    break;
                }
            }
        }
        if (ancestorPlanted) {
            continue;
        }
        // ... and the pin goes to the limb's most proximal FOOT-CLASS joint — the ANKLE — even
        // when that joint is not itself down: a foot standing on its toes (tucked under a kneel,
        // on tiptoe) is still a foot, and its model hangs off the ankle (the ball held hard, the
        // heel softly: Armature::solveIk). On the main figure family a tucked foot's ankle stays
        // under the contact height and was pinned anyway; on four other generations it rises
        // past it, the pin went to the TOES joint — no ball, no heel row, nothing to bring the
        // heel down — and a figure got up out of a kneel onto heels 2-4cm in the air.
        int pinNode = c;
        for (int cur = m_parents[static_cast<std::size_t>(c)];
             cur >= 0 && !isEffector(cur) && m_bodyNode[static_cast<std::size_t>(cur)] && descendsFromPelvis(cur) &&
             isFootClass(cur) &&
             !liftedByDrag(cur);
             cur = m_parents[static_cast<std::size_t>(cur)]) {
            pinNode = cur;
        }
        if (pinNode != c) {
            bool have = false;
            for (const IkEffector& existing : m_pins) {
                have = have || existing.node == pinNode;
            }
            if (have) {
                continue;
            }
            target = positions[static_cast<std::size_t>(pinNode)];
            const float ankleFloorY = m_bindPos[static_cast<std::size_t>(pinNode)].y + m_groundOffsetY;
            if (std::abs(target.y - ankleFloorY) < kGroundHealBand * m_sizeScale) {
                target.y = ankleFloorY; // (no part of the hover statistics: a pitched foot is not a hover)
            }
        }
        IkEffector pin{pinNode, target, 1.0f, -1.0f};
        pin.leashRadius = socketLeash(pinNode, target, positions, pin.leashOffset);
        m_pins.push_back(pin);
    }
    // USER pins: joints the user explicitly pinned are held exactly where they are — no ground
    // healing (the pin is wherever the user left it), no lift-reach release, and they are never
    // stepped (a pinned foot is intent, even when it stands on the ground). A pin on the
    // effector or inside its subtree would fight the drag itself, so it sits out this drag:
    // dragging a pinned hand simply moves the pin. A user pin that coincides with a contact
    // pin takes the contact's place with user semantics.
    m_pinUser.assign(m_pins.size(), 0);
    std::size_t userPinCount = 0;
    if (userPins != nullptr) {
        for (const int u : *userPins) {
            if (u < 0 || u >= n || isEffector(u) || !m_bodyNode[static_cast<std::size_t>(u)]) {
                continue;
            }
            if (m_girdleGrab && u == m_grabbed) {
                continue; // (the grabbed joint itself, promoted to the pelvis: dragging a pin moves it)
            }
            // The subtree exclusion is for LIMB drags (a pinned toe under a dragged foot would
            // fight the drag). A PELVIS drag has the whole body in its subtree — there the user
            // pins are exactly the anchors the drag must respect (a crouch under a held hand).
            // (... for the effector and for each of THE GROUP's secondaries: a pinned toe under a
            // second dragged foot would fight that foot's row as it would the first's.)
            const auto inSubtreeOf = [&](int eff) {
                if (eff == m_pelvis) {
                    return false;
                }
                // (... and a SCOPED TRUNK drag — Ctrl on the chest, the abdomen, the neck, 2026-10-28:
                // the chain alone moves, and a pin below it is what the user asked to hold — the
                // solve serves it with the pinned limb's own bones, which hang below the effector
                // and are unknowns for that: a pinned hand stays while the chest bends toward it and
                // its arm folds, and when the arm runs out the pin stops the chest. A PLAIN trunk
                // drag leaves such a pin out, as before: the body follows the chest there.)
                if (m_scoped && eff == m_effector && effectorIsTrunk()) {
                    return false;
                }
                for (int cur = m_parents[static_cast<std::size_t>(u)]; cur >= 0;
                     cur = m_parents[static_cast<std::size_t>(cur)]) {
                    if (cur == eff) {
                        return true;
                    }
                }
                return false;
            };
            static const bool kFixedHoldReach = std::getenv("IK_PIN_HOLD_FIXED_REACH") != nullptr; // A/B probe
            const float limbLength = pathLenToRoot(u) - pathLenToRoot(limbJunction(u));
            const float holdReach = kFixedHoldReach ? kEffectorPinHoldReach * m_sizeScale
                                                    : std::min(kEffectorPinHoldReach * m_sizeScale, kEffectorPinHoldShare * limbLength);
            bool sitsOut = inSubtreeOf(m_effector) && (!belowEffectorInItsLimb(u) || pathLenToEffector(u) < holdReach);
            for (std::size_t k = 0; k < m_secondaries.size() && k < m_secAncestor.size() && !sitsOut; ++k) {
                sitsOut = inSubtreeOf(m_secondaries[k]) &&
                          (!belowInItsLimb(u, m_secondaries[k]) ||
                           pathLenToChain(u, m_secAncestor[k], m_secAncestorLen[k]) < holdReach);
            }
            if (sitsOut) {
                continue; // (further down the dragged joint's own limb it HOLDS: see kEffectorPinHoldReach)
            }
            const glm::vec3& held = positions[static_cast<std::size_t>(u)];
            IkEffector userPin{u, held, 1.0f, -1.0f};
            userPin.leashRadius = socketLeash(u, held, positions, userPin.leashOffset);
            userPin.hard = true; // held in full, and harder than any contact (IkEffector::hard)
            bool replaced = false;
            for (std::size_t p = 0; p < m_pins.size(); ++p) {
                if (m_pins[p].node == u) {
                    m_pins[p] = userPin;
                    m_pinUser[p] = 1;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                m_pins.push_back(userPin);
                m_pinUser.push_back(1);
            }
            static const bool kNoSeat = std::getenv("IK_NO_SEAT") != nullptr; // A/B probe
            if (!kNoSeat && m_seatPin < 0 && isGirdleJoint(u)) {
                for (std::size_t p = 0; p < m_pins.size(); ++p) {
                    if (m_pins[p].node == u) {
                        m_seatPin = static_cast<int>(p); // (see seatPin())
                    }
                }
            }
            ++userPinCount;
        }
    }
    // Airborne fallback: nothing planted AND nothing user-pinned — pin the root in place (a
    // user-pinned hand is an anchor in its own right; the body may swing from it).
    if (planted.empty() && userPinCount == 0 && m_effector != m_pelvis) {
        m_pins.push_back({m_pelvis, positions[static_cast<std::size_t>(m_pelvis)], 1.0f});
        m_pinUser.push_back(0);
    }
    // Live contacts (updateContacts) start empty: nothing here was detected mid-drag.
    m_pinLive.assign(m_pins.size(), 0);
    m_liveErrTicks.assign(m_pins.size(), 0);
    m_liveSpan.assign(m_pins.size(), 0.0f);
    m_contactBlocked.assign(static_cast<std::size_t>(n), 0);
    m_supportHull = BalanceController::supportPolygon(std::move(support));
    m_balanceCorrection = glm::vec2(0.0f);
    // The imbalance the pose BEGINS with is the user's (see m_startImbalance): a landing out of a
    // lift begins a new stance, and owes none.
    m_startImbalance = 0.0f;
    static const bool kNoStartImbalance = std::getenv("IK_NO_START_IMBALANCE") != nullptr;             // A/B probe
    static const bool kStartImbalanceFeetOnly = std::getenv("IK_START_IMBALANCE_FEET_ONLY") != nullptr; // A/B probe: against the FEET alone, before the pose's contacts are seeded (below)
    if (dragStart && !kNoStartImbalance && kStartImbalanceFeetOnly && !m_supportHull.empty() && positions.size() == m_parents.size()) {
        glm::vec2 need(0.0f);
        BalanceController::balanceCorrection(positions, m_parents, m_masses, m_supportHull,
                                             kBalanceMargin * m_sizeScale, need);
        m_startImbalance = glm::length(need);
    }
    m_suspended = false;
    m_suspendTicks = 0;
    m_lastDownIntent = false;
    m_lowestPelvisTarget = 1.0e9f;
    m_lastApplied.clear();
    // Pins snapped DOWN (bind below current => negative snap sum): a hovering figure heals.
    m_healing = snapCount > 0 && snapSum < -0.005f;

    // Stepping state (see updateStepping): per-pin footprints (each planted contact's XZ offset
    // from its owning pin — the pin it, or its nearest planted ancestor, resolved to), which
    // pins are steppable standing feet, and the stance SHAPE (each steppable pin's offset from
    // the stance center) that a step re-centers on the CoM.
    m_stepPin = -1;
    m_stepTick = 0;
    m_stepTotal = 0;
    m_imbalanceTicks = 0;
    m_stepsTaken = 0;
    m_stanceShift = glm::vec3(0.0f);
    m_pinFootprint.assign(m_pins.size(), {});
    for (const int c : planted) {
        int owner = -1;
        for (int cur = c; cur >= 0 && owner < 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            for (std::size_t p = 0; p < m_pins.size(); ++p) {
                if (m_pins[p].node == cur) {
                    owner = static_cast<int>(p);
                    break;
                }
            }
        }
        if (owner >= 0) {
            const glm::vec3& t = m_pins[static_cast<std::size_t>(owner)].target;
            m_pinFootprint[static_cast<std::size_t>(owner)].push_back(
                glm::vec2(positions[static_cast<std::size_t>(c)].x,
                          positions[static_cast<std::size_t>(c)].z) -
                glm::vec2(t.x, t.z));
        }
    }
    m_pinSteppable.assign(m_pins.size(), 0);
    m_pinStanceOffset.assign(m_pins.size(), glm::vec2(0.0f));
    {
        glm::vec2 stanceCenter(0.0f);
        int stanceCount = 0;
        for (std::size_t p = 0; p < m_pins.size(); ++p) {
            const int node = m_pins[p].node;
            // A steppable pin is a STANDING FOOT: a low joint whose pin target sits at its bind
            // height (ground-healed). A kneeling knee or an airborne-kept contact never steps.
            // (... and never the foot below a dragged knee: that one is the user's.)
            if (m_pins.size() >= 2 && !m_pinUser[p] && !pinUnderAnyEffector(p) &&
                isFootClass(node) &&
                std::abs(m_pins[p].target.y -
                         (m_bindPos[static_cast<std::size_t>(node)].y + m_groundOffsetY)) <
                    0.02f * m_sizeScale) {
                m_pinSteppable[p] = 1;
                stanceCenter += glm::vec2(m_pins[p].target.x, m_pins[p].target.z);
                ++stanceCount;
            }
        }
        m_stanceRootOffset = glm::vec2(0.0f);
        if (stanceCount >= 2) {
            stanceCenter /= static_cast<float>(stanceCount);
            for (std::size_t p = 0; p < m_pins.size(); ++p) {
                if (m_pinSteppable[p]) {
                    m_pinStanceOffset[p] =
                        glm::vec2(m_pins[p].target.x, m_pins[p].target.z) - stanceCenter;
                }
            }
            // Where the stance naturally sits relative to the hip (see m_stanceRootOffset):
            // a pelvis-drag landing re-creates THIS relationship at the drag target. From the
            // pose the drag begins in — while that pose STANDS. A figure that begins low (a
            // kneel has its hips a third of a metre ahead of its feet, a sit half a metre
            // behind them) holds that offset because of the pose, not as a stance: taken from
            // it, a kneeling figure dragged up by the hip stood up and then took four steps,
            // 80cm in all, to put its feet back behind its hips. From 8cm below the standing
            // height the offset blends to the BIND stance's, fully by 25cm.
            const std::size_t rootIdx = static_cast<std::size_t>(m_graph.root());
            const glm::vec3& rootPos = positions[rootIdx];
            const glm::vec2 poseOffset = stanceCenter - glm::vec2(rootPos.x, rootPos.z);
            glm::vec2 bindCenter(0.0f);
            for (std::size_t p = 0; p < m_pins.size(); ++p) {
                if (m_pinSteppable[p]) {
                    const glm::vec3& b = m_bindPos[static_cast<std::size_t>(m_pins[p].node)];
                    bindCenter += glm::vec2(b.x, b.z);
                }
            }
            bindCenter /= static_cast<float>(stanceCount);
            const glm::vec2 bindOffset = bindCenter - glm::vec2(m_bindPos[rootIdx].x, m_bindPos[rootIdx].z);
            const float drop = (m_bindPos[rootIdx].y + m_groundOffsetY) - rootPos.y;
            const float low = glm::smoothstep(0.08f * m_sizeScale, 0.25f * m_sizeScale, drop);
            m_stanceRootOffset = glm::mix(poseOffset, bindOffset, low);
            static const bool kStanceTrace = std::getenv("IK_STEP_TRACE") != nullptr;
            if (kStanceTrace) {
                std::fprintf(stderr, "[stance] feet %d pose offset (%.3f %.3f) bind offset (%.3f %.3f) root y %.3f standing %.3f drop %.3f low %.2f -> (%.3f %.3f)\n",
                             stanceCount, poseOffset.x, poseOffset.y, bindOffset.x, bindOffset.y, rootPos.y, m_bindPos[rootIdx].y + m_groundOffsetY, drop, low,
                             m_stanceRootOffset.x, m_stanceRootOffset.y);
            }
        } else {
            // Fewer than two standing feet: stepping the single support would be a fall.
            m_pinSteppable.assign(m_pins.size(), 0);
        }
    }

    // The contacts the POSE made (see the top of this function), as live pins — after every
    // per-pin array above exists (addLivePin appends to them all), before the active set.
    seedPoseContacts(positions);
    // (... and the imbalance the pose BEGINS with is measured against the support they make too:
    // against the FEET alone, a kneeling body — its weight over its knees, a third of a metre
    // ahead of its feet — began every drag "30cm out of balance", and that forgiveness hid the
    // real need of the leaning stand a chest rise out of a flat kneel ended in: no step ever
    // came, 2026-09-26.)
    if (dragStart && !kNoStartImbalance && !kStartImbalanceFeetOnly && !m_supportHull.empty() && positions.size() == m_parents.size()) {
        glm::vec2 need(0.0f);
        BalanceController::balanceCorrection(positions, m_parents, m_masses, m_supportHull,
                                             kBalanceMargin * m_sizeScale, need);
        m_startImbalance = glm::length(need);
    }

    // Active subgraph: the paths joining effector and pins to the root. Everything else —
    // fingers during an arm drag, the face — rides along rigidly.
    rebuildActiveSet();
    m_startPose = positions; // the drag-start reference the intents measure against
    // If the pins were ground-snapped (the figure started hovering), the reference heals by
    // the same shift: "below the start pelvis" is measured from where the figure will stand.
    if (snapCount > 0) {
        const float snapMean = snapSum / static_cast<float>(snapCount);
        for (glm::vec3& p : m_startPose) {
            p.y += snapMean;
        }
    }
}

} // namespace pose
