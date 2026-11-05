/**
 * @file menumanager.cpp
 * @brief Builds the application's top menu bar (File / Edit / View / Help).
 *
 * Live today: File → New (a fresh launch), Import (.OBJ meshes and .DUF character figures),
 * Edit → Undo/Redo (the viewport's unified pose + lighting stack), Delete Selected Object, the
 * pose utilities (reset joint/limb/pose, mirror pose/limb) and Preferences; the whole View menu
 * (Show Skeleton, the axis views, Flip, Frame Selected and Home View — all with app-wide
 * shortcuts in Blender's numpad convention); Help → the User Manual (F1), the release notes, the
 * website link and About. The remaining
 * entries (Open/Save, the other import formats, Export, clipboard, docs) are disabled
 * placeholders that establish the menu structure and icon conventions those features will slot
 * into — enabling one means replacing its disabled entry with a real handler here.
 */

#include "menumanager.h"
#include "splashoverlay.h"
#include "preferencesdialog.h"
#include "assetmanagerwidget.h"
#include "environmentpanel.h"
#include "preferencesmanager.h"
#include "constants.h"
#include "updatecheck.h"
#include "helpwindow.h"
#include "whatsnewdialog.h"
#include "viewport/viewportwidget.h"
#include "jointtransform.h" // pose::JointTransform: which of the joint-mode keys are live
#include "scene/shademode.h" // kDefaultShadeMode (File → New's picker reset)
#include "vrsettings.h"      // VR Mode: can it start, and why not
#include <QDir>
#include <QMenu>
#include <QProcess>
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
#include <QStandardPaths>
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
    return PreferencesManager::instance().rememberedDirectory(
        QLatin1String(prefKey), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

MenuManager::MenuManager(QMainWindow *parent) : QObject(parent), mainWindow(parent) {}

void MenuManager::setupMenus() {
    if (!mainWindow) return;

    // =========================================================================
    // FILE MENU — document lifecycle, import/export, and application exit
    // =========================================================================
    QMenu *fileMenu = mainWindow->menuBar()->addMenu(tr("File"));

    // File → New: a fresh launch — the scene, its lighting and the Environment tab all go back to
    // the startup defaults (see newScene). Ctrl+N, window-level like Quit's Ctrl+Q.
    QAction *newSceneAction = fileMenu->addAction(loadDualStateIcon("new"), tr("New"));
    newSceneAction->setShortcut(QKeySequence::New);
    QObject::connect(newSceneAction, &QAction::triggered, mainWindow, [this]() { newScene(); });
    // File → Open / Save / Save As: the .pss project document (the whole scene — figures with
    // their poses, transforms, environment and camera). Shortcuts follow the platform convention.
    QAction *openProjectAction = fileMenu->addAction(loadDualStateIcon("open"), tr("Open..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    QObject::connect(openProjectAction, &QAction::triggered, mainWindow,
                     [this]() { openProjectFile(); });
    fileMenu->addAction(tr("Open Recent..."))->setEnabled(false);
    fileMenu->addSeparator();

    QAction *saveProjectAction = fileMenu->addAction(loadDualStateIcon("save"), tr("Save"));
    saveProjectAction->setShortcut(QKeySequence::Save);
    QObject::connect(saveProjectAction, &QAction::triggered, mainWindow,
                     [this]() { saveProject(); });
    QAction *saveProjectAsAction = fileMenu->addAction(tr("Save As..."));
    saveProjectAsAction->setShortcut(QKeySequence::SaveAs);
    QObject::connect(saveProjectAsAction, &QAction::triggered, mainWindow,
                     [this]() { saveProjectAs(); });
    fileMenu->addAction(tr("Save Copy..."))->setEnabled(false);
    fileMenu->addSeparator();

    // Import submenu: one entry per supported file format, kept alphabetical. Every entry is live
    // (a handler of nullptr would be a disabled placeholder, the form the formats took before
    // their importers landed; the Alembic, BVH, Collada, PLY and STL placeholders were dropped on
    // 2026-11-04 — none is a character format worth the menu room). .DUF is the rigged character
    // figure: its own native scene format + pipeline (geometry + skeleton + morphs + materials),
    // listed as a file format alongside the mesh formats.
    QMenu *importMenu = fileMenu->addMenu(loadDualStateIcon("import"), tr("Import"));
    struct ImportFormat {
        const char *label;
        void (MenuManager::*handler)(); // nullptr = importer not built yet
    };
    const ImportFormat importFormats[] = {
        {".DUF (DUF File)",                   &MenuManager::importFigureFile},
        {".FBX (FBX File)",                   &MenuManager::importFbxFile},
        {".GLB (GL Transmission Format .glTF)", &MenuManager::importGltfFile},
        {".OBJ (Wavefront)",                  &MenuManager::importObjFile},
        {".PLY / .SPLAT / .SPZ (Gaussian Splat)",    &MenuManager::importSplatFile},
        {".USD (Universal Scene Description)", &MenuManager::importUsdFile},
        {".VRM (VRM Avatar)",                 &MenuManager::importVrmFile},
    };
    for (const ImportFormat &format : importFormats) {
        QAction *action = importMenu->addAction(format.label);
        if (format.handler) {
            QObject::connect(action, &QAction::triggered, mainWindow,
                             [this, handler = format.handler]() { (this->*handler)(); });
        } else {
            action->setEnabled(false); // importer not built yet
        }
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
    // Ctrl+Q covers Windows/Linux explicitly; QKeySequence::Quit adds the platform-standard
    // binding too (e.g. Cmd+Q on macOS), so both are listed rather than picking just one.
    quitAction->setShortcuts({QKeySequence("Ctrl+Q"), QKeySequence::Quit});
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
    redoAction->setShortcut(QKeySequence::Redo);
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
            relaunchApplication();
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
        relaunchApplication();
    }
}

void MenuManager::relaunchApplication() {
    // THE SCENE SURVIVES THE RESTART: it is saved to a temporary project the new process reopens
    // (main.cpp's --restore-scene), deletes, and re-attaches to the document it belonged to —
    // unsaved as it was — so a restart costs the user nothing and asks nothing. Only when that
    // temporary file cannot be written does the unsaved-changes prompt stand in.
    QStringList args;
    bool preserved = false;
    if (viewportWidget) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString temp = dir + QStringLiteral("/restart-scene.pss");
        if (QDir().mkpath(dir) && viewportWidget->saveProjectFile(temp)) {
            preserved = true;
            args << QStringLiteral("--restore-scene") << temp;
            if (!viewportWidget->projectPath().isEmpty()) {
                args << QStringLiteral("--project") << viewportWidget->projectPath();
            }
            if (!viewportWidget->isProjectDirty()) {
                args << QStringLiteral("--project-clean");
            }
        }
    }
    if (!preserved && !confirmDiscardChanges()) {
        return; // Save cancelled, or the user changed their mind
    }
    if (viewportWidget) {
        viewportWidget->setProjectClean(); // preserved, saved or let go of: no second prompt on the way out
    }
    // The Factory Reset relaunch (factoryresetpreferencespanel.cpp), with only the restore
    // arguments: the new process must not replay this one's "open-with" paths.
    QProcess::startDetached(QApplication::applicationFilePath(), args);
    QApplication::quit();
}

void MenuManager::newScene() {
    if (!viewportWidget) return;
    if (!confirmDiscardChanges()) {
        return; // the user cancelled: the dirty scene stays as it was
    }
    // 1. The two picker mirrors first — the panel's restore gesture below does not touch them, and
    //    these go through the sync signals (pss-shade-mode-sync), so the shader picker and the
    //    View → Show Skeleton check mark land on their defaults too.
    viewportWidget->setShowSkeleton(false);
    viewportWidget->setShadeMode(pose::kDefaultShadeMode);
    // 2. The Environment tab: dials / backdrop mode back to the startup values, and the HDRI
    //    caption to the stock panorama (its environmentChosen re-bakes it; resetToEmptyScene's own
    //    bake supersedes it if it lands later — the request id discards the stale one).
    if (environmentPanel) {
        environmentPanel->resetToStartupDefaults();
    }
    // 3. The scene itself: models, camera home, deferred state to startup defaults, history cleared.
    viewportWidget->resetToEmptyScene();
}

void MenuManager::setEnvironmentPanel(pose::EnvironmentPanel *panel) {
    environmentPanel = panel;
}

void MenuManager::importObjFile() {
    if (!viewportWidget) return;
    // (No unsaved-changes prompt: an import ADDS to the scene, it replaces nothing.)

    const QString path = QFileDialog::getOpenFileName(
        mainWindow, tr("Import OBJ"), rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR),
        tr("Wavefront OBJ (*.obj)"));
    if (path.isEmpty()) return; // user cancelled

    // Remember the folder this model came from for the next import.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importObj(path);
}

