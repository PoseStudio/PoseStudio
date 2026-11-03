/**
 * @file rigconformer.cpp
 * @brief The rig standard's conformer (see rigconformer.h): frames, orders and limits by role.
 */

#include "rigconformer.h"

#include "armature.h"
#include "ikmath.h"
#include "ikrig.h"
#include "rigroles.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_set>

namespace pose {

namespace {

struct Range {
    float mn = 0.0f;
    float mx = 0.0f;
    bool  limited = true;
};
Range lim(float a, float b) { return {a, b, true}; }
Range locked() { return {0.0f, 0.0f, true}; }
Range full() { return {-180.0f, 180.0f, true}; } // (the hips': limited but whole)
Range unlimited() { return {0.0f, 0.0f, false}; }

/// A role's channels on the LEFT side (x, y, z) with its rotation order (the format's: the first
/// letter innermost, the bone's length axis). The right side mirrors y and z.
struct RoleLimits {
    const char* order = "XYZ";
    Range       x, y, z;
};

/// The reference generation's spine links, bottom to top, and its two neck links. A chain of N
/// links maps onto them by PLACE — each link's limits interpolated at its relative place along the
/// reference chain — and RENORMALIZED per channel so that the N links together carry the reference
/// chain's TOTAL range (the sum of its links', each side separately): a three-link spine that took
/// links 1, 3 and 4 of the four had a third less flexion than the reference's trunk, and the bow,
/// the crouching lean and the Ctrl drag of the lower spine all rested short by that (2026-11-03).
/// Four links reproduce the reference exactly; one link takes the whole chain's range.
const RoleLimits kSpine[4] = {
    {"YZX", lim(-20, 35), lim(-15, 15), lim(-15, 15)},
    {"YZX", lim(-25, 40), lim(-20, 20), lim(-24, 24)},
    {"YZX", lim(-25, 35), lim(-12, 12), lim(-20, 20)},
    {"YZX", lim(-15, 15), lim(-10, 10), lim(-10, 10)},
};
const RoleLimits kNeck[2] = {
    {"YZX", lim(-25, 40), lim(-22, 22), lim(-40, 40)},
    {"YZX", lim(-27, 12), lim(-22, 22), lim(-10, 10)},
};
RoleLimits mapChain(const RoleLimits* ref, int refCount, int index, int links) {
    links = std::max(1, links);
    index = std::max(1, std::min(links, index));
    const auto at = [&](double place) { // the reference's limits interpolated at a place in [0, refCount - 1]
        const int    lo = std::max(0, std::min(refCount - 1, static_cast<int>(std::floor(place))));
        const int    hi = std::min(refCount - 1, lo + 1);
        const double f = place - lo;
        RoleLimits   r = ref[lo];
        Range*       mine[3] = {&r.x, &r.y, &r.z};
        const Range* a[3] = {&ref[lo].x, &ref[lo].y, &ref[lo].z};
        const Range* b[3] = {&ref[hi].x, &ref[hi].y, &ref[hi].z};
        for (int c = 0; c < 3; ++c) {
            mine[c]->mn = static_cast<float>((1.0 - f) * a[c]->mn + f * b[c]->mn);
            mine[c]->mx = static_cast<float>((1.0 - f) * a[c]->mx + f * b[c]->mx);
        }
        return r;
    };
    const auto placeOf = [&](int i) { return links == 1 ? 0.5 * (refCount - 1) : (i - 1.0) * (refCount - 1.0) / (links - 1.0); };
    // The chain's sums against the reference's, per channel and side.
    double sumMn[3] = {0, 0, 0}, sumMx[3] = {0, 0, 0}, refMn[3] = {0, 0, 0}, refMx[3] = {0, 0, 0};
    for (int i = 1; i <= links; ++i) {
        const RoleLimits r = at(placeOf(i));
        const Range*     ch[3] = {&r.x, &r.y, &r.z};
        for (int c = 0; c < 3; ++c) {
            sumMn[c] += ch[c]->mn;
            sumMx[c] += ch[c]->mx;
        }
    }
    for (int k = 0; k < refCount; ++k) {
        const Range* ch[3] = {&ref[k].x, &ref[k].y, &ref[k].z};
        for (int c = 0; c < 3; ++c) {
            refMn[c] += ch[c]->mn;
            refMx[c] += ch[c]->mx;
        }
    }
    RoleLimits out = at(placeOf(index));
    Range*     ch[3] = {&out.x, &out.y, &out.z};
    for (int c = 0; c < 3; ++c) {
        ch[c]->mn = static_cast<float>(std::abs(sumMn[c]) > 1.0e-6 ? ch[c]->mn * refMn[c] / sumMn[c] : ch[c]->mn);
        ch[c]->mx = static_cast<float>(std::abs(sumMx[c]) > 1.0e-6 ? ch[c]->mx * refMx[c] / sumMx[c] : ch[c]->mx);
    }
    return out;
}

RoleLimits limitsFor(const RigRoleTag& t, int spineLinks, int neckLinks, bool segmentHasTwistHelper, bool footHasMetatarsal) {
    switch (t.role) {
    case RigRole::Hips: return {"YZX", full(), full(), full()};
    case RigRole::Pelvis: return {"YZX", lim(-25, 25), lim(-15, 15), lim(-10, 10)};
    case RigRole::Spine: return mapChain(kSpine, 4, t.index, spineLinks);
    case RigRole::Neck: return mapChain(kNeck, 2, t.index, neckLinks);
    case RigRole::Head: return {"YZX", lim(-30, 25), lim(-22, 22), lim(-20, 20)};
    case RigRole::Jaw: return {"XYZ", lim(-10, 25), lim(-12, 12), locked()};
    case RigRole::Eye: return {"XYZ", lim(-30, 30), lim(-40, 40), lim(-30, 30)};
    case RigRole::Face: return {"XYZ", lim(-40, 40), lim(-40, 40), lim(-40, 40)};
    case RigRole::Pectoral: return {"ZYX", lim(-10, 10), lim(-10, 10), lim(-15, 15)};
    case RigRole::Collar: return {"XYZ", lim(-30, 30), lim(-26, 17), lim(-10, 55)};
    case RigRole::UpperArm: return {"XYZ", segmentHasTwistHelper ? locked() : lim(-95, 80), lim(-110, 40), lim(-40, 90)};
    case RigRole::UpperArmTwist: return {"XYZ", lim(-95, 80), locked(), locked()};
    case RigRole::Forearm: return {"XZY", segmentHasTwistHelper ? locked() : lim(-90, 80), lim(-135, 20), locked()};
    case RigRole::ForearmTwist: return {"XZY", lim(-90, 80), locked(), locked()};
    case RigRole::Hand: return {"XYZ", lim(-10, 10), lim(-28, 30), lim(-70, 80)};
    case RigRole::Carpal: return {"XYZ", locked(), lim(-4, 4), lim(-4, 4)};
    case RigRole::Finger:
        if (t.index == 1) { // the thumb
            if (t.joint <= 1) return {"XZY", lim(-15, 36), lim(-26, 40), lim(-20, 20)};
            if (t.joint == 2) return {"XZY", locked(), lim(-15, 65), locked()};
            return {"XZY", locked(), lim(-20, 90), locked()};
        }
        if (t.joint <= 1) return {"XYZ", lim(-5, 5), lim(-15, 15), lim(-90, 50)};
        if (t.joint == 2) return {"XYZ", locked(), locked(), lim(-105, 12)};
        return {"XYZ", locked(), locked(), lim(-90, 20)};
    case RigRole::Thigh: return {"YZX", lim(-115, 35), segmentHasTwistHelper ? locked() : lim(-75, 75), lim(-26, 85)};
    case RigRole::ThighTwist: return {"YZX", locked(), lim(-75, 75), locked()};
    case RigRole::Shin: return {"YZX", lim(-11, 155), lim(-25, 10), lim(-5, 5)};
    case RigRole::ShinTwist: return {"YZX", locked(), lim(-30, 30), locked()};
    // THE FOOT'S PITCH IS TWO JOINTS' ON THE REFERENCE: the ankle (-45..65) and a metatarsal joint
    // between it and the ball (-20..25), ~70 degrees of dorsiflexion in all — which is what a kneel
    // spends: the shin leans forward over a planted, flat foot until the knee is nearly down. A rig
    // with ONE foot bone takes the metatarsal's range at the ankle, or its kneel stops with the
    // knees 10cm up (2026-11-03: the character-creation rig, whose share bone beside the ball is
    // a locked helper; widened, its knees plant).
    case RigRole::Foot: return footHasMetatarsal ? RoleLimits{"ZYX", lim(-45, 65), lim(-25, 10), lim(-35, 15)}
                                                 : RoleLimits{"ZYX", lim(-70, 85), lim(-25, 10), lim(-35, 15)};
    case RigRole::Heel: return {"ZYX", locked(), locked(), locked()};
    case RigRole::Metatarsal: return {"ZYX", lim(-20, 25), locked(), lim(-20, 20)};
    case RigRole::Ball: return {"ZYX", lim(-60, 40), locked(), lim(-20, 20)};
    case RigRole::Toe:
        if (t.joint <= 1) return {"ZYX", lim(-65, 45), lim(-15, 10), lim(-5, 5)};
        return {"ZYX", lim(-30, 60), locked(), locked()};
    case RigRole::Helper: return {"XYZ", locked(), locked(), locked()};
    case RigRole::Other:
    case RigRole::None:
    case RigRole::Count: break;
    }
    return {"XYZ", unlimited(), unlimited(), unlimited()};
}

Range mirrored(const Range& r) {
    return {-r.mx, -r.mn, r.limited};
}

/// A role whose frame is built along its bone (the letter), or identity (0).
char lengthLetter(RigRole role) {
    switch (role) {
    case RigRole::Spine:
    case RigRole::Neck:
    case RigRole::Head:
    case RigRole::Thigh:
    case RigRole::ThighTwist:
    case RigRole::Shin:
    case RigRole::ShinTwist: return 'Y';
    case RigRole::Collar:
    case RigRole::UpperArm:
    case RigRole::UpperArmTwist:
    case RigRole::Forearm:
    case RigRole::ForearmTwist:
    case RigRole::Hand:
    case RigRole::Carpal:
    case RigRole::Finger: return 'X';
    case RigRole::Foot:
    case RigRole::Heel:
    case RigRole::Metatarsal:
    case RigRole::Ball:
    case RigRole::Toe:
    case RigRole::Pectoral: return 'Z';
    default: return 0;
    }
}

/// The least rotation that takes the world axis @p letter onto @p dir, or onto -dir where that is
/// the nearer (the reference generation's frames: the left arm's X runs out along the arm, the
/// right arm's X back up it, a thigh's Y up along it) — the orientation of a bone that lies along @p dir.
glm::mat3 frameAlong(char letter, glm::vec3 dir) {
    const float len = glm::length(dir);
    if (len < 1.0e-6f) {
        return glm::mat3(1.0f);
    }
    dir /= len;
    const glm::vec3 axis = letter == 'X' ? glm::vec3(1.0f, 0.0f, 0.0f) : letter == 'Y' ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    if (glm::dot(axis, dir) < 0.0f) {
        dir = -dir;
    }
    const float     c = std::max(-1.0f, std::min(1.0f, glm::dot(axis, dir)));
    const glm::vec3 cr = glm::cross(axis, dir);
    const float     s = glm::length(cr);
    if (s < 1.0e-6f) {
        return glm::mat3(1.0f);
    }
    return glm::mat3(glm::rotate(glm::mat4(1.0f), std::atan2(s, c), cr / s));
}

bool isTwistRole(RigRole r) {
    return r == RigRole::UpperArmTwist || r == RigRole::ForearmTwist || r == RigRole::ThighTwist || r == RigRole::ShinTwist;
}

/// The orientation Euler (degrees) the Armature composes as Rz * Ry * Rx (Armature::build: "ZYX").
glm::vec3 orientationEuler(const glm::mat3& frame) {
    return eulerFromMatrix(frame, "ZYX");
}

std::string lowerCopy(const std::string& name) {
    std::string lower;
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

bool nameSaysTwist(const std::string& name) {
    const std::string lower = lowerCopy(name);
    return lower.find("twist") != std::string::npos || lower.find("roll") != std::string::npos;
}

/// The limb SEGMENT a bone's name says it belongs to (0 none; 1 thigh, 2 shin, 3 upper arm, 4
/// forearm) — the words every rigging convention uses for them. A bone that repeats its PARENT's
/// segment word is the second piece of one segment (a thigh cut in two, "thigh1" and "thigh2"),
/// which the posing engine knows as a twist helper: it turns about the bone and never bends.
int segmentWord(const std::string& name) {
    const std::string s = lowerCopy(name);
    const auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
    if (has("thigh") || has("upleg") || has("upperleg") || has("up_leg") || has("upper_leg")) return 1;
    if (has("shin") || has("calf") || has("lowerleg") || has("lower_leg") || has("lowleg") || has("leg")) return 2;
    if (has("forearm") || has("fore_arm") || has("lowerarm") || has("lower_arm") || has("lowarm")) return 4;
    if (has("upperarm") || has("upper_arm") || has("uparm") || has("shldr") || has("arm")) return 3;
    return 0;
}

} // namespace

RigConformReport conformRigToStandard(ModelData& data) {
    RigConformReport report;
    const int        n = static_cast<int>(data.bones.size());
    report.bones = n;
    if (n == 0) {
        return report;
    }
    const auto idx = [](int b) { return static_cast<std::size_t>(b); };
    // The hierarchy, the positions, each bone's subtree size and MAIN child (the largest subtree).
    std::vector<std::vector<int>> children(idx(n));
    std::vector<glm::vec3>        pos(idx(n));
    std::vector<int>              size(idx(n), 1);
    for (int i = 0; i < n; ++i) {
        pos[idx(i)] = glm::vec3(data.bones[idx(i)].bindGlobal[3]);
        const int p = data.bones[idx(i)].parent;
        if (p >= 0 && p < n) {
            children[idx(p)].push_back(i);
        }
    }
    for (int i = n; i-- > 0;) {
        const int p = data.bones[idx(i)].parent;
        if (p >= 0 && p < n && p < i) {
            size[idx(p)] += size[idx(i)];
        }
    }
    const auto mainChild = [&](int b) {
        int best = -1;
        for (const int c : children[idx(b)]) {
            if (best < 0 || size[idx(c)] > size[idx(best)]) {
                best = c;
            }
        }
        return best;
    };
    // The direction a bone lies along, by STRUCTURE alone (the provisional pass, before the roles):
    // to its main child; for a leaf, to the tip its file gave it (ModelBone::tip), else from its
    // parent; for a leaf lying AT its parent's joint (a game export's twist bone, zero length), its
    // parent's direction. A leaf NAMED twist lies along its segment whatever its tip says
    // (2026-11-03): a biped export hangs an end marker off each twist bone that sits 12cm
    // BEHIND it, square to the arm, and read toward it the provisional frame's length axis stood
    // across the segment — no twist bone (the free channel must lie along the bone), so every
    // twist helper of the rig was "other": unlimited, undriven, a joint of its own.
    const std::function<glm::vec3(int)> directionOf = [&](int b) -> glm::vec3 {
        const int c = mainChild(b);
        if (c >= 0) {
            return pos[idx(c)] - pos[idx(b)];
        }
        if (data.bones[idx(b)].hasTip && !nameSaysTwist(data.bones[idx(b)].name)) {
            return data.bones[idx(b)].tip - pos[idx(b)];
        }
        const int p = data.bones[idx(b)].parent;
        if (p < 0) {
            return glm::vec3(0.0f);
        }
        const glm::vec3 own = pos[idx(b)] - pos[idx(p)];
        return glm::length(own) > 1.0e-4f ? own : directionOf(p);
    };
    const auto localTranslation = [&](int b) {
        const int p = data.bones[idx(b)].parent;
        return p >= 0 ? pos[idx(b)] - pos[idx(p)] : pos[idx(b)];
    };

    // ---- 1. THE PROVISIONAL ARMATURE: every frame along its bone (Y), the twist helpers locked
    // across so the rig reads them as such, no other limits — enough structure for the roles.
    // The skin each bone carries (a leaf no skin follows is an end marker, not a joint).
    std::vector<int> skinCount(idx(n), 0);
    std::vector<float> weightSum(idx(n), 0.0f); // (every influence, dominant or not: the trace's reading of a bone that drives little)
    for (const MeshData& mesh : data.meshes) {
        for (const Vertex& v : mesh.vertices) {
            int best = 0;
            for (int j = 1; j < 4; ++j) {
                if (v.weights[j] > v.weights[best]) {
                    best = j;
                }
            }
            if (v.weights[best] > 0.0f && v.joints[best] < static_cast<unsigned>(n)) {
                ++skinCount[idx(static_cast<int>(v.joints[best]))];
            for (int q = 0; q < 4; ++q) {
                if (v.weights[q] > 0.0f && v.joints[q] < static_cast<unsigned>(n)) {
                    weightSum[idx(static_cast<int>(v.joints[q]))] += v.weights[q];
                }
            }
            }
        }
    }
    // A bone NAMED twist is a helper only when nothing hangs below it but other twists and
    // skinless leaves: a neck named "NeckTwist" carries the head, and is a neck.
    const std::function<bool(int)> onlyTwistsBelow = [&](int b) -> bool {
        for (const int c : children[idx(b)]) {
            const bool skinlessLeaf = children[idx(c)].empty() && skinCount[idx(c)] == 0;
            if (skinlessLeaf) {
                continue;
            }
            if (!nameSaysTwist(data.bones[idx(c)].name) || !onlyTwistsBelow(c)) {
                return false;
            }
        }
        return true;
    };
    std::vector<ArmatureBone> provisional(idx(n));
    std::vector<char>         twistNamed(idx(n), 0);
    std::vector<char>         segmentRepeated(idx(n), 0);
    for (int i = 0; i < n; ++i) {
        ArmatureBone& b = provisional[idx(i)];
        b.name = data.bones[idx(i)].name;
        b.parent = data.bones[idx(i)].parent;
        b.localBindTranslation = localTranslation(i);
        b.orientation = orientationEuler(frameAlong('Y', directionOf(i)));
        b.rotationOrder = "YZX";
        const bool repeatsSegment = b.parent >= 0 && segmentWord(b.name) != 0 &&
                                    segmentWord(b.name) == segmentWord(data.bones[idx(b.parent)].name);
        const bool twistByName = nameSaysTwist(b.name) && onlyTwistsBelow(i);
        twistNamed[idx(i)] = twistByName ? 1 : 0;
        segmentRepeated[idx(i)] = repeatsSegment ? 1 : 0;
        if ((twistByName || repeatsSegment) && b.parent >= 0) {
            b.rotationMin = glm::vec3(0.0f);
            b.rotationMax = glm::vec3(0.0f);
            b.rotationLimited = glm::bvec3(true, false, true); // (x and z locked; y, along the bone, free)
        }
    }
    std::vector<BodyMeshPoint> sample;
    for (const MeshData& mesh : data.meshes) {
        for (const Vertex& v : mesh.vertices) {
            int best = 0;
            for (int j = 1; j < 4; ++j) {
                if (v.weights[j] > v.weights[best]) {
                    best = j;
                }
            }
            if (v.weights[best] > 0.0f && v.joints[best] < static_cast<unsigned>(n)) {
                sample.push_back({v.pos, static_cast<int>(v.joints[best])});
            }
        }
    }
    Armature arm;
    arm.build(provisional);
    if (!sample.empty()) {
        arm.setBodyMesh(std::move(sample));
    }
    arm.prepareIkRig();
    const IkRig* rig = arm.ikRig();
    if (rig == nullptr || !rig->roles().built()) {
        return report;
    }
    const RigRoles& roles = rig->roles();
    static const bool kConformTrace = std::getenv("POSESTUDIO_CONFORM_TRACE") != nullptr;
    if (kConformTrace) {
        // What the provisional pass made of each bone, before the roles: a twist by its name, a
        // repeated segment word, the locks, the armature's own twist-bone reading, the rig's helper.
        for (int i = 0; i < n; ++i) {
            const ArmatureBone& b = provisional[idx(i)];
            std::fprintf(stderr, "[conform] %3d %-28s twistName=%d repeat=%d locked=%d%d%d skin=%d weight=%.1f helper=%d dir=(%.3f %.3f %.3f)\n", i,
                         b.name.c_str(), twistNamed[idx(i)], segmentRepeated[idx(i)], b.rotationLimited.x ? 1 : 0, b.rotationLimited.y ? 1 : 0,
                         b.rotationLimited.z ? 1 : 0, skinCount[idx(i)], weightSum[idx(i)], rig->helperNode(i) ? 1 : 0, directionOf(i).x,
                         directionOf(i).y, directionOf(i).z);
        }
    }
    report.complete = roles.complete();
    report.missing = roles.missing();
    report.roles = roles.report();
    report.spineLinks = roles.spineLinks();
    report.neckLinks = roles.neckLinks();

    // The direction a bone lies along BY ROLE — what its frame is built on. A chain bone points at
    // its next joint through whatever helpers lie between (the thigh at the shin, the spine link at
    // the next link, the neck at the head); the hand at the mean of its fingers' bases and the ball
    // at the mean of its toes' (at the first child listed the ball's frame stood 66/82/59 degrees
    // off, pointing at the pinky on the left and the big toe on the right: the kneel's toes could not
    // bend back, and the two sides' dials mirrored 25mm apart); a leaf at the tip its file gave it;
    // a single-bone toe, which has no tip, along its ball. Everything else as the structure says.
    const auto meanOffsetTo = [&](int b, const std::vector<int>& targets) {
        glm::vec3 sum(0.0f);
        int       count = 0;
        for (const int t : targets) {
            sum += pos[idx(t)] - pos[idx(b)];
            ++count;
        }
        return count > 0 ? sum / static_cast<float>(count) : glm::vec3(0.0f);
    };
    const auto directionFor = [&](int b) -> glm::vec3 {
        const RigRoleTag& t = roles.tag(b);
        const auto toRole = [&](RigRole r, RigSide side, int index = 0, int joint = 0) -> glm::vec3 {
            const int target = roles.find(r, side, index, joint);
            return target >= 0 && target != b ? pos[idx(target)] - pos[idx(b)] : glm::vec3(0.0f);
        };
        glm::vec3 d(0.0f);
        switch (t.role) {
        // THE TRUNK IS WORLD-ALIGNED: the reference generation authors identity frames on its hips,
        // pelvis, every spine link, the neck and the head whatever the spine's curve (the engine's
        // trunk rules — the hinge, the coupling, the righting — read flexion about the world's
        // lateral axis), so a trunk bone's frame is Y up, X lateral, Z forward. A frame along a
        // leaning neck or chest top turned its "flexion" into a mix of flexion and roll.
        case RigRole::Hips:
        case RigRole::Pelvis:
        case RigRole::Spine:
        case RigRole::Neck:
        case RigRole::Head: d = glm::vec3(0.0f, 1.0f, 0.0f); break;
        case RigRole::Collar: d = toRole(RigRole::UpperArm, t.side); break;
        case RigRole::UpperArm: d = toRole(RigRole::Forearm, t.side); break;
        case RigRole::Forearm: d = toRole(RigRole::Hand, t.side); break;
        case RigRole::Thigh: d = toRole(RigRole::Shin, t.side); break;
        case RigRole::Shin: d = toRole(RigRole::Foot, t.side); break;
        case RigRole::Foot:
            d = toRole(RigRole::Ball, t.side);
            if (glm::length(d) < 1.0e-5f) d = toRole(RigRole::Metatarsal, t.side, 1);
            break;
        case RigRole::Hand: {
            std::vector<int> bases;
            for (const int c : children[idx(b)]) {
                const RigRole cr = roles.role(c);
                if (cr == RigRole::Finger || cr == RigRole::Carpal) {
                    bases.push_back(c);
                }
            }
            d = meanOffsetTo(b, bases);
            break;
        }
        case RigRole::Ball: {
            std::vector<int> toes;
            for (const int c : children[idx(b)]) {
                if (roles.role(c) == RigRole::Toe) {
                    toes.push_back(c);
                }
            }
            d = meanOffsetTo(b, toes);
            break;
        }
        case RigRole::Toe:
            if (children[idx(b)].empty() && !data.bones[idx(b)].hasTip) {
                const int ball = roles.find(RigRole::Ball, t.side);
                d = ball >= 0 ? directionOf(ball) : glm::vec3(0.0f); // (a single-bone toe lies along its foot)
                if (ball >= 0) {
                    std::vector<int> toes;
                    for (const int c : children[idx(ball)]) {
                        if (roles.role(c) == RigRole::Toe) {
                            toes.push_back(c);
                        }
                    }
                    const glm::vec3 fan = meanOffsetTo(ball, toes);
                    if (glm::length(fan) > 1.0e-5f) {
                        d = fan;
                    }
                }
            }
            break;
        default: break;
        }
        return glm::length(d) > 1.0e-5f ? d : directionOf(b);
    };

    // ---- 2. THE STANDARD'S FRAMES, ORDERS AND LIMITS, by role.
    // A segment's own twist channel is LOCKED only when a twist helper of the segment is ON THE
    // CHAIN — between the segment and its next joint, carrying it, as the reference generation's
    // thigh twist carries the shin: there the helper's channel IS the segment's twist. A helper
    // hanging as a LEAF beside the chain (a game rig's TwistO1/02 under the thigh, next to the
    // shin; the newest reference generation's) carries skin alone, and the segment keeps its own
    // twist — locked on top of leaf helpers, a leg had no twist at all: a knee swung out 2.4cm of
    // the 8 asked, a hand could not turn its palm.
    //
    // THE THUMB'S ROLL IS THE PALM'S (2026-11-03). A finger's bend axis lies in the palm's plane,
    // and the least rotation that takes X onto the finger finds it there on the binds seen (an
    // A-pose with the palms in, a T-pose with the palms down: the reference generation's own
    // finger frames read within 5 degrees of it); a THUMB's bend axis is not where any rotation
    // about a world axis puts it — the thumb lies out of the palm's plane and flexes into it. The
    // reference generation authors the thumb's bend axis as the BACK-OF-HAND NORMAL rolled
    // kThumbRollDeg about the thumb's length (the same on every joint of the thumb, and the same
    // on four generations to a degree; the least rotation stood 27-54 off). The normal is the
    // index and little fingers' bases about the wrist, its sense by side; the thumb's X runs along
    // the bone on the left and back up it on the right (the reference's convention, which the
    // mirrored limits need: so the right thumb's y and z read as the left's mirrored).
    constexpr float kThumbRollDeg = -28.5f; // (the angle about the frame's X taking the normal onto the authored bend axis)
    const auto      thumbFrame = [&](int b, const RigRoleTag& t, glm::mat3& out) -> bool {
        const int hand = roles.find(RigRole::Hand, t.side);
        int       first = -1;
        int       last = -1;
        for (int digit = 2; digit <= 5 && first < 0; ++digit) {
            first = roles.find(RigRole::Finger, t.side, digit, 1);
        }
        for (int digit = 5; digit >= 2 && last < 0; --digit) {
            last = roles.find(RigRole::Finger, t.side, digit, 1);
        }
        if (hand < 0 || first < 0 || last < 0 || first == last) {
            return false;
        }
        glm::vec3 normal = glm::cross(pos[idx(first)] - pos[idx(hand)], pos[idx(last)] - pos[idx(hand)]);
        if (t.side == RigSide::Right) {
            normal = -normal;
        }
        const glm::vec3 dir = directionFor(b);
        if (glm::length(normal) < 1.0e-8f || glm::length(dir) < 1.0e-6f) {
            return false;
        }
        const glm::vec3 x = glm::normalize(t.side == RigSide::Right ? -dir : dir);
        glm::vec3       y = normal - glm::dot(normal, x) * x;
        if (glm::length(y) < 1.0e-6f) {
            return false;
        }
        y = glm::normalize(y);
        y = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(kThumbRollDeg), x)) * y;
        out = glm::mat3(x, y, glm::cross(x, y));
        return true;
    };
    const auto twistOnChain = [&](int segment, RigRole nextRole, RigRole twistRole, RigSide side) {
        const int next = roles.find(nextRole, side);
        for (int c = next >= 0 ? data.bones[idx(next)].parent : -1; c >= 0 && c != segment; c = data.bones[idx(c)].parent) {
            if (roles.role(c) == twistRole) {
                return true;
            }
        }
        return false;
    };
    std::vector<glm::mat3> frame(idx(n), glm::mat3(1.0f));
    for (int i = 0; i < n; ++i) {
        const RigRoleTag& t = roles.tag(i);
        ModelBone&        bone = data.bones[idx(i)];
        if (t.role != RigRole::None) {
            ++report.bodyBones;
        }
        switch (t.role) {
        case RigRole::UpperArmTwist:
        case RigRole::ForearmTwist:
        case RigRole::ThighTwist:
        case RigRole::ShinTwist: ++report.twistHelpers; break;
        case RigRole::Finger: ++report.fingerJoints; break;
        case RigRole::Toe: ++report.toeJoints; break;
        case RigRole::Other: ++report.unplaced; break;
        default: break;
        }
        bool helperForSegment = false;
        switch (t.role) {
        case RigRole::UpperArm: helperForSegment = twistOnChain(i, RigRole::Forearm, RigRole::UpperArmTwist, t.side); break;
        case RigRole::Forearm: helperForSegment = twistOnChain(i, RigRole::Hand, RigRole::ForearmTwist, t.side); break;
        case RigRole::Thigh: helperForSegment = twistOnChain(i, RigRole::Shin, RigRole::ThighTwist, t.side); break;
        default: break;
        }
        RoleLimits lims = limitsFor(t, roles.spineLinks(), roles.neckLinks(), helperForSegment, roles.find(RigRole::Metatarsal, t.side) >= 0);
        if (t.role == RigRole::UpperArm) {
            // THE LIMITS ARE RELATIVE TO THE BIND. The reference generation binds its arms 46
            // degrees from straight down, and its shoulder raises 90 and lowers 40 from there — an
            // absolute reach from 6 to 136 degrees of elevation. A figure bound with its arms out
            // horizontally (a T-pose) keeps that reach: its elevation channel shifts by the bind's
            // elevation against the reference's, so it can lower the arm 84 and raise it 46.
            const glm::vec3 dir = directionFor(i);
            const float     len = glm::length(dir);
            if (len > 1.0e-6f) {
                constexpr float kReferenceElevationDeg = 46.0f;
                const float elevation = glm::degrees(std::acos(std::max(-1.0f, std::min(1.0f, glm::dot(dir / len, glm::vec3(0.0f, -1.0f, 0.0f))))));
                const float shift = elevation - kReferenceElevationDeg; // (positive: the arm stands higher than the reference's)
                lims.z.mn -= shift;
                lims.z.mx -= shift;
            }
        }
        if (t.side == RigSide::Right) {
            lims.y = mirrored(lims.y);
            lims.z = mirrored(lims.z);
        }
        bone.rotationOrder = lims.order;
        bone.rotationMin = glm::vec3(lims.x.mn, lims.y.mn, lims.z.mn);
        bone.rotationMax = glm::vec3(lims.x.mx, lims.y.mx, lims.z.mx);
        bone.rotationLimited = glm::bvec3(lims.x.limited, lims.y.limited, lims.z.limited);
        // The frame: along the bone for the roles that have a length; a leaf twist helper takes its
        // segment's; the rest identity.
        const char letter = lengthLetter(t.role);
        const bool leafTwist = (t.role == RigRole::UpperArmTwist || t.role == RigRole::ForearmTwist || t.role == RigRole::ThighTwist ||
                                t.role == RigRole::ShinTwist) &&
                               children[idx(i)].empty() && bone.parent >= 0;
        if (leafTwist) {
            frame[idx(i)] = frame[idx(bone.parent)]; // (parents precede children: the segment's frame is set)
        } else if (letter != 0) {
            glm::mat3 thumb(1.0f);
            if (t.role == RigRole::Finger && t.index == 1 && thumbFrame(i, t, thumb)) {
                frame[idx(i)] = thumb;
            } else {
                frame[idx(i)] = frameAlong(letter, directionFor(i));
            }
        }
        bone.orientation = orientationEuler(frame[idx(i)]);
    }

