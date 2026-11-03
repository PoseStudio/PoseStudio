/**
 * @file armature.cpp
 * @brief Armature's skeleton construction, forward kinematics, pose snapshots, and the skeleton
 *        dump/load used by the IK harness. The full-body-IK integration lives in armatureik.cpp.
 */

#include "armature.h"

#include "ikmath.h" // eulerMatrix (the pose composition)
#include "ikrig.h"  // complete type for the unique_ptr
#include "jointsolver.h" // likewise

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pose {

namespace {

// Pose-snapshot rows carrying a bone's pose TRANSLATION rather than a rotation are keyed
// "@trans:<boneName>" — flowing through the existing (name, vec3) snapshot/undo/.pose-file
// machinery unchanged (bone names never contain '@', and readers ignore unknown names, so old
// files load and old builds skip the rows). Written by FBIK for the skeleton root: rotations
// alone can't move it, and a feet-pinned crouch must drop the hip. (The prefixes live in
// armature.h — shared with the .pss project codec.)

} // namespace

Armature::Armature() = default;
Armature::~Armature() = default;
Armature::Armature(Armature&&) noexcept = default;
Armature& Armature::operator=(Armature&&) noexcept = default;

namespace {

/// The other side's name for a left/right-prefixed bone name, or "" for a centre/unpaired one.
/// Figures prefix `l`/`r` before an uppercase letter or underscore (`lShin`, `l_thigh`); a few
/// use `Left`/`Right`. A plain lowercase continuation (`lowerJaw`, `rectus`) is not a side.
/// Other rigs carry the side as a TOKEN anywhere in the name — `CC_Base_L_Thigh`, `DEF-thigh.L`,
/// `thigh_l`, `rig:LeftUpLeg`: a lone `L`/`R` (either case) bounded by `_ . - :` or the
/// name's ends, or `Left`/`Right` (any case) bounded likewise or by a change of case; the first
/// such token is swapped. (Until 2026-11-03 only the prefixes were read, and an FBX rig's toes
/// and twist helpers paired with themselves — 474mm "lopsided" at rest to the harness, and
/// Mirror Pose flipped them in place like centre bones.)
std::string mirroredBoneName(const std::string& name) {
    if (name.size() >= 2 && (name[0] == 'l' || name[0] == 'r')) {
        const char next = name[1];
        if ((next >= 'A' && next <= 'Z') || next == '_') {
            std::string other = name;
            other[0] = name[0] == 'l' ? 'r' : 'l';
            return other;
        }
    }
    static const char* const kWords[][2] = {{"Left", "Right"}, {"Right", "Left"},
                                            {"left", "right"}, {"right", "left"}};
    for (const auto& pair : kWords) {
        const std::string from(pair[0]);
        if (name.size() > from.size() && name.compare(0, from.size(), from) == 0) {
            return pair[1] + name.substr(from.size());
        }
    }
    const auto isSep = [](char c) { return c == '_' || c == '.' || c == '-' || c == ':' || c == ' '; };
    const auto isUpper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto isLowerOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    // A lone letter token.
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c != 'L' && c != 'R' && c != 'l' && c != 'r') {
            continue;
        }
        const bool startOk = i == 0 || isSep(name[i - 1]);
        const bool endOk = i + 1 == name.size() || isSep(name[i + 1]);
        if (startOk && endOk) {
            std::string other = name;
            other[i] = c == 'L' ? 'R' : c == 'R' ? 'L' : c == 'l' ? 'r' : 'l';
            return other;
        }
    }
    // A word token: bounded by a separator, the name's ends, or a change of case (`LeftArm`).
    static const char* const kSideWords[][2] = {{"Left", "Right"}, {"Right", "Left"}, {"left", "right"},
                                                {"right", "left"}, {"LEFT", "RIGHT"}, {"RIGHT", "LEFT"}};
    for (std::size_t i = 0; i < name.size(); ++i) {
        for (const auto& pair : kSideWords) {
            const std::string from(pair[0]);
            if (name.compare(i, from.size(), from) != 0) {
                continue;
            }
            const std::size_t end = i + from.size();
            const bool startOk = i == 0 || isSep(name[i - 1]) || (isLowerOrDigit(name[i - 1]) && isUpper(from[0]));
            const bool endOk = end == name.size() || isSep(name[end]) || (isUpper(name[end]) && !isUpper(from.back())) ||
                               (isLowerOrDigit(name[end]) && isUpper(from.back()));
            if (startOk && endOk) {
                return name.substr(0, i) + pair[1] + name.substr(end);
            }
        }
    }
    return {};
}

} // namespace

