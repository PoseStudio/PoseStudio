/**
 * @file menumanager.h
 * @brief Declares MenuManager, which builds and owns the application's top menu bar.
 *
 * The menus are the shell's one dependency on the rest of the app: File → Import / pose I/O and
 * every Edit / View action drive the viewport through the ViewportWidget facade, and
 * Preferences (plus the Asset Manager's "Manage Asset Folders") open the PreferencesDialog from
 * one entry point, openPreferencesDialog(), which is also where the dialog's library-change
 * signals are wired back to the live Asset Manager. Both collaborators are registered after
 * setupMenus() (main.cpp builds the menus before the panels exist), so the actions capture
 * `this` and look the widgets up at trigger time rather than binding to them at build time.
 */

#ifndef MENUMANAGER_H
#define MENUMANAGER_H

#include <QObject>
#include <QMainWindow>

class AssetManagerWidget;
class QAction;

namespace pose {
class EnvironmentPanel;
class ViewportWidget;
}

/**
 * @class MenuManager
 * @brief Builds and owns the application's top menu bar.
 */
class MenuManager : public QObject {
    Q_OBJECT

public:
    explicit MenuManager(QMainWindow *parent = nullptr);

    /// Constructs the File/Edit/View/Help menus and attaches them to the main window.
    void setupMenus();

    /// Registers the live Asset Manager: wires its "Manage Asset Folders" action to open
    /// Preferences, and lets openPreferencesDialog() connect the Assets panel's change/navigate
    /// signals back to it each time the dialog opens — without MenuManager needing to know
    /// anything else about AssetManagerWidget.
    void setAssetManagerWidget(AssetManagerWidget *widget);

    /// Gives the File → Import actions a viewport to load models into. Must be called before the
    /// user can import (from main.cpp, once the viewport exists).
    void setViewportWidget(pose::ViewportWidget *viewport);

    /// Registers the live Environment tab so File → New can reset it to the startup defaults
    /// (dials + HDRI caption) alongside the scene. From main.cpp, once the panel exists.
    void setEnvironmentPanel(pose::EnvironmentPanel *panel);

    /// Opens the Preferences dialog, jumping straight to `initialTab` if given (e.g. "Assets"),
    /// or leaving it on whichever tab it last opened to otherwise. Shared by the Edit menu's
    /// "Preferences" action and any other entry point that wants a specific tab (e.g. the
    /// Asset Manager's "Manage Asset Folders" context menu action).
    void openPreferencesDialog(const QString &initialTab = QString());

    /// The shared unsaved-changes gate before replacing the scene (New / Open, and window close):
    /// Save / Discard Changes / Cancel. True = proceed — after Save only if the save actually
    /// happened; false = cancelled. Public because main.cpp's close guard calls it.
    bool confirmDiscardChanges();

private:
    /// File → New: a fresh launch — picker mirrors synced via signals, the Environment tab reset,
    /// then the scene itself (models / camera / lighting / HDRI / history) to startup defaults.
    void newScene();

    /// Opens a file dialog and imports the chosen OBJ into the viewport.
    void importObjFile();

    /// Opens a file dialog and imports the chosen native figure (`.duf`/`.dsf`) into the viewport.
    void importFigureFile();
    /// Open a rigged character in an interchange format (FBX, glTF / GLB, USD): one file dialog
    /// each, all through the figure route (ViewportWidget::importFigure dispatches by extension).
    void importFbxFile();
    void importGltfFile();
    void importUsdFile();
    void importVrmFile();
    void importRiggedFile(const QString &title, const QString &filter);
    /// Opens a file dialog and imports the chosen Gaussian splat scene (`.ply` / `.splat`).
    void importSplatFile();

    /// Saves / loads the current figure's pose to/from a `.pose` file via a file dialog.
    void savePoseFile();
    void loadPoseFile();

    /// File → Save / Save As: writes the whole scene (figures + poses, transforms, environment,
    /// camera) as a .pss project. Save reuses the document's path — falling back to Save As when
    /// there is none; both remember the last project folder.
    void saveProject();
    void saveProjectAs();

    /// File → Open: loads a .pss project — atomic parse first, link recovery for missing figure
    /// sources (per-row re-point pickers), then import + restore in document order.
    void openProjectFile();

    QMainWindow *mainWindow;
    AssetManagerWidget *assetManagerWidget = nullptr;
    pose::ViewportWidget *viewportWidget = nullptr;
    pose::EnvironmentPanel *environmentPanel = nullptr;
    QAction *m_showSkeletonAction = nullptr; // View → Show Skeleton; wired in setViewportWidget
    // Edit → Turn Joint with Mouse → Bend / Side-Side / Twist (B / S / T); wired and kept
    // enabled-with-the-selection in setViewportWidget.
    QAction *m_turnJointActions[3] = {nullptr, nullptr, nullptr};

    /// View → VR Mode (checkable) and Recenter VR View; wired in setViewportWidget.
    QAction *m_vrModeAction = nullptr;
    QAction *m_recenterVrAction = nullptr;
    /// The VR Mode entry's response when VR cannot start: why, and what to do — enable VR support
    /// (a restart, offered on the spot), or connect the headset and restart.
    void explainVrUnavailable();
    /// Restarts the application (the Factory Reset relaunch), after the unsaved-changes gate.
    void relaunchApplication();
};

#endif // MENUMANAGER_H
