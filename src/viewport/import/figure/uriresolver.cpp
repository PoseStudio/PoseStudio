/**
 * @file uriresolver.cpp
 * @brief Implementation of UriResolver. See uriresolver.h.
 */

#include "uriresolver.h"

#include "figureutils.h"
#include "parallelfor.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace pose {

namespace fs = std::filesystem;

std::string UriResolver::urlDecode(const std::string& s) {
    return pose::urlDecode(s);
}

UriResolver::UriResolver(std::vector<std::string> contentRoots) : m_roots(std::move(contentRoots)) {}

const std::unordered_map<std::string, std::string>&
UriResolver::directoryListing(const std::string& dir) const {
    const auto it = m_listings.find(dir);
    if (it != m_listings.end()) {
        return it->second;
    }
    // First visit: list the directory once. emplace keeps the FIRST entry per lowercased name, in
    // iteration order — the same match the original per-miss scan returned. A directory that
    // can't be listed (missing, not a directory) memoizes as empty, so it misses cheaply next time.
    std::unordered_map<std::string, std::string> listing;
    std::error_code dirEc;
    for (const auto& entry : fs::directory_iterator(fs::path(dir), dirEc)) {
        const std::string name = entry.path().filename().string();
        listing.emplace(toLowerAscii(name), name);
    }
    return m_listings.emplace(dir, std::move(listing)).first->second;
}

// Resolves `base / tail` tolerating case differences in any path component. Linux/macOS filesystems
// are case-sensitive, but the figure URIs mix cases (e.g. "one One" vs the on-disk "ONE One") that NTFS
// would absorb for free — so when the exact-case path is missing we re-derive it component by
// component, matching each name case-insensitively. Returns the actual on-disk path, or an empty
// string if it can't be found.
std::string UriResolver::resolveCaseTolerant(const std::string& base, const std::string& tail) const {
    std::error_code ec;
    fs::path current = base;
    for (const fs::path component : fs::path(tail)) {
        const fs::path exact = current / component;
        if (fs::exists(exact, ec)) {
            current = exact;
            continue;
        }
        // Exact case missing: look the component up case-insensitively in `current`'s listing.
        const std::unordered_map<std::string, std::string>& listing = directoryListing(current.string());
        const auto found = listing.find(toLowerAscii(component.string()));
        if (found == listing.end()) {
            return std::string(); // a component didn't match at all
        }
        current /= found->second;
    }
    std::error_code existsEc;
    if (!fs::exists(current, existsEc)) {
        return std::string();
    }
    return current.lexically_normal().string();
}

ResolvedUri UriResolver::resolve(const std::string& uri, const std::string& referringFileDir) const {
    // Split off the "#fragment" (asset id) first, then URL-decode each half independently.
    const std::size_t hash = uri.find('#');
    const std::string rawPath = (hash == std::string::npos) ? uri : uri.substr(0, hash);
    const std::string rawFrag = (hash == std::string::npos) ? std::string() : uri.substr(hash + 1);

    ResolvedUri result;
    result.fragment = urlDecode(rawFrag);

    const std::string relPath = urlDecode(rawPath);
    if (relPath.empty()) {
        return result; // fragment-only reference (into the current file); caller handles that
    }

    std::error_code ec;
    if (!relPath.empty() && (relPath.front() == '/' || relPath.front() == '\\')) {
        // Root-relative: try each content root. The leading slash makes it absolute within a root,
        // so we strip it before joining. First existing file wins. Two passes, ALL roots each: the
        // exact-case path first (free on NTFS — and with several roots a file under root[1] must
        // not pay a directory walk of root[0] first), then resolveCaseTolerant() re-derives it
        // case-insensitively for the case-sensitive filesystems (Linux/macOS) where the URIs'
        // mixed case would otherwise miss.
        const std::string tail = relPath.substr(1);
        for (const std::string& root : m_roots) {
            fs::path candidate = fs::path(root) / fs::path(tail);
            if (fs::exists(candidate, ec)) {
                result.path = candidate.lexically_normal().string();
                return result;
            }
        }
        for (const std::string& root : m_roots) {
            const std::string tolerant = resolveCaseTolerant(root, tail);
            if (!tolerant.empty()) {
                result.path = tolerant;
                return result;
            }
        }
        return result; // unresolved
    }

    // Relative reference: resolve against the directory of the referring file.
    if (!referringFileDir.empty()) {
        fs::path candidate = fs::path(referringFileDir) / fs::path(relPath);
        if (fs::exists(candidate, ec)) {
            result.path = candidate.lexically_normal().string();
        } else {
            const std::string tolerant = resolveCaseTolerant(referringFileDir, relPath);
            if (!tolerant.empty()) {
                result.path = tolerant;
            }
        }
    }
    return result;
}

