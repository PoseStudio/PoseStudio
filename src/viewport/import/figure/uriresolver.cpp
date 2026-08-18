/**
 * @file uriresolver.cpp
 * @brief Implementation of UriResolver. See uriresolver.h.
 */

#include "uriresolver.h"

#include "parallelfor.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace pose {

namespace fs = std::filesystem;

std::string UriResolver::urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

UriResolver::UriResolver(std::vector<std::string> contentRoots) : m_roots(std::move(contentRoots)) {}

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
        // so we strip it before joining. First existing file wins (NTFS resolves case for us; a
        // case-folding pass would be needed for a case-sensitive filesystem — see header note).
        const std::string tail = relPath.substr(1);
        for (const std::string& root : m_roots) {
            fs::path candidate = fs::path(root) / fs::path(tail);
            if (fs::exists(candidate, ec)) {
                result.path = candidate.lexically_normal().string();
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
    auto cached = m_cache.find(r.path);
    if (cached != m_cache.end()) {
        return cached->second;
    }
    auto doc = std::make_shared<FigureDocument>(FigureDocument::loadFromFile(r.path));
    m_cache.emplace(r.path, doc);
    return doc;
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
