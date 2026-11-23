// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetbreadcrumblabel.cpp
 * @brief Implementation of AssetBreadcrumbLabel. See the header for the design.
 */

#include "assetbreadcrumblabel.h"
#include "assetscan.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QFontMetrics>
#include <QMenu>
#include <QResizeEvent>

AssetBreadcrumbLabel::AssetBreadcrumbLabel(QWidget* parent)
    : QLabel(tr("Select a folder to view assets..."), parent)
    , m_openIcon(QStringLiteral(":/resources/icons/open-item.png"))
    , m_browseIcon(QStringLiteral(":/resources/icons/browse-folder.png")) {
    setObjectName("AssetManagerTitle");
    setTextFormat(Qt::RichText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(this, &QLabel::linkActivated, this, [this](const QString& link) {
        emit navigateRequested(link);
    });
    connect(this, &QLabel::linkHovered, this, [this](const QString& link) {
        m_hoveredLink = link;
        relayout();
    });
}

void AssetBreadcrumbLabel::setFolder(const QString& folderPath, const QStringList& libraryRoots) {
    m_folderPath = folderPath;
    m_plainTitle.clear();
    m_hoveredLink.clear(); // stale hover from the previous folder shouldn't carry over

    m_libRoot.clear();
    m_libName.clear();
    m_segments.clear();
    m_segmentPaths.clear();

    for (const QString& lib : libraryRoots) {
        if (AssetScan::folderWithinLibrary(folderPath, lib)) {
            m_libRoot = lib;
            break;
        }
    }

    if (m_libRoot.isEmpty()) {
        m_libName = AssetScan::folderDisplayName(folderPath);
    } else {
        m_libName = AssetScan::folderDisplayName(m_libRoot);
        const QString relPath = QDir(m_libRoot).relativeFilePath(folderPath);
        m_segments = (relPath == "." || relPath.isEmpty()) ? QStringList() : relPath.split('/');

        QString cumPath = m_libRoot;
        m_segmentPaths.reserve(m_segments.size());
        for (const QString& seg : m_segments) {
            cumPath = QDir::cleanPath(cumPath + "/" + seg);
            m_segmentPaths.append(cumPath);
        }
    }

    relayout();
}

void AssetBreadcrumbLabel::setPlainTitle(const QString& text) {
    m_folderPath.clear();
    m_plainTitle = text;
    m_hoveredLink.clear();
    relayout();
}

void AssetBreadcrumbLabel::clearTitle() {
    m_folderPath.clear();
    m_plainTitle.clear();
    m_hoveredLink.clear();
    setText(tr("Select a folder to view assets..."));
}

void AssetBreadcrumbLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    relayout();
}

void AssetBreadcrumbLabel::contextMenuEvent(QContextMenuEvent* event) {
    // Right-click targets whichever ancestor link is currently hovered, falling back to the
    // current folder when right-clicking the bright, non-link current-folder segment. Only
    // physical folders get an Open/Browse menu, never the virtual Collections/Favorites titles.
    event->accept(); // always suppress Qt's built-in "Copy Link" menu
    if (m_folderPath.isEmpty()) return;
    const QString targetPath = !m_hoveredLink.isEmpty() ? m_hoveredLink : m_folderPath;

    QMenu menu(this);
    menu.setObjectName("AssetManagerContextMenu");
    QAction* openAction = menu.addAction(m_openIcon, tr("Open"));
    openAction->setEnabled(!m_hoveredLink.isEmpty());
    menu.addSeparator();
    QAction* browseAction = menu.addAction(m_browseIcon, tr("Browse Folder"));
    QAction* selected = menu.exec(event->globalPos());
    if (selected == openAction) emit navigateRequested(targetPath);
    else if (selected == browseAction) emit browseRequested(targetPath);
}

/**
 * @brief Rebuilds the label text from the cached breadcrumb/title state. Called whenever the
 *        folder changes, a link's hover state changes, or the label is resized (splitter drag).
 */
