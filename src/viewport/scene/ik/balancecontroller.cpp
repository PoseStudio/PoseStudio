/**
 * @file balancecontroller.cpp
 * @brief The balance controller's mass model, center of mass, support polygon and correction.
 *
 * Segment masses come from a tolerant NAME-FRAGMENT table (biomechanical fractions; same-class
 * same-side bones split a share so a Bend+Twist pair is one thigh; unclassified bones get a token
 * mass so they perturb, never dominate) — a rig whose names match nothing degrades to token
 * masses everywhere. The support polygon is Andrew's monotone-chain hull of the contacts in XZ;
 * degenerate one- and two-point hulls get a foot-area slack radius (kDegenerateSupportRadius) so
 * point-feet never demand exact-line balance and the controller does not fight the pins forever.
 * Qt-free (std + GLM).
 */
#include "balancecontroller.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>

namespace pose {

namespace {

// One body-segment class: name fragments that identify it (checked in table order, first hit
// wins) and its share of total body mass (biomechanical segment fractions, Winter/Dempster-ish).
// Same-class same-side bones split the share (a ThighBend+ThighTwist pair is still one thigh).
struct SegmentClass {
    const char* fragment;
    float       massFraction;
};

// Order matters: more specific fragments before the substrings they contain.
constexpr SegmentClass kSegmentTable[] = {
    {"forearm", 0.016f},   {"upperarm", 0.028f}, {"shldr", 0.028f},  {"shoulder", 0.028f},
    {"collar", 0.021f},    {"hand", 0.006f},     {"thigh", 0.100f},  {"shin", 0.0465f},
    {"calf", 0.0465f},     {"metatarsal", 0.005f}, {"toe", 0.004f},  {"foot", 0.0137f},
    {"head", 0.081f},      {"neck", 0.012f},     {"chest", 0.170f},  {"abdomen", 0.100f},
    {"spine", 0.100f},     {"pelvis", 0.112f},   {"hip", 0.050f},
};

constexpr float kUnclassifiedMass = 0.0015f; // face/finger/helper bones: perturb, don't dominate

// Degenerate support hulls (one contact point, or two — a line segment between point-feet) get
// this much slack radius: real feet have AREA, so demanding the CoM sit exactly on the contact
// point/line would make the balance controller correct forever and fight the drag/pins.
constexpr float kDegenerateSupportRadius = 0.05f;

std::string normalizedName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// Side for share-splitting: 'l'/'r' when the name starts with one (lShin, r_foot, leftFoot),
// 'c' (center) otherwise — read off the RAW name's side TOKENS first (2026-11-03, the rule
// Armature::mirroredBoneName reads: a lone l/r bounded by `_ . - :` or the name's ends, Left/Right
// bounded likewise or by a change of case, wherever it stands), then the old prefix rule on the
// letters alone. A game-engine export puts the side LAST (`thigh_r`, `calf_twist_01_l`), and read
// by the first letter every thigh of such a rig was a CENTRE thigh: the class's share split six
// ways over both sides and their helpers, a leg's subtree read 3.5% of the body, and a thigh
// socket was no girdle grab (its drag left the hips where they stood). `lowerarm_r` read as LEFT
// and `ring_01_l` as RIGHT the same way. The reference generations' names carry the prefix, so
// the token rule and the old one agree there bone for bone.
bool isSideBoundary(char c) {
    return c == '_' || c == '.' || c == '-' || c == ':';
}

char sideOfRaw(const std::string& raw) {
    const std::size_t n = raw.size();
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto isUpper = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
    const auto isLower = [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };
    for (std::size_t i = 0; i < n; ++i) {
        const char c = lower(raw[i]);
        if (c != 'l' && c != 'r') {
            continue;
        }
        // "left" / "right": bounded by a boundary, the ends, or a change of case (LeftArm, armLeft).
        const char* word = c == 'l' ? "left" : "right";
        const std::size_t len = c == 'l' ? 4 : 5;
        if (i + len <= n) {
            bool match = true;
            for (std::size_t k = 0; k < len && match; ++k) {
                match = lower(raw[i + k]) == word[k];
            }
            const bool before = i == 0 || isSideBoundary(raw[i - 1]) || (isLower(raw[i - 1]) && isUpper(raw[i]));
            const bool after = i + len == n || isSideBoundary(raw[i + len]) || isUpper(raw[i + len]) ||
                               std::isdigit(static_cast<unsigned char>(raw[i + len]));
            if (match && before && after) {
                return c;
            }
        }
        // A lone l/r: at the start before an uppercase letter or an underscore (lShin, l_thigh),
        // or bounded on both sides by a boundary or an end (thigh_r, DEF-thigh1.L, cc_base_l_x).
        const bool before = i == 0 || isSideBoundary(raw[i - 1]);
        const bool after = i + 1 == n || isSideBoundary(raw[i + 1]) || (i == 0 && isUpper(raw[i + 1]));
        if (before && after) {
            return c;
        }
    }
    return 0;
}

char sideOf(const std::string& normalized, const std::string& raw) {
    if (const char token = sideOfRaw(raw)) {
        return token;
    }
    if (normalized.rfind("left", 0) == 0) return 'l';
    if (normalized.rfind("right", 0) == 0) return 'r';
    if (!normalized.empty() && (normalized[0] == 'l' || normalized[0] == 'r')) {
        return normalized[0];
    }
    return 'c';
}

int classify(const std::string& normalized) {
    // Rig HELPER bones named after the segment they hang off ("l_hand_anchor" on the newest
    // generation) are not body segments and must not split the segment's share: the hand's
    // 0.006 halved to 0.003 read as token mass, and a hand reaching the floor mid-drag then
    // planted its ELBOW (the first real-mass joint above) 40cm up.
    if (normalized.find("anchor") != std::string::npos) {
        return -1;
    }
    for (std::size_t t = 0; t < std::size(kSegmentTable); ++t) {
        if (normalized.find(kSegmentTable[t].fragment) != std::string::npos) {
            return static_cast<int>(t);
        }
    }
    return -1;
}

/// The balance margin is never more than this share of a support polygon's narrowest width (see
/// closestBalancedPoint): all of 3cm inside a two-footed stance, 1.5cm inside a single foot.
constexpr float kMarginWidthShare = 0.2f;

float cross2(const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

glm::vec2 closestOnSegment(const glm::vec2& a, const glm::vec2& b, const glm::vec2& p) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-12f) {
        return a;
    }
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + ab * t;
}

} // namespace

