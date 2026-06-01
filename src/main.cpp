#include "database.h"
#include "menumanager.h"
#include "splashoverlay.h"
#include "constants.h"
#include "preferencesmanager.h"
#include "assetmanagerwidget.h"
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QMainWindow>
#include <QMessageBox>
#include <QRect>
#include <QScreen>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSplitter>
#include <QTabWidget>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Change this to 0 when you want to stop resetting the database!
    int initDb = 0; 

    // One function call handles nuking, creating, reading the file, and connecting
    QSqlDatabase db = initializeDatabase(initDb);

    if (!db.isOpen()) {
        // If it failed to open, abort launching the app
        return false; 
    }

    PreferencesManager::instance().loadFromDatabase();

    // Get the Asset Folder value from the preferences table.
    QString assetFolder = PreferencesManager::instance().getValue("AssetDir", "").toString();

    app.setWindowIcon(QIcon(":/resources/icons/icon.png"));
    app.setStyle("Fusion");

    // --- LOAD EXTERNAL STYLESHEET ---
    QFile styleFile(":/resources/styles/style.qss");
    if (styleFile.open(QFile::ReadOnly)) {
        // Read the file and apply it to the whole app
        QString styleSheet = QLatin1String(styleFile.readAll());
        app.setStyleSheet(styleSheet);
        styleFile.close();
    } else {
        qWarning() << "Could not load the stylesheet from resources!";
    }
    // --------------------------------

    QMainWindow mainWindow;
    QString windowTitle = QString("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION);
    mainWindow.setWindowTitle(windowTitle);

    QScreen *screen = app.primaryScreen();
    QRect screenGeometry = screen->availableGeometry();

    int windowHeight = screenGeometry.height() * 0.80;
    int windowWidth = screenGeometry.height() * 1.4;
    mainWindow.resize(windowWidth, windowHeight); 

    // --- NEW MODULAR MENU SETUP ---
    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();
    // ------------------------------


    // --- MAIN UI LAYOUT ---
    
    // 1. Create the Splitter
    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);

    // 2. Create the Viewport FIRST (so it sits on the left side)
    QWidget *viewportPlaceholder = new QWidget(mainSplitter);
    viewportPlaceholder->setStyleSheet("background-color: #1e1f22;"); 

    // 3. Create the Side Panel SECOND (so it sits on the right side)
    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    
    // Change this to West so the tabs face the central viewport!
    sidePanel->setTabPosition(QTabWidget::West); 
    


    AssetManagerWidget *assetsTab = new AssetManagerWidget();


    QWidget *propertiesTab = new QWidget();
    sidePanel->addTab(assetsTab, "Asset Manager");
    sidePanel->addTab(propertiesTab, "Properties");

    // 4. Add them to the splitter in the new left-to-right order
    mainSplitter->addWidget(viewportPlaceholder);
    mainSplitter->addWidget(sidePanel);

    // 5. Flip the initial width ratios (e.g., 1500px for viewport, 350px for panel)
    mainSplitter->setSizes({1500, 350});

    // 6. Lock it into the window
    mainWindow.setCentralWidget(mainSplitter);

    // ----------------------



    mainWindow.show();

    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    return app.exec();
}