void Armature::build(const std::vector<ArmatureBone>& bones) {
    // Start from a fresh armature: EVERY member — skeleton, pose, selection, pins, transform,
    // and the whole IK state block (held orientations, the posture reference, the pin easing
    // and follower state, ...) — is reset, so a rebuilt armature behaves exactly like a
    // freshly constructed one. Clearing only the skeleton members left the IK block stale;
    // harmless while build() runs once per object, which is the contract (see the header).
    *this = Armature();

    // inverseBind and localBind are precomputed; computeSkinMatrices() then fills the skin data —
    // every skin transform is identity at bind pose, so a freshly imported model draws exactly as
    // its geometry.
    m_bones.resize(bones.size());
    std::vector<glm::vec3> bindGlobal(bones.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const ArmatureBone& src = bones[i];
        m_bones[i].drivenBy = src.drivenBy >= 0 && src.drivenBy < static_cast<int>(bones.size()) && src.drivenBy != static_cast<int>(i) ? src.drivenBy : -1;
        m_bones[i].driveFactor = src.driveFactor;
        Bone& bone = m_bones[i];
        const bool hasParent = src.parent >= 0 && static_cast<std::size_t>(src.parent) < i;
        bone.parent = hasParent ? src.parent : -1;
        const glm::vec3 parentGlobal =
            hasParent ? bindGlobal[static_cast<std::size_t>(src.parent)] : glm::vec3(0.0f);
        bindGlobal[i] = parentGlobal + src.localBindTranslation;
        bone.localBind = glm::translate(glm::mat4(1.0f), src.localBindTranslation);
        bone.inverseBind = glm::translate(glm::mat4(1.0f), -bindGlobal[i]);
        // The rest orientation frame the pose rotations act in. The format gives it as Euler angles
        // APPLIED in x, y, z order — Rz * Ry * Rx, which eulerMatrix (leftmost letter outermost)
        // spells "ZYX". Composed the other way round (Rx * Ry * Rz, until 2026-09-30) the frame is
        // the same wherever one angle dominates — a thigh, a shin, an upper arm: under 2 degrees off
        // — and wrong wherever two or three are large: a forearm's bend axis stood 11 degrees off, a
        // foot's 7, a finger's 8, and a THUMB's 44-56 (it folded sideways across the palm). The
        // check: the frame's length axis (the rotation order's first) lies along the bone to its
        // child to 0.1% on every limb bone of every generation this way, and only this way.
        // (POSESTUDIO_ORIENT_XYZ is the A/B: the frames as they were.)
        static const bool kOrientXyz = std::getenv("POSESTUDIO_ORIENT_XYZ") != nullptr;
        bone.orient = eulerMatrix(src.orientation, kOrientXyz ? "XYZ" : "ZYX");
        bone.invOrient = glm::inverse(bone.orient);
        bone.orientationDeg = src.orientation;
        // THE ROTATION ORDER IS THE ORDER THE CHANNELS ARE APPLIED IN (2026-10-27): the format's
        // first letter — the twist, about the bone's own length — acts FIRST (innermost), the
        // last — the wide bend — last: "YZX" is Rx · Rz · Ry. eulerMatrix composes its string
        // leftmost OUTERMOST, so the engine keeps the order REVERSED ("XZY" for that bone) and
        // every reader of the stored string — eulerMatrix, eulerFromMatrix, the solver's
        // channel-axis prefix, the dials' sweep — composes the format's way; dump() writes the
        // format's string back, and the length axis is the stored string's LAST letter
        // (lengthAxisLetter). Composed the other way, as it was since the importer was written,
        // one non-zero channel was the same and two were not: a thigh flexed and then moved
        // Side-Side ROLLED about its own length where a hip abducts, and a bent forearm's twist
        // on the one-bone generations swung it round the upper arm's line. The evidence: the
        // orientation (the same convention, measured by the frames' length axes); the
        // generations that cut a limb's twist out into a CHILD bone, which apply it innermost by
        // construction and author the same ranges as the one-bone generations; and the format's
        // reference importer, whose Euler type's order string is its application order.
        // (POSESTUDIO_ORDER_AS_WRITTEN is the A/B: the composition as it was.)
        bone.rotationOrder = kOrderAsWritten ? src.rotationOrder
                                             : std::string(src.rotationOrder.rbegin(), src.rotationOrder.rend());
        bone.rotMin = src.rotationMin;
        bone.rotMax = src.rotationMax;
        bone.rotLimited = src.rotationLimited;
        bone.poseLocal = bone.localBind;
        m_boneIndex.emplace(src.name, static_cast<int>(i));
        m_boneNames.push_back(src.name);
    }
    m_boneEuler.assign(m_bones.size(), glm::vec3(0.0f));
    m_boneTranslation.assign(m_bones.size(), glm::vec3(0.0f));
    m_boneWorldPos.assign(m_bones.size(), glm::vec3(0.0f));
    m_bonePinned.assign(m_bones.size(), 0);
    m_poseGlobal.assign(m_bones.size(), glm::mat4(1.0f));

    // Children lists (subtree walks for the reset/mirror utilities) and each bone's mirror —
    // the other side's bone by name, itself for centre bones and names without a counterpart.
    m_children.assign(m_bones.size(), {});
    m_mirrorBone.resize(m_bones.size());
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        if (m_bones[i].parent >= 0) {
            m_children[static_cast<std::size_t>(m_bones[i].parent)].push_back(static_cast<int>(i));
        }
        const std::string other = mirroredBoneName(m_boneNames[i]);
        const int j = other.empty() ? -1 : boneIndex(other);
        m_mirrorBone[i] = j >= 0 ? j : static_cast<int>(i);
    }

    // Highlight twins (see m_highlightTwin): a TWIST bone — two locked axes and the free one
    // ALONG the bone (isTwistBone, which reads the children lists above) — is paired with its
    // bend parent both ways; the first twist child wins for the parent. NOT any child with two
    // locked axes, as it was until 2026-09-30: a HINGE has two locked axes too, its free one
    // across the bone, and a finger's or a toe's middle joint is one — so the second joint of
    // every finger was its first joint's "twin", the two lit together whichever was selected,
    // and nobody could tell which finger joint a click had taken.
    m_highlightTwin.assign(m_bones.size(), -1);
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const int parent = m_bones[i].parent;
        if (parent < 0 || parent >= static_cast<int>(m_bones.size()) || !isTwistBone(static_cast<int>(i))) {
            continue;
        }
        if (m_highlightTwin[static_cast<std::size_t>(parent)] < 0) {
            m_highlightTwin[static_cast<std::size_t>(parent)] = static_cast<int>(i);
            m_highlightTwin[i] = parent;
        }
    }
    // A name without a counterpart is not always a centre bone: one figure generation's base file
    // spells some thirty left/right ids differently on its two sides (a big toe's second joint ends
    // `2` on one and `_2` on the other, a lip corner is misspelt on one side only, an eyelid is
    // `Top` here and `Upper` there). Such a bone stands OFF the sagittal plane, and its twin is the
    // unpaired bone at its mirrored bind position under the mirrored parent — parents come first
    // in the list, so theirs is already known. Left paired with themselves, Mirror Pose flipped
    // each of them in place like a centre bone: a mirrored hand kept its own carpals' pose.
    if (!m_bones.empty()) {
        float lowest = bindGlobal[0].y;
        float highest = bindGlobal[0].y;
        for (const glm::vec3& p : bindGlobal) {
            lowest = std::min(lowest, p.y);
            highest = std::max(highest, p.y);
        }
        const float height = std::max(highest - lowest, 1.0e-3f);
        const float offPlane = 0.003f * height; // (a centre bone of an asymmetric character: millimetres)
        const float within = 0.02f * height;
        // (And a name's counterpart is not always the bone's twin: the same file's `…Carpal2` is one
        // bone of the palm on the left and ANOTHER on the right, 2cm apart. A name pair whose bind
        // positions do not mirror each other is dissolved, and its bones are paired by place below —
        // with each other again, if an asymmetric character's rig is all that was wrong.)
        const float mirrored = 0.005f * height;
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            const int j = m_mirrorBone[i];
            if (j > static_cast<int>(i) &&
                glm::length(glm::vec3(-bindGlobal[i].x, bindGlobal[i].y, bindGlobal[i].z) - bindGlobal[static_cast<std::size_t>(j)]) > mirrored) {
                m_mirrorBone[static_cast<std::size_t>(j)] = j;
                m_mirrorBone[i] = static_cast<int>(i);
            }
        }
        for (std::size_t i = 0; i < m_bones.size(); ++i) {
            if (m_mirrorBone[i] != static_cast<int>(i) || std::abs(bindGlobal[i].x) < offPlane) {
                continue;
            }
            const int parent = m_bones[i].parent;
            const int parentTwin = parent >= 0 ? m_mirrorBone[static_cast<std::size_t>(parent)] : -1;
            const glm::vec3 mirrored(-bindGlobal[i].x, bindGlobal[i].y, bindGlobal[i].z);
            int twin = -1;
            float nearest = within;
            for (std::size_t j = 0; j < m_bones.size(); ++j) {
                if (j == i || m_mirrorBone[j] != static_cast<int>(j) || m_bones[j].parent != parentTwin ||
                    bindGlobal[j].x * bindGlobal[i].x >= 0.0f) {
                    continue;
                }
                const float d = glm::length(bindGlobal[j] - mirrored);
                if (d < nearest) {
                    nearest = d;
                    twin = static_cast<int>(j);
                }
            }
            if (twin >= 0) {
                m_mirrorBone[i] = twin;
                m_mirrorBone[static_cast<std::size_t>(twin)] = static_cast<int>(i);
            }
        }
    }

    // Skin data: two vec4s per joint (a static armature has one identity joint).
    const std::uint32_t joints = jointCount();
    m_skinDualQuats.assign(std::size_t{2} * joints, glm::vec4(0.0f));
    for (std::uint32_t j = 0; j < joints; ++j) {
        m_skinDualQuats[2 * j] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // identity rotation
    }
    computeSkinMatrices(); // bind pose until a caller poses a bone
}

