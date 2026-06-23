#ifndef MENUMANAGER_H
#define MENUMANAGER_H

#include <QObject>
#include <QMainWindow>

class AssetManagerWidget;

/**
 * @class MenuManager
 * @brief Builds and owns the application's top menu bar.
 */
class MenuManager : public QObject {
    Q_OBJECT

public:
    explicit MenuManager(QMainWindow *parent = nullptr);

    /// Constructs the File/Edit/Help menus and attaches them to the main window.
    void setupMenus();

    /// Lets the Preferences dialog refresh the live asset tree after library changes, and
    /// wires up the Asset Manager's "Manage Asset Folders" action to open Preferences —
    /// without MenuManager needing to know anything else about AssetManagerWidget.
    void setAssetManagerWidget(AssetManagerWidget *widget);

    /// Opens the Preferences dialog, jumping straight to `initialTab` if given (e.g. "Assets"),
    /// or leaving it on whichever tab it last opened to otherwise. Shared by the Edit menu's
    /// "Preferences" action and any other entry point that wants a specific tab (e.g. the
    /// Asset Manager's "Manage Asset Folders" context menu action).
    void openPreferencesDialog(const QString &initialTab = QString());

private:
    QMainWindow *mainWindow;
    AssetManagerWidget *assetManagerWidget = nullptr;
};

#endif // MENUMANAGER_H