/**
 * @file main.cpp
 * @brief Application entry point and primary bootstrap sequence for PoseStudio.
 * * This file handles the initialization of core services (like the database), 
 * UI theming, and the construction of the main application window and its 
 * primary layout components.
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
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QIcon>

// =========================================================================
// [ HELPER FUNCTIONS ]
// =========================================================================

/**
 * @brief Reads and concatenates multiple QSS stylesheet files.
 * * Modules are loaded in order to maintain a strict CSS hierarchy.
 * Marked static to confine linkage to this translation unit for faster builds.
 * * @return A single QString containing the complete application stylesheet.
 */
static QString loadStylesheets() {
    QString combinedStyles;
    
    // QStringLiteral optimizes memory by resolving strings at compile time
    const QStringList filesToLoad = {
        QStringLiteral(":/resources/styles/global.qss"),       
        QStringLiteral(":/resources/styles/_assetmanager.qss"),
        QStringLiteral(":/resources/styles/_menumanager.qss")
    };

    for (const QString& filePath : filesToLoad) {
        QFile file(filePath);
        if (file.open(QFile::ReadOnly | QFile::Text)) {
            // Append efficiency avoids creating temporary QString copies
            combinedStyles.append(file.readAll()).append(QLatin1Char('\n')); 
            file.close();
        } else {
            // Log a warning but continue loading other modules if one fails
            qWarning() << "Failed to load stylesheet module:" << filePath;
        }
    }
    return combinedStyles;
}

// =========================================================================
// [ MAIN EXECUTION ]
// =========================================================================

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // =========================================================================
    // [ CORE SERVICES & DATA LAYER ]
    // =========================================================================

    // Initializes the SQLite data schema if necessary (0 = normal execution mode)
    initializeDatabase(1); 
    
    // Retrieve the active database connection to ensure readiness.
    // Kept as a local const to prevent shutdown lifecycle warnings in Qt.
    const QSqlDatabase persistentDb = QSqlDatabase::database(QStringLiteral("db_conn"));

    if (!persistentDb.isOpen()) {
        qCritical() << "Fatal Error: PoseStudio cannot launch without a valid database connection.";
        return -1;
    }

    // Load user preferences into memory before generating UI elements
    PreferencesManager::instance().loadFromDatabase();

    // =========================================================================
    // [ BRANDING & THEMING ]
    // =========================================================================

    app.setWindowIcon(QIcon(QStringLiteral(":/resources/icon.png")));
    
    // Enforce Fusion style to guarantee a consistent cross-platform baseline
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(loadStylesheets());

    // =========================================================================
    // [ MAIN WINDOW LAYOUT ]
    // =========================================================================

    QMainWindow mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION));

    // Calculate window size dynamically based on primary monitor resolution
    if (QScreen *screen = app.primaryScreen()) {
        const QRect screenGeometry = screen->availableGeometry();
        // Set default dimensions to maintain aspect scale across varying DPIs
        mainWindow.resize(static_cast<int>(screenGeometry.height() * 1.4), 
                          static_cast<int>(screenGeometry.height() * 0.80));
    }

    // Initialize the top-level application menu structure
    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();

    // Main layout uses a horizontal splitter: [ 3D Viewport | Side Panel ]
    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);
    
    // Viewport area for 3D rendering engine (currently a placeholder widget)
    QWidget *viewportPlaceholder = new QWidget(mainSplitter);
    viewportPlaceholder->setStyleSheet(QStringLiteral("background-color: #1e1f22;")); 

    // Sidebar containing application tool tabs
    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West);
    
    // Add primary functional tabs to the side panel
    sidePanel->addTab(new AssetManagerWidget(), QStringLiteral("Asset Manager"));
    sidePanel->addTab(new QWidget(), QStringLiteral("Properties"));

    // Assemble the splitter and set default visual weight/ratio (Viewport vs Sidebar)
    mainSplitter->addWidget(viewportPlaceholder);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 480}); 
    
    mainWindow.setCentralWidget(mainSplitter);

    // =========================================================================
    // [ EXECUTION ]
    // =========================================================================

    // Display the main window and trigger the startup splash overlay
    mainWindow.show();
    
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    return app.exec();
}