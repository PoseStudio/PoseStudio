/**
 * @file rigroles.cpp
 * @brief The rig standard's role inference (see rigroles.h): the body's roles found from the
 *        bind skeleton's structure, the report, and the reference generation's names as tags.
 *
 * THE RULES, in the order they run (each reads only what the ones before it settled):
 *  - the HIPS are the solve root; a bone is of the body if it descends from them;
 *  - the LEGS hang from the hips' children that have a foot below them — directly (a thigh each)
 *    or from one child carrying both (the PELVIS bone); down each leg's main chain (the child
 *    with the most mass below it) the twist helpers before the first real joint are THIGH TWISTS,
 *    that joint is the SHIN, the helpers after it SHIN TWISTS, and the first foot-class joint the
 *    FOOT; leaf helpers beside the chain are the segment's twists too;
 *  - under the FOOT: the first bone with two children or more is the BALL the toes fan from
 *    (else the deepest bone with a child); the chain from the ankle to it is METATARSAL; each
 *    of the ball's children starts a TOE (its digit by its name's token where it has one, else
 *    by its place across the foot — the big toe nearest the midline); a leaf under the ankle
 *    behind it is the HEEL, any other bone there a metatarsal;
 *  - the SPINE is the chain of the largest foot-free subtrees up from the hips, ending at the
 *    first link with side children of real size on both sides (the collars): the chest top;
 *  - at the top each side's heaviest child is the COLLAR; every other side child of a spine
 *    link is a PECTORAL (a skinless leaf a HELPER); down each arm's main chain: UPPER ARM, its
 *    twists, FOREARM, its twists, HAND — the twist helpers as on the legs;
 *  - under the HAND: a skinless leaf is a helper (a prop anchor); a child that carries two
 *    chains or more, or whose own chain is four bones or longer, is a CARPAL and the chains
 *    below it FINGERS; any other child starts a finger; a finger's digit is its name's token
 *    where it has one (thumb, index, mid, ring, pinky), else its place across the hand (the
 *    thumb foremost);
 *  - from the top's heaviest centre child: NECK links while a bone has one child, the first
 *    with two or more the HEAD (a chain that ends in a leaf ends in the head); under the head
 *    the EYES (a direct child named so, by side) and the JAW (the lower one), the rest FACE;
 *  - what is left of the body is a HELPER if it is a skinless leaf, else OTHER, and listed.
 *
 * Verified by construction against the eight reference generations: the roles agree with the
 * bone names those rigs use, and the engine's results are bit-identical to what the name-based
 * lookups gave before the roles (the round that introduced them, 2026-11-02).
 */

#include "rigroles.h"

#include "bodymesh.h"
#include "namerules.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <sstream>