float BalanceController::unclassifiedMass() {
    return kUnclassifiedMass;
}

std::vector<float> BalanceController::assignMasses(const std::vector<std::string>& names) {
    const std::size_t n = names.size();
    std::vector<float> masses(n, kUnclassifiedMass);
    std::vector<int>   classOf(n, -1);

    // Count how many bones share each (class, side) so they split the segment's mass share.
    // Key = class * 4 + side slot.
    std::vector<int> shareCount(std::size(kSegmentTable) * 4, 0);
    auto slotOf = [](char side) { return side == 'l' ? 0 : (side == 'r' ? 1 : 2); };
    std::vector<char> side(n, 'c');
    for (std::size_t i = 0; i < n; ++i) {
        const std::string norm = normalizedName(names[i]);
        classOf[i] = classify(norm);
        side[i] = sideOf(norm, names[i]);
        if (classOf[i] >= 0) {
            ++shareCount[static_cast<std::size_t>(classOf[i]) * 4 +
                         static_cast<std::size_t>(slotOf(side[i]))];
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (classOf[i] >= 0) {
            const int count = shareCount[static_cast<std::size_t>(classOf[i]) * 4 +
                                         static_cast<std::size_t>(slotOf(side[i]))];
            masses[i] = kSegmentTable[classOf[i]].massFraction / static_cast<float>(std::max(1, count));
        }
    }
    return masses;
}

glm::vec3 BalanceController::centerOfMass(const std::vector<glm::vec3>& positions,
                                          const std::vector<int>& parents,
                                          const std::vector<float>& masses) {
    const std::size_t n = positions.size();
    if (n == 0 || masses.size() != n || parents.size() != n) {
        return glm::vec3(0.0f);
    }
    // Each bone's segment reaches to the mean of its anatomical children; accumulate child sums.
    std::vector<glm::vec3> childSum(n, glm::vec3(0.0f));
    std::vector<int>       childCount(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const int p = parents[i];
        if (p >= 0 && static_cast<std::size_t>(p) < n) {
            childSum[static_cast<std::size_t>(p)] += positions[i];
            ++childCount[static_cast<std::size_t>(p)];
        }
    }
    glm::vec3 weighted(0.0f);
    float total = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3 segmentEnd =
            childCount[i] > 0 ? childSum[i] / static_cast<float>(childCount[i]) : positions[i];
        weighted += masses[i] * 0.5f * (positions[i] + segmentEnd);
        total += masses[i];
    }
    return total > 0.0f ? weighted / total : glm::vec3(0.0f);
}