std::shared_ptr<const FigureDocument> UriResolver::loadDocument(const std::string& uri,
                                                              const std::string& referringFileDir) {
    const ResolvedUri r = resolve(uri, referringFileDir);
    if (!r.resolved()) {
        throw std::runtime_error("Could not resolve figure reference to a file: " + uri);
    }
    return loadDocument(r);
}

std::shared_ptr<const FigureDocument> UriResolver::loadDocument(const ResolvedUri& resolved) {
    if (!resolved.resolved()) {
        throw std::runtime_error("Could not resolve figure reference to a file (unresolved reference)");
    }
    auto cached = m_cache.find(resolved.path);
    if (cached != m_cache.end()) {
        return cached->second;
    }
    auto doc = std::make_shared<FigureDocument>(FigureDocument::loadFromFile(resolved.path));
    m_cache.emplace(resolved.path, doc);
    return doc;
}

std::vector<std::string> UriResolver::listFiles(const std::string& directoryUri,
                                                const std::string& extension) const {
    std::vector<std::string> out;
    std::string              relDir = urlDecode(directoryUri);
    while (!relDir.empty() && (relDir.front() == '/' || relDir.front() == '\\')) {
        relDir.erase(relDir.begin());
    }
    if (relDir.empty()) {
        return out;
    }
    for (const std::string& root : m_roots) {
        std::error_code ec;
        fs::path        dir = fs::path(root) / fs::path(relDir);
        if (!fs::is_directory(dir, ec)) {
            const std::string tolerant = resolveCaseTolerant(root, relDir);
            if (tolerant.empty() || !fs::is_directory(fs::path(tolerant), ec)) {
                continue;
            }
            dir = tolerant;
        }
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec) ||
                toLowerAscii(it->path().extension().string()) != extension) {
                continue;
            }
            // Back to the root-relative form, with the three characters a URI gives a meaning to
            // escaped (a file name may hold a '%' or a '#').
            // (lexically: fs::relative would ask the filesystem about every one of them.)
            const std::string rel = it->path().lexically_relative(fs::path(root)).generic_string();
            if (rel.empty() || rel.rfind("..", 0) == 0) {
                continue;
            }
            std::string uri = "/";
            for (const char c : rel) {
                if (c == '%') {
                    uri += "%25";
                } else if (c == '#') {
                    uri += "%23";
                } else if (c == '?') {
                    uri += "%3F";
                } else {
                    uri.push_back(c);
                }
            }
            out.push_back(std::move(uri));
        }
    }
    return out;
}

std::shared_ptr<const FigureDocument> UriResolver::cached(const std::string& path) const {
    const auto it = m_cache.find(path);
    return it != m_cache.end() ? it->second : nullptr;
}

void UriResolver::prefetchDocuments(const std::vector<std::string>& uris,
                                    const std::string& referringFileDir) {
    // Resolve serially (cheap), collecting the unique paths not yet cached.
    std::vector<std::string> paths;
    paths.reserve(uris.size());
    for (const std::string& uri : uris) {
        const ResolvedUri r = resolve(uri, referringFileDir);
        if (r.resolved() && m_cache.find(r.path) == m_cache.end()) {
            paths.push_back(r.path);
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.empty()) {
        return;
    }

    // Load + inflate + parse across cores into a plain array (each index disjoint), then publish
    // to the cache serially — the cache map itself is never touched from the workers.
    std::vector<std::shared_ptr<const FigureDocument>> loaded(paths.size());
    parallelFor(static_cast<int>(paths.size()), [&](int i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        try {
            loaded[idx] = std::make_shared<FigureDocument>(FigureDocument::loadFromFile(paths[idx]));
        } catch (const std::exception&) {
            // Unreadable/unparsable — left null; the caller's loadDocument reports it.
        }
    });
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (loaded[i]) {
            m_cache.emplace(paths[i], std::move(loaded[i]));
        }
    }
}

} // namespace pose
