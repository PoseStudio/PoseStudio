// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file menumanager.h
 * @brief Declares MenuManager, which builds and owns the application's top menu bar.
 *
 * The menus are the shell's one dependency on the rest of the app: File → Import / pose I/O and
 * every Edit / View / Render action drive the viewport through the ViewportWidget facade, the
 * document's own entries (File → New / Open / Save / Save As) call DocumentActions, and
 * Preferences (plus the Asset Manager's "Manage Asset Folders") open the PreferencesDialog from
 * one entry point, openPreferencesDialog(), which is also where the dialog's library-change
 * signals are wired back to the live Asset Manager. The panels are registered after
 * setupMenus() (main.cpp builds the menus before the panels exist), so the actions capture
 * `this` and look the widgets up at trigger time rather than binding to them at build time.
 */

#ifndef MENUMANAGER_H
#define MENUMANAGER_H

#include <QList>
#include <QObject>
#include <QMainWindow>
#include <QPointer>

class AssetManagerWidget;
class DocumentActions;
class QAction;

namespace pose {
class ViewportWidget;
}

/**
 * @class MenuManager
 * @brief Builds and owns the application's top menu bar.
 */
class MenuManager : public QObject {
    Q_OBJECT

public:
    /// @p documents is what File → New / Open / Save / Save As and a restart call (see
    /// documentactions.h); it outlives the menus (both are the main window's).
    MenuManager(QMainWindow *parent, DocumentActions *documents);

    /// Constructs the File / Edit / View / Render / Help menus and attaches them to the main window.
    void setupMenus();

    /// Registers the live Asset Manager: wires its "Manage Asset Folders" action to open
    /// Preferences, and lets openPreferencesDialog() connect the Assets panel's change/navigate
    /// signals back to it each time the dialog opens — without MenuManager needing to know
    /// anything else about AssetManagerWidget.
    void setAssetManagerWidget(AssetManagerWidget *widget);

    /// Registers another library tab built on the Asset Manager's widget (the Materials
    /// Library): it is refreshed with the Asset Manager when Preferences changes the asset
    /// libraries, since a root of its own is resolved through them (the user's library).
    void addLibraryBrowser(AssetManagerWidget *widget);

    /// Gives the File → Import actions a viewport to load models into. Must be called before the
    /// user can import (from main.cpp, once the viewport exists).
    void setViewportWidget(pose::ViewportWidget *viewport);

    /// Opens the Preferences dialog, jumping straight to `initialTab` if given (e.g. "Assets"),
    /// or leaving it on whichever tab it last opened to otherwise. Shared by the Edit menu's
    /// "Preferences" action and any other entry point that wants a specific tab (e.g. the
    /// Asset Manager's "Manage Asset Folders" context menu action).
    void openPreferencesDialog(const QString &initialTab = QString());

private:
    /// Render → Render Image / Show Render Window: opens the Render window (src/render/) on the
    /// viewport's scene and, with @p start, begins a render. Explains itself in a build without
    /// the render engine.
    void renderImage(bool start);

    /// One File → Import entry: asks for a file under @p title and @p filter, starting in the
    /// folder the last import came from, and hands it to the viewport through @p route —
    /// importObj for a static model, importSplat for a Gaussian splat scene, importFigure for a
    /// native figure and for a rigged character in an interchange format (it dispatches by
    /// extension). The entries themselves are a table in setupMenus().
    using ImportRoute = void (pose::ViewportWidget::*)(const QString &);
    void importWithDialog(const QString &title, const QString &filter, ImportRoute route);

    /// Saves / loads the current figure's pose to/from a `.pose` file via a file dialog.
    void savePoseFile();
    void loadPoseFile();

    QMainWindow *mainWindow;
    DocumentActions *m_documents; ///< New / Open / Save and the restart (documentactions.h)
    AssetManagerWidget *assetManagerWidget = nullptr;
    QList<QPointer<AssetManagerWidget>> m_libraryBrowsers; ///< See addLibraryBrowser.
    pose::ViewportWidget *viewportWidget = nullptr;
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
};

#endif // MENUMANAGER_H
