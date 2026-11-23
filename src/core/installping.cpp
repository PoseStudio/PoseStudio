// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file installping.cpp
 * @brief Implements InstallPing — see installping.h for what is sent and why.
 */

#include "installping.h"
#include "constants.h"
#include "preferencesmanager.h"
#include "runmode.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUuid>

// Both come from CMake (POSESTUDIO_PING_KEY / POSESTUDIO_PING_URL cache variables). The
// fallbacks keep a stray build without them compiling — and, with an empty key, silent.
#ifndef POSESTUDIO_PING_KEY
#define POSESTUDIO_PING_KEY ""
#endif
#ifndef POSESTUDIO_PING_URL
#define POSESTUDIO_PING_URL "https://www.posestudio.io/ping"
#endif

namespace {

// Fires shortly after the window is up: late enough that the ping never competes with startup
// (the request itself costs nothing visible), early enough that a quick "open it, look, close
// it" launch still counts — at 8 s a Release launch delivered 11 s after start, and a session
// shorter than that sent nothing at all.
constexpr int kStartupDelayMs = 2000;
// Hard cap on the whole request — a dead endpoint costs nothing visible.
constexpr int kTransferTimeoutMs = 10000;

QString endpointUrl() {
    const QByteArray overrideUrl = qgetenv("POSESTUDIO_PING_URL");
    return overrideUrl.isEmpty() ? QStringLiteral(POSESTUDIO_PING_URL) : QString::fromUtf8(overrideUrl);
}

/// "installer" when the Inno Setup uninstaller sits next to the executable (a per-user install
/// from the Setup.exe), else "portable" (the zip, or any other layout without one).
QString installKind() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    return QFile::exists(appDir.filePath(QStringLiteral("unins000.exe"))) ? QStringLiteral("installer")
                                                                           : QStringLiteral("portable");
}

QByteArray userAgent() {
    return QStringLiteral("PoseStudio/%1 (%2; %3)")
        .arg(QLatin1String(Constants::APP_VERSION), QSysInfo::prettyProductName(),
             QSysInfo::currentCpuArchitecture())
        .toUtf8();
}

} // namespace

InstallPing::InstallPing(QObject* parent) : QObject(parent) {}

void InstallPing::scheduleAtStartup(QObject* parent) {
    auto* ping = new InstallPing(parent);
    QTimer::singleShot(kStartupDelayMs, ping, &InstallPing::send);
}

bool InstallPing::isEnabled() {
    // Absent = on. Stored as "1"/"0"; QVariant::toBool also reads "true"/"false" should an
    // older row ever hold those.
    return PreferencesManager::instance().getValue(Constants::PREF_PING_ENABLED, true).toBool();
}

void InstallPing::setEnabled(bool enabled) {
    PreferencesManager::instance().setValue(Constants::PREF_PING_ENABLED,
                                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

bool InstallPing::isBuiltIn() {
    return !QByteArrayLiteral(POSESTUDIO_PING_KEY).isEmpty();
}

QString InstallPing::existingInstallId() {
    return PreferencesManager::instance().getValue(Constants::PREF_INSTALL_ID).toString();
}

QString InstallPing::installId() {
    QString id = existingInstallId();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        PreferencesManager::instance().setValue(Constants::PREF_INSTALL_ID, id);
    }
    return id;
}

QByteArray InstallPing::buildPayload() {
    // QJsonObject keeps its keys sorted, so the body is deterministic for a given set of values
    // (the server signs the raw bytes it receives, so this is a convenience, not a requirement).
    QJsonObject body;
    body.insert(QStringLiteral("install"), installId());
    body.insert(QStringLiteral("version"), QLatin1String(Constants::APP_VERSION));
    body.insert(QStringLiteral("os"), QSysInfo::productType());              // "windows", "macos", "ubuntu"…
    body.insert(QStringLiteral("os_version"), QSysInfo::prettyProductName()); // "Windows 11 Version 24H2"
    body.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());  // "x86_64", "arm64"
    body.insert(QStringLiteral("kind"), installKind());                       // "installer" / "portable"
    body.insert(QStringLiteral("qt"), QLibraryInfo::version().toString());
    body.insert(QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch());   // lets the server reject replays
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void InstallPing::send() {
    if (!isBuiltIn()) {
        qDebug() << "[ping] no signing key in this build; skipped";
        return;
    }
    if (!qEnvironmentVariableIsEmpty("POSESTUDIO_NO_PING")) {
        qDebug() << "[ping] POSESTUDIO_NO_PING set; skipped";
        return;
    }
    if (RunMode::probing()) {
        // A run a script or a probe drives is not somebody's launch (and its runner must not
        // have to remember POSESTUDIO_NO_PING for the count to stay honest).
        qDebug() << "[ping] a scripted or probing run; skipped";
        return;
    }
    if (!isEnabled()) {
        return;
    }

    // One ping per launch, deliberately with no daily cap: the server keys on the install ID, so
    // repeat launches never inflate the install count — they become a launches-per-install figure.
    const QByteArray body = buildPayload();
    const QByteArray signature = QMessageAuthenticationCode::hash(
        body, QByteArrayLiteral(POSESTUDIO_PING_KEY), QCryptographicHash::Sha256).toHex();

    if (!m_network) {
        // Route through the platform's proxy settings (Windows: the IE/WinHTTP configuration,
        // auto-detect and PAC included) so a corporate-network install still reaches the
        // endpoint. Official Qt binaries already default to this (QT_FEATURE_system_proxies), but
        // the default is a Qt CONFIGURE flag — a distro-packaged or custom-built Qt may have it off
        // — so it is asserted here rather than assumed. Safe to set app-wide: this is the app's
        // only network user, so there is no application proxy or factory to override.
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        m_network = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{QUrl(endpointUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader(QByteArrayLiteral("X-PoseStudio-Signature"), signature);
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply* reply = m_network->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
            qDebug() << "[ping] delivered, HTTP" << status;
        } else {
            qDebug() << "[ping] not delivered:" << reply->errorString() << "HTTP" << status;
        }
        reply->deleteLater();
    });
}
