/**
 * @file menumanager.cpp
 * @brief Builds the application's top menu bar (File / Edit / Help).
 *
 * Most actions here are disabled placeholders for features that don't exist yet —
 * the goal of this file is just to establish the menu structure and icon conventions
 * future features will slot into.
 */

#include "menumanager.h"
#include "splashoverlay.h"
#include "preferencesdialog.h"
#include "assetmanagerwidget.h"
#include "viewport/viewportwidget.h"
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QIcon>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>
#include <cstring>

/**
 * @brief Loads a normal/disabled icon pair following the "name.png" / "name-d.png" convention.
 */
static QIcon loadDualStateIcon(const QString& baseName) {
    QIcon icon;
    icon.addPixmap(QPixmap(QStringLiteral(":/resources/icons/%1.png").arg(baseName)), QIcon::Normal);
    icon.addPixmap(QPixmap(QStringLiteral(":/resources/icons/%1-d.png").arg(baseName)), QIcon::Disabled);
    return icon;
}

MenuManager::MenuManager(QMainWindow *parent) : QObject(parent), mainWindow(parent) {}

void MenuManager::setupMenus() {
    if (!mainWindow) return;

    // =========================================================================
    // FILE MENU — document lifecycle, import/export, and application exit
    // =========================================================================
    QMenu *fileMenu = mainWindow->menuBar()->addMenu("File");

    fileMenu->addAction(loadDualStateIcon("new"), "New...")->setEnabled(false);
    fileMenu->addAction(loadDualStateIcon("open"), "Open...")->setEnabled(false);
    fileMenu->addAction("Open Recent...")->setEnabled(false);
    fileMenu->addSeparator();

    fileMenu->addAction(loadDualStateIcon("save"), "Save")->setEnabled(false);
    fileMenu->addAction("Save As...")->setEnabled(false);
    fileMenu->addAction("Save Copy...")->setEnabled(false);
    fileMenu->addSeparator();

    // Import submenu: one entry per supported file format, kept alphabetical. Only formats with a
    // working importer are enabled; the rest are disabled placeholders until their importer lands.
    QMenu *importMenu = fileMenu->addMenu(loadDualStateIcon("import"), "Import");
    const char *importFormats[] = {
        ".ABC (Alembic)",
        ".BVH (Biovision Hierarchy)",
        ".DAE (Collada)",
        ".FBX (FBX File)",
        ".GLB (GL Transmission Format .glTF)",
        ".OBJ (Wavefront)",
        ".PLY (Polygon File Format)",
        ".STL (Stereolithography)",
        ".USD (Universal Scene Description)",
    };
    for (const char *format : importFormats) {
        QAction *action = importMenu->addAction(format);
        if (std::strcmp(format, ".OBJ (Wavefront)") == 0) {
            QObject::connect(action, &QAction::triggered, mainWindow, [this]() { importObjFile(); });
        } else {
            action->setEnabled(false); // importer not built yet
        }
    }

    fileMenu->addAction(loadDualStateIcon("export"), "Export...")->setEnabled(false);
    fileMenu->addSeparator();

    QAction *quitAction = fileMenu->addAction("Quit");
    // Ctrl+Q covers Windows/Linux explicitly; QKeySequence::Quit adds the platform-standard
    // binding too (e.g. Cmd+Q on macOS), so both are listed rather than picking just one.
    quitAction->setShortcuts({QKeySequence("Ctrl+Q"), QKeySequence::Quit});
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);

    // =========================================================================
    // EDIT MENU — undo/redo history, clipboard, and preferences
    // =========================================================================
    QMenu *editMenu = mainWindow->menuBar()->addMenu("Edit");

    editMenu->addAction(loadDualStateIcon("undo"), "Undo")->setEnabled(false);
    editMenu->addAction(loadDualStateIcon("redo"), "Redo")->setEnabled(false);
    editMenu->addAction("Undo History...")->setEnabled(false);
    editMenu->addSeparator();

    editMenu->addAction(loadDualStateIcon("copy"), "Copy")->setEnabled(false);
    editMenu->addAction("Paste")->setEnabled(false);
    editMenu->addSeparator();

    QAction *preferencesAction = editMenu->addAction(loadDualStateIcon("preferences"), "Preferences");
    QObject::connect(preferencesAction, &QAction::triggered, mainWindow, [this]() {
        openPreferencesDialog();
    });

    // =========================================================================
    // HELP MENU — documentation, support links, and the About dialog
    // =========================================================================
    QMenu *helpMenu = mainWindow->menuBar()->addMenu("Help");

    helpMenu->addAction("Release Notes")->setEnabled(false);
    helpMenu->addAction(loadDualStateIcon("tutorials"), "Tutorials")->setEnabled(false);
    helpMenu->addAction("Support")->setEnabled(false);
    helpMenu->addSeparator();

    QAction *websiteAction = helpMenu->addAction(QIcon(":/resources/icons/globe.png"), "PoseStudio.org");
    QObject::connect(websiteAction, &QAction::triggered, mainWindow, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://posestudio.org")));
    });
    helpMenu->addSeparator();

    // "About" reuses the boot splash overlay for branding/version info
    QAction *aboutAction = helpMenu->addAction(loadDualStateIcon("about"), "About PoseStudio");
    QObject::connect(aboutAction, &QAction::triggered, mainWindow, [this]() {
        SplashOverlay *splash = new SplashOverlay(mainWindow);
        splash->show();
    });
}

void MenuManager::setAssetManagerWidget(AssetManagerWidget *widget) {
    assetManagerWidget = widget;
    if (!assetManagerWidget) return;

    QObject::connect(assetManagerWidget, &AssetManagerWidget::manageAssetFoldersRequested,
                      mainWindow, [this]() { openPreferencesDialog(QStringLiteral("Assets")); });
}

void MenuManager::setViewportWidget(pose::ViewportWidget *viewport) {
    viewportWidget = viewport;
}

void MenuManager::importObjFile() {
    if (!viewportWidget) return;

    const QString startDir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        mainWindow, QStringLiteral("Import OBJ"), startDir,
        QStringLiteral("Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return; // user cancelled

    viewportWidget->importObj(path);
}

void MenuManager::openPreferencesDialog(const QString &initialTab) {
    PreferencesDialog dialog(mainWindow);
    if (!initialTab.isEmpty()) dialog.selectTab(initialTab);

    if (assetManagerWidget) {
        QObject::connect(&dialog, &PreferencesDialog::assetLibrariesChanged,
                          assetManagerWidget, &AssetManagerWidget::refreshAssetManager);
        QObject::connect(&dialog, &PreferencesDialog::navigateToLibraryRequested,
                          assetManagerWidget, &AssetManagerWidget::navigateToLibraryRoot);
    }
    dialog.exec();
}
