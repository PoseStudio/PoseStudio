/**
 * @file main.cpp
 * @brief Application entry point and primary bootstrap sequence for PoseStudio.
 * * This file initializes the core QApplication event loop. It establishes the 
 * global SQLite database connection, loads user preferences into memory, applies 
 * the unified CSS dark theme, and constructs the primary window layout 
 * (Viewport + Docked Side Panels).
 */

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

/**
 * @brief Main execution function.
 * @param argc Command line argument count.
 * @param argv Command line argument vector.
 * @return Application exit code (0 for success, non-zero for fatal errors).
 */
int main(int argc, char *argv[]) {
    // 1. Initialize the Qt Application framework
    QApplication app(argc, argv);

    // =========================================================================
    // [ CORE SERVICES & DATA LAYER ]
    // =========================================================================

    // DANGER: Setting initDb to 1 executes a destructive factory reset of the local DB!
    // This must strictly default to 0 for production and community builds.
    int initDb = 0; 

    // Establish connection pool and execute schema validation
    QSqlDatabase db = initializeDatabase(initDb);
    if (!db.isOpen()) {
        qCritical() << "Fatal Error: PoseStudio cannot launch without a valid database connection.";
        return -1; // Return a non-zero exit code to alert the OS of a failure
    }

    // Load global application settings into the Singleton manager
    PreferencesManager::instance().loadFromDatabase();

    // =========================================================================
    // [ BRANDING & THEMING ]
    // =========================================================================

    app.setWindowIcon(QIcon(":/resources/icon.png"));
    
    // Enforce the 'Fusion' style to override native OS variations (Windows/macOS)
    // This guarantees our custom CSS renders consistently across all platforms.
    app.setStyle("Fusion");

    // Inject global stylesheet
    QFile styleFile(":/resources/styles/style.qss");
    if (styleFile.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(styleFile.readAll());
        app.setStyleSheet(styleSheet);
        styleFile.close();
    } else {
        qWarning() << "UI Warning: Could not locate compiled style.qss in resources.";
    }

    // =========================================================================
    // [ MAIN WINDOW GEOMETRY ]
    // =========================================================================

    QMainWindow mainWindow;
    QString windowTitle = QString("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION);
    mainWindow.setWindowTitle(windowTitle);

    // Dynamically scale the initial window size based on the user's primary monitor
    QScreen *screen = app.primaryScreen();
    QRect screenGeometry = screen->availableGeometry();

    int windowHeight = screenGeometry.height() * 0.80;
    int windowWidth  = screenGeometry.height() * 1.4;
    mainWindow.resize(windowWidth, windowHeight); 

    // Attach modular top menu bar
    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();

    // =========================================================================
    // [ WORKSPACE LAYOUT ARCHITECTURE ]
    // =========================================================================

    // The primary horizontal splitter dividing the 3D space from the UI panels
    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);

    // --- 1. Central 3D Viewport (Left) ---
    // TODO: Replace this placeholder with the OpenGL/Vulkan rendering context
    QWidget *viewportPlaceholder = new QWidget(mainSplitter);
    viewportPlaceholder->setStyleSheet("background-color: #1e1f22;"); 

    // --- 2. Tool & Asset Panels (Right) ---
    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West); // Anchor tabs to the inner spine
    
    AssetManagerWidget *assetsTab = new AssetManagerWidget();
    QWidget *propertiesTab = new QWidget(); // Placeholder for Node/Material properties
    
    sidePanel->addTab(assetsTab, "Asset Manager");
    sidePanel->addTab(propertiesTab, "Properties");

    // Assemble the splitter in strict Left-to-Right order
    mainSplitter->addWidget(viewportPlaceholder);
    mainSplitter->addWidget(sidePanel);

    // Establish default workspace ratios (heavily favoring the 3D viewport)
    mainSplitter->setSizes({1500, 480});

    // Lock the fully assembled workspace into the main window
    mainWindow.setCentralWidget(mainSplitter);

    // =========================================================================
    // [ EXECUTION ]
    // =========================================================================

    mainWindow.show();

    // Overlay the splash screen while background threads (like scanning) boot up
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    // Hand control over to the Qt Event Loop
    return app.exec();
}