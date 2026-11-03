/**
 * @file whatsnewdialog.cpp
 * @brief The "What's New" window (see whatsnewdialog.h).
 */

#include "whatsnewdialog.h"

#include "constants.h"
#include "helpwindow.h"
#include "preferencesmanager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextList>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>

namespace {

constexpr const char* kChangelogFile = "changelog.md";

/// The one window open at a time (Help → What's New… raises it instead of stacking a second).
QPointer<WhatsNewDialog> g_open;

/// A run that drives the app itself — a scripted posing test, the bench, a manual screenshot, a
/// skeleton dump — never sees the window: it would sit over the frames such a run reads.
bool probingRun() {
    return qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT") || qEnvironmentVariableIsSet("POSESTUDIO_IK_BENCH") ||
           qEnvironmentVariableIsSet("POSESTUDIO_MANUAL_SHOT") || qEnvironmentVariableIsSet("POSESTUDIO_DUMP_SKELETON");
}

} // namespace

WhatsNewDialog::WhatsNewDialog(QWidget* owner) : QDialog(owner), m_owner(owner) {
    setObjectName(QStringLiteral("WhatsNewDialog"));
    setWindowTitle(tr("What's New in PoseStudio %1").arg(QLatin1String(Constants::APP_VERSION)));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(640, 560);
    m_manual.load(); // (the manifest: a link from the notes to a manual page opens that page)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // THE NOTES: the running version's CHANGELOG section under a title, rendered and styled as
    // the manual renders its pages (the same headings, links, code and tables), scrollable.
    m_browser = new QTextBrowser(this);
    m_browser->setObjectName(QStringLiteral("WhatsNewBrowser"));
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->setOpenLinks(false); // every link goes through onAnchorClicked
    m_browser->setOpenExternalLinks(false);
    m_browser->setReadOnly(true);
    m_browser->document()->setDocumentMargin(24.0);
    connect(m_browser, &QTextBrowser::anchorClicked, this, &WhatsNewDialog::onAnchorClicked);
    layout->addWidget(m_browser, 1);

    QString       date;
    const QString notes = notesFor(QLatin1String(Constants::APP_VERSION), &date);
    QString       markdown = QStringLiteral("# What's New in PoseStudio %1\n\n").arg(QLatin1String(Constants::APP_VERSION));
    if (!date.isEmpty()) {
        markdown += tr("*Released %1*\n\n").arg(date);
    }
    if (notes.isEmpty()) {
        markdown += tr("This build has no release notes section in its changelog yet. "
                       "The full history is in the manual's *What's New* page.");
    } else {
        markdown += notes;
    }
    m_manual.renderMarkdown(markdown, *m_browser->document());
    // Air between the entries: the notes are read, not scanned like a manual page — a little
    // more line height, and a gap after every paragraph and list item.
    for (QTextBlock block = m_browser->document()->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        if (format.headingLevel() > 0) {
            continue;
        }
        format.setLineHeight(118.0, QTextBlockFormat::ProportionalHeight);
        format.setBottomMargin(std::max(format.bottomMargin(), block.textList() != nullptr ? 5.0 : 10.0));
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
    }
    // The cursor stands at the end after a render, and a shown browser scrolls its cursor into
    // view — the window would open at the bottom of the notes. Start at the top.
    m_browser->moveCursor(QTextCursor::Start);
    m_browser->verticalScrollBar()->setValue(0);

    // THE FOOTER: the toggle that Preferences → General also carries, the manual's full history,
    // Close (Enter / Esc).
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("WhatsNewFooter"));
    auto* row = new QHBoxLayout(footer);
    row->setContentsMargins(16, 10, 16, 10);
    row->setSpacing(10);
    auto* toggle = new QCheckBox(tr("Show this window after an update"), footer);
    toggle->setChecked(isEnabled());
    connect(toggle, &QCheckBox::toggled, this, [](bool enabled) { setEnabled(enabled); });
    row->addWidget(toggle);
    row->addStretch(1);
    auto* history = new QPushButton(tr("Release Notes"), footer);
    history->setToolTip(tr("The full release history, in the User Manual"));
    connect(history, &QPushButton::clicked, this, [this]() {
        if (m_owner) {
            HelpWindow::open(m_owner, QStringLiteral("whats-new"));
        }
    });
    row->addWidget(history);
    auto* close = new QPushButton(tr("Close"), footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    row->addWidget(close);
    layout->addWidget(footer);
}