const bool Armature::kOrderAsWritten = std::getenv("POSESTUDIO_ORDER_AS_WRITTEN") != nullptr;

std::string Armature::formatRotationOrder(const Bone& b) {
    // (The format's own string: the stored one reversed, unless the A/B keeps it as written.)
    return kOrderAsWritten ? b.rotationOrder : std::string(b.rotationOrder.rbegin(), b.rotationOrder.rend());
}

char Armature::lengthAxisLetter(const Bone& b) {
    // The bone's length axis is the format's FIRST letter (the twist) — the stored string's last.
    if (b.rotationOrder.empty()) {
        return 'X';
    }
    return kOrderAsWritten ? b.rotationOrder.front() : b.rotationOrder.back();
}

bool Armature::dump(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& b = m_bones[i];
        const glm::vec3 t(b.localBind[3]);
        const glm::vec3& o = b.orientationDeg;
        out << m_boneNames[i] << ' ' << b.parent << ' ' << formatRotationOrder(b) << ' ' << t.x << ' '
            << t.y << ' ' << t.z << ' ' << o.x << ' ' << o.y << ' ' << o.z << ' ' << b.rotMin.x
            << ' ' << b.rotMin.y << ' ' << b.rotMin.z << ' ' << b.rotMax.x << ' ' << b.rotMax.y
            << ' ' << b.rotMax.z << ' ' << (b.rotLimited.x ? 1 : 0) << ' '
            << (b.rotLimited.y ? 1 : 0) << ' ' << (b.rotLimited.z ? 1 : 0) << '\n';
    }
    if (!m_bodyMesh.empty()) {
        saveBodyMesh(path + ".mesh", m_bodyMesh); // the sidecar (see setBodyMesh)
    }
    return static_cast<bool>(out);
}

