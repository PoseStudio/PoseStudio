// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file materialpanel_build.cpp
 * @brief MaterialPanel's construction: the tab's sections, rows and texture-slot definitions,
 *        top to bottom as the tab shows them. See materialpanel.h.
 */

#include "materialpanel.h"

#include "colorswatch.h"
#include "dragnumberbox.h"
#include "materialthumbbutton.h"
#include "menupickerbutton.h"
#include "overlaystack.h"
#include "propertyrows.h"
#include "viewportwidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace pose {

namespace {

/// The Overlays header's folded "+": a mark, DRAWN — two bars on whole pixels — so that its four
/// arms are the same length and it is centered exactly (a "+" glyph sits wherever its font's
/// math axis puts it). An EVEN size around a 2 px bar: 4 + 2 + 4 with a pixel of margin all
/// round (an odd one left the top and left arms a pixel short — a cross).
class PlusMark : public QWidget {
public:
    static constexpr int kSize = 12; ///< logical pixels, the margin included
    static constexpr int kBar = 2;
    explicit PlusMark(QWidget* parent) : QWidget(parent) {
        setFixedSize(kSize, kSize);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter     painter(this);
        const QColor ink(0xcf, 0xcf, 0xd2);
        const int    mid = (kSize - kBar) / 2;
        painter.fillRect(QRect(mid, 1, kBar, kSize - 2), ink);
        painter.fillRect(QRect(1, mid, kSize - 2, kBar), ink);
    }
};

} // namespace

void MaterialPanel::buildUi() {
    const propertyrows::ScrollColumn tab = propertyrows::makeScrollColumn(this, QStringLiteral("MaterialContent"), 10);
    m_scroll = tab.scroll;
    QVBoxLayout* col = tab.column;

    setProperty("testId", QStringLiteral("materials.panel")); // (the scripted `ui call` / `ui post`: the panel's own slots)

    // Top to bottom, as the tab shows them. (The slots' thumbnails are attached last: a thumbnail
    // is put in front of a row that is already built.)
    buildHeading(col);
    buildZoneGroup(col);
    buildSurfaceSections(col);
    buildVolumeSections(col);
    QFormLayout* detail = buildGeometrySections(col);
    defineSlots();
    // (The Detail Weight Mask scales no value: a row of its own, at the end of its section.)
    addSlotRow(detail, MaterialMap::DetailWeight, tr("Weight Mask"), m_slots[at(MaterialMap::DetailWeight)].tip);
    attachSlots();
    buildFooter(col);

    col->addStretch(1);
    tab.finish();
}

void MaterialPanel::buildHeading(QVBoxLayout* col) {
    // The heading: the object's name, and across from it the Import and Save buttons.
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);
    m_objectName = new QLabel;
    m_objectName->setObjectName(QStringLiteral("MaterialObjectName"));
    heading->addWidget(m_objectName, 1);
    m_import = new QPushButton(tr("Import"));
    m_import->setObjectName(QStringLiteral("PropertyResetButton")); // (Save's look)
    m_import->setCursor(Qt::ArrowCursor);
    m_import->setToolTip(tr("Puts a material from a file on this zone: a MaterialX material (.mtlx), the format material "
                            "libraries are published in, or a material saved by PoseStudio (.psm). One undo step."));
    m_import->setProperty("testId", QStringLiteral("materials.import"));
    connect(m_import, &QPushButton::clicked, this, &MaterialPanel::importMaterial);
    heading->addWidget(m_import, 0, Qt::AlignTop);
    m_save = new QPushButton(tr("Save"));
    m_save->setObjectName(QStringLiteral("PropertyResetButton")); // (the side panel's one plain-button look)
    m_save->setCursor(Qt::ArrowCursor);
    m_save->setToolTip(tr("Saves this zone's material — its values and its maps — to a file in your Materials library, "
                          "with a picture of it on a ball as its icon. A saved material is applied to another object "
                          "by double-clicking it in the Materials Library tab."));
    m_save->setProperty("testId", QStringLiteral("materials.save"));
    connect(m_save, &QPushButton::clicked, this, &MaterialPanel::saveMaterial);
    heading->addWidget(m_save, 0, Qt::AlignTop);
    col->addLayout(heading);
}

void MaterialPanel::buildZoneGroup(QVBoxLayout* col) {
    // --- Zone: which material group of the object the rows show ---
    QFormLayout* zone = propertyrows::addGroup(col, tr("Zone"));
    m_zonePicker = new MenuPickerButton;
    // (The HDRI picker's look by its object name, deliberately: one field style for every picker
    // on the side panel — without the caption inset propertyrows::stylePickerField adds.)
    m_zonePicker->setObjectName(QStringLiteral("EnvironmentHdriButton"));
    m_zonePicker->setToolTip(tr("The material zone shown below. Clicking a surface in the viewport selects the zone under the cursor."));
    connect(m_zonePicker, &MenuPickerButton::currentChanged, this, [this](int index) {
        if (m_viewport) {
            m_viewport->setMaterialZone(index);
            refresh();
        }
    });
    zone->addRow(QString(), m_zonePicker);
    m_linked = new QCheckBox(tr("Edit matching zones together"));
    m_linked->setChecked(true);
    m_linked->setToolTip(tr("Applies every value change to the other zones that were imported with the same material — a figure's skin zones in one go. "
                            "The eyes, the mouth and the nails have materials of their own and stay as they are. Texture slots are always the zone's own."));
    connect(m_linked, &QCheckBox::toggled, this, [this](bool) { refresh(); });
    zone->addRow(QString(), m_linked);
    m_linkedNote = new QLabel;
    m_linkedNote->setObjectName(QStringLiteral("MaterialHint"));
    zone->addRow(QString(), m_linkedNote);
}

