/**
 * @file materialpanel.cpp
 * @brief MaterialPanel's core: the mirror (refresh) and the edits of a zone's values. See
 *        materialpanel.h, which also maps the class's four translation units.
 */

#include "materialpanel.h"

#include "colorswatch.h"
#include "dragnumberbox.h"
#include "menupickerbutton.h"
#include "overlaystack.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QEvent>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <string>

namespace pose {

using colorswatch::displayColor;
using colorswatch::linearColor;

namespace {

/// A collapsible section's body margin (propertyrows::addCollapsibleGroup's form margins): how
/// far in from the section's edge its rows — and their restore buttons — end.
constexpr int kSectionMargin = 8;

/// Which object and zone @p view shows, as one key ("" for none): what the sections' auto-open
/// and an open color pick belong to.
std::string shownZoneKey(const MaterialView& view) {
    return view.valid ? view.modelName + "#" + std::to_string(view.zone) : std::string();
}

} // namespace

MaterialPanel::MaterialPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("MaterialPanel"));
    buildUi();
    if (m_viewport) {
        // The selection, the zone or a material changed — by a click, undo, a project opened, one
        // of these rows: the panel follows.
        connect(m_viewport, &ViewportWidget::materialViewChanged, this, &MaterialPanel::onViewChanged);
    }
    refresh();
}

void MaterialPanel::onViewChanged() {
    // An edit made on this tab ends in refresh(), so by the time the next frame announces it the
    // rows already show it: the announcement is then an echo, and mirroring the whole tab a
    // second time per step of a scrub would double the scrub's cost. The test is the viewport's
    // own for "has what the tab shows changed" (MaterialView::sameView: the selection, the zone
    // and the model's material revision).
    if (m_viewport && m_view.valid && m_viewport->materialView().sameView(m_view)) {
        applyScrollLever(); // (a refresh would have run it)
        return;
    }
    refresh();
}

bool MaterialPanel::linked() const {
    return m_linked != nullptr && m_linked->isChecked();
}

