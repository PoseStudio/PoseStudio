// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetlibraryprompt.cpp
 * @brief The "Add Asset Folder" dialog flow. See assetlibraryprompt.h.
 */

#include "assetlibraryprompt.h"

#include "assetlibraries.h"
#include "constants.h"
#include "preferencesmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

namespace AssetLibraryPrompt {

bool addLibrary(QWidget* parent) {
    // (A free function has no tr() of its own: its texts are looked up under the context
    // "AssetLibraryPrompt", context and text both literals at each call so an extractor finds them.)

    // Start the browser in the parent of the last folder added (persisted in Preferences), so
    // adding several sibling libraries doesn't mean re-navigating from home each time; a stored
    // folder that was since moved or deleted falls back to home.
    const QString startDir = PreferencesManager::instance().rememberedDirectory(
        Constants::PREF_LAST_ASSET_FOLDER_PARENT, QDir::homePath());

    const QString folderPath = QFileDialog::getExistingDirectory(
        parent, QCoreApplication::translate("AssetLibraryPrompt", "Select Asset Library Folder"), startDir);
    if (folderPath.isEmpty()) return false;

    switch (AssetLibraries::add(folderPath)) {
    case AssetLibraries::AddResult::Failed:
        return false; // logged by the repository
    case AssetLibraries::AddResult::AlreadyRegistered:
        // Tell the user instead of silently doing nothing — without feedback a re-add of an
        // existing folder looks like the add simply failed.
        QMessageBox::information(
            parent, QCoreApplication::translate("AssetLibraryPrompt", "Already Added"),
            QCoreApplication::translate("AssetLibraryPrompt",
                                        "That folder is already registered as an asset library:\n%1")
                .arg(folderPath));
        return false;
    case AssetLibraries::AddResult::Added:
        break;
    }

    // Remember this folder's parent as the default location for the next add.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_ASSET_FOLDER_PARENT,
                                            QFileInfo(folderPath).absolutePath());
    return true;
}

} // namespace AssetLibraryPrompt
