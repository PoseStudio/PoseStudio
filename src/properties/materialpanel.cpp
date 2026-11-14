/**
 * @file materialpanel.cpp
 * @brief Implementation of MaterialPanel. See materialpanel.h.
 */

#include "materialpanel.h"

#include "assetscan.h"      // pathsWithin (is the remembered save folder still in the library)
#include "constants.h"
#include "importformats.h"  // the material file's extension
#include "librarypaths.h"   // the Materials library folder
#include "materialthumbbutton.h"
#include "overlaystack.h"
#include "dragnumberbox.h"
#include "menupickerbutton.h"
#include "preferencesmanager.h"
#include "propertyrows.h"
#include "viewportwidget.h"
#include "modeldata.h" // DecodedImage (the thumbnails)

#include <QAbstractButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QUrl>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QEnterEvent>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QImage>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cmath>
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
/// A collapsible section's body margin (propertyrows::addCollapsibleGroup's form margins): how
/// far in from the section's edge its rows — and their restore buttons — end.
constexpr int kSectionMargin = 8;

/// Linear → display (sRGB) and back, per channel: the swatch shows what the screen shows.
float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
QColor displayColor(const glm::vec3& linear) {
    return QColor::fromRgbF(linearToSrgb(linear.x), linearToSrgb(linear.y), linearToSrgb(linear.z));
}
glm::vec3 linearColor(const QColor& srgb) {
    return glm::vec3(srgbToLinear(static_cast<float>(srgb.redF())), srgbToLinear(static_cast<float>(srgb.greenF())),
                     srgbToLinear(static_cast<float>(srgb.blueF())));
}

std::size_t at(MaterialField field) {
    return static_cast<std::size_t>(field);
}
std::size_t at(MaterialColor which) {
    return static_cast<std::size_t>(which);
}
std::size_t at(MaterialMap map) {
    return static_cast<std::size_t>(map);
}

/// The swatch's look: its background IS the color, the caption in a contrasting ink.
void paintSwatch(QPushButton* swatch, const QColor& c, bool enabled) {
    const bool dark = c.lightnessF() < 0.5;
    swatch->setText(enabled ? c.name(QColor::HexRgb).toUpper() : QString());
    swatch->setStyleSheet(QStringLiteral("QPushButton#MaterialColorButton { background-color: %1; color: %2; border: 1px solid #4a4b4d; "
                                         "border-radius: 4px; min-height: 20px; padding: 2px 8px; }"
                                         "QPushButton#MaterialColorButton:hover { border-color: #5b87cc; }"
                                         "QPushButton#MaterialColorButton:disabled { background-color: #2a2b2d; color: #6a6b6e; }")
                              .arg(c.name(QColor::HexRgb), dark ? QStringLiteral("#e0e0e0") : QStringLiteral("#1b1c1e")));
}

} // namespace

MaterialPanel::MaterialPanel(ViewportWidget* viewport, QWidget* parent)
    : QWidget(parent), m_viewport(viewport) {
    setObjectName(QStringLiteral("MaterialPanel"));
    buildUi();
    if (m_viewport) {
        // The selection, the zone or a material changed — by a click, undo, a project opened, one
        // of these rows: the panel follows.
        connect(m_viewport, &ViewportWidget::materialViewChanged, this, &MaterialPanel::refresh);
    }
    refresh();
}

void MaterialPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    m_scroll = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("MaterialContent"));
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(12, 12, 12, 12);
    col->setSpacing(10);

    // The heading: the object's name, and across from it the Save button.
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(8);
    m_objectName = new QLabel;
    m_objectName->setObjectName(QStringLiteral("MaterialObjectName"));
    heading->addWidget(m_objectName, 1);
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

    // --- Zone: which material group of the object the rows show ---
    QFormLayout* zone = propertyrows::addGroup(col, tr("Zone"));
    m_zonePicker = new MenuPickerButton;
    // (The HDRI picker's look, deliberately: one field style for every picker on the side panel.)
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

    // --- Base: the color and the specular response ---
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
    m_thinWalled = new QCheckBox(tr("Thin Walled"));
    m_thinWalled->setToolTip(tr("The surface is a single sheet (a window pane, a leaf, a soap bubble) with no solid interior, rather than a solid object."));
    connect(m_thinWalled, &QCheckBox::toggled, this, &MaterialPanel::setThinWalled);
    m_thinWalledRestore = propertyrows::makeRestoreButton();
    m_thinWalledRestore->setToolTip(tr("Back to what the file imported with"));
    connect(m_thinWalledRestore, &QToolButton::clicked, this, [this]() {
        if (m_view.valid) {
            setThinWalled(m_view.imported.thinWalled);
        }
    });
    transmission->addRow(QString(), propertyrows::rowWithRestore(m_thinWalled, m_thinWalledRestore));

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

    // --- Geometry: opacity, the detail maps' strengths, the UV placement ---
    QFormLayout* geometry = addSection(col, tr("Geometry"), /*expanded=*/true, nullptr, MaterialLayer::Geometry);
    addRow(geometry, MaterialField::Opacity, tr("Opacity"),
           tr("100 is solid; anything less is drawn see-through, blended over what is behind it (a cornea, a lens). A cutout mask in the texture "
              "still applies."),
           0.0, 100.0, 1.0, 0);
    addRow(geometry, MaterialField::NormalStrength, tr("Normal Strength"),
           tr("How strongly the zone's normal map perturbs the surface: 100 is the map as painted, 0 flat, more exaggerates it. Greyed without one."), 0.0,
           400.0, 5.0, 0, 100.0);
    addRow(geometry, MaterialField::BumpStrength, tr("Bump Strength"),
           tr("How strongly the zone's bump (height) map perturbs the surface: 100 is the map as painted. Greyed without one."), 0.0, 400.0, 5.0, 0,
           100.0);
    addRow(geometry, MaterialField::AmbientOcclusion, tr("Ambient Occlusion"),
           tr("How much a baked ambient-occlusion map darkens the crevices: 100 is the map as painted. Greyed without one. The viewport "
              "applies it over the occlusion it bakes at import; a renderer reads the map."),
           0.0, 100.0, 1.0, 0, 100.0);
    m_doubleSided = new QCheckBox(tr("Double Sided"));
    m_doubleSided->setToolTip(tr("Render the back faces of the surface too — a leaf, a cloth card, an open shell. Stored for rendering engines; the viewport "
                                "always lights both sides."));
    connect(m_doubleSided, &QCheckBox::toggled, this, &MaterialPanel::setDoubleSided);
    m_doubleSidedRestore = propertyrows::makeRestoreButton();
    m_doubleSidedRestore->setToolTip(tr("Back to what the file imported with"));
    connect(m_doubleSidedRestore, &QToolButton::clicked, this, [this]() {
        if (m_view.valid) {
            setDoubleSided(m_view.imported.doubleSided);
        }
    });
    geometry->addRow(QString(), propertyrows::rowWithRestore(m_doubleSided, m_doubleSidedRestore));
    // THE SMOOTHING (import/smoothnormals.h): the zone's shading normals computed here, by angle.
    m_smoothByAngle = new QCheckBox(tr("Smooth by Angle"));
    m_smoothByAngle->setToolTip(tr("Shade the surface smoothly: PoseStudio works out its normals from its faces instead of using the file's. Faces that "
                                   "meet at less than the Smoothing Angle blend into one another; sharper edges stay crisp. Off, the surface is shaded "
                                   "as its file says — which may already be smooth, or faceted. On by default for a file that brings no smoothing of "
                                   "its own. With matching zones edited together, the zones smoothed the same way follow."));
    m_smoothByAngle->setProperty("testId", QStringLiteral("materials.smooth"));
    connect(m_smoothByAngle, &QCheckBox::toggled, this, &MaterialPanel::setSmoothByAngle);
    m_smoothByAngleRestore = propertyrows::makeRestoreButton();
    m_smoothByAngleRestore->setToolTip(tr("Back to what the file imported with"));
    connect(m_smoothByAngleRestore, &QToolButton::clicked, this, [this]() {
        if (m_view.valid) {
            setSmoothByAngle(m_view.imported.smoothByAngle);
        }
    });
    geometry->addRow(QString(), propertyrows::rowWithRestore(m_smoothByAngle, m_smoothByAngleRestore));
    addRow(geometry, MaterialField::SmoothAngle, tr("Smoothing Angle"),
           tr("How sharp an edge may be and still be smoothed over, in degrees: faces that meet at less than this blend, sharper edges stay crisp. "
              "60 rounds a sphere and keeps a cube's edges; 0 is faceted; 180 smooths everything. Greyed while Smooth by Angle is off."),
           0.0, 180.0, 1.0, 0);

    // --- Displacement: a renderer's tessellated displacement (stored) ---
    QFormLayout* displacement = addSection(col, tr("Displacement"), /*expanded=*/false,
                                           [](const MaterialView& v) { return v.maps.has(MaterialMap::Displacement); }, MaterialLayer::Displacement);
    addRow(displacement, MaterialField::DisplacementStrength, tr("Strength"),
           tr("How far the displacement map pushes the surface, in centimetres from the map's black to its white (negative inverts it). "
              "Greyed without a map. Stored for rendering engines, which subdivide and displace the mesh; the viewport does not."),
           -100.0, 100.0, 0.05, 2);
    addRow(displacement, MaterialField::DisplacementMidLevel, tr("Mid Level"),
           tr("The map value that leaves the surface where it is: at 50 black sinks and white rises by half the strength each; at 0 the map only rises. "
              "Stored for rendering engines."),
           0.0, 100.0, 1.0, 0, 50.0);
    addRow(displacement, MaterialField::DisplacementSubdiv, tr("Subdivision"),
           tr("How many times a renderer subdivides the mesh before displacing it (0 none, 2-3 for fine detail). Stored for rendering engines."),
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

    // --- THE SLOTS: each map's label and tip. A slot's thumbnail lives on the line of the value it
    // scales (attachSlot below); the Detail Weight Mask scales no value and gets a row of its own. ---
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
                  tr("A grayscale height map a renderer displaces the mesh by: Displacement Strength and Mid Level scale it. Stored for rendering engines."));
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
    addSlotRow(detail, MaterialMap::DetailWeight, tr("Weight Mask"), m_slots[at(MaterialMap::DetailWeight)].tip);

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

    col->addStretch(1);
    scroll->setWidget(content);
}

