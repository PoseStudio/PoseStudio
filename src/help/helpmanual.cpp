/**
 * @file helpmanual.cpp
 * @brief Implements HelpManual: the manifest, the embedded pages, and the dark-theme styling of a
 *        rendered page (see helpmanual.h).
 */

#include "helpmanual.h"

#include "constants.h"

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTextTableCell>

namespace {

const QString kManualRoot = QStringLiteral(":/manual/");

/// The page palette. The body text and background come from the browser's QSS (_help.qss);
/// these are the parts a QTextDocument carries in its own formats.
const QColor kHeadingColor(QStringLiteral("#ffffff"));
const QColor kLinkColor(Constants::COLOR_ACCENT);
const QColor kCodeText(QStringLiteral("#e8e8e8"));
const QColor kCodeBackground(QStringLiteral("#2a2d30"));
const QColor kCodeBlockBackground(QStringLiteral("#141516"));
const QColor kQuoteText(QStringLiteral("#a8a8a8"));
const QColor kTableBorder(QStringLiteral("#3a3b3c"));
const QColor kTableHeaderBackground(QStringLiteral("#2a2d30"));

QString readResource(const QString& path) {
    QFile file(path);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

ManualPage parsePage(const QJsonObject& object) {
    ManualPage page;
    page.id = object.value(QStringLiteral("id")).toString();
    page.title = object.value(QStringLiteral("title")).toString();
    page.file = object.value(QStringLiteral("file")).toString();
    const QJsonArray children = object.value(QStringLiteral("children")).toArray();
    for (const QJsonValue& child : children) {
        if (child.isObject()) {
            page.children.push_back(parsePage(child.toObject()));
        }
    }
    return page;
}

/// True for a line that opens or closes a fenced code block (``` or ~~~).
bool isFence(const QString& line) {
    const QString trimmed = line.trimmed();
    return trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"));
}

} // namespace

bool HelpManual::load() {
    m_pages.clear();
    m_error.clear();
    const QString text = readResource(kManualRoot + QStringLiteral("manual.json"));
    if (text.isEmpty()) {
        m_error = QStringLiteral("The manual's table of contents (manual.json) is not embedded in this build.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QStringLiteral("The manual's table of contents (manual.json) could not be read: %1")
                      .arg(parseError.errorString());
        return false;
    }
    const QJsonArray pages = doc.object().value(QStringLiteral("pages")).toArray();
    for (const QJsonValue& value : pages) {
        if (value.isObject()) {
            m_pages.push_back(parsePage(value.toObject()));
        }
    }
    if (m_pages.empty()) {
        m_error = QStringLiteral("The manual's table of contents (manual.json) lists no pages.");
        return false;
    }
    return true;
}

void HelpManual::collect(const ManualPage& page, std::vector<const ManualPage*>& out) {
    out.push_back(&page);
    for (const ManualPage& child : page.children) {
        collect(child, out);
    }
}

const ManualPage* HelpManual::pageById(const QString& id) const {
    std::vector<const ManualPage*> all;
    for (const ManualPage& page : m_pages) {
        collect(page, all);
    }
    for (const ManualPage* page : all) {
        if (page->id == id) {
            return page;
        }
    }
    return nullptr;
}

const ManualPage* HelpManual::pageByFile(const QString& file) const {
    std::vector<const ManualPage*> all;
    for (const ManualPage& page : m_pages) {
        collect(page, all);
    }
    for (const ManualPage* page : all) {
        if (page->file == file) {
            return page;
        }
    }
    return nullptr;
}

QString HelpManual::markdownOf(const QString& file) {
    auto it = m_markdownCache.find(file);
    if (it != m_markdownCache.end()) {
        return it.value();
    }
    const QString text = readResource(kManualRoot + file);
    m_markdownCache.insert(file, text);
    return text;
}

QString HelpManual::slugFor(const QString& headingText) {
    QString slug;
    slug.reserve(headingText.size());
    for (const QChar c : headingText.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-')) {
            slug.append(c);
        } else if (c.isSpace()) {
            slug.append(QLatin1Char('-'));
        }
        // (any other punctuation is dropped, as GitHub drops it)
    }
    return slug;
}

std::vector<ManualHeading> HelpManual::headingsOf(const QString& file) {
    static const QRegularExpression kHeading(QStringLiteral("^(#{1,6})\\s+(.*?)\\s*#*\\s*$"));
    std::vector<ManualHeading> out;
    QHash<QString, int> seen;
    bool inFence = false;
    const QStringList lines = markdownOf(file).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (isFence(line)) {
            inFence = !inFence;
            continue;
        }
        if (inFence) {
            continue;
        }
        const QRegularExpressionMatch match = kHeading.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        ManualHeading heading;
        heading.level = static_cast<int>(match.captured(1).size());
        heading.text = match.captured(2);
        // (the text as the rendered block will carry it: inline code marks and emphasis dropped)
        heading.text.remove(QLatin1Char('`'));
        heading.text.remove(QLatin1Char('*'));
        QString slug = slugFor(heading.text);
        const int count = seen.value(slug, 0);
        seen.insert(slug, count + 1);
        if (count > 0) {
            slug += QStringLiteral("-%1").arg(count);
        }
        heading.slug = slug;
        out.push_back(heading);
    }
    return out;
}

