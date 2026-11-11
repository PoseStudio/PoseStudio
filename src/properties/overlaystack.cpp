/**
 * @file overlaystack.cpp
 * @brief Implementation of OverlayStack, the Materials tab's Overlays section. See overlaystack.h.
 */

#include "overlaystack.h"

#include "constants.h"
#include "dragnumberbox.h"
#include "materialthumbbutton.h"
#include "menupickerbutton.h"
#include "overlaycompositor.h" // OverlayImage (the thumbnails)
#include "preferencesmanager.h"
#include "propertyrows.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pose {

namespace {

std::size_t at(OverlayField field) {
    return static_cast<std::size_t>(field);
}
std::size_t at(OverlayElement element) {
    return static_cast<std::size_t>(element);
}

/// The width a card's name is elided to (the header must not widen the side panel).
constexpr int kTitleWidthPx = 150;

/// The Offset rows, in percent of the map: the whole map either way (beyond it nothing of the
/// overlay is left on it); a full sweep of the box moves the overlay a fifth of the map, so a
/// scrub places it to a few texels of a 4K map, and a typed value to the texel.
constexpr double kOffsetRangePct = 100.0;
constexpr double kOffsetStepPct = 0.05;
constexpr double kOffsetScrubSpanPct = 20.0;

/// The Rotation and Scale rows: half a degree and half a percent a step; a full sweep of the box
/// is half a turn, or the painted size again (the ranges are wider: a whole turn, a tenth of the
/// size to five times it).
constexpr double kRotationStepDeg = 0.5;
constexpr double kRotationScrubSpanDeg = 180.0;
constexpr double kScaleStepPct = 0.5;
constexpr double kScaleScrubSpanPct = 100.0;

/// What one of an overlay's images is read as, by the element it is.
OverlayImageKind kindOf(OverlayElement element) {
    switch (element) {
    case OverlayElement::Bump:
    case OverlayElement::Displacement: return OverlayImageKind::Height;
    case OverlayElement::Normal: return OverlayImageKind::Normal;
    case OverlayElement::Opacity:  // (the overlay's mask)
    case OverlayElement::Metallic: return OverlayImageKind::Mask; // (it limits: read as a mask)
    default: return OverlayImageKind::Color;
    }
}

QString elementLabel(OverlayElement element) {
    switch (element) {
    case OverlayElement::Opacity: return OverlayStack::tr("Opacity map");
    case OverlayElement::Metallic: return OverlayStack::tr("Metallic map");
    case OverlayElement::Bump: return OverlayStack::tr("Bump image");
    case OverlayElement::Normal: return OverlayStack::tr("Normal image");
    case OverlayElement::Displacement: return OverlayStack::tr("Displacement image");
    default: return OverlayStack::tr("Picture");
    }
}

/// The blend picker's entries, in OverlayBlend's order.
QList<MenuPickerButton::Item> blendItems() {
    return {
        {OverlayStack::tr("Multiply"), false,
         OverlayStack::tr("Darkens the skin that is there: ink, bruises, freckles, veins. White changes nothing, so the skin's own tone and grain show through.")},
        {OverlayStack::tr("Normal"), false, OverlayStack::tr("Covers the skin: opaque makeup, body paint, a sticker.")},
        {OverlayStack::tr("Soft Light"), false, OverlayStack::tr("A gentle tint that keeps the skin's light and dark: blush, a faint mark.")},
        {OverlayStack::tr("Overlay"), false, OverlayStack::tr("A stronger tint with more contrast.")},
        {OverlayStack::tr("Screen"), false, OverlayStack::tr("Lightens: scars, stretch marks, highlighter. Black changes nothing.")},
        {OverlayStack::tr("Darken"), false, OverlayStack::tr("The darker of the image and the skin.")},
        {OverlayStack::tr("Lighten"), false, OverlayStack::tr("The lighter of the image and the skin.")},
    };
}

/// The swatch's look (the Materials tab's color rows'): its background IS the color.
void paintSwatch(QPushButton* swatch, const QColor& c, bool enabled) {
    const bool dark = c.lightnessF() < 0.5;
    swatch->setText(enabled ? c.name(QColor::HexRgb).toUpper() : QString());
    swatch->setStyleSheet(QStringLiteral("QPushButton#MaterialColorButton { background-color: %1; color: %2; border: 1px solid #4a4b4d; "
                                         "border-radius: 4px; min-height: 20px; padding: 2px 8px; }"
                                         "QPushButton#MaterialColorButton:hover { border-color: #5b87cc; }"
                                         "QPushButton#MaterialColorButton:disabled { background-color: #2a2b2d; color: #6a6b6e; }")
                              .arg(c.name(QColor::HexRgb), dark ? QStringLiteral("#e0e0e0") : QStringLiteral("#1b1c1e")));
}

QColor tintColor(const glm::vec3& tint) {
    return QColor::fromRgbF(std::clamp(tint.x, 0.0f, 1.0f), std::clamp(tint.y, 0.0f, 1.0f), std::clamp(tint.z, 0.0f, 1.0f));
}

/// Names a widget for the scripted test's `ui` command (a dynamic property: no cost to the app).
template <typename W>
W* tagged(W* widget, const char* testId) {
    widget->setProperty("testId", QString::fromLatin1(testId));
    return widget;
}

/// One overlay's image paths as one string: what a card is built for.
std::string structureOf(const MaterialOverlay& o) {
    return o.color + "|" + o.opacityMap + "|" + o.metallicMap + "|" + o.bump + "|" + o.normal + "|" + o.displacement;
}

/// A small flat button of a card's header: move up / down (the style's own arrow — a text arrow
/// is drawn from whatever font has the glyph, and came out as a colored emoji) or remove (a
/// plain multiplication sign).
QToolButton* headerButton(Qt::ArrowType arrow, const QString& tip) {
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("OverlayCardButton"));
    if (arrow == Qt::NoArrow) {
        button->setText(QString(QChar(0x00D7))); // the multiplication sign
    } else {
        button->setArrowType(arrow);
    }
    button->setFixedSize(20, 20);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoRaise(true);
    return button;
}

} // namespace

