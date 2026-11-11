/**
 * @file main.cpp
 * @brief Application entry point: boots core services, then builds the main window.
 *
 * Pure wiring — the theming lives in shell/apptheme.*, the database bootstrap in core/database.*.
 * Startup order matters here — the database and preferences cache must be ready before any UI
 * that might read from them gets constructed, and the theme must be installed before the first
 * widget polishes.
 */

#include "importformats.h"
#include "database.h"
#include "menumanager.h"
#include "materialpanel.h"
#include "softbodypanel.h"
#include "splashoverlay.h"
#include "whatsnewdialog.h"
#include "helpwindow.h"
#include "constants.h"
#include "preferencesmanager.h"
#include "selectionsettings.h"
#include "installping.h"
#include "updatecheck.h"
#include "assetmanagerwidget.h"
#include "assetscriptlink.h"
#include "apptheme.h"
#include "environmentpanel.h"
#include "transformpanel.h"
#include "viewport/viewportwidget.h"

#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <QScreen>
#include <QSplitter>
#include <QTimer>
#include <QTabWidget>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Vetoed window close while the scene has unsaved changes.
 *
 * The prompt and the Save path live in MenuManager (which owns the dirty flag's UI); a
 * declined close is vetoed, an accepted one proceeds.
 */
class CloseGuard : public QObject {
public:
    explicit CloseGuard(MenuManager* menus) : m_menus(menus) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Close && m_menus && !m_menus->confirmDiscardChanges()) {
            event->ignore(); // the user cancelled: stay open
            return true;
        }
        return false;
    }

private:
    MenuManager* m_menus;
};

/**
 * @brief The main window's title for a given document state.
 *
 * A saved/opened project is named by FILE NAME only — its folder would be truncated in the
 * taskbar and adds nothing (the path stays visible in the Save/Open dialogs); unsaved changes
 * earn an asterisk prefix; a new/empty project shows the plain application title. Unoptimized
 * builds keep their marker as the tail of the string: a Debug build is ~5x slower at figure
 * import (MSVC debug-STL overhead), which has been mistaken for a real performance regression
 * when a Debug window was benchmarked against other applications' shipped binaries.
 */
static QString windowTitleFor(const QString& path, bool dirty) {
    QString title = QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION);
#ifndef NDEBUG
    title += QObject::tr(" [Debug build — slow imports]");
