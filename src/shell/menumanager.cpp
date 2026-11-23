// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file menumanager.cpp
 * @brief Builds the application's top menu bar (File / Edit / View / Render / Help).
 *
 * Live today: File → New / Open / Save / Save As (the .pss project: DocumentActions does the
 * work), Import (one entry per format), Save / Load Pose and Quit; Edit → Undo/Redo (the
 * viewport's unified edit stack), Delete Selected Object, Add Light, the pose utilities (reset
 * joint/limb/pose, mirror pose/limb), Turn Joint with Mouse and Preferences; the whole View menu
 * (Show Skeleton, the axis views, Flip, Frame Selected and Home View — all with app-wide
 * shortcuts in Blender's numpad convention — and VR Mode); Render → Render Image / Show Render
 * Window; Help → the User Manual (F1), What's New, the release notes, the website link, Check
 * for Updates and About. The remaining entries (Open Recent, Save Copy, Export, Undo History,
 * Copy / Paste, Tutorials, Support) are disabled placeholders that establish the menu structure
 * and icon conventions those features will slot into — enabling one means replacing its
 * disabled entry with a real handler here.
 */

#include "menumanager.h"
#include "documentactions.h"
#include "splashoverlay.h"
#include "preferencesdialog.h"
#include "assetmanagerwidget.h"
#include "preferencesmanager.h"
#include "importformats.h"
#include "constants.h"
#include "updatecheck.h"
#include "helpwindow.h"
#include "whatsnewdialog.h"
#include "licensenotice.h"   // Help → License: the program's legal notices (see the header)
#include "renderwindow.h"    // Render → Render Image: the Render window and its session
#include "viewport/viewportwidget.h"
#include "jointtransform.h" // pose::JointTransform: which of the joint-mode keys are live
#include "vrsettings.h"      // VR Mode: can it start, and why not
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QApplication>
#include <QAbstractButton>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>

/**
 * @brief Loads a normal/disabled icon pair following the "name.png" / "name-d.png" convention.
 */
static QIcon loadDualStateIcon(const QString& baseName) {
    QIcon icon;
    icon.addPixmap(QPixmap(QStringLiteral(":/resources/icons/%1.png").arg(baseName)), QIcon::Normal);
    icon.addPixmap(QPixmap(QStringLiteral(":/resources/icons/%1-d.png").arg(baseName)), QIcon::Disabled);
    return icon;
}

/**
 * @brief The folder a file dialog should start in: the one remembered under `prefKey` (the last
 * import / pose folder), falling back to Documents when nothing is saved yet or the stored folder
 * was moved/deleted. Shared by every File → Import and pose I/O entry point.
 */
static QString rememberedStartDir(const char* prefKey) {
    return PreferencesManager::instance().rememberedDirectoryOrDocuments(QLatin1String(prefKey));
}

MenuManager::MenuManager(QMainWindow *parent, DocumentActions *documents)
    : QObject(parent), mainWindow(parent), m_documents(documents) {}

void MenuManager::setupMenus() {
    if (!mainWindow) return;

    // =========================================================================
    // FILE MENU — document lifecycle, import/export, and application exit
    // =========================================================================
    QMenu *fileMenu = mainWindow->menuBar()->addMenu(tr("File"));

    // File → New: a fresh launch — the scene, its lighting and the Environment tab all go back to
    // the startup defaults (DocumentActions::newScene). Ctrl+N, window-level like Quit's Ctrl+Q.
    QAction *newSceneAction = fileMenu->addAction(loadDualStateIcon("new"), tr("New"));
    newSceneAction->setShortcut(QKeySequence::New);
    QObject::connect(newSceneAction, &QAction::triggered, mainWindow, [this]() { m_documents->newScene(); });
    // File → Open / Save / Save As: the .pss project document (the whole scene — figures with
    // their poses, transforms, environment and camera). Shortcuts follow the platform convention.
    QAction *openProjectAction = fileMenu->addAction(loadDualStateIcon("open"), tr("Open..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    QObject::connect(openProjectAction, &QAction::triggered, mainWindow,
                     [this]() { m_documents->openProjectWithDialog(); });
    fileMenu->addAction(tr("Open Recent..."))->setEnabled(false);
    fileMenu->addSeparator();

    QAction *saveProjectAction = fileMenu->addAction(loadDualStateIcon("save"), tr("Save"));
    saveProjectAction->setShortcut(QKeySequence::Save);
    QObject::connect(saveProjectAction, &QAction::triggered, mainWindow,
                     [this]() { m_documents->saveProject(); });
    QAction *saveProjectAsAction = fileMenu->addAction(tr("Save As..."));
    saveProjectAsAction->setShortcut(QKeySequence::SaveAs);
    QObject::connect(saveProjectAsAction, &QAction::triggered, mainWindow,
                     [this]() { m_documents->saveProjectAs(); });
    fileMenu->addAction(tr("Save Copy..."))->setEnabled(false);
    fileMenu->addSeparator();

    // Import submenu: one entry per supported file format, kept alphabetical — every one the same
    // function (importWithDialog) with its own dialog title, file filter and viewport entry
    // point. The filters' extensions come from core/importformats.h, the one place that says
    // which extensions a format has. (The Alembic, BVH, Collada, PLY and STL placeholders were
    // dropped on 2026-11-04 — none is a character format worth the menu room.)
    QMenu *importMenu = fileMenu->addMenu(loadDualStateIcon("import"), tr("Import"));
    namespace Formats = pose::ImportFormats;
    struct ImportFormat {
        const char *label;  // the menu entry (QT_TR_NOOP: translated where it is shown)
        QString     title;  // the file dialog's title
        QString     filter; // … and its filter
        ImportRoute route;  // where the chosen file goes
    };
    const ImportFormat importFormats[] = {
        // .DUF is the rigged character figure: its own native scene format + pipeline (geometry
        // + skeleton + morphs + materials), listed as a file format alongside the mesh formats.
        {QT_TR_NOOP(".DUF (DUF File)"), tr("Import Character Figure"),
         Formats::dialogFilter(tr("Figure Files"), Formats::nativeFigureExtensions()),
         &pose::ViewportWidget::importFigure},
        // A rigged character in an FBX file: imported through the figure route (VulkanWindow
        // dispatches by extension), conformed to the rig standard on the way in. A static FBX
        // mesh imports too, as a model without a skeleton.
        {QT_TR_NOOP(".FBX (FBX File)"), tr("Import FBX Character"),
         Formats::dialogFilter(tr("FBX Files"), Formats::fbxExtensions()),
         &pose::ViewportWidget::importFigure},
        // A rigged character in a glTF file — the binary .glb, or a .gltf with its buffers and
        // images beside it — through the same route as FBX.
        {QT_TR_NOOP(".GLB (GL Transmission Format .glTF)"), tr("Import glTF Character"),
         Formats::dialogFilter(tr("glTF Files"), Formats::gltfExtensions()),
         &pose::ViewportWidget::importFigure},
        {QT_TR_NOOP(".OBJ (Wavefront)"), tr("Import OBJ"),
         Formats::dialogFilter(tr("Wavefront OBJ"), Formats::modelExtensions()),
         &pose::ViewportWidget::importObj},
        // A Gaussian splat scene — the trained .ply, the compact .splat or the compressed .spz —
        // as a static, unlit model (SplatImportService; drawn by the scene's splat pass).
        {QT_TR_NOOP(".PLY / .SPLAT / .SPZ (Gaussian Splat)"), tr("Import Gaussian Splat"),
         Formats::dialogFilter(tr("Gaussian Splat Files"), Formats::splatExtensions()),
         &pose::ViewportWidget::importSplat},
        // A rigged character in a USD file — ASCII, crate, or a USDZ package — through the
        // figure route as well.
        {QT_TR_NOOP(".USD (Universal Scene Description)"), tr("Import USD Character"),
         Formats::dialogFilter(tr("USD Files"), Formats::usdExtensions()),
         &pose::ViewportWidget::importFigure},
        // A VRM avatar: a GLB with a humanoid bone map, read by the glTF reader through the same
        // route.
        {QT_TR_NOOP(".VRM (VRM Avatar)"), tr("Import VRM Avatar"),
         Formats::dialogFilter(tr("VRM Avatars"), Formats::vrmExtensions()),
         &pose::ViewportWidget::importFigure},
    };
    for (const ImportFormat &format : importFormats) {
        QAction *action = importMenu->addAction(tr(format.label));
        QObject::connect(action, &QAction::triggered, mainWindow,
                         [this, title = format.title, filter = format.filter, route = format.route]() {
                             importWithDialog(title, filter, route);
                         });
    }

    fileMenu->addAction(loadDualStateIcon("export"), tr("Export..."))->setEnabled(false);
    fileMenu->addSeparator();

    // Pose I/O: save the current figure's joint rotations to a small text file and restore them
    // later. Enabled regardless of scene state; the handlers warn if no figure is loaded.
    QAction *savePoseAction = fileMenu->addAction(tr("Save Pose..."));
    QObject::connect(savePoseAction, &QAction::triggered, mainWindow, [this]() { savePoseFile(); });
    QAction *loadPoseAction = fileMenu->addAction(tr("Load Pose..."));
    QObject::connect(loadPoseAction, &QAction::triggered, mainWindow, [this]() { loadPoseFile(); });
    fileMenu->addSeparator();

    QAction *quitAction = fileMenu->addAction(tr("Quit"));
    // The platform's own Quit bindings, with Ctrl+Q in front where it is not among them: Windows
    // has none, while the X11 desktops and macOS (where Qt's Ctrl is Cmd) bind that very key —
    // and one sequence listed twice on an action is two entries in the shortcut map, which Qt
    // reports as ambiguous instead of triggering.
    QList<QKeySequence> quitKeys = QKeySequence::keyBindings(QKeySequence::Quit);
    if (!quitKeys.contains(QKeySequence(Qt::CTRL | Qt::Key_Q))) {
        quitKeys.prepend(QKeySequence(Qt::CTRL | Qt::Key_Q));
    }
    quitAction->setShortcuts(quitKeys);
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);

    // =========================================================================
    // EDIT MENU — undo/redo history, clipboard, and preferences
    // =========================================================================
    QMenu *editMenu = mainWindow->menuBar()->addMenu(tr("Edit"));

    // Undo/Redo drive the viewport's shared edit stack (pose changes + Environment-panel lighting
    // gestures). The QAction shortcuts are what make Ctrl+Z/Ctrl+Y work app-wide — the panel's
    // controls take keyboard focus, so the viewport's own keyPressEvent alone wouldn't see the
    // keys after a dial edit. Text fields still keep their own Ctrl+Z: a focused QLineEdit accepts
    // the ShortcutOverride for its editing keys, which parks these window-level shortcuts.
    QAction *undoAction = editMenu->addAction(loadDualStateIcon("undo"), tr("Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    QObject::connect(undoAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->undo();
    });
    QAction *redoAction = editMenu->addAction(loadDualStateIcon("redo"), tr("Redo"));
    // EVERY redo key of the platform, not its first alone: setShortcut(QKeySequence::Redo) binds
    // the primary one (Ctrl+Y on Windows), and Ctrl+Shift+Z — the second standard redo key, and
    // the 3D tools' usual one — then redid only while the viewport had the keyboard, through its
    // own key handler; pressed after a dial edit in a panel it did nothing.
    redoAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::Redo));
    QObject::connect(redoAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->redo();
    });
    editMenu->addAction(tr("Undo History..."))->setEnabled(false);
    editMenu->addSeparator();

    editMenu->addAction(loadDualStateIcon("copy"), tr("Copy"))->setEnabled(false);
    editMenu->addAction(tr("Paste"))->setEnabled(false);
    // Delete removes the SELECTED (outlined) viewport object. The "Del" shown here is a hint,
    // not a bound QKeySequence: the key itself is handled by the viewport while it has focus
    // (VulkanWindow::keyPressEvent). A window-level shortcut would hijack Delete from the Asset
    // Manager's tree and grid, where the key must stay theirs.
    QAction *deleteAction = editMenu->addAction(tr("Delete Selected Object\tDel"));
    QObject::connect(deleteAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->deleteSelectedObject();
    });
    // The LIGHT OBJECTS (the Lights tab lists and edits them): a new light stands two metres
    // from the figure, above it and aimed at it, and is selected.
    QMenu *addLightMenu = editMenu->addMenu(tr("Add Light"));
    const struct {
        pose::LightKind kind;
        QString         label;
    } kLightKinds[] = {
        {pose::LightKind::Point, tr("Point Light")},
        {pose::LightKind::Spot, tr("Spot Light")},
        {pose::LightKind::Area, tr("Area Light")},
        {pose::LightKind::Sun, tr("Sun")},
    };
    for (const auto& entry : kLightKinds) {
        QAction *action = addLightMenu->addAction(entry.label);
        const pose::LightKind kind = entry.kind;
        QObject::connect(action, &QAction::triggered, mainWindow, [this, kind]() {
            if (viewportWidget) viewportWidget->addLight(kind);
        });
    }
    editMenu->addSeparator();

    // Pose utilities, on the ACTIVE figure (the one whose joint was last clicked). "Limb" = the
    // selected joint and everything below it. Each is one undo step; pins are left alone. The
    // joint context menu in the viewport offers the per-joint ones too.
    QAction *resetJointAction = editMenu->addAction(tr("Reset Selected Joint"));
    QObject::connect(resetJointAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->resetSelectedJoint();
    });
    QAction *resetLimbAction = editMenu->addAction(tr("Reset Limb"));
    QObject::connect(resetLimbAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->resetSelectedLimb();
    });
    QAction *resetPoseAction = editMenu->addAction(tr("Reset Pose"));
    QObject::connect(resetPoseAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->resetPose();
    });
    editMenu->addSeparator();
    // THE JOINT MOUSE MODE's keys (VulkanWindow::toggleJointModal): B / S / T turn the selected
    // joint's Bend / Side-Side / Twist with the mouse. They are bound here as window shortcuts so
    // that they work WHICHEVER PANEL HAS THE FOCUS — bound to the viewport alone, the key did
    // nothing after a click on the Transform tab until the viewport was clicked again. A text
    // field keeps its letters (a focused QLineEdit accepts the ShortcutOverride for text keys),
    // and each action is enabled only while the selected joint has that motion
    // (setViewportWidget), so with nothing to turn the letters go where they always went — the
    // asset lists' type-ahead. No auto-repeat: a key held a moment too long must not toggle the
    // mode off again.
    QMenu *turnJointMenu = editMenu->addMenu(tr("Turn Joint with Mouse"));
    static const struct { QString label; Qt::Key key; } kTurnJoint[3] = {
        {tr("Bend"), Qt::Key_B}, {tr("Side-Side"), Qt::Key_S}, {tr("Twist"), Qt::Key_T}};
    for (int kind = 0; kind < 3; ++kind) {
        QAction *action = turnJointMenu->addAction(kTurnJoint[kind].label);
        action->setShortcut(QKeySequence(kTurnJoint[kind].key));
        action->setAutoRepeat(false);
        action->setEnabled(false); // until a joint is selected
        QObject::connect(action, &QAction::triggered, mainWindow, [this, kind]() {
            if (viewportWidget) viewportWidget->toggleJointModal(kind);
        });
        m_turnJointActions[kind] = action;
    }
    editMenu->addSeparator();
    QAction *mirrorPoseAction = editMenu->addAction(tr("Mirror Pose"));
    QObject::connect(mirrorPoseAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->mirrorPose();
    });
    QAction *mirrorLimbAction = editMenu->addAction(tr("Mirror Limb to Other Side"));
    QObject::connect(mirrorLimbAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->mirrorSelectedLimb();
    });
    editMenu->addSeparator();

    QAction *preferencesAction = editMenu->addAction(loadDualStateIcon("preferences"), tr("Preferences"));
    QObject::connect(preferencesAction, &QAction::triggered, mainWindow, [this]() {
        openPreferencesDialog();
    });

    // =========================================================================
    // VIEW MENU — viewport display options (skeleton overlay)
    // =========================================================================
    QMenu *viewMenu = mainWindow->menuBar()->addMenu(tr("View"));

    // "Show Skeleton": toggle the skeleton overlay (the joint→parent bone lines drawn over the
    // figure). Off by default — joints are grabbed directly on the figure — but joints stay
    // clickable either way. Kept in sync with the overlay's Skeleton button via the
    // skeletonVisibilityChanged signal. The viewport is registered after setupMenus() (see
    // setViewportWidget), so the action's connections are attached there.
    m_showSkeletonAction = viewMenu->addAction(tr("Show Skeleton"));
    m_showSkeletonAction->setCheckable(true);
    viewMenu->addSeparator();

    // Camera views in Blender's numpad convention, bound APP-WIDE (window shortcuts, so they
    // work whichever panel has focus — text fields still get their digits and periods: a
    // focused QLineEdit/spin box accepts the ShortcutOverride for text keys, which parks these).
    // Each has two bindings: the number-row key, which the shortcut map also matches for the
    // numpad digit with NumLock ON (it retries a keypad key without its keypad modifier), and
    // the navigation key the numpad sends with NumLock OFF (End/PageDown/Home/PageUp/Delete),
    // bound WITH the keypad modifier so the real End/Home/Delete keys stay untouched.
    struct AxisViewEntry {
        QString        label;
        pose::AxisView view;
        Qt::Key        rowKey;   // number row / numpad with NumLock on
        Qt::Key        padKey;   // what the same numpad key sends with NumLock off
        bool           ctrl;
    };
    const AxisViewEntry axisViews[] = {
        {tr("Front View"),  pose::AxisView::Front,  Qt::Key_1, Qt::Key_End,      false},
        {tr("Back View"),   pose::AxisView::Back,   Qt::Key_1, Qt::Key_End,      true},
        {tr("Right View"),  pose::AxisView::Right,  Qt::Key_3, Qt::Key_PageDown, false},
        {tr("Left View"),   pose::AxisView::Left,   Qt::Key_3, Qt::Key_PageDown, true},
        {tr("Top View"),    pose::AxisView::Top,    Qt::Key_7, Qt::Key_Home,     false},
        {tr("Bottom View"), pose::AxisView::Bottom, Qt::Key_7, Qt::Key_Home,     true},
    };
    for (const AxisViewEntry& entry : axisViews) {
        QAction *action = viewMenu->addAction(entry.label);
        const Qt::KeyboardModifiers mods = entry.ctrl ? Qt::ControlModifier : Qt::NoModifier;
        action->setShortcuts({QKeySequence(mods | entry.rowKey),
                              QKeySequence(mods | Qt::KeypadModifier | entry.padKey)});
        const pose::AxisView view = entry.view;
        QObject::connect(action, &QAction::triggered, mainWindow, [this, view]() {
            if (viewportWidget) viewportWidget->setAxisView(view);
        });
    }
    viewMenu->addSeparator();
    QAction *flipViewAction = viewMenu->addAction(tr("Flip View"));
    flipViewAction->setShortcuts({QKeySequence(Qt::Key_9),
                                  QKeySequence(Qt::KeypadModifier | Qt::Key_PageUp)});
    QObject::connect(flipViewAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->flipView();
    });
    QAction *frameSelectedAction = viewMenu->addAction(tr("Frame Selected"));
    frameSelectedAction->setShortcuts({QKeySequence(Qt::Key_Period),
                                       QKeySequence(Qt::KeypadModifier | Qt::Key_Delete),
                                       QKeySequence(Qt::KeypadModifier | Qt::Key_Comma)});
    QObject::connect(frameSelectedAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->frameSelected();
    });
    // Home View = the viewport's Home button: the default perspective three-quarter framing.
    // On 5 — the centre of the numpad's view cluster (numpad 5 sends Clear with NumLock off).
    QAction *homeViewAction = viewMenu->addAction(tr("Home View"));
    homeViewAction->setShortcuts({QKeySequence(Qt::Key_5),
                                  QKeySequence(Qt::KeypadModifier | Qt::Key_Clear)});
    QObject::connect(homeViewAction, &QAction::triggered, mainWindow, [this]() {
        if (viewportWidget) viewportWidget->resetView();
    });

    // VR Mode: the scene in the headset (OpenXR — Quest Link / Air Link, SteamVR, Virtual
    // Desktop), the window mirroring its left eye. Checkable; kept in sync with the viewport's
    // vrModeChanged (the runtime can end a session on its own). Recenter puts the user back in
    // front of the figure. Both wired in setViewportWidget, like Show Skeleton.
    viewMenu->addSeparator();
    m_vrModeAction = viewMenu->addAction(tr("VR Mode"));
    m_vrModeAction->setCheckable(true);
    m_recenterVrAction = viewMenu->addAction(tr("Recenter VR View"));
    m_recenterVrAction->setEnabled(false);

    // =========================================================================
    // RENDER MENU — the production render (src/render/: the Render window and the Cycles engine)
    // Render Image opens the Render window and starts a render of the scene as the viewport
    // shows it; F12 is app-wide, like the View keys (a render is asked for from wherever the
    // user is). Show Render Window brings the window back without starting anything.
    QMenu *renderMenu = mainWindow->menuBar()->addMenu(tr("Render"));
    QAction *renderImageAction = renderMenu->addAction(tr("Render Image"));
    renderImageAction->setShortcut(QKeySequence(Qt::Key_F12));
    QObject::connect(renderImageAction, &QAction::triggered, mainWindow, [this]() { renderImage(/*start=*/true); });
    QAction *renderWindowAction = renderMenu->addAction(tr("Show Render Window"));
    QObject::connect(renderWindowAction, &QAction::triggered, mainWindow, [this]() { renderImage(/*start=*/false); });

    // =========================================================================
    // HELP MENU — documentation, support links, and the About dialog
    QMenu *helpMenu = mainWindow->menuBar()->addMenu(tr("Help"));

    // The User Manual (src/help/, content in docs/manual/): F1 opens it app-wide — a window-level
    // shortcut like the View keys, so it works whichever panel has focus. Release Notes opens the
    // manual at its "What's New" page, which is the CHANGELOG embedded at build time.
    QAction *manualAction = helpMenu->addAction(loadDualStateIcon("tutorials"), tr("User Manual"));
    manualAction->setShortcut(QKeySequence::HelpContents);
    QObject::connect(manualAction, &QAction::triggered, mainWindow, [this]() {
        HelpWindow::open(mainWindow);
    });
    // What's New…: the running version's release notes in their own window — the one that opens
    // by itself on the first start of a new version (whatsnewdialog.h); here, on demand.
    QAction *whatsNewAction = helpMenu->addAction(tr("What's New..."));
    QObject::connect(whatsNewAction, &QAction::triggered, mainWindow, [this]() {
        WhatsNewDialog::showNow(mainWindow);
    });
    QAction *releaseNotesAction = helpMenu->addAction(tr("Release Notes"));
    QObject::connect(releaseNotesAction, &QAction::triggered, mainWindow, [this]() {
        HelpWindow::open(mainWindow, QStringLiteral("whats-new"));
    });
    helpMenu->addAction(tr("Tutorials"))->setEnabled(false);
    helpMenu->addAction(tr("Support"))->setEnabled(false);
    helpMenu->addSeparator();

    QAction *websiteAction = helpMenu->addAction(QIcon(":/resources/icons/globe.png"), "PoseStudio.org");
    QObject::connect(websiteAction, &QAction::triggered, mainWindow, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://posestudio.org")));
    });
    // The on-demand twin of the startup update check (see updatecheck.h): reports every outcome.
    QAction *updateAction = helpMenu->addAction(tr("Check for Updates..."));
    QObject::connect(updateAction, &QAction::triggered, mainWindow, [this]() {
        UpdateCheck::checkNow(mainWindow);
    });
    helpMenu->addSeparator();

    // "About" reuses the boot splash overlay for branding/version info
    QAction *aboutAction = helpMenu->addAction(loadDualStateIcon("about"), tr("About PoseStudio"));
    QObject::connect(aboutAction, &QAction::triggered, mainWindow, [this]() {
        SplashOverlay *splash = new SplashOverlay(mainWindow);
        splash->show();
    });

    // License: PoseStudio's own legal notices — the copyright, the AGPL v3 with its additional
    // terms, no warranty, the trademarks, the commercial option (licensenotice.h says why the
    // wording is part of the license's working).
    QAction *licenseAction = helpMenu->addAction(tr("License"));
    QObject::connect(licenseAction, &QAction::triggered, mainWindow, [this]() {
        LicenseNotice::show(mainWindow);
    });

    // Qt is used under the LGPL v3, which asks that a program showing copyright notices at
    // runtime include Qt's, with a pointer to the license texts. QApplication::aboutQt() is
    // Qt's own such notice; the texts themselves ship in licenses\ next to the executable
    // (see packaging/licenses/NOTICES.txt).
    QAction *aboutQtAction = helpMenu->addAction(tr("About Qt"));
    QObject::connect(aboutQtAction, &QAction::triggered, mainWindow, []() {
        QApplication::aboutQt();
    });
}

