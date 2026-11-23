// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file armaturesoftbodyshared.h
 * @brief What the soft body's translation units share — armaturesoftbody.cpp, armaturebreasts.cpp,
 *        armaturebreastfield.cpp, armaturebuttocks.cpp — and nobody else includes: the reader of
 *        the look's env levers, the trace switches, and when a mass counts as at rest.
 *
 * Each part's MODEL and its tuning constants stay in its own file: the breasts and the buttocks
 * were calibrated apart and their integrators are deliberately not one. Qt-free (std only).
 */

#ifndef ARMATURE_SOFT_BODY_SHARED_H
#define ARMATURE_SOFT_BODY_SHARED_H

#include <cstdlib>

namespace pose {

/// An A/B lever of the look (POSESTUDIO_BREAST_*, POSESTUDIO_BUTTOCK_*): the variable's value, or
/// @p fallback when it is unset or EMPTY. Read once by its caller, at start-up.
inline float softBodyEnv(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && *v ? static_cast<float>(std::atof(v)) : fallback;
}

/// POSESTUDIO_BREAST_TRACE=1: what the set-up read off the figure — each breast's profile and the
/// fullness it was given, a boneless breast's field, the cheeks' apexes. =2: the first breast's and
/// the left cheek's mass per tick as well.
inline const bool kSoftBodyTrace = std::getenv("POSESTUDIO_BREAST_TRACE") != nullptr;
inline const bool kSoftBodyTickTrace = kSoftBodyTrace && std::getenv("POSESTUDIO_BREAST_TRACE")[0] == '2';

/// A mass is STILL MOVING while it stands further than this from its rest (m), or travels faster
/// than this (m/s): what the settle's tail and the fall's tail wait for.
constexpr float kSoftBodyRestDistance = 0.0003f;
constexpr float kSoftBodyRestSpeed = 0.003f;

} // namespace pose

#endif // ARMATURE_SOFT_BODY_SHARED_H
