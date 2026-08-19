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
#include "environmentpanel.h"
#include "viewport/viewportwidget.h"

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QMainWindow>
#include <QMenu>
#include <QPainterPath>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QIcon>

namespace {
/**
 * @brief Strips the heavy native drop shadow from every popup menu, application-wide.
 *
 * Rounded menus (the border-radius in _menumanager.qss) are *translucent* popup windows, which Windows
 * gives a heavy native drop shadow. Setting Qt::NoDropShadowWindowHint on each menu removes that shadow
 * while keeping the rounded corners. It's applied on the menu's Polish event — before its native window
 * is created, so the shadow never appears — which covers menu-bar dropdowns, their submenus, context
 * menus, and the viewport's shader menu alike, from one place.
 */
class MenuShadowFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* menu = qobject_cast<QMenu*>(watched);
        if (!menu) {
            return QObject::eventFilter(watched, event);
        }
        if (event->type() == QEvent::Polish) {
            menu->setWindowFlag(Qt::NoDropShadowWindowHint, true);
            menu->setAttribute(Qt::WA_TranslucentBackground, true);
        } else if (event->type() == QEvent::Resize) {
            // NoDropShadowWindowHint leaves a 2-3px opaque black speck at each sharp outer corner (the
            // triangle outside the border-radius won't clear to transparent). Clip the popup to its
            // rounded-rect shape so those specks are cut away — the mask radius matches the 4px QSS
            // border-radius, so it removes only the corner triangles and not the visible border.
            QPainterPath path;
            path.addRoundedRect(QRectF(menu->rect()), 4, 4);
            menu->setMask(QRegion(path.toFillPolygon().toPolygon()));
        }
        return QObject::eventFilter(watched, event);
    }
};
} // namespace

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
        QStringLiteral(":/resources/styles/_preferences.qss"),
        QStringLiteral(":/resources/styles/_environment.qss")
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

    // Strip the native drop shadow from every popup menu app-wide, keeping their rounded corners
    // (see MenuShadowFilter). Parented to the app so it lives for the whole session.
    app.installEventFilter(new MenuShadowFilter(&app));

    // --- 3. Main window layout ---
    QMainWindow mainWindow;
#ifdef NDEBUG
    mainWindow.setWindowTitle(QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION));
#else
    // Mark unoptimized builds in the title: a Debug build is ~5x slower at figure import (MSVC
    // debug-STL overhead), which has been mistaken for a real performance regression when a Debug
    // window was benchmarked against other applications' shipped binaries.
    mainWindow.setWindowTitle(QStringLiteral("%1 %2 [Debug build — slow imports]")
                                  .arg(Constants::APP_NAME, Constants::APP_VERSION));
#endif

    // Size the window relative to the screen rather than using a fixed pixel size,
    // so it's usable on both small laptop displays and large monitors. The height-derived
    // width must still be clamped to the screen: on portrait or 5:4 displays height × 1.4
    // exceeds the available width and the window would open wider than the desktop.
    if (QScreen *screen = app.primaryScreen()) {
        const QRect screenGeometry = screen->availableGeometry();
        const int width = qMin(static_cast<int>(screenGeometry.height() * 1.4),
                               static_cast<int>(screenGeometry.width() * 0.95));
        mainWindow.resize(width, static_cast<int>(screenGeometry.height() * 0.80));
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
    // The Environment tab: live image-based-lighting controls for the viewport (see EnvironmentPanel).
    sidePanel->addTab(new pose::EnvironmentPanel(viewport), QStringLiteral("Environment"));

    menuManager->setAssetManagerWidget(assetsTab);
    menuManager->setViewportWidget(viewport);

    // Double-clicking an asset in the grid imports it into the viewport — the same entry points the
    // menu and command-line "open with" use: an .obj as a static model, a character figure (.duf/.dsf)
    // through the figure pipeline.
    QObject::connect(assetsTab, &AssetManagerWidget::importModelRequested,
                     viewport, &pose::ViewportWidget::importObj);
    QObject::connect(assetsTab, &AssetManagerWidget::importFigureRequested,
                     viewport, &pose::ViewportWidget::importFigure);

    mainSplitter->addWidget(viewport);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 500});

    mainWindow.setCentralWidget(mainSplitter);

    // --- 4. Execution ---
    mainWindow.show();

    // Boot branding screen; dismisses itself on the user's next click anywhere
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    // "Open with" / drag-onto-exe convenience: import any model/figure paths passed on the command
    // line. The importers queue them until the renderer exists, so this is safe pre-show.
    const QStringList launchArgs = QCoreApplication::arguments();
    for (int i = 1; i < launchArgs.size(); ++i) {
        const QString& arg = launchArgs.at(i);
        if (arg.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive)) {
            viewport->importObj(arg);
        } else if (arg.endsWith(QStringLiteral(".duf"), Qt::CaseInsensitive) ||
                   arg.endsWith(QStringLiteral(".dsf"), Qt::CaseInsensitive)) {
            viewport->importFigure(arg);
        } else if (arg.endsWith(QStringLiteral(".pose"), Qt::CaseInsensitive)) {
            // A pose file applies onto whichever figure was imported (queued until the figure exists).
            viewport->loadPose(arg);
        }
    }

    return app.exec();
}