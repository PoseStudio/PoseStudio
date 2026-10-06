/**
 * @file helpwindow.cpp
 * @brief Implements HelpWindow (see helpwindow.h): the contents tree, the page browser, search,
 *        and the back / forward history.
 */

#include "helpwindow.h"

#include "constants.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

/// The one manual window of the process (see HelpWindow::open).
QPointer<HelpWindow> g_window;

/// Tree item roles: the page file, the heading anchor ("" for the page itself), the page id.
constexpr int kFileRole = Qt::UserRole;
constexpr int kAnchorRole = Qt::UserRole + 1;
constexpr int kIdRole = Qt::UserRole + 2;

constexpr int kSearchDebounceMs = 150;

} // namespace

// --- Construction ------------------------------------------------------------------------------

HelpWindow::HelpWindow(QWidget* owner)
    : QWidget(owner, Qt::Window) {
    setObjectName(QStringLiteral("HelpWindow"));
    setWindowTitle(tr("%1 User Manual").arg(Constants::APP_NAME));
    setAttribute(Qt::WA_DeleteOnClose, false); // (one window per app: hidden, not destroyed, on close)

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // --- The toolbar: back / forward, then the search field ---
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("HelpToolbar"));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(10, 8, 10, 8);
    toolbarLayout->setSpacing(6);

    m_back = new QToolButton(toolbar);
    m_back->setObjectName(QStringLiteral("HelpBack"));
    m_back->setText(tr("‹ Back"));
    m_back->setToolTip(tr("Back to the previous page (Alt+Left)"));
    m_back->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    m_forward = new QToolButton(toolbar);
    m_forward->setObjectName(QStringLiteral("HelpForward"));
    m_forward->setText(tr("Forward ›"));
    m_forward->setToolTip(tr("Forward again (Alt+Right)"));
    m_forward->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    toolbarLayout->addWidget(m_back);
    toolbarLayout->addWidget(m_forward);
    toolbarLayout->addStretch(1);

    m_search = new QLineEdit(toolbar);
    m_search->setObjectName(QStringLiteral("HelpSearch"));
    m_search->setPlaceholderText(tr("Search the manual  (Ctrl+F)"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(280);
    toolbarLayout->addWidget(m_search);
    outer->addWidget(toolbar);

    // --- The body: contents on the left, the page on the right ---
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("HelpSplitter"));
    splitter->setChildrenCollapsible(false);

    m_contents = new QTreeWidget(splitter);
    m_contents->setObjectName(QStringLiteral("HelpToc"));
    m_contents->setHeaderHidden(true);
    m_contents->setRootIsDecorated(true);
    m_contents->setIndentation(14);
    m_contents->setUniformRowHeights(true);
    m_contents->setFrameShape(QFrame::NoFrame);
    m_contents->setMinimumWidth(180);
    splitter->addWidget(m_contents);

    m_browser = new QTextBrowser(splitter);
    m_browser->setObjectName(QStringLiteral("HelpBrowser"));
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->setOpenLinks(false);   // every link goes through onAnchorClicked
    m_browser->setOpenExternalLinks(false);
    m_browser->setReadOnly(true);
    m_browser->document()->setDocumentMargin(28.0);
    splitter->addWidget(m_browser);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 800});
    outer->addWidget(splitter, 1);

    // --- Wiring ---
    connect(m_back, &QToolButton::clicked, this, &HelpWindow::goBack);
    connect(m_forward, &QToolButton::clicked, this, &HelpWindow::goForward);
    connect(m_browser, &QTextBrowser::anchorClicked, this, &HelpWindow::onAnchorClicked);
    connect(m_contents, &QTreeWidget::currentItemChanged, this, [this]() { onContentsSelectionChanged(); });

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(kSearchDebounceMs);
    connect(m_searchDebounce, &QTimer::timeout, this, [this]() {
        applySearchFilter();
        highlightMatches();
    });
    connect(m_search, &QLineEdit::textChanged, m_searchDebounce, qOverload<>(&QTimer::start));
    connect(m_search, &QLineEdit::returnPressed, this, [this]() { findNextMatch(false); });

    auto* focusSearch = new QShortcut(QKeySequence::Find, this);
    connect(focusSearch, &QShortcut::activated, this, [this]() {
        m_search->setFocus();
        m_search->selectAll();
    });
    auto* nextMatch = new QShortcut(QKeySequence::FindNext, this);
    connect(nextMatch, &QShortcut::activated, this, [this]() { findNextMatch(false); });
    auto* previousMatch = new QShortcut(QKeySequence::FindPrevious, this);
    connect(previousMatch, &QShortcut::activated, this, [this]() { findNextMatch(true); });
    auto* closeWindow = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(closeWindow, &QShortcut::activated, this, &QWidget::close);

    // --- The manual ---
    if (!m_manual.load()) {
        m_browser->setMarkdown(tr("# User Manual\n\n%1").arg(m_manual.errorText()));
    } else {
        buildContents();
        if (const ManualPage* first = m_manual.firstPage()) {
            openLocation({first->file, QString()}, true);
        }
    }
    updateNavigationButtons();

    // A comfortable reading size, no larger than the screen; centred on the owner.
    QSize size(1120, 780);
    if (const QScreen* screen = owner != nullptr ? owner->screen() : QApplication::primaryScreen()) {
        size = size.boundedTo(screen->availableGeometry().size() * 0.92);
    }
    resize(size);
    if (owner != nullptr) {
        move(owner->frameGeometry().center() - QPoint(size.width() / 2, size.height() / 2));
    }
}