void MaterialPanel::refresh() {
    m_view = m_viewport ? m_viewport->materialView() : MaterialView{};
    // A color being picked belongs to the zone its dialog was opened for. With another zone or
    // object shown under it (the dialog is window-modal, so not by the mouse: a script's
    // selection, a VR controller's click) the pick is ABANDONED — the dialog closes and the first
    // zone keeps the color as picked. Left open, every further color would be painted on the zone
    // shown now, and Cancel would write the first zone's color-at-open onto it.
    if (m_colorPick.isOpen() && shownZoneKey(m_view) != m_shownZone) {
        m_colorPick.abandon();
        if (!m_colorPick.isOpen()) {
            return; // (pickColor's `done` has ended the pick's edit and refreshed the tab)
        }
    }
    const MaterialView& view = m_view;

    m_objectName->setText(view.valid ? QString::fromStdString(view.modelName) : tr("No object selected"));
    m_save->setEnabled(view.valid && view.zone >= 0);
    m_import->setEnabled(view.valid && view.zone >= 0);
    m_hint->setVisible(!view.valid);

    // The zone picker: rebuilt only when the zone list changed (a rebuild closes an open menu).
    QStringList names;
    for (const std::string& zone : view.zones) {
        names.push_back(QString::fromStdString(zone));
    }
    if (names != m_zoneNames) {
        m_zoneNames = names;
        QList<MenuPickerButton::Item> items;
        for (const QString& name : names) {
            items.push_back({name, false, QString()});
        }
        const QSignalBlocker blocker(m_zonePicker);
        m_zonePicker->setItems(items);
    }
    {
        const QSignalBlocker blocker(m_zonePicker);
        if (view.valid && view.zone >= 0) {
            m_zonePicker->setCurrentIndex(view.zone);
        } else {
            m_zonePicker->setDetachedCaption(tr("No zones"));
        }
    }
    m_zonePicker->setEnabled(view.valid && !view.zones.empty());
    m_linked->setEnabled(view.valid);
    m_linkedNote->setText(!view.valid               ? QString()
                          : view.linkedZones == 0   ? tr("No other zone shares this zone's material.")
                          : view.linkedZones == 1   ? (linked() ? tr("1 other zone follows this zone's edits.") : tr("1 other zone shares this material."))
                          : linked()                ? tr("%1 other zones follow this zone's edits.").arg(view.linkedZones)
                                                    : tr("%1 other zones share this material.").arg(view.linkedZones));

    // The color swatches: each color as the screen shows it.
    for (std::size_t i = 0; i < m_colors.size(); ++i) {
        ColorRow& row = m_colors[i];
        if (row.swatch == nullptr) {
            continue;
        }
        const QColor c = view.valid ? displayColor(materialColorValue(view.params, static_cast<MaterialColor>(i))) : QColor(42, 43, 45);
        colorswatch::setSwatchColor(row.swatch, c, view.valid);
        row.swatch->setEnabled(view.valid);
        row.label->setEnabled(view.valid);
        row.restore->setEnabled(view.valid);
    }

    // The scrub fields: a detail map's strength is greyed without the map.
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        Row& row = m_rows[i];
        if (row.box == nullptr) {
            continue;
        }
        const auto field = static_cast<MaterialField>(i);
        bool       on = view.valid;
        // (An OVERLAY's relief counts as a normal map: the viewport draws an overlay's normal
        // image AND its bump image through the zone's normal map, so a zone without one of its
        // own still has a Normal Strength to set once a scar is layered on it.)
        const auto overlaid = [&view](MaterialMap map) {
            return std::any_of(view.params.overlays.begin(), view.params.overlays.end(),
                               [map](const MaterialOverlay& o) { return overlayAffects(o, map); });
        };
        if (field == MaterialField::NormalStrength) {
            on = on && (view.maps.has(MaterialMap::Normal) || overlaid(MaterialMap::Normal) || overlaid(MaterialMap::Bump));
        } else if (field == MaterialField::BumpStrength) {
            on = on && view.maps.has(MaterialMap::Bump);
        } else if (field == MaterialField::PoreDetail || field == MaterialField::PoreTiles || field == MaterialField::DetailRoughness ||
                   field == MaterialField::DetailTilesV || field == MaterialField::DetailOffsetU || field == MaterialField::DetailOffsetV) {
            on = on && view.maps.has(MaterialMap::PoreDetail);
        } else if (field == MaterialField::CoatBumpStrength) {
            on = on && view.maps.has(MaterialMap::CoatBump);
        } else if (field == MaterialField::DisplacementStrength || field == MaterialField::DisplacementMidLevel || field == MaterialField::DisplacementSubdiv) {
            // (A displacement map — or an overlay's displacement image, whose height the zone's
            // Strength scales with the map's.)
            bool overlayHeight = false;
            for (const MaterialOverlay& overlay : view.params.overlays) {
                overlayHeight = overlayHeight || (overlay.enabled && !overlay.displacement.empty());
            }
            on = on && (view.maps.has(MaterialMap::Displacement) || overlayHeight);
        } else if (field == MaterialField::AmbientOcclusion) {
            on = on && view.maps.has(MaterialMap::AmbientOcclusion);
        } else if (field == MaterialField::SmoothAngle) {
            on = on && view.params.smoothByAngle;
        } else if (field == MaterialField::Lobe1Roughness || field == MaterialField::Lobe2Roughness || field == MaterialField::LobeRatio) {
            // Dual-lobe materials only (skin): the rows are hidden, not greyed, for the rest.
            const bool dual = view.valid && (view.params.dualLobe() || view.imported.dualLobe());
            on = on && dual;
            if (row.form != nullptr) {
                row.form->setRowVisible(row.field, dual);
            }
        }
        {
            // Signals blocked: mirroring the material into the row must not set it back.
            const QSignalBlocker blocker(row.box);
            if (field == MaterialField::Specular) {
                // 100 is the standard dielectric; a material whose specular mask carries the
                // attenuation imports above it, and the range reaches that far so the value is
                // not cut. The ceiling is the IMPORTED value's (or the current one's, if a
                // project or a script set it higher), never the current value's alone: this runs
                // at every step of a scrub, and a ceiling that followed the value down would not
                // let the same scrub — or a later one — bring it back up.
                const double top = std::max({100.0, std::ceil(static_cast<double>(materialFieldValue(view.params, field))),
                                             std::ceil(static_cast<double>(materialFieldValue(view.imported, field)))});
                row.box->setRange(0.0, top);
            }
            // (A greyed row reads 0 — but the smoothing's angle is kept while it is off, and shown.)
            const bool shown = on || (view.valid && field == MaterialField::SmoothAngle);
            row.box->setValue(shown ? static_cast<double>(materialFieldValue(view.params, field)) : 0.0);
        }
        // (After the value: disabling a box mid-gesture ends the gesture, which closes its edit.)
        row.box->setEnabled(on);
        row.label->setEnabled(on);
        row.restore->setEnabled(on);
    }
    // The checkbox rows. (Flip Normal is greyed without a normal map, as Normal Strength is: there
    // is nothing to flip.)
    const bool hasNormalMap = view.valid && !view.params.maps[at(MaterialMap::Normal)].source.empty();
    for (const FlagRow& row : m_flags) {
        {
            const QSignalBlocker blocker(row.box);
            row.box->setChecked(view.valid && view.params.*row.member);
        }
        const bool on = row.needsNormalMap ? hasNormalMap : view.valid;
        row.box->setEnabled(on);
        row.restore->setEnabled(on);
    }

    refreshSlots();
    if (m_overlays != nullptr) {
        m_overlays->setView(view);
    }

    // A section opens as its material COMES INTO USE — a zone newly shown that uses it, or a map
    // or value arriving by any path (the tab, a script, undo, a project) — and closes never: the
    // user's folds stand.
    const std::string shown = shownZoneKey(view);
    const bool        zoneChanged = shown != m_shownZone;
    m_shownZone = shown;
    for (Section& section : m_sections) {
        const bool enabled = section.layer == MaterialLayer::Count || (view.valid && materialLayerIsEnabled(view.params, section.layer));
        if (section.toggle != nullptr) {
            const QSignalBlocker blocker(section.toggle);
            section.toggle->setChecked(enabled);
            section.toggle->setEnabled(view.valid);
        }
        if (section.dim != nullptr) {
            section.dim->setEnabled(!enabled); // (dimmed while the layer is off: addSection)
        }
        const bool use = enabled && view.valid && section.inUse && section.inUse(view);
        if (section.header != nullptr && use && (zoneChanged || !section.wasInUse)) {
            section.header->setChecked(true);
        }
        section.wasInUse = use;
    }
    syncOverlaySummary();

    m_resetZone->setEnabled(view.valid);
    m_resetAll->setEnabled(view.valid);
}

