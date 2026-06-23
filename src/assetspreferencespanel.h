/**
 * @file assetspreferencespanel.h
 * @brief "Assets" page of the Preferences dialog: manages the AssetLibraries table that
 *        drives which folders the Asset Manager browses.
 */

#ifndef ASSETSPREFERENCESPANEL_H
#define ASSETSPREFERENCESPANEL_H

#include "preferencespanel.h"

class QListWidget;
class QListWidgetItem;

/// Lists registered, user-manageable asset library folders and lets the user add or remove
/// them. The built-in "Maquettes" library (shipped with the app) is excluded here — it's not
/// user-manageable — though it still appears in the Asset Manager tree itself.
class AssetsPreferencesPanel : public PreferencesPanel {
    Q_OBJECT

public:
    explicit AssetsPreferencesPanel(QWidget* parent = nullptr);

signals:
    /// Emitted whenever a library is added or removed, so the live Asset Manager tree
    /// (which has its own in-memory copy) knows to refresh.
    void librariesChanged();

    /// Emitted when the user double-clicks a library row, asking to jump to it in the
    /// live Asset Manager tree.
    void navigateToLibraryRequested(const QString& path);

private:
    void reloadLibraries();
    void promptAddLibrary();
    void removeSelectedLibrary();

    QListWidget* m_libraryList = nullptr;
};

#endif // ASSETSPREFERENCESPANEL_H
