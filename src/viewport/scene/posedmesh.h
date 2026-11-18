/**
 * @file posedmesh.h
 * @brief A mesh as posed, on the CPU: what Model::bakePosedMesh fills and a render engine is
 *        handed (through RenderSnapshot), and what scene/meshdisplace.h subdivides and displaces
 *        on the way. Plain std + GLM: no Vulkan, no Qt.
 */

#ifndef POSEDMESH_H
#define POSEDMESH_H

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace pose {

/// One zone's geometry in WORLD space: a vertex is a position, a unit shading normal, a UV
/// (v down the image) and a UV tangent with its handedness in w; three indices a triangle.
struct PosedMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec4> tangents;
    std::vector<uint32_t>  indices;
};

} // namespace pose

#endif // POSEDMESH_H
