/**
 * @file correctiveparser.h
 * @brief Detects and parses a pose-corrective (joint-driven corrective morph) from a modifier definition.
 *
 * A rigged figure ships corrective shapes that fire as a function of joint rotation to fix the mesh
 * crumpling at bent joints. Each is a morph modifier whose blend weight is driven, through a small
 * driver formula, by one or more joint-rotation channels. This turns one such modifier into the engine's
 * PoseCorrective IR: the sparse deltas plus the driver formula as a tiny stack program (the same
 * push/const/spline/mult vocabulary the native base correctives use). Character/enable gates (the
 * mult-stage drivers) are resolved to constants here so the runtime formula is rotation-only, and a
 * corrective whose gate resolves to 0 (its character isn't dialed in) is reported as "not live" so the
 * importer drops it. Pure std + nlohmann + GLM — no Qt, no Vulkan.
 */

#ifndef CORRECTIVEPARSER_H
#define CORRECTIVEPARSER_H

#include "modeldata.h" // PoseCorrective

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <unordered_set>

namespace pose {

/// What a corrective needs to resolve its non-rotation drivers (character/enable gates) at import.
struct CorrectiveContext {
    const std::unordered_set<std::string>* boneNames = nullptr; ///< Skeleton joints = valid rotation drivers.
    /// Resolves a value-channel driver URL ("…dsf#Channel?value") to its constant weight: the dialed
    /// weight if the preset dials that channel, else the channel's own default (0 = off).
    std::function<float(const std::string& url)> resolveValue;
};

/// If @p modifier (one modifier_library entry) is a *live* pose corrective — it carries sparse morph
/// deltas and at least one joint-rotation-driven formula whose driver joints are all in the skeleton,
/// and its gates didn't resolve to ~0 — fills @p out and returns true. Otherwise returns false (not a
/// corrective, an unknown driver joint, or gated off) and @p out is left unspecified.
bool parseCorrective(const nlohmann::json& modifier, const CorrectiveContext& ctx, PoseCorrective& out);

} // namespace pose

#endif // CORRECTIVEPARSER_H