QString HelpManual::searchTextOf(const QString& file) {
    static const QRegularExpression kImage(QStringLiteral("!\\[[^\\]]*\\]\\([^)]*\\)"));
    static const QRegularExpression kLink(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression kMarkup(QStringLiteral("[`*_#>|]"));
    QString text = markdownOf(file);
    text.remove(kImage);
    text.replace(kLink, QStringLiteral("\\1"));
    text.remove(kMarkup);
    return text.toLower();
}

void HelpManual::render(const QString& file, QTextDocument& document) {
    const QString markdown = markdownOf(file);
    renderMarkdown(markdown.isEmpty()
                       ? QStringLiteral("# Page not found\n\nThe page `%1` is not embedded in this build.").arg(file)
                       : markdown,
                   document);
}

void HelpManual::renderMarkdown(const QString& markdown, QTextDocument& document) const {
    document.clear();
    document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    styleDocument(document);
}

void HelpManual::styleDocument(QTextDocument& document) const {
    // Headings: white, an anchor named by the slug (in document order, duplicates numbered as
    // headingsOf numbers them), and a little air above.
    QHash<QString, int> seen;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat blockFormat = block.blockFormat();
        const int level = blockFormat.headingLevel();
        const bool codeBlock = blockFormat.hasProperty(QTextFormat::BlockCodeLanguage) ||
                               blockFormat.hasProperty(QTextFormat::BlockCodeFence) ||
                               blockFormat.nonBreakableLines();
        const bool quote = blockFormat.hasProperty(QTextFormat::BlockQuoteLevel) &&
                           blockFormat.intProperty(QTextFormat::BlockQuoteLevel) > 0;
        QTextCursor cursor(block);
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + std::max(0, block.length() - 1), QTextCursor::KeepAnchor);
        if (level > 0) {
            QString slug = slugFor(block.text());
            const int count = seen.value(slug, 0);
            seen.insert(slug, count + 1);
            if (count > 0) {
                slug += QStringLiteral("-%1").arg(count);
            }
            QTextCharFormat heading;
            heading.setAnchor(true);
            heading.setAnchorNames({slug});
            heading.setForeground(kHeadingColor);
            heading.setFontWeight(QFont::Bold);
            cursor.mergeCharFormat(heading);
            blockFormat.setTopMargin(level == 1 ? 6.0 : 18.0);
            blockFormat.setBottomMargin(6.0);
            cursor.mergeBlockFormat(blockFormat);
            continue;
        }
        if (codeBlock) {
            blockFormat.setBackground(kCodeBlockBackground);
            blockFormat.setLeftMargin(12.0);
            blockFormat.setRightMargin(12.0);
            blockFormat.setTopMargin(2.0);
            blockFormat.setBottomMargin(2.0);
            cursor.mergeBlockFormat(blockFormat);
            QTextCharFormat code;
            code.setForeground(kCodeText);
            cursor.mergeCharFormat(code);
            continue;
        }
        if (quote) {
            blockFormat.setLeftMargin(16.0);
            cursor.mergeBlockFormat(blockFormat);
            QTextCharFormat muted;
            muted.setForeground(kQuoteText);
            muted.setFontItalic(true);
            cursor.mergeCharFormat(muted);
        }
        // Inline: links in the accent color, inline code on a darker field.
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const QTextCharFormat format = fragment.charFormat();
            QTextCharFormat replacement;
            bool change = false;
            if (format.isAnchor() && !format.anchorHref().isEmpty()) {
                replacement.setForeground(kLinkColor);
                replacement.setFontUnderline(false);
                change = true;
            } else if (format.fontFixedPitch()) {
                replacement.setForeground(kCodeText);
                replacement.setBackground(kCodeBackground);
                change = true;
            }
            if (change) {
                QTextCursor span(&document);
                span.setPosition(fragment.position());
                span.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                span.mergeCharFormat(replacement);
            }
        }
    }
    // Tables: a visible border, padded cells, a shaded header row.
    std::vector<QTextFrame*> frames{document.rootFrame()};
    while (!frames.empty()) {
        QTextFrame* frame = frames.back();
        frames.pop_back();
        for (QTextFrame::iterator it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame* child = it.currentFrame()) {
                frames.push_back(child);
            }
        }
        auto* table = qobject_cast<QTextTable*>(frame);
        if (table == nullptr) {
            continue;
        }
        QTextTableFormat format = table->format();
        format.setBorder(1.0);
        format.setBorderBrush(kTableBorder);
        format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
        format.setBorderCollapse(true);
        format.setCellPadding(6.0);
        format.setCellSpacing(0.0);
        format.setTopMargin(6.0);
        format.setBottomMargin(10.0);
        table->setFormat(format);
        const int headerRows = table->format().headerRowCount();
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextCharFormat cellFormat = cell.format();
                cellFormat.setVerticalAlignment(QTextCharFormat::AlignTop);
                if (row < headerRows) {
                    cellFormat.setBackground(kTableHeaderBackground);
                    // (the text itself, not the cell: a cell's own format does not embolden it)
                    QTextCursor text = cell.firstCursorPosition();
                    text.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                    QTextCharFormat bold;
                    bold.setFontWeight(QFont::Bold);
                    text.mergeCharFormat(bold);
                }
                cell.setFormat(cellFormat);
            }
        }
    }
}
