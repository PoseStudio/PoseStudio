/**
 * @file armaturebreastfield.cpp
 * @brief THE BONELESS BREAST (2026-11-10; the user, of a character rigged on an animation
 *        package's biped: "it does not seem to have soft body physics working, however other FBX
 *        characters I loaded do … I would like it to work for all characters"): the soft breasts
 *        (armaturebreasts.cpp) turn the bone that carries a breast — the rig standard's Pectoral
 *        role — and a rig that has no such bone had none. Bipeds, motion-capture skeletons, most
 *        game mannequins, scans rigged automatically and avatar formats skin the breasts to the
 *        SPINE; there the breast is the chest's own skin, and its mass moves it as a SOFT-BODY
 *        DELTA FIELD, the buttocks' kind (armaturebuttocks.cpp): per breast three correctives —
 *        a delta of the field's weight along the chest's lateral axis, along the chest, and out
 *        along the tether — on every render vertex the field reaches, which the GPU blends before
 *        skinning at the mass's displacement (softBodyWeight). The MASS is armaturebreasts.cpp's,
 *        unchanged: its fullness, its rest under the pose's gravity, its spring, floor, wall and
 *        the landing's kick; only what carries the displacement to the skin differs.
 *
 * WHERE THE BREAST IS, per side: the APEX is the furthest point forward of the trunk's skin (the
 * spine links', their riders', the collars' and the neck's) between kFieldBandLow and
 * kFieldBandHigh of the height from the hips to the shoulder socket, off the midline and inside
 * the socket. Its PROFILE — the frontmost skin per centimetre of height within kFieldProfileBand of
 * the apex's x — is walked DOWN to the FOLD: the first centimetre where the skin has come
 * kFieldFoldDrop back from the apex and falls no further (the ribs under the breast). A bump with
 * no fold is none: skin that stands further forward just below the apex is a belly's upper slope
 * (the search goes on above it), and a profile that keeps falling to the pelvis is the belly
 * itself. THE SIZE is the profile's bulge beyond the line from the fold to the upper chest
 * (kFieldTopBin over the apex), in cm² figure-scaled — the bone path's measure in kind, read off a
 * silhouette where that one reads a patch — mapped onto its fullness scale by the native figures
 * (2026-11-10: 24 characters of one generation read both ways; rms 0.2 between the two): under
 * kFieldAreaFlat the chest is flat (the base males read 14-17, a muscular one 17.3: no field),
 * kFieldAreaFull is the base female (1.0), and a large bust runs on at kFieldAreaAbove per unit.
 *
 * WHICH BREASTS ARE FIELDS: one whose apex NO pectoral bone of its side carries (the skin within
 * kFieldCarriedRadius of it is the trunk's). A native figure's and a character-creation rig's
 * breast is carried by its bone and keeps the bone path, untouched; a rig with no pectoral bone,
 * or one whose only chest riders are helpers beside the breast (a latissimus bone, a rib twist, a
 * cape's), gets the field. The two sides are one body: both boneless, they share the mean
 * fullness and reach, and a side whose own reading found no bump mirrors the other's.
 *
 * THE FIELD'S SHAPE (breastFieldWeights) is a pectoral bone's skin weights, reconstructed: in
 * the chest's frontal plane about the apex, 1 over the breast's core (kFieldPlateau of the reach)
 * and falling SMOOTHLY AND WIDELY to 0 — at the sternum (the reach inward is the apex's distance
 * from the midline: the two fields meet at nothing), kFieldReachBelowFold under the fold, toward
 * the armpit and up the chest — and only on the chest's front and the breast's side (a soft gate
 * behind the wall under the fold). Measured on the native figures, a pectoral's half-weight line
 * runs INSIDE the breast (half way from the apex to the midline, four fifths of the way down to the
 * fold): a breast bone's weights fade across the breast's edge, which is why a rigid patch slides
 * without creasing. Three shapes were looked at on the first boneless character (on her back, on
 * her side, bowed): a tight bump ending AT the fold and the sternum hooked the breast's underside
 * over a 36 mm spread; a weight by how far the skin stands off the chest wall (the mound sheared
 * from its base) collapsed its outer side into a ridge and its top into a bowl; the wide bump is
 * what stands, and on the base female and a large bust it reads as the bone path does, side by
 * side (POSESTUDIO_BREAST_AS_FIELD: every breast a field, the A/B on one figure). Clothing skinned
 * with the chest moves with it.
 *
 * THE SKIN MUST NOT FOLD OVER ITSELF: a rigid patch slides as far as it is asked, but a field is
 * a map of the skin onto itself, one-to-one only while the displacement is smaller than the band
 * the weight falls off over. ACROSS the chest the field therefore carries the mass's displacement
 * only up to kFieldTravelShare of that band, softly (breastFieldDisplacement) — the upper breast
 * of a figure on her side drapes 2-3 cm where a bone carries it 6 — while along the tether it is
 * carried whole (pulled out the skin stretches; pressed in, the mass's own limit holds, and a
 * field FLATTENS the dome where a rigid patch can only sink).
 *
 * The fields exist from the import whether or not the Soft Body Physics tab has the breasts on
 * (the vertex data is baked once); off, their weights read 0. POSESTUDIO_BREAST_TRACE=1 prints
 * each side's reading and what was decided; POSESTUDIO_BREAST_FIELD_PLATEAU / _OUT / _UP / _BELOW
 * (mm) / _DEPTH_FULL / _DEPTH_GONE (mm) / _TRAVEL are the shape's levers, read once.
 */

