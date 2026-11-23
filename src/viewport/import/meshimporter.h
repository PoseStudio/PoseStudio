// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file meshimporter.h
 * @brief The abstract interface a STATIC model format's importer implements.
 *
 * One concrete importer per file format (import/obj/ today), each turning a file on disk into the
 * format-neutral ModelData (modeldata.h). Importers are Qt-free — they take a std::string path
 * and return pure data; the Qt layer (ModelImportService) handles the UI, texture decoding, and
 * GPU upload. Instances are owned and dispatched by ImporterRegistry (importerregistry.h): add a
 * format by implementing this interface and registering it there. The rigged-character readers
 * (import/fbx/, gltf/, usd/), the native figure importer and the splat reader do NOT implement
 * it: each reports more than a ModelData (an import summary, a skeleton to conform, splats) and
 * has a Qt-facing service of its own.
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

    /// File extensions this importer handles, lowercase and without the dot, e.g. {"obj"}. A single
    /// importer may claim several. Used by ImporterRegistry to dispatch.
    virtual std::vector<std::string> extensions() const = 0;

    /// Parses @p path into a format-neutral ModelData (geometry + material colors + texture sources;
    /// textures are NOT decoded here — that's the Qt layer's job). Throws std::runtime_error on
    /// parse/IO failure.
    virtual ModelData load(const std::string& path) const = 0;
};

} // namespace pose

#endif // MESHIMPORTER_H
