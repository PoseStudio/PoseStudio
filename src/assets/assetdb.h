/**
 * @file assetdb.h
 * @brief Every Collections/Favorites SQL statement the Asset Manager runs, in one place.
 *
 * The Asset Manager keeps two kinds of virtual, database-backed sources: nested Collections
 * (AssetCollections rows, AssetCollectionParentID = 0 at the top level, each holding a flat
 * list of asset paths in AssetCollectionItems with a manual AssetCollectionItemSortOrder) and
 * the flat Favorites list (Favorites rows, FavoritePath unique, FavoriteSortOrder). Before
 * this header existed the statements were spread across the widget's context menus, drop
 * handlers and tree builders, several unchecked. Everything here returns success explicitly
 * and logs its own failure, so callers can keep the UI honest (A5: a drag-move never removes
 * an asset's only filing when the add half failed, because the move is one transaction here).
 * Library folders are NOT this module's business: those go through core/assetlibraries.h.
 *
 * More than one browser is built on this data (the Asset Manager and the Materials Library,
 * assets/assetmanagerscope.h). COLLECTIONS belong to one browser: every collection row carries
 * the SCOPE of the browser it was made in (AssetCollectionScope: "" = the Asset Manager), and
 * the three calls that are not about one known collection — list them, name a new one, create
 * one — take that scope. FAVORITES are one list for the whole application: a browser that looks
 * at part of the disk shows the favorites that lie there (the caller filters), and reordering
 * that part leaves the rest of the list as it was (setFavoriteOrder).
 *
 * Uses the shared application connection (appDatabase()); GUI thread only like the rest of the
 * data layer. QtSql only, no widgets.
 */

#ifndef ASSETDB_H
#define ASSETDB_H

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace AssetDb {

/// One AssetCollections row.
struct CollectionRow {
    int id = 0;
    QString name;
    int parentId = 0; ///< 0 = top level
};

/// Every collection of `scope`, ordered by name (COLLATE NOCASE, the order the tree shows
/// siblings in). One query for the whole hierarchy: the caller nests them in memory.
QList<CollectionRow> allCollections(const QString& scope = QString());

/// `baseName` if no sibling under `parentCollectionId` in `scope` already has it, otherwise
/// "baseName (2)", "baseName (3)", ... so repeated "New Collection" clicks under the same
/// parent each create a distinct collection instead of colliding on name. (The top level is
/// where the scope matters: two browsers' top-level collections all have parent 0.)
QString uniqueCollectionName(const QString& baseName, int parentCollectionId,
                             const QString& scope = QString());

/// Inserts a new collection of `scope` under `parentCollectionId` (0 = top level). Returns its
/// id, or -1.
int createCollection(const QString& name, int parentCollectionId, const QString& scope = QString());

bool renameCollection(int collectionId, const QString& newName);

/// The stored name of a collection (empty if it no longer exists): what an inline rename
/// reverts to when the new name is refused or the update fails.
QString collectionName(int collectionId);

/// Deletes the collection AND its items in one transaction (the tree row must only go when
/// this returns true). Sub-collections are the caller's concern: the UI only offers Delete
/// for collections that are empty (see collectionIsEmpty).
bool deleteCollection(int collectionId);

/// Re-parents one collection; descendants keep referencing its id, so they move with it.
bool reparentCollection(int collectionId, int newParentId);

/// True when the collection holds neither asset items nor sub-collections.
bool collectionIsEmpty(int collectionId);

/// Every collection's item paths, keyed by collection id: the one aggregate query behind the
/// tree's "does this collection hold an existing asset" icon state.
QHash<int, QStringList> collectionItemPathsByCollection();

/// A collection's item paths in the user's manual order (sort order, then insertion order).
QStringList collectionItemPaths(int collectionId);

/// Every favorited path in the user's manual order (sort order, then insertion order).
QStringList favoritePaths();

/// Files `path` into a collection, appended after the current max sort order.
bool addCollectionItem(const QString& path, int collectionId);
bool removeCollectionItem(const QString& path, int collectionId);

/// Adds `path` to Favorites (appended; a no-op if already favorited).
bool addFavorite(const QString& path);
bool removeFavorite(const QString& path);

/// Files many paths into a collection in one transaction (the "folder as collection" fill).
bool addCollectionItems(int collectionId, const QStringList& paths);

/// Where an asset is filed: Favorites, or a collection by id. Used to describe both halves of
/// a move.
struct Filing {
    bool favorites = false;
    int collectionId = -1;
};

/// Moves `path` from one filing to another ATOMICALLY: the remove and the add succeed or fail
/// together, so a failed add can never delete the asset's only filing.
bool moveItem(const QString& path, const Filing& from, const Filing& to);

/// Puts the favorites named in `pathsInOrder` into that order, in one transaction. The list may
/// be PART of the favorites (a browser that shows only those under its own folder, or a grid
/// that left out favorites whose files have gone): its members trade the places they hold among
/// themselves and every other favorite keeps its place.
bool setFavoriteOrder(const QStringList& pathsInOrder);

/// Counts every change of the Favorites list (add, remove, reorder) in this process. Favorites
/// are shared between the browsers, so one that is showing them compares this with the count it
/// displayed at to know its grid has gone stale behind another's edit.
int favoritesRevision();

/// Rewrites a collection's AssetCollectionItemSortOrder to match `pathsInOrder`, in one
/// transaction.
bool setCollectionOrder(int collectionId, const QStringList& pathsInOrder);

/// The asset filed as `oldPath` is at `newPath` now — renamed or moved on disk (assetstore.h):
/// its Favorites entry and every collection filing follow it, each keeping its place. Where the
/// new path is filed already (a favorite, or in the same collection), the old filing just goes.
/// One transaction.
bool repathItem(const QString& oldPath, const QString& newPath);

/// The asset filed as `path` is gone from disk: its Favorites entry and every collection filing
/// go with it. One transaction.
bool forgetItem(const QString& path);

} // namespace AssetDb

#endif // ASSETDB_H
