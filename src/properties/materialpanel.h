// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file materialpanel.h
 * @brief The "Materials" properties tab: the selected object's material zones as sliders, color
 *        swatches and texture slots on PoseStudio's one canonical material.
 *
 * A self-contained docker panel (its own module, like TransformPanel), built from the shared
 * builders (propertyrows.h, colorswatch.h). With an object selected in the viewport — a figure,
 * an OBJ, any imported model with meshes — it shows, top to bottom:
 *
 *   the heading   the object's name, and across from it Import (a `.mtlx` or `.psm` material put
 *                 on the shown zone) and Save (the shown zone's material as a `.psm` file with
 *                 its ball icon, into the Materials library)
 *   Zone          a picker of the object's material ZONES (one per material group: a figure's
 *                 Torso, Face, Irises …; an OBJ's MTL materials) and "Edit matching zones together"
 *
 * and then the selected zone's material (materialparams.h — the OpenPBR-style parameter set
 * every importer fills and every renderer reads) in COLLAPSIBLE sections, the surface and what
 * lies on it first, then the volume, then the shape:
 *
 *   Base          Base Color, Roughness, Metallic, Diffuse Roughness
 *   Overlays      images layered over the zone's maps — tattoos, bruises, freckles, makeup — one
 *                 card each (OverlayStack, overlaystack.h)
 *   Specular      Specular (0-100 %, 100 = the standard dielectric), Specular Color, IOR,
 *                 Anisotropy + Rotation, and for a dual-lobe material (skin) the two lobes' rows
 *   Top Coat      Weight, Roughness, Color, IOR, Bump Strength, Anisotropy + Rotation
 *   Sheen         Sheen, Sheen Color, Sheen Roughness
 *   Thin Film     Thin Film, Thickness, Film IOR
 *   Transmission  Transmission, its Color, Depth, Dispersion, Thin Walled
 *   Subsurface    Translucency Weight and Color, Scattering Color, Distance and Direction
 *   Emission      Emission Color, Emission
 *   Geometry      Opacity, Normal Strength, Flip Normal, Bump Strength, Ambient Occlusion, Double
 *                 Sided, Smooth by Angle + Smoothing Angle
 *   Displacement  Strength, Mid Level, Subdivision
 *   Detail        the tiled micro-detail normal: Weight, Roughness, tiling and offset, a weight mask
 *   Tiling        the material's UV repeat, offset and rotation
 *
 * above Reset Zone / Reset All Zones. Every section but Overlays carries a LAYER SWITCH in its
 * header: off, the layer contributes nothing and its rows keep their values (the body dims).
 * Base, Specular and Geometry start open; the others open by themselves as a zone COMES TO USE
 * them (a coat on skin, an emission on a lamp, a map chosen into them) and are never closed by
 * the panel. A value or map the viewport does not draw is edited and saved all the same — its
 * tooltip says "stored for rendering engines".
 *
 * THE TEXTURE SLOTS have no list of their own: a slot's small THUMBNAIL sits on the line of the
 * value its map scales (the base color map before the Base Color swatch, the roughness map
 * before Roughness), or — scaling no value — on a row of its own. A click chooses an image
 * file, a right-click offers remove / restore the imported map / open its folder, and the pointer
 * at rest shows a larger preview; a row's restore arrow puts its map back with its value.
 *
 * Clicking a surface in the viewport selects the zone under the cursor (the joint pick and the
 * object click both report it), so the tab follows what the user touches. "Edit matching zones
 * together" (on by default) applies a VALUE edit to every zone that was imported with the same
 * material — a figure's skin zones in one go — while the eyes, mouth and nails stay their own;
 * a texture slot is always the zone's own.
 *
 * Live both ways: a scrub repacks the zone's material entry (no GPU upload) and the next frame
 * shows it; a texture choice decodes the file and rebinds the zone; the rows follow the
 * viewport whenever the selection, the zone or a material changes by other means — a click,
 * undo, a project opened (ViewportWidget::materialViewChanged).
 *
 * Undo: every completed gesture (a scrub, a type-in, a color pick, a checkbox, a layer switch,
 * a texture choice or clear, a row's restore button, a material applied from a file, Reset Zone,
 * Reset All Zones) is ONE entry on the viewport's unified undo stack, bracketed
 * begin/endMaterialEdit. The restore buttons return a value or a slot to what the file
 * imported, never to a generic default. A project file carries the zones that differ from the
 * import, their texture slots included.
 *
 * ONE CLASS OVER FOUR TRANSLATION UNITS, each touching the others only through the viewport,
 * the mirrored view (m_view) and refresh():
 *   materialpanel.cpp        the core — the mirror (refresh) and the edits of a zone's values
 *   materialpanel_build.cpp  the construction — sections, rows, the slots' names and places
 *   materialpanel_slots.cpp  the texture slots — thumbnail pictures and their cache, the
 *                            thumbnails' menu and file dialog, the slots' part of the mirror
 *   materialpanel_files.cpp  material files — Save (with its icon), Import, apply
 */