void Armature::setBodyMesh(std::vector<BodyMeshPoint> mesh) {
    m_bodyMesh = std::move(mesh);
    m_ikRig.reset(); // rebuilt with the mesh on the next use (ensureIkRig)
    m_boneClass.clear(); // (... and the bone classes with it)
}

bool Armature::loadDump(const std::string& path, std::vector<ArmatureBone>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // The name may contain spaces (a merged follower's scene node, e.g. a tear-line figure
        // named after its character): the 17 fields after it are fixed, so the name is
        // everything before the last 17 tokens.
        std::vector<std::string> tokens;
        {
            std::istringstream ss(line);
            std::string tok;
            while (ss >> tok) {
                tokens.push_back(tok);
            }
        }
        constexpr std::size_t kFields = 17;
        if (tokens.size() < kFields + 1) {
            out.clear();
            return false;
        }
        ArmatureBone b;
        const std::size_t nameTokens = tokens.size() - kFields;
        for (std::size_t k = 0; k < nameTokens; ++k) {
            if (k > 0) {
                b.name.push_back(' ');
            }
            b.name += tokens[k];
        }
        std::string rest;
        for (std::size_t k = nameTokens; k < tokens.size(); ++k) {
            rest += tokens[k];
            rest.push_back(' ');
        }
        std::istringstream ss(rest);
        int lx = 0, ly = 0, lz = 0;
        if (!(ss >> b.parent >> b.rotationOrder >> b.localBindTranslation.x >>
              b.localBindTranslation.y >> b.localBindTranslation.z >> b.orientation.x >>
              b.orientation.y >> b.orientation.z >> b.rotationMin.x >> b.rotationMin.y >>
              b.rotationMin.z >> b.rotationMax.x >> b.rotationMax.y >> b.rotationMax.z >> lx >>
              ly >> lz)) {
            out.clear();
            return false;
        }
        b.rotationLimited = glm::bvec3(lx != 0, ly != 0, lz != 0);
        if (b.parent >= static_cast<int>(out.size())) {
            out.clear();
            return false; // parents must precede children
        }
        out.push_back(std::move(b));
    }
    return true;
}