void MaterialPanel::buildSurfaceSections(QVBoxLayout* col) {
    // --- Base: the color and the diffuse response ---
    QFormLayout* base = addSection(col, tr("Base"), /*expanded=*/true, nullptr, MaterialLayer::Base);
    addColorRow(base, MaterialColor::Base, tr("Base Color"),
                tr("The surface's color. With a base-color map it tints the map: white leaves the map as painted."));
    addRow(base, MaterialField::Roughness, tr("Roughness"),
           tr("How matte the surface is: 0 is mirror-smooth, 100 is fully rough. Over a roughness map it scales the map."), 0.0, 100.0, 1.0, 0);
    addRow(base, MaterialField::Metallic, tr("Metallic"),
           tr("0 is a dielectric (plastic, skin, cloth), 100 a metal, whose reflections take the base color and which has no diffuse color of its own. "
              "Over a metalness map it scales the map."),
           0.0, 100.0, 1.0, 0);
    addRow(base, MaterialField::DiffuseRoughness, tr("Diffuse Roughness"),
           tr("How rough the diffuse surface is at the micro scale (cloth, chalk, the moon): 0 is a smooth Lambertian surface, 100 flattens the shading toward the light."),
           0.0, 100.0, 1.0, 0);

    // --- Overlays: images layered over the zone's skin (tattoos, bruises, freckles, makeup) —
    // first under the zone, where the thing most often reached for is not twelve sections down.
    // The section is OverlayStack's (overlaystack.h); it edits through the viewport and says so.
    QFormLayout* overlays = addSection(col, tr("Overlays"), /*expanded=*/false, [](const MaterialView& v) { return !v.params.overlays.empty(); });
    m_overlays = new OverlayStack(m_viewport);
    connect(m_overlays, &OverlayStack::edited, this, &MaterialPanel::refresh);
    overlays->addRow(m_overlays);
    // Folded, the header says what is inside: a "+" while the zone has no overlay, else how many
    // it has. MARKS, not controls: they are children of the header button itself and let the
    // mouse through, so the header's hover runs under them and a click there folds or unfolds
    // the section like a click anywhere else on it. Both sit in the restore buttons' column
    // (placeOverlaySummary).
    if (QToolButton* header = m_sections.back().header) {
        m_overlayHeader = header;
        header->setProperty("testId", QStringLiteral("overlay.section")); // (the scripted test folds it with `ui click`)
        m_overlayAdd = new PlusMark(header);
        m_overlayAdd->setProperty("testId", QStringLiteral("overlay.header.add"));
        m_overlayCount = new QLabel(header);
        m_overlayCount->setObjectName(QStringLiteral("MaterialSectionCount"));
        m_overlayCount->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_overlayCount->setAlignment(Qt::AlignCenter);
        m_overlayCount->setProperty("testId", QStringLiteral("overlay.header.count"));
        header->installEventFilter(this); // (its Resize re-places the two marks)
        connect(header, &QToolButton::toggled, this, [this](bool) { syncOverlaySummary(); });
    }

    // --- Specular: the dielectric reflection (a metal's is its base color) ---
    QFormLayout* specular = addSection(col, tr("Specular"), /*expanded=*/true, nullptr, MaterialLayer::Specular);
    addRow(specular, MaterialField::Specular, tr("Specular"),
           tr("How much a dielectric reflects at normal incidence: 100 is the standard dielectric (4 %), skin is authored around 30-80, 0 reflects nothing. "
              "Over a specular mask it scales the mask."),
           0.0, 100.0, 1.0, 0);
    addColorRow(specular, MaterialColor::Specular, tr("Specular Color"),
                tr("A tint on the dielectric reflection — white for nearly every real material; a colored tint is the look of some stylized or coated "
                   "surfaces. The PBR Shaded mode tints its reflection with it."));
    addRow(specular, MaterialField::Ior, tr("IOR"),
           tr("The index of refraction: 1.5 is glass and the standard dielectric, water 1.33, skin about 1.4, a diamond 2.4. "
              "The viewport's reflectance follows it together with Specular: 1.5 is the 4 % that row describes, 2.4 a diamond's 17 %."),
           1.0, 3.0, 0.01, 2, 1.5);
    addRow(specular, MaterialField::Anisotropy, tr("Anisotropy"),
           tr("How much the highlight is stretched along the surface's grain (brushed metal, hair): 0 is round, 100 a line."), 0.0,
           100.0, 1.0, 0);
    addRow(specular, MaterialField::AnisotropyRotation, tr("Anisotropy Rotation"),
           tr("The grain's direction in degrees about the surface normal (0 runs along the texture's horizontal)."), 0.0, 360.0, 1.0, 0);
    // The skin extension's two lobes: rows a dual-lobe material shows and every other hides
    // (refresh). Roughness above scales both together; these set each.
    addRow(specular, MaterialField::Lobe1Roughness, tr("Lobe 1 Roughness"),
           tr("Skin's tight specular lobe: the sharper of the two reflections the PBR Shaded mode blends. Roughness scales both lobes together; "
              "this sets one. Its map is the Roughness slot (the import puts the active lobe's map there), which scales both lobes."),
           0.0, 100.0, 1.0, 0);
    addRow(specular, MaterialField::Lobe2Roughness, tr("Lobe 2 Roughness"),
           tr("Skin's broad specular lobe: the soft sheen under the tight reflection."), 0.0, 100.0, 1.0, 0);
    addRow(specular, MaterialField::LobeRatio, tr("Lobe Ratio"),
           tr("How much of the reflection is lobe 1 against lobe 2: 100 is lobe 1 alone (a single-lobe material), 0 lobe 2 alone. "
              "Skin imports around 85."),
           0.0, 100.0, 1.0, 0);


    // --- Coat: a thin clear layer over the surface ---
    QFormLayout* coat = addSection(col, tr("Top Coat"), /*expanded=*/false,
                                   [](const MaterialView& v) { return v.params.topCoat > 0.0f; }, MaterialLayer::TopCoat);
    addRow(coat, MaterialField::TopCoat, tr("Weight"), tr("A thin clear layer over the surface — the oil on skin, a varnish, car paint's lacquer. 0 is none."), 0.0,
           200.0, 5.0, 0);
    addRow(coat, MaterialField::TopCoatRoughness, tr("Roughness"), tr("How sharp the top coat's reflections are: 0 glassy, 100 dull."), 0.0, 100.0,
           1.0, 0);
    addColorRow(coat, MaterialColor::Coat, tr("Color"),
                tr("The coat's tint: what passes through it to the surface below takes this color (a candy-apple red over metal flakes); white is a clear coat."));
    addRow(coat, MaterialField::CoatIor, tr("IOR"), tr("The coat layer's index of refraction: 1.5 is the standard 4% coat, a lacquer about 1.5-1.6, water 1.33."),
           1.0, 3.0, 0.01, 2, 1.5);
    addRow(coat, MaterialField::CoatBumpStrength, tr("Bump Strength"),
           tr("How strongly the coat's own bump or normal map perturbs the coat layer (orange peel on a lacquer): 100 is the map as painted. "
              "Greyed without one. Stored for rendering engines."),
           0.0, 400.0, 5.0, 0, 100.0);
    addRow(coat, MaterialField::CoatAnisotropy, tr("Anisotropy"),
           tr("How much the coat's highlight is stretched along a grain (brushed lacquer): 0 is round."), 0.0, 100.0,
           1.0, 0);
    addRow(coat, MaterialField::CoatAnisotropyRotation, tr("Anisotropy Rotation"),
           tr("The coat grain's direction in degrees about the surface normal."), 0.0, 360.0, 1.0, 0);

    // --- Sheen: the soft fuzz of cloth and velvet ---
    QFormLayout* sheen = addSection(col, tr("Sheen"), /*expanded=*/false, [](const MaterialView& v) { return v.params.sheen > 0.0f; }, MaterialLayer::Sheen);
    addRow(sheen, MaterialField::Sheen, tr("Sheen"), tr("The soft back-scatter of fibres at grazing angles — velvet, felt, peach fuzz. 0 is none."), 0.0,
           100.0, 1.0, 0);
    addColorRow(sheen, MaterialColor::Sheen, tr("Sheen Color"), tr("The fuzz's color (white for most cloth)."));
    addRow(sheen, MaterialField::SheenRoughness, tr("Sheen Roughness"), tr("How broad the sheen is: 0 a tight rim, 100 a soft bloom over the whole surface."),
           0.0, 100.0, 1.0, 0);

    // --- Thin film: iridescence (soap bubbles, oil on water, beetle shells) ---
    QFormLayout* film = addSection(col, tr("Thin Film"), /*expanded=*/false, [](const MaterialView& v) { return v.params.thinFilm > 0.0f; }, MaterialLayer::ThinFilm);
    addRow(film, MaterialField::ThinFilm, tr("Thin Film"), tr("How much a thin surface film colors the reflection by interference. 0 is none."), 0.0, 100.0,
           1.0, 0);
    addRow(film, MaterialField::ThinFilmThickness, tr("Thickness"), tr("The film's thickness in nanometres (a soap bubble is a few hundred)."), 0.0,
           2000.0, 10.0, 0, 500.0);
    addRow(film, MaterialField::ThinFilmIor, tr("Film IOR"), tr("The film's index of refraction."), 1.0, 3.0, 0.01, 2, 1.4);
}