void AssetBreadcrumbLabel::relayout() {
    if (m_folderPath.isEmpty() && m_plainTitle.isEmpty()) return;

    // Reserve room for the leading "&nbsp;" and the QSS padding on #AssetManagerTitle.
    constexpr int kMargin = 30;
    const int availableWidth = qMax(0, width() - kMargin);

    QString html;
    if (!m_plainTitle.isEmpty()) {
        QFont pathFont = font();
        pathFont.setBold(true);
        pathFont.setPixelSize(16);
        const QFontMetrics pathFm(pathFont);
        const QString elided = pathFm.elidedText(m_plainTitle, Qt::ElideLeft, availableWidth);
        html = QString("&nbsp;<span style='font-size:16px; font-weight:bold;'>%1</span>")
                    .arg(elided.toHtmlEscaped());
    } else {
        html = buildBreadcrumbHtml(availableWidth);
    }

    setText(html);
}

/**
 * @brief Builds the breadcrumb's HTML, fitting as many TRAILING segments as actually fit
 *        within availableWidth and rendering them as real clickable links. Whatever doesn't
 *        fit (the leading library name and/or earliest ancestors) collapses into a
 *        non-clickable "...". The current (last) segment is always shown, bright, non-link.
 */
QString AssetBreadcrumbLabel::buildBreadcrumbHtml(int availableWidth) const {
    QFont linkFont = font();
    linkFont.setBold(true);
    linkFont.setPixelSize(14);
    const QFontMetrics linkFm(linkFont);

    QFont curFont = font();
    curFont.setBold(true);
    curFont.setPixelSize(16);
    const QFontMetrics curFm(curFont);

    auto linkStyle = [&](const QString& path) -> QString {
        const bool hovered = (!m_hoveredLink.isEmpty() && path == m_hoveredLink);
        return hovered
            ? "font-size:14px; font-weight:bold; color:#aaaaaa; text-decoration:underline;"
            : "font-size:14px; font-weight:bold; color:#6e6e6e; text-decoration:none;";
    };
    const QString greySep = "<span style='color:#6e6e6e; font-weight:bold;'> / </span>";
    const int sepWidth = linkFm.horizontalAdvance(QStringLiteral(" / "));

    // No segments: the library root itself is the current folder.
    if (m_segments.isEmpty()) {
        const QString elidedName = curFm.elidedText(m_libName, Qt::ElideLeft, availableWidth);
        return QString("&nbsp;<span style='font-size:16px; font-weight:bold; color:#cccccc;'>%1</span>")
                    .arg(elidedName.toHtmlEscaped());
    }

    // The last segment (current folder) is always shown in full, bright, non-link.
    const QString& lastSeg = m_segments.last();
    const QString elidedLast = curFm.elidedText(lastSeg, Qt::ElideRight, qMax(availableWidth, 0));
    int usedWidth = curFm.horizontalAdvance(elidedLast);

    // Walk backward through the remaining segments, keeping as many as fit.
    int startIdx = m_segments.size() - 1;
    QList<int> shownIndices;
    for (int i = m_segments.size() - 2; i >= 0; --i) {
        const int segW = linkFm.horizontalAdvance(m_segments[i]) + sepWidth;
        if (usedWidth + segW > availableWidth) break;
        usedWidth += segW;
        shownIndices.prepend(i);
        startIdx = i;
    }

    // If every segment fit, see if the library name itself also fits.
    bool showLibName = false;
    if (startIdx == 0) {
        const int libW = linkFm.horizontalAdvance(m_libName) + sepWidth;
        if (usedWidth + libW <= availableWidth) showLibName = true;
    }

    // href attributes are DOUBLE-quoted deliberately: toHtmlEscaped() escapes `"` but not `'`,
    // so a single-quoted attribute truncates at the first apostrophe in a path ("D:/John's
    // Models" -> navigating to the nonexistent "D:/John").
    QString html = "&nbsp;";
    if (showLibName) {
        html += QString("<a href=\"%1\" style='%2'>%3</a>")
                    .arg(m_libRoot.toHtmlEscaped(), linkStyle(m_libRoot), m_libName.toHtmlEscaped());
    } else {
        html += "<span style='font-size:14px; font-weight:bold; color:#6e6e6e;'>...</span>";
    }

    for (int idx : shownIndices) {
        html += greySep;
        html += QString("<a href=\"%1\" style='%2'>%3</a>")
                    .arg(m_segmentPaths[idx].toHtmlEscaped(), linkStyle(m_segmentPaths[idx]),
                         m_segments[idx].toHtmlEscaped());
    }

    html += greySep;
    html += QString("<span style='font-size:16px; font-weight:bold; color:#cccccc;'>%1</span>")
                .arg(elidedLast.toHtmlEscaped());

    return html;
}