OverlayStack::OverlayStack(ViewportWidget* viewport, QWidget* parent) : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("OverlayStack"));
    setProperty("testId", QStringLiteral("overlay.stack")); // (the scripted test's `ui call`: beginRename)
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(8);

    auto* addRow = new QHBoxLayout;
    addRow->setContentsMargins(0, 0, 0, 0);
    m_add = new QPushButton(tr("+ Add Overlay"));
    m_add->setObjectName(QStringLiteral("PropertyResetButton"));
    tagged(m_add, "overlay.add");
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setToolTip(tr("Adds an empty overlay on top of this zone's stack. Its card is where it gets a picture (Base Color), or a "
                         "color and an opacity map (a mask), and relief of its own."));
    connect(m_add, &QPushButton::clicked, this, [this]() { addOverlay(); });
    addRow->addWidget(m_add);
    addRow->addStretch(1);
    col->addLayout(addRow);

    // The cards, as an image editor lists its layers: the TOP of the stack first (a new overlay
    // appears right under the button that added it).
    m_cardColumn = new QVBoxLayout;
    m_cardColumn->setContentsMargins(0, 0, 0, 0);
    m_cardColumn->setSpacing(6);
    col->addLayout(m_cardColumn);

    m_zonesNote = new QLabel;
    m_zonesNote->setObjectName(QStringLiteral("MaterialHint"));
    col->addWidget(m_zonesNote);
}

void OverlayStack::setView(const MaterialView& view) {
    m_view = view;
    const std::vector<MaterialOverlay>& overlays = m_view.params.overlays;

    // The fold state belongs to the zone shown: another zone starts folded (a lone overlay open).
    const std::string shown = view.valid ? view.modelName + "#" + std::to_string(view.zone) : std::string();
    bool              force = false;
    if (shown != m_shownZone) {
        m_shownZone = shown;
        m_open.assign(overlays.size(), overlays.size() == 1);
        force = true;
    }
    std::vector<std::string> structure;
    structure.reserve(overlays.size());
    for (const MaterialOverlay& o : overlays) {
        structure.push_back(structureOf(o));
    }
    if (force || structure != m_structure || m_cards.size() != overlays.size()) {
        // A tint being picked belongs to the overlay it was opened for: with another zone shown
        // or the stack reshuffled under it, the dialog closes and the color stays as picked.
        if (m_tintDialog != nullptr) {
            m_tintAbandoned = true;
            m_tintDialog->close();
        }
        m_open.resize(overlays.size(), true); // (an overlay that arrived is shown open)
        m_structure = std::move(structure);
        rebuild();
    }
    for (int i = 0; i < static_cast<int>(m_cards.size()); ++i) {
        syncCard(i);
    }

    m_add->setEnabled(view.valid);
    // Which zones the stack is painted on, when the zone shares its color map.
    QString note;
    if (view.valid && view.overlayZones.size() > 1) {
        QString line;
        QString names;
        for (const int zone : view.overlayZones) {
            if (zone < 0 || static_cast<std::size_t>(zone) >= view.zones.size()) {
                continue;
            }
            const QString name = QString::fromStdString(view.zones[static_cast<std::size_t>(zone)]);
            if (!line.isEmpty() && line.size() + name.size() > 42) {
                names += line + QStringLiteral(",\n");
                line.clear();
            }
            line += (line.isEmpty() ? QString() : QStringLiteral(", ")) + name;
        }
        names += line;
        note = tr("Painted on every zone that shares this color map:\n%1").arg(names);
    }
    m_zonesNote->setText(note);
    m_zonesNote->setVisible(!note.isEmpty());
}

void OverlayStack::rebuild() {
    if (m_nameEditor) {
        finishRename(/*commit=*/false); // (the cards are going: an open name editor goes with its card)
    }
    for (Card& card : m_cards) {
        m_cardColumn->removeWidget(card.frame);
        card.frame->hide();
        card.frame->setParent(nullptr); // (out of the widget tree now: a search of it finds live cards alone)
        card.frame->deleteLater();      // (a card's own button may be the caller: never deleted under it)
    }
    m_cards.clear();
    const int count = static_cast<int>(m_view.params.overlays.size());
    m_cards.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        m_cards.push_back(buildCard(i));
        m_cardColumn->insertWidget(0, m_cards.back().frame); // (the last overlay is the top of the stack: listed first)
        // (Shown now: a widget added to a visible parent's layout is otherwise shown by a queued
        // call, a turn of the event loop later.)
        m_cards.back().frame->show();
    }
}