#endif
    if (path.isEmpty()) return title;
    const QString name = QFileInfo(path).fileName();
    return (dirty ? QStringLiteral("*") : QString()) + name + QStringLiteral(" — ") + title;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // App identity. Must be set before any QStandardPaths lookup: the per-user data dir the
    // database now lives in (AppDataLocation) is derived from these names, so initializeDatabase
    // below depends on it. Keep this first.
    QApplication::setApplicationName(QStringLiteral("PoseStudio"));
    QApplication::setApplicationDisplayName(QStringLiteral("PoseStudio"));

    // --- 1. Core services & data layer ---
    // Fail here, with a clear error, rather than confusingly the first time some unrelated widget
    // tries to run a query later.
    if (!initializeDatabase(DbInitMode::Normal)) {
        qCritical() << "Fatal Error: PoseStudio cannot launch without a valid database connection.";
        return -1;
    }

    PreferencesManager::instance().loadFromDatabase();
    pose::SoftBodyPanel::applyStoredSettings(); // (the soft body's switches and dials, before any figure is imported)

    // --- 2. Branding & theming (window icon, Fusion proxy style, the QSS modules, menu shadows) ---
    AppTheme::install(app);

    // --- 3. Main window layout ---
    QMainWindow mainWindow;
    // The title tracks the current document (file name, a * for unsaved changes); at startup
    // there is no document yet, so it starts as the plain application title.
    mainWindow.setWindowTitle(windowTitleFor(QString(), /*dirty=*/false));

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

    // Unsaved-changes gate on window close (Save / Don't save / Cancel).
    mainWindow.installEventFilter(new CloseGuard(menuManager));

    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, &mainWindow);

    // The 3D viewport: a self-contained Vulkan-backed widget. Everything graphics-API
    // related lives behind this facade (see src/viewport/). If Vulkan is unavailable it
    // degrades to an inline message rather than failing to launch.
    pose::ViewportWidget *viewport = new pose::ViewportWidget(mainSplitter);
    // Preferences → Selection: the stored color and opacity of the selection outline and of the
    // joint highlight. (An isolated scripted run starts from the defaults, in the viewport and on
    // the page alike — its pictures are compared with references; nothing is written: see
    // PreferencesManager::isolatedRun.)
    if (PreferencesManager::isolatedRun()) {
        SelectionSettings::restoreDefaults();
    }
    viewport->setSelectionOutline(SelectionSettings::outlineColor(), SelectionSettings::outlineDrawnOpacity());
    viewport->setJointHighlight(SelectionSettings::jointColor(), SelectionSettings::jointDrawnOpacity());

    // Document state changes (save/open/new, dirty transitions) re-title the window — the
    // facade's signal is the only wiring point, per its contract.
    QObject::connect(viewport, &pose::ViewportWidget::documentChanged, &mainWindow,
                     [&mainWindow, viewport]() {
                         mainWindow.setWindowTitle(windowTitleFor(viewport->projectPath(),
                                                                  viewport->isProjectDirty()));
                     });

    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West);

    AssetManagerWidget *assetsTab = new AssetManagerWidget();
    sidePanel->addTab(assetsTab, QObject::tr("Asset Manager"));
    // The Transform tab: the selected joint's rotation as three dials (see TransformPanel).
    sidePanel->addTab(new pose::TransformPanel(viewport), QObject::tr("Transform"));
    // The Materials tab: the selected object's material zones as sliders (see MaterialPanel).
    sidePanel->addTab(new pose::MaterialPanel(viewport), QObject::tr("Materials"));
    // The Soft Body Physics tab: the breasts' and the buttocks' switches and dials (see SoftBodyPanel).
    sidePanel->addTab(new pose::SoftBodyPanel(viewport), QObject::tr("Soft Body Physics"));
    // The Environment tab: live image-based-lighting controls for the viewport (see EnvironmentPanel).
    pose::EnvironmentPanel *environmentTab = new pose::EnvironmentPanel(viewport);
    sidePanel->addTab(environmentTab, QObject::tr("Environment"));
    // Developer lever: POSESTUDIO_TAB=<title> starts on that side tab (the scripted test's
    // `uishot` then pictures it — tools/ikscripts/README.md).
    if (const QString startTab = qEnvironmentVariable("POSESTUDIO_TAB"); !startTab.isEmpty()) {
        for (int i = 0; i < sidePanel->count(); ++i) {
            if (sidePanel->tabText(i).compare(startTab, Qt::CaseInsensitive) == 0) {
                sidePanel->setCurrentIndex(i);
            }
        }
    }

    menuManager->setAssetManagerWidget(assetsTab);
    menuManager->setViewportWidget(viewport);
    menuManager->setEnvironmentPanel(environmentTab);

    // Double-clicking an asset in the grid imports it into the viewport — the same entry points the
    // menu and command-line "open with" use: an .obj as a static model, a character figure (.duf/.dsf)
    // through the figure pipeline.
    QObject::connect(assetsTab, &AssetManagerWidget::importModelRequested,
                     viewport, &pose::ViewportWidget::importObj);
    QObject::connect(assetsTab, &AssetManagerWidget::importFigureRequested,
                     viewport, &pose::ViewportWidget::importFigure);
    QObject::connect(assetsTab, &AssetManagerWidget::importSplatRequested,
                     viewport, &pose::ViewportWidget::importSplat);

    mainSplitter->addWidget(viewport);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 500});

    mainWindow.setCentralWidget(mainSplitter);

    // --- 4. Execution ---
    mainWindow.show();

    // Boot branding screen; dismisses itself on the user's next click anywhere. A SCRIPTED RUN
    // (POSESTUDIO_IK_SCRIPT) has none: nobody is there to dismiss it, and the press that
    // dismisses it is CONSUMED — the first mouse press a script sent to a widget (`ui scrub`, an
    // `app click`) went to the splash and did nothing.
    if (!qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT")) {
        SplashOverlay *splash = new SplashOverlay(&mainWindow);
        splash->show();
        // The What's New window (see whatsnewdialog.h): on the first start of a new version, its
        // release notes — raised once the splash is dismissed, so they are the first thing seen of
        // the version and never sit under the branding. POSESTUDIO_WHATS_NEW=1 raises it at once.
        QObject::connect(splash, &SplashOverlay::dismissed, &mainWindow, [&mainWindow]() {
            WhatsNewDialog::showIfUpdated(&mainWindow);
        });
    }
    if (qEnvironmentVariableIsSet("POSESTUDIO_WHATS_NEW")) {
        QTimer::singleShot(800, &mainWindow, [&mainWindow]() {
            WhatsNewDialog::showIfUpdated(&mainWindow);
            // POSESTUDIO_WHATS_NEW_SHOT=<png>: save a picture of the window and exit (the
            // manual shot's twin: how the window is LOOKED at without a desktop).
            if (const QByteArray shotPath = qgetenv("POSESTUDIO_WHATS_NEW_SHOT"); !shotPath.isEmpty()) {
                QTimer::singleShot(600, &mainWindow, [shotPath]() {
                    for (QWidget* top : QApplication::topLevelWidgets()) {
                        if (auto* dialog = qobject_cast<WhatsNewDialog*>(top)) {
                            dialog->grab().save(QString::fromUtf8(shotPath));
                        }
                    }
                    QApplication::quit();
                });
            }
        });
    }

    // "Open with" / drag-onto-exe convenience: import any model/figure paths passed on the command
    // line. Two paths, depending on the platform: the viewport builds its renderer on the first
    // expose, and on Windows that expose is delivered synchronously inside mainWindow.show()
    // above, so here the renderer already exists and these imports run the ordinary interactive
    // path (progress dialog and all). Where the expose arrives later (other platforms), the
    // importers queue the paths and drain them, in this order, once the renderer is up — so the
    // calls are safe either way.
    const QStringList launchArgs = QCoreApplication::arguments();
    QString projectToOpen;   // a .pss on the command line ("open with" a project file)
    QString restoreScene;    // --restore-scene <tmp.pss>: the SCENE-PRESERVING RESTART (below)
    QString restoreProject;  // --project <original.pss>: the document that scene belonged to
    bool    restoreClean = false; // --project-clean: ... and it was saved, not unsaved work
    for (int i = 1; i < launchArgs.size(); ++i) {
        const QString& arg = launchArgs.at(i);
        if (arg == QStringLiteral("--restore-scene") && i + 1 < launchArgs.size()) {
            restoreScene = launchArgs.at(++i);
        } else if (arg == QStringLiteral("--project") && i + 1 < launchArgs.size()) {
            restoreProject = launchArgs.at(++i);
        } else if (arg == QStringLiteral("--project-clean")) {
            restoreClean = true;
        } else if (pose::ImportFormats::isModelFile(arg)) {
            viewport->importObj(arg);
        } else if (pose::ImportFormats::isSplatFile(arg)) {
            viewport->importSplat(arg);
        } else if (pose::ImportFormats::isFigureRouteFile(arg)) {
            viewport->importFigure(arg); // (an FBX / glTF / USD character takes the figure route too)
        } else if (arg.endsWith(QStringLiteral(".pose"), Qt::CaseInsensitive)) {
            // A pose file applies onto whichever figure was imported (queued until the figure exists).
            viewport->loadPose(arg);
        } else if (arg.endsWith(QStringLiteral(".pss"), Qt::CaseInsensitive)) {
            projectToOpen = arg;
        } else if (AssetScriptLink::isScriptFile(arg)) {
            // A content-library script ("open with" a .dsa): followed as the Asset Manager's
            // double-click follows it — the folder it links to shown, its one character imported.
            assetsTab->openAsset(arg);
        }
    }
    // A project on the command line, or the scene of the process that restarted into this one
    // (MenuManager::relaunchApplication — the VR Mode "Turn On and Restart": it saved its scene
    // to a temporary project, which is reopened here, deleted, and re-attached to the document
    // it came from, unsaved as it was, so nothing is lost across the restart). The load needs
    // the renderer; on Windows the first expose built it inside show() above, elsewhere the
    // open is retried until it exists.
    const QString projectPath = restoreScene.isEmpty() ? projectToOpen : restoreScene;
    if (!projectPath.isEmpty()) {
        auto openProject = std::make_shared<std::function<void(int)>>();
        *openProject = [=](int attempt) {
            std::vector<std::string> missing;
            std::string              error;
            const int status = viewport->loadProjectFile(projectPath, {}, missing, error);
            if (status == -1 && error == "no renderer yet" && attempt < 40) {
                QTimer::singleShot(250, viewport, [=]() { (*openProject)(attempt + 1); });
                return;
            }
            if (status != 0) {
                qWarning() << "[project] could not open" << projectPath << ":" << QString::fromStdString(error)
                           << (missing.empty() ? "" : "(missing figure sources)");
            }
            if (!restoreScene.isEmpty()) {
                QFile::remove(restoreScene); // the temporary project has served
                viewport->setProjectPath(restoreProject);
                if (status == 0 && !restoreClean) {
                    viewport->markProjectDirty(); // it was unsaved work; it still is
                }
            }
        };
        (*openProject)(0);
    }

    // The anonymous per-launch install ping (see installping.h): armed now, it fires a few
    // seconds into the session so it never competes with startup, and is a no-op in builds
    // without a signing key or when the user has switched it off. Owned by the main window, NOT
    // the application object — see scheduleAtStartup for why that distinction matters at exit.
    InstallPing::scheduleAtStartup(&mainWindow);
    // The update check (see updatecheck.h): a few seconds in, asks GitHub for the latest release
    // and prompts — with a link to its page — only when it is newer than this build and the user
    // has not skipped it. Same ownership rule as the ping.
    UpdateCheck::scheduleAtStartup(&mainWindow);

    // Developer lever: POSESTUDIO_MANUAL_SHOT=<png> opens the User Manual (at the page named by
    // POSESTUDIO_MANUAL_PAGE, else its first page), saves a picture of the window and exits — the
    // way to LOOK at a manual change without a desktop (the docs/manual/README.md workflow).
    if (const QByteArray shotPath = qgetenv("POSESTUDIO_MANUAL_SHOT"); !shotPath.isEmpty()) {
        const QString page = QString::fromUtf8(qgetenv("POSESTUDIO_MANUAL_PAGE"));
        QTimer::singleShot(1200, &mainWindow, [&mainWindow, page, shotPath]() {
            HelpWindow::open(&mainWindow, page);
            QTimer::singleShot(600, &mainWindow, [shotPath]() {
                for (QWidget* top : QApplication::topLevelWidgets()) {
                    if (auto* help = qobject_cast<HelpWindow*>(top)) {
                        // (POSESTUDIO_MANUAL_SEARCH=<words> exercises the search too)
                        const QString search = QString::fromUtf8(qgetenv("POSESTUDIO_MANUAL_SEARCH"));
                        if (!search.isEmpty()) {
                            help->setSearchText(search);
                        }
                        help->grab().save(QString::fromUtf8(shotPath));
                    }
                }
                QApplication::quit();
            });
        });
    }

    return app.exec();
}