std::vector<glm::vec2> BalanceController::supportPolygon(std::vector<glm::vec2> points) {
    if (points.size() < 3) {
        return points; // a point or a segment — closestBalancedPoint handles the degeneracy
    }
    // Andrew's monotone chain, counter-clockwise.
    std::sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const glm::vec2& a, const glm::vec2& b) {
                                 return glm::dot(a - b, a - b) < 1e-10f;
                             }),
                 points.end());
    if (points.size() < 3) {
        return points;
    }
    std::vector<glm::vec2> hull(points.size() * 2);
    std::size_t k = 0;
    for (const glm::vec2& p : points) { // lower hull
        while (k >= 2 && cross2(hull[k - 2], hull[k - 1], p) <= 0.0f) --k;
        hull[k++] = p;
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = points.size() - 1; i-- > 0;) { // upper hull
        const glm::vec2& p = points[i];
        while (k >= lower && cross2(hull[k - 2], hull[k - 1], p) <= 0.0f) --k;
        hull[k++] = p;
    }
    hull.resize(k - 1); // last point == first
    return hull;
}

bool BalanceController::insidePolygon(const std::vector<glm::vec2>& hull, const glm::vec2& p) {
    if (hull.size() < 3) {
        return false;
    }
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const glm::vec2& a = hull[i];
        const glm::vec2& b = hull[(i + 1) % hull.size()];
        if (cross2(a, b, p) < -1e-7f) { // CCW hull: p must be left of (or on) every edge
            return false;
        }
    }
    return true;
}

glm::vec2 BalanceController::closestBalancedPoint(const std::vector<glm::vec2>& hull,
                                                  const glm::vec2& p, float margin) {
    return closestBalancedPoint(hull, insetPolygon(hull, margin), p, margin);
}

BalanceController::Inset BalanceController::insetPolygon(const std::vector<glm::vec2>& hull, float margin) {
    Inset out;
    if (hull.empty()) {
        return out;
    }
    // The centroid: what the degenerate cases pull toward, and what stands in for a polygon the
    // margin swallowed whole.
    glm::vec2 centroid(0.0f);
    for (const glm::vec2& v : hull) {
        centroid += v;
    }
    centroid /= static_cast<float>(hull.size());
    out.centroid = centroid;
    if (hull.size() < 3) {
        return out; // (a point or a segment: closestBalancedPoint handles those without an inset)
    }
    // A POLYGON: the balanced point is the nearest point of the hull shrunk by the margin — every edge moved inward and
    // the hull clipped by it. (Until 2026-09-20 this was the branch below for polygons too: the
    // nearest BOUNDARY point pulled the margin toward the CENTROID. That is the inset polygon only
    // where the centroid lies square behind the edge; off to one side the pull is oblique, and a
    // centre of mass crossing into the margin band found its "balanced point" a finite distance
    // away along the edge — the need stepped from zero to a centimetre and more, a CLIFF in the
    // solver's balance row. A chest bowed 23 degrees off the figure's heading ran along it: every
    // step across was refused whatever its size, six ticks of stall, then the spine twisted 10
    // degrees in one tick to get round.)
    // (The margin is a band inside a STANCE. A single foot's outline is 5cm wide at the heel
    // and a 3cm band swallows all of it but a sliver under the ball — the weight of a body
    // mid-step was asked 9cm further forward than the foot's middle. No more of a hull than
    // kMarginWidthShare of its narrowest width: the hull's own measure, not p's.)
    float narrowest = 1e30f;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const glm::vec2& a = hull[i];
        const glm::vec2 edge = hull[(i + 1) % hull.size()] - a;
        const float length = glm::length(edge);
        if (length < 1.0e-6f) {
            continue;
        }
        float across = 0.0f;
        for (const glm::vec2& v : hull) {
            across = std::max(across, (edge.x * (v.y - a.y) - edge.y * (v.x - a.x)) / length);
        }
        narrowest = std::min(narrowest, across);
    }
    float inset = std::max(std::min(margin, kMarginWidthShare * narrowest), 0.0f);
    for (int attempt = 0; attempt < 10; ++attempt, inset *= 0.5f) {
        std::vector<glm::vec2> inner = hull;
        for (std::size_t i = 0; i < hull.size() && inner.size() >= 3 && inset > 1.0e-6f; ++i) {
            const glm::vec2& a = hull[i];
            const glm::vec2 edge = hull[(i + 1) % hull.size()] - a;
            const float length = glm::length(edge);
            if (length < 1.0e-6f) {
                continue;
            }
            const glm::vec2 inward(-edge.y / length, edge.x / length); // (a CCW hull's left)
            std::vector<glm::vec2> kept;
            kept.reserve(inner.size() + 1);
            for (std::size_t k = 0; k < inner.size(); ++k) {
                const glm::vec2& u = inner[k];
                const glm::vec2& v = inner[(k + 1) % inner.size()];
                const float du = glm::dot(u - a, inward) - inset;
                const float dv = glm::dot(v - a, inward) - inset;
                if (du >= 0.0f) {
                    kept.push_back(u);
                }
                if ((du < 0.0f) != (dv < 0.0f)) {
                    kept.push_back(u + (v - u) * (du / (du - dv)));
                }
            }
            inner.swap(kept);
        }
        if (inner.size() < 3) {
            continue; // (the margin swallowed the hull: a smaller one — the hull's alone, not p's)
        }
        out.polygon.swap(inner);
        out.valid = true;
        return out;
    }
    return out; // (no inset kept three vertices: the centroid stands in)
}