MaterialThumbButton* OverlayStack::makeThumb(int index, OverlayElement element) {
    auto* thumb = new MaterialThumbButton(kThumbSmallPx, kThumbSmallRadiusPx);
    connect(thumb, &QAbstractButton::clicked, this, [this, index, element]() { chooseImage(index, element); });
    thumb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(thumb, &QWidget::customContextMenuRequested, this,
            [this, index, element, thumb](const QPoint& pos) { showThumbMenu(index, element, thumb->mapToGlobal(pos)); });
    return thumb;
}

void OverlayStack::addFieldRow(Card& card, QFormLayout* form, int index, OverlayField field, const QString& label, const QString& tip,
                               double min, double max, double step, int decimals, double origin, OverlayElement element) {
    FieldRow& row = card.rows[at(field)];
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.box = new DragNumberBox;
    row.box->setRange(min, max);
    row.box->setSingleStep(step);
    row.box->setDecimals(decimals);
    row.box->setOrigin(origin);
    row.box->setToolTip(tip);
    row.box->setProperty("testId", QStringLiteral("overlay.") + QString::fromLatin1(overlayFieldName(field)));
    // One undo entry per completed gesture: the box brackets its scrub or type-in.
    connect(row.box, &DragNumberBox::editingStarted, this, [this]() {
        if (m_viewport) {
            m_viewport->beginMaterialEdit();
        }
    });
    connect(row.box, &DragNumberBox::editingFinished, this, [this]() {
        if (m_viewport) {
            m_viewport->endMaterialEdit();
        }
    });
    connect(row.box, &DragNumberBox::valueChanged, this, [this, index, field](double value) { setField(index, field, value); });
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to the default"));
    row.restore->setProperty("testId", QStringLiteral("overlay.restore.") + QString::fromLatin1(overlayFieldName(field)));
    connect(row.restore, &QToolButton::clicked, this, [this, index, field]() { restoreField(index, field); });
    QWidget* fieldWidget = propertyrows::rowWithRestore(row.box, row.restore);
    if (element != OverlayElement::Count) {
        // The row's own image beside its strength, as the tab's maps sit beside their values.
        MaterialThumbButton* thumb = makeThumb(index, element);
        card.thumbs[at(element)] = thumb;
        if (auto* h = qobject_cast<QHBoxLayout*>(fieldWidget->layout())) {
            h->insertWidget(0, thumb, 0, Qt::AlignVCenter);
            h->insertSpacing(1, 2);
        }
    }
    form->addRow(row.label, fieldWidget);
}