#ifndef MATERIALPANEL_H
#define MATERIALPANEL_H

#include "colorswatch.h"
#include "materialparams.h"

#include <QColor>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QFormLayout;
class QGraphicsOpacityEffect;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace pose {

class DragNumberBox;
class MaterialThumbButton;
class MenuPickerButton;
class OverlayStack;
class ViewportWidget;
struct DecodedImage;

class MaterialPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport whose selected object this panel shows and edits. Not owned.
    explicit MaterialPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

public slots:
    /// THE SAVE BUTTON (the heading's right end): the shown zone's material saved as a `.psm`
    /// file — the dialog opens in the Materials library ("My PoseStudio Library/Materials", or
    /// the folder of it saved into last) — with its ICON beside it under the same name: the
    /// material on a ball, Constants::MATERIAL_ICON_SIZE pixels square on a transparent
    /// background, which is what lists the file in the Materials Library. A file that already has
    /// an icon asks first whether to render it again. See scene/materialfile.h.
    void saveMaterial();
    /// The heading's Import button: a file dialog for a MaterialX document (`.mtlx`) — or a
    /// saved material (`.psm`) from anywhere on disk — whose material is put on the zone this
    /// tab shows, through applyMaterialFile. (It was File > Import > .MTLX until 2026-11-14: a
    /// material is not a model, and the tab that shows the zone is where it is put on one.)
    void importMaterial();
    /// A saved material (`.psm`) put on the zone this tab shows — one undoable edit — with a
    /// message where it cannot be (nothing selected, not a material file) or where some of its
    /// maps are no longer on disk. What a double-click on one in a library tab runs.
    void applyMaterialFile(const QString& path);
    /// The Save button's icon question alone, with nothing saved: how the box is LOOKED AT under
    /// a script (`ui post materials.panel 0 previewIconQuestion`, then `uishot`; its buttons
    /// carry the id `materials.iconquestion`). The int is the `ui` hook's and unused.
    void previewIconQuestion(int unused = 0);

signals:
    /// A material was saved (file and, unless kept, icon): the Materials Library re-reads.
    void materialSaved(const QString& path);

protected:
    /// The first show: the label column aligned across every section (see alignLabelColumn).
    void showEvent(QShowEvent* event) override;
    /// The Overlays header's Resize: its folded summary re-placed (placeOverlaySummary).
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// What the icon question was answered with.
    enum class IconChoice { Recreate, Keep, Cancel };

