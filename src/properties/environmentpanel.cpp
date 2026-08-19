/**
 * @file environmentpanel.cpp
 * @brief Implementation of EnvironmentPanel. See environmentpanel.h.
 */

#include "environmentpanel.h"

#include "dragnumberbox.h"
#include "librarypaths.h"
#include "viewportwidget.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace pose {

namespace {
// Builds a titled QGroupBox with a QFormLayout inside, appended to @p parent; returns the form.
QFormLayout* addGroup(QVBoxLayout* parent, const QString& title) {
    auto* box = new QGroupBox(title);
    box->setObjectName(QStringLiteral("EnvironmentGroup"));
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    parent->addWidget(box);
    return form;
}

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
        painter->setPen(QColor(0x4a, 0x4b, 0x4d)); // the app's border/hairline grey
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

// The small restore-default icon button that sits right of every Environment row (the dials and
// the HDRI selector). The caller wires clicked to the row's own restore behaviour.
QToolButton* makeRestoreButton() {
    auto* restore = new QToolButton;
    restore->setObjectName(QStringLiteral("EnvironmentRowRestore"));
    restore->setIcon(QIcon(QStringLiteral(":/resources/icons/default.png")));
    restore->setIconSize(QSize(14, 14));
    restore->setToolTip(QObject::tr("Restore default"));
    restore->setCursor(Qt::PointingHandCursor);
    restore->setAutoRaise(true);
    return restore;
}

// The menu thumbnail for @p baseName: the first same-basename image file found in @p dir, decoded
// at thumbnail size (the codec does the reduction — same trick as the Asset Manager grid). A
// panorama with no paired image simply gets no icon.
//
// Results are cached across menu opens, keyed on (path, mtime): the menu is rebuilt on every
// aboutToShow (so files added while the app runs appear), and re-decoding every preview
// synchronously on the GUI thread made each open visibly lag once a collection grew. A changed
// mtime re-decodes; an unchanged file is a hash lookup.
QIcon hdriIcon(const QDir& dir, const QString& baseName) {
    static QHash<QString, QPair<QDateTime, QIcon>> cache;
    for (const char* ext : kIconExtensions) {
        const QString candidate = dir.filePath(baseName + QLatin1Char('.') + QLatin1String(ext));
        const QFileInfo info(candidate);
        if (!info.exists()) {
            continue;
        }
        const QDateTime mtime = info.lastModified();
        if (const auto it = cache.constFind(candidate); it != cache.cend() && it->first == mtime) {
            if (it->second.isNull()) {
                continue; // known-undecodable at this mtime: try the next extension
            }
            return it->second;
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
            cache.insert(candidate, {mtime, icon});
            return icon;
        }
        // Undecodable (e.g. .webp on a Qt install without the optional Image Formats add-on):
        // remember that verdict too, then try the next extension rather than giving up.
        cache.insert(candidate, {mtime, QIcon()});
    }
    return QIcon();
}
} // namespace

EnvironmentPanel::EnvironmentPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("EnvironmentPanel"));
    buildUi();
    if (m_viewport) {
        // Undo/redo of a lighting edit applies renderer-side in the viewport; mirror it here so
        // the dials and checkbox track the restored state.
        connect(m_viewport, &ViewportWidget::lightingRestored, this,
                &EnvironmentPanel::applyRestoredSettings);
    }
}

DragNumberBox* EnvironmentPanel::addValueRow(QFormLayout* form, const QString& label, double min,
                                             double max, double step, double value) {
    auto* box = new DragNumberBox;
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(step >= 1.0 ? 0 : 2);
    box->setValue(value);
    // Undo bracketing: one entry per completed gesture (scrub or type-in), not one per increment
    // — the box brackets user-driven changes with these signals around its valueChanged stream.
    connect(box, &DragNumberBox::editingStarted, this, &EnvironmentPanel::beginSettingsEdit);
    connect(box, &DragNumberBox::editingFinished, this, &EnvironmentPanel::commitSettingsEdit);

    // Per-row restore: snaps just this dial back to its default. Rows are built while m_settings
    // is still default-constructed, so @p value IS the LightingSettings default — captured here.
    // setValue on a changed box fires valueChanged, which pushes the updated settings live exactly
    // like a hand edit (and no-ops when already at the default).
    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, this, [this, box, value]() {
        beginSettingsEdit();
        box->setValue(value);
        commitSettingsEdit(); // registers no entry if the dial was already at its default
    });

    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(box, 1);
    row->addWidget(restore, 0);
    form->addRow(label, field);
    return box;
}

void EnvironmentPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("EnvironmentContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(12);

    // --- Environment ---
    QFormLayout* env = addGroup(col, tr("Environment"));
    buildEnvironmentSelector(env);
    m_rotation = addValueRow(env, tr("Rotation°"), 0.0, 360.0, 1.0, m_settings.environmentRotationDeg);

    // --- Backdrop (the visible environment behind the figure; PBR mode only) ---
    QFormLayout* backdrop = addGroup(col, tr("Backdrop"));
    buildBackdropModeRow(backdrop);
    m_backdropBlur = addValueRow(backdrop, tr("Blur"), 0.0, 4.0, 0.05, m_settings.backdropBlur);
    m_backdropBrightness =
        addValueRow(backdrop, tr("Brightness"), 0.0, 2.0, 0.01, m_settings.backdropBrightness);
    m_domeRadius = addValueRow(backdrop, tr("Dome radius"), 2.0, 100.0, 0.5, m_settings.domeRadius);

    // --- Exposure & Tone ---
    QFormLayout* tone = addGroup(col, tr("Exposure && Tone")); // && — a lone & becomes an accelerator underline
    m_exposure = addValueRow(tone, tr("Exposure"), 0.0, 3.0, 0.01, m_settings.exposure);
    m_tonemap = new QCheckBox(tr("ACES filmic tonemap"));
    m_tonemap->setChecked(m_settings.tonemap);
    tone->addRow(QString(), m_tonemap);

    // --- Image-Based Lighting ---
    QFormLayout* ibl = addGroup(col, tr("Image-Based Lighting"));
    m_diffuse = addValueRow(ibl, tr("Diffuse"), 0.0, 3.0, 0.01, m_settings.diffuseIntensity);
    m_specular = addValueRow(ibl, tr("Specular"), 0.0, 2.0, 0.01, m_settings.specularIntensity);
    m_ambientFill = addValueRow(ibl, tr("Ambient fill"), 0.0, 1.0, 0.01, m_settings.ambientFill);

    // --- Key Light ---
    QFormLayout* key = addGroup(col, tr("Key Light"));
    m_keyIntensity = addValueRow(key, tr("Intensity"), 0.0, 5.0, 0.05, m_settings.keyIntensity);
    m_keyAzimuth = addValueRow(key, tr("Azimuth°"), -180.0, 180.0, 1.0, m_settings.keyAzimuthDeg);
    m_keyElevation = addValueRow(key, tr("Elevation°"), 0.0, 85.0, 1.0, m_settings.keyElevationDeg);

    // --- Shadow (the key light's shadows: figure self-shadowing + the PCSS ground shadow) ---
    QFormLayout* shadow = addGroup(col, tr("Shadow"));
    m_shadowsEnabled = new QCheckBox(tr("Enable shadows"));
    m_shadowsEnabled->setChecked(m_settings.shadowsEnabled);
    shadow->addRow(QString(), m_shadowsEnabled);
    m_shadowIntensity = addValueRow(shadow, tr("Intensity"), 0.0, 1.0, 0.01, m_settings.shadowIntensity);
    m_shadowSoftness = addValueRow(shadow, tr("Softness"), 0.0, 1.0, 0.01, m_settings.shadowSoftness);
    m_shadowReach = addValueRow(shadow, tr("Reach"), 1.0, 25.0, 0.5, m_settings.shadowReach);

    // --- Skin & Rim (PBR-mode accents) ---
    QFormLayout* accents = addGroup(col, tr("Skin && Rim"));
    m_subsurface = addValueRow(accents, tr("Subsurface"), 0.0, 1.0, 0.01, m_settings.subsurface);
    m_rim = addValueRow(accents, tr("Rim light"), 0.0, 1.5, 0.01, m_settings.rimIntensity);

    // --- Reset ---
    auto* reset = new QPushButton(tr("Restore All Defaults"));
    reset->setObjectName(QStringLiteral("EnvironmentResetButton"));
    col->addWidget(reset, 0, Qt::AlignLeft);

    col->addStretch(1);
    scroll->setWidget(content);

    // --- Wiring: every control updates m_settings and pushes it live to the viewport ---
    connect(m_rotation, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.environmentRotationDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_backdropBlur, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.backdropBlur = static_cast<float>(d);
        pushSettings();
    });
    connect(m_backdropBrightness, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.backdropBrightness = static_cast<float>(d);
        pushSettings();
    });
    connect(m_domeRadius, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.domeRadius = static_cast<float>(d);
        pushSettings();
    });
    connect(m_exposure, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.exposure = static_cast<float>(d);
        pushSettings();
    });
    connect(m_tonemap, &QCheckBox::toggled, this, [this](bool on) {
        beginSettingsEdit(); // a toggle is a complete gesture in itself
        m_settings.tonemap = on;
        pushSettings();
        commitSettingsEdit();
    });
    connect(m_diffuse, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.diffuseIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_specular, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.specularIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_ambientFill, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.ambientFill = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyIntensity, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyAzimuth, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyAzimuthDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_keyElevation, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.keyElevationDeg = static_cast<float>(d);
        pushSettings();
    });
    connect(m_shadowsEnabled, &QCheckBox::toggled, this, [this](bool on) {
        beginSettingsEdit(); // a toggle is a complete gesture in itself
        m_settings.shadowsEnabled = on;
        pushSettings();
        commitSettingsEdit();
    });
    connect(m_shadowIntensity, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.shadowIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(m_shadowSoftness, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.shadowSoftness = static_cast<float>(d);
        pushSettings();
    });
    connect(m_shadowReach, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.shadowReach = static_cast<float>(d);
        pushSettings();
    });
    connect(m_subsurface, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.subsurface = static_cast<float>(d);
        pushSettings();
    });
    connect(m_rim, &DragNumberBox::valueChanged, this, [this](double d) {
        m_settings.rimIntensity = static_cast<float>(d);
        pushSettings();
    });
    connect(reset, &QPushButton::clicked, this, &EnvironmentPanel::resetToDefaults);
}