void MaterialPanel::setLayerEnabled(MaterialLayer layer, bool on) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    materialLayerEnabled(p, layer) = on;
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::setField(MaterialField field, double value) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    // From the values as they stand (re-read: another row may have moved them since the last
    // refresh), the one field changed.
    MaterialParams p = m_viewport->materialView().params;
    setMaterialField(p, field, static_cast<float>(value));
    m_viewport->setSelectedMaterial(p, linked());
    refresh();
}

void MaterialPanel::copyField(MaterialParams& dst, const MaterialParams& src, MaterialField field) {
    if (field == MaterialField::Roughness) {
        // The Roughness row stands for the dual lobe too: the lobes come back as imported, not
        // scaled from the single value.
        dst.roughness = src.roughness;
        dst.lobe1Roughness = src.lobe1Roughness;
        dst.lobe2Roughness = src.lobe2Roughness;
        dst.lobeRatio = src.lobeRatio;
        return;
    }
    setMaterialField(dst, field, materialFieldValue(src, field));
}

void MaterialPanel::restoreField(MaterialField field) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    // A line item with a map puts the map back with its value (textures first — the rule).
    if (const MaterialMap map = m_rowSlot[at(field)]; map != MaterialMap::Count && m_view.params.maps[at(map)] != m_view.imported.maps[at(map)]) {
        m_viewport->setMaterialTexture(map, QString::fromLatin1(kImportedTexture));
    }
    MaterialParams p = m_viewport->materialView().params;
    copyField(p, m_view.imported, field);
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit(); // registers no entry if the value was the imported one already
    refresh();
}

