// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file hdriselector.cpp
 * @brief Implementation of HdriSelector. See hdriselector.h.
 */

#include "hdriselector.h"

#include "constants.h"
#include "librarypaths.h"
#include "propertyrows.h"

#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <utility>

namespace pose {

namespace {
// Extensions a panorama's menu thumbnail may use: a same-basename image file next to the .hdr/.exr
// (the stock set ships .jpg previews — never rely on .webp alone, which Qt only decodes with the
// optional "Qt Image Formats" add-on installed; users can pair any of these). Same pairing rule as
// the Asset Manager's thumbnail matching, first hit wins.
constexpr const char* kIconExtensions[] = {"webp", "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff"};

// Thumbnail size (2:1 like the equirect panoramas) and how many rows the drop-down shows before
// scrolling. The list inside the menu is a real QListWidget, so beyond kHdriVisibleRows the rest
// of the collection is reached with an ordinary scroll bar.
constexpr int kHdriIconW = 178;
constexpr int kHdriIconH = 89;
constexpr int kHdriVisibleRows = 6;

// Category-heading geometry (see HdriHeadingDelegate): the gap above the text separating a section
// from the previous rows, the gap between the text and the rule under it, breathing room between
// the rule and the section's first thumbnail, and the side inset (matching #HdriList::item's 6px
// horizontal padding so the heading aligns with the row content).
constexpr int kHeadingTopGap = 24;
constexpr int kHeadingRuleGap = 3;
constexpr int kHeadingBottomPad = 3;
constexpr int kHeadingInset = 6;

// Paints #HdriList rows. Panorama rows defer to the default (QSS-styled) item painting — hover and
// selection keep their _environment.qss look. Category headings (rows carrying no UserRole path)
// get section chrome QSS can't express for individual items: a gap above separating the section
// from the previous rows, the heading text in the same bright color as the file names, and a
// hairline rule directly under the text.
class HdriHeadingDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static bool isHeading(const QModelIndex& index) {
        return index.data(Qt::UserRole).toString().isEmpty();
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (!isHeading(index)) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index); // pulls the item's bold font + display text
        const QFontMetrics fm(opt.font);
        const QRect r = opt.rect.adjusted(kHeadingInset, 0, -kHeadingInset, 0);
        const int textTop = opt.rect.y() + kHeadingTopGap;
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(QColor(0xe0, 0xe0, 0xe0)); // as bright as the file names (#HdriList color)
        painter->drawText(QRect(r.x(), textTop, r.width(), fm.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, opt.text);
        const int ruleY = textTop + fm.height() + kHeadingRuleGap;
        painter->setPen(QColor(QLatin1String(Constants::COLOR_SEPARATOR))); // the app-wide divider grey
        painter->drawLine(r.left(), ruleY, r.right(), ruleY);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const QSize base = QStyledItemDelegate::sizeHint(option, index);
        if (!isHeading(index)) {
            return base;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        return QSize(base.width(), kHeadingTopGap + QFontMetrics(opt.font).height() +
                                       kHeadingRuleGap + 1 + kHeadingBottomPad);
    }
};
} // namespace

HdriSelector::HdriSelector(QWidget* parent) : QPushButton(parent) {
    // The side panel's picker field: the object name the style sheet styles, and the spacer icon
    // that insets the environment's name (propertyrows.h says why an icon and not QSS padding).
    propertyrows::stylePickerField(this);
    m_menu = new QMenu(this); // inherits the app-wide QMenu styling like every other menu
    setMenu(m_menu);
    // Rebuild on every open so .hdr files added/removed while the app runs show up immediately.
    connect(m_menu, &QMenu::aboutToShow, this, &HdriSelector::rebuildMenu);

    // The one startup-time guarantee that "drop files here" always has a target: create the
    // hdri/ folder now (a plain path lookup afterwards — see LibraryPaths::ensureHdriDirectory).
    LibraryPaths::ensureHdriDirectory();

    // Initial selection mirrors the viewport's startup default without firing a redundant re-load —
    // the viewport already loads it itself. Both sides ask LibraryPaths::defaultHdri() (which also
    // honours the <appDir>/environment.hdr|exr override), so the caption and what the viewport
    // actually loaded come from one resolution.
    setCurrentPath(LibraryPaths::defaultHdri());
}

void HdriSelector::setCurrentPath(const QString& path) {
    m_currentPath = path;
    if (!path.isEmpty()) {
        setText(QFileInfo(path).completeBaseName());
    } else {
        setText(tr("Procedural Studio")); // no panoramas yet — the built-in fallback
    }
}

void HdriSelector::restoreDefault() {
    const QString def = LibraryPaths::defaultHdri();
    if (def.isEmpty() || def.compare(m_currentPath, Qt::CaseInsensitive) == 0) {
        return; // no panoramas at all, or already on the default
    }
    choose(def, QFileInfo(def).completeBaseName());
}

void HdriSelector::choose(const QString& path, const QString& name) {
    m_currentPath = path;
    setText(name);
    emit environmentChosen(path);
}

// The menu thumbnail for @p baseName: the first same-basename image file found in @p dir, decoded
// at thumbnail size (the codec does the reduction — same trick as the Asset Manager grid). A
// panorama with no paired image simply gets no icon.
//
// Results are cached across menu opens, keyed on (path, mtime): the menu is rebuilt on every
// aboutToShow (so files added while the app runs appear), and re-decoding every preview
// synchronously on the GUI thread made each open visibly lag once a collection grew. A changed
// mtime re-decodes; an unchanged file is a hash lookup.
QIcon HdriSelector::thumbnailFor(const QDir& dir, const QString& baseName, IconCache& fresh) {
    for (const char* ext : kIconExtensions) {
        const QString candidate = dir.filePath(baseName + QLatin1Char('.') + QLatin1String(ext));
        const QFileInfo info(candidate);
        if (!info.exists()) {
            continue;
        }
        const QDateTime mtime = info.lastModified();
        if (const auto it = m_iconCache.constFind(candidate);
            it != m_iconCache.cend() && it->mtime == mtime) {
            fresh.insert(candidate, *it); // still listed: keep it through this rebuild
            if (it->icon.isNull()) {
                continue; // known-undecodable at this mtime: try the next extension
            }
            return it->icon;
        }
        QImageReader reader(candidate);
        reader.setAutoTransform(true);
        const QSize orig = reader.size();
        if (orig.isValid()) {
            reader.setScaledSize(orig.scaled(kHdriIconW, kHdriIconH, Qt::KeepAspectRatio));
        }
        const QImage img = reader.read();
        if (!img.isNull()) {
            const QIcon icon(QPixmap::fromImage(img));
            fresh.insert(candidate, {mtime, icon});
            return icon;
        }
        // Undecodable (e.g. .webp on a Qt install without the optional Image Formats add-on):
        // remember that verdict too, then try the next extension rather than giving up.
        fresh.insert(candidate, {mtime, QIcon()});
    }
    return QIcon();
}

void HdriSelector::rebuildMenu() {
    m_menu->clear(); // deletes the previous QWidgetAction and its hosted widgets

    // Recursive and pre-sorted: uncategorized root files first, then each subfolder (= category)
    // alphabetically — the headings below rely on same-category files arriving contiguously. The
    // category and display name come precomputed with the path (LibraryPaths::hdriEntries).
    const QList<LibraryPaths::HdriEntry> entries = LibraryPaths::hdriEntries();

    // The menu hosts one QWidgetAction whose widget is a scrollable thumbnail list plus a footer
    // link — a plain QMenu can't show a real scroll bar or cap itself at N rows, a QListWidget can.
    auto* container = new QWidget(m_menu);
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    if (!entries.isEmpty()) {
        auto* list = new QListWidget(container);
        list->setObjectName(QStringLiteral("HdriList"));
        list->setIconSize(QSize(kHdriIconW, kHdriIconH));
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        // (No setUniformItemSizes: category headings are text-height rows, panoramas thumbnail-height.)
        list->setFrameShape(QFrame::NoFrame);
        list->setMouseTracking(true); // menu-like hover highlight (see _environment.qss)
        list->setItemDelegate(new HdriHeadingDelegate(list)); // heading chrome; file rows unchanged

        IconCache fresh; // becomes the cache once the rebuild has touched every listed file
        QString lastCategory; // uncategorized root files (empty category) get no heading
        for (const LibraryPaths::HdriEntry& entry : entries) {
            const QString& category = entry.category; // "" for root files
            if (!category.isEmpty() && category.compare(lastCategory, Qt::CaseInsensitive) != 0) {
                // Category heading: the subfolder's name as a disabled row — no hover, no
                // selection, itemClicked never fires. HdriHeadingDelegate paints its section
                // chrome (top gap + bright text + rule) and supplies its height; the bold font
                // set here feeds both.
                auto* header = new QListWidgetItem(category);
                header->setFlags(Qt::NoItemFlags);
                QFont headerFont = list->font();
                headerFont.setBold(true);
                header->setFont(headerFont);
                list->addItem(header);
            }
            lastCategory = category;

            // Thumbnails pair with the panorama in its own (sub)folder, same rule as before.
            const QDir folder(QFileInfo(entry.path).absolutePath());
            auto* item = new QListWidgetItem(thumbnailFor(folder, entry.name, fresh), entry.name);
            item->setData(Qt::UserRole, entry.path);
            list->addItem(item);
            if (entry.path.compare(m_currentPath, Qt::CaseInsensitive) == 0) {
                list->setCurrentItem(item); // highlight the active environment
            }
        }
        m_iconCache = std::move(fresh);

        // Cap the visible span at kHdriVisibleRows thumbnail rows (headings spend from the same
        // budget); the scroll bar covers the rest. Rows differ in height now, so sum the real rows
        // for the fits-entirely case and measure an actual panorama row for the cap. Width fits
        // thumbnail + name + the scroll bar.
        int contentHeight = 0;
        for (int i = 0; i < list->count(); ++i) {
            contentHeight += list->sizeHintForRow(i);
        }
        int thumbRowHeight = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->data(Qt::UserRole).toString().isEmpty()) {
                thumbRowHeight = list->sizeHintForRow(i); // first panorama row (entries is non-empty)
                break;
            }
        }
        list->setFixedHeight(std::min(contentHeight, kHdriVisibleRows * thumbRowHeight) + 4);
        const int scrollBarW =
            list->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, list);
        list->setFixedWidth(list->sizeHintForColumn(0) + scrollBarW + 8);
        if (list->currentItem() != nullptr) {
            list->scrollToItem(list->currentItem(), QAbstractItemView::PositionAtCenter);
        }

        connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            const QString path = item->data(Qt::UserRole).toString();
            if (path.isEmpty()) {
                return; // a category heading (disabled anyway — belt and braces)
            }
            // The entry that is already the environment only closes the menu (restoreDefault's
            // rule): choosing it again would bake the same panorama again, re-aim the key light
            // over an azimuth and elevation the user may have dialled by hand since, and mark
            // the project changed — for a click that changed nothing.
            if (path.compare(m_currentPath, Qt::CaseInsensitive) != 0) {
                choose(path, item->text());
            }
            m_menu->close();
        });
        column->addWidget(list);
    } else {
        m_iconCache.clear(); // nothing listed: nothing worth remembering
        auto* none = new QLabel(tr("No .hdr or .exr files found"), container);
        none->setObjectName(QStringLiteral("HdriEmptyLabel"));
        none->setContentsMargins(8, 4, 8, 4);
        column->addWidget(none);
    }

    // Footer: take the user straight to where panoramas live, so adding one is a drop away.
    auto* open = new QPushButton(tr("Open HDRI Folder…"), container);
    open->setObjectName(QStringLiteral("HdriFolderLink"));
    open->setCursor(Qt::PointingHandCursor);
    open->setFlat(true);
    connect(open, &QPushButton::clicked, this, [this]() {
        // Created on demand: the folder may have been deleted since startup, and a link to a
        // missing folder would just fail silently in the file manager.
        QDesktopServices::openUrl(QUrl::fromLocalFile(LibraryPaths::ensureHdriDirectory()));
        m_menu->close();
    });
    column->addWidget(open);

    auto* hostAction = new QWidgetAction(m_menu);
    hostAction->setDefaultWidget(container);
    m_menu->addAction(hostAction);
}

} // namespace pose