OverlayStack::Card OverlayStack::buildCard(int index) {
    Card card;
    card.frame = new QFrame;
    card.frame->setObjectName(QStringLiteral("OverlayCard"));
    auto* col = new QVBoxLayout(card.frame);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // --- The header: the fold arrow + name, on/off, move, remove ---
    auto* head = new QWidget;
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(6, 4, 4, 4);
    h->setSpacing(4);
    card.header = new QToolButton;
    card.header->setObjectName(QStringLiteral("OverlayCardHeader"));
    card.header->setCheckable(true);
    card.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    card.header->setAutoRaise(true);
    card.header->setCursor(Qt::PointingHandCursor);
    card.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tagged(card.header, "overlay.header");
    card.header->setContextMenuPolicy(Qt::CustomContextMenu);
    QToolButton* nameButton = card.header;
    connect(nameButton, &QWidget::customContextMenuRequested, this, [this, index, nameButton](const QPoint& pos) {
        QMenu          menu(this);
        const QAction* rename = menu.addAction(tr("Rename…"));
        if (menu.exec(nameButton->mapToGlobal(pos)) == rename) {
            beginRename(index);
        }
    });
    h->addWidget(card.header, 1);
    card.enabled = new QCheckBox;
    card.enabled->setObjectName(QStringLiteral("PropertyLayerToggle"));
    tagged(card.enabled, "overlay.enabled");
    card.enabled->setCursor(Qt::PointingHandCursor);
    card.enabled->setToolTip(tr("Switch this overlay on or off; its images and values are kept."));
    connect(card.enabled, &QCheckBox::toggled, this, [this, index](bool on) {
        if (m_viewport && m_viewport->beginMaterialEdit()) {
            setField(index, OverlayField::Enabled, on ? 1.0 : 0.0);
            m_viewport->endMaterialEdit();
        }
    });
    h->addWidget(card.enabled, 0, Qt::AlignVCenter);
    card.up = headerButton(Qt::UpArrow, tr("Move up: over the overlay above it"));
    card.down = headerButton(Qt::DownArrow, tr("Move down: under the overlay below it"));
    card.remove = headerButton(Qt::NoArrow, tr("Remove this overlay"));
    tagged(card.up, "overlay.up");
    tagged(card.down, "overlay.down");
    tagged(card.remove, "overlay.remove");
    connect(card.up, &QToolButton::clicked, this, [this, index]() { moveOverlay(index, +1); });
    connect(card.down, &QToolButton::clicked, this, [this, index]() { moveOverlay(index, -1); });
    connect(card.remove, &QToolButton::clicked, this, [this, index]() { removeOverlay(index); });
    h->addWidget(card.up);
    h->addWidget(card.down);
    h->addWidget(card.remove);
    col->addWidget(head);

    // --- The body: the color's controls (its picture and its mask among them), the gloss, the
    // relief images, the overlay's place ---
    card.body = new QWidget;
    auto* form = new QFormLayout(card.body);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setContentsMargins(8, 2, 6, 8);

    card.blendLabel = new QLabel(tr("Blend"));
    card.blend = new MenuPickerButton;
    card.blend->setObjectName(QStringLiteral("EnvironmentHdriButton")); // (the tab's one picker look)
    card.blend->setItems(blendItems());
    tagged(card.blend, "overlay.blend");
    const QString blendTip = tr("How the image meets the skin under it. Multiply suits ink, bruises and freckles on any skin tone; "
                                "Normal covers like paint; Soft Light and Overlay tint; Screen lightens.");
    card.blend->setToolTip(blendTip);
    card.blendLabel->setToolTip(blendTip);
    connect(card.blend, &MenuPickerButton::currentChanged, this, [this, index](int mode) {
        if (m_viewport && m_viewport->beginMaterialEdit()) {
            setField(index, OverlayField::Blend, mode);
            m_viewport->endMaterialEdit();
        }
    });
    card.blendRestore = propertyrows::makeRestoreButton();
    card.blendRestore->setToolTip(tr("Back to Multiply"));
    tagged(card.blendRestore, "overlay.restore.blend");
    connect(card.blendRestore, &QToolButton::clicked, this, [this, index]() { restoreField(index, OverlayField::Blend); });
    form->addRow(card.blendLabel, propertyrows::rowWithRestore(card.blend, card.blendRestore));

    // BASE COLOR: the overlay's color (the swatch) and its picture (the small square before it),
    // as the tab's own Base Color row has them.
    card.tintLabel = new QLabel(tr("Base Color"));
    card.tint = new QPushButton;
    card.tint->setObjectName(QStringLiteral("MaterialColorButton"));
    card.tint->setCursor(Qt::PointingHandCursor);
    tagged(card.tint, "overlay.basecolor");
    const QString tintTip = tr("The overlay's color, and — the small square — its picture. With a picture the color multiplies it: white "
                               "leaves it as painted, and a white or grey picture takes the color whole. With no picture the overlay is "
                               "this color itself, where the Opacity's map lets it (or, with no map, over the whole zone). Click the "
                               "square to choose the picture; right-click it to remove it.");
    card.tint->setToolTip(tintTip);
    card.tintLabel->setToolTip(tintTip);
    connect(card.tint, &QPushButton::clicked, this, [this, index]() { pickTint(index); });
    card.tintRestore = propertyrows::makeRestoreButton();
    card.tintRestore->setToolTip(tr("Back to white: the picture as painted"));
    tagged(card.tintRestore, "overlay.restore.basecolor");
    connect(card.tintRestore, &QToolButton::clicked, this, [this, index]() {
        if (m_viewport && m_viewport->beginMaterialEdit()) {
            setTint(index, QColor(Qt::white));
            m_viewport->endMaterialEdit();
        }
    });
    QWidget* colorField = propertyrows::rowWithRestore(card.tint, card.tintRestore);
    card.thumbs[at(OverlayElement::Color)] = makeThumb(index, OverlayElement::Color);
    if (auto* row = qobject_cast<QHBoxLayout*>(colorField->layout())) {
        row->insertWidget(0, card.thumbs[at(OverlayElement::Color)], 0, Qt::AlignVCenter);
        row->insertSpacing(1, 2);
    }
    form->addRow(card.tintLabel, colorField);

    addFieldRow(card, form, index, OverlayField::Brightness, tr("Brightness"), tr("Lightens or darkens the image before it is blended."), -100.0, 100.0,
                1.0, 0, 0.0);
    addFieldRow(card, form, index, OverlayField::Contrast, tr("Contrast"),
                tr("Spreads the image's values apart (more) or pulls them toward mid grey (less) before it is blended."), -100.0, 100.0, 1.0, 0, 0.0);
    addFieldRow(card, form, index, OverlayField::Gamma, tr("Gamma"),
                tr("The image's midtones: above 1 lightens them, below 1 darkens them; black and white stay."), 0.2, 5.0, 0.01, 2, 1.0);
    addFieldRow(card, form, index, OverlayField::Hue, tr("Hue"), tr("Turns the image's colors around the color wheel, in degrees."), -180.0, 180.0,
                1.0, 0, 0.0);
    addFieldRow(card, form, index, OverlayField::Saturation, tr("Saturation"), tr("-100 is grey, 0 the image's own colors, 100 twice as vivid."), -100.0,
                100.0, 1.0, 0, 0.0);
    addFieldRow(card, form, index, OverlayField::Gloss, tr("Gloss"),
                tr("How much glossier (above 0) or more matte (below 0) the surface is where the overlay is — lip gloss, a fresh wound, "
                   "powder. It follows the picture's shape and the Opacity's map, not the Opacity value."),
                -100.0, 100.0, 1.0, 0, 0.0);
    addFieldRow(card, form, index, OverlayField::Metallic, tr("Metallic"),
                tr("How much more metallic (above 0) or less (below 0) the surface is where the overlay is — metallic ink, gold leaf, "
                   "glitter; below 0, paint over a metal surface. The small square is an image that limits it within the overlay: a "
                   "grayscale image (white is metallic) or one with transparency (its opaque part is). Like the gloss, it follows the "
                   "picture's shape and the Opacity's map, not the Opacity value."),
                -100.0, 100.0, 1.0, 0, 0.0, OverlayElement::Metallic);
    // OPACITY, with its map — the overlay's mask — after the color's, the gloss's and the metal's
    // own rows (it shapes all three) and before the relief, which it does not touch.
    addFieldRow(card, form, index, OverlayField::Opacity, tr("Opacity"),
                tr("How much of the overlay's COLOR shows: 100 is the color as painted, 0 none of it. The small square is its map — the "
                   "overlay's MASK: a grayscale image (white shows, black does not) or an image with transparency (its opaque part "
                   "shows). With a Base Color and no picture, the map is the overlay's whole shape: a lipstick, an eye shadow. The gloss "
                   "and the metallic take their shape from the map but not the value; the relief (Bump, Normal, Displacement) follows "
                   "neither."),
                0.0, 100.0, 1.0, 0, 0.0, OverlayElement::Opacity);
    addFieldRow(card, form, index, OverlayField::BumpStrength, tr("Bump"),
                tr("A grayscale height image — a scar's ridge, raised ink, a groove: mid grey is no change, lighter rises, darker sinks. "
                   "The number is its strength (negative turns it inside out). Click the picture to choose the image."),
                -400.0, 400.0, 5.0, 0, 100.0, OverlayElement::Bump);
    addFieldRow(card, form, index, OverlayField::NormalStrength, tr("Normal"),
                tr("A tangent-space normal image blended over the zone's normal map. The number is its strength. "
                   "Click the picture to choose the image."),
                0.0, 400.0, 5.0, 0, 100.0, OverlayElement::Normal);
    addFieldRow(card, form, index, OverlayField::DisplacementStrength, tr("Displacement"),
                tr("A grayscale height image for a renderer's displacement, by its strength. Stored for rendering engines; "
                   "the viewport does not displace. Click the picture to choose the image."),
                -400.0, 400.0, 5.0, 0, 100.0, OverlayElement::Displacement);
    // Where the overlay is, which way up and how large — LAST in the card: what it is comes first,
    // and these place every image of the group together.
    addFieldRow(card, form, index, OverlayField::OffsetX, tr("Offset X"),
                tr("Moves the whole overlay — its color, gloss and relief together — sideways across the map it is painted for, "
                   "in percent of the map's width: positive to the right. On the figure it travels across the surface as the zone is "
                   "unwrapped; what runs off its part of the map leaves the surface."),
                -kOffsetRangePct, kOffsetRangePct, kOffsetStepPct, 2, 0.0);
    addFieldRow(card, form, index, OverlayField::OffsetY, tr("Offset Y"),
                tr("Moves the whole overlay up (positive) or down the map it is painted for, in percent of the map's height."),
                -kOffsetRangePct, kOffsetRangePct, kOffsetStepPct, 2, 0.0);
    for (const OverlayField field : {OverlayField::OffsetX, OverlayField::OffsetY}) {
        card.rows[at(field)].box->setScrubSpan(kOffsetScrubSpanPct);
    }
    addFieldRow(card, form, index, OverlayField::Rotation, tr("Rotation"),
                tr("Turns the whole overlay about its own centre, in degrees: positive is clockwise as the map is seen. "
                   "(On a part of the figure that is unwrapped mirrored it turns the other way.)"),
                -180.0, 180.0, kRotationStepDeg, 1, 0.0);
    card.rows[at(OverlayField::Rotation)].box->setScrubSpan(kRotationScrubSpanDeg);
    addFieldRow(card, form, index, OverlayField::Scale, tr("Scale"),
                tr("The overlay's size, in percent of the size it was painted at: it grows and shrinks about its own centre."),
                static_cast<double>(kOverlayScaleMin) * 100.0, static_cast<double>(kOverlayScaleMax) * 100.0, kScaleStepPct, 1, 100.0);
    card.rows[at(OverlayField::Scale)].box->setScrubSpan(kScaleScrubSpanPct);
    col->addWidget(card.body);
    // The body dims while the overlay is switched off (still editable: its values are kept).
    card.dim = new QGraphicsOpacityEffect(card.body);
    card.dim->setOpacity(1.0);
    card.body->setGraphicsEffect(card.dim);

    QToolButton* header = card.header;
    QWidget*     body = card.body;
    connect(header, &QToolButton::toggled, this, [this, index, header, body](bool on) {
        body->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        if (index >= 0 && static_cast<std::size_t>(index) < m_open.size()) {
            m_open[static_cast<std::size_t>(index)] = on;
        }
    });
    return card;
}

