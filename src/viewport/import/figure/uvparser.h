/**
 * @file uvparser.h
 * @brief Parses a figure UV-set document into UVs + per-corner seam overrides.
 *
 * A figure's UVs live in a separate file. The geometry names its DEFAULT set (default_uv_set),
 * and each of a preset's scene materials may name the set ITS zone's maps are painted on
 * (`uv_set`, spread over the material's `groups` — read by sceneMaterialUvSets): a newer
 * generation's character commonly keeps the previous generation's skin maps and names that
 * generation's layout for the skin zones while its eyes and nails use the new one. The two
 * layouts differ by a few dozen UVs and seam entries — read through the default alone, every
 * skin feature landed a little off (a crack along the scalp's seam, a patch between the shoulder
 * blades, the throat's notch displaced; found 2026-10-26). Most vertices map 1:1 to a UV of the
 * same index; where a UV seam runs, a polygon corner is redirected to a different UV (so the mesh
 * must be split there). This captures both: the UV array and the sparse (polygon, base-vertex)
 * -> uv-index override table. Pure std + GLM + nlohmann — no Qt.
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
    /// The set's declared `vertex_count` (0 when absent). A set authored for ANOTHER cage is
    /// indexed by base vertex all the same and would scramble the mapping, so the importer refuses
    /// one whose count is not the geometry's.
    uint32_t                                  vertexCount = 0;

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
