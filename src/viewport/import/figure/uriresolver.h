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
 * re-inflated for every reference into it.
 *
 * Resolution order (see resolve()): the EXACT-case path is tried under every root first, and only
 * then does each root get the case-tolerant component walk. The tolerant walk exists for the
 * case-sensitive filesystems (Linux/macOS) where the URIs' mixed case ("one One" vs the on-disk
 * "ONE One") would miss; on NTFS the exact probe already absorbs case, so the walk never runs there.
 * Trying all roots exactly first matters with two or more roots: a file that lives under root[1]
 * used to pay a directory walk of root[0] on every reference. The walk's per-directory listings
 * are memoized on the resolver, which is why resolve() is const but NOT thread-safe — like the
 * document cache, call it from one thread (the parallel prefetch keeps its workers off both).
 * Pure std (+ FigureDocument) — no Qt, no Vulkan.
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
 *
 * Single-threaded by contract: resolve() memoizes directory listings and loadDocument() fills the
 * document cache. One resolver serves a whole figure import, follower addons included — the roots
 * are the same and the cache is what keeps a shared base file from being inflated per addon.
 */
class UriResolver {
public:
    /// @param contentRoots Content-root directories (each directly containing "data/").
    ///   Tried in order for root-relative ("/...") URIs.
    explicit UriResolver(std::vector<std::string> contentRoots);

    /// Splits @p uri at '#', URL-decodes both parts, and resolves the file part to an absolute path:
    /// a root-relative path ("/data/...") is tried under each content root — exact case under
    /// every root first, then case-tolerantly per root (first existing wins); a relative path is
    /// resolved against @p referringFileDir (the directory of the file that contained the
    /// reference). The returned path is empty if nothing on disk matched.
    ResolvedUri resolve(const std::string& uri, const std::string& referringFileDir = "") const;

    /// Resolves @p uri and returns the (cached) parsed document it points at. Throws
    /// std::runtime_error if the URI can't be resolved or the file fails to load/parse.
    std::shared_ptr<const FigureDocument> loadDocument(const std::string& uri,
                                                     const std::string& referringFileDir = "");

    /// The (cached) parsed document at an already-resolved URI — for callers that resolved the
    /// URI themselves (to test resolved() or read the fragment) and shouldn't pay a second
    /// resolution. Throws std::runtime_error if @p resolved is unresolved or the file fails to
    /// load/parse.
    std::shared_ptr<const FigureDocument> loadDocument(const ResolvedUri& resolved);

    /// The cached document for an absolute on-disk @p path (as loadDocument/prefetchDocuments
    /// keyed it — a resolve() result), or null if that file hasn't been loaded through this
    /// resolver. Lets a scan that walks directories on its own reuse documents already inflated
    /// instead of re-reading them from disk.
    std::shared_ptr<const FigureDocument> cached(const std::string& path) const;

    /// Resolves every URI and loads the not-yet-cached documents in parallel across cores, filling
    /// the cache so subsequent loadDocument calls are instant. The per-document work (file read +
    /// gzip inflate + JSON parse) is independent and dominates figure import when a preset reaches
    /// hundreds of morph files. Unresolvable/unparsable entries are skipped silently — the caller's
    /// own loadDocument reports those. Not itself thread-safe: call from one thread; the
    /// parallelism is internal.
    void prefetchDocuments(const std::vector<std::string>& uris,
                           const std::string& referringFileDir = "");

    /// Every file with the (lowercase, dotted) extension @p extension under the root-relative
    /// directory @p directoryUri ("/data/…/Morphs", percent-encoded or not), recursively, across
    /// ALL content roots — each as a root-relative URI resolve() takes ("/data/…/Some%20Morph.dsf":
    /// only '%', '#' and '?' are encoded, which is all resolve() needs). Lists names, opens
    /// nothing. For finding a file by its NAME when a reference gives an id and no file.
    std::vector<std::string> listFiles(const std::string& directoryUri,
                                       const std::string& extension) const;

    /// Percent-decodes a URL-encoded string ("My%20Figures" -> "My Figures"). Forwards to
    /// figureutils' urlDecode (kept here so resolver callers read naturally).
    static std::string urlDecode(const std::string& s);

private:
    /// Resolves `base / tail` tolerating case differences in any component; empty if not found.
    std::string resolveCaseTolerant(const std::string& base, const std::string& tail) const;
    /// The memoized lowercase-name -> on-disk-name map of one directory (built on first use).
    const std::unordered_map<std::string, std::string>& directoryListing(const std::string& dir) const;

    std::vector<std::string>                                        m_roots;
    std::unordered_map<std::string, std::shared_ptr<const FigureDocument>> m_cache; // keyed by abs path
    /// Per-directory listings for the case-tolerant walk: dir path -> (lowercased name -> actual
    /// name, first in iteration order). Mutable because resolve() is logically const.
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_listings;
};

} // namespace pose

#endif // URIRESOLVER_H
