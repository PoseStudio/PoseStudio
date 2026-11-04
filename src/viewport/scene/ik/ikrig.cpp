/**
 * @file ikrig.cpp
 * @brief IkRig construction: the per-figure FBIK policy state built once from the bind skeleton
 *        (the body tree, the masses, the floor clearances fitted to the mesh), plus the metric
 *        helpers every drag phase shares (path lengths, socket leashes, limb junctions).
 *
 * The rig's implementation spans six translation units so the phases can evolve independently:
 * this file, ikrigvolumes.cpp (the self-collision BODY VOLUMES: the mesh fit, the drag-start
 * mask, the drag-target clamp), ikrigdrag.cpp (beginDrag / plantContacts — contacts, pins, user
 * pins, promotion), ikrigpolicy.cpp (the per-tick intents, the lift-off test, the step policy's
 * entry), ikrigstepping.cpp (balance- and anchor-driven foot re-planting) and ikrigcontacts.cpp
 * (live contact re-detection, landings, pin-set maintenance); the tuning constants they share
 * live in ikrig_constants.h. build() hosts the load-bearing rooting decision: the FBIK root is the
 * first multi-child DESCENDANT of the anatomical root (real figures root at an origin-level
 * figure node whose chain is excluded from the rig wholesale — no graph edge, no contact
 * eligibility, no balance mass), and every world-space threshold scales with the figure's
 * pelvis height (sizeScale). Qt-free (std + GLM).
 */

#include "ikrig.h"
#include "ikmath.h"

#include "balancecontroller.h"
#include "ikrig_constants.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