void EnvironmentPanel::buildEnvironmentSelector(QFormLayout* form) {
    m_envButton = new QPushButton;
    m_envButton->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    m_envButton->setCursor(Qt::PointingHandCursor);
    // Qt's style engine ignores padding-left for a left-aligned QPushButton label (the text is laid
    // out from the border box regardless of the QSS padding), so a transparent spacer icon provides
    // the left inset for the environment name instead — icon + icon/text spacing ≈ a 16px indent.
    QPixmap spacer(10, 10);
    spacer.fill(Qt::transparent);
    m_envButton->setIcon(QIcon(spacer));
    m_envButton->setIconSize(QSize(10, 10));
    m_envMenu = new QMenu(m_envButton);
    // The object name keys AppProxyStyle's PM_SmallIconSize override (a larger icon column for the
    // thumbnails) and lets the menu inherit the app-wide QMenu styling like every other menu.
    m_envMenu->setObjectName(QStringLiteral("HdriMenu"));
    m_envButton->setMenu(m_envMenu);
    // Rebuild on every open so .hdr files added/removed while the app runs show up immediately.
    connect(m_envMenu, &QMenu::aboutToShow, this, &EnvironmentPanel::rebuildEnvironmentMenu);

    // Restore-default for the HDRI row: back to the stock/first panorama (the same resolution the
    // viewport uses at startup). Skipped when already there — switching HDRIs re-bakes the IBL,
    // so a no-op click shouldn't cost a bake. (Deliberately NOT on the undo stack, like any HDRI
    // switch.)
    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, this, [this]() {
        const QString def = LibraryPaths::defaultHdri();
        if (def.isEmpty() || def.compare(m_currentEnvPath, Qt::CaseInsensitive) == 0) {
            return; // no panoramas at all, or already on the default
        }
        chooseEnvironment(def, QFileInfo(def).completeBaseName());
    });

    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(m_envButton, 1);
    row->addWidget(restore, 0);
    form->addRow(tr("HDRI"), field);

    // Initial selection mirrors the viewport's startup default without firing a redundant re-load —
    // the viewport already loads it itself. Both sides ask LibraryPaths::defaultHdri(), so the
    // button text can't disagree with what VulkanWindow actually loaded.
    const QString initial = LibraryPaths::defaultHdri();
    if (!initial.isEmpty()) {
        m_currentEnvPath = initial;
        m_envButton->setText(QFileInfo(initial).completeBaseName());
    } else {
        m_envButton->setText(tr("Procedural Studio")); // no panoramas yet — the built-in fallback
    }
}

