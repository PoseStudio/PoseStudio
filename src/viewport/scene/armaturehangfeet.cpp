/**
 * @file armaturehangfeet.cpp
 * @brief THE HANGING FEET COME DOWN FLAT: a figure lifted off the floor hangs her feet, and coming
 *        back down — dropped by the Ground button, lowered by the hips, carried down with Ctrl —
 *        they go flat again.
 *
 * The user (2026-11-12): "When a character is lifted off the ground, her feet go into a hanging tip
 * toe position; dropping back to the ground should also make their feet go back to flat on the
 * ground position."
 *
 * A lift leaves the feet in two steps: before the feet let go she is on TIPTOE (the foot rolled
 * onto its ball: the mid-foot pitched to its limit, the toes bent back against the floor), and once
 * she hangs THE DANGLE points the ankle. Let go of in the air she keeps all of it, which is right
 * while she hangs — and was wrong the moment she came down: the Ground button's fall rests the
 * pose's lowest point on the floor, so she landed standing on the tips of her toes; lowered by the
 * hips in another drag her toes touched first and the standing foot's model, which holds the heel
 * over the ball AS FOUND, kept the heels up; and a figure let go of only just off the floor counted
 * as standing on her hanging toes and was hauled down onto them by her next drag of anything.
 *
 * WHOSE FEET. Only feet still as a lift left them. When a drag ends SUSPENDED (recordHangFeet) each
 * foot's hanging pose is remembered, bone by bone: the ankle, and the bones below it that the drag
 * turned — with what FLAT is for each (the ankle level under its shin; a lower bone as it was
 * before the lift where the foot stood flat then, else straight). A foot is the lift's while its
 * channels lie on the line between the two (findHangFeet: a least-squares share of the hang, every
 * channel within a degree of it) — so it survives an undo, a pose of another limb, a landing begun
 * and let go of half way; and a foot the user has turned since, or pointed themselves, is theirs
 * and lands as it is.
 *
 * HOW. Forward kinematics on bones no solve has (a floating figure's legs ride): the foot is posed
 * a share of the way from flat to as found (poseHangFeet) —
 *  - under the Ground button's FALL by the window, over the fall's last stretch, with the drop
 *    measured to where she will STAND (her flat soles), so she lands on them and the landing's
 *    bounce absorbs it as ever;
 *  - under a DRAG of a floating figure as a post-step of the tick (landHangFeetByFloor): no more
 *    hanging than keeps the foot's lowest part kHangFeetSkim over the floor — the floor presses it
 *    flat as the body comes down — so what reaches the rig's contact band is a flat sole, and the
 *    rig lands a standing foot. The rig is told which feet hang (IkRig::setHangingFeet): they are no
 *    contacts at the drag's start, however low the toes;
 *  - found at a press with her SOLES already inside the rig's heal band — a descent let go of just
 *    over the floor — she is set down there and then (Armature::beginIkDrag): the feet flat, the
 *    figure lowered the last centimetres onto them;
 *  - under the FIGURE MOVE (Ctrl + the hips) the same way as under a drag, to the floor itself.
 * (Within the LIFTING drag the landing is the rig's own, as ever — IkRig::landAt and the standing
 * foot's model, whose references are the standing foot the drag began on.)
 * The legs stay as they hang (together, the knees soft): she lands where she hangs, as a figure
 * lowered within the lifting drag always has. Qt-free (std + GLM).
 */

#include "armature.h"
#include "armatureiksolvetuning.h"

#include "ikmath.h"
#include "ikrig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pose {

namespace {

constexpr float kHangFootTurnedDeg = 0.05f; ///< A bone of the foot the lifting drag turned by more than this is the lift's.
constexpr float kHangFootMatchDeg = 1.0f;   ///< A foot is still as the lift left it while every channel is within this of the line from flat to hanging.
constexpr float kHangFootMinShare = 0.05f;  ///< Under this share of the hang the foot is flat already: nothing to land.
constexpr float kHangFootStoodCos = 0.966f; ///< The foot stood flat when the lift began: its up axis within 15 degrees of the world's.
constexpr float kHangFootLegDown = 0.5f;    ///< The foot hangs under its leg: the shin within 60 degrees of straight down.

float largest(const glm::vec3& v) {
    return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
}

bool hangFeetTrace() {
    static const bool kTrace = std::getenv("POSESTUDIO_HANG_FEET_TRACE") != nullptr;
    return kTrace;
}

} // namespace

