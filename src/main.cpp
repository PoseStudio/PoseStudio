/**
 * @file main.cpp
 * @brief Application entry point: boots core services, then builds the main window.
 *
 * Startup order matters here — the database and preferences cache must be ready
 * before any UI that might read from them gets constructed.
 */

#include "database.h"
#include "menumanager.h"
#include "splashoverlay.h"
#include "constants.h"
#include "preferencesmanager.h"
#include "assetmanagerwidget.h"
#include "appproxystyle.h"
#include "viewport/viewportwidget.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QMainWindow>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QIcon>

/**
 * @brief Concatenates the app's QSS modules into one stylesheet, in cascade order
 *        (later files can override rules from earlier ones).
 */
static QString loadStylesheets() {
    QString combinedStyles;

    const QStringList filesToLoad = {
        QStringLiteral(":/resources/styles/global.qss"),
        QStringLiteral(":/resources/styles/_assetmanager.qss"),
        QStringLiteral(":/resources/styles/_menumanager.qss"),
        QStringLiteral(":/resources/styles/_preferences.qss")
    };

    for (const QString& filePath : filesToLoad) {
        QFile file(filePath);
        if (file.open(QFile::ReadOnly | QFile::Text)) {
            combinedStyles.append(file.readAll()).append(QLatin1Char('\n'));
        } else {
            qWarning() << "[!] Failed to load stylesheet module:" << filePath;
        }
    }
    return combinedStyles;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // App identity. Must be set before any QStandardPaths lookup: the per-user data dir the
    // database now lives in (AppDataLocation) is derived from these names, so initializeDatabase
    // below depends on it. Keep this first.
    QApplication::setApplicationName(QStringLiteral("PoseStudio"));
    QApplication::setApplicationDisplayName(QStringLiteral("PoseStudio"));

    // --- 1. Core services & data layer ---
    initializeDatabase(DbInitMode::Normal);

    // Sanity-check the connection now, with a clear error, rather than failing confusingly
    // the first time some unrelated widget tries to run a query later.
    if (!QSqlDatabase::database(QStringLiteral("db_conn")).isOpen()) {
        qCritical() << "Fatal Error: PoseStudio cannot launch without a valid database connection.";
        return -1;
    }

    PreferencesManager::instance().loadFromDatabase();

    // --- 2. Branding & theming ---
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/icon.png")));

    // Disable the OS-level slide/fade animations for all tooltips (the asset grid uses a
    // custom interactive tooltip and expects instant show/hide).
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);

    // AppProxyStyle layered over Fusion: forces the custom dark theme to render identically on
    // every platform (never the native OS style), and adds app-wide tweaks (tooltip delays,
    // submenu overlap, dimmed disabled icons). The proxy takes ownership of the Fusion base.
    app.setStyle(new AppProxyStyle(QStringLiteral("Fusion")));
    app.setStyleSheet(loadStylesheets());

    // --- 3. Main window layout ---
    QMainWindow mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION));

    // Size the window relative to the screen rather than using a fixed pixel size,
    // so it's usable on both small laptop displays and large monitors.
    if (QScreen *screen = app.primaryScreen()) {
        const QRect screenGeometry = screen->availableGeometry();
        mainWindow.resize(static_cast<int>(screenGeometry.height() * 1.4),
                          static_cast<int>(screenGeometry.height() * 0.80));
    }

    MenuManager *menuManager = new MenuManager(&mainWindow);
    menuManager->setupMenus();

    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);

    // The 3D viewport: a self-contained Vulkan-backed widget. Everything graphics-API
    // related lives behind this facade (see src/viewport/). If Vulkan is unavailable it
    // degrades to an inline message rather than failing to launch.
    pose::ViewportWidget *viewport = new pose::ViewportWidget(mainSplitter);

    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West);

    AssetManagerWidget *assetsTab = new AssetManagerWidget();
    sidePanel->addTab(assetsTab, QStringLiteral("Asset Manager"));
    sidePanel->addTab(new QWidget(), QStringLiteral("Properties")); // placeholder tab

    menuManager->setAssetManagerWidget(assetsTab);
    menuManager->setViewportWidget(viewport);

    mainSplitter->addWidget(viewport);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 500});

    mainWindow.setCentralWidget(mainSplitter);

    // --- 4. Execution ---
    mainWindow.show();

    // Boot branding screen; dismisses itself on the user's next click anywhere
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    // "Open with" / drag-onto-exe convenience: import any .obj paths passed on the command line.
    // ViewportWidget::importObj queues them until the renderer exists, so this is safe pre-show.
    const QStringList launchArgs = QCoreApplication::arguments();
    for (int i = 1; i < launchArgs.size(); ++i) {
        if (launchArgs.at(i).endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive)) {
            viewport->importObj(launchArgs.at(i));
        }
    }

    return app.exec();
}