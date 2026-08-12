/**
 * @file uvparser.h
 * @brief Parses a figure UV-set document into UVs + per-corner seam overrides.
 *
 * A figure's UVs live in a separate file referenced by the geometry's default_uv_set URI. Most
 * vertices map 1:1 to a UV of the same index; where a UV seam runs, a polygon corner is redirected
 * to a different UV (so the mesh must be split there). This captures both: the UV array and the
 * sparse (polygon, base-vertex) -> uv-index override table. Pure std + GLM + nlohmann — no Qt.
 */

#ifndef UVPARSER_H
#define UVPARSER_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

/// A parsed UV set: the UV coordinates plus the seam overrides that redirect specific polygon
/// corners to a UV other than their base vertex's.
struct UvSet {
    std::vector<glm::vec2>                    uvs;
    std::unordered_map<uint64_t, uint32_t>    overrides; ///< key = (polygonIndex<<32)|baseVertex.

    bool empty() const { return uvs.empty(); }

    /// The UV index for base vertex @p baseVertex on polygon @p polygonIndex — the seam override if
    /// one exists, else the base vertex index itself (the default 1:1 mapping).
    uint32_t uvIndexFor(uint32_t polygonIndex, uint32_t baseVertex) const {
        const auto it = overrides.find((static_cast<uint64_t>(polygonIndex) << 32) | baseVertex);
        return it != overrides.end() ? it->second : baseVertex;
    }
};

/// Parses the uv_set_library entry named @p fragmentId from @p uvDoc (or the first entry if the
/// fragment isn't found). Returns an empty UvSet if the document carries no UV set.
UvSet parseUvSet(const nlohmann::json& uvDoc, const std::string& fragmentId);

} // namespace pose

#endif // UVPARSER_H