QFormLayout* MaterialPanel::addSection(QVBoxLayout* col, const QString& title, bool expanded,
                                       std::function<bool(const MaterialView&)> inUse, MaterialLayer layer) {
    QFormLayout* form = propertyrows::addCollapsibleGroup(col, title, expanded);
    Section      section;
    section.layer = layer;
    // The header is the builder's: the body's parent frame holds it.
    QWidget* body = form->parentWidget();
    QWidget* box = body != nullptr ? body->parentWidget() : nullptr;
    if (box != nullptr) {
        section.header = box->findChild<QToolButton*>(QStringLiteral("PropertyCollapsibleHeader"));
    }
    if (layer != MaterialLayer::Count && section.header != nullptr && box != nullptr) {
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
        if (auto* boxCol = qobject_cast<QVBoxLayout*>(box->layout())) {
            boxCol->replaceWidget(section.header, headerRow);
        }
        h->addWidget(section.header, 1);
        h->addWidget(toggle, 0, Qt::AlignVCenter);
        section.toggle = toggle;
        section.dim = new QGraphicsOpacityEffect(body);
        section.dim->setOpacity(1.0);
        body->setGraphicsEffect(section.dim);
    }
    section.inUse = std::move(inUse);
    m_sections.push_back(std::move(section));
    return form;
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

void MaterialPanel::addRow(QFormLayout* form, MaterialField field, const QString& label, const QString& tip, double min, double max,
                           double step, int decimals, double origin) {
    Row& row = m_rows[at(field)];
    row.form = form;
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.box = new DragNumberBox;
    row.box->setRange(min, max);
    row.box->setSingleStep(step);
    row.box->setDecimals(decimals);
    row.box->setOrigin(origin);
    row.box->setToolTip(tip);
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
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to the value the file imported with"));
    connect(row.restore, &QToolButton::clicked, this, [this, field]() { restoreField(field); });
    row.field = propertyrows::rowWithRestore(row.box, row.restore);
    form->addRow(row.label, row.field);
}

void MaterialPanel::addColorRow(QFormLayout* form, MaterialColor which, const QString& label, const QString& tip) {
    ColorRow& row = m_colors[at(which)];
    row.label = new QLabel(label);
    row.label->setToolTip(tip);
    row.swatch = new QPushButton;
    row.swatch->setObjectName(QStringLiteral("MaterialColorButton"));
    row.swatch->setCursor(Qt::PointingHandCursor);
    row.swatch->setToolTip(tip);
    connect(row.swatch, &QPushButton::clicked, this, [this, which]() { pickColor(which); });
    row.restore = propertyrows::makeRestoreButton();
    row.restore->setToolTip(tr("Back to the color the file imported with"));
    connect(row.restore, &QToolButton::clicked, this, [this, which]() { restoreColor(which); });
    row.field = propertyrows::rowWithRestore(row.swatch, row.restore);
    form->addRow(row.label, row.field);
}

QPixmap MaterialPanel::slotThumbnail(MaterialMap map) {
    return slotPixmap(map, kThumbPx, m_thumbs);
}

QPixmap MaterialPanel::slotPreview(MaterialMap map) {
    return slotPixmap(map, kPreviewPx, m_previews);
}

QPixmap MaterialPanel::slotPixmap(MaterialMap map, int px, std::unordered_map<const DecodedImage*, Thumb>& cache) {
    if (!m_viewport) {
        return {};
    }
    int                                 channel = 0;
    std::shared_ptr<const DecodedImage> image = m_viewport->materialSlotImage(map, &channel);
    if (!image || image->width == 0 || image->height == 0) {
        return {};
    }
    // A roughness / metalness map is shown through the channel it is read from, as grey.
    const bool grey = map == MaterialMap::Roughness || map == MaterialMap::Metalness || map == MaterialMap::DiffuseRoughness ||
                      map == MaterialMap::CoatWeight || map == MaterialMap::CoatRoughness || map == MaterialMap::CoatReflectivity ||
                      map == MaterialMap::TranslucencyWeight || map == MaterialMap::Anisotropy || map == MaterialMap::AnisotropyRotation ||
                      map == MaterialMap::Lobe2Roughness || map == MaterialMap::LobeRatio || map == MaterialMap::DetailWeight ||
                      map == MaterialMap::DetailRoughness || map == MaterialMap::Displacement || map == MaterialMap::AmbientOcclusion ||
                      map == MaterialMap::TransmissionWeight || map == MaterialMap::CoatAnisotropy || map == MaterialMap::CoatAnisotropyRotation ||
                      map == MaterialMap::SheenWeight || map == MaterialMap::SheenRoughness || map == MaterialMap::ThinFilmWeight ||
                      map == MaterialMap::ThinFilmThickness;
    const int  key = grey ? channel : -1;
    if (auto it = cache.find(image.get()); it != cache.end()) {
        if (!it->second.image.expired() && it->second.channel == key) {
            return it->second.pixmap;
        }
        cache.erase(it);
    }
    if (cache.size() >= 64) {
        cache.clear();
    }
    // Strided nearest sample into a small image (at most four times the thumbnail each way), then
    // a smooth scale to size: a 4096-square map is read at 240-square, not 16 million texels.
    const qreal dpr = devicePixelRatioF();
    const int   target = std::max(1, static_cast<int>(std::lround(px * dpr)));
    const int   sw = static_cast<int>(std::min<uint32_t>(image->width, static_cast<uint32_t>(target * 4)));
    const int   sh = static_cast<int>(std::min<uint32_t>(image->height, static_cast<uint32_t>(target * 4)));
    QImage      small(sw, sh, QImage::Format_RGBA8888);
    const int   ch = std::clamp(channel, 0, 3);
    for (int y = 0; y < sh; ++y) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * image->height) / sh);
        uchar*         dst = small.scanLine(y);
        for (int x = 0; x < sw; ++x) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * image->width) / sw);
            const uint8_t* px = &image->pixels[(static_cast<size_t>(sy) * image->width + sx) * 4];
            if (grey) {
                dst[0] = dst[1] = dst[2] = px[ch];
            } else {
                dst[0] = px[0];
                dst[1] = px[1];
                dst[2] = px[2];
            }
            dst[3] = 255; // (a cutout's alpha is shown as the base color's own picture)
            dst += 4;
        }
    }
    // Filled to the square (a map is a texture, not a picture to letterbox); the button rounds
    // and frames it as it paints.
    QImage  scaled = small.scaled(target, target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    QPixmap pixmap = QPixmap::fromImage(scaled);
    cache[image.get()] = Thumb{image, key, pixmap};
    return pixmap;
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

MaterialThumbButton* MaterialPanel::makeThumb(MaterialMap map, int px, int radius, const QString& tip) {
    auto* thumb = new MaterialThumbButton(px, radius);
    thumb->setToolTip(tip);
    connect(thumb, &QAbstractButton::clicked, this, [this, map]() { chooseTexture(map); });
    thumb->setPreview([this, map]() { return slotPreview(map); }, QString());
    thumb->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(thumb, &QWidget::customContextMenuRequested, this, [this, map, thumb](const QPoint& pos) { showThumbMenu(map, thumb->mapToGlobal(pos)); });
    return thumb;
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

void MaterialPanel::showThumbMenu(MaterialMap map, const QPoint& globalPos) {
    if (!m_view.valid) {
        return;
    }
    const std::string& source = m_view.params.maps[at(map)].source;
    const bool         has = m_view.maps.present[at(map)];
    QMenu              menu(this);
    QAction* choose = menu.addAction(tr("Choose Image…"));
    QAction* remove = menu.addAction(tr("Remove"));
    remove->setEnabled(has);
    QAction* restore = menu.addAction(tr("Restore Imported Map"));
    restore->setEnabled(source != m_view.imported.maps[at(map)].source);
    menu.addSeparator();
    // The map's folder in the system's file browser (Qt opens a folder portably; an embedded
    // image has none).
    const QString path = QString::fromStdString(m_view.mapPaths[at(map)]);
    QAction*      openLocation = menu.addAction(tr("Open File Location"));
    openLocation->setEnabled(has && !path.isEmpty() && QFileInfo::exists(path));
    QAction* picked = menu.exec(globalPos);
    if (picked == openLocation) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        return;
    }
    if (picked == choose) {
        chooseTexture(map);
    } else if (picked == remove) {
        setTexture(map, QString());
    } else if (picked == restore) {
        setTexture(map, QString::fromLatin1(kImportedTexture));
    }
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
    if (m_colorDialog) {
        m_colorDialog->raise();
        m_colorDialog->activateWindow();
        return;
    }
    if (!m_viewport->beginMaterialEdit()) {
        return;
    }
    m_colorPicking = which;
    m_colorAtOpen = displayColor(materialColorValue(m_view.params, which));
    // The app's own themed dialog (the native one ignores the dark theme), non-modal so the
    // color can be judged on the figure while it changes.
    m_colorDialog = new QColorDialog(m_colorAtOpen, this);
    m_colorDialog->setOption(QColorDialog::DontUseNativeDialog, true);
    m_colorDialog->setWindowTitle(m_colors[at(which)].label ? m_colors[at(which)].label->text() : tr("Color"));
    m_colorDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_colorDialog, &QColorDialog::currentColorChanged, this, [this](const QColor& c) { setColor(m_colorPicking, c); });
    connect(m_colorDialog, &QColorDialog::finished, this, [this](int result) {
        if (result != QDialog::Accepted) {
            setColor(m_colorPicking, m_colorAtOpen); // Cancel: the color as it was when the dialog opened
        }
        if (m_viewport) {
            m_viewport->endMaterialEdit(); // one undo entry for the whole pick
        }
        m_colorDialog = nullptr;
        refresh();
    });
    m_colorDialog->open();
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

void MaterialPanel::setThinWalled(bool on) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    p.thinWalled = on;
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::setDoubleSided(bool on) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    p.doubleSided = on;
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::setSmoothByAngle(bool on) {
    if (!m_viewport || !m_view.valid || !m_viewport->beginMaterialEdit()) {
        return;
    }
    MaterialParams p = m_viewport->materialView().params;
    p.smoothByAngle = on;
    m_viewport->setSelectedMaterial(p, linked());
    m_viewport->endMaterialEdit();
    refresh();
}

void MaterialPanel::chooseTexture(MaterialMap map) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_TEXTURE_DIR);
    const QString       start = prefs.rememberedDirectory(key, QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    const QString       slot = m_slots[at(map)].label;
    const QString       path = QFileDialog::getOpenFileName(this, tr("Choose a %1 map").arg(slot), start,
                                                            tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.tga *.webp *.gif);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    prefs.setValue(key, QFileInfo(path).absolutePath());
    setTexture(map, path);
}

