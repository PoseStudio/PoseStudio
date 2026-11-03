/**
 * @file whatsnewdialog.h
 * @brief The "What's New in PoseStudio x.y.z" window: the running version's release notes,
 *        shown ONCE — on the first start of a new version, as soon as the boot splash has been
 *        dismissed — and on demand from Help → What's New…
 *
 * The notes are the CHANGELOG's section for the running version (Constants::APP_VERSION), read
 * from the copy embedded for the manual (qrc:/manual/changelog.md) and rendered as the manual
 * renders a page (HelpManual::renderMarkdown), so the release notes are still written once.
 * "Once" is a remembered version (Constants::PREF_WHATS_NEW_SEEN_VERSION): the window opens when
 * the remembered version is not this one — a fresh install included, where nothing is remembered
 * — and the version is remembered the moment it opens, whichever way it is closed. With the
 * toggle off (Preferences → General, Constants::PREF_WHATS_NEW_ENABLED) the version is remembered
 * silently, so switching it back on never shows a stale one. A build whose CHANGELOG has no
 * section for its version shows nothing at startup.
 *
 * Why after the splash: the boot splash covers the window until the user's first click, and a
 * dialog raised under it would be read as part of the branding; raised on its dismissal, the
 * notes are the first thing the user sees of the new version.
 *
 * Developer levers: POSESTUDIO_WHATS_NEW=1 opens it at startup whatever is remembered, without
 * waiting for the splash; POSESTUDIO_NO_WHATS_NEW=1 never opens it at startup; and a scripted or
 * probing run (POSESTUDIO_IK_SCRIPT, POSESTUDIO_IK_BENCH, POSESTUDIO_MANUAL_SHOT,
 * POSESTUDIO_DUMP_SKELETON) never sees it. POSESTUDIO_WHATS_NEW_SHOT=<png> (with
 * POSESTUDIO_WHATS_NEW=1) saves a picture of the window and exits — the way to LOOK at it.
 */

#ifndef WHATS_NEW_DIALOG_H
#define WHATS_NEW_DIALOG_H

#include "helpmanual.h"

#include <QDialog>
#include <QPointer>
#include <QString>

class QTextBrowser;
class QUrl;

/**
 * @class WhatsNewDialog
 * @brief The window; see the file comment. Non-modal, deleted on close, one at a time.
 */
class WhatsNewDialog : public QDialog {
    Q_OBJECT

public:
    /// @p owner is the main window: the parent of the dialog, and where the manual opens from.
    explicit WhatsNewDialog(QWidget* owner);

    /// The startup rule (see the file comment): opens the window when this version has not been
    /// shown yet and the toggle is on, and remembers the version either way.
    static void showIfUpdated(QWidget* owner);
    /// Help → What's New…: opens the window now (raising the one already open).
    static WhatsNewDialog* showNow(QWidget* owner);

    /// Whether the window opens after an update (Preferences → General; default on).
    static bool isEnabled();
    static void setEnabled(bool enabled);

    /// The CHANGELOG section for @p version as Markdown (its heading line left out), or an empty
    /// string when the embedded changelog has none. @p dateOut receives the heading's date, if any.
    static QString notesFor(const QString& version, QString* dateOut = nullptr);

private:
    void onAnchorClicked(const QUrl& url);

    QPointer<QWidget> m_owner;
    HelpManual        m_manual;
    QTextBrowser*     m_browser = nullptr;
};

#endif // WHATS_NEW_DIALOG_H