void Armature::translateY(float dy) {
    m_transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, dy, 0.0f)) * m_transform;
    computeSkinMatrices(); // refresh the transform-dependent bone world positions (picking/markers)
}

void Armature::shiftPoseY(float dy) {
    const int root = (!m_bones.empty() && ensureIkRig()) ? m_ikRig->rootNode() : -1;
    if (root < 0) {
        translateY(dy); // (no skeleton: the model itself moves)
        return;
    }
    // World -> model -> the root's parent frame, where a pose translation lives.
    const Bone& bone = m_bones[static_cast<std::size_t>(root)];
    glm::vec3   delta = glm::inverse(glm::mat3(m_transform)) * glm::vec3(0.0f, dy, 0.0f);
    if (bone.parent >= 0) {
        delta = glm::transpose(glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)])) * delta;
    }
    m_boneTranslation[static_cast<std::size_t>(root)] += delta;
    recomposePoseLocal(static_cast<std::size_t>(root));
    computeSkinMatrices();
}

void Armature::setTransform(const glm::mat4& transform) {
    m_transform = transform;
    computeSkinMatrices(); // refresh the transform-dependent bone world positions (picking/markers)
}

void Armature::computeSkinMatrices() {
    ++m_skinVersion;
    if (m_skinDualQuats.size() < 2) {
        return; // not built yet
    }
    glm::vec4* dst = m_skinDualQuats.data();
    if (m_bones.empty()) {
        dst[0] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // static model: one identity joint
        dst[1] = glm::vec4(0.0f);
        return;
    }
    // THE DRIVEN BONES (ArmatureBone::drivenBy, 2026-11-03): a rig's helper whose driver
    // expression the file could not carry — a knee's "share" bone taking half the bend, a twist
    // helper taking its share of the segment's twist — is re-derived from its driver here, the
    // one place every pose change passes through, before the hierarchy walk. It is never an
    // unknown of a solve (not on any active path, or locked) and never a dial; a pose file's
    // value for it is overwritten on load. The drivers' Eulers are read, never written, so the
    // bone order between the two does not matter.
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& bone = m_bones[i];
        if (bone.drivenBy < 0 || static_cast<std::size_t>(bone.drivenBy) >= m_bones.size()) {
            continue;
        }
        const glm::vec3 driven = bone.driveFactor * m_boneEuler[static_cast<std::size_t>(bone.drivenBy)];
        if (driven != m_boneEuler[i]) {
            m_boneEuler[i] = driven;
            recomposePoseLocal(i);
        }
    }
    // Accumulate each bone's posed global transform down the hierarchy (parents precede children),
    // then convert the rigid skinTransform = poseGlobal · inverseBind into a unit dual quaternion
    // (real = rotation stored (x,y,z,w); dual = 0.5·(0,t)·real). The transform is exactly rigid —
    // binds are translation-only and every pose factor is a rotation or translation — so the
    // conversion is lossless. m_poseGlobal is a member scratch buffer — this runs on every
    // drag-move while posing, so it mustn't allocate per call.
    std::vector<glm::mat4>& poseGlobal = m_poseGlobal;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& bone = m_bones[i];
        poseGlobal[i] =
            (bone.parent >= 0) ? poseGlobal[bone.parent] * bone.poseLocal : bone.poseLocal;
        const glm::mat4 skin = poseGlobal[i] * bone.inverseBind;
        const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(skin)));
        const glm::vec3 t = glm::vec3(skin[3]);
        const glm::quat d = glm::quat(0.0f, t.x, t.y, t.z) * q; // (0,t)·q
        dst[2 * i] = glm::vec4(q.x, q.y, q.z, q.w);
        dst[2 * i + 1] = 0.5f * glm::vec4(d.x, d.y, d.z, d.w);
        // The joint's world position (for the posing overlay / picking) = model · poseGlobal · origin.
        m_boneWorldPos[i] = glm::vec3(m_transform * poseGlobal[i][3]);
    }
}

