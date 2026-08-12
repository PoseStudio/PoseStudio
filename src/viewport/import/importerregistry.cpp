/**
 * @file importerregistry.cpp
 * @brief Implementation of ImporterRegistry. See importerregistry.h.
 */

#include "importerregistry.h"

#include "obj/objimporter.h"

#include <algorithm>
#include <cctype>

namespace pose {

namespace {

// Lowercased file extension of @p path without the leading dot ("Model.OBJ" -> "obj"). Empty if the
// path has no extension. Only the final path component's dot counts, so "foo.bar/model" -> "".
std::string extensionOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return std::string();
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

ImporterRegistry::ImporterRegistry() {
    // The one explicit list of built-in importers. Add a new format here (and under import/<fmt>/).
    m_importers.push_back(std::make_unique<ObjImporter>());
}

const ImporterRegistry& ImporterRegistry::instance() {
    static const ImporterRegistry registry;
    return registry;
}

const MeshImporter* ImporterRegistry::forExtension(const std::string& extLower) const {
    if (extLower.empty()) {
        return nullptr;
    }
    for (const std::unique_ptr<MeshImporter>& importer : m_importers) {
        for (const std::string& ext : importer->extensions()) {
            if (ext == extLower) {
                return importer.get();
            }
        }
    }
    return nullptr;
}

const MeshImporter* ImporterRegistry::forPath(const std::string& path) const {
    return forExtension(extensionOf(path));
}

} // namespace pose
