/**
 * @file ikrig.cpp
 * @brief IkRig construction: the per-figure FBIK policy state built once from the bind skeleton
 *        (the body tree, the masses, the floor clearances, the mesh-fitted body volumes), plus
 *        the metric helpers every drag phase shares (path lengths, socket leashes, limb
 *        junctions) and the volume queries (the drag-start mask, the drag-target clamp).
 *
 * The rig's implementation spans five translation units so the phases can evolve independently:
 * this file, ikrigdrag.cpp (beginDrag / plantContacts — contacts, pins, user pins, promotion),
 * ikrigpolicy.cpp (the per-tick intents, the lift-off test, the step policy's entry),
 * ikrigstepping.cpp (balance- and anchor-driven foot re-planting) and ikrigcontacts.cpp (live
 * contact re-detection, landings, pin-set maintenance); the tuning constants they share live in
 * ikrig_constants.h. build() hosts the load-bearing rooting decision: the FBIK root is the
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
#include <initializer_list>
#include <cctype>
#include <cmath>
#include <string>
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
    // share — it is on the chain and its share is harmless there.
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_children[i].empty()) {
            continue;
        }
        std::string lower;
        for (const char c : names[i]) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower.find("twist") != std::string::npos) {
            m_masses[i] = std::min(m_masses[i], 0.0015f);
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
        const float bindY = m_bindPos[i].y;
        m_floorClearance[i] = bindY < 0.15f * m_sizeScale ? std::max(bindY, 0.0f)
                                                          : 0.015f * m_sizeScale;
    }
    // BODY VOLUMES (see bodyVolumes()): the trunk and the limb roots by tolerant NAME (the
    // fragments the mass table uses), the radii from the hip-socket spacing so they scale with
    // the rig, each volume applied to the other limbs' joints below their roots.
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
    // detection measures a joint's height through them.
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
            m_ridingDescendants[i].push_back(c);
            stack.insert(stack.end(), m_children[ci].begin(), m_children[ci].end());
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
        if (!m_bodyNode[i] || m_bindPos[i].y < 0.15f * m_sizeScale || own[i].size() < kMinPoints) {
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
        if (!m_bodyNode[i] || !m_fleshPoints[i].empty() || m_bindPos[i].y < 0.15f * m_sizeScale) {
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
    float           reach = 0.015f * m_sizeScale; // (never less than the old radius)
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

void IkRig::buildBodyVolumes(const std::vector<std::string>& names, const std::vector<BodyMeshPoint>* mesh) {
    const std::size_t n = m_bindPos.size();
    m_bodyVolumes.clear();
    m_volumeClearance.assign(n, 0.015f * m_sizeScale);
    m_volumeSegmentRadius.assign(n, 0.0f);
    m_riderHand.assign(n, -1);
    m_riderClearance = kRiderClearance * m_sizeScale;
    if (n == 0 || names.size() != n) {
        return;
    }
    const auto norm = [](const std::string& name) {
        std::string out;
        for (const char c : name) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return out;
    };
    const auto has = [](const std::string& s, std::initializer_list<const char*> frags) {
        for (const char* f : frags) {
            if (s.find(f) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    const auto sideOf = [](const std::string& s) -> int {
        if (s.rfind("left", 0) == 0) return 0;
        if (s.rfind("right", 0) == 0) return 1;
        if (!s.empty() && s[0] == 'l') return 0;
        if (!s.empty() && s[0] == 'r') return 1;
        return -1;
    };
    std::vector<std::string> nn(n);
    for (std::size_t i = 0; i < n; ++i) {
        nn[i] = norm(names[i]);
    }
    const auto isArm = [&](int i) { return has(nn[static_cast<std::size_t>(i)], {"shldr", "shoulder", "upperarm"}); };
    const auto isThigh = [&](int i) { return has(nn[static_cast<std::size_t>(i)], {"thigh"}); };
    const auto isShin = [&](int i) { return has(nn[static_cast<std::size_t>(i)], {"shin", "calf"}); };
    const auto isNeck = [&](int i) { return has(nn[static_cast<std::size_t>(i)], {"neck"}); };
    // Limb roots: the first arm / thigh-class body node of each side whose parent is not of
    // that class (the shoulder bend bone under the collar; the thigh under the pelvis).
    int armRoot[2] = {-1, -1};
    int thighRoot[2] = {-1, -1};
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || has(nn[i], {"anchor"})) {
            continue;
        }
        const int side = sideOf(nn[i]);
        if (side < 0) {
            continue;
        }
        const int p = m_parents[i];
        if (isArm(static_cast<int>(i)) && (p < 0 || !isArm(p)) && armRoot[side] < 0) {
            armRoot[side] = static_cast<int>(i);
        }
        if (isThigh(static_cast<int>(i)) && (p < 0 || !isThigh(p)) && thighRoot[side] < 0) {
            thighRoot[side] = static_cast<int>(i);
        }
    }
    // Limb membership: 0/1 = the arms, 2/3 = the legs (the root included; excluded from the
    // tested set below).
    std::vector<int> limbOf(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (int cur = static_cast<int>(i); cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur == armRoot[0]) { limbOf[i] = 0; break; }
            if (cur == armRoot[1]) { limbOf[i] = 1; break; }
            if (cur == thighRoot[0]) { limbOf[i] = 2; break; }
            if (cur == thighRoot[1]) { limbOf[i] = 3; break; }
        }
    }
    int knee[2] = {-1, -1};
    int ankle[2] = {-1, -1};
    int elbow[2] = {-1, -1};
    int hand[2] = {-1, -1};
    int upperArm[2] = {-1, -1}; // a node NAMED upperarm (the newest generation's arm root
                                // is its collar, "shoulder"); else the arm root
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || has(nn[i], {"anchor"})) {
            continue;
        }
        const int leg = limbOf[i] >= 2 ? limbOf[i] - 2 : -1;
        const int arm = (limbOf[i] == 0 || limbOf[i] == 1) ? limbOf[i] : -1;
        if (leg >= 0 && isShin(static_cast<int>(i)) && knee[leg] < 0) knee[leg] = static_cast<int>(i);
        if (leg >= 0 && has(nn[i], {"foot"}) && ankle[leg] < 0) ankle[leg] = static_cast<int>(i);
        if (arm >= 0 && has(nn[i], {"forearm", "elbow"}) && !has(nn[i], {"twist"}) && elbow[arm] < 0) {
            elbow[arm] = static_cast<int>(i);
        }
        if (arm >= 0 && has(nn[i], {"hand"}) && hand[arm] < 0) hand[arm] = static_cast<int>(i);
        if (arm >= 0 && has(nn[i], {"upperarm"}) && !has(nn[i], {"twist"}) && upperArm[arm] < 0) {
            upperArm[arm] = static_cast<int>(i);
        }
    }
    for (int side = 0; side < 2; ++side) {
        if (upperArm[side] < 0) {
            upperArm[side] = armRoot[side];
        }
    }
    int neckBase = -1;
    int headNode = -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (!m_bodyNode[i] || limbOf[i] >= 0) {
            continue;
        }
        const int p = m_parents[i];
        if (neckBase < 0 && isNeck(static_cast<int>(i)) && (p < 0 || !isNeck(p))) {
            neckBase = static_cast<int>(i);
        }
        if (headNode < 0 && has(nn[i], {"head"})) {
            headNode = static_cast<int>(i);
        }
    }
    m_headNode = headNode;
    m_neckBase = neckBase;
    // Hands and feet carry their fingers and toes outside the joint: a wider clearance.
    for (std::size_t i = 0; i < n; ++i) {
        if (m_bodyNode[i] && !has(nn[i], {"anchor"}) &&
            (has(nn[i], {"hand"}) || has(nn[i], {"foot"}))) {
            m_volumeClearance[i] = 0.02f * m_sizeScale;
        }
    }
    const float halfSpacing =
        (thighRoot[0] >= 0 && thighRoot[1] >= 0)
            ? 0.5f * std::abs(m_bindPos[static_cast<std::size_t>(thighRoot[0])].x -
                              m_bindPos[static_cast<std::size_t>(thighRoot[1])].x)
            : 0.08f * m_sizeScale;
    // RIDERS of a hand or a foot — the fingers, thumbs, carpals, toes, metatarsals — are never
    // tested as JOINTS, whatever mass their subtrees sum to: the hand joint stands for the
    // hand, the foot joint for the foot (their clearance is the wider one for it). The mass
    // rule alone let the third generation's THUMB through: its subtree cleared the token
    // threshold, and the other arm's capsules pushed the thumb 7cm while the hand it rides sat
    // outside them. A HAND's riders are tested as riders instead (kVolumeAppliesRider) against
    // the torso and the head, where their positions are exact: a hand pressed into the chest
    // stops at its fingertips.
    std::vector<char> rider(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (int cur = m_parents[i]; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur == hand[0] || cur == hand[1]) {
                rider[i] = 1;
                m_riderHand[i] = cur;
                break;
            }
            if (cur == ankle[0] || cur == ankle[1]) {
                rider[i] = 1;
                break;
            }
        }
    }
    static const bool kTrace = std::getenv("IK_VOLUME_TRACE") != nullptr;
    static const bool kNoMesh = std::getenv("IK_VOLUME_NO_MESH") != nullptr;     // A/B probe
    static const bool kNoRiders = std::getenv("IK_VOLUME_NO_RIDERS") != nullptr; // A/B probe
    const bool fitted = mesh != nullptr && !mesh->empty() && !kNoMesh;

    // ------------------------------------------------------------------------------------
    // THE MESH FIT. With a body mesh sample (bodymesh.h) each capsule is sized from the
    // vertices that follow its bones: a limb is a TAPERED capsule (the median radial distance
    // regressed to its ends: the thigh's hip flesh and its knee), the torso a stack of
    // per-bone segments, each covered by a centre capsule and two side capsules whose axes
    // run ahead of and beside the spine (the section's front/back extent gives the depth and
    // the forward offset — the breasts; its width the sides), the head a sphere-ended capsule
    // reaching the crown. Without a sample the volumes are sized from the hip-socket
    // half-spacing as before (and IK_VOLUME_NO_MESH=1 forces that, the A/B probe).
    // ------------------------------------------------------------------------------------
    constexpr int   kBands = 4;            // bands along the axis for the taper regression
    constexpr int   kMinBandPoints = 24;   // a band with fewer points says nothing
    constexpr float kLimbPct = 0.5f;       // a limb's radius: the median radial distance
    constexpr float kInnerPct = 0.8f;      // a leg's inner surface: the extent toward the other leg
    constexpr float kSidePct = 0.85f;      // a torso section's half-width
    constexpr float kFrontPct = 0.9f;      // ... its front
    constexpr float kBackPct = 0.1f;       // ... and its back
    constexpr float kHeadPct = 0.75f;      // the skull's radius
    constexpr float kFitLo = -0.1f;        // the axis window a member point must fall in
    constexpr float kFitHi = 1.1f;
    // The member points of a capsule from a to b: the bones on the chain from b (exclusive,
    // unless includeB) up to a (inclusive) and their RIDERS — off-chain descendants whose
    // subtree is token (the pectorals on the chest, the newest generation's leaf twist
    // helpers on the thigh, the face on the head), never a real limb or the next chain node;
    // the root segment also takes the PELVIS bone (the buttocks), a real-mass off-chain child
    // of the hip that is not a limb.
    const auto chainMembers = [&](int a, int b, bool includeB) {
        std::vector<char> member(n, 0);
        std::vector<int> chain;
        bool reached = false;
        for (int cur = b; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            if (cur != b || includeB) {
                chain.push_back(cur);
            }
            if (cur == a) {
                reached = true;
                break;
            }
        }
        if (!reached) {
            return member;
        }
        std::vector<char> onChain(n, 0);
        for (const int c : chain) {
            onChain[static_cast<std::size_t>(c)] = 1;
        }
        onChain[static_cast<std::size_t>(b)] = 1;
        for (const int c : chain) {
            member[static_cast<std::size_t>(c)] = 1;
            std::vector<int> stack(m_children[static_cast<std::size_t>(c)].begin(),
                                   m_children[static_cast<std::size_t>(c)].end());
            while (!stack.empty()) {
                const int d = stack.back();
                stack.pop_back();
                const std::size_t di = static_cast<std::size_t>(d);
                if (onChain[di] || !m_bodyNode[di]) {
                    continue;
                }
                const bool token = m_subtreeMass[di] < kTokenLimbMass;
                const bool pelvis = c == m_pelvis && limbOf[di] < 0 && m_parents[di] == m_pelvis;
                if (!token && !pelvis) {
                    continue;
                }
                member[di] = 1;
                stack.insert(stack.end(), m_children[di].begin(), m_children[di].end());
            }
        }
        return member;
    };
    const auto pct = [](std::vector<float>& v, float p) -> float {
        if (v.empty()) {
            return 0.0f;
        }
        std::size_t k = static_cast<std::size_t>(p * static_cast<float>(v.size() - 1) + 0.5f);
        k = std::min(k, v.size() - 1);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    };
    // A per-band statistic (the percentile of each band's values) regressed to the axis ends:
    // a straight line through the band centres, read at t = 0 and t = 1. ok = false if no
    // band had enough points.
    struct EndPair {
        float a = 0.0f;
        float b = 0.0f;
        bool  ok = false;
    };
    const auto regress = [&](std::vector<std::vector<float>>& bands, float p) {
        EndPair out;
        float sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
        int count = 0;
        for (int k = 0; k < kBands; ++k) {
            std::vector<float>& band = bands[static_cast<std::size_t>(k)];
            if (static_cast<int>(band.size()) < kMinBandPoints) {
                continue;
            }
            const float x = kFitLo + (kFitHi - kFitLo) * (static_cast<float>(k) + 0.5f) / static_cast<float>(kBands);
            const float y = pct(band, p);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
            ++count;
        }
        if (count == 0) {
            return out;
        }
        out.ok = true;
        if (count == 1) {
            out.a = out.b = sy;
            return out;
        }
        const float cnt = static_cast<float>(count);
        const float denom = cnt * sxx - sx * sx;
        const float slope = std::abs(denom) > 1e-9f ? (cnt * sxy - sx * sy) / denom : 0.0f;
        const float icpt = (sy - slope * sx) / cnt;
        out.a = icpt;
        out.b = icpt + slope;
        return out;
    };
    // The points of @p member along the axis A0->B0 (already extended), binned into bands by
    // their parameter t: the radial distance, and — with a frame — the lateral (|u|) and
    // forward (w) components of the radial vector.
    struct Bands {
        std::vector<std::vector<float>> r, u, w;
        std::vector<float> axial; // t * L of every member point (the head's crown)
        int total = 0;
    };
    const auto collect = [&](const std::vector<char>& member, const glm::vec3& A0, const glm::vec3& B0,
                             const glm::vec3* lat, const glm::vec3* fwd, const glm::vec3* half) {
        Bands out;
        out.r.assign(kBands, {});
        out.u.assign(kBands, {});
        out.w.assign(kBands, {});
        const glm::vec3 ab = B0 - A0;
        const float L = glm::length(ab);
        if (L < 1e-6f) {
            return out;
        }
        const glm::vec3 up = ab / L;
        for (const BodyMeshPoint& pt : *mesh) {
            if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                continue;
            }
            const glm::vec3 rel = pt.pos - A0;
            const float axial = glm::dot(rel, up);
            const float t = axial / L;
            out.axial.push_back(axial);
            if (t < kFitLo || t > kFitHi) {
                continue;
            }
            int k = static_cast<int>((t - kFitLo) / (kFitHi - kFitLo) * static_cast<float>(kBands));
            k = std::max(0, std::min(kBands - 1, k));
            const glm::vec3 radial = rel - up * axial;
            if (half != nullptr) {
                // The limb's extent TOWARD @p half (its inner surface): the far side and the
                // front/back are not the surface this capsule stands for.
                const float toward = glm::dot(radial, *half);
                if (toward <= 0.0f) {
                    continue;
                }
                out.r[static_cast<std::size_t>(k)].push_back(toward);
                ++out.total;
                continue;
            }
            out.r[static_cast<std::size_t>(k)].push_back(glm::length(radial));
            if (lat != nullptr && fwd != nullptr) {
                out.u[static_cast<std::size_t>(k)].push_back(std::abs(glm::dot(radial, *lat)));
                out.w[static_cast<std::size_t>(k)].push_back(glm::dot(radial, *fwd));
            }
            ++out.total;
        }
        return out;
    };
    // The section frame at bind for lateral references (refR -> refL, orthogonalized to the
    // axis; forward = their cross) — the same frame volumeAxis rebuilds in every pose.
    const auto bindFrame = [&](int refL, int refR, const glm::vec3& up, glm::vec3& lat, glm::vec3& fwd) {
        if (refL < 0 || refR < 0) {
            return false;
        }
        lat = m_bindPos[static_cast<std::size_t>(refL)] - m_bindPos[static_cast<std::size_t>(refR)];
        lat -= up * glm::dot(lat, up);
        if (glm::length(lat) < 1e-6f) {
            return false;
        }
        lat = glm::normalize(lat);
        fwd = glm::cross(lat, up);
        return true;
    };

    struct VolumeSpec {
        int              a = -1;
        int              b = -1;
        float            radius = 0.0f;
        float            radiusB = -1.0f;
        float            extendA = 0.0f;
        float            extendB = 0.0f;
        glm::vec3        offA{0.0f};
        glm::vec3        offB{0.0f};
        int              refL = -1;
        int              refR = -1;
        std::vector<int> limbs;
        bool             segments = false;
        bool             riders = false;
        std::vector<char> segmentNodes; // per node: its segment is tested even without `segments` (kVolumeAppliesJointSegment)
        const char*      what = "";
    };
    const auto addVolume = [&](const VolumeSpec& spec) {
        if (spec.a < 0 || spec.b < 0 || spec.radius <= 0.0f) {
            return;
        }
        BodyVolume v;
        v.a = spec.a;
        v.b = spec.b;
        v.radius = spec.radius;
        v.radiusB = spec.radiusB;
        v.extendA = spec.extendA;
        v.extendB = spec.extendB;
        v.offA = spec.offA;
        v.offB = spec.offB;
        v.refL = spec.refL;
        v.refR = spec.refR;
        v.segments = spec.segments;
        v.applies.assign(n, 0);
        // The capsule at BIND, for the rest-overlap exemption.
        const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return m_bindPos[static_cast<std::size_t>(i)]; });
        if (!ax.ok) {
            return;
        }
        int applied = 0;
        int ridersApplied = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!m_bodyNode[i] || limbOf[i] < 0 || has(nn[i], {"anchor"})) {
                continue;
            }
            const int limb = limbOf[i];
            if (std::find(spec.limbs.begin(), spec.limbs.end(), limb) == spec.limbs.end()) {
                continue;
            }
            char applies = 0;
            float clearance = 0.0f;
            if (rider[i]) {
                // A hand's rider, where the volume takes riders.
                if (!spec.riders || kNoRiders || m_riderHand[i] < 0) {
                    continue;
                }
                applies = kVolumeAppliesRider;
                clearance = m_riderClearance;
            } else {
                // Never a token-LIMB joint (the fingers, the toes): riders the solve never
                // places (they follow the hand rigidly), so a thumb can still touch the chest
                // — as it would — while the hand keeps its clearance. A mid-limb twist helper
                // (its subtree carries the hand) IS tested: it samples the forearm's middle,
                // where a forearm laid across the chest cuts through it with the elbow and
                // the hand both outside.
                const int root = limb < 2 ? armRoot[limb] : thighRoot[limb - 2];
                if (m_subtreeMass[i] < kTokenLimbMass || static_cast<int>(i) == root) {
                    continue;
                }
                applies = (i < spec.segmentNodes.size() && spec.segmentNodes[i]) ? kVolumeAppliesJointSegment
                                                                                  : kVolumeAppliesJoint;
                clearance = m_volumeClearance[i];
            }
            const glm::vec3& P = m_bindPos[i];
            const float t = ax.ab2 > 1e-12f ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
            const float dist = glm::length(P - (ax.A + ax.ab * t));
            if (dist < volumeRadiusAt(v, t) + clearance) {
                if (kTrace) {
                    std::printf("[volume] %s overlaps volume %d-%d at bind (%.1f < %.1f mm): exempt\n",
                                names[i].c_str(), spec.a, spec.b, dist * 1000.0f,
                                (volumeRadiusAt(v, t) + clearance) * 1000.0f);
                }
                continue;
            }
            v.applies[i] = applies;
            if (applies == kVolumeAppliesRider) {
                ++ridersApplied;
            } else {
                ++applied;
            }
        }
        if (kTrace) {
            std::printf("[volume] %s %s-%s radius %.1f/%.1f mm ext %.1f/%.1f off (%.1f,%.1f)/(%.1f,%.1f)%s applies to %d joints, %d riders\n",
                        spec.what, names[static_cast<std::size_t>(spec.a)].c_str(),
                        names[static_cast<std::size_t>(spec.b)].c_str(), v.radius * 1000.0f,
                        volumeRadiusAt(v, 1.0f) * 1000.0f, v.extendA * 1000.0f, v.extendB * 1000.0f,
                        v.offA.x * 1000.0f, v.offA.z * 1000.0f, v.offB.x * 1000.0f, v.offB.z * 1000.0f,
                        v.segments ? " segments" : "", applied, ridersApplied);
        }
        m_bodyVolumes.push_back(std::move(v));
    };
    // A limb capsule (a -> b, circular, tapered) fitted to its bones' vertices; @p fallback is
    // the skeleton-sized radius, which also bounds the fit (a sample assigned to the wrong
    // bones must not make a thigh a metre wide or a wrist a pin). With @p half, only the
    // vertices on that side of the axis count: a LEG's capsule stands for its INNER surface,
    // the one the other leg meets — a thigh is 10cm to its outer hip flesh and 6cm to the
    // inner thigh, and an all-round radius held the crossing legs 15cm apart at the crotch,
    // where they rest touching (the legs-crossed knee trembled 440mm against it).
    const auto fitLimb = [&](VolumeSpec& spec, float fallback, const glm::vec3* half) {
        spec.radius = fallback;
        spec.radiusB = -1.0f;
        if (!fitted) {
            return;
        }
        const std::vector<char> member = chainMembers(spec.a, spec.b, false);
        const glm::vec3 A0 = m_bindPos[static_cast<std::size_t>(spec.a)];
        const glm::vec3 B0 = m_bindPos[static_cast<std::size_t>(spec.b)];
        glm::vec3 halfDir(0.0f);
        const glm::vec3* halfPtr = nullptr;
        if (half != nullptr && glm::length(B0 - A0) > 1e-6f) {
            const glm::vec3 up = glm::normalize(B0 - A0);
            halfDir = *half - up * glm::dot(*half, up);
            if (glm::length(halfDir) > 1e-6f) {
                halfDir = glm::normalize(halfDir);
                halfPtr = &halfDir;
            }
        }
        // The inner surface takes a higher percentile than the all-round median: the median of
        // an extent is half-way to the surface (the points spread from the axis outward).
        Bands bands = collect(member, A0, B0, nullptr, nullptr, halfPtr);
        const EndPair r = regress(bands.r, halfPtr != nullptr ? kInnerPct : kLimbPct);
        if (!r.ok) {
            return;
        }
        spec.radius = glm::clamp(r.a, 0.5f * fallback, 2.5f * fallback);
        spec.radiusB = glm::clamp(r.b, 0.5f * fallback, 2.5f * fallback);
    };
    // The limbs against the torso and the head; the legs against each other; the arms against
    // each other. (The arms answer to no leg volume and the legs to no arm volume: hands rest
    // on knees and thighs, hang beside them in a crouch and pass them on the way to the floor
    // — the all-fours descent on the oldest generation stopped 17cm up with the hands held off
    // the thighs — and a hand through a thigh is a rare failure next to that.)
    const std::vector<int> arms{0, 1};
    // ---- THE TORSO: fitted as ONE PROFILE along the whole spine (banded by height, each
    // band's section measured over thousands of vertices), sampled at each spine joint for
    // that bone's capsules. A per-bone fit was tried first and rejected: a spine bone is
    // 3-10cm long, its vertices a thin slab that interleaves with its neighbours', and a
    // taper regressed within one slab extrapolated nonsense (a neck segment 137mm wide, a
    // chest 50mm at one end and 120mm at the other).
    bool torsoFitted = false;
    if (fitted && m_pelvis >= 0 && neckBase >= 0) {
        std::vector<int> chain; // the neck base down to the root
        bool reached = false;
        for (int cur = neckBase; cur >= 0; cur = m_parents[static_cast<std::size_t>(cur)]) {
            chain.push_back(cur);
            if (cur == m_pelvis) {
                reached = true;
                break;
            }
        }
        // The lateral references: the arm roots for the segments at and above the chest node
        // the arms hang from, the hip sockets below (a twisted trunk turns its shoulders).
        int armJunction = -1;
        if (armRoot[0] >= 0) {
            for (int cur = m_parents[static_cast<std::size_t>(armRoot[0])]; cur >= 0;
                 cur = m_parents[static_cast<std::size_t>(cur)]) {
                if (std::find(chain.begin(), chain.end(), cur) != chain.end()) {
                    armJunction = cur;
                    break;
                }
            }
        }
        const bool haveArms = armRoot[0] >= 0 && armRoot[1] >= 0;
        const bool haveHips = thighRoot[0] >= 0 && thighRoot[1] >= 0;
        int fittedSegments = 0;
        const glm::vec3 rootPos = m_bindPos[static_cast<std::size_t>(m_pelvis)];
        const glm::vec3 neckPos = m_bindPos[static_cast<std::size_t>(neckBase)];
        const float L0 = glm::length(neckPos - rootPos);
        glm::vec3 lat, fwd;
        const glm::vec3 up = L0 > 1e-6f ? (neckPos - rootPos) / L0 : glm::vec3(0.0f, 1.0f, 0.0f);
        const bool frameOk = haveHips ? bindFrame(thighRoot[0], thighRoot[1], up, lat, fwd)
                                      : bindFrame(armRoot[0], armRoot[1], up, lat, fwd);
        if (reached && chain.size() >= 2 && (haveArms || haveHips) && L0 > 1e-6f && frameOk) {
            const float fallback = 1.2f * halfSpacing;
            // The axis: the root's point extended down to the hip sockets' level (the pelvis
            // flesh), up to the neck base.
            float extendRoot = 0.0f;
            if (haveHips) {
                const float socketY = 0.5f * (m_bindPos[static_cast<std::size_t>(thighRoot[0])].y +
                                              m_bindPos[static_cast<std::size_t>(thighRoot[1])].y);
                extendRoot = std::max(0.0f, rootPos.y - socketY);
            }
            const glm::vec3 A0 = rootPos - up * extendRoot;
            const float Ltot = L0 + extendRoot;
            const std::vector<char> member = chainMembers(m_pelvis, neckBase, false);
            const int nb = std::max(4, std::min(16, static_cast<int>(Ltot / 0.05f + 0.5f)));
            std::vector<std::vector<float>> bu(static_cast<std::size_t>(nb)), bw(static_cast<std::size_t>(nb));
            for (const BodyMeshPoint& pt : *mesh) {
                if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                    continue;
                }
                const glm::vec3 rel = pt.pos - A0;
                const float axial = glm::dot(rel, up);
                if (axial < -0.02f || axial > Ltot + 0.02f) {
                    continue;
                }
                const int k = std::max(0, std::min(nb - 1, static_cast<int>(axial / Ltot * static_cast<float>(nb))));
                const glm::vec3 radial = rel - up * axial;
                bu[static_cast<std::size_t>(k)].push_back(std::abs(glm::dot(radial, lat)));
                bw[static_cast<std::size_t>(k)].push_back(glm::dot(radial, fwd));
            }
            std::vector<float> width(static_cast<std::size_t>(nb)), front(static_cast<std::size_t>(nb)), back(static_cast<std::size_t>(nb));
            std::vector<char> valid(static_cast<std::size_t>(nb), 0);
            int validCount = 0;
            for (int k = 0; k < nb; ++k) {
                const std::size_t ki = static_cast<std::size_t>(k);
                if (static_cast<int>(bu[ki].size()) < kMinBandPoints) {
                    continue;
                }
                std::vector<float> w2 = bw[ki];
                width[ki] = pct(bu[ki], kSidePct);
                front[ki] = pct(bw[ki], kFrontPct);
                back[ki] = pct(w2, kBackPct);
                valid[ki] = 1;
                ++validCount;
            }
            // An empty band (none expected) takes its nearest measured neighbour's section.
            for (int k = 0; k < nb && validCount > 0; ++k) {
                if (valid[static_cast<std::size_t>(k)]) {
                    continue;
                }
                int best = -1;
                for (int d = 1; d < nb && best < 0; ++d) {
                    if (k - d >= 0 && valid[static_cast<std::size_t>(k - d)]) best = k - d;
                    else if (k + d < nb && valid[static_cast<std::size_t>(k + d)]) best = k + d;
                }
                const std::size_t bi = static_cast<std::size_t>(best);
                width[static_cast<std::size_t>(k)] = width[bi];
                front[static_cast<std::size_t>(k)] = front[bi];
                back[static_cast<std::size_t>(k)] = back[bi];
            }
            // The section at an axial position: interpolated between the band centres.
            const auto sample = [&](float axial, float& w, float& f, float& b) {
                const float x = axial / Ltot * static_cast<float>(nb) - 0.5f;
                const int k0 = std::max(0, std::min(nb - 1, static_cast<int>(std::floor(x))));
                const int k1 = std::max(0, std::min(nb - 1, k0 + 1));
                const float f01 = std::max(0.0f, std::min(1.0f, x - static_cast<float>(k0)));
                const std::size_t i0 = static_cast<std::size_t>(k0), i1 = static_cast<std::size_t>(k1);
                w = width[i0] + (width[i1] - width[i0]) * f01;
                f = front[i0] + (front[i1] - front[i0]) * f01;
                b = back[i0] + (back[i1] - back[i0]) * f01;
            };
            if (kTrace && validCount > 0) {
                for (int k = 0; k < nb; ++k) {
                    const std::size_t ki = static_cast<std::size_t>(k);
                    std::printf("[volume] torso band %d at %.0f mm: half-width %.0f front %.0f back %.0f (%zu points)\n", k,
                                (static_cast<float>(k) + 0.5f) / static_cast<float>(nb) * Ltot * 1000.0f,
                                width[ki] * 1000.0f, front[ki] * 1000.0f, back[ki] * 1000.0f, bu[ki].size());
                }
            }
            bool useArms = false;
            for (std::size_t k = chain.size() - 1; k >= 1 && validCount > 0; --k) {
                const int a = chain[k];
                const int b = chain[k - 1];
                if (a == armJunction) {
                    useArms = true;
                }
                const int refL = (useArms && haveArms) || !haveHips ? armRoot[0] : thighRoot[0];
                const int refR = (useArms && haveArms) || !haveHips ? armRoot[1] : thighRoot[1];
                const float extendA = a == m_pelvis ? extendRoot : 0.0f;
                const float axialA = glm::dot(m_bindPos[static_cast<std::size_t>(a)] - A0, up) - extendA;
                const float axialB = glm::dot(m_bindPos[static_cast<std::size_t>(b)] - A0, up);
                float wA, fA, kA, wB, fB, kB;
                sample(axialA, wA, fA, kA);
                sample(axialB, wB, fB, kB);
                const float depthA = glm::clamp(0.5f * (fA - kA), 0.5f * fallback, 2.5f * fallback);
                const float depthB = glm::clamp(0.5f * (fB - kB), 0.5f * fallback, 2.5f * fallback);
                const float centreA = 0.5f * (fA + kA);
                const float centreB = 0.5f * (fB + kB);
                const float sideA = glm::clamp(wA - depthA, 0.0f, 3.0f * halfSpacing);
                const float sideB = glm::clamp(wB - depthB, 0.0f, 3.0f * halfSpacing);
                VolumeSpec centre;
                centre.a = a;
                centre.b = b;
                centre.radius = depthA;
                centre.radiusB = depthB;
                centre.extendA = extendA;
                centre.offA = glm::vec3(0.0f, 0.0f, centreA);
                centre.offB = glm::vec3(0.0f, 0.0f, centreB);
                centre.refL = refL;
                centre.refR = refR;
                centre.limbs = arms;
                centre.riders = true;
                // The fitted torso tests the UPPER ARM's segments (kVolumeAppliesJointSegment:
                // the nodes from the elbow up to the arm root): laid against the ribs the
                // upper arm is kept out by its own flesh, not only at its two joints. Not the
                // forearm's and hand's, which sweep across the chest and the breasts (held to
                // their flesh they stopped the cross-body reach: the base rig stepped after
                // the cursor, the male's hand stalled 23cm short). The coarse cylinder could
                // test no segment at all. IK_VOLUME_TORSO_NO_SEGMENTS=1 is the A/B probe.
                static const bool kTorsoNoSegments = std::getenv("IK_VOLUME_TORSO_NO_SEGMENTS") != nullptr;
                if (!kTorsoNoSegments) {
                    centre.segmentNodes.assign(n, 0);
                    for (int side = 0; side < 2; ++side) {
                        for (int cur = elbow[side]; cur >= 0 && cur != upperArm[side] && cur != armRoot[side];
                             cur = m_parents[static_cast<std::size_t>(cur)]) {
                            centre.segmentNodes[static_cast<std::size_t>(cur)] = 1;
                        }
                    }
                }
                centre.what = "torso";
                addVolume(centre);
                ++fittedSegments;
                if (sideA > 0.01f || sideB > 0.01f) {
                    for (int side = 0; side < 2; ++side) {
                        VolumeSpec sv = centre;
                        const float sign = side == 0 ? 1.0f : -1.0f;
                        sv.offA.x = sign * sideA;
                        sv.offB.x = sign * sideB;
                        sv.what = side == 0 ? "torso-left" : "torso-right";
                        addVolume(sv);
                    }
                }
            }
        }
        torsoFitted = fittedSegments > 0;
    }
    if (!torsoFitted && m_pelvis >= 0 && neckBase >= 0) {
        VolumeSpec torso;
        torso.a = m_pelvis;
        torso.b = neckBase;
        torso.radius = 1.2f * halfSpacing;
        torso.limbs = arms;
        torso.riders = true;
        torso.what = "torso";
        addVolume(torso);
    }
    // ---- THE HEAD: the neck base through the head joint, the skull past it.
    if (neckBase >= 0 && headNode >= 0) {
        VolumeSpec head;
        head.a = neckBase;
        head.b = headNode;
        head.radius = 1.0f * halfSpacing;
        head.extendB = 0.75f * halfSpacing;
        head.limbs = arms;
        head.riders = true;
        head.what = "head";
        if (fitted) {
            const std::vector<char> member = chainMembers(neckBase, headNode, true);
            const glm::vec3 A0 = m_bindPos[static_cast<std::size_t>(neckBase)];
            const glm::vec3 B0 = m_bindPos[static_cast<std::size_t>(headNode)];
            const float L = glm::length(B0 - A0);
            // The skull: the radial percentile over the head's own points, the crown from the
            // axial extent; the window is the head itself (the joint on), not the neck.
            std::vector<float> radial;
            std::vector<float> axialAll;
            if (L > 1e-6f) {
                const glm::vec3 up = (B0 - A0) / L;
                for (const BodyMeshPoint& pt : *mesh) {
                    if (pt.bone < 0 || pt.bone >= static_cast<int>(n) || !member[static_cast<std::size_t>(pt.bone)]) {
                        continue;
                    }
                    const glm::vec3 rel = pt.pos - A0;
                    const float axial = glm::dot(rel, up);
                    if (axial < L) {
                        continue;
                    }
                    radial.push_back(glm::length(rel - up * axial));
                    axialAll.push_back(axial);
                }
            }
            if (static_cast<int>(radial.size()) >= kMinBandPoints) {
                const float r = glm::clamp(pct(radial, kHeadPct), 0.5f * halfSpacing, 2.0f * halfSpacing);
                const float crown = pct(axialAll, 0.98f);
                head.radius = r;
                head.extendB = glm::clamp(crown - r - L, 0.0f, 2.0f * halfSpacing);
            }
        }
        addVolume(head);
    }
    // ---- THE THIGHS keep the OTHER leg out (legs never cross through each other).
    for (int side = 0; side < 2; ++side) {
        if (thighRoot[side] >= 0 && knee[side] >= 0) {
            VolumeSpec thigh;
            thigh.a = thighRoot[side];
            thigh.b = knee[side];
            thigh.limbs = std::vector<int>{2 + (1 - side)};
            thigh.segments = true;
            thigh.what = "thigh";
            glm::vec3 inner(0.0f);
            const glm::vec3* innerPtr = nullptr;
            if (thighRoot[1 - side] >= 0) {
                inner = m_bindPos[static_cast<std::size_t>(thighRoot[1 - side])] -
                        m_bindPos[static_cast<std::size_t>(thighRoot[side])];
                innerPtr = &inner;
            }
            fitLimb(thigh, 0.9f * halfSpacing, innerPtr);
            addVolume(thigh);
        }
    }
    // LIMB AGAINST LIMB (the self-collision round 2): each SHIN keeps the other leg out (a
    // foot dragged across went around the thigh and through the shin), each UPPER ARM and
    // FOREARM keeps the other arm out (a hand dragged across the chest went through the
    // other forearm).
    static const bool kNoLimbVolumes = std::getenv("IK_VOLUME_NO_LIMBS") != nullptr; // A/B probe
    for (int side = 0; side < 2 && !kNoLimbVolumes; ++side) {
        if (knee[side] >= 0 && ankle[side] >= 0) {
            VolumeSpec shin;
            shin.a = knee[side];
            shin.b = ankle[side];
            shin.limbs = std::vector<int>{2 + (1 - side)};
            shin.segments = true;
            shin.what = "shin";
            glm::vec3 inner(0.0f);
            const glm::vec3* innerPtr = nullptr;
            if (thighRoot[side] >= 0 && thighRoot[1 - side] >= 0) {
                inner = m_bindPos[static_cast<std::size_t>(thighRoot[1 - side])] -
                        m_bindPos[static_cast<std::size_t>(thighRoot[side])];
                innerPtr = &inner;
            }
            fitLimb(shin, 0.55f * halfSpacing, innerPtr);
            addVolume(shin);
        }
        if (upperArm[side] >= 0 && elbow[side] >= 0) {
            VolumeSpec upper;
            upper.a = upperArm[side];
            upper.b = elbow[side];
            upper.limbs = std::vector<int>{1 - side};
            upper.segments = true;
            upper.what = "upper arm";
            fitLimb(upper, 0.55f * halfSpacing, nullptr);
            addVolume(upper);
        }
        if (elbow[side] >= 0 && hand[side] >= 0) {
            // Not past the wrist: hands plant side by side in an all-fours and clasp each
            // other, and a capsule reaching into the palm held them 5cm apart.
            VolumeSpec fore;
            fore.a = elbow[side];
            fore.b = hand[side];
            fore.limbs = std::vector<int>{1 - side};
            fore.segments = true;
            fore.what = "forearm";
            fitLimb(fore, 0.45f * halfSpacing, nullptr);
            addVolume(fore);
        }
    }
    // Every node on a capsule's chain (its b end up to, excluding, its a end) carries that
    // capsule's radius WHERE IT STANDS as the flesh radius of the segment ending at it — the
    // volume rules keep the whole segment out with it (see IkRig::volumeSegmentRadius).
    for (const BodyVolume& v : m_bodyVolumes) {
        const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return m_bindPos[static_cast<std::size_t>(i)]; });
        if (!ax.ok) {
            continue;
        }
        int guard = 0;
        for (int cur = v.b; cur >= 0 && cur != v.a && guard < 64; cur = m_parents[static_cast<std::size_t>(cur)], ++guard) {
            const glm::vec3& P = m_bindPos[static_cast<std::size_t>(cur)];
            const float t = ax.ab2 > 1e-12f ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
            m_volumeSegmentRadius[static_cast<std::size_t>(cur)] =
                std::max(m_volumeSegmentRadius[static_cast<std::size_t>(cur)], volumeRadiusAt(v, t));
        }
    }
    // The rest-overlap exemption for SEGMENTS: a tested joint whose segment already crosses a
    // volume at bind is exempt from that volume, like a joint that sits inside one (the
    // newest generation's collar runs through the chest, and its upper arm read 97mm inside
    // the torso in every pose — the solver then fought the rest pose itself).
    for (BodyVolume& v : m_bodyVolumes) {
        const VolumeAxis ax = volumeAxis(v, static_cast<int>(n), [&](int i) { return m_bindPos[static_cast<std::size_t>(i)]; });
        if (!ax.ok) {
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const int par = m_parents[i];
            const float segR = m_volumeSegmentRadius[i];
            if (!volumeAppliesSegment(v, v.applies[i]) || par < 0 || segR <= 0.0f) {
                continue;
            }
            float sPar = 0.0f;
            float tPar = 0.0f;
            const float dSeg = closestSegmentPoints(m_bindPos[static_cast<std::size_t>(par)],
                                                    m_bindPos[i], ax.A, ax.B, sPar, tPar);
            const float keepSeg = volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, m_volumeClearance[i]);
            if (dSeg < keepSeg) {
                if (kTrace) {
                    std::printf("[volume] the segment into %s crosses volume %d-%d at bind (%.1f < %.1f mm): exempt\n",
                                names[i].c_str(), v.a, v.b, dSeg * 1000.0f, keepSeg * 1000.0f);
                }
                v.applies[i] = 0;
            }
        }
    }
}

void IkRig::maskOverlappingVolumes(const std::vector<glm::vec3>& positions) {
    const int n = static_cast<int>(positions.size());
    m_dragVolumes = m_bodyVolumes;
    if (static_cast<int>(m_volumeClearance.size()) != n) {
        return;
    }
    constexpr float kOverlapTol = 0.002f;
    static const bool kTrace = std::getenv("IK_VOLUME_TRACE") != nullptr;
    for (BodyVolume& v : m_dragVolumes) {
        const VolumeAxis ax = volumeAxis(v, n, [&](int i) { return positions[static_cast<std::size_t>(i)]; });
        if (!ax.ok) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const char applies = v.applies[static_cast<std::size_t>(i)];
            if (!applies) {
                continue;
            }
            const float clearance = applies == kVolumeAppliesRider ? m_riderClearance
                                                                    : m_volumeClearance[static_cast<std::size_t>(i)];
            const glm::vec3& P = positions[static_cast<std::size_t>(i)];
            const float t = ax.ab2 > 1e-12f ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
            const float d = glm::length(P - (ax.A + ax.ab * t));
            bool overlaps = d < volumeRadiusAt(v, t) + clearance - kOverlapTol;
            const int par = m_parents[static_cast<std::size_t>(i)];
            const float segR = m_volumeSegmentRadius[static_cast<std::size_t>(i)];
            if (!overlaps && volumeAppliesSegment(v, applies) && segR > 0.0f && par >= 0) {
                float sPar = 0.0f;
                float tPar = 0.0f;
                const float dSeg = closestSegmentPoints(positions[static_cast<std::size_t>(par)], P, ax.A, ax.B, sPar, tPar);
                overlaps = dSeg < volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, clearance) - kOverlapTol;
            }
            if (overlaps) {
                v.applies[static_cast<std::size_t>(i)] = 0;
                if (kTrace) {
                    std::printf("[volume] node %d overlaps volume %d-%d at drag start: exempt this drag\n",
                                i, v.a, v.b);
                }
            }
        }
    }
}

glm::vec3 IkRig::clampOutOfVolumes(int node, glm::vec3 target,
                                   const std::vector<glm::vec3>& positions) const {
    const int n = static_cast<int>(positions.size());
    if (node < 0 || node >= n || static_cast<int>(m_volumeClearance.size()) != n) {
        return target;
    }
    const auto pos = [&](int i) { return positions[static_cast<std::size_t>(i)]; };
    // The SEGMENT into the effector too (its parent where it stands now — the forearm laid
    // across the chest to a cursor just past the far shoulder cut the chest by 3cm while the
    // cursor itself sat outside the capsule, and the finisher held the hand there): the
    // target is moved out along the segment's closest point, over that point's parameter.
    const int par = m_parents[static_cast<std::size_t>(node)];
    const float segR = volumeSegmentRadius(node);
    for (int round = 0; round < 4 && par >= 0 && segR > 0.0f; ++round) {
        bool moved = false;
        for (const BodyVolume& v : dragVolumes()) {
            const VolumeAxis ax = volumeAxis(v, n, pos);
            if (!ax.ok || !volumeAppliesSegment(v, v.applies[static_cast<std::size_t>(node)])) {
                continue;
            }
            const glm::vec3& PP = positions[static_cast<std::size_t>(par)];
            float sPar = 0.0f;
            float tPar = 0.0f;
            const float dSeg = closestSegmentPoints(PP, target, ax.A, ax.B, sPar, tPar);
            const float keepSeg = volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, m_volumeClearance[static_cast<std::size_t>(node)]) + 0.001f;
            if (dSeg >= keepSeg || sPar < 0.05f) {
                continue;
            }
            const glm::vec3 S = PP + (target - PP) * sPar;
            glm::vec3 nrm = S - (ax.A + ax.ab * tPar);
            if (glm::length(nrm) > 1e-5f) {
                nrm = glm::normalize(nrm);
            } else if (ax.ab2 > 1e-12f) {
                nrm = glm::cross(glm::normalize(ax.ab), target - PP);
                nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : glm::vec3(1.0f, 0.0f, 0.0f);
            } else {
                nrm = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            target += nrm * ((keepSeg - dSeg) / sPar);
            moved = true;
        }
        if (!moved) {
            break;
        }
    }
    const auto pointOut = [&](const BodyVolume& v, const VolumeAxis& ax, const glm::vec3& P, float clearance,
                              glm::vec3& push) {
        const float t = ax.ab2 > 1e-12f ? glm::clamp(glm::dot(P - ax.A, ax.ab) / ax.ab2, 0.0f, 1.0f) : 0.0f;
        const glm::vec3 Q = ax.A + ax.ab * t;
        const float keep = volumeRadiusAt(v, t) + clearance + 0.001f;
        const glm::vec3 away = P - Q;
        const float d = glm::length(away);
        if (d >= keep) {
            return false;
        }
        glm::vec3 nrm(1.0f, 0.0f, 0.0f);
        if (d > 1e-5f) {
            nrm = away / d;
        } else if (ax.ab2 > 1e-12f) {
            nrm = glm::cross(ax.ab / std::sqrt(ax.ab2), glm::vec3(0.0f, 1.0f, 0.0f));
            nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : glm::vec3(1.0f, 0.0f, 0.0f);
        }
        push = nrm * (keep - d);
        return true;
    };
    for (const BodyVolume& v : dragVolumes()) {
        const VolumeAxis ax = volumeAxis(v, n, pos);
        if (!ax.ok || !volumeAppliesJoint(v.applies[static_cast<std::size_t>(node)])) {
            continue;
        }
        glm::vec3 push;
        if (pointOut(v, ax, target, m_volumeClearance[static_cast<std::size_t>(node)], push)) {
            target += push;
        }
    }
    // The effector's RIDERS (a hand's fingers), carried along with it: where the hand would
    // stand at the target, each rider stands at its current offset from the hand, and the
    // target moves so the deepest of them clears the volume — a cursor pushed into the chest
    // asks for the hand at the fingertips' reach, not the wrist's.
    std::vector<int> riders;
    for (int i = 0; i < n; ++i) {
        if (riderHand(i) == node) {
            riders.push_back(i);
        }
    }
    for (int round = 0; round < 3 && !riders.empty(); ++round) {
        bool moved = false;
        for (const BodyVolume& v : dragVolumes()) {
            const VolumeAxis ax = volumeAxis(v, n, pos);
            if (!ax.ok) {
                continue;
            }
            for (const int r : riders) {
                if (v.applies[static_cast<std::size_t>(r)] != kVolumeAppliesRider) {
                    continue;
                }
                const glm::vec3 Pr = target + (positions[static_cast<std::size_t>(r)] - positions[static_cast<std::size_t>(node)]);
                glm::vec3 push;
                if (pointOut(v, ax, Pr, m_riderClearance, push)) {
                    target += push;
                    moved = true;
                }
            }
        }
        if (!moved) {
            break;
        }
    }
    return target;
}

} // namespace pose