void EnvironmentPanel::rebuildEnvironmentMenu() {
    m_envMenu->clear(); // deletes the previous QWidgetAction and its hosted widgets

    const QDir root(LibraryPaths::hdriDirectory());
    // Recursive and pre-sorted: uncategorized root files first, then each subfolder (= category)
    // alphabetically — the headings below rely on same-category files arriving contiguously.
    const QStringList files = LibraryPaths::hdriFiles();

    // The menu hosts one QWidgetAction whose widget is a scrollable thumbnail list plus a footer
    // link — a plain QMenu can't show a real scroll bar or cap itself at N rows, a QListWidget can.
    auto* container = new QWidget(m_envMenu);
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    if (!files.isEmpty()) {
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

        QString lastCategory; // uncategorized root files (empty category) get no heading
        for (const QString& path : files) {
            const QString rel = root.relativeFilePath(path);
            const QString category = rel.section(QLatin1Char('/'), 0, -2); // "" for root files
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

            const QFileInfo info(path);
            const QString base = info.completeBaseName();
            // Thumbnails pair with the panorama in its own (sub)folder, same rule as before.
            auto* item = new QListWidgetItem(hdriIcon(QDir(info.absolutePath()), base), base);
            item->setData(Qt::UserRole, path);
            list->addItem(item);
            if (path.compare(m_currentEnvPath, Qt::CaseInsensitive) == 0) {
                list->setCurrentItem(item); // highlight the active environment
            }
        }

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
                thumbRowHeight = list->sizeHintForRow(i); // first panorama row (files is non-empty)
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
            chooseEnvironment(path, item->text());
            m_envMenu->close();
        });
        column->addWidget(list);
    } else {
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
        QDesktopServices::openUrl(QUrl::fromLocalFile(LibraryPaths::hdriDirectory()));
        m_envMenu->close();
    });
    column->addWidget(open);

    auto* hostAction = new QWidgetAction(m_envMenu);
    hostAction->setDefaultWidget(container);
    m_envMenu->addAction(hostAction);
}

void EnvironmentPanel::buildBackdropModeRow(QFormLayout* form) {
    m_backdropModeButton = new QPushButton;
    // Deliberately the HDRI selector's object name: the QSS ID selector styles every widget with
    // the name identically (nothing looks widgets up by it), so the mode button inherits that
    // field look for free — including the spacer-icon left-inset trick (see the note there).
    m_backdropModeButton->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    m_backdropModeButton->setCursor(Qt::PointingHandCursor);
    QPixmap spacer(10, 10);
    spacer.fill(Qt::transparent);
    m_backdropModeButton->setIcon(QIcon(spacer));
    m_backdropModeButton->setIconSize(QSize(10, 10));

    auto* menu = new QMenu(m_backdropModeButton);
    menu->setObjectName(QStringLiteral("HdriMenu")); // the app-wide menu look
    menu->setToolTipsVisible(true); // QMenu shows action tooltips only when opted in (Dome's below)
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    const QString names[3] = {tr("Off"), tr("Environment"), tr("Dome")};
    for (int i = 0; i < 3; ++i) {
        QAction* action = menu->addAction(names[i]);
        action->setCheckable(true);
        group->addAction(action);
        m_backdropModeActions[i] = action;
        connect(action, &QAction::triggered, this, [this, i]() { setBackdropMode(i); });
    }
    m_backdropModeActions[2]->setToolTip(
        tr("Ground-projected dome: the figure stands on the environment's floor"));
    m_backdropModeButton->setMenu(menu);
    syncBackdropModeUi();

    auto* restore = makeRestoreButton();
    connect(restore, &QToolButton::clicked, this,
            [this]() { setBackdropMode(LightingSettings().backdropMode); });

    auto* field = new QWidget;
    auto* row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(m_backdropModeButton, 1);
    row->addWidget(restore, 0);
    form->addRow(tr("Mode"), field);
}

void EnvironmentPanel::setBackdropMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    beginSettingsEdit(); // one undo entry per change (no-op entry when already at this mode)
    m_settings.backdropMode = mode;
    syncBackdropModeUi();
    pushSettings();
    commitSettingsEdit();
}

void EnvironmentPanel::syncBackdropModeUi() {
    const int mode = std::clamp(m_settings.backdropMode, 0, 2);
    const QString names[3] = {tr("Off"), tr("Environment"), tr("Dome")};
    if (m_backdropModeButton != nullptr) {
        m_backdropModeButton->setText(names[mode]);
    }
    for (int i = 0; i < 3; ++i) {
        if (m_backdropModeActions[i] != nullptr) {
            m_backdropModeActions[i]->setChecked(i == mode); // setChecked never re-fires triggered
        }
    }
}

void EnvironmentPanel::chooseEnvironment(const QString& path, const QString& name) {
    m_currentEnvPath = path;
    m_envButton->setText(name);
    if (m_viewport) {
        m_viewport->setEnvironment(path);
    }
}

void EnvironmentPanel::pushSettings() {
    if (m_viewport) {
        m_viewport->setLightingSettings(m_settings);
    }
}