void MenuManager::setAssetManagerWidget(AssetManagerWidget *widget) {
    assetManagerWidget = widget;
    if (!assetManagerWidget) return;

    QObject::connect(assetManagerWidget, &AssetManagerWidget::manageAssetFoldersRequested,
                      mainWindow, [this]() { openPreferencesDialog(tr("Assets")); });
}

void MenuManager::addLibraryBrowser(AssetManagerWidget *widget) {
    if (widget) m_libraryBrowsers.append(widget);
}

void MenuManager::setViewportWidget(pose::ViewportWidget *viewport) {
    viewportWidget = viewport;
    if (!viewport || !m_showSkeletonAction) {
        return;
    }
    // Edit → Turn Joint with Mouse: each key is live only while the selected joint has that
    // motion (an elbow has no Side-Side; nothing selected has none). jointTransformChanged says
    // when to look again: a selection, a deselection, a deleted figure.
    const auto syncTurnJoint = [this]() {
        const pose::JointTransform transform = viewportWidget ? viewportWidget->jointTransform() : pose::JointTransform{};
        for (int kind = 0; kind < 3; ++kind) {
            if (m_turnJointActions[kind]) {
                m_turnJointActions[kind]->setEnabled(transform.valid && transform.dials[kind].enabled);
            }
        }
    };
    QObject::connect(viewport, &pose::ViewportWidget::jointTransformChanged, this, syncTurnJoint);
    syncTurnJoint();
    // View → Show Skeleton drives the same overlay state as the viewport's Skeleton button; the
    // skeletonVisibilityChanged signal keeps the two in lockstep (blockSignals keeps the round-trip
    // to one hop).
    m_showSkeletonAction->setChecked(viewport->showSkeleton());
    QObject::connect(m_showSkeletonAction, &QAction::toggled, this, [this](bool on) {
        if (viewportWidget) viewportWidget->setShowSkeleton(on);
    });
    QObject::connect(viewport, &pose::ViewportWidget::skeletonVisibilityChanged, this, [this](bool visible) {
        if (!m_showSkeletonAction || m_showSkeletonAction->isChecked() == visible) {
            return;
        }
        m_showSkeletonAction->blockSignals(true);
        m_showSkeletonAction->setChecked(visible);
        m_showSkeletonAction->blockSignals(false);
    });

    // View → VR Mode: a start the viewport cannot do explains itself (explainVrUnavailable)
    // instead of failing silently; the viewport's vrModeChanged keeps the check mark honest
    // (the headset runtime can end a session by itself), and Recenter is live only in VR.
    if (m_vrModeAction) {
        QObject::connect(m_vrModeAction, &QAction::triggered, this, [this](bool on) {
            if (!viewportWidget) {
                return;
            }
            if (!on) {
                viewportWidget->setVrMode(false);
                return;
            }
            if (!VrSettings::ready() || !viewportWidget->setVrMode(true)) {
                m_vrModeAction->blockSignals(true);
                m_vrModeAction->setChecked(false);
                m_vrModeAction->blockSignals(false);
                if (!VrSettings::ready()) {
                    explainVrUnavailable();
                }
            }
        });
        QObject::connect(viewport, &pose::ViewportWidget::vrModeChanged, this, [this](bool on) {
            if (m_vrModeAction && m_vrModeAction->isChecked() != on) {
                m_vrModeAction->blockSignals(true);
                m_vrModeAction->setChecked(on);
                m_vrModeAction->blockSignals(false);
            }
            if (m_recenterVrAction) {
                m_recenterVrAction->setEnabled(on);
            }
        });
        QObject::connect(viewport, &pose::ViewportWidget::vrError, this, [this](const QString &message) {
            QMessageBox::warning(mainWindow, tr("VR Mode"), message);
        });
        // The strip's VR button and its view picker's VR Mode entry take the menu entry's path
        // (trigger: the toggle + the handler above, explanation included), so every way in is one.
        QObject::connect(viewport, &pose::ViewportWidget::vrModeRequested, this, [this](bool on) {
            if (m_vrModeAction && m_vrModeAction->isChecked() != on) {
                m_vrModeAction->trigger();
            }
        });
    }
    if (m_recenterVrAction) {
        QObject::connect(m_recenterVrAction, &QAction::triggered, this, [this]() {
            if (viewportWidget) viewportWidget->recenterVr();
        });
    }
    // The viewport strip's Selection button, and the scripted test's `prefs <tab>`: the Preferences
    // dialog on that tab. QUEUED — the dialog's exec() must run from the event loop, not inside the
    // button's click or the script's own step, which goes on stepping (its timers fire in the
    // dialog's loop) and closes the dialog through its button.
    QObject::connect(viewport, &pose::ViewportWidget::preferencesRequested, this,
                     [this](const QString &tab) { openPreferencesDialog(tab); }, Qt::QueuedConnection);
    // The scripted test's `renderwindow [start]`: the Render menu's two entries.
    QObject::connect(viewport, &pose::ViewportWidget::renderWindowRequested, this,
                     [this](bool start) { renderImage(start); }, Qt::QueuedConnection);
}