void HelpWindow::open(QWidget* owner, const QString& pageId) {
    if (g_window.isNull()) {
        g_window = new HelpWindow(owner);
    }
    if (!pageId.isEmpty()) {
        g_window->showPage(pageId);
    }
    g_window->show();
    g_window->raise();
    g_window->activateWindow();
}

void HelpWindow::showPage(const QString& pageId) {
    if (const ManualPage* page = m_manual.pageById(pageId)) {
        openLocation({page->file, QString()}, true);
    }
}

void HelpWindow::setSearchText(const QString& text) {
    m_search->setText(text);
    m_searchDebounce->stop();
    applySearchFilter();
    highlightMatches();
}

// --- The table of contents -----------------------------------------------------------------------

void HelpWindow::buildContents() {
    m_contents->clear();
    // A page becomes an item; its `##` headings become its sub-items (its `#` title is the page
    // itself), and its child pages follow them.
    std::function<void(QTreeWidgetItem*, const ManualPage&)> add = [&](QTreeWidgetItem* parent, const ManualPage& page) {
        auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_contents);
        item->setText(0, page.title);
        item->setData(0, kFileRole, page.file);
        item->setData(0, kAnchorRole, QString());
        item->setData(0, kIdRole, page.id);
        item->setToolTip(0, page.title);
        for (const ManualHeading& heading : m_manual.headingsOf(page.file)) {
            if (heading.level != 2) {
                continue;
            }
            auto* section = new QTreeWidgetItem(item);
            section->setText(0, heading.text);
            section->setData(0, kFileRole, page.file);
            section->setData(0, kAnchorRole, heading.slug);
            section->setData(0, kIdRole, QString());
            section->setToolTip(0, heading.text);
        }
        for (const ManualPage& child : page.children) {
            add(item, child);
        }
    };
    for (const ManualPage& page : m_manual.pages()) {
        add(nullptr, page);
    }
    m_contents->collapseAll();
}

QTreeWidgetItem* HelpWindow::itemFor(const QString& file, const QString& anchor) const {
    QTreeWidgetItem* pageItem = nullptr;
    for (QTreeWidgetItemIterator it(m_contents); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->data(0, kFileRole).toString() != file) {
            continue;
        }
        const QString itemAnchor = item->data(0, kAnchorRole).toString();
        if (itemAnchor == anchor) {
            return item;
        }
        if (itemAnchor.isEmpty()) {
            pageItem = item;
        }
    }
    return pageItem; // (a heading with no entry of its own — a `###` — selects its page)
}

void HelpWindow::syncContentsSelection(const Location& where) {
    QTreeWidgetItem* item = itemFor(where.file, where.anchor);
    if (item == nullptr) {
        return;
    }
    // Programmatic selection: the collapse / expand / scroll below re-lays-out the tree, and a
    // QTreeWidget will then re-resolve its current index and selection to the row under a
    // stationary mouse cursor - so the TOC ends up highlighting whatever happens to sit under
    // the pointer instead of the page we just opened. Block the signals for the whole sync so
    // that re-selection cannot re-enter onContentsSelectionChanged and open the page under the
    // mouse, then re-assert the target as current + selected once the event loop has settled
    // (the drift lands in a later iteration, after this function returns).
    const QSignalBlocker blocker(m_contents);
    m_syncingContents = true; // (belt and braces: also guards a same-event-loop re-entrancy)
    auto pinTarget = [this, item]() {
        m_contents->setCurrentItem(item);
        m_contents->selectionModel()->select(
            m_contents->indexFromItem(item),
            QItemSelectionModel::Clear | QItemSelectionModel::Select | QItemSelectionModel::Rows
            | QItemSelectionModel::NoUpdate);
    };
    if (m_search->text().trimmed().isEmpty()) {
        m_contents->collapseAll(); // (only the page being read stays open; a search keeps its matches open)
    }
    for (QTreeWidgetItem* parent = item->parent(); parent != nullptr; parent = parent->parent()) {
        parent->setExpanded(true);
    }
    if (where.anchor.isEmpty()) {
        item->setExpanded(true);
    }
    pinTarget();
    m_contents->scrollToItem(item);
    QTimer::singleShot(0, this, [this, item, pinTarget]() {
        // The relayout / scroll can re-resolve the selection to the row under a stationary
        // cursor after we return; force it back onto the page we actually opened.
        m_syncingContents = true;
        pinTarget();
        m_syncingContents = false;
    });
    m_syncingContents = false;
}

void HelpWindow::onContentsSelectionChanged() {
    if (m_syncingContents) {
        return;
    }
    QTreeWidgetItem* item = m_contents->currentItem();
    if (item == nullptr) {
        return;
    }
    openLocation({item->data(0, kFileRole).toString(), item->data(0, kAnchorRole).toString()}, true);
}