void Armature::recordHangFeet() {
    const std::size_t n = m_bones.size();
    if (!m_ikRig || n == 0) {
        return;
    }
    const IkRig& rig = *m_ikRig;
    if (m_hangFootKind.size() != n) {
        m_hangFootKind.assign(n, 0);
        m_hangFootEuler.assign(n, glm::vec3(0.0f));
        m_hangFootFlat.assign(n, glm::vec3(0.0f));
    }
    const bool haveHome = m_hangHomeEuler.size() == n;
    const auto turned = [&](std::size_t b) {
        return haveHome && largest(m_boneEuler[b] - m_hangHomeEuler[b]) > kHangFootTurnedDeg;
    };
    for (std::size_t a = 0; a < n; ++a) {
        const int parent = m_bones[a].parent;
        // An ANKLE: the foot-class joint of a leg whose parent is none (the dangle's own reading).
        if (parent < 0 || rig.legOf(static_cast<int>(a)) < 0 || !rig.isFootClass(static_cast<int>(a)) || rig.isFootClass(parent)) {
            continue;
        }
        std::vector<int> subtree;
        collectSubtree(static_cast<int>(a), subtree); // (the ankle first, parents before children)
        bool any = false;
        for (const int b : subtree) {
            any = any || turned(static_cast<std::size_t>(b));
        }
        if (!any && m_hangFootKind[a] == 1) {
            continue; // (it hung through this drag as an earlier lift left it: that memory stands)
        }
        glm::vec3 level(0.0f);
        if (!levelFootEuler(static_cast<int>(a), level)) {
            for (const int b : subtree) {
                m_hangFootKind[static_cast<std::size_t>(b)] = 0;
            }
            continue;
        }
        // Did the foot STAND FLAT when the drag began? Then a lower bone's flat pose is the one it
        // had (a toe the user had spread keeps its spread); out of a kneel or a hang it is straight.
        const bool stoodFlat = haveHome && a < m_jsStartRot.size() &&
                               (glm::mat3(m_transform) * m_jsStartRot[a] * glm::vec3(0.0f, 1.0f, 0.0f)).y > kHangFootStoodCos;
        for (const int bi : subtree) {
            const std::size_t b = static_cast<std::size_t>(bi);
            if (b == a) {
                continue;
            }
            if (m_bones[b].drivenBy >= 0) {
                m_hangFootKind[b] = 0; // (a driven helper follows its driver: computeSkinMatrices)
            } else if (turned(b)) {
                m_hangFootKind[b] = 2;
                m_hangFootEuler[b] = m_boneEuler[b];
                m_hangFootFlat[b] = stoodFlat ? m_hangHomeEuler[b] : glm::vec3(0.0f);
            } else if (!(m_hangFootKind[b] == 2 && largest(m_boneEuler[b] - m_hangFootEuler[b]) < kHangFootMatchDeg)) {
                m_hangFootKind[b] = 0; // (else: turned by an earlier lift, hanging since — kept)
            }
        }
        m_hangFootKind[a] = 1;
        m_hangFootEuler[a] = m_boneEuler[a];
        m_hangFootFlat[a] = level;
        if (hangFeetTrace()) {
            std::fprintf(stderr, "[hang-feet] recorded %s: hangs (%.1f %.1f %.1f), level (%.1f %.1f %.1f)%s", m_boneNames[a].c_str(),
                         m_boneEuler[a].x, m_boneEuler[a].y, m_boneEuler[a].z, level.x, level.y, level.z, stoodFlat ? "; stood flat at the press" : "");
            for (const int bi : subtree) {
                const std::size_t b = static_cast<std::size_t>(bi);
                if (m_hangFootKind[b] == 2) {
                    std::fprintf(stderr, "; %s (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)", m_boneNames[b].c_str(), m_hangFootEuler[b].x, m_hangFootEuler[b].y,
                                 m_hangFootEuler[b].z, m_hangFootFlat[b].x, m_hangFootFlat[b].y, m_hangFootFlat[b].z);
                }
            }
            std::fputc(10, stderr);
        }
    }
}