#include "armature.h"

#include "ikrig.h"
#include "rigroles.h"
#include "softbodysettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pose {

namespace {

constexpr float kFieldBandLow = 0.50f;        ///< The apex is looked for from this share of the height from the hips to the shoulder socket (the under-bust line on the figures measured) ...
constexpr float kFieldBandHigh = 0.90f;       ///< ... to this (the apexes read 0.62-0.72).
constexpr float kFieldLateralIn = 0.20f;      ///< ... further from the midline than this share of the socket's distance from it ...
constexpr float kFieldLateralOut = 1.00f;     ///< ... and inside the socket (the apexes read 0.40-0.59).
constexpr float kFieldFoldLowest = 0.40f;     ///< A fold below this share of that height is under a belly, not a breast.
constexpr float kFieldBin = 0.01f;            ///< The profile's bins of height (m, figure-scaled).
constexpr int   kFieldBinsDown = 13;          ///< The profile is kept this many bins below the apex ...
constexpr int   kFieldWalkDown = 12;          ///< ... and the fold looked for within this many.
constexpr int   kFieldTopBin = 10;            ///< The wall line's upper end: the upper chest this many bins over the apex (the mean of the three bins about it).
constexpr int   kFieldGapBins = 3;            ///< Empty bins between two rows of a coarse mesh's vertices are bridged up to this many.
constexpr float kFieldProfileBand = 0.03f;    ///< The profile is read within this of the apex's x (m, figure-scaled): the bone path's kBulgeBand.
constexpr float kFieldFoldDrop = 0.004f;      ///< THE FOLD: the first bin below the apex where the skin has come this far back from it (m, figure-scaled; the apex's own plateau is not a fold) ...
constexpr float kFieldFoldFlat = 0.0015f;     ///< ... and the bin below falls back no further than this.
constexpr float kFieldRise = 0.001f;          ///< Skin this far FORWARD of the apex just below it: the apex is no bump (a belly's upper slope).
constexpr int   kFieldAttempts = 12;          ///< The search above a rejected apex is tried this many times ...
constexpr float kFieldRetryStep = 0.03f;      ///< ... each from this far over the last (m, figure-scaled).
constexpr float kFieldAreaFlat = 18.0f;       ///< THE PROFILE'S BULGE (cm², figure-scaled) under which the chest is flat: no field (the base males 14-17, a muscular one 17.3; the smallest female busts measured read 17.9 — "none for very small").
constexpr float kFieldAreaFull = 39.0f;       ///< ... at which the breast is the base female's: fullness 1 (she reads 39.1 this way, 40 the bone path's).
constexpr float kFieldAreaAbove = 44.0f;      ///< ... and above it, this many cm² per unit of fullness more (the silhouette grows faster than the bone path's patch mean: the fullest bust measured reads 78.9 here and 1.9 there).
constexpr float kFieldFullnessMax = 3.0f;     ///< The bone path's kBreastFullnessMax.
constexpr float kFieldCarriedRadius = 0.025f; ///< A PECTORAL BONE CARRIES THE BREAST when it owns kFieldCarriedShare of the skin within this of the apex (m, figure-scaled) ...
constexpr float kFieldCarriedShare = 0.25f;   ///< ... (a native pectoral owns all of it; a helper beside the breast none).
// THE FIELD'S SHAPE, about the apex in the chest's frontal plane.
constexpr float kFieldReachMin = 0.04f;       ///< No reach under this (m, figure-scaled) ...
constexpr float kFieldReachMax = 0.14f;       ///< ... or over this.

float fieldEnv(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}
const float kFieldReachOut = fieldEnv("POSESTUDIO_BREAST_FIELD_OUT", 1.5f);       ///< Its reach toward the armpit, as a share of the apex's distance from the midline (its reach toward the sternum is that distance itself: the two fields meet at nothing).
const float kFieldReachUp = fieldEnv("POSESTUDIO_BREAST_FIELD_UP", 1.5f);         ///< ... and up the chest.
const float kFieldReachBelowFold = fieldEnv("POSESTUDIO_BREAST_FIELD_BELOW", 30.0f) / 1000.0f; ///< Its reach down: this far below the fold (m, figure-scaled).
const float kFieldPlateau = fieldEnv("POSESTUDIO_BREAST_FIELD_PLATEAU", 0.25f);   ///< The share of the reach over which the field is 1: the breast's core moves as one piece.
const float kFieldDepthFull = fieldEnv("POSESTUDIO_BREAST_FIELD_DEPTH_FULL", 15.0f) / 1000.0f; ///< The field is whole on skin no further than this behind the wall under the fold (m, figure-scaled) ...
const float kFieldDepthGone = fieldEnv("POSESTUDIO_BREAST_FIELD_DEPTH_GONE", 70.0f) / 1000.0f; ///< ... and gone this far behind it: the chest's front and the breast's side, never the back.
const float kFieldTravelShare = fieldEnv("POSESTUDIO_BREAST_FIELD_TRAVEL", 0.5f); ///< THE SKIN DOES NOT FOLD OVER ITSELF: the field carries a displacement across the chest only up to this share of the band it falls off over, that way (softly: a rigid patch slides as far as it is asked, a field must stay a one-to-one map of the skin).

const bool kFieldTrace = std::getenv("POSESTUDIO_BREAST_TRACE") != nullptr;
const bool kFieldAlways = std::getenv("POSESTUDIO_BREAST_AS_FIELD") != nullptr; ///< The A/B: every breast a field, whatever bone carries it (the bone path beside the field's, on one figure).

/// The fullness a profile's bulge reads as (0 flat .. 1 the base female .. kFieldFullnessMax).
float fieldFullness(float areaCm2) {
    if (areaCm2 <= kFieldAreaFlat) {
        return 0.0f;
    }
    if (areaCm2 <= kFieldAreaFull) {
        return (areaCm2 - kFieldAreaFlat) / (kFieldAreaFull - kFieldAreaFlat);
    }
    return std::min(kFieldFullnessMax, 1.0f + (areaCm2 - kFieldAreaFull) / kFieldAreaAbove);
}

float smooth01(float edge0, float edge1, float x) {
    const float t = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

std::vector<glm::vec3> Armature::bindJointPositions() const {
    std::vector<glm::vec3> bind(m_bones.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        const Bone& b = m_bones[i];
        bind[i] = (b.parent >= 0 ? bind[static_cast<std::size_t>(b.parent)] : glm::vec3(0.0f)) + glm::vec3(b.localBind[3]);
    }
    return bind;
}

void Armature::setupBreastFields() {
    m_breastFields[0] = BreastField{};
    m_breastFields[1] = BreastField{};
    m_breastFieldSkin.clear();
    m_breastFieldsReady = false;
    if (!m_ikRig || !m_ikRig->roles().built() || m_bones.empty() || m_bodyMesh.empty()) {
        return;
    }
    m_breastFieldsReady = true; // (it ran: a side with no field stays invalid)
    const RigRoles& roles = m_ikRig->roles();
    const float     scale = m_ikRig->sizeScale() > 0.0f ? m_ikRig->sizeScale() : 1.0f;
    const int       root = m_ikRig->rootNode();
    if (root < 0 || static_cast<std::size_t>(root) >= m_bones.size()) {
        return;
    }
    const std::vector<glm::vec3> bind = bindJointPositions();
    const glm::vec3              hips = bind[static_cast<std::size_t>(root)];
    // THE TRUNK'S SKIN: the spine links', their riders' (a pectoral of either side, a centre leaf),
    // the collars' and the neck's — what a chest's front is made of, on any rig — and that of the
    // helpers and unplaced bones that hang from those (a breast bone under a "breast root", a
    // pectoral helper under the clavicle: no role of their own, the chest's skin all the same). The
    // arms' skin and the head's (hair hanging over the chest) are not: every bone of a limb or the
    // face carries a role of its own.
    m_breastFieldSkin.assign(m_bones.size(), 0);
    for (std::size_t i = 0; i < m_bones.size(); ++i) {
        int     bone = static_cast<int>(i);
        RigRole role = roles.role(bone);
        while ((role == RigRole::Helper || role == RigRole::Other) && m_bones[static_cast<std::size_t>(bone)].parent >= 0) {
            bone = m_bones[static_cast<std::size_t>(bone)].parent;
            role = roles.role(bone);
        }
        if (role == RigRole::Spine || role == RigRole::Pectoral || role == RigRole::Collar || role == RigRole::Neck) {
            m_breastFieldSkin[i] = 1;
        }
    }
    const auto trunk = [&](int bone) {
        return bone >= 0 && static_cast<std::size_t>(bone) < m_breastFieldSkin.size() && m_breastFieldSkin[static_cast<std::size_t>(bone)] != 0;
    };

    // ONE READING PER SIDE (the file comment: the apex, its profile, the fold, the bulge).
    struct Reading {
        bool      found = false; ///< A bump with a fold.
        glm::vec3 apex{0.0f};
        int       apexBone = -1;
        int       foldBin = 0;      ///< Bins below the apex (negative).
        float     foldZ = 0.0f;
        float     area = 0.0f;      ///< cm², figure-scaled.
        int       attempts = 0;
        int       carrier = -1;     ///< The pectoral bone of this side that carries the apex's skin, if one does.
    };
    constexpr float kNone = -std::numeric_limits<float>::max();
    const float     bin = kFieldBin * scale;
    const auto      read = [&](RigSide side) {
        Reading   r;
        const int socketBone = roles.find(RigRole::UpperArm, side);
        if (socketBone < 0 || static_cast<std::size_t>(socketBone) >= bind.size()) {
            return r;
        }
        const glm::vec3 socket = bind[static_cast<std::size_t>(socketBone)];
        const float     sign = side == RigSide::Left ? 1.0f : -1.0f; // (her left is +x)
        const float     span = socket.y - hips.y;
        const float     reach = (socket.x - hips.x) * sign;
        if (span < 0.10f * scale || reach < 0.03f * scale) {
            return r;
        }
        float       low = hips.y + kFieldBandLow * span;
        const float high = hips.y + kFieldBandHigh * span;
        for (int attempt = 0; attempt < kFieldAttempts && low < high; ++attempt) {
            r.attempts = attempt + 1;
            // THE APEX: the trunk's furthest skin forward in the band.
            float     zApex = kNone;
            glm::vec3 apex(0.0f);
            int       apexBone = -1;
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (!trunk(p.bone) || p.pos.y <= low || p.pos.y >= high || p.pos.z <= zApex) {
                    continue;
                }
                const float lateral = (p.pos.x - hips.x) * sign;
                if (lateral <= kFieldLateralIn * reach || lateral >= kFieldLateralOut * reach) {
                    continue;
                }
                zApex = p.pos.z;
                apex = p.pos;
                apexBone = p.bone;
            }
            if (apexBone < 0) {
                return r;
            }
            // ITS PROFILE: the frontmost skin per bin of height at the apex's x.
            std::array<float, kFieldBinsDown + kFieldTopBin + 2> profile;
            profile.fill(kNone);
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (!trunk(p.bone) || std::abs(p.pos.x - apex.x) >= kFieldProfileBand * scale || p.pos.z <= hips.z) {
                    continue;
                }
                const int k = static_cast<int>(std::lround((p.pos.y - apex.y) / bin));
                if (k < -kFieldBinsDown || k > kFieldTopBin + 1) {
                    continue;
                }
                float& slot = profile[static_cast<std::size_t>(k + kFieldBinsDown)];
                slot = std::max(slot, p.pos.z);
            }
            // A coarse mesh (a game character's vertices stand two or three centimetres apart) leaves
            // bins empty between rows of vertices: a gap of up to kFieldGapBins is the line between
            // its neighbours.
            for (std::size_t i = 1; i + 1 < profile.size(); ++i) {
                if (profile[i] > kNone || profile[i - 1] <= kNone) {
                    continue;
                }
                std::size_t next = i;
                while (next < profile.size() && profile[next] <= kNone) {
                    ++next;
                }
                if (next < profile.size() && next - i <= static_cast<std::size_t>(kFieldGapBins)) {
                    const float from = profile[i - 1], to = profile[next];
                    for (std::size_t g = i; g < next; ++g) {
                        profile[g] = from + (to - from) * static_cast<float>(g - i + 1) / static_cast<float>(next - i + 1);
                    }
                }
                i = next;
            }
            const auto at = [&](int k) { return profile[static_cast<std::size_t>(k + kFieldBinsDown)]; };
            const auto has = [&](int k) { return k >= -kFieldBinsDown && k <= kFieldTopBin + 1 && at(k) > kNone; };
            // THE FOLD: down from the apex to where the skin, come back from it, falls no further.
            int  fold = 0;
            bool closed = false;
            for (int k = -1; k >= -kFieldWalkDown; --k) {
                if (!has(k) || at(k) > apex.z + kFieldRise * scale) {
                    break; // (the skin ends, or stands forward of the apex below it: no bump here)
                }
                if (apex.z - at(k) >= kFieldFoldDrop * scale && has(k - 1) && at(k - 1) >= at(k) - kFieldFoldFlat * scale) {
                    fold = k;
                    closed = true;
                    break;
                }
            }
            if (closed && apex.y + static_cast<float>(fold) * bin > hips.y + kFieldFoldLowest * span) {
                // THE BULGE beyond the wall line, fold to upper chest.
                float zTop = 0.0f;
                int   tops = 0;
                for (int k = kFieldTopBin - 1; k <= kFieldTopBin + 1; ++k) {
                    if (has(k)) {
                        zTop += at(k);
                        ++tops;
                    }
                }
                if (tops == 0) {
                    return r; // (no upper chest over the bump: not a chest this reading understands)
                }
                zTop /= static_cast<float>(tops);
                const float zFold = at(fold);
                float       bulge = 0.0f;
                for (int k = fold; k <= kFieldTopBin; ++k) {
                    if (has(k)) {
                        const float zLine = zFold + (zTop - zFold) * static_cast<float>(k - fold) / static_cast<float>(kFieldTopBin - fold);
                        bulge += std::max(0.0f, at(k) - zLine) * bin;
                    }
                }
                r.found = true;
                r.apex = apex;
                r.apexBone = apexBone;
                r.foldBin = fold;
                r.foldZ = zFold;
                r.area = bulge * 1e4f / (scale * scale);
                // WHO CARRIES IT: the skin about the apex, by owner.
                int              all = 0;
                std::vector<int> owned(m_bones.size(), 0);
                const float      radius2 = kFieldCarriedRadius * scale * kFieldCarriedRadius * scale;
                for (const BodyMeshPoint& p : m_bodyMesh) {
                    const glm::vec3 d = p.pos - apex;
                    if (!trunk(p.bone) || glm::dot(d, d) > radius2) {
                        continue;
                    }
                    ++all;
                    ++owned[static_cast<std::size_t>(p.bone)];
                }
                for (const int bone : roles.all(RigRole::Pectoral, side)) {
                    if (bone >= 0 && static_cast<std::size_t>(bone) < owned.size() &&
                        static_cast<float>(owned[static_cast<std::size_t>(bone)]) >= kFieldCarriedShare * static_cast<float>(all) &&
                        (r.carrier < 0 || owned[static_cast<std::size_t>(bone)] > owned[static_cast<std::size_t>(r.carrier)])) {
                        r.carrier = bone;
                    }
                }
                return r;
            }
            low = apex.y + kFieldRetryStep * scale; // (above the rejected apex)
        }
        return r;
    };
    Reading reading[2] = {read(RigSide::Left), read(RigSide::Right)};
    // A side with ANY pectoral bone that carries skin and no bump of its own stays the bone path's
    // (a native male: his pectoral is the chest's whole front, and reads flat there).
    bool boneless[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        const RigSide side = s == 0 ? RigSide::Left : RigSide::Right;
        if (reading[s].found) {
            boneless[s] = reading[s].carrier < 0 || kFieldAlways;
            continue;
        }
        bool pectoralSkin = false;
        for (const int bone : roles.all(RigRole::Pectoral, side)) {
            for (const BodyMeshPoint& p : m_bodyMesh) {
                if (p.bone == bone) {
                    pectoralSkin = true;
                    break;
                }
            }
            if (pectoralSkin) {
                break;
            }
        }
        boneless[s] = !pectoralSkin;
    }
    if (kFieldTrace) {
        for (int s = 0; s < 2; ++s) {
            const Reading& r = reading[s];
            if (!r.found) {
                std::fprintf(stderr, "[breast-field] %s no bump with a fold on the trunk's skin (%d tries)%s\n", s == 0 ? "L" : "R", r.attempts,
                             boneless[s] ? "" : "; the side has a pectoral bone: the bone path's");
                continue;
            }
            std::fprintf(stderr,
                         "[breast-field] %s apex %.0f mm out, %.3f m up, %.0f mm forward (skin of %s); fold %d cm below at %.0f mm; profile %.1f cm2 -> fullness %.2f; %s%s\n",
                         s == 0 ? "L" : "R", std::abs(r.apex.x - hips.x) * 1000.0f, r.apex.y, r.apex.z * 1000.0f,
                         m_boneNames[static_cast<std::size_t>(r.apexBone)].c_str(), -r.foldBin, r.foldZ * 1000.0f, r.area, fieldFullness(r.area),
                         r.carrier >= 0 ? "carried by " : "carried by no pectoral bone: a field",
                         r.carrier >= 0 ? m_boneNames[static_cast<std::size_t>(r.carrier)].c_str() : "");
        }
    }
    if (!boneless[0] && !boneless[1]) {
        return;
    }
    // ONE BODY: both sides boneless, a side with no bump of its own mirrors the other's, and the
    // two share the mean fullness and reach (a profile is read off a centimetre grid and whatever
    // is worn; the body's two sides are the same).
    if (boneless[0] && boneless[1] && reading[0].found != reading[1].found) {
        const int from = reading[0].found ? 0 : 1;
        Reading   mirrored = reading[from];
        mirrored.apex.x = 2.0f * hips.x - mirrored.apex.x;
        reading[1 - from] = mirrored;
    }
    float size[2] = {0.0f, 0.0f};
    float down[2] = {0.0f, 0.0f};
    for (int s = 0; s < 2; ++s) {
        if (boneless[s] && reading[s].found) {
            size[s] = fieldFullness(reading[s].area);
            down[s] = static_cast<float>(-reading[s].foldBin) * bin;
        }
    }
    if (boneless[0] && boneless[1] && reading[0].found && reading[1].found) {
        size[0] = size[1] = 0.5f * (size[0] + size[1]);
        down[0] = down[1] = 0.5f * (down[0] + down[1]);
    }
    for (int s = 0; s < 2; ++s) {
        const Reading& r = reading[s];
        if (!boneless[s] || !r.found || size[s] <= 0.0f) {
            continue;
        }
        // THE CHEST: the spine link whose skin the apex is (a rider's or a collar's parent link).
        int chest = r.apexBone;
        while (chest >= 0 && roles.role(chest) != RigRole::Spine) {
            chest = m_bones[static_cast<std::size_t>(chest)].parent;
        }
        if (chest < 0) {
            continue;
        }
        BreastField& f = m_breastFields[s];
        const float  half = std::abs(r.apex.x - hips.x);
        const float  least = kFieldReachMin * scale, most = kFieldReachMax * scale;
        f.chest = chest;
        f.size = size[s];
        f.apexBind = r.apex;
        f.wallZ = r.foldZ;
        f.radiusIn = glm::clamp(half, least, most);
        f.radiusOut = glm::clamp(kFieldReachOut * half, least, most);
        f.radiusUp = glm::clamp(kFieldReachUp * half, least, most);
        f.radiusDown = glm::clamp(down[s] + kFieldReachBelowFold * scale, least, most);
        f.valid = true;
        if (kFieldTrace) {
            std::fprintf(stderr, "[breast-field] %s a field on %s: fullness %.2f; reach %.0f mm in, %.0f out, %.0f up, %.0f down; the wall %.0f mm behind the apex\n",
                         s == 0 ? "L" : "R", m_boneNames[static_cast<std::size_t>(chest)].c_str(), f.size, f.radiusIn * 1000.0f, f.radiusOut * 1000.0f,
                         f.radiusUp * 1000.0f, f.radiusDown * 1000.0f, (f.apexBind.z - f.wallZ) * 1000.0f);
        }
    }
}

