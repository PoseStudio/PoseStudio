/**
 * @file armatureiksolvedrag.cpp
 * @brief The solve's first stages — what the drag IS and which channels answer it (see
 *        armatureiksolve.cpp for the map): the unknowns, the trunk's policy for the tick, the
 *        stiffness classes, and the foot / knee / elbow drag geometry. Also ensureHandMaps, the
 *        per-rig structural map of the hands (found once, read by the rows and the post-steps).
 *
 * Every stage is a member function over the tick's IkSolveScratch (armatureiksolvestate.h) and
 * begins by naming the members it reads and writes. Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvestate.h"
#include "armatureiksolvetuning.h"

#include "balancecontroller.h"
#include "ikmath.h"
#include "ikrig.h"
#include "jointsolver.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace pose {

/// The UNKNOWNS: which bones' channels the solve owns (the active paths, the pins, the toes), the foot contact model's balls, and whether the root rotates.
void Armature::ikChooseUnknowns(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const JointSolver& solver = s.solver;
    const std::vector<IkEffector>& pins = s.pins;
    const std::vector<char>& active = s.active;

    // --- The unknowns ---------------------------------------------------------------------
    s.dofBone.assign(n, 0);
    std::vector<char>& dofBone = s.dofBone;
    for (std::size_t c = 0; c < n && c < active.size(); ++c) {
        if (!active[c] || static_cast<int>(c) == root) {
            continue;
        }
        const int p = m_bones[c].parent;
        if (p >= 0 && solver.isAncestorOrSelf(root, p)) {
            dofBone[static_cast<std::size_t>(p)] = 1;
        }
    }
    for (const IkEffector& pin : pins) {
        if (pin.node >= 0 && pin.node != root && solver.isAncestorOrSelf(root, pin.node)) {
            dofBone[static_cast<std::size_t>(pin.node)] = 1; // its held orientation
        }
    }
    // A dragged WRIST's own channels (2026-09-25; IK_JS_NO_WRIST_DOF): a hand laid on the body or
    // slid along the floor turns at the wrist too (ikCursorRows' lay and slide rows). The effector's
    // own channels move nothing the solve places, so they were never unknowns — the palm was turned
    // by the forearm's twist alone, and a hand laid on top of the head pronated the forearm to its
    // limit and then flipped basins (176mm in a tick, in the hold). With nothing asking for them
    // they rest at their reference: every other hand drag is unchanged.
    static const bool kNoWristDof = std::getenv("IK_JS_NO_WRIST_DOF") != nullptr; // A/B probe
    ensureHandMaps();
    if (!kNoWristDof && effector >= 0 && effector != root && m_jsWristOf.size() == n &&
        m_jsWristOf[static_cast<std::size_t>(effector)] == effector && solver.isAncestorOrSelf(root, effector)) {
        dofBone[static_cast<std::size_t>(effector)] = 1;
    }
    // (... and THE GROUP's secondary wrists the same: with the first hand's wrist free and the
    // second's not, both hands pushed forward together ended with their thumbs 19mm apart on the
    // newest generation — the one freedom the two hands did not share.)
    for (const int sec : s.secondaries) {
        if (!kNoWristDof && sec >= 0 && sec != root && m_jsWristOf.size() == n &&
            m_jsWristOf[static_cast<std::size_t>(sec)] == sec && solver.isAncestorOrSelf(root, sec)) {
            dofBone[static_cast<std::size_t>(sec)] = 1;
        }
    }
    // The foot contact model's balls (see kHeelHoldWeight): found once per pin per drag.
    static const bool kNoBall = std::getenv("IK_JS_NO_BALL") != nullptr; // A/B: rigid ankle pins
    if (m_jsBall.size() != n) {
        m_jsBall.assign(n, -2);
        m_jsToeMates.assign(n, {});
        m_jsBallOffset.assign(n, glm::vec3(0.0f));
    }
    if (m_jsBallTarget.size() != n) {
        m_jsBallTarget.assign(n, glm::vec3(0.0f));
        m_jsBallTargetValid.assign(n, 0);
    }
    if (m_jsHeelOffset.size() != n) {
        m_jsHeelOffset.assign(n, glm::vec3(0.0f));
    }
    s.floorModel = -m_transform[3][1]; // the floor's height in model space
    const float& floorModel = s.floorModel;
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const int node = pins[p].node;
        if (node < 0 || static_cast<std::size_t>(node) >= n || m_jsBall[static_cast<std::size_t>(node)] != -2) {
            continue;
        }
        int ball = -1;
        if (!kNoBall && !rig.pinIsUser(p) && !rig.pinIsLive(p) && node != root) {
            // BIND heights are heights over the bind's own floor (y = 0) — the model-space
            // floor has no part in them. (It was subtracted here, and a figure the Ground
            // button had raised 1.5cm or more lost its balls: rigid ankle pins, silently.)
            const float pinHeight = m_ikBindPos[static_cast<std::size_t>(node)].y;
            // Level by level under the ankle; of the first level's candidates, the one nearest
            // their middle. (Most rigs have ONE — the bone all the toes hang from. One
            // generation hangs every toe straight off the mid-foot, so each toe's base
            // qualifies, and first-found was the LITTLE toe: a foot pivoting on its outer edge.)
            // ... and THE JOINT THE TOES FAN OUT FROM, not a mid-foot joint above it (2026-09-23;
            // IK_JS_BALL_FIRST_LEVEL): the third generation's metatarsal bone sits low enough
            // to qualify a level above its toe joint, has ONE child and 12 degrees of range, and
            // taken for the ball it left the real toe joint (65 degrees of dorsiflexion) never an
            // unknown, never held: in a kneel the foot stood vertical on toes pointing straight
            // down, 26mm of skin through the floor (the base rig's toes bend back 24 degrees).
            // A level whose candidates all have a single child is remembered and the walk goes
            // on; the first level with a candidate the toes fan out from (two children or more)
            // is the ball's, and only with none does the remembered level stand.
            static const bool kBallFirstLevel = std::getenv("IK_JS_BALL_FIRST_LEVEL") != nullptr; // A/B probe
            const auto nearestMiddle = [&](const std::vector<int>& found) {
                glm::vec3 middle(0.0f);
                for (const int f : found) {
                    middle += m_ikBindPos[static_cast<std::size_t>(f)];
                }
                middle /= static_cast<float>(found.size());
                float best = 1.0e9f;
                int   pick = -1;
                for (const int f : found) {
                    const float d = glm::length(m_ikBindPos[static_cast<std::size_t>(f)] - middle);
                    if (d < best) {
                        best = d;
                        pick = f;
                    }
                }
                return pick;
            };
            int fallback = -1;
            std::vector<int> fallbackMates;
            std::vector<int> level{node};
            while (!level.empty() && ball < 0) {
                std::vector<int> next;
                std::vector<int> found;
                std::vector<int> fans;
                for (const int parentBone : level) {
                    for (const int c : m_children[static_cast<std::size_t>(parentBone)]) {
                        const float h = m_ikBindPos[static_cast<std::size_t>(c)].y;
                        if (h < kBallHeightFraction * pinHeight && !m_children[static_cast<std::size_t>(c)].empty()) {
                            found.push_back(c);
                            if (m_children[static_cast<std::size_t>(c)].size() >= 2) {
                                fans.push_back(c);
                            }
                        }
                        next.push_back(c);
                    }
                }
                if (!found.empty()) {
                    if (kBallFirstLevel || !fans.empty()) {
                        ball = nearestMiddle(kBallFirstLevel ? found : fans);
                    } else if (fallback < 0) {
                        fallback = nearestMiddle(found);
                        // THE BALL'S TOE MATES (2026-09-23; IK_JS_NO_TOE_MATES): on the generation
                        // that hangs every toe straight off the mid-foot, the ball is ONE toe's base
                        // among five — held flat and an unknown — and the other four rode the
                        // mid-foot rigidly: in a kneel they pointed straight down with the foot,
                        // 26mm of skin through the floor. Its siblings at that level are its mates:
                        // unknowns with the toes' flat hold, so every toe bends back as the foot
                        // pitches over them.
                        static const bool kNoToeMates = std::getenv("IK_JS_NO_TOE_MATES") != nullptr; // A/B probe
                        if (!kNoToeMates && found.size() >= 2) {
                            fallbackMates = found;
                        }
                    }
                }
                level = std::move(next);
            }
            if (ball < 0) {
                ball = fallback;
                for (const int mate : fallbackMates) {
                    if (mate != ball) {
                        m_jsToeMates[static_cast<std::size_t>(node)].push_back(mate);
                    }
                }
            }
        }
        m_jsBall[static_cast<std::size_t>(node)] = ball;
        static const bool kBallTrace = std::getenv("IK_JS_BALL_TRACE") != nullptr; // which joint each pin's ball is
        if (kBallTrace) {
            std::fprintf(stderr, "[js-ball] pin %s (bind y %.4f): ball %s", m_boneNames[static_cast<std::size_t>(node)].c_str(), m_ikBindPos[static_cast<std::size_t>(node)].y, ball >= 0 ? m_boneNames[static_cast<std::size_t>(ball)].c_str() : "none");
            std::fputc(10, stderr);
        }
        if (ball >= 0) {
            // Relative to the pin's PLANTED target, the ball at its OWN planted height: where
            // it is, or — under a ground-healed pin (one the rig planted at its bind height) —
            // its own floor height when it is near it. The ankle and the ball heal
            // separately: a flat hovering foot comes down whole, and a foot whose HEEL is
            // lifted (a deep crouch or a kneel let go of, then dragged again) keeps its ball
            // on the floor while only the soft heel row asks the heel down. Carried rigidly
            // below the ankle's pin, the ball was driven under the floor by the heel's lift.
            const glm::vec3 ballNow(m_poseGlobal[static_cast<std::size_t>(ball)][3]);
            const glm::vec3 pinNow(m_poseGlobal[static_cast<std::size_t>(node)][3]);
            const float scale = rig.sizeScale();
            float ballY = ballNow.y + (pins[p].target.y - pinNow.y);
            const float pinFloorY = m_ikBindPos[static_cast<std::size_t>(node)].y + floorModel;
            const float ballFloorY = m_ikBindPos[static_cast<std::size_t>(ball)].y + floorModel;
            if (std::abs(pins[p].target.y - pinFloorY) < 0.001f * scale &&
                std::abs(ballNow.y - ballFloorY) < 0.12f * scale) {
                ballY = std::abs(ballNow.y - ballFloorY) < 0.001f * scale ? ballNow.y : ballFloorY;
            }
            m_jsBallOffset[static_cast<std::size_t>(node)] =
                glm::vec3(ballNow.x - pinNow.x, ballY - pins[p].target.y, ballNow.z - pinNow.z);
            m_jsHeelOffset[static_cast<std::size_t>(node)] = pinNow - ballNow;
        }
    }
    for (const IkEffector& pin : pins) {
        const int ball = pin.node >= 0 ? m_jsBall[static_cast<std::size_t>(pin.node)] : -1;
        for (int cur = ball; cur >= 0 && cur != pin.node; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            dofBone[static_cast<std::size_t>(cur)] = 1; // the toes bend as the foot pitches
        }
        if (pin.node >= 0 && static_cast<std::size_t>(pin.node) < m_jsToeMates.size()) {
            for (const int mate : m_jsToeMates[static_cast<std::size_t>(pin.node)]) {
                dofBone[static_cast<std::size_t>(mate)] = 1; // (the ball's toe mates bend with it: see the ball finder)
            }
        }
    }
    // The ROOT's rotation: an unknown under a PELVIS drag only (a sit rolls the pelvis back, a
    // deep fold pitches it), priced like a stiff joint; every other drag leaves it alone, as
    // the position-space solve always did — nothing anatomical limits a root, and a limb
    // drag that could turn the whole body would.
    //
    // ... and while the body RISES out of a low pose (m_jsRootRise), whatever is dragged: a kneel
    // leaves the pelvis pitched some 35 degrees forward, a sit 25 back, and every channel's home
    // is the pose the drag began in — so a figure pulled back up by the chest stood up with her
    // pelvis still pitched, her thighs flexed 48 degrees against it, her knees bent and her
    // heels 4cm off the floor (the hip cannot reach its standing height over a pitched pelvis on
    // flat feet). Rising, the root's rotation reference turns from where it began to UPRIGHT
    // (its heading kept: m_jsRootUprightEuler), and for a drag that is not the pelvis's own its
    // price comes down from frozen to the pelvis drag's as the rise completes — a channel
    // that simply appeared at that price would pop.
    static const bool kNoUpright = std::getenv("IK_JS_NO_UPRIGHT") != nullptr; // A/B probe
    s.rising = kNoUpright ? 0.0f : m_jsRootRise;
    const float& rising = s.rising;
    // ... and how FAR ALONG the rise is (m_jsRiseProgress): what turns the references home.
    s.risen = kNoUpright ? 0.0f : m_jsRiseProgress;
    const float& risen = s.risen;
    s.rootTurns = (effector == root || rising > 1.0e-3f) && kRootRot > 0.0;
    const bool& rootTurns = s.rootTurns;
    dofBone[static_cast<std::size_t>(root)] = rootTurns ? 1 : 0;
    s.rootDofMark = static_cast<std::size_t>(root);
    const std::size_t& rootDofMark = s.rootDofMark;
    // A SCOPED drag (IkScope::Chain — Ctrl+drag, 2026-09-26): the unknowns are the grabbed CHAIN
    // alone — the bones from the effector up to, not including, its limb junction (IkRig's
    // mass-based one, so the chain IS what the plain drag classes as the dragged limb: an arm up
    // to the chest with its collar, a leg to the pelvis bone, the head and neck to the chest, a
    // spine joint's chain down to the root, a collar by itself) plus what hangs BELOW the effector
    // (a planted foot under a dragged knee, a dragged wrist's own channels) — and nothing else
    // moves: no root translation or rotation (ikPostureModel skips the root's dofs), no other limb,
    // no spine under an arm. The rows on everything else — the planted feet's pins — stay and are
    // constant; the limits, the floor and the body volumes hold as ever. A pelvis grab keeps every
    // active bone (the legs; a planted hand's arm) and the root's six degrees: the pelvis moves and
    // what is planted stays planted.
    if (m_ikScope == IkScope::Chain && effector != root) {
        const int junction = rig.limbJunction(effector);
        std::vector<char> allowed(n, 0);
        for (int cur = effector; cur >= 0 && cur != junction && cur != root; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            allowed[static_cast<std::size_t>(cur)] = 1;
        }
        for (std::size_t b = 0; b < n; ++b) {
            if (dofBone[b] && !allowed[b] && solver.isAncestorOrSelf(effector, static_cast<int>(b))) {
                allowed[b] = 1; // (what hangs below the effector: a planted foot under a dragged knee)
            }
        }
        for (std::size_t b = 0; b < n; ++b) {
            if (!allowed[b]) {
                dofBone[b] = 0;
            }
        }
        s.rootTurns = false;
        dofBone[static_cast<std::size_t>(root)] = 0;
    }
}

/// The TRUNK's policy for this tick: the trunk follow and the unfold, the hip sway, the hip hinge's balance, and whether the spine chain is among the unknowns.
void Armature::ikTrunkPolicy(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    const glm::vec3* const dragTarget = s.dragTarget;
    std::vector<char>& dofBone = s.dofBone;
    const float& floorModel = s.floorModel;
    const float& rising = s.rising;
    const bool& rootTurns = s.rootTurns;
    const std::size_t& rootDofMark = s.rootDofMark;

    // A SCOPED drag (IkScope::Chain) has no trunk policy: the chain alone answers the cursor — no
    // follow, no hinge, no sway, no reach fold, no unfold — and the root is none of its unknowns
    // (ikChooseUnknowns). Every member the policy would set is left off.
    if (m_ikScope == IkScope::Chain) {
        m_jsTrunkFollow = glm::vec2(0.0f);
        m_jsTrunkCounter = 0.0f;
        m_jsTrunkHinge = 0.0f;
        m_jsUnfoldShift = glm::vec2(0.0f);
        m_jsReachHinge = 0.0f;
        m_jsTrunkWalk = 0.0f;
        m_jsTrunkInSolve = false;
        return;
    }

    // THE TRUNK FOLLOW (see kTrunkLeanFree): an upper-body trunk joint — above the root, no foot
    // below it — dragged across the floor by a figure that began standing. A function of the
    // target; kept through the release settle (which has no target to read it from).
    static const bool kNoTrunkFollow = std::getenv("IK_JS_NO_TRUNK_FOLLOW") != nullptr; // A/B probe
    if (dragTarget != nullptr) {
        m_jsTrunkFollow = glm::vec2(0.0f);
        m_jsTrunkCounter = 0.0f;
        const float lever = m_jsStartGrabFromRoot.y;
        // (No foot below it rules out a knee's and a foot's drag; an ELBOW's by its fold — one
        // generation's forearm carries mass enough to read as trunk-class, and its raised elbow
        // took the hips 2cm along.)
        // ("Above the root" is read off the pose the drag found — but a figure LYING on the floor has
        // her chest level with her hips: there it is the rig's own statement, the bind's. Without
        // it a figure laid down on her back could not be sat up again: no hinge, no roll, the
        // chest brought up by a spine curled to its limits over a pelvis that stayed on its back.)
        // (... and ON HER KNEES the same: on all fours the chest lies level with the hips — 5cm
        // above them on the reference rigs, level or below on nine custom bodies of the sweep, whose
        // chest lifted off all fours then had no hinge: the spine arched to its limits over a
        // pelvis still pitched 46 degrees, 89mm short.)
        const float above = (m_jsFloorSeatStart || m_jsKneelStart) ? m_ikBindPos[static_cast<std::size_t>(effector)].y -
                                                                         m_ikBindPos[static_cast<std::size_t>(root)].y
                                                                   : lever;
        bool upperBody = effector != root && rig.effectorIsTrunk() && above > 0.05f * rig.sizeScale();
        if (upperBody) {
            const Bone& bone = m_bones[static_cast<std::size_t>(effector)];
            for (int a = 0; a < 3 && upperBody; ++a) {
                const float range = bone.rotMax[a] - bone.rotMin[a];
                if (bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f) {
                    upperBody = std::min(-bone.rotMin[a], bone.rotMax[a]) >
                                kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a]);
                }
            }
        }
        if (upperBody) {
            std::vector<int> stack{effector};
            while (!stack.empty() && upperBody) {
                const int node = stack.back();
                stack.pop_back();
                upperBody = m_ikBindPos[static_cast<std::size_t>(node)].y >= 0.20f * rig.sizeScale();
                for (const int c : m_children[static_cast<std::size_t>(node)]) {
                    stack.push_back(c);
                }
            }
        }
        // THE BOW (kTrunkHingeStiffness): under such a drag the pelvis may PITCH.
        static const bool kNoTrunkHinge = std::getenv("IK_JS_NO_TRUNK_HINGE") != nullptr; // A/B probe
        // (... and under ANY such drag of a SEATED figure, however low she sits: a pinned pelvis
        // keeps its place, and rocks on it.)
        // (... and of a figure SITTING ON THE FLOOR — IkRig::floorSeat: her pelvis rests there and
        // rocks there. Without it a sit leaned back by the chest bent the spine to its limits over a
        // pelvis that stayed as it sat, "her hip pointed up" — she could not be laid down.)
        static const bool kNoFloorRock = std::getenv("IK_NO_FLOOR_SEAT_ROCK") != nullptr; // A/B probe
        // (... and of a figure ON HER KNEES — IkRig::onKnees at the press: planted knees are as good
        // a base to fold over as standing feet. Without it a kneeling figure's chest taken forward
        // and down was the spine's alone to follow: curled to its 35-degree limit over a pelvis that
        // never pitched, 26-30cm short of the cursor with her hands 19cm over the floor — a hunch,
        // and no way onto all fours.)
        static const bool kNoKneelFold = std::getenv("IK_JS_NO_KNEEL_FOLD") != nullptr; // A/B probe
        m_jsTrunkHinge = (!kNoTrunkHinge && upperBody && !rig.suspended() && !m_jsWasSuspended)
                             ? (rig.seatPin() >= 0 || (!kNoFloorRock && m_jsFloorSeatStart) || (!kNoKneelFold && m_jsKneelStart)
                                    ? 1.0f
                                    : 1.0f - glm::smoothstep(kRootYieldRoomFrom * rig.sizeScale(),
                                                             kRootYieldRoomFull * rig.sizeScale(), m_jsRootRiseRoom))
                             : 0.0f;
        if (!kNoTrunkFollow && upperBody && !rig.suspended() && !m_jsWasSuspended) {
            // (The travel is the grab's over the STANCE as the rig has walked it: once the feet
            // have stepped under the body the chest stands over its hips again, the lean and the
            // follow measured afresh from there.)
            const glm::vec3 travel = *dragTarget - m_jsStartTarget;
            glm::vec2 across(travel.x - rig.stanceShift().x, travel.z - rig.stanceShift().z);
            // THE BOW UNDONE (see the bow's arc below): a trunk that began folded forward and is
            // taken back UP comes back over its hips as it rises — that much of its travel toward
            // them is the fold opening, not a body taken backward across the floor. Read as one,
            // a bow let go of and the chest then taken back up walked the hips' home 12cm back
            // under her: she stood up 25mm short with her pelvis still pitched 10 degrees.
            static const bool kNoBowArc = std::getenv("IK_JS_NO_BOW_ARC") != nullptr; // A/B probe
            if (!kNoBowArc && travel.y > 0.0f) {
                const glm::vec2 lean0(m_jsStartGrabFromRoot.x, m_jsStartGrabFromRoot.z);
                const float began = glm::length(lean0);
                if (began > 1.0e-4f) {
                    const float reach = glm::length(m_jsStartGrabFromRoot);
                    const float tall = glm::clamp(lever + travel.y, 0.0f, reach);
                    const float opened = began - std::sqrt(std::max(0.0f, reach * reach - tall * tall));
                    const float toward = -glm::dot(across, lean0) / began;
                    across += lean0 * (glm::clamp(toward, 0.0f, std::max(0.0f, opened)) / began);
                }
            }
            const float went = glm::length(across);
            float free = kTrunkLeanFree * lever;
            float width = kTrunkLeanWidth * lever;
            float standRoom = 0.0f;
            float leanRoom = 1.0e9f; // (the balance room the way the drag goes; unbounded until measured)
            if (went > 0.5f * free && static_cast<std::size_t>(root) < m_jsStartPos.size()) {
                // The lean balance has room for (kTrunkLeanBalanced), the way the drag goes.
                if (m_jsLeanMoment < 0.0f) {
                    std::vector<int> parents(n);
                    std::vector<char> upper(n, 0), hasFoot(n, 0);
                    for (std::size_t i = n; i-- > 0;) { // children follow parents
                        hasFoot[i] = hasFoot[i] || m_ikBindPos[i].y < 0.20f * rig.sizeScale();
                        const int par = m_bones[i].parent;
                        if (par >= 0) {
                            hasFoot[static_cast<std::size_t>(par)] = hasFoot[static_cast<std::size_t>(par)] || hasFoot[i];
                        }
                    }
                    const std::vector<float>& masses = rig.masses();
                    float total = 0.0f, moment = 0.0f;
                    for (std::size_t i = 0; i < n; ++i) {
                        parents[i] = m_bones[i].parent;
                        const int par = m_bones[i].parent;
                        upper[i] = par >= 0 && ((par == root && !hasFoot[i]) || upper[static_cast<std::size_t>(par)]);
                        const float mass = i < masses.size() ? masses[i] : 0.0f;
                        total += mass;
                        if (upper[i]) {
                            moment += mass * (m_jsStartPos[i].y - m_jsStartPos[static_cast<std::size_t>(root)].y);
                        }
                    }
                    m_jsLeanMoment = total > 1.0e-6f ? std::max(moment / total, 1.0e-3f) : 1.0e-3f;
                    const glm::vec3 com = BalanceController::centerOfMass(m_jsStartPos, parents, masses);
                    m_jsStartCom = glm::vec2(com.x, com.z);
                    m_jsStartStanceNodes.clear();
                    for (std::size_t p = 0; p < rig.pins().size(); ++p) {
                        if (rig.pinIsLive(p)) {
                            m_jsStartStanceNodes.push_back(rig.pins()[p].node);
                        }
                    }
                }
                const glm::vec2 way = across / went;
                // (The STANCE the lean is measured against is the feet's — as the rig has stepped
                // them — and the live contacts the drag FOUND (a kneel's knees), never a contact the
                // drag itself makes. A hand that lands is a support the balance row reads at once;
                // the ROOM the lean has is the drag's own measure, and read off every contact it
                // grew by 23cm in the tick the hands touched down: the counter below fell from 1
                // to 0, the pelvis's price snapped from 250 back to 4500 and hauled the hips 11cm
                // forward to the reference the hinge had left — the chest, on its arms over the
                // planted hands, ran 6cm ahead of its cursor and could not be brought back. And
                // the feet's as they STAND, not the drag-start polygon walked by the steps — tried
                // first: a walking bow's split stance has more room fore and aft than the stance
                // it began with, and read short the counter stayed on after the step, the settle
                // moving the head 27mm where it moves 9.)
                static const bool kLiveStance = std::getenv("IK_JS_COUNTER_LIVE_STANCE") != nullptr; // A/B probe
                std::vector<glm::vec2> stancePoints;
                for (std::size_t p = 0; p < rig.pins().size(); ++p) {
                    const bool found = std::find(m_jsStartStanceNodes.begin(), m_jsStartStanceNodes.end(), rig.pins()[p].node) !=
                                       m_jsStartStanceNodes.end();
                    if (kLiveStance || !rig.pinIsLive(p) || found) {
                        const std::vector<glm::vec2> own = rig.pinFootprintPoints(p);
                        stancePoints.insert(stancePoints.end(), own.begin(), own.end());
                    }
                }
                const std::vector<glm::vec2> stance = BalanceController::supportPolygon(std::move(stancePoints));
                const glm::vec2 com = m_jsStartCom + glm::vec2(rig.stanceShift().x, rig.stanceShift().z);
                // (Room is what the drag may take before the body is WORSE balanced than it began:
                // two generations stand with their feet together and their weight a little outside
                // the inset support as posed — the imbalance a drag starts with is the user's, here
                // as in the solve's own balance row.)
                const auto needAt = [&](float t) {
                    const glm::vec2 at = com + way * t;
                    return glm::length(BalanceController::closestBalancedPoint(stance, at, rig.balanceMargin()) - at);
                };
                const float began = stance.empty() ? 0.0f : needAt(0.0f);
                const auto balancedAt = [&](float t) { return needAt(t) <= began + 0.001f * rig.sizeScale(); };
                float room = 0.0f;
                if (!stance.empty()) {
                    float outside = 0.6f * rig.sizeScale();
                    for (int it = 0; it < 14; ++it) {
                        const float mid = 0.5f * (room + outside);
                        (balancedAt(mid) ? room : outside) = mid;
                    }
                }
                const float leanCap = std::min(free + width, kTrunkLeanBalanced * room * lever / m_jsLeanMoment);
                free = std::min(free, 0.5f * leanCap);
                width = std::max(leanCap - free, 0.01f * rig.sizeScale());
                // What is left of that room once the lean has had its share is all the hips may
                // take of a stance that CANNOT walk after them (below).
                standRoom = std::max(0.0f, kTrunkLeanBalanced * room - leanCap * m_jsLeanMoment / lever);
                leanRoom = room;
            }
            // THE BOW'S ARC: a trunk taken DOWN as it goes forward is folding, not walking — what a
            // trunk of this length reaches forward of its hips at the target's height, over what
            // it reached at the height it began, is the lean's for free.
            float leanArc = 0.0f;
            if (!kNoBowArc && went > 1.0e-4f && travel.y < 0.0f) {
                const float reach = glm::length(m_jsStartGrabFromRoot);
                const float tall = glm::clamp(lever + travel.y, 0.0f, reach);
                const float arcStart = std::sqrt(std::max(0.0f, reach * reach - std::min(lever, reach) * std::min(lever, reach)));
                const float arcNow = std::sqrt(std::max(0.0f, reach * reach - tall * tall));
                const glm::vec2 fore(std::sin(m_jsRootHeading), std::cos(m_jsRootHeading));
                // (A FOLD is sagittal: taken down and 37 degrees off to one side the chest had to be
                // reached by side bend and twist instead of a walk, stalled six ticks against the
                // spine's limits and caught up in one 18cm move.)
                const float forward = glm::smoothstep(kBowArcSagittalFrom, kBowArcSagittalFull, glm::dot(across / went, fore));
                static const float kArcShare = static_cast<float>(envOr("IK_JS_BOWARC", kBowArcShare));
                leanArc = kArcShare * std::max(0.0f, arcNow - arcStart) * forward;
                // (... less what the HIPS give back: a fold takes the weight forward, the counter
                // below lets the hips go back under it, and the chest's reach over the stance is
                // short by as much. What balance room the level lean leaves is spent first.)
                if (leanRoom < 1.0e8f) {
                    const float ratio = m_jsLeanMoment / lever;
                    const float spare = std::max(0.0f, kTrunkLeanBalanced * leanRoom - (free + width) * ratio);
                    if (leanArc * ratio > spare) {
                        leanArc = (leanArc + spare) / (1.0f + ratio);
                    }
                }
                free += leanArc;
            }
            if (went > 1.0e-4f) {
                const float past = std::max(0.0f, went - free);
                float follow = past - width * std::tanh(past / width);
                // (Not a body on its way DOWN — a bow, a fold, a crouch by the chest keeps its hips
                // over its feet — nor one that began low or is getting up: the rise has its own
                // way with the hips.)
                const float downShare = std::max(0.0f, -travel.y) / std::max(glm::length(travel), 1.0e-4f);
                // (A gate of the trunk follow's own since 2026-09-23 (kTrunkFollowDownFrom/Gone 0.3-0.55;
                // it read the pelvis hinge's 0.55-0.80, set for hip drags that fall onto supports):
                // a bow by the chest — down a third of its lever, forward four fifths, a 0.38 share —
                // is a fold, and at the hinge's gate it kept a walk's whole follow, 5.8cm, let go of
                // as slack on the balance row: the weight rested that far past the toes, the effort
                // trigger confirmed, and a step needs a foot kStepMinDistance (6cm) from its
                // anchored spot — the base rig read 5.9 and kept its feet, a top-heavy toon 6.03 and
                // took two steps: a knife edge on every rig. At 4.4cm of follow the toon keeps her
                // feet 1.5cm inside the edge and the chest rests 2.6mm from its cursor; the follow
                // cut outright (0.2-0.5) left it 29-30mm short — that far forward and down needs the
                // weight past the toes or a step, and the design chose the weight.)
                static const float kFollowDownFrom = static_cast<float>(envOr("IK_JS_FOLLOW_DOWN_FROM", static_cast<double>(kTrunkFollowDownFrom)));
                static const float kFollowDownGone = static_cast<float>(envOr("IK_JS_FOLLOW_DOWN_GONE", static_cast<double>(kTrunkFollowDownGone)));
                follow *= 1.0f - glm::smoothstep(kFollowDownFrom, kFollowDownGone, downShare);
                // (Not WHILE she gets up — the rise has its own way with the hips — but a drag that
                // merely BEGAN low is as much a trunk taken across the floor: gated off there, a
                // crouching figure's chest dragged 15cm forward at its own height got no hips,
                // arched to hold the height — lower spine flexed 27 degrees, the chest bone
                // extended to its limit against it — stopped 4cm short and then CORKSCREWED, 47
                // degrees of spine twist for 7mm of reach. A low body takes no step, so there the
                // follow is the bounded one below: the hips go as far as balance has room.)
                static const bool kFollowStandingOnly = std::getenv("IK_JS_FOLLOW_STANDING_ONLY") != nullptr; // A/B probe
                if (kFollowStandingOnly) {
                    follow *= 1.0f - glm::smoothstep(kRootYieldRoomFrom * rig.sizeScale(),
                                                     kRootYieldRoomFull * rig.sizeScale(), m_jsRootRiseRoom);
                }
                follow *= 1.0f - rising;
                // The follow's premise is a stance that WALKS after the hips. One that cannot —
                // a user-pinned foot leaves a single foot free to step, and the last one never
                // does; a body that has gone LOW takes no step — keeps its balance instead: the
                // hips go only as far as the support has room for (a moved reference is a spring
                // without the cursor's bound: with a pinned foot under a 45cm chest drag it
                // hauled the hips 30cm out over both planted feet, a side lunge on a foot twisted
                // 12 degrees).
                // (How low the body was when the drag BEGAN — a constant of the drag. Read off the
                // pose as it stands it fed back: the hinge below raises the hips, a risen body
                // "walks", a walking body's hips are asked forward and down again — and a chest
                // held 25cm out of a crouch threw the figure between the two poses every tick.)
                const float standing = m_ikBindPos[static_cast<std::size_t>(root)].y + floorModel;
                const float low = standing - m_jsStartPos[static_cast<std::size_t>(root)].y;
                m_jsTrunkWalk = rig.steppablePins() >= 2
                                    ? 1.0f - glm::smoothstep(0.06f * rig.sizeScale(), 0.12f * rig.sizeScale(), low)
                                    : 0.0f;
                follow = glm::mix(std::min(follow, standRoom), follow, m_jsTrunkWalk);
                // ... and what such a stance does instead is COUNTER the lean: the hips go BACK as
                // the chest goes forward (a hinge), which the balance row and the cursor ask for
                // between them as soon as the pelvis is not held to its place at 4500/m^2. It
                // yields (kTrunkCounterStiffness) by how far the lean would take the centre of mass
                // past the room it has. Held, a crouching figure's chest dragged 15cm forward
                // leaned into the support's edge, stopped 4-5cm short, and wrung the spine to its
                // twist limits — 47 degrees — to shuffle the arms' weight back for a few
                // millimetres more.
                static const bool kNoBowCounter = std::getenv("IK_JS_NO_BOW_COUNTER") != nullptr; // A/B probe
                const float leaned = kNoBowCounter ? went : went - follow * m_jsTrunkWalk;
                const float over = std::max(0.0f, leaned * m_jsLeanMoment / lever - kTrunkLeanBalanced * leanRoom);
                m_jsTrunkCounter = (kNoBowCounter ? 1.0f - m_jsTrunkWalk : 1.0f) *
                                   glm::smoothstep(0.0f, 0.04f * rig.sizeScale(), over);
                m_jsTrunkFollow = across * (follow / went);
                // THE UNFOLD BRINGS THE HIPS UNDER (2026-09-23; IK_JS_NO_UNFOLD): a trunk that comes
                // MORE UPRIGHT than the drag found it — a bow let go of and the chest taken back
                // up — brings its hips under its target as it does, by the bind's offset of the
                // grabbed joint over the root turned to the figure's heading, by how much of the
                // found tilt the target undoes. The bow's counter-hinge takes the hips BACK under
                // the weight; the next drag begins with them there, its posture reference, and
                // nothing brought them forward again: she stood back up leaning 10 degrees at the
                // hips (5 before the bow's follow was cut), the trunk reaching a chest target the
                // hips stood behind. Standing starts only: a kneeling trunk's target is not over
                // its hips (the kneel-up's seat lift), a seat rocks on its pin.
                static const bool kNoUnfold = std::getenv("IK_JS_NO_UNFOLD") != nullptr; // A/B probe
                m_jsUnfoldShift = glm::vec2(0.0f);
                if (!kNoUnfold && !m_jsKneelStart && !m_jsFloorSeatStart && rig.seatPin() < 0 && m_jsTrunkHinge > 1.0e-3f &&
                    static_cast<std::size_t>(root) < m_jsStartPos.size()) {
                    const float reach = glm::length(m_jsStartGrabFromRoot);
                    const float tiltStart = reach > 1.0e-4f ? std::acos(glm::clamp(m_jsStartGrabFromRoot.y / reach, -1.0f, 1.0f)) : 0.0f;
                    const glm::vec3 fromRoot = *dragTarget - m_jsStartPos[static_cast<std::size_t>(root)];
                    const float len = glm::length(fromRoot);
                    const float tiltTarget = len > 1.0e-4f ? std::acos(glm::clamp(fromRoot.y / len, -1.0f, 1.0f)) : 0.0f;
                    if (tiltStart > glm::radians(kUnfoldFromDeg)) {
                        const float progress = glm::clamp(1.0f - tiltTarget / tiltStart, 0.0f, 1.0f) *
                                               glm::smoothstep(glm::radians(kUnfoldFromDeg), glm::radians(kUnfoldFullDeg), tiltStart);
                        // (the standing offset of the grab over the root, turned to the heading)
                        const glm::vec3 bindOff = m_ikBindPos[static_cast<std::size_t>(effector)] - m_ikBindPos[static_cast<std::size_t>(root)];
                        const glm::vec2 offXZ(std::cos(m_jsRootHeading) * bindOff.x + std::sin(m_jsRootHeading) * bindOff.z,
                                              -std::sin(m_jsRootHeading) * bindOff.x + std::cos(m_jsRootHeading) * bindOff.z);
                        const glm::vec2 under(dragTarget->x - offXZ.x, dragTarget->z - offXZ.y);
                        const glm::vec2 home(m_jsStartPos[static_cast<std::size_t>(root)].x + rig.stanceShift().x + m_jsTrunkFollow.x,
                                             m_jsStartPos[static_cast<std::size_t>(root)].z + rig.stanceShift().z + m_jsTrunkFollow.y);
                        m_jsUnfoldShift = (under - home) * progress;
                    }
                }
                static const bool kFollowTrace = std::getenv("IK_JS_FOLLOW_TRACE") != nullptr;
                if (kFollowTrace) {
                    std::fprintf(stderr, "[follow] went %.4f free %.4f width %.4f follow %.4f walk %.2f standRoom %.4f moment %.4f lever %.3f arc %.4f counter %.2f\n",
                                 went, free, width, follow, m_jsTrunkWalk, standRoom, m_jsLeanMoment, lever, leanArc,
                                 m_jsTrunkCounter);
                    std::fprintf(stderr, "[follow]   unfold(%.4f %.4f)\n", m_jsUnfoldShift.x, m_jsUnfoldShift.y);
                }
            }
        }
        m_jsWasSuspended = m_jsWasSuspended || rig.suspended();
    }
    // REACHING DOWN (2026-09-25; IK_JS_NO_REACH_HINGE): a HAND pushed down from a standing start
    // folds the trunk at the hips as well as bending the knees — the bow's hinge (the root's
    // pitch an unknown at kTrunkHingeStiffness, coupled into the spine chain) by how far and how
    // vertically the hand's target has gone below where the drag began (m_jsReachDown: the
    // push-down's own function of the target), the hips' horizontal price yielding as under the
    // bow's counter so they go BACK as the trunk folds forward, and their vertical yield only
    // kReachSquatShare of a chest push's (ikPostureModel). Until now a hand brought to the knee
    // was a half squat with the trunk bolt upright, and a hand taken to the floor a deep squat,
    // hands between the knees, gaze level: the knee is the cheapest joint in the body, the hips'
    // vertical price yielded to a hand's push exactly as to a chest's, and the pelvis could not
    // pitch at all, so nothing ever folded — a person reaching for her knee bows, and one picking
    // something up off the floor bends at the hips AND the knees, looking at it. A standing start
    // only (the bow's room gate): a seated figure rocks on its pin under a TRUNK drag, a kneeling
    // or lying one has its own rules.
    static const bool kNoReachHinge = std::getenv("IK_JS_NO_REACH_HINGE") != nullptr; // A/B probe
    if (dragTarget != nullptr) {
        ensureHandMaps();
        const std::size_t e = static_cast<std::size_t>(effector);
        const bool handDrag = effector != root && e < m_jsWristOf.size() && m_jsWristOf[e] == effector;
        const float standingStart = 1.0f - glm::smoothstep(kRootYieldRoomFrom * rig.sizeScale(),
                                                           kRootYieldRoomFull * rig.sizeScale(), m_jsRootRiseRoom);
        m_jsReachHinge = (!kNoReachHinge && handDrag && !rig.suspended() && !m_jsWasSuspended && rig.seatPin() < 0 &&
                          !m_jsKneelStart && !m_jsFloorSeatStart)
                             ? m_jsReachDown * standingStart
                             : 0.0f;
        m_jsTrunkHinge = std::max(m_jsTrunkHinge, m_jsReachHinge);
        // THE SEAT ROCKS UNDER A HAND DRAG TOO (2026-10-28; IK_JS_SEAT_ROCK_TRUNK_ONLY): a pinned
        // pelvis rocked under a TRUNK drag alone, and a seated figure reaching with a hand bent her
        // spine over a pelvis held bolt upright — a hand laid on the knee leaned the trunk 25-30cm
        // at the head through the spine. A person reaching forward or down from a chair hinges at
        // the hips: the trunk drag's hinge (the root's pitch at the bow's price, coupled into the
        // spine's chain; the seat pin's pitch hold zero, ikPinRows), the reference where she sits,
        // so the arm's reach pulls the pelvis over only as far as its bounded pull buys.
        static const bool kSeatRockTrunkOnly = std::getenv("IK_JS_SEAT_ROCK_TRUNK_ONLY") != nullptr; // A/B probe
        if (!kSeatRockTrunkOnly && handDrag && rig.seatPin() >= 0 && !rig.suspended() && !m_jsWasSuspended) {
            m_jsTrunkHinge = 1.0f;
        }
    }
    if (m_jsTrunkHinge > 1.0e-3f) {
        dofBone[rootDofMark] = 1;
    }
    // The HIP SWAY: a pelvis dragged SIDEWAYS over its standing feet rolls — the side it goes
    // toward comes up. Each standing leg says how high its hip socket may sit over its foot
    // (straight: sqrt(length^2 - the socket's horizontal distance from the ankle^2)); a sway
    // takes one socket over its foot (that leg could be longer) and the other away from its
    // (shorter), and the pelvis's roll REFERENCE takes up the difference — the slope of those
    // changes across the pelvis, against the stance as the rig has walked it. The price alone
    // could not: a knee is the cheapest joint in the body (1/range^2), so at any roll price the
    // leg the hips went over BENT, 12 degrees for an 8cm sway, under a level pelvis, and the
    // other stood straight on a lifted heel — contrapposto backwards.
    static const bool kNoSway = std::getenv("IK_JS_NO_SWAY_ROLL") != nullptr; // A/B probe
    // (The sway is the pelvis drag's. Given to a FOLLOWED trunk drag too — the root's rotation
    // brought in at 800/rad^2 for the roll alone — it took a sideways weight shift's far heel
    // from 3cm up to 2, put the grabbed chest 5mm further from its cursor (the spine's side bend
    // spent on undoing the roll) and raised a stepping chest drag's knee tremble from 8mm to 20,
    // the roll's reference jumping with every landing: not worth its keep.)
    s.swayValid = false;
    bool& swayValid = s.swayValid;
    s.swayEuler = glm::vec3(0.0f);
    glm::vec3& swayEuler = s.swayEuler;
    if (!kNoSway && effector == root && rootTurns && dragTarget != nullptr && rising <= 0.0f &&
        static_cast<std::size_t>(root) < m_jsStartPos.size()) {
        const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
        const glm::vec3 fore(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));
        const glm::vec3 rootStart = m_jsStartPos[static_cast<std::size_t>(root)];
        const glm::vec2 stanceAt(rootStart.x + rig.stanceShift().x, rootStart.z + rig.stanceShift().z);
        const glm::vec2 hipsAt(dragTarget->x, dragTarget->z);
        std::vector<glm::vec2> samples; // (the socket's place across the pelvis, its straight height's change)
        float legInHand = 1.0e9f;
        for (std::size_t p = 0; p < pins.size(); ++p) {
            const int node = pins[p].node;
            if (node < 0 || rig.pinIsLive(p) || m_ikBindPos[static_cast<std::size_t>(node)].y >= 0.20f * rig.sizeScale()) {
                continue; // a standing FOOT's leg
            }
            const int junction = rig.limbJunction(node);
            int top = node;
            float length = 0.0f;
            for (;;) {
                const int par = m_bones[static_cast<std::size_t>(top)].parent;
                if (par < 0 || par == junction || par == root) {
                    break;
                }
                length += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(top)].localBind[3]));
                top = par;
            }
            if (top == node || static_cast<std::size_t>(top) >= m_jsStartPos.size()) {
                continue;
            }
            const glm::vec3 socket = m_jsStartPos[static_cast<std::size_t>(top)] - rootStart;
            const auto straight = [&](const glm::vec2& pelvis) {
                const glm::vec2 across(pelvis.x + socket.x - pins[p].target.x, pelvis.y + socket.z - pins[p].target.z);
                return std::sqrt(std::max(0.0f, length * length - glm::dot(across, across)));
            };
            samples.emplace_back(glm::dot(socket, lateral), straight(hipsAt) - straight(stanceAt));
            // (How much LEG this one has in hand where the cursor takes the hips: its straight
            // height there less the height its socket is asked to.)
            const float asked = dragTarget->y + socket.y - pins[p].target.y;
            legInHand = std::min(legInHand, straight(hipsAt) - asked);
        }
        if (samples.size() >= 2) {
            glm::vec2 mean(0.0f);
            for (const glm::vec2& v : samples) {
                mean += v;
            }
            mean /= static_cast<float>(samples.size());
            float sxx = 0.0f;
            float sxy = 0.0f;
            for (const glm::vec2& v : samples) {
                sxx += (v.x - mean.x) * (v.x - mean.x);
                sxy += (v.x - mean.x) * (v.y - mean.y);
            }
            // The roll is there so that no leg has to be LONGER than it is: it answers a leg the
            // sway has run out of, and has no business where every leg is bent with length in
            // hand (kSwayLegInHand). In a split stance — one foot a stride behind — hips taken
            // down and back change the two legs' straight heights by different amounts with no
            // sway at all: a lunge came out with the pelvis rolled 14 degrees, the front foot
            // rolled to its limit and the front knee 32cm across the midline.
            float slope = sxx > 1.0e-6f ? glm::clamp(sxy / sxx, -0.35f, 0.35f) : 0.0f;
            slope *= 1.0f - glm::smoothstep(kSwayLegInHandFrom * rig.sizeScale(), kSwayLegInHandGone * rig.sizeScale(), legInHand);
            if (std::abs(slope) > 1.0e-4f) {
                const Bone& bone = m_bones[static_cast<std::size_t>(root)];
                const glm::mat3 roll(glm::mat4_cast(glm::angleAxis(std::asin(slope), fore)));
                const glm::mat3 parentRot = bone.parent >= 0 ? glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)])
                                                             : glm::mat3(1.0f);
                const glm::mat3 local =
                    glm::mat3(bone.invOrient) * glm::transpose(parentRot) * roll * m_jsEffectorStartRot * glm::mat3(bone.orient);
                swayEuler = eulerFromMatrix(local, bone.rotationOrder);
                for (int a = 0; a < 3; ++a) {
                    while (swayEuler[a] - m_ikStartEuler[static_cast<std::size_t>(root)][a] > 180.0f) {
                        swayEuler[a] -= 360.0f;
                    }
                    while (swayEuler[a] - m_ikStartEuler[static_cast<std::size_t>(root)][a] < -180.0f) {
                        swayEuler[a] += 360.0f;
                    }
                }
                swayValid = true;
            }
        }
    }
    // ... and the spine takes the roll up (kSwayChestLevelWeight): the chain from the root to the
    // CHEST — up through the child with the biggest subtree that has no foot in it, to the first
    // bone three big branches leave (the neck and the two arms) — joins the unknowns. Nothing
    // pulls on it but the chest's level row, so it stays where its references put it otherwise.
    static const bool kNoPelvisBalance = std::getenv("IK_JS_NO_PELVIS_BALANCE") != nullptr; // A/B probe
    s.pelvisBalance = 0.0f;
    float& pelvisBalance = s.pelvisBalance;
    s.hingeSlack = 0.0f; // what the hinge has LET GO of (a walk, hips leaving the stance): the row's slack
    float& hingeSlack = s.hingeSlack;
    // (A drag that began STANDING and has not walked: one that begins low — a kneel, a sit, a
    // crouch let go of — has its supports changing under it as it moves (knees lifting off, the
    // body rising), and one that has taken a step is a walk: between landings the trunk swung
    // back over each new stance.)
    // (By the height the legs had left when the drag began, not a switch at 2cm of it: a hinge
    // let go of leaves the hips a few centimetres low, and the next drag of the same hips is as
    // much a standing one.)
    const float standingStart =
        1.0f - glm::smoothstep(kRootYieldRoomFrom * rig.sizeScale(), kRootYieldRoomFull * rig.sizeScale(),
                               m_jsRootRiseRoom);
    // A drag that WALKS lets the hinge go — over ticks, not in one: the row stays and its SLACK
    // grows kPelvisBalanceReleaseStep a tick from the first step on (a one-sided row under a
    // growing slack gives the lean back at a steady pace; its weight cannot do that, and cut in
    // the tick a step began the trunk snapped upright: hips taken 25cm back threw the head
    // 16cm in one tick). Gone for the drag once it has let go of kPelvisBalanceReleaseGone.
    if (effector == root && dragTarget != nullptr && (rig.stepsTaken() > 0 || rig.steppingPin() >= 0)) {
        m_jsHingeRelease += kPelvisBalanceReleaseStep * rig.sizeScale();
    }
    if (!kNoPelvisBalance && effector == root && dragTarget != nullptr && !rig.suspended() &&
        standingStart > 0.0f && m_jsHingeRelease < kPelvisBalanceReleaseGone * rig.sizeScale()) {
        const float standing = m_ikBindPos[static_cast<std::size_t>(root)].y;
        const float height = dragTarget->y - floorModel;
        // (... and it stands down as the body RISES out of a low pose, as it does for a rising
        // trunk drag: getting up carries the hips a third of a metre over the feet on its own.)
        pelvisBalance = standing > 1.0e-3f
                            ? glm::smoothstep(kPelvisBalanceFrom, kPelvisBalanceFull, height / standing) *
                                  (1.0f - rising) * (1.0f - rising) * standingStart
                            : 0.0f;
        // ... and not on the way DOWN TO THE FLOOR: a drag that is mostly a descent (its downward
        // share of the hips' whole travel: in full under kPelvisBalanceDownFrom, none from
        // kPelvisBalanceDownGone) is a kneel, a sit, a body going onto all fours — deliberately
        // falling onto supports it does not have yet, and balanced over its feet meanwhile it
        // leaned back from the floor it was reaching for: the oldest generations' hands and knees
        // never came down at all.
        const glm::vec3 travel = *dragTarget - m_jsStartTarget;
        const float downShare = std::max(0.0f, -travel.y) / std::max(glm::length(travel), 0.03f * rig.sizeScale());
        pelvisBalance *= 1.0f - glm::smoothstep(kPelvisBalanceDownFrom, kPelvisBalanceDownGone, downShare);
        // (... and only once the hips have gone somewhere ACROSS the floor: the share above means
        // nothing in a drag's first centimetres, where every drag read as "not a descent" — and
        // four ticks of it were enough to shift when one generation's kneeling knees came down,
        // which came out as a 3cm jump in its still hold. A hinge needs the hips 5cm and more off
        // their stance before the centre of mass has anywhere to go.)
        pelvisBalance *= glm::smoothstep(0.02f * rig.sizeScale(), 0.05f * rig.sizeScale(),
                                         glm::length(glm::vec2(travel.x, travel.z)));
        // ... nor once the hips have LEFT THE STANCE (kPelvisBalanceStanceFrom): that is a WALK,
        // and what answers hips that outrun their feet is a step. Balanced through it, the trunk
        // leaned back over the feet it was leaving and swung upright again at every landing: a
        // hip walk's knee tremble went from 24mm to 87. Let go of as SLACK, like the walk's own
        // release above.
        hingeSlack = m_jsHingeRelease;
        if (static_cast<std::size_t>(root) < m_jsStartPos.size()) {
            const glm::vec3 stance = m_jsStartPos[static_cast<std::size_t>(root)] + glm::vec3(rig.stanceShift());
            const float away = glm::length(glm::vec2(dragTarget->x - stance.x, dragTarget->z - stance.z));
            hingeSlack += kPelvisBalanceStanceGain * std::max(0.0f, away - kPelvisBalanceStanceFrom * rig.sizeScale());
        }
    }
    // (Once in, the chain stays in for the drag: taken out again the moment nothing pulled on
    // it — the hips back within centimetres of their stance — its joints froze a few tenths of a
    // degree short of home, and a hinge brought back left the head 3mm from where it began.)
    s.swayChest = -1;
    int& swayChest = s.swayChest;
    // (... and while the body RISES, 2026-10-28: THE TRUNK COMES UP WITH THE RISE — the spine's
    // references turn home with the rise's progress in ikPostureModel, which needs the chain among
    // the unknowns. Standing up by the hips off all fours or out of a sit-up, the root came upright
    // and the spine kept its fold: she stood bent double with her head down. IK_JS_NO_RISE_TRUNK_HOME.)
    static const bool kNoRiseTrunkHome = std::getenv("IK_JS_NO_RISE_TRUNK_HOME") != nullptr; // A/B probe
    // (... and only once NO HAND IS PINNED (m_jsHandsPlanted: a live, non-shadow wrist contact): in
    // the solve while the hands held, the hand rows bent the spine against its references as the
    // hips rose, and it sprang back the tick the pins went — 271mm at a fingertip, 105 at the brow.
    // Entering after the lift-off it enters at its references: the spine rode the pelvis rigidly
    // until then, its channels unchanged.)
    m_jsHandsPlanted = false;
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const int node = pins[p].node;
        if (node >= 0 && static_cast<std::size_t>(node) < n && m_jsWristOf.size() == n && m_jsWristOf[static_cast<std::size_t>(node)] == node &&
            rig.pinIsLive(p) && !rig.pinIsShadow(p)) {
            m_jsHandsPlanted = true;
        }
    }
    m_jsTrunkInSolve = m_jsTrunkInSolve || swayValid || pelvisBalance > 0.0f || (!kNoRiseTrunkHome && rising > 0.0f && !m_jsHandsPlanted);
    if (m_jsTrunkInSolve && effector == root) {
        std::vector<int> size(n, 1);
        std::vector<char> hasFoot(n, 0);
        for (std::size_t i = n; i-- > 0;) { // children follow parents
            hasFoot[i] = hasFoot[i] || m_ikBindPos[i].y < 0.20f * rig.sizeScale();
            const int par = m_bones[i].parent;
            if (par >= 0) {
                size[static_cast<std::size_t>(par)] += size[i];
                hasFoot[static_cast<std::size_t>(par)] = hasFoot[static_cast<std::size_t>(par)] || hasFoot[i];
            }
        }
        int cur = root;
        for (int depth = 0; depth < 8 && swayChest < 0; ++depth) {
            int next = -1;
            for (const int c : m_children[static_cast<std::size_t>(cur)]) {
                if (!hasFoot[static_cast<std::size_t>(c)] &&
                    (next < 0 || size[static_cast<std::size_t>(c)] > size[static_cast<std::size_t>(next)])) {
                    next = c;
                }
            }
            if (next < 0) {
                break;
            }
            dofBone[static_cast<std::size_t>(next)] = 1;
            int branches = 0;
            for (const int c : m_children[static_cast<std::size_t>(next)]) {
                branches += size[static_cast<std::size_t>(c)] >= 4 ? 1 : 0;
            }
            if (branches >= 3) {
                swayChest = next;
            }
            cur = next;
        }
        if (!swayValid) {
            swayChest = -1; // (the chest's level row is the sway's)
        }
    }
}

/// The STIFFNESS CLASSES: which bones are the dragged limb, which serve a pin (and carry the limb twist price), a landed hand's arm, the pinned joints, the pelvis bone, and what goes home as the body rises.
void Armature::ikStiffnessClasses(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const std::vector<IkEffector>& pins = s.pins;

    // Stiffness classes: 0 = trunk, 1 = the dragged limb, 2 = a limb serving a pin.
    s.cls.assign(n, 0);
    std::vector<char>& cls = s.cls;
    // ... and which of those carry the limb's TWIST price (kPinLimbTwistScale): the pinned joint
    // and the segments above it — the thigh and the shin, whose twist would swing a planted
    // foot's knee, and the FOOT, whose roll does the same from below (without its price three
    // rigs' knees knocked 5-6cm inward in a deep crouch and a fourth would not crouch at all) —
    // never what hangs below the pin. The twist axis is read off a bone's longest
    // child, and the ball bone's longest child is a small toe out to its SIDE: its "twist" is the
    // toes' BEND, which carried the x256 — a spring of 42 per rad^2 holding the toes bent as the
    // drag found them. A figure stood up out of a kneel with its heels 4cm off the floor on it.
    ensureHandMaps();
    s.twistPriced.assign(n, 0);
    std::vector<char>& twistPriced = s.twistPriced;
    s.landedArm.assign(n, 0); // the arm of a hand that has come down on the floor
    std::vector<char>& landedArm = s.landedArm;
    s.pinJunction.assign(n, 0); // the bone a pin's limb hangs from (the pelvis bone)
    std::vector<char>& pinJunction = s.pinJunction;
    s.pinNode.assign(n, 0); // the pinned joints themselves
    std::vector<char>& pinNode = s.pinNode;
    s.homeLimb.assign(n, 0); // a STANDING FOOT's limb: what goes home as the body rises
    std::vector<char>& homeLimb = s.homeLimb;
    s.girdle.assign(n, 0); // under an elbow drag: the bones above the arm's socket (the collar)
    std::vector<char>& girdle = s.girdle;
    for (const IkEffector& pin : pins) {
        const int junction = rig.limbJunction(pin.node);
        if (pin.node >= 0 && static_cast<std::size_t>(pin.node) < n) {
            pinNode[static_cast<std::size_t>(pin.node)] = 1;
        }
        if (junction >= 0 && junction != root && !rig.pinIsLive(static_cast<std::size_t>(&pin - pins.data()))) {
            pinJunction[static_cast<std::size_t>(junction)] = 1;
        }
        const int ball = m_jsBall[static_cast<std::size_t>(pin.node)];
        const std::size_t pinIndex = static_cast<std::size_t>(&pin - pins.data());
        const bool standingFoot = !rig.pinIsLive(pinIndex) && !rig.pinIsUser(pinIndex);
        for (int cur = ball >= 0 ? ball : pin.node; cur >= 0 && cur != junction && cur != root;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            cls[static_cast<std::size_t>(cur)] = 2;
            if (standingFoot) {
                homeLimb[static_cast<std::size_t>(cur)] = 1;
            }
        }
        if (pin.node >= 0 && static_cast<std::size_t>(pin.node) < m_jsToeMates.size()) {
            for (const int mate : m_jsToeMates[static_cast<std::size_t>(pin.node)]) { // (the ball's toe mates: see the ball finder)
                cls[static_cast<std::size_t>(mate)] = 2;
                if (standingFoot) {
                    homeLimb[static_cast<std::size_t>(mate)] = 1;
                }
            }
        }
        // (Not the arm of a HAND that has come down on the floor: to lie on its palm, fingers
        // forward, a hand that hung palm-to-thigh must PRONATE, a quarter turn of the forearm -
        // an arm's twist is how a hand gets anywhere. Priced like a planted leg's, the arm could
        // not turn: the tick its hands landed a figure going onto all fours lost her cursor by
        // 5-10cm and her trunk sprang 13 degrees back up, 14cm at the brow. A USER pin on a hand
        // holds its orientation too, and keeps the price.)
        static const bool kHandTwistPriced = std::getenv("IK_JS_HAND_TWIST_PRICED") != nullptr; // A/B probe
        const bool landedHand = !kHandTwistPriced && rig.pinIsLive(pinIndex) && !pin.hard && !rig.pinIsUser(pinIndex) &&
                                pin.node >= 0 && m_jsWristOf.size() == n && m_jsWristOf[static_cast<std::size_t>(pin.node)] >= 0;
        for (int cur = pin.node; cur >= 0 && cur != junction && cur != root && landedHand; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            landedArm[static_cast<std::size_t>(cur)] = 1; // (its elbow is not the fold retry's: JointSolverDof::foldRetry)
        }
        for (int cur = pin.node; cur >= 0 && cur != junction && cur != root && !landedHand;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            twistPriced[static_cast<std::size_t>(cur)] = 1;
        }
    }
    // THE COLLAR UNDER A HAND DRAG (2026-09-25; kCollarElevationMaxDeg in ikPostureModel): the girdle
    // bone the dragged arm's SOCKET hangs from — the socket is the first joint above the wrist that
    // swings, past the forearm's and the upper arm's twist bones, as the elbow drag finds it. At the
    // limb's price a collar with a 65-degree range was the cheapest joint in the arm, and a hand
    // dragged straight to the nape had it at all three of its limits with the socket on top of the
    // neck; its elevation is capped at what a clavicle gives.
    s.handCollars.clear();
    s.handCollarAxes.clear();
    const int effector = s.effector;
    // (... and THE GROUP's secondary wrists get the same box: a second hand pulled high ran its
    // collar to the rig's 55 degrees as the first did before the box.)
    const auto collarOfWrist = [&](int wrist) {
        int collar = -1;
        if (wrist < 0 || wrist == root || m_jsWristOf.size() != n || m_jsWristOf[static_cast<std::size_t>(wrist)] != wrist) {
            return collar;
        }
        const auto freeChannels = [&](int b) {
            const Bone& bone = m_bones[static_cast<std::size_t>(b)];
            int count = 0;
            for (int a = 0; a < 3; ++a) {
                count += (bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg) ? 0 : 1;
            }
            return count;
        };
        // The ELBOW first — the fold joint up from the wrist (a channel of kFoldRangeDeg and more, all
        // but kFoldNarrowFraction of it to one side) — then the socket: the first bone above the elbow
        // that swings (two free channels), past the upper arm's twist bones. "The first bone above the
        // wrist with two free channels" stopped at the FOREARM on the rigs whose forearm carries its own
        // twist as a second channel (the two oldest generations, the newest), took the shoulder for the
        // collar, and capped the arm's ABDUCTION at 30 degrees — found by the trace, not by a gate.
        const auto isFold = [&](int b) {
            const Bone& bone = m_bones[static_cast<std::size_t>(b)];
            for (int a = 0; a < 3; ++a) {
                const float range = bone.rotMax[a] - bone.rotMin[a];
                if (bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f &&
                    std::min(-bone.rotMin[a], bone.rotMax[a]) <= kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a])) {
                    return true;
                }
            }
            return false;
        };
        int elbow = m_bones[static_cast<std::size_t>(wrist)].parent;
        while (elbow >= 0 && elbow != root && !isFold(elbow)) {
            elbow = m_bones[static_cast<std::size_t>(elbow)].parent;
        }
        int socket = elbow >= 0 && elbow != root ? m_bones[static_cast<std::size_t>(elbow)].parent : -1;
        while (socket >= 0 && socket != root && freeChannels(socket) < 2) {
            socket = m_bones[static_cast<std::size_t>(socket)].parent;
        }
        if (socket >= 0 && socket != root) {
            const int junction = rig.limbJunction(wrist);
            const int above = m_bones[static_cast<std::size_t>(socket)].parent;
            if (above >= 0 && above != junction && above != root) {
                collar = above;
            }
        }
        return collar;
    };
    // ... and the collar's ELEVATION channel (the one ikPostureModel caps): the unlocked channel whose
    // axis lies most along the FORE axis — world z at bind, every bind being translation-only.
    const auto elevationAxisOf = [&](int collar) {
        int axis = -1;
        if (collar < 0) {
            return axis;
        }
        const Bone& bone = m_bones[static_cast<std::size_t>(collar)];
        float best = 0.0f;
        for (int a = 0; a < 3; ++a) {
            if (bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg) {
                continue;
            }
            const float along = std::abs(glm::vec3(bone.orient[a]).z);
            if (along > best) {
                best = along;
                axis = a;
            }
        }
        return best < 0.5f ? -1 : axis;
    };
    {
        std::vector<int> wrists{effector};
        wrists.insert(wrists.end(), s.secondaries.begin(), s.secondaries.end());
        for (const int wrist : wrists) {
            const int collar = collarOfWrist(wrist);
            if (collar >= 0 && std::find(s.handCollars.begin(), s.handCollars.end(), collar) == s.handCollars.end()) {
                s.handCollars.push_back(collar);
                s.handCollarAxes.push_back(elevationAxisOf(collar));
            }
        }
    }
    static const bool kCollarTrace = std::getenv("IK_JS_COLLAR_TRACE") != nullptr;
    if (kCollarTrace) {
        for (std::size_t k = 0; k < s.handCollars.size(); ++k) {
            const Bone& bone = m_bones[static_cast<std::size_t>(s.handCollars[k])];
            std::fprintf(stderr,
                         "[collar] %s axis %d | channel axes at bind: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) | limits x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f\n",
                         m_boneNames[static_cast<std::size_t>(s.handCollars[k])].c_str(), s.handCollarAxes[k],
                         bone.orient[0].x, bone.orient[0].y, bone.orient[0].z, bone.orient[1].x, bone.orient[1].y, bone.orient[1].z,
                         bone.orient[2].x, bone.orient[2].y, bone.orient[2].z, bone.rotMin[0], bone.rotMax[0], bone.rotMin[1], bone.rotMax[1],
                         bone.rotMin[2], bone.rotMax[2]);
        }
        if (s.handCollars.empty()) {
            std::fprintf(stderr, "[collar] none (effector %d, junction %d)\n", effector, effector >= 0 ? rig.limbJunction(effector) : -1);
        }
    }
}

/// A FOOT drag: the SLIDE (how much a dragged foot is on the floor), which is what reshapes the stance.
void Armature::ikFootDrag(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const int root = s.root;
    const int effector = s.effector;
    const glm::vec3* const dragTarget = s.dragTarget;
    const glm::vec3* const holdTarget = s.holdTarget;
    std::vector<char>& dofBone = s.dofBone;
    const float& floorModel = s.floorModel;
    std::vector<char>& cls = s.cls;

    // A FOOT drag (the grabbed joint is foot-class: low at BIND — an ankle, or a toe promoted to
    // one). The leg's segments carry the limb twist price like a planted leg's: an ARM's twist is
    // how a hand gets anywhere, but a leg placed by its foot has no business turning — free, the
    // solve spent 57 degrees of thigh twist on a 30cm step forward to tidy millimetres of sideways
    // error (the knee and the foot swivelled outward with it), 66 on a foot taken across the
    // other leg, 75 on crossed legs.
    s.legScale = rig.sizeScale();
    const float& legScale = s.legScale;
    s.legDrag = effector != root && m_ikBindPos[static_cast<std::size_t>(effector)].y < 0.20f * legScale;
    bool& legDrag = s.legDrag;
    static const bool kNoLegDrag = std::getenv("IK_JS_NO_LEG_DRAG") != nullptr; // A/B probe
    legDrag = legDrag && !kNoLegDrag;
    // The SLIDE: how much a dragged FOOT is on the floor — 1 with the target at the ankle's
    // standing height, 0 from 12cm up; a function of the target alone, so the pose stays one too.
    // A foot in the air is placed by its leg and nothing else moves (the hips stay, the foot rides
    // its shin, balance never asks back what the drag began with — lifting a foot has always been
    // that, and stays that, bit for bit). A foot ON THE FLOOR is a stance being reshaped: the
    // hips come between the feet and down, the sole stays on the floor, and the foot bears weight.
    s.goalPoint = dragTarget != nullptr ? dragTarget : holdTarget;
    const glm::vec3*& goalPoint = s.goalPoint;
    const bool footDrag = legDrag && goalPoint != nullptr;
    s.footFloor = m_ikBindPos[static_cast<std::size_t>(effector)].y + floorModel;
    const float& footFloor = s.footFloor;
    static const bool kNoSlide = std::getenv("IK_JS_NO_SLIDE") != nullptr; // A/B probe
    s.slide = (footDrag && !kNoSlide)
                  ? 1.0f - glm::smoothstep(0.03f * legScale, 0.12f * legScale, goalPoint->y - footFloor)
                  : 0.0f;
    const float& slide = s.slide;
    if (footDrag && !kNoSlide) {
        dofBone[static_cast<std::size_t>(effector)] = 1; // the ankle: what keeps the sole down
        cls[static_cast<std::size_t>(effector)] = 1;
    }
    if (slide <= 0.0f) {
        m_jsSoleEaseValid = false; // (a foot lifted clear of the slide's band: its sole hold begins afresh when it comes down)
    }
}

/// A KNEE drag: the knee's goal over its planted foot, or the foot letting go as the knee lifts (`plant`), the lateral reach clamp and the goal's raise.
void Armature::ikKneeDrag(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    std::vector<char>& dofBone = s.dofBone;
    std::vector<char>& cls = s.cls;
    std::vector<char>& twistPriced = s.twistPriced;
    const float& legScale = s.legScale;
    bool& legDrag = s.legDrag;
    const glm::vec3*& goalPoint = s.goalPoint;

    // A KNEE drag: the grabbed joint is a FOLD joint (a hinge: see foldSign) with a foot below
    // it. While its foot is planted (the rig keeps it: IkRig::liftedByDrag) the knee swings over
    // it — out, in, forward, down — and the foot holds; pulled UP past where the drag began the
    // foot lets go (`plant`, a function of the target alone), the leg hangs from the knee, and
    // brought back down it returns to its spot. Hanging, the SHIN keeps its world orientation.
    static const bool kNoKneeDrag = std::getenv("IK_JS_NO_KNEE_DRAG") != nullptr; // A/B probe
    s.kneeDrag = false;
    bool& kneeDrag = s.kneeDrag;
    s.kneeAnkle = -1; // the first foot-class joint below the dragged knee
    int& kneeAnkle = s.kneeAnkle;
    s.effectorFolds = false; // a hinge: a knee, an elbow
    bool& effectorFolds = s.effectorFolds;
    s.effectorFoldAxis = -1;
    int& effectorFoldAxis = s.effectorFoldAxis;
    if (effector != root) {
        const Bone& bone = m_bones[static_cast<std::size_t>(effector)];
        for (int a = 0; a < 3 && !effectorFolds; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            if (bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f) {
                effectorFolds = std::min(-bone.rotMin[a], bone.rotMax[a]) <=
                                kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a]);
                effectorFoldAxis = effectorFolds ? a : -1;
            }
        }
    }
    if (!kNoKneeDrag && effector != root && !legDrag && goalPoint != nullptr) {
        const bool fold = effectorFolds;
        std::vector<int> stack(m_children[static_cast<std::size_t>(effector)].begin(),
                               m_children[static_cast<std::size_t>(effector)].end());
        while (fold && !stack.empty() && !kneeDrag) {
            const int b = stack.back();
            stack.pop_back();
            kneeDrag = m_ikBindPos[static_cast<std::size_t>(b)].y < 0.20f * legScale;
            kneeAnkle = kneeDrag ? b : -1;
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(b)].begin(),
                         m_children[static_cast<std::size_t>(b)].end());
        }
    }
    s.kneeFootPin = -1;
    int& kneeFootPin = s.kneeFootPin;
    for (std::size_t p = 0; kneeDrag && p < pins.size() && kneeFootPin < 0; ++p) {
        // (A USER-pinned foot counts, and never lets go: the knee then swings over a foot that is
        // held in full — place and heading both, so as far as the leg's own joints allow.)
        if (pins[p].node >= 0 && rig.pinUnderEffector(p) && !rig.pinIsLive(p) &&
            m_ikBindPos[static_cast<std::size_t>(pins[p].node)].y < 0.20f * legScale) {
            kneeFootPin = static_cast<int>(p);
        }
    }
    s.kneeLegBone.assign(n, 0); // the dragged knee's leg: its twist price comes back as the foot lets go (below)
    std::vector<char>& kneeLegBone = s.kneeLegBone;
    if (kneeFootPin >= 0) {
        // (Its leg carries no twist price: turning the thigh is what swinging a knee IS — over its
        // PLANTED foot. Once the foot has let go (plant, below) the price comes back: a lifted leg
        // swings its knee at the hip's flexion and abduction, and a twist only turns the shin's
        // plane — free, a knee drawn up out of a kneel to hip height twisted its thigh 63 degrees
        // and stood the knee 10cm out with the foot behind and in, a frog's leg.)
        for (int cur = pins[static_cast<std::size_t>(kneeFootPin)].node; cur >= 0 && cur != root;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            twistPriced[static_cast<std::size_t>(cur)] = 0;
            kneeLegBone[static_cast<std::size_t>(cur)] = 1;
        }
    }
    s.plant = 0.0f; // 1 = the foot below the dragged knee holds; 0 = the leg hangs from it
    float& plant = s.plant;
    s.kneeLift = 0.0f; // how far the knee's target is pulled UP off its planted foot (m)
    float& kneeLift = s.kneeLift;
    s.kneeGoal = glm::vec3(0.0f);
    glm::vec3& kneeGoal = s.kneeGoal;
    if (kneeDrag) {
        kneeGoal = *goalPoint;
        dofBone[static_cast<std::size_t>(effector)] = 1; // the knee's own bend: what lets the shin hang
        if (kneeFootPin >= 0) {
            // The LIFT: the target's rise over where the drag began — and no more of it than it
            // stands ABOVE the planted shin's reach (the top of the ankle's sphere over where the
            // target is). Standing, the knee begins AT that top and the two are one; but a knee
            // that begins on the floor in a kneel, or low in a crouch, has the whole swing of its
            // shin about the planted ankle to come up through first — a lunge, a leg straightened
            // — and must not shed its foot 8cm into it. (The reach alone will not do either: a
            // knee swung 20cm sideways at the cursor's height is 5cm "beyond" it.)
            {
                const glm::vec3 ankle = pins[static_cast<std::size_t>(kneeFootPin)].target;
                float shin = 0.0f;
                for (int cur = pins[static_cast<std::size_t>(kneeFootPin)].node; cur >= 0 && cur != effector;
                     cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                    shin += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(cur)].localBind[3]));
                }
                const glm::vec2 across(goalPoint->x - ankle.x, goalPoint->z - ankle.z);
                const float top = ankle.y + std::sqrt(std::max(0.0f, shin * shin - glm::dot(across, across)));
                kneeLift = std::max(0.0f, std::min(goalPoint->y - m_jsStartTarget.y, goalPoint->y - top));
                static const bool kKneeTrace = std::getenv("IK_JS_KNEE_TRACE") != nullptr;
                if (kKneeTrace) {
                    std::fprintf(stderr, "[knee] goal(%.3f %.3f %.3f) ankle(%.3f %.3f %.3f) top %.3f shin %.3f lift %.4f", goalPoint->x, goalPoint->y, goalPoint->z,
                                 ankle.x, ankle.y, ankle.z, top, shin, kneeLift);
                    std::fputc(10, stderr);
                }
            }
            plant = rig.pinIsUser(static_cast<std::size_t>(kneeFootPin))
                        ? 1.0f
                        : 1.0f - glm::smoothstep(kKneeLiftFrom * legScale, kKneeLiftFull * legScale, kneeLift);
            // Planted, the knee lives within the shin's length of its ankle: a target further
            // out is taken back along the line to the ankle — by the plant — or the cursor's
            // pull, with nowhere else to go, lifted the soft heel (a knee swung 15cm sideways at
            // the cursor's height stood on tiptoe to stay under it).
            const glm::vec3 ankle = pins[static_cast<std::size_t>(kneeFootPin)].target;
            float reach = 0.0f;
            for (int cur = pins[static_cast<std::size_t>(kneeFootPin)].node; cur >= 0 && cur != effector;
                 cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                reach += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(cur)].localBind[3]));
            }
            const glm::vec3 out = kneeGoal - ankle;
            const float far = glm::length(out);
            // (In full until the foot's hold is SOFT — its last quarter, the ball's 1e8 down to
            // 1e2, a tenth of the pelvis's own price — then released: a target already beyond
            // the reach of a foot that still HOLDS puts the cursor's whole pull through the leg
            // into the hips, and she rose 3cm onto the other foot's toes before the foot gave.)
            if (far > reach && far > 1.0e-5f) {
                static const double kProj = envOr("IK_JS_KNEEPROJ", 0.25);
                kneeGoal -= out * ((far - reach) / far * glm::smoothstep(0.0f, static_cast<float>(kProj), plant));
            }
        } else {
            cls[static_cast<std::size_t>(effector)] = 1; // (no pin below: not a pin's limb, and not trunk)
        }
        // A knee that HANGS lives on the thigh's reach about its hip socket, and a cursor drags in
        // a plane: pulled straight up in a front view the target goes INSIDE that reach, where
        // the nearest point of it is the one the knee is already at (straight below the socket:
        // nothing moved at all), and up-and-a-little-forward in a side view it stays inside by
        // centimetres (a knee raise stopped 17cm short). Such a target goes FORWARD — the way a
        // thigh flexes, along the figure's heading — out onto the reach: the knee comes up
        // under the cursor's height, as far ahead as the thigh's length puts it.
        if (plant < 1.0f) {
            const int junction = rig.limbJunction(effector);
            int top = effector;
            float thigh = 0.0f;
            for (;;) {
                const int par = m_bones[static_cast<std::size_t>(top)].parent;
                if (par < 0 || par == junction || par == root) {
                    break;
                }
                thigh += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(top)].localBind[3]));
                top = par;
            }
            if (top != effector && thigh > 1.0e-4f) {
                // (The socket as the drag FOUND it: read off the pose, the target followed the
                // hips it was moving, and a held knee crept forward for seconds.)
                const glm::vec3 socket = static_cast<std::size_t>(top) < m_jsStartPos.size()
                                             ? m_jsStartPos[static_cast<std::size_t>(top)]
                                             : glm::vec3(m_poseGlobal[static_cast<std::size_t>(top)][3]);
                const glm::vec3 fore(std::sin(m_jsRootHeading), 0.0f, std::cos(m_jsRootHeading));
                // (Off the floor the first centimetres of HEIGHT are taken up gently — the knee
                // rises h x smoothstep(h / kKneeLiftEase) under a cursor h up: a thigh hanging
                // straight down gains height as the SQUARE of its swing, so a knee that matched
                // the cursor's height from the first centimetre had 25cm to swing forward in the
                // cursor's first eight: the solve ran out of iterations four ticks running, the
                // hips rode up a centimetre on the unconverged poses and snapped back.)
                glm::vec3 lifted = *goalPoint;
                if (kneeFootPin >= 0 && kneeLift > 0.0f) {
                    lifted.y -= kneeLift * (1.0f - glm::smoothstep(0.0f, kKneeLiftEase * legScale, kneeLift));
                }
                glm::vec3 v = lifted - socket;
                // ... AND WITHIN THE THIGH'S LATERAL REACH at that flexion (2026-09-23;
                // IK_JS_NO_KNEE_LATERAL_REACH): the thigh's rotation order puts its abduction
                // channel about the fore axis, INSIDE its flexion, so a thigh flexed to horizontal
                // has no abduction left — the channel rolls it — and its own twist is locked: a
                // knee drawn up to hip height comes in front of its socket whatever the cursor
                // asks sideways (lateral = thigh x cos(flexion) x sin(abduction limit)). Asked
                // where the kneel had it — 9cm outboard on the base rig, 18 on a wide-kneeling
                // character — the knee's row went unmet by that much at the top, its bounded pull
                // leaned on the pelvis and the shin's side channel through the hold, and the pose
                // flipped basins (the shin's side channel from one limit to the other, the pelvis
                // 7 degrees in a tick, a toe 68-74mm; the character's knee drifted through its
                // still hold). The goal comes in as the thigh comes up, and the row is met.
                static const bool kNoLateralReach = std::getenv("IK_JS_NO_KNEE_LATERAL_REACH") != nullptr; // A/B probe
                if (!kNoLateralReach) {
                    const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
                    const float     lat = glm::dot(v, lateral);
                    const float     along = glm::dot(v, fore);
                    const float     flexion = std::atan2(std::max(along, 0.0f), std::max(-v.y, 0.0f)); // 0 hanging, pi/2 horizontal forward
                    const bool      outward = lat * socket.x > 0.0f; // (away from the midline)
                    const glm::vec3& lim = outward ? (socket.x >= 0.0f ? m_bones[static_cast<std::size_t>(top)].rotMax : m_bones[static_cast<std::size_t>(top)].rotMin)
                                                   : (socket.x >= 0.0f ? m_bones[static_cast<std::size_t>(top)].rotMin : m_bones[static_cast<std::size_t>(top)].rotMax);
                    const float     abduct = glm::radians(std::min(std::abs(lim.z), 89.0f));
                    // (The plain geometry, no floor under cos(flexion): a quarter of the reach kept
                    // at full flexion — a hedge — left the foot 21mm behind its knee on a
                    // wide-kneeling character and 48 on the oldest generation, where the reach as
                    // it is lands it 6mm ahead and 8 behind; nothing else moved.)
                    // (With the rotation order composed the format's way — 2026-10-27 — the
                    // abduction is applied INSIDE the flexion and keeps its whole lateral reach
                    // at any flexion: the cos(flexion) was the roll, which is gone. The A/B
                    // POSESTUDIO_ORDER_AS_WRITTEN keeps the old composition, and its clamp.)
                    const float     latMax = thigh * (kOrderAsWritten ? std::cos(flexion) : 1.0f) * std::sin(abduct);
                    if (std::abs(lat) > latMax) {
                        v -= lateral * (lat - glm::sign(lat) * latMax);
                        lifted = socket + v;
                    }
                }
                const float inside = thigh * thigh - glm::dot(v, v);
                if (inside > 0.0f) {
                    const float along = glm::dot(v, fore);
                    lifted += fore * (-along + std::sqrt(along * along + inside));
                }
                static const double kLiftBlend = envOr("IK_JS_KNEELIFTBLEND", 1.0);
                kneeGoal = glm::mix(kneeGoal, lifted, 1.0f - glm::smoothstep(0.0f, static_cast<float>(kLiftBlend), plant));
                // ... AND THE LAST CENTIMETRE IS THE FOOT'S (2026-09-22): a knee at hip height in a
                // half kneel is a centimetre short of a shin's length over the floor on every rig,
                // and the hang's target (kneeHangOffset) asks the foot under it; the ankle's
                // floor contact should lift the knee that centimetre, but the knee sits inside
                // its cursor row's Huber knee, where the model sees the row's whole 1e6 and
                // never proposes the rise the true cost (2 against the hang's 7) would take —
                // the foot rested 9-14cm behind the knee with its toes curled. The goal itself
                // rises, by the hang's under-share, to where a vertical shin stands on the
                // floor: the knee ends up to 2cm over the cursor, and the foot under the knee.
                if (kneeFootPin >= 0 && kneeAnkle >= 0 && plant < 1.0f &&
                    static_cast<std::size_t>(kneeAnkle) < m_jsStartPos.size()) {
                    const glm::vec3 found = m_jsStartPos[static_cast<std::size_t>(kneeAnkle)] - m_jsStartPos[static_cast<std::size_t>(effector)];
                    const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
                    const float     lat = glm::dot(found, lateral);
                    const float     len = glm::length(found);
                    const float     vlen = std::sqrt(std::max(0.0f, len * len - lat * lat));
                    const float     floorAnkle = -m_transform[3][1] + rig.floorClearance(kneeAnkle);
                    const float     height = kneeGoal.y - floorAnkle;
                    const float     reach2 = vlen * vlen - height * height;
                    if (reach2 > 0.0f && height > 0.0f) {
                        // (The WHOLE deficit, under a cap that ramps in with the cursor's reach: a
                        // share of it leaves the foot on the floor behind a knee the Huber-blind
                        // model will not lift, and the oldest generation's shin is 7cm longer
                        // than its thigh — its half kneel stands the knee 7cm over the hip.)
                        const float underShare = 1.0f - glm::smoothstep(kKneeUnderReachFrom * len, kKneeUnderReachTo * len, std::sqrt(reach2));
                        const float cap = underShare * kKneeGoalRaiseShare * len;
                        kneeGoal.y += (1.0f - plant) * std::min(cap, vlen - height);
                    }
                }
                static const bool kKneeTrace2 = std::getenv("IK_JS_KNEE_TRACE") != nullptr;
                if (kKneeTrace2) {
                    std::fprintf(stderr, "[knee]   plant %.3f socket(%.3f %.3f %.3f) thigh %.3f inside %.4f lifted(%.3f %.3f %.3f) -> goal(%.3f %.3f %.3f)", plant, socket.x, socket.y, socket.z, thigh, inside, lifted.x, lifted.y, lifted.z, kneeGoal.x, kneeGoal.y, kneeGoal.z);
                    std::fputc(10, stderr);
                }
            }
        }
    }
}

/// An ELBOW drag: the elbow's goal on the upper arm's reach, the hand that tends to stay, a pin below the elbow that holds.
void Armature::ikElbowDrag(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const int root = s.root;
    const int effector = s.effector;
    const std::vector<IkEffector>& pins = s.pins;
    std::vector<char>& dofBone = s.dofBone;
    std::vector<char>& cls = s.cls;
    std::vector<char>& twistPriced = s.twistPriced;
    std::vector<char>& girdle = s.girdle;
    const float& legScale = s.legScale;
    bool& legDrag = s.legDrag;
    const glm::vec3*& goalPoint = s.goalPoint;
    bool& kneeDrag = s.kneeDrag;
    bool& effectorFolds = s.effectorFolds;

    // An ELBOW drag: the grabbed joint is a fold joint that is not a knee. What an elbow is
    // grabbed FOR is its swivel — elbows out, in, tucked, a hand kept on a hip — so the HAND tends
    // to stay where it is: a soft hold of the wrist's place (kElbowHandHoldWeight), a user pin
    // on it (held in full now: kEffectorPinHoldReach) or a hand on the floor (seeded as a live
    // contact below the elbow). The forearm rode the elbow rigidly before: every elbow adjustment
    // threw the hand off whatever it had been placed on. Taken right away, the elbow still
    // carries its arm (the hold goes with the target's distance from that place).
    static const bool kNoElbowDrag = std::getenv("IK_JS_NO_ELBOW_DRAG") != nullptr; // A/B probe
    const auto freeChannels = [&](int b) {
        const Bone& bone = m_bones[static_cast<std::size_t>(b)];
        int count = 0;
        for (int a = 0; a < 3; ++a) {
            count += (bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg) ? 0 : 1;
        }
        return count;
    };
    s.elbowDrag = false;
    bool& elbowDrag = s.elbowDrag;
    s.elbowWrist = -1;   // the limb's end below the elbow: past the forearm's twist bones
    int& elbowWrist = s.elbowWrist;
    s.elbowHeldPin = -1; // a pin below the elbow (a user pin, a hand on the floor)
    int& elbowHeldPin = s.elbowHeldPin;
    s.elbowGoal = glm::vec3(0.0f);
    glm::vec3& elbowGoal = s.elbowGoal;
    if (!kNoElbowDrag && effectorFolds && !kneeDrag && !legDrag && goalPoint != nullptr) {
        int cur = effector;
        for (int depth = 0; depth < 4 && elbowWrist < 0; ++depth) {
            int main = -1;
            float longest = 0.0f;
            for (const int c : m_children[static_cast<std::size_t>(cur)]) {
                const float len = glm::length(glm::vec3(m_bones[static_cast<std::size_t>(c)].localBind[3]));
                if (len > longest) {
                    longest = len;
                    main = c;
                }
            }
            if (main < 0) {
                break;
            }
            if (freeChannels(main) >= 2) {
                elbowWrist = main;
            }
            cur = main;
        }
        elbowDrag = elbowWrist >= 0 && static_cast<std::size_t>(elbowWrist) < m_jsStartPos.size();
    }
    if (elbowDrag) {
        elbowGoal = *goalPoint;
        dofBone[static_cast<std::size_t>(effector)] = 1; // the elbow's own bend: what lets the hand stay
        for (std::size_t p = 0; p < pins.size() && elbowHeldPin < 0; ++p) {
            if (pins[p].node >= 0 && rig.pinUnderEffector(p)) {
                elbowHeldPin = static_cast<int>(p);
            }
        }
        if (elbowHeldPin < 0) {
            cls[static_cast<std::size_t>(effector)] = 1; // (no pin below: not a pin's limb, and not trunk)
        }
        // The elbow lives on the upper arm's reach about its SOCKET (the first joint above it
        // that swings: past the upper arm's twist bones; as the drag found it), and a cursor
        // drags in a plane: a target INSIDE that reach could only be met by moving the socket
        // itself, and was — the collar shrugged 46 degrees to raise an elbow 20cm while the
        // shoulder turned 25 the other way. Such a target goes onto the reach ALONG ITS OWN
        // DIRECTION from the socket (the nearest point of it: in, out, forward, back, the elbow
        // goes the way it is dragged) — turned OUTWARD, away from the body's midline, by as
        // much as the drag is an UPWARD pull: an arm hanging beside the body has its elbow all
        // but under its socket, where "up" is straight at the socket and the nearest point is
        // the one the elbow is at; pulled up, an elbow lifts out to the side. (A knee's goes
        // forward, the way a thigh flexes. Outward for EVERY inside target was tried: an elbow
        // dragged inward went outward.)
        int socket = m_bones[static_cast<std::size_t>(effector)].parent;
        float upper = glm::length(glm::vec3(m_bones[static_cast<std::size_t>(effector)].localBind[3]));
        while (socket >= 0 && socket != root && freeChannels(socket) < 2) {
            upper += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(socket)].localBind[3]));
            socket = m_bones[static_cast<std::size_t>(socket)].parent;
        }
        if (socket >= 0 && socket != root) {
            const int junction = rig.limbJunction(effector);
            for (int cur = m_bones[static_cast<std::size_t>(socket)].parent; cur >= 0 && cur != junction && cur != root;
                 cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                girdle[static_cast<std::size_t>(cur)] = 1;
            }
        }
        if (socket >= 0 && socket != root && static_cast<std::size_t>(socket) < m_jsStartPos.size()) {
            const glm::vec3 lateral(std::cos(m_jsRootHeading), 0.0f, -std::sin(m_jsRootHeading));
            const glm::vec3 socketAt = m_jsStartPos[static_cast<std::size_t>(socket)];
            const float side = glm::dot(socketAt - m_jsStartPos[static_cast<std::size_t>(root)], lateral) < 0.0f ? -1.0f : 1.0f;
            const glm::vec3 outward = lateral * side;
            const glm::vec3 v = elbowGoal - socketAt;
            const float dist = glm::length(v);
            if (dist < upper && dist > 1.0e-4f) {
                // (The outward turn eases in over the first centimetres of depth: an arm hanging
                // near straight below its socket swings as the square root of the height its
                // elbow gains — raised 1cm at the cursor's height it was asked 3.5cm outward.)
                const float depth = upper - dist;
                const glm::vec3 pulled = *goalPoint - m_jsStartTarget;
                const float travel = glm::length(pulled);
                const float upness = travel > 1.0e-4f ? std::max(0.0f, pulled.y / travel) : 0.0f;
                const glm::vec3 turned =
                    v + outward * (depth * glm::smoothstep(0.0f, kElbowReachEase * legScale, depth) * upness);
                elbowGoal = socketAt + turned * (upper / glm::length(turned));
            } else if (dist > upper) {
                // ... and a target a little BEYOND the reach comes back onto it too (the first
                // kElbowReachBand of the excess; what is past that still goes through, and takes
                // the body with it): a cursor dragged sideways in a plane leaves the sphere at
                // once, and the 3cm it was outside after an 8cm swing were 3cm of TRUNK — the
                // chest leaned after an elbow that was only being swung out.
                const float excess = dist - upper;
                elbowGoal = socketAt + v * ((upper + std::max(0.0f, excess - kElbowReachBand * legScale)) / dist);
            }
        }
        // A pin below it that is held IN FULL (a user pin; a hand on the floor, which the rig
        // makes hard below a dragged joint): the elbow lives within the forearm's length of it.
        if (elbowHeldPin >= 0 && (rig.pinIsUser(static_cast<std::size_t>(elbowHeldPin)) ||
                                  pins[static_cast<std::size_t>(elbowHeldPin)].hard)) {
            const glm::vec3 held = pins[static_cast<std::size_t>(elbowHeldPin)].target;
            float reach = 0.0f;
            for (int cur = pins[static_cast<std::size_t>(elbowHeldPin)].node; cur >= 0 && cur != effector;
                 cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                reach += glm::length(glm::vec3(m_bones[static_cast<std::size_t>(cur)].localBind[3]));
            }
            const glm::vec3 out = elbowGoal - held;
            const float far = glm::length(out);
            if (far > reach && far > 1.0e-5f) {
                elbowGoal -= out * ((far - reach) / far);
            }
        }
    }
    {
        const int junction = rig.limbJunction(effector);
        for (int cur = m_bones[static_cast<std::size_t>(effector)].parent;
             cur >= 0 && cur != junction && cur != root;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            cls[static_cast<std::size_t>(cur)] = 1;
            if (legDrag) {
                twistPriced[static_cast<std::size_t>(cur)] = 1;
            }
        }
    }
    // THE GROUP's secondaries (2026-10-27): each one's limb is a dragged limb too — class 1 from
    // the joint up to its junction, with no twist price (a second hand is as free as the first)
    // unless the limb serves a pin below the joint (a secondary knee over its planted foot swings
    // at the pin's price, as the primary knee's leg does before its foot lets go) or the joint is
    // a foot's (a leg placed by its foot has no business turning: the primary foot drag's rule).
    for (const int sec : s.secondaries) {
        if (sec < 0 || static_cast<std::size_t>(sec) >= s.n || sec == s.root) {
            continue;
        }
        bool pinBelow = false;
        for (std::size_t p = 0; p < s.pins.size() && !pinBelow; ++p) {
            pinBelow = s.pins[p].node >= 0 && s.pins[p].node != sec && s.solver.isAncestorOrSelf(sec, s.pins[p].node);
        }
        if (pinBelow) {
            continue;
        }
        const bool secLeg = m_ikBindPos[static_cast<std::size_t>(sec)].y < 0.20f * s.legScale;
        // The joint's OWN channels are priced as the primary's are: a foot's or a fold joint's
        // (a knee's, an elbow's) at the limb's price, a WRIST's at the trunk's — the primary wrist's
        // channels are unknowns (the hand lay) at that price, and priced as a limb a second hand's
        // ran to its 70-degree limit under its fingers' body-volume rows while the first stayed
        // at zero: 14-18cm at a thumb, on every rig.
        const Bone& secBone = m_bones[static_cast<std::size_t>(sec)];
        bool secFolds = false;
        for (int a = 0; a < 3 && !secFolds; ++a) {
            const float range = secBone.rotMax[a] - secBone.rotMin[a];
            secFolds = secBone.rotLimited[a] && range >= kFoldRangeDeg && secBone.rotMin[a] < 0.0f && secBone.rotMax[a] > 0.0f &&
                       std::min(-secBone.rotMin[a], secBone.rotMax[a]) <= kFoldNarrowFraction * std::max(-secBone.rotMin[a], secBone.rotMax[a]);
        }
        if (secLeg || secFolds) {
            s.cls[static_cast<std::size_t>(sec)] = 1;
        }
        const int junction = s.rig.limbJunction(sec);
        for (int cur = m_bones[static_cast<std::size_t>(sec)].parent; cur >= 0 && cur != junction && cur != s.root;
             cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            s.cls[static_cast<std::size_t>(cur)] = 1;
            s.twistPriced[static_cast<std::size_t>(cur)] = secLeg ? 1 : 0;
        }
    }
}

void Armature::ensureHandMaps() const {
    const std::size_t n = m_bones.size();
    if (m_jsWristOf.size() == n || !m_ikRig) {
        return;
    }
    m_jsWristOf.assign(n, -1);
    m_jsHandTip.assign(n, -1);
    const IkRig& rig = *m_ikRig;
    const auto under = [&](int node, int ancestor) {
        for (int cur = node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
            if (cur == ancestor) {
                return true;
            }
        }
        return false;
    };
    const auto freeChannels = [&](int b) {
        const Bone& bone = m_bones[static_cast<std::size_t>(b)];
        int count = 0;
        for (int a = 0; a < 3; ++a) {
            count += !(bone.rotLimited[a] && bone.rotMax[a] - bone.rotMin[a] < kLockedRangeDeg);
        }
        return count;
    };
    for (std::size_t e = 0; e < n; ++e) {
        const Bone& bone = m_bones[e];
        bool folds = false; // an ELBOW: a fold joint ...
        for (int a = 0; a < 3 && !folds; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            folds = bone.rotLimited[a] && range >= kFoldRangeDeg && bone.rotMin[a] < 0.0f && bone.rotMax[a] > 0.0f &&
                    std::min(-bone.rotMin[a], bone.rotMax[a]) <= kFoldNarrowFraction * std::max(-bone.rotMin[a], bone.rotMax[a]);
        }
        if (!folds || static_cast<int>(e) == rig.rootNode()) {
            continue;
        }
        bool leg = false; // ... with no foot below it (that is a knee)
        for (std::size_t i = 0; i < n && !leg; ++i) {
            leg = m_ikBindPos[i].y < 0.20f * rig.sizeScale() && under(static_cast<int>(i), static_cast<int>(e));
        }
        if (leg) {
            continue;
        }
        int wrist = -1;
        int cur = static_cast<int>(e);
        for (int depth = 0; depth < 4 && wrist < 0; ++depth) {
            int   main = -1;
            float longest = 0.0f;
            for (const int c : m_children[static_cast<std::size_t>(cur)]) {
                const float len = glm::length(glm::vec3(m_bones[static_cast<std::size_t>(c)].localBind[3]));
                if (len > longest) {
                    longest = len;
                    main = c;
                }
            }
            if (main < 0) {
                break;
            }
            if (freeChannels(main) >= 2) {
                wrist = main;
            }
            cur = main;
        }
        if (wrist < 0 || m_jsWristOf[static_cast<std::size_t>(wrist)] >= 0) {
            continue; // (a finger's middle joint is a fold joint too: its "wrist" is inside a hand already found)
        }
        float reach = 0.0f;
        for (std::size_t i = static_cast<std::size_t>(wrist); i < n; ++i) {
            if (!under(static_cast<int>(i), wrist)) {
                continue;
            }
            m_jsWristOf[i] = wrist;
            const float d = glm::length(m_ikBindPos[i] - m_ikBindPos[static_cast<std::size_t>(wrist)]);
            if (d > reach) {
                reach = d;
                m_jsHandTip[static_cast<std::size_t>(wrist)] = static_cast<int>(i);
            }
        }
    }
}

glm::vec3 Armature::handPalmNormal(int wrist) const {
    ensureHandMaps();
    const std::size_t n = m_bones.size();
    if (wrist < 0 || static_cast<std::size_t>(wrist) >= n || m_jsWristOf.size() != n ||
        m_jsWristOf[static_cast<std::size_t>(wrist)] != wrist || m_jsHandTip[static_cast<std::size_t>(wrist)] < 0) {
        return glm::vec3(0.0f);
    }
    // The bind's fingers line and palm normal (every bind is translation-only: the bone's local
    // axes are the world's there — the floor palm's frame, see ikPinRows).
    const std::size_t tip = static_cast<std::size_t>(m_jsHandTip[static_cast<std::size_t>(wrist)]);
    const glm::vec3   fingers = m_ikBindPos[tip] - m_ikBindPos[static_cast<std::size_t>(wrist)];
    const float       reach = glm::length(fingers);
    if (reach < 1.0e-4f) {
        return glm::vec3(0.0f);
    }
    const glm::vec3 f = fingers / reach;
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    glm::vec3       palm = down - f * glm::dot(down, f);
    if (glm::length(palm) < 0.2f) {
        return glm::vec3(0.0f);
    }
    palm = glm::normalize(palm);
    const glm::vec3 posed = glm::mat3(m_transform) * (glm::mat3(m_poseGlobal[static_cast<std::size_t>(wrist)]) * palm);
    const float     len = glm::length(posed);
    return len > 1.0e-6f ? posed / len : glm::vec3(0.0f);
}

} // namespace pose