bool Armature::levelFootEuler(int ankle, glm::vec3& euler) const {
    if (ankle < 0 || static_cast<std::size_t>(ankle) >= m_bones.size() || m_poseGlobal.size() != m_bones.size()) {
        return false;
    }
    const std::size_t a = static_cast<std::size_t>(ankle);
    const Bone&       bone = m_bones[a];
    // The LEVEL rotation of the foot (binds are translation-only: poseGlobal's rotation part is the
    // pose's, and the identity is the foot as it stands at bind — flat): up straight up, and its
    // heading the SHIN's — the lateral axis of the bone it hangs from, laid in the floor's plane:
    // the foot points where its knee points. NOT the foot's own lateral axis: that turns a little
    // as the foot is posed between hanging and flat, so "flat" moved under the pose it was read
    // from — and a foot let go of half way down was no longer on the line from hanging to flat it
    // is recognised by (4 degrees off: left pointed, and healed onto its toes). With W = T * P * OR
    // * R * OR^-1 (T the model transform's rotation, P the parent's), R' = OR^-1 * P^-1 * T^-1 * W'
    // * OR, read back on the branch the ankle is on.
    const glm::mat3 model(m_transform);
    glm::mat3       parentRot(1.0f);
    if (bone.parent >= 0) {
        parentRot = glm::mat3(m_poseGlobal[static_cast<std::size_t>(bone.parent)]);
    }
    glm::vec3 lateral = model * parentRot * glm::vec3(1.0f, 0.0f, 0.0f);
    lateral.y = 0.0f;
    if (glm::length(lateral) < 0.3f) {
        return false; // (the leg on its side: no heading to stand on)
    }
    lateral = glm::normalize(lateral);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::mat3 level(lateral, up, glm::cross(lateral, up));
    const glm::mat3 local = glm::mat3(bone.invOrient) * glm::transpose(parentRot) * glm::inverse(model) * level * glm::mat3(bone.orient);
    euler = eulerFromMatrixNear(local, bone.rotationOrder, m_boneEuler[a]);
    for (int c = 0; c < 3; ++c) {
        if (bone.rotLimited[c]) {
            euler[c] = glm::clamp(euler[c], bone.rotMin[c], bone.rotMax[c]);
        }
    }
    return true;
}

