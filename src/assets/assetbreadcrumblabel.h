// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetbreadcrumblabel.h
 * @brief The title above the asset grid: a clickable breadcrumb for physical folders, a plain
 *        name for virtual sources (Favorites, Collections).
 *
 * For a physical folder the label resolves which library root the folder lives under and
 * renders "Library / a / b / current" as rich text where every ancestor is a real link
 * (clicking navigates there) and the current folder is bright and inert. The structure
 * (library root + name, segments, each segment's full path) is resolved ONCE in setFolder and
 * cached; a resize only re-lays it out, fitting as many TRAILING segments as the width allows
 * and collapsing the rest into a non-clickable "...". Hovering a link underlines it (the
 * label re-renders its HTML on linkHovered). Right-clicking offers Open/Browse for the hovered
 * ancestor (or the current folder), suppressing Qt's built-in "Copy Link" menu; virtual titles
 * get no menu. The label emits requests rather than acting: the owner decides what "navigate"
 * means (deselecting the tree, displaying the folder).
 */

#ifndef ASSETBREADCRUMBLABEL_H
#define ASSETBREADCRUMBLABEL_H

#include <QIcon>
#include <QLabel>
#include <QString>
#include <QStringList>

class QContextMenuEvent;
class QResizeEvent;

/**
 * @class AssetBreadcrumbLabel
 * @brief The grid's title label: breadcrumb for folders, plain title for virtual sources.
 */
class AssetBreadcrumbLabel : public QLabel {
    Q_OBJECT
public:
    explicit AssetBreadcrumbLabel(QWidget* parent = nullptr);

    /// Shows a breadcrumb for the physical folder `folderPath`. `libraryRoots` are the enabled
    /// library paths (display order); the first one containing the folder becomes the crumb's
    /// root. A folder under no library shows just its own name.
    void setFolder(const QString& folderPath, const QStringList& libraryRoots);

    /// Shows `text` as a plain, non-clickable title (Favorites, a Collection's name).
    void setPlainTitle(const QString& text);

    /// Back to the idle prompt ("Select a folder to view assets..."), no menu, no links.
    void clearTitle();

signals:
    /// The user clicked a breadcrumb link or picked "Open" on one: show `folderPath`.
    void navigateRequested(const QString& folderPath);
    /// The user picked "Browse Folder": open `folderPath` in the OS file browser.
    void browseRequested(const QString& folderPath);

protected:
    /// Re-fits the breadcrumb to the new width (no recomputation, just re-layout).
    void resizeEvent(QResizeEvent* event) override;
    /// Open/Browse menu for the hovered ancestor link (or the current folder when right-clicking
    /// the bright, non-link current segment). Always consumed: Qt's "Copy Link" menu never shows.
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void relayout();                                  ///< Re-renders the HTML for the current width/hover state.
    QString buildBreadcrumbHtml(int availableWidth) const;

    QString m_folderPath;          ///< Current physical folder (breadcrumb mode); empty otherwise
    QString m_plainTitle;          ///< Current plain title (virtual mode); empty otherwise
    QString m_hoveredLink;         ///< Breadcrumb link path under the cursor, for the underline + menu target

    // Cached breadcrumb structure for the current physical folder, rebuilt on each setFolder()
    // call and re-laid-out (without recomputation) whenever the label is resized.
    QString m_libRoot;
    QString m_libName;
    QStringList m_segments;
    QStringList m_segmentPaths;

    const QIcon m_openIcon;
    const QIcon m_browseIcon;
};

#endif // ASSETBREADCRUMBLABEL_H
