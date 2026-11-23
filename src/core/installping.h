// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file installping.h
 * @brief Declares InstallPing, the anonymous "this install exists" beacon sent on every launch.
 *
 * PoseStudio counts active installations and their versions by sending one small, signed POST
 * per launch to the project's ping endpoint. It is deliberately minimal and fully disclosed:
 *
 *   - It sends a random install ID (a UUID generated on first use and kept in Preferences), the
 *     app version, the operating system name and version, the CPU architecture, whether the app
 *     was installed or is running portable, the Qt version, and a timestamp. No names, no paths,
 *     no hardware IDs, no usage data — see buildPayload() for the exact set.
 *   - It never blocks or delays anything: it fires from a timer a few seconds after the main
 *     window shows, carries a transfer timeout, ignores every failure, and never retries in the
 *     same session. One ping per launch, no daily cap: the server keys on the install ID, so
 *     repeat launches can't inflate the install count.
 *   - Users can switch it off in Preferences → General (PREF_PING_ENABLED, default on).
 *
 * Anti-spoofing: the request body is signed with HMAC-SHA256 (X-PoseStudio-Signature) under a
 * key compiled in from the POSESTUDIO_PING_KEY build definition. The key is NOT in the
 * repository — official release builds receive it from a GitHub Actions secret; any other build
 * has an empty key and never pings at all (the server rejects unsigned pings, so there is nothing
 * to gain by sending them). A key inside a shipped binary is extractable in principle, so it is
 * only the first layer: the server also counts distinct install IDs rather than requests,
 * validates every field, and rate-limits new IDs per address.
 *
 * The key is the ONE lever: a build configured with -DPOSESTUDIO_PING_KEY=... pings, Debug or
 * Release, and a build without one never does. (An earlier cut also required an endpoint
 * override in Debug builds; the second gate only confused local testing and was dropped.)
 *
 * Developer levers:
 *   - POSESTUDIO_NO_PING=1 in the environment disables the ping for that run.
 *   - POSESTUDIO_PING_URL=<url> overrides the endpoint (a local listener, a staging server).
 * A scripted or probing run (core/runmode.h) never pings: it is not somebody's launch.
 */

#ifndef INSTALLPING_H
#define INSTALLPING_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;

/**
 * @class InstallPing
 * @brief Sends the anonymous per-launch install ping; a thin, fire-and-forget QObject.
 *
 * Qt-facing (core services layer): it reads and writes PreferencesManager, so — like the rest
 * of the UI code — use it only from the GUI thread.
 */
class InstallPing : public QObject {
    Q_OBJECT

public:
    explicit InstallPing(QObject* parent = nullptr);

    /// Arms the startup ping: send() runs once, a few seconds after this call, on an InstallPing
    /// owned by `parent`. Call after the main window is shown so it never competes with startup.
    /// Pass the MAIN WINDOW (or any object that dies before the QApplication), never the
    /// application object itself: a QApplication's children are deleted by QObject's destructor
    /// AFTER QCoreApplication's own destructor body has torn down the event dispatcher, and the
    /// QNetworkAccessManager created under the ping on first send dying that late is a
    /// use-after-teardown. Owned by the window, the ping is destroyed while the event system is
    /// still alive; an in-flight reply is simply aborted — the fire-and-forget contract.
    static void scheduleAtStartup(QObject* parent);

    /// Whether the user has the ping switched on (Preferences → General; default on).
    static bool isEnabled();
    static void setEnabled(bool enabled);

    /// True when this binary carries a signing key, i.e. it is an official build that can ping.
    /// Local builds have none, and the General preferences page says so next to the toggle.
    static bool isBuiltIn();

    /// The random per-installation ID, created (and persisted) on first use — the value the
    /// ping sends. Minting it is a side effect, so DISPLAY code must use existingInstallId().
    static QString installId();

    /// The stored install ID, or empty if none has been created yet — never creates one.
    /// Preferences → General shows it, and merely opening a dialog must not mint and persist an
    /// identifier (in a keyless build, or with the ping switched off, one legitimately never
    /// exists).
    static QString existingInstallId();

    /// Sends the ping now, if every gate passes (build key, environment, and the user's
    /// preference). Failures are logged at debug level and ignored.
    void send();

private:
    /// The compact JSON body — the complete set of fields the ping ever carries.
    static QByteArray buildPayload();

    QNetworkAccessManager* m_network = nullptr; // created on first send, off the startup path
};

#endif // INSTALLPING_H