void EnvironmentPanel::resetToDefaults() {
    const LightingSettings defaults;
    // One gesture, one undo entry: the depth counter folds the nested brackets (the checkbox's
    // toggled handler runs its own begin/commit) into this outermost pair.
    beginSettingsEdit();
    // Setting the widgets (unblocked) fires each control's handler, which rebuilds m_settings from the
    // defaults and pushes — so the viewport lands back on the default look.
    m_rotation->setValue(defaults.environmentRotationDeg);
    m_exposure->setValue(defaults.exposure);
    m_tonemap->setChecked(defaults.tonemap);
    m_diffuse->setValue(defaults.diffuseIntensity);
    m_specular->setValue(defaults.specularIntensity);
    m_ambientFill->setValue(defaults.ambientFill);
    m_keyIntensity->setValue(defaults.keyIntensity);
    m_keyAzimuth->setValue(defaults.keyAzimuthDeg);
    m_keyElevation->setValue(defaults.keyElevationDeg);
    m_shadowsEnabled->setChecked(defaults.shadowsEnabled);
    m_shadowIntensity->setValue(defaults.shadowIntensity);
    m_shadowSoftness->setValue(defaults.shadowSoftness);
    m_shadowReach->setValue(defaults.shadowReach);
    m_subsurface->setValue(defaults.subsurface);
    m_rim->setValue(defaults.rimIntensity);
    m_backdropBlur->setValue(defaults.backdropBlur);
    m_backdropBrightness->setValue(defaults.backdropBrightness);
    m_domeRadius->setValue(defaults.domeRadius);
    setBackdropMode(defaults.backdropMode); // nested begin/commit — folds into this one gesture
    pushSettings(); // covers the case where every widget was already at its default (no signals fired)
    commitSettingsEdit();
}

void EnvironmentPanel::beginSettingsEdit() {
    if (m_editDepth++ == 0) {
        m_preEditSettings = m_settings;
    }
}

void EnvironmentPanel::commitSettingsEdit() {
    if (m_editDepth == 0) {
        return; // unbalanced commit (shouldn't happen) — never underflow the depth
    }
    if (--m_editDepth == 0 && m_viewport && m_settings != m_preEditSettings) {
        m_viewport->registerLightingUndo(m_preEditSettings);
    }
}

void EnvironmentPanel::applyRestoredSettings(const LightingSettings& settings) {
    m_settings = settings;
    // Sync the widgets without firing their handlers: the viewport already applied these values,
    // and firing would re-enter the gesture machinery.
    const QSignalBlocker blockers[] = {
        QSignalBlocker(m_rotation),     QSignalBlocker(m_exposure),  QSignalBlocker(m_tonemap),
        QSignalBlocker(m_diffuse),      QSignalBlocker(m_specular),  QSignalBlocker(m_ambientFill),
        QSignalBlocker(m_keyIntensity), QSignalBlocker(m_keyAzimuth),
        QSignalBlocker(m_keyElevation), QSignalBlocker(m_subsurface), QSignalBlocker(m_rim),
        QSignalBlocker(m_backdropBlur), QSignalBlocker(m_backdropBrightness),
        QSignalBlocker(m_domeRadius),   QSignalBlocker(m_shadowsEnabled),
        QSignalBlocker(m_shadowIntensity), QSignalBlocker(m_shadowSoftness),
        QSignalBlocker(m_shadowReach)};
    m_rotation->setValue(settings.environmentRotationDeg);
    m_exposure->setValue(settings.exposure);
    m_tonemap->setChecked(settings.tonemap);
    m_diffuse->setValue(settings.diffuseIntensity);
    m_specular->setValue(settings.specularIntensity);
    m_ambientFill->setValue(settings.ambientFill);
    m_keyIntensity->setValue(settings.keyIntensity);
    m_keyAzimuth->setValue(settings.keyAzimuthDeg);
    m_keyElevation->setValue(settings.keyElevationDeg);
    m_shadowsEnabled->setChecked(settings.shadowsEnabled);
    m_shadowIntensity->setValue(settings.shadowIntensity);
    m_shadowSoftness->setValue(settings.shadowSoftness);
    m_shadowReach->setValue(settings.shadowReach);
    m_subsurface->setValue(settings.subsurface);
    m_rim->setValue(settings.rimIntensity);
    m_backdropBlur->setValue(settings.backdropBlur);
    m_backdropBrightness->setValue(settings.backdropBrightness);
    m_domeRadius->setValue(settings.domeRadius);
    syncBackdropModeUi(); // button text + checked action (never re-fires triggered)
}

} // namespace pose
