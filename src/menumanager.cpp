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
    newIcon.addPixmap(QPixmap(":/resources/icons/new.png"), QIcon::Normal);
    newIcon.addPixmap(QPixmap(":/resources/icons/new-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(newIcon), "New...")->setEnabled(false);

    QIcon openIcon;
    openIcon.addPixmap(QPixmap(":/resources/icons/open.png"), QIcon::Normal);
    openIcon.addPixmap(QPixmap(":/resources/icons/open-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(openIcon), "Open...")->setEnabled(false);

    fileMenu->addAction("Open Recent...")->setEnabled(false);
    fileMenu->addSeparator();

    QIcon saveIcon;
    saveIcon.addPixmap(QPixmap(":/resources/icons/save.png"), QIcon::Normal);
    saveIcon.addPixmap(QPixmap(":/resources/icons/save-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(saveIcon), "Save")->setEnabled(false);

    fileMenu->addAction("Save As...")->setEnabled(false);
    fileMenu->addAction("Save Copy...")->setEnabled(false);
    fileMenu->addSeparator();

    QIcon importIcon;
    importIcon.addPixmap(QPixmap(":/resources/icons/import.png"), QIcon::Normal);
    importIcon.addPixmap(QPixmap(":/resources/icons/import-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(importIcon), "Import...")->setEnabled(false);

    QIcon exportIcon;
    exportIcon.addPixmap(QPixmap(":/resources/icons/export.png"), QIcon::Normal);
    exportIcon.addPixmap(QPixmap(":/resources/icons/export-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(exportIcon), "Export...")->setEnabled(false);

    fileMenu->addSeparator();
    
    QAction *quitAction = fileMenu->addAction("Quit");
    quitAction->setShortcuts({QKeySequence("Ctrl+Q"), QKeySequence::Quit});
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);

    // EDIT MENU
    QMenu *editMenu = mainWindow->menuBar()->addMenu("Edit");

    QIcon undoIcon;
    undoIcon.addPixmap(QPixmap(":/resources/icons/undo.png"), QIcon::Normal);
    undoIcon.addPixmap(QPixmap(":/resources/icons/undo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(undoIcon), "Undo")->setEnabled(false);

    QIcon redoIcon;
    redoIcon.addPixmap(QPixmap(":/resources/icons/redo.png"), QIcon::Normal);
    redoIcon.addPixmap(QPixmap(":/resources/icons/redo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(redoIcon), "Redo")->setEnabled(false);

    editMenu->addAction("Undo History...")->setEnabled(false);
    editMenu->addSeparator();

    QIcon copyIcon;
    copyIcon.addPixmap(QPixmap(":/resources/icons/copy.png"), QIcon::Normal);
    copyIcon.addPixmap(QPixmap(":/resources/icons/copy-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(copyIcon), "Copy")->setEnabled(false);

    editMenu->addAction("Paste")->setEnabled(false);
    editMenu->addSeparator();

    QIcon preferencesIcon;
    preferencesIcon.addPixmap(QPixmap(":/resources/icons/preferences.png"), QIcon::Normal);
    preferencesIcon.addPixmap(QPixmap(":/resources/icons/preferences-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(preferencesIcon), "Preferences")->setEnabled(false);

    // HELP MENU
    QMenu *helpMenu = mainWindow->menuBar()->addMenu("Help");

    helpMenu->addAction("Release Notes")->setEnabled(false);

    QIcon tutorialsIcon;
    tutorialsIcon.addPixmap(QPixmap(":/resources/icons/tutorials.png"), QIcon::Normal);
    tutorialsIcon.addPixmap(QPixmap(":/resources/icons/tutorials-d.png"), QIcon::Disabled);
    helpMenu->addAction(QIcon(tutorialsIcon), "Tutorials")->setEnabled(false);

    helpMenu->addAction("Support")->setEnabled(false);
    helpMenu->addSeparator();

    QIcon aboutIcon;
    aboutIcon.addPixmap(QPixmap(":/resources/icons/about.png"), QIcon::Normal);
    aboutIcon.addPixmap(QPixmap(":/resources/icons/about-d.png"), QIcon::Disabled);
    
    QAction *aboutAction = helpMenu->addAction(aboutIcon, "About PoseStudio");
    
    QObject::connect(aboutAction, &QAction::triggered, mainWindow, [this]() {
        SplashOverlay *splash = new SplashOverlay(this->mainWindow);
        splash->show();
    });
}