void OverlayStack::syncCard(int index) {
    const std::vector<MaterialOverlay>& overlays = m_view.params.overlays;
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size() || static_cast<std::size_t>(index) >= m_cards.size()) {
        return;
    }
    const MaterialOverlay& o = overlays[static_cast<std::size_t>(index)];
    Card&                  card = m_cards[static_cast<std::size_t>(index)];
    const int              count = static_cast<int>(overlays.size());

    // The header: the name (elided), folded or not, on or off, where it can move.
    const QString title = displayName(index);
    card.header->setToolTip(tr("%1\nClick to fold or unfold; right-click to rename.").arg(title));
    card.header->setText(QFontMetrics(card.header->font()).elidedText(title, Qt::ElideMiddle, kTitleWidthPx));
    const bool open = static_cast<std::size_t>(index) < m_open.size() && m_open[static_cast<std::size_t>(index)];
    {
        const QSignalBlocker blocker(card.header);
        card.header->setChecked(open);
    }
    card.header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    card.body->setVisible(open);
    {
        const QSignalBlocker blocker(card.enabled);
        card.enabled->setChecked(o.enabled);
    }
    card.up->setEnabled(index < count - 1); // (the stack's top is the highest index, listed first)
    card.down->setEnabled(index > 0);
    card.dim->setOpacity(o.enabled ? 1.0 : 0.45);

    // The color's controls are always live (with no picture the overlay is a flat Base Color); a
    // relief's strength needs its own image; the place needs any image at all — a flat color over
    // the whole zone has nothing to move.
    const bool hasColor = true;
    const bool hasImage = !o.color.empty() || !o.opacityMap.empty() || !o.metallicMap.empty() || !o.bump.empty() || !o.normal.empty() ||
                          !o.displacement.empty();
    {
        const QSignalBlocker blocker(card.blend);
        card.blend->setCurrentIndex(static_cast<int>(o.blend));
    }
    card.blend->setEnabled(hasColor);
    card.blendLabel->setEnabled(hasColor);
    card.blendRestore->setEnabled(hasColor);
    paintSwatch(card.tint, tintColor(o.tint), hasColor);
    card.tint->setEnabled(hasColor);
    card.tintLabel->setEnabled(hasColor);
    card.tintRestore->setEnabled(hasColor);
    for (std::size_t i = 0; i < card.rows.size(); ++i) {
        FieldRow& row = card.rows[i];
        if (row.box == nullptr) {
            continue;
        }
        const auto field = static_cast<OverlayField>(i);
        const bool on = field == OverlayField::BumpStrength           ? !o.bump.empty()
                        : field == OverlayField::NormalStrength       ? !o.normal.empty()
                        : field == OverlayField::DisplacementStrength ? !o.displacement.empty()
                        : (field == OverlayField::OffsetX || field == OverlayField::OffsetY || field == OverlayField::Rotation ||
                           field == OverlayField::Scale)
                              ? hasImage
                                                                      : hasColor;
        {
            const QSignalBlocker blocker(row.box); // mirroring a value must not set it back
            row.box->setValue(static_cast<double>(overlayFieldValue(o, field)));
        }
        row.box->setEnabled(on);
        row.label->setEnabled(on);
        row.restore->setEnabled(on);
    }

    // The thumbnails: each element's image, its file in the tooltip and under the hover preview.
    for (int e = 0; e < static_cast<int>(OverlayElement::Count); ++e) {
        const auto           element = static_cast<OverlayElement>(e);
        MaterialThumbButton* thumb = card.thumbs[at(element)];
        if (thumb == nullptr) {
            continue;
        }
        const std::string& path = overlayElementSource(o, element);
        thumb->setImage(path.empty() ? QPixmap() : imagePixmap(path, kindOf(element), kThumbSmallPx, m_thumbs));
        const QString file = path.empty() ? QString() : QFileInfo(QString::fromStdString(path)).fileName();
        thumb->setPreview(
            [this, index, element]() {
                const std::vector<MaterialOverlay>& now = m_view.params.overlays;
                if (index < 0 || static_cast<std::size_t>(index) >= now.size()) {
                    return QPixmap();
                }
                return imagePixmap(overlayElementSource(now[static_cast<std::size_t>(index)], element), kindOf(element), kPreviewPx, m_previews);
            },
            file);
        thumb->setToolTip(tr("%1: %2\nClick to choose an image file; right-click to remove it or open its folder.")
                              .arg(elementLabel(element), path.empty() ? tr("none") : QString::fromStdString(path)));
    }
}

