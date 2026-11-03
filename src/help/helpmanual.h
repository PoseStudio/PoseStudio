/**
 * @file helpmanual.h
 * @brief The user manual's CONTENT model: the table of contents (docs/manual/manual.json), the
 *        Markdown pages embedded under qrc:/manual/, and the styling that turns a page into a
 *        QTextDocument the HelpWindow shows.
 *
 * The manual is written as plain Markdown in docs/manual/ (readable on GitHub as it is) and
 * embedded into the executable at build time — CMake globs the folder, so a new page needs only
 * the .md file and a line in manual.json. Each page's `##` headings become its sub-entries in
 * the table of contents automatically, and every heading gets an anchor (its GitHub-style slug),
 * so a page links to another's section as `posing.md#joint-pins`. CHANGELOG.md is embedded as
 * `changelog.md` and shown as the "What's New" page, so the release notes are never a second
 * copy to maintain. tools/manual/check_manual.py checks the manifest, the links and the
 * coverage of the menus' actions.
 */

#ifndef HELP_MANUAL_H
#define HELP_MANUAL_H

#include <QString>
#include <QStringList>
#include <QHash>

#include <vector>

class QTextDocument;

/// One entry of the manual's table of contents: a page (a Markdown file) with optional nested
/// pages. Sub-sections are not listed here — they are the page's own `##` headings.
struct ManualPage {
    QString id;      ///< A stable identifier (the manifest's `id`; used by HelpWindow::showPage).
    QString title;   ///< The title shown in the table of contents.
    QString file;    ///< The page's file name under qrc:/manual/ (e.g. "posing.md").
    std::vector<ManualPage> children;
};

/// A heading found in a page: its level (1 = `#`), its text and the anchor it can be linked by.
struct ManualHeading {
    int     level = 1;
    QString text;
    QString slug;
};

/**
 * @class HelpManual
 * @brief Loads the manifest and the pages, and prepares a page for display.
 */
class HelpManual {
public:
    /// Reads qrc:/manual/manual.json. False (with a message in errorText()) when the manifest is
    /// missing or malformed — the window then shows the message instead of a table of contents.
    bool load();
    const QString& errorText() const { return m_error; }

    /// The top-level pages, in manifest order.
    const std::vector<ManualPage>& pages() const { return m_pages; }
    /// The page with this id, or null. Ids are unique across the whole tree.
    const ManualPage* pageById(const QString& id) const;
    /// The page with this file name, or null.
    const ManualPage* pageByFile(const QString& file) const;
    /// The first page (the manual's landing page), or null when the manifest is empty.
    const ManualPage* firstPage() const { return m_pages.empty() ? nullptr : &m_pages.front(); }

    /// The raw Markdown of a page file (cached), or an empty string when it is not embedded.
    QString markdownOf(const QString& file);
    /// The page's headings, in order (ATX `#`..`######` lines outside fenced code blocks), each
    /// with the anchor slug the rendered document carries.
    std::vector<ManualHeading> headingsOf(const QString& file);
    /// The page as searchable plain text (lower-cased Markdown with the markup thinned out).
    QString searchTextOf(const QString& file);

    /// Renders a page's Markdown into @p document: parses it (GitHub dialect), then restyles it for
    /// the dark theme (heading colours, link colour, code backgrounds, table borders) and gives
    /// every heading an anchor named by its slug so `scrollToAnchor(slug)` works.
    void render(const QString& file, QTextDocument& document);
    /// Renders @p markdown itself the same way (the What's New window's notes: a changelog
    /// section under a title of its own).
    void renderMarkdown(const QString& markdown, QTextDocument& document) const;

    /// The GitHub-style anchor for a heading's text: lower-cased, punctuation dropped, spaces to
    /// hyphens ("Joint pins (P)" -> "joint-pins-p"). Duplicates within a page get "-2", "-3", …
    static QString slugFor(const QString& headingText);

private:
    static void collect(const ManualPage& page, std::vector<const ManualPage*>& out);
    void styleDocument(QTextDocument& document) const;

    std::vector<ManualPage>  m_pages;
    QString                  m_error;
    QHash<QString, QString>  m_markdownCache;
};

#endif // HELP_MANUAL_H