void MaterialPanel::pickColor(MaterialColor which) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    if (m_colorPick.isOpen()) {
        m_colorPick.raise();
        return;
    }
    if (!m_viewport->beginMaterialEdit()) {
        return;
    }
    m_colorPicking = which;
    const QColor atOpen = displayColor(materialColorValue(m_view.params, which));
    // The color is judged on the figure, behind the dialog, while it changes (colorswatch.h).
    // (Both writes ask the viewport which zone it shows NOW: the selection may have moved since
    // the tab last mirrored it, and a color of this pick is the zone's the dialog was opened for.)
    const auto stillShown = [this]() { return m_viewport && shownZoneKey(m_viewport->materialView()) == m_shownZone; };
    m_colorPick.open(
        this, m_colors[at(which)].label ? m_colors[at(which)].label->text() : tr("Color"), atOpen,
        [this, stillShown](const QColor& c) {
            if (stillShown()) {
                setColor(m_colorPicking, c);
            } else {
                refresh(); // (abandons the pick)
            }
        },
        [this, atOpen, stillShown](colorswatch::ColorPick::Outcome outcome) {
            if (outcome == colorswatch::ColorPick::Outcome::Cancelled && stillShown()) {
                setColor(m_colorPicking, atOpen); // Cancel: the color as it was when the dialog opened
            }
            if (m_viewport) {
                m_viewport->endMaterialEdit(); // one undo entry for the whole pick
            }
            refresh();
        });
}

void MaterialPanel::setColor(MaterialColor which, const QColor& srgb) {
    if (!m_viewport || !m_view.valid || !srgb.isValid()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    materialColorRef(p, which) = linearColor(srgb);
    m_viewport->setSelectedMaterial(p, linked());
    refresh();
}

void MaterialPanel::restoreColor(MaterialColor which) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    if (const MaterialMap map = m_colorSlot[at(which)]; map != MaterialMap::Count && m_view.params.maps[at(map)] != m_view.imported.maps[at(map)]) {
        m_viewport->setMaterialTexture(map, QString::fromLatin1(kImportedTexture));
    }
    MaterialParams p = m_viewport->materialView().params;
    materialColorRef(p, which) = materialColorValue(m_view.imported, which);
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::setFlag(bool MaterialParams::* member, bool on) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    p.*member = on;
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::syncOverlaySummary() {
    if (m_overlayHeader == nullptr) {
        return;
    }
    const bool folded = !m_overlayHeader->isChecked();
    const int  count = m_view.valid ? static_cast<int>(m_view.params.overlays.size()) : 0;
    m_overlayCount->setText(QString::number(count));
    placeOverlaySummary();
    m_overlayAdd->setVisible(folded && m_view.valid && count == 0);
    m_overlayCount->setVisible(folded && count > 0);
}

void MaterialPanel::placeOverlaySummary() {
    if (m_overlayHeader == nullptr) {
        return;
    }
    // THE RESTORE BUTTONS' COLUMN: a section's body is as wide as its header and lays its rows
    // out inside kSectionMargin, each row ending in a restore button — so the column's centre is
    // that margin plus half a restore button in from the header's right edge.
    int restoreWidth = 18;
    if (!m_rows.empty() && m_rows.front().restore != nullptr) {
        QToolButton* restore = m_rows.front().restore;
        restoreWidth = restore->isVisible() ? restore->width() : restore->sizeHint().width();
    }
    const QPoint centre(m_overlayHeader->width() - kSectionMargin - restoreWidth / 2, m_overlayHeader->height() / 2);
    for (QWidget* mark : {m_overlayAdd, static_cast<QWidget*>(m_overlayCount)}) {
        if (mark == m_overlayCount) {
            mark->ensurePolished(); // (the stylesheet's font, before it is measured)
            mark->resize(mark->sizeHint());
        }
        QRect place = mark->rect();
        place.moveCenter(centre);
        mark->move(place.topLeft());
    }
}

bool MaterialPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_overlayHeader && event->type() == QEvent::Resize) {
        placeOverlaySummary();
    }
    return QWidget::eventFilter(watched, event);
}

void MaterialPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!m_columnAligned) {
        m_columnAligned = true;
        alignLabelColumn();
    }
}

} // namespace pose
