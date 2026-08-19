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
#include "preferencesmanager.h"
#include "constants.h"
#include "viewport/viewportwidget.h"
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
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

/**
 * @brief Resolves the folder an import file dialog should start in: the folder of the last
 * import (persisted in Preferences), falling back to Documents when nothing is saved yet or
 * the stored folder was moved/deleted. Shared by every File → Import entry point.
 */
static QString lastImportStartDir() {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString startDir =
        PreferencesManager::instance().getValue(Constants::PREF_LAST_IMPORT_DIR, documents).toString();
    if (startDir.isEmpty() || !QDir(startDir).exists()) return documents;
    return startDir;
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
        ".DUF (DUF File)",
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
        } else if (std::strcmp(format, ".DUF (DUF File)") == 0) {
            // Rigged character figures: their own native scene format + pipeline (geometry + skeleton
            // + morphs + materials), listed here as a file format alongside the mesh formats.
            QObject::connect(action, &QAction::triggered, mainWindow, [this]() { importFigureFile(); });
        } else {
            action->setEnabled(false); // importer not built yet
        }
    }

    fileMenu->addAction(loadDualStateIcon("export"), "Export...")->setEnabled(false);
    fileMenu->addSeparator();

    // Pose I/O: save the current figure's joint rotations to a small text file and restore them
    // later. Enabled regardless of scene state; the handlers warn if no figure is loaded.
    QAction *savePoseAction = fileMenu->addAction("Save Pose...");
    QObject::connect(savePoseAction, &QAction::triggered, mainWindow, [this]() { savePoseFile(); });
    QAction *loadPoseAction = fileMenu->addAction("Load Pose...");
    QObject::connect(loadPoseAction, &QAction::triggered, mainWindow, [this]() { loadPoseFile(); });
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

    // Undo/Redo drive the viewport's shared edit stack (pose changes + Environment-panel lighting
    // gestures). The QAction shortcuts are what make Ctrl+Z/Ctrl+Y work app-wide — the panel's
    // controls take keyboard focus, so the viewport's own keyPressEvent alone wouldn't see the
    // keys after a dial edit. Text fields still keep their own Ctrl+Z: a focused QLineEdit accepts
    // the ShortcutOverride for its editing keys, which parks these window-level shortcuts.
    QAction *undoAction = editMenu->addAction(loadDualStateIcon("undo"), "Undo");
    undoAction->setShortcut(QKeySequence::Undo);
    QObject::connect(undoAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->undo();
    });
    QAction *redoAction = editMenu->addAction(loadDualStateIcon("redo"), "Redo");
    redoAction->setShortcut(QKeySequence::Redo);
    QObject::connect(redoAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->redo();
    });
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

    const QString path = QFileDialog::getOpenFileName(
        mainWindow, QStringLiteral("Import OBJ"), lastImportStartDir(),
        QStringLiteral("Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return; // user cancelled

    // Remember the folder this model came from for the next import.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importObj(path);
}

void MenuManager::importFigureFile() {
    if (!viewportWidget) return;

    const QString path = QFileDialog::getOpenFileName(
        mainWindow, QStringLiteral("Import Character Figure"), lastImportStartDir(),
        QStringLiteral("Figure Files (*.duf *.dsf)"));
    if (path.isEmpty()) return; // user cancelled

    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importFigure(path);
}

void MenuManager::savePoseFile() {
    if (!viewportWidget) return;
    if (!viewportWidget->hasPosableFigure()) {
        QMessageBox::information(mainWindow, QStringLiteral("Save Pose"),
                                QStringLiteral("Import a character figure before saving a pose."));
        return;
    }

    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QFileDialog::getSaveFileName(mainWindow, QStringLiteral("Save Pose"), documents,
                                                QStringLiteral("Pose Files (*.pose)"));
    if (path.isEmpty()) return; // user cancelled
    if (!path.endsWith(QStringLiteral(".pose"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".pose");
    }
    if (!viewportWidget->savePose(path)) {
        QMessageBox::warning(mainWindow, QStringLiteral("Save Pose"),
                             QStringLiteral("Could not write the pose file."));
    }
}

void MenuManager::loadPoseFile() {
    if (!viewportWidget) return;
    if (!viewportWidget->hasPosableFigure()) {
        QMessageBox::information(mainWindow, QStringLiteral("Load Pose"),
                                QStringLiteral("Import a character figure before loading a pose."));
        return;
    }

    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(mainWindow, QStringLiteral("Load Pose"),
                                                      documents,
                                                      QStringLiteral("Pose Files (*.pose)"));
    if (path.isEmpty()) return; // user cancelled
    if (!viewportWidget->loadPose(path)) {
        QMessageBox::warning(mainWindow, QStringLiteral("Load Pose"),
                             QStringLiteral("Could not read the pose file."));
    }
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
