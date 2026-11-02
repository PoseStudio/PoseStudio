/**
 * @file simulator.cpp
 * @brief The drag-loop simulator's implementation (see simulator.h): the scripted cursor mirrors
 *        VulkanWindow::benchAdvance, and every per-tick measurement is taken from the Armature's
 *        world positions exactly as the viewport would render them.
 */

#include "simulator.h"

#include "bonealiases.h"

#include "armature.h"
#include "balancecontroller.h"
#include "cursorfilter.h"
#include "bodyvolume.h"
#include "ikmath.h"
#include "ikrig.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace pose;

namespace ikharness {

std::vector<pose::BodyMeshPoint> g_bodyMesh;

void applyBodyMesh(Armature& arm, float scale) {
    if (g_bodyMesh.empty()) {
        return;
    }
    std::vector<BodyMeshPoint> mesh = g_bodyMesh;
    if (scale != 1.0f) {
        for (BodyMeshPoint& p : mesh) {
            p.pos *= scale;
        }
    }
    arm.setBodyMesh(std::move(mesh));
}

std::vector<ArmatureBone> scaledBones(const std::vector<ArmatureBone>& bones, float scale) {
    std::vector<ArmatureBone> out = bones;
    for (ArmatureBone& b : out) {
        b.localBindTranslation *= scale;
    }
    return out;
}


int resolveBone(const Armature& arm, const std::string& canonical) {
    return aliasedBoneIndex(arm, canonical); // (the one alias table: scene/ik/bonealiases.h)
}

double volumePenetration(const Armature& arm, int* worstNode, int* worstVolume, bool riders) {
    const IkRig* rig = arm.ikRig();
    const int n = static_cast<int>(arm.boneCount());
    if (worstNode != nullptr) {
        *worstNode = -1;
    }
    if (worstVolume != nullptr) {
        *worstVolume = -1;
    }
    if (rig == nullptr) {
        return 0.0;
    }
    const auto pos = [&](int i) { return arm.boneWorldPosition(static_cast<std::size_t>(i)); };
    double worst = 0.0;
    for (std::size_t vi = 0; vi < rig->bodyVolumes().size(); ++vi) {
        const BodyVolume& v = rig->bodyVolumes()[vi];
        const VolumeAxis ax = volumeAxis(v, n, pos);
        if (!ax.ok) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const char applies = v.applies[static_cast<std::size_t>(i)];
            if (!applies || (applies == kVolumeAppliesRider) != riders) {
                continue;
            }
            const glm::vec3 P = pos(i);
            const float t = volumeAxisParam(ax, P);
            const float d = glm::length(P - (ax.A + ax.ab * t));
            double depth;
            if (applies == kVolumeAppliesRider) {
                depth = volumeRadiusAt(v, t) + rig->riderClearance() - d;
            } else {
                depth = volumeRadiusAt(v, t) + rig->volumeClearance(i) - d;
                // The segment into the joint (two limbs cross at their middles).
                const float segR = rig->volumeSegmentRadius(i);
                const int par = arm.boneParent(static_cast<std::size_t>(i));
                if (volumeAppliesSegment(v, applies) && segR > 0.0f && par >= 0) {
                    float sPar = 0.0f;
                    float tPar = 0.0f;
                    const float dSeg = closestSegmentPoints(pos(par), P, ax.A, ax.B, sPar, tPar);
                    depth = std::max(depth, static_cast<double>(volumeRadiusAt(v, tPar) + std::max(kVolumeSegmentFlesh * segR, rig->volumeClearance(i)) - dSeg));
                }
            }
            if (depth > worst) {
                worst = depth;
                if (worstNode != nullptr) {
                    *worstNode = i;
                }
                if (worstVolume != nullptr) {
                    *worstVolume = static_cast<int>(vi);
                }
            }
        }
    }
    return worst;
}

std::vector<int> resolveBoneList(const Armature& arm, const char* commaSeparated) {
    std::vector<int> out;
    const std::string names(commaSeparated != nullptr ? commaSeparated : "");
    std::size_t from = 0;
    while (from <= names.size()) {
        const std::size_t comma = names.find(',', from);
        const std::string name = names.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
        from = comma == std::string::npos ? names.size() + 1 : comma + 1;
        const int idx = resolveBone(arm, name);
        if (idx >= 0) {
            out.push_back(idx);
        }
    }
    return out;
}

std::string resolveBoneName(const Armature& arm, const std::string& canonical) {
    return aliasedBoneName(arm, canonical);
}