void MenuManager::explainVrUnavailable() {
    QMessageBox box(mainWindow);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("VR Mode"));
    if (!VrSettings::isEnabled()) {
        // Opt-in: the headset runtime is consulted as PoseStudio starts (before the Vulkan device
        // exists), so turning VR on is a restart — offered right here.
        box.setText(tr("VR support is off."));
        box.setInformativeText(
            tr("PoseStudio connects to the headset runtime when it starts, so VR support takes a restart to "
               "turn on. Connect your headset first (Quest Link or Air Link, Virtual Desktop, or SteamVR), "
               "then restart — your scene comes back with it.\n\nTurn VR support on now?"));
        QAbstractButton *restart = box.addButton(tr("Turn On and Restart"), QMessageBox::AcceptRole);
        QAbstractButton *later = box.addButton(tr("Turn On, Restart Later"), QMessageBox::ActionRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == restart || box.clickedButton() == later) {
            VrSettings::setEnabled(true);
        }
        if (box.clickedButton() == restart) {
            m_documents->relaunchApplication();
        }
        return;
    }
    // VR support is on, but the startup found no usable headset: the status says why.
    box.setText(tr("VR Mode cannot start."));
    box.setInformativeText(tr("%1\n\nConnect the headset (Quest Link or Air Link, Virtual Desktop, or SteamVR), "
                              "then restart PoseStudio — your scene comes back with it.")
                               .arg(VrSettings::status()));
    QAbstractButton *restart = box.addButton(tr("Restart Now"), QMessageBox::AcceptRole);
    box.addButton(tr("OK"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == restart) {
        m_documents->relaunchApplication();
    }
}

