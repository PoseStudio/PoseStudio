/**
 * @file ikrigstructure.cpp
 * @brief The IkRig's per-node STRUCTURE queries: what each joint of the bind skeleton is to the
 *        rig — a real-mass joint or a token one, a joint of the pelvis girdle or of the spine, the
 *        pelvis bone, a leg joint above its foot, a limb's tip, a twist helper, a joint of the
 *        head chain — and the tree metrics the policy measures with: the path length to the root
 *        or to an effector's chain, a limb's junction with the trunk and its rest span.
 *
 * Constants of the skeleton, not of a drag: every answer depends on the bind skeleton, the
 * masses and the roles alone. The policy asks several of them per pin per tick, so build()
 * caches those per node (the end of IkRig::build; the compute* functions here fill the caches,
 * the inline accessors in ikrig.h read them). See ikrig.cpp for the TU layout; the constants
 * come from ikrig_constants.h. Qt-free (std + GLM).
 */

#include "ikrig.h"

#include "ikrig_constants.h"

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace pose {

bool IkRig::realMass(int node) const {
    const std::size_t i = static_cast<std::size_t>(node);
    return m_masses[i] >= kTokenBoneMass || m_subtreeMass[i] >= kTokenLimbMass;
}

float IkRig::pathLenToRoot(int node) const {
    float len = 0.0f;
    for (int cur = node; cur >= 0 && cur != m_pelvis;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        len += m_edgeRestLen[static_cast<std::size_t>(cur)];
    }
    return len;
}

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

int IkRig::limbJunction(int node) const {
    int prevOnPath = node;
    for (int cur = m_parents[static_cast<std::size_t>(node)];
         cur >= 0 && cur != m_pelvis; cur = m_parents[static_cast<std::size_t>(cur)]) {
        float offPathMass = 0.0f;
        for (const int c : m_children[static_cast<std::size_t>(cur)]) {
            if (c != prevOnPath && m_bodyNode[static_cast<std::size_t>(c)]) {
                offPathMass += m_subtreeMass[static_cast<std::size_t>(c)];
            }
        }
        if (offPathMass > kTrunkMass) { // (~5% of the body hanging off the path)
            return cur; // first real junction walking up = where the limb attaches
        }
        prevOnPath = cur;
    }
    return m_pelvis;
}

glm::vec3 IkRig::limbRestSpan(int node, int junction) const {
    glm::vec3 span(0.0f);
    for (int cur = node; cur >= 0 && cur != junction; cur = m_parents[static_cast<std::size_t>(cur)]) {
        span += m_edgeRestDir[static_cast<std::size_t>(cur)] * m_edgeRestLen[static_cast<std::size_t>(cur)];
    }
    return span;
}

bool IkRig::isGirdleJoint(int node) const {
    if (node < 0 || node >= static_cast<int>(m_parents.size())) {
        return false;
    }
    if (node == m_pelvis) {
        return true;
    }
    // ... and NEVER THE TOP SPINE LINK (2026-11-03): the chest top the collars and the neck hang
    // from carries the arms and the head, and is a chest however near the hips a short spine puts
    // it. A biped export's two-link spine has its top 18cm up, two bones from the root, and read
    // as the girdle every grab of the chest was a HIPS drag: the trunk never bent, a bow was a
    // walk of two steps, and the pinned seat moved under a lean (the pin on the grabbed joint
    // sits out). The reference's top link is four bones up and never met the walk below.
    if (m_roles.built()) {
        const RigRoleTag& t = m_roles.tag(node);
        if (t.role == RigRole::Spine && static_cast<int>(t.index) == m_roles.spineLinks()) {
            return false;
        }
    }
    int depth = 0; // (within two bones of the solve root: the walk stops at two)
    int cur = node;
    while (cur >= 0 && cur != m_pelvis && depth < 2) {
        cur = m_parents[static_cast<std::size_t>(cur)];
        ++depth;
    }
    return cur == m_pelvis && m_subtreeMass[static_cast<std::size_t>(node)] > kTrunkMass &&
           pathLenToRoot(node) < kGirdleGrabReach * m_sizeScale;
}

bool IkRig::isSpineJoint(int node) const {
    return node >= 0 && node < static_cast<int>(m_parents.size()) && node != m_pelvis &&
           m_bodyNode[static_cast<std::size_t>(node)] && limbJunction(node) == m_pelvis && !hasFootBelow(node);
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
        legs += isFootClass(child) || hasFootBelow(child) ? 1 : 0; // (a leg: a foot-class joint at or below the child)
    }
    return legs >= 2;
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

bool IkRig::computeIsLegJointAboveFoot(int node) const {
    // A leg joint with a foot-class joint below it — a knee, a thigh (onKnees' test for one contact).
    if (node < 0 || node == m_pelvis || isGirdleJoint(node) || !descendsFromPelvis(node) ||
        isFootClass(node)) {
        return false;
    }
    return hasFootBelow(node); // (build() caches the foot-below flags before it asks this)
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

bool IkRig::computeIsLimbTip(int node) const {
    // A real-mass joint with no real-mass joint below it: a hand (its fingers are token), a
    // foot, a carpal on the generations whose carpals are real joints, the head.
    const int n = static_cast<int>(m_parents.size());
    for (int d = 0; d < n; ++d) {
        const std::size_t di = static_cast<std::size_t>(d);
        if (d == node || !realMass(d)) {
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

bool IkRig::helperNode(int node) const {
    static const bool kHelperContacts = std::getenv("IK_HELPER_CONTACTS") != nullptr; // A/B probe: twist helpers as contacts, as before
    if (kHelperContacts || node < 0 || static_cast<std::size_t>(node) >= m_helperNode.size()) {
        return false;
    }
    // A twist helper (IkRigBone::helper), or a bone the rig standard reads as a HELPER — an anchor,
    // a control, a skinless leaf (rigroles.h): no joint a body rests on either.
    return m_helperNode[static_cast<std::size_t>(node)] != 0 || m_roles.role(node) == RigRole::Helper;
}

bool IkRig::inHeadChain(int node) const {
    for (int cur = node; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
        if (cur == m_neckBase) {
            return true;
        }
    }
    return false;
}

} // namespace pose