    // ---- 3. THE DRIVEN HELPERS (ModelBone::drivenBy). A game-style rig keeps its correctives in
    // the skeleton: twist helpers along a segment and "share" bones at a knee or an elbow, driven
    // by expressions the FBX export loses. Left undriven, all of a segment's twist happens at its
    // joint (the candy wrapper) and all of a fold at one joint (the pinched elbow). Each gets its
    // driver here, the driver's frame and rotation order copied so the channels correspond:
    //  - a HELPER-role bone standing AT a real joint (within kDriverAtJointM of it) takes HALF of
    //    that joint's rotation — as the joint's child, counter-rotated by a half (the skin then
    //    follows half the bend); as its sibling, a half;
    //  - a LEAF twist helper of a segment takes the SHARE of the segment's twist (the channel along
    //    the bone) that its place along the segment says, the skin twisting in proportion to its
    //    distance from the joint: a helper at the joint none, one at the far end all; a chain's
    //    local values are the differences of the shares, the first against the segment's whole
    //    twist. A twist helper ON the chain (carrying the next joint) is posed by the solve and
    //    gets no driver.
    constexpr float   kDriverAtJointM = 0.01f;
    static const bool kNoDrivers = std::getenv("POSESTUDIO_FBX_NO_DRIVERS") != nullptr; // A/B: the helpers rigid, as before
    const auto segmentOf = [&](int b) -> int { // a twist helper's segment: its first non-twist ancestor
        int p = data.bones[idx(b)].parent;
        while (p >= 0 && isTwistRole(roles.role(p))) {
            p = data.bones[idx(p)].parent;
        }
        return p;
    };
    const auto twistAxisOf = [&](const std::string& formatOrder) -> int {
        // The segment's twist channel: the FORMAT's first letter (the conformer writes the
        // format's order; Armature::build stores it reversed).
        const char c = formatOrder.empty() ? 'X' : formatOrder.front();
        return c == 'X' ? 0 : c == 'Y' ? 1 : 2;
    };
    const auto realJoint = [&](int b) {
        const RigRole r = roles.role(b);
        return r != RigRole::None && r != RigRole::Helper && r != RigRole::Other && !isTwistRole(r);
    };
    for (int i = 0; i < n && !kNoDrivers; ++i) {
        const RigRoleTag& t = roles.tag(i);
        ModelBone&        bone = data.bones[idx(i)];
        if (t.role == RigRole::Helper) {
            const int p = bone.parent;
            if (p < 0) {
                continue;
            }
            int joint = -1;
            if (realJoint(p) && glm::length(pos[idx(i)] - pos[idx(p)]) < kDriverAtJointM) {
                joint = p; // at its parent's joint: the knee's share bone under the shin
            } else {
                for (const int s : children[idx(p)]) {
                    if (s != i && realJoint(s) && glm::length(pos[idx(i)] - pos[idx(s)]) < kDriverAtJointM) {
                        joint = s; // at a sibling's joint: the ball's share bone under the foot
                        break;
                    }
                }
            }
            if (joint < 0) {
                continue;
            }
            const ModelBone& driver = data.bones[idx(joint)];
            bone.orientation = driver.orientation;
            bone.rotationOrder = driver.rotationOrder;
            bone.drivenBy = joint;
            bone.driveFactor = glm::vec3(joint == p ? -0.5f : 0.5f);
            ++report.drivenHelpers;
        } else if (isTwistRole(t.role) && children[idx(i)].empty()) {
            const int segment = segmentOf(i);
            if (segment < 0) {
                continue;
            }
            std::vector<int> chain; // the segment's helpers down to this leaf, in chain order
            for (int c = i; c >= 0 && c != segment; c = data.bones[idx(c)].parent) {
                chain.insert(chain.begin(), c);
            }
            const glm::vec3    segDir = directionFor(segment);
            const float        segLen = glm::length(segDir);
            std::vector<float> share(chain.size(), 0.0f);
            bool               degenerate = true;
            for (std::size_t k = 0; k < chain.size(); ++k) {
                const float along = segLen > 1.0e-5f ? glm::dot(pos[idx(chain[k])] - pos[idx(segment)], segDir) / (segLen * segLen) : 0.0f;
                share[k] = std::max(0.0f, std::min(1.0f, along));
                degenerate = degenerate && share[k] < 0.05f;
            }
            if (degenerate) { // every helper at the joint: an even ramp
                for (std::size_t k = 0; k < share.size(); ++k) {
                    share[k] = static_cast<float>(k + 1) / static_cast<float>(share.size() + 1);
                }
            }
            const ModelBone& driver = data.bones[idx(segment)];
            const int        axis = twistAxisOf(driver.rotationOrder);
            for (std::size_t k = 0; k < chain.size(); ++k) {
                ModelBone& link = data.bones[idx(chain[k])];
                glm::vec3  factor(0.0f);
                factor[axis] = share[k] - (k == 0 ? 1.0f : share[k - 1]);
                link.orientation = driver.orientation;
                link.rotationOrder = driver.rotationOrder;
                link.drivenBy = segment;
                link.driveFactor = factor;
            }
            report.drivenHelpers += static_cast<int>(chain.size());
        }
    }
    // ---- 4. THE STANDARD'S NAMES (2026-11-03; the user: "should we use standard bone names? I
    // noticed they are retaining the original names"). Every bone the standard names takes that
    // name — hips, spine1, upper_arm_l, thumb2_r, ball_l — and keeps its file's name where the
    // standard has none (the face rig, a helper, a bone no role was found for). The skin and the
    // drivers bind by index, so nothing moves; what changes is what a POSE FILE carries: every
    // conformed rig shares the standard's frames, rotation orders and ranges, so with its names a
    // pose saved on one FBX character loads on every other, and the Transform tab reads
    // `thumb1_l` where it read `Bip001 L Finger0`. A standard name a kept name already uses, or
    // one two roles would share, is not applied: that bone keeps its own. (A rig authored to the
    // standard is renamed to itself.) The native figures are never renamed — their correctives,
    // their pose files and the content around them are keyed to their own names, which the
    // standard reads as its first convention.
    {
        std::vector<std::string>        next(idx(n));
        std::unordered_set<std::string> taken;
        for (int i = 0; i < n; ++i) {
            next[idx(i)] = RigRoles::standardName(roles.tag(i));
            if (next[idx(i)].empty()) {
                next[idx(i)] = data.bones[idx(i)].name;
                taken.insert(next[idx(i)]);
            }
        }
        for (int i = 0; i < n; ++i) {
            if (RigRoles::standardName(roles.tag(i)).empty()) {
                continue;
            }
            if (taken.count(next[idx(i)]) != 0 && next[idx(i)] != data.bones[idx(i)].name) {
                next[idx(i)] = data.bones[idx(i)].name; // (a collision: the bone keeps its own name)
            }
            taken.insert(next[idx(i)]);
        }
        for (int i = 0; i < n; ++i) {
            if (next[idx(i)] != data.bones[idx(i)].name) {
                data.bones[idx(i)].name = next[idx(i)];
                ++report.renamed;
            }
        }
    }
    report.conformed = true;
    return report;
}

} // namespace pose