void IkRig::build(const std::vector<IkRigBone>& bones, const std::vector<BodyMeshPoint>* mesh) {
    const std::size_t n = bones.size();
    m_parents.assign(n, -1);
    m_bindPos.assign(n, glm::vec3(0.0f));
    m_edgeRestDir.assign(n, glm::vec3(0.0f, 1.0f, 0.0f));
    m_edgeRestLen.assign(n, 0.0f);
    m_helperNode.assign(n, 0);
    m_pelvis = -1;
    m_pelvisBoneNode = -2;

    std::vector<std::string> names(n);
    int anatomicalRoot = -1;
    for (std::size_t i = 0; i < n; ++i) {
        m_parents[i] = bones[i].parent;
        m_bindPos[i] = bones[i].bindPos;
        m_helperNode[i] = bones[i].helper ? 1 : 0;
        names[i] = bones[i].name;
        if (bones[i].parent < 0 && anatomicalRoot < 0) {
            anatomicalRoot = static_cast<int>(i);
        }
        if (bones[i].parent >= 0 && static_cast<std::size_t>(bones[i].parent) < n) {
            const IkRigBone& parent = bones[static_cast<std::size_t>(bones[i].parent)];
            const glm::vec3 offset = bones[i].bindPos - parent.bindPos;
            const float len = glm::length(offset);
            m_edgeRestLen[i] = len;
            if (len > 1e-6f) {
                m_edgeRestDir[i] = offset / len;
            }
        }
    }
    // Anatomical children per bone, in ASCENDING index order: limbJunction sums off-path subtree
    // masses over these, and ascending order is the summation order the former whole-skeleton
    // scan used — the sums stay bit-identical.
    m_children.assign(n, {});
    for (std::size_t i = 0; i < n; ++i) {
        if (bones[i].parent >= 0 && static_cast<std::size_t>(bones[i].parent) < n) {
            m_children[static_cast<std::size_t>(bones[i].parent)].push_back(static_cast<int>(i));
        }
    }
    // The FBIK root ("the pelvis") is the first MULTI-CHILD descendant of the anatomical root,
    // not the anatomical root itself: real figures root at a FIGURE NODE sitting at the ORIGIN
    // (floor level) with the hip as its lone child a meter up. Rooting the solve there aimed
    // every root mechanism at the wrong point — reach leashes measured ~2m paths and never bound
    // (the single-pull float), the root stiffness pinned a point on the floor, and balance
    // targeted the origin. The walk is generation-agnostic; the figure node stays outside the
    // active solve and rides along.
    m_pelvis = anatomicalRoot;
    std::vector<int> childCount(n, 0);
    std::vector<int> onlyChild(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        if (bones[i].parent >= 0) {
            ++childCount[static_cast<std::size_t>(bones[i].parent)];
            onlyChild[static_cast<std::size_t>(bones[i].parent)] = static_cast<int>(i);
        }
    }
    if (m_pelvis >= 0) {
        int walk = m_pelvis;
        while (walk >= 0 && childCount[static_cast<std::size_t>(walk)] == 1) {
            walk = onlyChild[static_cast<std::size_t>(walk)];
        }
        // A skeleton that never branches (a pure chain: a tail/snake rig, a minimal test rig)
        // has no multi-child descendant — the walk lands on the LEAF, and rooting there would
        // flag every other bone as a non-body "figure-node ancestor". Such a rig keeps its
        // anatomical root as the FBIK root instead.
        if (walk >= 0 && childCount[static_cast<std::size_t>(walk)] > 1) {
            m_pelvis = walk;
        }
    }

    // The pelvis's strict ancestors — the figure-node chain — are NOT body parts, and they are
    // excluded from the rig wholesale: no graph edge (the hip→origin link is not a bone the
    // solver may compress), no ground-contact eligibility (the figure node sits ON the floor, so
    // contact detection planted it and pinned the whole figure to the origin — the solver's
    // per-tick compromise between that phantom pin and the real leg pins ratcheted the hip
    // floorward, the single-pull whole-body sink), and no balance mass.
    m_bodyNode.assign(n, 1);
    std::vector<int> graphParents = m_parents;
    if (m_pelvis >= 0) {
        graphParents[static_cast<std::size_t>(m_pelvis)] = -1;
        for (int cur = anatomicalRoot; cur >= 0 && cur != m_pelvis;
             cur = onlyChild[static_cast<std::size_t>(cur)]) {
            m_bodyNode[static_cast<std::size_t>(cur)] = 0;
        }
    }
    m_graph.build(graphParents);
    // Root the graph at the pelvis right away: build() defaults to the first parentless node,
    // which after the exclusion above can be the (isolated) figure node. beginDrag() re-asserts
    // this per drag, but a freshly-built rig should never sit in a nonsensical rooting.
    if (m_pelvis >= 0) {
        m_graph.setRoot(m_pelvis);
    }
    m_masses = BalanceController::assignMasses(names);
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i]) {
            m_masses[i] = 0.0f;
        }
    }
    // LEAF twist helpers carry no segment: the newest generation hangs its twist bones OFF the
    // chain as childless siblings of the shin and the forearm (`l_thightwist1/2`), and the
    // name-class table gave each a third of the thigh — enough "off-path mass" at the thigh
    // for limbJunction to end the foot's limb chain at the knee, cutting the leg's stiffness
    // class (and a limb's reach measure) in half. A pass-through twist bone (the earlier generations') keeps its
    // share — it is on the chain and its share is harmless there. (A twist helper by STRUCTURE —
    // IkRigBone::helper, Armature::isTwistBone — not by its name, since 2026-11-02.)
    for (std::size_t i = 0; i < n; ++i) {
        if (m_children[i].empty() && m_helperNode[i]) {
            m_masses[i] = std::min(m_masses[i], BalanceController::unclassifiedMass());
        }
    }
    // Per-node subtree mass (own + all descendants): the token-limb promotion and the
    // trunk-junction search read it. Figure skeletons list parents before children (hierarchy
    // order), so one reverse pass accumulates leaves into roots.
    m_subtreeMass = m_masses;
    for (std::size_t i = n; i-- > 1;) {
        const int p = m_parents[i];
        if (p >= 0) {
            m_subtreeMass[static_cast<std::size_t>(p)] += m_subtreeMass[i];
        }
    }
    // Figure size relative to the adult reference every world-space threshold here was tuned
    // on: the pelvis bind height (~1m standing). A 0.54-scale child's pelvis-to-foot path is
    // ~0.5m — less than the ADULT contact-release reach — so unscaled thresholds released the
    // child's feet on a hip drag and the figure translated rigidly instead of crouching. The
    // dead zone keeps every adult rig (real figures' pelvis heights vary ~1.0-1.1) at EXACTLY
    // the verified 1.0 tuning; only genuinely small/large figures engage the scaling.
    m_sizeScale = 1.0f;
    if (m_pelvis >= 0 && static_cast<std::size_t>(m_pelvis) < n) {
        const float raw = m_bindPos[static_cast<std::size_t>(m_pelvis)].y;
        if (raw <= 0.85f || raw >= 1.15f) {
            m_sizeScale = glm::clamp(raw, 0.25f, 2.5f);
        }
    }
    // Floor clearances (see floorClearance()): a joint that rests within the contact band of
    // the floor keeps its rest height (the ankle above the sole, the toes just off the ground);
    // any other joint gets a flesh radius. Relative to the bind floor — the per-drag ground
    // offset is added by the solve.
    m_floorClearance.assign(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i]) {
            continue;
        }
        // (A foot-class joint's clearance IS its bind height — below the floor too, 2026-11-03: a
        // biped export binds its toe bone, the ball of the foot, 11cm under a sole that rests on
        // the floor, and clamped at 0 the ball's clearance stood 11cm over the joint's own place:
        // every floor reading of the rig began 111mm "through the floor", while the foot model,
        // which holds the ball at its clearance, was asked a foot the skin never has.)
        const float bindY = m_bindPos[i].y;
        m_floorClearance[i] = bindY < kContactHeight * m_sizeScale ? bindY : kFleshRadius * m_sizeScale;
    }
    // THE RIG STANDARD'S ROLES (rigroles.h): what each bone is, from the structure settled above
    // — the root, the body tree, the masses, the helpers, the foot class — and the mesh sample.
    // Everything below that asks for a part of the body by name asks the roles instead.
    {
        std::vector<char> footClass(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            footClass[i] = m_bodyNode[i] && isFootClass(static_cast<int>(i)) ? 1 : 0;
        }
        RigRoles::Input in;
        in.parents = &m_parents;
        in.children = &m_children;
        in.bindPos = &m_bindPos;
        in.names = &names;
        in.bodyNode = &m_bodyNode;
        in.subtreeMass = &m_subtreeMass;
        in.helper = &m_helperNode;
        in.footClass = &footClass;
        in.mesh = mesh;
        in.root = m_pelvis;
        in.sizeScale = m_sizeScale;
        std::vector<std::string> given(n);
        for (std::size_t i = 0; i < n; ++i) {
            given[i] = bones[i].roleHint; // (a VRM avatar's humanoid map; empty elsewhere)
        }
        in.givenNames = &given;
        m_roles.build(in);
        // THE ROOT IS THE SEAT where the rig has no pelvis bone (2026-11-03): a biped export and a
        // rigging add-on's export hang the thighs straight off the hips, and the buttocks' flesh
        // is the root's own. The contact rules keep the root out of the contacts on every rig
        // with a pelvis bone (that bone is the seat there); on these it is the one joint a
        // sitting figure rests on, and excluded she had no seat: reclined by the chest she leaned
        // on nothing, and laid down by it she arched her spine to its limits 15cm off the floor.
        // (... and only on a rig the roles could place — every required role found: a figure
        // whose inference failed, a leg with no foot, must not turn its hips into a contact.)
        m_rootIsSeat = m_roles.built() && m_roles.complete() && m_roles.find(RigRole::Pelvis) < 0;
        m_headNode = m_roles.find(RigRole::Head);
        m_neckBase = m_roles.find(RigRole::Neck, RigSide::Centre, 1);
        static const bool kRolesTrace = std::getenv("POSESTUDIO_RIG_ROLES_TRACE") != nullptr;
        if (kRolesTrace) {
            std::fputs(m_roles.report().c_str(), stderr);
        }
        // A STANDARD-NAMED rig — every bone the standard names carries that name: the FBX
        // conformer's rigs since 2026-11-03, or a figure authored to the standard — is massed BY
        // ROLE outright. The name table classes by fragments of the reference generation's names,
        // and read on the standard's it put every spine link in the one "spine" class, a third of
        // the reference's trunk (the game-engine export's known short bow); by role the lower
        // links share the abdomen's fraction and the upper the chest's, as the table splits the
        // reference. The reference rigs carry their own names and keep the table, bit for bit.
        // A standard name that TWO bones of the rig would carry (two riders of a side tagged
        // pectoral) is applied to neither by the conformer: such bones keep their file names and
        // do not unmake a standard-named rig (read strictly, three of five conformed rigs read
        // as NOT standard-named for a day and fell back to the name table).
        std::unordered_map<std::string, int> uses;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string standard = RigRoles::standardName(m_roles.tag(static_cast<int>(i)));
            if (!standard.empty()) {
                ++uses[standard];
            }
        }
        m_standardNamed = m_roles.built();
        for (std::size_t i = 0; i < n && m_standardNamed; ++i) {
            const std::string standard = RigRoles::standardName(m_roles.tag(static_cast<int>(i)));
            m_standardNamed = standard.empty() || standard == names[i] || uses[standard] > 1;
        }
        if (m_standardNamed) {
            assignMassesByRole();
        } else {
            fillMassesByRole();
        }
    }
    // BODY VOLUMES (see bodyVolumes()): the trunk and the limb roots by their ROLES, the radii
    // from the hip-socket spacing so they scale with the rig, each volume applied to the other
    // limbs' joints below their roots.
    buildBodyVolumes(names, mesh);
    static const bool kNoMeshFloor = std::getenv("IK_FLOOR_NO_MESH") != nullptr; // A/B probe
    if (mesh != nullptr && !mesh->empty() && !kNoMeshFloor) {
        fitFloorClearances(*mesh);
    }
    // Riding descendants (see m_ridingDescendants): each real-mass joint's token descendants —
    // token by their own segment AND by their subtree, the token-limb promotion's pair — the
    // walk stopping at the first child that is not: a hand's whole finger tree, the head's
    // face bones, a foot's toes; the pelvis stops at the thighs and the spine, a forearm at
    // the hand even where a helper bone has halved the hand's own share. The live contact
    // detection measures a joint's height through them. NOT a HELPER (2026-11-03): a leaf twist
    // helper beside a shin carries the LOWER calf's skin, and, no segment (no child), answers
    // for all of it down to the ankle — read as the knee's riding part, that skin put the knee
    // "within the kneel band" with the knee 25cm up, and a game-engine rig's kneel planted both
    // knees in mid-air and stalled there (IK_HELPER_RIDERS is the A/B). A helper splits a rigid
    // segment and has no joint of its own; what rides a joint is the token flesh that moves with
    // it — a finger, a toe, the face — not the far half of its own segment.
    static const bool kHelperRiders = std::getenv("IK_HELPER_RIDERS") != nullptr; // A/B probe
    m_ridingDescendants.assign(n, {});
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || (m_masses[i] < kTokenBoneMass && m_subtreeMass[i] < kTokenLimbMass)) {
            continue;
        }
        std::vector<int> stack(m_children[i].begin(), m_children[i].end());
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            const std::size_t ci = static_cast<std::size_t>(c);
            if (!m_bodyNode[ci] || m_masses[ci] >= kTokenBoneMass ||
                m_subtreeMass[ci] >= kTokenLimbMass) {
                continue;
            }
            if (!kHelperRiders && m_helperNode[ci]) {
                continue;
            }
            m_ridingDescendants[i].push_back(c);
            stack.insert(stack.end(), m_children[ci].begin(), m_children[ci].end());
        }
    }
    // The per-node STRUCTURE the policy asks for every tick, cached: each is an O(n x depth) walk
    // over the bind skeleton that the contact rules, the pin rows and the posture model asked for
    // per pin per tick (a knee's test alone five times a pin). In dependency order: the leg joint
    // reads descendsFromPelvis, the leg reads isLegJointAboveFoot.
    m_underPelvis.assign(n, 0);
    m_footBelow.assign(n, 0);
    m_limbTip.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        m_underPelvis[i] = computeDescendsFromPelvis(static_cast<int>(i)) ? 1 : 0;
        m_footBelow[i] = computeHasFootBelow(static_cast<int>(i)) ? 1 : 0;
        m_limbTip[i] = computeIsLimbTip(static_cast<int>(i)) ? 1 : 0;
    }
    m_legAboveFoot.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        m_legAboveFoot[i] = computeIsLegJointAboveFoot(static_cast<int>(i)) ? 1 : 0;
    }
    m_legOfNode.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        m_legOfNode[i] = computeLegOf(static_cast<int>(i));
    }
}