double rotationAngleDeg(const glm::mat3& a, const glm::mat3& b) {
    const glm::mat3 rel = glm::transpose(a) * b;
    const float c = glm::clamp((rel[0][0] + rel[1][1] + rel[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    return glm::degrees(std::acos(c));
}

RunResult runScenario(const std::vector<ArmatureBone>& baseBones, const Scenario& sc,
                      bool verbose) {
    RunResult r;
    // (Sized first: a phase that indexes r.phases[0] on a run that failed early — a bone the rig
    // lacks — must read an empty PhaseStats, not an empty vector's end.)
    r.phases.resize(sc.path.size() > 1 ? sc.path.size() - 1 : 0);
    for (std::size_t s = 0; s + 1 < sc.path.size(); ++s) {
        r.phases[s].label = sc.path[s + 1].label;
    }
    Armature arm;
    // One scaled copy for the whole run (a child-scale scenario); the base list otherwise.
    const std::vector<ArmatureBone> scaled =
        sc.scale == 1.0f ? std::vector<ArmatureBone>{} : scaledBones(baseBones, sc.scale);
    const std::vector<ArmatureBone>& bones = sc.scale == 1.0f ? baseBones : scaled;
    arm.build(bones);
    applyBodyMesh(arm, sc.scale);
    const std::size_t n = arm.boneCount();
    const auto applyPrePose = [&]() {
        for (const auto& [bone, euler] : sc.prePose) {
            if (!arm.setBoneRotation(resolveBoneName(arm, bone), euler)) {
                r.error = "pre-pose bone not found: " + bone;
                return false;
            }
        }
        return true;
    };
    if (!applyPrePose()) {
        return r;
    }
    if (sc.hover != 0.0f) {
        auto pose = arm.capturePose();
        // The rig root is the first multi-child descendant of the anatomical root (the hip on
        // real figures); the hover goes on the hip's pose translation like the engine writes it.
        int hip = resolveBone(arm, "hip");
        if (hip < 0) {
            hip = 0;
        }
        pose.emplace_back("@trans:" + arm.boneName(static_cast<std::size_t>(hip)),
                          glm::vec3(0.0f, sc.hover, 0.0f));
        arm.applyPose(pose);
    }
    const auto applyUserPins = [&]() {
        for (const std::string& pin : sc.userPins) {
            const int idx = resolveBone(arm, pin);
            if (idx < 0) {
                r.error = "pin bone not found: " + pin;
                return false;
            }
            arm.setSelectedBone(idx);
            arm.togglePinSelectedBone();
        }
        return true;
    };
    if (!sc.pinsAfterBefore && !applyUserPins()) {
        return r;
    }
    // Where every joint sits before the preludes (Scenario::toRest).
    std::vector<glm::vec3> restPos(n);
    for (std::size_t i = 0; i < n; ++i) {
        restPos[i] = arm.boneWorldPosition(i);
    }
    // The drags made and released before the measured one (the window's sequence: press, the
    // 60 Hz ticks through the cursor filter, mouse-up, the settle, the end).
    for (const PreludeDrag& pre : sc.before) {
        const int g = resolveBone(arm, pre.grab);
        if (g < 0) {
            r.error = "prelude grab bone not found: " + pre.grab;
            return r;
        }
        arm.setSelectedBone(g);
        if (!arm.beginIkDrag()) {
            r.error = "prelude beginIkDrag failed";
            return r;
        }
        const glm::vec3 start = arm.boneWorldPosition(static_cast<std::size_t>(g));
        IkCursorFilter preFilter;
        preFilter.seed(start);
        const int total = pre.moveTicks + pre.holdTicks;
        for (int t = 1; t <= total; ++t) {
            const float f = pre.moveTicks > 0
                                ? std::min(1.0f, static_cast<float>(t) / static_cast<float>(pre.moveTicks))
                                : 1.0f;
            arm.dragIkTo(preFilter.update(start + pre.offset * f));
        }
        for (int k = 0; k < 600 && arm.settleIkTick(); ++k) {
        }
        arm.endIkDrag();
    }
    if (sc.resetAfterBefore) {
        arm.resetPose(); // (Reset Pose: every channel to zero, the pose translation too)
        if (!applyPrePose()) {
            return r;
        }
    }
    if (sc.pinsAfterBefore && !applyUserPins()) {
        return r;
    }
    r.grabIndex = resolveBone(arm, sc.grab);
    if (r.grabIndex >= 0) {
        arm.setSelectedBone(r.grabIndex);
        if (sc.grabShare >= 0.0f) {
            arm.setIkGrabOnSegment(r.grabIndex, sc.grabShare);
        } else if (glm::length(sc.grabOffset) > 0.0f) {
            arm.setIkGrabPoint(r.grabIndex, arm.boneWorldPosition(static_cast<std::size_t>(r.grabIndex)) + sc.grabOffset);
        } else if (arm.selectedBone() != r.grabIndex) {
            // (A bone of the FACE RIG selects the head — Armature::posingBone: what the user has
            // hold of is then the point where the named bone is, as a click on the face leaves it.)
            arm.setIkGrabPoint(r.grabIndex, arm.boneWorldPosition(static_cast<std::size_t>(r.grabIndex)));
        }
    }
    if (r.grabIndex < 0) {
        r.error = "grab bone not found: " + sc.grab;
        return r;
    }
    for (const std::string& name : sc.alsoGrab) { // (THE GROUP: after the selection, which clears it)
        const int member = resolveBone(arm, name);
        if (member < 0) {
            r.error = "group bone not found: " + name;
            return r;
        }
        arm.toggleGroupBone(member);
    }
    const glm::vec3 grabPointStart = arm.ikGrabPointWorld();
    r.startEuler.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        r.startEuler[i] = arm.boneEuler(i);
    }
    if (!arm.beginIkDrag(sc.scoped ? pose::IkScope::Part : pose::IkScope::Body)) {
        r.error = "beginIkDrag failed";
        return r;
    }
    const IkRig* rig = arm.ikRig();
    // The STANDING contacts (the feet): a floor contact the pose made — a kneeling knee, a hand
    // on the floor, seeded as a live pin — may lift off, which is not a foot drifting.
    for (std::size_t p = 0; p < rig->pins().size(); ++p) {
        if (!rig->pinIsUser(p) && !rig->pinIsLive(p)) {
            r.contactPins.push_back(rig->pins()[p].node);
        }
    }
    r.startPos.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        r.startPos[i] = arm.boneWorldPosition(i);
    }
    r.grabStart = grabPointStart;
    r.grabEulerStart = arm.boneEuler(static_cast<std::size_t>(r.grabIndex));
    std::vector<Waypoint> path = sc.path;
    r.restPos = restPos;
    if (sc.toRest) {
        const glm::vec3 home = restPos[static_cast<std::size_t>(r.grabIndex)] - r.grabStart;
        for (Waypoint& w : path) {
            if (glm::length(w.offset) > 0.0f) {
                w.offset = home;
            }
        }
    }
    std::vector<glm::vec3> startEuler(n);
    for (std::size_t i = 0; i < n; ++i) {
        startEuler[i] = arm.boneEuler(i);
    }
    std::vector<glm::vec3> contactStart;
    for (const int c : r.contactPins) {
        contactStart.push_back(r.startPos[static_cast<std::size_t>(c)]);
        // Bind height: the local translations summed (the armature's transform is identity).
        float y = 0.0f;
        for (int cur = c; cur >= 0; cur = arm.boneParent(static_cast<std::size_t>(cur))) {
            y += bones[static_cast<std::size_t>(cur)].localBindTranslation.y;
        }
        r.contactBindMinY = std::min(r.contactBindMinY, static_cast<double>(y));
    }
    // User-pin state (world position + rotation at drag start).
    std::vector<int> userPinIdx;
    std::vector<glm::vec3> userPinPos;
    std::vector<glm::mat3> userPinRot;
    for (const std::string& pin : sc.userPins) {
        const int idx = resolveBone(arm, pin);
        userPinIdx.push_back(idx);
        userPinPos.push_back(arm.boneWorldPosition(static_cast<std::size_t>(idx)));
        userPinRot.push_back(glm::mat3(arm.poseGlobal(static_cast<std::size_t>(idx))));
    }
    const int hip = resolveBone(arm, "hip");
    const int head = resolveBone(arm, "head");
    // (Every bind is translation-only, so a bone's posed rotation is its rotation away from the
    // bind: the head's +Z is where it looks, its +X its lateral axis.)
    const auto headAxisElevation = [&](const glm::vec3& local) {
        if (head < 0) {
            return -999.0;
        }
        const glm::vec3 axis = glm::normalize(glm::mat3(arm.poseGlobal(static_cast<std::size_t>(head))) * local);
        return static_cast<double>(glm::degrees(std::asin(glm::clamp(axis.y, -1.0f, 1.0f))));
    };
    r.gazeStartDeg = headAxisElevation(glm::vec3(0.0f, 0.0f, 1.0f));
    // (Left/right asymmetry: see RunResult::asymmetryStart. About the plane through the HIPS: a
    // figure that has walked or side-stepped is not lopsided for standing somewhere else.)
    const auto asymmetryIn = [&](const Armature& of, std::size_t i) {
        const int twin = of.mirrorBone(i);
        const float plane = hip >= 0 ? of.boneWorldPosition(static_cast<std::size_t>(hip)).x : 0.0f;
        const glm::vec3 here = of.boneWorldPosition(i);
        if (twin < 0 || static_cast<std::size_t>(twin) == i) {
            return static_cast<double>(std::abs(here.x - plane));
        }
        const glm::vec3 there = of.boneWorldPosition(static_cast<std::size_t>(twin));
        return static_cast<double>(glm::length(glm::vec3(2.0f * plane - here.x, here.y, here.z) - there));
    };
    const auto asymmetryOf = [&](std::size_t i) { return asymmetryIn(arm, i); };
    // (Only the bones BELOW the hips: the figure's and a follower's scene nodes stand at the origin
    // whatever the body does.)
    std::vector<char> belowHips(arm.boneCount(), hip < 0 ? 1 : 0);
    for (std::size_t i = 0; i < arm.boneCount() && hip >= 0; ++i) {
        const int parent = arm.boneParent(i);
        belowHips[i] = static_cast<int>(i) == hip || (parent >= 0 && belowHips[static_cast<std::size_t>(parent)]);
    }
    const auto asymmetryNow = [&]() {
        double worst = 0.0;
        for (std::size_t i = 0; i < arm.boneCount(); ++i) {
            if (belowHips[i]) {
                worst = std::max(worst, asymmetryOf(i));
            }
        }
        return worst;
    };
    r.asymmetryStart = asymmetryNow();
    // (What the RIG itself is lopsided by, at rest: a character's morphs need not be symmetric.)
    double asymmetryRest = 0.0;
    {
        Armature rest;
        rest.build(bones);
        for (std::size_t i = 0; i < rest.boneCount() && i < belowHips.size(); ++i) {
            if (belowHips[i]) {
                asymmetryRest = std::max(asymmetryRest, asymmetryIn(rest, i));
            }
        }
    }
    {
        const auto centreBone = [&](const std::string& name) {
            const int idx = resolveBone(arm, name);
            return idx >= 0 && arm.mirrorBone(static_cast<std::size_t>(idx)) == idx;
        };
        bool symmetric = centreBone(sc.grab) && sc.cursorNoise == 0.0f;
        for (const Waypoint& w : sc.path) {
            symmetric = symmetric && std::abs(w.offset.x) < 1.0e-6f;
        }
        for (const PreludeDrag& pre : sc.before) {
            symmetric = symmetric && centreBone(pre.grab) && std::abs(pre.offset.x) < 1.0e-6f;
        }
        for (const std::string& pin : sc.userPins) {
            symmetric = symmetric && centreBone(pin);
        }
        // (The pose the measured drag FOUND says whether the pre-pose and the preludes were.)
        // ... and only on a rig that is ITSELF symmetric (every base figure is, to 0.0002mm): a
        // character's is not quite, and a body's own asymmetry is amplified, rightly — a leg a
        // millimetre longer stands 4 degrees more bent at the knee under a level pelvis (its length
        // goes as the SQUARE of the knee's angle near straight: 18mm at the knee), and skin that
        // differs left to right kneels 9mm askew on a perfectly symmetric skeleton.
        r.symmetricGesture = symmetric && asymmetryRest < 1.0e-5 && r.asymmetryStart < 0.005;
        r.asymmetryGateMm = sc.asymmetryGateMm;
    }
    if (std::getenv("IK_HARNESS_ASYMMETRY_TRACE") != nullptr) {
        for (std::size_t i = 0; i < arm.boneCount(); ++i) {
            if (belowHips[i] && arm.mirrorBone(i) >= static_cast<int>(i) && asymmetryOf(i) > 0.02) {
                std::printf("  [asymmetry] %s | %s: %.1f mm at the start\n", arm.boneName(i).c_str(),
                            arm.boneName(static_cast<std::size_t>(arm.mirrorBone(i))).c_str(), asymmetryOf(i) * 1000.0);
            }
        }
    }
    const int chest = resolveBone(arm, "chest");
    // The feet, for the suspension / floor checks; the hands, for the live-contact phases.
    std::vector<int> feet;
    for (const char* f : {"lFoot", "rFoot"}) {
        const int idx = resolveBone(arm, f);
        if (idx >= 0) {
            feet.push_back(idx);
        }
    }
    std::vector<int> hands;
    for (const char* h : {"lHand", "rHand"}) {
        const int idx = resolveBone(arm, h);
        if (idx >= 0) {
            hands.push_back(idx);
        }
    }
    const auto handsNow = [&]() {
        double y = 1e9;
        for (const int h : hands) {
            y = std::min(y, static_cast<double>(arm.boneWorldPosition(static_cast<std::size_t>(h)).y));
        }
        return y;
    };
    // Bind heights of every joint (penetration check).
    std::vector<float> bindY(n, 0.0f);
    {
        for (std::size_t i = 0; i < n; ++i) {
            const int p = bones[i].parent;
            bindY[i] = bones[i].localBindTranslation.y + (p >= 0 ? bindY[static_cast<std::size_t>(p)] : 0.0f);
        }
    }

    for (const int f : feet) {
        r.feetBindMinY = std::min(r.feetBindMinY, static_cast<double>(bindY[static_cast<std::size_t>(f)]));
    }

    // Body joints (below the rig root) — the floor rule applies to these only.
    std::vector<char> bodyNode(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (int cur = static_cast<int>(i); cur >= 0; cur = arm.boneParent(static_cast<std::size_t>(cur))) {
            if (cur == rig->rootNode()) {
                bodyNode[i] = 1;
                break;
            }
        }
    }
    // Leg diagnostics (see RunResult): each leg's hip socket, thigh twist bone (its one free
    // channel = the widest authored range), knee and ankle; plus each contact-pinned foot's
    // drag-start rotation.
    struct Leg {
        int       socket = -1, twist = -1, knee = -1, ankle = -1, twistAxis = 1;
        float     twistStart = 0.0f;
        glm::vec3 prevKnee{0.0f}, prevKneeStep{0.0f};
        double    osc = 0.0;
    };
    std::vector<Leg> legs;
    for (const char* side : {"l", "r"}) {
        Leg leg;
        const std::string s(side);
        leg.socket = resolveBone(arm, s + "Thigh");
        leg.twist = resolveBone(arm, s + "ThighTwist");
        leg.knee = resolveBone(arm, s + "Shin");
        leg.ankle = resolveBone(arm, s + "Foot");
        if (leg.socket < 0 || leg.knee < 0 || leg.ankle < 0) {
            continue;
        }
        if (leg.twist >= 0) {
            const ArmatureBone& tb = bones[static_cast<std::size_t>(leg.twist)];
            float widest = -1.0f;
            for (int a = 0; a < 3; ++a) {
                const float range = tb.rotationLimited[a] ? tb.rotationMax[a] - tb.rotationMin[a] : 360.0f;
                if (range > widest) {
                    widest = range;
                    leg.twistAxis = a;
                }
            }
            leg.twistStart = arm.boneEuler(static_cast<std::size_t>(leg.twist))[leg.twistAxis];
        }
        leg.prevKnee = arm.boneWorldPosition(static_cast<std::size_t>(leg.knee));
        legs.push_back(leg);
    }
    const auto wrapDeg = [](float d) {
        while (d > 180.0f) d -= 360.0f;
        while (d <= -180.0f) d += 360.0f;
        return d;
    };
    const auto kneeTwist = [&](const Leg& leg) {
        if (leg.twist < 0) {
            return 0.0; // a generation without thigh twist bones: no twist channel to read
        }
        return static_cast<double>(std::abs(wrapDeg(
            arm.boneEuler(static_cast<std::size_t>(leg.twist))[leg.twistAxis] - leg.twistStart)));
    };
    const auto kneeInward = [&](const Leg& leg) {
        const glm::vec3 socket = arm.boneWorldPosition(static_cast<std::size_t>(leg.socket));
        const glm::vec3 ankle = arm.boneWorldPosition(static_cast<std::size_t>(leg.ankle));
        const glm::vec3 knee = arm.boneWorldPosition(static_cast<std::size_t>(leg.knee));
        glm::vec3 axis = ankle - socket;
        const float len = glm::length(axis);
        if (len < 1e-4f) {
            return 0.0;
        }
        axis /= len;
        const glm::vec3 off = (knee - socket) - axis * glm::dot(knee - socket, axis);
        glm::vec3 inward(0.0f); // toward the other leg's socket, perpendicular to the line
        for (const Leg& other : legs) {
            if (other.socket != leg.socket) {
                inward = arm.boneWorldPosition(static_cast<std::size_t>(other.socket)) - socket;
            }
        }
        inward -= axis * glm::dot(inward, axis);
        const float il = glm::length(inward);
        return il > 1e-4f ? static_cast<double>(glm::dot(off, inward / il)) : 0.0;
    };
    // The knee's OUTBOARD offset from its socket along the socket-to-socket line (the splay).
    const auto kneeSplay = [&](const Leg& leg) {
        const glm::vec3 socket = arm.boneWorldPosition(static_cast<std::size_t>(leg.socket));
        const glm::vec3 knee = arm.boneWorldPosition(static_cast<std::size_t>(leg.knee));
        glm::vec3 inward(0.0f);
        for (const Leg& other : legs) {
            if (other.socket != leg.socket) {
                inward = arm.boneWorldPosition(static_cast<std::size_t>(other.socket)) - socket;
            }
        }
        inward.y = 0.0f;
        const float il = glm::length(inward);
        return il > 1e-4f ? -static_cast<double>(glm::dot(knee - socket, inward / il)) : 0.0;
    };
    const auto kneeOverFoot = [&](const Leg& leg) {
        const glm::vec3 socket = arm.boneWorldPosition(static_cast<std::size_t>(leg.socket));
        const glm::vec3 knee = arm.boneWorldPosition(static_cast<std::size_t>(leg.knee));
        const glm::vec3 ankle = arm.boneWorldPosition(static_cast<std::size_t>(leg.ankle));
        glm::vec3 inward(0.0f);
        for (const Leg& other : legs) {
            if (other.socket != leg.socket) {
                inward = arm.boneWorldPosition(static_cast<std::size_t>(other.socket)) - socket;
            }
        }
        inward.y = 0.0f;
        const float il = glm::length(inward);
        return il > 1e-4f ? -static_cast<double>(glm::dot(knee - ankle, inward / il)) : 0.0;
    };
    std::vector<glm::mat3> contactStartRot;
    for (const int c : r.contactPins) {
        contactStartRot.push_back(glm::mat3(arm.poseGlobal(static_cast<std::size_t>(c))));
    }
    // The cursor the app can SERVE: pushed out of the body volumes the way the Armature clamps
    // its drag goal (IkRig::clampOutOfVolumes) — a cursor inside the chest asks for the hand at
    // the chest's surface, and the lag / resting-miss metrics measure against that (the bench
    // path's flick lands 6cm from the spine on the T-pose rigs, whose pre-posed hand rests
    // closer to the body; refusing to enter the body is the contract, not a miss). Only when
    // the grabbed joint IS the effector (a promoted grab's clamp acts on the hand, not the
    // finger).
    const auto servable = [&](const glm::vec3& cursor) {
        if (!rig->dragActive() || rig->dragEffector() != r.grabIndex || rig->bodyVolumes().empty()) {
            return cursor;
        }
        std::vector<glm::vec3> current(n);
        for (std::size_t i = 0; i < n; ++i) {
            current[i] = arm.boneWorldPosition(i);
        }
        return rig->clampOutOfVolumes(r.grabIndex, cursor, current);
    };
    // Body-volume penetration (see RunResult::volumePenetrationMax): the deepest tested joint
    // inside any volume, from the rig's own volumes at the current world positions (a rigid
    // transform preserves distances, so world and model space agree).
    int volumeWorstNode = -1;
    int volumeWorstVolume = -1;
    const auto volumeDepth = [&]() {
        // The shared measure (joints AND the segments into them), see volumePenetration.
        return volumePenetration(arm, &volumeWorstNode, &volumeWorstVolume);
    };
    int riderWorstNode = -1;
    const auto riderDepth = [&]() {
        return volumePenetration(arm, &riderWorstNode, nullptr, true);
    };
    const auto footRotNow = [&]() {
        double worst = 0.0;
        for (std::size_t c = 0; c < r.contactPins.size(); ++c) {
            worst = std::max(worst, rotationAngleDeg(contactStartRot[c],
                                                     glm::mat3(arm.poseGlobal(static_cast<std::size_t>(r.contactPins[c])))));
        }
        return worst;
    };
    IkCursorFilter filter;
    filter.seed(r.grabStart);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<glm::vec3> prev(n);      // every joint at the last tick
    std::vector<glm::vec3> prevPrev(n);  // ... and the tick before (the pose trace's step column)
    for (std::size_t i = 0; i < n; ++i) {
        prev[i] = arm.boneWorldPosition(i);
    }
    glm::vec3 prevEff = r.grabStart;
    glm::vec3 prevEffStep(0.0f);
    double prevEffSpeed = 0.0;
    glm::vec3 prevCursor = r.grabStart;
    double prevCursorSpeed = 0.0;
    glm::vec3 raw = r.grabStart;
    int tick = 0;
    int livePrev = -1; // (live contacts at the previous tick: lift-offs are counted from drops)
    while (true) {
        ++tick;
        // The scripted cursor (mirrors VulkanWindow::benchAdvance).
        std::size_t seg = sc.path.size();
        for (std::size_t s = 1; s < sc.path.size(); ++s) {
            if (tick <= sc.path[s].tick) {
                const float f = static_cast<float>(tick - sc.path[s - 1].tick) /
                                static_cast<float>(sc.path[s].tick - sc.path[s - 1].tick);
                raw = r.grabStart + glm::mix(path[s - 1].offset, path[s].offset, f);
                seg = s - 1;
                break;
            }
        }
        if (seg >= sc.path.size()) {
            break; // past the last waypoint: release
        }
        if (sc.cursorNoise > 0.0f) {
            raw += glm::vec3(noise(rng), noise(rng), noise(rng)) * sc.cursorNoise;
        }
        PhaseStats& ph = r.phases[seg];
        const bool moved = arm.dragIkTo(filter.update(raw));
        ++ph.ticks;
        ++r.dragTicks;
        if (moved) {
            ++ph.movedTicks;
        }
        const glm::vec3 eff = arm.ikGrabPointWorld();
        const double lag = glm::length(eff - servable(raw));
        ph.lagSum += lag;
        ph.lagMax = std::max(ph.lagMax, lag);
        // Motion profile of the grabbed joint and of the cursor.
        const glm::vec3 effStep = eff - prevEff;
        const double effSpeed = glm::length(effStep);
        ph.effSpeedUpMax = std::max(ph.effSpeedUpMax, effSpeed - prevEffSpeed);
        ph.effSpeedDownMax = std::max(ph.effSpeedDownMax, prevEffSpeed - effSpeed);
        const double cursorSpeed = glm::length(raw - prevCursor);
        ph.cursorSpeedUpMax = std::max(ph.cursorSpeedUpMax, cursorSpeed - prevCursorSpeed);
        ph.cursorSpeedDownMax = std::max(ph.cursorSpeedDownMax, prevCursorSpeed - cursorSpeed);
        prevCursorSpeed = cursorSpeed;
        prevCursor = raw;
        // Oscillation (the app's [ikperf] osc): reversal overlap of the grabbed joint's steps.
        {
            const float pl = glm::length(prevEffStep);
            if (pl > 1e-6f && effSpeed > 1e-6) {
                const float against = -glm::dot(effStep, prevEffStep) / pl;
                if (against > 0.0f) {
                    ph.effOsc += std::min(static_cast<double>(against), static_cast<double>(pl));
                }
            }
            prevEffStep = effStep;
        }
        prevEff = eff;
        prevEffSpeed = effSpeed;
        // Per-joint steps: jumps, idle oscillation, contact drift, pin fidelity, floor.
        // IK_HARNESS_POSE_TRACE=1 also dumps the pose at the first few ticks any joint jumps
        // more than 3cm — a basin flip caught in the act.
        static const bool kJumpTrace = std::getenv("IK_HARNESS_POSE_TRACE") != nullptr;
        static int jumpDumps = 0;
        double tickJump = 0.0;
        std::size_t tickJumpJoint = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 p = arm.boneWorldPosition(i);
            const double step = glm::length(p - prev[i]);
            if (step > tickJump) {
                tickJumpJoint = i;
            }
            tickJump = std::max(tickJump, step);
            ph.maxJump = std::max(ph.maxJump, step);
            if (static_cast<int>(i) == head) {
                r.headJumpMax = std::max(r.headJumpMax, step);
            }
            r.minJointY = std::min(r.minJointY, static_cast<double>(p.y - bindY[i]));
            if (bodyNode[i] && !arm.footLyingExempt(static_cast<int>(i))) { // (a lying foot's ankle and mid-foot: their clearances are the standing foot's)
                const double pen = rig->floorClearance(static_cast<int>(i)) - p.y;
                if (pen > r.penetrationMax) {
                    r.penetrationMax = pen;
                    r.penetrationNode = static_cast<int>(i);
                    static const bool kPenTrace = std::getenv("IK_HARNESS_PENETRATION_TRACE") != nullptr;
                    if (kPenTrace && pen > 0.01) {
                        std::printf("[penetration] tick %d: %s %.1f mm through the floor (clearance %.1f)", tick, arm.boneName(i).c_str(), pen * 1000.0,
                                    rig->floorClearance(static_cast<int>(i)) * 1000.0);
                        std::putchar(10);
                    }
                }
            }
            prevPrev[i] = prev[i];
            prev[i] = p;
        }
        // (IK_HARNESS_POSE_TRACE=<n> > 1: that many ticks instead of the first six.)
        static const int kJumpDumpLimit = std::max(6, std::atoi(std::getenv("IK_HARNESS_POSE_TRACE") ? std::getenv("IK_HARNESS_POSE_TRACE") : "0"));
        // (IK_HARNESS_POSE_TRACE_MM=<mm> lowers the dump's threshold from 30mm: a hold-still gate
        // of 25 that fails at 27 names its joint that way.)
        static const double kJumpDumpM = std::getenv("IK_HARNESS_POSE_TRACE_MM") != nullptr
                                             ? std::atof(std::getenv("IK_HARNESS_POSE_TRACE_MM")) * 0.001
                                             : 0.03;
        if (kJumpTrace && tickJump > kJumpDumpM && jumpDumps < kJumpDumpLimit) {
            ++jumpDumps;
            std::printf("[pose] the joint that jumped farthest: %s\n", arm.boneName(tickJumpJoint).c_str());
            std::printf("[pose] tick %d: a joint jumped %.1f mm; cursor(%.3f %.3f %.3f) grab(%.3f %.3f %.3f)\n",
                        tick, tickJump * 1000.0, raw.x, raw.y, raw.z, eff.x, eff.y, eff.z);
            for (std::size_t i = 0; i < n; ++i) {
                const glm::vec3& e = arm.boneEuler(i);
                if (std::max({std::abs(e.x), std::abs(e.y), std::abs(e.z)}) > 3.0f) {
                    std::printf("[pose]   %-18s euler(%7.1f %7.1f %7.1f) step %.1f mm\n",
                                arm.boneName(i).c_str(), e.x, e.y, e.z,
                                glm::length(arm.boneWorldPosition(i) - prevPrev[i]) * 1000.0);
                }
            }
        }
        for (std::size_t c = 0; c < r.contactPins.size(); ++c) {
            const glm::vec3 p = arm.boneWorldPosition(static_cast<std::size_t>(r.contactPins[c]));
            r.contactDriftMaxDrag = std::max(r.contactDriftMaxDrag, static_cast<double>(glm::length(p - contactStart[c])));
            r.contactMinY = std::min(r.contactMinY, static_cast<double>(p.y));
            r.contactRiseMax = std::max(r.contactRiseMax,
                                        static_cast<double>(p.y - bindY[static_cast<std::size_t>(r.contactPins[c])]));
        }
        {
            const double depth = volumeDepth();
            if (depth > r.volumePenetrationMax) {
                r.volumePenetrationMax = depth;
                r.volumePenetrationNode = volumeWorstNode;
                r.volumePenetrationTick = tick;
            }
            {
                const double rd = riderDepth();
                if (rd > r.riderPenetrationMax) {
                    r.riderPenetrationMax = rd;
                    r.riderPenetrationNode = riderWorstNode;
                }
            }
            // IK_HARNESS_VOLUME_TRACE=1: the applied pose's deepest joint in a body volume, per tick.
            // IK_HARNESS_ASYMMETRY_TRACE=2: the worst left/right pair, per tick, once it passes 5mm.
            static const bool kAsymTick = std::getenv("IK_HARNESS_ASYMMETRY_TRACE") != nullptr &&
                                          std::getenv("IK_HARNESS_ASYMMETRY_TRACE")[0] == '2';
            if (kAsymTick) {
                std::size_t worstBone = 0;
                double      worst = 0.0;
                for (std::size_t i = 0; i < arm.boneCount(); ++i) {
                    if (belowHips[i] && asymmetryOf(i) > worst) {
                        worst = asymmetryOf(i);
                        worstBone = i;
                    }
                }
                if (worst > 0.005) {
                    std::printf("[asym] tick %d: %.1f mm at %s", tick, worst * 1000.0, arm.boneName(worstBone).c_str());
                    std::putchar(10);
                }
            }
            static const bool kVolTrace = std::getenv("IK_HARNESS_VOLUME_TRACE") != nullptr;
            if (kVolTrace && depth > 0.0005) {
                const IkRig* rigT = arm.ikRig();
                const int va = (rigT != nullptr && volumeWorstVolume >= 0) ? rigT->bodyVolumes()[static_cast<std::size_t>(volumeWorstVolume)].a : -1;
                const int vb = (rigT != nullptr && volumeWorstVolume >= 0) ? rigT->bodyVolumes()[static_cast<std::size_t>(volumeWorstVolume)].b : -1;
                std::printf("[vol] tick %d applied %.1f mm at %s in %s-%s#%d\n", tick, depth * 1000.0,
                            volumeWorstNode >= 0 ? arm.boneName(static_cast<std::size_t>(volumeWorstNode)).c_str() : "?",
                            va >= 0 ? arm.boneName(static_cast<std::size_t>(va)).c_str() : "?",
                            vb >= 0 ? arm.boneName(static_cast<std::size_t>(vb)).c_str() : "?", volumeWorstVolume);
            }
        }
        for (std::size_t k = 0; k < userPinIdx.size(); ++k) {
            const std::size_t j = static_cast<std::size_t>(userPinIdx[k]);
            const glm::vec3 p = arm.boneWorldPosition(j);
            r.userPinDistMax = std::max(r.userPinDistMax, static_cast<double>(glm::length(p - userPinPos[k])));
            r.userPinRotMaxDeg = std::max(r.userPinRotMaxDeg, rotationAngleDeg(userPinRot[k], glm::mat3(arm.poseGlobal(j))));
        }
        if (rig->pins().empty() && !r.contactPins.empty()) {
            r.suspended = true;
        }
        r.stepsTaken = rig->stepsTaken();
        {
            const std::vector<IkEffector>& pins = rig->pins();
            r.pinsMax = std::max(r.pinsMax, static_cast<int>(pins.size()));
            int live = 0;
            for (std::size_t p = 0; p < pins.size(); ++p) {
                if (!rig->pinIsLive(p)) {
                    continue;
                }
                ++live;
                const glm::vec3 pos(arm.poseGlobal(static_cast<std::size_t>(pins[p].node))[3]);
                r.livePinSlideMax = std::max(
                    r.livePinSlideMax,
                    static_cast<double>(glm::length(
                        glm::vec2(pos.x - pins[p].target.x, pos.z - pins[p].target.z))));
            }
            r.livePinsMax = std::max(r.livePinsMax, live);
            // Live contacts LOST since the last tick: a lift-off (the rig's release of a taut,
            // risen contact), whatever lands afterwards.
            if (livePrev >= 0 && live < livePrev) {
                r.liveLiftOffs += livePrev - live;
            }
            livePrev = live;
        }
        r.handsMinY = std::min(r.handsMinY, handsNow());
        // IK_HARNESS_ARM_TRACE=1: the left arm's shoulder / twist / elbow Euler and the hand's
        // position per drag tick — which joints an arm gesture is served by, tick by tick.
        static const bool kArmTrace = std::getenv("IK_HARNESS_ARM_TRACE") != nullptr;
        if (kArmTrace) {
            const int sh = resolveBone(arm, "lShldr");
            const int tw = resolveBone(arm, "lShldrTwist");
            const int el = resolveBone(arm, "lForeArm");
            const int hd = resolveBone(arm, "lHand");
            const auto eul = [&](int b) { return b >= 0 ? arm.boneEuler(static_cast<std::size_t>(b)) : glm::vec3(0.0f); };
            const glm::vec3 s = eul(sh), t = eul(tw), e = eul(el);
            const glm::vec3 h = hd >= 0 ? arm.boneWorldPosition(static_cast<std::size_t>(hd)) : glm::vec3(0.0f);
            std::printf("[arm] tick %d shldr(%.1f %.1f %.1f) twist(%.1f %.1f %.1f) elbow(%.1f %.1f %.1f) hand(%.3f %.3f %.3f)\n",
                        tick, s.x, s.y, s.z, t.x, t.y, t.z, e.x, e.y, e.z, h.x, h.y, h.z);
        }
        // IK_HARNESS_TRUNK_TRACE=1: per tick, the cursor, the grabbed joint, the hip, the left
        // knee, the pin counts and the spine's channels — where the trunk's motion comes from.
        static const bool kTrunkTrace = std::getenv("IK_HARNESS_TRUNK_TRACE") != nullptr;
        if (kTrunkTrace) {
            const auto eul = [&](const char* name) {
                const int b = resolveBone(arm, name);
                return b >= 0 ? arm.boneEuler(static_cast<std::size_t>(b)) : glm::vec3(0.0f);
            };
            const auto at = [&](const char* name) {
                const int b = resolveBone(arm, name);
                return b >= 0 ? arm.boneWorldPosition(static_cast<std::size_t>(b)) : glm::vec3(0.0f);
            };
            int live = 0;
            for (std::size_t p = 0; p < rig->pins().size(); ++p) {
                live += rig->pinIsLive(p) ? 1 : 0;
            }
            const glm::vec3 hp = at("hip"), kn = at("lShin"), rt = eul("hip");
            const glm::vec3 a1 = eul("abdomenLower"), a2 = eul("abdomen2"), c1 = eul("chest"), c2 = eul("chest_2");
            std::printf("[trunk] tick %3d cur(%.3f %.3f) grab(%.3f %.3f) hip(%.3f %.3f) rootX %.1f knee(%.3f %.3f)"
                        " pins %zu live %d | abdLo(%.0f %.0f %.0f) abd2(%.0f %.0f %.0f) chest(%.0f %.0f %.0f)"
                        " chest2(%.0f %.0f %.0f)\n",
                        tick, raw.y, raw.z, eff.y, eff.z, hp.y, hp.z, rt.x, kn.y, kn.z, rig->pins().size(), live,
                        a1.x, a1.y, a1.z, a2.x, a2.y, a2.z, c1.x, c1.y, c1.z, c2.x, c2.y, c2.z);
        }
        // Leg diagnostics per drag tick (IK_HARNESS_LEG_TRACE=1 prints them).
        static const bool kLegTrace = std::getenv("IK_HARNESS_LEG_TRACE") != nullptr;
        for (Leg& leg : legs) {
            r.kneeTwistMaxDeg = std::max(r.kneeTwistMaxDeg, kneeTwist(leg));
            r.kneeInwardMax = std::max(r.kneeInwardMax, kneeInward(leg));
            const glm::vec3 knee = arm.boneWorldPosition(static_cast<std::size_t>(leg.knee));
            if (kLegTrace) {
                // ... plus the pelvis bone's Euler and the hip's world position, so a knee
                // alternation can be traced to the pelvis tilt or to the leg itself.
                const int pelvisBone = resolveBone(arm, "pelvis");
                const glm::vec3 pe = pelvisBone >= 0 ? arm.boneEuler(static_cast<std::size_t>(pelvisBone))
                                                      : glm::vec3(0.0f);
                const glm::vec3 hipPos = hip >= 0 ? arm.boneWorldPosition(static_cast<std::size_t>(hip))
                                                  : glm::vec3(0.0f);
                // ... and the ankle: its height, its pin target's height (if pinned), and how far
                // the foot subtree's lowest riding part sits below its clearance (the toe dip).
                float ankleY = 0.0f, ankleTargetY = 0.0f, toeDip = 0.0f;
                bool anklePinned = false;
                if (leg.ankle >= 0) {
                    ankleY = arm.boneWorldPosition(static_cast<std::size_t>(leg.ankle)).y;
                    for (const IkEffector& pin : rig->pins()) {
                        if (pin.node == leg.ankle) {
                            anklePinned = true;
                            ankleTargetY = pin.target.y;
                        }
                    }
                    std::vector<int> stack{leg.ankle};
                    while (!stack.empty()) {
                        const int c = stack.back();
                        stack.pop_back();
                        for (std::size_t j = 0; j < n; ++j) {
                            if (bones[j].parent == c) {
                                stack.push_back(static_cast<int>(j));
                                if (bodyNode[j]) {
                                    toeDip = std::max(toeDip, rig->floorClearance(static_cast<int>(j)) -
                                                                  arm.boneWorldPosition(j).y);
                                }
                            }
                        }
                    }
                }
                std::printf("[leg] tick %d %s knee(%.4f %.4f %.4f) twist %.2f inward %.4f pelvis(%.2f %.2f %.2f) hip(%.4f %.4f %.4f) ankleY %.4f pin %s%.4f toeDip %.4f\n",
                            tick, arm.boneName(static_cast<std::size_t>(leg.knee)).c_str(), knee.x,
                            knee.y, knee.z, kneeTwist(leg), kneeInward(leg), pe.x, pe.y, pe.z, hipPos.x,
                            hipPos.y, hipPos.z, ankleY, anklePinned ? "" : "(none)", ankleTargetY, toeDip);
                // IK_HARNESS_EULER_TICKS="lThigh,lShin,...": those bones' channels EVERY tick (degrees).
                if (const char* list = std::getenv("IK_HARNESS_EULER_TICKS")) {
                    const std::vector<int> traced = resolveBoneList(arm, list); // (a trace: per tick is fine)
                    std::printf("[euler-tick] %d", tick);
                    for (const int idx : traced) {
                        const std::size_t b = static_cast<std::size_t>(idx);
                        const glm::vec3   e = arm.boneEuler(b);
                        std::printf(" %s(%.1f %.1f %.1f)", arm.boneName(b).c_str(), e.x, e.y, e.z);
                    }
                    std::printf("\n");
                }
            }
            const glm::vec3 kneeStep = knee - leg.prevKnee;
            const float kl = glm::length(kneeStep);
            r.kneeStepMax = std::max(r.kneeStepMax, static_cast<double>(kl));
            const float pl = glm::length(leg.prevKneeStep);
            if (pl > 1e-6f && kl > 1e-6f) {
                const float against = -glm::dot(kneeStep, leg.prevKneeStep) / pl;
                if (against > 0.0f) {
                    leg.osc += std::min(static_cast<double>(against), static_cast<double>(pl));
                }
            }
            leg.prevKneeStep = kneeStep;
            leg.prevKnee = knee;
        }
        r.footRotMaxDeg = std::max(r.footRotMaxDeg, footRotNow());
    }
    for (const Leg& leg : legs) {
        r.kneeOscMax = std::max(r.kneeOscMax, leg.osc);
    }
    // (Idle-joint tremble metrics come from runIdleMetrics — a second, identical run that keeps
    // the per-joint step history; the main loop stays a plain mirror of the viewport's tick.)
    r.grabEnd = arm.ikGrabPointWorld();
    r.cursorEnd = raw;
    r.pinsAtRelease = static_cast<int>(rig->pins().size());
    // IK_HARNESS_POSE_TRACE=1: the joints the drag rotated most (Euler degrees) and any pose
    // translation, at mouse-up — which part of the body served the gesture.
    static const bool kPoseTrace = std::getenv("IK_HARNESS_POSE_TRACE") != nullptr;
    if (kPoseTrace) {
        std::vector<std::pair<float, std::size_t>> byMagnitude;
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3& e = arm.boneEuler(i);
            const float mag = std::max({std::abs(e.x), std::abs(e.y), std::abs(e.z)});
            if (mag > 3.0f || glm::length(arm.boneTranslation(i)) > 0.005f) {
                byMagnitude.emplace_back(mag, i);
            }
        }
        std::sort(byMagnitude.begin(), byMagnitude.end(), std::greater<>());
        std::printf("[pose] at mouse-up, %zu joints past 3 deg:\n", byMagnitude.size());
        for (std::size_t k = 0; k < byMagnitude.size() && k < 24; ++k) {
            const std::size_t i = byMagnitude[k].second;
            const glm::vec3& e = arm.boneEuler(i);
            const glm::vec3& t = arm.boneTranslation(i);
            std::printf("[pose]   %-18s euler(%7.1f %7.1f %7.1f) trans(%.3f %.3f %.3f)\n",
                        arm.boneName(i).c_str(), e.x, e.y, e.z, t.x, t.y, t.z);
        }
    }
    r.gazeDragEndDeg = headAxisElevation(glm::vec3(0.0f, 0.0f, 1.0f));
    r.dragEndPos.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        r.dragEndPos[i] = arm.boneWorldPosition(i);
    }
    if (hip >= 0) {
        r.hipDropAtRelease = r.startPos[static_cast<std::size_t>(hip)].y -
                             r.dragEndPos[static_cast<std::size_t>(hip)].y;
    }
    r.restingMiss = glm::length(r.grabEnd - servable(raw));
    // Release settle.
    if (sc.release) {
        std::vector<glm::vec3> before(n);
        int guard = 0;
        while (true) {
            for (std::size_t i = 0; i < n; ++i) {
                before[i] = arm.boneWorldPosition(i);
            }
            const bool more = arm.settleIkTick();
            if (!more) {
                break;
            }
            ++r.settleTicks;
            {
                const double depth = volumeDepth();
                if (depth > r.volumePenetrationMax) {
                    r.volumePenetrationMax = depth;
                    r.volumePenetrationNode = volumeWorstNode;
                    r.volumePenetrationTick = -1 - r.settleTicks;
                }
                const double rd = riderDepth();
                if (rd > r.riderPenetrationMax) {
                    r.riderPenetrationMax = rd;
                    r.riderPenetrationNode = riderWorstNode;
                }
            }
            for (std::size_t i = 0; i < n; ++i) {
                r.settleMaxStep = std::max(r.settleMaxStep, static_cast<double>(glm::length(arm.boneWorldPosition(i) - before[i])));
                if (bodyNode[i] && !arm.footLyingExempt(static_cast<int>(i))) {
                    r.penetrationMax = std::max(
                        r.penetrationMax,
                        static_cast<double>(rig->floorClearance(static_cast<int>(i)) -
                                            arm.boneWorldPosition(i).y));
                }
            }
            for (std::size_t k = 0; k < userPinIdx.size(); ++k) {
                const std::size_t j = static_cast<std::size_t>(userPinIdx[k]);
                r.userPinStepMax = std::max(r.userPinStepMax, static_cast<double>(glm::length(arm.boneWorldPosition(j) - before[j])));
                r.userPinDistMax = std::max(r.userPinDistMax, static_cast<double>(glm::length(arm.boneWorldPosition(j) - userPinPos[k])));
            }
            if (++guard > 200) {
                break;
            }
            r.footRotMaxDeg = std::max(r.footRotMaxDeg, footRotNow());
            r.handsMinY = std::min(r.handsMinY, handsNow());
        }
        const std::vector<IkEffector>& pins = rig->pins();
        for (std::size_t p = 0; p < pins.size(); ++p) {
            if (rig->pinIsUser(p)) {
                continue;
            }
            const glm::vec3 pos(arm.poseGlobal(static_cast<std::size_t>(pins[p].node))[3]);
            r.settleWorstPinErr = std::max(r.settleWorstPinErr, static_cast<double>(glm::length(pos - pins[p].target)));
        }
    }
    r.holdDrift = glm::length(arm.ikGrabPointWorld() - r.grabEnd);
    r.grabEulerEnd = arm.boneEuler(static_cast<std::size_t>(r.grabIndex));
    {
        std::vector<std::pair<std::string, double>> deltas;
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 e = arm.boneEuler(i);
            double d = 0.0;
            for (int a = 0; a < 3; ++a) {
                d = std::max(d, static_cast<double>(std::abs(wrapDeg(e[a] - startEuler[i][a]))));
            }
            deltas.emplace_back(arm.boneName(i), d);
        }
        std::sort(deltas.begin(), deltas.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        deltas.resize(std::min<std::size_t>(deltas.size(), 6));
        r.topEulerDeltas = deltas;
        r.gazeEndDeg = headAxisElevation(glm::vec3(0.0f, 0.0f, 1.0f));
        r.headRollEndDeg = headAxisElevation(glm::vec3(1.0f, 0.0f, 0.0f));
        r.asymmetryEnd = asymmetryNow();
        r.endEuler.resize(n);
        r.boneNames.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            r.endEuler[i] = arm.boneEuler(i);
            r.boneNames[i] = arm.boneName(i);
        }
    }
    for (const Leg& leg : legs) {
        r.kneeTwistEndDeg = std::max(r.kneeTwistEndDeg, kneeTwist(leg));
        r.kneeInwardEnd = std::max(r.kneeInwardEnd, kneeInward(leg));
        r.kneeSplayEnd = std::max(r.kneeSplayEnd, kneeSplay(leg));
        r.kneeOverFootEnd = std::max(r.kneeOverFootEnd, kneeOverFoot(leg));
    }
    if (r.kneeInwardMax < -1e8) {
        r.kneeInwardMax = 0.0;
    }
    r.footRotEndDeg = footRotNow();
    arm.endIkDrag();
    // IK_HARNESS_EULER_TRACE="lFoot,lToe,...": those bones' Euler pose (degrees) at the drag's
    // start and at its end, with their authored limits — which joint is stuck against what.
    if (const char* list = std::getenv("IK_HARNESS_EULER_TRACE")) {
        for (const int idx : resolveBoneList(arm, list)) {
            const std::size_t b = static_cast<std::size_t>(idx);
            const glm::vec3 e = arm.boneEuler(b);
            const glm::vec3 w = arm.boneWorldPosition(b);
            std::printf("         [euler] %-16s start(%7.2f %7.2f %7.2f) end(%7.2f %7.2f %7.2f) min(%6.1f %6.1f %6.1f) max(%6.1f %6.1f %6.1f) at(%.3f %.3f %.3f)\n",
                        arm.boneName(b).c_str(), startEuler[b].x, startEuler[b].y, startEuler[b].z, e.x, e.y, e.z,
                        bones[b].rotationMin.x, bones[b].rotationMin.y, bones[b].rotationMin.z,
                        bones[b].rotationMax.x, bones[b].rotationMax.y, bones[b].rotationMax.z, w.x, w.y, w.z);
        }
    }
    r.endPos.resize(n);
    r.endPalm.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        r.endPos[i] = arm.boneWorldPosition(i);
        r.endPalm[i] = arm.handPalmNormal(static_cast<int>(i));
    }
    if (const IkRig* rigEnd = arm.ikRig(); rigEnd != nullptr && rigEnd->masses().size() == n) {
        std::vector<int> parents(n);
        for (std::size_t i = 0; i < n; ++i) {
            parents[i] = bones[i].parent;
        }
        r.comStart = BalanceController::centerOfMass(r.startPos, parents, rigEnd->masses());
        r.comEnd = BalanceController::centerOfMass(r.endPos, parents, rigEnd->masses());
    }
    r.volumePenetrationEnd = volumeDepth();
    r.riderPenetrationEnd = riderDepth();
    if (verbose && r.riderPenetrationMax > 0.001) {
        std::printf("  rider penetration: worst %.1f mm at %s, %.1f mm at the end\n", r.riderPenetrationMax * 1000.0,
                    r.riderPenetrationNode >= 0 ? arm.boneName(static_cast<std::size_t>(r.riderPenetrationNode)).c_str() : "?",
                    r.riderPenetrationEnd * 1000.0);
    }
    if (verbose && r.volumePenetrationMax > 0.001) {
        std::printf("  body-volume penetration: worst %.1f mm at %s (tick %d)\n", r.volumePenetrationMax * 1000.0,
                    r.volumePenetrationNode >= 0 ? arm.boneName(static_cast<std::size_t>(r.volumePenetrationNode)).c_str() : "?",
                    r.volumePenetrationTick);
    }
    for (std::size_t c = 0; c < r.contactPins.size(); ++c) {
        r.contactDriftMax = std::max(r.contactDriftMax, static_cast<double>(glm::length(r.endPos[static_cast<std::size_t>(r.contactPins[c])] - contactStart[c])));
    }
    if (hip >= 0) {
        r.hipDisp = glm::length(r.endPos[static_cast<std::size_t>(hip)] - r.startPos[static_cast<std::size_t>(hip)]);
        r.hipDispY = r.endPos[static_cast<std::size_t>(hip)].y - r.startPos[static_cast<std::size_t>(hip)].y;
    }
    if (head >= 0) {
        r.headDisp = glm::length(r.endPos[static_cast<std::size_t>(head)] - r.startPos[static_cast<std::size_t>(head)]);
    }
    if (chest >= 0) {
        r.chestDisp = glm::length(r.endPos[static_cast<std::size_t>(chest)] - r.startPos[static_cast<std::size_t>(chest)]);
    }
    for (const int f : feet) {
        r.feetMinY = std::min(r.feetMinY, static_cast<double>(r.endPos[static_cast<std::size_t>(f)].y));
    }
    r.handsEndY = handsNow();
    for (const Leg& leg : legs) {
        r.kneesEndY = std::min(r.kneesEndY, static_cast<double>(r.endPos[static_cast<std::size_t>(leg.knee)].y));
    }
    r.ok = true;
    if (verbose) {
        for (const PhaseStats& ph : r.phases) {
            std::printf("         %-10s ticks %3d moved %3d | lag mean %6.1f max %6.1f mm | osc %6.1f mm | jump %5.1f mm | eff dv +%5.1f -%5.1f (cursor +%5.1f -%5.1f) mm/tick\n",
                        ph.label.c_str(), ph.ticks, ph.movedTicks, ph.lagMean() * 1000.0, ph.lagMax * 1000.0,
                        ph.effOsc * 1000.0, ph.maxJump * 1000.0, ph.effSpeedUpMax * 1000.0,
                        ph.effSpeedDownMax * 1000.0, ph.cursorSpeedUpMax * 1000.0, ph.cursorSpeedDownMax * 1000.0);
        }
        std::printf("         settle %d ticks, max step %.1f mm, worst pin %.1f mm; steps %d; suspended %d\n",
                    r.settleTicks, r.settleMaxStep * 1000.0, r.settleWorstPinErr * 1000.0, r.stepsTaken,
                    r.suspended ? 1 : 0);
    }
    return r;
}

