/**
 * @file documentactions.h
 * @brief Declares DocumentActions: the life of the scene as a DOCUMENT.
 *
 * One contract lives here — what the document is (its .pss project file, or none yet), whether
 * the scene has changed since it was saved, and that unsaved work is never let go of without the
 * user's say:
 *
 *   - File → New / Open / Save / Save As, and the unsaved-changes prompt in front of whatever
 *     replaces the scene;
 *   - the main window's close, vetoed while that prompt is declined, and its title, which names
 *     the document and marks unsaved changes;
 *   - the RESTART THAT KEEPS THE SCENE (VR support is negotiated at startup, so turning it on is
 *     a restart): the scene is saved to a temporary project, handed to the new process on its
 *     command line, reopened there and re-attached to the document it belonged to — and the
 *     one that does not, Factory Reset's, which asks about unsaved changes before it wipes;
 *   - a project named on the command line ("open with" a .pss).
 *
 * MenuManager builds the menu entries and calls in; main.cpp constructs the one instance and
 * hands it the launch arguments. The scene itself is the viewport's: everything here goes
 * through the ViewportWidget facade (projectPath / isProjectDirty / saveProjectFile /
 * loadProjectFile), whose documentChanged signal is what re-titles the window.
 */

#ifndef DOCUMENTACTIONS_H
#define DOCUMENTACTIONS_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QEvent;
class QMainWindow;

namespace pose {
class EnvironmentPanel;
class ViewportWidget;
}

/**
 * @class DocumentActions
 * @brief New / Open / Save, the unsaved-changes gate, the window title and the scene-keeping
 *        restart. Owned by the main window.
 */
class DocumentActions : public QObject {
    Q_OBJECT

public:
    /// Titles @p window for an empty document and becomes its close gate (an event filter on
    /// it). The viewport is registered afterwards: main.cpp builds this before the panels exist.
    explicit DocumentActions(QMainWindow *window);

    /// The viewport whose scene is the document; from here on the window title follows its
    /// documentChanged. Every action is a no-op until this has been called.
    void setViewportWidget(pose::ViewportWidget *viewport);

    /// The live Environment tab, which File → New resets to the startup defaults (dials + HDRI
    /// caption) alongside the scene.
    void setEnvironmentPanel(pose::EnvironmentPanel *panel);

    /// File → New: the unsaved-changes prompt, then a fresh launch — picker mirrors synced via
    /// signals, the Environment tab reset, then the scene itself (models / camera / lighting /
    /// HDRI / history) to startup defaults.
    void newScene();

    /// File → Open: asks for a .pss project, then the unsaved-changes prompt, then openProject().
    void openProjectWithDialog();

    /// File → Save / Save As: writes the whole scene (figures + poses, transforms, environment,
    /// camera) as a .pss project. Save reuses the document's path — falling back to Save As when
    /// there is none; both remember the last project folder.
    void saveProject();
    void saveProjectAs();

    /// The shared unsaved-changes gate before the scene is replaced (New / Open, the window's
    /// close, a restart that cannot keep the scene): Save / Discard Changes / Cancel. True =
    /// proceed — after Save only if the save actually happened; false = cancelled.
    bool confirmDiscardChanges();

    /// Restarts the application with its scene: saved to a temporary project the new process
    /// reopens (see takeLaunchArguments / openAtStartup). The unsaved-changes prompt stands in
    /// only when that temporary project cannot be written. If the new process cannot be
    /// started this one says so and stays.
    void relaunchApplication();

    /// Preferences → Factory Reset: the unsaved-changes prompt (Cancel = nothing is reset), then
    /// the database wiped and rebuilt, then a restart with no scene — everything in memory was
    /// derived from the old data.
    void factoryReset();

    /// Reads the launch arguments that are the document's own — a .pss to open, and the flags
    /// relaunchApplication() hands the process it starts — and returns the others in order (the
    /// "open with" files main.cpp imports). @p arguments is QCoreApplication::arguments(): its
    /// first entry, the program, is skipped.
    QStringList takeLaunchArguments(const QStringList &arguments);

    /// Opens what takeLaunchArguments() found: the project named on the command line — as File →
    /// Open opens one, with its messages and its questions about missing sources — or the scene
    /// of the process that restarted into this one. The load needs the renderer, which is built
    /// on the viewport's first expose; where that has not arrived yet the open is retried until
    /// it has.
    void openAtStartup();

protected:
    /// The main window's close, vetoed when the unsaved-changes prompt is declined.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// How an open ended: not at all (the reason was shown, or the user cancelled), with the
    /// document as it was saved, or after the user re-pointed or skipped sources that were
    /// missing — the scene then differs from what the file says.
    enum class Opened { No, AsSaved, Recovered };

    /// Loads the project at @p path over the current scene: parse and link pre-flight first (a
    /// bad file or a cancelled recovery leaves the scene as it was), a message box per missing
    /// source to re-point or skip it, then the import and restore in document order.
    Opened openProject(const QString &path);
    /// openProject() from its first pass's result on (@p status, @p missing and @p error as
    /// ViewportWidget::loadProjectFile returned them): the messages, the recovery, the second pass.
    Opened finishOpen(const QString &path, int status, const std::vector<std::string> &missing, std::string error);

    /// One attempt of openAtStartup(), re-armed while the viewport has no renderer yet.
    void openAtStartupAttempt(int attempt);

    /// Starts another instance of the application with @p arguments; false if it did not start.
    bool startNewInstance(const QStringList &arguments);

    void updateWindowTitle();

    QMainWindow *m_window;
    pose::ViewportWidget *m_viewport = nullptr;
    pose::EnvironmentPanel *m_environmentPanel = nullptr;

    // What takeLaunchArguments() found, for openAtStartup().
    QString m_launchProject;        ///< A .pss on the command line ("open with" a project file)
    QString m_restoreScene;         ///< The temporary project of the process that restarted into this one
    QString m_restoreProject;       ///< … the document that scene belonged to ("" = none yet)
    bool    m_restoreClean = false; ///< … and whether it was saved, not unsaved work
};

#endif // DOCUMENTACTIONS_H