void MaterialPanel::buildVolumeSections(QVBoxLayout* col) {
    // --- Transmission: light through the material (glass, water, gems) ---
    QFormLayout* transmission = addSection(col, tr("Transmission"), /*expanded=*/false,
                                           [](const MaterialView& v) { return v.params.transmission > 0.0f; }, MaterialLayer::Transmission);
    addRow(transmission, MaterialField::Transmission, tr("Transmission"),
           tr("How much light passes THROUGH the material rather than scattering off it: 100 is clear glass, 0 an opaque solid. "
              "For a see-through surface drawn in the viewport today, use Opacity (Geometry)."),
           0.0, 100.0, 1.0, 0);
    addColorRow(transmission, MaterialColor::Transmission, tr("Transmission Color"),
                tr("The tint light takes on as it passes through (green bottle glass, amber)."));
    addRow(transmission, MaterialField::TransmissionDepth, tr("Depth"),
           tr("Over how many centimetres the tint is reached inside the material: 0 applies it at the surface."), 0.0, 1000.0, 1.0, 0);
    addRow(transmission, MaterialField::Dispersion, tr("Dispersion"),
           tr("How much the index of refraction varies with wavelength — a diamond's fire. 0 is none."), 0.0, 100.0, 1.0, 0);
    addFlagRow(transmission, &MaterialParams::thinWalled, tr("Thin Walled"),
               tr("The surface is a single sheet (a window pane, a leaf, a soap bubble) with no solid interior, rather than a solid object."));

    // --- Subsurface: light under the surface (skin, wax, marble, leaves) ---
    QFormLayout* subsurface = addSection(col, tr("Subsurface"), /*expanded=*/false, [](const MaterialView& v) {
        return v.maps.has(MaterialMap::Translucency) || v.maps.has(MaterialMap::TranslucencyWeight) || v.params.subsurfaceColor != glm::vec3(1.0f) ||
               v.params.subsurfaceRadiusColor != glm::vec3(1.0f) || v.params.subsurfaceAnisotropy != 0.0f;
    }, MaterialLayer::Subsurface);
    addRow(subsurface, MaterialField::Subsurface, tr("Translucency Weight"),
           tr("How much light passes under the surface and comes back softened and reddened: skin's translucency. 100 is the shader's full amount; "
              "0 is an opaque material like plastic. Scaled by the translucency map where there is one."),
           0.0, 200.0, 5.0, 0);
    addColorRow(subsurface, MaterialColor::Subsurface, tr("Translucency Color"),
                tr("The color light takes on under the surface (skin's is reddish, wax's warm, jade's green). With a translucency color map it tints the map. "
                   "Stored for rendering engines; the viewport's skin profile is fixed."));
    // The volume's scattering (OpenPBR's subsurface radius color, radius scale and anisotropy —
    // the source app's SSS Color, Scattering Measurement Distance and SSS Direction).
    addColorRow(subsurface, MaterialColor::ScatterColor, tr("Scattering Color"),
                tr("How far each color of light scatters relative to the others — red deepest in skin, so its scattering color is reddish; white scatters "
                   "all colors alike. Stored for rendering engines."));
    addRow(subsurface, MaterialField::SubsurfaceRadius, tr("Scattering Distance"),
           tr("How far light travels under the surface before it comes back, in centimetres (skin about 0.1-0.3, wax a few). Stored for rendering engines."),
           0.0, 100.0, 0.05, 2, 1.0);
    addRow(subsurface, MaterialField::SubsurfaceAnisotropy, tr("Scattering Direction"),
           tr("Which way light scatters inside: 0 evenly, towards 100 forward (through thin parts), towards -100 back. Stored for rendering engines."),
           -100.0, 100.0, 1.0, 0);

    // --- Emission: the surface as a light ---
    QFormLayout* emission = addSection(col, tr("Emission"), /*expanded=*/false, [](const MaterialView& v) {
        return v.params.emission > 0.0f || v.maps.has(MaterialMap::Emission);
    }, MaterialLayer::Emission);
    addColorRow(emission, MaterialColor::Emission, tr("Emission Color"), tr("The color the surface gives off. With an emission map it tints the map."));
    addRow(emission, MaterialField::Emission, tr("Emission"),
           tr("How brightly the surface glows: 0 is none, 1 the color at the scene's full radiance, more for a lamp or a screen."), 0.0, 100.0, 0.5, 1);
}

