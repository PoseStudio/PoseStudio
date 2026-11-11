/**
 * @file updatecheck.cpp
 * @brief Implements UpdateCheck — see updatecheck.h for what is fetched and why.
 */

#include "updatecheck.h"
#include "constants.h"
#include "preferencesmanager.h"

#include <QDebug>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace {

// The project's latest full release (GitHub excludes drafts and pre-releases here — the stock
// content zip lives on a pre-release and is never offered as an update), and the page the
// prompt links to when the response carries no html_url of its own.
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/PoseStudio/PoseStudio/releases/latest";
constexpr const char* kLatestReleasePage = "https://github.com/PoseStudio/PoseStudio/releases/latest";

// A little after the install ping's 2 s: the window is up and idle, and a quick "open it,
// look, close it" launch still gets its prompt.
constexpr int kStartupDelayMs = 3000;
// Hard cap on the whole request — an unreachable GitHub costs nothing visible.
constexpr int kTransferTimeoutMs = 10000;

QUrl endpointUrl() {
    const QByteArray overrideUrl = qgetenv("POSESTUDIO_UPDATE_URL");
    return QUrl(overrideUrl.isEmpty() ? QString::fromLatin1(kLatestReleaseApi)
                                      : QString::fromUtf8(overrideUrl));
}

QByteArray userAgent() {
    return QStringLiteral("PoseStudio/%1").arg(QLatin1String(Constants::APP_VERSION)).toUtf8();
}

/// "v0.3.14", "0.3.14-rc1", "0.3" -> {0, 3, 14} / {0, 3, 14} / {0, 3, 0}.
void parseVersion(const QString& text, int out[3]) {
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        s.remove(0, 1);
    }
    const QStringList parts = s.split(QLatin1Char('.'));
    for (int i = 0; i < 3; ++i) {
        out[i] = 0;
        if (i < parts.size()) {
            // The leading digits only: "14-rc1" reads 14, "14beta" reads 14.
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("^\\d+")).match(parts.at(i).trimmed());
            if (m.hasMatch()) {
                out[i] = m.captured(0).toInt();
            }
        }
    }
}

QString skippedVersion() {
    return PreferencesManager::instance().getValue(Constants::PREF_UPDATE_SKIPPED_VERSION).toString();
}

} // namespace

UpdateCheck::UpdateCheck(QWidget* window) : QObject(window), m_window(window) {}

void UpdateCheck::scheduleAtStartup(QWidget* window) {
    auto* check = new UpdateCheck(window);
    QTimer::singleShot(kStartupDelayMs, check, [check]() { check->check(/*interactive=*/false); });
}

void UpdateCheck::checkNow(QWidget* window) {
    auto* check = new UpdateCheck(window);
    check->check(/*interactive=*/true);
}

bool UpdateCheck::isEnabled() {
    // Absent = on. Stored as "1"/"0".
    return PreferencesManager::instance().getValue(Constants::PREF_UPDATE_CHECK_ENABLED, true).toBool();
}

void UpdateCheck::setEnabled(bool enabled) {
    PreferencesManager::instance().setValue(Constants::PREF_UPDATE_CHECK_ENABLED,
                                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

int UpdateCheck::compareVersions(const QString& a, const QString& b) {
    int va[3];
    int vb[3];
    parseVersion(a, va);
    parseVersion(b, vb);
    for (int i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) {
            return va[i] < vb[i] ? -1 : 1;
        }
    }
    return 0;
}

void UpdateCheck::check(bool interactive) {
    if (!interactive) {
        if (!qEnvironmentVariableIsEmpty("POSESTUDIO_NO_UPDATE_CHECK")) {
            qInfo() << "[update] POSESTUDIO_NO_UPDATE_CHECK set; skipped";
            deleteLater();
            return;
        }
        if (!isEnabled()) {
            deleteLater();
            return;
        }
    }
    if (!m_network) {
        // The platform's proxy settings, as the install ping asserts them (see installping.cpp).
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request{endpointUrl()};
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setTransferTimeout(kTransferTimeoutMs);
    // GitHub answers the API with a redirect only in edge cases; follow them, never off https.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, interactive]() {
        handleReply(reply, interactive);
        reply->deleteLater();
        deleteLater(); // one check per object
    });
}

void UpdateCheck::handleReply(QNetworkReply* reply, bool interactive) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        qInfo() << "[update] check failed:" << reply->errorString() << "HTTP" << status;
        if (interactive && m_window) {
            QMessageBox::warning(m_window, tr("Check for Updates"),
                                 tr("PoseStudio couldn't check for updates right now.\n\n%1")
                                     .arg(reply->errorString()));
        }
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject release = doc.object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.isEmpty()) {
        qInfo() << "[update] check failed: no tag_name in the response";
        if (interactive && m_window) {
            QMessageBox::warning(m_window, tr("Check for Updates"),
                                 tr("PoseStudio couldn't read the release information."));
        }
        return;
    }
    QString version = tag;
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    QUrl page(release.value(QStringLiteral("html_url")).toString());
    if (!page.isValid() || page.scheme() != QLatin1String("https")) {
        page = QUrl(QString::fromLatin1(kLatestReleasePage));
    }
    const QString current = QLatin1String(Constants::APP_VERSION);
    if (compareVersions(version, current) <= 0) {
        qInfo() << "[update] up to date:" << current << "(latest release" << version << ")";
        if (interactive && m_window) {
            QMessageBox::information(m_window, tr("Check for Updates"),
                                     tr("You are running the latest release (PoseStudio %1).")
                                         .arg(current));
        }
        return;
    }
    if (!interactive && skippedVersion() == version) {
        qInfo() << "[update] newer release" << version << "available but skipped by the user";
        return;
    }
    qInfo() << "[update] newer release available:" << version << "(running" << current << ")";
    promptNewer(version, page);
}

void UpdateCheck::promptNewer(const QString& version, const QUrl& page) {
    if (!m_window) {
        return;
    }
    // Non-blocking (open(), not exec()): this runs from a network reply a few seconds into the
    // session, and must never hold up a pending import or the event loop. Deleted on close.
    auto* box = new QMessageBox(m_window);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(tr("Update Available"));
    box->setIcon(QMessageBox::Information);
    box->setTextFormat(Qt::RichText);
    // Qt's QSS engine does not style <a> inside a QLabel: the link color goes inline, the
    // app's accent (see the same pattern in the Asset Manager's hint label).
    box->setText(tr("<b>PoseStudio %1 is available.</b><br>You are running %2.<br><br>"
                    "<a href=\"%3\" style=\"color:%4;\">%3</a>")
                     .arg(version.toHtmlEscaped(), QLatin1String(Constants::APP_VERSION),
                          page.toString().toHtmlEscaped(), QLatin1String(Constants::COLOR_ACCENT)));
    QPushButton* openButton = box->addButton(tr("Open Download Page"), QMessageBox::AcceptRole);
    QPushButton* skipButton = box->addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
    box->addButton(tr("Later"), QMessageBox::RejectRole);
    box->setDefaultButton(openButton);
    connect(box, &QDialog::finished, box, [box, openButton, skipButton, page, version]() {
        if (box->clickedButton() == openButton) {
            QDesktopServices::openUrl(page);
        } else if (box->clickedButton() == skipButton) {
            PreferencesManager::instance().setValue(Constants::PREF_UPDATE_SKIPPED_VERSION, version);
        }
    });
    box->open();
}
