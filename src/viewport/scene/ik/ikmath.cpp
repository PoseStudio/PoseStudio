/**
 * @file ikmath.cpp
 * @brief The IK module's shared rotation math (see ikmath.h): the Euler composition and its exact
 *        decomposition for the figure format's per-joint rotation orders, and the closest points
 *        of two segments.
 *
 * One policy here is load-bearing: eulerFromMatrix is the EXACT inverse of eulerMatrix for all
 * six orders — the middle angle comes from atan2(sin, cos) rather than asin so it stays
 * conditioned near gimbal lock, where the third angle folds into the first — and the pair lives
 * together so the composition the Armature poses with and the decomposition the head's righting
 * and the dials read back can never drift apart. (The shortest-arc quaternion and the signed
 * angle about an axis that the position-space solver needed went with it, 2026-10-29.)
 * Qt-free (std + GLM).
 */
#include "ikmath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace pose {

float closestSegmentPoints(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2,
                           const glm::vec3& q2, float& s, float& t) {
    constexpr float kEps = 1e-10f;
    const glm::vec3 d1 = q1 - p1;
    const glm::vec3 d2 = q2 - p2;
    const glm::vec3 r = p1 - p2;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);
    if (a <= kEps && e <= kEps) {
        s = t = 0.0f;
        return glm::length(p1 - p2);
    }
    if (a <= kEps) {
        s = 0.0f;
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= kEps) {
            t = 0.0f;
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > kEps ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return glm::length((p1 + d1 * s) - (p2 + d2 * t));
}

glm::mat4 eulerMatrix(const glm::vec3& degrees, const std::string& order) {
    glm::mat4 m(1.0f);
    for (const char c : order) {
        const int axis = axisIndexOf(c);
        if (axis < 0) {
            continue;
        }
        glm::vec3 axisVec(0.0f);
        axisVec[axis] = 1.0f;
        m = m * glm::rotate(glm::mat4(1.0f), glm::radians(degrees[axis]), axisVec);
    }
    return m;
}

glm::vec3 eulerFromMatrix(const glm::mat3& m, const std::string& order) {
    // Axis indices of the three applied rotations, M = Ri(a) · Rj(b) · Rk(c).
    int i = 0, j = 1, k = 2;
    if (order.size() >= 3) {
        const int oi = axisIndexOf(order[0]);
        const int oj = axisIndexOf(order[1]);
        const int ok = axisIndexOf(order[2]);
        if (oi >= 0 && oj >= 0 && ok >= 0 && oi != oj && oj != ok && oi != ok) {
            i = oi;
            j = oj;
            k = ok;
        }
    }
    // Parity: +1 for cyclic orders (XYZ, YZX, ZXY), -1 for the anticyclic ones. For
    // M = Ri(a)·Rj(b)·Rk(c) (right-handed principal rotations), the row-major element
    // M[i][k] = eps·sin(b); GLM is column-major, so row r / col c reads m[c][r].
    const float eps = (j == (i + 1) % 3) ? 1.0f : -1.0f;
    const float sb = glm::clamp(eps * m[k][i], -1.0f, 1.0f);
    // cos(b) from the rest of row i (cb·cc, ±cb·sc): far better conditioned near |b| = 90 than
    // asin alone, whose derivative blows up as sb -> ±1.
    const float cb = std::sqrt(m[i][i] * m[i][i] + m[j][i] * m[j][i]);
    const float b = std::atan2(sb, cb);
    float a = 0.0f;
    float c = 0.0f;
    if (cb > 1e-5f) {
        a = std::atan2(-eps * m[k][j], m[k][k]); // rows j,k of column k
        c = std::atan2(-eps * m[j][i], m[i][i]); // columns j,i of row i
    } else {
        // Gimbal lock (|b| = 90 deg): only a±c is determined. Fold c into a (c = 0); the row-major
        // elements M[j][i] / M[j][j] read sin/cos of that combined angle for every order.
        const float t = std::atan2(m[i][j], m[j][j]);
        a = (b > 0.0f) ? t : -t;
    }
    glm::vec3 outDegrees(0.0f);
    outDegrees[i] = glm::degrees(a);
    outDegrees[j] = glm::degrees(b);
    outDegrees[k] = glm::degrees(c);
    return outDegrees;
}

namespace {

// An angle in degrees brought into [-180, 180].
float wrapDegrees(float x) {
    return x - 360.0f * std::round(x / 360.0f);
}

} // namespace

glm::vec3 eulerFromMatrixNear(const glm::mat3& m, const std::string& order, const glm::vec3& near) {
    int i = 0, j = 1, k = 2;
    if (order.size() >= 3) {
        const int oi = axisIndexOf(order[0]);
        const int oj = axisIndexOf(order[1]);
        const int ok = axisIndexOf(order[2]);
        if (oi >= 0 && oj >= 0 && ok >= 0 && oi != oj && oj != ok && oi != ok) {
            i = oi;
            j = oj;
            k = ok;
        }
    }
    // The plain decomposition, and the same with the third-applied angle held at near's: M =
    // M' · Rk(c), M' decomposed (its third angle 0 by construction) — equal to the first away
    // from gimbal lock, and AT it the split of the folded angle that keeps the third where it was.
    glm::vec3 axisK(0.0f);
    axisK[k] = 1.0f;
    const glm::mat3 rk(glm::rotate(glm::mat4(1.0f), glm::radians(near[k]), axisK));
    glm::vec3       held = eulerFromMatrix(m * glm::transpose(rk), order);
    held[k] += near[k];
    const glm::vec3 bases[2] = {eulerFromMatrix(m, order), held};

    glm::vec3 best(0.0f);
    float     bestDist = -1.0f;
    for (const glm::vec3& base : bases) {
        // Each base and its other branch: Ri(a+180)·Rj(180-b)·Rk(c+180) = Ri(a)·Rj(b)·Rk(c).
        glm::vec3 flipped = base;
        flipped[i] += 180.0f;
        flipped[j] = 180.0f - flipped[j];
        flipped[k] += 180.0f;
        for (const glm::vec3& cand : {base, flipped}) {
            glm::vec3 out(0.0f);
            float     dist = 0.0f;
            for (int a = 0; a < 3; ++a) {
                // Nearest to near modulo 360, then into [-180, 180] (an exact ±180 keeps near's sign).
                const float d = wrapDegrees(cand[a] - near[a]);
                float       v = near[a] + d;
                if (v > 180.0f) {
                    v -= 360.0f;
                } else if (v < -180.0f) {
                    v += 360.0f;
                }
                out[a] = v;
                dist += d * d;
            }
            if (bestDist < 0.0f || dist < bestDist) {
                bestDist = dist;
                best = out;
            }
        }
    }
    return best;
}

} // namespace pose
