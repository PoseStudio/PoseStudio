/**
 * @file morphparser.h
 * @brief Reads a morph asset's sparse vertex deltas (the displacements that reshape the base mesh).
 *
 * A figure's identity (and expressions, corrective shapes, …) is a set of *morphs*: each is a sparse
 * list of per-vertex offsets from the base mesh, dialed to some weight by the character preset. This
 * lifts one morph's deltas out of its file; the importer resolves and applies the preset's dialed
 * morphs (base position += weight · delta) before assembling the mesh. Pure std + GLM + nlohmann.
 */

#ifndef MORPHPARSER_H
#define MORPHPARSER_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pose {

/// One entry of a morph: the base-vertex index it displaces and the displacement (in the mesh's
/// native units).
using MorphDelta = std::pair<uint32_t, glm::vec3>;

/// Returns the deltas of the morph modifier named @p fragment in @p morphDoc (see
/// findMorphModifier for which modifier that is). Empty if it carries no morph — a control, a
/// scale, a formula-only driver. @p outVertexCount, when given, receives the vertex count of the
/// geometry the morph says it was authored for (its `vertex_count`), or 0 where it does not say:
/// a morph for ANOTHER geometry indexes other vertices, and must not be applied to this one.
std::vector<MorphDelta> parseMorphDeltas(const nlohmann::json& morphDoc, const std::string& fragment,
                                         std::size_t* outVertexCount = nullptr);

} // namespace pose

#endif // MORPHPARSER_H
