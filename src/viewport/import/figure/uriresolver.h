/**
 * @file uriresolver.h
 * @brief Resolves the cross-file asset references used by the native figure scene format.
 *
 * A figure is spread across many files: a character preset (.duf) references a base figure (.dsf),
 * which references a UV set (.dsf), morphs (.dsf), etc. References are URIs like
 *   "/data/Vendor/FigureBase/basefigure.dsf#geometry"
 * — a URL-encoded, root-relative file path plus a "#fragment" naming an asset inside that file.
 * This resolver turns a URI into an on-disk path (searching the configured content roots),
 * URL-decodes it, and caches the parsed documents it loads so a shared base file isn't re-read and
 * re-inflated for every reference into it. Pure std (+ FigureDocument) — no Qt, no Vulkan.
 */

#ifndef URIRESOLVER_H
#define URIRESOLVER_H

#include "figuredocument.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

/// A URI split into its on-disk file and the asset id within that file.
struct ResolvedUri {
    std::string path;     ///< Absolute on-disk file path, or empty if it couldn't be resolved.
    std::string fragment; ///< URL-decoded asset id after '#', or empty if the URI had none.

    bool resolved() const { return !path.empty(); }
};

/**
 * @class UriResolver
 * @brief Maps figure-format URIs to files under the content roots, and caches loaded documents.
 */
class UriResolver {
public:
    /// @param contentRoots Content-root directories (each directly containing "data/").
    ///   Tried in order for root-relative ("/...") URIs.
    explicit UriResolver(std::vector<std::string> contentRoots);

    /// Splits @p uri at '#', URL-decodes both parts, and resolves the file part to an absolute path:
    /// a root-relative path ("/data/...") is tried under each content root (first existing wins); a
    /// relative path is resolved against @p referringFileDir (the directory of the file that
    /// contained the reference). The returned path is empty if nothing on disk matched.
    ResolvedUri resolve(const std::string& uri, const std::string& referringFileDir = "") const;

    /// Resolves @p uri and returns the (cached) parsed document it points at. Throws
    /// std::runtime_error if the URI can't be resolved or the file fails to load/parse.
    std::shared_ptr<const FigureDocument> loadDocument(const std::string& uri,
                                                     const std::string& referringFileDir = "");

    /// Resolves every URI and loads the not-yet-cached documents in parallel across cores, filling
    /// the cache so subsequent loadDocument calls are instant. The per-document work (file read +
    /// gzip inflate + JSON parse) is independent and dominates figure import when a preset reaches
    /// hundreds of morph files. Unresolvable/unparsable entries are skipped silently — the caller's
    /// own loadDocument reports those. Not itself thread-safe: call from one thread; the
    /// parallelism is internal.
    void prefetchDocuments(const std::vector<std::string>& uris,
                           const std::string& referringFileDir = "");

    /// Percent-decodes a URL-encoded string ("My%20Figures" -> "My Figures"). '+' is left literal (the
    /// figure format encodes spaces as %20, not '+').
    static std::string urlDecode(const std::string& s);

private:
    std::vector<std::string>                                        m_roots;
    std::unordered_map<std::string, std::shared_ptr<const FigureDocument>> m_cache; // keyed by abs path
};

} // namespace pose

#endif // URIRESOLVER_H