    /// One scrub-field row: its label (dimmed with the row), the field, the restore button.
    struct Row {
        QLabel*        label = nullptr;
        DragNumberBox* box = nullptr;
        QToolButton*   restore = nullptr;
        QWidget*       field = nullptr; ///< the box + restore (a slot's small thumbnail is inserted before them)
        QFormLayout*   form = nullptr;  ///< the section's form (a row shown for some materials only is hidden through it)
    };
    /// One color row: a swatch whose background is the color, its hex as the caption.
    struct ColorRow {
        QLabel*      label = nullptr;
        QPushButton* swatch = nullptr;
        QToolButton* restore = nullptr;
        QWidget*     field = nullptr;
    };
    /// One checkbox row (Thin Walled, Flip Normal, Double Sided, Smooth by Angle): the flag of the
    /// material it shows and sets, and its restore-to-imported button.
    struct FlagRow {
        QCheckBox*             box = nullptr;
        QToolButton*           restore = nullptr;
        bool MaterialParams::* member = nullptr;
        bool                   needsNormalMap = false; ///< greyed for a zone without a normal map (Flip Normal)
    };
    /// What a texture slot IS to the tab: its label (the file dialog's title, the menus) and its
    /// tooltip. Its thumbnail lives on the line of the value it scales (attachSlot) or, scaling
    /// no value, on a row of its own (addSlotRow).
    struct SlotInfo {
        QString label;
        QString tip;
    };
    /// What a slot's thumbnails were last given (refreshSlots): a slot that still shows exactly
    /// this is skipped — its picture, preview caption and tooltip are what they were.
    struct SlotShown {
        bool                              set = false; ///< false until the slot has been shown once
        bool                              valid = false;
        bool                              has = false;
        std::string                       source;
        std::string                       name;
        std::weak_ptr<const DecodedImage> image; ///< the slot's image (shared and immutable: its identity is its content)
        int                               channel = 0;
    };
    /// A thumbnail picture, kept while its decoded image lives (the images are shared and
    /// immutable; the weak pointer says when one is gone and its address may be another's).
    struct Thumb {
        std::weak_ptr<const DecodedImage> image;
        QPixmap                           pixmap;
    };
    /// The thumbnail caches' key: the decoded image and the channel its picture was read through
    /// (-1: its color). One packed image serves several slots, each through its own channel.
    using ThumbKey = std::pair<const DecodedImage*, int>;
    using ThumbCache = std::map<ThumbKey, Thumb>;
    /// A collapsible section and the test that says a zone USES it (opened for such a zone).
    struct Section {
        QToolButton*                                  header = nullptr;
        std::function<bool(const MaterialView& view)> inUse;
        bool                                          wasInUse = false; ///< inUse at the last refresh (a section opens as it COMES into use)
        MaterialLayer                                 layer = MaterialLayer::Count; ///< the layer its header switch turns on and off (Count: none)
        QCheckBox*                                    toggle = nullptr;             ///< that switch
        QGraphicsOpacityEffect*                       dim = nullptr;                ///< dims the body; enabled only while the layer is off
    };

    static std::size_t at(MaterialField field) { return static_cast<std::size_t>(field); }
    static std::size_t at(MaterialColor which) { return static_cast<std::size_t>(which); }
    static std::size_t at(MaterialMap map) { return static_cast<std::size_t>(map); }

    // --- The construction (materialpanel_build.cpp) -------------------------------------------
    /// The whole tab, top to bottom, through the parts below.
    void buildUi();
    void buildHeading(QVBoxLayout* col);
    void buildZoneGroup(QVBoxLayout* col);
    /// Base, Overlays, Specular, Top Coat, Sheen, Thin Film: the surface and what lies on it.
    void buildSurfaceSections(QVBoxLayout* col);
    /// Transmission, Subsurface, Emission: the volume and the surface as a light.
    void buildVolumeSections(QVBoxLayout* col);
    /// Geometry, Displacement, Detail, Tiling: the shape and the maps' placement. Returns the
    /// Detail section's form (the Detail Weight Mask's slot row is added to it).
    QFormLayout* buildGeometrySections(QVBoxLayout* col);
    /// Every slot's label and tip (defineSlot).
    void defineSlots();
    /// Every line item's small thumbnail (attachSlot): which map sits beside which value.
    void attachSlots();
    /// The no-selection hint and the Reset buttons.
    void buildFooter(QVBoxLayout* col);
    /// A collapsible section appended to @p col; @p inUse opens it for a zone that uses it (null:
    /// never by itself). Returns the body's form.
    QFormLayout* addSection(QVBoxLayout* col, const QString& title, bool expanded,
                            std::function<bool(const MaterialView&)> inUse, MaterialLayer layer = MaterialLayer::Count);
    /// Adds one scrub-field row to @p form, wired to setField / restoreField. @p origin is where
    /// the field's fill starts (the rest value: 1.5 for an IOR, 100 for a scale).
    void addRow(QFormLayout* form, MaterialField field, const QString& label, const QString& tip, double min, double max,
                double step, int decimals, double origin = 0.0);
    /// Adds one color row (a swatch opening the color dialog) wired to pickColor / restoreColor.
    void addColorRow(QFormLayout* form, MaterialColor which, const QString& label, const QString& tip);
    /// Adds one checkbox row for the flag @p member, wired to setFlag and — its restore button —
    /// to the imported value. @p testId names the box for the scripted test (null: none).
    void addFlagRow(QFormLayout* form, bool MaterialParams::* member, const QString& label, const QString& tip,
                    const char* testId = nullptr, bool needsNormalMap = false);
    /// Names a slot (its label for the file dialog and the menus, its tooltip for its thumbnails).
    void defineSlot(MaterialMap map, const QString& label, const QString& tip);
    /// A row for a slot that scales no value: the label, the thumbnail and the bound file's name.
    void addSlotRow(QFormLayout* form, MaterialMap map, const QString& label, const QString& tip);
    /// Puts a small thumbnail of slot @p map in front of a line item's field (the row's box or
    /// swatch), so the map and the value it scales sit side by side.
    void attachSlot(QWidget* field, MaterialMap map, const QString& tip);
    /// Gives every row label the widest label's width, so the fields line up down the whole tab.
    void alignLabelColumn();