IdleMetrics runIdleMetrics(const std::vector<ArmatureBone>& baseBones, const Scenario& sc) {
    IdleMetrics m;
    Armature arm;
    arm.build(sc.scale == 1.0f ? baseBones : scaledBones(baseBones, sc.scale));
    applyBodyMesh(arm, sc.scale);
    for (const auto& [bone, euler] : sc.prePose) {
        arm.setBoneRotation(resolveBoneName(arm, bone), euler);
    }
    for (const std::string& pin : sc.userPins) {
        const int idx = resolveBone(arm, pin);
        if (idx >= 0) {
            arm.setSelectedBone(idx);
            arm.togglePinSelectedBone();
        }
    }
    const int grab = resolveBone(arm, sc.grab);
    if (grab >= 0) {
        arm.setSelectedBone(grab);
        if (arm.selectedBone() != grab) { // (a face-rig bone: the head, grabbed where the named bone is)
            arm.setIkGrabPoint(grab, arm.boneWorldPosition(static_cast<std::size_t>(grab)));
        }
    }
    if (grab < 0 || !arm.beginIkDrag()) {
        return m;
    }
    std::vector<int> idle;
    for (const std::string& name : sc.idleJoints) {
        const int idx = resolveBone(arm, name);
        if (idx >= 0) {
            idle.push_back(idx);
        }
    }
    const glm::vec3 start = arm.boneWorldPosition(static_cast<std::size_t>(grab));
    std::vector<glm::vec3> prevPos(idle.size()), prevStep(idle.size(), glm::vec3(0.0f));
    std::vector<double> osc(idle.size(), 0.0);
    for (std::size_t k = 0; k < idle.size(); ++k) {
        prevPos[k] = arm.boneWorldPosition(static_cast<std::size_t>(idle[k]));
    }
    IkCursorFilter filter;
    filter.seed(start);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    glm::vec3 raw = start;
    int tick = 0;
    while (true) {
        ++tick;
        bool done = true;
        for (std::size_t s = 1; s < sc.path.size(); ++s) {
            if (tick <= sc.path[s].tick) {
                const float f = static_cast<float>(tick - sc.path[s - 1].tick) /
                                static_cast<float>(sc.path[s].tick - sc.path[s - 1].tick);
                raw = start + glm::mix(sc.path[s - 1].offset, sc.path[s].offset, f);
                done = false;
                break;
            }
        }
        if (done) {
            break;
        }
        if (sc.cursorNoise > 0.0f) {
            raw += glm::vec3(noise(rng), noise(rng), noise(rng)) * sc.cursorNoise;
        }
        arm.dragIkTo(filter.update(raw));
        for (std::size_t k = 0; k < idle.size(); ++k) {
            const glm::vec3 p = arm.boneWorldPosition(static_cast<std::size_t>(idle[k]));
            const glm::vec3 step = p - prevPos[k];
            const float len = glm::length(step);
            m.stepMax = std::max(m.stepMax, static_cast<double>(len));
            const float pl = glm::length(prevStep[k]);
            if (pl > 1e-6f && len > 1e-6f) {
                const float against = -glm::dot(step, prevStep[k]) / pl;
                if (against > 0.0f) {
                    osc[k] += std::min(static_cast<double>(against), static_cast<double>(pl));
                }
            }
            prevStep[k] = step;
            prevPos[k] = p;
        }
    }
    for (std::size_t k = 0; k < idle.size(); ++k) {
        m.oscMax = std::max(m.oscMax, osc[k]);
    }
    arm.endIkDrag();
    return m;
}

std::vector<Waypoint> pullPath(const glm::vec3& offset, int moveTicks, int holdTicks) {
    return {{0, glm::vec3(0.0f), "start"}, {moveTicks, offset, "move"}, {moveTicks + holdTicks, offset, "hold"}};
}

} // namespace ikharness