void WhatsNewDialog::showIfUpdated(QWidget* owner) {
    if (qEnvironmentVariableIsSet("POSESTUDIO_NO_WHATS_NEW") || probingRun()) {
        return;
    }
    const bool    force = qEnvironmentVariableIsSet("POSESTUDIO_WHATS_NEW");
    const QString version = QLatin1String(Constants::APP_VERSION);
    const QString seen = PreferencesManager::instance().getValue(Constants::PREF_WHATS_NEW_SEEN_VERSION).toString();
    if (!force && seen == version) {
        return;
    }
    // Remembered as soon as the decision is taken, whichever way the window is closed — and
    // when the toggle is off, so that switching it on later never shows a stale version.
    PreferencesManager::instance().setValue(Constants::PREF_WHATS_NEW_SEEN_VERSION, version);
    if (!force && !isEnabled()) {
        qInfo("[whats-new] version %s first seen; the window is switched off", qPrintable(version));
        return;
    }
    if (notesFor(version).isEmpty()) {
        qInfo("[whats-new] version %s first seen, but the changelog has no section for it", qPrintable(version));
        return;
    }
    qInfo("[whats-new] version %s first seen (was %s): showing its release notes", qPrintable(version),
          seen.isEmpty() ? "nothing" : qPrintable(seen));
    showNow(owner);
}

WhatsNewDialog* WhatsNewDialog::showNow(QWidget* owner) {
    if (g_open) {
        g_open->raise();
        g_open->activateWindow();
        return g_open;
    }
    auto* dialog = new WhatsNewDialog(owner);
    g_open = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
}

bool WhatsNewDialog::isEnabled() {
    // Absent = on. Stored as "1"/"0" (the update check's convention).
    return PreferencesManager::instance().getValue(Constants::PREF_WHATS_NEW_ENABLED, true).toBool();
}

void WhatsNewDialog::setEnabled(bool enabled) {
    PreferencesManager::instance().setValue(Constants::PREF_WHATS_NEW_ENABLED,
                                            enabled ? QStringLiteral("1") : QStringLiteral("0"));
}

QString WhatsNewDialog::notesFor(const QString& version, QString* dateOut) {
    // The changelog keeps one `## [x.y.z] - date` section per release (Keep a Changelog); the
    // section runs to the next `## ` heading. The embedded copy is the manual's.
    HelpManual     manual;
    const QString  markdown = manual.markdownOf(QLatin1String(kChangelogFile));
    const QString  heading = QStringLiteral("## [%1]").arg(version);
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    int from = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith(heading) && (line.size() == heading.size() || !line.at(heading.size()).isLetterOrNumber())) {
            from = i;
            if (dateOut != nullptr) {
                const int dash = line.indexOf(QLatin1String(" - "), heading.size());
                *dateOut = dash >= 0 ? line.mid(dash + 3).trimmed() : QString();
            }
            break;
        }
    }
    if (from < 0) {
        return QString();
    }
    QStringList section;
    for (int i = from + 1; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QLatin1String("## "))) {
            break;
        }
        section.append(lines.at(i));
    }
    return section.join(QLatin1Char('\n')).trimmed();
}

void WhatsNewDialog::onAnchorClicked(const QUrl& url) {
    // A web link opens in the browser; a link to a manual page (`posing.md#joint-pins`, as the
    // pages link each other) opens the manual there; anything else the manual's What's New page.
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
        return;
    }
    if (!m_owner) {
        return;
    }
    const QString   file = url.path().section(QLatin1Char('/'), -1);
    const ManualPage* page = file.endsWith(QLatin1String(".md")) ? m_manual.pageByFile(file) : nullptr;
    HelpWindow::open(m_owner, page != nullptr ? page->id : QStringLiteral("whats-new"));
}