// THE BALANCE MASSES OF A RIG THE NAME TABLE DOES NOT KNOW (2026-11-03): a bone left at the token
// mass whose ROLE is a real segment takes the role's share — the name table's own biomechanical
// fraction for that segment, split among the bones of that role class on that side as the name
// table splits a class (a thigh and its twist helpers are one thigh; the spine's lower half takes
// the abdomen's share, its upper half the chest's; the ball and the toes are one). A rig whose names
// the table knows (the reference generations: every real bone named) is untouched and bit-identical;
// a rig in another convention gets a centre of mass instead of a hundred token masses. The leaf
// twist helpers' clamp is re-applied after, the subtree masses recomputed.
namespace {

/// The balance class a role belongs to: a twist helper is its segment's, a toe the ball's.
RigRole massClassOf(RigRole r) {
    switch (r) {
    case RigRole::UpperArmTwist: return RigRole::UpperArm;
    case RigRole::ForearmTwist: return RigRole::Forearm;
    case RigRole::ThighTwist: return RigRole::Thigh;
    case RigRole::ShinTwist: return RigRole::Shin;
    case RigRole::Toe: return RigRole::Ball;
    default: return r;
    }
}

/// Whether spine link @p spineIndex (1-based) lies in the upper half of a chain of @p spineLinks.
bool spineUpperHalf(int spineIndex, int spineLinks) {
    return spineIndex * 2 > spineLinks;
}

/// A class's share of the body's mass — the name table's biomechanical fractions, read from the
/// table itself (BalanceController::segmentMassFraction: one definition of each number), the
/// spine's lower half the abdomen's, its upper half the chest's; 0 for a class with none.
float massShareOf(RigRole cls, int spineIndex, int spineLinks) {
    const auto of = [](const char* fragment) { return BalanceController::segmentMassFraction(fragment); };
    switch (cls) {
    case RigRole::Hips: return of("hip");
    case RigRole::Pelvis: return of("pelvis");
    case RigRole::Spine: return spineUpperHalf(spineIndex, spineLinks) ? of("chest") : of("abdomen");
    case RigRole::Neck: return of("neck");
    case RigRole::Head: return of("head");
    case RigRole::Collar: return of("collar");
    case RigRole::UpperArm: return of("upperarm");
    case RigRole::Forearm: return of("forearm");
    case RigRole::Hand: return of("hand");
    case RigRole::Thigh: return of("thigh");
    case RigRole::Shin: return of("shin");
    case RigRole::Foot: return of("foot");
    case RigRole::Metatarsal: return of("metatarsal");
    case RigRole::Ball: return of("toe");
    default: return 0.0f;
    }
}

} // namespace