void MenuManager::renderImage(bool start) {
    // A build without the engine says so here, where a render is asked for — and nowhere else:
    // the application is whole without one.
    if (!pose::RenderSession::engineInstalled()) {
        QMessageBox::information(
            mainWindow, tr("Render"),
            tr("This copy of PoseStudio has no render engine, so there is nothing to render with.\n\n"
               "The Cycles engine is a separate part of the application — the engines folder beside it. A copy built "
               "from the source code has it when it was built with POSESTUDIO_WITH_CYCLES turned on."));
        return;
    }
    pose::RenderWindow *window = pose::RenderWindow::open(mainWindow);
    window->setSceneSource([this]() -> std::shared_ptr<pose::RenderSceneHolder> {
        return viewportWidget ? viewportWidget->captureRenderScene() : nullptr;
    });
    if (start) {
        window->startRender();
    }
}

void MenuManager::importWithDialog(const QString &title, const QString &filter, ImportRoute route) {
    if (!viewportWidget) return;
    // (No unsaved-changes prompt: an import ADDS to the scene, it replaces nothing.)

    const QString path = QFileDialog::getOpenFileName(
        mainWindow, title, rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR), filter);
    if (path.isEmpty()) return; // user cancelled

    // Remember the folder this file came from for the next import.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    (viewportWidget->*route)(path);
}