void MenuManager::importFigureFile() {
    if (!viewportWidget) return;
    // (No unsaved-changes prompt: an import ADDS to the scene, it replaces nothing.)

    const QString path = QFileDialog::getOpenFileName(
        mainWindow, tr("Import Character Figure"),
        rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR),
        tr("Figure Files (*.duf *.dsf)"));
    if (path.isEmpty()) return; // user cancelled

    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importFigure(path);
}

void MenuManager::importFbxFile() {
    // A rigged character in an FBX file: imported through the figure route (VulkanWindow dispatches
    // by extension), conformed to the rig standard on the way in. A static FBX mesh imports too, as
    // a model without a skeleton.
    importRiggedFile(tr("Import FBX Character"), tr("FBX Files (*.fbx)"));
}

void MenuManager::importGltfFile() {
    // A rigged character in a glTF file — the binary .glb, or a .gltf with its buffers and
    // images beside it — through the same route as FBX.
    importRiggedFile(tr("Import glTF Character"), tr("glTF Files (*.glb *.gltf)"));
}

void MenuManager::importUsdFile() {
    // A rigged character in a USD file — ASCII, crate, or a USDZ package — through the same route.
    importRiggedFile(tr("Import USD Character"), tr("USD Files (*.usd *.usda *.usdc *.usdz)"));
}

