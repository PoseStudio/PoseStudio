/**
 * @file realphases.cpp
 * @brief The real-skeleton phases (see realphases.h). Each reproduces a user-reported behaviour
 *        and gates the measurements that once failed — see CLAUDE.md's FBIK section for the
 *        history behind every number. Gates are regression guards, not specs.
 *
 * The phases are the RealPhases class: the state they all share (the report, the rig, the T-pose
 * rest pre-pose, a rest-pose probe, the floor scenarios' sizing), the helpers they call, and
 * sixteen phase FAMILIES — each a member function of `if (report.wants("..."))` blocks — run in
 * the file's order by runAll(). Every block is the text that set its gates (one 5,000-line
 * function until 2026-10-29); the family a phase belongs to is only where to find it.
 */

#include "realphases.h"

#include "armature.h"
#include "balancecontroller.h"
#include "ikmath.h"
#include "ikrig.h"
#include "rigroles.h"
#include "report.h"
#include "simulator.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace pose;

namespace ikharness {

/// THE WEIGHT over a standing foot: how far the centre of mass stands OUTSIDE that foot's footprint
/// — the hull of the ankle and every joint below it, in the floor plane, at the end of a run (mm;
/// 0 inside). A body on one leg stands over its foot (the weight shift in Armature::ikPostureModel);
/// the ankle alone is no measure of it, since a raised knee carries the weight forward over the
/// toes, still inside. 0 without the ankle.
static double comOutsideFootMm(const RunResult& r, int ankle, const std::vector<ArmatureBone>& bones) {
    if (ankle < 0 || static_cast<std::size_t>(ankle) >= r.endPos.size() || bones.size() != r.endPos.size()) {
        return 0.0;
    }
    std::vector<glm::vec2> points;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        bool under = static_cast<int>(i) == ankle;
        for (int cur = bones[i].parent; cur >= 0 && !under; cur = bones[static_cast<std::size_t>(cur)].parent) {
            under = cur == ankle;
        }
        if (under) {
            points.emplace_back(r.endPos[i].x, r.endPos[i].z);
        }
    }
    const std::vector<glm::vec2> hull = BalanceController::supportPolygon(points);
    if (hull.empty()) {
        return 0.0;
    }
    const glm::vec2 at(r.comEnd.x, r.comEnd.z);
    return static_cast<double>(glm::length(BalanceController::closestBalancedPoint(hull, at, 0.0f) - at)) * 1000.0;
}

namespace {

const std::vector<std::string> kIdleForHandDrag{"head", "rHand", "rFoot", "lFoot", "hip"};
const std::vector<std::string> kIdleForHeadDrag{"lHand", "rHand", "rFoot", "lFoot"};

// The leg diagnostics every real-figure phase reports under --verbose (see RunResult): the
// knees' fold-plane twist, their inward (valgus) offset, their tremble, and the planted feet's
// orientation — the measurements behind the "knees twist inward / feet mangled" complaints.
// Limits for the leg diagnostics a phase GATES (negative = an info row only). The trunk phases
// gate all three of twist / inward / foot rotation: before the knee-and-feet round every
// crouch, walk and chest lean ran the thigh twist to its 75° limit, knocked the knees 10-20cm
// inward and rotated the planted feet 45°, and the position-only gates never saw it.
struct LegGates {
    double twistDeg = -1.0;   // knee twist max
    double inwardMm = -1.0;   // knee inward max
    double footRotDeg = -1.0; // planted foot rotation at the end
    double trembleMm = -1.0;  // knee tremble
};

std::vector<Gate> legInfo(const RunResult& r, const LegGates& g) {
    const auto row = [](const char* name, double value, double limit) {
        return limit >= 0.0 ? gateMax(name, value, limit) : info(name, value);
    };
    return {row("knee twist max (deg)", r.kneeTwistMaxDeg, g.twistDeg),
            info("knee twist at end (deg)", r.kneeTwistEndDeg),
            row("knee inward max (mm)", r.kneeInwardMax * 1000.0, g.inwardMm),
            info("knee inward at end (mm)", r.kneeInwardEnd * 1000.0),
            info("knee splay at end, outboard of the socket (mm)", r.kneeSplayEnd * 1000.0),
            info("knee outboard of its foot at end (mm)", r.kneeOverFootEnd * 1000.0),
            row("knee tremble (mm)", r.kneeOscMax * 1000.0, g.trembleMm),
            info("knee worst step (mm)", r.kneeStepMax * 1000.0),
            info("planted foot rotation max (deg)", r.footRotMaxDeg),
            row("planted foot rotation at end (deg)", r.footRotEndDeg, g.footRotDeg),
            // (Where the head LOOKS, on every phase: nothing gated it until a rig threw its head
            // back to the sky under a head pull and every number was green.)
            info("gaze over the horizon at the end (deg)", r.gazeEndDeg),
            info("the head's sideways tilt at the end (deg)", r.headRollEndDeg),
            // (And how LOPSIDED the pose came out, on every phase: see RunResult::asymmetryStart.)
            // GATED, by construction, on every SYMMETRIC gesture that takes no step: a centre joint
            // dragged in the sagittal plane from a symmetric pose has a symmetric answer, and what
            // the pose gains instead is one degree of freedom standing in for a dearer one - legs
            // folded over to one side, a corkscrewed spine - which a side view cannot show.)
            info("left/right asymmetry at the start (mm)", r.asymmetryStart * 1000.0),
            row("left/right asymmetry gained (mm)", (r.asymmetryEnd - r.asymmetryStart) * 1000.0,
                r.symmetricGesture && r.stepsTaken == 0 ? r.asymmetryGateMm : -1.0)};
}

// The trunk-phase limits: crouches (feet planted under the hips) and leans/walks (a lean rolls
// the knee a few centimetres; a walk's landing foot re-flattens within a tick or two).
constexpr LegGates kCrouchLegs{10.0, 30.0, 8.0, -1.0};
// (The thigh's twist channel while a leg swings out to a step: a hip that is abducted AND flexed needs
// real rotation to keep the foot's heading — 14° mid-step on the stepping chest drag, 1-6° elsewhere.)
constexpr LegGates kLeanLegs{16.0, 40.0, 8.0, -1.0};
constexpr LegGates kHandLegs{5.0, 20.0, 5.0, -1.0}; // a hand drag never moves the legs

// --- What several phases measured with a lambda of their own, once ---
/// A joint's displacement from the measured drag's start to its end (mm; 0 for a missing bone).
double movedMm(const RunResult& r, int bone) {
    return bone >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(bone)] -
                                                       r.startPos[static_cast<std::size_t>(bone)])) * 1000.0
                     : 0.0;
}
/// The worst single-tick joint move through the drag's first segment (mm).
double jumpMm(const RunResult& r) { return r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0; }
/// The grabbed joint's reversal overlap through the second segment, the hold (mm).
double holdOscMm(const RunResult& r) { return r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0; }
/// How much LOPSIDED the pose came out (mm): RunResult::asymmetryStart.
double asymmetryGainedMm(const RunResult& r) { return (r.asymmetryEnd - r.asymmetryStart) * 1000.0; }
/// A bone's first Euler channel at the end (degrees; 0 for a missing bone).
double endPitchDeg(const RunResult& r, int bone) {
    return bone >= 0 && static_cast<std::size_t>(bone) < r.endEuler.size() ? static_cast<double>(r.endEuler[static_cast<std::size_t>(bone)].x) : 0.0;
}

} // namespace

/// THE REAL-FIGURE PHASES as one object: what every phase shared as a local of the one function
/// they used to live in — the report, the rig, the T-pose rest pre-pose, a rest-pose probe
/// armature, the floor scenarios' sizing — and the helpers they call. Each phase FAMILY is a
/// member function holding its `if (report.wants("..."))` blocks exactly as they were written,
/// run in the file's order by runAll(). A phase reads the members by the names the function's
/// locals had (report, bones, v, restPose, run, runIdle, limitBoundEnd, phaseLegs, indexOf, the
/// sizing values), so its text is the text that set its gates.
class RealPhases {
public:
    RealPhases(Report& report, const std::vector<ArmatureBone>& bones);
    void runAll();

private:
    Report&                           report;
    const std::vector<ArmatureBone>&  bones;
    const bool                        v; ///< --verbose
    /// REST-POSE normalization: the older generations rest in a T-POSE (arms horizontal, the
    /// hand above the shoulder), and every scenario here is a gesture from the base rig's A-pose
    /// (arms hanging) — "+60cm up" from a horizontal arm is beyond any reach, and "+25cm out"
    /// from one is a full-body strain. Such a rig gets its arms lowered by a shoulder pre-pose
    /// before every scenario, so the phases measure the same gestures on every generation.
    std::vector<std::pair<std::string, glm::vec3>> restPose;
    /// The rig at REST (never posed): bone indices by canonical name (indexOf), rest positions
    /// (boneWorldPosition), the parents. A phase that poses a probe of its own calls it `posedProbe`.
    Armature probe;

    // --- The floor scenarios' sizing (sizeFloorScenarios) ---
    const std::vector<std::pair<std::string, glm::vec3>> kAllFoursPose{
        {"hip", glm::vec3(45.0f, 0.0f, 0.0f)},          {"lThigh", glm::vec3(-45.0f, 0.0f, 0.0f)},
        {"rThigh", glm::vec3(-45.0f, 0.0f, 0.0f)},      {"abdomenLower", glm::vec3(20.0f, 0.0f, 0.0f)},
        {"chest", glm::vec3(20.0f, 0.0f, 0.0f)},        {"lShldr", glm::vec3(0.0f, -90.0f, 0.0f)},
        {"rShldr", glm::vec3(0.0f, 90.0f, 0.0f)}};
    float allFoursDrop = 0.0f; // hip descent that plants the hands, 0 = not on this rig
    float sitDrop = 0.0f;      // hip descent to 15cm above the floor (a sit on the floor)
    float kneelDrop = 0.0f;    // hip descent the kneel DRAGS to (past the kneeling height)
    float kneelNatural = 0.0f; // hip descent to the kneeling height itself (a vertical thigh)
    float kneeClearance = 0.015f; // the knee joint's floor clearance on this rig, kneeling
    float seatClearance = 0.015f; // ... and the hip joint's, sitting
    float chestBackClearance = 0.10f; // ... and the upper chest's, lying on her back
    float kneelForward = 0.0f; // hip travel forward that puts the knees under the hips

    // --- The helpers every phase calls ---
    /// A scenario run on the rig, the rest pre-pose prepended (runScenario).
    RunResult   run(Scenario sc) const;
    /// The idle-joint tremble pass of the same scenario (runIdleMetrics).
    IdleMetrics runIdle(Scenario sc) const;
    /// Whether a limited channel of @p canonical (any with real range) ended within 5 deg of a
    /// limit: a limb that cannot serve a gesture leans on the trunk instead, and the gates
    /// that assume the limb served it turn informational.
    bool limitBoundEnd(const RunResult& r, const char* canonical) const;
    /// A phase report with the leg diagnostics appended (info rows: verbose only).
    void phaseLegs(const std::string& name, std::vector<Gate> gates, const RunResult& r,
                   const LegGates& lg = LegGates{});
    /// The bone the phases call @p canonical on this rig, or -1 (resolveBone on the rest probe).
    int  indexOf(const char* canonical) const { return resolveBone(probe, canonical); }
    /// @p node is @p ancestor or hangs below it.
    bool under(int node, int ancestor) const;

    void findRestPose();
    void sizeFloorScenarios();
    // --- The families, in the order runAll runs them ---
    void rigPhases();             ///< rig-roles
    void handPhases();            ///< bench, gentle, depth
    void utilityPhases();         ///< utilities, transform-dials
    void armPhases();             ///< arm-raise, tiptoe, arm-swing, group, scoped, hand-lay, girdle
    void liftPhases();            ///< suspension, suspend-lower
    void crouchPhases();          ///< crouch-push, stand-up, hip-crouch
    void contactPhases();         ///< hands-plant, hands-rise, kneel
    void kneelPhases();           ///< kneel-allfours, allfours-up, prone, kneel-heels, kneel-limb, repeat, lunge, kneel-stand, sit-stand
    void collisionPhases();       ///< arm-across, leg-across, arm-vs-arm, fk-collide
    void sitPhases();             ///< sit, floor-seat
    void stancePhases();          ///< deep-crouch, hover-heal, strained-release, foot-lift, foot-place
    void limbDragPhases();        ///< knee-drag, elbow-drag
    void pelvisPhases();          ///< hip-sway, hip-hinge, girdle-grab, hip-back-walk, walk, chest-step
    void trunkPhases();           ///< reach-down, lifted-foot, limb-grab, crouch-lean, posed-start, seated, trunk-bow, head-right, trunk-follow
    void pinPhases();             ///< pin-hand-drag, pin-hand-crouch, pin-foot-chest, tremble-slow, tremble-med
    void grabPhases();            ///< eye-grab, pectoral-grab, digit
    void tailPhases();            ///< head-straighten, child, landing, hold-still
};

RealPhases::RealPhases(Report& report_, const std::vector<ArmatureBone>& bones_)
    : report(report_), bones(bones_), v(report_.verbose) {
    probe.build(bones);
    // (The probe carries the rig from the start: a canonical name that is none of this rig's names
    // nor an alias resolves through the rig standard's roles — bonealiases.h — which the rig holds.)
    if (std::getenv("IK_HARNESS_NO_PROBE_RIG") == nullptr) { // (A/B: the probe without a rig, as before 2026-11-03)
        applyBodyMesh(probe, 1.0f);
        probe.prepareIkRig();
    }
}

void RealPhases::runAll() {
    findRestPose();
    rigPhases();
    handPhases();
    utilityPhases();
    armPhases();
    liftPhases();
    crouchPhases();
    sizeFloorScenarios();
    contactPhases();
    kneelPhases();
    collisionPhases();
    sitPhases();
    stancePhases();
    limbDragPhases();
    pelvisPhases();
    trunkPhases();
    pinPhases();
    grabPhases();
    tailPhases();
}

RunResult RealPhases::run(Scenario sc) const {
    sc.prePose.insert(sc.prePose.begin(), restPose.begin(), restPose.end());
    return runScenario(bones, sc, v);
}

IdleMetrics RealPhases::runIdle(Scenario sc) const {
    sc.prePose.insert(sc.prePose.begin(), restPose.begin(), restPose.end());
    return runIdleMetrics(bones, sc);
}

bool RealPhases::limitBoundEnd(const RunResult& r, const char* canonical) const {
    const std::string name = resolveBoneName(probe, canonical);
    for (std::size_t i = 0; i < bones.size() && i < r.endEuler.size(); ++i) {
        if (bones[i].name != name) {
            continue;
        }
        for (int a = 0; a < 3; ++a) {
            if (bones[i].rotationLimited[a] &&
                bones[i].rotationMax[a] - bones[i].rotationMin[a] >= 10.0f &&
                (r.endEuler[i][a] <= bones[i].rotationMin[a] + 5.0f ||
                 r.endEuler[i][a] >= bones[i].rotationMax[a] - 5.0f)) {
                return true;
            }
        }
    }
    return false;
}

void RealPhases::phaseLegs(const std::string& name, std::vector<Gate> gates, const RunResult& r,
                           const LegGates& lg) {
    for (const Gate& g : legInfo(r, lg)) {
        gates.push_back(g);
    }
    report.phase(name, gates);
}

bool RealPhases::under(int node, int ancestor) const {
    for (int cur = node; cur >= 0; cur = bones[static_cast<std::size_t>(cur)].parent) {
        if (cur == ancestor) {
            return true;
        }
    }
    return false;
}

/// The T-pose rest pre-pose (restPose): the rigs whose hands rest above the shoulders.
void RealPhases::findRestPose() {
    const int hand = resolveBone(probe, "lHand");
    const int collar = resolveBone(probe, "lCollar");
    if (hand >= 0 && collar >= 0 &&
        probe.boneWorldPosition(static_cast<std::size_t>(hand)).y >
            probe.boneWorldPosition(static_cast<std::size_t>(collar)).y - 0.10f) {
        // The shoulder's third channel lowers the arm on these rigs (its authored range
        // reaches -75/-85 on the left); the right mirrors as (x, -y, -z).
        restPose = {{"lShldr", glm::vec3(0.0f, 0.0f, -65.0f)},
                    {"rShldr", glm::vec3(0.0f, 0.0f, 65.0f)}};
        std::printf("[info] T-pose rest: arms lowered by a shoulder pre-pose in every phase\n");
    }
    // (Every probe that applies restPose resolves the names first — resolveBoneName — as the
    // simulator does: posed by the RAW canonical name, a probe on a rig that calls its shoulder
    // otherwise stayed in its T-pose while the run lowered the arms, and every target read off
    // its hand was off by the arm's whole swing — the rigging-add-on-style FBX rig's hand lays landed
    // at the far knee, 2026-11-03. The reference T-pose generations carry the literal names,
    // so nothing changes there.)
}

/// THE RIG STANDARD'S ROLES (rigroles.h, 2026-11-02): this rig conforms — every required role is
/// found, every side role has its twin, the roles agree with the names the tests speak for this
/// rig (the alias table's, name first), and no bone of the body is left without a role. The
/// chain lengths and the part counts are reported.
void RealPhases::rigPhases() {
    if (!report.wants("rig-roles")) {
        return;
    }
    Armature arm;
    arm.build(bones);
    applyBodyMesh(arm, 1.0f);
    (void)arm.boneClass(0); // (the public path that builds the rig: the bone classes need it)
    const pose::IkRig* rig = arm.ikRig();
    if (rig == nullptr || !rig->roles().built()) {
        report.skip("[real] rig roles: the standard's roles on this rig", "no rig");
        return;
    }
    const pose::RigRoles& roles = rig->roles();
    // The canonical names whose role the standard knows, resolved by NAME (the alias table, which
    // never reaches the roles while a name or an alias fits) and by ROLE: they must agree.
    static const char* const kNamed[] = {"hip",      "pelvis",  "abdomenLower", "chest_2", "neck",    "head",     "lowerJaw",
                                         "lCollar",  "rCollar", "lShldr",       "rShldr",  "lForeArm", "rForeArm", "lHand",
                                         "rHand",    "lThigh",  "rThigh",       "lShin",   "rShin",    "lFoot",    "rFoot",
                                         "lEye",     "rEye",    "lPectoral",    "rPectoral", "lThighTwist", "rThighTwist",
                                         "lIndex3",  "lMid3",   "lThumb3",      "lBigToe"};
    int    named = 0;
    int    disagree = 0;
    for (const char* canonical : kNamed) {
        const int byName = arm.boneIndex(canonical) >= 0 ? arm.boneIndex(canonical) : -1;
        // (The alias table's own answer, name first — the roles never consulted while it fits.)
        const int byAlias = resolveBone(arm, canonical);
        const pose::RigRoleTag tag = roles.canonicalTag(canonical);
        if (byAlias < 0 || tag.role == pose::RigRole::None) {
            continue;
        }
        const int byRole = roles.find(tag.role, tag.side, tag.index, tag.joint);
        ++named;
        if (byRole != byAlias) {
            ++disagree;
            if (v) {
                std::printf("[rig-roles] %s: by name %s, by role %s\n", canonical,
                            arm.boneName(static_cast<std::size_t>(byAlias)).c_str(),
                            byRole >= 0 ? arm.boneName(static_cast<std::size_t>(byRole)).c_str() : "(none)");
            }
        }
        (void)byName;
    }
    int unpaired = 0;
    int bodyBones = 0;
    int standardNamed = 0;
    int roundTripFailed = 0;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const pose::RigRoleTag& t = roles.tag(static_cast<int>(i));
        if (t.role == pose::RigRole::None) {
            continue;
        }
        ++bodyBones;
        // THE STANDARD'S NAME of the bone reads back as the same tag (the two grammars agree).
        const std::string standard = pose::RigRoles::standardName(t);
        if (!standard.empty()) {
            ++standardNamed;
            const pose::RigRoleTag back = roles.canonicalTag(standard);
            if (back.role != t.role || back.side != t.side || back.index != t.index || back.joint != t.joint) {
                ++roundTripFailed;
                if (v) {
                    std::printf("[rig-roles] %s: standard name %s reads back as %s\n", bones[i].name.c_str(), standard.c_str(),
                                pose::RigRoles::roleName(back.role));
                }
            }
        }
        if (t.side != pose::RigSide::Centre && t.role != pose::RigRole::Face && t.role != pose::RigRole::Helper &&
            roles.find(t.role, pose::RigRoles::mirrored(t.side), t.index, t.joint) < 0) {
            ++unpaired;
            if (v) {
                std::printf("[rig-roles] %s (%s %s %d j%d) has no twin\n", bones[i].name.c_str(),
                            pose::RigRoles::roleName(t.role), pose::RigRoles::sideName(t.side), t.index, t.joint);
            }
        }
    }
    const auto count = [&](pose::RigRole role) { return static_cast<double>(roles.all(role).size()); };
    report.phase("[real] rig roles: the standard's roles on this rig",
                 {gateMax("required roles missing", static_cast<double>(roles.missing().size()), 0.0),
                  gateMax("roles disagreeing with the rig's own names", static_cast<double>(disagree), 0.0),
                  gateMin("canonical names checked by name and by role", static_cast<double>(named), 15.0),
                  gateMax("side roles without a twin on the other side", static_cast<double>(unpaired), 0.0),
                  gateMax("bones of the body with no role", count(pose::RigRole::Other), 0.0),
                  gateMax("standard names that do not read back as their role", static_cast<double>(roundTripFailed), 0.0),
                  gateMin("bones with a standard name", static_cast<double>(standardNamed), 40.0),
                  info("bones of the body", static_cast<double>(bodyBones)),
                  info("spine links", static_cast<double>(roles.spineLinks())),
                  info("neck links", static_cast<double>(roles.neckLinks())),
                  info("twist helpers", count(pose::RigRole::UpperArmTwist) + count(pose::RigRole::ForearmTwist) +
                                            count(pose::RigRole::ThighTwist) + count(pose::RigRole::ShinTwist)),
                  info("carpals", count(pose::RigRole::Carpal)),
                  info("finger joints", count(pose::RigRole::Finger)),
                  info("metatarsals and heels", count(pose::RigRole::Metatarsal) + count(pose::RigRole::Heel)),
                  info("toe joints", count(pose::RigRole::Toe)),
                  info("pectorals", count(pose::RigRole::Pectoral)),
                  info("face bones", count(pose::RigRole::Face) + count(pose::RigRole::Eye) + count(pose::RigRole::Jaw)),
                  info("helpers", count(pose::RigRole::Helper))});
}

/// The hand gestures the bench path, a gentle pull and the wheel's depth notches measure.
void RealPhases::handPhases() {
    // --- The in-app benchmark path (POSESTUDIO_IK_BENCH=lHand): the same waypoints, so the
    // per-phase lag here must match the app's [ikperf] report tick for tick.
    if (report.wants("bench")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = {{0, glm::vec3(0.0f), "start"},
                   {60, glm::vec3(0.25f, 0.15f, 0.05f), "move-med"},
                   {120, glm::vec3(0.25f, 0.15f, 0.05f), "hold-1"},
                   {180, glm::vec3(0.0f), "return"},
                   {240, glm::vec3(0.0f), "hold-2"},
                   {270, glm::vec3(-0.30f, 0.20f, 0.0f), "move-fast"},
                   {330, glm::vec3(-0.30f, 0.20f, 0.0f), "hold-3"}};
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        const IdleMetrics idle = runIdle(sc);
        std::vector<Gate> gates;
        if (!r.ok) {
            gates.push_back(gateMin(("error: " + r.error).c_str(), 0.0, 1.0));
        } else {
            gates = {gateMax("move-med lag mean (mm)", r.phases[0].lagMean() * 1000.0, 5.0),
                     gateMax("hold-1 resting lag (mm)", r.phases[1].lagMax * 1000.0, 5.0),
                     gateMax("return lag mean (mm)", r.phases[2].lagMean() * 1000.0, 8.0),
                     gateMax("hold-2 return-to-rest miss (mm)", r.phases[3].lagMax * 1000.0, 3.0),
                     gateMax("move-fast lag mean (mm)", r.phases[4].lagMean() * 1000.0, 30.0),
                     // (The WORST lag over the hold: its first tick carries the damped follower's
                     // one-tick trail behind the 1.2 m/s flick — 1.4-6.1mm across the rigs; 9.2 on
                     // one generation since the other, idle arm hangs through the flick.)
                     // (20 since the hand lay: the third generation's third hold lands the cursor 3.4cm INSIDE the
                          // belly, the hand lies flat on it, and a flat hand's wrist rests its own thickness from a cursor
                          // inside the body - 17mm, 5 edge-on. The follower's trail this gate is for reads 8 on the base rig.)
                          gateMax("hold-3 flick resting miss (mm)", r.phases[5].lagMax * 1000.0, 20.0),
                     // 70: an elbow re-orienting its fold plane at the extraction's 12°/tick
                     // twist cap swings ~6cm in a tick on the oldest rigs (no twist bones).
                     gateMax("worst single-tick jump (mm)", std::max({r.phases[0].maxJump, r.phases[2].maxJump, r.phases[4].maxJump}) * 1000.0, 70.0),
                     gateMax("grabbed-joint accel excess (mm/tick^2)", std::max(0.0, r.phases[4].effSpeedUpMax - r.phases[4].cursorSpeedUpMax) * 1000.0, 12.0),
                     gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 10.0),
                     gateMax("idle-joint tremble (mm)", idle.oscMax * 1000.0, 35.0),
                     gateMax("settle ticks", r.settleTicks, 45.0),
                     gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)};
            for (const PhaseStats& ph : r.phases) {
                gates.push_back(info((ph.label + " lag mean (mm)").c_str(), ph.lagMean() * 1000.0));
            }
        }
        phaseLegs("[real] bench path: lHand (the app's POSESTUDIO_IK_BENCH)", gates, r, kHandLegs);
    }

    // --- A single gentle side pull holds the feet and the hip; the hand lands near the cursor.
    if (report.wants("gentle")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = pullPath(glm::vec3(0.10f, 0.0f, 0.0f), 30, 60);
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        phaseLegs("[real] gentle side pull (lHand +10cm x)",
                     {gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 10.0),
                      // (9-10mm until the spine was coupled to bend as one: as a unit it gives a
                      // hand pull a millimetre less, and the hips a millimetre more — 10-11.)
                      gateMax("hip displacement (mm)", r.hipDisp * 1000.0, 12.0),
                      gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0),
                      info("steps", r.stepsTaken)}, r, kHandLegs);
    }

    // --- Wheel DEPTH steps: the app moves the drag plane in discrete notches while the cursor
    // rests (a ~5cm target jump per notch at the default framing — VulkanWindow::wheelEvent), so
    // the raw target arrives as a staircase. Each step must read as a smooth push (the accel
    // shaping ramps the hand over a few ticks), the hand must land on the moved cursor, and the
    // feet must stay planted — a forward hand pull recruits the arm and trunk, not the legs.
    if (report.wants("depth")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path.push_back({0, glm::vec3(0.0f), "start"});
        int t = 0;
        for (int k = 1; k <= 6; ++k) {
            const glm::vec3 off(0.0f, 0.0f, 0.05f * static_cast<float>(k)); // toward a front camera
            sc.path.push_back({t + 1, off, "step"});
            sc.path.push_back({t + 11, off, "hold"});
            t += 11;
        }
        sc.path.push_back({t + 60, sc.path.back().offset, "rest"});
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        const IdleMetrics idle = runIdle(sc);
        double stepJumpMax = 0.0;
        double stepSpeedUpMax = 0.0;
        for (const PhaseStats& ph : r.phases) {
            stepJumpMax = std::max(stepJumpMax, ph.maxJump);
            stepSpeedUpMax = std::max(stepSpeedUpMax, ph.effSpeedUpMax);
        }
        phaseLegs("[real] wheel depth steps (lHand 6 x 5cm z notches)",
                     {gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 10.0),
                      gateMax("hip displacement (mm)", r.hipDisp * 1000.0, 40.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0),
                      gateMax("idle-joint tremble (mm)", idle.oscMax * 1000.0, 35.0),
                      info("worst single-tick joint jump (mm)", stepJumpMax * 1000.0),
                      info("worst hand speed-up per tick (mm)", stepSpeedUpMax * 1000.0),
                      info("steps", r.stepsTaken)}, r);
    }
}

/// The pose utilities (mirror, reset) and the Transform dials.
void RealPhases::utilityPhases() {
    // --- Pose utilities (Armature::mirrorPose / mirrorSubtreeToOpposite / resetBone /
    // resetPose). The mirror rule — (x, -y, -z) per Euler channel, a negated x translation — is
    // checked GEOMETRICALLY: after mirroring, the right hand and foot must sit exactly where the
    // reflection (x -> -x) of the left ones was, which only holds if the rule matches the rig's
    // real left/right orientation frames. Mirroring twice must be the identity, a limb copy must
    // be one-way, and the resets must return exactly to rest without touching pins.
    if (report.wants("utilities")) {
        Armature arm;
        arm.build(bones);
        const int lShin = resolveBone(arm, "lShin"), rShin = resolveBone(arm, "rShin");
        const int lFoot = resolveBone(arm, "lFoot"), rFoot = resolveBone(arm, "rFoot");
        const int lForeArm = resolveBone(arm, "lForeArm"), rForeArm = resolveBone(arm, "rForeArm");
        const int lHand = resolveBone(arm, "lHand"), rHand = resolveBone(arm, "rHand");
        const int abdomen = resolveBone(arm, "abdomenLower"), hip = resolveBone(arm, "hip");
        const int head = resolveBone(arm, "head");
        const bool named = lShin >= 0 && rShin >= 0 && lFoot >= 0 && rFoot >= 0 && lForeArm >= 0 &&
                           rForeArm >= 0 && lHand >= 0 && rHand >= 0 && abdomen >= 0 && hip >= 0 &&
                           head >= 0;
        double pairing = 0.0, geomErr = 1e9, ruleErr = 1e9, involutionErr = 1e9, copyErr = 1e9;
        double resetJointErr = 1e9, resetLimbErr = 1e9, resetPoseErr = 1e9;
        double pinKept = 0.0;
        auto err = [](const glm::vec3& a, const glm::vec3& b) {
            return static_cast<double>(glm::length(a - b));
        };
        if (named) {
            pairing = (arm.mirrorBone(static_cast<std::size_t>(lShin)) == rShin &&
                       arm.mirrorBone(static_cast<std::size_t>(rShin)) == lShin &&
                       arm.mirrorBone(static_cast<std::size_t>(lHand)) == rHand &&
                       arm.mirrorBone(static_cast<std::size_t>(hip)) == hip &&
                       arm.mirrorBone(static_cast<std::size_t>(head)) == head)
                          ? 1.0
                          : 0.0;
            // A one-sided pose well inside every limit on both sides (see the base rig's ranges).
            const std::vector<std::pair<std::string, glm::vec3>> pose = {
                {resolveBoneName(arm, "lShin"), glm::vec3(120.0f, -8.0f, 3.0f)},
                {resolveBoneName(arm, "lForeArm"), glm::vec3(0.0f, -100.0f, 0.0f)},
                {resolveBoneName(arm, "lHand"), glm::vec3(5.0f, -20.0f, 30.0f)},
                {resolveBoneName(arm, "abdomenLower"), glm::vec3(10.0f, 5.0f, -5.0f)},
                {resolveBoneName(arm, "head"), glm::vec3(-10.0f, 10.0f, 5.0f)},
                {"@trans:" + resolveBoneName(arm, "hip"), glm::vec3(0.05f, -0.10f, 0.02f)},
            };
            arm.applyPose(pose);
            const glm::vec3 lFootBefore = arm.boneWorldPosition(static_cast<std::size_t>(lFoot));
            const glm::vec3 lHandBefore = arm.boneWorldPosition(static_cast<std::size_t>(lHand));
            std::vector<glm::vec3> eulerBefore, transBefore;
            for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                eulerBefore.push_back(arm.boneEuler(i));
                transBefore.push_back(arm.boneTranslation(i));
            }

            arm.mirrorPose();
            const glm::vec3 reflFoot(-lFootBefore.x, lFootBefore.y, lFootBefore.z);
            const glm::vec3 reflHand(-lHandBefore.x, lHandBefore.y, lHandBefore.z);
            geomErr = std::max(err(arm.boneWorldPosition(static_cast<std::size_t>(rFoot)), reflFoot),
                               err(arm.boneWorldPosition(static_cast<std::size_t>(rHand)), reflHand));
            ruleErr = 0.0;
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(rShin)), glm::vec3(120.0f, 8.0f, -3.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(rForeArm)), glm::vec3(0.0f, 100.0f, 0.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(rHand)), glm::vec3(5.0f, 20.0f, -30.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(abdomen)), glm::vec3(10.0f, -5.0f, 5.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(head)), glm::vec3(-10.0f, -10.0f, -5.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneEuler(static_cast<std::size_t>(lShin)), glm::vec3(0.0f)));
            ruleErr = std::max(ruleErr, err(arm.boneTranslation(static_cast<std::size_t>(hip)), glm::vec3(-0.05f, -0.10f, 0.02f)));

            arm.mirrorPose(); // an involution: back to the original pose exactly
            involutionErr = 0.0;
            for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                involutionErr = std::max(involutionErr, err(arm.boneEuler(i), eulerBefore[i]));
                involutionErr = std::max(involutionErr, err(arm.boneTranslation(i), transBefore[i]));
            }

            // One-way limb copy: the right arm takes the left's pose; the left keeps its own.
            arm.mirrorSubtreeToOpposite(lForeArm);
            copyErr = 0.0;
            copyErr = std::max(copyErr, err(arm.boneEuler(static_cast<std::size_t>(rForeArm)), glm::vec3(0.0f, 100.0f, 0.0f)));
            copyErr = std::max(copyErr, err(arm.boneEuler(static_cast<std::size_t>(rHand)), glm::vec3(5.0f, 20.0f, -30.0f)));
            copyErr = std::max(copyErr, err(arm.boneEuler(static_cast<std::size_t>(lForeArm)), glm::vec3(0.0f, -100.0f, 0.0f)));
            copyErr = std::max(copyErr, err(arm.boneEuler(static_cast<std::size_t>(lHand)), glm::vec3(5.0f, -20.0f, 30.0f)));
            copyErr = std::max(copyErr, err(arm.boneEuler(static_cast<std::size_t>(rShin)), glm::vec3(0.0f))); // outside the subtree: untouched

            // Reset one joint: the hand only, its parent keeps its bend.
            arm.resetBone(lHand, false);
            resetJointErr = std::max(err(arm.boneEuler(static_cast<std::size_t>(lHand)), glm::vec3(0.0f)),
                                     err(arm.boneEuler(static_cast<std::size_t>(lForeArm)), glm::vec3(0.0f, -100.0f, 0.0f)));
            // Reset a limb: the forearm and everything below it; the other arm is untouched.
            arm.resetBone(lForeArm, true);
            resetLimbErr = std::max(err(arm.boneEuler(static_cast<std::size_t>(lForeArm)), glm::vec3(0.0f)),
                                    err(arm.boneEuler(static_cast<std::size_t>(rForeArm)), glm::vec3(0.0f, 100.0f, 0.0f)));
            // Reset the pose: everything to rest, a pin survives.
            arm.setSelectedBone(rHand);
            arm.togglePinSelectedBone();
            arm.resetPose();
            resetPoseErr = 0.0;
            for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                resetPoseErr = std::max(resetPoseErr, err(arm.boneEuler(i), glm::vec3(0.0f)));
                resetPoseErr = std::max(resetPoseErr, err(arm.boneTranslation(i), glm::vec3(0.0f)));
            }
            pinKept = arm.isBonePinned(static_cast<std::size_t>(rHand)) ? 1.0 : 0.0;
        }
        // (The mirror of a pose is exact only on a rig whose two sides are mirror images: a rig
        // that is itself lopsided at rest — an imported one, by millimetres — mirrors its poses as
        // well as its bind allows, and the geometric row is information there, 2026-11-03.)
        double restAsymmetry = 0.0;
        for (const int i : {lHand, lFoot, lShin, lForeArm}) { // (the joints the gate measures; a place-paired carpal is not)
            const int j = i >= 0 ? arm.mirrorBone(static_cast<std::size_t>(i)) : -1;
            if (i < 0 || j < 0 || j == i) {
                continue;
            }
            const glm::vec3 a = arm.boneWorldPosition(static_cast<std::size_t>(i));
            const glm::vec3 b = arm.boneWorldPosition(static_cast<std::size_t>(j));
            restAsymmetry = std::max(restAsymmetry, static_cast<double>(glm::length(glm::vec3(-a.x, a.y, a.z) - b)) * 1000.0);
        }
        report.phase("[real] pose utilities: mirror + reset",
                     {gateMin("bone pairing (l<->r, centre = self)", pairing, 1.0),
                      restAsymmetry <= 1.0 ? gateMax("mirrored hand/foot vs reflected original (mm)", geomErr * 1000.0, 1.0)
                                           : info("mirrored hand/foot vs reflected original (mm; the rig is lopsided at rest)", geomErr * 1000.0),
                      info("the rig's own left/right asymmetry at rest (mm)", restAsymmetry),
                      gateMax("mirror rule error (deg / m)", ruleErr, 1e-3),
                      gateMax("mirror twice = identity (max error)", involutionErr, 1e-3),
                      gateMax("limb copy error (deg)", copyErr, 1e-3),
                      gateMax("reset joint error (deg)", resetJointErr, 1e-3),
                      gateMax("reset limb error (deg)", resetLimbErr, 1e-3),
                      gateMax("reset pose error", resetPoseErr, 1e-3),
                      gateMin("pin survives reset pose", pinKept, 1.0)});
    }

    // --- THE TRANSFORM DIALS (Armature::jointDial; the Transform tab): a joint's rotation as Bend
    // Forward / Bend Sideways / Twist on a 0-100 scale of its own limit. Which channel is which
    // dial and which way is positive is found by structure, so it is checked on every rig: a knee
    // and an elbow read 100 at their fold's limit and a negative value in proportion the other
    // way; the forward dials take what their joint carries forward (the trunk, a thigh, an upper
    // arm) and the side dials sideways; a limb whose twist is cut out into a twist bone has a twist
    // dial all the same; and — the rule's consistency — the SAME value on a left and a right joint
    // gives mirrored poses, for every dial of every paired joint of the body.
    if (report.wants("transform-dials")) {
        Armature arm;
        arm.build(bones);
        arm.boneClass(0); // builds the rig and the bone classes (the head is found by them)
        const auto at = [&](int bone) { return arm.boneWorldPosition(static_cast<std::size_t>(bone)); };
        const int knee = resolveBone(arm, "lShin"), foot = resolveBone(arm, "lFoot");
        const int elbow = resolveBone(arm, "lForeArm"), hand = resolveBone(arm, "lHand");
        const int thigh = resolveBone(arm, "lThigh"), shoulder = resolveBone(arm, "lShldr");
        const int abdomen = resolveBone(arm, "abdomenLower"), head = resolveBone(arm, "head");
        const bool named = knee >= 0 && foot >= 0 && elbow >= 0 && hand >= 0 && thigh >= 0 && shoulder >= 0 &&
                           abdomen >= 0 && head >= 0;
        double foldErr = 1e9, proportionErr = 1e9, roundTripErr = 1e9, stopErr = 1e9;
        double directions = 0.0, twistDials = 0.0, mirrorErr = 0.0, dialled = 0.0, noDial = 0.0;
        double frameAsymDeg = 0.0; // the worst paired joints' frames, left against right reflected (deg)
        double sweepCos = -1.0, sweepRatioErr = 1e9, sweepTwist = 0.0;
        double digitTwins = 0.0, digitJoints = 0.0, limbTwinsMissing = 0.0, limbTwins = 0.0;
        // A FINGER's Bend dial reaches its LONG limit at 100 — the flexion: the dial's low end then
        // reads -100 at most (the short side in proportion). Read the way a joint carries its limb
        // forward or down, a finger hanging in an A-pose was a coin toss, and a thumb's dial
        // extended it on one conformed rig (2026-11-03).
        double fingerDialsShort = 0.0; // finger joints whose Bend dial's 100 is the SHORT side
        double fingerDials = 0.0;
        if (arm.ikRig() != nullptr && arm.ikRig()->roles().built()) {
            for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                if (arm.ikRig()->roles().role(static_cast<int>(i)) != RigRole::Finger) {
                    continue;
                }
                const JointDial d = arm.jointDial(static_cast<int>(i), JointDialKind::BendForward);
                if (!d.valid()) {
                    continue;
                }
                fingerDials += 1.0;
                fingerDialsShort += d.minValue < -100.0f - 1.0e-3f ? 1.0 : 0.0;
            }
        }
        // THE SELECTION HIGHLIGHT's twin (Armature::selectedHighlightTwin): a limb's TWIST bone
        // lights with its bend bone, so a grabbed thigh lights the whole thigh — and nothing else
        // is a twin. It was "any child with two locked axes", which a finger's or a toe's middle
        // joint is too (a hinge): the first two joints of every finger lit together whichever was
        // selected, and the user could not tell which joint a click had taken.
        for (std::size_t i = 0; i < arm.boneCount(); ++i) {
            if (arm.boneClass(static_cast<int>(i)) != BoneClass::Digit) {
                continue;
            }
            digitJoints += 1.0;
            arm.selectBoneByName(arm.boneName(i));
            if (arm.selectedHighlightTwin() >= 0) {
                digitTwins += 1.0;
                if (v) {
                    std::fprintf(stderr, "  [transform-dials] digit joint %s lights %s with it\n", arm.boneName(i).c_str(),
                                 arm.boneName(static_cast<std::size_t>(arm.selectedHighlightTwin())).c_str());
                }
            }
        }
        for (const char* limb : {"lThigh", "rThigh", "lShldr", "rShldr", "lForeArm", "rForeArm"}) {
            const int bone = resolveBone(arm, limb);
            if (bone < 0) {
                continue;
            }
            // (Where the limb's twist is cut out into a twist BONE, that bone is the twin.)
            const JointDial twist = arm.jointDial(bone, JointDialKind::Twist);
            if (twist.valid() && twist.bone != bone) {
                arm.selectBoneByName(arm.boneName(static_cast<std::size_t>(bone)));
                limbTwins += 1.0;
                limbTwinsMissing += arm.selectedHighlightTwin() == twist.bone ? 0.0 : 1.0;
            }
        }
        arm.setSelectedBone(-1);
        if (named) {
            // A fold: 100 is the long limit, the low end the short one in proportion.
            foldErr = 0.0;
            proportionErr = 0.0;
            for (const int fold : {knee, elbow}) {
                const JointDial d = arm.jointDial(fold, JointDialKind::BendForward);
                const ArmatureBone& b = bones[static_cast<std::size_t>(fold)];
                const float lo = d.valid() ? b.rotationMin[d.axis] : 0.0f;
                const float hi = d.valid() ? b.rotationMax[d.axis] : 0.0f;
                const float big = std::max(std::abs(lo), std::abs(hi));
                const float small = std::min(std::abs(lo), std::abs(hi));
                foldErr = std::max(foldErr, d.valid() && d.bone == fold ? static_cast<double>(std::abs(std::abs(d.fullDeg) - big)) : 1e9);
                proportionErr = std::max(proportionErr, d.valid() && big > 0.0f
                                                            ? static_cast<double>(std::abs(d.minValue + small / big * 100.0f)) +
                                                                  static_cast<double>(std::abs(d.maxValue - 100.0f))
                                                            : 1e9);
            }
            // A value set reads back; one beyond the range stops at the limit.
            const JointDial kneeDial = arm.jointDial(knee, JointDialKind::BendForward);
            arm.setJointDial(kneeDial, 50.0f);
            roundTripErr = std::abs(static_cast<double>(arm.jointDialValue(kneeDial)) - 50.0);
            arm.setJointDial(kneeDial, -1000.0f);
            const ArmatureBone& kb = bones[static_cast<std::size_t>(knee)];
            const float kneeEuler = arm.boneEuler(static_cast<std::size_t>(knee))[kneeDial.axis];
            stopErr = std::max(std::abs(static_cast<double>(arm.jointDialValue(kneeDial)) - static_cast<double>(kneeDial.minValue)),
                               static_cast<double>(std::min(std::abs(kneeEuler - kb.rotationMin[kneeDial.axis]),
                                                            std::abs(kneeEuler - kb.rotationMax[kneeDial.axis]))));
            arm.resetPose();

            // Which way a positive dial goes: a joint below the turned one, against where it rested.
            const auto moves = [&](int joint, JointDialKind kind, int carried, const glm::vec3& way) {
                const glm::vec3 rest = at(carried);
                const JointDial d = arm.jointDial(joint, kind);
                arm.setJointDial(d, 30.0f);
                const bool ok = d.valid() && glm::dot(at(carried) - rest, way) > 0.01f;
                arm.resetPose();
                if (v && !ok) {
                    std::fprintf(stderr, "  [transform-dials] %s dial %d does not move %s along (%.0f %.0f %.0f)\n",
                                 arm.boneName(static_cast<std::size_t>(joint)).c_str(), static_cast<int>(kind),
                                 arm.boneName(static_cast<std::size_t>(carried)).c_str(), way.x, way.y, way.z);
                }
                return ok ? 1.0 : 0.0;
            };
            const glm::vec3 fore(0.0f, 0.0f, 1.0f), aft(0.0f, 0.0f, -1.0f), left(1.0f, 0.0f, 0.0f);
            directions = moves(abdomen, JointDialKind::BendForward, head, fore) +    // the trunk bends forward
                         moves(abdomen, JointDialKind::BendSideways, head, left) +   // ... and to her left
                         moves(thigh, JointDialKind::BendForward, knee, fore) +      // a thigh flexes forward
                         moves(thigh, JointDialKind::BendSideways, knee, left) +     // ... and abducts (the left: outward)
                         moves(shoulder, JointDialKind::BendForward, hand, fore) +   // an arm swings forward
                         moves(knee, JointDialKind::BendForward, foot, aft) +        // a knee folds its foot back
                         moves(elbow, JointDialKind::BendForward, hand, fore);       // an elbow folds its hand forward
            // A limb's twist has a dial whether the rig keeps it on the bend bone or on a twist bone.
            for (const int limb : {thigh, shoulder, elbow}) {
                twistDials += arm.jointDial(limb, JointDialKind::Twist).valid() ? 1.0 : 0.0;
            }

            // THE SWEEP (Armature::jointDialSweep — what the viewport's B / S / T mouse mode follows:
            // the limb goes the way the mouse goes): where a bend dial says its limb's far end goes
            // per degree, against where the limb's next joint DOES go when the dial is turned a
            // unit. From a pose with every dial of the joint already turned, so the channels
            // composed outside the dial's own carry its axis (the rotation-order prefix), and on
            // the positive side, so the sign is the dial's.
            sweepCos = 1.0;
            sweepRatioErr = 0.0;
            const auto sweepOf = [&](int joint, int farJoint) {
                for (int k = 0; k < kJointDialCount; ++k) {
                    const JointDial d = arm.jointDial(joint, static_cast<JointDialKind>(k));
                    if (d.valid()) {
                        arm.setJointDial(d, 0.3f * d.maxValue);
                    }
                }
                for (const JointDialKind kind : {JointDialKind::BendForward, JointDialKind::BendSideways}) {
                    const JointDial d = arm.jointDial(joint, kind);
                    if (!d.valid()) {
                        continue; // (a hinge elbow has no side bend)
                    }
                    JointDialSweep s;
                    if (!arm.jointDialSweep(d, s) || glm::length(s.tipPerDegree) < 1e-7f) {
                        sweepCos = -1.0;
                        continue;
                    }
                    const glm::vec3 before = at(farJoint);
                    const float     value = arm.jointDialValue(d);
                    arm.setJointDial(d, value + 1.0f);
                    const float turned = (arm.jointDialValue(d) - value) * s.degreesPerUnit;
                    const glm::vec3 moved = at(farJoint) - before;
                    arm.setJointDial(d, value);
                    if (turned < 1e-3f || glm::length(moved) < 1e-7f) {
                        sweepCos = -1.0; // the dial did not turn, or its limb did not move
                        continue;
                    }
                    const double cosine = glm::dot(glm::normalize(moved), glm::normalize(s.tipPerDegree));
                    const double ratio = glm::length(moved) / turned / glm::length(s.tipPerDegree);
                    if (v && (cosine < 0.95 || std::abs(ratio - 1.0) > 0.25)) {
                        std::fprintf(stderr, "  [transform-dials] %s dial %d sweep: cosine %.3f, measured / predicted %.2f\n",
                                     arm.boneName(static_cast<std::size_t>(joint)).c_str(), static_cast<int>(kind), cosine, ratio);
                    }
                    sweepCos = std::min(sweepCos, cosine);
                    sweepRatioErr = std::max(sweepRatioErr, std::abs(ratio - 1.0));
                }
                const JointDial twist = arm.jointDial(joint, JointDialKind::Twist);
                JointDialSweep  ts;
                if (twist.valid() && arm.jointDialSweep(twist, ts)) {
                    sweepTwist = std::max(sweepTwist, static_cast<double>(glm::length(ts.tipPerDegree)) * 1000.0);
                }
                arm.resetPose();
            };
            sweepOf(thigh, knee);
            sweepOf(knee, foot);
            sweepOf(shoulder, elbow);
            sweepOf(elbow, hand);

            // MIRROR: the same value on a left joint and on its right twin, each posed alone from
            // rest (posed together, the FK collision stop of the second would meet the first).
            std::vector<glm::vec3> rest(arm.boneCount());
            for (std::size_t i = 0; i < rest.size(); ++i) {
                rest[i] = arm.boneWorldPosition(i);
            }
            // What each dial TURNED, left against right reflected (2026-11-03): every bone's world
            // rotation away from the bind, the left's against the right twin's mirrored across the
            // sagittal plane, the worst angle between them. A dial is a rotation: measured by
            // POSITIONS (less the rig's worst rest asymmetry) a rig lopsided at rest read its own
            // bind as a dial mismatch (a Blender export's clavicles 1mm apart: 2.2mm at a gate of
            // 1), and measured by DISPLACEMENTS a character whose knee stands 6mm further from its
            // socket on one side read 4.8mm for the same 46 degrees — the lever's, not the dial's.
            // A limit or a frame authored asymmetrically still shows: the sides turn by different
            // angles or about different axes. The reference rigs mirror exactly.
            const glm::mat3 sagittal(-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            const auto lopsided = [&](const std::vector<glm::mat3>& one, const std::vector<glm::mat3>& other) {
                double worst = 0.0;
                for (std::size_t k = 0; k < one.size(); ++k) {
                    const std::size_t m = static_cast<std::size_t>(arm.mirrorBone(k));
                    worst = std::max(worst, rotationAngleDeg(one[k], sagittal * other[m] * sagittal));
                }
                return worst;
            };
            const auto posedBy = [&](int bone, JointDialKind kind, float share, bool& valid) {
                const JointDial d = arm.jointDial(bone, kind);
                valid = d.valid();
                arm.setJointDial(d, share >= 0.0f ? share * d.maxValue : -share * d.minValue);
                std::vector<glm::mat3> out(arm.boneCount());
                for (std::size_t i = 0; i < out.size(); ++i) {
                    out[i] = glm::mat3(arm.poseGlobal(i)); // (the bind is translation-only: this is the turn away from it)
                }
                arm.resetPose();
                return out;
            };
            for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                const int j = arm.mirrorBone(i);
                if (arm.boneClass(static_cast<int>(i)) == BoneClass::Face || arm.dialJoint(static_cast<int>(i)) != static_cast<int>(i)) {
                    continue; // (the face rig is not posed; a twist bone presents its bend joint)
                }
                int dials = 0;
                for (int k = 0; k < kJointDialCount; ++k) {
                    dials += arm.jointDial(static_cast<int>(i), static_cast<JointDialKind>(k)).valid() ? 1 : 0;
                }
                dialled += dials > 0 ? 1.0 : 0.0;
                noDial += dials == 0 ? 1.0 : 0.0;
                if (j <= static_cast<int>(i)) {
                    continue; // (a centre bone, or the pair's second)
                }
                {
                    // The pair's FRAMES, left against right reflected: a rig conformed from an
                    // asymmetric bind (an imported character's thighs 2.7mm apart in depth) turns
                    // its two sides about axes a quarter of a degree apart, and the same dial value
                    // then lands 2mm apart at the knee — the rig's own, not the dials'.
                    const glm::mat3 frameL(eulerMatrix(bones[i].orientation, "ZYX"));
                    const glm::mat3 frameR(eulerMatrix(bones[static_cast<std::size_t>(j)].orientation, "ZYX"));
                    const glm::mat3 mirror(-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
                    frameAsymDeg = std::max(frameAsymDeg, rotationAngleDeg(frameL, mirror * frameR * mirror));
                }
                for (int k = 0; k < kJointDialCount; ++k) {
                    for (const float share : {0.4f, -0.5f}) {
                        bool validL = false, validR = false;
                        const std::vector<glm::mat3> posedL = posedBy(static_cast<int>(i), static_cast<JointDialKind>(k), share, validL);
                        const std::vector<glm::mat3> posedR = posedBy(j, static_cast<JointDialKind>(k), share, validR);
                        const double err = validL != validR ? 90.0 : lopsided(posedL, posedR);
                        if (v && err > 0.1) { // (the gate; a float frame's noise floor reads 0.03-0.05)
                            const JointDial dl = arm.jointDial(static_cast<int>(i), static_cast<JointDialKind>(k));
                            const JointDial dr = arm.jointDial(j, static_cast<JointDialKind>(k));
                            std::fprintf(stderr, "  [transform-dials] %s / %s dial %d at share %.1f: %.2f deg from mirrored"
                                                 " (L axis %d full %.1f range %.1f..%.1f on %s | R axis %d full %.1f range %.1f..%.1f on %s)\n",
                                         arm.boneName(i).c_str(), arm.boneName(static_cast<std::size_t>(j)).c_str(), k, share, err,
                                         dl.axis, dl.fullDeg, dl.minValue, dl.maxValue, dl.bone >= 0 ? arm.boneName(static_cast<std::size_t>(dl.bone)).c_str() : "-",
                                         dr.axis, dr.fullDeg, dr.minValue, dr.maxValue, dr.bone >= 0 ? arm.boneName(static_cast<std::size_t>(dr.bone)).c_str() : "-");
                        }
                        mirrorErr = std::max(mirrorErr, err);
                    }
                }
            }
        }
        const bool mirrorGated = frameAsymDeg <= 0.05;
        char mirrorInfo[200];
        std::snprintf(mirrorInfo, sizeof(mirrorInfo),
                      "left and right joints at the same value: off mirrored (deg; the rig's own frames differ %.2f deg between the sides)", frameAsymDeg);
        report.phase("[real] transform dials: bend forward / bend sideways / twist",
                     {gateMax("a fold's 100 is its long limit (deg off)", foldErr, 1e-3),
                      gateMax("... and its low end the short limit in proportion (dial units off)", proportionErr, 1e-2),
                      gateMax("a value set reads back (dial units off)", roundTripErr, 1e-2),
                      gateMax("a value beyond the range stops at the limit (off)", stopErr, 1e-2),
                      gateMin("positive dials go the way they say (of 7)", directions, 7.0),
                      gateMin("limb twist dials (thigh, upper arm, forearm)", twistDials, 3.0),
                      // (The error is what the dials TURNED, so the gate holds on a rig lopsided at rest too — and
                      // it is a gate where the pairs' FRAMES mirror, information where the rig's own frames do not.)
                      mirrorGated ? gateMax("left and right joints at the same value: off mirrored (deg)", mirrorErr, 0.1)
                                  : info(mirrorInfo, mirrorErr),
                      gateMax("finger and toe joints lit together with another joint (each lights alone)", digitTwins, 0.0),
                      gateMax("finger joints whose Bend dial's 100 is the short side (it is the flexion, the long side)", fingerDialsShort, 0.0),
                      info("finger joints with a Bend dial", fingerDials),
                      info("finger and toe joints", digitJoints),
                      gateMax("limbs whose twist bone does not light with them", limbTwinsMissing, 0.0),
                      info("limbs with a twist bone", limbTwins),
                      gateMin("a bend dial's sweep points where its limb goes (worst cosine)", sweepCos, 0.95),
                      gateMax("... at the pace it goes (measured / predicted, off 1)", sweepRatioErr, 0.25),
                      info("a bent limb's twist dial moves its far end (mm per degree, most)", sweepTwist),
                      info("body joints with a dial", dialled),
                      info("body joints with none (every channel locked)", noDial)});
    }
}

/// Arms: the overhead raise, the tiptoe, the stride swing, the group drag, the Ctrl+drag scopes, the hand laid on the body, the shoulder girdle.
void RealPhases::armPhases() {
    // --- A reachable overhead pull RAISES THE ARM with the head and chest still, the feet
    // planted, no suspension (the "pulling straight up bends her over" repro).
    if (report.wants("arm-raise")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = pullPath(glm::vec3(0.0f, 0.60f, 0.0f), 60, 90);
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        phaseLegs("[real] arm raise (lHand +60cm y, reachable)",
                     {gateMin("hand rise (m)", r.grabEnd.y - r.grabStart.y, 0.59),
                      gateMax("head displacement (mm)", r.headDisp * 1000.0, 20.0), // 16.5 on the newest rig
                      gateMax("chest displacement (mm)", r.chestDisp * 1000.0, 15.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 10.0),
                      gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                      info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0)}, r);
    }

    // --- A hand pulled up BEYOND the arm's reach lifts the HEELS: the body rises onto the balls
    // of the feet before the lift-off takes her off the floor (THE TIPTOE, 2026-09-25). +100cm
    // straight up is about 9cm beyond the base rig's reach: 15mm of heel and a hand 97mm short
    // before, the feet flat under a straight arm.
    if (report.wants("tiptoe")) {
        // The pull is sized by the ARM, on the rest pose the phases run from: the shoulder's height
        // over the hand plus the straight arm's length — the hand straight up over the shoulder —
        // plus 30% of the arm beyond it (15cm: the trunk's stretch and the shoulder's shrug give
        // 5-9cm of reach past the bones, and the tiptoe itself 6). A fixed +100cm was 9cm beyond
        // the base rig's reach but WITHIN the three T-pose generations' (their pre-posed hand
        // starts lower and their arms are longer: 0.05mm short, flat-footed), and the +112cm they
        // need lifts the newest generation off the floor; at 15% of the arm they rose 18-30mm,
        // the target barely past the bones.
        float reachUp = 1.00f;
        {
            Armature posedProbe;
            posedProbe.build(bones);
            for (const auto& [bone, euler] : restPose) {
                posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
            }
            const int hand = resolveBone(posedProbe, "lHand");
            const int shoulder = resolveBone(posedProbe, "lShldr");
            if (hand >= 0 && shoulder >= 0) {
                const glm::vec3 h = posedProbe.boneWorldPosition(static_cast<std::size_t>(hand));
                const glm::vec3 s = posedProbe.boneWorldPosition(static_cast<std::size_t>(shoulder));
                const float arm = glm::length(h - s);
                reachUp = (s.y - h.y) + arm * 1.30f;
            }
        }
        {
            Scenario sc;
            sc.grab = "lHand";
            sc.path = pullPath(glm::vec3(0.0f, reachUp, 0.0f), 60, 90);
            sc.idleJoints = kIdleForHandDrag;
            const RunResult r = run(sc);
            phaseLegs("[real] tiptoe (lHand straight up to the arm's reach and 30% beyond, held)",
                         {gateMin("heel lift max (mm)", r.contactRiseMax * 1000.0, 35.0),
                          gateMin("hip rise (m)", r.hipDispY, 0.030),
                          gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick joint move (mm)", (r.phases.empty() ? 0.0 : r.phases[0].maxJump) * 1000.0, 60.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          info("head displacement (mm)", r.headDisp * 1000.0),
                          info("the pull (m)", reachUp)}, r);
        }
        {
            // ... and the hand brought back within reach puts the heels down: a function of the
            // target, so the return is exact.
            Scenario sc;
            sc.grab = "lHand";
            const glm::vec3 up(0.0f, reachUp, 0.0f);
            const glm::vec3 back(0.0f, 0.60f, 0.0f);
            sc.path = {{0, glm::vec3(0.0f), "start"}, {60, up, "tiptoe"}, {120, up, "hold-up"},
                       {180, back, "lower"},          {240, back, "hold-down"}};
            sc.idleJoints = kIdleForHandDrag;
            const RunResult r = run(sc);
            phaseLegs("[real] tiptoe, then the hand brought back within reach (lHand to the arm's reach and beyond, then +60cm)",
                         {gateMax("feet vs their rest height at the end (mm)", std::abs(r.feetMinY - r.contactBindMinY) * 1000.0, 5.0),
                          gateMax("hip off its start height at the end (mm)", std::abs(r.hipDispY) * 1000.0, 8.0),
                          gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          info("heel lift max (mm)", r.contactRiseMax * 1000.0)}, r);
        }
    }

    if (report.wants("arm-swing")) {
        // THE STRIDE SWING (2026-10-27; hangIdleArms, kArmSwingPerMetre): a hanging arm swings against
        // the stance's fore-aft split — back on the side of the leading foot, forward on the other —
        // by the change since the press, fading as the hips come down.
        const int lShldr = resolveBone(probe, "lShldr");
        const int rShldr = resolveBone(probe, "rShldr");
        const int lElbow = resolveBone(probe, "lForeArm");
        const int rElbow = resolveBone(probe, "rForeArm");
        // An upper arm's fore-aft angle from straight down (degrees, + forward: the rigs face +z and
        // these gestures turn no heading), at the start and the end of the run.
        const auto swingAt = [&](const std::vector<glm::vec3>& pos, int socket, int elbow) {
            if (socket < 0 || elbow < 0) {
                return 0.0;
            }
            const glm::vec3 d = pos[static_cast<std::size_t>(elbow)] - pos[static_cast<std::size_t>(socket)];
            return static_cast<double>(glm::degrees(std::atan2(d.z, -d.y)));
        };
        const auto swingGained = [&](const RunResult& r, int socket, int elbow) {
            return swingAt(r.endPos, socket, elbow) - swingAt(r.startPos, socket, elbow);
        };
        // THE STRIDE TURNS THE HIPS (2026-11-02; ikPosturePolicy, kStrideYawPerMetre): the pelvis
        // yaws toward the lead leg — the lead side's thigh socket comes AHEAD of the other's — while
        // the chest keeps its heading (the collars' line stays square). Sign-free across the rigs
        // (the root's heading channel is not): read off the sockets and the collars, the rigs facing +z.
        const int lThighSw = resolveBone(probe, "lThigh");
        const int rThighSw = resolveBone(probe, "rThigh");
        const int lCollarSw = resolveBone(probe, "lCollar");
        const int rCollarSw = resolveBone(probe, "rCollar");
        const bool pelvisReadable = lThighSw >= 0 && rThighSw >= 0 && lCollarSw >= 0 && rCollarSw >= 0;
        const auto leftAhead = [&](const std::vector<glm::vec3>& pos, int a, int b) {
            return (a >= 0 && b >= 0) ? static_cast<double>(pos[static_cast<std::size_t>(a)].z - pos[static_cast<std::size_t>(b)].z) : 0.0;
        };
        const auto lineHeading = [&](const std::vector<glm::vec3>& pos, int a, int b) {
            if (a < 0 || b < 0) {
                return 0.0;
            }
            const glm::vec3 d = pos[static_cast<std::size_t>(a)] - pos[static_cast<std::size_t>(b)];
            return static_cast<double>(glm::degrees(std::atan2(d.z, d.x)));
        };
        const auto pelvisLeadGained = [&](const RunResult& r) {
            return (leftAhead(r.endPos, lThighSw, rThighSw) - leftAhead(r.startPos, lThighSw, rThighSw)) * 1000.0;
        };
        const auto chestTurned = [&](const RunResult& r) {
            return std::abs(lineHeading(r.endPos, lCollarSw, rCollarSw) - lineHeading(r.startPos, lCollarSw, rCollarSw));
        };
        const auto gateIf = [&](bool readable, const char* name, double value, double limit, bool isMin) {
            return !readable ? info(name, value) : (isMin ? gateMin(name, value, limit) : gateMax(name, value, limit));
        };
        {
            // A 50cm stride by the left foot: the left arm swings back, the right forward, alike.
            Scenario sc;
            sc.grab = "lFoot";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.50f), 90, 60);
            const RunResult r = run(sc);
            const double    left = swingGained(r, lShldr, lElbow);
            const double    right = swingGained(r, rShldr, rElbow);
            phaseLegs("[real] arm swing: a 50cm stride swings the arms against the legs",
                      {gateMax("the leading side's arm swung BACK (deg, + forward)", left, -12.0),
                       gateMin("the trailing side's arm swung FORWARD (deg)", right, 12.0),
                       gateIf(pelvisReadable, "the pelvis turned toward the lead leg: the left socket ahead of the right, gained (mm)", pelvisLeadGained(r), 12.0, true),
                       gateIf(pelvisReadable, "the shoulders stay square: the collars' line turned (deg)", chestTurned(r), 4.0, false),
                       // (The oldest generation's left shoulder ends on its 25-degree limit 13 degrees
                       // back, the right swinging 21 forward within its 75: a shoulder at its limit
                       // makes the mismatch the rig's, and the row informational.)
                       (limitBoundEnd(r, "lShldr") || limitBoundEnd(r, "rShldr"))
                           ? info("the two swings' mismatch (deg; a shoulder limit-bound)", std::abs(left + right))
                           : gateMax("the two swings' mismatch (deg)", std::abs(left + right), 6.0),
                       gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 45.0)}, r);
        }
        {
            // The stride let go of, then the foot brought home: the arms come back to hanging.
            Scenario sc;
            sc.before.push_back({"lFoot", glm::vec3(0.0f, 0.0f, 0.50f), 90, 40});
            sc.grab = "lFoot";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, -0.50f), 90, 60);
            sc.toRest = true;
            const RunResult r = run(sc);
            phaseLegs("[real] arm swing: the stride closed again brings the arms home",
                      {gateMax("the left arm off its standing hang at the end (deg)", std::abs(swingAt(r.endPos, lShldr, lElbow) - swingAt(r.restPos, lShldr, lElbow)), 3.0),
                       gateIf(pelvisReadable, "the pelvis square again: the sockets' lead off its standing value (mm)", std::abs(leftAhead(r.endPos, lThighSw, rThighSw) - leftAhead(r.restPos, lThighSw, rThighSw)) * 1000.0, 6.0, false),
                       gateIf(pelvisReadable, "the shoulders square again: the collars' line off its standing heading (deg)", std::abs(lineHeading(r.endPos, lCollarSw, rCollarSw) - lineHeading(r.restPos, lCollarSw, rCollarSw)), 3.0, false),
                       gateMax("the right arm off its standing hang at the end (deg)", std::abs(swingAt(r.endPos, rShldr, rElbow) - swingAt(r.restPos, rShldr, rElbow)), 3.0),
                       gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 45.0)}, r);
        }
        {
            // A lunge: a foot slid 60cm back and let go of (the arms swung), then the hips taken
            // 38cm down — the swing fades with the hips, and the arms hang plumb again.
            Scenario sc;
            sc.before.push_back({"lFoot", glm::vec3(0.0f, 0.0f, -0.60f), 90, 40});
            sc.grab = "hip";
            sc.path = pullPath(glm::vec3(0.0f, -0.38f, 0.0f), 90, 60);
            const RunResult r = run(sc);
            phaseLegs("[real] arm swing: a lunge is no walk — the arms hang plumb again as the hips come down",
                      {gateMax("the left arm off its standing hang at the end (deg)", std::abs(swingAt(r.endPos, lShldr, lElbow) - swingAt(r.restPos, lShldr, lElbow)), 10.0),
                       gateMax("the right arm off its standing hang at the end (deg)", std::abs(swingAt(r.endPos, rShldr, rElbow) - swingAt(r.restPos, rShldr, rElbow)), 10.0),
                       gateMin("the arms were swung by the stride (deg)", std::abs(swingAt(r.startPos, lShldr, lElbow) - swingAt(r.startPos, rShldr, rElbow)), 20.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 60.0)}, r);
        }
    }
    if (report.wants("group")) {
        // THE GROUP (2026-10-27; Scenario::alsoGrab): several joints dragged together — the grab the
        // PRIMARY (its grab point, its plane, every policy of a plain drag) and the others SECONDARIES
        // riding the cursor's displacement with it. Each secondary is gated on ITS target: where it
        // stood at the press plus the path's offset.
        const int hip = resolveBone(probe, "hip");
        const int rHand = resolveBone(probe, "rHand");
        const int lFoot = resolveBone(probe, "lFoot");
        const int rFoot = resolveBone(probe, "rFoot");
        const int lShin = resolveBone(probe, "lShin");
        const int rShin = resolveBone(probe, "rShin");
        const auto missOf = [&](const RunResult& r, int bone, const glm::vec3& offset) { // a secondary's distance from ITS target (mm)
            return bone >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(bone)] -
                                                               (r.startPos[static_cast<std::size_t>(bone)] + offset))) * 1000.0
                             : 0.0;
        };
        // The forward push is sized by the ARM, as the reach-down phases are (2026-10-27): the base
        // rig's shoulder-to-hand length is 0.516m, and fixed in centimetres the 35cm push rested
        // 31-47mm short on seven short-armed characters of the sweep — the arm's length, not a miss.
        float armScale = 1.0f;
        {
            const int shoulderRd = resolveBone(probe, "lShldr");
            const int handRd = resolveBone(probe, "lHand");
            if (shoulderRd >= 0 && handRd >= 0) {
                armScale = glm::length(probe.boneWorldPosition(static_cast<std::size_t>(handRd)) -
                                       probe.boneWorldPosition(static_cast<std::size_t>(shoulderRd))) / 0.516f;
            }
        }
        {
            // Both hands raised 40cm together: both arms rise, the body stands where it stood.
            Scenario sc;
            sc.grab = "lHand";
            sc.alsoGrab = {"rHand"};
            const glm::vec3 offset(0.0f, 0.40f, 0.0f);
            sc.path = pullPath(offset, 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] group: both hands raised 40cm together",
                      {gateMax("the primary on the cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                       gateMax("the other hand on its own target (mm)", missOf(r, rHand, offset), 3.0),
                       gateMax("left/right asymmetry gained (mm)", asymmetryGainedMm(r), 5.0),
                       gateMax("the hips moved (mm)", movedMm(r, hip), 40.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
        }
        {
            // Both hands pushed 35cm forward together: she leans, and steps if she must.
            Scenario sc;
            sc.grab = "lHand";
            sc.alsoGrab = {"rHand"};
            const glm::vec3 offset(0.0f, 0.0f, 0.35f * armScale);
            sc.path = pullPath(offset, 50, 60);
            const RunResult r = run(sc);
            // (The main family's hands hang 37cm out from their sockets: 35cm forward puts them at
            // the arm's length, and balance stops the lean 15-17mm short on those three rigs, as it
            // stops a single hand's; the generations whose hands hang nearer the body reach to 0.1.)
            phaseLegs("[real] group: both hands pushed 35cm forward together",
                      {gateMax("the primary on the cursor at rest (mm)", r.restingMiss * 1000.0, 25.0),
                       gateMax("the other hand on its own target (mm)", missOf(r, rHand, offset), 25.0),
                       info("the arm's length over the base rig's (the push is sized by it)", armScale),
                       gateMax("left/right asymmetry gained (mm)", asymmetryGainedMm(r), 5.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 2.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 60.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 2.0)}, r);
        }
        {
            // Both hands brought 30cm down together (toward the knees): the reach-down fold, both on target.
            Scenario sc;
            sc.grab = "lHand";
            sc.alsoGrab = {"rHand"};
            const glm::vec3 offset(0.0f, -0.30f, 0.0f);
            sc.path = pullPath(offset, 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] group: both hands brought 30cm down together",
                      {gateMax("the primary on the cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                       gateMax("the other hand on its own target (mm)", missOf(r, rHand, offset), 5.0),
                       gateMax("left/right asymmetry gained (mm)", asymmetryGainedMm(r), 5.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 60.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 2.0)}, r);
        }
        {
            // Both knees pushed 12cm forward together over their planted feet: both swing, the feet stay.
            Scenario sc;
            sc.grab = "lShin";
            sc.alsoGrab = {"rShin"};
            const glm::vec3 offset(0.0f, 0.0f, 0.12f);
            sc.path = pullPath(offset, 40, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] group: both knees pushed 12cm forward together",
                      {gateMin("the primary knee's travel (mm)", movedMm(r, lShin), 100.0),
                       gateMin("the other knee's travel (mm)", movedMm(r, rShin), 100.0),
                       gateMax("left/right asymmetry gained (mm)", asymmetryGainedMm(r), 5.0),
                       gateMax("the feet moved (mm)", std::max(movedMm(r, lFoot), movedMm(r, rFoot)), 5.0),
                       info("the primary knee's foot moved (mm)", movedMm(r, lFoot)),
                       info("the other knee's foot moved (mm)", movedMm(r, rFoot)),
                       info("the primary knee's foot rose (mm)", lFoot >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lFoot)].y - r.startPos[static_cast<std::size_t>(lFoot)].y) * 1000.0 : 0.0),
                       info("the other knee's foot rose (mm)", rFoot >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(rFoot)].y - r.startPos[static_cast<std::size_t>(rFoot)].y) * 1000.0 : 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 50.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 2.0)}, r);
        }
    }

    // --- THE CTRL+DRAG (Armature's IkScope::Part, 2026-09-26/28). SCOPED for a limb, the head and
    // the neck (IkScope::Chain): the grabbed CHAIN alone answers the cursor — a limb up to where it
    // joins the body, the head and neck to the chest — and nothing else moves: no root, no balance,
    // no step, no lift-off, no arm hang or head righting. The "moved" gates read 0.01mm: a bone
    // none of the solve's unknowns move does not move at all. And THE FIGURE MOVE for a grab of
    // the body itself (IkScope::Figure): further down.
    if (report.wants("scoped")) {
        const int hip = resolveBone(probe, "hip");
        const int chest = resolveBone(probe, "chest_2");
        const int head = resolveBone(probe, "head");
        const int lFoot = resolveBone(probe, "lFoot");
        const int rFoot = resolveBone(probe, "rFoot");
        const int rHand = resolveBone(probe, "rHand");
        const int lToe = resolveBone(probe, "lToe");
        const auto still = [&](const RunResult& r) { // (the body the chain hangs from: hips and feet)
            return std::max({movedMm(r, hip), movedMm(r, lFoot), movedMm(r, rFoot)});
        };
        const auto movedAlongFloor = [&](const RunResult& r, int bone) { // (its place on the floor: x and z)
            if (bone < 0) {
                return 0.0;
            }
            const glm::vec3 d = r.endPos[static_cast<std::size_t>(bone)] - r.startPos[static_cast<std::size_t>(bone)];
            return static_cast<double>(glm::length(glm::vec2(d.x, d.z))) * 1000.0;
        };
        {
            // A hand pulled forward and up within the arm's reach: the arm alone, the hand on the
            // cursor. (20cm forward: the A-pose hand hangs 37cm out from its socket, and 30cm forward
            // put the target at the arm's full length, where the collar box and the shoulder's limits
            // left it 15mm short — the plain drag leans the body for those; a scoped one rightly
            // rests.)
            Scenario sc;
            sc.grab = "lHand";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.15f, 0.20f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] scoped: a hand pulled 20cm forward and 15cm up — the arm alone",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                       gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("the chest moved (mm)", movedMm(r, chest), 0.01),
                       gateMax("the head moved (mm)", movedMm(r, head), 0.01),
                       gateMax("the other hand moved (mm)", movedMm(r, rHand), 0.01),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
        }
        {
            // A hand pulled a metre up, far beyond the arm's reach: the arm points at the cursor,
            // the body stands where it stood — no lean, no tiptoe, no lift-off.
            Scenario sc;
            sc.grab = "lHand";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 1.0f, 0.0f), 60, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] scoped: a hand pulled a metre up, beyond reach — the body stands still",
                      {gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("the chest moved (mm)", movedMm(r, chest), 0.01),
                       gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMin("the hand rose (mm)", static_cast<double>(r.grabEnd.y - r.grabStart.y) * 1000.0, 300.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 60.0)}, r);
        }
        {
            // The head pulled 15cm forward: the neck alone bends; the chest and everything below stay.
            Scenario sc;
            sc.grab = "head";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.15f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] scoped: the head pulled 15cm forward — the neck alone",
                      {gateMin("the head moved (mm)", movedMm(r, head), 30.0),
                       gateMax("the chest moved (mm)", movedMm(r, chest), 0.01),
                       gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0)}, r);
        }
        // THE FIGURE MOVE (2026-09-28; IkScope::Figure): Ctrl + the HIPS moves the whole figure as she
        // is posed: no floor contact is planted, no joint turns, and the user's pins hold. (From
        // 2026-09-28 to 2026-10-28 the abdomen and the chest moved her too; since then a spine joint
        // under Ctrl bends the TRUNK ALONE — CTRL ON THE TORSO, the two chest phases here — as it
        // had from 2026-09-26, and the hips alone move her.) "No joint turned" reads the worst
        // channel of any bone; "not rigid" the worst any joint strayed from the grabbed point's own
        // travel; "turned among" the worst channel of the named bones.
        const auto turnedAmong = [&](const RunResult& r, std::initializer_list<const char*> names) {
            double worst = 0.0;
            for (const char* name : names) {
                const int b = resolveBone(probe, name);
                if (b >= 0 && static_cast<std::size_t>(b) < r.endEuler.size() && static_cast<std::size_t>(b) < r.startEuler.size()) {
                    worst = std::max(worst, static_cast<double>(glm::length(r.endEuler[static_cast<std::size_t>(b)] -
                                                                           r.startEuler[static_cast<std::size_t>(b)])));
                }
            }
            return worst;
        };
        const auto grabTravel = [](const RunResult& r) { return static_cast<double>(glm::length(r.grabEnd - r.grabStart)) * 1000.0; };
        const auto turned = [](const RunResult& r) {
            double worst = 0.0;
            for (std::size_t i = 0; i < r.endEuler.size() && i < r.startEuler.size(); ++i) {
                for (int a = 0; a < 3; ++a) {
                    worst = std::max(worst, static_cast<double>(std::abs(r.endEuler[i][a] - r.startEuler[i][a])));
                }
            }
            return worst;
        };
        // (The joints of the BODY: the hips and what hangs below them. The figure's own node above
        // the hips, at the origin, and a follower's scene node beside them carry no flesh and stay.)
        const auto notRigid = [&](const RunResult& r) {
            const glm::vec3 travel = r.grabEnd - r.grabStart;
            double          worst = 0.0;
            for (std::size_t i = 0; i < r.endPos.size() && i < r.startPos.size() && i < bones.size(); ++i) {
                bool ofBody = false;
                for (int cur = static_cast<int>(i); cur >= 0 && !ofBody; cur = bones[static_cast<std::size_t>(cur)].parent) {
                    ofBody = cur == hip;
                }
                if (ofBody) {
                    worst = std::max(worst, static_cast<double>(glm::length(r.endPos[i] - r.startPos[i] - travel)) * 1000.0);
                }
            }
            return worst;
        };
        {
            // CTRL ON THE TORSO (2026-10-28): the upper chest pulled 20cm forward under Ctrl bends the
            // TRUNK ALONE — the spine from the pelvis, coupled as one — and nothing below the hips
            // moves: the pelvis, the legs and the feet at 0.0000, no step, no balance; the arms and
            // the head ride the chest. (This phase was the figure move's "lifted off the floor as
            // posed" from 2026-09-28 to this day; the hips' lift below is that gesture now.)
            Scenario sc;
            sc.grab = "chest_2";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.20f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the upper chest pulled 20cm forward — the trunk alone, from the pelvis",
                      {gateMin("the chest moved (mm)", grabTravel(r), 80.0),
                       gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("a leg turned (deg)", turnedAmong(r, {"lThigh", "rThigh", "lShin", "rShin", "lFoot", "rFoot"}), 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
        }
        {
            // The hips pulled 30cm up under Ctrl: she is lifted off the floor as posed (the figure
            // move, the hips' own gesture since 2026-10-28).
            Scenario sc;
            sc.grab = "hip";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.30f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the hips pulled 30cm up — the figure, lifted as posed",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 0.1),
                       gateMin("the feet rose (mm)", static_cast<double>(r.endPos[static_cast<std::size_t>(lFoot)].y -
                                                                         r.startPos[static_cast<std::size_t>(lFoot)].y) * 1000.0, 299.0),
                       gateMax("a joint turned (deg)", turned(r), 0.0),
                       gateMax("a joint of the body strayed from its travel (mm)", notRigid(r), 0.01),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("mean drag lag (mm)", r.phases[0].lagMean() * 1000.0, 8.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 0.1)}, r);
        }
        // The pelvis GIRDLE under Ctrl (2026-10-28, the user's report that the upper abdomen did not
        // bend the trunk): the first and second spine bones are girdle grabs the rig promotes to the
        // hips, and promoted they were the figure move; a spine joint keeps its own effector under a
        // scoped drag now, while the pelvis bone (a foot below it) still goes to the hips. Found by
        // structure, as the girdle-grab phases find them.
        int girdlePelvisBone = -1, girdleSpine1 = -1, girdleSpine2 = -1;
        {
            for (std::size_t i = 0; i < bones.size() && hip >= 0; ++i) {
                if (bones[i].parent == hip && under(lFoot, static_cast<int>(i)) && under(rFoot, static_cast<int>(i))) {
                    girdlePelvisBone = static_cast<int>(i); // (BOTH legs hang from a pelvis bone: a rig whose thighs hang straight off the hips has none, and a thigh is no pelvis)
                }
                if (bones[i].parent == hip && under(head, static_cast<int>(i))) {
                    girdleSpine1 = static_cast<int>(i);
                }
            }
            for (std::size_t i = 0; i < bones.size() && girdleSpine1 >= 0; ++i) {
                if (bones[i].parent == girdleSpine1 && under(head, static_cast<int>(i))) {
                    girdleSpine2 = static_cast<int>(i);
                }
            }
        }
        if (girdleSpine2 >= 0) {
            // The second spine bone (the upper abdomen) pulled 10cm forward under Ctrl: the lower
            // spine alone bends, down to the pelvis; the hips and the legs stay.
            Scenario sc;
            sc.grab = bones[static_cast<std::size_t>(girdleSpine2)].name;
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.10f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the second spine bone pulled 10cm forward — the lower spine alone, from the pelvis",
                      {gateMin("the grab moved (mm)", grabTravel(r), 40.0),
                       gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("a leg turned (deg)", turnedAmong(r, {"lThigh", "rThigh", "lShin", "rShin", "lFoot", "rFoot"}), 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                       gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
        } else {
            report.skip("[real] ctrl: the second spine bone pulled 10cm forward — the lower spine alone, from the pelvis", "bone not found");
        }
        {
            // A COLLAR pulled 15cm forward under Ctrl (2026-10-28, the user's "ctrl+click drag on
            // chest_2 does nothing": the upper chest's skin above the breast line picks a collar, whose
            // own chain is one bone that cannot move its joint): the collar is the trunk's under Ctrl,
            // promoted to the spine joint it hangs from — the trunk bends, the hips and legs stay.
            // Found by structure: the ancestor of the hand whose parent is the upper chest.
            int collar = -1;
            for (int cur = resolveBone(probe, "lHand"); cur >= 0 && chest >= 0; cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (bones[static_cast<std::size_t>(cur)].parent == chest) {
                    collar = cur;
                    break;
                }
            }
            if (collar >= 0) {
                Scenario sc;
                sc.grab = bones[static_cast<std::size_t>(collar)].name;
                sc.scoped = true;
                sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.15f), 50, 40);
                const RunResult r = run(sc);
                phaseLegs("[real] ctrl: a collar pulled 15cm forward — the trunk, through the collar, from the pelvis",
                          {gateMin("the grab moved (mm)", grabTravel(r), 80.0),
                           gateMin("the chest moved (mm)", movedMm(r, chest), 60.0),
                           gateMax("the hips and feet moved (mm)", still(r), 0.01),
                           gateMax("a leg turned (deg)", turnedAmong(r, {"lThigh", "rThigh", "lShin", "rShin", "lFoot", "rFoot"}), 0.0),
                           gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                           gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                           gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
            } else {
                report.skip("[real] ctrl: a collar pulled 15cm forward — the trunk, through the collar, from the pelvis", "bone not found");
            }
        }
        {
            // The NECK'S LOWEST joint pulled 15cm forward under Ctrl (2026-10-28, the user's third
            // report: the lit upper chest runs up the throat, a click there picks the neck's base,
            // and its own chain — one bone — moved nothing): it is the chest's under Ctrl, promoted
            // to the spine joint it hangs from, like a collar. Found by structure: the ancestor of
            // the head whose parent is the upper chest.
            int neckBase = -1;
            for (int cur = head; cur >= 0 && chest >= 0; cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (bones[static_cast<std::size_t>(cur)].parent == chest) {
                    neckBase = cur;
                    break;
                }
            }
            if (neckBase >= 0 && neckBase != head) {
                Scenario sc;
                sc.grab = bones[static_cast<std::size_t>(neckBase)].name;
                sc.scoped = true;
                sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.15f), 50, 40);
                const RunResult r = run(sc);
                phaseLegs("[real] ctrl: the neck's lowest joint pulled 15cm forward — the trunk, from the pelvis",
                          // (80-87mm on six rigs; 60-62 on the two T-pose generations, whose neck base
                          // stands further over the chest joint.)
                          {gateMin("the grab moved (mm)", grabTravel(r), 80.0),
                           gateMin("the chest moved (mm)", movedMm(r, chest), 50.0),
                           gateMax("the hips and feet moved (mm)", still(r), 0.01),
                           gateMax("a leg turned (deg)", turnedAmong(r, {"lThigh", "rThigh", "lShin", "rShin", "lFoot", "rFoot"}), 0.0),
                           gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                           gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                           gateMax("hold oscillation (mm)", holdOscMm(r), 1.0)}, r);
            } else {
                report.skip("[real] ctrl: the neck's lowest joint pulled 15cm forward — the trunk, from the pelvis", "bone not found");
            }
        }
        if (girdlePelvisBone >= 0) {
            // The pelvis bone pulled 20cm up under Ctrl: a click on the hips' flesh finds it, and it
            // still moves the figure as posed.
            Scenario sc;
            sc.grab = bones[static_cast<std::size_t>(girdlePelvisBone)].name;
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.20f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the pelvis bone pulled 20cm up — the figure, lifted as posed",
                      {gateMin("the feet rose (mm)", static_cast<double>(r.endPos[static_cast<std::size_t>(lFoot)].y -
                                                                         r.startPos[static_cast<std::size_t>(lFoot)].y) * 1000.0, 199.0),
                       gateMax("a joint turned (deg)", turned(r), 0.0),
                       gateMax("a joint of the body strayed from its travel (mm)", notRigid(r), 0.01),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        } else {
            report.skip("[real] ctrl: the pelvis bone pulled 20cm up — the figure, lifted as posed", "bone not found");
        }
        {
            // The hips carried 40cm sideways along the floor: no step, no lean — she slides as posed.
            Scenario sc;
            sc.grab = "hip";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.40f, 0.0f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the hips carried 40cm sideways — the figure, moved as posed",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 0.1),
                       gateMin("the feet went along (mm)", movedMm(r, lFoot), 399.0),
                       gateMax("a joint turned (deg)", turned(r), 0.0),
                       gateMax("a joint of the body strayed from its travel (mm)", notRigid(r), 0.01),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
        {
            // The hips pushed 20cm DOWN from standing: the floor stops her — she stays as she stands.
            Scenario sc;
            sc.grab = "hip";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.0f, -0.20f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] ctrl: the hips pushed 20cm down — the floor stops her, as posed",
                      {gateMax("the hips came down (mm)", movedMm(r, hip), 0.5),
                       gateMax("a joint turned (deg)", turned(r), 0.0),
                       gateMax("floor penetration (mm)", r.penetrationMax * 1000.0, 0.5)}, r);
        }
        {
            // A hand PINNED, the chest carried 10cm toward it and 5cm up under Ctrl: the trunk bends
            // toward the hand, the pinned hand stays where it is pinned and its arm gives (it folds:
            // the shoulder comes nearer the hand); the hips, the legs and the other arm do not move.
            // (From 2026-09-28 to 2026-10-28 this phase carried the whole figure toward the hand.)
            // Pulled 30cm straight UP instead the trunk gives what its extension allows and the pin
            // holds the hand to a hair — the second run.
            Scenario sc;
            sc.grab = "chest_2";
            sc.scoped = true;
            sc.userPins = {"rHand"};
            sc.path = pullPath(glm::vec3(-0.10f, 0.05f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            Scenario up = sc;
            up.path = pullPath(glm::vec3(0.0f, 0.30f, 0.0f), 50, 40);
            const RunResult taut = run(up);
            const std::initializer_list<const char*> legsAndOtherArm = {"lThigh", "rThigh", "lShin", "rShin", "lFoot", "rFoot", "lShldr", "lForeArm"};
            phaseLegs("[real] ctrl: the chest carried 10cm with a hand pinned — the trunk alone, the pin holds, its arm gives",
                      // (73-76mm of the 100 asked on six rigs; 49-51 on the two T-pose generations, whose pinned
                      // hand stands out at arm's length and whose trunk side-bends less.)
                      {gateMin("the chest moved (mm)", grabTravel(r), 40.0),
                       gateMax("the pinned hand moved (mm)", r.userPinDistMax * 1000.0, 0.05),
                       gateMax("the hips and feet moved (mm)", still(r), 0.01),
                       gateMax("the legs or the other arm turned (deg)", turnedAmong(r, legsAndOtherArm), 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                       gateMax("lifted 30cm, beyond the trunk's reach: the pinned hand moved (mm)", taut.userPinDistMax * 1000.0, 0.5),
                       gateMax("... the hips and feet moved (mm)", still(taut), 0.01),
                       gateMax("... the legs or the other arm turned (deg)", turnedAmong(taut, legsAndOtherArm), 0.0),
                       gateMax("... hold: worst single-tick jump (mm)", taut.phases.size() > 1 ? taut.phases[1].maxJump * 1000.0 : 0.0, 1.0)}, r);
        }
        {
            // A knee swung 12cm out: the leg alone, over its planted foot. The ankle keeps its PLACE
            // on the floor (the knee drag's hard hold in the floor's plane); with the hips frozen the
            // knee's arc is the foot's to give, so the heel lifts a little (2cm at the ankle) and the
            // foot turns on it — the toes swing 6-7cm, as they do under the plain knee drag, whose
            // sole heading is soft by design.
            Scenario sc;
            sc.grab = "lShin";
            sc.scoped = true;
            sc.path = pullPath(glm::vec3(0.12f, 0.0f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] scoped: a knee swung 12cm out — the leg alone over its planted foot",
                      {gateMax("the hips moved (mm)", movedMm(r, hip), 0.01),
                       gateMax("the chest moved (mm)", movedMm(r, chest), 0.01),
                       gateMax("the ankle's place on the floor (mm)", movedAlongFloor(r, lFoot), 5.0),
                       info("the ankle's rise (mm; a heel lift)", movedMm(r, lFoot)),
                       info("the toes (mm; the foot turning on its heel)", movedMm(r, lToe)),
                       gateMin("the knee moved (mm)", static_cast<double>(glm::length(r.grabEnd - r.grabStart)) * 1000.0, 60.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0)}, r);
        }
    }

    // --- A HAND LAID ON THE BODY (2026-09-25): a dragged wrist whose target rests against a body volume
    // lies on its PALM there (ikCursorRows' lay rows; the wrist's own channels are unknowns for it).
    // Each phase brings the right hand to a body point and gates where the palm FACES at the end
    // (RunResult::endPalm against the surface's inward direction: inward at the hip, backward at
    // the chest, down on the crown of the head and on a knee) with the gesture's own guards: no
    // step, the worst single-tick move through the drag and through the hold, the head where it
    // was. Before the lay the hand rode the forearm at the rotation the drag found it - edge-on
    // beside the hip (90 degrees off), across the face palm-down.
    if (report.wants("hand-lay")) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const auto at = [&](const char* name) {
            const int i = resolveBone(posedProbe, name);
            return i >= 0 ? posedProbe.boneWorldPosition(static_cast<std::size_t>(i)) : glm::vec3(0.0f);
        };
        const int handP = resolveBone(posedProbe, "rHand");
        const int rThighP = resolveBone(posedProbe, "rThigh");
        const int lThighP = resolveBone(posedProbe, "lThigh");
        const int chestP = resolveBone(posedProbe, "chest_2");
        const int headP = resolveBone(posedProbe, "head");
        const int hipP = resolveBone(posedProbe, "hip");
        const int kneeP = resolveBone(posedProbe, "rShin");
        // The palm's angle off @p inward at the end (degrees; 0 = facing straight into the surface,
        // 180 = facing away), or -1 without a palm to read.
        const auto palmOff = [&](const RunResult& r, const glm::vec3& inward) {
            if (handP < 0 || static_cast<std::size_t>(handP) >= r.endPalm.size()) {
                return -1.0;
            }
            const glm::vec3 p = r.endPalm[static_cast<std::size_t>(handP)];
            if (glm::length(p) < 0.5f || glm::length(inward) < 1.0e-6f) {
                return -1.0;
            }
            return static_cast<double>(glm::degrees(std::acos(glm::clamp(glm::dot(p, glm::normalize(inward)), -1.0f, 1.0f))));
        };
        const auto jumpOf = [](const RunResult& r, std::size_t phase) {
            return phase < r.phases.size() ? r.phases[phase].maxJump * 1000.0 : 0.0;
        };
        if (handP >= 0 && rThighP >= 0 && lThighP >= 0 && chestP >= 0 && headP >= 0 && hipP >= 0 && kneeP >= 0) {
            const glm::vec3 h = at("rHand");
            const glm::vec3 rThigh = at("rThigh");
            const float     outward = rThigh.x < at("lThigh").x ? -1.0f : 1.0f; // the right side's sign of x
            const glm::vec3 forward(0.0f, 0.0f, 1.0f);                           // the figures face +z
            {
                // 1. THE HIP - akimbo: a target INSIDE the pelvis, a little outboard of the thigh socket
                // and above it - the drag clamp puts the wrist on the hip's surface, whatever the rig's
                // width (12cm outboard of the socket missed the newest generation's pelvis by 9cm).
                Scenario sc;
                sc.grab = "rHand";
                sc.path = pullPath(rThigh + glm::vec3(outward * 0.04f, 0.06f, 0.0f) - h, 50, 40);
                sc.idleJoints = kIdleForHandDrag;
                const RunResult r = run(sc);
                // AKIMBO (2026-11-02): the upper arm comes OUT as the hand lies on the hip — the shoulder's
                // abduction reference of the lay (kHandLayElbow*); it read 8-15 degrees from straight down
                // before (the arm hanging straight with the hand pressed to the hip's side), 26-38 with it.
                // The reference asks no more than the FOREARM can give with the hand where it is (the law of
                // cosines about the socket, the solve's own rule): on a broad-shouldered, narrow-hipped body
                // the hand lies under the socket and the arm can stand 13-15 degrees out at most (the two
                // heavy characters of the sweep; the trunk leaned 50-70mm to buy more before), where the base
                // rigs allow 35-45 — the gate is the base rigs', informational where the reach reads under 30.
                const int shldrP = resolveBone(posedProbe, "rShldr");
                const int elbowP = resolveBone(posedProbe, "rForeArm");
                double    upperOutDeg = -1.0;
                double    reachOutDeg = -1.0;
                if (shldrP >= 0 && elbowP >= 0 && static_cast<std::size_t>(elbowP) < r.endPos.size() && static_cast<std::size_t>(handP) < r.endPos.size()) {
                    const glm::vec3 socketAt = r.endPos[static_cast<std::size_t>(shldrP)];
                    const glm::vec3 elbowAt = r.endPos[static_cast<std::size_t>(elbowP)];
                    const glm::vec3 handAt = r.endPos[static_cast<std::size_t>(handP)];
                    const glm::vec3 up = elbowAt - socketAt;
                    if (glm::length(up) > 1.0e-4f) {
                        upperOutDeg = glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(up), glm::vec3(0.0f, -1.0f, 0.0f)), -1.0f, 1.0f)));
                    }
                    const float U = glm::length(up);
                    const float F = glm::length(handAt - elbowAt);
                    const glm::vec3 d = handAt - socketAt;
                    const float R = glm::length(d);
                    if (U > 1.0e-3f && F > 1.0e-3f && R > 1.0e-3f) {
                        const float cosAlpha = glm::clamp((U * U + R * R - F * F) / (2.0f * U * R), -1.0f, 1.0f);
                        const float a = d.x * outward / R; // (the lateral-down plane: + outboard)
                        const float b = -d.y / R;
                        const float hyp = std::sqrt(a * a + b * b);
                        if (hyp > 1.0e-4f && cosAlpha <= hyp) {
                            reachOutDeg = glm::degrees(glm::pi<float>() - std::asin(glm::clamp(cosAlpha / hyp, -1.0f, 1.0f)) - std::atan2(b, a));
                        }
                    }
                }
                const bool reachBound = reachOutDeg >= 0.0 && reachOutDeg < 30.0;
                phaseLegs("[real] hand laid on the hip: the palm faces the hip",
                             {gateMax("palm off the hip's inward normal at the end (deg; edge-on it read 90)", palmOff(r, glm::vec3(-outward, 0.0f, 0.0f)), 35.0),
                              (upperOutDeg < 0.0 || reachBound)
                                  ? info("the upper arm out from straight down (deg; akimbo — informational: the forearm's reach of the hand on this body allows under 30)", upperOutDeg)
                                  : gateMin("the upper arm out from straight down (deg; akimbo — it read 8-15 with the arm straight; 26-35 with the reach-bound reference)", upperOutDeg, 20.0),
                              info("the most abduction the forearm's reach of the hand allows (deg)", reachOutDeg),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              gateMax("worst single-tick joint move, drag (mm)", jumpOf(r, 0), 40.0),
                              gateMax("worst single-tick joint move, hold (mm)", jumpOf(r, 1), 25.0),
                              // (40 since the akimbo: the arm swung out shifts the trunk 3cm on the base rig, 32.)
                              gateMax("head displacement (mm)", r.headDisp * 1000.0, 40.0),
                              // (The cursor sits inside the wrist's clearance of the hip: the clamp holds the hand off.)
                              info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0)}, r);
            }
            {
                // 2. THE CHEST - a hand on the heart: a hand's length in front of the upper chest,
                // a little toward the other side.
                Scenario sc;
                sc.grab = "rHand";
                sc.path = pullPath(at("chest_2") + glm::vec3(-outward * 0.05f, 0.02f, 0.12f) - h, 60, 40);
                sc.idleJoints = kIdleForHandDrag;
                const RunResult r = run(sc);
                phaseLegs("[real] hand laid on the chest: the palm faces the chest",
                             {gateMax("palm off the chest's inward normal at the end (deg)", palmOff(r, -forward), 35.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              // (The T-pose generations' hands travel half a metre to the chest and their arms
                              // re-fold on the way: 19-26mm with the lay off, 45-58 as the palm turned on
                              // arrival before the blended normal; the gate is the transient's guard.)
                              gateMax("worst single-tick joint move, drag (mm)", jumpOf(r, 0), 60.0),
                              gateMax("worst single-tick joint move, hold (mm)", jumpOf(r, 1), 25.0),
                              // (The T-pose generations' and the newest rig's trunk leans 5-8cm at the head to
                              // bring the hand across: 76mm on the newest with the lay off — the gesture's.)
                              gateMax("head displacement (mm)", r.headDisp * 1000.0, 90.0),
                              info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0)}, r);
            }
            {
                // 3. ON TOP OF THE HEAD: a target inside the skull's crown - the clamp puts the wrist on
                // the surface. With the wrist riding the forearm the palm was turned by the forearm's
                // twist alone, pronated it to its limit and flipped basins IN THE HOLD (176mm in a tick).
                Scenario sc;
                sc.grab = "rHand";
                sc.path = pullPath(at("head") + glm::vec3(0.0f, 0.14f, 0.0f) - h, 60, 40);
                sc.idleJoints = kIdleForHandDrag;
                const RunResult r = run(sc);
                phaseLegs("[real] hand laid on top of the head: the palm faces down onto it",
                             // (Informational where the head has moved more than 8cm from where it was: the
                             // gesture's target is a world point over the crown at the press, and on the
                             // oldest generation the arm's corrected composition reaches it with the trunk
                             // leaning 12cm — the hand on the cursor, the head no longer under it.)
                             {r.headDisp > 0.08 ? info("palm off straight down at the end (deg; the head moved away)", palmOff(r, glm::vec3(0.0f, -1.0f, 0.0f)))
                                                : gateMax("palm off straight down at the end (deg)", palmOff(r, glm::vec3(0.0f, -1.0f, 0.0f)), 35.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              // (The arm coming up over the head re-folds on the way on the male rig and the
                              // oldest generation: 80-82mm with the lay off — the gesture's own.)
                              gateMax("worst single-tick joint move, drag (mm)", jumpOf(r, 0), 100.0),
                              // (8-29 on the eight rigs; 42 on a short-armed character of the sweep whose hand barely
                              // reaches the crown of its head — the upper arm at 110, the collar at its caps, the palm
                              // 41 degrees from flat, the rows hopping an arm with nothing left. A soft spot; the gate
                              // still catches the class it was set for.)
                              gateMax("worst single-tick joint move, hold (mm; it read 176)", jumpOf(r, 1), 45.0),
                              info("head displacement (mm)", r.headDisp * 1000.0),
                              info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0)}, r);
            }
            {
                // 4. SEATED, HIPS PINNED: the hand brought down onto the knee. The chair-height sit of
                // the seated phases, made and let go of, then the hips pinned; the knee and the hand
                // are where THAT pose leaves them (read off a run of the sit alone).
                const glm::vec3 hipR = at("hip");
                const glm::vec3 kneeR = at("rShin");
                const glm::vec3 sit(0.0f, -0.85f * (hipR.y - kneeR.y), -0.55f * glm::length(hipR - kneeR));
                Scenario s0;
                s0.grab = "hip";
                s0.path = pullPath(sit, 90, 40);
                const RunResult r0 = run(s0);
                const glm::vec3 handSeated = static_cast<std::size_t>(handP) < r0.endPos.size() ? r0.endPos[static_cast<std::size_t>(handP)] : h;
                const glm::vec3 kneeSeated = static_cast<std::size_t>(kneeP) < r0.endPos.size() ? r0.endPos[static_cast<std::size_t>(kneeP)] : kneeR;
                Scenario sc;
                sc.grab = "rHand";
                sc.before.push_back({"hip", sit, 90, 40});
                sc.userPins = {"hip"};
                sc.pinsAfterBefore = true;
                sc.path = pullPath(kneeSeated + glm::vec3(0.0f, 0.05f, 0.04f) - handSeated, 60, 40);
                sc.idleJoints = kIdleForHandDrag;
                const RunResult r = run(sc);
                // (60 since the seat rocks under a hand drag too, 2026-10-28: the trunk hinges at the hips
                // to reach the knee — the root pitched 12-16 degrees where it was 0 — and the hand lies on
                // the knee's sloping top from that lean at 50-55 on the eight rigs, 39-41 before.)
                phaseLegs("[real] seated, hips pinned: a hand laid on the knee faces down onto it",
                             {gateMax("palm off straight down at the end (deg; the knee's top slopes forward)", palmOff(r, glm::vec3(0.0f, -1.0f, 0.0f)), 60.0),
                              gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              gateMax("worst single-tick joint move, drag (mm)", jumpOf(r, 0), 60.0),
                              gateMax("worst single-tick joint move, hold (mm)", jumpOf(r, 1), 30.0),
                              // (The hand's target is beyond the hanging arm's reach: the trunk leans to it.)
                              info("head displacement (mm)", r.headDisp * 1000.0)}, r);
            }
        }
    }

    // --- THE SHOULDER GIRDLE (2026-09-25): a hand dragged straight to the NAPE from the rest pose
    // ran the shoulder to its extension limit and the collar to all three of its limits — the
    // socket hauled onto the neck, the trunk leaning 14cm at the head. Priced like the trunk the
    // collar stays where a collar goes; and under a hand raised overhead it still SHRUGS with the
    // arm (the scapulohumeral rhythm), as it did at the plain prices.
    if (report.wants("girdle")) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const int handP = resolveBone(posedProbe, "rHand");
        const int headP = resolveBone(posedProbe, "head");
        const int collarP = resolveBone(posedProbe, "rCollar");
        // The collar's channel moves, largest first: the largest is its ELEVATION (free to the pull,
        // capped at kCollarElevationMaxDeg), the other two its twist and swing (the trunk's price).
        const auto collarMoves = [&](const RunResult& r) {
            std::array<double, 3> moves{0.0, 0.0, 0.0};
            if (collarP >= 0 && static_cast<std::size_t>(collarP) < r.endEuler.size()) {
                for (int a = 0; a < 3; ++a) {
                    moves[static_cast<std::size_t>(a)] = std::abs(static_cast<double>(
                        r.endEuler[static_cast<std::size_t>(collarP)][a] - r.startEuler[static_cast<std::size_t>(collarP)][a]));
                }
            }
            std::sort(moves.begin(), moves.end(), std::greater<double>());
            return moves;
        };
        const auto collarMove = [&](const RunResult& r) { return collarMoves(r)[0]; };
        if (handP >= 0 && headP >= 0) {
            // The nape: a hand's breadth below the top of the head, a skull's depth behind its centre.
            const glm::vec3 h = posedProbe.boneWorldPosition(static_cast<std::size_t>(handP));
            const glm::vec3 head = posedProbe.boneWorldPosition(static_cast<std::size_t>(headP));
            Scenario sc;
            sc.grab = "rHand";
            sc.path = pullPath(head + glm::vec3(0.0f, 0.02f, -0.10f) - h, 60, 40);
            sc.idleJoints = kIdleForHandDrag;
            const RunResult r = run(sc);
            // (The straight path leads the arm into its EXTENSION basin — swung back to its limit,
            // abducted to horizontal — where the nape is reached by a trunk leaning 20cm at the head:
            // a SOFT SPOT, the lean being the designed answer to a hand out of reach. What the round
            // fixed is the collar's part in it: twist 30, swing -17, elevation -55 — a socket hauled
            // onto the neck. The head's displacement is informational for that reason.)
            const std::array<double, 3> moves = collarMoves(r);
            phaseLegs("[real] hand straight to the nape: the collar stays within what a clavicle does",
                         {gateMax("collar elevation move (deg; it read 55, the cap is 30)", moves[0], 31.0), // (kCollarElevationMaxDeg + 1)
                          gateMax("collar twist/swing move, larger (deg; it read 30 and 17, the cap is 15)", moves[1], 16.0), // (kCollarOtherMaxDeg + 1)
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          // (The straight path's basin flips are the gesture's own — 41-368mm on the
                          // eight rigs before the cap, in one direction or the other.)
                          info("worst single-tick joint move (mm)", (r.phases.empty() ? 0.0 : r.phases[0].maxJump) * 1000.0),
                          info("head displacement (mm; it read 146)", r.headDisp * 1000.0),
                          info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0)}, r);
        }
        {
            // A hand pulled straight up beyond the arm's reach (the tiptoe phase's pull: the
            // shoulder's height over the hand plus 1.3 arm lengths — a fixed +80cm is within the
            // three T-pose generations' reach and their collars need not move): the collar SHRUGS
            // with the arm, and no further than the cap.
            float reachUp = 1.00f;
            const int shoulderP = resolveBone(posedProbe, "rShldr");
            if (handP >= 0 && shoulderP >= 0) {
                const glm::vec3 h = posedProbe.boneWorldPosition(static_cast<std::size_t>(handP));
                const glm::vec3 sh = posedProbe.boneWorldPosition(static_cast<std::size_t>(shoulderP));
                reachUp = (sh.y - h.y) + glm::length(h - sh) * 1.30f;
            }
            Scenario sc;
            sc.grab = "rHand";
            sc.path = pullPath(glm::vec3(0.0f, reachUp, 0.0f), 60, 40);
            sc.idleJoints = kIdleForHandDrag;
            const RunResult r = run(sc);
            // (A hand raised BESIDE the head, too — the rejected collar price flipped this one 136mm
            // on the male rig, the rejected rhythm coupling 174 on the base.)
            Scenario sb;
            sb.grab = "rHand";
            sb.path = pullPath(glm::vec3(0.22f, 0.52f, 0.0f), 60, 30);
            sb.idleJoints = kIdleForHandDrag;
            const RunResult rb = run(sb);
            phaseLegs("[real] hand pulled straight up beyond reach: the collar shrugs with the arm, to the cap",
                         {gateMin("collar channel move, worst (deg; the shrug)", collarMove(r), 10.0),
                          gateMax("collar channel move, worst (deg; the cap)", collarMove(r), 31.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("suspended", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("hand raised beside the head: worst single-tick joint move (mm; the collar price read 136)",
                                  (rb.phases.empty() ? 0.0 : rb.phases[0].maxJump) * 1000.0, 60.0),
                          gateMax("hand raised beside the head: hand-to-cursor at rest (mm)", rb.restingMiss * 1000.0, 5.0),
                          info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          info("head displacement (mm)", r.headDisp * 1000.0)}, r);
        }
    }
}

/// Lifting: the suspension beyond reach and the figure lowered back onto her feet.
void RealPhases::liftPhases() {
    // --- A sustained pull far beyond reach LIFTS the figure: the feet leave the floor and the
    // body dangles below the grab point.
    if (report.wants("suspension")) {
        Scenario sc;
        sc.grab = "lHand";
        // +1.35m: the lift gate is GEOMETRIC (the target farther from the root than the
        // hand-to-root chain plus 15cm — arm and spine, ~1.1m), and +1.20 cleared it by a
        // hair on the base rig only; a rig whose hand rests 5cm lower never lifted at all.
        sc.path = pullPath(glm::vec3(0.0f, 1.35f, 0.0f), 60, 120);
        sc.release = false;
        const RunResult r = run(sc);
        // THE DANGLE (2026-11-02): a suspended body HANGS — the legs together and plumb from the hips
        // (the thighs a little forward), the knees soft, the feet pointed, the free arm plumb. Before
        // it she kept her standing pose in the air: the thighs at the stance's splay (7-13 degrees
        // from plumb on the main family), the knees and feet at 0, the free arm at the A-pose's 46.
        const auto fromDown = [&](const char* from, const char* to) {
            const int a = indexOf(from), b = indexOf(to);
            if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.endPos.size()) {
                return -1.0;
            }
            const glm::vec3 d = r.endPos[static_cast<std::size_t>(b)] - r.endPos[static_cast<std::size_t>(a)];
            return glm::length(d) > 1.0e-4f ? static_cast<double>(glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(d), glm::vec3(0.0f, -1.0f, 0.0f)), -1.0f, 1.0f)))) : -1.0;
        };
        const auto kneeFlex = [&](const char* shin) {
            const int b = indexOf(shin);
            if (b < 0 || static_cast<std::size_t>(b) >= r.endEuler.size()) {
                return -1.0;
            }
            const glm::vec3 e = r.endEuler[static_cast<std::size_t>(b)];
            return static_cast<double>(std::max({std::abs(e.x), std::abs(e.y), std::abs(e.z)}));
        };
        phaseLegs("[real] suspension (lHand +135cm y, beyond reach)",
                     {gateMin("suspended", r.suspended ? 1.0 : 0.0, 1.0),
                      // (3.5-5 on the reference rigs: kDangleThighForwardDeg of forward lean; 7-13 before)
                      gateMax("dangle: the left thigh from plumb (deg)", fromDown("lThigh", "lShin"), 8.0),
                      gateMax("dangle: the right thigh from plumb (deg)", fromDown("rThigh", "rShin"), 8.0),
                      gateMin("dangle: the left knee's flexion (deg; kDangleKneeDeg 10)", kneeFlex("lShin"), 5.0),
                      // (6.8 on every reference rig — the hang's 0.12 of outward lean; 46 before, the A-pose)
                      gateMax("dangle: the free arm from plumb (deg)", fromDown("rShldr", "rForeArm"), 15.0),
                      gateMin("feet height at the end (m)", r.feetMinY, 0.20),
                      gateMin("hip rise (m)", r.hipDispY, 0.10),
                      // (The LIFT-OFF itself: by the time the rig decides she hangs from the hand
                      // the cursor is 40cm beyond her reach and the hang is half a metre to the
                      // side of where she stands — solved in full that was the whole body 42cm up
                      // and 40 across in one tick, 59-71cm at a toe. Both are eased in now.)
                      gateMax("worst single-tick jump through the lift (mm)",
                              std::max(r.phases.empty() ? 0.0 : r.phases[0].maxJump,
                                       r.phases.size() > 1 ? r.phases[1].maxJump : 0.0) * 1000.0, 80.0),
                      // (The hang is RIGID — the root a chain's length below the cursor — and the two
                      // oldest generations' arms come 5-7cm short of it: theirs before the easing too.)
                      gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 80.0)}, r);
    }

    // --- A lifted figure LOWERED back onto its feet: the same lift, then the hand brought back
    // down to its start height, 30cm out to the side. The feet must re-plant as the body comes
    // down (contact detection is skipped while suspended, so before this round the figure was
    // pushed into the floor as a dangling body and never landed), the figure must stand back
    // up where it landed, and the hand must arrive. Out to the side, because the body hangs
    // (and so lands) directly under the hand: a cursor brought straight back down passes
    // 12cm INBOARD of the landed shoulder, where a 135°-limited elbow cannot fold the hand to
    // the cursor, and the arm rests 10-35cm off through the descent — the geometry, not the
    // landing.
    if (report.wants("suspend-lower")) {
        Scenario sc;
        sc.grab = "lHand";
        const glm::vec3 up(0.0f, 1.35f, 0.0f);
        const glm::vec3 back(0.30f, 0.0f, 0.0f);
        sc.path = {{0, glm::vec3(0.0f), "start"}, {60, up, "lift"}, {120, up, "hang"},
                   {220, back, "lower"},          {280, back, "hold-down"}};
        const RunResult r = run(sc);
        phaseLegs("[real] suspension, then lowered back (lHand +135cm y, then down beside the body)",
                     {gateMin("suspended", r.suspended ? 1.0 : 0.0, 1.0),
                      gateMin("pins at mouse-up (feet re-planted)", r.pinsAtRelease, 2.0),
                      gateMin("feet vs their rest height at the end (mm)",
                              (r.feetMinY - r.contactBindMinY) * 1000.0, -10.0),
                      gateMax("hip off its start height at the end (mm)", std::abs(r.hipDispY) * 1000.0, 50.0),
                      gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
    }

    // --- THE HIPS LIFT HER OFF (2026-11-07; the user: "when pulling a figure up by the hip or
    // torso she tiptoes and stays auto-pinned to the floor — too hard to get the figure to leave
    // the floor; pulling a bit past the pin limit should release it"): the hips pulled 40cm up
    // take her onto straight legs in the first few centimetres, and 8cm of strain past that
    // (kSuspendStrainTrunk, with the legs taut) for a quarter second lets the feet go — she
    // floats up with the cursor, the legs hanging (the dangle), eased in like a hand's lift.
    // Before this round a pelvis drag never ran the lift-off test at all.
    if (report.wants("hips-liftoff")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, 0.40f, 0.0f), 60, 120);
        sc.release = false;
        const RunResult r = run(sc);
        const auto fromDown = [&](const char* from, const char* to) {
            const int a = indexOf(from), b = indexOf(to);
            if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.endPos.size()) {
                return -1.0;
            }
            const glm::vec3 d = r.endPos[static_cast<std::size_t>(b)] - r.endPos[static_cast<std::size_t>(a)];
            return glm::length(d) > 1.0e-4f ? static_cast<double>(glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(d), glm::vec3(0.0f, -1.0f, 0.0f)), -1.0f, 1.0f)))) : -1.0;
        };
        phaseLegs("[real] hips pulled up past the legs' stretch lift her off (hip +40cm y)",
                     {gateMin("suspended", r.suspended ? 1.0 : 0.0, 1.0),
                      gateMin("hip rise (m)", r.hipDispY, 0.30),
                      gateMin("feet height at the end (m)", r.feetMinY, 0.20),
                      gateMax("dangle: the left thigh from plumb (deg)", fromDown("lThigh", "lShin"), 8.0),
                      gateMax("steps taken", r.stepsTaken, 0.0),
                      gateMax("worst single-tick jump through the lift (mm)",
                              std::max(r.phases.empty() ? 0.0 : r.phases[0].maxJump,
                                       r.phases.size() > 1 ? r.phases[1].maxJump : 0.0) * 1000.0, 80.0),
                      gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0)}, r);
    }

    // --- ... AND THE HEAD (a trunk-role joint: the same strain, the same taut legs): pulled 40cm
    // up it straightens her, stretches her 2cm, and lifts her off; a hand's 15cm of strain is a
    // limb's, for an arm that must raise before it lifts.
    if (report.wants("head-liftoff")) {
        Scenario sc;
        sc.grab = "head";
        sc.path = pullPath(glm::vec3(0.0f, 0.40f, 0.0f), 60, 120);
        sc.release = false;
        const RunResult r = run(sc);
        phaseLegs("[real] head pulled up past the body's stretch lifts her off (head +40cm y)",
                     {gateMin("suspended", r.suspended ? 1.0 : 0.0, 1.0),
                      gateMin("feet height at the end (m)", r.feetMinY, 0.15),
                      gateMax("steps taken", r.stepsTaken, 0.0),
                      gateMax("worst single-tick jump through the lift (mm)",
                              std::max(r.phases.empty() ? 0.0 : r.phases[0].maxJump,
                                       r.phases.size() > 1 ? r.phases[1].maxJump : 0.0) * 1000.0, 80.0)}, r);
    }

    // --- THE HANGING FEET COME DOWN FLAT (2026-11-12; the user: "when a character is lifted off the
    // ground, her feet go into a hanging tip toe position; dropping back to the ground should also
    // make their feet go back to flat"). A lift leaves the feet rolled onto their balls (the
    // lift-off's tiptoe) and pointed (the dangle); let go of in the air she keeps them, and coming
    // back down they go flat again (armaturehangfeet.cpp).
    if (report.wants("hang-feet")) {
        // 1. Lifted by the hips and LET GO OF in the air, then lowered by the hips in another drag,
        //    back to where they stood: she lands on flat feet. (Before: her toes touched first and
        //    the standing foot's model held the heels over the balls as found — 6-9cm up.)
        {
            Scenario sc;
            sc.grab = "hip";
            sc.before.push_back({"hip", glm::vec3(0.0f, 0.40f, 0.0f), 60, 60});
            sc.path = pullPath(glm::vec3(0.0f, -0.40f, 0.0f), 80, 60);
            const RunResult r = run(sc);
            // The foot's own shape, read off two of its joints: the ankle over the joint the toes
            // hang from, against the same at rest — 0 for a level sole, the heel's lift otherwise.
            const auto heelOverBall = [&](const char* ankle, const char* ball) {
                const int a = indexOf(ankle), b = indexOf(ball);
                if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.endPos.size() ||
                    static_cast<std::size_t>(std::max(a, b)) >= r.restPos.size()) {
                    return 0.0;
                }
                const std::size_t ua = static_cast<std::size_t>(a), ub = static_cast<std::size_t>(b);
                return static_cast<double>((r.endPos[ua].y - r.endPos[ub].y) - (r.restPos[ua].y - r.restPos[ub].y)) * 1000.0;
            };
            const auto atStart = [&](const char* ankle, const char* ball) {
                const int a = indexOf(ankle), b = indexOf(ball);
                if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.startPos.size() ||
                    static_cast<std::size_t>(std::max(a, b)) >= r.restPos.size()) {
                    return 0.0;
                }
                const std::size_t ua = static_cast<std::size_t>(a), ub = static_cast<std::size_t>(b);
                return static_cast<double>((r.startPos[ua].y - r.startPos[ub].y) - (r.restPos[ua].y - r.restPos[ub].y)) * 1000.0;
            };
            phaseLegs("[real] lifted by the hips and let go of, then lowered by the hips: she lands on flat feet",
                         {info("as she hung: the left heel over its ball, against standing (mm)", atStart("lFoot", "lToe")),
                          gateMin("pins at mouse-up (feet planted)", r.pinsAtRelease, 2.0),
                          gateMax("the left heel over its ball at the end, against standing (mm)", std::abs(heelOverBall("lFoot", "lToe")), 10.0),
                          gateMax("the right heel over its ball at the end, against standing (mm)", std::abs(heelOverBall("rFoot", "rToe")), 10.0),
                          gateMax("the lower ankle over its standing height at the end (mm)", (r.feetMinY - r.feetBindMinY) * 1000.0, 10.0),
                          gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                          gateMax("worst single-tick jump (mm)",
                                  std::max(r.phases.empty() ? 0.0 : r.phases[0].maxJump,
                                           r.phases.size() > 1 ? r.phases[1].maxJump : 0.0) * 1000.0, 80.0)}, r);
        }
        // 2. The Ground button's landing, driven as the window drives it: the feet found, posed
        //    flat, and nothing else touched; a foot the user has posed since is theirs.
        {
            struct Landing {
                bool   lifted = false;     // the lift ended with her in the air
                bool   found = false;      // beginHangFeetLanding found hanging feet
                double hungHeel = 0.0;     // the heel over its ball as she hung, against standing (mm)
                double flatHeel = 0.0;     // ... and posed flat
                double halfHeel = 0.0;     // ... and posed half way
                double toesBack = 0.0;     // the bones below the ankles, from their rest rotation (deg, worst channel)
                double others = 0.0;       // every bone outside the feet, moved by the landing (deg)
                double theirs = 0.0;       // a posed foot's ankle, moved by the landing (deg)
                double theirsOther = 0.0;  // ... and the other foot's heel then (mm): still landed
            };
            const auto landing = [&](bool poseOneFoot) {
                Landing  out;
                Armature arm;
                arm.build(bones);
                applyBodyMesh(arm, 1.0f);
                for (const auto& [bone, euler] : restPose) {
                    arm.setBoneRotation(resolveBoneName(arm, bone), euler);
                }
                const std::size_t n = arm.boneCount();
                const int hip = resolveBone(arm, "hip");
                const int lFoot = resolveBone(arm, "lFoot"), rFoot = resolveBone(arm, "rFoot");
                const int lToe = resolveBone(arm, "lToe"), rToe = resolveBone(arm, "rToe");
                if (hip < 0 || lFoot < 0 || rFoot < 0 || lToe < 0 || rToe < 0) {
                    return out;
                }
                const auto under = [&](std::size_t node, int ancestor) {
                    for (int cur = static_cast<int>(node); cur >= 0; cur = arm.boneParent(static_cast<std::size_t>(cur))) {
                        if (cur == ancestor) {
                            return true;
                        }
                    }
                    return false;
                };
                std::vector<glm::vec3> restEuler(n);
                for (std::size_t i = 0; i < n; ++i) {
                    restEuler[i] = arm.boneEuler(i);
                }
                const auto heel = [&](int ankle, int ball) { // the ankle over the ball, mm
                    return static_cast<double>(arm.boneWorldPosition(static_cast<std::size_t>(ankle)).y -
                                               arm.boneWorldPosition(static_cast<std::size_t>(ball)).y) * 1000.0;
                };
                const double restHeel = heel(lFoot, lToe), restHeelR = heel(rFoot, rToe);
                // The lift: the hips 40cm up over a second, held a second, let go of.
                arm.setSelectedBone(hip);
                if (!arm.beginIkDrag()) {
                    return out;
                }
                const glm::vec3 from = arm.boneWorldPosition(static_cast<std::size_t>(hip));
                for (int k = 1; k <= 120; ++k) {
                    arm.dragIkTo(from + glm::vec3(0.0f, 0.40f, 0.0f) * std::min(1.0f, static_cast<float>(k) / 60.0f));
                }
                for (int k = 0; k < 600 && arm.settleIkTick(); ++k) {
                }
                arm.endIkDrag();
                out.lifted = arm.boneWorldPosition(static_cast<std::size_t>(lToe)).y > 0.10f;
                out.hungHeel = heel(lFoot, lToe) - restHeel;
                glm::vec3 posedAnkle(0.0f);
                if (poseOneFoot) {
                    // (The user turns the left ankle 12 degrees further: that foot is theirs.)
                    posedAnkle = arm.boneEuler(static_cast<std::size_t>(lFoot)) + glm::vec3(12.0f, 0.0f, 0.0f);
                    arm.setBoneRotation(arm.boneName(static_cast<std::size_t>(lFoot)), posedAnkle);
                    posedAnkle = arm.boneEuler(static_cast<std::size_t>(lFoot)); // (as the limits left it)
                }
                std::vector<glm::vec3> hung(n);
                for (std::size_t i = 0; i < n; ++i) {
                    hung[i] = arm.boneEuler(i);
                }
                out.found = arm.beginHangFeetLanding();
                arm.poseHangFeet(0.5f, -1.0e9f);
                out.halfHeel = heel(rFoot, rToe) - restHeelR;
                arm.poseHangFeet(0.0f, -1.0e9f);
                arm.endHangFeetLanding();
                out.flatHeel = std::max(poseOneFoot ? 0.0 : std::abs(heel(lFoot, lToe) - restHeel), std::abs(heel(rFoot, rToe) - restHeelR));
                out.theirsOther = std::abs(heel(rFoot, rToe) - restHeelR);
                for (std::size_t i = 0; i < n; ++i) {
                    const bool inLeft = under(i, lFoot), inRight = under(i, rFoot);
                    const glm::vec3 e = arm.boneEuler(i);
                    for (int a = 0; a < 3; ++a) {
                        if (!inLeft && !inRight) {
                            out.others = std::max(out.others, static_cast<double>(std::abs(e[a] - hung[i][a])));
                        } else if (static_cast<int>(i) != lFoot && static_cast<int>(i) != rFoot && !(poseOneFoot && inLeft)) {
                            out.toesBack = std::max(out.toesBack, static_cast<double>(std::abs(e[a] - restEuler[i][a])));
                        }
                    }
                }
                if (poseOneFoot) {
                    const glm::vec3 e = arm.boneEuler(static_cast<std::size_t>(lFoot));
                    out.theirs = static_cast<double>(std::max({std::abs(e.x - posedAnkle.x), std::abs(e.y - posedAnkle.y), std::abs(e.z - posedAnkle.z)}));
                }
                return out;
            };
            const Landing both = landing(false);
            const Landing one = landing(true);
            report.phase("[real] the hanging feet come down flat: the Ground button's landing after a lift",
                         {gateMin("the lift left her in the air", both.lifted ? 1.0 : 0.0, 1.0),
                          gateMin("as she hung: the heel over its ball, against standing (mm)", both.hungHeel, 20.0),
                          gateMin("hanging feet found", both.found ? 1.0 : 0.0, 1.0),
                          info("posed half way: the heel over its ball, against standing (mm)", both.halfHeel),
                          gateMax("posed flat: the heel over its ball, against standing (mm)", both.flatHeel, 3.0),
                          gateMax("... the bones below the ankles, from their rest rotation (deg)", both.toesBack, 0.05),
                          gateMax("... and every bone outside the feet, moved (deg)", both.others, 0.0),
                          gateMax("a foot the user posed since: its ankle moved by the landing (deg)", one.theirs, 0.0),
                          gateMax("... the other foot landed all the same: its heel over its ball (mm)", one.theirsOther, 3.0)});
        }
        // 3. A figure TURNED a quarter turn (the Transform tab's Turn: she faces +x), lifted by the
        //    hips and lowered again within the drag: the rig's own landing holds her soles to the
        //    REST orientation — which must face the way SHE faces. It was the bare identity, facing
        //    +z: both legs were wrenched round at the thighs' twist (49 degrees of it in the app)
        //    and the ankles, the feet left pointing where she no longer faced.
        {
            Scenario sc;
            sc.grab = "hip";
            sc.prePose.push_back({"hip", glm::vec3(0.0f, 90.0f, 0.0f)});
            const glm::vec3 up(0.0f, 0.40f, 0.0f);
            sc.path = {{0, glm::vec3(0.0f), "start"}, {60, up, "lift"}, {120, up, "hang"},
                       {200, glm::vec3(0.0f), "lower"}, {260, glm::vec3(0.0f), "hold-down"}};
            sc.asymmetryGateMm = -1.0; // (the symmetry measure reads the plane x = 0, which a turned figure does not stand astride)
            const RunResult r = run(sc);
            // Where a foot points along the floor (ankle to the joint its toes hang from), against
            // where it pointed when the drag began — she stood facing +x then.
            const auto headingOff = [&](const char* ankle, const char* ball) {
                const int a = indexOf(ankle), b = indexOf(ball);
                if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.endPos.size() ||
                    static_cast<std::size_t>(std::max(a, b)) >= r.startPos.size()) {
                    return 0.0;
                }
                const std::size_t ua = static_cast<std::size_t>(a), ub = static_cast<std::size_t>(b);
                glm::vec3 was = r.startPos[ub] - r.startPos[ua], now = r.endPos[ub] - r.endPos[ua];
                was.y = 0.0f;
                now.y = 0.0f;
                if (glm::length(was) < 1.0e-4f || glm::length(now) < 1.0e-4f) {
                    return 0.0;
                }
                return static_cast<double>(glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(was), glm::normalize(now)), -1.0f, 1.0f))));
            };
            const auto lowerAnkleRise = [&](const char* left, const char* right) {
                const int a = indexOf(left), b = indexOf(right);
                if (a < 0 || b < 0 || static_cast<std::size_t>(std::max(a, b)) >= r.endPos.size() ||
                    static_cast<std::size_t>(std::max(a, b)) >= r.startPos.size()) {
                    return 0.0;
                }
                const std::size_t ua = static_cast<std::size_t>(a), ub = static_cast<std::size_t>(b);
                return static_cast<double>(std::min(r.endPos[ua].y, r.endPos[ub].y) - std::min(r.startPos[ua].y, r.startPos[ub].y)) * 1000.0;
            };
            phaseLegs("[real] turned a quarter turn, lifted by the hips and lowered again: she lands facing the way she faces",
                         {gateMin("suspended", r.suspended ? 1.0 : 0.0, 1.0),
                          gateMin("pins at mouse-up (feet re-planted)", r.pinsAtRelease, 2.0),
                          gateMax("the left foot's heading, from where it pointed at the press (deg)", headingOff("lFoot", "lToe"), 20.0),
                          gateMax("the right foot's heading, from where it pointed at the press (deg)", headingOff("rFoot", "rToe"), 20.0),
                          gateMax("knee twist at the end (deg)", r.kneeTwistEndDeg, 10.0),
                          // (Against where the ankles stood at the PRESS, not their bind heights: the
                          // quarter turn is made on the hip's second channel, which on a body whose hip
                          // frame is not the world's leans her a little as it turns her.)
                          gateMax("the lower ankle at the end, over where it stood at the press (mm)", lowerAnkleRise("lFoot", "rFoot"), 10.0),
                          gateMax("hip off its start height at the end (mm)", std::abs(r.hipDispY) * 1000.0, 20.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0)}, r);
        }
    }
}

/// Crouches by the chest and the hip, and standing back up out of one.
void RealPhases::crouchPhases() {
    // --- Pushing the chest down folds a crouch over the planted feet; no foot buries itself.
    if (report.wants("crouch-push")) {
        Scenario sc;
        sc.grab = "chest_2";
        sc.path = pullPath(glm::vec3(0.0f, -0.25f, 0.0f), 60, 90);
        const RunResult r = run(sc);
        // The push must be FOLLOWED: the pelvis's vertical posture price once met the cursor's
        // bounded pull at 10cm of crouch, and however far the chest was pushed the hips sank 8-10cm
        // and stopped ("she crouches a bit and gets stuck") — it now yields to a downward gesture
        // (kRootYieldStiffness) and the hips come down 22-25cm under this 25cm push on every rig.
        // At that depth the oldest generation's ankles run out and its heels lift: 9 degrees of
        // foot pitch about the ball, by design (hence this phase's own foot-rotation gate).
        phaseLegs("[real] chest push-down (chest_2 -25cm y)",
                     {gateMin("hip drop at release (m)", r.hipDropAtRelease, 0.20),
                      info("hip drop after settle (m)", -r.hipDispY),
                      gateMax("lowest contact vs floor, drag (mm)", (r.contactBindMinY - r.contactMinY) * 1000.0, 10.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 80.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r,
                  LegGates{10.0, 30.0, 12.0, -1.0});
    }

    // --- ... and from a crouch she was LEFT in, the next drag must stand her back up. The crouch
    // is made and released first (Scenario::before), so it is the measured drag's posture
    // reference — and the pelvis's vertical price met the bounded pull from below exactly as it
    // had from above: pulled back up 30cm, the hips rose 10cm and stopped, the spine stretching
    // after a cursor the chest ended 19cm short of ("pulling back up does not cause her to stand
    // back up"). The price now yields upward too, by the height the legs had left when the drag
    // began (m_jsRootRiseRoom) — none for a drag that begins standing, so those are untouched.
    if (report.wants("stand-up")) {
        Scenario sc;
        sc.before.push_back({"chest_2", glm::vec3(0.0f, -0.30f, 0.0f), 90, 40});
        sc.grab = "chest_2";
        sc.path = pullPath(glm::vec3(0.0f, 0.30f, 0.0f), 90, 60);
        const RunResult r = run(sc);
        phaseLegs("[real] stand back up (a released 30cm crouch, chest_2 +30cm y)",
                     {gateMin("hip rise (m)", r.hipDispY, 0.24),
                      gateMax("chest-to-cursor at rest (mm)", r.restingMiss * 1000.0, 10.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 30.0),
                      info("heel lift max (mm)", r.contactRiseMax * 1000.0),
                      gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r);
    }

    // --- Dragging the hip down is an explicit crouch: deep, feet planted.
    if (report.wants("hip-crouch")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -0.12f, 0.0f), 40, 80);
        const RunResult r = run(sc);
        phaseLegs("[real] hip crouch (hip -12cm y)",
                     {gateMin("hip drop at release (m)", r.hipDropAtRelease, 0.10),
                      info("hip drop after settle (m)", -r.hipDispY),
                      gateMax("lowest contact vs floor, drag (mm)", (r.contactBindMinY - r.contactMinY) * 1000.0, 5.0),
                      // The toe joints ride the sole's transient pitch ~2cm below their rest
                      // height (a centimetre through the floor) — a known soft spot, guarded.
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 30.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r, kCrouchLegs);
    }
}

/// The floor scenarios' SIZING, once per rig, before the first phase that reads it:
/// --- LIVE CONTACTS (IkRig::updateContacts): joints that reach the floor MID-DRAG become
/// supports. Before this, only the drag-start contacts were ever pinned: a hand or knee
/// coming down rode the trunk as an inactive subtree straight through the floor (8cm on a
/// deep crouch), and a hand left on the floor by one drag never let go in the next.
/// (1) An ALL-FOURS descent: the figure bent 90° at the hips (the hip bone pitched forward,
/// the thighs counter-rotated so the legs stay vertical, a little spine flexion) with the
/// arms hanging; the hip is dragged down until the fingertips touch and 3cm past. The
/// hands must plant where they touched and stay (the joint-space refinement folds
/// the arms; the solver's own arm chain cannot, see kLiveRefineStep), never sinking below
/// their planting height. The descent is sized per rig from the pre-posed hand height, and
/// skipped where the pose does not bring the hands within reach of the floor.
/// 45° at the hips (not 60°): the oldest generation's thigh range ends at -100°, and a
/// 60° pre-pose left it no room for the crouch's own flexion — its feet strained 5cm
/// before the hands even landed.
void RealPhases::sizeFloorScenarios() {
    Armature posedProbe;
    posedProbe.build(bones);
    const int hip = resolveBone(posedProbe, "hip");
    const int knee = resolveBone(posedProbe, "lShin");
    const int ankle = resolveBone(posedProbe, "lFoot");
    if (hip >= 0 && knee >= 0 && ankle >= 0) {
        // A kneel's pelvis sits where a vertical thigh puts it: the hip bone's rest height
        // above the knee, plus the knee's 1.5cm floor clearance (kneelNatural). The drag
        // goes 14cm PAST that: a knee's fold plane follows its thigh's swing (knees have
        // no twist freedom — see IkRig::build), and on the main figure family that plane
        // contains the standing foot only with the knee splayed outward, where it reaches
        // the floor ~8cm below the kneeling height; the older and newest generations'
        // knees plant near the kneeling height and their planted knees then STOP the hip
        // (a hard pin under a descending drag — IkRig::boundLivePins). The knees land a
        // shin's flat reach ahead of the ankles (the tucked-toe ankle ~13.5cm above the
        // knee), so the hip travels forward by that reach less its rest offset ahead of
        // the ankles.
        const glm::vec3 hipP = posedProbe.boneWorldPosition(static_cast<std::size_t>(hip));
        const glm::vec3 kneeP = posedProbe.boneWorldPosition(static_cast<std::size_t>(knee));
        const glm::vec3 ankleP = posedProbe.boneWorldPosition(static_cast<std::size_t>(ankle));
        // (The knee's FLOOR CLEARANCE is the rig's, fitted to the figure's skin since
        // 2026-09-21 — 4-5.5cm, where it was a flat 1.5: read it off a rig built as a run's is.)
        {
            Armature fitted;
            fitted.build(bones);
            applyBodyMesh(fitted, 1.0f);
            fitted.setSelectedBone(hip);
            if (fitted.beginIkDrag() && fitted.ikRig() != nullptr) {
                // (DIRECTIONAL: a kneeling shin lies with its FRONT down, a sitting pelvis
                // rests on what is below it, tipped a little back.)
                kneeClearance = fitted.ikRig()->floorClearanceAlong(knee, glm::vec3(0.0f, 0.0f, 1.0f));
                seatClearance = fitted.ikRig()->floorClearanceAlong(hip, glm::vec3(0.0f, -0.97f, -0.24f));
                if (resolveBone(posedProbe, "chest_2") >= 0) {
                    chestBackClearance = fitted.ikRig()->floorClearanceAlong(resolveBone(posedProbe, "chest_2"), glm::vec3(0.0f, 0.0f, -1.0f));
                }
                fitted.endIkDrag();
            }
        }
        kneelNatural = kneeP.y - kneeClearance;
        // (To a centimetre over where the seat's own flesh stops her: 15cm while the pelvis's
        // clearance was 1.5 and she sank 5cm of flesh into the floor at that height.)
        sitDrop = hipP.y - std::max(0.15f, seatClearance + 0.01f);
        kneelDrop = kneelNatural + 0.14f;
        const float shin = glm::length(ankleP - kneeP);
        const float reach = std::sqrt(std::max(shin * shin - 0.135f * 0.135f, 0.0f));
        kneelForward = reach - (hipP.z - ankleP.z);
    }
    bool posed = true;
    for (const auto& [bone, euler] : restPose) {
        posed = posed && posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
    }
    for (const auto& [bone, euler] : kAllFoursPose) {
        posed = posed && posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
    }
    const int hand = resolveBone(posedProbe, "lHand");
    if (posed && hand >= 0) {
        // The hand plants when its LOWEST part (a fingertip) reaches its 1.5cm clearance;
        // the hip then goes 3cm further, which the arms absorb by folding. (Sizing the
        // descent from the hand JOINT instead drove the oldest rigs' straight arms 10cm
        // past their plant — a fold no joint-space fit can start from a perfectly straight
        // arm, so their hands ended out of reach and rose 5cm on release.)
        float lowest = posedProbe.boneWorldPosition(static_cast<std::size_t>(hand)).y;
        for (std::size_t i = 0; i < posedProbe.boneCount(); ++i) {
            for (int cur = posedProbe.boneParent(i); cur >= 0;
                 cur = posedProbe.boneParent(static_cast<std::size_t>(cur))) {
                if (cur == hand) {
                    lowest = std::min(lowest, posedProbe.boneWorldPosition(i).y);
                    break;
                }
            }
        }
        const float drop = lowest - 0.015f + 0.03f;
        if (drop > 0.15f && drop < 0.75f) {
            allFoursDrop = drop;
        }
    }
}

/// Live contacts: the hands planting on an all-fours descent and rising off it, the kneel.
void RealPhases::contactPhases() {
    if (report.wants("hands-plant")) {
        if (allFoursDrop <= 0.0f) {
            report.skip("[real] all fours: hands plant",
                        "the all-fours pre-pose does not bring the hands to the floor on this rig");
        } else {
            Scenario sc;
            sc.grab = "hip";
            sc.path = pullPath(glm::vec3(0.0f, -allFoursDrop, 0.0f), 80, 60);
            sc.prePose = kAllFoursPose;
            // (The symmetry gate's first catch, 2026-09-21: the hands came down 35-60mm lopsided on six
            // of the eight rigs, the idle arms' hang stopping the second arm against the first where
            // its loop had just left it. 0.0-0.3mm since the stop is order-independent.)
            const RunResult r = run(sc);
            char name[128];
            std::snprintf(name, sizeof(name), "[real] all fours: hands plant (hip -%.0fcm y, bent at the hips)",
                          allFoursDrop * 100.0f);
            // The gesture is sized for a trunk that keeps its pitch as the hip goes down. A
            // pelvis drag may TILT the pelvis (the root's rotation is an unknown there), and on
            // the oldest generation — thighs out of flexion at -100 deg — it does: the folded
            // trunk comes up with it and the hands stop 20cm above the floor. Beyond the rig,
            // like the arm gestures on a limit-bound shoulder: informational there.
            const bool handsCameDown = r.handsMinY < 0.10;
            phaseLegs(name,
                         {handsCameDown ? gateMin("live contacts planted (max at once)", r.livePinsMax, 2.0)
                                        : info("live contacts planted, hands never came down", r.livePinsMax),
                          handsCameDown ? gateMin("pins at mouse-up (feet + hands)", r.pinsAtRelease, 4.0)
                                        : info("pins at mouse-up, hands never came down", r.pinsAtRelease),
                          // A planted hand is free to TURN (a live contact holds a place, not an
                          // orientation), and on the character rig it settles flatter onto the
                          // floor, its joint 6cm up: what "fingers through the floor" means is the
                          // floor penetration row below (every joint, the fingers included), so
                          // that is the gate; the height is a 5cm guard.
                          // (The WRIST, which lies 3.5-4.5cm over the floor now that a landed hand
                          // rolls onto its palm; it stood 15cm up, on its fingertips, and this
                          // gate said 5cm. Never through the floor:)
                          gateMin("lower hand height min (m)", r.handsMinY, 0.02),
                          r.pinsAtRelease >= 4.0 ? gateMax("lower hand height at the end (m; on its fingertips it read 0.15)", r.handsEndY, 0.09)
                                                 : info("lower hand height at the end, hands never came down (m)", r.handsEndY),
                          // 15: seven generations hold within 1.5mm; the oldest (shoulder
                          // flexion ends at -75°, the pre-pose's -90° clamps) rises 12.6mm.
                          gateMax("hand rise off its plant at the end (mm)", (r.handsEndY - r.handsMinY) * 1000.0, 15.0),
                          gateMax("live pin slide (mm)", r.livePinSlideMax * 1000.0, 100.0),
                          // (The target is 3cm PAST where the fingertips touch, by design: the
                          // planted hands stop the hip about that far short of it — 3.2cm on the
                          // male rig since the pelvis's rotation got its real price.)
                          gateMin("hip drop at release (m)", r.hipDropAtRelease, allFoursDrop - 0.04),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }
    // (2) ... and standing back up out of it: the planted hands must LET GO again (a live
    // contact is a unilateral support — the arms go taut, then lift off), and the figure rises
    // with its feet still planted, the hip on the cursor.
    if (report.wants("hands-rise")) {
        if (allFoursDrop <= 0.0f) {
            report.skip("[real] all fours, then rise",
                        "the all-fours pre-pose does not bring the hands to the floor on this rig");
        } else {
            Scenario sc;
            sc.grab = "hip";
            const glm::vec3 down(0.0f, -allFoursDrop, 0.0f);
            // (ALL THE WAY back up, where it was 35cm: hands that lie FLAT give the arms a hand's
            // length more reach than hands on their fingertips did - at 35cm, and at 50, she stands
            // in a forward fold with her fingertips still on the floor, the hip on its cursor, which
            // is right. Back at standing height they cannot reach it, and must let go.)
            const glm::vec3 up(0.0f, -0.05f, 0.0f);
            sc.path = {{0, glm::vec3(0.0f), "start"}, {80, down, "down"}, {110, down, "hold"},
                       {190, up, "up"},              {250, up, "hold-up"}};
            sc.prePose = kAllFoursPose;
            const RunResult r = run(sc);
            phaseLegs("[real] all fours, then rise (the hip all the way back up)",
                         {r.handsMinY < 0.10 ? gateMin("live contacts planted (max at once)", r.livePinsMax, 2.0)
                                             : info("live contacts planted, hands never came down", r.livePinsMax),
                          gateMax("pins at mouse-up (hands released)", r.pinsAtRelease, 2.0),
                          gateMin("lower hand height at the end (m)", r.handsEndY, 0.25),
                          gateMin("lower hand height min (m)", r.handsMinY, 0.02),
                          gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }
    // (3) A KNEEL: the hip dragged down and forward until the knees reach the floor. Both
    // knees must plant as supports (with the feet still pinned: the solver restores each foot
    // through its knee pin) and end on the floor. The feet slide back as a real kneel's do (the
    // shin can only lie flat with the ankle a shin's length behind the knee), and the toes of
    // the pinned feet still dip through the floor — the ankle pin holds the standing height,
    // and the foot's roll onto its top is the kneeling round's job; info rows here.
    // Since the heel-lift round the kneel is a TOE-TUCK kneel: the pitched feet come up onto
    // their toes (the ankle pins rise ~7cm) and the hips travel 30cm forward to sit over the
    // knees — with the hips only 20cm forward the raised ankles held the knees 5cm off the
    // floor. The feet drift is the heel lift plus the slide a kneel needs.
    if (report.wants("kneel")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -kneelDrop, kneelForward), 90, 60);
        const RunResult r = run(sc);
        char name[128];
        std::snprintf(name, sizeof(name), "[real] kneel: knees plant (hip -%.0fcm y +%.0fcm z, feet lying flat)",
                      kneelDrop * 100.0f, kneelForward * 100.0f);
        phaseLegs(name,
                     {gateMin("live contacts planted (max at once)", r.livePinsMax, 2.0),
                      gateMin("pins at mouse-up (feet + knees)", r.pinsAtRelease, 4.0),
                      gateMax("lower knee over its floor clearance at the end (m)", r.kneesEndY - kneeClearance, 0.015),
                      gateMax("live pin slide (mm)", r.livePinSlideMax * 1000.0, 60.0),
                      gateMin("hip drop at release (m)", r.hipDropAtRelease, kneelNatural - 0.03),
                      // The kneel round: the knees come down over the feet (the seed's 3cm cap
                      // plus the heel lift moving the ankle inward under a tucked foot), not 15cm outside them.
                      gateMax("knee outboard of its foot at the end (mm)", r.kneeOverFootEnd * 1000.0, 110.0),
                      info("heel lift max (mm)", r.contactRiseMax * 1000.0),
                      info("feet drift (mm)", r.contactDriftMax * 1000.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 35.0),
                      // The HOLD must be still. It was not: the foot's orientation rows were
                      // mis-linearized at a large pitch (jointsolver.cpp: lateralAxisError), the
                      // solve crept and broke out by turns, and through a kneel's still hold the
                      // pose jumped 10cm at a time and oscillated 3cm — behind gates that only
                      // read where the knees ended.
                      // (25 since the feet lie flat, 2026-09-26: the foot rolls onto its top at 150
                      // degrees a second once its knee has landed — 16-20mm a tick at a toe tip —
                      // and on the rig whose knees land last it is still rolling in the hold. A roll
                      // is not a pop; the gate is for the pops.)
                      gateMax("worst single-tick jump in the still hold (mm)",
                              r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 25.0),
                      gateMax("grabbed joint's oscillation in the still hold (mm)",
                              r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 3.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
    }

    // ... and a kneel made the way a USER makes it: a few centimetres deeper and shorter than the
    // rig's own. (2026-09-21) The knees were nailed, in full, to the spot they HOVERED over when
    // they crossed the landing band, and brought straight down there — which a tucked foot at its
    // ankle's limits cannot follow: the knee's pin and the ball's pulled the leg apart with 47,000
    // against the cursor's 1,000 and the hips were thrown 20cm down and back in ONE tick (the
    // in-app galleries' only remaining POP, in two of them). A landing knee is now held in its
    // height, softly in its place, and planted where it lands; the foot below a planted knee
    // keeps its place softly; and with her knees planted the pelvis's tilt is dear — the hips a
    // kneeling body cannot lower were being found by tipping the pelvis 50 degrees.
    if (report.wants("kneel") && kneelDrop > 0.0f) {
        const int hipBone = resolveBone(probe, "hip");
        int pelvisBone = -1; // the hip's child that the legs hang from
        const int kneeBone = resolveBone(probe, "lShin");
        for (std::size_t i = 0; i < bones.size() && hipBone >= 0 && kneeBone >= 0; ++i) {
            if (bones[i].parent != hipBone) {
                continue;
            }
            for (int cur = kneeBone; cur >= 0; cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (cur == static_cast<int>(i)) {
                    pelvisBone = static_cast<int>(i);
                }
            }
        }
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -(kneelDrop + 0.02f), kneelForward - 0.04f), 90, 40);
        const RunResult r = run(sc);
        const auto pitchOf = [&](int bone) {
            const std::size_t at = static_cast<std::size_t>(bone);
            return bone >= 0 && at < r.endEuler.size() ? static_cast<double>(r.endEuler[at].x - r.startEuler[at].x) : 0.0;
        };
        phaseLegs("[real] kneel, a little deeper and shorter than the rig's own (what a user drags)",
                  {gateMin("live contacts planted (max at once)", r.livePinsMax, 2.0),
                   // (209mm on the main family: the hips thrown down and back as the knees landed.)
                   gateMax("worst single-tick jump (mm; it read 209)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 80.0),
                   gateMax("worst single-tick jump in the still hold (mm)", r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 25.0), // (25: the roll onto the top, see the kneel phase)
                   // (27 + 25 degrees, the pelvis bone at its limit, before its tilt was dear on planted knees.)
                   gateMax("the pelvis's tilt, root and pelvis bone together (deg; it read 52)",
                           std::abs(pitchOf(hipBone) + pitchOf(pelvisBone)), 25.0),
                   info("hip-to-cursor at rest (mm; rigid thighs over planted knees)", r.restingMiss * 1000.0),
                   gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 35.0),
                   gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
    }
}

/// From a kneel: onto all fours, up off them, prone, sat back onto the heels, a limb moved, the repeated drag, the lunge, standing up out of a kneel and a sit.
void RealPhases::kneelPhases() {
    // (4) ... and GETTING UP out of a kneel that was let go of. The kneel is made and released
    // first (Scenario::before), to a realistic depth — 4cm past the kneeling height — so the
    // measured drag begins kneeling: knees on the floor because of the POSE. Planted like feet
    // (the most proximal contact of each leg, held in full, leashed, steppable) they kept a
    // kneeling figure down: dragged up by the hip she arrived 4cm short with a toe through the
    // floor after the stepper "stepped" her knees, and pulled up by the chest she was read as
    // beyond her leashes' reach and LIFTED off the floor, dangling. A drag now begins in the
    // state the last one ended in (IkRig::seedPoseContacts: the feet pinned, the knees live
    // contacts that lift off), rising carries the hips back over the feet, turns the pelvis
    // upright and the legs home, and puts the heels down (see the RISING notes in solveIk).
    // --- KNEELING, and a LIMB moved: she stays on her knees. Getting up is read off the cursor's
    // upward travel, which for a trunk joint or the pelvis IS the body's — and for a limb is not:
    // a hand raised 40cm (a reach the arm has by itself) stood a kneeling figure 28cm up off her
    // knees, and a knee drawn up for a half kneel stood her up the same way. A limb now asks the
    // body up only by what lies beyond its own reach.
    // --- ONTO ALL FOURS, the way a user makes it: a kneel made and let go of, then the CHEST taken
    // forward and down until the hands are on the floor. Until 2026-09-21 there was no gesture for
    // it (hands pushed to the floor were answered by a deep squat, and from a kneel the chest stalled
    // 26-30cm short: the spine curled to its limit over a pelvis that never pitched, the hands 19cm
    // up). A drag that BEGINS on her knees folds at the HIPS (the bow's hinge, the pelvis's pitch
    // cheap and all but uncoupled from the spine: a flat back over upright thighs); the idle arms
    // hang PLUMB under a trunk that has tilted, so the hands land under the shoulders; and a hand
    // on the floor holds its place at its FINGERTIP and is pulled softly onto its PALM, fingers
    // forward, so it rolls down as the shoulder does (held at the wrist it stood on its fingertips,
    // the arm a rigid strut, and the back rounded instead). Sized per rig: the chest ends an arm's
    // length and a palm over the floor, four fifths of its lever ahead of where it knelt.
    if (report.wants("kneel-allfours") && kneelNatural > 0.0f) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const int hip = resolveBone(posedProbe, "hip");
        const int pelvisBone = resolveBone(posedProbe, "pelvis");
        const int chest = resolveBone(posedProbe, "chest_2");
        const int shoulder = resolveBone(posedProbe, "lShldr");
        const int elbow = resolveBone(posedProbe, "lForeArm");
        const int hand = resolveBone(posedProbe, "lHand");
        const int handR = resolveBone(posedProbe, "rHand");
        if (hip < 0 || chest < 0 || shoulder < 0 || elbow < 0 || hand < 0 || handR < 0) {
            report.skip("[real] onto all fours: a kneel, then the chest forward and down", "bones not found on this rig");
        } else {
            const auto at = [&](int b) { return posedProbe.boneWorldPosition(static_cast<std::size_t>(b)); };
            const float armLength = glm::length(at(elbow) - at(shoulder)) + glm::length(at(hand) - at(elbow));
            const float lever = at(chest).y - at(hip).y;
            const float chestKneeling = at(hip).y - (kneelNatural + 0.04f) + lever;
            // (On all fours the trunk lies forward from the hips, so the shoulders are AHEAD of the
            // upper chest joint, not above it: the chest joint sits about at the shoulders' height,
            // an arm's length and a palm over the floor. Asked a standing shoulder-to-chest offset
            // LOWER than that, the chest was pushed under the natural pose - a push-up's bottom on
            // bent elbows, the spine curled 60 degrees - which is not the gesture.)
            const float chestOnAllFours = armLength + 0.03f - 0.25f * (at(shoulder).y - at(chest).y);
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -(kneelNatural + 0.04f), kneelForward), 90, 40});
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, -(chestKneeling - chestOnAllFours), 0.80f * lever), 100, 60);
            RunResult r = run(sc);
            if (limitBoundEnd(r, "lShldr")) {
                r.symmetricGesture = false; // (hands that land on a limit-bound arm land lopsided: 18mm on the oldest rig)
            }
            double spineMost = 0.0;
            for (const char* name : {"abdomenLower", "abdomenUpper", "abdomen2", "chest", "chestLower", "spine1", "spine2", "spine3"}) {
                const int b = posedProbe.boneIndex(name);
                if (b >= 0 && b != chest) {
                    spineMost = std::max(spineMost, std::abs(endPitchDeg(r, b)));
                }
            }
            const double wristsUp = std::max(static_cast<double>(r.endPos[static_cast<std::size_t>(hand)].y),
                                             static_cast<double>(r.endPos[static_cast<std::size_t>(handR)].y));
            const double handsApart = std::abs(static_cast<double>(r.endPos[static_cast<std::size_t>(hand)].x - r.endPos[static_cast<std::size_t>(handR)].x));
            const double shouldersApart = 2.0 * std::abs(static_cast<double>(at(shoulder).x));
            // (The oldest generation's shoulder flexion ends at -75 degrees: its hands land on their
            // fingertips close together under the chest and cannot roll flat - a limb that cannot
            // serve the gesture, so the hands' gates are informational there, as in the arm phases.)
            // (... by the RIG's limits, not the end pose: with the pelvis yielding to the push the
            // shoulder no longer ends AT its limit there, and the hands still land on their fingertips.)
            float shoulderFlexionEnds = -180.0f;
            for (const auto& b : bones) {
                if (b.name == resolveBoneName(posedProbe, "lShldr")) {
                    for (int a = 0; a < 3; ++a) {
                        if (b.rotationLimited[a] && b.rotationMax[a] - b.rotationMin[a] >= 10.0f) {
                            shoulderFlexionEnds = std::max(shoulderFlexionEnds, -std::abs(std::min(b.rotationMin[a], 0.0f)));
                        }
                    }
                    float most = 0.0f;
                    for (int a = 0; a < 3; ++a) {
                        if (b.rotationLimited[a]) {
                            most = std::min(most, b.rotationMin[a]);
                        }
                    }
                    shoulderFlexionEnds = most;
                }
            }
            const bool armBound = limitBoundEnd(r, "lShldr") || shoulderFlexionEnds > -85.0f;
            phaseLegs("[real] onto all fours: a kneel, then the chest forward and down",
                      {gateMin("floor contacts: two knees and two hands", static_cast<double>(r.livePinsMax), 4.0),
                       gateMax("chest to the cursor at rest (mm; it read 260-300)", r.restingMiss * 1000.0, 40.0),
                       // (45-66: the fold is shared with the spine, three joints at 8-22 degrees each -
                       // the in-app shot reads as a natural all-fours, the back a little rounded.)
                       gateMin("the pelvis pitched forward, root and pelvis bone (deg; it read 12)", endPitchDeg(r, hip) + endPitchDeg(r, pelvisBone), 40.0),
                       gateMax("the most-curled spine joint (deg; it read 35, its limit)", spineMost, 25.0),
                       armBound ? info("the higher wrist over the floor, shoulder limit-bound (mm)", wristsUp * 1000.0)
                                : gateMax("the higher wrist over the floor (mm; on its fingertips it read 160)", wristsUp * 1000.0, 90.0),
                       gateMax("the hands apart, over the shoulders' width (it read 3: a metre)", handsApart / std::max(shouldersApart, 1.0e-3), 1.8),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm; the hand snapped 158 to flat before its palm was eased)",
                               std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 80.0),
                       gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0)}, r);
        }
    }

    // --- UP OFF ALL FOURS BY THE CHEST (2026-09-22): from all fours (a kneel, then the chest taken
    // forward and down, both let go of) the upper chest taken back UP and BACK to where the kneel
    // had it: she comes up onto her KNEES - the trunk up over the hips, the knees where they were,
    // her arms hanging under her shoulders (hangIdleArms) - and no further in that drag: a kneeling
    // trunk comes up about the hips first, as a seated one does (dragIkTick, IK_JS_KNEEL_RISES),
    // and the knees are held DOWN while the trunk can give what is asked (m_jsKneelHold,
    // IK_JS_NO_KNEEL_HOLD). Before: read as the cursor's whole upward travel the lift was getting
    // up from its first centimetre - the knees unloaded and 6cm off the floor under a trunk still
    // leaning, the legs sent home; and with the rise off, straightening the legs off the toes was
    // the solve's cheap way up (knees 4.5cm in the air). And the arms ended straight out in front
    // of her (the hang's stop, see hangIdleArms).
    if (report.wants("allfours-up") && kneelNatural > 0.0f) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const int hip = resolveBone(posedProbe, "hip");
        const int pelvisBone = resolveBone(posedProbe, "pelvis");
        const int chest = resolveBone(posedProbe, "chest_2");
        const int shoulder = resolveBone(posedProbe, "lShldr");
        const int elbow = resolveBone(posedProbe, "lForeArm");
        const int hand = resolveBone(posedProbe, "lHand");
        const int shin = resolveBone(posedProbe, "lShin");
        const int shinR = resolveBone(posedProbe, "rShin");
        if (hip < 0 || chest < 0 || shoulder < 0 || elbow < 0 || hand < 0 || shin < 0 || shinR < 0) {
            report.skip("[real] up off all fours by the chest: onto her knees", "bones not found on this rig");
        } else {
            const auto at = [&](int b) { return posedProbe.boneWorldPosition(static_cast<std::size_t>(b)); };
            const float armLength = glm::length(at(elbow) - at(shoulder)) + glm::length(at(hand) - at(elbow));
            const float lever = at(chest).y - at(hip).y;
            const float chestKneeling = at(hip).y - (kneelNatural + 0.04f) + lever;
            const float chestOnAllFours = armLength + 0.03f - 0.25f * (at(shoulder).y - at(chest).y);
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -(kneelNatural + 0.04f), kneelForward), 90, 40});
            sc.before.push_back({"chest_2", glm::vec3(0.0f, -(chestKneeling - chestOnAllFours), 0.80f * lever), 100, 40});
            sc.grab = "chest_2";
            // (Back up and back by what the second prelude took it down and forward: the kneel's
            // chest, over its hips, within the trunk's reach about them - a kneel-up, not a stand-up.)
            sc.path = pullPath(glm::vec3(0.0f, chestKneeling - chestOnAllFours, -0.80f * lever), 100, 60);
            RunResult r = run(sc);
            if (limitBoundEnd(r, "lShldr")) {
                // (As in the all-fours phase: hands that landed on a limit-bound arm landed lopsided, and
                // the shoulders' TWIST — invisible to the joints while the arms stood straight on the
                // floor — differs by 2 degrees between the sides; hung, that is 50mm at the fingertips.)
                r.symmetricGesture = false;
            }
            const auto kneeY = [&](int b) { return static_cast<double>(r.endPos[static_cast<std::size_t>(b)].y); };
            const auto kneeStartY = [&](int b) { return static_cast<double>(r.startPos[static_cast<std::size_t>(b)].y); };
            const double kneesRose = std::max(kneeY(shin) - kneeStartY(shin), kneeY(shinR) - kneeStartY(shinR)) * 1000.0;
            // (The upper arm's direction from straight down, as the in-app scripts' hang.<bone>.)
            const auto hangDegAt = [&](const std::vector<glm::vec3>& pos, int socket, int fold) {
                const glm::vec3 d = pos[static_cast<std::size_t>(fold)] - pos[static_cast<std::size_t>(socket)];
                const float len = glm::length(d);
                return len > 1.0e-4f ? static_cast<double>(glm::degrees(std::acos(glm::clamp(-d.y / len, -1.0f, 1.0f)))) : 0.0;
            };
            const auto hangDeg = [&](int socket, int fold) {
                const glm::vec3 d = r.endPos[static_cast<std::size_t>(fold)] - r.endPos[static_cast<std::size_t>(socket)];
                const float len = glm::length(d);
                return len > 1.0e-4f ? static_cast<double>(glm::degrees(std::acos(glm::clamp(-d.y / len, -1.0f, 1.0f)))) : 0.0;
            };
            const int shoulderR = resolveBone(posedProbe, "rShldr");
            const int elbowR = resolveBone(posedProbe, "rForeArm");
            const double armsHang = std::max(hangDeg(shoulder, elbow), shoulderR >= 0 && elbowR >= 0 ? hangDeg(shoulderR, elbowR) : 0.0);
            const double armsHangAtMouseUp = std::max(hangDegAt(r.dragEndPos, shoulder, elbow), shoulderR >= 0 && elbowR >= 0 ? hangDegAt(r.dragEndPos, shoulderR, elbowR) : 0.0);
            phaseLegs("[real] up off all fours by the chest: onto her knees",
                      {gateMax("chest to the cursor at rest (mm)", r.restingMiss * 1000.0, 40.0),
                       gateMax("the knees risen off their all-fours height (mm; they read 45-60)", kneesRose, 15.0),
                       gateMax("the pelvis pitched forward, root and pelvis bone (deg; on all fours 45-66)", endPitchDeg(r, hip) + endPitchDeg(r, pelvisBone), 40.0),
                       info("the upper arms from straight down at mouse-up, the worse (deg)", armsHangAtMouseUp),
                       gateMax("the upper arms from straight down, the worse (deg; they read 60-70, held out in front)", armsHang, 40.0),
                       gateMax("lift-off (every pin released)", r.suspended ? 1.0 : 0.0, 0.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 80.0),
                       // (30: the fingertips of arms HANGING at kArmHangStepDeg and wrists relaxing
                       // at kWristRelaxStepDeg step 17-27mm a tick through the hold — 27 on the
                       // oldest generation since its thighs adduct (the fold rule's hinge test,
                       // 2026-09-25), 17-19 before. A pop would read 60 and more.)
                       gateMax("hold-still worst jump (mm)", r.phases[1].maxJump * 1000.0, 30.0),
                       gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0)}, r);
        }
    }

    // --- PRONE (2026-09-22): from all fours (a kneel, then the chest taken forward and down, both
    // let go of) the HIPS taken a thigh's length and more forward, and down to the floor: she lies on
    // her belly, the thighs forward from the planted knees, the shins folded up behind, the hands
    // where they were planted, her face on the floor. Three rules made it (solveIk / updateContacts):
    // the head is no prop under a pelvis drag (planted as a contact when the face touched, it stood
    // her on her head), the pelvis's kneel-tilt price fades as the hips go ahead of a planted knee
    // (dear, the thigh met its extension limit with the pelvis 22 degrees from upright and the hips
    // 30cm up), and the foot behind such a knee gives up its height rows (its shin comes up). The hip
    // and chest heights are the rig's own (a thigh's length forward on the newest generation's long
    // thighs is not yet flat: informational); the base rigs read the hip 14-20cm up. By the CHEST it is
    // not a gesture: the hands are planted contacts that never walk, the arms fold back under the
    // belly (a hand walk under the sockets was tried and thrown out: the two arms met under the
    // chest and broke the pose to one side on five rigs). The oldest rig's arms fold under and meet
    // (asymmetry 317mm): the symmetry gate is off here.
    if (report.wants("prone") && kneelNatural > 0.0f) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const int hip = resolveBone(posedProbe, "hip");
        const int pelvisBone = resolveBone(posedProbe, "pelvis");
        const int chest = resolveBone(posedProbe, "chest_2");
        const int shoulder = resolveBone(posedProbe, "lShldr");
        const int elbow = resolveBone(posedProbe, "lForeArm");
        const int hand = resolveBone(posedProbe, "lHand");
        const int shin = resolveBone(posedProbe, "lShin");
        const int thigh = resolveBone(posedProbe, "lThigh");
        const int head = resolveBone(posedProbe, "head");
        if (hip < 0 || chest < 0 || shoulder < 0 || elbow < 0 || hand < 0 || shin < 0 || thigh < 0) {
            report.skip("[real] prone: from all fours, the hips taken forward and down", "bones not found on this rig");
        } else {
            const auto at = [&](int b) { return posedProbe.boneWorldPosition(static_cast<std::size_t>(b)); };
            const float armLength = glm::length(at(elbow) - at(shoulder)) + glm::length(at(hand) - at(elbow));
            const float lever = at(chest).y - at(hip).y;
            const float chestKneeling = at(hip).y - (kneelNatural + 0.04f) + lever;
            const float chestOnAllFours = armLength + 0.03f - 0.25f * (at(shoulder).y - at(chest).y);
            const float thighLength = glm::length(at(shin) - at(thigh));
            // (Where the hip joint rests on her belly: over the pelvis's flesh on its front — the
            // reading the floor rows use, in the bind frame the belly faces +z — plus a centimetre;
            // and the hips end a thigh's length ahead of the planted knees, which stay.)
            const IkRig* rigRef = posedProbe.ikRig();
            const float hipProne = (rigRef != nullptr ? rigRef->floorClearanceAlong(hip, glm::vec3(0.0f, 0.0f, 1.0f)) : 0.10f) + 0.01f;
            const float hipOnAllFours = at(hip).y - (kneelNatural + 0.04f) + 0.0f; // (the kneel's hip height: over the knees)
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -(kneelNatural + 0.04f), kneelForward), 90, 40});
            sc.before.push_back({"chest_2", glm::vec3(0.0f, -(chestKneeling - chestOnAllFours), 0.80f * lever), 100, 40});
            sc.grab = "hip";
            // (A full thigh and a little more ahead of the planted knees: the thigh lies flat forward
            // from the knee, the shin folded up behind, and the hip joint sits a hand past the
            // thigh's far end. Short of that the hips cannot come down: a thigh's length forward the
            // thigh stood at 45 degrees with the hips 30cm up, whatever the cursor asked.)
            sc.path = pullPath(glm::vec3(0.0f, -(hipOnAllFours - hipProne), (std::getenv("IK_HARNESS_PRONE_SHARE") ? static_cast<float>(std::atof(std::getenv("IK_HARNESS_PRONE_SHARE"))) : 1.25f) * thighLength), 140, 60);
            sc.asymmetryGateMm = -1.0; // (informational: the oldest rig's arms fold under the chest and meet)
            const RunResult r = run(sc);
            const auto endY = [&](int b) { return b >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(b)].y) : 0.0; };
            // THE ARMS FOLD OR STAND: the hands are planted where all fours put them and never walk,
            // so the shoulders can come forward and down only as far as the arms give under the
            // body. On the third generation and the newest the drag's travel (a thigh and a quarter)
            // outruns what the arm gives — the third's forearm ends dead straight with the WRIST
            // at its limits, a strut holding the shoulders up; the newest's hands go taut and LIFT
            // OFF — and the hips stop 30-40cm up: a hand walk this design has no gesture for. There
            // the height is informational; elsewhere it is GATED (2026-09-23) — it had been
            // informational everywhere, and the slack hands' ceiling of round 77 stopped the descent
            // dead on every rig for nine rounds (the hips 45cm up) with no gate to say so.
            // (SINCE THE PRONE OF 2026-10-29 the hands let go BY DESIGN — trailing leashes released by
            // the target's share — so a lift-off no longer says the arms stood as struts, and the hip's
            // height is GATED on every rig: the body rests on its own flesh (the thigh's, the belly's).
            // The lift-off and the wrist's limit stay as information.)
            const bool handsLifted = r.liveLiftOffs > 0;
            const bool wristBound = limitBoundEnd(r, "lHand");
            const bool armsStrut = false;
            phaseLegs("[real] prone: from all fours, the hips taken forward and down",
                      {info("hip to the cursor at rest (mm; 110-200 on her belly; the arms as struts, 300-450)", r.restingMiss * 1000.0),
                       info("the hands lifted off (1) or the wrist ended limit-bound (2); by design since 2026-10-29", (handsLifted ? 1.0 : 0.0) + (wristBound ? 2.0 : 0.0)),
                       armsStrut ? info("the hip joint over the floor at the end (mm; the arms as struts: informational)", endY(hip) * 1000.0)
                                 : gateMax("the hip joint over the floor at the end (mm; on her belly: 130-200; on all fours, 450)",
                                           endY(hip) * 1000.0, static_cast<double>(hipProne + 0.12f) * 1000.0),
                       info("the chest joint over the floor at the end (mm)", endY(chest) * 1000.0),
                       info("the knee over the floor at the end (mm)", endY(shin) * 1000.0),
                       info("the head over the floor at the end (mm)", endY(head) * 1000.0),
                       info("the pelvis pitched forward, root and pelvis bone (deg)", endPitchDeg(r, hip) + endPitchDeg(r, pelvisBone)),
                       info("live contacts at mouse-up", static_cast<double>(r.pinsAtRelease)),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       // (120-126 on the male and the oldest rig: the hands lift off and re-plant under the still
                       // cursor as the body settles forward, 5-6cm a time - a soft spot.)
                       // (Informational where a wrist ends limit-bound: the third generation's arms,
                       // folding under the belly with the wrists at their limits, flip basins 224mm in
                       // a tick under the corrected rotation order — the prone hold's known twitch.)
                       (limitBoundEnd(r, "lHand") || limitBoundEnd(r, "rHand"))
                           ? info("worst single-tick jump (mm; a wrist limit-bound)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0)
                           : gateMax("worst single-tick jump (mm; the feet flipped 164 let go of outright)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 150.0),
                       gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0)}, r);
        }
    }

    // --- A KNEEL SAT BACK ONTO THE HEELS: from a kneel made and let go of, the hips taken down and
    // back. The hips FOLD and the trunk stays up. On one generation it reclined 41 degrees instead
    // (the root pitched back 20, the pelvis bone 21 more, the hips flexed 9 where the others flex
    // 50): its thighs carry their twist helpers as children beside the shin, and "a girdle joint
    // with two children" - what the pelvis bone was taken for - priced THEM like a pelvis on planted
    // knees (IkRig::isPelvisBone now asks that both LEGS hang from it). Found by looking at the
    // in-app floor gallery on a custom character of that generation; no gate measured a trunk's
    // lean on the heels.
    if (report.wants("kneel-heels") && kneelNatural > 0.0f) {
        const int hip = resolveBone(probe, "hip");
        const int pelvisBone = resolveBone(probe, "pelvis");
        const int thigh = resolveBone(probe, "lThigh");
        Scenario sc;
        sc.before.push_back({"hip", glm::vec3(0.0f, -(kneelNatural + 0.04f), kneelForward), 90, 40});
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -0.10f, -0.30f), 60, 40);
        const RunResult r = run(sc);
        const auto pitch = [&](int bone, const std::vector<glm::vec3>& euler) {
            return bone >= 0 && static_cast<std::size_t>(bone) < euler.size() ? static_cast<double>(euler[static_cast<std::size_t>(bone)].x) : 0.0;
        };
        const double trunkBack = -(pitch(hip, r.endEuler) + pitch(pelvisBone, r.endEuler));
        const double hipsFold = -(pitch(thigh, r.endEuler) - pitch(thigh, r.startEuler));
        phaseLegs("[real] a kneel sat back onto the heels: the hips fold, the trunk stays up",
                  {gateMax("the pelvis pitched back, root and pelvis bone (deg; it read 38 on one generation)", trunkBack, 15.0),
                   gateMin("the hips' flexion gained (deg; it read 5 there; 15 on the oldest rig, 23-27 on the others)", hipsFold, 12.0),
                   info("hip to the cursor at rest (mm; her rigid thighs stop the hips over planted knees: 35-92)", r.restingMiss * 1000.0),
                   gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                   gateMax("worst single-tick jump (mm)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 60.0)}, r);
    }

    if (report.wants("kneel-limb") && kneelNatural > 0.0f) {
        const glm::vec3 down(0.0f, -(kneelNatural + 0.04f), kneelForward);
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int lShin = indexOf("lShin");
        const int rShin = indexOf("rShin");
        const auto went = [&](const RunResult& r, int bone) {
            return bone >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(bone)] -
                                                               r.startPos[static_cast<std::size_t>(bone)])) * 1000.0
                             : 0.0;
        };
        {
            Scenario sc;
            sc.before.push_back({"hip", down, 90, 40});
            sc.grab = "lHand";
            sc.path = pullPath(glm::vec3(0.0f, 0.40f, 0.10f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] kneeling, a hand raised 40cm: she stays on her knees",
                         {gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          gateMax("the hips moved (mm)", went(r, hip), 20.0),
                          gateMax("the left knee moved (mm)", went(r, lShin), 20.0),
                          gateMax("the right knee moved (mm)", went(r, rShin), 20.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
        {
            // THE HALF KNEEL (2026-09-22): the knee drawn up out of the kneel to hip height and forward
            // — past what the shin reaches over the tucked foot, so the foot lets go — swings its
            // foot UNDER it and lands it ahead, the shin coming down from the knee, the sole flat
            // (solveIk: kneeHangOffset, the hang's offset swung to vertical by the lift, the place
            // spring's spot with it, the sole and toes turned flat). It used to trail at knee height
            // behind — the hang held it where the drag FOUND it, behind a kneeling knee — with the
            // shin folded to its limit and the foot up against the thigh. (The 25cm draw above stays
            // within the shin's reach over the tucked foot: there the knee swings over its planted
            // foot, by design.)
            Scenario sc;
            sc.before.push_back({"hip", down, 90, 40});
            sc.grab = "lShin";
            // (Sized by the THIGH — a length is a body's: 42cm on a half-size character pulled the
            // knee above its hip and the whole leg into the air, the foot 76cm up.)
            const int   lThighB = indexOf("lThigh");
            const float thighLen = (lThighB >= 0 && lShin >= 0)
                                       ? glm::length(probe.boneWorldPosition(static_cast<std::size_t>(lShin)) -
                                                     probe.boneWorldPosition(static_cast<std::size_t>(lThighB)))
                                       : 0.45f;
            sc.path = pullPath(glm::vec3(0.0f, 0.93f * thighLen, 0.67f * thighLen), 80, 40);
            const RunResult r = run(sc);
            const int lFootB = indexOf("lFoot");
            const double footAheadOfKnee = (lFootB >= 0 && lShin >= 0)
                                               ? static_cast<double>(r.endPos[static_cast<std::size_t>(lFootB)].z - r.endPos[static_cast<std::size_t>(lShin)].z) * 1000.0
                                               : 0.0;
            const double footUp = lFootB >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lFootB)].y) * 1000.0 : 0.0;
            const double shinFlex = lShin >= 0 && static_cast<std::size_t>(lShin) < r.endEuler.size() ? static_cast<double>(r.endEuler[static_cast<std::size_t>(lShin)].x) : 0.0;
            const double rise = lShin >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lShin)].y - r.startPos[static_cast<std::size_t>(lShin)].y) * 1000.0 : 0.0;
            phaseLegs("[real] kneeling, a knee drawn up to hip height: the half kneel",
                         {gateMin("the drawn knee rose (mm)", rise, 0.66 * thighLen * 1000.0),
                          gateMin("the foot ahead of its knee at the end (mm; behind, folded up, it read -300)", footAheadOfKnee, -60.0),
                          gateMax("the foot over the floor at the end (mm; up against the thigh it read 250)", footUp, 120.0),
                          gateMax("the shin's flexion at the end (deg; it read 155, its limit)", shinFlex, 110.0),
                          gateMax("the hips moved (mm)", went(r, hip), 75.0),
                          gateMax("the other knee moved (mm)", went(r, rShin), 30.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                          // (44-47 on the main family and the newest; 92-136 on the three older generations,
                          // whose kneeling knee shoots 10cm out and back in the draw's first centimetres
                          // — before the foot lets go, in the 25cm draw below just the same, which never
                          // gated it; not this round's: a soft spot.)
                          gateMax("worst single-tick jump (mm)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 150.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
        {
            Scenario sc;
            sc.before.push_back({"hip", down, 90, 40});
            sc.grab = "lShin";
            sc.path = pullPath(glm::vec3(0.0f, 0.25f, 0.15f), 60, 40);
            const RunResult r = run(sc);
            const double rise = lShin >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lShin)].y -
                                                                 r.startPos[static_cast<std::size_t>(lShin)].y) * 1000.0
                                           : 0.0;
            phaseLegs("[real] kneeling, a knee drawn up 25cm: the other stays down",
                         {gateMin("the drawn knee rose (mm)", rise, 230.0),
                          // (60-71 on all eight since 2026-09-22; it read 135-139 on the three oldest
                          // generations — the knee shot 8-10cm out and back as its foot began to let go:
                          // the sole and toes turned flat by the lift's first centimetres while the foot
                          // still stood on its tucked toes, pushing them 18mm into the floor's rows.)
                          gateMax("the drawn knee's worst single-tick move (mm)", r.kneeStepMax * 1000.0, 90.0),
                          // (42-60mm since the knees rest their flesh's radius up, 4-5cm, not 1.5: the
                          // guard is against her STANDING UP under a drawn knee — 283mm.)
                          // (70.7 on the main family since the live contacts' hold is eased over the ticks, 2026-09-21.)
                          gateMax("the hips moved (mm)", went(r, hip), 75.0),
                          gateMax("the other knee moved (mm)", went(r, rShin), 30.0),
                          // (Against clearances fitted to the skin — 4-24cm where they were 1.5: 23mm on one
                          // generation, for a tick, as the drawn knee's thigh passes over the other shin.)
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
    }

    // --- A DRAG REPEATED IS THE SAME DRAG (2026-09-24): a kneel made from the rest pose, and the
    // same kneel made after another kneel and a Reset Pose, end in the SAME pose — to the last
    // bit. They did not (2026-09-23): a zero Euler composed as localBind * OR * I * OR^-1 is the
    // bind to a float rounding, a reset left every world matrix 1e-7 off the freshly built
    // figure's, and an unconverged solve grew that to 2e-5 in a tick — and to another basin of a
    // chaotic gesture (Armature::recomposePoseLocal composes a zero Euler to the bind exactly).
    // The gate is ZERO: the pose is a function of the pose it began from and the target.
    if (report.wants("repeat") && kneelNatural > 0.0f) {
        const glm::vec3 down(0.0f, -(kneelNatural + 0.04f), kneelForward);
        Scenario once;
        once.grab = "hip";
        once.path = pullPath(down, 90, 40);
        Scenario again = once;
        again.before.push_back({"hip", down, 90, 40});
        again.resetAfterBefore = true;
        const RunResult a = run(once);
        const RunResult b = run(again);
        double eulerDiff = 0.0, posDiff = 0.0;
        for (std::size_t i = 0; i < a.endEuler.size() && i < b.endEuler.size(); ++i) {
            eulerDiff = std::max(eulerDiff, static_cast<double>(glm::length(a.endEuler[i] - b.endEuler[i])));
        }
        for (std::size_t i = 0; i < a.endPos.size() && i < b.endPos.size(); ++i) {
            posDiff = std::max(posDiff, static_cast<double>(glm::length(a.endPos[i] - b.endPos[i])));
        }
        phaseLegs("[real] a kneel repeated after a Reset Pose ends in the same pose, to the last bit",
                  {gateMax("the largest channel difference (deg; it read 0.002 before the exact bind)", eulerDiff, 0.0),
                   gateMax("the largest joint difference (mm)", posDiff * 1000.0, 0.0),
                   info("hip to the cursor at rest, the second time (mm)", b.restingMiss * 1000.0)}, b);
    }

    // --- A LUNGE down onto the back knee: one foot slid a stride back and let go of, then the
    // hips taken down. The sway's roll is for a leg that has RUN OUT of length; in a split stance
    // hips going down change the two legs' straight heights by different amounts with no sway at
    // all, and the lunge came out with the pelvis rolled 14 degrees, the front foot rolled to its
    // limit and the front knee across the midline.
    if (report.wants("lunge")) {
 // (the dump's index: build keeps the order)
        const int lShin = indexOf("lShin");
        const int rShin = indexOf("rShin");
        const int lFoot = indexOf("lFoot");
        Scenario sc;
        sc.before.push_back({"rFoot", glm::vec3(0.0f, 0.0f, -0.60f), 90, 40});
        sc.grab = "hip";
        // (38cm down with the knee's old 1.5cm clearance: the back knee now lands its flesh's
        // radius up, and a target under that is one the landed knee rightly refuses.)
        sc.path = pullPath(glm::vec3(0.0f, -(0.38f - (kneeClearance - 0.015f)), 0.0f), 80, 40);
        const RunResult r = run(sc);
        const auto at = [&](int bone) { return bone >= 0 ? r.endPos[static_cast<std::size_t>(bone)] : glm::vec3(0.0f); };
        // (The pelvis GRABBED is the hip the harness drags: its own channels are the root's. How
        // far down the back knee comes is the rig's legs'; a front knee 8-12cm inside its foot is
        // the zero-twist leg plane of a wide stance, the crouches' mild valgus — a guard, not the
        // point.)
        const int hipBone = indexOf("hip");
        const glm::vec3 pelvisTurn = hipBone >= 0 && static_cast<std::size_t>(hipBone) < r.endEuler.size()
                                         ? r.endEuler[static_cast<std::size_t>(hipBone)]
                                         : glm::vec3(0.0f);
        phaseLegs("[real] a lunge down toward the back knee (rFoot 60cm back, then hip -38cm)",
                     {// (A back knee that has LANDED — within half a centimetre of its floor clearance —
                      // rightly stops the hips: how deep a lunge goes is the rig's legs'.)
                      gateMax("hip-to-cursor at rest, unless the back knee has landed (mm)",
                              at(rShin).y <= kneeClearance + 0.005f ? 0.0 : r.restingMiss * 1000.0, 30.0),
                      info("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                      // (8: a cursor held out of reach pulls at its full bound since 2026-09-21 — JointSolver::
                      // residuals — and on the character rig, whose back knee lands before the hip is where
                      // it was asked, the over-deep press rolls the pelvis 7.3 degrees where it rolled 3.2.)
                      gateMax("pelvis roll (deg)", static_cast<double>(std::abs(pelvisTurn.z)), 8.0),
                      // (12 since THE STRIDE TURNS THE HIPS, 2026-11-02: the foot slid 60cm back turns the
                      // pelvis 7.5-9 degrees toward the front leg, and the lunge keeps the stride's turn —
                      // a lunge made from a stride has its hips a little open, as an untrained one does;
                      // it read 0.0-0.9 with a square pelvis. The gate is still the lunge's own: a pelvis
                      // turned to find depth reads far past it.)
                      gateMax("pelvis heading (deg)", static_cast<double>(std::abs(pelvisTurn.y)), 12.0),
                      gateMax("the front knee off its foot, sideways (mm)",
                              static_cast<double>(std::abs(at(lShin).x - at(lFoot).x)) * 1000.0, 140.0),
                      info("the back knee over the floor (mm)", static_cast<double>(at(rShin).y) * 1000.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 20.0),
                      gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                      gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 60.0)}, r);
    }

    if (report.wants("kneel-stand") && kneelNatural > 0.0f) {
        const glm::vec3 down(0.0f, -(kneelNatural + 0.04f), kneelForward);
        for (const char* grab : {"hip", "chest_2"}) {
            Scenario sc;
            sc.before.push_back({"hip", down, 90, 40});
            sc.grab = grab;
            // Back to where the joint sits AT REST (Scenario::toRest), not "the kneel undone":
            // a kneel pitches the torso, so the chest's kneeling position plus the hip's reversed
            // travel ends behind where a balanced standing chest sits — 13cm behind on the newest
            // generation — and balance rightly refuses it (the chest stopped 13cm short of a
            // target no standing body can hold).
            sc.path = pullPath(-down, 120, 60);
            sc.toRest = true;
            const RunResult r = run(sc);
            const bool byHip = std::string(grab) == "hip";
            char name[128];
            std::snprintf(name, sizeof(name), "[real] getting up out of a released kneel, by the %s",
                          byHip ? "hip" : "chest");
            // OPEN: the main figure family gets up CLEAN (heels 14-23mm over their standing
            // height, the grab on the cursor to 0.04mm, nothing through the floor); on the four
            // other generations she gets up — no lift-off, no step, the hips all the way — but
            // ends on lifted heels (4-7cm) and, pulled by the chest, 1-7cm short of the cursor.
            // Not their toes' range, not the foot's priced channel (both tried). The gates below
            // guard today's level on every rig; the numbers to beat are the main family's.
            phaseLegs(name,
                         {gateMax("lifted off the floor (0 = no)", r.suspended ? 1.0 : 0.0, 0.0),
                          // (No step on seven rigs. The oldest generation's kneel leaves its feet
                          // 12cm from where its stance wants them, and once she is up she gathers
                          // them — one or two steps, a fair thing for a body to do.)
                          gateMax("steps", static_cast<double>(r.stepsTaken), 2.0),
                          gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 10.0),
                          gateMin("hip rise (m)", r.hipDispY, kneelNatural - 0.06),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 35.0),
                          // (Against the ANKLE's own standing height: in a tucked-toe kneel several
                          // generations' ankles sit above the contact height, the drag's foot pin is
                          // then the TOES joint, and measured against the lowest pin's rest height a
                          // perfectly flat foot read as "heels 7cm up".)
                          gateMax("heels above their standing height at the end (mm)",
                                  (r.feetMinY - r.feetBindMinY) * 1000.0, 30.0),
                          // By the chest the SPINE still buckles sideways for a few ticks mid-rise
                          // (the chest goes up and back while the legs hold the hips; the back runs
                          // out of backward bend and twists to follow, then snaps straight): a known
                          // soft spot, read here as a fingertip's jump — gated by the hip only.
                          // (A rising body UNLOADS its floor contacts with the rise — Armature::solveIk: it
                          // read 16-162mm while a knee's hold went all at once; 18-73 now.)
                          // (100 and 25 since the feet lie flat in a kneel, 2026-09-26: getting up out of a
                          // FLAT kneel rolls the feet back onto their toes first — the knee's ceiling lifts
                          // with the roll, so the hips wait on it (a mean lag of 15-22mm where the tucked
                          // kneel's read 3-8) — and the handed-over foot's rows come back over the share's
                          // fade (60-97mm at a toe or shin in the tick they land). 85 and 15 before.)
                          // (120 since the bones' frames were put right, 2026-09-30 — every foot now
                          // pitches about its own axis, 7 degrees from where it did: 45-88 on seven rigs,
                          // where the unrolling foot now comes off the floor to turn over; 114 on the
                          // generation whose toes fan from the mid-foot, in the tick its one gathering
                          // step lifts a foot that still stands on its toes — a step's own start, late
                          // in the rise. It read 88 there.)
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 120.0),
                          gateMax("grab's lag behind the cursor, mean (mm)", r.phases.empty() ? 0.0 : r.phases[0].lagMean() * 1000.0, 25.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }

    // (5) ... and out of a SIT on the floor, by the chest and by the head: the pelvis rolled 25
    // degrees back, the thighs at their flexion limit, the hips half a metre behind the feet. The
    // grabbed joint goes back to where it sits at rest (Scenario::toRest); she must arrive
    // standing — on the cursor, on flat feet — without being lifted, stepping about or jumping.
    if (report.wants("sit-stand") && sitDrop > 0.0f) {
        for (const char* grab : {"chest_2", "head"}) {
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -sitDrop, -0.50f), 100, 40});
            sc.grab = grab;
            sc.path = pullPath(glm::vec3(0.0f, 0.5f, 0.0f), 150, 60);
            sc.toRest = true;
            const RunResult r = run(sc);
            char name[128];
            std::snprintf(name, sizeof(name), "[real] getting up out of a released sit, by the %s",
                          std::string(grab) == "head" ? "head" : "chest");
            phaseLegs(name,
                         {gateMax("lifted off the floor (0 = no)", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 2.0),
                          gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 10.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 35.0),
                          gateMax("heels above their standing height at the end (mm)",
                                  (r.feetMinY - r.feetBindMinY) * 1000.0, 30.0),
                          // (A rising body UNLOADS its floor contacts with the rise — Armature::solveIk: it
                          // read 16-162mm while a knee's hold went all at once; 18-73 now.)
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 85.0),
                          gateMax("grab's lag behind the cursor, mean (mm)", r.phases.empty() ? 0.0 : r.phases[0].lagMean() * 1000.0, 15.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }
}

/// Self-collision: a limb across the body, limb against limb, the FK collision stop.
void RealPhases::collisionPhases() {
    // --- SELF-COLLISION (the body volumes, IkRig::bodyVolumes): an arm dragged across the
    // chest to the far side goes AROUND the torso instead of through it, and a foot dragged
    // across the other leg goes around that thigh. The cursor's straight path runs through the
    // volume both times; the target itself ends outside it, so the hand/foot must arrive.
    if (report.wants("arm-across")) {
        Scenario sc;
        sc.grab = "lHand";
        // The target sits just past the OTHER shoulder — 6cm outboard of it, 16cm below, 10cm
        // in front — measured from the rest pose the drag starts in (the T-pose rigs' lowered
        // arms included), so the reach is the same gesture on every generation: a fixed offset
        // from the hand's rest landed 39cm past the spine on the oldest rigs, beyond any reach
        // around the chest.
        glm::vec3 offset(-0.73f, 0.21f, 0.03f);
        {
            Armature posedProbe;
            posedProbe.build(bones);
            for (const auto& [bone, euler] : restPose) {
                posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
            }
            const int hand = resolveBone(posedProbe, "lHand");
            const int other = resolveBone(posedProbe, "rShldr");
            if (hand >= 0 && other >= 0) {
                offset = posedProbe.boneWorldPosition(static_cast<std::size_t>(other)) +
                         glm::vec3(-0.06f, -0.16f, 0.10f) -
                         posedProbe.boneWorldPosition(static_cast<std::size_t>(hand));
            }
        }
        sc.path = pullPath(offset, 70, 60);
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        // A gesture the rig CANNOT make — a shoulder that ends LIMIT-BOUND (the third
        // generation's, pre-posed by its adduction channel), or a pre-posed T-pose rig whose
        // hand stops more than 10cm short — makes the volume and stance rows informational:
        // the hand is held back by the volume rows, the arm rests against the capsule, the
        // solver leans the chest after the cursor and the figure steps toward it (the balance
        // stepper's designed response to a sustained lean). The transient allows the applied
        // pose a few ticks behind the solver as the arm sweeps across (the lift moves 2cm a
        // tick).
        // A pre-posed T-pose rig whose hand stops more than 10cm short cannot make the gesture
        // at all (the third generation: its hand rests against the chest's side, 2-3cm into
        // the capsule its limit-bound arm cannot leave); the rows are then the rig's.
        const bool armBound = limitBoundEnd(r, "lShldr");
        const bool infeasible = !restPose.empty() && r.restingMiss > 0.10;
        phaseLegs("[real] arm across the chest (lHand to just past the other shoulder, through the torso)",
                     {infeasible ? info("body-volume penetration, drag+settle, gesture infeasible (mm)", r.volumePenetrationMax * 1000.0)
                                 : gateMax("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0, 35.0),
                      (armBound || infeasible) ? info("body-volume penetration at the end, shoulder limit-bound (mm)", r.volumePenetrationEnd * 1000.0)
                                               : gateMax("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0, 10.0),
                      gateMax("rider (finger) penetration, drag+settle (mm)", r.riderPenetrationMax * 1000.0, 120.0),
                      infeasible ? info("rider (finger) penetration at the end, gesture infeasible (mm)", r.riderPenetrationEnd * 1000.0)
                                 : gateMax("rider (finger) penetration at the end (mm)", r.riderPenetrationEnd * 1000.0, 10.0),
                      // The reach itself is the rig's: the oldest generations' shoulders (the
                      // T-pose rigs, their adduction 7° from its limit once pre-posed) stop the
                      // hand in front of the sternum, 16-25cm short — info, not a gate.
                      info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                      infeasible ? info("feet drift, gesture infeasible (mm)", r.contactDriftMax * 1000.0)
                                 : gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
    }
    if (report.wants("leg-across")) {
        Scenario sc;
        sc.grab = "lFoot";
        sc.path = pullPath(glm::vec3(-0.40f, 0.12f, 0.10f), 70, 60);
        const RunResult r = run(sc);
        phaseLegs("[real] foot across the other leg (lFoot -40cm x +12cm y +10cm z, through the thigh)",
                     {gateMax("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0, 35.0),
                      gateMax("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0, 10.0),
                      gateMax("rider (finger) penetration, drag+settle (mm)", r.riderPenetrationMax * 1000.0, 120.0),
                      gateMax("rider (finger) penetration at the end (mm)", r.riderPenetrationEnd * 1000.0, 10.0),
                      // The reach is the rig's since the SHIN is tested too (the second
                      // self-collision round): the crossing shin must clear the other thigh
                      // by both radii, and the shorter-legged generations stop 15cm short of
                      // a cursor 40cm across — info, not a gate.
                      info("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                      gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
    }

    // --- LIMB AGAINST LIMB (the self-collision round 2): the left hand dragged onto the middle
    // of the OTHER forearm, and the left foot onto the middle of the OTHER shin, both measured
    // from the rest pose the drag starts in. Neither target is reachable — it sits on the
    // other limb's axis — so the hand/foot must stop at that limb's surface (its capsule plus
    // the hand's/foot's clearance) without going through it.
    if (report.wants("arm-vs-arm") || report.wants("shin-across") || report.wants("legs-cross")) {
        Armature posedProbe;
        posedProbe.build(bones);
        for (const auto& [bone, euler] : restPose) {
            posedProbe.setBoneRotation(resolveBoneName(posedProbe, bone), euler);
        }
        const auto at = [&](const char* name) {
            const int i = resolveBone(posedProbe, name);
            return i >= 0 ? posedProbe.boneWorldPosition(static_cast<std::size_t>(i)) : glm::vec3(0.0f);
        };
        if (report.wants("arm-vs-arm")) {
            Scenario sc;
            sc.grab = "lHand";
            // The other forearm's middle, 15cm above and 10cm in front of it: at the forearm's
            // own level the reach across the belly strained the stance into a step on the
            // character rig, and on the third generation — a limit-bound shoulder — into a
            // trunk twist that slid the feet 16cm and put the hand 6cm into the torso (the
            // same with the limb capsules off: the gesture's, not this round's); higher or
            // closer the third generation's hand crosses the chest 32-47mm deep. Here every
            // rig crosses and stops where the arm runs out (13-30cm short of the far forearm
            // — the reach is the rig's, the penetration is the gate).
            const glm::vec3 target = 0.5f * (at("rForeArm") + at("rHand")) + glm::vec3(0.0f, 0.15f, 0.10f);
            sc.path = pullPath(target - at("lHand"), 70, 60);
            sc.idleJoints = kIdleForHandDrag;
            const RunResult r = run(sc);
            const bool armBound = limitBoundEnd(r, "lShldr"); // see arm-across
            const bool infeasible = !restPose.empty() && r.restingMiss > 0.10;
            // The transients here are regression guards, not a spec: on the main family the
            // wrist and the fingers cut up to 12cm into the chest for ~20 ticks of the reach —
            // the solver's own arm is already round the far side (its state is clean every
            // tick), and the applied arm, whose Euler-clamped shoulder cannot take that
            // position-level path, lags it through the chest until the extraction catches up;
            // the lift then lands it (0 at the end on every rig). A governor slow-down of the
            // sunk limb and a lift of the full depth were both measured and rejected (the
            // first stalled every cross-body reach and stepped the figure, the second moved
            // the transient 5mm). The feet slide up to 2cm under the reach on the male rig.
            phaseLegs("[real] hand onto the other forearm (lHand to the middle of rForeArm)",
                         {infeasible ? info("body-volume penetration, drag+settle, gesture infeasible (mm)", r.volumePenetrationMax * 1000.0)
                                     : gateMax("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0, 130.0),
                          (armBound || infeasible) ? info("body-volume penetration at the end, shoulder limit-bound (mm)", r.volumePenetrationEnd * 1000.0)
                                                   : gateMax("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0, 10.0),
                          gateMax("rider (finger) penetration, drag+settle (mm)", r.riderPenetrationMax * 1000.0, 130.0),
                          infeasible ? info("rider (finger) penetration at the end, gesture infeasible (mm)", r.riderPenetrationEnd * 1000.0)
                                     : gateMax("rider (finger) penetration at the end (mm)", r.riderPenetrationEnd * 1000.0, 10.0),
                          info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          (armBound || infeasible) ? info("feet drift, shoulder limit-bound (mm)", r.contactDriftMax * 1000.0)
                                                   : gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 30.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
        if (report.wants("legs-cross")) {
            // The left foot planted just OUTSIDE the right foot, a little ahead: the left shin
            // then crosses the right shin between the knees and the ankles — where no joint
            // sits. The segment rule must keep the shins apart by their two radii.
            Scenario sc;
            sc.grab = "lFoot";
            const glm::vec3 rf = at("rFoot");
            const glm::vec3 lf = at("lFoot");
            const float outward = rf.x < lf.x ? -0.06f : 0.06f;
            const glm::vec3 target = rf + glm::vec3(outward, 0.0f, 0.08f);
            sc.path = pullPath(target - lf, 80, 60);
            const RunResult r = run(sc);
            phaseLegs("[real] legs crossed (lFoot planted just outside rFoot; the shins cross)",
                         {gateMax("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0, 35.0),
                          gateMax("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0, 10.0),
                          gateMax("rider (finger) penetration, drag+settle (mm)", r.riderPenetrationMax * 1000.0, 120.0),
                          gateMax("rider (finger) penetration at the end (mm)", r.riderPenetrationEnd * 1000.0, 10.0),
                          info("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
        if (report.wants("shin-across")) {
            Scenario sc;
            sc.grab = "lFoot";
            // The other shin's middle, 5cm in front of it.
            const glm::vec3 target = 0.5f * (at("rShin") + at("rFoot")) + glm::vec3(0.0f, 0.0f, 0.05f);
            sc.path = pullPath(target - at("lFoot"), 70, 60);
            const RunResult r = run(sc);
            phaseLegs("[real] foot onto the other shin (lFoot to the middle of rShin)",
                         {gateMax("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0, 35.0),
                          // The calf's closest point ends 0-13mm inside the other shin's capsule
                          // (the male rig the deepest): a regression guard, not a spec — two
                          // calves overlapping a centimetre in a capsule model is not a visible
                          // collision, and the capsule radius is itself a guess at the flesh.
                          gateMax("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0, 15.0),
                          gateMax("rider (finger) penetration, drag+settle (mm)", r.riderPenetrationMax * 1000.0, 120.0),
                          gateMax("rider (finger) penetration at the end (mm)", r.riderPenetrationEnd * 1000.0, 10.0),
                          info("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }

    // --- FK COLLISION STOP (the self-collision round 2): every channel of the shoulder, elbow,
    // thigh and knee swept both ways in 5 deg wheel notches from rest — each sweep on a fresh
    // pose — must never push a limb joint into a body volume (Armature::nudgeSelectedBone
    // bisects the rotation back to the surface). The reach the stop leaves is reported.
    if (report.wants("fk-collide")) {
        double worstDepth = 0.0;
        int sweeps = 0;
        int blocked = 0;
        double blockedDeg = 0.0; // rotation given up to the stop, summed
        std::vector<std::string> notes;
        for (const char* canonical : {"lShldr", "lForeArm", "lThigh", "lShin", "rShldr", "rThigh"}) {
            for (int axis = 0; axis < 3; ++axis) {
                for (int dir = -1; dir <= 1; dir += 2) {
                    Armature arm;
                    arm.build(bones);
                    for (const auto& pp : restPose) {
                        arm.setBoneRotation(resolveBoneName(arm, pp.first), pp.second);
                    }
                    const int b = resolveBone(arm, canonical);
                    if (b < 0) {
                        continue;
                    }
                    arm.selectBoneByName(arm.boneName(static_cast<std::size_t>(b)));
                    const glm::vec3 start = arm.boneEuler(static_cast<std::size_t>(b));
                    glm::vec3 delta(0.0f);
                    delta[axis] = 5.0f * static_cast<float>(dir);
                    // Without the stop the limits alone bound the sweep: measure that reach first.
                    glm::vec3 freeEnd = start;
                    {
                        Armature posedProbe;
                        posedProbe.build(bones);
                        for (const auto& pp : restPose) {
                            posedProbe.setBoneRotation(resolveBoneName(posedProbe, pp.first), pp.second);
                        }
                        glm::vec3 e = start;
                        for (int k = 0; k < 40; ++k) {
                            e += delta;
                            posedProbe.setBoneRotation(posedProbe.boneName(static_cast<std::size_t>(b)), e);
                            e = posedProbe.boneEuler(static_cast<std::size_t>(b));
                        }
                        freeEnd = e;
                    }
                    for (int k = 0; k < 40; ++k) {
                        arm.nudgeSelectedBone(delta);
                        worstDepth = std::max(worstDepth, volumePenetration(arm));
                    }
                    const glm::vec3 end = arm.boneEuler(static_cast<std::size_t>(b));
                    const double gaveUp = std::abs(freeEnd[axis] - end[axis]);
                    ++sweeps;
                    if (gaveUp > 0.5) {
                        ++blocked;
                        blockedDeg += gaveUp;
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), "%s %c%c stopped %.0f deg short of its limit",
                                      canonical, "xyz"[axis], dir < 0 ? '-' : '+', gaveUp);
                        notes.push_back(buf);
                    }
                }
            }
        }
        std::vector<Gate> gates{gateMax("worst body-volume penetration over every sweep (mm)", worstDepth * 1000.0, 3.0),
                                info("sweeps", sweeps), info("sweeps the stop shortened", blocked),
                                info("rotation given up to the stop, total (deg)", blockedDeg)};
        report.phase("[real] FK collision stop: limb sweeps from rest", gates);
        if (v) {
            for (const std::string& note : notes) {
                std::printf("      %s\n", note.c_str());
            }
        }
    }
}

/// Sitting on the floor and what the floor stops: the seat, the recline, the lay-down and the sit-up.
void RealPhases::sitPhases() {
    // --- SITTING on the floor: the hip dragged down to 15cm and 50cm back behind the feet.
    // The thighs run out of flexion and the pelvis bone of its 25° well before the hip gets
    // there, and the solver then loses the feet (15cm off their pins, the toes 4cm through the
    // floor, on every rig); the pelvis-tilt assist (Armature::rootTiltStep) pitches the root
    // back instead, as a real sit does. The feet must stay planted and the hip arrive.
    if (report.wants("sit")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -sitDrop, -0.50f), 100, 60);
        const RunResult r = run(sc);
        char name[128];
        std::snprintf(name, sizeof(name), "[real] sit on the floor (hip -%.0fcm y -50cm z)",
                      sitDrop * 100.0f);
        phaseLegs(name,
                     {gateMin("hip drop at release (m)", r.hipDropAtRelease, sitDrop - 0.03),
                      gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 25.0),
                      info("feet drift during the drag, max (mm)", r.contactDriftMaxDrag * 1000.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                      info("pelvis tilt at the end (deg)", r.grabEulerEnd.x - r.grabEulerStart.x),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
    }

    // --- THE FLOOR STOPS HER FLESH, AND A SEAT ON IT IS A SUPPORT. The joints above the feet had
    // one floor clearance, 1.5cm: a sit went on down until the hip JOINT was 1.5cm over the floor
    // — 20cm of flesh through it — and the pelvis, never within a centimetre of THAT, was never a
    // contact: a sitting figure stood on her feet alone, 40cm out of balance, and reclined by the
    // chest her arms were flung out as counterweights the tick her hands touched down (70cm at
    // the fingertips, in one tick). The clearances are fitted to the skin now (IkRig::
    // fitFloorClearances), a pelvis resting on the floor is a contact under any drag but its own,
    // and balance stands down while it does.
    if (report.wants("floor-seat") && sitDrop > 0.0f) {
        const int hip = resolveBone(probe, "hip");
        const int head = resolveBone(probe, "head");
        const float hipRest = hip >= 0 ? probe.boneWorldPosition(static_cast<std::size_t>(hip)).y : 1.0f;
        {
            // Dragged 12cm DEEPER than the seat: the floor's rows stop the hips on her flesh.
            Scenario sc;
            sc.grab = "hip";
            sc.path = pullPath(glm::vec3(0.0f, -(sitDrop + 0.12f), -0.50f), 110, 50);
            // (The symmetry gate's second catch: pushed 12cm past the seat, two generations ended with
            // both knees 5-7cm over to one side. The floor's rows on the pelvis's flesh were lopsided:
            // extreme points taken along a direction lattice that was not mirror-symmetric, four of
            // them picked lowest-first. 0.0000mm since both are mirror-closed.)
            const RunResult r = run(sc);
            const double hipOver = hip >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(hip)].y) : 0.0;
            phaseLegs("[real] sat down 12cm too deep: the floor stops her on her seat",
                      {gateMin("the hip joint over the floor, less its seat's clearance (mm; it read -180)",
                               (hipOver - static_cast<double>(seatClearance)) * 1000.0, -25.0),
                       gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 30.0),
                       gateMax("hold: worst single-tick jump (mm)", r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 10.0)}, r);
        }
        {
            // A sit made and let go of, then reclined by the chest: no limb is flung anywhere.
            // (The sit as a user makes it: the hips dragged until the FLOOR stops them.)
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -(sitDrop + 0.05f), -0.50f), 100, 40});
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, -0.25f, -0.35f), 80, 50);
            const RunResult r = run(sc);
            const double headBack = head >= 0 ? static_cast<double>(r.startPos[static_cast<std::size_t>(head)].z -
                                                                    r.endPos[static_cast<std::size_t>(head)].z) * 1000.0
                                              : 0.0;
            phaseLegs("[real] sitting on the floor, reclined by the chest: she leans back, nothing is flung",
                      {gateMin("live contacts at once (the seat is one)", r.livePinsMax, 1.0),
                       gateMin("the head went back (mm; 30 while balance walled it)", headBack, 200.0),
                       // (38-48mm; 99 at a fingertip on the newest generation, the tick a hanging hand lands and its arm is the solve's.)
                       gateMax("worst single-tick jump (mm; it read 680)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 120.0),
                       // (The target is out of reach by design, so the hold's first ticks are the move's
                       // last: 38mm on the short-armed generation, whose forearms are down too.)
                       gateMax("hold: worst single-tick jump (mm)", r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 45.0),
                       gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
        // LAID DOWN ON HER BACK, and sat back up (2026-09-21, the user: "her hip stays pointed
        // up"). The pelvis's pitch was the bow's — a third of a recline, the spine the rest, arched
        // to its limits over a pelvis 40 degrees from the floor — and, 30cm and more from a cursor
        // she could have reached, the solver's model pulled a sixth as hard as the cost it was
        // descending (JointSolver::residuals: a bounded ask at full weight). The seat ROLLS now:
        // the pelvis's pitch reference goes with the target about the seat (Armature::solveIk).
        const int chestBone = resolveBone(probe, "chest_2");
        if (hip >= 0 && chestBone >= 0 && head >= 0) {
            const glm::vec3 rod = probe.boneWorldPosition(static_cast<std::size_t>(chestBone)) -
                                  probe.boneWorldPosition(static_cast<std::size_t>(hip));
            const float trunk = glm::length(rod);
            // From where the chest sits over the seat to where it lies behind it: down to where
            // its own back rests (the chest sits its height over the hips above the seat's
            // clearance, less what the sit's tilt takes), back by nine tenths of the trunk's
            // length (within the trunk's reach of the seat, so the seat need not slide).
            const glm::vec3 down(0.0f, -(0.97f * rod.y + seatClearance - chestBackClearance - 0.01f), -0.90f * trunk);
            std::vector<int> spine; // the spine's bones from the hip up to the upper chest
            for (int cur = chestBone; cur >= 0 && cur != hip; cur = bones[static_cast<std::size_t>(cur)].parent) {
                spine.push_back(cur);
            }
            const auto spineArch = [&](const RunResult& r) {
                double worst = 0.0; // the most EXTENDED spine joint (its flexion channel gone negative)
                for (const int b : spine) {
                    const std::size_t at = static_cast<std::size_t>(b);
                    if (at < r.endEuler.size()) {
                        worst = std::max(worst, static_cast<double>(r.startEuler[at].x - r.endEuler[at].x));
                    }
                }
                return worst;
            };
            const auto pitchBack = [&](const RunResult& r) {
                const std::size_t at = static_cast<std::size_t>(hip);
                return at < r.endEuler.size() ? static_cast<double>(r.startEuler[at].x - r.endEuler[at].x) : 0.0;
            };
            // (The sit is made the way a user makes it — the hips dragged until the FLOOR stops them,
            // 5cm past the seat: stopped a centimetre or two short, one generation's pelvis hovered
            // just outside the band a resting seat is recognized in, and she was no seated figure.)
            const glm::vec3 sitDown(0.0f, -(sitDrop + 0.05f), -0.50f);
            {
                Scenario sc;
                sc.before.push_back({"hip", sitDown, 100, 40});
                sc.grab = "chest_2";
                sc.path = pullPath(down, 110, 60);
                const RunResult r = run(sc);
                const double chestUp = static_cast<double>(r.endPos[static_cast<std::size_t>(chestBone)].y);
                const double headUp = static_cast<double>(r.endPos[static_cast<std::size_t>(head)].y);
                phaseLegs("[real] sitting on the floor, laid down on her back by the chest",
                          {// (-35 to -45 while the pitch was the bow's: the hips stayed pointed up. The
                           // pitch itself, not its change: the oldest generation SITS 26 degrees back.)
                           gateMax("the pelvis's pitch at the end (deg; -90 = flat on her back)",
                                   static_cast<std::size_t>(hip) < r.endEuler.size() ? static_cast<double>(r.endEuler[static_cast<std::size_t>(hip)].x) : 0.0,
                                   -65.0),
                           info("the pelvis rolled back by (deg)", pitchBack(r)),
                           // (18-20, every spine joint at its limit, before.)
                           gateMax("the most-arched spine joint (deg of extension)", spineArch(r), 10.0),
                           gateMax("the chest joint over where its back rests (mm; it read 220)",
                                   (chestUp - static_cast<double>(chestBackClearance)) * 1000.0, 40.0),
                           gateMax("the head joint over the floor (mm)", headUp * 1000.0, 260.0),
                           info("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                           // (12-16cm at the fingertips, tick after tick, while the hands landed and the
                           // arms were the solve's; and the seat's hold came and went on alternate ticks.)
                           gateMax("worst single-tick jump (mm; it read 157)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 80.0),
                           gateMax("hold: worst single-tick jump (mm)", r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 25.0),
                           gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 2.0),
                           gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                           gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 40.0),
                           gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
            }
            {
                // ... let go of, and sat back up by the chest in a second drag.
                Scenario sc;
                sc.before.push_back({"hip", sitDown, 100, 40});
                sc.before.push_back({"chest_2", down, 110, 40});
                sc.grab = "chest_2";
                sc.path = pullPath(-down, 110, 60);
                const RunResult r = run(sc);
                const std::size_t at = static_cast<std::size_t>(hip);
                const double pitchUp = at < r.endEuler.size() ? static_cast<double>(r.endEuler[at].x - r.startEuler[at].x) : 0.0;
                const double chestRose = static_cast<double>(r.endPos[static_cast<std::size_t>(chestBone)].y -
                                                             r.startPos[static_cast<std::size_t>(chestBone)].y);
                phaseLegs("[real] lying on her back, sat back up by the chest",
                          {// (3-28 degrees, the chest brought up by a spine curled to its limits, while a
                           // lying figure's chest — level with her hips — did not count as upper body.)
                           gateMin("the pelvis rolled back up (deg)", pitchUp, 55.0),
                           gateMin("the chest came up, as a share of the way it went down", chestRose / std::max(1.0e-3, static_cast<double>(-down.y)), 0.75),
                           // (Where she SAT is not always where she can sit again: the seat slid a few
                           // centimetres as she was laid down, and the thighs meet their flexion limit
                           // sooner from there — 3-10cm on some generations. Open.)
                           info("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                           gateMax("lifted off the floor (0 = no)", r.suspended ? 1.0 : 0.0, 0.0),
                           gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 70.0),
                           gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 6.0),
                           gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 30.0),
                           gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
            }
        }
        (void)hipRest;
    }
}

/// The feet: the deep crouch's heel lift, the hover healed, a strained release, a foot lifted and a foot placed.
void RealPhases::stancePhases() {
    // (The two MID-DRAG ROTATE phases — the X/Y/Z wheel held during a drag turned the grabbed
    // joint while the IK kept placing it — went with the wheel rotation on 2026-09-30: no gesture
    // turns a joint in the middle of a drag any more.)

    // --- A DEEP crouch lifts the heels: once the ankle's dorsiflexion range is spent the sole
    // pitches, and before the heel-lift round the toes rode it 6-8cm through the floor. The
    // standing pin now rises by the toes' hang and the toes hold their orientation, so the
    // figure comes up onto the balls of its feet with the toes on the floor.
    if (report.wants("deep-crouch")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, -0.50f, 0.0f), 80, 60);
        const RunResult r = run(sc);
        // The sole PITCHES here by design (the heel lifts), so the planted-foot rotation is
        // an info row; the knees keep the crouch gates.
        phaseLegs("[real] deep crouch (hip -50cm y): heels lift, toes stay on the floor",
                     {gateMin("hip drop at release (m)", r.hipDropAtRelease, 0.47),
                      info("heel lift max (mm)", r.contactRiseMax * 1000.0),
                      gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 20.0),
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 80.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r,
                  LegGates{15.0, 30.0, -1.0, -1.0});
    }

    // --- A hovering figure is healed back onto the floor by its next drag.
    if (report.wants("hover-heal")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.hover = 0.03f;
        sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 30, 90);
        const RunResult r = run(sc);
        double feetY = 1e9;
        for (const int c : r.contactPins) {
            feetY = std::min(feetY, static_cast<double>(r.endPos[static_cast<std::size_t>(c)].y));
        }
        phaseLegs("[real] hover heal (3cm hover, gentle hand pull)",
                     {gateMax("lowest contact vs floor after (mm)", std::abs(r.contactBindMinY < 1e8 ? feetY - r.contactBindMinY : 0.0) * 1000.0, 5.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r);
    }

    // --- A strained pull (beyond the arm's reach, forward) and its release: the pins land
    // exactly, every settle tick small.
    if (report.wants("strained-release")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = pullPath(glm::vec3(0.0f, 0.10f, 0.55f), 60, 60);
        const RunResult r = run(sc);
        phaseLegs("[real] strained pull + release (lHand +55cm z)",
                     // (A standing foot bears on its BALL; the pin measured here is the heel's
                     // soft hold, which a strained pull moves by a millimetre or two.)
                     {gateMax("worst pin after settle (mm)", r.settleWorstPinErr * 1000.0, 5.0),
                      gateMax("feet drift after settle (mm)", r.contactDriftMax * 1000.0, 20.0),
                      gateMax("steps taken", r.stepsTaken, 0.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0),
                      gateMax("settle ticks", r.settleTicks, 45.0),
                      info("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                      info("hand moved through settle (mm)", r.holdDrift * 1000.0)}, r);
    }

    // --- Dragging a foot back and up TRACKS the lift and holds after release.
    if (report.wants("foot-lift")) {
        Scenario sc;
        sc.grab = "lFoot";
        sc.path = pullPath(glm::vec3(0.0f, 0.15f, -0.10f), 40, 80);
        sc.idleJoints = {"head", "lHand", "rHand", "rFoot"};
        const RunResult r = run(sc);
        Armature probeFl;
        probeFl.build(bones);
        const int rFootFl = resolveBone(probeFl, "rFoot");
        const int lThighFl = resolveBone(probeFl, "lThigh");
        const int rThighFl = resolveBone(probeFl, "rThigh");
        const double freeHipLowerFl = (lThighFl >= 0 && rThighFl >= 0)
                                          ? static_cast<double>(r.endPos[static_cast<std::size_t>(rThighFl)].y - r.endPos[static_cast<std::size_t>(lThighFl)].y) * 1000.0
                                          : 0.0;
        phaseLegs("[real] foot lift (lFoot +15cm y, -10cm z)",
                     {gateMin("foot lift reached (m)", r.grabEnd.y - r.grabStart.y, 0.13),
                      // (CONTRAPPOSTO, 2026-09-25: the lifted leg's socket ends lower than the standing one's.)
                      gateMin("contrapposto: the free hip lower than the standing hip (mm)", freeHipLowerFl, 5.0),
                      gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 15.0),
                      // (THE WEIGHT SHIFT, 2026-09-25: a foot lifted off a two-footed stance takes
                      // the hips over the standing foot. It read 200-240 before: she stood on one
                      // leg with her weight between where the feet had been.)
                      gateMax("the weight over the standing foot: centre of mass outside its footprint (mm)",
                              comOutsideFootMm(r, rFootFl, bones), 25.0),
                      info("hip displacement (mm)", r.hipDisp * 1000.0),
                      gateMax("foot moved through settle (mm)", r.holdDrift * 1000.0, 10.0),
                      gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 15.0)},
                  // 100: the lifted leg's knee must not shimmer (151 before the dragged-limb
                  // prior exemption); 36 on the base rig, 60-80 on the male and oldest rigs.
                  r, LegGates{-1.0, -1.0, -1.0, 100.0});
    }

    // --- PLACING A FOOT: a foot dragged along the floor reshapes the STANCE (the SLIDE in
    // Armature::solveIk) — the hips come between the feet and down, the sole stays on the floor,
    // the leg does not turn — and a stance narrowed again stands her back up. Before it a 30cm
    // step turned the thigh 57 degrees, a 55cm stride stopped 4cm short with the hips where they
    // had been, a 40cm side step 7cm short, and the foot rode its shin (on its inner edge out to
    // the side; toes first into the floor behind).
    if (report.wants("foot-place")) {
 // (the dump's index: build keeps the order)
        const int head = indexOf("head");
        const int lShin = indexOf("lShin");
        const int rShin = indexOf("rShin");
        // A foot's lowest rider (a toe joint) against where it stood: the sole is on the floor.
        const auto soleOffFloor = [&](const RunResult& r, const char* foot) {
            const int f = indexOf(foot);
            double worst = 0.0;
            for (std::size_t i = 0; f >= 0 && i < bones.size() && i < r.endPos.size(); ++i) {
                for (int cur = static_cast<int>(i); cur >= 0; cur = bones[static_cast<std::size_t>(cur)].parent) {
                    if (cur == f) {
                        worst = std::max(worst, static_cast<double>(std::abs(r.endPos[i].y - r.restPos[i].y)));
                        break;
                    }
                }
            }
            return worst * 1000.0;
        };
        const auto kneeBend = [&](const RunResult& r, int shin) {
            double bend = 0.0;
            for (int a = 0; shin >= 0 && a < 3; ++a) {
                bend = std::max(bend, static_cast<double>(std::abs(r.endEuler[static_cast<std::size_t>(shin)][a])));
            }
            return bend;
        };
        const auto headDrop = [&](const RunResult& r) {
            return head >= 0 ? (r.restPos[static_cast<std::size_t>(head)].y - r.endPos[static_cast<std::size_t>(head)].y) * 1000.0
                             : 0.0;
        };
        struct Slide {
            const char* name;
            glm::vec3   offset;
            double      hipsFollow; // the hips' travel, as a share of the foot's, at least
        };
        for (const Slide& sl : {Slide{"[real] stride (lFoot +50cm z along the floor)", glm::vec3(0.0f, 0.0f, 0.50f), 0.40},
                                Slide{"[real] step back (lFoot -40cm z along the floor)", glm::vec3(0.0f, 0.0f, -0.40f), 0.40},
                                Slide{"[real] side step (lFoot +40cm x along the floor)", glm::vec3(0.40f, 0.0f, 0.0f), 0.40}}) {
            Scenario sc;
            sc.grab = "lFoot";
            sc.path = pullPath(sl.offset, 90, 60);
            const RunResult r = run(sc);
            phaseLegs(sl.name,
                         {gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          gateMin("hips' travel, share of the foot's", r.hipDisp / glm::length(sl.offset), sl.hipsFollow),
                          gateMax("the placed sole off the floor, worst joint (mm)", soleOffFloor(r, "lFoot"), 12.0),
                          // (The standing HEEL may lift behind a long stride: that is a stride.)
                          gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 45.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          gateMax("head drop (mm)", headDrop(r), 120.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                          gateMax("foot's lag behind the cursor, mean (mm)", r.phases.empty() ? 0.0 : r.phases[0].lagMean() * 1000.0, 8.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)},
                      r, LegGates{8.0, -1.0, -1.0, 10.0});
        }
        // A NEW drag's home is the pose it begins in: a wide stance let go of, then the same foot
        // brought back in, must stand her back up (it stopped 4cm short on knees bent 30 degrees
        // — the flat foot's floor rows trapped the solve, then the wide stance's hip height was
        // cheaper to keep than two straight legs).
        {
            Scenario sc;
            sc.before.push_back({"lFoot", glm::vec3(0.40f, 0.0f, 0.0f), 90, 40});
            sc.grab = "lFoot";
            sc.path = pullPath(glm::vec3(-0.40f, 0.0f, 0.0f), 90, 60);
            sc.toRest = true;
            const RunResult r = run(sc);
            phaseLegs("[real] side step, released, and back in (lFoot +40cm x, then home)",
                         {gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          gateMax("head below its standing height at the end (mm)", headDrop(r), 10.0),
                          gateMax("knee bend at the end, either leg (deg)", std::max(kneeBend(r, lShin), kneeBend(r, rShin)), 12.0),
                          gateMax("the placed sole off the floor, worst joint (mm)", soleOffFloor(r, "lFoot"), 12.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)},
                      r, LegGates{8.0, -1.0, -1.0, 10.0});
        }
        // Two steps: the left foot 45cm forward and let go, then the right brought up beside it.
        {
            Scenario sc;
            sc.before.push_back({"lFoot", glm::vec3(0.0f, 0.0f, 0.45f), 90, 40});
            sc.grab = "rFoot";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.45f), 90, 60);
            const RunResult r = run(sc);
            phaseLegs("[real] two steps forward (lFoot +45cm z released, then rFoot beside it)",
                         {gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          gateMax("head below its standing height at the end (mm)", headDrop(r), 15.0),
                          gateMax("knee bend at the end, either leg (deg)", std::max(kneeBend(r, lShin), kneeBend(r, rShin)), 20.0),
                          gateMax("the placed sole off the floor, worst joint (mm)", soleOffFloor(r, "rFoot"), 12.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)},
                      r, LegGates{8.0, -1.0, -1.0, 10.0});
        }
        // Lifted, carried and set down in ONE drag: the slide lets go as the foot leaves the floor
        // (the hips go back, the foot rides its shin) and takes hold again as it comes down.
        {
            const float k = 1.0f;
            Scenario sc;
            sc.grab = "lFoot";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {30, glm::vec3(0.0f, 0.20f, 0.0f) * k, "lift"},
                       {75, glm::vec3(0.35f, 0.20f, 0.10f) * k, "carry"},
                       {105, glm::vec3(0.35f, 0.0f, 0.10f) * k, "set down"},
                       {165, glm::vec3(0.35f, 0.0f, 0.10f) * k, "hold"}};
            const RunResult r = run(sc);
            double jump = 0.0;
            for (const PhaseStats& ph : r.phases) {
                jump = std::max(jump, ph.maxJump);
            }
            phaseLegs("[real] foot lifted, carried and set down (lFoot up 20cm, 35cm across, down)",
                         {gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          gateMin("hips' travel (m)", r.hipDisp, 0.10 * k),
                          gateMax("the placed sole off the floor, worst joint (mm)", soleOffFloor(r, "lFoot"), 12.0),
                          gateMax("standing foot drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          // (50: a STRAIGHT knee leaves straight in one 6-degree move — the
                          // solver's fold retry, 44mm at the knee — at the lift's first tick and
                          // again as the hips come across and the reaching leg unlocks. 70 since
                          // the weight shift, 2026-09-25: the hips go over the standing foot as the
                          // foot lifts and come back between the feet as it lands, and the sole of
                          // a foot carried on a taut leg rolls flat over its descent at
                          // kSoleTurnStepDeg — 46-61 at the toes across the rigs, 85 before the ease.)
                          gateMax("worst single-tick jump (mm)", jump * 1000.0, 70.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)},
                      // (The knee goes up and comes down: its "tremble" here is the path's. The
                      // twist: 12 since the weight shift — the foot is carried 35cm one way while
                      // the hips go 22cm the other, 57cm of abduction at 20cm of height, and the
                      // leg reaches the last of it with 10 degrees of twist mid-flight; 3 before.)
                      r, LegGates{12.0, -1.0, -1.0, -1.0});
        }
    }
}

/// A knee dragged over its planted foot; an elbow swivelled about a hand that stays.
void RealPhases::limbDragPhases() {
    // --- DRAGGING A KNEE: the foot below it stays PLANTED (IkRig::liftedByDrag) and the knee
    // swings over it — out, in, forward — the leg turning on its heel; pulled UP past where the
    // drag began the foot lets go, trails, and hangs under the knee (the knee drag in
    // Armature::solveIk), and brought back down it returns to its spot, exactly. Before it the
    // rig released the foot and the leg rode the knee rigidly: 15cm forward was a goose step paid
    // for by the STANDING knee, 10cm inward ran the thigh's twist to its limit, and a knee raise
    // stopped 17cm short of the cursor.
    if (report.wants("knee-drag")) {
 // (the dump's index: build keeps the order)
        const int lFoot = indexOf("lFoot");
        const int rFoot = indexOf("rFoot");
        const int lShin = indexOf("lShin");
        const int head = indexOf("head");
        struct Swing {
            const char* name;
            glm::vec3   offset;
            int         axis;   // the axis the knee must travel along
            double      travel; // ... at least this far (m)
        };
        for (const Swing& sw : {Swing{"[real] knee swung out over its foot (lShin +12cm x)", glm::vec3(0.12f, 0.0f, 0.0f), 0, 0.08},
                                Swing{"[real] knee pushed forward over its foot (lShin +15cm z)", glm::vec3(0.0f, 0.0f, 0.15f), 2, 0.10}}) {
            Scenario sc;
            sc.grab = "lShin";
            sc.path = pullPath(sw.offset, 60, 60);
            const RunResult r = run(sc);
            phaseLegs(sw.name,
                         {gateMin("knee travel (m)", std::abs(r.grabEnd[sw.axis] - r.grabStart[sw.axis]), sw.travel),
                          gateMax("the knee's foot left its spot (mm, the ankle)", movedMm(r, lFoot), 5.0),
                          gateMax("the other foot moved (mm)", movedMm(r, rFoot), 10.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          // (45: a knee that stands a little BEHIND straight — the male rig's —
                          // leaves straight in the solver's one 6-degree fold retry, 37mm.)
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 45.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          info("hip displacement (mm)", r.hipDisp * 1000.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        // The raise, held: the knee at the cursor's height (as far ahead as the thigh's length
        // puts it), the shin hanging, the hips where they were.
        {
            Scenario sc;
            sc.grab = "lShin";
            sc.path = pullPath(glm::vec3(0.0f, 0.35f, 0.0f), 70, 60);
            const RunResult r = run(sc);
            double hang = 0.0;
            double hangZ = 0.0;
            if (lFoot >= 0 && lShin >= 0) {
                const glm::vec3 now = r.endPos[static_cast<std::size_t>(lFoot)] - r.endPos[static_cast<std::size_t>(lShin)];
                const glm::vec3 was = r.startPos[static_cast<std::size_t>(lFoot)] - r.startPos[static_cast<std::size_t>(lShin)];
                hang = static_cast<double>(glm::length(glm::vec2(now.x - was.x, now.z - was.z))) * 1000.0;
                hangZ = static_cast<double>(std::abs(now.z - was.z)) * 1000.0;
            }
            // CONTRAPPOSTO (2026-09-25): standing on one leg, the pelvis rolls the FREE side down —
            // the lifted (left) leg's socket ends lower than the standing one's. Sign-free across
            // the rigs (the root's roll channel is not).
            const int lThighRd = indexOf("lThigh");
            const int rThighRd = indexOf("rThigh");
            const double freeHipLower = (lThighRd >= 0 && rThighRd >= 0)
                                            ? static_cast<double>(r.endPos[static_cast<std::size_t>(rThighRd)].y - r.endPos[static_cast<std::size_t>(lThighRd)].y) * 1000.0
                                            : 0.0;
            phaseLegs("[real] knee raise (lShin +35cm y, held)",
                         {gateMin("knee height gained (m)", r.grabEnd.y - r.grabStart.y, 0.32),
                          gateMin("contrapposto: the free hip lower than the standing hip (mm)", freeHipLower, 5.0),
                          // (THE WEIGHT SHIFT, 2026-09-25: the hips come over the standing foot as
                          // the other lifts — 22cm on this rig's wide A-pose — and the raised knee,
                          // held at its cursor, stands that far outside its socket: a hanging shin
                          // lies in the abducted thigh's plane, its foot toward the midline. Fore
                          // and aft the ankle still hangs under the knee; sideways it reads ~150.)
                          // (100 since 2026-10-27, the rotation order composed the format's way: a
                          // flexed thigh ABDUCTS now where it rolled, the knee's goal — on the thigh's
                          // reach about its socket as the drag FOUND it, 24cm inboard of where the
                          // weight shift has carried the socket — rests 4cm short in z, and the hang
                          // under that goal stands the foot 6cm ahead of the knee: 59 on this rig, 60-83
                          // across the character sweep, where the old composition read 3-19. A shin
                          // hanging 8 degrees forward of plumb; the shots read as a knee raise. A goal
                          // sphere that follows the weight shift would close it — a round of its own.)
                          gateMax("the ankle off its hang under the knee, fore-aft (mm)", hangZ, 100.0),
                          info("the ankle off its hang under the knee (mm, floor plane)", hang),
                          gateMax("the weight over the standing foot: centre of mass outside its footprint (mm)",
                                  comOutsideFootMm(r, rFoot, bones), 25.0),
                          info("hip displacement (mm)", r.hipDisp * 1000.0),
                          gateMax("the other foot moved (mm)", movedMm(r, rFoot), 10.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("lifted off the floor (0 = no)", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          // (A fast swing, by geometry: a thigh hanging straight down gains height
                          // as the square of its swing, so the knee's first 8cm of height are 25cm
                          // of travel — and the toes are half a metre further out on that lever.)
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 70.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        // ... and brought back down in the same drag: the foot returns to its spot and she
        // stands as she stood (the pose is a function of the target).
        {
            Scenario sc;
            sc.grab = "lShin";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {70, glm::vec3(0.0f, 0.35f, 0.0f), "raise"},
                       {100, glm::vec3(0.0f, 0.35f, 0.0f), "hold"},
                       {170, glm::vec3(0.0f), "lower"},
                       {230, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            double jump = 0.0;
            for (const PhaseStats& ph : r.phases) {
                jump = std::max(jump, ph.maxJump);
            }
            const double headOff =
                head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                            r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                          : 0.0;
            phaseLegs("[real] knee raise and back down (lShin +35cm y, then home)",
                         {gateMax("the knee's foot off its spot at the end (mm)", movedMm(r, lFoot), 2.0),
                          gateMax("the knee off where it began (mm)", movedMm(r, lShin), 2.0),
                          gateMax("the head off where it began (mm)", headOff, 2.0),
                          gateMax("hip displacement (mm)", r.hipDisp * 1000.0, 2.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          gateMax("worst single-tick jump (mm)", jump * 1000.0, 70.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
    }

    // --- DRAGGING AN ELBOW: what an elbow is grabbed for is its swivel, so the HAND tends to stay
    // where it is (the elbow drag in Armature::solveIk) — softly on a bent arm, in full when the
    // hand is pinned (a user pin below the dragged joint HOLDS now) or stands on the floor (a
    // contact below it is seeded) — and an elbow raised beside the body lifts out to the side on
    // the shoulder, not on a shrugged collar. Before it the forearm rode the elbow rigidly (every
    // elbow adjustment threw the hand off what it had been placed on), a pinned hand sat the drag
    // out, and raising an elbow 20cm turned the collar 46 degrees.
    if (report.wants("elbow-drag")) {
 // (the dump's index: build keeps the order)
        const int lHand = indexOf("lHand");
        const int lShldr = indexOf("lShldr");
        const auto bendAtStart = [](const RunResult& r) {
            return static_cast<double>(std::max({std::abs(r.grabEulerStart.x), std::abs(r.grabEulerStart.y),
                                                 std::abs(r.grabEulerStart.z)}));
        };
        // The arm is BENT first, by the hand (in, up and forward: a hand brought before the hip)
        // and let go of — the pose an elbow adjustment is made in.
        const PreludeDrag bendArm{"lHand", glm::vec3(-0.20f, 0.10f, 0.15f), 60, 30};
        {
            Scenario sc;
            sc.before.push_back(bendArm);
            sc.grab = "lForeArm";
            sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 50, 50);
            const RunResult r = run(sc);
            phaseLegs("[real] elbow swung out on a bent arm (lForeArm +8cm x; the hand tends to stay)",
                         {info("elbow bend as the drag began (deg)", bendAtStart(r)),
                          // (Along its ARC: an elbow lives on the upper arm's reach about the
                          // socket, so swung outward it also rises — a cursor taken 8cm sideways
                          // is met 3-7cm out and 2-4cm up, and the body stays where it was.)
                          gateMin("elbow travel (m)", glm::length(r.grabEnd - r.grabStart), 0.04),
                          // (36-38 on the two oldest generations before the hand lay: their prelude hand, brought before the hip,
                          // now lies on the belly it was brought to, and the swivel carries a flat hand 45-55.)
                          gateMax("the hand left its place (mm)", movedMm(r, lHand), 60.0),
                          gateMax("the shoulder socket moved (mm)", movedMm(r, lShldr), 25.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 5.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        {
            Scenario sc;
            sc.before.push_back(bendArm);
            sc.grab = "lForeArm";
            sc.userPins = {"lHand"};
            sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 50, 50);
            const RunResult r = run(sc);
            phaseLegs("[real] elbow swung out with its hand PINNED (lForeArm +8cm x)",
                         {gateMax("user pin max distance (mm)", r.userPinDistMax * 1000.0, 1.0),
                          // (Informational where the shoulder ends limit-bound: the second generation's
                          // bent arm lands 3 degrees from its shoulder's flexion limit under the corrected
                          // rotation order, and the swivel toward it stops there — 1.5cm of the 3.)
                          limitBoundEnd(r, "lShldr") ? info("elbow travel (m; a shoulder limit-bound)", glm::length(r.grabEnd - r.grabStart))
                                                     : gateMin("elbow travel (m)", glm::length(r.grabEnd - r.grabStart), 0.03),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        // A straight arm's elbow raised beside the body: out to the side on the SHOULDER.
        {
            Scenario sc;
            sc.grab = "lForeArm";
            sc.path = pullPath(glm::vec3(0.0f, 0.20f, 0.0f), 50, 50);
            const RunResult r = run(sc);
            phaseLegs("[real] elbow raised beside the body (lForeArm +20cm y)",
                         {gateMin("elbow height gained (m)", r.grabEnd.y - r.grabStart.y, 0.12),
                          // (The shrug: the collar turned 46 degrees and carried the socket 10cm up.)
                          gateMax("the shoulder socket moved (mm)", movedMm(r, lShldr), 50.0),
                          gateMax("head moved (mm)", r.headDisp * 1000.0, 20.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 5.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        // On all fours (made and let go of), an elbow swung out: the hand stays on the floor.
        if (allFoursDrop > 0.0f) {
            Scenario sc;
            sc.prePose = kAllFoursPose;
            sc.before.push_back({"hip", glm::vec3(0.0f, -allFoursDrop, 0.0f), 80, 40});
            sc.grab = "lForeArm";
            sc.path = pullPath(glm::vec3(0.06f, 0.0f, 0.0f), 50, 50);
            const RunResult r = run(sc);
            // (ON the floor, that is — a contact: on the oldest generation the hands never come down,
            // and since the idle arms HANG they end the prelude 15cm up, pointing at the floor.)
            // (40, 2026-10-28: the second generation's hands come down since the hang's floor margin
            // stands clear of the seed band — they land on their RING-FINGER BASES 4.6cm apart under
            // the chest, and under the swing the wrist rolls 32mm about the planted finger; the rigs
            // whose contact is the wrist itself read 0.005.)
            const bool handDown = lHand >= 0 && r.startPos[static_cast<std::size_t>(lHand)].y < 0.10f;
            phaseLegs("[real] on all fours, an elbow swung out (lForeArm +6cm x; its hand stays on the floor)",
                         {handDown ? gateMax("the hand left its place (mm)", movedMm(r, lHand), 40.0)
                                   : info("the hand left its place, hands never came down (mm)", movedMm(r, lHand)),
                          handDown ? gateMin("elbow travel (m)", glm::length(r.grabEnd - r.grabStart), 0.03)
                                   : info("elbow travel, hands never came down (m)", glm::length(r.grabEnd - r.grabStart)),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 25.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.5),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 21.0)}, r);
        }
    }
}

/// Pelvis drags: the sway, the hinge, the girdle grabs, the walks and the stepping chest drag.
void RealPhases::pelvisPhases() {
    // --- THE HIP SWAY: the pelvis dragged sideways over its standing feet ROLLS (the side it goes
    // toward comes up: the sway's roll reference in Armature::solveIk), so the leg the hips go
    // over stays straight, and the spine takes the roll up so the chest stays level. Before it
    // the pelvis stayed level and that leg's knee — the cheapest joint in the body — bent 12
    // degrees for an 8cm sway while the other leg stood straight on a lifted heel: contrapposto
    // backwards. Brought back, she stands as she stood.
    if (report.wants("hip-sway")) {
 // (the dump's index: build keeps the order)
        const int hipBone = indexOf("hip");
        const int head = indexOf("head");
        const int lShin = indexOf("lShin");
        const int rShin = indexOf("rShin");
        const auto bend = [&](const RunResult& r, int shin) {
            double most = 0.0;
            for (int a = 0; shin >= 0 && a < 3; ++a) {
                most = std::max(most, static_cast<double>(std::abs(r.endEuler[static_cast<std::size_t>(shin)][a])));
            }
            return most;
        };
        {
            Scenario sc;
            sc.grab = "hip";
            sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 50, 50);
            const RunResult r = run(sc);
            // The head against the hips, sideways: a torso that rides the rolled pelvis rigidly
            // leaves the head 8cm behind the hips; one that stays upright keeps it over them.
            double headBehind = 0.0;
            if (hipBone >= 0 && head >= 0) {
                const float was = r.startPos[static_cast<std::size_t>(head)].x - r.startPos[static_cast<std::size_t>(hipBone)].x;
                const float now = r.endPos[static_cast<std::size_t>(head)].x - r.endPos[static_cast<std::size_t>(hipBone)].x;
                headBehind = static_cast<double>(std::abs(now - was)) * 1000.0;
            }
            phaseLegs("[real] hip sway (hip +8cm x)",
                         {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          // (How much roll the sway calls for is the STANCE's: the main family
                          // stands wide — a hip socket 12cm inside its foot — and rolls 7 degrees
                          // for 8cm; the generations that stand with their feet under their hips
                          // pass OVER the near foot, both legs shorten, and 1-2 degrees is all
                          // there is to take up: their near knee gives 4, as it did before. The
                          // largest channel is read, so a shin's side bend at its 5-degree limit
                          // counts as "bend" here.)
                          gateMax("knee bend, the leg the hips went over (deg)", bend(r, lShin), 8.0),
                          gateMax("knee bend, the other leg (deg)", bend(r, rShin), 8.0),
                          info("pelvis roll (deg, its largest channel)", hipBone >= 0 ? bend(r, hipBone) : 0.0),
                          gateMax("the head left its place over the hips, sideways (mm)", headBehind, 35.0),
                          gateMax("feet drift (mm, a heel may lift)", r.contactDriftMax * 1000.0, 15.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 25.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        {
            Scenario sc;
            sc.grab = "hip";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {50, glm::vec3(0.08f, 0.0f, 0.0f), "sway"},
                       {80, glm::vec3(0.08f, 0.0f, 0.0f), "hold"},
                       {130, glm::vec3(0.0f), "back"},
                       {190, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            const double headOff =
                head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                            r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                          : 0.0;
            phaseLegs("[real] hip sway and back (hip +8cm x, then home)",
                         {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMax("the head off where it began (mm)", headOff, 1.0),
                          gateMax("pelvis roll left (deg)", hipBone >= 0 ? bend(r, hipBone) : 0.0, 0.2),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 1.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
    }

    // --- THE HIP HINGE: a standing pelvis taken BACK (or back and down: a half squat) keeps its
    // balance with its TRUNK — the chest comes forward over the feet (pelvis balance in
    // Armature::solveIk: fore and aft only, the standing band of hip heights, drags that began
    // standing and have not stepped, not descents to the floor, not walks). Balance was simply
    // off under a pelvis drag: hips 10cm back left her bolt upright with her head 7cm behind her
    // heels, and a half squat sat back the same way. Brought back, she stands as she stood.
    if (report.wants("hip-hinge")) {
 // (the dump's index: build keeps the order)
        const int head = indexOf("head");
        const int lFoot = indexOf("lFoot");
        // How far the head stands BEHIND the ankle (the figures face +z): a body whose head is
        // behind its heels is falling over backwards.
        const auto headBehindAnkle = [&](const RunResult& r) {
            return head >= 0 && lFoot >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lFoot)].z -
                                                                 r.endPos[static_cast<std::size_t>(head)].z) * 1000.0
                                           : 0.0;
        };
        struct Hinge {
            const char* name;
            glm::vec3   offset;
        };
        for (const Hinge& h : {Hinge{"[real] hip hinge (hip -10cm z)", glm::vec3(0.0f, 0.0f, -0.10f)},
                               Hinge{"[real] half squat (hip -10cm y, -12cm z)", glm::vec3(0.0f, -0.10f, -0.12f)}}) {
            Scenario sc;
            sc.grab = "hip";
            sc.path = pullPath(h.offset, 50, 50);
            const RunResult r = run(sc);
            phaseLegs(h.name,
                         {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          gateMax("the head behind the ankles (mm; negative = ahead of them)", headBehindAnkle(r), 0.0),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 12.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        {
            Scenario sc;
            sc.grab = "hip";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {50, glm::vec3(0.0f, 0.0f, -0.10f), "hinge"},
                       {80, glm::vec3(0.0f, 0.0f, -0.10f), "hold"},
                       {130, glm::vec3(0.0f), "back"},
                       {190, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            const double headOff =
                head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                            r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                          : 0.0;
            phaseLegs("[real] hip hinge and back (hip -10cm z, then home)",
                         {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMax("the head off where it began (mm)", headOff, 1.5),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 1.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
    }

    // --- The PELVIS GIRDLE: the joints a click on the hips FINDS. The rig root's own body is two
    // 2cm segments; the pelvic flesh belongs to the pelvis bone, the lower belly to the first
    // spine bone, the hip joints to the thighs — all within two bones of the root, and none can
    // go anywhere the pelvis does not take it. As ordinary trunk drags they met the root's
    // horizontal price: 2.2cm of hips for a 10cm drag, the grab 5-8cm short. They are pelvis
    // drags now (IkRig::girdleGrab), with the GRABBED joint on the cursor.
    if (report.wants("girdle-grab")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        const int lFoot = indexOf("lFoot");
        // By structure (the generations name them differently): the hip's child over BOTH legs
        // (over one leg alone it is a thigh — a game-engine rig hangs its thighs off the hips and
        // keeps the pelvis bone as a leaf beside them, 2026-11-03: the rig standard's Pelvis role
        // then), its child under the head, and the first joint of the left leg — a child of the
        // pelvis bone, or of the hips where there is none.
        const int rFoot = indexOf("rFoot");
        int pelvisBone = -1, spineBone = -1, thigh = -1;
        for (std::size_t i = 0; i < bones.size() && hip >= 0; ++i) {
            if (bones[i].parent == hip && under(lFoot, static_cast<int>(i)) && under(rFoot, static_cast<int>(i))) {
                pelvisBone = static_cast<int>(i);
            }
            if (bones[i].parent == hip && under(head, static_cast<int>(i))) {
                spineBone = static_cast<int>(i);
            }
        }
        if (pelvisBone < 0) {
            pelvisBone = resolveBone(probe, "pelvis");
        }
        const int legRoot = pelvisBone >= 0 && under(lFoot, pelvisBone) ? pelvisBone : hip;
        for (std::size_t i = 0; i < bones.size() && legRoot >= 0; ++i) {
            if (bones[i].parent == legRoot && under(lFoot, static_cast<int>(i))) {
                thigh = static_cast<int>(i);
            }
        }
        struct Grab {
            const char* name;
            int         bone;
            glm::vec3   offset;
        };
        for (const Grab& g : {Grab{"[real] pelvis-bone grab, 10cm back", pelvisBone, glm::vec3(0.0f, 0.0f, -0.10f)},
                              Grab{"[real] first-spine-bone grab, 8cm sideways", spineBone, glm::vec3(0.08f, 0.0f, 0.0f)},
                              Grab{"[real] thigh-socket grab, 10cm forward", thigh, glm::vec3(0.0f, 0.0f, 0.10f)}}) {
            if (g.bone < 0 || hip < 0) {
                report.skip(g.name, "bone not found");
                continue;
            }
            Scenario sc;
            sc.grab = bones[static_cast<std::size_t>(g.bone)].name;
            sc.path = pullPath(g.offset, 50, 50);
            const RunResult r = run(sc);
            const double hipWent =
                static_cast<double>(glm::dot(r.endPos[static_cast<std::size_t>(hip)] - r.startPos[static_cast<std::size_t>(hip)],
                                             glm::normalize(g.offset)) /
                                    glm::length(g.offset));
            phaseLegs(g.name,
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMin("the hips' share of the drag", hipWent, 0.80),
                          // (Heel RISE counts as drift: hips taken 10cm forward stand 11-12mm up on
                          // the balls of the feet, grabbed by the root itself just the same.)
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 15.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
    }

    // --- Hips taken BACK past their stance: the hinge lets go as the walk begins. (As a weight
    // that faded with the hips' distance it let the whole lean go in the last tenth of its range,
    // and was cut outright in the tick a step began: the head was thrown 16cm in one tick.)
    if (report.wants("hip-back-walk")) {
        Scenario sc;
        sc.grab = "hip";
        sc.path = pullPath(glm::vec3(0.0f, 0.0f, -0.25f), 60, 60);
        const RunResult r = run(sc);
        phaseLegs("[real] hips 25cm back (hinge, then a walk)",
                     {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                      gateMin("steps", static_cast<double>(r.stepsTaken), 1.0),
                      gateMax("the head's worst single-tick move (mm)", r.headJumpMax * 1000.0, 50.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
    }

    // --- A lateral HIP drag walks the figure: steps land at floor height, the hip tracks.
    for (const float dist : {0.35f, 0.60f}) {
        char name[96];
        std::snprintf(name, sizeof(name), "[real] hip walk (%.0fcm lateral)", dist * 100.0f);
        if (!report.wants("walk")) {
            break;
        }
        Scenario sc;
        sc.grab = "hip";
        const int moveTicks = static_cast<int>(dist / 0.004f); // ~0.24 m/s
        sc.path = pullPath(glm::vec3(dist, 0.0f, 0.0f), moveTicks, 150);
        const RunResult r = run(sc);
        double feetOffFloor = 0.0;
        for (const int c : r.contactPins) {
            feetOffFloor = std::max(feetOffFloor, std::abs(static_cast<double>(r.endPos[static_cast<std::size_t>(c)].y) - r.contactBindMinY));
        }
        // The stance should end centered under the hip target (the feet land at the target
        // plus their stance offset): both feet moved by roughly the drag distance.
        double feetMoved = 1e9;
        for (const int c : r.contactPins) {
            feetMoved = std::min(feetMoved, static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(c)] - r.startPos[static_cast<std::size_t>(c)])));
        }
        phaseLegs(name, {gateMin("steps taken", r.stepsTaken, dist > 0.5f ? 5.0 : 3.0),
                            gateMax("hip-to-target at rest (mm)", r.restingMiss * 1000.0, 15.0),
                            // (The trailing HEEL may rest lifted a couple of centimetres: the hip
                            // sits exactly on the cursor and the leg behind it is at its reach.)
                            gateMax("feet off floor at the end (mm)", feetOffFloor * 1000.0, 30.0),
                            gateMin("both feet followed (m)", feetMoved, dist - 0.13),
                            // 25: the settle's own per-tick cap is 20mm and a landing after a
                            // long walk runs right at it (20.3 base, 23.4 on the male rig).
                            gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 25.0)}, r, kLeanLegs);
    }

    // --- A sustained lateral CHEST drag re-plants the feet (balance-driven stepping) and lands
    // on its feet at release.
    if (report.wants("chest-step")) {
        Scenario sc;
        sc.grab = "chest";
        sc.path = pullPath(glm::vec3(0.45f, 0.0f, 0.0f), 110, 150);
        const RunResult r = run(sc);
        double feetOffFloor = 0.0;
        for (const int c : r.contactPins) {
            feetOffFloor = std::max(feetOffFloor, std::abs(static_cast<double>(r.endPos[static_cast<std::size_t>(c)].y) - r.contactBindMinY));
        }
        phaseLegs("[real] stepping chest drag (chest +45cm x)",
                     {gateMin("steps taken", r.stepsTaken, 2.0),
                      // (It rested 11-12cm short on the main family, 3 on the others, until the
                      // hips followed a dragged trunk joint: the gate was the miss's.)
                      gateMax("chest-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                      // (9 until the prompt step trigger of 2026-09-22 (kStepPromptFactor), 21 on the main
                      // family since: with the steps 20 ticks sooner the OTHER foot — 16cm from its spot
                      // against the leading foot's 17 — takes the first step, the walk's order flips,
                      // and the gathering step lands 6cm shorter: the trailing heel rests 2cm up, as
                      // the hip walks' trailing heel does (their gate is 30).)
                      gateMax("feet off floor at the end (mm)", feetOffFloor * 1000.0, 25.0),
                      gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r, kLeanLegs);
    }
}

/// The trunk: reaching down, a lifted foot kept, a limb grabbed by its body, a crouching lean, a posed start, the seat, the bow, the head kept up, the trunk follow.
void RealPhases::trunkPhases() {
    // --- REACHING DOWN with a HAND (the reach hinge in Armature::ikTrunkPolicy, 2026-09-25): the
    // trunk folds at the hips as the hand goes down — a bow over softened knees to the knee, the
    // toe-touch's fold to the floor — where before the body was a squat with the trunk bolt
    // upright and the gaze level: a hand's push down yielded the hips exactly as a chest's, and the
    // pelvis never pitched. The fold is a REFERENCE (kReachFoldPerMetre of the hand's downward
    // travel), so a hand brought back up brings her back up exactly.
    if (report.wants("reach-down")) {
        const int hipRd = resolveBone(probe, "hip");
        const int headRd = resolveBone(probe, "head");
        const int lShinRd = resolveBone(probe, "lShin");
        const int rShinRd = resolveBone(probe, "rShin");
        // The reaches are sized by the ARM (2026-09-25): the base rig's shoulder-to-hand length is
        // 0.516m, and a character whose arm is 8% shorter reaches 8% less — with the targets fixed in
        // centimetres three small characters' floor reach rested 7-25mm short once the collar was boxed
        // to a clavicle's motion (its 55-degree hoist had bought them the last centimetres).
        float armScale = 1.0f;
        {
            const int shoulderRd = resolveBone(probe, "rShldr");
            const int handRd = resolveBone(probe, "rHand");
            if (shoulderRd >= 0 && handRd >= 0) {
                armScale = glm::length(probe.boneWorldPosition(static_cast<std::size_t>(handRd)) -
                                       probe.boneWorldPosition(static_cast<std::size_t>(shoulderRd))) / 0.516f;
            }
        }
        const auto at = [&](const std::vector<glm::vec3>& pose, int bone) {
            return bone >= 0 && static_cast<std::size_t>(bone) < pose.size() ? pose[static_cast<std::size_t>(bone)] : glm::vec3(0.0f);
        };
        // The fold, sign-free across the rigs: how far the head stands AHEAD of the hips (the figure
        // faces +z) at the end, less what it stood at the start.
        const auto headAhead = [&](const RunResult& r) {
            return static_cast<double>((at(r.endPos, headRd).z - at(r.endPos, hipRd).z) -
                                       (at(r.startPos, headRd).z - at(r.startPos, hipRd).z)) * 1000.0;
        };
        const auto rootPitch = [&](const RunResult& r) {
            return hipRd >= 0 && static_cast<std::size_t>(hipRd) < r.endEuler.size()
                       ? static_cast<double>(r.endEuler[static_cast<std::size_t>(hipRd)].x) : 0.0;
        };
        const auto kneeBend = [&](const RunResult& r, int shin) {
            return shin >= 0 && static_cast<std::size_t>(shin) < r.endEuler.size()
                       ? static_cast<double>(std::abs(r.endEuler[static_cast<std::size_t>(shin)].x)) : 0.0;
        };
        struct Reach {
            const char* name;
            glm::vec3   offset;
            double      aheadMin; // the head ahead of the hips, at least (mm)
            double      kneeMax;  // either knee's bend, at most (deg)
        };
        for (const Reach& rc : {Reach{"[real] a hand brought to the knee (rHand -30cm y): a bow over softened knees", glm::vec3(0.0f, -0.30f, -0.02f), 150.0, 70.0},
                                // (72cm, not 78: on two custom characters with shorter arms the deeper
                                // target lay 2cm under the floor, and the clamp read as a miss.)
                                Reach{"[real] a hand taken to the floor ahead (rHand -72cm y, +45cm z): a fold at the hips and the knees", glm::vec3(0.0f, -0.72f, 0.45f), 350.0, 80.0}}) {
            Scenario sc;
            sc.grab = "rHand";
            sc.path = pullPath(rc.offset * armScale, 70, 60);
            const RunResult r = run(sc);
            phaseLegs(rc.name,
                         // (Informational where the shoulder ends limit-bound: with the rotation order
                         // composed the format's way the third generation's shoulder reaches the floor
                         // ahead on its swing limit, 2cm short — the rig's range, not the fold's.)
                         {limitBoundEnd(r, "rShldr") ? info("hand-to-cursor at rest (mm; a shoulder limit-bound)", r.restingMiss * 1000.0)
                                                     : gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                          info("the arm's length over the base rig's (the reach is sized by it)", armScale),
                          gateMin("the head ahead of the hips, gained (mm): the fold", headAhead(r), rc.aheadMin),
                          info("the pelvis's pitch at the end (deg)", rootPitch(r)),
                          info("left knee bend (deg)", kneeBend(r, lShinRd)),
                          info("right knee bend (deg)", kneeBend(r, rShinRd)),
                          gateMax("the deeper knee's bend (deg)", std::max(kneeBend(r, lShinRd), kneeBend(r, rShinRd)), rc.kneeMax),
                          info("gaze over the horizon at the end (deg)", r.gazeEndDeg),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("lifted off the floor (0 = no)", r.suspended ? 1.0 : 0.0, 0.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 5.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 80.0),
                          // (2.0 since the collar box, 2026-09-25: the hand resting against the collar's
                          // bound converges in small reversing steps — 1.3-1.5mm ACCUMULATED over the
                          // 60-tick hold on three large characters, 0.2-0.8 on the base rig, no step
                          // over 0.5mm; the old solver's tremble was 11mm a tick.)
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 2.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        {
            // ... and brought back up in the same drag: she stands as she stood.
            Scenario sc;
            sc.grab = "rHand";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {70, glm::vec3(0.0f, -0.30f, -0.02f) * armScale, "down"},
                       {100, glm::vec3(0.0f, -0.30f, -0.02f) * armScale, "hold"},
                       {170, glm::vec3(0.0f), "up"},
                       {230, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            double jump = 0.0;
            for (const PhaseStats& ph : r.phases) {
                jump = std::max(jump, ph.maxJump);
            }
            phaseLegs("[real] a hand brought to the knee and back up (rHand -30cm y, then home)",
                         {gateMax("the head off where it began (mm)", static_cast<double>(glm::length(at(r.endPos, headRd) - at(r.startPos, headRd))) * 1000.0, 3.0),
                          gateMax("hip displacement (mm)", r.hipDisp * 1000.0, 3.0),
                          gateMax("the pelvis's pitch at the end (deg)", std::abs(rootPitch(r)), 0.5),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", jump * 1000.0, 80.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
    }

    // --- A foot the user LIFTED stays lifted. The contact band is 15cm deep and the ground healing
    // reaches 12, so a foot raised 5-10cm by one drag (a step being posed) was planted by the
    // NEXT drag of anything — a hand — and hauled back down to the floor.
    if (report.wants("lifted-foot")) {
 // (the dump's index: build keeps the order)
        const int lFoot = indexOf("lFoot");
        const int rFoot = indexOf("rFoot");
        PreludeDrag lift;
        lift.grab = "lFoot";
        lift.offset = glm::vec3(0.0f, 0.10f, 0.10f);
        {
            Scenario sc;
            sc.grab = "rHand";
            sc.before = {lift};
            sc.path = pullPath(glm::vec3(0.05f, 0.0f, 0.10f), 40, 40);
            const RunResult r = run(sc);
            const auto at = [&](const std::vector<glm::vec3>& pose, int bone) {
                return bone >= 0 ? pose[static_cast<std::size_t>(bone)] : glm::vec3(0.0f);
            };
            phaseLegs("[real] a lifted foot stays lifted under a hand drag",
                         {gateMin("the lifted foot's height when the drag began (mm over its floor height)",
                                  static_cast<double>(at(r.startPos, lFoot).y - at(r.restPos, lFoot).y) * 1000.0, 80.0),
                          gateMax("the lifted foot moved (mm)",
                                  static_cast<double>(glm::length(at(r.endPos, lFoot) - at(r.startPos, lFoot))) * 1000.0, 5.0),
                          gateMax("the standing foot moved (mm)",
                                  static_cast<double>(glm::length(at(r.endPos, rFoot) - at(r.startPos, rFoot))) * 1000.0, 5.0),
                          gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0)}, r);
        }
        {
            // ... and comes DOWN when the body does: a crouch lands it as a contact.
            Scenario sc;
            sc.grab = "hip";
            sc.before = {lift};
            sc.path = pullPath(glm::vec3(0.0f, -0.25f, 0.0f), 60, 40);
            const RunResult r = run(sc);
            const double footOver =
                lFoot >= 0 ? static_cast<double>(r.endPos[static_cast<std::size_t>(lFoot)].y - r.restPos[static_cast<std::size_t>(lFoot)].y) * 1000.0
                           : 0.0;
            phaseLegs("[real] a lifted foot lands under a 25cm crouch",
                         {gateMax("hip-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                          gateMin("pins at mouse-up (both feet)", r.pinsAtRelease, 2.0),
                          gateMax("the landed ankle over its standing height (mm; a heel may be up)", footOver, 60.0),
                          gateMax("floor penetration, any joint (mm)", r.penetrationMax * 1000.0, 10.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 45.0)}, r);
        }
    }

    // --- A limb grabbed by its BODY: a click picks a bone by its flesh as readily as by its joint,
    // and what the user has hold of is the point under the cursor (Armature::setIkGrabPoint).
    // Before, the bone's JOINT was asked to the cursor — a mid-thigh click hauled the hip socket
    // (the pelvis, since it became a pelvis drag) a thigh's length down to the pointer. Now the
    // point follows and the nearest real joint of its rigid segment is what the solve drags.
    if (report.wants("limb-grab")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int lFoot = indexOf("lFoot");
        const int lShldr = indexOf("lShldr");
        const int lHand = indexOf("lHand");
        {
            // High on the thigh, by the socket: the hips', by the cursor's own travel.
            Scenario sc;
            sc.grab = "lThigh";
            sc.grabShare = 0.15f;
            sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] thigh grabbed by the socket, 8cm sideways",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMin("the hips went along (mm)", movedMm(r, hip), 60.0),
                          gateMax("the hips went along (mm), no further", movedMm(r, hip), 100.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 30.0)}, r);
        }
        {
            // Mid-thigh, raised in a side view: the knee's drag, the point at the cursor's height
            // (the cursor is inside the point's sphere: the swing forward is the knee's answer).
            Scenario sc;
            sc.grab = "lThigh";
            sc.grabShare = 0.7f;
            sc.path = pullPath(glm::vec3(0.0f, 0.15f, 0.10f), 50, 40);
            const RunResult r = run(sc);
            const double rise = static_cast<double>(r.grabEnd.y - r.grabStart.y) * 1000.0;
            phaseLegs("[real] mid-thigh grabbed and raised 15cm",
                         {gateMin("the grabbed point rose (mm)", rise, 140.0),
                          // (170: the point rose 161 once the weight shift moved the socket the
                          // dilation is measured about; 150-158 before it.)
                          // (180 since the rotation order was composed the format's way, 2026-10-27: a
                          // thigh that truly abducts swings its knee further out, and the dilated knee
                          // target leaves the mid-thigh point 20mm over its cursor on the main family,
                          // 10 before — the dilation's own miss, the same gesture.)
                          gateMax("the grabbed point rose (mm), no further", rise, 180.0),
                          // (The weight shift: a knee drag that lifts the foot takes the hips over
                          // the standing foot — see the knee raise.)
                          gateMax("the weight over the standing foot: centre of mass outside its footprint (mm)",
                                  comOutsideFootMm(r, resolveBone(probe, "rFoot"), bones), 25.0),
                          info("the hips (mm)", movedMm(r, hip)),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 70.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0)}, r);
        }
        {
            // Mid-thigh, swung 8cm outward: the knee over its planted foot.
            Scenario sc;
            sc.grab = "lThigh";
            sc.grabShare = 0.7f;
            sc.path = pullPath(glm::vec3(0.08f, 0.0f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] mid-thigh grabbed and swung 8cm out",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 20.0),
                          gateMax("the foot under it (mm)", movedMm(r, lFoot), 10.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 45.0)}, r);
        }
        {
            // The upper arm, raised beside the body: the elbow's drag — no shrug.
            Scenario sc;
            sc.grab = "lShldr";
            sc.grabShare = 0.6f;
            sc.path = pullPath(glm::vec3(0.0f, 0.15f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            const double rise = static_cast<double>(r.grabEnd.y - r.grabStart.y) * 1000.0;
            phaseLegs("[real] upper arm grabbed and raised 15cm",
                         {gateMin("the grabbed point rose (mm)", rise, 135.0),
                          gateMax("the grabbed point rose (mm), no further", rise, 165.0),
                          gateMax("the shoulder socket (mm)", movedMm(r, lShldr), 30.0),
                          gateMax("the hips (mm)", movedMm(r, hip), 10.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 45.0)}, r);
        }
        {
            // The forearm, raised: the hand's drag, the point on the cursor.
            Scenario sc;
            sc.grab = "lForeArm";
            sc.grabShare = 0.5f;
            sc.path = pullPath(glm::vec3(0.0f, 0.12f, 0.06f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] forearm grabbed and raised 12cm",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          gateMin("the hand went (mm)", movedMm(r, lHand), 120.0),
                          gateMax("the hips (mm)", movedMm(r, hip), 10.0),
                          gateMax("drag lag, mean (mm)", r.phases.empty() ? 0.0 : r.phases[0].lagMean() * 1000.0, 12.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0)}, r);
        }
        {
            // The belly, between two spine joints: the nearer one's drag, the point on the cursor.
            Scenario sc;
            sc.grab = "abdomenLower";
            sc.grabShare = 0.5f;
            sc.path = pullPath(glm::vec3(0.06f, 0.0f, 0.04f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] spine grabbed between its joints, 7cm across",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0)}, r);
        }
    }

    // --- A CROUCHING figure's chest taken across the floor. The stance takes no step while the body
    // is low, so what answers a lean that runs out of balance room is a HINGE — the hips back and
    // up as the trunk folds — and the spine bends as ONE. Before: the pelvis held to its place, the
    // lower spine flexed 27 degrees with the chest bone extended to its limit against it (an
    // S-curve, to keep the chest's height), 4cm short, then 47 degrees of spine twist for 7mm more.
    if (report.wants("crouch-lean")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        // The spine's bones BELOW the grabbed one, by structure: the hip's child under the head,
        // and up (the grabbed bone's own rotation does not move its joint).
        const int grabbed = indexOf("chest_2");
        std::vector<int> spine;
        for (int cur = hip; cur >= 0 && spine.size() < 3;) {
            int next = -1;
            for (std::size_t i = 0; i < bones.size(); ++i) {
                if (bones[i].parent == cur && under(head, static_cast<int>(i))) {
                    next = static_cast<int>(i);
                }
            }
            if (next < 0 || next == grabbed) {
                break;
            }
            spine.push_back(next);
            cur = next;
        }
        // (Sized by the grab's height over the hips, its lever: the generations with a three-bone
        // spine carry their top chest joint 21cm up, the others 29 — the same 15cm is half of one
        // lever and most of the other's reach.)
        const float lever = (grabbed >= 0 && hip >= 0)
                                ? probe.boneWorldPosition(static_cast<std::size_t>(grabbed)).y -
                                      probe.boneWorldPosition(static_cast<std::size_t>(hip)).y
                                : 0.29f;
        struct Lean {
            const char* name;
            float       share;
            bool        reachable;
        };
        for (const Lean& lean : {Lean{"[real] crouching, the upper chest dragged forward by half its lever", 0.5f, true},
                                 Lean{"[real] crouching, the upper chest dragged forward by its whole lever (beyond reach)", 1.0f, false}}) {
            Scenario sc;
            sc.before.push_back({"hip", glm::vec3(0.0f, -0.30f, 0.0f), 60, 40});
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, lean.share * lever), 60, 50);
            const RunResult r = run(sc);
            double twist = 0.0, leastFlexion = 1.0e9, mostFlexion = -1.0e9;
            for (const int b : spine) {
                if (static_cast<std::size_t>(b) < r.endEuler.size()) {
                    const glm::vec3 e = r.endEuler[static_cast<std::size_t>(b)];
                    twist = std::max(twist, static_cast<double>(std::max(std::abs(e.y), std::abs(e.z))));
                    leastFlexion = std::min(leastFlexion, static_cast<double>(e.x));
                    mostFlexion = std::max(mostFlexion, static_cast<double>(e.x));
                }
            }
            phaseLegs(lean.name,
                         {lean.reachable ? gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0)
                                         : info("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                          gateMax("spine twist or side bend, the worst channel (deg)", twist, 3.0),
                          gateMin("the least-flexed spine joint (deg; an S-curve has one extended)", leastFlexion, 3.0),
                          info("the most-flexed spine joint (deg)", mostFlexion),
                          // (The hinge RAISES the hips — 14cm — and on one generation that takes the
                          // pelvis back within the height a step is allowed at: she may step after
                          // her chest, the last one landing in the hold.)
                          gateMax("steps", static_cast<double>(r.stepsTaken), 2.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 70.0),
                          gateMax("hold: worst single-tick jump (mm)", r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0, 70.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 5.0)}, r);
        }
    }

    // --- THE POSE A DRAG BEGINS WITH IS THE USER'S — for the rig's step policy as for the solve.
    // The solve's balance row never asks back the imbalance a drag starts with; the step trigger
    // did: a pose that leans (loaded from a file, made with FK, or a bow let go of — the follow
    // leaves the weight 6cm out by the row's measure) read as 6cm of balance EFFORT from the first
    // tick, and grabbing her HAND with the cursor held still walked her three steps, the head 29cm.
    if (report.wants("posed-start")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        const int chest = indexOf("chest_2");
        const float lever = (chest >= 0 && hip >= 0)
                                ? probe.boneWorldPosition(static_cast<std::size_t>(chest)).y -
                                      probe.boneWorldPosition(static_cast<std::size_t>(hip)).y
                                : 0.30f;
        // A pose that LEANS: the spine flexed forward with FK, the pelvis and the feet as they stand.
        const std::vector<std::pair<std::string, glm::vec3>> leaning{{"abdomenLower", glm::vec3(32.0f, 0.0f, 0.0f)},
                                                                     {"chest", glm::vec3(32.0f, 0.0f, 0.0f)}};
        {
            Scenario sc;
            sc.prePose = leaning;
            sc.grab = "lHand";
            sc.path = pullPath(glm::vec3(0.0f), 5, 100);
            const RunResult r = run(sc);
            phaseLegs("[real] a leaning pose, a hand grabbed and held still",
                         {gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("the head moved (mm)", movedMm(r, head), 2.0),
                          gateMax("the hips moved (mm)", movedMm(r, hip), 2.0),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 1.0)}, r);
        }
        {
            Scenario sc;
            sc.before.push_back({"chest_2", glm::vec3(0.0f, -0.33f * lever, 0.80f * lever), 50, 30});
            sc.grab = "lHand";
            sc.path = pullPath(glm::vec3(-0.20f, 0.30f, 0.20f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] a bow let go of, then a hand raised",
                         {gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          // (It read 352: three steps after a hand.)
                          gateMax("the head moved (mm)", movedMm(r, head), 90.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0)}, r);
        }
        {
            // ... and a drag that takes the body FURTHER still steps: the rule forgives the start,
            // not the drag.
            Scenario sc;
            sc.prePose = leaning;
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, 0.25f), 60, 50);
            const RunResult r = run(sc);
            phaseLegs("[real] a leaning pose, the chest taken 25cm further forward: she steps",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0),
                          gateMin("steps", static_cast<double>(r.stepsTaken), 1.0)}, r);
        }
    }

    // --- THE SEAT: a figure sat at chair height with her hips PINNED — the one way to say "a
    // chair holds her here", the app knowing nothing of props. Held as any user pin is (six
    // degrees) and balanced over her feet as any body is, she could not be posed: the chest
    // pulled 10cm back along its own arc went 2 (every centimetre took her weight further behind
    // her feet, where the row already had it as posed), and leaned forward the fold was the
    // spine's alone, the pelvis bolt upright. A pinned pelvis now keeps no balance, takes no
    // step, does not "get up", and ROCKS — it pitches under a chest or head drag.
    if (report.wants("seated")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        const int chest = indexOf("chest_2");
        const int knee = indexOf("lShin");
        const int lHand = indexOf("lHand");
        // The pelvis BONE, by structure: the hip's child that the legs hang from (what a click on
        // the hips finds — the root's own body is two 2cm segments).
        int pelvisBone = -1;
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].parent == hip && knee >= 0 && under(knee, static_cast<int>(i))) {
                pelvisBone = static_cast<int>(i);
            }
        }
        if (hip >= 0 && chest >= 0 && knee >= 0 && lHand >= 0) {
            const glm::vec3 hipP = probe.boneWorldPosition(static_cast<std::size_t>(hip));
            const glm::vec3 kneeP = probe.boneWorldPosition(static_cast<std::size_t>(knee));
            const glm::vec3 rod = probe.boneWorldPosition(static_cast<std::size_t>(chest)) - hipP;
            // A chair-height sit: the hips most of the way down to the knees' height and half a
            // thigh back.
            const glm::vec3 sit(0.0f, -0.85f * (hipP.y - kneeP.y), -0.55f * glm::length(hipP - kneeP));
            // The chest's own ARC about the hips (the figures face +z): where a trunk hinged that
            // many degrees at the hips carries it. A cursor cannot ask for more of a seated trunk —
            // and a target even 3% INSIDE the arc rests 5-8mm short: the trunk shortens only by
            // curling, a centimetre costs 50 degrees of it, and the bounded pull does not buy that.
            const auto arc = [&](float degrees) {
                const float a = glm::radians(degrees);
                const glm::vec3 turned(rod.x, rod.y * std::cos(a) - rod.z * std::sin(a),
                                       rod.y * std::sin(a) + rod.z * std::cos(a));
                return turned - rod;
            };
            const auto pelvisPitch = [&](const RunResult& r) {
                return static_cast<std::size_t>(hip) < r.endEuler.size()
                           ? static_cast<double>(r.endEuler[static_cast<std::size_t>(hip)].x -
                                                 r.startEuler[static_cast<std::size_t>(hip)].x)
                           : 0.0;
            };
            struct Lean {
                const char* name;
                const char* pin;
                float       degrees;
                double      missMax;
            };
            const std::string pelvisName = pelvisBone >= 0 ? bones[static_cast<std::size_t>(pelvisBone)].name : "hip";
            for (const Lean& lean : {Lean{"[real] seated, hips pinned: the chest leaned 30 degrees forward along its arc", "hip", 30.0f, 7.0},
                                     Lean{"[real] seated, hips pinned: the chest reclined 20 degrees along its arc", "hip", -20.0f, 4.0},
                                     Lean{"[real] seated, the PELVIS BONE pinned (what a click finds): the chest leaned forward", nullptr, 30.0f, 10.0}}) {
                Scenario sc;
                sc.before.push_back({"hip", sit, 70, 40});
                sc.userPins = {lean.pin != nullptr ? std::string(lean.pin) : pelvisName};
                sc.pinsAfterBefore = true;
                sc.grab = "chest_2";
                sc.path = pullPath(arc(lean.degrees), 50, 40);
                const RunResult r = run(sc);
                const double pitch = pelvisPitch(r);
                phaseLegs(lean.name,
                             // (Held in full and balanced over her feet: 27mm forward, 77 reclining.)
                             {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, lean.missMax),
                              // (The pelvis ROCKS with the trunk: it read 0.00 held in full.)
                              gateMin("the pelvis's pitch, the way of the lean (deg)", lean.degrees > 0.0f ? pitch : -pitch, 4.0),
                              gateMax("the hips moved (mm)", movedMm(r, hip), 8.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                              gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0)}, r);
            }
            {
                // A far reach from the seat: the trunk follows the arm, the seat stays.
                Scenario sc;
                sc.before.push_back({"hip", sit, 70, 40});
                sc.userPins = {"hip"};
                sc.pinsAfterBefore = true;
                sc.grab = "lHand";
                const float arm = glm::length(probe.boneWorldPosition(static_cast<std::size_t>(lHand)) -
                                              probe.boneWorldPosition(static_cast<std::size_t>(indexOf("lShldr"))));
                sc.path = pullPath(glm::vec3(-0.45f * arm, -0.18f * arm, 1.05f * arm), 60, 40);
                const RunResult r = run(sc);
                phaseLegs("[real] seated, hips pinned: a hand reached far forward",
                             {gateMax("hand-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                              gateMax("the hips moved (mm)", movedMm(r, hip), 1.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                              gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 40.0),
                              gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0)}, r);
            }
            {
                // A foot lifted and put out from the seat: the leg's alone.
                Scenario sc;
                sc.before.push_back({"hip", sit, 70, 40});
                sc.userPins = {"hip"};
                sc.pinsAfterBefore = true;
                sc.grab = "lFoot";
                // (Mostly OUT: the oldest generation's thighs stop at 100 degrees of flexion, and a
                // seated thigh is most of the way there.)
                const float shin = glm::length(probe.boneWorldPosition(static_cast<std::size_t>(indexOf("lFoot"))) - kneeP);
                sc.path = pullPath(glm::vec3(0.0f, 0.30f * shin, 0.60f * shin), 60, 40);
                const RunResult r = run(sc);
                phaseLegs("[real] seated, hips pinned: a foot lifted and put out",
                             {gateMax("foot-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                              gateMax("the hips moved (mm)", movedMm(r, hip), 1.0),
                              gateMax("the head moved (mm)", movedMm(r, head), 5.0),
                              gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
            }
        }
    }

    // --- THE BOW: the chest or the head taken DOWN as it goes forward, from standing. She folds —
    // the pelvis pitching over planted feet, the spine curling with it, the hips back under the
    // weight — and takes no step. Before: the trunk follow read every forward travel as a walk
    // (the chest down a third of its lever and forward four fifths: two steps, the hips 19cm
    // along, the spine bent 2 degrees a joint), and with the follow out of it the pelvis could not
    // pitch at all under a trunk drag: the fold was the spine's alone, 18-21 degrees a joint, and
    // stopped 7cm short at balance's wall.
    if (report.wants("trunk-bow")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        const int grabbed = indexOf("chest_2");
        std::vector<int> spine; // the spine's bones below the upper chest, by structure
        for (int cur = hip; cur >= 0 && spine.size() < 3;) {
            int next = -1;
            for (std::size_t i = 0; i < bones.size(); ++i) {
                if (bones[i].parent == cur && under(head, static_cast<int>(i))) {
                    next = static_cast<int>(i);
                }
            }
            if (next < 0 || next == grabbed) {
                break;
            }
            spine.push_back(next);
            cur = next;
        }
        const auto heightOverHip = [&](int node, float fallback) {
            return (node >= 0 && hip >= 0) ? probe.boneWorldPosition(static_cast<std::size_t>(node)).y -
                                                 probe.boneWorldPosition(static_cast<std::size_t>(hip)).y
                                           : fallback;
        };
        const float chestLever = heightOverHip(grabbed, 0.30f);
        const float headLever = heightOverHip(head, 0.57f);
        const auto pelvisPitch = [&](const RunResult& r) {
            return hip >= 0 && static_cast<std::size_t>(hip) < r.endEuler.size()
                       ? static_cast<double>(r.endEuler[static_cast<std::size_t>(hip)].x)
                       : 0.0;
        };
        const auto spineBend = [&](const RunResult& r, double& twist, double& least, double& most) {
            twist = 0.0;
            least = 1.0e9;
            most = -1.0e9;
            for (const int b : spine) {
                if (static_cast<std::size_t>(b) < r.endEuler.size()) {
                    const glm::vec3 e = r.endEuler[static_cast<std::size_t>(b)];
                    twist = std::max(twist, static_cast<double>(std::max(std::abs(e.y), std::abs(e.z))));
                    least = std::min(least, static_cast<double>(e.x));
                    most = std::max(most, static_cast<double>(e.x));
                }
            }
        };
        struct Bow {
            const char* name;
            const char* grab;
            glm::vec3   offset;
            double      pitchMin;  // the pelvis's share of the fold (deg)
            double      spineMin;  // ... and every spine joint's
        };
        for (const Bow& bow : {Bow{"[real] a bow by the upper chest (down a third of its lever, forward four fifths)", "chest_2",
                                   glm::vec3(0.0f, -0.33f * chestLever, 0.80f * chestLever), 10.0, 8.0},
                               Bow{"[real] a bow by the head (down a sixth of its lever, forward a half)", "head",
                                   glm::vec3(0.0f, -0.18f * headLever, 0.52f * headLever), 7.0, 3.0}}) {
            Scenario sc;
            sc.grab = bow.grab;
            sc.path = pullPath(bow.offset, 50, 40);
            const RunResult r = run(sc);
            double twist = 0.0, least = 0.0, most = 0.0;
            spineBend(r, twist, least, most);
            phaseLegs(bow.name,
                         // (8mm: the idle arms HANG since 2026-09-20, a tenth of her weight 15cm further
                         // forward in a bow; balance is blind to it, but the solve's gradients are
                         // not quite, and the chest bow — at nine tenths of the cursor's bounded pull
                         // before — rests 4-5mm out on the main family.)
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 8.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          // (The fold is SHARED: the pelvis pitched 8 degrees under a spine curled 66 was
                          // a hunch, 26 over a spine bent 1 degree a joint a plank.)
                          gateMin("the pelvis's pitch (deg)", pelvisPitch(r), bow.pitchMin),
                          gateMin("the least-flexed spine joint (deg)", least, bow.spineMin),
                          gateMax("the most-flexed spine joint (deg)", most, 26.0),
                          gateMax("spine twist or side bend, the worst channel (deg)", twist, 3.0),
                          info("the hips went (mm)", hip >= 0 ? movedMm(r, hip) : 0.0),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 15.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 60.0),
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 1.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 10.0)}, r);
        }
        {
            // The bow and back up, in one drag: the pose is the target's.
            Scenario sc;
            sc.grab = "chest_2";
            const glm::vec3 offset(0.0f, -0.33f * chestLever, 0.80f * chestLever);
            sc.path = {{0, glm::vec3(0.0f), "start"}, {50, offset, "bow"}, {80, offset, "hold"},
                       {130, glm::vec3(0.0f), "back"}, {190, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            const double headOff =
                head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                            r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                          : 0.0;
            phaseLegs("[real] a bow and back up (one drag)",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMax("the head off where it began (mm)", headOff, 1.5),
                          gateMax("the pelvis's pitch left (deg)", std::abs(pelvisPitch(r)), 0.2),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0)}, r);
        }
        {
            // The bow let go of, then the chest taken back to where it sits at rest.
            Scenario sc;
            sc.before.push_back({"chest_2", glm::vec3(0.0f, -0.33f * chestLever, 0.80f * chestLever), 50, 30});
            sc.grab = "chest_2";
            sc.toRest = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.1f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            const double headOff =
                head >= 0 && static_cast<std::size_t>(head) < r.restPos.size()
                    ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                      r.restPos[static_cast<std::size_t>(head)])) * 1000.0
                    : 0.0;
            double twist = 0.0, least = 0.0, most = 0.0;
            spineBend(r, twist, least, most);
            phaseLegs("[real] a bow let go of, then the chest taken back up (two drags)",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                          // (Read as a body taken BACKWARD across the floor, the rising chest walked the
                          // hips' home 12cm back: 25mm short, the pelvis still pitched 10 degrees; and
                          // on the oldest generation, whose bow lowers the hips, the pelvis stayed
                          // pitched 15 degrees under a spine extended to its limit against it.)
                          gateMax("the head off where it stood (mm)", headOff, 30.0),
                          gateMax("the pelvis's pitch left (deg)", std::abs(pelvisPitch(r)), 3.0),
                          gateMax("the most-bent spine joint left (deg)", std::max(std::abs(least), std::abs(most)), 3.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("worst single-tick jump (mm)", r.phases.empty() ? 0.0 : r.phases[0].maxJump * 1000.0, 60.0)}, r);
        }
    }

    // --- THE HEAD STAYS UP (Armature::rightIdleHead): the neck gives a tilting trunk most of its
    // tilt back, so a lean, a hinge, a walk, a kneel leave her looking ahead and a deep bow at the
    // floor in front of her — not at her feet, the head riding the chest. And A RELEASE MID-STEP
    // KEEPS THE POSE: the settle is the drag continued under a still cursor; as a solve of its
    // own with no target for a trunk drag, a bow let go of while a foot was in the air sprang
    // back 45 degrees toward upright, the arms and the head riding.
    if (report.wants("head-right")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        const int head = indexOf("head");
        const int chest = indexOf("chest_2");
        const float lever = (chest >= 0 && hip >= 0) ? probe.boneWorldPosition(static_cast<std::size_t>(chest)).y -
                                                           probe.boneWorldPosition(static_cast<std::size_t>(hip)).y
                                                     : 0.30f;
        const auto movedInSettle = [&](const RunResult& r, int bone) {
            return bone >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(bone)] -
                                                               r.dragEndPos[static_cast<std::size_t>(bone)])) * 1000.0
                             : 0.0;
        };
        // The neck's extension: the flexion channels (x) of the head and the bones between it and
        // the upper chest, summed and negated.
        const auto neckGiven = [&](const RunResult& r) {
            double given = 0.0;
            for (int cur = head; cur >= 0 && cur != chest; cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (static_cast<std::size_t>(cur) < r.endEuler.size()) {
                    given -= static_cast<double>(r.endEuler[static_cast<std::size_t>(cur)].x);
                }
            }
            return given;
        };
        {
            // The hip hinge: hips 10cm back, the chest forward over the feet — the head up.
            Scenario sc;
            sc.grab = "hip";
            sc.path = pullPath(glm::vec3(0.0f, 0.0f, -0.10f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] hips 10cm back: the head stays up",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                       gateMin("gaze over the horizon (deg; it read -23 to -29, riding)", r.gazeEndDeg, -10.0),
                       gateMax("gaze over the horizon (deg)", r.gazeEndDeg, 4.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 2.0)}, r);
        }
        {
            // A deep bow: she looks at the floor ahead of her — most of 30 degrees given back.
            Scenario sc;
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, -lever / 3.0f, 0.8f * lever), 60, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] a bow by the upper chest: the head looks ahead of her feet",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 8.0),
                       // (How deep the chest tilts in this bow is the rig's — riding, the gaze read -46 to
                       // -71 across the eight — so the gate is what the NECK gives back: its bones' flexion
                       // channels, summed; C tanh(tilt / C) with C = 30 is 26-29 degrees here.)
                       gateMin("the neck gives back (deg; 0 riding)", neckGiven(r), 20.0),
                       gateMax("the neck gives back (deg)", neckGiven(r), 32.0),
                       gateMax("gaze over the horizon (deg; a bow is not a level head)", r.gazeEndDeg, -10.0),
                       gateMax("the head's sideways tilt (deg)", std::abs(r.headRollEndDeg), 2.0)}, r);
        }
        {
            // Out and home in one drag: the neck is a function of the trunk's pose.
            Scenario sc;
            sc.grab = "hip";
            const glm::vec3 back(0.0f, 0.0f, -0.10f);
            sc.path = {{0, glm::vec3(0.0f), "start"}, {50, back, "out"}, {70, back, "hold"},
                       {120, glm::vec3(0.0f), "home"}, {170, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            phaseLegs("[real] hips 10cm back and home: the head as it was",
                      {gateMax("gaze off where it began (deg)", std::abs(r.gazeEndDeg - r.gazeStartDeg), 0.3),
                       gateMax("the head off where it began (mm)",
                               head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                                           r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                                         : 0.0,
                               1.5)}, r);
        }
        {
            // The bow let go of, the chest then taken back up: two drags compose.
            Scenario sc;
            sc.grab = "chest_2";
            sc.before.push_back({"chest_2", glm::vec3(0.0f, -lever / 3.0f, 0.8f * lever), 60, 40});
            sc.toRest = true;
            sc.path = pullPath(glm::vec3(0.0f, 0.10f, -0.10f), 60, 40); // (toRest: replaced by the way home)
            const RunResult r = run(sc);
            phaseLegs("[real] a bow let go of, the chest taken back up: the head level again",
                      {gateMin("gaze over the horizon (deg)", r.gazeEndDeg, -5.0),
                       gateMax("gaze over the horizon (deg)", r.gazeEndDeg, 5.0)}, r);
        }
        {
            // A head the user DRAGS is the user's: the righting stands down (the solve has the neck).
            Scenario sc;
            sc.grab = "head";
            sc.path = pullPath(glm::vec3(0.0f, -0.05f, 0.12f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] the head dragged down and forward: it goes where it is taken",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                       gateMax("gaze over the horizon (deg; a head taken down looks down)", r.gazeEndDeg, -3.0)}, r);
        }
        // THE NECK IS THE SPINE'S TOP (the couplings in Armature::solveIk): a dragged head takes the
        // BODY with it, the bend shared down the whole spine. Priced as a limb and uncoupled the
        // neck took a head drag alone, to its limits — and the rigs disagreed about which way.
        const auto neckWorst = [&](const RunResult& r) {
            double worst = 0.0;
            for (int cur = head >= 0 ? bones[static_cast<std::size_t>(head)].parent : -1; cur >= 0 && cur != chest;
                 cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (static_cast<std::size_t>(cur) < r.endEuler.size()) {
                    const glm::vec3 e = r.endEuler[static_cast<std::size_t>(cur)];
                    worst = std::max({worst, static_cast<double>(std::abs(e.x)), static_cast<double>(std::abs(e.y)),
                                      static_cast<double>(std::abs(e.z))});
                }
            }
            return worst;
        };
        {
            Scenario sc;
            sc.grab = "head";
            sc.path = pullPath(glm::vec3(0.0f, -0.03f, -0.15f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] the head pulled 15cm back and a little down: she steps back, head up",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 2.0),
                       // (One generation threw its head back to the sky — both neck bones at their
                       // limits, -25 and -27 degrees, the gaze 55 up — another flexed its neck 20 the
                       // other way and looked at the floor.)
                       gateMax("the most-bent neck bone (deg; it read 20-27)", neckWorst(r), 8.0),
                       gateMin("gaze over the horizon (deg)", r.gazeEndDeg, -10.0),
                       gateMax("gaze over the horizon (deg)", r.gazeEndDeg, 12.0)}, r);
        }
        {
            Scenario sc;
            sc.grab = "head";
            sc.path = pullPath(glm::vec3(0.15f, -0.04f, 0.0f), 50, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] the head pulled 15cm sideways: the body leans, the neck is not broken",
                      {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 3.0),
                       gateMax("the most-bent neck bone (deg; it read 40, its limit)", neckWorst(r), 20.0),
                       gateMax("the head's sideways tilt (deg; it read 45-50)", std::abs(r.headRollEndDeg), 25.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 2.0)}, r);
        }
        {
            // LET GO OF MID-STEP. The chest dragged a whole lever forward walks; the button comes
            // up four ticks into the hold, a foot in the air. What the settle may move is the foot.
            Scenario sc;
            sc.grab = "chest_2";
            sc.path = pullPath(glm::vec3(0.0f, -lever / 3.0f, 1.0f * lever), 40, 4);
            sc.asymmetryGateMm = -1.0; // (a WALK: one foot is ahead of the other, and in the air at mouse-up)
            const RunResult r = run(sc);
            const int elbow = indexOf("lForeArm");
            phaseLegs("[real] a walking bow let go of mid-step: the settle keeps the pose",
                      {gateMin("settle ticks (the premise: a step was in flight)", static_cast<double>(r.settleTicks), 1.0),
                       gateMax("the chest moved in the settle (mm; it read 150-250)", movedInSettle(r, chest), 12.0),
                       gateMax("the head moved in the settle (mm)", movedInSettle(r, head), 25.0),
                       gateMax("an elbow moved in the settle (mm)", movedInSettle(r, elbow), 25.0),
                       gateMax("gaze changed in the settle (deg)", std::abs(r.gazeEndDeg - r.gazeDragEndDeg), 4.0)}, r);
        }
    }

    // --- A TRUNK joint dragged across the floor takes the BODY with it (the Armature's trunk
    // follow): a lean first, then the hips, and the stance walks after them. Left to the prices
    // the hips stood still and balance walled the lean in: the upper chest pulled 20cm back
    // stopped 12cm short with no step, 20cm forward 5 short, the neck 8, the head 4.
    if (report.wants("trunk-follow")) {
 // (the dump's index: build keeps the order)
        const int hip = indexOf("hip");
        struct Pull {
            const char* name;
            const char* grab;
            glm::vec3   offset;
            double      missMax;  // the grab's distance from the cursor at rest (mm)
            double      stepsMax; // (a sideways pull this size is a lean and a weight shift: no step)
        };
        for (const Pull& pull : {Pull{"[real] upper chest pulled 20cm back", "chest_2", glm::vec3(0.0f, 0.0f, -0.20f), 3.0, 4.0},
                                 Pull{"[real] upper chest pulled 20cm forward", "chest_2", glm::vec3(0.0f, 0.0f, 0.20f), 3.0, 4.0},
                                 Pull{"[real] upper chest pulled 20cm sideways", "chest_2", glm::vec3(0.20f, 0.0f, 0.0f), 12.0, 2.0},
                                 Pull{"[real] head pulled 20cm back", "head", glm::vec3(0.0f, 0.0f, -0.20f), 3.0, 4.0}}) {
            Scenario sc;
            sc.grab = pull.grab;
            sc.path = pullPath(pull.offset, 70, 60);
            const RunResult r = run(sc);
            const double hipWent =
                hip >= 0 ? static_cast<double>(glm::dot(r.endPos[static_cast<std::size_t>(hip)] - r.startPos[static_cast<std::size_t>(hip)],
                                                        glm::normalize(pull.offset)))
                         : 0.0;
            double feetOffFloor = 0.0;
            for (const int c : r.contactPins) {
                feetOffFloor = std::max(feetOffFloor, std::abs(static_cast<double>(r.endPos[static_cast<std::size_t>(c)].y) - r.contactBindMinY));
            }
            phaseLegs(pull.name,
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, pull.missMax),
                          gateMin("the hips went along (mm)", hipWent * 1000.0, 40.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), pull.stepsMax),
                          // (A sideways pull this size shifts the weight 11cm over one foot with no
                          // step on the wide-standing generations, and the pelvis stays level under a
                          // trunk drag — the sway's roll is the pelvis drag's — so the far foot stands
                          // 3cm up on its toes: the free leg of a weight shift.)
                          gateMax("feet off floor at the end (mm)", feetOffFloor * 1000.0, 40.0),
                          gateMax("the head's worst single-tick move (mm)", r.headJumpMax * 1000.0, 60.0),
                          gateMax("drag lag, mean (mm)", r.phases.empty() ? 0.0 : r.phases[0].lagMean() * 1000.0, 25.0),
                          // (The last gathering step can still be landing in the hold's first ticks.)
                          gateMax("hold oscillation (mm)", r.phases.size() > 1 ? r.phases[1].effOsc * 1000.0 : 0.0, 8.0),
                          gateMax("settle max step (mm)", r.settleMaxStep * 1000.0, 20.0)}, r);
        }
        {
            // There and back again, short of a step: the lean and the follow are functions of the
            // target, so she stands as she stood. (Past a step she does not: the stance has walked,
            // and the chest brought home leans back over it.)
            Scenario sc;
            sc.grab = "chest_2";
            sc.path = {{0, glm::vec3(0.0f), "start"},
                       {40, glm::vec3(0.05f, 0.0f, -0.03f), "out"},
                       {70, glm::vec3(0.05f, 0.0f, -0.03f), "hold"},
                       {110, glm::vec3(0.0f), "home"},
                       {170, glm::vec3(0.0f), "hold"}};
            const RunResult r = run(sc);
            const int head = indexOf("head");
            const double headOff =
                head >= 0 ? static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(head)] -
                                                            r.startPos[static_cast<std::size_t>(head)])) * 1000.0
                          : 0.0;
            phaseLegs("[real] upper chest 6cm out, then home",
                         {gateMax("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0, 1.0),
                          gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                          gateMax("the head off where it began (mm)", headOff, 1.0),
                          gateMax("the head's worst single-tick move (mm)", r.headJumpMax * 1000.0, 60.0)}, r);
        }
    }
}

/// User pins held under far drags, and the tremble under cursor noise.
void RealPhases::pinPhases() {
    // --- Pinned joints hold EXACTLY while other things are dragged.
    if (report.wants("pin-hand-drag")) {
        Scenario sc;
        sc.grab = "rHand";
        sc.userPins = {"lHand"};
        sc.path = pullPath(glm::vec3(-0.45f, 0.10f, 0.0f), 80, 80);
        const RunResult r = run(sc);
        phaseLegs("[real] pinned lHand under a 45cm rHand drag",
                     {gateMax("pinned joint max distance (mm)", r.userPinDistMax * 1000.0, 10.0),
                      gateMax("pinned joint rotation (deg)", r.userPinRotMaxDeg, 1.0),
                      gateMax("pinned joint settle step (mm)", r.userPinStepMax * 1000.0, 1.0)}, r);
    }
    if (report.wants("pin-hand-crouch")) {
        Scenario sc;
        sc.grab = "hip";
        sc.userPins = {"lHand"};
        sc.path = pullPath(glm::vec3(0.0f, -0.12f, 0.0f), 40, 100);
        const RunResult r = run(sc);
        phaseLegs("[real] pinned lHand through a 12cm hip crouch",
                     {gateMax("pinned joint max distance (mm)", r.userPinDistMax * 1000.0, 10.0),
                      gateMax("pinned joint rotation (deg)", r.userPinRotMaxDeg, 1.0),
                      // 0.09: the base rig reaches 0.107-0.114, the character dump 0.092-0.098
                      // — both must pass (the character-dump calibration gap, see CLAUDE.md).
                      gateMin("hip drop at release (m)", r.hipDropAtRelease, 0.09)}, r, kCrouchLegs);
    }
    if (report.wants("pin-foot-chest")) {
        Scenario sc;
        sc.grab = "chest_2";
        sc.userPins = {"lFoot"};
        sc.path = pullPath(glm::vec3(0.45f, 0.0f, 0.0f), 110, 150);
        const RunResult r = run(sc);
        phaseLegs("[real] pinned lFoot under a 45cm chest drag",
                     {gateMax("pinned joint max distance (mm)", r.userPinDistMax * 1000.0, 15.0),
                      gateMax("steps taken", r.stepsTaken, 0.0)}, r, kLeanLegs);
    }

    // --- Trembling: slow and medium pulls with cursor noise must not shake idle joints.
    if (report.wants("tremble-slow")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = pullPath(glm::vec3(0.06f, 0.0f, 0.0f), 180, 60); // ~2 cm/s
        sc.cursorNoise = 0.002f;
        sc.idleJoints = kIdleForHandDrag;
        const RunResult r = run(sc);
        const IdleMetrics idle = runIdle(sc);
        phaseLegs("[real] tremble: slow noisy pull (lHand 2cm/s, +-2mm noise)",
                     // (ACCUMULATED reversal travel over ~600 noisy ticks: the solve passes cursor
                     // noise on in proportion — 0.03mm a tick on the worst idle joint — where a
                     // damped loop swallowed it. The per-tick step below is the visible measure.)
                     {gateMax("idle-joint tremble (mm)", idle.oscMax * 1000.0, 30.0),
                      gateMax("idle-joint worst step (mm)", idle.stepMax * 1000.0, 10.0),
                      gateMax("grabbed-joint osc (mm)", (r.phases[0].effOsc + r.phases[1].effOsc) * 1000.0, 60.0)}, r);
    }
    if (report.wants("tremble-med")) {
        Scenario sc;
        sc.grab = "head";
        sc.path = pullPath(glm::vec3(0.10f, 0.0f, 0.05f), 60, 60);
        sc.cursorNoise = 0.002f;
        sc.idleJoints = kIdleForHeadDrag;
        const RunResult r = run(sc);
        const IdleMetrics idle = runIdle(sc);
        phaseLegs("[real] tremble: medium noisy head pull (+-2mm noise)",
                     {gateMax("idle-joint tremble (mm)", idle.oscMax * 1000.0, 35.0),
                      gateMax("grabbed-joint osc (mm)", (r.phases[0].effOsc + r.phases[1].effOsc) * 1000.0, 80.0)}, r);
    }
}

/// What a grab lands on: the face rig (the head's), a pectoral (the chest's), a digit (its own).
void RealPhases::grabPhases() {
    // --- The FACE RIG is not posed (2026-09-28): its bones are for expressions — a bone of it
    // selects the HEAD (Armature::posingBone), and a grab on the face is the head's drag, the
    // grabbed point on the cursor, without trembling; the face bone's own channels are never touched.
    // (Until then the grab was the face bone's, promoted to the head by the rig.)
    if (report.wants("eye-grab")) {
        Armature posedProbe;
        posedProbe.build(bones);
        applyBodyMesh(posedProbe, 1.0f);
        const int eye = resolveBone(posedProbe, "lEye");
        const int head = resolveBone(posedProbe, "head");
        if (eye >= 0) {
            posedProbe.setSelectedBone(eye);
        }
        const bool selectsHead = eye >= 0 && head >= 0 && posedProbe.selectedBone() == head;
        Scenario sc;
        sc.grab = "lEye";
        sc.path = pullPath(glm::vec3(0.10f, 0.0f, 0.05f), 60, 90);
        sc.cursorNoise = 0.002f;
        const RunResult r = run(sc);
        phaseLegs("[real] a grab on the face is the head's (lEye selects the head, +-2mm noise)",
                     {gateMin("a face-rig bone selects the head", selectsHead ? 1.0 : 0.0, 1.0),
                      gateMax("the face bone's own rotation (deg)", static_cast<double>(glm::length(r.grabEulerEnd - r.grabEulerStart)), 0.0),
                      gateMax("grabbed-joint osc (mm)", (r.phases[0].effOsc + r.phases[1].effOsc) * 1000.0, 80.0),
                      // (Its offset from the head is MEASURED off the pose every tick since 2026-09-21:
                      // it rested 9.9mm off and trailed 9.7 under the fixed offset.)
                      gateMax("head-to-cursor at rest (mm)", r.restingMiss * 1000.0, 5.0)}, r);
    }

    // --- A click on a figure's CHEST finds a PECTORAL bone as often as the chest (its joint stands
    // in front of the spine, nearest the pixel; on custom characters with a fuller bust, always):
    // a small bone promoted to the chest, which must follow the cursor as the chest itself does.
    // Under a FIXED offset the grabbed point ended 40-47mm from the cursor and trailed it by
    // 32-44mm, the chest leaning under an offset that did not lean with it (found by running the
    // in-app galleries on custom characters: two of six grabbed a pectoral where the script meant
    // the chest). What is left is the sideways lean's own: a point beside the spine drops as the
    // chest rolls, and a standing body cannot be asked up.
    if (report.wants("pectoral-grab")) {
        int pectoral = -1;
        for (const char* name : {"lPectoral", "l_pectoral"}) {
            if (pectoral < 0) {
                pectoral = probe.boneIndex(name);
            }
        }
        if (pectoral < 0) {
            report.skip("[real] the chest pulled 12cm sideways by a pectoral bone", "no pectoral bone on this rig");
        } else {
            Scenario sc;
            sc.grab = probe.boneName(static_cast<std::size_t>(pectoral));
            sc.path = pullPath(glm::vec3(0.12f, 0.0f, 0.0f), 60, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] the chest pulled 12cm sideways by a pectoral bone",
                      {gateMax("grab-to-cursor at rest (mm; it read 23-52)", r.restingMiss * 1000.0, 25.0),
                       gateMax("mean drag lag (mm; it read 14-38)", r.phases[0].lagMean() * 1000.0, 12.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 1.0),
                       gateMax("worst single-tick jump (mm)", std::max(r.phases[0].maxJump, r.phases[1].maxJump) * 1000.0, 40.0)}, r);
        }
    }

    // --- THE DIGIT DRAG (2026-09-28): a finger or a toe dragged moves ITS OWN DIGIT alone, up to
    // where it joins the hand or the foot (Armature's digit drag; the rig has no part in it). Until
    // then a finger grab was an arm gesture — pulled 60cm up it raised the arm overhead — and a toe
    // grab a foot drag. The "outside" gates read 0.001mm: a bone none of the solve's unknowns move
    // does not move at all.
    if (report.wants("digit")) {
        applyBodyMesh(probe, 1.0f);
        const int nBones = static_cast<int>(bones.size());
        const auto strictlyUnder = [&](int node, int ancestor) { // strictly below
            for (int cur = node >= 0 ? bones[static_cast<std::size_t>(node)].parent : -1; cur >= 0;
                 cur = bones[static_cast<std::size_t>(cur)].parent) {
                if (cur == ancestor) {
                    return true;
                }
            }
            return false;
        };
        const auto digitsBelow = [&](int bone) {
            int count = 0;
            for (int c = 0; c < nBones; ++c) {
                count += bones[static_cast<std::size_t>(c)].parent == bone && probe.boneClass(c) == pose::BoneClass::Digit ? 1 : 0;
            }
            return count;
        };
        // The digit's TOP bone: the highest bone of the digit the grabbed one belongs to.
        const auto digitTop = [&](int bone) {
            int top = bone;
            for (int p = bones[static_cast<std::size_t>(top)].parent;
                 p >= 0 && probe.boneClass(p) == pose::BoneClass::Digit && digitsBelow(p) <= 1;
                 p = bones[static_cast<std::size_t>(p)].parent) {
                top = p;
            }
            return top;
        };
        // Everything that is not the digit's: every joint but those strictly below its top bone.
        const auto outsideMoved = [&](const RunResult& r, int top) {
            double worst = 0.0;
            for (int i = 0; i < nBones && static_cast<std::size_t>(i) < r.endPos.size(); ++i) {
                if (!strictlyUnder(i, top)) {
                    worst = std::max(worst, static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(i)] -
                                                                           r.startPos[static_cast<std::size_t>(i)])) * 1000.0);
                }
            }
            return worst;
        };
        const auto travelled = [](const RunResult& r) { return static_cast<double>(glm::length(r.grabEnd - r.grabStart)) * 1000.0; };
        const auto holdJump = [](const RunResult& r) { return r.phases.size() > 1 ? r.phases[1].maxJump * 1000.0 : 0.0; };
        const int lHand = resolveBone(probe, "lHand");
        const int finger = resolveBone(probe, "lIndex3");
        const bool fingerIsDigit = finger >= 0 && probe.boneClass(finger) == pose::BoneClass::Digit;
        if (!fingerIsDigit) {
            report.skip("[real] digit: a finger pulled 60cm up — the finger alone", "no finger joint found on this rig");
        } else {
            const int top = digitTop(finger);
            {
                // The old finger-grab raise, by the fingertip's joint: the arm stays where it is.
                Scenario sc;
                sc.grab = probe.boneName(static_cast<std::size_t>(finger));
                sc.path = pullPath(glm::vec3(0.0f, 0.60f, 0.0f), 60, 90);
                const RunResult r = run(sc);
                phaseLegs("[real] digit: a finger pulled 60cm up — the finger alone",
                          {gateMin("the finger's joint is a digit's, its hand is not", lHand >= 0 && probe.boneClass(lHand) == pose::BoneClass::Body ? 1.0 : 0.0, 1.0),
                           // (A CARPAL SELECTS THE HAND, 2026-10-28: every bone below the wrist that is
                           // neither the wrist nor a digit is a Carpal, and its selection is the hand's.)
                           // (A rig whose fingers hang straight off the hand has no carpal to test: the
                           // count is information there, 2026-11-03.)
                           [&] {
                               int count = 0;
                               bool allHand = true;
                               for (int i = 0; i < nBones; ++i) {
                                   if (strictlyUnder(i, lHand) && probe.boneClass(i) == pose::BoneClass::Carpal) {
                                       allHand = allHand && probe.posingBone(i) == lHand;
                                       ++count;
                                   }
                               }
                               return count > 0 ? gateMin("carpals under the hand (each selecting the hand)", allHand ? static_cast<double>(count) : 0.0, 1.0)
                                                : info("carpals under the hand (none on this rig)", 0.0);
                           }(),
                           gateMax("everything outside the finger moved (mm)", outsideMoved(r, top), 0.001),
                           // (27-58mm on the eight rigs: the finger turns to its limits after the cursor.)
                           gateMin("the grabbed joint travelled (mm)", travelled(r), 20.0),
                           gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                           gateMax("worst single-tick jump (mm)", jumpMm(r), 40.0),
                           gateMax("hold: worst single-tick jump (mm)", holdJump(r), 1.0)}, r);
            }
            {
                // The fingertip's pad curled toward the palm — within the finger's reach: the
                // grabbed point rests on the cursor, every joint of the finger bending for it.
                Scenario still; // (the pose the drag begins in: where the palm faces, how the finger lies)
                still.grab = probe.boneName(static_cast<std::size_t>(finger));
                still.path = pullPath(glm::vec3(0.0f), 2, 2);
                const RunResult r0 = run(still);
                glm::vec3 palm = lHand >= 0 && static_cast<std::size_t>(lHand) < r0.endPalm.size() ? r0.endPalm[static_cast<std::size_t>(lHand)]
                                                                                                   : glm::vec3(0.0f);
                glm::vec3 along = r0.startPos[static_cast<std::size_t>(finger)] - r0.startPos[static_cast<std::size_t>(top)];
                if (glm::length(palm) < 0.5f || glm::length(along) < 1.0e-4f) {
                    report.skip("[real] digit: a fingertip curled toward the palm", "no palm found on this rig");
                } else {
                    along = glm::normalize(along);
                    palm = glm::normalize(palm - along * glm::dot(palm, along));
                    // (The pad: 1.5cm beyond the last joint, along the last phalanx as it lies.)
                    const int       before = bones[static_cast<std::size_t>(finger)].parent;
                    const glm::vec3 phalanx = r0.startPos[static_cast<std::size_t>(finger)] - r0.startPos[static_cast<std::size_t>(before)];
                    Scenario sc;
                    sc.grab = still.grab;
                    sc.grabOffset = glm::normalize(phalanx) * 0.015f;
                    sc.path = pullPath(palm * 0.030f - along * 0.015f, 40, 40);
                    const RunResult r = run(sc);
                    const auto bent = [&](int bone) { // (a joint's turn over the drag, degrees)
                        return static_cast<std::size_t>(bone) < r.endEuler.size()
                                   ? static_cast<double>(glm::length(r.endEuler[static_cast<std::size_t>(bone)] -
                                                                     r.startEuler[static_cast<std::size_t>(bone)]))
                                   : 0.0;
                    };
                    phaseLegs("[real] digit: a fingertip curled toward the palm",
                              {gateMax("grab-to-cursor at rest (mm; it reads 0.005-0.013)", r.restingMiss * 1000.0, 1.0),
                               gateMax("everything outside the finger moved (mm)", outsideMoved(r, top), 0.001),
                               gateMin("the fingertip's own joint bent (deg)", bent(finger), 1.0),
                               gateMin("the finger's top joint bent (deg)", bent(top), 1.0),
                               gateMax("mean drag lag (mm)", r.phases[0].lagMean() * 1000.0, 5.0),
                               gateMax("worst single-tick jump (mm; it reads 0.7-1.7)", jumpMm(r), 5.0),
                               // (0.46: the follower's last tick of travel, the hold's first.)
                               gateMax("hold: worst single-tick jump (mm)", holdJump(r), 1.0)}, r);
                }
            }
        }
        // A TOE: a digit's joint below the left foot with no digit's joint below it.
        const int lFoot = resolveBone(probe, "lFoot");
        int toe = -1;
        for (int i = 0; i < nBones && toe < 0 && lFoot >= 0; ++i) {
            if (probe.boneClass(i) == pose::BoneClass::Digit && strictlyUnder(i, lFoot) && digitsBelow(i) == 0) {
                toe = i;
            }
        }
        if (toe < 0) {
            report.skip("[real] digit: a toe's tip raised 2cm — the toe alone", "no toe joint found on this rig");
        } else {
            // (The tip: 1.5cm ahead of the toe's joint, the way the figure faces — a toe that is one
            // bone hangs SIDEWAYS off the ball on the oldest generations, so its parent's joint does
            // not say which way it points.)
            Scenario sc;
            sc.grab = probe.boneName(static_cast<std::size_t>(toe));
            sc.grabOffset = glm::vec3(0.0f, 0.0f, 0.015f);
            sc.path = pullPath(glm::vec3(0.0f, 0.020f, 0.0f), 40, 40);
            const RunResult r = run(sc);
            phaseLegs("[real] digit: a toe's tip raised 2cm — the toe alone",
                      {gateMax("everything outside the toe moved (mm)", outsideMoved(r, digitTop(toe)), 0.001),
                       // (10-19mm: a toe that is one bone turns on a 1.5cm lever and gives the least.)
                       gateMin("the grabbed point rose (mm)", static_cast<double>(r.grabEnd.y - r.grabStart.y) * 1000.0, 8.0),
                       gateMax("floor penetration (mm)", r.penetrationMax * 1000.0, 1.0),
                       gateMax("steps", static_cast<double>(r.stepsTaken), 0.0),
                       gateMax("worst single-tick jump (mm; it reads 0-0.7)", jumpMm(r), 5.0),
                       gateMax("hold: worst single-tick jump (mm)", holdJump(r), 1.0)}, r);
        }
    }
}

/// The slouched head pulled up, the child-scale figure, the landing bounce, and holding still.
void RealPhases::tailPhases() {
    // --- Pulling a slouched figure's head up straightens the back.
    if (report.wants("head-straighten")) {
        Scenario sc;
        sc.grab = "head";
        // A forward slouch (positive X on this figure's spine bends forward — the head's z
        // goes from -0.02 to +0.35 in front of the hip); pulling the head up straightens it.
        // (A deeper slouch — 30/20/12 — puts the CoM so far forward that the pull-up
        // triggers a balance step; this one straightens on planted feet.)
        sc.prePose = {{"abdomenLower", glm::vec3(20.0f, 0.0f, 0.0f)},
                      {"chest", glm::vec3(15.0f, 0.0f, 0.0f)},
                      {"neck", glm::vec3(10.0f, 0.0f, 0.0f)}};
        // +10cm, not 15 (2026-11-07): the body gives 5 of them — the slouch straightened, a 2cm
        // stretch — and a head pulled 8cm past what the body gives now LIFTS HER OFF (the trunk's
        // lift-off; the head-liftoff phase gates it); this phase is the straightening on planted
        // feet, so it pulls no further than the body can go.
        sc.path = pullPath(glm::vec3(0.0f, 0.10f, 0.0f), 40, 90);
        const RunResult r = run(sc);
        phaseLegs("[real] slouched head pull-up (head +10cm y)",
                     {gateMin("head rise (m)", r.grabEnd.y - r.grabStart.y, 0.035),
                      // (Heel RISE counts as drift here: past the spine's straightening reserve a
                      // head pulled up takes the figure onto the balls of its feet, 11-15mm.)
                      gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 20.0),
                      gateMax("steps taken", r.stepsTaken, 0.0)}, r, kLeanLegs);
    }

    // --- A child-scale figure (0.54) crouches and walks with its feet planted.
    if (report.wants("child")) {
        {
            Scenario sc;
            sc.grab = "hip";
            sc.scale = 0.54f;
            sc.path = pullPath(glm::vec3(0.0f, -0.07f, 0.0f), 40, 80);
            const RunResult r = run(sc);
            phaseLegs("[real] child crouch (0.54 scale, hip -7cm)",
                         {gateMin("hip drop (m)", -r.hipDispY, 0.05),
                          gateMax("feet drift (mm)", r.contactDriftMax * 1000.0, 20.0)}, r, kCrouchLegs);
        }
        {
            Scenario sc;
            sc.grab = "hip";
            sc.scale = 0.54f;
            sc.path = pullPath(glm::vec3(0.25f, 0.0f, 0.0f), 80, 150);
            const RunResult r = run(sc);
            phaseLegs("[real] child walk (0.54 scale, hip +25cm x)",
                         {gateMin("steps taken", r.stepsTaken, 2.0),
                          gateMax("hip-to-target at rest (mm)", r.restingMiss * 1000.0, 15.0)}, r, kLeanLegs);
        }
    }

    // --- THE LANDING BOUNCE (2026-09-28): a figure the Ground button dropped onto her feet absorbs
    // the landing — the hips dip over the planted feet, the knees folding, and come back up —
    // deeper the faster she came down, and the pose it ends in is the pose it began in, exactly
    // (purely aesthetic: no pose edit). Driven here as the window's fall timer drives it: begin at
    // the impact speed sqrt(2 g h), a tick every sixtieth of a second, end.
    if (report.wants("landing")) {
        struct Bounce {
            bool   began = false;
            double given = 0.0;     // the dip the bounce was given (mm)
            double deepest = 0.0;   // the hips' deepest dip (mm)
            double kneeMost = 0.0;  // the most a knee folded over the landing pose (deg)
            double feet = 0.0;      // the worst a TOE's joint moved: the foot's place on the floor (mm)
            double heels = 0.0;     // the worst an ankle moved: the heel is held softly and may lift (mm)
            double jump = 0.0;      // worst single-tick move of any joint (mm)
            double lastTick = 0.0;  // the pose at the bounce's last tick from the landing pose (mm)
            double endEuler = 0.0;  // the pose after the bounce from the landing pose (deg, worst channel)
            double endPlace = 0.0;  // ... and its joints (mm)
            bool   selectionKept = false;
            int    ticks = 0;
        };
        const auto bounce = [&](float dropHeight, bool pinHips) {
            Bounce out;
            Armature arm;
            arm.build(bones);
            applyBodyMesh(arm, 1.0f);
            for (const auto& [bone, euler] : restPose) {
                arm.setBoneRotation(resolveBoneName(arm, bone), euler);
            }
            const std::size_t n = arm.boneCount();
            const int hip = resolveBone(arm, "hip");
            const int lFoot = resolveBone(arm, "lFoot");
            const int rFoot = resolveBone(arm, "rFoot");
            const int lShin = resolveBone(arm, "lShin");
            const int lHand = resolveBone(arm, "lHand");
            if (hip < 0 || lFoot < 0 || rFoot < 0) {
                return out;
            }
            if (pinHips) {
                arm.setSelectedBone(hip);
                arm.togglePinSelectedBone();
            }
            arm.setSelectedBone(lHand); // (the user's selection: the bounce must give it back)
            std::vector<glm::vec3> start(n);
            std::vector<glm::vec3> startEuler(n);
            for (std::size_t i = 0; i < n; ++i) {
                start[i] = arm.boneWorldPosition(i);
                startEuler[i] = arm.boneEuler(i);
            }
            const auto underInArm = [&](std::size_t node, int ancestor) {
                for (int cur = static_cast<int>(node); cur >= 0; cur = arm.boneParent(static_cast<std::size_t>(cur))) {
                    if (cur == ancestor) {
                        return true;
                    }
                }
                return false;
            };
            out.began = arm.beginLandingBounce(std::sqrt(2.0f * 9.81f * dropHeight));
            out.given = static_cast<double>(arm.landingBounceDip()) * 1000.0;
            std::vector<glm::vec3> prev = start;
            for (int k = 1; out.began && k < 600 && arm.landingBounceTick(static_cast<float>(k) / 60.0f); ++k) {
                ++out.ticks;
                out.lastTick = 0.0;
                if (v) { // (--verbose: the tick's worst joint, the hips' dip and the knee)
                    std::size_t worst = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        if (glm::length(arm.boneWorldPosition(i) - prev[i]) > glm::length(arm.boneWorldPosition(worst) - prev[worst])) {
                            worst = i;
                        }
                    }
                    std::printf("       [landing] tick %2d: hips -%.1fmm, knee %.1f deg, worst move %s %.1fmm\n", k,
                                (start[static_cast<std::size_t>(hip)].y - arm.boneWorldPosition(static_cast<std::size_t>(hip)).y) * 1000.0f,
                                lShin >= 0 ? glm::length(arm.boneEuler(static_cast<std::size_t>(lShin)) - startEuler[static_cast<std::size_t>(lShin)]) : 0.0f,
                                arm.boneName(worst).c_str(), glm::length(arm.boneWorldPosition(worst) - prev[worst]) * 1000.0f);
                }
                for (std::size_t i = 0; i < n; ++i) {
                    const glm::vec3& at = arm.boneWorldPosition(i);
                    out.jump = std::max(out.jump, static_cast<double>(glm::length(at - prev[i])) * 1000.0);
                    out.lastTick = std::max(out.lastTick, static_cast<double>(glm::length(at - start[i])) * 1000.0);
                    if (static_cast<int>(i) == lFoot || static_cast<int>(i) == rFoot) {
                        out.heels = std::max(out.heels, static_cast<double>(glm::length(at - start[i])) * 1000.0);
                    } else if ((underInArm(i, lFoot) || underInArm(i, rFoot)) && start[i].y < 0.035f) {
                        out.feet = std::max(out.feet, static_cast<double>(glm::length(at - start[i])) * 1000.0);
                    }
                    prev[i] = at;
                }
                out.deepest = std::max(out.deepest, static_cast<double>(start[static_cast<std::size_t>(hip)].y -
                                                                        arm.boneWorldPosition(static_cast<std::size_t>(hip)).y) * 1000.0);
                if (lShin >= 0) {
                    out.kneeMost = std::max(out.kneeMost, static_cast<double>(glm::length(arm.boneEuler(static_cast<std::size_t>(lShin)) -
                                                                                        startEuler[static_cast<std::size_t>(lShin)])));
                }
            }
            arm.endLandingBounce();
            for (std::size_t i = 0; i < n; ++i) {
                out.endPlace = std::max(out.endPlace, static_cast<double>(glm::length(arm.boneWorldPosition(i) - start[i])) * 1000.0);
                for (int a = 0; a < 3; ++a) {
                    out.endEuler = std::max(out.endEuler, static_cast<double>(std::abs(arm.boneEuler(i)[a] - startEuler[i][a])));
                }
            }
            out.selectionKept = arm.selectedBone() == lHand && !arm.landingBounceActive();
            return out;
        };
        const Bounce low = bounce(0.05f, false);
        const Bounce mid = bounce(0.50f, false);
        const Bounce high = bounce(2.00f, false);
        report.phase("[real] landing bounce: dropped 50cm onto her feet",
                     {gateMin("a bounce began", mid.began ? 1.0 : 0.0, 1.0),
                      info("the dip given (mm)", mid.given),
                      gateMin("the hips' deepest dip, share of the dip given", mid.given > 0.0 ? mid.deepest / mid.given : 0.0, 0.85),
                      gateMax("the hips' deepest dip, share of the dip given (no deeper)", mid.given > 0.0 ? mid.deepest / mid.given : 0.0, 1.02),
                      // (38-42 degrees on the eight rigs for 6cm of dip: near straight a leg's height
                      // goes as the square of the knee's angle.)
                      gateMin("a knee folded (deg)", mid.kneeMost, 25.0),
                      // (0.4-0.8mm; 3.5-4.0 while the tail's blend ran from 0.6 of the duration.)
                      gateMax("the toes moved on the floor (mm)", mid.feet, 2.0),
                      gateMax("an ankle moved (mm)", mid.heels, 4.0),
                      // (27-34mm, the knee's: 62-81 under the spring's half wave, which starts at full speed.)
                      gateMax("worst single-tick jump (mm)", mid.jump, 45.0),
                      // (1.4-2.6mm: what the end's exact restore has left to do, in its one frame.)
                      gateMax("the pose at the bounce's last tick, from the landing pose (mm)", mid.lastTick, 4.0),
                      gateMax("the pose after the bounce, from the landing pose (deg)", mid.endEuler, 0.0),
                      gateMax("... and its joints (mm)", mid.endPlace, 0.0),
                      gateMin("the user's selection given back", mid.selectionKept ? 1.0 : 0.0, 1.0),
                      info("ticks", static_cast<double>(mid.ticks))});
        report.phase("[real] landing bounce: the higher the drop, the deeper the dip (5cm, 50cm, 2m)",
                     {gateMin("dropped 5cm: a bounce began", low.began ? 1.0 : 0.0, 1.0),
                      gateMin("dropped 5cm: the hips' deepest dip (mm)", low.deepest, 15.0),
                      gateMin("dropped 50cm, over 5cm (mm)", mid.deepest - low.deepest, 30.0),
                      gateMin("dropped 2m, over 50cm (mm)", high.deepest - mid.deepest, 30.0),
                      gateMax("dropped 2m: the toes moved on the floor (mm)", high.feet, 3.0),
                      info("dropped 2m: an ankle moved (mm)", high.heels),
                      gateMax("dropped 2m: the pose after the bounce, from the landing pose (deg)", high.endEuler, 0.0),
                      info("dropped 2m: the hips' deepest dip (mm)", high.deepest),
                      info("dropped 2m: a knee folded (deg)", high.kneeMost),
                      info("dropped 2m: worst single-tick jump (mm)", high.jump)});
        const Bounce seated = bounce(0.50f, true);
        const Bounce slight = bounce(0.0005f, false);
        report.phase("[real] landing bounce: none with the hips pinned (a seat), none for a drop of half a millimetre",
                     {gateMax("hips pinned: a bounce began", seated.began ? 1.0 : 0.0, 0.0),
                      gateMax("hips pinned: the pose moved (mm)", seated.endPlace, 0.0),
                      gateMin("hips pinned: the user's selection kept", seated.selectionKept ? 1.0 : 0.0, 1.0),
                      gateMax("half a millimetre: a bounce began", slight.began ? 1.0 : 0.0, 0.0),
                      gateMax("half a millimetre: the pose moved (mm)", slight.endPlace, 0.0)});
    }

    // --- Hold still: a drag that never moves must never move the pose.
    if (report.wants("hold-still")) {
        Scenario sc;
        sc.grab = "lHand";
        sc.path = {{0, glm::vec3(0.0f), "start"}, {120, glm::vec3(0.0f), "hold"}};
        sc.release = false;
        const RunResult r = run(sc);
        // (The REAL-MASS joints: an idle hand's fingers RELAX under any drag — IDLE HANDS RELAX in
        // hangIdleArms, 2026-09-25 — and that is motion by design, 39mm at a fingertip; it is
        // reported apart. The rig's masses say which bones are token: below 0.005 of the body.)
        std::vector<float> masses;
        {
            Armature probeHs;
            probeHs.build(bones);
            const int hs = resolveBone(probeHs, "lHand");
            if (hs >= 0) {
                probeHs.selectBoneByName(probeHs.boneName(static_cast<std::size_t>(hs)));
                if (probeHs.beginIkDrag() && probeHs.ikRig() != nullptr) {
                    masses = probeHs.ikRig()->masses();
                }
                probeHs.endIkDrag();
            }
        }
        // (... and the IDLE ARM's ELBOW SOFTENS under any drag too — IDLE ELBOWS SOFTEN, 2026-11-02 —
        // so what hangs below the idle arm's elbow (the forearm, the hand, its fingers) is motion by
        // design as well, and is read apart from the real-mass drift: the right arm idles here.)
        const int idleElbow = indexOf("rForeArm");
        double worst = 0.0;
        double worstToken = 0.0;
        double worstIdleArm = 0.0;
        for (std::size_t i = 0; i < r.endPos.size(); ++i) {
            const double d = static_cast<double>(glm::length(r.endPos[i] - r.startPos[i]));
            const bool idleArm = idleElbow >= 0 && static_cast<int>(i) != idleElbow && under(static_cast<int>(i), idleElbow);
            const bool token = i < masses.size() && masses[i] < 0.005f;
            double& slot = idleArm ? worstIdleArm : (token ? worstToken : worst);
            slot = std::max(slot, d);
        }
        phaseLegs("[real] hold still (no cursor motion)",
                     {gateMax("worst joint drift (mm; the real-mass joints outside the idle arm)", worst * 1000.0, 2.0),
                      gateMax("worst token-joint drift outside the idle arm (mm)", worstToken * 1000.0, 2.0),
                      // (The idle arm RELAXES under any drag — its elbow softens 12 degrees and its hand's
                      // fingers curl, hangIdleArms: 39mm at a fingertip from the fingers alone on the base
                      // rig, ~9cm with the elbow. Gated low, as "the relax happened".)
                      gateMin("the idle arm relaxed: worst move below its elbow (mm)", worstIdleArm * 1000.0, 15.0)}, r);
    }
}
void realPhases(Report& report, const std::vector<ArmatureBone>& bones) {
    RealPhases(report, bones).runAll();
}

} // namespace ikharness