QFormLayout* MaterialPanel::buildGeometrySections(QVBoxLayout* col) {
    // --- Geometry: opacity, the surface maps' strengths, the sides and the smoothing ---
    QFormLayout* geometry = addSection(col, tr("Geometry"), /*expanded=*/true, nullptr, MaterialLayer::Geometry);
    addRow(geometry, MaterialField::Opacity, tr("Opacity"),
           tr("100 is solid; anything less is drawn see-through, blended over what is behind it (a cornea, a lens). A cutout mask in the texture "
              "still applies."),
           0.0, 100.0, 1.0, 0);
    addRow(geometry, MaterialField::NormalStrength, tr("Normal Strength"),
           tr("How strongly the zone's normal map perturbs the surface: 100 is the map as painted, 0 flat, more exaggerates it. Greyed without one."), 0.0,
           400.0, 5.0, 0, 100.0);
    addFlagRow(geometry, &MaterialParams::flipNormal, tr("Flip Normal"),
               tr("Reads the normal map the other way up: tick it for a map made for DirectX (often named \"DX\"), whose green channel "
                  "points down. PoseStudio reads the OpenGL kind (\"GL\") as it is. A map of the wrong kind looks pressed in where it "
                  "should stand out, as if lit from below. Greyed without a normal map."),
               "materials.flipnormal", /*needsNormalMap=*/true);
    addRow(geometry, MaterialField::BumpStrength, tr("Bump Strength"),
           tr("How strongly the zone's bump (height) map perturbs the surface: 100 is the map as painted. Greyed without one."), 0.0, 400.0, 5.0, 0,
           100.0);
    addRow(geometry, MaterialField::AmbientOcclusion, tr("Ambient Occlusion"),
           tr("How much a baked ambient-occlusion map darkens the crevices: 100 is the map as painted. Greyed without one. The viewport "
              "applies it over the occlusion it bakes at import; a renderer reads the map."),
           0.0, 100.0, 1.0, 0, 100.0);
    addFlagRow(geometry, &MaterialParams::doubleSided, tr("Double Sided"),
               tr("Render the back faces of the surface too — a leaf, a cloth card, an open shell. Stored for rendering engines; the viewport "
                  "always lights both sides."));
    // THE SMOOTHING (import/smoothnormals.h): the zone's shading normals computed here, by angle.
    addFlagRow(geometry, &MaterialParams::smoothByAngle, tr("Smooth by Angle"),
               tr("Shade the surface smoothly: PoseStudio works out its normals from its faces instead of using the file's. Faces that "
                  "meet at less than the Smoothing Angle blend into one another; sharper edges stay crisp. Off, the surface is shaded "
                  "as its file says — which may already be smooth, or faceted. On by default for a file that brings no smoothing of "
                  "its own. With matching zones edited together, the zones smoothed the same way follow."),
               "materials.smooth");
    addRow(geometry, MaterialField::SmoothAngle, tr("Smoothing Angle"),
           tr("How sharp an edge may be and still be smoothed over, in degrees: faces that meet at less than this blend, sharper edges stay crisp. "
              "60 rounds a sphere and keeps a cube's edges; 0 is faceted; 180 smooths everything. Greyed while Smooth by Angle is off."),
           0.0, 180.0, 1.0, 0);

    // --- Displacement: the surface really moved by a height map — in a render and the Rendered
    // viewport mode (scene/meshdisplace.h); the rasterized modes do not displace ---
    QFormLayout* displacement = addSection(col, tr("Displacement"), /*expanded=*/false,
                                           [](const MaterialView& v) { return v.maps.has(MaterialMap::Displacement); }, MaterialLayer::Displacement);
    addRow(displacement, MaterialField::DisplacementStrength, tr("Strength"),
           tr("How far the displacement map moves the surface, in CENTIMETRES from the map's black to its white (negative inverts it): "
              "a brick wall's joints are about 1, deep stonework 3 to 5. Greyed without a map. Shown in a render and in the "
              "Rendered viewport mode; the other viewport modes do not displace."),
           -100.0, 100.0, 0.05, 2);
    // (A sweep of the box is ten centimetres: the range is there for the odd giant, typed.)
    m_rows[at(MaterialField::DisplacementStrength)].box->setScrubSpan(10.0);
    addRow(displacement, MaterialField::DisplacementMidLevel, tr("Mid Level"),
           tr("The map value that leaves the surface where it is: at 50 black sinks and white rises by half the strength each; at 0 the map only rises."),
           0.0, 100.0, 1.0, 0, 50.0);
    addRow(displacement, MaterialField::DisplacementSubdiv, tr("Subdivision"),
           tr("How many times every triangle is cut in four before the surface is displaced: a displacement can only move the mesh's "
              "points, so a coarse mesh needs more of them. 0 moves the mesh as it is; each level is four times the triangles "
              "(a level is dropped where the mesh would pass five million). The shape itself is not rounded."),
           0.0, 5.0, 1.0, 0);
    m_rows[at(MaterialField::DisplacementSubdiv)].box->setFineStep(0.0); // a count of levels: no fine scrub

    // --- Detail: the tiled micro-detail normal over the surface (pores, weave, grain) ---
    QFormLayout* detail = addSection(col, tr("Detail"), /*expanded=*/false, [](const MaterialView& v) { return v.maps.has(MaterialMap::PoreDetail); }, MaterialLayer::Detail);
    addRow(detail, MaterialField::PoreDetail, tr("Weight"),
           tr("The strength of the Detail Normal Map, a small tiling normal map repeated across the surface — a skin's pores, a cloth's weave, "
              "leather grain. Greyed without one."),
           0.0, 99.0, 1.0, 0);
    addRow(detail, MaterialField::DetailRoughness, tr("Roughness"),
           tr("A roughness multiplier by the detail layer's weight, so the pores read a little rougher than the skin between them: 100 is none. "
              "Its map is stored for rendering engines."),
           0.0, 400.0, 5.0, 0, 100.0);
    addRow(detail, MaterialField::PoreTiles, tr("Horizontal Tiles"), tr("How many times the detail map repeats across the zone horizontally."), 1.0,
           512.0, 1.0, 0, 1.0);
    addRow(detail, MaterialField::DetailOffsetU, tr("Horizontal Offset"), tr("Shifts the detail map across the zone horizontally, in repeats."),
           -1000.0, 1000.0, 0.01, 2);
    addRow(detail, MaterialField::DetailTilesV, tr("Vertical Tiles"),
           tr("… and vertically (the detail map stretches when the two differ)."), 1.0,
           512.0, 1.0, 0, 1.0);
    addRow(detail, MaterialField::DetailOffsetV, tr("Vertical Offset"), tr("Shifts the detail map across the zone vertically, in repeats."),
           -1000.0, 1000.0, 0.01, 2);
    // (The detail layer repeats a whole number of times: the viewport floors the count.)
    m_rows[at(MaterialField::PoreTiles)].box->setFineStep(0.0);
    m_rows[at(MaterialField::DetailTilesV)].box->setFineStep(0.0);

    // --- Tiling: the material's UV repeat and shift, every map together ---
    QFormLayout* tiling = addSection(col, tr("Tiling"), /*expanded=*/false, [](const MaterialView& v) {
        return v.params.uvTiling != glm::vec2(1.0f) || v.params.uvOffset != glm::vec2(0.0f) || v.params.uvRotation != 0.0f;
    }, MaterialLayer::Tiling);
    addRow(tiling, MaterialField::TilingX, tr("Horizontal Tiles"), tr("How many times the textures repeat across the zone horizontally (a tiled floor)."),
           0.01, 1000.0, 0.1, 2, 1.0);
    addRow(tiling, MaterialField::OffsetX, tr("Horizontal Offset"), tr("Shifts the textures across the zone horizontally, in repeats."), -1000.0, 1000.0,
           0.01, 2);
    addRow(tiling, MaterialField::TilingY, tr("Vertical Tiles"), tr("How many times the textures repeat across the zone vertically."), 0.01, 1000.0, 0.1, 2,
           1.0);
    addRow(tiling, MaterialField::OffsetY, tr("Vertical Offset"), tr("Shifts the textures across the zone vertically, in repeats."), -1000.0, 1000.0, 0.01,
           2);
    addRow(tiling, MaterialField::TilingRotation, tr("Rotation"),
           tr("Turns the textures across the zone, in degrees, about the texture's origin after the tiling."), -360.0, 360.0,
           1.0, 0);
    return detail;
}

