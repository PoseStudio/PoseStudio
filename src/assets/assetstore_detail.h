// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetstore_detail.h
 * @brief PRIVATE to the asset store's translation units (assetstore.cpp, assetfileops.cpp,
 *        assetpackage.cpp): the small path, file and JSON helpers all three use. Not part of the
 *        store's interface; include assetstore.h, assetfileops.h or assetpackage.h instead.
 *
 * Everything is inline, in AssetStore::detail; each of the three files opens the namespace for
 * itself (`using namespace AssetStore::detail;`).
 */

#ifndef ASSETSTORE_DETAIL_H
#define ASSETSTORE_DETAIL_H

#include "assetpaths.h"
#include "assetstore.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QSaveFile>
#include <QString>

#include <string>

namespace AssetStore::detail {

using nlohmann::json;

/// The size of the pieces a file is read in when it is compared or summed.
inline constexpr qint64 kChunk = 1 << 20;

/// A path in the module's one spelling (assetpaths.h).
inline QString clean(const QString& path) {
    return AssetScan::normalizePath(path);
}

/// How the store tells two FILES apart: by the file system's own rule. Names that differ only
/// by case are one file on Windows and on macOS's default volumes, two files on Linux — and
/// folded there too, `Wood.png` and `wood.png` of one folder were one key: the collector handed
/// back the first file's path for the second, and a material came out naming one map for two
/// slots. (The BROWSERS compare case-insensitively on every system — AssetScan::kPathCase: that
/// rule is about a path a user may have retyped, not about which file is which.)
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
inline constexpr Qt::CaseSensitivity kFileCase = Qt::CaseInsensitive;
#else
inline constexpr Qt::CaseSensitivity kFileCase = Qt::CaseSensitive;
#endif

/// A path as the store compares it — "is this the same file?", "does it lie under that
/// folder?" — and keys its sets by: cleaned, and case folded where kFileCase says so.
inline QString key(const QString& path) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return clean(path).toLower();
#else
    return clean(path);
#endif
}

inline bool samePath(const QString& a, const QString& b) {
    return key(a) == key(b);
}

/// True when @p path IS @p folder or lies under it.
inline bool within(const QString& path, const QString& folder) {
    const QString p = key(path);
    const QString f = key(folder);
    if (p == f) {
        return true;
    }
    return p.startsWith(f.endsWith(QLatin1Char('/')) ? f : f + QLatin1Char('/'));
}

inline bool readAll(const QString& path, QByteArray& out) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    out = in.readAll();
    return in.error() == QFileDevice::NoError;
}

inline bool writeAll(const QString& path, const QByteArray& bytes) {
    QSaveFile out(path); // (whole or not at all)
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
}

/// A copy must be the user's to remove again, whatever the source's attributes were.
inline void makeWritable(const QString& path) {
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
}

/// A file read as a JSON object (false: not there, not JSON, not an object).
inline bool readJson(const QString& path, json& root) {
    QByteArray bytes;
    if (!readAll(path, bytes)) {
        return false;
    }
    root = json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, /*allow_exceptions=*/false);
    return root.is_object();
}

inline bool writeJson(const QString& path, const json& root) {
    try {
        const std::string text = root.dump(2) + "\n"; // (materialFileText's own form)
        return writeAll(path, QByteArray(text.data(), static_cast<qsizetype>(text.size())));
    } catch (const json::exception&) {
        return false;
    }
}

/// The texture list of a saved material's JSON, or null when @p root is not one.
inline json* materialTextures(json& root) {
    if (!root.is_object() || !root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != "posestudio.material" || !root.contains("material") || !root["material"].is_object()) {
        return nullptr;
    }
    json& material = root["material"];
    if (!material.contains("textures") || !material["textures"].is_array()) {
        material["textures"] = json::array();
    }
    return &material["textures"];
}

inline bool isMaterial(json& root) {
    return materialTextures(root) != nullptr;
}

inline QString sourceOf(const json& entry) {
    if (!entry.is_object() || !entry.contains("source") || !entry["source"].is_string()) {
        return {};
    }
    return QString::fromStdString(entry["source"].get<std::string>());
}

/// A source as a path of THIS system: a file written on Windows may name `.textures\a.png`.
inline QString portable(const QString& source) {
    QString s = source;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return s;
}

inline QString storeRoot(const QString& folder) {
    return clean(folder) + QLatin1Char('/') + AssetStore::texturesFolderName();
}

/// The files of a walk that must see EVERYTHING below its root, hidden names included: the
/// texture stores are hidden folders (`.textures`), and on macOS and Linux Qt neither lists
/// nor enters a dot name unless the filter says so.
inline constexpr QDir::Filters kEveryFile = AssetScan::kFileFilter | QDir::Hidden;

inline AssetStore::Result failed(const QString& why) {
    AssetStore::Result r;
    r.error = why;
    return r;
}

} // namespace AssetStore::detail

#endif // ASSETSTORE_DETAIL_H
