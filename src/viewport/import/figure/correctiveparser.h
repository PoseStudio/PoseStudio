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
 * importer drops it.
 *
 * The operations are read by formulaops (shared with the morph walk). A GATE is evaluated here in
 * the whole vocabulary — inv included: "this corrective's weight divided by that dial" is a
 * mult-stage `push <dial>?value, inv`. A DRIVER (sum stage) must fit the runtime program, which
 * has push / spline (smooth or linear) / mult / add: inv, neg, sub and div fold where their
 * operands are constants, and a corrective that applies one of them to a joint's angle, or uses
 * a step spline or an operation nobody knows, is refused whole — never carried with the operation
 * left out, which is another weight, not a nearby one.
 *
 * Pure std + nlohmann + GLM — no Qt, no Vulkan.
 */

#ifndef CORRECTIVEPARSER_H
#define CORRECTIVEPARSER_H

#include "modeldata.h" // PoseCorrective

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
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
    /// The document the modifier being parsed came from. A gate written as a same-file reference
    /// ("#Toggle?value" — no file part) names a channel in THIS document, which the resolver can't
    /// load by URI; resolveValue reads its default here instead. Set by the scan per document.
    const nlohmann::json* ownerDoc = nullptr;
    /// The figure's base cage size. A corrective's deltas are indexed by base vertex, so one whose
    /// morph says it was made for a geometry of another size is refused (0 = not known: no test).
    std::size_t cageVertexCount = 0;
};

/// If @p modifier (one modifier_library entry) is a *live* pose corrective — it carries sparse morph
/// deltas and at least one joint-rotation-driven formula whose driver joints are all in the skeleton,
/// and its gates didn't resolve to ~0 — fills @p out and returns true. Otherwise returns false (not a
/// corrective, an unknown driver joint, or gated off) and @p out is left unspecified.
/// @p refused (optional) is filled, and false returned, where the modifier IS a live corrective of
/// this skeleton by its formulas and still cannot be carried — it was made for another geometry,
/// or a formula of its weight uses an operation that cannot be read or expressed — with the
/// reason, for the import's report (@p out then carries the modifier's id and nothing else to
/// rely on); it is left empty in every other case.
bool parseCorrective(const nlohmann::json& modifier, const CorrectiveContext& ctx, PoseCorrective& out,
                     std::string* refused = nullptr);

} // namespace pose

#endif // CORRECTIVEPARSER_H