    // --- The core (materialpanel.cpp) ---------------------------------------------------------
    /// ViewportWidget::materialViewChanged: refresh(), unless the rows already show this state
    /// of the viewport (the frame's echo of an edit made here).
    void onViewChanged();
    /// Mirrors the viewport's selected object into the heading, the picker and every row (signals
    /// blocked: nothing here edits a material); opens the sections a newly shown zone uses.
    void refresh();
    bool linked() const;
    /// The header switch: layer @p layer on or off, every value kept (one undo entry; linked zones follow).
    void setLayerEnabled(MaterialLayer layer, bool on);
    /// Live: the field set to @p value on the selected zone (and its linked zones).
    void setField(MaterialField field, double value);
    /// One bracket: the field back to the imported value.
    void restoreField(MaterialField field);
    /// Copies one field's value (the Roughness row's dual lobe with it) from @p src into @p dst.
    static void copyField(MaterialParams& dst, const MaterialParams& src, MaterialField field);
    /// A color swatch: opens the color dialog (one bracket for the whole pick).
    void pickColor(MaterialColor which);
    /// Sets a color from a display (sRGB) color, live.
    void setColor(MaterialColor which, const QColor& srgb);
    /// One bracket: a color back to the imported one.
    void restoreColor(MaterialColor which);
    /// A checkbox row: the flag @p member set (one bracket).
    void setFlag(bool MaterialParams::* member, bool on);
    /// The Overlays header's folded summary: which mark shows, and where (the restore buttons' column).
    void syncOverlaySummary();
    void placeOverlaySummary();

    // --- The texture slots (materialpanel_slots.cpp) -------------------------------------------
    /// A thumbnail button for slot @p map, @p px wide with @p radius corners: a click chooses an
    /// image, a right-click offers remove / restore / open its folder, the pointer at rest previews.
    MaterialThumbButton* makeThumb(MaterialMap map, int px, int radius, const QString& tip);
    /// The thumbnails' context menu: Choose Image / Remove / Restore Imported Map / Open File Location.
    void showThumbMenu(MaterialMap map, const QPoint& globalPos);
    /// A file dialog for slot @p map, then setTexture.
    void chooseTexture(MaterialMap map);
    /// Slot @p map made @p source ("" clears it, kImportedTexture puts the import's back, else a
    /// file) — one undo entry; a file that cannot be read is reported.
    void setTexture(MaterialMap map, const QString& source);
    /// The slots' part of refresh: every thumbnail's image, preview caption and tooltip, the slot rows' names.
    void refreshSlots();
    /// The developer levers for a UI shot (POSESTUDIO_MATERIALS_SCROLL / _SCROLL_END): the tab
    /// scrolled to a fraction of its height. A no-op without them.
    void applyScrollLever();
    /// The thumbnail picture of @p image read through @p channel for slot @p map: a strided
    /// sample of the decoded image at four times the picture's size, smooth-scaled down, so a 4K
    /// map costs tens of thousands of pixel reads, not sixteen million; cached per image and channel.
    QPixmap slotThumbnail(MaterialMap map, const std::shared_ptr<const DecodedImage>& image, int channel);
    /// The same at the hover preview's size (kPreviewPx) for the selected zone's map in slot
    /// @p map, cached apart (a null pixmap without a map).
    QPixmap slotPreview(MaterialMap map);
    /// The sampler behind both: @p image at @p px logical pixels, through @p cache.
    QPixmap slotPixmap(MaterialMap map, const std::shared_ptr<const DecodedImage>& image, int channel, int px, ThumbCache& cache);

