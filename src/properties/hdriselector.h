// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file hdriselector.h
 * @brief The Environment tab's HDRI picker: a button that drops a scrollable thumbnail menu of
 *        every panorama in the user library's hdri/ tree.
 *
 * Split out of EnvironmentPanel so the panel stays a list of lighting dials and this widget owns
 * everything about choosing an environment file: the menu that is rebuilt on every open (files
 * the user adds/removes/refiles while the app runs show up immediately), the per-subfolder
 * category headings, the thumbnail decode cache, and the "which file is current" state.
 *
 * It is the app's button + QMenu picker pattern (see menupickerbutton.h for why not QComboBox),
 * but the menu's content is a QWidgetAction hosting a QListWidget (#HdriList) rather than plain
 * actions: a bare QMenu can neither cap itself at N rows nor show a real scroll bar. All of its
 * styling lives in _environment.qss under the object names set here (#EnvironmentHdriButton,
 * #HdriList, #HdriFolderLink, #HdriEmptyLabel); the category headings are painted by a delegate
 * in the .cpp because QSS cannot address individual list items.
 *
 * The widget IS the button (a QPushButton subclass) so the form row that hosts it, and the QSS
 * ID selector that styles it, see exactly the widget they always did.
 */

#ifndef HDRISELECTOR_H
#define HDRISELECTOR_H

#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QPushButton>
#include <QString>

class QDir;
class QMenu;

namespace pose {

/**
 * @class HdriSelector
 * @brief Button showing the current environment's name; its menu lists the library's panoramas
 *        with thumbnails, grouped by subfolder, and a footer link to the folder itself.
 */
class HdriSelector : public QPushButton {
    Q_OBJECT

public:
    explicit HdriSelector(QWidget* parent = nullptr);

    /// Mirrors an environment chosen elsewhere in the caption (basename of @p path, or the
    /// "Procedural Studio" label for an empty path). Emits nothing — this reflects state, it does
    /// not request a load.
    void setCurrentPath(const QString& path);

    /// Back to the stock/first panorama (LibraryPaths::defaultHdri(), the same resolution the
    /// viewport uses at startup). Skipped when already there or when the library holds no
    /// panoramas: switching HDRIs re-bakes the IBL, so a no-op click must not cost a bake.
    void restoreDefault();

signals:
    /// The user picked (or restored to) a panorama that differs from the current one.
    void environmentChosen(const QString& path);

private:
    /// (Re)fills the menu from the library's hdri/ tree, one heading per subfolder.
    void rebuildMenu();
    /// Makes @p path current, shows @p name, and emits environmentChosen.
    void choose(const QString& path, const QString& name);

    /// A decoded thumbnail, remembered with the mtime it was decoded from.
    struct CachedIcon {
        QDateTime mtime;
        QIcon     icon; ///< Null = known-undecodable at that mtime (try the next extension)
    };
    using IconCache = QHash<QString, CachedIcon>;

    /// The menu thumbnail for @p baseName in @p dir (see the .cpp). Hits and fresh decodes are
    /// written into @p fresh, which replaces the cache after a rebuild — so entries for files
    /// that vanished from the library are evicted instead of accumulating for the session.
    QIcon thumbnailFor(const QDir& dir, const QString& baseName, IconCache& fresh);

    QMenu*    m_menu = nullptr;
    QString   m_currentPath; // the panorama shown as selected ("": the procedural studio, no panoramas in the library)
    IconCache m_iconCache; // a member, not a function static: QIcon pixmaps must not outlive
                           // the QApplication (static destruction runs after the platform
                           // integration is gone)
};

} // namespace pose

#endif // HDRISELECTOR_H
