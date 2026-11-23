// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file documentactions.cpp
 * @brief Implementation of DocumentActions: File → New / Open / Save / Save As, the
 *        unsaved-changes gate, the window title, and the restart that keeps the scene. See
 *        documentactions.h.
 */

#include "documentactions.h"

#include "constants.h"
#include "database.h"
#include "environmentpanel.h"
#include "importformats.h"
#include "preferencesmanager.h"
#include "runmode.h"
#include "scene/shademode.h" // kDefaultShadeMode (File → New's picker reset)
#include "viewport/viewportwidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton> // (QMessageBox::addButton returns one: its base class must be known)
#include <QStandardPaths>
#include <QTimer>

#include <map>
#include <string>
#include <vector>

namespace {

// The arguments relaunchApplication() hands the process it starts, read back by
// takeLaunchArguments() in that process: the temporary project holding the scene, the document
// that scene belonged to, and whether it had no unsaved changes.
const QString kRestoreSceneFlag = QStringLiteral("--restore-scene");
const QString kRestoreProjectFlag = QStringLiteral("--project");
const QString kRestoreCleanFlag = QStringLiteral("--project-clean");

// openAtStartup()'s wait for the viewport's renderer (ViewportWidget::awaitingRenderer): how
// often it asks, and how many times.
constexpr int kStartupOpenRetryMs = 250;
constexpr int kStartupOpenAttempts = 40;

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
QString windowTitleFor(const QString& path, bool dirty) {
    QString title = QStringLiteral("%1 %2").arg(Constants::APP_NAME, Constants::APP_VERSION);
#ifndef NDEBUG
    title += QObject::tr(" [Debug build — slow imports]");
#endif
    if (path.isEmpty()) return title;
    const QString name = QFileInfo(path).fileName();
    return (dirty ? QStringLiteral("*") : QString()) + name + QStringLiteral(" — ") + title;
}

} // namespace

DocumentActions::DocumentActions(QMainWindow *window) : QObject(window), m_window(window) {
    // The title tracks the current document (file name, a * for unsaved changes); at startup
    // there is no document yet, so it starts as the plain application title.
    m_window->setWindowTitle(windowTitleFor(QString(), /*dirty=*/false));
    // Unsaved-changes gate on window close (Save / Discard Changes / Cancel).
    m_window->installEventFilter(this);
}

void DocumentActions::setViewportWidget(pose::ViewportWidget *viewport) {
    m_viewport = viewport;
    if (!m_viewport) return;
    // Document state changes (save/open/new, dirty transitions) re-title the window — the
    // facade's signal is the only wiring point, per its contract.
    connect(m_viewport, &pose::ViewportWidget::documentChanged, this, &DocumentActions::updateWindowTitle);
}

void DocumentActions::setEnvironmentPanel(pose::EnvironmentPanel *panel) {
    m_environmentPanel = panel;
}

void DocumentActions::updateWindowTitle() {
    if (!m_viewport) return;
    m_window->setWindowTitle(windowTitleFor(m_viewport->projectPath(), m_viewport->isProjectDirty()));
}

bool DocumentActions::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::Close && !confirmDiscardChanges()) {
        event->ignore(); // the user cancelled: stay open
        return true;
    }
    return false;
}

void DocumentActions::newScene() {
    if (!m_viewport) return;
    if (!confirmDiscardChanges()) {
        return; // the user cancelled: the dirty scene stays as it was
    }
    // 1. The two picker mirrors first — the panel's restore gesture below does not touch them, and
    //    these go through the sync signals (pss-shade-mode-sync), so the shader picker and the
    //    View → Show Skeleton check mark land on their defaults too.
    m_viewport->setShowSkeleton(false);
    m_viewport->setShadeMode(pose::kDefaultShadeMode);
    // 2. The Environment tab: dials / backdrop mode back to the startup values, and the HDRI
    //    caption to the stock panorama (its environmentChosen re-bakes it; resetToEmptyScene's own
    //    bake supersedes it if it lands later — the request id discards the stale one).
    if (m_environmentPanel) {
        m_environmentPanel->resetToStartupDefaults();
    }
    // 3. The scene itself: models, camera home, deferred state to startup defaults, history cleared.
    m_viewport->resetToEmptyScene();
}