void MaterialPanel::defineSlots() {
    // THE SLOTS: each map's label and tip. A slot's thumbnail lives on the line of the value it
    // scales (attachSlots); the Detail Weight Mask scales no value and gets a row of its own
    // (buildUi).
    defineSlot(MaterialMap::BaseColor, tr("Base Color"), tr("The color map (sRGB). The Base Color swatch tints it."));
    defineSlot(MaterialMap::Normal, tr("Normal"), tr("A tangent-space normal map. Normal Strength scales it."));
    defineSlot(MaterialMap::Bump, tr("Bump"), tr("A grayscale height map. Bump Strength scales it; with a normal map both apply."));
    defineSlot(MaterialMap::Roughness, tr("Roughness"), tr("A grayscale roughness map (a chosen file's red channel), multiplied by Roughness."));
    defineSlot(MaterialMap::Metalness, tr("Metalness"), tr("A grayscale metalness map (a chosen file's red channel), multiplied by Metallic."));
    defineSlot(MaterialMap::DiffuseRoughness, tr("Diffuse Roughness"),
                  tr("A grayscale map scaling Diffuse Roughness per texel (a chosen file's red channel). Stored for rendering engines."));
    defineSlot(MaterialMap::SpecularMask, tr("Specular Mask"), tr("A grayscale map scaling the specular reflection per texel."));
    defineSlot(MaterialMap::Translucency, tr("Translucency Color"),
                  tr("The tint light takes on under the surface, painted per region (sRGB); the viewport reads its brightness as the weight too."));
    defineSlot(MaterialMap::Emission, tr("Emission"), tr("The glow's map (sRGB), tinted by Emission Color and scaled by Emission."));
    defineSlot(MaterialMap::PoreDetail, tr("Detail Normal Map"),
                  tr("A small tiling normal map layered over the surface Detail Tiles times — pores, weave, grain. Detail Weight scales it."));
    defineSlot(MaterialMap::DetailWeight, tr("Detail Weight Mask"),
                  tr("A grayscale map of where the detail layer applies and how strongly. Stored for rendering engines."));
    defineSlot(MaterialMap::TransmissionColor, tr("Transmission Color"),
                  tr("The tint map of light passing through (sRGB) — stained glass; Transmission Color tints it. Stored for rendering engines."));
    defineSlot(MaterialMap::AmbientOcclusion, tr("Ambient Occlusion"),
                  tr("A baked ambient-occlusion map (grayscale, white = open), applied by its weight over the occlusion the viewport bakes."));
    defineSlot(MaterialMap::Displacement, tr("Displacement"),
                  tr("A grayscale height map the surface is displaced by in a render and in the Rendered viewport mode: its Strength, Mid Level "
                     "and Subdivision say how."));
    defineSlot(MaterialMap::DetailRoughness, tr("Detail Roughness"),
                  tr("A grayscale map of the detail layer's roughness multiplier, tiled with it. Stored for rendering engines."));
    defineSlot(MaterialMap::Opacity, tr("Cutout"),
                  tr("A grayscale cutout mask: black is cut away, white kept — a lash card's shape, a leaf's edge."));
    defineSlot(MaterialMap::CoatWeight, tr("Top Coat Weight"), tr("A grayscale map scaling the top coat's weight per texel."));
    defineSlot(MaterialMap::CoatColor, tr("Top Coat Color"), tr("The top coat's tint map (sRGB); Top Coat Color tints it. Stored for rendering engines."));
    defineSlot(MaterialMap::CoatRoughness, tr("Top Coat Roughness"), tr("A grayscale map scaling the top coat's roughness per texel."));
    defineSlot(MaterialMap::CoatReflectivity, tr("Top Coat Reflectivity"),
                  tr("A grayscale map scaling the top coat's reflectance per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::SpecularColor, tr("Specular Color"), tr("The dielectric reflection's tint map (sRGB); Specular Color tints it. Stored for rendering engines."));
    defineSlot(MaterialMap::Anisotropy, tr("Anisotropy"), tr("A grayscale map scaling Anisotropy per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::AnisotropyRotation, tr("Anisotropy Rotation"), tr("The grain's direction per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::Lobe2Roughness, tr("Lobe 2 Roughness"), tr("A grayscale map scaling the broad lobe's roughness per texel (skin). Stored for rendering engines."));
    defineSlot(MaterialMap::LobeRatio, tr("Lobe Ratio"), tr("A grayscale map of the lobe ratio per texel (skin). Stored for rendering engines."));
    defineSlot(MaterialMap::TranslucencyWeight, tr("Translucency Weight"),
                  tr("A grayscale map of how strongly each region passes light under the surface — the ears, the nostrils, the fingertips. Stored for rendering engines."));
    defineSlot(MaterialMap::CoatBump, tr("Top Coat Bump"),
                  tr("The coat layer's own bump (height) or normal map — orange peel on a lacquer. Top Coat Bump Strength scales it. Stored for rendering engines."));
    defineSlot(MaterialMap::TransmissionWeight, tr("Transmission"), tr("A grayscale map scaling Transmission per texel — the glass panes of a window frame."));
    defineSlot(MaterialMap::CoatAnisotropy, tr("Top Coat Anisotropy"), tr("A grayscale map scaling the coat's anisotropy per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::CoatAnisotropyRotation, tr("Top Coat Anisotropy Rotation"), tr("The coat grain's direction per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::SheenWeight, tr("Sheen"), tr("A grayscale map scaling Sheen per texel."));
    defineSlot(MaterialMap::SheenColor, tr("Sheen Color"), tr("The fuzz's color map (sRGB); Sheen Color tints it."));
    defineSlot(MaterialMap::SheenRoughness, tr("Sheen Roughness"), tr("A grayscale map scaling Sheen Roughness per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::ThinFilmWeight, tr("Thin Film"), tr("A grayscale map scaling Thin Film per texel. Stored for rendering engines."));
    defineSlot(MaterialMap::ThinFilmThickness, tr("Thin Film Thickness"),
               tr("A grayscale map of the film's thickness: black is none, white the Thickness row — the swirls of a soap bubble."));
}

void MaterialPanel::attachSlots() {
    // THE LINE ITEMS' THUMBNAILS: the map beside the value it scales — a click chooses, a
    // right-click removes or restores, the pointer at rest previews.
    m_rowSlot.fill(MaterialMap::Count);
    m_colorSlot.fill(MaterialMap::Count);
    const auto slotAt = [this](MaterialField field, MaterialMap map) {
        m_rowSlot[at(field)] = map;
        attachSlot(m_rows[at(field)].field, map, m_slots[at(map)].tip);
    };
    const auto colorSlotAt = [this](MaterialColor which, MaterialMap map) {
        m_colorSlot[at(which)] = map;
        attachSlot(m_colors[at(which)].field, map, m_slots[at(map)].tip);
    };
    colorSlotAt(MaterialColor::Base, MaterialMap::BaseColor);
    slotAt(MaterialField::Roughness, MaterialMap::Roughness);
    slotAt(MaterialField::Lobe1Roughness, MaterialMap::Roughness); // (a skin's lobe 1 map IS the roughness map)
    slotAt(MaterialField::Metallic, MaterialMap::Metalness);
    slotAt(MaterialField::DiffuseRoughness, MaterialMap::DiffuseRoughness);
    slotAt(MaterialField::TopCoat, MaterialMap::CoatWeight);
    colorSlotAt(MaterialColor::Coat, MaterialMap::CoatColor);
    slotAt(MaterialField::TopCoatRoughness, MaterialMap::CoatRoughness);
    slotAt(MaterialField::CoatIor, MaterialMap::CoatReflectivity);
    slotAt(MaterialField::CoatBumpStrength, MaterialMap::CoatBump);
    colorSlotAt(MaterialColor::Specular, MaterialMap::SpecularColor);
    slotAt(MaterialField::Anisotropy, MaterialMap::Anisotropy);
    slotAt(MaterialField::AnisotropyRotation, MaterialMap::AnisotropyRotation);
    slotAt(MaterialField::Lobe2Roughness, MaterialMap::Lobe2Roughness);
    slotAt(MaterialField::LobeRatio, MaterialMap::LobeRatio);
    slotAt(MaterialField::Specular, MaterialMap::SpecularMask);
    slotAt(MaterialField::Subsurface, MaterialMap::TranslucencyWeight);
    colorSlotAt(MaterialColor::Subsurface, MaterialMap::Translucency);
    colorSlotAt(MaterialColor::Emission, MaterialMap::Emission);
    slotAt(MaterialField::Opacity, MaterialMap::Opacity);
    slotAt(MaterialField::NormalStrength, MaterialMap::Normal);
    slotAt(MaterialField::BumpStrength, MaterialMap::Bump);
    slotAt(MaterialField::PoreDetail, MaterialMap::PoreDetail); // (the weight line carries the map it scales, as Normal Strength does)
    slotAt(MaterialField::DetailRoughness, MaterialMap::DetailRoughness);
    slotAt(MaterialField::DisplacementStrength, MaterialMap::Displacement);
    slotAt(MaterialField::AmbientOcclusion, MaterialMap::AmbientOcclusion);
    colorSlotAt(MaterialColor::Transmission, MaterialMap::TransmissionColor);
    slotAt(MaterialField::Transmission, MaterialMap::TransmissionWeight);
    slotAt(MaterialField::CoatAnisotropy, MaterialMap::CoatAnisotropy);
    slotAt(MaterialField::CoatAnisotropyRotation, MaterialMap::CoatAnisotropyRotation);
    slotAt(MaterialField::Sheen, MaterialMap::SheenWeight);
    colorSlotAt(MaterialColor::Sheen, MaterialMap::SheenColor);
    slotAt(MaterialField::SheenRoughness, MaterialMap::SheenRoughness);
    slotAt(MaterialField::ThinFilm, MaterialMap::ThinFilmWeight);
    slotAt(MaterialField::ThinFilmThickness, MaterialMap::ThinFilmThickness);
}

void MaterialPanel::buildFooter(QVBoxLayout* col) {
    m_hint = new QLabel(tr("Click an object in the viewport to edit its materials. Click a surface to pick the zone under the cursor."));
    m_hint->setObjectName(QStringLiteral("MaterialHint"));
    m_hint->setWordWrap(true);
    col->addWidget(m_hint);

    // --- Reset ---
    auto* buttons = new QWidget;
    auto* buttonRow = new QHBoxLayout(buttons);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    m_resetZone = new QPushButton(tr("Reset Zone"));
    m_resetZone->setObjectName(QStringLiteral("PropertyResetButton"));
    m_resetZone->setToolTip(tr("Returns this zone's material values (and its matching zones', when editing them together) to the values the file imported with. "
                               "The texture slots are put back one by one with their arrows, and the overlays are removed with their own buttons."));
    connect(m_resetZone, &QPushButton::clicked, this, [this]() {
        if (m_viewport) {
            m_viewport->resetSelectedMaterial(linked(), /*allZones=*/false);
            refresh();
        }
    });
    m_resetAll = new QPushButton(tr("Reset All Zones"));
    m_resetAll->setObjectName(QStringLiteral("PropertyResetButton"));
    m_resetAll->setToolTip(tr("Returns every zone of the object to the material values the file imported with"));
    connect(m_resetAll, &QPushButton::clicked, this, [this]() {
        if (m_viewport) {
            m_viewport->resetSelectedMaterial(/*linked=*/true, /*allZones=*/true);
            refresh();
        }
    });
    buttonRow->addWidget(m_resetZone);
    buttonRow->addWidget(m_resetAll);
    buttonRow->addStretch(1);
    col->addWidget(buttons);
}

QFormLayout* MaterialPanel::addSection(QVBoxLayout* col, const QString& title, bool expanded,
                                       std::function<bool(const MaterialView&)> inUse, MaterialLayer layer) {
    const propertyrows::CollapsibleGroup group = propertyrows::addCollapsibleGroup(col, title, expanded);
    QWidget* const                       body = group.body;
    Section                              section;
    section.layer = layer;
    section.header = group.header;
    if (layer != MaterialLayer::Count) {
        // THE LAYER SWITCH: a checkbox at the right of the header — the section's rows keep their
        // values while the layer is off, and the body dims to say so (still editable).
        auto* toggle = new QCheckBox;
        toggle->setObjectName(QStringLiteral("PropertyLayerToggle"));
        QString meaning;
        switch (layer) {
        case MaterialLayer::Base: meaning = tr("Off: no diffuse color and no metal — a black dielectric, the state of glass or a coat alone."); break;
        case MaterialLayer::Specular: meaning = tr("Off: no dielectric reflection — a matte surface; the top coat still reflects."); break;
        case MaterialLayer::Geometry: meaning = tr("Off: the normal, bump and occlusion maps at rest — a smooth surface; opacity and Double Sided stay."); break;
        case MaterialLayer::Tiling: meaning = tr("Off: the maps as painted — one repeat, no offset, no rotation."); break;
        default: meaning = tr("Off: the layer contributes nothing."); break;
        }
        toggle->setToolTip(tr("Switch this layer on or off; its values are kept. %1").arg(meaning));
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setChecked(true);
        connect(toggle, &QCheckBox::toggled, this, [this, layer](bool on) { setLayerEnabled(layer, on); });
        auto* headerRow = new QWidget;
        auto* h = new QHBoxLayout(headerRow);
        h->setContentsMargins(0, 0, 8, 0);
        h->setSpacing(4);
        // (The header's place in the box goes to the row; the layout item that held it is ours to delete.)
        delete group.column->replaceWidget(section.header, headerRow);
        h->addWidget(section.header, 1);
        h->addWidget(toggle, 0, Qt::AlignVCenter);
        section.toggle = toggle;
        // The dimming effect is ENABLED only while the layer is off (refresh): at full opacity
        // an enabled effect draws the same pixels as none, but every repaint of one row would
        // go through the effect for the whole body.
        section.dim = new QGraphicsOpacityEffect(body);
        section.dim->setOpacity(0.45);
        section.dim->setEnabled(false);
        body->setGraphicsEffect(section.dim);
    }
    section.inUse = std::move(inUse);
    m_sections.push_back(std::move(section));
    return group.form;
}

void MaterialPanel::addRow(QFormLayout* form, MaterialField field, const QString& label, const QString& tip, double min, double max,
                           double step, int decimals, double origin) {
    const propertyrows::ScrubRow built = propertyrows::makeScrubRow(label, tip, min, max, step, decimals);
    Row&                         row = m_rows[at(field)];
    row.form = form;
    row.label = built.label;
    row.box = built.box;
    row.restore = built.restore;
    row.field = built.field;
    row.box->setOrigin(origin);
    // One undo entry per completed gesture (a scrub or a type-in), not one per value it passes
    // through: the box brackets the user's changes with these two signals.
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
    // Live: every value the box lands on sets the material.
    connect(row.box, &DragNumberBox::valueChanged, this, [this, field](double value) { setField(field, value); });
    row.restore->setToolTip(tr("Back to the value the file imported with"));
    connect(row.restore, &QToolButton::clicked, this, [this, field]() { restoreField(field); });
    form->addRow(row.label, row.field);
}

void MaterialPanel::addColorRow(QFormLayout* form, MaterialColor which, const QString& label, const QString& tip) {
    ColorRow& row = m_colors[at(which)];
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.swatch = colorswatch::makeSwatch();
    row.swatch->setToolTip(tip);
    row.swatch->setProperty("testId", QStringLiteral("materials.color")); // (the scripted `ui click`: the swatches in the tab's order)
    connect(row.swatch, &QPushButton::clicked, this, [this, which]() { pickColor(which); });
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to the color the file imported with"));
    connect(row.restore, &QToolButton::clicked, this, [this, which]() { restoreColor(which); });
    row.field = propertyrows::rowWithRestore(row.swatch, row.restore);
    form->addRow(row.label, row.field);
}

void MaterialPanel::addFlagRow(QFormLayout* form, bool MaterialParams::* member, const QString& label, const QString& tip,
                               const char* testId, bool needsNormalMap) {
    FlagRow row;
    row.member = member;
    row.needsNormalMap = needsNormalMap;
    row.box = new QCheckBox(label);
    row.box->setToolTip(tip);
    if (testId != nullptr) {
        row.box->setProperty("testId", QString::fromLatin1(testId));
    }
    connect(row.box, &QCheckBox::toggled, this, [this, member](bool on) { setFlag(member, on); });
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to what the file imported with"));
    connect(row.restore, &QToolButton::clicked, this, [this, member]() {
        if (m_view.valid) {
            setFlag(member, m_view.imported.*member);
        }
    });
    form->addRow(QString(), propertyrows::rowWithRestore(row.box, row.restore));
    m_flags.push_back(row);
}

void MaterialPanel::defineSlot(MaterialMap map, const QString& label, const QString& tip) {
    m_slots[at(map)] = SlotInfo{label, tip};
}

void MaterialPanel::addSlotRow(QFormLayout* form, MaterialMap map, const QString& label, const QString& tip) {
    // [label]  [thumb] file name — the thumbnail as every line item's, the name dimmed beside it.
    auto* rowLabel = new QLabel(label);
    m_slotRowLabels.push_back(rowLabel);
    rowLabel->setToolTip(tip);
    MaterialThumbButton* thumb = makeThumb(map, kThumbSmallPx, kThumbSmallRadiusPx, tip);
    m_rowThumbs[at(map)].push_back(thumb);
    auto* name = new QLabel;
    name->setObjectName(QStringLiteral("MaterialHint"));
    name->setToolTip(tip);
    m_slotNames[at(map)] = name;
    auto* field = new QWidget;
    auto* h = new QHBoxLayout(field);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    h->addWidget(thumb, 0, Qt::AlignVCenter);
    h->addWidget(name, 1);
    form->addRow(rowLabel, field);
}

void MaterialPanel::attachSlot(QWidget* field, MaterialMap map, const QString& tip) {
    auto* h = qobject_cast<QHBoxLayout*>(field->layout());
    if (h == nullptr) {
        return;
    }
    MaterialThumbButton* thumb = makeThumb(map, kThumbSmallPx, kThumbSmallRadiusPx, tip);
    h->insertWidget(0, thumb, 0, Qt::AlignVCenter);
    h->insertSpacing(1, 2);
    m_rowThumbs[at(map)].push_back(thumb);
}

void MaterialPanel::alignLabelColumn() {
    // ONE LABEL COLUMN for the whole tab: every section is its own form (propertyrows::
    // alignLabelColumn says what that does to the fields, and why this waits for the first show).
    std::vector<QLabel*> labels;
    for (const Row& row : m_rows) {
        labels.push_back(row.label);
    }
    for (const ColorRow& row : m_colors) {
        labels.push_back(row.label);
    }
    labels.insert(labels.end(), m_slotRowLabels.begin(), m_slotRowLabels.end());
    propertyrows::alignLabelColumn(labels);
}

} // namespace pose
