/**
 * @file importerregistry.h
 * @brief The single place that knows which model-import formats exist and maps a file to its importer.
 *
 * A process-wide, read-only catalogue of MeshImporter instances (meshimporter.h). Dispatch is by
 * file extension. Registration is one explicit list in the constructor (importerregistry.cpp) —
 * mirroring the explicit SHADER_SOURCES list in CMake — so adding a format is: implement MeshImporter
 * under import/<fmt>/, then add one push_back here. Qt-free.
 */

#ifndef IMPORTERREGISTRY_H
#define IMPORTERREGISTRY_H

#include "meshimporter.h"

#include <memory>
#include <string>
#include <vector>

namespace pose {

/**
 * @class ImporterRegistry
 * @brief Owns the built-in MeshImporters and resolves a file (by extension) to the one that loads it.
 */
class ImporterRegistry {
public:
    /// The shared, lazily-constructed registry. Construction registers every built-in importer.
    static const ImporterRegistry& instance();

    /// Importer whose extensions() include @p extLower (lowercase, no dot), or nullptr if none.
    const MeshImporter* forExtension(const std::string& extLower) const;

    /// Importer for @p path, chosen by its file extension, or nullptr if the format is unsupported.
    const MeshImporter* forPath(const std::string& path) const;

    /// All registered importers, in registration order.
    const std::vector<std::unique_ptr<MeshImporter>>& all() const { return m_importers; }

private:
    ImporterRegistry(); // registers the built-in importers

    ImporterRegistry(const ImporterRegistry&) = delete;
    ImporterRegistry& operator=(const ImporterRegistry&) = delete;

    std::vector<std::unique_ptr<MeshImporter>> m_importers;
};

} // namespace pose

#endif // IMPORTERREGISTRY_H