void DocumentActions::saveProjectAs() {
    if (!m_viewport) return;

    QString path = QFileDialog::getSaveFileName(
        m_window, tr("Save Project"),
        PreferencesManager::instance().rememberedDirectoryOrDocuments(Constants::PREF_LAST_PROJECT_DIR),
        tr("PoseStudio Projects (*.pss)"));
    if (path.isEmpty()) return; // user cancelled
    if (!path.endsWith(QStringLiteral(".pss"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".pss");
    }
    // Remember the project folder for the next save/open, like Import remembers its folder.
    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());
    if (!m_viewport->saveProjectFile(path)) {
        QMessageBox::warning(m_window, tr("Save Project"),
                             tr("Could not write the project file."));
        return;
    }
    m_viewport->setProjectPath(path); // Save As adopts the document path: plain Save now writes here
    m_viewport->setProjectClean(); // a successful save is what clears the dirty flag
}

void DocumentActions::saveProject() {
    if (!m_viewport) return;
    const QString path = m_viewport->projectPath();
    if (path.isEmpty()) {
        saveProjectAs(); // no document path yet: Save falls back to Save As
        return;
    }
    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());
    if (!m_viewport->saveProjectFile(path)) {
        QMessageBox::warning(m_window, tr("Save Project"),
                             tr("Could not write the project file."));
        return;
    }
    m_viewport->setProjectClean();
}

bool DocumentActions::confirmDiscardChanges() {
    if (!m_viewport || !m_viewport->isProjectDirty()) {
        return true; // nothing to lose
    }
    // Explicit button labels: the platform theme's stock text for Discard is a long sentence
    // that the message box truncates ("lose without..."). Custom buttons keep the wording ours.
    QMessageBox box(m_window);
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
        saveProject();                       // its own dialog (Save As for a new document)
        return !m_viewport->isProjectDirty(); // a cancelled Save As or a failed write: nothing
    }                                        // was saved, so the scene is NOT let go of
    return box.clickedButton() == discard;
}

void DocumentActions::openProjectWithDialog() {
    if (!m_viewport) return;

    const QString path = QFileDialog::getOpenFileName(
        m_window, tr("Open Project"),
        PreferencesManager::instance().rememberedDirectoryOrDocuments(Constants::PREF_LAST_PROJECT_DIR),
        tr("PoseStudio Projects (*.pss)"));
    if (path.isEmpty()) return; // user cancelled
    if (!confirmDiscardChanges()) {
        return;
    }

    PreferencesManager::instance().setValue(Constants::PREF_LAST_PROJECT_DIR,
                                            QFileInfo(path).absolutePath());
    openProject(path);
}

DocumentActions::Opened DocumentActions::openProject(const QString &path) {
    // First pass: parse + link pre-flight. A missing source stops here — the scene is untouched.
    std::vector<std::string> missing;
    std::string error;
    const int status = m_viewport->loadProjectFile(path, {}, missing, error);
    return finishOpen(path, status, missing, error);
}