    // --- Material files (materialpanel_files.cpp) ----------------------------------------------
    /// "<name> already has an icon": make it again, keep it, or save nothing.
    IconChoice askIconOverwrite(const QString& name);

    ViewportWidget*   m_viewport = nullptr;
    QScrollArea*      m_scroll = nullptr;     // the tab's scroll area
    QLabel*           m_objectName = nullptr; // the selected object's file name, or "No object selected"
    QPushButton*      m_import = nullptr;     // left of Save: puts a material from a file (.mtlx / .psm) on the shown zone
    QPushButton*      m_save = nullptr;       // across from the name: saves the shown zone's material to a file
    QLabel*           m_hint = nullptr;       // how to get an object selected (shown while none is)
    MenuPickerButton* m_zonePicker = nullptr; // the object's zones
    QCheckBox*        m_linked = nullptr;     // "Edit matching zones together"
    QLabel*           m_linkedNote = nullptr; // how many other zones share the imported material
    QPushButton*      m_resetZone = nullptr;
    QPushButton*      m_resetAll = nullptr;
    bool              m_columnAligned = false; // alignLabelColumn has run (the first show)

    std::array<Row, static_cast<std::size_t>(MaterialField::Count)>      m_rows{};
    std::array<ColorRow, static_cast<std::size_t>(MaterialColor::Count)> m_colors{};
    std::vector<FlagRow>                                                 m_flags;
    std::vector<Section>                                                 m_sections;
    /// Which slot a line item carries (MaterialMap::Count: none) — its restore arrow puts the map
    /// back with the value.
    std::array<MaterialMap, static_cast<std::size_t>(MaterialField::Count)> m_rowSlot{};
    std::array<MaterialMap, static_cast<std::size_t>(MaterialColor::Count)> m_colorSlot{};

    std::array<SlotInfo, kMaterialMapCount>  m_slots{};       // the slots' labels and tips
    std::array<SlotShown, kMaterialMapCount> m_slotShown{};   // what each slot's thumbnails show now
    std::array<QLabel*, kMaterialMapCount>   m_slotNames{};   // a slot row's file-name label (addSlotRow)
    std::vector<QLabel*>                     m_slotRowLabels; // the slot rows' labels (the shared label column)
    /// The thumbnails, by slot: a slot may show on more than one line (the Roughness slot is the
    /// roughness map AND a skin's lobe 1 map).
    std::array<std::vector<MaterialThumbButton*>, kMaterialMapCount> m_rowThumbs{};
    ThumbCache m_thumbs;   // the thumbnails' pictures (sampled at kThumbPx)
    ThumbCache m_previews; // the hover previews' (kPreviewPx)

    OverlayStack* m_overlays = nullptr; // the Overlays section: the zone's overlay stack (overlaystack.h)
    // The Overlays header's summary while it is FOLDED: a "+" while the zone has no overlay, else
    // the number it carries (syncOverlaySummary). MARKS, not controls: children of the header
    // that let the mouse through (its hover runs under them, a click there folds or unfolds the
    // section like a click anywhere else on it), centered on the restore buttons' column.
    QToolButton* m_overlayHeader = nullptr;
    QWidget*     m_overlayAdd = nullptr;   // (the drawn "+": PlusMark in materialpanel_build.cpp)
    QLabel*      m_overlayCount = nullptr;

    QStringList  m_zoneNames; // the picker's items as last built
    MaterialView m_view;      // what the rows show now
    std::string  m_shownZone; // which object + zone the sections were last opened for

    colorswatch::ColorPick m_colorPick;                          // a color row's dialog, while one is open
    MaterialColor          m_colorPicking = MaterialColor::Base; // … and the color it edits
};

} // namespace pose

#endif // MATERIALPANEL_H
