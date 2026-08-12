/**
 * @file objimporter.h
 * @brief Wavefront .obj (+ referenced .mtl) importer, backed by tinyobjloader.
 *
 * The first concrete MeshImporter (meshimporter.h). It triangulates the file, de-indexes unique
 * vertices, generates smooth normals when the file lacks them, groups faces by material, and records
 * each material's Kd as base color and the resolved path of its map_Kd. Pure data + std + GLM — no
 * Vulkan, no Qt. The tinyobjloader implementation is compiled in objimporter.cpp (the project's
 * single TINYOBJLOADER translation unit).
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
    std::string displayName() const override { return "Wavefront OBJ"; }
    std::vector<std::string> extensions() const override { return {"obj"}; }

    /// Loads and triangulates @p path, resolving its .mtl relative to the file. Generates smooth
    /// normals when the file has none. Throws std::runtime_error on parse/IO failure.
    ModelData load(const std::string& path) const override;
};

} // namespace pose

#endif // OBJIMPORTER_H
