/**
 * @file helpwindow.h
 * @brief The User Manual window (Help → User Manual, F1): a table of contents on the left, the
 *        page on the right, a search field, and back / forward through the pages visited.
 *
 * The window is UI only — what it shows comes from HelpManual (helpmanual.h): the manifest's
 * pages, each with its `##` headings as sub-entries, rendered from the Markdown embedded under
 * qrc:/manual/. Links between pages (`posing.md#joint-pins`) and to the web are handled here;
 * the search filters the table of contents to the pages that mention the words and highlights
 * them on the page shown (Enter steps through the matches).
 *
 * One window per application: open() shows it, raises it, and turns to a page when asked; it
 * is a top-level window owned by the main window, so it stays above the app while you work and
 * closes with it. The rest of the app never includes this header directly — MenuManager opens
 * it from the Help menu.
 */

#ifndef HELP_WINDOW_H
#define HELP_WINDOW_H

#include "helpmanual.h"

#include <QWidget>
#include <QString>

#include <vector>

class QLineEdit;
class QTextBrowser;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;

/**
 * @class HelpWindow
 * @brief The manual's window; see the file comment.
 */
class HelpWindow : public QWidget {
    Q_OBJECT

public:
    explicit HelpWindow(QWidget* owner);

    /// Shows the window (creating it on first use, owned by @p owner) and turns to the page with
    /// this id (a manifest `id`, e.g. "posing"), or leaves it where it was when @p pageId is empty.
    static void open(QWidget* owner, const QString& pageId = QString());

    /// Turns to the page with this id (see open()) — or, "id#heading-slug", to that section of
    /// it. A missing id is ignored.
    void showPage(const QString& pageId);
    /// Searches as if the words had been typed into the search field (the screenshot lever).
    void setSearchText(const QString& text);

private:
    /// Where the reader is: a page file and, optionally, the heading it scrolled to.
    struct Location {
        QString file;
        QString anchor;
    };

    void buildContents();
    QTreeWidgetItem* itemFor(const QString& file, const QString& anchor) const;
    void openLocation(const Location& where, bool remember);
    void syncContentsSelection(const Location& where);
    void goBack();
    void goForward();
    void updateNavigationButtons();
    void onAnchorClicked(const QUrl& url);
    void onContentsSelectionChanged();
    void applySearchFilter();
    void highlightMatches();
    void findNextMatch(bool backward);

    HelpManual            m_manual;
    QToolButton*          m_back = nullptr;
    QToolButton*          m_forward = nullptr;
    QLineEdit*            m_search = nullptr;
    QTimer*               m_searchDebounce = nullptr;
    QTreeWidget*          m_contents = nullptr;
    QTextBrowser*         m_browser = nullptr;
    std::vector<Location> m_history;
    int                   m_historyIndex = -1;
    QString               m_currentFile;
    bool                  m_syncingContents = false; ///< A programmatic selection, not the user's.
};

#endif // HELP_WINDOW_H
