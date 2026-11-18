/**
 * @file importprogress.h
 * @brief One import's modal staged progress dialog and its phase clock — what the four import
 *        services (model, figure, rigged character, Gaussian splats) show while a file is read,
 *        decoded and uploaded on the GUI thread — and the one report of an import that failed
 *        (reportImportFailure).
 */

#ifndef IMPORTPROGRESS_H
#define IMPORTPROGRESS_H

#include <QElapsedTimer>
#include <QString>

#include <memory>

class QProgressDialog;

namespace pose {

/**
 * @class ImportProgress
 * @brief One import's modal staged progress dialog (optional) and its phase clock.
 *
 * Import runs synchronously on the GUI thread (parse -> texture decode -> bake -> GPU
 * upload), so a large model would otherwise freeze the UI with no feedback. Shown, the
 * dialog is ApplicationModal with no cancel button: each phase is a short, atomic, blocking
 * operation that can't be safely interrupted partway through. It pumps the event loop on
 * setValue(), so the bar/label repaint between phases. Not shown (the startup/command-line
 * drain, called from inside the expose/init path where that pump could re-enter it), the
 * object still keeps the clock so the timing log is identical either way. The dialog closes
 * when done() reaches the maximum, or with the object on an exception path.
 */
class ImportProgress {
public:
    /// @p show drives the dialog; @p windowTitle / @p readingLabel are its title and the
    /// busy-indicator label shown while the file parses. The dialog is forced on-screen and
    /// painted NOW: QProgressDialog's own auto-show is driven by a remaining-time estimate
    /// that can't see the upcoming blocking parse, so it would otherwise first appear
    /// mid-import.
    ImportProgress(bool show, const QString& windowTitle, const QString& readingLabel);
    ~ImportProgress();
    ImportProgress(const ImportProgress&) = delete;
    ImportProgress& operator=(const ImportProgress&) = delete;

    /// The parse finished: records its time and turns the busy indicator into a determinate
    /// bar at step 1 of kSteps.
    void parsed();
    /// Advances the bar one step under @p label (decode / bake / upload).
    void phase(const QString& label);
    /// Jumps the bar to its maximum, which closes the dialog.
    void done();

    qint64 elapsedMs() const { return m_clock.elapsed(); }
    qint64 parseMs() const { return m_parseMs; }

private:
    static constexpr int kSteps = 5; // parse, decode, bake, upload, done

    std::unique_ptr<QProgressDialog> m_dialog; // null when not shown
    QElapsedTimer                    m_clock;
    qint64                           m_parseMs = 0;
    int                              m_step = 0;
};

/// THE REPORT OF AN IMPORT THAT FAILED, one for the import services: a warning box titled
/// @p title over the application's window — `Could not import "<path>".` and, under it,
/// @p detail (the reader's own words for what is wrong with the file).
///
/// Only for an INTERACTIVE import (File → Import, a double-click in a library). It shows nothing
/// when @p interactive is false — the startup and command-line imports run inside the window's
/// expose / initialisation path, where a modal box's event loop could re-enter it — nor in a
/// scripted run (POSESTUDIO_IK_SCRIPT), where nobody is there to dismiss it and it would hold
/// the script. The log line is the report there: the CALLER writes it, in every case.
///
/// The caller's ImportProgress must be gone by then — it lives INSIDE the service's try block —
/// or its busy dialog stands under the message.
void reportImportFailure(bool interactive, const QString& title, const QString& path, const QString& detail);

} // namespace pose

#endif // IMPORTPROGRESS_H