glm::vec2 BalanceController::closestBalancedPoint(const std::vector<glm::vec2>& hull, const Inset& inset,
                                                  const glm::vec2& p, float margin) {
    if (hull.empty()) {
        return p;
    }
    if (hull.size() == 1) {
        return hull[0];
    }
    if (hull.size() >= 3) {
        if (!inset.valid) {
            return inset.centroid;
        }
        const std::vector<glm::vec2>& inner = inset.polygon;
        if (insidePolygon(inner, p)) {
            return p;
        }
        glm::vec2 nearest = inner[0];
        float nearest2 = 1e30f;
        for (std::size_t k = 0; k < inner.size(); ++k) {
            const glm::vec2 c = closestOnSegment(inner[k], inner[(k + 1) % inner.size()], p);
            const float d2 = glm::dot(c - p, c - p);
            if (d2 < nearest2) {
                nearest2 = d2;
                nearest = c;
            }
        }
        return nearest;
    }
    // Two points: the closest point on the segment, inset toward the centroid.
    const glm::vec2& centroid = inset.centroid;
    const bool inside = insidePolygon(hull, p);
    glm::vec2 best(0.0f);
    float bestDist2 = 1e30f;
    const std::size_t edges = hull.size() == 2 ? 1 : hull.size();
    for (std::size_t i = 0; i < edges; ++i) {
        const glm::vec2 c = closestOnSegment(hull[i], hull[(i + 1) % hull.size()], p);
        const float d2 = glm::dot(c - p, c - p);
        if (d2 < bestDist2) {
            bestDist2 = d2;
            best = c;
        }
    }
    if (inside && bestDist2 >= margin * margin) {
        return p; // safely inside: balanced as-is
    }
    // On/near/outside the boundary: come to the boundary point, inset toward the centroid.
    const glm::vec2 toCentroid = centroid - best;
    const float len = glm::length(toCentroid);
    return len > 1e-6f ? best + toCentroid * std::min(1.0f, margin / len) : best;
}

bool BalanceController::balanceCorrection(const std::vector<glm::vec3>& positions,
                                          const std::vector<int>& parents,
                                          const std::vector<float>& masses,
                                          const std::vector<glm::vec2>& hull, float margin,
                                          glm::vec2& correctionXZ) {
    correctionXZ = glm::vec2(0.0f);
    if (hull.empty()) {
        return false;
    }
    const glm::vec3 com = centerOfMass(positions, parents, masses);
    const glm::vec2 comXZ(com.x, com.z);
    const glm::vec2 target = closestBalancedPoint(hull, comXZ, margin);
    correctionXZ = target - comXZ;
    if (hull.size() < 3) {
        // Degenerate support (a point / the line between two point-feet): allow the foot-area
        // slack, and only correct the CoM to the edge of that band, not exactly onto the line.
        const float len = glm::length(correctionXZ);
        if (len <= kDegenerateSupportRadius) {
            correctionXZ = glm::vec2(0.0f);
            return false;
        }
        correctionXZ *= 1.0f - kDegenerateSupportRadius / len;
    }
    return glm::dot(correctionXZ, correctionXZ) > 1e-8f;
}

} // namespace pose