DocumentActions::Opened DocumentActions::finishOpen(const QString &path, int status,
                                                    const std::vector<std::string> &missing, std::string error) {
    if (status == 0) {
        return Opened::AsSaved; // loaded clean
    }
    if (status != 1) {
        QMessageBox::warning(m_window, tr("Open Project"),
                             tr("Could not read the project file:\n%1")
                                             .arg(QString::fromStdString(error)));
        return Opened::No;
    }

    // LINK RECOVERY — a message box per missing source: re-point it at another file, or skip
    // it. The skipped ones are named in one warning afterwards.
    std::map<std::string, std::string> recovered;
    QStringList skipped;
    for (const std::string& src : missing) {
        const QString original = QString::fromStdString(src);
        QMessageBox box(m_window);
        box.setWindowTitle(tr("Missing File"));
        box.setText(tr("A file this project refers to is missing:"));
        box.setInformativeText(original);
        QAbstractButton *pickButton = box.addButton(tr("Choose File..."),
                                                 QMessageBox::ActionRole);
        QAbstractButton *skipButton = box.addButton(tr("Skip"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == pickButton) {
            // Whatever a project can refer to — every format an import route takes — is offered,
            // so the replacement for a moved FBX, glTF, USD or splat file is listed too.
            namespace Formats = pose::ImportFormats;
            const QString replacement = QFileDialog::getOpenFileName(
                m_window, tr("Locate File"),
                PreferencesManager::instance().rememberedDirectoryOrDocuments(Constants::PREF_LAST_IMPORT_DIR),
                Formats::dialogFilter(tr("Importable Files"),
                                      Formats::nativeFigureExtensions() + Formats::riggedExtensions() +
                                          Formats::modelExtensions() + Formats::splatExtensions()));
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
            return Opened::No; // Cancel: the whole open is aborted, the scene stays as it was
        }
    }
    if (!skipped.isEmpty()) {
        QMessageBox::warning(m_window, tr("Open Project"),
                             tr("Skipped missing files:\n%1")
                                             .arg(skipped.join(QLatin1Char('\n'))));
    }

    // Second pass: the recovered paths are applied, everything else is imported as saved.
    std::vector<std::string> stillMissing;
    if (m_viewport->loadProjectFile(path, recovered, stillMissing, error) != 0) {
        QMessageBox::warning(m_window, tr("Open Project"),
                             tr("Could not load the project:\n%1")
                                             .arg(QString::fromStdString(error)));
        return Opened::No;
    }
    return Opened::Recovered;
}

void DocumentActions::relaunchApplication() {
    // THE SCENE SURVIVES THE RESTART: it is saved to a temporary project the new process reopens
    // (openAtStartup, by the restore flags), deletes, and re-attaches to the document it belonged
    // to — unsaved as it was — so a restart costs the user nothing and asks nothing. Only when
    // that temporary file cannot be written does the unsaved-changes prompt stand in.
    QStringList args;
    bool preserved = false;
    if (m_viewport) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString temp = dir + QStringLiteral("/restart-scene.pss");
        if (QDir().mkpath(dir) && m_viewport->saveProjectFile(temp)) {
            preserved = true;
            args << kRestoreSceneFlag << temp;
            if (!m_viewport->projectPath().isEmpty()) {
                args << kRestoreProjectFlag << m_viewport->projectPath();
            }
            if (!m_viewport->isProjectDirty()) {
                args << kRestoreCleanFlag;
            }
        }
    }
    if (!preserved && !confirmDiscardChanges()) {
        return; // Save cancelled, or the user changed their mind
    }
    // The new process FIRST, and this one goes only if it started: quitting regardless left the
    // work in a temporary project that a launch by hand never reads. Only the restore arguments
    // are handed on — the new process must not replay this one's "open-with" paths.
    if (!startNewInstance(args)) {
        QMessageBox::warning(m_window, tr("Restart"),
                             tr("PoseStudio could not start itself again, so it has not restarted.\n\n"
                                "Your scene is still open here. Close PoseStudio and start it again yourself "
                                "when you are ready."));
        return;
    }
    if (m_viewport) {
        m_viewport->setProjectClean(); // preserved, saved or let go of: no second prompt on the way out
    }
    QApplication::quit();
}

void DocumentActions::factoryReset() {
    // The unsaved-changes prompt comes BEFORE anything is wiped. Left to the window's close on
    // the way out, it came after the database was already gone — and a Cancel there kept this
    // instance running on the wiped database beside the new one.
    if (!confirmDiscardChanges()) {
        return;
    }
    initializeDatabase(DbInitMode::FactoryReset);
    if (m_viewport) {
        m_viewport->setProjectClean(); // saved or let go of above: no second prompt on the way out
    }
    // Wiped and rebuilt, then relaunched: many widgets (the asset tree, PreferencesManager's
    // cache) hold in-memory state derived from the old data, so rather than reconciling all of
    // it live the process restarts. With NO arguments: QApplication::arguments() would hand the
    // new process its own argv[0] as a file argument, and would also replay any original
    // "open-with" paths — re-importing last session's file into a factory-fresh state.
    if (!startNewInstance(QStringList())) {
        // (This instance cannot go on over a database it no longer matches: it closes anyway.)
        QMessageBox::warning(m_window, tr("Factory Reset"),
                             tr("The reset is done, but PoseStudio could not start itself again.\n\n"
                                "It will close now. Start it again yourself."));
    }
    QApplication::quit();
}