std::vector<MaterialOverlay> OverlayStack::stack() const {
    return m_viewport ? m_viewport->materialView().params.overlays : std::vector<MaterialOverlay>();
}

void OverlayStack::apply(const std::vector<MaterialOverlay>& overlays) {
    if (!m_viewport) {
        return;
    }
    m_viewport->setMaterialOverlays(overlays);
    emit edited();
}

QString OverlayStack::askImage(const QString& title) {
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_OVERLAY_DIR);
    const QString       start = prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    const QString       filter = tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.tga *.webp);;All files (*)");
    const QString       path = QFileDialog::getOpenFileName(this, title, start, filter);
    if (!path.isEmpty()) {
        prefs.setValue(key, QFileInfo(path).absolutePath());
    }
    return path;
}

void OverlayStack::addOverlay() {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    // An EMPTY overlay on top of the stack (one undo entry): what it is — a picture, a color and
    // a mask, relief — is chosen on its card, which opens unfolded. Empty, it paints nothing.
    // It is NAMED as it is added — "Overlay 3", one past the highest such name in the stack — and
    // keeps the name wherever it is moved; the header's Rename… gives it another.
    std::vector<MaterialOverlay> overlays = stack();
    MaterialOverlay              added;
    const QString                prefix = tr("Overlay");
    added.name = QStringLiteral("%1 %2").arg(prefix).arg(nextOverlayNumber(overlays, prefix.toStdString())).toStdString();
    overlays.push_back(std::move(added));
    apply(overlays);
}

QString OverlayStack::displayName(int index) const {
    const std::vector<MaterialOverlay>& overlays = m_view.params.overlays;
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return QString();
    }
    const std::string& name = overlayTitle(overlays[static_cast<std::size_t>(index)]);
    return name.empty() ? tr("Overlay %1").arg(index + 1) : QString::fromStdString(name);
}

