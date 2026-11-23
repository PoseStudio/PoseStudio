// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetmanagerscope.cpp
 * @brief The scopes the application builds an AssetManagerWidget with. See the header.
 */

#include "assetmanagerscope.h"

#include "librarypaths.h"

#include <QCoreApplication>

AssetManagerScope AssetManagerScope::assetManager() {
    return {};
}

AssetManagerScope AssetManagerScope::materialsLibrary() {
    AssetManagerScope scope;
    scope.collections = QStringLiteral("materials");
    // Resolved on every call: the user library is looked up in the database (cached, and the
    // cache dropped when a library is added or removed), and the folder is created if it is
    // missing, so the tab always has its root.
    scope.roots = []() { return QStringList{LibraryPaths::ensureMaterialsDirectory()}; };
    // The widget's translation context: these are its strings, said once per scope.
    scope.searchPlaceholder = QCoreApplication::translate("AssetManagerWidget", "Search materials...");
    return scope;
}