bool DocumentActions::startNewInstance(const QStringList &arguments) {
    return QProcess::startDetached(QApplication::applicationFilePath(), arguments);
}

QStringList DocumentActions::takeLaunchArguments(const QStringList &arguments) {
    QStringList others;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString& arg = arguments.at(i);
        if (arg == kRestoreSceneFlag && i + 1 < arguments.size()) {
            m_restoreScene = arguments.at(++i);
        } else if (arg == kRestoreProjectFlag && i + 1 < arguments.size()) {
            m_restoreProject = arguments.at(++i);
        } else if (arg == kRestoreCleanFlag) {
            m_restoreClean = true;
        } else if (arg.endsWith(QStringLiteral(".pss"), Qt::CaseInsensitive)) {
            m_launchProject = arg;
        } else {
            others << arg;
        }
    }
    return others;
}

void DocumentActions::openAtStartup() {
    // A project on the command line, or the scene of the process that restarted into this one
    // (relaunchApplication — the VR Mode "Turn On and Restart": it saved its scene to a temporary
    // project, which is reopened here, deleted, and re-attached to the document it came from,
    // unsaved as it was, so nothing is lost across the restart).
    if (!m_viewport || (m_restoreScene.isEmpty() && m_launchProject.isEmpty())) {
        return;
    }
    openAtStartupAttempt(0);
}

void DocumentActions::openAtStartupAttempt(int attempt) {
    // The load needs the renderer; on Windows the first expose built it inside the window's
    // show(), elsewhere the open waits until it exists (and, after the last attempt, lets the
    // load itself say that there is none).
    if (m_viewport->awaitingRenderer() && attempt < kStartupOpenAttempts) {
        QTimer::singleShot(kStartupOpenRetryMs, this, [this, attempt]() { openAtStartupAttempt(attempt + 1); });
        return;
    }
    const QString projectPath = m_restoreScene.isEmpty() ? m_launchProject : m_restoreScene;
    std::vector<std::string> missing;
    std::string              error;
    const int status = m_viewport->loadProjectFile(projectPath, {}, missing, error);
    if (status != 0) {
        qWarning() << "[project] could not open" << projectPath << ":" << QString::fromStdString(error)
                   << (missing.empty() ? "" : "(missing figure sources)");
    }
    if (m_restoreScene.isEmpty()) {
        // A project named on the command line: from here on as File → Open — an unreadable file
        // says so, a missing source is asked for. Not in a scripted run, where nobody is there
        // to answer: that one logs and goes on.
        if (status != 0 && !RunMode::scripted()) {
            finishOpen(projectPath, status, missing, error);
        }
        return;
    }
    // THE SCENE OF THE PROCESS THAT RESTARTED INTO THIS ONE. A source that has gone missing since
    // it was imported (the old process still had it in memory) is asked for as File → Open asks.
    const Opened opened = status == 0             ? Opened::AsSaved
                          : RunMode::scripted() ? Opened::No
                                                : finishOpen(projectPath, status, missing, error);
    if (opened == Opened::No) {
        // It did not come back, and the temporary project is all there is of it: the file STAYS,
        // the user is told where it is — and the window is NOT given the document it belonged
        // to. Titled with that project over an empty scene, and clean, the next Save would have
        // written the empty scene over the project without a question.
        if (!RunMode::scripted()) {
            QMessageBox::warning(m_window, tr("Restart"),
                                 tr("PoseStudio could not bring back the scene you had before the restart.\n\n"
                                    "It was kept as a project file, which File > Open can open:\n%1")
                                     .arg(QDir::toNativeSeparators(m_restoreScene)));
        }
        return;
    }
    QFile::remove(m_restoreScene); // the temporary project has served
    m_viewport->setProjectPath(m_restoreProject);
    if (!m_restoreClean || opened == Opened::Recovered) {
        // It was unsaved work and still is — or it came back with sources re-pointed or left
        // out, and is no longer the document as it was saved.
        m_viewport->markProjectDirty();
    }
}
