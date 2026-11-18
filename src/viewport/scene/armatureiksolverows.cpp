/**
 * @file armatureiksolverows.cpp
 * @brief The solve's rows that belong to no joint's hold (see armatureiksolve.cpp for the map):
 *        BALANCE — the centre of mass over the support polygon, with its slack ramp — and the
 *        FLOOR's and the BODY VOLUMES' one-sided plane rows, whose source the solver regenerates at
 *        every linearization. (The pins' rows are armatureiksolvepins.cpp, the dragged joint's
 *        armatureiksolvecursor.cpp.)
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace pose {

namespace {
// The plane source's probes (ikPlaneRows), read once here: the source runs at every linearization and
// every trial of every solve, over bones x volumes, and a function-local static is a guard check per pass.
const bool   kCeilingTrace = std::getenv("IK_JS_CEILING_TRACE") != nullptr;      // the hand / knee ceilings and where their joints stand
const bool   kNoFleshRows = std::getenv("IK_JS_NO_FLESH_ROWS") != nullptr;       // A/B probe
const bool   kDropBorrowed = std::getenv("IK_JS_DROP_BORROWED") != nullptr;      // probe
const bool   kPlaneTrace = std::getenv("IK_JS_PLANE_TRACE") != nullptr;          // every floor and volume row in contact (which joint, against what)
const bool   kNoTwins = std::getenv("IK_JS_FLESH_NO_TWINS") != nullptr;          // A/B probe
const bool   kBareCaps = std::getenv("IK_JS_BARE_JOINT_CAPS") != nullptr;        // A/B probe
const bool   kSegmentRowsCut = std::getenv("IK_JS_SEGMENT_ROWS_CUT") != nullptr; // A/B probe
} // namespace

/// The BALANCE row: the support polygon (grown into, a stepping foot kept in it), the slack ramp, the hinge's and the follow's release, the seat that keeps no balance.
void Armature::ikBalanceRow(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const int effector = s.effector;
    const float& rising = s.rising;
    float& pelvisBalance = s.pelvisBalance;
    float& hingeSlack = s.hingeSlack;
    const float& slide = s.slide;
    int& kneeFootPin = s.kneeFootPin;
    float& plant = s.plant;
    JointSolver::Problem& problem = s.problem;
    const bool& trunkDrag = s.trunkDrag;
    const bool& trunkRise = s.trunkRise;

    if (m_ikScope == IkScope::Chain) {
        return; // (a SCOPED drag keeps no balance: the chain alone moves, and the body it hangs from stays where it was)
    }
    // (The pose with every hung arm and the righted head put back where they would ride —
    // balancePositions() — computed once a tick, where two readings below ask for it.)
    const auto ridePositions = [&]() -> const std::vector<glm::vec3>& {
        if (!s.ridePositionsValid) {
            balancePositions(s.ridePositions);
            s.ridePositionsValid = true;
        }
        return s.ridePositions;
    };

    // Balance — not under a pelvis drag (the user is driving the pelvis).
    static const double kBalance = envOr("IK_JS_BALANCE", kBalanceWeight);
    // The SUPPORT: the rig's (the planted contacts) — plus a SLIDING foot: a foot slid along the
    // floor bears weight, and the body's weight may come between the feet (it must, for a
    // stride). Its footprint joins the support BY THE SLIDE — drawn out from the middle of the
    // rig's support to where the foot is as the foot comes down — because a support that arrives
    // in one tick is a jump: a foot lifted, carried 35cm sideways and set down had the hips sink
    // 12cm on the way down (balance holding the weight over the standing foot while the leg
    // reached) and spring 6cm back up in the tick the foot joined the support.
    s.support = rig.supportHull();
    std::vector<glm::vec2>& support = s.support;
    bool footInSupport = false;
    // A foot in a STEP stays in it, where its swing has it (the rig's own polygon drops it: "the
    // foot in flight supports nothing"). A step is a fall caught by the foot that takes it, not a
    // balance on the other one: held over the standing foot alone by a hard row, the centre of
    // mass was hauled 10cm sideways in the twelve ticks of a swing — the spine twisted to its
    // limits to do it, the arms swinging 20cm a tick — and let go again at the landing.
    static const bool kNoSwingSupport = std::getenv("IK_JS_NO_SWING_SUPPORT") != nullptr; // A/B probe
    // (Not under a pelvis drag: its balance is the hinge's, fore and aft only, and the hinge is
    // being let go of as the walk begins.)
    if (!kNoSwingSupport && effector != root && rig.steppingPin() >= 0) {
        support = BalanceController::supportPolygon(rig.supportPoints(-1));
    }
    if (slide > 0.0f && !support.empty()) {
        glm::vec2 middle(0.0f);
        for (const glm::vec2& v : support) {
            middle += v;
        }
        middle /= static_cast<float>(support.size());
        std::vector<int> stack{effector};
        while (!stack.empty()) {
            const int b = stack.back();
            stack.pop_back();
            const glm::vec3 at(m_poseGlobal[static_cast<std::size_t>(b)][3]);
            support.push_back(glm::mix(middle, glm::vec2(at.x, at.z), slide));
            stack.insert(stack.end(), m_children[static_cast<std::size_t>(b)].begin(),
                         m_children[static_cast<std::size_t>(b)].end());
        }
        support = BalanceController::supportPolygon(std::move(support));
        footInSupport = true;
    }
    // ... and the foot below a dragged knee LEAVES it by the plant, the same way round: its
    // footprint drawn in to the middle of the others' as it lets go. (The balance base below is
    // measured without it from the start, as for a dragged foot: a leg lifted by its knee has
    // never taken the body's weight over the other one either.)
    std::vector<glm::vec2> othersHull;
    if (kneeFootPin >= 0) {
        std::vector<glm::vec2> others = rig.supportPoints(kneeFootPin);
        if (!others.empty()) {
            glm::vec2 middle(0.0f);
            for (const glm::vec2& v : others) {
                middle += v;
            }
            middle /= static_cast<float>(others.size());
            othersHull = BalanceController::supportPolygon(others);
            if (plant < 1.0f) {
                for (const glm::vec2& v : rig.pinFootprintPoints(static_cast<std::size_t>(kneeFootPin))) {
                    others.push_back(glm::mix(middle, v, plant));
                }
                support = BalanceController::supportPolygon(std::move(others));
                footInSupport = true;
            }
        }
    }
    // A FOLLOWED trunk drag is the user taking the body somewhere, as a pelvis drag is: what
    // answers hips that outrun their feet is a step, and until it lands the body is as far off
    // its support as the follow has taken it — let go of as SLACK (the lean's own share stays
    // balanced). Held to the support's edge instead, the hips waited there for every step, the
    // steps came 6cm long, and a chest dragged at walking pace trailed 18cm behind its cursor.
    const float followSlack = glm::length(m_jsTrunkFollow) * m_jsTrunkWalk;
    // THE SEAT (IkRig::seatPin: a user pin on the pelvis girdle) keeps no balance: what holds a
    // pinned pelvis up is whatever the user sat her on, and the row knows only the feet. With it
    // on, a seated figure — her weight a forearm's length behind her feet, as posed — could not
    // recline at all (the chest pulled 12cm back went 3: every centimetre made the "imbalance"
    // worse) and leaned forward only as far as kept it no worse.
    // (... and A PELVIS ON THE FLOOR — IkRig::floorSeat, a live contact of the girdle — is a seat too:
    // there is nowhere to fall from a sit on the floor, and reclining onto her back takes the
    // weight behind every support she has until the back itself lands. Balanced, she would not
    // recline at all: the chest stopped 49cm from its cursor.)
    static const bool kNoFloorSeat = std::getenv("IK_NO_FLOOR_SEAT") != nullptr; // A/B probe
    // (... and A FIGURE ON HER KNEES whose TRUNK is dragged keeps no balance either: a kneeling
    // body cannot step, and her trunk taken forward past her knees is going onto her hands —
    // which is what the knees are for. Balanced over knees and toes, the row's answer to a
    // chest taken forward from a kneel was to haul the hips back and UP: the knees, unilateral
    // contacts, lifted 7-8cm off the floor as the shins came up onto the toes, and when the
    // hands landed the body dropped back onto them — 104-144mm at the head in one tick.)
    static const bool kNoKneelBalanceOff = std::getenv("IK_JS_KNEEL_BALANCED") != nullptr; // A/B probe
    const bool seated = rig.seatPin() >= 0 || (!kNoFloorSeat && rig.floorSeat()) ||
                        (!kNoKneelBalanceOff && m_jsKneelStart && trunkDrag);
    if ((effector != root || pelvisBalance > 0.0f) && !seated && !support.empty() && kBalance > 0.0) {
        problem.masses = &rig.masses();
        problem.supportHull = &support;
        problem.balanceMargin = rig.balanceMargin();
        // (Balance is BLIND to the idle arms' hang — hangIdleArms: the centre of mass is shifted
        // back by what the hang has moved it, for this solve.)
        if (balanceIsBlind() || kArmHangTrace) { // (... and to the head's righting)
            std::vector<glm::vec3>& real = s.realPositions;
            real.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                real[i] = glm::vec3(m_poseGlobal[i][3]);
            }
            const glm::vec3 hung = BalanceController::centerOfMass(real, rig.parents(), rig.masses());
            const glm::vec3 rides = BalanceController::centerOfMass(ridePositions(), rig.parents(), rig.masses());
            problem.comShift = glm::dvec2(rides.x - hung.x, rides.z - hung.z);
            if (kArmHangTrace) {
                const glm::vec2 at(rides.x, rides.z);
                const glm::vec2 to = BalanceController::closestBalancedPoint(support, at, rig.balanceMargin());
                std::fprintf(stderr, "[hang] com hung(%.4f %.4f) rides(%.4f %.4f) shift(%.4f %.4f) need %.4f slack %.4f+%.4f",
                             hung.x, hung.z, rides.x, rides.z, problem.comShift.x, problem.comShift.y,
                             glm::length(to - at), m_jsBalanceSlack, followSlack);
                std::fputc(10, stderr);
            }
        }
        problem.balanceWeight = effector == root ? kBalance * static_cast<double>(pelvisBalance) : kBalance;
        if (effector == root) {
            // (Fore and aft only: a hip HINGE. Sideways is the sway's — on the generations that
            // stand with their feet together an 8cm sway takes the centre of mass off the
            // support, and a sideways lean against the level chest it holds trembled 8cm.)
            problem.balanceAxis = glm::dvec2(s.fore.x, s.fore.z);
        }
        // The ramp: a changed support polygon (keyed by its vertex sum) starts with the
        // centre of mass' current distance as free slack, taken back a centimetre a tick.
        // (Keyed by the RIG's polygon and whether the dragged foot stands in the support — not by
        // that foot's place, which changes every tick of a slide: a key that changed every tick
        // reset the slack every tick, and balance never engaged.)
        float key = static_cast<float>(rig.supportHull().size()) + (footInSupport ? 1000.0f : 0.0f);
        for (const glm::vec2& v : rig.supportHull()) {
            key += v.x * 1.37f + v.y * 0.73f;
        }
        // (A foot in a step never left the solve's support, and lands where its swing already
        // had it: neither end of a step is a change to ramp in.)
        const bool swinging = !kNoSwingSupport && effector != root && rig.steppingPin() >= 0;
        if ((swinging || m_jsWasSwinging) && m_jsHullKey >= 0.0f) {
            m_jsHullKey = key;
        }
        m_jsWasSwinging = swinging;
        // A support that GROWS — a step landing under a pelvis drag, a hand come down — grows over
        // at kSupportGrowStep a tick: the new polygon drawn out from the middle of the one it replaces. A
        // one-sided row has nothing to say once it is met, so a body that LEANED to keep its
        // balance over one foot (the hip hinge through a step) was given its whole lean back the
        // tick the other foot landed: hips taken 25cm back, the head went 9-10cm in that tick.
        // (Until the balance point became the true inset polygon's, see closestBalancedPoint, its
        // oblique pull left a need standing after the landing, and that — by accident — was what
        // had eased this.)
        static const bool kNoSupportGrow = std::getenv("IK_JS_NO_SUPPORT_GROW") != nullptr; // A/B probe
        if (key != m_jsHullKey && m_jsHullKey >= 0.0f) {
            bool grew = !kNoSupportGrow && support.size() >= 3 && !m_jsLastSupport.empty();
            for (std::size_t i = 0; i < m_jsLastSupport.size() && grew; ++i) {
                const glm::vec2& v = m_jsLastSupport[i];
                grew = glm::length(BalanceController::closestBalancedPoint(support, v, 0.0f) - v) < 0.003f * rig.sizeScale();
            }
            m_jsGrown = 0.0f;
            m_jsGrowFrom.clear();
            if (grew) {
                m_jsGrowFrom = m_jsLastSupport;
            }
        }
        m_jsLastSupport = support;
        if (!m_jsGrowFrom.empty()) {
            // (Each new vertex grows out from the NEAREST point of the polygon it joins, not from
            // its middle: drawn out from the middle, a hand landed 26cm ahead of the knees was
            // inside the old polygon for its first 22 ticks — the support did not move at all —
            // while the drag took the weight on forward, and the balance row's answer to a chest
            // held on its cursor was to swing the hips UP about it: the knees lifted 25cm off the
            // floor and she went onto all fours on her toes, hips at standing height.)
            static const bool kGrowFromMiddle = std::getenv("IK_JS_GROW_FROM_MIDDLE") != nullptr; // A/B probe
            glm::vec2 centre(0.0f);
            for (const glm::vec2& v : m_jsGrowFrom) {
                centre += v;
            }
            centre /= static_cast<float>(m_jsGrowFrom.size());
            m_jsGrown += kSupportGrowStep * rig.sizeScale();
            float                  farthest = 0.0f;
            std::vector<glm::vec2> points = m_jsGrowFrom;
            for (const glm::vec2& v : support) {
                const glm::vec2 from = kGrowFromMiddle ? centre : BalanceController::closestBalancedPoint(m_jsGrowFrom, v, 0.0f);
                const float     d = glm::length(v - from);
                farthest = std::max(farthest, d);
                points.push_back(d > m_jsGrown ? from + (v - from) * (m_jsGrown / d) : v);
            }
            if (m_jsGrown >= farthest) {
                m_jsGrowFrom.clear(); // grown
            } else {
                support = BalanceController::supportPolygon(std::move(points));
            }
        }
        if (key != m_jsHullKey) {
            m_jsHullKey = key;
            const std::vector<glm::vec3>& positions = ridePositions(); // (blind to the arms' hang)
            const std::vector<int>&       parents = rig.parents();
            glm::vec2 need(0.0f);
            BalanceController::balanceCorrection(positions, parents, rig.masses(), support,
                                                 rig.balanceMargin(), need);
            if (effector == root) {
                const glm::vec2 axis(s.fore.x, s.fore.z);
                need = axis * glm::dot(need, axis);
            }
            // (What the hinge has already let go of is slack in its own right: counted twice, a
            // support that changed gave the rest of the lean back at once.)
            m_jsBalanceSlack = std::max(m_jsBalanceSlack, glm::length(need) - hingeSlack - followSlack);
            if (m_jsBalanceBase < 0.0f) {
                // The drag's first solve: whatever imbalance the pose STARTS with is the user's
                // (the drag-start pose is the contract) and stays free for the whole drag — a
                // figure posed leaning must not straighten itself under a still cursor.
                // (Measured against the RIG's support, the dragged foot not in it: a foot lifted
                // off the floor has never taken the body's weight over the other one — the drag
                // begins with the weight between the feet, and that is the imbalance it keeps.)
                // (... unless THE WEIGHT SHIFT is on — the drag began on two planted feet, one of
                // which it can lift (m_jsWeightShift, beginIkDrag): then the weight DOES go over
                // the standing foot. The hips' home takes it most of the way, and the row polices
                // the rest — the lifted leg's own mass, which a home read at the press cannot
                // know: a knee held at its cursor 24cm outside its shifted socket carries the
                // weight 7cm back toward the lifted side, and forgiven it stayed there, 12cm from
                // the standing ankle. Measured against the support AS IT STOOD at the press, both
                // feet in it, the base is what it truly was: nothing.)
                static const bool kWeightForgiven = std::getenv("IK_JS_WEIGHT_FORGIVEN") != nullptr; // A/B probe: the old base
                const bool weightShifts = !kWeightForgiven && glm::dot(m_jsWeightShift, m_jsWeightShift) > 0.0f;
                glm::vec2 rigNeed(0.0f);
                BalanceController::balanceCorrection(positions, parents, rig.masses(),
                                                     weightShifts ? support : (othersHull.empty() ? rig.supportHull() : othersHull),
                                                     rig.balanceMargin(), rigNeed);
                m_jsBalanceBase = std::max(m_jsBalanceSlack, glm::length(rigNeed));
            }
        } else if (m_jsGrowFrom.empty()) {
            // (Not while the support is still growing under it: taken back at its own pace the
            // slack ran out before a landed foot's polygon had reached the weight, and the row
            // bit — a lean of 2 degrees a joint for two ticks, the head 7cm, given back as the
            // next step lifted off.)
            m_jsBalanceSlack = std::max(m_jsBalanceBase, m_jsBalanceSlack - kBalanceRampStep);
        }
        problem.balanceSlack = m_jsBalanceSlack + hingeSlack + followSlack;
        static const bool kHingeTrace = std::getenv("IK_JS_HINGE_TRACE") != nullptr;
        static const bool kHingeTraceAll = kHingeTrace && std::getenv("IK_JS_HINGE_TRACE")[0] == '2'; // (=2: every drag)
        if (kHingeTrace && (effector == root || kHingeTraceAll)) {
            std::vector<glm::vec3> at(n);
            std::vector<int> parentOf(n);
            for (std::size_t i = 0; i < n; ++i) {
                at[i] = glm::vec3(m_poseGlobal[i][3]);
                parentOf[i] = m_bones[i].parent;
            }
            glm::vec2 needNow(0.0f);
            BalanceController::balanceCorrection(at, parentOf, rig.masses(), support, rig.balanceMargin(), needNow);
            std::fprintf(stderr, "[hinge] gate %.3f slack %.4f base %.4f release %.4f steps %d stepping %d hull %zu grown %.3f need %.4f\n",
                         pelvisBalance, m_jsBalanceSlack, m_jsBalanceBase, hingeSlack, rig.stepsTaken(),
                         rig.steppingPin(), support.size(), m_jsGrowFrom.empty() ? -1.0f : m_jsGrown,
                         glm::length(needNow));
            {
                const glm::vec3 c = BalanceController::centerOfMass(at, parentOf, rig.masses());
                float z0 = 1e9f, z1 = -1e9f, x0 = 1e9f, x1 = -1e9f;
                for (const glm::vec2& v : support) {
                    x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
                    z0 = std::min(z0, v.y); z1 = std::max(z1, v.y);
                }
                std::fprintf(stderr, "[hinge]   com(%.3f %.3f) support x[%.3f %.3f] z[%.3f %.3f] margin %.3f need(%.3f %.3f)\n",
                             c.x, c.z, x0, x1, z0, z1, rig.balanceMargin(), needNow.x, needNow.y);
            }
        }
        if (trunkRise) {
            problem.balanceWeight *= static_cast<double>((1.0f - rising) * (1.0f - rising));
        }
    }
}

/// The FLOOR's and the BODY VOLUMES' one-sided rows: the exemptions, a letting-go foot's scale and allowance, the ceilings, and the plane source the solver regenerates at every linearization.
void Armature::ikPlaneRows(IkSolveScratch& s) {
    IkRig& rig = s.rig;
    const std::size_t n = s.n;
    const int root = s.root;
    const JointSolver& solver = s.solver;
    const std::vector<IkEffector>& pins = s.pins;
    const float& legScale = s.legScale;
    int& kneeFootPin = s.kneeFootPin;
    float& plant = s.plant;
    float& kneeLift = s.kneeLift;
    JointSolver::Problem& problem = s.problem;
    std::vector<char>& footFreed = s.footFreed;
    std::vector<std::tuple<int, float, float>>& handCeilings = s.handCeilings;

    // The FLOOR and the BODY VOLUMES: one-sided rows, regenerated from the current frames at
    // every linearization (the contact normal follows a joint around a capsule). A pin's own
    // subtree is exempt from the floor (a planted foot's toes are the heel lift's business),
    // and everything the rig exempts from a volume stays exempt (IkRig::dragVolumes: the
    // pairs masked at bind and at drag start, the fingers against the limbs).
    static const bool kNoPlanes = std::getenv("IK_JS_NO_PLANES") != nullptr; // A/B probe
    std::vector<char>& floorExempt = s.floorExempt;
    std::vector<float>& floorScale = s.floorScale; // a joint's floor rows at less than their weight: a foot LETTING GO (below)
    std::vector<float>& floorAllow = s.floorAllow; // ... and forgiven the depth they were FOUND at (m): a row that comes in on a joint already through its surface asks only that it go no deeper
    floorExempt.assign(n, 0);
    floorScale.assign(n, 1.0f);
    floorAllow.assign(n, 0.0f);
    for (std::size_t p = 0; p < pins.size(); ++p) {
        const IkEffector& pin = pins[p];
        if (rig.pinIsLive(p) && !pin.hard) {
            continue; // a unilateral contact: the floor row IS its vertical hold
        }
        if (rig.pinIsLive(p)) {
            // (A HARD live contact — a knee planted over its foot — holds its own joint and nothing
            // below it: the shin and the foot keep their floor rows. Its whole subtree was exempt,
            // and a foot let go of behind it — THE FEET COME UP — had no floor at all: the newest
            // generation's toes went 44mm through it as she lay down on her belly.)
            floorExempt[static_cast<std::size_t>(pin.node)] = 1;
            continue;
        }
        if (static_cast<int>(p) == kneeFootPin && plant < 1.0f) {
            // Letting go: a foot coming off the floor, and the floor is the floor — but the floor's
            // rows come IN with the release, at (1 - plant) of their weight, not all at once: a
            // kneeling foot stands on its tucked toe tips, which by their flesh's measure are 2-5mm
            // "through" the floor, and the moment its exemption dropped (the lift's first
            // centimetre) a dozen 1e7 rows landed on it at once — a cost of 800 from nowhere, and
            // the knee shoved 8cm sideways and back (75-135mm in a tick, on every rig, worst on
            // the three whose kneel lies flattest). IK_JS_KNEE_FLOOR_SWITCH restores the switch.
            static const bool kFloorSwitch = std::getenv("IK_JS_KNEE_FLOOR_SWITCH") != nullptr; // A/B probe
            if (!kFloorSwitch) {
                std::vector<int>& footSubtree = s.subtreeBuf; // (the scratch's buffer)
                footSubtree.clear();
                collectSubtree(pin.node, footSubtree);
                // ... AND FORGIVEN THE DEPTH THEY ARE FOUND AT (2026-09-23; IK_JS_KNEE_FLOOR_NO_ALLOW):
                // the share brings the rows in gently, but a row's residual is the whole depth
                // from its first tick, and a foot whose tucked toe tips stand 15-18mm through the
                // floor by their flesh's measure (the third generation's; 2-5mm on the others)
                // was pushed out of it at a fiftieth of 1e7 already — the foot rolled and yawed to
                // its limits, the shin's side channel too, and the thigh abducted 15 degrees in
                // one tick: the knee shot 8cm out and back (84mm at the shin). A row that comes
                // in on a joint ALREADY through its surface asks only that it go no deeper —
                // the depth at the release's first tick is its allowance, a constant of the
                // drag, given back over the TRAIL (kKneeLiftFull..kKneeTrailFull of lift, by
                // when the foot is in the air and level).
                static const bool kNoAllow = std::getenv("IK_JS_KNEE_FLOOR_NO_ALLOW") != nullptr; // A/B probe
                if (!kNoAllow && !m_jsFootAllowValid) {
                    m_jsFootAllow.assign(n, 0.0f);
                    for (const int b : footSubtree) {
                        const float clearance = rig.floorClearance(b);
                        if (clearance <= 0.0f) {
                            continue;
                        }
                        const float depth = -m_transform[3][1] + clearance - m_poseGlobal[static_cast<std::size_t>(b)][3].y;
                        m_jsFootAllow[static_cast<std::size_t>(b)] = std::max(0.0f, depth);
                    }
                    m_jsFootAllowValid = 1;
                }
                const float trailNow = glm::smoothstep(kKneeLiftFull * legScale, kKneeTrailFull * legScale, kneeLift);
                for (const int b : footSubtree) {
                    floorScale[static_cast<std::size_t>(b)] = 1.0f - plant;
                    if (!kNoAllow && m_jsFootAllowValid && m_jsFootAllow.size() == n) {
                        floorAllow[static_cast<std::size_t>(b)] = m_jsFootAllow[static_cast<std::size_t>(b)] * (1.0f - trailNow);
                    }
                }
            }
            continue;
        }
        if (p < s.footLying.size() && s.footLying[p]) {
            // (A KNEELING FOOT LIES FLAT: the foot-class joints' clearances are their BIND heights —
            // the standing model's — and a lying ankle sits at the shin's radius, its mid-foot
            // joint beside it: their rows would hold the foot level 5cm off the floor. Exempt from
            // the ankle down to the ball's parent; the ball's and the toes' rows, a toe's thickness
            // over the floor, stay and stop the foot's top there.)
            const int ball = m_jsBall[static_cast<std::size_t>(pin.node)];
            for (int cur = ball >= 0 ? m_bones[static_cast<std::size_t>(ball)].parent : pin.node; cur >= 0; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                floorExempt[static_cast<std::size_t>(cur)] = 1;
                if (cur == pin.node) {
                    break;
                }
            }
            continue;
        }
        if (p < footFreed.size() && footFreed[p]) {
            continue; // (a foot behind a planted knee the hips have gone ahead of: its height rows
                      // are gone, and only the floor keeps it up — exempt, two characters' feet
                      // went 36-45mm through it as she lay down)
        }
        std::vector<int>& subtree = s.subtreeBuf; // (the scratch's buffer)
        subtree.clear();
        collectSubtree(pin.node, subtree);
        for (const int b : subtree) {
            floorExempt[static_cast<std::size_t>(b)] = 1;
        }
        if (m_jsBall[static_cast<std::size_t>(pin.node)] >= 0) {
            floorExempt[static_cast<std::size_t>(pin.node)] = 0; // a soft heel must not sink
        }
    }
    // (An arm that HANGS — hangIdleArms — keeps itself out of the BODY with its own stop, and is
    // none of the solve's unknowns: a volume row on one of its joints could only be answered by
    // moving the body away from the arm. A chest pulled sideways on a narrow-stanced rig, its
    // hanging arm brushing the hip, threw the head 10cm in a tick. The FLOOR's rows stay: a body
    // that comes down takes its hanging hands to the floor, and they must stop it there — exempt,
    // the fingers went 4-7cm through it before the hand's contact formed.)
    std::vector<char>& volumeExempt = s.volumeExempt;
    volumeExempt.assign(n, 0);
    // (A hand planted on the floor under the body keeps the torso's rows on its riders: exempting
    // them — the torso lies on the hand, and the capsule cannot know the flesh gives — was tried
    // for the prone hold's hand twitch and dropped, a finger jumping 126mm mid-drag on the base
    // rig once the torso could lie through the hand.)
    for (const HangArm& arm : m_jsHangArms) {
        bool held = false;
        for (const IkEffector& pin : pins) {
            for (int cur = pin.node; cur >= 0 && !held; cur = m_bones[static_cast<std::size_t>(cur)].parent) {
                held = cur == arm.socket;
            }
        }
        if (!held) {
            std::vector<int>& subtree = s.subtreeBuf; // (the scratch's buffer)
            subtree.clear();
            collectSubtree(arm.socket, subtree);
            for (const int b : subtree) {
                volumeExempt[static_cast<std::size_t>(b)] = 1;
                // (While THE SEAT ROLLS the hang keeps the arm off the floor itself — hangIdleArms —
                // and the floor's rows on it, which only the BODY can answer, stand down: a little
                // finger 16mm low, the tick the hang fell behind, shoved the whole figure 3cm up
                // the floor as she was laid down.)
                // (A RELEASED arm — HangArm::released — keeps its rows: the hang keeps it clear of the
                // floor (its avoid probes the real turn, 2026-10-29), and exempted, an arm the lying
                // trunk carried down went 27mm through the floor at a thumb. Under THE PRONE its rows
                // had propped a heavy character's chest into a cobra, rows only the BODY could answer;
                // exempting them there was tried, 2026-11-02, and put the arms 3-8cm through the floor
                // on four rigs: the released arm's bones are the SOLVE's there instead — ikTrunkPolicy's
                // hangArmDof — and its rows bend the arm.)
                if (m_jsSeatRolls) {
                    floorExempt[static_cast<std::size_t>(b)] = 1;
                }
            }
        }
    }
    // (... and so, while the seat rolls, does the HEAD the righting has: its neck is none of the
    // solve's unknowns either, the righting turns it a tick AFTER the solve has answered for where
    // it was, and a skull that reaches the floor as she is laid down closed that loop — the floor's
    // row tipped the body, the tilt changed the righting, the righting moved the skull: a 2-tick
    // cycle under a still cursor, 8-10cm at the brow, on the generation whose head comes down.)
    static const bool kHeadRows = std::getenv("IK_JS_HEAD_FLOOR_ROWS") != nullptr; // A/B probe
    if (m_jsSeatRolls && !kHeadRows) {
        for (const int bone : m_jsHeadFree) {
            std::vector<int>& subtree = s.subtreeBuf; // (the scratch's buffer)
            subtree.clear();
            collectSubtree(bone, subtree);
            for (const int b : subtree) {
                floorExempt[static_cast<std::size_t>(b)] = 1;
            }
        }
    }
    const float floorY = -m_transform[3][1];
    if (!kNoPlanes) {
        // (The exemptions and the ceilings are the scratch's, captured by REFERENCE: copied into the
        // std::function they were four n-vectors allocated per tick; the scratch outlives the solve.)
        problem.planeSource = [this, &rig, &solver, &s, &floorExempt, &floorScale, &floorAllow, &volumeExempt, floorY, root, n, &handCeilings](
                                  const JointSolver::Frames& f, std::vector<JointPlaneTask>& out) {
            const int count = static_cast<int>(n);
            for (const auto& [node, height, share] : handCeilings) {
                JointPlaneTask ceiling;
                ceiling.bone = node;
                ceiling.planePoint = glm::dvec3(0.0, static_cast<double>(height), 0.0);
                ceiling.normal = glm::dvec3(0.0, -1.0, 0.0);
                ceiling.weight = kPlaneWeight * static_cast<double>(share);
                if (kCeilingTrace) {
                    std::fprintf(stderr, "[js-ceiling] %s at %.4f share %.3f: joint y %.4f (%+.1fmm)", m_boneNames[static_cast<std::size_t>(node)].c_str(), height, share,
                                 f.pos[static_cast<std::size_t>(node)].y, (f.pos[static_cast<std::size_t>(node)].y - static_cast<double>(height)) * 1000.0);
                    std::fputc(10, stderr);
                }
                out.push_back(ceiling);
            }
            for (int i = 0; i < count; ++i) {
                const float clearance = rig.floorClearance(i);
                if (clearance <= 0.0f || floorExempt[static_cast<std::size_t>(i)] ||
                    !solver.isAncestorOrSelf(root, i)) {
                    continue;
                }
                // A joint whose clearance is FITTED TO ITS SKIN is stopped by the skin itself: the
                // rows are on its flesh's lowest points, rigid with the bone, so the model knows
                // what turning the bone does to them. The clearance is a constant of the tick —
                // right for a pelvis, which turns a degree a tick under a hard load, and a limit
                // cycle for a HAND: resting lightly on the floor under a forearm coming down, its
                // wrist turned 40 degrees in a tick, its clearance was 3cm other the next, the
                // floor's row threw it back up, and the wrist turned back — solves that ran out of
                // iterations, 12-16cm at the fingertips tick after tick while a sitting figure was
                // laid down on her back. (IK_JS_NO_FLESH_ROWS; IK_JS_DROP_BORROWED)
                if (kDropBorrowed && rig.fleshBorrowed(i)) {
                    continue; // (the neighbour's own rows stop this flesh)
                }
                const std::vector<glm::vec3>& flesh = rig.fleshPoints(i);
                if (!kNoFleshRows && !flesh.empty()) {
                    const std::size_t at = static_cast<std::size_t>(i);
                    if (f.pos[at].y - static_cast<double>(clearance) - 0.30 > static_cast<double>(floorY) + kPlaneMargin) {
                        continue; // (nowhere near: no flesh reaches 30cm further than it does now)
                    }
                    const glm::dmat3& rot = f.rot[at];
                    // The lowest point, and the low point furthest from it across the floor (a
                    // bone lying on the floor rests on two: one row is a pivot).
                    int    lowest = -1;
                    double lowestY = 0.0;
                    for (std::size_t k = 0; k < flesh.size(); ++k) {
                        const double y = f.pos[at].y + (rot * glm::dvec3(flesh[k])).y;
                        if (lowest < 0 || y < lowestY) {
                            lowest = static_cast<int>(k);
                            lowestY = y;
                        }
                    }
                    const double radius = 0.015 * static_cast<double>(rig.sizeScale()); // (the joint itself: never less)
                    const bool   jointLower = f.pos[at].y - radius < lowestY;
                    if (std::min(lowestY, f.pos[at].y - radius) >= static_cast<double>(floorY) + kPlaneMargin) {
                        continue;
                    }
                    if (kPlaneTrace && std::min(lowestY, f.pos[at].y - radius) < static_cast<double>(floorY) + 0.001) {
                        std::fprintf(stderr, "[js-plane] %s's flesh on the floor: %.1fmm under it (joint %.1fmm up)\n",
                                     m_boneNames[at].c_str(), (static_cast<double>(floorY) - std::min(lowestY, f.pos[at].y - radius)) * 1000.0,
                                     (f.pos[at].y - static_cast<double>(floorY)) * 1000.0);
                    }
                    JointPlaneTask row;
                    row.bone = i;
                    row.normal = glm::dvec3(0.0, 1.0, 0.0);
                    row.weight = kPlaneWeight * static_cast<double>(floorScale[at]);
                    if (jointLower) {
                        row.planePoint = glm::dvec3(0.0, static_cast<double>(floorY) + radius, 0.0);
                        out.push_back(row);
                        continue;
                    }
                    row.planePoint = glm::dvec3(0.0, static_cast<double>(floorY), 0.0);
                    // The lowest point, then — lowest first — the low points that lie well away
                    // from every one taken (a bone resting on the floor rests on several: one row
                    // is a pivot, and the lowest ALONE of a pelvis's two buttocks is whichever a
                    // rounding error says, a symmetric sit solved askew).
                    const double apart = 0.04 * static_cast<double>(rig.sizeScale());
                    std::vector<std::pair<double, int>>& low = s.fleshLow;
                    low.clear();
                    for (std::size_t k = 0; k < flesh.size(); ++k) {
                        const double y = f.pos[at].y + (rot * glm::dvec3(flesh[k])).y;
                        if (y < static_cast<double>(floorY) + kPlaneMargin) {
                            low.emplace_back(y, static_cast<int>(k));
                        }
                    }
                    std::sort(low.begin(), low.end());
                    // (On a CENTRE bone a point brings its MIRROR TWIN with it, if that is low too:
                    // four taken lowest-first from a symmetric set are left, right, a midline point
                    // and then a LEFT one whose twin no longer fits, and pressed hard into the floor
                    // the odd row out is a roll: IK_JS_FLESH_NO_TWINS is the A/B.)
                    const bool centreBone = !kNoTwins && std::abs(m_bones[at].inverseBind[3][0]) < 0.01f * rig.sizeScale();
                    std::vector<glm::dvec3>& taken = s.fleshTaken;
                    std::vector<int>&        takenIndex = s.fleshTakenIndex;
                    taken.clear();
                    takenIndex.clear();
                    const auto take = [&](int index, const glm::dvec3& world) {
                        taken.push_back(world);
                        takenIndex.push_back(index);
                        row.offset = glm::dvec3(flesh[static_cast<std::size_t>(index)]);
                        out.push_back(row);
                    };
                    const auto clearOf = [&](const glm::dvec3& world) {
                        for (const glm::dvec3& t : taken) {
                            if (std::hypot(world.x - t.x, world.z - t.z) < apart) {
                                return false;
                            }
                        }
                        return true;
                    };
                    for (const auto& candidate : low) {
                        const glm::vec3& local = flesh[static_cast<std::size_t>(candidate.second)];
                        const glm::dvec3 w = rot * glm::dvec3(local);
                        if (std::find(takenIndex.begin(), takenIndex.end(), candidate.second) != takenIndex.end() || !clearOf(w)) {
                            continue;
                        }
                        take(candidate.second, w);
                        if (centreBone && std::abs(static_cast<double>(local.x)) > 0.5 * apart) {
                            const glm::vec3 mirrored(-local.x, local.y, local.z);
                            int             twin = -1;
                            float           nearest = 0.01f * rig.sizeScale();
                            for (const auto& other : low) {
                                const float d = glm::length(flesh[static_cast<std::size_t>(other.second)] - mirrored);
                                if (other.second != candidate.second && d < nearest) {
                                    nearest = d;
                                    twin = other.second;
                                }
                            }
                            if (twin >= 0 && std::find(takenIndex.begin(), takenIndex.end(), twin) == takenIndex.end()) {
                                const glm::dvec3 twinWorld = rot * glm::dvec3(flesh[static_cast<std::size_t>(twin)]);
                                if (clearOf(twinWorld)) {
                                    take(twin, twinWorld);
                                }
                            }
                        }
                        if (taken.size() >= 4) {
                            break;
                        }
                    }
                    continue;
                }
                const double level = static_cast<double>(floorY) + clearance - static_cast<double>(floorAllow[static_cast<std::size_t>(i)]);
                if (f.pos[static_cast<std::size_t>(i)].y < level + kPlaneMargin) {
                    if (kPlaneTrace && f.pos[static_cast<std::size_t>(i)].y < level + 0.001) {
                        std::fprintf(stderr, "[js-plane] %s on the floor: %.1fmm under its clearance (%.1fmm; y %.4f floor %.4f)\n",
                                     m_boneNames[static_cast<std::size_t>(i)].c_str(),
                                     (level - f.pos[static_cast<std::size_t>(i)].y) * 1000.0, clearance * 1000.0f, f.pos[static_cast<std::size_t>(i)].y, static_cast<double>(floorY));
                    }
                    JointPlaneTask row;
                    row.bone = i;
                    row.planePoint = glm::dvec3(0.0, level, 0.0);
                    row.normal = glm::dvec3(0.0, 1.0, 0.0);
                    row.weight = kPlaneWeight * static_cast<double>(floorScale[static_cast<std::size_t>(i)]);
                    out.push_back(row);
                }
            }
            const auto pos = [&f](int i) { return glm::vec3(f.pos[static_cast<std::size_t>(i)]); };
            for (const BodyVolume& v : rig.dragVolumes()) {
                const VolumeAxis ax = volumeAxis(v, count, pos);
                if (!ax.ok) {
                    continue;
                }
                const std::size_t ref = static_cast<std::size_t>(v.a);
                const glm::dmat3 refInv = glm::transpose(f.rot[ref]);
                const auto emit = [&](int bone, const glm::dvec3& offset, const glm::vec3& point,
                                      const glm::vec3& onAxis, float radius, double share = 1.0) {
                    const glm::vec3 away = point - onAxis;
                    const float dist = glm::length(away);
                    if (dist < 1e-5f || radius - dist < -static_cast<float>(kPlaneMargin)) {
                        return; // well clear (or degenerate: on the axis, no normal to push along)
                    }
                    if (kPlaneTrace && radius - dist > -0.001f) {
                        std::fprintf(stderr, "[js-plane] %s against %s-%s: %.1fmm inside\n",
                                     m_boneNames[static_cast<std::size_t>(bone)].c_str(),
                                     m_boneNames[static_cast<std::size_t>(v.a)].c_str(),
                                     v.b >= 0 ? m_boneNames[static_cast<std::size_t>(v.b)].c_str() : "-",
                                     (radius - dist) * 1000.0f);
                    }
                    const glm::dvec3 nrm(away / dist);
                    const glm::dvec3 surface = glm::dvec3(onAxis) + nrm * static_cast<double>(radius);
                    JointPlaneTask row;
                    row.bone = bone;
                    row.offset = offset;
                    row.refBone = v.a;
                    row.planePoint = refInv * (surface - f.pos[ref]);
                    row.normal = refInv * nrm;
                    row.weight = kPlaneWeight * share * share; // (the share is the DEPTH's: the row's residual goes as it)
                    out.push_back(row);
                };
                for (int i = 0; i < count; ++i) {
                    const char applies = v.applies[static_cast<std::size_t>(i)];
                    if (!applies || volumeExempt[static_cast<std::size_t>(i)]) {
                        continue; // (a hanging arm's joint — see above)
                    }
                    const glm::vec3 P = pos(i);
                    const float t = volumeAxisParam(ax, P);
                    const float clearance =
                        applies == kVolumeAppliesRider ? rig.riderClearance() : rig.volumeClearance(i);
                    const float segR = rig.volumeSegmentRadius(i);
                    const int par = m_bones[static_cast<std::size_t>(i)].parent;
                    // (A joint that ENDS a fleshy segment is as wide as the segment's end — the knee
                    // is the thigh's cap: its own row keeps the segment's flesh margin, not the bare
                    // joint clearance. The segment row below stands down within 2% of its ends, and
                    // held there at 1.5cm the model's margin dropped 2cm the moment two crossed
                    // thighs touched at the knee: on three characters with fuller thighs the legs
                    // crossed with the knees 13-14mm inside each other by the harness's measure,
                    // which counts the segment's flesh right to its end. IK_JS_BARE_JOINT_CAPS)
                    const bool capped = !kBareCaps && applies != kVolumeAppliesRider && volumeAppliesSegment(v, applies) &&
                                        segR > 0.0f && par >= 0;
                    emit(i, glm::dvec3(0.0), P, ax.A + ax.ab * t,
                         volumeRadiusAt(v, t) + (capped ? std::max(kVolumeSegmentFlesh * segR, clearance) : clearance));
                    // The SEGMENT into the joint (two limbs cross at their middles): its closest
                    // point rides the parent bone.
                    if (applies != kVolumeAppliesRider && volumeAppliesSegment(v, applies) &&
                        segR > 0.0f && par >= 0) {
                        float sPar = 0.0f;
                        float tPar = 0.0f;
                        closestSegmentPoints(pos(par), P, ax.A, ax.B, sPar, tPar);
                        if (sPar > 0.02f && sPar < 0.98f) { // the ends are the joint tests'
                            // (... and the row COMES IN from each end over kSegmentRowRamp of the
                            // segment, 2026-09-28: cut in at full strength 2% along, it made the
                            // cost of a pose DISCONTINUOUS — an upper arm lying against the chest
                            // with its closest point at the socket, whose joint the chest has no
                            // row for, cost nothing, and a hair further along 19 (1.4mm through at
                            // 1e7). Judged by its own rows (JointSolver: a trial is judged by the
                            // rows it makes) no step across that line is downhill, however small:
                            // the male rig's prone descent stood still for twelve ticks under a
                            // leaving cursor and let go 13cm at once. IK_JS_SEGMENT_ROWS_CUT.)
                            const double ramp = kSegmentRowsCut
                                                    ? 1.0
                                                    : static_cast<double>(glm::smoothstep(0.02f, kSegmentRowRamp, sPar) *
                                                                          (1.0f - glm::smoothstep(1.0f - kSegmentRowRamp, 0.98f, sPar)));
                            const glm::vec3 onSeg = glm::mix(pos(par), P, sPar);
                            const std::size_t pb = static_cast<std::size_t>(par);
                            emit(par, glm::transpose(f.rot[pb]) * (glm::dvec3(onSeg) - f.pos[pb]), onSeg,
                                 ax.A + ax.ab * tPar,
                                 volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, clearance), ramp);
                        }
                    }
                }
            }
        };
    }
}

} // namespace pose
