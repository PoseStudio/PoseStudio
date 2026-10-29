/**
 * @file balancecontroller.h
 * @brief The FBIK system's BALANCE MODEL: a mass model over the skeleton, the 2D support polygon
 *        between the planted contacts, and the shift that would bring the centre of mass back
 *        over it — what the joint solver's balance row holds and the rig's step policy reads.
 *
 * Every bone gets an approximate segment mass (biomechanical body-segment fractions, classified
 * from bone names tolerantly across figure generations — "lThighBend" and "l_thigh" both read as
 * a thigh; same-class same-side bones split their segment's share so a Bend+Twist pair doesn't
 * double a limb's mass). The posed CoM is the mass-weighted mean of each segment's midpoint. The
 * support polygon is the convex hull, in the ground (XZ) plane, of the planted ground contacts —
 * both feet when standing, one foot mid-step, knees+feet when kneeling. When the CoM's ground
 * projection leaves the (margin-inset) polygon, balanceCorrection returns the XZ shift that
 * brings it to the nearest balanced point (closestBalancedPoint: the nearest point of the inset
 * polygon). The joint solver's balance row (JointSolver::balanceNeed) holds that distance to
 * zero inside the solve, and the rig's step policy reads it as the balance EFFORT its step
 * trigger low-passes. Qt-free (std + GLM).
 */

#ifndef BALANCECONTROLLER_H
#define BALANCECONTROLLER_H

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace pose {

/**
 * @class BalanceController
 * @brief Stateless CoM / support-polygon math for the IK rig's balance loop.
 */
class BalanceController {
public:
    /// Per-bone segment masses (arbitrary consistent units — the CoM is a ratio) from tolerant
    /// name classification, split among same-class same-side bones. Unclassified bones (face,
    /// fingers, helper joints) get a token mass so they perturb, not dominate.
    static std::vector<float> assignMasses(const std::vector<std::string>& names);

    /// Mass-weighted center of the posed skeleton: each bone contributes at its segment midpoint
    /// (joint to the mean of its anatomical children; leaves contribute at the joint itself).
    static glm::vec3 centerOfMass(const std::vector<glm::vec3>& positions,
                                  const std::vector<int>& parents,
                                  const std::vector<float>& masses);

    /// Convex hull (counter-clockwise) of the contact points in the ground plane (x = world X,
    /// y = world Z). Degenerate inputs pass through (1 point / 2 collinear points stay as-is).
    static std::vector<glm::vec2> supportPolygon(std::vector<glm::vec2> contactPoints);

    /// True if @p p lies inside (or on) the counter-clockwise hull.
    static bool insidePolygon(const std::vector<glm::vec2>& hull, const glm::vec2& p);

    /// The support polygon SHRUNK by the balance margin (see closestBalancedPoint): every edge
    /// moved inward and the hull clipped by it, the margin halved until three vertices survive.
    /// A function of the hull and the margin alone — the solver builds it ONCE per descent
    /// (JointSolver::buildComModel) and reads it at every residual, where closestBalancedPoint
    /// used to rebuild it (an O(hull^2) clip in the innermost loop). `valid` is false for a hull
    /// under three points (the point / segment cases need none) and for one the margin swallowed
    /// whole (the centroid stands in).
    struct Inset {
        std::vector<glm::vec2> polygon;
        glm::vec2              centroid{0.0f};
        bool                   valid = false;
    };
    static Inset insetPolygon(const std::vector<glm::vec2>& hull, float margin);

    /// The balanced point nearest @p p: the nearest point of the hull SHRUNK by @p margin (@p p
    /// itself inside it) — continuous in @p p, which the solver's balance row depends on.
    /// Degenerate hulls (a single planted foot, one contact point) resolve to the point/segment.
    static glm::vec2 closestBalancedPoint(const std::vector<glm::vec2>& hull, const glm::vec2& p,
                                          float margin);
    /// ... with the hull's inset already built (insetPolygon).
    static glm::vec2 closestBalancedPoint(const std::vector<glm::vec2>& hull, const Inset& inset,
                                          const glm::vec2& p, float margin);

    /// One controller step: projects the posed CoM onto the ground plane and, if it isn't safely
    /// inside the support polygon, writes the XZ shift that would re-balance it to
    /// @p correctionXZ and returns true. Returns false when already balanced (or no polygon).
    static bool balanceCorrection(const std::vector<glm::vec3>& positions,
                                  const std::vector<int>& parents,
                                  const std::vector<float>& masses,
                                  const std::vector<glm::vec2>& hull, float margin,
                                  glm::vec2& correctionXZ);
};

} // namespace pose

#endif // BALANCECONTROLLER_H