void Armature::clampBoneEuler(int index) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return;
    }
    const Bone& bone = m_bones[static_cast<std::size_t>(index)];
    glm::vec3& e = m_boneEuler[static_cast<std::size_t>(index)];
    for (int a = 0; a < 3; ++a) {
        if (bone.rotLimited[a]) {
            e[a] = glm::clamp(e[a], bone.rotMin[a], bone.rotMax[a]);
        }
    }
}

void Armature::recomposePoseLocal(std::size_t index) {
    Bone& bone = m_bones[index];
    // A ZERO Euler is the bind itself, exactly (2026-09-23): composed as localBind * OR * I *
    // OR^-1 it was the bind to a float rounding, and a pose reset — or any joint brought back to
    // zero — left every world matrix 1e-7 off the freshly built figure's. A drag then began its
    // solve from residuals a few 1e-7 apart, and one that runs out of iterations grows that to
    // 2e-5 in a tick and to a different basin of a chaotic gesture (the in-app prone push-up
    // landed its arms straight after a fresh load and folded after a reset). Reset Pose is a
    // clean slate now: the same pose and target give the same drag, bit for bit.
    const glm::vec3& e = m_boneEuler[index];
    bone.poseLocal = (e.x == 0.0f && e.y == 0.0f && e.z == 0.0f)
                         ? bone.localBind
                         : bone.localBind * bone.orient * eulerMatrix(e, bone.rotationOrder) * bone.invOrient;
    // Pose translation adds in the parent frame (only ever non-zero where FBIK moved the root).
    bone.poseLocal[3] += glm::vec4(m_boneTranslation[index], 0.0f);
}

void Armature::applyBoneEuler(int index) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return;
    }
    clampBoneEuler(index); // keep the joint within its anatomical range of motion
    recomposePoseLocal(static_cast<std::size_t>(index));
    computeSkinMatrices();
}

void Armature::nudgeSelectedBone(const glm::vec3& deltaEulerDegrees) {
    nudgeBone(m_selectedBone, deltaEulerDegrees);
}