// THE BALANCE MASSES OF A STANDARD-NAMED RIG (2026-11-03): every real bone by its role, the class's
// share split among the bones of that class on that side (and, for the spine, in that half) — the
// name table's own split of the reference generation, now read off the roles, so a conformed rig
// of any naming weighs as the reference does: a two-link spine carries the abdomen's and the
// chest's fractions, a three-link one too, where the name table gave every "spineN" a share of
// one class. What has no class — a finger, a carpal, a face bone, a pectoral, a helper — is a
// token mass, as on the reference. The leaf twist helpers' clamp is re-applied after and the
// subtree masses recomputed, as fillMassesByRole does. IK_NO_ROLE_MASSES keeps the name table's.
void IkRig::assignMassesByRole() {
    static const bool kNoRoleMasses = std::getenv("IK_NO_ROLE_MASSES") != nullptr; // A/B probe
    if (kNoRoleMasses || !m_roles.built()) {
        return;
    }
    const std::size_t n = m_masses.size();
    const float       token = BalanceController::unclassifiedMass();
    const auto        keyOf = [&](const RigRoleTag& t, RigRole cls) {
        const int half = cls == RigRole::Spine && spineUpperHalf(t.index, m_roles.spineLinks()) ? 1 : 0;
        return (static_cast<int>(cls) * 3 + static_cast<int>(t.side)) * 2 + half;
    };
    std::vector<int>   group(n, -1);
    std::vector<int>   keys;
    std::vector<int>   groupCount;
    std::vector<float> groupShare;
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i]) {
            m_masses[i] = 0.0f;
            continue;
        }
        m_masses[i] = token;
        // (A LEAF twist helper joins its class's group and then loses its share to the clamp
        // below, as it does under the name table — a thigh with two leaf helpers weighs a third
        // of the class's fraction. Giving the segment the whole share was TRIED in the IK
        // refactoring round of 2026-11-03 and put back: physically right, it moved the
        // character-creation rig 141 → 132 and its Blender export 136 → 126 of 157 — the knee
        // drags, the kneels and the seated leans, whose rules and gates are calibrated on the
        // reference rigs' effective masses, the newest generation's with the same drop. A tuning
        // round with the masses re-gated on every rig, not a refactor's change.)
        const RigRoleTag& t = m_roles.tag(static_cast<int>(i));
        const RigRole     cls = massClassOf(t.role);
        const float       share = massShareOf(cls, t.index, m_roles.spineLinks());
        if (share <= 0.0f) {
            continue;
        }
        const int key = keyOf(t, cls);
        int       g = -1;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            if (keys[k] == key) {
                g = static_cast<int>(k);
                break;
            }
        }
        if (g < 0) {
            keys.push_back(key);
            groupCount.push_back(0);
            groupShare.push_back(share);
            g = static_cast<int>(keys.size()) - 1;
        }
        group[i] = g;
        ++groupCount[static_cast<std::size_t>(g)];
    }
    int massed = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (group[i] >= 0) {
            const std::size_t g = static_cast<std::size_t>(group[i]);
            m_masses[i] = groupShare[g] / static_cast<float>(std::max(1, groupCount[g]));
            ++massed;
        }
    }
    static const bool kRolesTrace = std::getenv("POSESTUDIO_RIG_ROLES_TRACE") != nullptr;
    if (kRolesTrace) {
        std::fprintf(stderr, "[rig-roles] a standard-named rig: %d balance mass(es) by role\n", massed);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (m_children[i].empty() && m_helperNode[i]) {
            m_masses[i] = std::min(m_masses[i], BalanceController::unclassifiedMass());
        }
    }
    m_subtreeMass = m_masses;
    for (std::size_t i = n; i-- > 1;) {
        const int p = m_parents[i];
        if (p >= 0) {
            m_subtreeMass[static_cast<std::size_t>(p)] += m_subtreeMass[i];
        }
    }
}