void MenuManager::importVrmFile() {
    // A VRM avatar: a GLB with a humanoid bone map, read by the glTF reader through the same route.
    importRiggedFile(tr("Import VRM Avatar"), tr("VRM Avatars (*.vrm)"));
}

void MenuManager::importSplatFile() {
    // A Gaussian splat scene — the trained .ply or the compact .splat — as a static, unlit model
    // (SplatImportService; drawn by the scene's splat pass).
    if (!viewportWidget) return;
    const QString path = QFileDialog::getOpenFileName(
        mainWindow, tr("Import Gaussian Splat"), rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR),
        tr("Gaussian Splat Files (*.ply *.splat *.spz)"));
    if (path.isEmpty()) return; // user cancelled

    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importSplat(path);
}

void MenuManager::importRiggedFile(const QString &title, const QString &filter) {
    if (!viewportWidget) return;
    const QString path = QFileDialog::getOpenFileName(
        mainWindow, title, rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR), filter);
    if (path.isEmpty()) return; // user cancelled

    PreferencesManager::instance().setValue(Constants::PREF_LAST_IMPORT_DIR,
                                            QFileInfo(path).absolutePath());
    viewportWidget->importFigure(path);
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

void MenuManager::saveProjectAs() {
    if (!viewportWidget) return;

    QString path = QFileDialog::getSaveFileName(mainWindow, tr("Save Project"),
                                                rememberedStartDir(Constants::PREF_LAST_PROJECT_DIR),
                                                tr("PoseStudio Projects (*.pss)"));
    if (path.isEmpty()) return; // user cancelled
    if (!path.endsWith(QStringLiteral(".pss"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".pss");
    }
    // Remember the project folder for the next save/open, like Import remembers its folder.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());
    if (!viewportWidget->saveProjectFile(path)) {
        QMessageBox::warning(mainWindow, tr("Save Project"),
                             tr("Could not write the project file."));
        return;
    }
    viewportWidget->setProjectPath(path); // Save As adopts the document path: plain Save now writes here
    viewportWidget->setProjectClean(); // a successful save is what clears the dirty flag
}

void MenuManager::saveProject() {
    if (!viewportWidget) return;
    const QString path = viewportWidget->projectPath();
    if (path.isEmpty()) {
        saveProjectAs(); // no document path yet: Save falls back to Save As
        return;
    }
    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());
    if (!viewportWidget->saveProjectFile(path)) {
        QMessageBox::warning(mainWindow, tr("Save Project"),
                             tr("Could not write the project file."));
        return;
    }
    viewportWidget->setProjectClean();
}