void Armature::nudgeBone(int bone, const glm::vec3& deltaEulerDegrees) {
    if (bone < 0 || bone >= static_cast<int>(m_bones.size())) {
        return;
    }
    const std::size_t sel = static_cast<std::size_t>(bone);
    const glm::vec3 before = m_boneEuler[sel];
    // The FK COLLISION STOP: a rotation may not push a limb's joint into a body volume (see
    // fkVolumeDepth — the same capsules the IK keeps the limbs out of). The rotation is applied
    // in full, and if any moving pair penetrates deeper than it already did (or than the
    // 2mm tolerance), the angle is bisected back to the largest fraction that does not — so a
    // joint can always rotate OUT of a penetration a loaded pose left it in, and stops at the
    // surface on the way in. The rig is built on the first rotation if no IK drag has yet.
    const bool guard = ensureIkRig() && !m_ikRig->bodyVolumes().empty();
    const float depthBefore = guard ? fkVolumeDepth(bone) : 0.0f;
    m_boneEuler[sel] = before + deltaEulerDegrees;
    applyBoneEuler(bone);
    if (guard) {
        constexpr float kFkCollideTol = 0.002f;
        const float allow = std::max(depthBefore, kFkCollideTol);
        if (fkVolumeDepth(bone) > allow) {
            float lo = 0.0f; // the fraction of the rotation last known not to penetrate
            float hi = 1.0f;
            for (int k = 0; k < 8; ++k) {
                const float mid = 0.5f * (lo + hi);
                m_boneEuler[sel] = before + deltaEulerDegrees * mid;
                applyBoneEuler(bone);
                if (fkVolumeDepth(bone) > allow) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            m_boneEuler[sel] = before + deltaEulerDegrees * lo;
            applyBoneEuler(bone);
        }
    }
    // (Never during an IK drag: every caller closes or refuses one first. Until 2026-09-30 the
    // X/Y/Z wheel could turn the grabbed joint mid-drag, and the rotation was then made part of
    // the solve's posture reference here — that went with the wheel.)
}

bool Armature::setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees) {
    const int index = boneIndex(boneName);
    if (index < 0) {
        return false;
    }
    // applyBoneEuler poses in the joint's oriented frame: localBind · orient · R(euler, order) ·
    // orient⁻¹. At rest (euler 0) orient·orient⁻¹ cancels, leaving the bind pose; a rotation then acts
    // about the joint's true axes (an elbow bends anatomically), composed in its rotation order.
    m_boneEuler[static_cast<std::size_t>(index)] = eulerDegrees;
    applyBoneEuler(index);
    return true;
}

std::vector<std::pair<std::string, glm::vec3>> Armature::capturePose() const {
    std::vector<std::pair<std::string, glm::vec3>> pose;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const glm::vec3& e = m_boneEuler[i];
        if (e.x != 0.0f || e.y != 0.0f || e.z != 0.0f) {
            pose.emplace_back(m_boneNames[i], e);
        }
        const glm::vec3& t = m_boneTranslation[i];
        if (t.x != 0.0f || t.y != 0.0f || t.z != 0.0f) {
            pose.emplace_back(kPoseTranslationPrefix + m_boneNames[i], t);
        }
    }
    for (std::size_t i = 0; i < m_bonePinned.size() && i < m_bones.size(); ++i) {
        if (m_bonePinned[i]) {
            pose.emplace_back(kPosePinPrefix + m_boneNames[i], glm::vec3(1.0f, 0.0f, 0.0f));
        }
    }
    return pose;
}

