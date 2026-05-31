#include "database.h"
#include "menumanager.h"
#include "splashoverlay.h"
#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <QMessageBox>
#include <QRect>
#include <QScreen>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    int initDb = 0;
    if (initDb == 1) {
        initializeDatabase();
        QSqlDatabase db = connectDatabase();
        if (!db.open()) {
            qCritical() << "Database Error: Could not connect to SQLite database.";
            qCritical() << "Reason:" << db.lastError().text();
            return false;
        }

        QSqlQuery query(db);
        if (!query.exec("SELECT appErrorText FROM appErrors")) {
            QMessageBox::critical(nullptr, "Database Error 3", 
                                    "Failed to query database:\n" + query.lastError().text());
        }
        if (query.next()) {
            QString poseName = query.value("appErrorText").toString();
            QMessageBox::information(nullptr, "Pose Found", 
                                    "The name of the pose is: " + poseName);
        }
    }

    app.setWindowIcon(QIcon("resources/icon.png"));
    app.setStyle("Fusion");
    app.setStyleSheet("QMainWindow { background-color: #191a1b; }"
                      "QLabel { color: #ffffff; font-size: 18px; padding: 4px;}"
                      "QMenu { background-color: #323232; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px; }"
                      "QMenu::item { padding: 6px 4px 6px 6px; }"
                      "QMenu::item:selected { background-color: #555555; border-radius: 4px; }"
                      "QMenu::item:disabled { color: #929292;}"
                      "QMenuBar { background-color: #191a1b; color: white; padding: 4px; border-bottom: 1px solid #313131;}" 
                      "QMenuBar::item { padding: 6px 12px;}"
                      "QMenuBar::item:selected { background-color: #323232; border-radius: 4px; }");

    QMainWindow mainWindow;
    mainWindow.setWindowTitle("PoseStudio 0.0.015");

    QScreen *screen = app.primaryScreen();
    QRect screenGeometry = screen->availableGeometry();

    int windowHeight = screenGeometry.height() * 0.80;
    int windowWidth = screenGeometry.height() * 1.4;
    mainWindow.resize(windowWidth, windowHeight); 

    // --- NEW MODULAR MENU SETUP ---
    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();
    // ------------------------------

    mainWindow.show();

    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    return app.exec();
}