void Armature::findHangFeet(std::vector<HangFoot>& out, std::vector<char>* flags) const {
    out.clear();
    const std::size_t n = m_bones.size();
    if (flags != nullptr) {
        flags->assign(n, 0);
    }
    static const bool kOff = std::getenv("POSESTUDIO_NO_HANG_FEET") != nullptr; // A/B probe: the feet land as they hang, as before 2026-11-12
    if (kOff || !m_ikRig || n == 0 || m_hangFootKind.size() != n || m_boneWorldPos.size() != n) {
        return;
    }
    const float scale = m_ikRig->sizeScale();
    for (std::size_t a = 0; a < n; ++a) {
        if (m_hangFootKind[a] != 1 || m_bones[a].parent < 0) {
            continue;
        }
        // Under a leg that points DOWN (the joint above that stands off the ankle: past a twist
        // helper at the ankle's own place). A figure laid on her side in the air does not land on
        // her soles.
        int up = m_bones[a].parent;
        while (up >= 0 && glm::length(m_boneWorldPos[static_cast<std::size_t>(up)] - m_boneWorldPos[a]) < 0.05f * scale) {
            up = m_bones[static_cast<std::size_t>(up)].parent;
        }
        if (up < 0) {
            continue;
        }
        const glm::vec3 shin = glm::normalize(m_boneWorldPos[a] - m_boneWorldPos[static_cast<std::size_t>(up)]);
        if (-shin.y < kHangFootLegDown) {
            continue;
        }
        std::vector<int> subtree;
        collectSubtree(static_cast<int>(a), subtree);
        // How much of the hang the foot still has: the least-squares share of its channels on the
        // line from flat to hanging — and every channel within a degree of it, or the foot has
        // been posed since and is the user's.
        double num = 0.0, den = 0.0;
        for (const int bi : subtree) {
            const std::size_t b = static_cast<std::size_t>(bi);
            if (m_hangFootKind[b] == 0 || (b != a && m_hangFootKind[b] != 2)) {
                continue;
            }
            const glm::vec3 line = m_hangFootEuler[b] - m_hangFootFlat[b];
            num += static_cast<double>(glm::dot(m_boneEuler[b] - m_hangFootFlat[b], line));
            den += static_cast<double>(glm::dot(line, line));
        }
        if (den < 1.0e-6) {
            continue; // (it never hung any way but flat)
        }
        const float share = static_cast<float>(glm::clamp(num / den, 0.0, 1.0));
        bool        theirs = share < kHangFootMinShare;
        for (const int bi : subtree) {
            const std::size_t b = static_cast<std::size_t>(bi);
            if (theirs || m_hangFootKind[b] == 0 || (b != a && m_hangFootKind[b] != 2)) {
                continue;
            }
            const glm::vec3 onLine = m_hangFootFlat[b] + (m_hangFootEuler[b] - m_hangFootFlat[b]) * share;
            theirs = largest(m_boneEuler[b] - onLine) > kHangFootMatchDeg;
        }
        glm::vec3 level(0.0f);
        if (theirs || !levelFootEuler(static_cast<int>(a), level)) {
            continue;
        }
        HangFoot foot;
        foot.ankle = static_cast<int>(a);
        foot.subtree = subtree;
        foot.parentSlot.assign(subtree.size(), -1);
        foot.boneSlot.assign(subtree.size(), -1);
        for (std::size_t k = 0; k < subtree.size(); ++k) {
            const std::size_t b = static_cast<std::size_t>(subtree[k]);
            for (std::size_t j = 0; j < k; ++j) {
                if (subtree[j] == m_bones[b].parent) {
                    foot.parentSlot[k] = static_cast<int>(j);
                    break;
                }
            }
            if (b == a || m_hangFootKind[b] == 2) {
                foot.boneSlot[k] = static_cast<int>(foot.bones.size());
                foot.bones.push_back(subtree[k]);
                foot.found.push_back(m_boneEuler[b]);
                foot.flat.push_back(b == a ? level : m_hangFootFlat[b]);
            }
            if (flags != nullptr) {
                (*flags)[b] = 1;
            }
        }
        if (hangFeetTrace()) {
            std::fprintf(stderr, "[hang-feet] %s still hangs as the lift left it: share %.2f, %zu bone(s)\n", m_boneNames[a].c_str(), share, foot.bones.size());
        }
        out.push_back(std::move(foot));
    }
}

bool Armature::beginHangFeetLanding() {
    m_hangFeet.clear();
    if (m_bones.empty() || !ensureIkRig() || m_ikRig->dragActive() || m_landing.active) {
        return false;
    }
    findHangFeet(m_hangFeet, nullptr);
    return !m_hangFeet.empty();
}

void Armature::hangFootEulers(const HangFoot& foot, float hang, std::vector<glm::vec3>& out) const {
    out.resize(foot.bones.size());
    for (std::size_t i = 0; i < foot.bones.size(); ++i) {
        glm::vec3 flat = foot.flat[i];
        if (i == 0) {
            levelFootEuler(foot.ankle, flat); // (level under the shin as it stands NOW; the found value's on failure)
        }
        out[i] = flat + (foot.found[i] - flat) * hang;
    }
}

float Armature::hangFootFloorGap(const HangFoot& foot, float hang) const {
    std::vector<glm::vec3> eulers;
    hangFootEulers(foot, hang, eulers);
    const float            floorModel = -m_transform[3][1]; // the floor's height in model space
    std::vector<glm::mat4> world(foot.subtree.size());
    float                  gap = 1.0e9f;
    for (std::size_t k = 0; k < foot.subtree.size(); ++k) {
        const std::size_t b = static_cast<std::size_t>(foot.subtree[k]);
        const Bone&       bone = m_bones[b];
        glm::mat4         local = bone.poseLocal;
        if (foot.boneSlot[k] >= 0) {
            const glm::vec3& e = eulers[static_cast<std::size_t>(foot.boneSlot[k])];
            local = (e.x == 0.0f && e.y == 0.0f && e.z == 0.0f) ? bone.localBind
                                                                 : bone.localBind * bone.orient * eulerMatrix(e, bone.rotationOrder) * bone.invOrient;
            local[3] += glm::vec4(m_boneTranslation[b], 0.0f);
        }
        const glm::mat4 parent = foot.parentSlot[k] >= 0 ? world[static_cast<std::size_t>(foot.parentSlot[k])]
                                                         : (bone.parent >= 0 ? m_poseGlobal[static_cast<std::size_t>(bone.parent)] : glm::mat4(1.0f));
        world[k] = parent * local;
        gap = std::min(gap, world[k][3].y - floorModel - m_ikRig->floorClearance(static_cast<int>(b)));
    }
    return gap;
}

