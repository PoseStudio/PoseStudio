#include "menumanager.h"
#include "splashoverlay.h"
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QApplication>
#include <QIcon>

// Constructor (Notice there is no 'void' here)
MenuManager::MenuManager(QMainWindow *parent) : QObject(parent), mainWindow(parent) {}

// Setup Function
void MenuManager::setupMenus() {
    if (!mainWindow) return;

    // FILE MENU
    QMenu *fileMenu = mainWindow->menuBar()->addMenu("File");

    QIcon newIcon;
    newIcon.addPixmap(QPixmap(":/resources/new.png"), QIcon::Normal);
    newIcon.addPixmap(QPixmap(":/resources/new-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(newIcon), "New...")->setEnabled(false);

    QIcon openIcon;
    openIcon.addPixmap(QPixmap(":/resources/open.png"), QIcon::Normal);
    openIcon.addPixmap(QPixmap(":/resources/open-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(openIcon), "Open...")->setEnabled(false);

    fileMenu->addAction("Open Recent...")->setEnabled(false);
    fileMenu->addSeparator();

    QIcon saveIcon;
    saveIcon.addPixmap(QPixmap(":/resources/save.png"), QIcon::Normal);
    saveIcon.addPixmap(QPixmap(":/resources/save-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(saveIcon), "Save")->setEnabled(false);

    fileMenu->addAction("Save As...")->setEnabled(false);
    fileMenu->addAction("Save Copy...")->setEnabled(false);
    fileMenu->addSeparator();

    QIcon importIcon;
    importIcon.addPixmap(QPixmap(":/resources/import.png"), QIcon::Normal);
    importIcon.addPixmap(QPixmap(":/resources/import-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(importIcon), "Import...")->setEnabled(false);

    QIcon exportIcon;
    exportIcon.addPixmap(QPixmap(":/resources/export.png"), QIcon::Normal);
    exportIcon.addPixmap(QPixmap(":/resources/export-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(exportIcon), "Export...")->setEnabled(false);

    fileMenu->addSeparator();
    
    QAction *quitAction = fileMenu->addAction("Quit");
    quitAction->setShortcuts({QKeySequence("Ctrl+Q"), QKeySequence::Quit});
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);

    // EDIT MENU
    QMenu *editMenu = mainWindow->menuBar()->addMenu("Edit");

    QIcon undoIcon;
    undoIcon.addPixmap(QPixmap(":/resources/undo.png"), QIcon::Normal);
    undoIcon.addPixmap(QPixmap(":/resources/undo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(undoIcon), "Undo")->setEnabled(false);

    QIcon redoIcon;
    redoIcon.addPixmap(QPixmap(":/resources/redo.png"), QIcon::Normal);
    redoIcon.addPixmap(QPixmap(":/resources/redo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(redoIcon), "Redo")->setEnabled(false);

    editMenu->addAction("Undo History...")->setEnabled(false);
    editMenu->addSeparator();

    QIcon copyIcon;
    copyIcon.addPixmap(QPixmap(":/resources/copy.png"), QIcon::Normal);
    copyIcon.addPixmap(QPixmap(":/resources/copy-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(copyIcon), "Copy")->setEnabled(false);

    editMenu->addAction("Paste")->setEnabled(false);
    editMenu->addSeparator();

    QIcon preferencesIcon;
    preferencesIcon.addPixmap(QPixmap(":/resources/preferences.png"), QIcon::Normal);
    preferencesIcon.addPixmap(QPixmap(":/resources/preferences-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(preferencesIcon), "Preferences")->setEnabled(false);

    // HELP MENU
    QMenu *helpMenu = mainWindow->menuBar()->addMenu("Help");

    helpMenu->addAction("Release Notes")->setEnabled(false);

    QIcon tutorialsIcon;
    tutorialsIcon.addPixmap(QPixmap(":/resources/tutorials.png"), QIcon::Normal);
    tutorialsIcon.addPixmap(QPixmap(":/resources/tutorials-d.png"), QIcon::Disabled);
    helpMenu->addAction(QIcon(tutorialsIcon), "Tutorials")->setEnabled(false);

    helpMenu->addAction("Support")->setEnabled(false);
    helpMenu->addSeparator();

    QIcon aboutIcon;
    aboutIcon.addPixmap(QPixmap(":/resources/about.png"), QIcon::Normal);
    aboutIcon.addPixmap(QPixmap(":/resources/about-d.png"), QIcon::Disabled);
    
    QAction *aboutAction = helpMenu->addAction(aboutIcon, "About PoseStudio");
    
    QObject::connect(aboutAction, &QAction::triggered, mainWindow, [this]() {
        SplashOverlay *splash = new SplashOverlay(this->mainWindow);
        splash->show();
    });
}