void Armature::applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose) {
    clearHangBases(); // (a pose from elsewhere: the arms' swing bases are re-read at the next press)
    for (glm::vec3& e : m_boneEuler) {
        e = glm::vec3(0.0f); // reset to bind pose first
    }
    for (glm::vec3& t : m_boneTranslation) {
        t = glm::vec3(0.0f);
    }
    m_bonePinned.assign(m_bones.size(), 0); // the snapshot's pins replace the current ones
    constexpr std::size_t prefixLen = sizeof(kPoseTranslationPrefix) - 1;
    constexpr std::size_t pinPrefixLen = sizeof(kPosePinPrefix) - 1;
    for (const auto& [name, value] : pose) {
        if (name.compare(0, pinPrefixLen, kPosePinPrefix) == 0) {
            const int index = boneIndex(name.substr(pinPrefixLen));
            if (index >= 0) {
                m_bonePinned[static_cast<std::size_t>(index)] = 1;
            }
            continue;
        }
        if (name.compare(0, prefixLen, kPoseTranslationPrefix) == 0) {
            const int index = boneIndex(name.substr(prefixLen));
            if (index >= 0) {
                // SANITIZE: unlike rotations (per-channel clamped below), translations have no
                // authored limits — a hand-edited/corrupt .pose row could tear a bone meters
                // off the skeleton or stream-parse to NaN, which poisons the skin matrices.
                // The engine itself only ever writes modest root offsets; a ±1m box per
                // component is far beyond any legitimate value.
                glm::vec3 t = value;
                if (!std::isfinite(t.x + t.y + t.z)) {
                    t = glm::vec3(0.0f);
                }
                m_boneTranslation[static_cast<std::size_t>(index)] =
                    glm::clamp(t, glm::vec3(-1.0f), glm::vec3(1.0f));
            }
            continue;
        }
        const int index = boneIndex(name);
        if (index >= 0) {
            m_boneEuler[static_cast<std::size_t>(index)] = value;
        }
    }
    // Recompute every bone's posed local transform from its (limit-clamped) Euler, then the skin
    // data once. Clamping here too keeps a loaded pose within the figure's range of motion.
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        clampBoneEuler(static_cast<int>(i));
        recomposePoseLocal(i);
    }
    computeSkinMatrices();
}

void Armature::collectSubtree(int index, std::vector<int>& out) const {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return;
    }
    out.push_back(index);
    for (const int child : m_children[static_cast<std::size_t>(index)]) {
        collectSubtree(child, out);
    }
}

void Armature::reposeAll() {
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        clampBoneEuler(static_cast<int>(i));
        recomposePoseLocal(i);
    }
    computeSkinMatrices();
}

namespace {
// The sagittal-plane reflection of a pose rotation, per Euler channel, and of a parent-frame
// translation. Reflecting a rotation about an axis flips its angle and mirrors the axis: the
// x axis maps to -x (angle flip cancels), y and z stay (angle flips) — so (x, -y, -z), in
// any rotation order, since each factor transforms independently.
glm::vec3 mirrorEuler(const glm::vec3& e) { return glm::vec3(e.x, -e.y, -e.z); }
glm::vec3 mirrorTranslation(const glm::vec3& t) { return glm::vec3(-t.x, t.y, t.z); }
} // namespace

bool Armature::resetBone(int index, bool subtree) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return false;
    }
    std::vector<int> bones;
    if (subtree) {
        collectSubtree(index, bones);
    } else {
        bones.push_back(index);
    }
    for (const int b : bones) {
        m_boneEuler[static_cast<std::size_t>(b)] = glm::vec3(0.0f);
        m_boneTranslation[static_cast<std::size_t>(b)] = glm::vec3(0.0f);
    }
    reposeAll();
    return true;
}

void Armature::resetPose() {
    clearHangBases();
    for (glm::vec3& e : m_boneEuler) {
        e = glm::vec3(0.0f);
    }
    for (glm::vec3& t : m_boneTranslation) {
        t = glm::vec3(0.0f);
    }
    reposeAll();
}

void Armature::mirrorPose() {
    // Read the whole pose first: a left/right pair writes each other's slot.
    const std::vector<glm::vec3> euler = m_boneEuler;
    const std::vector<glm::vec3> translation = m_boneTranslation;
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const auto j = static_cast<std::size_t>(m_mirrorBone[i]);
        m_boneEuler[j] = mirrorEuler(euler[i]);
        m_boneTranslation[j] = mirrorTranslation(translation[i]);
    }
    reposeAll();
}

bool Armature::mirrorSubtreeToOpposite(int index) {
    if (index < 0 || index >= static_cast<int>(m_bones.size())) {
        return false;
    }
    std::vector<int> bones;
    collectSubtree(index, bones);
    const std::vector<glm::vec3> euler = m_boneEuler;
    const std::vector<glm::vec3> translation = m_boneTranslation;
    for (const int b : bones) {
        const auto i = static_cast<std::size_t>(b);
        const auto j = static_cast<std::size_t>(m_mirrorBone[i]);
        m_boneEuler[j] = mirrorEuler(euler[i]);
        m_boneTranslation[j] = mirrorTranslation(translation[i]);
    }
    reposeAll();
    return true;
}

} // namespace pose
