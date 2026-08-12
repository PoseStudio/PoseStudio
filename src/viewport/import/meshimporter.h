/**
 * @file meshimporter.h
 * @brief The abstract interface every model-format importer implements.
 *
 * One concrete importer per file format (import/obj/, and future import/gltf/, import/usd/, …), each
 * turning a file on disk into the format-neutral ModelData (modeldata.h). Importers are Qt-free —
 * they take a std::string path and return pure data; the Qt layer (ModelImportService) handles the
 * UI, texture decoding, and GPU upload. Instances are owned and dispatched by ImporterRegistry
 * (importerregistry.h): add a format by implementing this interface and registering it there.
 */

#ifndef MESHIMPORTER_H
#define MESHIMPORTER_H

#include <string>
#include <vector>

namespace pose {

struct ModelData;

/**
 * @class MeshImporter
 * @brief Parses one family of model file formats into a ModelData. Stateless and reusable.
 */
class MeshImporter {
public:
    virtual ~MeshImporter() = default;

    /// Human-readable format name, e.g. "Wavefront OBJ". For logs / future UI.
    virtual std::string displayName() const = 0;

    /// File extensions this importer handles, lowercase and without the dot, e.g. {"obj"}. A single
    /// importer may claim several (e.g. glTF's {"gltf", "glb"}). Used by ImporterRegistry to dispatch.
    virtual std::vector<std::string> extensions() const = 0;

    /// Parses @p path into a format-neutral ModelData (geometry + material colors + texture sources;
    /// textures are NOT decoded here — that's the Qt layer's job). Throws std::runtime_error on
    /// parse/IO failure.
    virtual ModelData load(const std::string& path) const = 0;
};

} // namespace pose

#endif // MESHIMPORTER_H