bool Armature::poseHangFeet(float hang, float floorMargin) {
    if (m_hangFeet.empty() || !m_ikRig) {
        return false;
    }
    bool                   moved = false;
    std::vector<glm::vec3> eulers;
    for (const HangFoot& foot : m_hangFeet) {
        float share = glm::clamp(hang, 0.0f, 1.0f);
        // No more hanging than the FLOOR allows: the largest share whose lowest part still clears
        // it by the margin (the gap falls as the foot points; a flat foot that does not clear it
        // either is simply flat).
        if (floorMargin > -1.0e8f && share > 0.0f && hangFootFloorGap(foot, share) < floorMargin) {
            float lo = 0.0f, hi = share;
            for (int i = 0; i < 14; ++i) {
                const float mid = 0.5f * (lo + hi);
                (hangFootFloorGap(foot, mid) >= floorMargin ? lo : hi) = mid;
            }
            share = lo;
        }
        hangFootEulers(foot, share, eulers);
        for (std::size_t i = 0; i < foot.bones.size(); ++i) {
            const std::size_t b = static_cast<std::size_t>(foot.bones[i]);
            if (largest(m_boneEuler[b] - eulers[i]) < 1.0e-4f) {
                continue;
            }
            m_boneEuler[b] = eulers[i];
            clampBoneEuler(foot.bones[i]);
            recomposePoseLocal(b);
            moved = true;
        }
    }
    if (moved) {
        computeSkinMatrices();
    }
    return moved;
}

bool Armature::poseHangFeetForFall(float remaining, float total) {
    if (m_hangFeet.empty() || !m_ikRig) {
        return false;
    }
    // Flat over the fall's last stretch — the whole of a short fall — and eased (the share's
    // square: the feet begin to turn gently and arrive flat with the floor).
    const float span = std::min(std::max(total, 1.0e-4f), kHangFeetFallSpan * m_ikRig->sizeScale());
    const float left = glm::clamp(remaining / span, 0.0f, 1.0f);
    return poseHangFeet(left * left, 0.0f);
}

bool Armature::landHangFeetByFloor() {
    if (m_hangFeet.empty() || !m_ikRig) {
        return false;
    }
    const IkRig& rig = *m_ikRig;
    if (rig.suspended()) {
        m_hangFeet.clear(); // (lifted again: the dangle's from here, and recorded afresh at the release)
        return false;
    }
    // A foot the SOLVE has — planted by a landing, or a joint of the limb being dragged — is no
    // longer this step's.
    const std::vector<char>& active = rig.activeNodes();
    for (std::size_t f = m_hangFeet.size(); f-- > 0;) {
        const int ankle = m_hangFeet[f].ankle;
        bool      solved = static_cast<std::size_t>(ankle) < active.size() && active[static_cast<std::size_t>(ankle)] != 0;
        for (const IkEffector& pin : rig.pins()) {
            solved = solved || (pin.node >= 0 && boneUnder(pin.node, ankle));
        }
        if (solved) {
            m_hangFeet.erase(m_hangFeet.begin() + static_cast<std::ptrdiff_t>(f));
        }
    }
    const bool moved = poseHangFeet(1.0f, kHangFeetSkim * rig.sizeScale());
    // (A bone a post-step has turned is referred to where the post-step LEFT it once the solve
    // takes it over — the landed foot's ankle and toes: m_jsPostStepEuler.)
    for (const HangFoot& foot : m_hangFeet) {
        for (const int bi : foot.bones) {
            const std::size_t b = static_cast<std::size_t>(bi);
            if (b < m_jsPostStepEuler.size() && b < m_jsPostStepTurned.size()) {
                m_jsPostStepEuler[b] = m_boneEuler[b];
                m_jsPostStepTurned[b] = 1;
            }
        }
    }
    return moved;
}

} // namespace pose