// --- Navigation --------------------------------------------------------------------------------

void HelpWindow::openLocation(const Location& where, bool remember) {
    if (where.file != m_currentFile) {
        m_manual.render(where.file, *m_browser->document());
        m_currentFile = where.file;
        highlightMatches();
    }
    if (where.anchor.isEmpty()) {
        m_browser->verticalScrollBar()->setValue(0);
        QTextCursor top(m_browser->document());
        m_browser->setTextCursor(top);
    } else {
        m_browser->scrollToAnchor(where.anchor);
    }
    if (remember) {
        // A new destination drops the forward branch, as a browser does.
        if (m_historyIndex >= 0 && m_historyIndex + 1 < static_cast<int>(m_history.size())) {
            m_history.resize(static_cast<std::size_t>(m_historyIndex + 1));
        }
        const bool same = !m_history.empty() && m_history.back().file == where.file &&
                          m_history.back().anchor == where.anchor;
        if (!same) {
            m_history.push_back(where);
            m_historyIndex = static_cast<int>(m_history.size()) - 1;
        }
    }
    syncContentsSelection(where);
    updateNavigationButtons();
}

void HelpWindow::goBack() {
    if (m_historyIndex > 0) {
        --m_historyIndex;
        openLocation(m_history[static_cast<std::size_t>(m_historyIndex)], false);
    }
}

void HelpWindow::goForward() {
    if (m_historyIndex + 1 < static_cast<int>(m_history.size())) {
        ++m_historyIndex;
        openLocation(m_history[static_cast<std::size_t>(m_historyIndex)], false);
    }
}

void HelpWindow::updateNavigationButtons() {
    m_back->setEnabled(m_historyIndex > 0);
    m_forward->setEnabled(m_historyIndex + 1 < static_cast<int>(m_history.size()));
}

void HelpWindow::onAnchorClicked(const QUrl& url) {
    const QString scheme = url.scheme();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
        return;
    }
    if (!scheme.isEmpty() && scheme != QLatin1String("qrc")) {
        return; // (nothing else is a page)
    }
    // A page link: "posing.md", "posing.md#joint-pins" or "#joint-pins" (this page). The
    // browser resolves relative links against qrc:/manual/, so take the file name alone.
    QString file = url.fileName();
    if (file.isEmpty()) {
        file = m_currentFile;
    }
    openLocation({file, url.fragment()}, true);
}

// --- Search ------------------------------------------------------------------------------------

void HelpWindow::applySearchFilter() {
    const QString query = m_search->text().trimmed().toLower();
    if (query.isEmpty()) {
        for (QTreeWidgetItemIterator it(m_contents); *it != nullptr; ++it) {
            (*it)->setHidden(false);
        }
        m_contents->collapseAll();
        if (m_currentFile.isEmpty()) {
            return;
        }
        // (back to the page the reader is on)
        syncContentsSelection({m_currentFile, QString()});
        return;
    }
    // A page is shown when its title or its text mentions the words; a section only when its own
    // title does (the page's text match is the page's). Ancestors of anything shown are shown,
    // and a page opens only when one of its sections is.
    QHash<QString, bool> pageMatches;
    for (QTreeWidgetItemIterator it(m_contents); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        const QString file = item->data(0, kFileRole).toString();
        if (!pageMatches.contains(file)) {
            pageMatches.insert(file, m_manual.searchTextOf(file).contains(query));
        }
        const bool isPage = item->data(0, kAnchorRole).toString().isEmpty();
        const bool titleMatches = item->text(0).toLower().contains(query);
        item->setHidden(!(titleMatches || (isPage && pageMatches.value(file))));
        item->setExpanded(false);
    }
    for (QTreeWidgetItemIterator it(m_contents); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        if (item->isHidden()) {
            continue;
        }
        for (QTreeWidgetItem* parent = item->parent(); parent != nullptr; parent = parent->parent()) {
            parent->setHidden(false);
            parent->setExpanded(true);
        }
    }
}

void HelpWindow::highlightMatches() {
    const QString query = m_search->text().trimmed();
    QList<QTextEdit::ExtraSelection> selections;
    if (!query.isEmpty()) {
        QTextCharFormat mark;
        mark.setBackground(QColor(91, 135, 204, 110)); // the accent, translucent
        QTextDocument* document = m_browser->document();
        QTextCursor cursor(document);
        while (true) {
            cursor = document->find(query, cursor);
            if (cursor.isNull()) {
                break;
            }
            QTextEdit::ExtraSelection selection;
            selection.cursor = cursor;
            selection.format = mark;
            selections.append(selection);
        }
    }
    m_browser->setExtraSelections(selections);
}

void HelpWindow::findNextMatch(bool backward) {
    const QString query = m_search->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    const QTextDocument::FindFlags flags = backward ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    if (m_browser->find(query, flags)) {
        return;
    }
    // Wrap around.
    QTextCursor cursor(m_browser->document());
    if (backward) {
        cursor.movePosition(QTextCursor::End);
    }
    m_browser->setTextCursor(cursor);
    m_browser->find(query, flags);
}