void Armature::breastFieldWeights(const glm::vec3& bindPos, int dominantBone, float out[2]) const {
    out[0] = out[1] = 0.0f;
    if (!m_breastFieldsReady || dominantBone < 0 || static_cast<std::size_t>(dominantBone) >= m_breastFieldSkin.size() ||
        m_breastFieldSkin[static_cast<std::size_t>(dominantBone)] == 0) {
        return;
    }
    const float scale = m_ikRig && m_ikRig->sizeScale() > 0.0f ? m_ikRig->sizeScale() : 1.0f;
    for (int s = 0; s < 2; ++s) {
        const BreastField& f = m_breastFields[s];
        if (!f.valid) {
            continue;
        }
        const float outward = (bindPos.x - f.apexBind.x) * (s == 0 ? 1.0f : -1.0f);
        const float up = bindPos.y - f.apexBind.y;
        const float dx = outward / (outward < 0.0f ? f.radiusIn : f.radiusOut);
        const float dy = up / (up < 0.0f ? f.radiusDown : f.radiusUp);
        const float d2 = dx * dx + dy * dy;
        if (d2 >= 1.0f) {
            continue;
        }
        // 1 over the core, smoothly to 0 at the reach; and only on the chest's front.
        const float across = 1.0f - smooth01(kFieldPlateau, 1.0f, std::sqrt(d2));
        const float front = smooth01(f.wallZ - kFieldDepthGone * scale, f.wallZ - kFieldDepthFull * scale, bindPos.z);
        out[s] = across * front;
    }
}