void IkRig::fillMassesByRole() {
    static const bool kNoRoleMasses = std::getenv("IK_NO_ROLE_MASSES") != nullptr; // A/B probe
    if (kNoRoleMasses) {
        return;
    }
    const std::size_t n = m_masses.size();
    const float       token = BalanceController::unclassifiedMass();
    const auto        classOf = [](RigRole r) { return massClassOf(r); };
    const auto        upperHalf = [&](int spineIndex) { return spineUpperHalf(spineIndex, m_roles.spineLinks()); };
    const auto        shareOf = [&](RigRole cls, int spineIndex) -> float { return massShareOf(cls, spineIndex, m_roles.spineLinks()); };
    // Only a class the name table gave NO mass to at all is filled: a class with one bone named
    // (the reference generations' forearm beside its twist helper named like a wrist) keeps the
    // table's books, so the reference rigs stay bit-identical.
    const auto keyOf = [&](const RigRoleTag& t, RigRole cls) {
        const int half = cls == RigRole::Spine && upperHalf(t.index) ? 1 : 0;
        return (static_cast<int>(cls) * 3 + static_cast<int>(t.side)) * 2 + half;
    };
    std::vector<int> namedKeys; // the (class, side, half) groups the name table already massed
    for (std::size_t i = 0; i < n; ++i) {
        const RigRoleTag& t = m_roles.tag(static_cast<int>(i));
        if (!m_bodyNode[i] || m_masses[i] == token || t.role == RigRole::None) {
            continue;
        }
        namedKeys.push_back(keyOf(t, classOf(t.role)));
    }
    std::vector<int>   group(n, -1);
    std::vector<int>   keys;
    std::vector<int>   groupCount;
    std::vector<float> groupShare;
    for (std::size_t i = 0; i < n; ++i) {
        const RigRoleTag& t = m_roles.tag(static_cast<int>(i));
        if (!m_bodyNode[i] || m_masses[i] != token || t.role == RigRole::None) {
            continue;
        }
        const RigRole cls = classOf(t.role);
        const float   share = shareOf(cls, t.index);
        if (share <= 0.0f) {
            continue;
        }
        const int key = keyOf(t, cls);
        if (std::find(namedKeys.begin(), namedKeys.end(), key) != namedKeys.end()) {
            continue; // (the table knows this class on this rig: its books stand)
        }
        int g = -1;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            if (keys[k] == key) {
                g = static_cast<int>(k);
                break;
            }
        }
        if (g < 0) {
            keys.push_back(key);
            groupCount.push_back(0);
            groupShare.push_back(share);
            g = static_cast<int>(keys.size()) - 1;
        }
        group[i] = g;
        ++groupCount[static_cast<std::size_t>(g)];
    }
    if (keys.empty()) {
        return;
    }
    int filled = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (group[i] >= 0) {
            const std::size_t g = static_cast<std::size_t>(group[i]);
            m_masses[i] = groupShare[g] / static_cast<float>(std::max(1, groupCount[g]));
            ++filled;
        }
    }
    static const bool kRolesTrace = std::getenv("POSESTUDIO_RIG_ROLES_TRACE") != nullptr;
    if (kRolesTrace) {
        std::fprintf(stderr, "[rig-roles] %d balance mass(es) filled by role (the name table knew none of their classes)\n", filled);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (m_children[i].empty() && m_helperNode[i]) {
            m_masses[i] = std::min(m_masses[i], BalanceController::unclassifiedMass());
        }
    }
    m_subtreeMass = m_masses;
    for (std::size_t i = n; i-- > 1;) {
        const int p = m_parents[i];
        if (p >= 0) {
            m_subtreeMass[static_cast<std::size_t>(p)] += m_subtreeMass[i];
        }
    }
}

