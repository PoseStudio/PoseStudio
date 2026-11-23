// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetnodeids.h
 * @brief The Asset Manager's node vocabulary: the ids that tag tree rows, the roles that tag
 *        grid items, and the helpers that format/parse them.
 *
 * The directory tree mixes physical folders (whose Qt::UserRole is the absolute path) with a
 * handful of "virtual" rows — Search Results, Favorites, the Collections header, each Collection,
 * and the separators — that are told apart purely by the string in that same role. Before this
 * header existed every module spelled those strings and the "COLLECTION_<id>" encoding by hand
 * (a dozen startsWith("COLLECTION_") sites, `mid(11)` to extract the id, "COLLECTION_%1" to build
 * it), so a typo or a changed prefix would silently break one site and not the others. Everything
 * that needs to recognise or produce a node id goes through here; nothing here has state.
 */

#ifndef ASSETNODEIDS_H
#define ASSETNODEIDS_H

#include <QString>
#include <QStringView>
#include <Qt>

namespace AssetNode {

// --- Reserved tree-row ids (Qt::UserRole on the tree's QStandardItems) ---
inline const QString SEARCH_ROOT      = QStringLiteral("SEARCH_ROOT");
inline const QString FAVORITES_ROOT   = QStringLiteral("FAVORITES_ROOT");
inline const QString COLLECTIONS_ROOT = QStringLiteral("COLLECTIONS_ROOT");
inline const QString SEPARATOR        = QStringLiteral("SEPARATOR");

/// A Collection row's id is "COLLECTION_<AssetCollectionID>".
inline const QString COLLECTION_PREFIX = QStringLiteral("COLLECTION_");

/// The tree-row id of the collection with database id `collectionId`.
inline QString collectionPath(int collectionId) {
    return COLLECTION_PREFIX + QString::number(collectionId);
}

/// True for a Collection row id (any nesting depth — the id carries no hierarchy).
inline bool isCollection(const QString& nodeId) {
    return nodeId.startsWith(COLLECTION_PREFIX);
}

/// The database id encoded in a Collection row id, or -1 if `nodeId` isn't one.
inline int collectionId(const QString& nodeId) {
    if (!isCollection(nodeId)) return -1;
    bool ok = false;
    const int id = QStringView(nodeId).mid(COLLECTION_PREFIX.size()).toInt(&ok);
    return ok ? id : -1;
}

/// True for the DB-backed sources (Favorites, any Collection): they list asset ITEMS held in
/// the database rather than a physical directory, so they show a plain title instead of a
/// breadcrumb, list no subfolders, and keep a manual (sortable) order.
inline bool isVirtual(const QString& nodeId) {
    return nodeId == FAVORITES_ROOT || isCollection(nodeId);
}

/// True for rows that are not browsable at all — separators and the "..." lazy-load
/// placeholders (empty id). Clicks, context menus and hit-state lookups all skip these.
inline bool isStructural(const QString& nodeId) {
    return nodeId.isEmpty() || nodeId == SEPARATOR;
}

/// Extra roles on tree items, beyond the Qt::UserRole node id.
enum TreeRole {
    /// bool — set on Search Results rows, whose display text is the truncated relative path
    /// ("a / b / matched"). The tree delegate greys the "a / b / " prefix and the click/menu
    /// handlers reduce the text to its last segment ONLY when this role is set — a collection
    /// the user named "Sci-Fi / Props" must render and title itself verbatim.
    SearchResult = Qt::UserRole + 1
};

} // namespace AssetNode

/// Roles on the asset grid's QListWidgetItems.
enum AssetGridRole {
    FilePath     = Qt::UserRole,     ///< QString: absolute path of the asset file, or of the subfolder for folder items
    ItemKind     = Qt::UserRole + 1, ///< QString: kFolderItemKind for subfolder shortcuts, empty for assets
    FileSize     = Qt::UserRole + 2, ///< qint64: asset file size in bytes (assets only; feeds the lazily-built tooltip)
    LastModified = Qt::UserRole + 3  ///< QDateTime: asset file mtime (assets only; ditto)
};

/// AssetGridRole::ItemKind value marking a subfolder shortcut item in the grid. Folder items open
/// the folder on double-click and are never draggable, favoritable or filed into collections.
inline const QString kFolderItemKind = QStringLiteral("FOLDER");

/// Dynamic property set on the grid (the QListWidget) while a hand-rolled asset drag is live.
/// AssetGridDelegate::paint reads it to suppress the per-item hover highlight in favour of the
/// between-items drop line.
inline constexpr const char* kGridDraggingProperty = "gridDragging";

#endif // ASSETNODEIDS_H
