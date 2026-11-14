/**
 * @file assetmanagerscope.h
 * @brief What one AssetManagerWidget browses: the plain description that tells the Asset
 *        Manager tab and the Materials Library tab apart.
 *
 * Both tabs are the SAME widget class, built twice — the tree over the grid, the search,
 * Favorites, Collections, the context menus, the drags: one code path, so a change to the
 * manager is a change to both. A scope is the whole of the difference, and it is data:
 *
 * - WHICH FOLDERS are the library roots. The Asset Manager's are the registered asset
 *   libraries (core/assetlibraries.h, managed in Preferences -> Assets): `roots` unset. A
 *   scoped browser has its own, resolved by a function each time the tree is rebuilt (the
 *   folder may move with the user's library): the Materials Library's one root is
 *   "My PoseStudio Library/Materials" (LibraryPaths::ensureMaterialsDirectory). Such a
 *   browser has nothing to manage in Preferences, so its menus leave "Manage Asset Folders"
 *   out, and it never shows the "Add Asset Folder" prompt.
 * - WHOSE COLLECTIONS it shows. Collections are made for one browser's content, so each
 *   carries the scope it was made in (`collections`, stored as AssetCollectionScope: "" is
 *   the Asset Manager); a browser lists, names and creates its own.
 * - FAVORITES are one list for the application. A scoped browser shows the favorites that lie
 *   under its roots: a material favorited in either tab is a favorite in both.
 * - The wording that names the content (`searchPlaceholder`).
 *
 * To add another library tab (poses, props): one more factory here and one addTab in main.cpp.
 */

#ifndef ASSETMANAGERSCOPE_H
#define ASSETMANAGERSCOPE_H

#include <QString>
#include <QStringList>

#include <functional>

struct AssetManagerScope {
    /// The collections this browser owns (AssetCollectionScope). "" = the Asset Manager.
    QString collections;

    /// The library roots, when they are this browser's own rather than the registered asset
    /// libraries. Called at every tree rebuild and when the tab is shown again.
    std::function<QStringList()> roots;

    /// The search field's placeholder. Empty = the Asset Manager's own ("Search assets...").
    QString searchPlaceholder;

    /// True for a browser with roots of its own (see the file comment for what follows from it).
    bool hasOwnRoots() const { return static_cast<bool>(roots); }

    /// The Asset Manager: every registered asset library.
    static AssetManagerScope assetManager();

    /// The Materials Library: the one folder "My PoseStudio Library/Materials".
    static AssetManagerScope materialsLibrary();
};

#endif // ASSETMANAGERSCOPE_H
