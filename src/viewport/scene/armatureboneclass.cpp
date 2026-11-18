/**
 * @file armatureboneclass.cpp
 * @brief The BONE CLASSES — what each bone is to the posing UI (BoneClass: a joint of the body, a
 *        bone of the face rig, a digit's joint, a carpal) — and what a selection of a bone selects
 *        and lights.
 *
 * Found by STRUCTURE with the IK rig (classifyBones, from ensureIkRig) and corrected by the rig
 * standard's roles. The face rig's bones are for expressions: nothing selects one (a click on the
 * face selects the HEAD, Armature::posingBone) and none is ever an unknown of a solve. A digit's
 * joint IS selectable, and its drag is a solve of its own (armatureikdigit.cpp). A CARPAL — what
 * is below a wrist or an ankle and is neither the limb end nor a digit: the carpals, a hand's prop
 * anchor, the mid-foot, the ball, the heel — is the hand's or the foot's own flesh (2026-10-28): a
 * click on one selects the HAND or the FOOT, as a click on the face selects the head, and it
 * lights with it. (Until then it was a Body bone: selectable, the hand's flesh split among four
 * carpals and a click on the palm as often a carpal's as the hand's — "the hand node can be
 * difficult to select, and posing carpals in IK mode is not practical".) Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvetuning.h" // kDigitMinRangeDeg, kDigitMinReachShare, kFreeRangeDeg

#include "ikrig.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace pose {

void Armature::classifyBones() {
    const std::size_t n = m_bones.size();
    m_boneClass.assign(n, static_cast<char>(BoneClass::Body));
    static const bool kAllBody = std::getenv("POSESTUDIO_POSE_ALL_BONES") != nullptr; // A/B probe
    if (!m_ikRig || kAllBody) {
        return;
    }
    const IkRig& rig = *m_ikRig;
    const int    head = rig.headNode();
    const int    root = rig.rootNode();
    ensureHandMaps();
    // (Parents precede children: one pass carries "below the head", "of the body", "foot-class"
    // and the LIMB END a bone hangs below — its wrist, or its ankle: the foot's most proximal
    // foot-class joint — with that hand's or foot's REACH, its farthest joint at bind.)
    std::vector<char>  underHead(n, 0);
    std::vector<char>  ofBody(n, 0);
    std::vector<char>  footClass(n, 0);
    std::vector<int>   limbEnd(n, -1);
    std::vector<float> reach(n, 0.0f);
    int faces = 0;
    int digits = 0;
    int carpals = 0;
    for (std::size_t b = 0; b < n; ++b) {
        const int  p = m_bones[b].parent;
        const bool hasParent = p >= 0 && static_cast<std::size_t>(p) < b;
        underHead[b] = hasParent && (p == head || underHead[static_cast<std::size_t>(p)]);
        ofBody[b] = static_cast<int>(b) == root || (hasParent && ofBody[static_cast<std::size_t>(p)]);
        footClass[b] = ofBody[b] && static_cast<int>(b) != root && rig.isFootClass(static_cast<int>(b));
        if (head >= 0 && underHead[b]) {
            m_boneClass[b] = static_cast<char>(BoneClass::Face);
            ++faces;
            continue;
        }
        if (m_jsWristOf.size() == n && m_jsWristOf[b] >= 0 && m_jsWristOf[b] != static_cast<int>(b)) {
            limbEnd[b] = m_jsWristOf[b];
        } else if (footClass[b] && hasParent && footClass[static_cast<std::size_t>(p)]) {
            limbEnd[b] = limbEnd[static_cast<std::size_t>(p)] >= 0 ? limbEnd[static_cast<std::size_t>(p)] : p;
        }
        if (limbEnd[b] >= 0) {
            const std::size_t end = static_cast<std::size_t>(limbEnd[b]);
            reach[end] = std::max(reach[end], glm::length(m_ikBindPos[b] - m_ikBindPos[end]));
        }
    }
    // A DIGIT's joint, children first (a bone that carries more than one digit is no digit):
    //   - it BENDS: its widest LIMITED channel spans kDigitMinRangeDeg or more (a carpal's spans
    //     7-20; a helper bone that turns freely or full circle on every channel — a hand's prop
    //     anchor, authored -180..180 — is no joint);
    //   - it stands OUT in the hand or the foot: at least kDigitMinReachShare of the hand's or
    //     the foot's reach from the wrist or the ankle (a thumb's base stands 0.15-0.17 of it out;
    //     a heel bone or a mid-foot bone at the ankle 0.02-0.05, with 45-60 degrees of range);
    //   - it carries ONE digit at most: the bone the toes fan from (the ball of the foot, 100
    //     degrees of range and five children) is the foot's, as a carpal with two fingers is the
    //     hand's — a grab there is the limb's.
    for (std::size_t k = n; k-- > 0;) {
        if (limbEnd[k] < 0 || m_boneClass[k] != static_cast<char>(BoneClass::Body)) {
            continue;
        }
        const Bone&       bone = m_bones[k];
        const std::size_t end = static_cast<std::size_t>(limbEnd[k]);
        float             widest = 0.0f;
        for (int a = 0; a < 3; ++a) {
            const float range = bone.rotMax[a] - bone.rotMin[a];
            widest = std::max(widest, bone.rotLimited[a] && range < static_cast<float>(kFreeRangeDeg) ? range : 0.0f);
        }
        int carried = 0;
        for (const int c : m_children[k]) {
            carried += m_boneClass[static_cast<std::size_t>(c)] == static_cast<char>(BoneClass::Digit) ? 1 : 0;
        }
        if (widest >= kDigitMinRangeDeg && carried <= 1 &&
            glm::length(m_ikBindPos[k] - m_ikBindPos[end]) >= kDigitMinReachShare * reach[end]) {
            m_boneClass[k] = static_cast<char>(BoneClass::Digit);
            ++digits;
        }
    }
    // A CARPAL: what is left below a wrist or an ankle — the carpals, a hand's prop anchor, the
    // mid-foot, the ball, the heel — is the hand's or the foot's own flesh, not a joint to pose: a
    // click on one selects the limb end (posingBone), and it lights with it.
    m_boneLimbEnd = limbEnd;
    for (std::size_t b = 0; b < n; ++b) {
        if (limbEnd[b] >= 0 && m_boneClass[b] == static_cast<char>(BoneClass::Body)) {
            m_boneClass[b] = static_cast<char>(BoneClass::Carpal);
            ++carpals;
        }
    }
    // BY THE RIG STANDARD'S ROLES (rigroles.h, 2026-11-03): a Finger or a Toe is a DIGIT and a
    // carpal, a metatarsal, the ball of the foot or the heel is the hand's or the foot's own,
    // whatever the structural rules above made of them. The two agree on the reference rigs; on
    // a biped export they disagreed once — its one-toe foot's ball (the Ball role: the toes are
    // one childless block under it) bends 100 degrees, stands out in the foot and carries no digit,
    // so the structure read it as a TOE, and a click on the ball of the foot began a digit drag.
    if (rig.roles().built()) {
        for (std::size_t b = 0; b < n; ++b) {
            const RigRoleTag& t = rig.roles().tag(static_cast<int>(b));
            const bool        digit = t.role == RigRole::Finger || t.role == RigRole::Toe;
            // (A BALL is the foot's own where the toes fan from it; a ball with no toe below it — the
            // one-bone toe block of a game rig, a rigging add-on's, a biped's — IS the toes, and keeps
            // what the structure read: a digit, where it bends. The foot model's ball is a role, not a class.)
            bool fans = false;
            for (const int c : m_children[b]) {
                fans = fans || rig.roles().role(c) == RigRole::Toe;
            }
            const bool        own = t.role == RigRole::Carpal || t.role == RigRole::Metatarsal || (t.role == RigRole::Ball && fans) || t.role == RigRole::Heel;
            if (!digit && !own) {
                continue;
            }
            const BoneClass was = static_cast<BoneClass>(m_boneClass[b]);
            if (was == BoneClass::Face) {
                continue;
            }
            if (digit && was != BoneClass::Digit) {
                m_boneClass[b] = static_cast<char>(BoneClass::Digit);
                digits += 1;
                carpals -= was == BoneClass::Carpal ? 1 : 0;
            } else if (own && was != BoneClass::Carpal) {
                const int end = rig.roles().find(t.role == RigRole::Carpal ? RigRole::Hand : RigRole::Foot, t.side);
                if (end < 0 && m_boneLimbEnd[b] < 0) {
                    continue;
                }
                m_boneClass[b] = static_cast<char>(BoneClass::Carpal);
                if (end >= 0) {
                    m_boneLimbEnd[b] = end;
                }
                carpals += 1;
                digits -= was == BoneClass::Digit ? 1 : 0;
            }
        }
    }
    // A HELPER by the rig standard's roles (rigroles.h) that is no twist bone — a game rig's
    // corrective "share" bone beside a knee or an elbow, a control, a skinless leaf — is the
    // flesh of the JOINT IT HANGS FROM, not a joint to pose: a click on one selects that joint
    // (the first non-helper ancestor), as a click on a carpal selects the hand, and it lights
    // with it. Left a Body bone, a click on the knee grabbed the share bone, whose token mass
    // the rig promoted to the limb's END — the foot — and a knee raise moved the hips 0.000.
    int helpers = 0;
    if (rig.roles().built()) {
        for (std::size_t b = 0; b < n; ++b) {
            if (m_boneClass[b] != static_cast<char>(BoneClass::Body) || rig.roles().role(static_cast<int>(b)) != RigRole::Helper ||
                isTwistBone(static_cast<int>(b))) {
                continue;
            }
            int joint = m_bones[b].parent;
            while (joint >= 0 && rig.roles().role(joint) == RigRole::Helper) {
                joint = m_bones[static_cast<std::size_t>(joint)].parent;
            }
            if (joint < 0) {
                continue;
            }
            m_boneClass[b] = static_cast<char>(BoneClass::Carpal);
            m_boneLimbEnd[b] = joint;
            ++helpers;
        }
    }
    static const bool kTrace = std::getenv("POSESTUDIO_BONE_CLASS_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[bone-class] head %s: %d face-rig bones; %d digit joints:",
                     head >= 0 ? m_boneNames[static_cast<std::size_t>(head)].c_str() : "(none)", faces, digits);
        for (std::size_t b = 0; b < n; ++b) {
            if (m_boneClass[b] == static_cast<char>(BoneClass::Digit)) {
                std::fprintf(stderr, " %s", m_boneNames[b].c_str());
            }
        }
        std::fputc(10, stderr);
        // (... and what below a wrist or an ankle is NOT a digit: the hand's and the foot's own —
        // the CARPALS, each selecting its limb end.)
        std::fprintf(stderr, "[bone-class] %d carpals, of the hands and feet themselves, and %d helpers of their joints:", carpals, helpers);
        for (std::size_t b = 0; b < n; ++b) {
            if (m_boneClass[b] == static_cast<char>(BoneClass::Carpal)) {
                std::fprintf(stderr, " %s->%s", m_boneNames[b].c_str(),
                             m_boneLimbEnd[b] >= 0 ? m_boneNames[static_cast<std::size_t>(m_boneLimbEnd[b])].c_str() : "?");
            }
        }
        std::fputc(10, stderr);
    }
}

BoneClass Armature::boneClass(int bone) {
    if (bone < 0 || bone >= static_cast<int>(m_bones.size()) || !ensureIkRig()) {
        return BoneClass::Body;
    }
    if (m_boneClass.size() != m_bones.size()) {
        classifyBones();
    }
    return static_cast<BoneClass>(m_boneClass[static_cast<std::size_t>(bone)]);
}

int Armature::posingBone(int bone) {
    const BoneClass cls = boneClass(bone);
    if (cls == BoneClass::Face) {
        return m_ikRig->headNode();
    }
    if (cls == BoneClass::Carpal && static_cast<std::size_t>(bone) < m_boneLimbEnd.size() && m_boneLimbEnd[static_cast<std::size_t>(bone)] >= 0) {
        return m_boneLimbEnd[static_cast<std::size_t>(bone)]; // (the hand, the foot)
    }
    return bone;
}

int Armature::knownPosingBone(int bone) const {
    if (!m_ikRig || bone < 0 || static_cast<std::size_t>(bone) >= m_boneClass.size()) {
        return bone;
    }
    const char cls = m_boneClass[static_cast<std::size_t>(bone)];
    if (cls == static_cast<char>(BoneClass::Face)) {
        return m_ikRig->headNode();
    }
    if (cls == static_cast<char>(BoneClass::Carpal) && static_cast<std::size_t>(bone) < m_boneLimbEnd.size() &&
        m_boneLimbEnd[static_cast<std::size_t>(bone)] >= 0) {
        return m_boneLimbEnd[static_cast<std::size_t>(bone)];
    }
    return bone;
}

bool Armature::highlightedWithSelection(int bone) const {
    if (bone < 0) {
        return false;
    }
    // THE GROUP's secondaries light with the selection, each with its highlight twin (a bend bone's
    // twist child and vice versa: the whole upper arm for a grabbed shoulder) — and a secondary
    // head lights its face rig as the selected head does.
    for (const int member : m_groupBones) {
        if (member == bone || (member >= 0 && static_cast<std::size_t>(member) < m_highlightTwin.size() &&
                               m_highlightTwin[static_cast<std::size_t>(member)] == bone)) {
            return true;
        }
    }
    if (!m_ikRig || static_cast<std::size_t>(bone) >= m_boneClass.size()) {
        return false;
    }
    // A CARPAL lights with its hand or foot (the palm is the carpals' flesh).
    if (m_boneClass[static_cast<std::size_t>(bone)] == static_cast<char>(BoneClass::Carpal)) {
        const int end = static_cast<std::size_t>(bone) < m_boneLimbEnd.size() ? m_boneLimbEnd[static_cast<std::size_t>(bone)] : -1;
        return end >= 0 && ((highlightBone() >= 0 && highlightBone() == end) || inGroup(end));
    }
    if (m_boneClass[static_cast<std::size_t>(bone)] != static_cast<char>(BoneClass::Face)) {
        return false;
    }
    if (highlightBone() >= 0 && highlightBone() == m_ikRig->headNode()) {
        return true;
    }
    return inGroup(m_ikRig->headNode());
}

} // namespace pose
