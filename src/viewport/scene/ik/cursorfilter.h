// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file cursorfilter.h
 * @brief The adaptive low-pass filter an IK drag runs its raw cursor target through before each
 *        solve tick — shared by the viewport and the IK harness so the two can never drift.
 *
 * The smoothing strength follows how far the smoothed target LAGS the raw one: near-still cursor
 * (the lag is pixel noise) — heavy smoothing keeps jitter out of the solver; fast deliberate
 * gesture (the lag is centimeters) — the filter opens all the way and follows tightly, so
 * smoothing costs almost no lag exactly when the user moves fast. The whole-body solve
 * (jointsolver.h) does not amplify target noise — the pose is a continuous function of the
 * target, so noise passes about 1:1, a pixel — and the filter only has to keep a NEAR-STILL cursor's jitter out
 * (alpha 0.30 at zero lag) and can open fully (alpha 1.0 from ~7mm of lag): its steady-state lag
 * at 0.45 m/s is ~3mm, and a fast flick pays nothing. Qt-free (GLM only).
 */

#ifndef CURSORFILTER_H
#define CURSORFILTER_H

#include <glm/glm.hpp>

namespace pose {

/// One drag's cursor filter state. seed() it at the grab point, then update() once per solve
/// tick with the latest raw target (the timer re-issues the last raw target while the cursor
/// rests, so the smoothed target keeps converging onto it).
struct IkCursorFilter {
    static constexpr float kAlphaMin = 0.30f;
    static constexpr float kAlphaMax = 1.0f;
    static constexpr float kAlphaGain = 100.0f; // per meter of lag

    glm::vec3 smoothed{0.0f};

    void seed(const glm::vec3& target) { smoothed = target; }

    /// Advances the filter toward @p raw and returns the new smoothed target.
    const glm::vec3& update(const glm::vec3& raw) {
        const float lag = glm::length(raw - smoothed);
        const float alpha = glm::clamp(kAlphaMin + lag * kAlphaGain, kAlphaMin, kAlphaMax);
        smoothed = glm::mix(smoothed, raw, alpha);
        return smoothed;
    }
};

} // namespace pose

#endif // CURSORFILTER_H
