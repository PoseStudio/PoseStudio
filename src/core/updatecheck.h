// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file updatecheck.h
 * @brief Declares UpdateCheck, the "is a newer PoseStudio out?" check against GitHub Releases.
 *
 * Once per launch, a few seconds after the main window shows, the app asks GitHub for the
 * project's latest release and, when its version is newer than the running one, shows a message
 * with a link to that release page. It is deliberately minimal and fully disclosed:
 *
 *   - One GET of the public releases/latest endpoint — no token, no account. Nothing about the
 *     user travels beyond the request itself and a "PoseStudio/<version>" user agent (GitHub
 *     requires one). Pre-releases and drafts are never offered: that endpoint excludes them.
 *   - It never blocks or delays anything: a timer arms it after the window is up, the request
 *     carries a transfer timeout, every failure is logged (qInfo, under "[update]") and
 *     ignored, and it never retries in the same session.
 *   - The user can switch the startup check off in Preferences → General
 *     (PREF_UPDATE_CHECK_ENABLED, default on) and can silence one particular version ("Skip This
 *     Version" in the prompt, PREF_UPDATE_SKIPPED_VERSION). Help → "Check for Updates…" runs the
 *     same check on demand and reports the outcome either way (up to date / newer / failed),
 *     ignoring a skipped version.
 *
 * Developer levers:
 *   - POSESTUDIO_NO_UPDATE_CHECK=1 in the environment disables the startup check for that run.
 *   - POSESTUDIO_UPDATE_URL=<url> overrides the endpoint — point it at a local server that
 *     returns {"tag_name":"v9.9.9","html_url":"..."} to see the prompt without a real release.
 * A scripted or probing run (core/runmode.h) skips the startup check: nobody is there to read
 * the prompt, and it would sit over the window such a run reads.
 */

#ifndef UPDATECHECK_H
#define UPDATECHECK_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWidget>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @class UpdateCheck
 * @brief Fetches the latest GitHub release and prompts when it is newer than this build.
 *
 * Qt-facing (core services layer): it reads and writes PreferencesManager and shows dialogs, so
 * use it only from the GUI thread.
 */
class UpdateCheck : public QObject {
    Q_OBJECT

public:
    /// @p window is both the owner (see InstallPing::scheduleAtStartup for why the owner must
    /// die before the QApplication) and the parent of any prompt.
    explicit UpdateCheck(QWidget* window);

    /// Arms the startup check: it runs once, a few seconds after this call, and prompts only
    /// when a newer, not-skipped release exists. Silent otherwise.
    static void scheduleAtStartup(QWidget* window);

    /// Help → Check for Updates…: runs the check now and always reports the outcome.
    static void checkNow(QWidget* window);

    /// Whether the startup check is switched on (Preferences → General; default on).
    static bool isEnabled();
    static void setEnabled(bool enabled);

    /// Compares two version strings numerically ("v0.3.14" vs "0.3.13"; a leading 'v' and any
    /// pre-release suffix are ignored, missing components read 0): positive when @p a is newer
    /// than @p b, negative when older, 0 when equal.
    static int compareVersions(const QString& a, const QString& b);

    /// Sends the request; @p interactive = report every outcome (the Help menu), else only a
    /// newer, not-skipped release (the startup check).
    void check(bool interactive);

private:
    void handleReply(QNetworkReply* reply, bool interactive);
    void promptNewer(const QString& version, const QUrl& page);

    QPointer<QWidget>      m_window;
    QNetworkAccessManager* m_network = nullptr; // created on first check, off the startup path
};

#endif // UPDATECHECK_H