void OverlayStack::beginRename(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_view.params.overlays.size() || static_cast<std::size_t>(index) >= m_cards.size()) {
        return;
    }
    if (m_nameEditor) {
        finishRename(/*commit=*/true); // (one at a time: the open one keeps its text)
    }
    // A line edit over the header's name, in place: Enter or a click elsewhere keeps the text, Esc
    // drops it (the event filter — a line edit has no cancel signal). `inlineEdit` tells the
    // application's Esc-deselects filter that this Esc is the editor's.
    QToolButton* header = m_cards[static_cast<std::size_t>(index)].header;
    auto*        editor = new QLineEdit(header);
    editor->setObjectName(QStringLiteral("OverlayNameEditor"));
    editor->setProperty("inlineEdit", true);
    editor->setProperty("testId", QStringLiteral("overlay.name"));
    editor->setMaxLength(60);
    editor->setText(displayName(index));
    editor->setGeometry(header->rect());
    editor->selectAll();
    editor->installEventFilter(this);
    m_nameEditor = editor;
    m_nameIndex = index;
    connect(editor, &QLineEdit::editingFinished, this, [this, editor]() {
        if (m_nameEditor == editor) {
            finishRename(/*commit=*/true);
        }
    });
    editor->show();
    editor->setFocus();
}

void OverlayStack::finishRename(bool commit) {
    QLineEdit* editor = m_nameEditor;
    if (editor == nullptr) {
        return;
    }
    m_nameEditor = nullptr; // (first: hiding the editor loses its focus, which would finish it again)
    const int     index = m_nameIndex;
    const QString name = editor->text().trimmed();
    m_nameIndex = -1;
    editor->hide();
    editor->setProperty("testId", QVariant()); // (gone, for a search of the widget tree, before the deferred delete)
    editor->deleteLater();
    if (commit && !name.isEmpty()) { // (an empty name is no name: the old one stays)
        setName(index, name);
    }
}

void OverlayStack::setName(int index, const QString& name) {
    std::vector<MaterialOverlay> overlays = stack();
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size() || overlays[static_cast<std::size_t>(index)].name == name.toStdString()) {
        return;
    }
    overlays[static_cast<std::size_t>(index)].name = name.toStdString();
    apply(overlays); // (one undo entry; nothing is re-painted: a name is no part of any map)
}

bool OverlayStack::eventFilter(QObject* watched, QEvent* event) {
    if (m_nameEditor && watched == m_nameEditor && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        finishRename(/*commit=*/false);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void OverlayStack::removeOverlay(int index) {
    std::vector<MaterialOverlay> overlays = stack();
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return;
    }
    overlays.erase(overlays.begin() + index);
    if (static_cast<std::size_t>(index) < m_open.size()) {
        m_open.erase(m_open.begin() + index);
    }
    apply(overlays);
}

void OverlayStack::moveOverlay(int index, int delta) {
    std::vector<MaterialOverlay> overlays = stack();
    const int                    to = index + delta;
    if (index < 0 || to < 0 || static_cast<std::size_t>(index) >= overlays.size() || static_cast<std::size_t>(to) >= overlays.size()) {
        return;
    }
    std::swap(overlays[static_cast<std::size_t>(index)], overlays[static_cast<std::size_t>(to)]);
    if (static_cast<std::size_t>(std::max(index, to)) < m_open.size()) {
        const bool a = m_open[static_cast<std::size_t>(index)];
        m_open[static_cast<std::size_t>(index)] = m_open[static_cast<std::size_t>(to)];
        m_open[static_cast<std::size_t>(to)] = a;
    }
    apply(overlays);
}

void OverlayStack::setField(int index, OverlayField field, double value) {
    // From the stack as it stands (re-read: another control may have moved it), the one field changed.
    std::vector<MaterialOverlay> overlays = stack();
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return;
    }
    setOverlayField(overlays[static_cast<std::size_t>(index)], field, static_cast<float>(value));
    apply(overlays);
}

void OverlayStack::restoreField(int index, OverlayField field) {
    if (!m_viewport || !m_viewport->beginMaterialEdit()) {
        return;
    }
    setField(index, field, static_cast<double>(overlayFieldValue(MaterialOverlay(), field)));
    m_viewport->endMaterialEdit(); // registers no entry if the value was the default already
}

void OverlayStack::chooseImage(int index, OverlayElement element) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    const QString path = askImage(tr("Choose the %1").arg(elementLabel(element)));
    if (!path.isEmpty()) {
        setImage(index, element, path);
    }
}

void OverlayStack::setImage(int index, OverlayElement element, const QString& path) {
    std::vector<MaterialOverlay> overlays = stack();
    if (!m_viewport || index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return;
    }
    if (!path.isEmpty()) {
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        const bool readable = m_viewport->overlayImage(path, kindOf(element)) != nullptr;
        QGuiApplication::restoreOverrideCursor();
        if (!readable) {
            QMessageBox::warning(this, tr("Overlays"), tr("The image could not be read:\n%1").arg(path));
            return;
        }
    }
    overlayElementSource(overlays[static_cast<std::size_t>(index)], element) = path.toStdString();
    apply(overlays);
}

void OverlayStack::showThumbMenu(int index, OverlayElement element, const QPoint& globalPos) {
    const std::vector<MaterialOverlay>& overlays = m_view.params.overlays;
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return;
    }
    const QString path = QString::fromStdString(overlayElementSource(overlays[static_cast<std::size_t>(index)], element));
    QMenu         menu(this);
    QAction*      choose = menu.addAction(tr("Choose Image…"));
    QAction*      remove = menu.addAction(tr("Remove"));
    remove->setEnabled(!path.isEmpty());
    menu.addSeparator();
    QAction* openLocation = menu.addAction(tr("Open File Location"));
    openLocation->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
    QAction* picked = menu.exec(globalPos);
    if (picked == choose) {
        chooseImage(index, element);
    } else if (picked == remove) {
        setImage(index, element, QString());
    } else if (picked == openLocation) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }
}