float Armature::breastFieldMoveMm(int side) const {
    const glm::vec3 d(breastFieldDisplacement(side, 0), breastFieldDisplacement(side, 1), breastFieldDisplacement(side, 2));
    return 1000.0f * glm::length(d);
}

float Armature::breastFieldDisplacement(int side, int axis) const {
    if (!softbody::breastMotion() || !m_breastsReady || axis < 0 || axis > 2) {
        return 0.0f;
    }
    for (const Breast& b : m_breasts) {
        if (b.field != side) {
            continue;
        }
        const float d = (m_breastDynamic ? b.disp : breastRestDisplacement(b))[axis];
        if (axis == 2) {
            return d; // (out along the tether the skin stretches; in, the mass's own press limit holds)
        }
        // Across the chest the field carries the mass's displacement only as far as the band it
        // falls off over allows (kFieldTravelShare of it, softly): past that the skin between the
        // breast and the still chest would be folded over itself.
        const BreastField& f = m_breastFields[side];
        const float        reach = axis == 0 ? (d * b.side < 0.0f ? f.radiusIn : f.radiusOut) : (d < 0.0f ? f.radiusDown : f.radiusUp);
        const float        most = kFieldTravelShare * (1.0f - kFieldPlateau) * reach;
        return most > 1e-5f ? most * std::tanh(d / most) : 0.0f;
    }
    return 0.0f;
}

} // namespace pose