bool MenuManager::confirmDiscardChanges() {
    if (!viewportWidget || !viewportWidget->isProjectDirty()) {
        return true; // nothing to lose
    }
    // Explicit button labels: the platform theme's stock text for Discard is a long sentence
    // that the message box truncates ("lose without..."). Custom buttons keep the wording ours.
    QMessageBox box(mainWindow);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Unsaved Changes"));
    box.setText(tr("The current scene has unsaved changes."));
    QAbstractButton *save = box.addButton(tr("Save"), QMessageBox::AcceptRole);
    QAbstractButton *discard = box.addButton(tr("Discard Changes"),
                                            QMessageBox::DestructiveRole);
    // The box sizes itself to the text line, which is narrower than the button row needs;
    // guarantee the full label stays visible.
    discard->setMinimumWidth(150);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == save) {
        saveProject();                           // its own dialog (Save As for a new document)
        return !viewportWidget->isProjectDirty(); // a cancelled Save As or a failed write: nothing
    }                                            // was saved, so the scene is NOT let go of
    return box.clickedButton() == discard;
}

void MenuManager::openProjectFile() {
    if (!viewportWidget) return;

    const QString path = QFileDialog::getOpenFileName(mainWindow, tr("Open Project"),
                                                      rememberedStartDir(Constants::PREF_LAST_PROJECT_DIR),
                                                      tr("PoseStudio Projects (*.pss)"));
    if (path.isEmpty()) return; // user cancelled
    if (!confirmDiscardChanges()) {
        return;
    }

    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());

    // First pass: parse + link pre-flight. A missing source stops here — the scene is untouched.
    std::vector<std::string> missing;
    std::string error;
    const int status = viewportWidget->loadProjectFile(path, {}, missing, error);
    if (status == 0) {
        return; // loaded clean
    }
    if (status != 1) {
        QMessageBox::warning(mainWindow, tr("Open Project"),
                             tr("Could not read the project file:\n%1")
                                             .arg(QString::fromStdString(error)));
        return;
    }

    // LINK RECOVERY — one consolidated dialog: a row per missing source, each with its own
    // re-point picker. A declined row is skipped with a warning naming the file.
    std::map<std::string, std::string> recovered;
    QStringList skipped;
    for (const std::string& src : missing) {
        const QString original = QString::fromStdString(src);
        QMessageBox box(mainWindow);
        box.setWindowTitle(tr("Missing Figure File"));
        box.setText(tr("The figure file referenced by this project is missing:"));
        box.setInformativeText(original);
        QAbstractButton *pickButton = box.addButton(tr("Choose File..."),
                                                 QMessageBox::ActionRole);
        QAbstractButton *skipButton = box.addButton(tr("Skip"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == pickButton) {
            const QString replacement = QFileDialog::getOpenFileName(
                mainWindow, tr("Locate Figure File"),
                rememberedStartDir(Constants::PREF_LAST_IMPORT_DIR),
                tr("Figure Files (*.duf *.dsf *.obj)"));
            if (replacement.isEmpty()) {
                skipped << original;
                recovered[src] = std::string(); // "" = leave this figure out of the load
                continue;
            }
            recovered[src] = replacement.toStdString();
        } else if (box.clickedButton() == skipButton) {
            skipped << original; // the row is skipped, the rest of the project still loads
            recovered[src] = std::string(); // (the load's pre-flight no longer reports it missing)
        } else if (box.clickedButton() &&
                   box.buttonRole(box.clickedButton()) == QMessageBox::RejectRole) {
            return; // Cancel: the whole open is aborted, the scene stays as it was
        }
    }
    if (!skipped.isEmpty()) {
        QMessageBox::warning(mainWindow, tr("Open Project"),
                             tr("Skipped missing figure files:\n%1")
                                             .arg(skipped.join(QLatin1Char('\n'))));
    }

    // Second pass: the recovered paths are applied, everything else is imported as saved.
    std::vector<std::string> stillMissing;
    if (viewportWidget->loadProjectFile(path, recovered, stillMissing, error) != 0) {
        QMessageBox::warning(mainWindow, tr("Open Project"),
                             tr("Could not load the project:\n%1")
                                             .arg(QString::fromStdString(error)));
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
    if (viewportWidget) {
        QObject::connect(&dialog, &PreferencesDialog::softBodySettingsChanged,
                          viewportWidget, &pose::ViewportWidget::applySoftBodySettings);
    }
    dialog.exec();
}
