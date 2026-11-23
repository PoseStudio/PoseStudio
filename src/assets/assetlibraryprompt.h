// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetlibraryprompt.h
 * @brief "Add Asset Folder": the one dialog flow that registers a folder as an asset library.
 *
 * The flow has two entry points — the Asset Manager's own "Add Asset Folder" link and
 * Preferences → Assets — and both call this function, so they cannot drift apart (they were two
 * copies, and only one of them said anything when the folder was already registered).
 *
 * What it does: opens the folder picker where the last library was added from
 * (PREF_LAST_ASSET_FOLDER_PARENT), registers the chosen folder through the AssetLibraries
 * repository, tells the user when that folder is a library already, and remembers the folder's
 * parent for the next time. It refreshes nothing: the caller knows which views show libraries.
 */

#ifndef ASSETLIBRARYPROMPT_H
#define ASSETLIBRARYPROMPT_H

class QWidget;

namespace AssetLibraryPrompt {

/// Asks for a folder and registers it as an asset library. `parent` owns the dialogs.
/// @return true when a library was ADDED (the caller refreshes its views); false when the user
///         cancelled, the folder was registered already (the user has been told), or the
///         database refused it (logged by the repository).
bool addLibrary(QWidget* parent);

} // namespace AssetLibraryPrompt

#endif // ASSETLIBRARYPROMPT_H