namespace pose {

namespace {

/// A centre bone stands within this of the sagittal plane at bind (figure-scaled): the hips,
/// the spine, the neck and the head do to a tenth of a millimetre; a collar stands 3cm out.
constexpr float kSideOffPlane = 0.01f;
/// A leaf under the ankle this far BEHIND it (figure-scaled) is a heel bone; ahead, a mid-foot one.
constexpr float kHeelBehind = 0.02f;
/// A leftover bone with no real child and under this share of the body's skin is a helper, not a
/// joint the standard lacks a role for (a corrective bone beside a knee carries a few vertices).
constexpr double kHelperSkinShare = 0.002;
/// A hand child whose chain is this long or longer carries a finger below its first bone (a
/// carpal or a metacarpal): a finger itself is three joints (a thumb's first joint IS its
/// metacarpal, on every rig seen, and the rigs that author a carpal for it name it so).
constexpr int kCarpalChainLength = 4;
/// A finger base this share of the way to the next-nearest base's distance from the wrist, or
/// nearer, is the THUMB (its chain begins at the carpus; a finger's at the knuckle, a palm's
/// length out: 0.27 of the index's on the reference generation, 0.44 on a biped rig).
constexpr float kThumbNearShare = 0.8f;

} // namespace

const RigRoleTag& RigRoles::tag(int bone) const {
    static const RigRoleTag kNone;
    return bone >= 0 && static_cast<std::size_t>(bone) < m_tags.size() ? m_tags[static_cast<std::size_t>(bone)] : kNone;
}

int RigRoles::find(RigRole role, RigSide side, int index, int joint) const {
    for (std::size_t i = 0; i < m_tags.size(); ++i) {
        const RigRoleTag& t = m_tags[i];
        if (t.role == role && t.side == side && (index == 0 || t.index == index) && (joint == 0 || t.joint == joint)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<int> RigRoles::all(RigRole role) const {
    std::vector<int> out;
    for (std::size_t i = 0; i < m_tags.size(); ++i) {
        if (m_tags[i].role == role) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

std::vector<int> RigRoles::all(RigRole role, RigSide side) const {
    std::vector<int> out;
    for (std::size_t i = 0; i < m_tags.size(); ++i) {
        if (m_tags[i].role == role && m_tags[i].side == side) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

const char* RigRoles::roleName(RigRole role) {
    switch (role) {
    case RigRole::None: return "-";
    case RigRole::Hips: return "hips";
    case RigRole::Pelvis: return "pelvis";
    case RigRole::Spine: return "spine";
    case RigRole::Neck: return "neck";
    case RigRole::Head: return "head";
    case RigRole::Jaw: return "jaw";
    case RigRole::Eye: return "eye";
    case RigRole::Face: return "face";
    case RigRole::Pectoral: return "pectoral";
    case RigRole::Collar: return "collar";
    case RigRole::UpperArm: return "upper arm";
    case RigRole::UpperArmTwist: return "upper arm twist";
    case RigRole::Forearm: return "forearm";
    case RigRole::ForearmTwist: return "forearm twist";
    case RigRole::Hand: return "hand";
    case RigRole::Carpal: return "carpal";
    case RigRole::Finger: return "finger";
    case RigRole::Thigh: return "thigh";
    case RigRole::ThighTwist: return "thigh twist";
    case RigRole::Shin: return "shin";
    case RigRole::ShinTwist: return "shin twist";
    case RigRole::Foot: return "foot";
    case RigRole::Heel: return "heel";
    case RigRole::Metatarsal: return "metatarsal";
    case RigRole::Ball: return "ball";
    case RigRole::Toe: return "toe";
    case RigRole::Helper: return "helper";
    case RigRole::Other: return "other";
    case RigRole::Count: break;
    }
    return "?";
}

const char* RigRoles::sideName(RigSide side) {
    return side == RigSide::Left ? "L" : side == RigSide::Right ? "R" : "";
}

namespace {

/// The standard's word for a role that has one (the part of its name before the number and the
/// side), or "" for a role whose bones keep their own names.
const char* standardWord(RigRole role) {
    switch (role) {
    case RigRole::Hips: return "hips";
    case RigRole::Pelvis: return "pelvis";
    case RigRole::Spine: return "spine";
    case RigRole::Neck: return "neck";
    case RigRole::Head: return "head";
    case RigRole::Jaw: return "jaw";
    case RigRole::Eye: return "eye";
    case RigRole::Pectoral: return "pectoral";
    case RigRole::Collar: return "collar";
    case RigRole::UpperArm: return "upper_arm";
    case RigRole::UpperArmTwist: return "upper_arm_twist";
    case RigRole::Forearm: return "forearm";
    case RigRole::ForearmTwist: return "forearm_twist";
    case RigRole::Hand: return "hand";
    case RigRole::Carpal: return "carpal";
    case RigRole::Thigh: return "thigh";
    case RigRole::ThighTwist: return "thigh_twist";
    case RigRole::Shin: return "shin";
    case RigRole::ShinTwist: return "shin_twist";
    case RigRole::Foot: return "foot";
    case RigRole::Heel: return "heel";
    case RigRole::Metatarsal: return "metatarsal";
    case RigRole::Ball: return "ball";
    default: return "";
    }
}

/// The fingers' and the toes' words, by digit 1..5.
const char* const kFingerWords[6] = {"", "thumb", "index", "middle", "ring", "pinky"};
const char* const kToeWords[6] = {"", "bigtoe", "indextoe", "middletoe", "ringtoe", "pinkytoe"};

/// Which roles carry a number, and what it is.
bool numberIsIndex(RigRole role) {
    return role == RigRole::Spine || role == RigRole::Neck || role == RigRole::UpperArmTwist ||
           role == RigRole::ForearmTwist || role == RigRole::Carpal || role == RigRole::ThighTwist ||
           role == RigRole::ShinTwist || role == RigRole::Metatarsal;
}
bool isCentreRole(RigRole role) {
    return role == RigRole::Hips || role == RigRole::Pelvis || role == RigRole::Spine || role == RigRole::Neck ||
           role == RigRole::Head || role == RigRole::Jaw;
}

/// THE STANDARD'S GRAMMAR, read back: `<word><number>_<side>` or `<word><number>` for a centre
/// role; a None tag for anything else (the reference generation's grammar is tried after it).
RigRoleTag standardTag(const std::string& name) {
    RigRoleTag tag;
    std::string body = name;
    RigSide     side = RigSide::Centre;
    if (body.size() > 2 && body[body.size() - 2] == '_' && (body.back() == 'l' || body.back() == 'r')) {
        side = body.back() == 'l' ? RigSide::Left : RigSide::Right;
        body.resize(body.size() - 2);
    }
    std::size_t end = body.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(body[end - 1]))) {
        --end;
    }
    if (end == 0) {
        return tag;
    }
    const std::string word = body.substr(0, end);
    const int         number = end < body.size() ? std::atoi(body.c_str() + end) : 0;
    for (const char c : word) {
        if (!(std::islower(static_cast<unsigned char>(c)) || c == '_')) {
            return tag; // (the standard's names are lowercase words)
        }
    }
    const auto make = [&](RigRole role, int index, int joint) {
        tag.role = role;
        tag.side = side;
        tag.index = static_cast<unsigned char>(std::max(0, std::min(255, index)));
        tag.joint = static_cast<unsigned char>(std::max(0, std::min(255, joint)));
        return tag;
    };
    for (int d = 1; d <= 5; ++d) {
        if (word == kFingerWords[d]) {
            return side == RigSide::Centre || number < 1 ? tag : make(RigRole::Finger, d, number);
        }
        if (word == kToeWords[d]) {
            return side == RigSide::Centre || number < 1 ? tag : make(RigRole::Toe, d, number);
        }
    }
    for (int r = 1; r < static_cast<int>(RigRole::Count); ++r) {
        const RigRole role = static_cast<RigRole>(r);
        const char*   w = standardWord(role);
        if (w[0] == '\0' || word != w) {
            continue;
        }
        if (isCentreRole(role) != (side == RigSide::Centre)) {
            return tag; // (a centre role takes no side; a side role needs one)
        }
        if (numberIsIndex(role)) {
            return number < 1 ? tag : make(role, number, 0);
        }
        return number != 0 ? tag : make(role, 0, 0);
    }
    return tag;
}

} // namespace

std::string RigRoles::standardName(const RigRoleTag& tag) {
    std::string out;
    if (tag.role == RigRole::Finger || tag.role == RigRole::Toe) {
        if (tag.index < 1 || tag.index > 5 || tag.joint < 1 || tag.side == RigSide::Centre) {
            return out;
        }
        out = (tag.role == RigRole::Finger ? kFingerWords : kToeWords)[tag.index];
        out += std::to_string(static_cast<int>(tag.joint));
    } else {
        const char* w = standardWord(tag.role);
        if (w[0] == '\0') {
            return out;
        }
        if (isCentreRole(tag.role) != (tag.side == RigSide::Centre)) {
            return out;
        }
        out = w;
        if (numberIsIndex(tag.role)) {
            if (tag.index < 1) {
                return std::string();
            }
            out += std::to_string(static_cast<int>(tag.index));
        }
    }
    if (tag.side == RigSide::Left) {
        out += "_l";
    } else if (tag.side == RigSide::Right) {
        out += "_r";
    }
    return out;
}

/**
 * @brief The inference's working state and its steps, one per rule of the file comment, run in
 *        that order by run(). A nested type of RigRoles (declared private in the header), so it
 *        writes the table directly; defined here alone. Every step reads only what the steps
 *        before it settled, and what one step hands the next (the spine, the chest top, the
 *        neck, the collars) is a member. Written as one 675-line function until the IK
 *        refactoring round of 2026-11-03; the steps are its blocks verbatim.
 */
struct RigRoles::Builder {
    RigRoles&                            out;   ///< The table being built (m_tags, m_names, m_missing, the chain lengths).
    const Input&                         in;
    const std::vector<int>&              par;   ///< in.parents
    const std::vector<std::vector<int>>& ch;    ///< in.children
    const std::vector<glm::vec3>&        pos;   ///< in.bindPos
    const int                            n;     ///< The bone count.
    const int                            root;  ///< The hips.
    const float                          scale; ///< The figure's size scale (1 when the input gives none).
    float                                rootX = 0.0f; ///< The hips' bind x: the sagittal plane.

    // Derived once, in prepare().
    std::vector<char> underRoot; ///< Per bone: descends from the root (the root itself included).
    std::vector<int>  size;      ///< Per bone: its subtree's bone count (the spine rule reads them).
    std::vector<char> hasFoot;   ///< Per bone: a foot-class joint below it, or is one.
    std::vector<int>  skin;      ///< Per bone: the body mesh sample's points it dominates.
    bool              haveMesh = false;

    // What one step hands the next.
    std::vector<int> spine;                ///< The spine links, hips upward (spineChain()).
    int              top = -1;             ///< The chest top: the last spine link, or the root with no spine.
    int              neck = -1;            ///< The neck's first link (chestTop()), -1 if none.
    int              collar[2] = {-1, -1}; ///< The collars, left then right (chestTop()).

    Builder(RigRoles& table, const Input& input)
        : out(table),
          in(input),
          par(*input.parents),
          ch(*input.children),
          pos(*input.bindPos),
          n(static_cast<int>(input.parents->size())),
          root(input.root),
          scale(input.sizeScale > 0.0f ? input.sizeScale : 1.0f) {}

    /// Every rule in order (see the file comment).
    void run() {
        prepare();
        set(root, RigRole::Hips, RigSide::Centre);
        legs();
        spineChain();
        chestTop();
        neckAndHead();
        leftovers();
        required();
    }

    // ---- The readers every step shares.
    static std::size_t idx(int b) { return static_cast<std::size_t>(b); }
    bool  isBody(int b) const { return b >= 0 && b < n && (in.bodyNode == nullptr || (*in.bodyNode)[idx(b)] != 0); }
    bool  isHelper(int b) const { return in.helper != nullptr && b >= 0 && b < n && (*in.helper)[idx(b)] != 0; }
    bool  isFoot(int b) const { return in.footClass != nullptr && b >= 0 && b < n && (*in.footClass)[idx(b)] != 0; }
    float mass(int b) const { return in.subtreeMass != nullptr && b >= 0 && b < n ? (*in.subtreeMass)[idx(b)] : 1.0f; }
    RigRole            roleOf(int b) const { return out.m_tags[idx(b)].role; }
    const std::string& name(int b) const { return out.m_names[idx(b)]; }
    void               set(int b, RigRole role, RigSide side, int index = 0, int joint = 0) {
        RigRoleTag& t = out.m_tags[idx(b)];
        t.role = role;
        t.side = side;
        t.index = static_cast<unsigned char>(std::max(0, std::min(255, index)));
        t.joint = static_cast<unsigned char>(std::max(0, std::min(255, joint)));
    }
    /// A leaf no skin follows (with no mesh sample: the one such bone seen, by its name).
    bool skinlessLeaf(int b) const {
        if (!ch[idx(b)].empty()) {
            return false;
        }
        if (haveMesh) {
            return skin[idx(b)] == 0;
        }
        return namerules::hasToken(namerules::lettersLower(name(b)), "anchor");
    }
    /// The bone's side by its bind x against the hips' (Centre within kSideOffPlane).
    RigSide sideByX(int b) const {
        const float dx = pos[idx(b)].x - rootX;
        if (std::abs(dx) < kSideOffPlane * scale) {
            return RigSide::Centre;
        }
        return dx > 0.0f ? RigSide::Left : RigSide::Right;
    }
    /// How many of the bone's children are of the body and no skinless leaf.
    int realKidCount(int b) const {
        int kids = 0;
        for (const int g : ch[idx(b)]) {
            kids += isBody(g) && !skinlessLeaf(g) ? 1 : 0;
        }
        return kids;
    }
    /// The child with the most mass below it (ties: the bigger subtree, then the lower index) —
    /// a twist helper never, while a real child exists (a helper chain hanging off a thigh can
    /// carry as much name-table mass as the shin, and it is not the leg).
    bool heavier(int c, int best) const {
        return best < 0 || mass(c) > mass(best) + 1.0e-9f || (std::abs(mass(c) - mass(best)) <= 1.0e-9f && size[idx(c)] > size[idx(best)]);
    }
    int mainChild(int b) const {
        int  best = -1;
        bool bestHelper = true;
        for (const int c : ch[idx(b)]) {
            if (!isBody(c)) {
                continue;
            }
            const bool helper = isHelper(c);
            if ((bestHelper && !helper) || (helper == bestHelper && heavier(c, best))) {
                best = c;
                bestHelper = helper;
            }
        }
        return best;
    }
    /// A segment's twist helpers: every helper hanging off it, and the helpers hanging off those.
    void tagHelperChains(int segment, RigRole twistRole, RigSide side, int& count) {
        std::vector<int> queue; // (child order: the first helper listed is twist 1)
        for (const int h : ch[idx(segment)]) {
            if (isBody(h) && roleOf(h) == RigRole::None && isHelper(h)) {
                queue.push_back(h);
            }
        }
        for (std::size_t q = 0; q < queue.size(); ++q) {
            const int h = queue[q];
            set(h, twistRole, side, ++count);
            for (const int g : ch[idx(h)]) {
                if (isBody(g) && roleOf(g) == RigRole::None) {
                    if (isHelper(g)) {
                        queue.push_back(g);
                    } else if (skinlessLeaf(g)) {
                        set(g, RigRole::Helper, side);
                    }
                }
            }
        }
    }

    // ---- The steps.

    /// Descent from the root, subtree sizes, feet below, the skin each bone dominates.
    void prepare() {
        underRoot.assign(idx(n), 0);
        size.assign(idx(n), 1);
        hasFoot.assign(idx(n), 0);
        for (int i = 0; i < n; ++i) {
            for (int cur = i; cur >= 0; cur = par[idx(cur)]) {
                if (cur == root) {
                    underRoot[idx(i)] = 1;
                }
            }
            for (int cur = par[idx(i)]; cur >= 0; cur = par[idx(cur)]) {
                ++size[idx(cur)];
                if (isFoot(i)) {
                    hasFoot[idx(cur)] = 1;
                }
            }
            if (isFoot(i)) {
                hasFoot[idx(i)] = 1;
            }
        }
        // The skin each bone dominates (the body mesh sample): a leaf no skin follows is a helper.
        skin.assign(idx(n), 0);
        haveMesh = in.mesh != nullptr && !in.mesh->empty();
        if (haveMesh) {
            for (const BodyMeshPoint& pt : *in.mesh) {
                if (pt.bone >= 0 && pt.bone < n) {
                    ++skin[idx(pt.bone)];
                }
            }
        }
        rootX = pos[idx(root)].x;
    }

    /// THE LEGS: the hips' children with a foot below them — directly (a thigh each) or one
    /// child carrying both (the PELVIS bone); then each side's heaviest thigh down its chain.
    void legs() {
        std::vector<int> thighs;
        for (const int c : ch[idx(root)]) {
            if (!isBody(c) || !hasFoot[idx(c)]) {
                continue;
            }
            int legCount = 0;
            for (const int g : ch[idx(c)]) {
                legCount += isBody(g) && hasFoot[idx(g)] ? 1 : 0;
            }
            if (legCount >= 2 && roleOf(c) == RigRole::None && out.find(RigRole::Pelvis) < 0) {
                set(c, RigRole::Pelvis, RigSide::Centre);
                for (const int g : ch[idx(c)]) {
                    if (isBody(g) && hasFoot[idx(g)]) {
                        thighs.push_back(g);
                    }
                }
            } else {
                thighs.push_back(c);
            }
        }
        int thigh[2] = {-1, -1};
        for (const int t : thighs) {
            const RigSide s = sideByX(t);
            if (s == RigSide::Centre) {
                continue;
            }
            int& slot = thigh[s == RigSide::Left ? 0 : 1];
            if (slot < 0 || mass(t) > mass(slot)) {
                slot = t;
            }
        }
        for (int k = 0; k < 2; ++k) {
            if (thigh[k] >= 0) {
                leg(thigh[k], k == 0 ? RigSide::Left : RigSide::Right);
            }
        }
    }

    /// One leg down its main chain: the helpers before the first real joint are THIGH TWISTS,
    /// that joint the SHIN, the helpers after it SHIN TWISTS, the first foot-class joint the
    /// FOOT; leaf helpers beside the chain are the segment's twists too.
    void leg(int t, RigSide s) {
        set(t, RigRole::Thigh, s);
        int thighTwists = 0;
        int c = mainChild(t);
        while (c >= 0 && isHelper(c) && !isFoot(c)) {
            set(c, RigRole::ThighTwist, s, ++thighTwists);
            c = mainChild(c);
        }
        int shin = -1;
        int ankle = -1;
        if (c >= 0 && !isFoot(c)) {
            shin = c;
            set(shin, RigRole::Shin, s);
            int shinTwists = 0;
            c = mainChild(shin);
            while (c >= 0 && isHelper(c) && !isFoot(c)) {
                set(c, RigRole::ShinTwist, s, ++shinTwists);
                c = mainChild(c);
            }
            ankle = c;
            tagHelperChains(shin, RigRole::ShinTwist, s, shinTwists);
        } else {
            ankle = c;
        }
        tagHelperChains(t, RigRole::ThighTwist, s, thighTwists);
        if (ankle < 0 || !isFoot(ankle)) {
            return;
        }
        set(ankle, RigRole::Foot, s);
        foot(ankle, s);
    }

    /// THE FOOT under its ankle: its bones by depth; the BALL (the first bone with two real
    /// children or more, else the ankle's main child); the chain to it METATARSAL; the toes;
    /// what is left a helper (skinless), the HEEL (a leaf behind the ankle, or named so at it)
    /// or a metatarsal.
    void foot(int ankle, RigSide s) {
        std::vector<int> order; // the descendants of the ankle, parents before children
        std::vector<int> depth(idx(n), 0);
        for (std::size_t q = 0; q <= order.size(); ++q) {
            const int from = q == 0 ? ankle : order[q - 1];
            for (const int d : ch[idx(from)]) {
                if (isBody(d)) {
                    depth[idx(d)] = depth[idx(from)] + 1;
                    order.push_back(d);
                }
            }
        }
        int ball = -1;
        for (const int d : order) {
            if (realKidCount(d) >= 2 && (ball < 0 || depth[idx(d)] < depth[idx(ball)])) {
                ball = d;
            }
        }
        if (ball < 0) {
            // No fan: a single toe bone, or a chain of them — the ankle's main child is the ball.
            const int c0 = mainChild(ankle);
            if (c0 >= 0 && !skinlessLeaf(c0)) {
                ball = c0;
            }
        }
        int metatarsals = 0;
        if (ball >= 0) {
            set(ball, RigRole::Ball, s);
            std::vector<int> between;
            for (int cur = par[idx(ball)]; cur >= 0 && cur != ankle; cur = par[idx(cur)]) {
                between.push_back(cur);
            }
            for (std::size_t q = between.size(); q-- > 0;) {
                set(between[q], RigRole::Metatarsal, s, ++metatarsals);
            }
            toes(ball, s);
        }
        // What is left under the ankle: a skinless leaf a helper; a leaf behind the ankle (or one
        // named so at it) the heel; anything else mid-foot.
        for (const int d : order) {
            if (roleOf(d) != RigRole::None) {
                continue;
            }
            if (skinlessLeaf(d)) {
                set(d, RigRole::Helper, s);
            } else if (ch[idx(d)].empty() &&
                       (pos[idx(d)].z < pos[idx(ankle)].z - kHeelBehind * scale ||
                        (namerules::hasToken(namerules::lettersLower(name(d)), "heel") && pos[idx(d)].z < pos[idx(ankle)].z + kHeelBehind * scale))) {
                set(d, RigRole::Heel, s);
            } else {
                set(d, RigRole::Metatarsal, s, ++metatarsals);
            }
        }
    }

    /// The TOES: each real child of the ball starts one; the digit by its name's token, else by
    /// its place across the foot (the big toe nearest the midline).
    void toes(int ball, RigSide s) {
        struct ToeBase {
            int   bone;
            int   digit;
            float across;
        };
        std::vector<ToeBase> bases;
        bool                 anyChain = false;
        for (const int c0 : ch[idx(ball)]) {
            anyChain = anyChain || (isBody(c0) && realKidCount(c0) > 0);
        }
        for (const int c0 : ch[idx(ball)]) {
            if (!isBody(c0)) {
                continue;
            }
            if (skinlessLeaf(c0)) {
                set(c0, RigRole::Helper, s);
                continue;
            }
            const int digit = namerules::digitByToken(namerules::lettersLower(name(c0)), true);
            // A childless, nameless stub in a fan whose other children carry toe chains is
            // no toe (the third generation's `lToe` sits among its five toes under the
            // mid-foot bone they all fan from); where no child has a chain every one is a toe.
            if (anyChain && realKidCount(c0) == 0 && digit == 0) {
                continue;
            }
            bases.push_back({c0, digit, pos[idx(c0)].x - pos[idx(ball)].x});
        }
        bool allTokens = !bases.empty();
        for (const ToeBase& b : bases) {
            allTokens = allTokens && b.digit > 0;
        }
        if (!allTokens) {
            // Inner (the big toe, nearest the body's midline) to outer: on the left foot
            // ascending x, on the right descending.
            std::sort(bases.begin(), bases.end(), [&](const ToeBase& a, const ToeBase& b) {
                return s == RigSide::Left ? a.across < b.across : a.across > b.across;
            });
            for (std::size_t q = 0; q < bases.size(); ++q) {
                bases[q].digit = static_cast<int>(q) + 1;
            }
        }
        for (const ToeBase& b : bases) {
            int joint = 0;
            for (int cur = b.bone; cur >= 0 && roleOf(cur) == RigRole::None && !skinlessLeaf(cur); cur = mainChild(cur)) {
                set(cur, RigRole::Toe, s, b.digit, ++joint);
                if (ch[idx(cur)].empty()) {
                    break;
                }
            }
        }
    }

    /// THE SPINE: the largest foot-free subtree up from the hips, link by link, to the first
    /// link with side children of real size on both sides — the collars' link, the chest top.
    void spineChain() {
        for (int cur = root, guard = 0; guard < 32; ++guard) {
            int next = -1;
            for (const int c : ch[idx(cur)]) {
                if (!isBody(c) || hasFoot[idx(c)] || roleOf(c) != RigRole::None || isHelper(c)) {
                    continue;
                }
                if (next < 0 || size[idx(c)] > size[idx(next)]) {
                    next = c;
                }
            }
            if (next < 0) {
                break;
            }
            spine.push_back(next);
            bool left = false;
            bool right = false;
            for (const int c : ch[idx(next)]) {
                if (!isBody(c) || size[idx(c)] < 2) {
                    continue;
                }
                const RigSide sd = sideByX(c);
                left = left || sd == RigSide::Left;
                right = right || sd == RigSide::Right;
            }
            if (left && right) {
                break;
            }
            cur = next;
        }
        for (std::size_t k = 0; k < spine.size(); ++k) {
            set(spine[k], RigRole::Spine, RigSide::Centre, static_cast<int>(k) + 1);
        }
        out.m_spineLinks = static_cast<int>(spine.size());
        top = spine.empty() ? root : spine.back();
    }

    /// AT THE TOP: the NECK is the real child nearest the midline; of the rest, each side's
    /// heaviest child is the COLLAR; every other side child of a spine link is a PECTORAL, its
    /// subtree with it (a skinless leaf a helper); then each arm from its collar.
    void chestTop() {
        for (const int c : ch[idx(top)]) {
            if (!isBody(c) || roleOf(c) != RigRole::None || skinlessLeaf(c) || isHelper(c) || hasFoot[idx(c)]) {
                continue;
            }
            // The nearest the midline — and of two there, the one with MORE below it (2026-11-03: a
            // biped export hangs a skinned leaf, its last spine link, beside the neck at the top,
            // both on the midline to 1e-8; the neck carries the head).
            const bool nearer = neck < 0 || std::abs(pos[idx(c)].x - rootX) < std::abs(pos[idx(neck)].x - rootX) - kSideOffPlane * scale;
            const bool level = neck >= 0 && std::abs(std::abs(pos[idx(c)].x - rootX) - std::abs(pos[idx(neck)].x - rootX)) <= kSideOffPlane * scale;
            if (nearer || (level && size[idx(c)] > size[idx(neck)])) {
                neck = c;
            }
        }
        for (const int c : ch[idx(top)]) {
            if (!isBody(c) || roleOf(c) != RigRole::None || skinlessLeaf(c) || c == neck || isHelper(c)) {
                continue;
            }
            RigSide sd = sideByX(c);
            if (sd == RigSide::Centre) {
                sd = pos[idx(c)].x - rootX >= 0.0f ? RigSide::Left : RigSide::Right; // (a collar at the midline: by its sign)
            }
            int& slot = collar[sd == RigSide::Left ? 0 : 1];
            if (slot < 0 || mass(c) > mass(slot)) {
                slot = c;
            }
        }
        std::vector<int> links = spine;
        if (spine.empty()) {
            links.push_back(root);
        }
        for (const int link : links) {
            for (const int c : ch[idx(link)]) {
                if (!isBody(c) || roleOf(c) != RigRole::None || c == collar[0] || c == collar[1] || c == neck) {
                    continue;
                }
                const RigSide sd = sideByX(c);
                if (sd == RigSide::Centre && (!ch[idx(c)].empty() || skinlessLeaf(c) || isHelper(c))) {
                    continue; // (a centre child with anything below it is no rider: the neck was taken above)
                }
                // (A skinned centre LEAF off a spine link — a biped export's last spine link, hung
                // beside the neck with the upper chest's skin — rides the link as a pectoral does,
                // 2026-11-03: classed "other" it was a selectable joint with no limits.)
                std::vector<int> stack{c};
                while (!stack.empty()) {
                    const int b = stack.back();
                    stack.pop_back();
                    if (!isBody(b) || roleOf(b) != RigRole::None) {
                        continue;
                    }
                    set(b, skinlessLeaf(b) ? RigRole::Helper : RigRole::Pectoral, sd);
                    stack.insert(stack.end(), ch[idx(b)].begin(), ch[idx(b)].end());
                }
            }
        }
        for (int k = 0; k < 2; ++k) {
            if (collar[k] >= 0) {
                arm(collar[k], k == 0 ? RigSide::Left : RigSide::Right);
            }
        }
    }

    /// One arm from its collar, down the main chain: UPPER ARM, its twists, FOREARM, its
    /// twists, HAND — the twist helpers as on the legs.
    void arm(int col, RigSide s) {
        set(col, RigRole::Collar, s);
        const int upper = mainChild(col);
        if (upper < 0) {
            return;
        }
        set(upper, RigRole::UpperArm, s);
        int upperTwists = 0;
        int c = mainChild(upper);
        while (c >= 0 && isHelper(c)) {
            set(c, RigRole::UpperArmTwist, s, ++upperTwists);
            c = mainChild(c);
        }
        const int forearm = c;
        int       wrist = -1;
        int       forearmTwists = 0;
        if (forearm >= 0) {
            set(forearm, RigRole::Forearm, s);
            c = mainChild(forearm);
            while (c >= 0 && isHelper(c)) {
                set(c, RigRole::ForearmTwist, s, ++forearmTwists);
                c = mainChild(c);
            }
            wrist = c;
            tagHelperChains(forearm, RigRole::ForearmTwist, s, forearmTwists);
        }
        tagHelperChains(upper, RigRole::UpperArmTwist, s, upperTwists);
        if (wrist < 0) {
            return;
        }
        set(wrist, RigRole::Hand, s);
        hand(wrist, s);
    }

    /// The real children of a hand bone, with its skinless leaves tagged helpers on the way.
    std::vector<int> realHandChildren(int b, RigSide s) {
        std::vector<int> result;
        for (const int g : ch[idx(b)]) {
            if (!isBody(g) || roleOf(g) != RigRole::None) {
                continue;
            }
            if (skinlessLeaf(g)) {
                set(g, RigRole::Helper, s);
            } else {
                result.push_back(g);
            }
        }
        return result;
    }

    /// THE HAND: skinless leaves are helpers; a bone carrying two chains or more is a CARPAL; a
    /// chain's LAST three real bones are a FINGER (the joints from the hand), the ones above it
    /// carpals; a finger's digit by its token, else by its place across the hand — the thumb
    /// the digit whose first joint sits nearest the wrist, then the rest foremost first.
    void hand(int wrist, RigSide s) {
        struct FingerBase {
            int   bone;
            int   digit;
            float across;
            float wristDistance; ///< The base's distance from the wrist (the thumb's is the least). (Not `near`: a Windows SDK macro.)
        };
        std::vector<FingerBase> bases;
        int                     carpals = 0;
        std::vector<int>        work = realHandChildren(wrist, s);
        std::size_t             next = 0;
        while (next < work.size()) {
            const int b = work[next++];
            if (roleOf(b) != RigRole::None) {
                continue;
            }
            std::vector<int> kids = realHandChildren(b, s);
            if (kids.size() >= 2) {
                set(b, RigRole::Carpal, s, ++carpals);
                work.insert(work.end(), kids.begin(), kids.end());
                continue;
            }
            if (kids.empty() && b != work[0] && roleOf(par[idx(b)]) == RigRole::Carpal) {
                set(b, RigRole::Carpal, s, ++carpals); // (a lone metacarpal beside the fingers under a carpal)
                continue;
            }
            // The chain from b down through single real children.
            std::vector<int> chain{b};
            while (kids.size() == 1) {
                chain.push_back(kids[0]);
                kids = realHandChildren(kids[0], s);
                if (kids.size() >= 2) {
                    break; // (a carpal deeper down: its chains join the work)
                }
            }
            const std::size_t fingerBones = std::min<std::size_t>(3, chain.size());
            const std::size_t fingerFrom = chain.size() - fingerBones;
            for (std::size_t q = 0; q < fingerFrom; ++q) {
                set(chain[q], RigRole::Carpal, s, ++carpals);
            }
            bases.push_back({chain[fingerFrom], namerules::digitByToken(namerules::lettersLower(name(chain[fingerFrom])), false),
                             pos[idx(chain[fingerFrom])].z - pos[idx(wrist)].z, glm::length(pos[idx(chain[fingerFrom])] - pos[idx(wrist)])});
            for (std::size_t q = fingerFrom; q < chain.size(); ++q) {
                set(chain[q], RigRole::Finger, s, 0, static_cast<int>(q - fingerFrom) + 1); // (the digit is filled below)
            }
            if (kids.size() >= 2) {
                set(chain.back(), RigRole::Carpal, s, ++carpals); // (the branching bone is a carpal after all)
                work.insert(work.end(), kids.begin(), kids.end());
            }
        }
        bool allTokens = !bases.empty();
        for (const FingerBase& b : bases) {
            allTokens = allTokens && b.digit > 0;
        }
        if (!allTokens) {
            // The thumb foremost (the palm faces the body at bind, the thumb points forward), the
            // little finger hindmost — EXCEPT THAT THE THUMB IS THE DIGIT WHOSE FIRST JOINT SITS
            // NEAREST THE WRIST where one does by kThumbNearShare (2026-11-03): a thumb's chain
            // begins at the carpus (its first bone is a metacarpal), a finger's at the knuckle, a
            // palm's length out. A biped rig names its digits Finger0..Finger4, no digit words,
            // and by the place alone the index's knuckle, further forward than the thumb's base
            // at the wrist, was the thumb: the thumb got a finger's frame and limits, the index
            // the thumb's, and the first relaxed hand curled its index sideways and folded its
            // thumb like a finger. The other four keep their order by place.
            std::sort(bases.begin(), bases.end(), [](const FingerBase& a, const FingerBase& b) { return a.across > b.across; });
            if (bases.size() >= 2) {
                std::size_t nearest = 0;
                for (std::size_t q = 1; q < bases.size(); ++q) {
                    if (bases[q].wristDistance < bases[nearest].wristDistance) {
                        nearest = q;
                    }
                }
                float second = std::numeric_limits<float>::max();
                for (std::size_t q = 0; q < bases.size(); ++q) {
                    if (q != nearest) {
                        second = std::min(second, bases[q].wristDistance);
                    }
                }
                if (bases[nearest].wristDistance < kThumbNearShare * second) {
                    std::rotate(bases.begin(), bases.begin() + static_cast<std::ptrdiff_t>(nearest),
                                bases.begin() + static_cast<std::ptrdiff_t>(nearest) + 1); // (the thumb first, the rest as they were)
                }
            }
            for (std::size_t q = 0; q < bases.size(); ++q) {
                bases[q].digit = static_cast<int>(q) + 1;
            }
        }
        for (const FingerBase& b : bases) {
            for (int cur = b.bone; cur >= 0 && roleOf(cur) == RigRole::Finger; cur = mainChild(cur)) {
                out.m_tags[idx(cur)].index = static_cast<unsigned char>(std::max(0, std::min(255, b.digit)));
                if (ch[idx(cur)].empty()) {
                    break;
                }
            }
        }
    }

    /// THE NECK AND THE HEAD from the neck found at the top: NECK links while a bone has one
    /// real child, the first with two or more (or named so) the HEAD — a chain that ends in a
    /// leaf ends in the head.
    void neckAndHead() {
        int head = -1;
        int links = 0;
        for (int cur = neck; cur >= 0;) {
            const int         kids = realKidCount(cur);
            const std::string nm = namerules::lettersLower(name(cur));
            if (kids >= 2 || (namerules::hasToken(nm, "head") && !namerules::hasToken(nm, "top") && !namerules::hasToken(nm, "end"))) {
                head = cur; // (two real children — the face rig — or a bone that says it is the head)
                break;
            }
            if (kids == 0) {
                // The chain ends in a leaf: that leaf is the head (a rig with no face rig).
                head = cur;
                break;
            }
            set(cur, RigRole::Neck, RigSide::Centre, ++links);
            cur = mainChild(cur);
        }
        if (head >= 0) {
            set(head, RigRole::Head, RigSide::Centre);
            face(head);
        }
        out.m_neckLinks = links;
    }

    /// THE FACE RIG: everything under the head; the EYES (a direct child named so, by side) and
    /// the JAW (the lower one) named for the tests, the rest FACE.
    void face(int head) {
        std::vector<int> stack(ch[idx(head)].begin(), ch[idx(head)].end());
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            if (!isBody(f) || roleOf(f) != RigRole::None) {
                continue;
            }
            const std::string nm = namerules::lettersLower(name(f));
            if (par[idx(f)] == head && namerules::hasToken(nm, "eye") && !namerules::hasToken(nm, "lid") && !namerules::hasToken(nm, "brow") && sideByX(f) != RigSide::Centre) {
                set(f, RigRole::Eye, sideByX(f));
            } else if (par[idx(f)] == head && namerules::hasToken(nm, "jaw") && namerules::hasToken(nm, "lower")) {
                set(f, RigRole::Jaw, RigSide::Centre);
            } else {
                set(f, RigRole::Face, sideByX(f));
            }
            stack.insert(stack.end(), ch[idx(f)].begin(), ch[idx(f)].end());
        }
        // The eyes and the jaw hang under a facial root bone on some rigs (not off the head
        // itself, as the reference generations' do): where a side has no eye, or there is no
        // jaw, the first face bone in bone order that carries the token takes the role.
        for (int k = 0; k < 2; ++k) {
            const RigSide side = k == 0 ? RigSide::Left : RigSide::Right;
            if (out.find(RigRole::Eye, side) >= 0) {
                continue;
            }
            for (int f = 0; f < n; ++f) {
                const std::string nm = namerules::lettersLower(name(f));
                if (roleOf(f) == RigRole::Face && sideByX(f) == side && namerules::hasToken(nm, "eye") && !namerules::hasToken(nm, "lid") &&
                    !namerules::hasToken(nm, "brow") && !namerules::hasToken(nm, "socket")) {
                    set(f, RigRole::Eye, side);
                    break;
                }
            }
        }
        if (out.find(RigRole::Jaw) < 0) {
            for (int f = 0; f < n; ++f) {
                const std::string nm = namerules::lettersLower(name(f));
                if (roleOf(f) == RigRole::Face && namerules::hasToken(nm, "jaw") && !namerules::hasToken(nm, "upper")) {
                    set(f, RigRole::Jaw, RigSide::Centre);
                    break;
                }
            }
        }
    }

    /// WHAT IS LEFT of the body: a skinless leaf is a HELPER; so is a bone with next to no skin
    /// (under kHelperSkinShare of the body's) and no real child — a game rig's corrective bones
    /// beside a knee or an elbow; a skinned leaf AT the hips' joint on a rig with no pelvis bone
    /// is the PELVIS; anything else is OTHER, and listed.
    void leftovers() {
        std::size_t totalSkin = 0;
        for (int i = 0; i < n; ++i) {
            if (underRoot[idx(i)] && isBody(i)) {
                totalSkin += static_cast<std::size_t>(skin[idx(i)]);
            }
        }
        for (int i = 0; i < n; ++i) {
            if (!underRoot[idx(i)] || !isBody(i) || roleOf(i) != RigRole::None) {
                continue;
            }
            const int  realKids = realKidCount(i);
            const bool scrap = haveMesh && realKids == 0 && totalSkin > 0 &&
                               static_cast<double>(skin[idx(i)]) < kHelperSkinShare * static_cast<double>(totalSkin);
            // ... and a SKINNED leaf lying AT the hips' joint, where no bone carries both legs, is the
            // PELVIS's flesh without its joint: a game-engine export re-parents the thighs to the hips
            // and leaves the pelvis bone as such a leaf. It keeps the pelvis's role — the seat a floor
            // sit rests on, the girdle a click on the hips finds — where a helper (no contact) left
            // the sitter with no seat, and Other left it listed.
            const bool atHips = realKids == 0 && par[idx(i)] == root && !skinlessLeaf(i) && !scrap &&
                                out.find(RigRole::Pelvis) < 0 &&
                                glm::length(pos[idx(i)] - pos[idx(par[idx(i)])]) < kSideOffPlane * scale;
            set(i, atHips ? RigRole::Pelvis : skinlessLeaf(i) || scrap ? RigRole::Helper : RigRole::Other, atHips ? RigSide::Centre : sideByX(i));
        }
    }

    /// THE REQUIRED ROLES: what missing() names.
    void require(RigRole role, RigSide side, const char* what) {
        if (out.find(role, side) < 0) {
            out.m_missing.push_back(std::string(what) + (side == RigSide::Centre ? "" : side == RigSide::Left ? " L" : " R"));
        }
    }
    void required() {
        require(RigRole::Spine, RigSide::Centre, "spine");
        require(RigRole::Head, RigSide::Centre, "head");
        for (const RigSide s : {RigSide::Left, RigSide::Right}) {
            require(RigRole::UpperArm, s, "upper arm");
            require(RigRole::Forearm, s, "forearm");
            require(RigRole::Hand, s, "hand");
            require(RigRole::Thigh, s, "thigh");
            require(RigRole::Shin, s, "shin");
            require(RigRole::Foot, s, "foot");
        }
    }
};

void RigRoles::build(const Input& in) {
    m_tags.clear();
    m_names.clear();
    m_missing.clear();
    m_spineLinks = 0;
    m_neckLinks = 0;
    if (in.parents == nullptr || in.children == nullptr || in.bindPos == nullptr) {
        return;
    }
    const int n = static_cast<int>(in.parents->size());
    if (n == 0 || static_cast<int>(in.children->size()) != n || static_cast<int>(in.bindPos->size()) != n) {
        return;
    }
    m_tags.assign(static_cast<std::size_t>(n), RigRoleTag{});
    m_names.assign(static_cast<std::size_t>(n), std::string());
    if (in.names != nullptr && static_cast<int>(in.names->size()) == n) {
        m_names = *in.names;
    }
    if (in.root < 0 || in.root >= n) {
        m_missing.push_back("hips");
        return;
    }
    Builder(*this, in).run();
}

std::string RigRoles::report() const {
    std::ostringstream out;
    out << "[rig-roles] spine " << m_spineLinks << " link(s), neck " << m_neckLinks << " link(s)";
    const int head = find(RigRole::Head);
    out << ", head " << (head >= 0 ? m_names[static_cast<std::size_t>(head)] : std::string("(none)"));
    if (!m_missing.empty()) {
        out << "; MISSING:";
        for (const std::string& m : m_missing) {
            out << ' ' << m;
        }
    } else if (built()) {
        out << "; complete";
    }
    out << '\n';
    for (std::size_t i = 0; i < m_tags.size(); ++i) {
        const RigRoleTag& t = m_tags[i];
        if (t.role == RigRole::None) {
            continue;
        }
        out << "  " << i << ' ' << m_names[i] << " -> " << roleName(t.role);
        if (t.side != RigSide::Centre) {
            out << ' ' << sideName(t.side);
        }
        if (t.index != 0) {
            out << ' ' << static_cast<int>(t.index);
        }
        if (t.joint != 0) {
            out << " j" << static_cast<int>(t.joint);
        }
        const std::string standard = standardName(t);
        if (!standard.empty()) {
            out << "  [" << standard << ']';
        }
        out << '\n';
    }
    return out.str();
}

RigRoleTag RigRoles::canonicalTag(const std::string& canonical) const {
    RigRoleTag tag;
    if (canonical.empty()) {
        return tag;
    }
    // The standard's own names first (`upper_arm_twist1_l`, `spine3`, `index2_r`).
    tag = standardTag(canonical);
    if (tag.role != RigRole::None) {
        return tag;
    }
    // The side prefix: `l`/`r` before an uppercase letter or an underscore.
    std::string rest = canonical;
    RigSide     side = RigSide::Centre;
    if (canonical.size() >= 2 && (canonical[0] == 'l' || canonical[0] == 'r') &&
        ((canonical[1] >= 'A' && canonical[1] <= 'Z') || canonical[1] == '_')) {
        side = canonical[0] == 'l' ? RigSide::Left : RigSide::Right;
        rest = canonical.substr(canonical[1] == '_' ? 2 : 1);
    }
    // The base (letters) and the number after it (`Index3` -> 3, `SmallToe2_2` -> 2 and `_2`).
    std::string base;
    std::size_t p = 0;
    while (p < rest.size() && std::isalpha(static_cast<unsigned char>(rest[p]))) {
        base.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(rest[p]))));
        ++p;
    }
    int number = 0;
    while (p < rest.size() && std::isdigit(static_cast<unsigned char>(rest[p]))) {
        number = number * 10 + (rest[p] - '0');
        ++p;
    }
    int second = 0;
    if (p < rest.size() && rest[p] == '_') {
        ++p;
        while (p < rest.size() && std::isdigit(static_cast<unsigned char>(rest[p]))) {
            second = second * 10 + (rest[p] - '0');
            ++p;
        }
    }
    const auto make = [&](RigRole role, int index = 0, int joint = 0, RigSide s = RigSide::Centre) {
        tag.role = role;
        tag.side = s;
        tag.index = static_cast<unsigned char>(std::max(0, std::min(255, index)));
        tag.joint = static_cast<unsigned char>(std::max(0, std::min(255, joint)));
        return tag;
    };
    const int links = m_spineLinks;
    if (side == RigSide::Centre) {
        if (base == "hip" || base == "hips") return make(RigRole::Hips);
        if (base == "pelvis") return make(RigRole::Pelvis);
        if (base == "abdomenlower" || (base == "abdomen" && number <= 1 && second == 0)) return make(RigRole::Spine, 1);
        if (base == "abdomenupper" || (base == "abdomen" && (number == 2 || second == 2))) return make(RigRole::Spine, 2);
        if (base == "chest" && number == 0 && second == 0) return make(RigRole::Spine, links >= 4 ? links - 1 : std::max(1, links));
        if (base == "chestlower") return make(RigRole::Spine, links >= 4 ? links - 1 : std::max(1, links));
        if ((base == "chest" && (number == 2 || second == 2)) || base == "chestupper") return make(RigRole::Spine, std::max(1, links));
        if (base == "neck" && number == 0 && second == 0) return make(RigRole::Neck, 1);
        if (base == "necklower") return make(RigRole::Neck, 1);
        if ((base == "neck" && (number == 2 || second == 2)) || base == "neckupper") return make(RigRole::Neck, 2);
        if (base == "head") return make(RigRole::Head);
        if (base == "lowerjaw") return make(RigRole::Jaw);
        return tag;
    }
    const int joint = second > 0 ? second : number;
    if (base == "collar") return make(RigRole::Collar, 0, 0, side);
    if (base == "shldr" || base == "shldrbend" || base == "upperarm") return make(RigRole::UpperArm, 0, 0, side);
    if (base == "shldrtwist" || base == "upperarmtwist") return make(RigRole::UpperArmTwist, std::max(1, number), 0, side);
    if (base == "forearm" || base == "forearmbend") return make(RigRole::Forearm, 0, 0, side);
    if (base == "wrist" || base == "forearmtwist") return make(RigRole::ForearmTwist, std::max(1, number), 0, side);
    if (base == "hand") return make(RigRole::Hand, 0, 0, side);
    if (base == "carpal") return make(RigRole::Carpal, std::max(1, number), 0, side);
    if (base == "thumb") return make(RigRole::Finger, 1, std::max(1, joint), side);
    if (base == "index") return make(RigRole::Finger, 2, std::max(1, joint), side);
    if (base == "mid") return make(RigRole::Finger, 3, std::max(1, joint), side);
    if (base == "ring") return make(RigRole::Finger, 4, std::max(1, joint), side);
    if (base == "pinky") return make(RigRole::Finger, 5, std::max(1, joint), side);
    if (base == "thigh" || base == "thighbend") return make(RigRole::Thigh, 0, 0, side);
    if (base == "thightwist") return make(RigRole::ThighTwist, std::max(1, number), 0, side);
    if (base == "shin") return make(RigRole::Shin, 0, 0, side);
    if (base == "foot") return make(RigRole::Foot, 0, 0, side);
    if (base == "metatarsals" || base == "metatarsal") return make(RigRole::Metatarsal, std::max(1, number), 0, side);
    if (base == "toe" && number == 0) return make(RigRole::Ball, 0, 0, side);
    if (base == "bigtoe") return make(RigRole::Toe, 1, std::max(1, joint), side);
    if (base == "smalltoe") return make(RigRole::Toe, number + 1, std::max(1, second), side);
    if (base == "heel") return make(RigRole::Heel, 0, 0, side);
    if (base == "pectoral") return make(RigRole::Pectoral, 0, 0, side);
    if (base == "eye") return make(RigRole::Eye, 0, 0, side);
    return tag;
}

} // namespace pose