void MaterialPanel::setTexture(MaterialMap map, const QString& source) {
    if (!m_viewport || !m_view.valid) {
        return;
    }
    if (!m_viewport->setMaterialTexture(map, source) && !source.isEmpty() && source != QLatin1String(kImportedTexture)) {
        QMessageBox::warning(this, tr("Materials"), tr("The image could not be read:\n%1").arg(source));
    }
    refresh(); // (a section the map brings into use opens itself: refresh)
}

void MaterialPanel::saveMaterial() {
    if (!m_viewport || !m_view.valid || m_view.zone < 0) {
        return;
    }
    // Where: the Materials library — the folder of it saved into last, while that is still
    // inside the library (a category is saved into again and again), else its root.
    PreferencesManager& prefs = PreferencesManager::instance();
    const QString       key = QString::fromLatin1(Constants::PREF_LAST_MATERIAL_DIR);
    const QString       library = LibraryPaths::ensureMaterialsDirectory();
    QString             start = prefs.rememberedDirectory(key, library);
    if (AssetScan::pathsWithin({start}, {library}).isEmpty()) {
        start = library;
    }
    // The name offered: the zone's, less what a file name cannot hold.
    QString suggested = m_view.zone < static_cast<int>(m_view.zones.size())
                            ? QString::fromStdString(m_view.zones[static_cast<std::size_t>(m_view.zone)])
                            : tr("Material");
    static const QString kForbidden = QStringLiteral("\\/:*?\"<>|");
    for (QChar& c : suggested) {
        if (kForbidden.contains(c)) {
            c = QLatin1Char('-');
        }
    }
    const QString extension = ImportFormats::materialFileExtension();
    QString       path = QFileDialog::getSaveFileName(this, tr("Save Material"), QDir(start).filePath(suggested + extension),
                                                      tr("PoseStudio Material (*%1)").arg(extension));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(extension, Qt::CaseInsensitive)) {
        path += extension;
    }
    const QFileInfo file(path);
    prefs.setValue(key, file.absolutePath());

    // The icon lies beside the file under its name. (The file dialog has already asked about
    // replacing the FILE.) An icon that is there is the user's to keep or to have made again.
    const QString iconPath = file.absoluteDir().filePath(file.completeBaseName() + QStringLiteral(".png"));
    bool          makeIcon = true;
    if (QFileInfo::exists(iconPath)) {
        QMessageBox box(QMessageBox::Question, tr("Save Material"),
                        tr("\"%1\" already has an icon.").arg(file.completeBaseName()), QMessageBox::NoButton, this);
        box.setInformativeText(tr("Recreate the icon from the material as it is now, and overwrite the old one?"));
        QPushButton* recreate = box.addButton(tr("Recreate Icon"), QMessageBox::YesRole);
        box.addButton(tr("Keep Existing Icon"), QMessageBox::NoRole);
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(recreate);
        box.exec();
        if (box.clickedButton() == cancel || box.clickedButton() == nullptr) {
            return; // nothing is saved
        }
        makeIcon = box.clickedButton() == recreate;
    }

    // The textures go WITH the material — copied into a `textures` folder beside it — unless
    // Preferences > Assets says to name them where they are (assets/assetstore.h).
    const bool         copyTextures = prefs.getValue(QString::fromLatin1(Constants::PREF_MATERIAL_COPY_TEXTURES), true).toBool();
    QString            error;
    MaterialSaveReport report;
    if (!m_viewport->saveMaterialFile(path, copyTextures, &error, &report)) {
        QMessageBox::warning(this, tr("Save Material"), error);
        return;
    }
    bool iconFailed = false;
    if (makeIcon) {
        // A render engine traces the ball, which takes a moment (the first time on a graphics
        // card, several seconds): the window waits behind a note, not a frozen button.
        QProgressDialog busy(tr("Rendering the material's icon..."), QString(), 0, 0, this);
        busy.setWindowTitle(tr("Save Material"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);
        busy.setCancelButton(nullptr);
        busy.show();
        QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const QImage icon = m_viewport->renderMaterialIcon(Constants::MATERIAL_ICON_SIZE);
        busy.close();
        iconFailed = icon.isNull() || !icon.save(iconPath, "PNG");
    }
    emit materialSaved(path);

    QStringList notes;
    if (iconFailed) {
        notes << tr("The icon could not be written, so the material will not be listed in the Materials Library until it has one "
                    "(an image named like the file, beside it).");
    }
    if (report.notCopied > 0) {
        notes << tr("%n texture file(s) could not be copied into the material's \"textures\" folder. The material refers to "
                    "them where they are, and will not find them on another computer.", nullptr, report.notCopied);
    }
    if (report.leftOut > 0) {
        notes << (copyTextures
                      ? tr("%n map(s) of this zone could not be written out as files: the saved material does not include them.",
                           nullptr, report.leftOut)
                      : tr("%n map(s) of this zone exist only inside the model's file and have no file of their own to refer to: "
                           "the saved material does not include them. (With \"Copy texture files when saving a material\" on, in "
                           "Preferences > Assets, they are written out beside the material.)", nullptr, report.leftOut));
    }
    if (!notes.isEmpty()) {
        QMessageBox::information(this, tr("Save Material"),
                                 tr("The material was saved as \"%1\".").arg(file.fileName()) + QStringLiteral("\n\n") +
                                     notes.join(QStringLiteral("\n\n")));
    }
}

