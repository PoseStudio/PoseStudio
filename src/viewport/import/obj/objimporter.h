/**
 * @file objimporter.h
 * @brief Wavefront .obj (+ referenced .mtl) importer, backed by tinyobjloader.
 *
 * The concrete MeshImporter (meshimporter.h). It triangulates the file, de-indexes unique
 * vertices, generates smooth normals when the file lacks them, groups faces by material, and reads
 * each material into the one material model: Kd / map_Kd as the base color and its map, the MTL
 * PBR extension (Pr, Pm, Pc, Ps, aniso, norm and their maps) and the classic channels (Ks as the
 * specular weight, Ns as the roughness, d / map_d, map_bump, disp, Ke, Ni, Tf) — the mapping is
 * described at the material block in objimporter.cpp. Texture names are resolved against the
 * .obj's folder. Pure data + std + GLM — no Vulkan, no Qt. The tinyobjloader implementation is
 * compiled in objimporter.cpp (the project's single TINYOBJLOADER translation unit).
 */

#ifndef OBJIMPORTER_H
#define OBJIMPORTER_H

#include "meshimporter.h"

#include "modeldata.h"

#include <string>
#include <vector>

namespace pose {

/**
 * @class ObjImporter
 * @brief Imports Wavefront .obj files into a format-neutral ModelData.
 */
class ObjImporter : public MeshImporter {
public:
    std::vector<std::string> extensions() const override { return {"obj"}; }

    /// Loads and triangulates @p path, resolving its .mtl relative to the file. Generates smooth
    /// normals when the file has none. Throws std::runtime_error on parse/IO failure and for a
    /// file that holds no face.
    ModelData load(const std::string& path) const override;
};

} // namespace pose

#endif // OBJIMPORTER_H
