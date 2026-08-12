/**
 * @file figuredocument.h
 * @brief Loads one native figure scene file (.duf/.dsf) into a parsed JSON tree.
 *
 * The native figure scene format is JSON, stored either as plain UTF-8 or gzip-compressed. This is
 * the lowest layer of the figure importer (import/figure/): it reads the file, transparently
 * inflates it if gzipped, and parses it. Everything above it (geometry/uv/node/skin/morph/material
 * parsers) works against the resulting nlohmann::json tree. Pure std + miniz + nlohmann — no Qt,
 * no Vulkan — so it stays trivially testable, like the rest of the import/ core.
 */

#ifndef FIGUREDOCUMENT_H
#define FIGUREDOCUMENT_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pose {

/// True if @p bytes begins with the gzip magic (1f 8b). Cheap header sniff, not a full validation.
bool isGzip(const std::vector<uint8_t>& bytes);

/// Inflates a gzip stream to its decompressed bytes. Parses the gzip container (header flags +
/// trailer) and raw-inflates the DEFLATE payload via miniz. Throws std::runtime_error if @p data is
/// not a well-formed gzip stream or inflation fails.
std::vector<uint8_t> gunzip(const uint8_t* data, std::size_t size);

/**
 * @class FigureDocument
 * @brief A parsed figure scene file: its JSON root, plus the load pipeline that produced it.
 */
class FigureDocument {
public:
    /// Reads @p path, gzip-inflating it if needed, and parses the JSON. Throws std::runtime_error on
    /// IO, decompression, or JSON-parse failure (the message names @p path).
    static FigureDocument loadFromFile(const std::string& path);

    /// Parses an in-memory buffer (already-decompressed or gzip; auto-detected). @p sourceName is
    /// only used to contextualise error messages. Throws std::runtime_error on failure.
    static FigureDocument loadFromBytes(std::vector<uint8_t> bytes, const std::string& sourceName);

    /// The parsed JSON tree (asset_info, geometry_library, node_library, modifier_library, scene, …).
    const nlohmann::json& root() const { return m_root; }

private:
    explicit FigureDocument(nlohmann::json root) : m_root(std::move(root)) {}

    nlohmann::json m_root;
};

} // namespace pose

#endif // FIGUREDOCUMENT_H