void MaterialPanel::applyMaterialFile(const QString& path) {
    if (!m_viewport) {
        return;
    }
    // A MATERIALX document may hold several materials: which one is the user's to say.
    QString material;
    QString error;
    if (pose::ImportFormats::isMaterialXFile(path)) {
        const QStringList names = m_viewport->materialNamesIn(path, &error);
        if (names.isEmpty()) {
            QMessageBox::information(this, tr("Apply Material"), error);
            return;
        }
        material = names.first();
        if (names.size() > 1) {
            bool chosen = false;
            material = QInputDialog::getItem(this, tr("Apply Material"),
                                             tr("\"%1\" holds %n material(s). Apply which one?", nullptr, static_cast<int>(names.size()))
                                                 .arg(QFileInfo(path).fileName()),
                                             names, 0, /*editable=*/false, &chosen);
            if (!chosen) {
                return;
            }
        }
    }
    int         missing = 0;
    QStringList notes;
    if (!m_viewport->applyMaterialFile(path, &error, &missing, nullptr, material, &notes)) {
        QMessageBox::information(this, tr("Apply Material"), error);
        return;
    }
    refresh();
    QStringList said;
    if (missing > 0) {
        said << tr("%n texture file(s) of \"%1\" could not be found — not where the material says, and not by name "
                   "beside it or anywhere in its library. Those slots were left empty.", nullptr, missing)
                    .arg(QFileInfo(path).fileName());
    }
    if (!notes.isEmpty()) {
        // What a MaterialX document describes that a PoseStudio material cannot hold.
        said << tr("Not everything in this material could be carried over:") + QStringLiteral("\n\n") + notes.join(QStringLiteral("\n\n"));
    }
    if (!said.isEmpty()) {
        QMessageBox box(missing > 0 ? QMessageBox::Warning : QMessageBox::Information, tr("Apply Material"), said.join(QStringLiteral("\n\n")),
                        QMessageBox::Ok, this);
        box.exec();
    }
}