float IkRig::pathLenToRoot(int node) const {
    float len = 0.0f;
    for (int cur = node; cur >= 0 && cur != m_pelvis;
         cur = m_parents[static_cast<std::size_t>(cur)]) {
        len += m_edgeRestLen[static_cast<std::size_t>(cur)];
    }
    return len;
}

float IkRig::socketLeash(int node, const glm::vec3& target,
                         const std::vector<glm::vec3>& positions, glm::vec3& offsetOut) const {
    const glm::vec3& rootPos = positions[static_cast<std::size_t>(m_pelvis)];
    int socket = node;
    while (socket >= 0 && m_parents[static_cast<std::size_t>(socket)] >= 0 &&
           m_parents[static_cast<std::size_t>(socket)] != m_pelvis) {
        socket = m_parents[static_cast<std::size_t>(socket)];
    }
    if (socket < 0 || m_parents[static_cast<std::size_t>(socket)] != m_pelvis) {
        // The pin is the root itself (or above it): a plain root-centered ball.
        offsetOut = glm::vec3(0.0f);
        return std::max(glm::length(rootPos - target), 0.9f * pathLenToRoot(node));
    }
    const glm::vec3& socketPos = positions[static_cast<std::size_t>(socket)];
    offsetOut = socketPos - rootPos;
    const float limb = pathLenToRoot(node) - m_edgeRestLen[static_cast<std::size_t>(socket)];
    // Never beyond what the limb can SPAN (its straight rest path): a leash measured from an
    // overstretched stance — a step landed while its leg could not yet reach the spot — recorded
    // the impossible distance as the allowed maximum, and two such balls pinned the root
    // between them (every forward-pass move toward one foot left the other's ball and was
    // projected straight back), so the release settle could never bring the feet down: the
    // figure stood 4cm in the air after every stepping chest drag. Capped, the projection pulls
    // the root back INTO reach and the feet plant.
    return std::min(std::max(glm::length(socketPos - target), 0.9f * limb), 0.995f * limb);
}

int IkRig::limbJunction(int node) const {
    constexpr float kTrunkJunctionMass = 0.05f; // ~5% of body mass hanging off the path
    int prevOnPath = node;
    for (int cur = m_parents[static_cast<std::size_t>(node)];
         cur >= 0 && cur != m_pelvis; cur = m_parents[static_cast<std::size_t>(cur)]) {
        float offPathMass = 0.0f;
        for (const int c : m_children[static_cast<std::size_t>(cur)]) {
            if (c != prevOnPath && m_bodyNode[static_cast<std::size_t>(c)]) {
                offPathMass += m_subtreeMass[static_cast<std::size_t>(c)];
            }
        }
        if (offPathMass > kTrunkJunctionMass) {
            return cur; // first real junction walking up = where the limb attaches
        }
        prevOnPath = cur;
    }
    return m_pelvis;
}