void MenuManager::savePoseFile() {
    if (!viewportWidget) return;
    if (!viewportWidget->hasPosableFigure()) {
        QMessageBox::information(mainWindow, tr("Save Pose"),
                                tr("Import a character figure before saving a pose."));
        return;
    }

    QString path = QFileDialog::getSaveFileName(mainWindow, tr("Save Pose"),
                                                rememberedStartDir(Constants::PREF_LAST_POSE_DIR),
                                                tr("Pose Files (*.pose)"));
    if (path.isEmpty()) return; // user cancelled
    if (!path.endsWith(QStringLiteral(".pose"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".pose");
    }
    // Remember the pose folder for the next save/load, like Import remembers its folder.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_POSE_DIR,
                                            QFileInfo(path).absolutePath());
    if (!viewportWidget->savePose(path)) {
        QMessageBox::warning(mainWindow, tr("Save Pose"),
                             tr("Could not write the pose file."));
    }
}

void MenuManager::loadPoseFile() {
    if (!viewportWidget) return;
    if (!viewportWidget->hasPosableFigure()) {
        QMessageBox::information(mainWindow, tr("Load Pose"),
                                tr("Import a character figure before loading a pose."));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(mainWindow, tr("Load Pose"),
                                                      rememberedStartDir(Constants::PREF_LAST_POSE_DIR),
                                                      tr("Pose Files (*.pose)"));
    if (path.isEmpty()) return; // user cancelled
    PreferencesManager::instance().setValue(Constants::PREF_LAST_POSE_DIR,
                                            QFileInfo(path).absolutePath());
    if (!viewportWidget->loadPose(path)) {
        QMessageBox::warning(mainWindow, tr("Load Pose"),
                             tr("Could not read the pose file."));
    }
}

void MenuManager::openPreferencesDialog(const QString &initialTab) {
    PreferencesDialog dialog(mainWindow);
    if (!initialTab.isEmpty()) dialog.selectTab(initialTab);

    if (assetManagerWidget) {
        QObject::connect(&dialog, &PreferencesDialog::assetLibrariesChanged,
                          assetManagerWidget, &AssetManagerWidget::refreshAssetManager);
        QObject::connect(&dialog, &PreferencesDialog::navigateToLibraryRequested,
                          assetManagerWidget, &AssetManagerWidget::navigateToLibraryRoot);
    }
    for (const QPointer<AssetManagerWidget>& browser : std::as_const(m_libraryBrowsers)) {
        if (!browser) continue;
        QObject::connect(&dialog, &PreferencesDialog::assetLibrariesChanged,
                          browser.data(), &AssetManagerWidget::refreshAssetManager);
    }
    if (viewportWidget) {
        // The Selection page: the viewport redraws with a color or an opacity as it changes,
        // behind the open dialog.
        QObject::connect(&dialog, &PreferencesDialog::selectionOutlineChanged,
                          viewportWidget, &pose::ViewportWidget::setSelectionOutline);
        QObject::connect(&dialog, &PreferencesDialog::jointHighlightChanged,
                          viewportWidget, &pose::ViewportWidget::setJointHighlight);
    }
    // The Factory Reset page asks; the document's keeper does it (prompt, wipe, relaunch).
    QObject::connect(&dialog, &PreferencesDialog::factoryResetRequested,
                      m_documents, &DocumentActions::factoryReset);
    dialog.exec();
}
