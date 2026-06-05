/**
 * @file main.cpp
 * @brief Application entry point and primary bootstrap sequence for PoseStudio.
 * @details This file dictates the strict startup order of the application. It guarantees 
 * that core services (like the SQLite database and Preference Manager) are fully initialized 
 * and verified before any visual UI components or stylesheets are constructed.
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
 * @brief Reads and concatenates multiple QSS stylesheet modules into a single string.
 * @details Modules are loaded in a specific order to maintain a strict CSS cascade hierarchy. 
 * Marked static to confine linkage to this translation unit, improving compilation speed.
 * @return A single QString containing the compiled application stylesheet.
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
            // Append efficiency avoids creating temporary QString copies in memory
            combinedStyles.append(file.readAll()).append(QLatin1Char('\n')); 
            file.close();
        } else {
            // Log a warning to the console but allow the application to continue booting
            qWarning() << "[!] Failed to load stylesheet module:" << filePath;
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
    // 1. CORE SERVICES & DATA LAYER
    // =========================================================================

    // Initialize the SQLite data schema if this is a first-time run (0 = normal mode)
    initializeDatabase(0); 
    
    // Retrieve the active database connection to ensure readiness before UI construction.
    // Kept as a local const to prevent Qt shutdown lifecycle warnings.
    const QSqlDatabase persistentDb = QSqlDatabase::database(QStringLiteral("db_conn"));

    if (!persistentDb.isOpen()) {
        qCritical() << "Fatal Error: PoseStudio cannot launch without a valid database connection.";
        return -1;
    }

    // Pre-load user preferences into the memory cache
    PreferencesManager::instance().loadFromDatabase();

    // =========================================================================
    // 2. BRANDING & THEMING
    // =========================================================================

    app.setWindowIcon(QIcon(QStringLiteral(":/resources/icon.png")));
    
    // Enforce Qt's native 'Fusion' style to guarantee a consistent cross-platform baseline
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(loadStylesheets());

    // =========================================================================
    // 3. MAIN WINDOW LAYOUT
    // =========================================================================

    QMainWindow mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION));

    // Intelligently scale the initial window size based on the user's primary monitor
    if (QScreen *screen = app.primaryScreen()) {
        const QRect screenGeometry = screen->availableGeometry();
        mainWindow.resize(static_cast<int>(screenGeometry.height() * 1.4), 
                          static_cast<int>(screenGeometry.height() * 0.80));
    }

    // Bootstrap top-level menus
    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();

    // Construct the primary flexible UI workspace
    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);
    
    QWidget *viewportPlaceholder = new QWidget(mainSplitter);
    viewportPlaceholder->setStyleSheet(QStringLiteral("background-color: #1e1f22;")); 

    // Sidebar containing primary application tool tabs (e.g., Asset Manager, Properties)
    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West);
    
    AssetManagerWidget *assetsTab = new AssetManagerWidget();
    
    sidePanel->addTab(assetsTab, QStringLiteral("Asset Manager"));
    sidePanel->addTab(new QWidget(), QStringLiteral("Properties"));

    // Assemble the splitter and assign default flex ratios
    mainSplitter->addWidget(viewportPlaceholder);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 480}); 
    
    mainWindow.setCentralWidget(mainSplitter);

    // =========================================================================
    // 4. EXECUTION
    // =========================================================================

    // Display the main window structure immediately
    mainWindow.show();
    
    // Trigger the asynchronous splash overlay while heavy assets/plugins load
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    // Hand control over to the Qt Event Loop
    return app.exec();
}