// THE FLOOR CLEARANCES OF THE JOINTS ABOVE THE FEET, FROM THE SKIN. They were one number, 1.5cm —
// "a flesh radius" — for a pelvis whose joint sits 19cm over the seat of the buttocks, a chest 9cm
// inside the back, a skull, a knee: so a figure sat down on the floor went on down until her hip
// JOINT was 1.5cm over it, her flesh 8cm and more through it (the in-app floor gallery's `meshlow`:
// no joint reading shows skin), and — since a joint is a floor CONTACT only within a centimetre of
// its clearance — her pelvis never was one: a sitting figure stood, to the rig, on her feet alone,
// 40cm out of balance, and the first limbs to join the solve were flung out as counterweights (a
// reclining figure's arms, 70cm in one tick, when her hands touched the floor).
//
// A joint's clearance is how far ITS OWN FLESH reaches below it, as the bone is turned now: the
// SUPPORT FUNCTION of the skin its bone dominates, about the joint, along the world's down in the
// bone's frame (every bind is translation-only, so the bind frame is the world's) — max over the
// bone's skin of (v - joint) . down. Exact for flesh that turns rigidly with its bone, and each
// bone answers for its own: a thigh's socket must stand a thigh's length over the floor while the
// thigh hangs down (the knee is there), the back of a thigh's thickness when she sits, and the
// pelvis rests on what is under IT. (Two fits by directions were tried first, the same day: the
// thinnest of 26 cones — one number, so a sit sank 9cm or a figure on her back hovered — and those
// cones over the joint's own AND its parent's skin, read in the child's frame: wrong the moment
// the joint bends, it held a sit 6cm in the air.) Kept per bone are the skin's EXTREME points
// only, along 128 directions spread over the sphere (the hull's support to a percent); the rig is
// handed the bones' rotations before a drag and at the top of every tick (setBoneRotations), so a
// clearance is a constant of the tick. Foot-class joints keep their bind heights — the standing
// contact model is what it was — and nothing is ever less than the old radius.
void IkRig::fitFloorClearances(const std::vector<BodyMeshPoint>& mesh) {
    const std::size_t n = m_bindPos.size();
    std::vector<std::vector<glm::vec3>> own(n);
    for (const BodyMeshPoint& point : mesh) {
        if (point.bone >= 0 && static_cast<std::size_t>(point.bone) < n) {
            own[static_cast<std::size_t>(point.bone)].push_back(point.pos);
        }
    }
    // 128 directions, evenly over the sphere: a Fibonacci lattice of 64 AND ITS MIRROR IMAGE across
    // the sagittal plane. A lattice alone is not mirror-symmetric, so the extreme points it found on
    // a symmetric pelvis were not either, nor a left thigh's the mirror of the right's: the floor's
    // rows on that flesh were lopsided, and a sit pushed hard into the floor rolled over to one side
    // (both knees 5-7cm across on two generations; found by the harness's symmetry measure).
    constexpr int kDirections = 64;
    std::vector<glm::vec3> directions;
    for (int k = 0; k < kDirections; ++k) {
        const float y = 1.0f - 2.0f * (static_cast<float>(k) + 0.5f) / static_cast<float>(kDirections);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float a = 2.399963f * static_cast<float>(k); // the golden angle
        directions.emplace_back(r * std::cos(a), y, r * std::sin(a));
        directions.emplace_back(-r * std::cos(a), y, r * std::sin(a));
    }
    constexpr std::size_t kMinPoints = 32; // less skin than this is a token bone's: the old radius
    static const bool kTrace = std::getenv("IK_FLOOR_TRACE") != nullptr;
    m_fleshPoints.assign(n, {});
    m_fleshBorrowed.assign(n, 0);
    // A SEGMENT's flesh is its own between its two joints. "Rigid with its bone" is true of the
    // skin along a limb's or the spine's segment and false of the skin a bone dominates PAST its
    // far joint (it bends with the next bone: a thigh's skin over the knee) or behind its near one
    // — and read rigidly that overhang made a kneeling thigh 10cm longer than it is: on the
    // one-bone thighs the hips could not come down to a kneel at all, and deep crouches stalled. A
    // bone with exactly ONE child that carries real skin is a segment, and its joint answers for
    // the NEAR part of it — the slab from the joint kSegmentNear of the way to that child's (the
    // child, and the contact at the limb's end, answer for the rest: a standing shin's flesh
    // reaches down to the ankle, and answered for whole it made the knee a floor "contact" 49cm
    // up — planted there, hard, the moment the hips began to come down); the pelvis, the chest,
    // the head and the hands — several such children, or none — keep all of theirs.
    std::vector<std::size_t> subtreeSkin(n, 0);
    for (std::size_t i = n; i-- > 0;) {
        subtreeSkin[i] += own[i].size();
        if (m_parents[i] >= 0) {
            subtreeSkin[static_cast<std::size_t>(m_parents[i])] += subtreeSkin[i];
        }
    }
    constexpr std::size_t kRealSkin = 400;
    constexpr float       kSegmentNear = 0.6f;
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || m_bindPos[i].y < kContactHeight * m_sizeScale || own[i].size() < kMinPoints) {
            continue; // (foot-class: its bind height, as ever)
        }
        int realChild = -1;
        int realChildren = 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (m_parents[k] == static_cast<int>(i) && subtreeSkin[k] >= kRealSkin) {
                realChild = static_cast<int>(k);
                ++realChildren;
            }
        }
        if (realChildren == 1) {
            const glm::vec3 along = m_bindPos[static_cast<std::size_t>(realChild)] - m_bindPos[i];
            const float     length = glm::length(along);
            if (length > 0.03f * m_sizeScale) {
                const glm::vec3 axis = along / length;
                std::vector<glm::vec3> kept;
                for (const glm::vec3& v : own[i]) {
                    const float t = glm::dot(v - m_bindPos[i], axis);
                    if (t >= 0.0f && t <= kSegmentNear * length) {
                        kept.push_back(v);
                    }
                }
                if (kept.size() >= kMinPoints) {
                    own[i] = kept;
                }
            }
        }
        std::vector<std::size_t> extreme;
        for (const glm::vec3& d : directions) {
            std::size_t best = 0;
            float       far = -1.0e9f;
            for (std::size_t k = 0; k < own[i].size(); ++k) {
                const float t = glm::dot(own[i][k] - m_bindPos[i], d);
                if (t > far) {
                    far = t;
                    best = k;
                }
            }
            if (std::find(extreme.begin(), extreme.end(), best) == extreme.end()) {
                extreme.push_back(best);
            }
        }
        for (const std::size_t k : extreme) {
            m_fleshPoints[i].push_back(own[i][k] - m_bindPos[i]);
        }
        if (kTrace) {
            std::fprintf(stderr, "[floor] bone %zu: %zu own points, %zu extreme; down %.1f back %.1f front %.1f mm\n", i,
                         own[i].size(), extreme.size(), floorClearanceAlong(static_cast<int>(i), glm::vec3(0.0f, -1.0f, 0.0f)) * 1000.0f,
                         floorClearanceAlong(static_cast<int>(i), glm::vec3(0.0f, 0.0f, -1.0f)) * 1000.0f,
                         floorClearanceAlong(static_cast<int>(i), glm::vec3(0.0f, 0.0f, 1.0f)) * 1000.0f);
        }
    }
    // A joint with no skin of its own to speak of, right beside one that has (the solve's root —
    // the hip — sits 2cm from the pelvis bone, which carries all the flesh): its neighbour's,
    // about ITS joint.
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || !m_fleshPoints[i].empty() || m_bindPos[i].y < kContactHeight * m_sizeScale) {
            continue;
        }
        std::size_t from = n;
        for (std::size_t k = 0; k < n; ++k) {
            const bool neighbour = m_parents[k] == static_cast<int>(i) || m_parents[i] == static_cast<int>(k);
            if (neighbour && !own[k].empty() && own[k].size() >= kMinPoints &&
                glm::length(m_bindPos[k] - m_bindPos[i]) < 0.05f * m_sizeScale &&
                (from == n || own[k].size() > own[from].size())) {
                from = k;
            }
        }
        if (from < n) {
            for (const glm::vec3& v : m_fleshPoints[from]) {
                m_fleshPoints[i].push_back(v + m_bindPos[from] - m_bindPos[i]);
            }
            m_fleshBorrowed[i] = 1;
        }
    }
    m_fleshRows.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        m_fleshRows[i] = !m_fleshPoints[i].empty() && i < m_masses.size() && m_masses[i] >= kTokenBoneMass;
    }
    // The clearances at the bind pose (the rig's rest; the Armature hands over the pose's before
    // anything reads them in a drag).
    std::vector<glm::mat3> rest(n, glm::mat3(1.0f));
    setBoneRotations(rest);
}

float IkRig::floorClearanceAlong(int node, const glm::vec3& downInBoneFrame) const {
    if (node < 0 || static_cast<std::size_t>(node) >= m_fleshPoints.size() ||
        m_fleshPoints[static_cast<std::size_t>(node)].empty() || glm::length(downInBoneFrame) < 1.0e-6f) {
        return floorClearance(node);
    }
    const glm::vec3 down = glm::normalize(downInBoneFrame);
    float           reach = kFleshRadius * m_sizeScale; // (never less than the old radius)
    for (const glm::vec3& v : m_fleshPoints[static_cast<std::size_t>(node)]) {
        reach = std::max(reach, glm::dot(v, down));
    }
    return reach;
}

void IkRig::setBoneRotations(const std::vector<glm::mat3>& worldRotations) {
    const std::size_t n = std::min(worldRotations.size(), m_fleshPoints.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_fleshPoints[i].empty()) {
            // (The world's DOWN in the bone's frame.)
            m_floorClearance[i] =
                floorClearanceAlong(static_cast<int>(i), glm::transpose(worldRotations[i]) * glm::vec3(0.0f, -1.0f, 0.0f));
        }
    }
}

} // namespace pose
