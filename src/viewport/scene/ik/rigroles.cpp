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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>

namespace pose {

namespace {

/// A centre bone stands within this of the sagittal plane at bind (figure-scaled): the hips,
/// the spine, the neck and the head do to a tenth of a millimetre; a collar stands 3cm out.
constexpr float kSideOffPlane = 0.01f;
/// A leaf under the ankle this far BEHIND it (figure-scaled) is a heel bone; ahead, a mid-foot one.
constexpr float kHeelBehind = 0.02f;
/// A hand child whose chain is this long or longer carries a finger below its first bone (a
/// carpal or a metacarpal): a finger itself is three joints (a thumb's first joint IS its
/// metacarpal, on every rig seen, and the rigs that author a carpal for it name it so).
constexpr int kCarpalChainLength = 4;

std::string lowerAlpha(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool hasToken(const std::string& normalized, const char* token) {
    return normalized.find(token) != std::string::npos;
}

/// The digit a finger's or a toe's name says it is (1..5), or 0 where the name says nothing. The
/// tokens are the universal ones; "small" is deliberately none (one generation numbers its four
/// small toes 1-4, which only their places across the foot can order).
int digitByToken(const std::string& normalized, bool toe) {
    if (hasToken(normalized, toe ? "big" : "thumb")) return 1;
    if (hasToken(normalized, "index") || hasToken(normalized, "second")) return 2;
    if (hasToken(normalized, "mid") || hasToken(normalized, "third")) return 3;
    if (hasToken(normalized, "ring") || hasToken(normalized, "fourth")) return 4;
    if (hasToken(normalized, "pinky") || hasToken(normalized, "little") || hasToken(normalized, "fifth")) return 5;
    return 0;
}

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

void RigRoles::build(const Input& in) {
    m_tags.clear();
    m_names.clear();
    m_missing.clear();
    m_spineLinks = 0;
    m_neckLinks = 0;
    if (in.parents == nullptr || in.children == nullptr || in.bindPos == nullptr) {
        return;
    }
    const std::vector<int>&              par = *in.parents;
    const std::vector<std::vector<int>>& ch = *in.children;
    const std::vector<glm::vec3>&        pos = *in.bindPos;
    const int                            n = static_cast<int>(par.size());
    if (n == 0 || static_cast<int>(ch.size()) != n || static_cast<int>(pos.size()) != n) {
        return;
    }
    m_tags.assign(static_cast<std::size_t>(n), RigRoleTag{});
    m_names.assign(static_cast<std::size_t>(n), std::string());
    if (in.names != nullptr && static_cast<int>(in.names->size()) == n) {
        m_names = *in.names;
    }
    const int root = in.root;
    if (root < 0 || root >= n) {
        m_missing.push_back("hips");
        return;
    }
    const float scale = in.sizeScale > 0.0f ? in.sizeScale : 1.0f;
    const auto  idx = [](int b) { return static_cast<std::size_t>(b); };
    const auto  isBody = [&](int b) { return b >= 0 && b < n && (in.bodyNode == nullptr || (*in.bodyNode)[idx(b)] != 0); };
    const auto  isHelper = [&](int b) { return in.helper != nullptr && b >= 0 && b < n && (*in.helper)[idx(b)] != 0; };
    const auto  isFoot = [&](int b) { return in.footClass != nullptr && b >= 0 && b < n && (*in.footClass)[idx(b)] != 0; };
    const auto  mass = [&](int b) { return in.subtreeMass != nullptr && b >= 0 && b < n ? (*in.subtreeMass)[idx(b)] : 1.0f; };
    const auto  roleOf = [&](int b) { return m_tags[idx(b)].role; };
    const auto  set = [&](int b, RigRole role, RigSide side, int index = 0, int joint = 0) {
        RigRoleTag& t = m_tags[idx(b)];
        t.role = role;
        t.side = side;
        t.index = static_cast<unsigned char>(std::max(0, std::min(255, index)));
        t.joint = static_cast<unsigned char>(std::max(0, std::min(255, joint)));
    };
    // Descent from the root, subtree sizes (bone counts: the spine rule reads them), feet below.
    std::vector<char> underRoot(idx(n), 0);
    std::vector<int>  size(idx(n), 1);
    std::vector<char> hasFoot(idx(n), 0);
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
    std::vector<int> skin(idx(n), 0);
    const bool       haveMesh = in.mesh != nullptr && !in.mesh->empty();
    if (haveMesh) {
        for (const BodyMeshPoint& pt : *in.mesh) {
            if (pt.bone >= 0 && pt.bone < n) {
                ++skin[idx(pt.bone)];
            }
        }
    }
    const auto skinlessLeaf = [&](int b) {
        if (!ch[idx(b)].empty()) {
            return false;
        }
        if (haveMesh) {
            return skin[idx(b)] == 0;
        }
        return hasToken(lowerAlpha(m_names[idx(b)]), "anchor"); // (no sample: the one such bone seen, by its name)
    };
    const float rootX = pos[idx(root)].x;
    const auto  sideByX = [&](int b) {
        const float dx = pos[idx(b)].x - rootX;
        if (std::abs(dx) < kSideOffPlane * scale) {
            return RigSide::Centre;
        }
        return dx > 0.0f ? RigSide::Left : RigSide::Right;
    };
    // The child with the most mass below it (ties: the bigger subtree, then the lower index).
    const auto mainChild = [&](int b) {
        int best = -1;
        for (const int c : ch[idx(b)]) {
            if (!isBody(c)) {
                continue;
            }
            if (best < 0 || mass(c) > mass(best) + 1.0e-9f ||
                (std::abs(mass(c) - mass(best)) <= 1.0e-9f && size[idx(c)] > size[idx(best)])) {
                best = c;
            }
        }
        return best;
    };

    set(root, RigRole::Hips, RigSide::Centre);

    // ---- THE LEGS.
    std::vector<int> thighs;
    for (const int c : ch[idx(root)]) {
        if (!isBody(c) || !hasFoot[idx(c)]) {
            continue;
        }
        int legs = 0;
        for (const int g : ch[idx(c)]) {
            legs += isBody(g) && hasFoot[idx(g)] ? 1 : 0;
        }
        if (legs >= 2 && roleOf(c) == RigRole::None && find(RigRole::Pelvis) < 0) {
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
        const int t = thigh[k];
        if (t < 0) {
            continue;
        }
        const RigSide s = k == 0 ? RigSide::Left : RigSide::Right;
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
            for (const int h : ch[idx(shin)]) {
                if (isBody(h) && roleOf(h) == RigRole::None && isHelper(h)) {
                    set(h, RigRole::ShinTwist, s, ++shinTwists);
                }
            }
        } else {
            ankle = c;
        }
        for (const int h : ch[idx(t)]) {
            if (isBody(h) && roleOf(h) == RigRole::None && isHelper(h)) {
                set(h, RigRole::ThighTwist, s, ++thighTwists);
            }
        }
        if (ankle < 0 || !isFoot(ankle)) {
            continue;
        }
        set(ankle, RigRole::Foot, s);
        // ---- THE FOOT. Its bones by depth; the ball; the mid-foot chain; the toes; the heel.
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
            int kids = 0;
            for (const int g : ch[idx(d)]) {
                kids += isBody(g) && !skinlessLeaf(g) ? 1 : 0;
            }
            if (kids >= 2 && (ball < 0 || depth[idx(d)] < depth[idx(ball)])) {
                ball = d;
            }
        }
        if (ball < 0) {
            for (const int d : order) { // (a single toe: the deepest bone with a child)
                if (!ch[idx(d)].empty() && (ball < 0 || depth[idx(d)] > depth[idx(ball)])) {
                    ball = d;
                }
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
            // The toes: each child of the ball starts one; the digit by token, else by place.
            struct ToeBase {
                int   bone;
                int   digit;
                float across;
            };
            std::vector<ToeBase> bases;
            bool anyChain = false;
            for (const int c0 : ch[idx(ball)]) {
                anyChain = anyChain || (isBody(c0) && !ch[idx(c0)].empty());
            }
            for (const int c0 : ch[idx(ball)]) {
                if (!isBody(c0) || skinlessLeaf(c0)) {
                    continue;
                }
                const int digit = digitByToken(lowerAlpha(m_names[idx(c0)]), true);
                // A childless, nameless stub in a fan whose other children carry toe chains is
                // no toe (the third generation's `lToe` sits among its five toes under the
                // mid-foot bone they all fan from); where no child has a chain every one is a toe.
                if (anyChain && ch[idx(c0)].empty() && digit == 0) {
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
                for (int cur = b.bone; cur >= 0 && roleOf(cur) == RigRole::None; cur = mainChild(cur)) {
                    set(cur, RigRole::Toe, s, b.digit, ++joint);
                    if (ch[idx(cur)].empty()) {
                        break;
                    }
                }
            }
        }
        // What is left under the ankle: a leaf behind it is the heel; anything else mid-foot.
        for (const int d : order) {
            if (roleOf(d) != RigRole::None) {
                continue;
            }
            if (skinlessLeaf(d)) {
                set(d, RigRole::Helper, s);
            } else if (ch[idx(d)].empty() &&
                       (pos[idx(d)].z < pos[idx(ankle)].z - kHeelBehind * scale ||
                        (hasToken(lowerAlpha(m_names[idx(d)]), "heel") && pos[idx(d)].z < pos[idx(ankle)].z + kHeelBehind * scale))) {
                // (A heel bone authored AT the ankle, a few millimetres ahead of it, says so by
                // its name — the third generation's.)
                set(d, RigRole::Heel, s);
            } else {
                set(d, RigRole::Metatarsal, s, ++metatarsals);
            }
        }
    }

    // ---- THE SPINE: the largest foot-free subtree up from the hips, link by link, to the first
    // link with side children of real size on both sides — the collars' link, the chest top.
    std::vector<int> spine;
    for (int cur = root, guard = 0; guard < 32; ++guard) {
        int next = -1;
        for (const int c : ch[idx(cur)]) {
            if (!isBody(c) || hasFoot[idx(c)] || roleOf(c) != RigRole::None) {
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
    m_spineLinks = static_cast<int>(spine.size());
    const int top = spine.empty() ? root : spine.back();

    // ---- THE COLLARS (the top's heaviest side child each way), THE PECTORALS (every other side
    // child of a spine link), THE ARMS.
    int collar[2] = {-1, -1};
    for (const int c : ch[idx(top)]) {
        if (!isBody(c) || roleOf(c) != RigRole::None || skinlessLeaf(c)) {
            continue;
        }
        const RigSide sd = sideByX(c);
        if (sd == RigSide::Centre) {
            continue;
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
            if (!isBody(c) || roleOf(c) != RigRole::None || c == collar[0] || c == collar[1]) {
                continue;
            }
            const RigSide sd = sideByX(c);
            if (sd == RigSide::Centre) {
                continue;
            }
            set(c, skinlessLeaf(c) ? RigRole::Helper : RigRole::Pectoral, sd);
        }
    }
    for (int k = 0; k < 2; ++k) {
        const int col = collar[k];
        if (col < 0) {
            continue;
        }
        const RigSide s = k == 0 ? RigSide::Left : RigSide::Right;
        set(col, RigRole::Collar, s);
        const int upper = mainChild(col);
        if (upper < 0) {
            continue;
        }
        set(upper, RigRole::UpperArm, s);
        int upperTwists = 0;
        int c = mainChild(upper);
        while (c >= 0 && isHelper(c)) {
            set(c, RigRole::UpperArmTwist, s, ++upperTwists);
            c = mainChild(c);
        }
        const int forearm = c;
        int       hand = -1;
        int       forearmTwists = 0;
        if (forearm >= 0) {
            set(forearm, RigRole::Forearm, s);
            c = mainChild(forearm);
            while (c >= 0 && isHelper(c)) {
                set(c, RigRole::ForearmTwist, s, ++forearmTwists);
                c = mainChild(c);
            }
            hand = c;
            for (const int h : ch[idx(forearm)]) {
                if (isBody(h) && roleOf(h) == RigRole::None && isHelper(h)) {
                    set(h, RigRole::ForearmTwist, s, ++forearmTwists);
                }
            }
        }
        for (const int h : ch[idx(upper)]) {
            if (isBody(h) && roleOf(h) == RigRole::None && isHelper(h)) {
                set(h, RigRole::UpperArmTwist, s, ++upperTwists);
            }
        }
        if (hand < 0) {
            continue;
        }
        set(hand, RigRole::Hand, s);
        // ---- THE HAND: helpers, carpals, fingers.
        struct FingerBase {
            int   bone;
            int   digit;
            float across;
        };
        std::vector<FingerBase> bases;
        int                     carpals = 0;
        const auto chainOf = [&](int from) {
            std::vector<int> chain;
            for (int cur = from; cur >= 0; cur = mainChild(cur)) {
                chain.push_back(cur);
                if (ch[idx(cur)].empty()) {
                    break;
                }
            }
            return chain;
        };
        const auto fingerFrom = [&](int base) {
            bases.push_back({base, digitByToken(lowerAlpha(m_names[idx(base)]), false), pos[idx(base)].z - pos[idx(hand)].z});
        };
        for (const int c0 : ch[idx(hand)]) {
            if (!isBody(c0) || roleOf(c0) != RigRole::None) {
                continue;
            }
            if (skinlessLeaf(c0)) {
                set(c0, RigRole::Helper, s);
                continue;
            }
            int realKids = 0;
            for (const int g : ch[idx(c0)]) {
                realKids += isBody(g) && !skinlessLeaf(g) ? 1 : 0;
            }
            if (realKids >= 2) {
                set(c0, RigRole::Carpal, s, ++carpals);
                for (const int g : ch[idx(c0)]) {
                    if (isBody(g) && !skinlessLeaf(g)) {
                        fingerFrom(g);
                    }
                }
            } else if (static_cast<int>(chainOf(c0).size()) >= kCarpalChainLength) {
                set(c0, RigRole::Carpal, s, ++carpals);
                const int g = mainChild(c0);
                if (g >= 0) {
                    fingerFrom(g);
                }
            } else {
                fingerFrom(c0);
            }
        }
        bool allTokens = !bases.empty();
        for (const FingerBase& b : bases) {
            allTokens = allTokens && b.digit > 0;
        }
        if (!allTokens) {
            // The thumb foremost (the palm faces the body at bind, the thumb points forward), the
            // little finger hindmost.
            std::sort(bases.begin(), bases.end(), [](const FingerBase& a, const FingerBase& b) { return a.across > b.across; });
            for (std::size_t q = 0; q < bases.size(); ++q) {
                bases[q].digit = static_cast<int>(q) + 1;
            }
        }
        for (const FingerBase& b : bases) {
            int joint = 0;
            for (int cur = b.bone; cur >= 0 && roleOf(cur) == RigRole::None; cur = mainChild(cur)) {
                set(cur, RigRole::Finger, s, b.digit, ++joint);
                if (ch[idx(cur)].empty()) {
                    break;
                }
            }
        }
    }

    // ---- THE NECK AND THE HEAD: from the top's heaviest centre child.
    {
        int neck = -1;
        for (const int c : ch[idx(top)]) {
            if (!isBody(c) || roleOf(c) != RigRole::None || sideByX(c) != RigSide::Centre) {
                continue;
            }
            if (neck < 0 || mass(c) > mass(neck) + 1.0e-9f || (std::abs(mass(c) - mass(neck)) <= 1.0e-9f && size[idx(c)] > size[idx(neck)])) {
                neck = c;
            }
        }
        int head = -1;
        int links = 0;
        for (int cur = neck; cur >= 0;) {
            int kids = 0;
            for (const int g : ch[idx(cur)]) {
                kids += isBody(g) ? 1 : 0;
            }
            if (kids >= 2) {
                head = cur;
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
            // The face rig: everything under the head; the eyes and the jaw named for the tests.
            std::vector<int> stack(ch[idx(head)].begin(), ch[idx(head)].end());
            while (!stack.empty()) {
                const int f = stack.back();
                stack.pop_back();
                if (!isBody(f) || roleOf(f) != RigRole::None) {
                    continue;
                }
                const std::string nm = lowerAlpha(m_names[idx(f)]);
                if (par[idx(f)] == head && hasToken(nm, "eye") && !hasToken(nm, "lid") && !hasToken(nm, "brow")) {
                    set(f, RigRole::Eye, sideByX(f));
                } else if (par[idx(f)] == head && hasToken(nm, "jaw") && hasToken(nm, "lower")) {
                    set(f, RigRole::Jaw, RigSide::Centre);
                } else {
                    set(f, RigRole::Face, sideByX(f));
                }
                stack.insert(stack.end(), ch[idx(f)].begin(), ch[idx(f)].end());
            }
        }
        m_neckLinks = links;
    }

    // ---- WHAT IS LEFT of the body: a skinless leaf is a helper, anything else is listed.
    for (int i = 0; i < n; ++i) {
        if (!underRoot[idx(i)] || !isBody(i) || roleOf(i) != RigRole::None) {
            continue;
        }
        set(i, skinlessLeaf(i) ? RigRole::Helper : RigRole::Other, sideByX(i));
    }

    // ---- THE REQUIRED ROLES.
    const auto require = [&](RigRole role, RigSide side, const char* what) {
        if (find(role, side) < 0) {
            m_missing.push_back(std::string(what) + (side == RigSide::Centre ? "" : side == RigSide::Left ? " L" : " R"));
        }
    };
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