bool MaterialPanel::linked() const {
    return m_linked != nullptr && m_linked->isChecked();
}

void MaterialPanel::refresh() {
    m_view = m_viewport ? m_viewport->materialView() : MaterialView{};
    const MaterialView& view = m_view;

    m_objectName->setText(view.valid ? QString::fromStdString(view.modelName) : tr("No object selected"));
    m_save->setEnabled(view.valid && view.zone >= 0);
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
        paintSwatch(row.swatch, c, view.valid);
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
            on = on && view.maps.has(MaterialMap::Displacement);
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
                // attenuation imports above it, and the range follows so the value is not cut.
                const double top = std::max(100.0, std::ceil(static_cast<double>(materialFieldValue(view.params, field))));
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
    {
        const QSignalBlocker blocker(m_thinWalled);
        m_thinWalled->setChecked(view.valid && view.params.thinWalled);
    }
    m_thinWalled->setEnabled(view.valid);
    m_thinWalledRestore->setEnabled(view.valid);
    {
        const QSignalBlocker blocker(m_doubleSided);
        m_doubleSided->setChecked(view.valid && view.params.doubleSided);
    }
    m_doubleSided->setEnabled(view.valid);
    m_doubleSidedRestore->setEnabled(view.valid);
    {
        const QSignalBlocker blocker(m_smoothByAngle);
        m_smoothByAngle->setChecked(view.valid && view.params.smoothByAngle);
    }
    m_smoothByAngle->setEnabled(view.valid);
    m_smoothByAngleRestore->setEnabled(view.valid);

    refreshSlots();
    if (m_overlays != nullptr) {
        m_overlays->setView(view);
    }

    // A section opens as its material COMES INTO USE — a zone newly shown that uses it, or a map
    // or value arriving by any path (the tab, a script, undo, a project) — and closes never: the
    // user's folds stand.
    const std::string shown = view.valid ? view.modelName + "#" + std::to_string(view.zone) : std::string();
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
            section.dim->setOpacity(enabled ? 1.0 : 0.45);
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

void MaterialPanel::alignLabelColumn() {
    // ONE LABEL COLUMN for the whole tab: every section is its own form, and left to themselves
    // their label columns differ by the longest label each holds, so the fields start at a
    // different x from section to section — give every label the widest label's width. Done at
    // the first show, when the stylesheet's 12 px font is on the labels: measured at construction
    // they wear the application's larger default font and the column came out half again too wide.
    std::vector<QLabel*> labels;
    for (const Row& row : m_rows) {
        if (row.label != nullptr) {
            labels.push_back(row.label);
        }
    }
    for (const ColorRow& row : m_colors) {
        if (row.label != nullptr) {
            labels.push_back(row.label);
        }
    }
    for (QLabel* label : m_slotRowLabels) {
        labels.push_back(label);
    }
    int widest = 0;
    for (QLabel* label : labels) {
        label->ensurePolished();
        widest = std::max(widest, label->fontMetrics().horizontalAdvance(label->text()) + 2);
    }
    for (QLabel* label : labels) {
        label->setMinimumWidth(widest);
    }
}

void MaterialPanel::refreshSlots() {
    // (Developer levers for the UI shot: POSESTUDIO_MATERIALS_SCROLL_END scrolls to the bottom,
    // POSESTUDIO_MATERIALS_SCROLL=<0..1> to that fraction of the way down.)
    static const bool  scrollEnd = qEnvironmentVariableIsSet("POSESTUDIO_MATERIALS_SCROLL_END");
    static const float scrollFrac = qEnvironmentVariableIsSet("POSESTUDIO_MATERIALS_SCROLL") ? qEnvironmentVariable("POSESTUDIO_MATERIALS_SCROLL").toFloat() : -1.0f;
    if ((scrollEnd || scrollFrac >= 0.0f) && m_scroll) {
        const float frac = scrollEnd ? 1.0f : scrollFrac;
        QTimer::singleShot(0, m_scroll, [scroll = m_scroll, frac]() {
            scroll->verticalScrollBar()->setValue(static_cast<int>(std::lround(frac * scroll->verticalScrollBar()->maximum())));
        });
    }
    const MaterialView& view = m_view;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const std::string& source = view.params.maps[i].source;
        const bool         has = view.valid && view.maps.present[i];
        QString            text;
        if (!view.valid) {
            text.clear();
        } else if (!has) {
            text = tr("none");
        } else {
            text = QString::fromStdString(view.mapNames[i]);
            if (text.isEmpty()) {
                text = tr("embedded");
            }
        }
        // The tooltip: what is bound (and where it came from), then what the slot is for.
        const QString bound = has ? (source == kImportedTexture ? tr("%1 (imported with the file)").arg(text) : QString::fromStdString(source))
                                  : tr("%1: none").arg(m_slots[i].label);
        const QString tip = tr("%1\n%2\nClick to choose an image file; right-click to remove it, restore the imported map or open its folder.")
                                .arg(bound, m_slots[i].tip);
        const QPixmap thumbnail = has ? slotThumbnail(static_cast<MaterialMap>(i)) : QPixmap();
        for (MaterialThumbButton* small : m_rowThumbs[i]) {
            small->setImage(thumbnail);
            small->setPreview([this, i]() { return slotPreview(static_cast<MaterialMap>(i)); }, text);
            small->setEnabled(view.valid);
            small->setToolTip(tip);
        }
        if (QLabel* name = m_slotNames[i]) {
            const QFontMetrics metrics(name->font());
            name->setText(metrics.elidedText(text, Qt::ElideMiddle, 150));
            name->setToolTip(tip);
            name->setEnabled(view.valid);
        }
    }
}

} // namespace pose