void OverlayStack::pickTint(int index) {
    const std::vector<MaterialOverlay>& overlays = m_view.params.overlays;
    if (!m_viewport || index < 0 || static_cast<std::size_t>(index) >= overlays.size()) {
        return;
    }
    if (m_tintDialog != nullptr) {
        m_tintDialog->raise();
        m_tintDialog->activateWindow();
        return;
    }
    if (!m_viewport->beginMaterialEdit()) {
        return;
    }
    m_tintIndex = index;
    m_tintAtOpen = tintColor(overlays[static_cast<std::size_t>(index)].tint);
    // The app's own themed dialog, non-modal: the tint is judged on the figure while it changes.
    m_tintDialog = new QColorDialog(m_tintAtOpen, this);
    m_tintDialog->setOption(QColorDialog::DontUseNativeDialog, true);
    m_tintDialog->setWindowTitle(tr("Overlay Base Color"));
    m_tintDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_tintDialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) { setTint(m_tintIndex, c); });
    connect(m_tintDialog, &QColorDialog::finished, this, [this](int result) {
        const bool abandoned = m_tintAbandoned; // (closed from setView: no restore, no re-entry)
        m_tintAbandoned = false;
        if (result != QDialog::Accepted && !abandoned) {
            setTint(m_tintIndex, m_tintAtOpen); // Cancel: the tint as it was when the dialog opened
        }
        if (m_viewport) {
            m_viewport->endMaterialEdit(); // one undo entry for the whole pick
        }
        m_tintDialog = nullptr;
        m_tintIndex = -1;
        if (!abandoned) {
            emit edited();
        }
    });
    m_tintDialog->open();
}

void OverlayStack::setTint(int index, const QColor& color) {
    std::vector<MaterialOverlay> overlays = stack();
    if (index < 0 || static_cast<std::size_t>(index) >= overlays.size() || !color.isValid()) {
        return;
    }
    overlays[static_cast<std::size_t>(index)].tint =
        glm::vec3(static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF()));
    apply(overlays);
}

QPixmap OverlayStack::imagePixmap(const std::string& path, OverlayImageKind kind, int px, std::unordered_map<std::string, Thumb>& cache) {
    if (!m_viewport || path.empty()) {
        return {};
    }
    const std::shared_ptr<const OverlayImage> image = m_viewport->overlayImage(QString::fromStdString(path), kind);
    if (!image || image->empty()) {
        return {};
    }
    const std::string key = std::to_string(static_cast<int>(kind)) + "|" + std::to_string(px) + "|" + path;
    if (const auto it = cache.find(key); it != cache.end()) {
        if (it->second.image.lock() == image) {
            return it->second.pixmap;
        }
        cache.erase(it);
    }
    if (cache.size() >= 64) {
        cache.clear();
    }
    // The cropped image — the tattoo itself, not the mostly empty square it is painted in —
    // fitted into the thumbnail over a backdrop the picture reads against: a light skin tone
    // under a color image (dark ink must show), mid grey under a height, flat under a normal.
    const QColor backdrop = kind == OverlayImageKind::Height ? QColor(128, 128, 128)
                            : kind == OverlayImageKind::Normal ? QColor(128, 128, 255)
                            : kind == OverlayImageKind::Mask   ? QColor(34, 35, 37) // (a mask is kept white: it reads on dark)
                                                               : QColor(214, 190, 172);
    const qreal dpr = devicePixelRatioF();
    const int   target = std::max(1, static_cast<int>(std::lround(px * dpr)));
    // A strided sample at no more than four times the thumbnail each way, smooth-scaled to fit.
    const int sw = static_cast<int>(std::min<uint32_t>(image->width, static_cast<uint32_t>(target * 4)));
    const int sh = static_cast<int>(std::min<uint32_t>(image->height, static_cast<uint32_t>(target * 4)));
    QImage    small(sw, sh, QImage::Format_RGBA8888);
    for (int y = 0; y < sh; ++y) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * image->height) / static_cast<uint64_t>(sh));
        uchar*         dst = small.scanLine(y);
        for (int x = 0; x < sw; ++x) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * image->width) / static_cast<uint64_t>(sw));
            const uint8_t* p = &image->rgba[(static_cast<std::size_t>(sy) * image->width + sx) * 4];
            const int      a = p[3];
            const int      r = p[0];
            const int      g = kind == OverlayImageKind::Height ? p[0] : p[1];
            const int      b = kind == OverlayImageKind::Height ? p[0] : p[2];
            dst[0] = static_cast<uchar>((r * a + backdrop.red() * (255 - a)) / 255);
            dst[1] = static_cast<uchar>((g * a + backdrop.green() * (255 - a)) / 255);
            dst[2] = static_cast<uchar>((b * a + backdrop.blue() * (255 - a)) / 255);
            dst[3] = 255;
            dst += 4;
        }
    }
    QImage canvas(target, target, QImage::Format_RGBA8888);
    canvas.fill(backdrop);
    const QImage fitted = small.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    {
        QPainter painter(&canvas);
        painter.drawImage((target - fitted.width()) / 2, (target - fitted.height()) / 2, fitted);
    }
    canvas.setDevicePixelRatio(dpr);
    QPixmap pixmap = QPixmap::fromImage(canvas);
    cache[key] = Thumb{image, pixmap};
    return pixmap;
}

} // namespace pose
