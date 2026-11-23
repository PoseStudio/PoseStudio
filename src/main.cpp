// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file main.cpp
 * @brief Application entry point: boots core services, then builds the main window.
 *
 * Construction and wiring: the theming lives in shell/apptheme.*, the database bootstrap in
 * core/database.*, the document's life (New / Open / Save, the close gate, the window title, the
 * scene kept across a restart) in shell/documentactions.*. What is decided here is the startup
 * ORDER — the database and preferences cache must be ready before any UI that might read from
 * them gets constructed, and the theme must be installed before the first widget polishes — and
 * what the files named on the command line do.
 */

#include "importformats.h"
#include "database.h"
#include "documentactions.h"
#include "menumanager.h"
#include "materialpanel.h"
#include "softbodypanel.h"
#include "lightspanel.h"
#include "splashoverlay.h"
#include "whatsnewdialog.h"
#include "helpwindow.h"
#include "licensenotice.h"
#include "constants.h"
#include "preferencesmanager.h"
#include "runmode.h"
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
#include <clocale>
#include <QDebug>
#include <QMainWindow>
#include <QScreen>
#include <QSplitter>
#include <QTimer>
#include <QTabWidget>
#include <QString>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Numeric parsing and formatting must be locale-independent: Qt installs the desktop's
    // locale (LC_NUMERIC included), and a comma-decimal one makes atof() stop at the decimal
    // dot — cgltf then reads a glTF matrix entry like -4.37e-08 as -4, and a figure imports
    // stretched. The UI language stays Qt-side (QLocale + the translators); the C numeric
    // locale is the app's own business.
    std::setlocale(LC_NUMERIC, "C");
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
    // The document's keeper (see documentactions.h): it titles the window — the plain
    // application title until there is a document — and gates its close on unsaved changes.
    DocumentActions *documents = new DocumentActions(&mainWindow);

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

    MenuManager *menuManager = new MenuManager(&mainWindow, documents);
    menuManager->setupMenus();

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

    documents->setViewportWidget(viewport); // (the window title follows the document from here on)

    QTabWidget *sidePanel = new QTabWidget(mainSplitter);
    sidePanel->setTabPosition(QTabWidget::West);

    AssetManagerWidget *assetsTab = new AssetManagerWidget();
    sidePanel->addTab(assetsTab, QObject::tr("Asset Manager"));
    // The Transform tab: the selected joint's rotation as three dials (see TransformPanel).
    sidePanel->addTab(new pose::TransformPanel(viewport), QObject::tr("Transform"));
    // The Materials tab: the selected object's material zones as sliders (see MaterialPanel).
    pose::MaterialPanel *materialsTab = new pose::MaterialPanel(viewport);
    sidePanel->addTab(materialsTab, QObject::tr("Materials"));
    // The Materials Library tab: the Asset Manager's own widget, built a second time over the one
    // folder "My PoseStudio Library/Materials" (see assetmanagerscope.h). Same class, same code:
    // what the manager learns, both tabs learn.
    AssetManagerWidget *materialsLibraryTab =
        new AssetManagerWidget(AssetManagerScope::materialsLibrary());
    sidePanel->addTab(materialsLibraryTab, QObject::tr("Materials Library"));
    // The Soft Body Physics tab: the breasts' and the buttocks' switches and dials (see SoftBodyPanel).
    sidePanel->addTab(new pose::SoftBodyPanel(viewport), QObject::tr("Soft Body Physics"));
    // The Environment tab: live image-based-lighting controls for the viewport (see EnvironmentPanel).
    pose::EnvironmentPanel *environmentTab = new pose::EnvironmentPanel(viewport);
    sidePanel->addTab(environmentTab, QObject::tr("Environment"));
    // The Lights tab: the scene's light objects — point, spot, area and sun lights (see LightsPanel).
    sidePanel->addTab(new pose::LightsPanel(viewport), QObject::tr("Lights"));
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
    menuManager->addLibraryBrowser(materialsLibraryTab);
    menuManager->setViewportWidget(viewport);
    documents->setEnvironmentPanel(environmentTab);

    // Double-clicking an asset in the grid imports it into the viewport — the same entry points the
    // menu and command-line "open with" use: an .obj as a static model, a character figure (.duf/.dsf)
    // through the figure pipeline.
    // Every library tab opens what it holds the same way.
    for (AssetManagerWidget *browser : {assetsTab, materialsLibraryTab}) {
        QObject::connect(browser, &AssetManagerWidget::importModelRequested,
                         viewport, &pose::ViewportWidget::importObj);
        QObject::connect(browser, &AssetManagerWidget::importFigureRequested,
                         viewport, &pose::ViewportWidget::importFigure);
        QObject::connect(browser, &AssetManagerWidget::importSplatRequested,
                         viewport, &pose::ViewportWidget::importSplat);
        // … and a saved material (.psm) is not opened but APPLIED: put on the zone the Materials
        // tab shows.
        QObject::connect(browser, &AssetManagerWidget::applyMaterialRequested,
                         materialsTab, &pose::MaterialPanel::applyMaterialFile);
    }
    // The Materials folder lies inside a library the Asset Manager browses too: files one browser
    // renamed, moved, deleted or installed are read again by the other.
    QObject::connect(assetsTab, &AssetManagerWidget::assetFilesChanged, materialsLibraryTab, &AssetManagerWidget::refreshAssetManager);
    QObject::connect(materialsLibraryTab, &AssetManagerWidget::assetFilesChanged, assetsTab, &AssetManagerWidget::refreshAssetManager);
    // (The scripted in-app test's `asset` command: the Materials Library's file operations.)
    QObject::connect(viewport, &pose::ViewportWidget::libraryOperationRequested, materialsLibraryTab, &AssetManagerWidget::runFileOperation);
    // A material saved from the Materials tab appears in the Materials Library at once.
    QObject::connect(materialsTab, &pose::MaterialPanel::materialSaved,
                     materialsLibraryTab, [materialsLibraryTab](const QString&) {
                         materialsLibraryTab->refreshAssetManager();
                     });

    mainSplitter->addWidget(viewport);
    mainSplitter->addWidget(sidePanel);
    mainSplitter->setSizes({1500, 500});

    mainWindow.setCentralWidget(mainSplitter);

    // --- 4. Execution ---
    // A SCRIPTED RUN (POSESTUDIO_IK_SCRIPT) opens BEHIND every other window and takes no focus:
    // it is started on a desktop someone is working at, and a window that jumps in front of their
    // work gets minimized — and a minimized window draws no frame, so the run's shots never come
    // (its frames are read back from the GPU: an occluded window serves as well as a visible
    // one). POSESTUDIO_IK_SCRIPT_FRONT=1 opens it as any other run, to watch a script go.
    if (RunMode::scripted() && !qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT_FRONT")) {
        mainWindow.setAttribute(Qt::WA_ShowWithoutActivating);
        mainWindow.setWindowFlag(Qt::WindowStaysOnBottomHint);
    }
    mainWindow.show();

    // Boot branding screen; dismisses itself on the user's next click anywhere. A SCRIPTED RUN
    // (POSESTUDIO_IK_SCRIPT) has none: nobody is there to dismiss it, and the press that
    // dismisses it is CONSUMED — the first mouse press a script sent to a widget (`ui scrub`, an
    // `app click`) went to the splash and did nothing.
    if (!RunMode::scripted()) {
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
    // calls are safe either way. (A project file, and the arguments of a restart that kept its
    // scene, are the document's: DocumentActions takes those and opens them after the imports.)
    const QStringList openWith = documents->takeLaunchArguments(QCoreApplication::arguments());
    for (const QString& arg : openWith) {
        if (pose::ImportFormats::isModelFile(arg)) {
            viewport->importObj(arg);
        } else if (pose::ImportFormats::isSplatFile(arg)) {
            viewport->importSplat(arg);
        } else if (pose::ImportFormats::isFigureRouteFile(arg)) {
            viewport->importFigure(arg); // (an FBX / glTF / USD character takes the figure route too)
        } else if (arg.endsWith(QStringLiteral(".pose"), Qt::CaseInsensitive)) {
            // A pose file applies onto whichever figure was imported (queued until the figure exists).
            viewport->loadPose(arg);
        } else if (AssetScriptLink::isScriptFile(arg)) {
            // A content-library script ("open with" a .dsa): followed as the Asset Manager's
            // double-click follows it — the folder it links to shown, its one character imported.
            assetsTab->openAsset(arg);
        }
    }
    documents->openAtStartup();

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

    // Developer lever: POSESTUDIO_LICENSE_SHOT=<png> opens Help → License, saves a picture of the
    // box and exits — how the notice is LOOKED at without a desktop.
    if (const QByteArray shotPath = qgetenv("POSESTUDIO_LICENSE_SHOT"); !shotPath.isEmpty()) {
        QTimer::singleShot(1200, &mainWindow, [&mainWindow, shotPath]() {
            LicenseNotice::show(&mainWindow);
            QTimer::singleShot(600, &mainWindow, [&mainWindow, shotPath]() {
                if (auto* box = mainWindow.findChild<QWidget*>(QStringLiteral("LicenseNotice"))) {
                    box->grab().save(QString::fromUtf8(shotPath));
                }
                QApplication::quit();
            });
        });
    }

    return app.exec();
}
