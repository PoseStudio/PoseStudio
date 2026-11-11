/**
 * @file materialpanel.h
 * @brief The "Materials" properties tab: the selected object's material zones as sliders, color
 *        swatches and texture slots on PoseStudio's one canonical material.
 *
 * A self-contained docker panel (its own module, like TransformPanel), built from the shared row
 * builders (propertyrows.h). With an object selected in the viewport — a figure, an OBJ, any
 * imported model with meshes — it shows the object's name, a picker of its material ZONES (one
 * per material group: a figure's Torso, Face, Irises …; an OBJ's MTL materials), and the
 * selected zone's material (materialparams.h — the OpenPBR-style parameter set every importer
 * fills and every renderer will read) in COLLAPSIBLE sections:
 *
 *   Overlays      images layered over the zone's skin — tattoos, bruises, freckles, makeup — each
 *                 a card of blend, opacity, tint and bitmap adjustments, gloss, and bump / normal
 *                 / displacement images of its own (OverlayStack, overlaystack.h)
 *   Base          Base Color, Roughness, Metallic, Specular (0-100 %, 100 = the standard
 *                 dielectric), Specular Color, IOR, Diffuse Roughness, Anisotropy (+ rotation)
 *   Coat          Top Coat, its Roughness, Coat Color, Coat IOR
 *   Transmission  Transmission, its Color, Depth, Dispersion, Thin Walled
 *   Subsurface    Subsurface, its Color, Radius
 *   Sheen         Sheen, its Color, Roughness
 *   Emission      Emission Color, Emission
 *   Thin Film     Thin Film, Thickness, IOR
 *   Geometry      Opacity, Normal / Bump Strength, Pore Detail + Tiles, UV Tiling and Offset
 *   Textures      one row per texture SLOT (base color, normal, bump, roughness, metalness,
 *                 specular mask, translucency, emission, pore detail, cutout): what is bound,
 *                 Choose… (an image file), clear, and back to the imported map
 *
 * Base, Geometry and Textures are open; the others open by themselves for a zone that USES them
 * (a coat on skin, an emission on a lamp) and otherwise stay as the user left them. A parameter
 * the viewport does not render yet (anisotropy, the specular tint, sheen, thin film, the
 * subsurface color …) is edited and saved all the same — the sections say so — for the
 * rendering engines to come.
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
 * Undo: every completed gesture (a scrub, a type-in, a color pick, a checkbox, a texture
 * choice or clear, a row's restore button, Reset Zone, Reset All Zones) is ONE entry on the
 * viewport's unified undo stack, bracketed begin/endMaterialEdit. The restore buttons return a
 * value or a slot to what the file imported, never to a generic default. A project file carries
 * the zones that differ from the import, their texture slots included.
 */

#ifndef MATERIALPANEL_H
#define MATERIALPANEL_H

#include "materialparams.h"

class QScrollArea;
class QGraphicsOpacityEffect;

namespace pose {
class MaterialThumbButton;
}
namespace pose {
struct DecodedImage;
}

#include <QColor>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

class QCheckBox;
class QColorDialog;
class QFormLayout;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace pose {

class DragNumberBox;
class MenuPickerButton;
class OverlayStack;
class ViewportWidget;

class MaterialPanel : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport whose selected object this panel shows and edits. Not owned.
    explicit MaterialPanel(ViewportWidget* viewport, QWidget* parent = nullptr);

private:
    /// The thumbnail cache entry: by the decoded image's identity (the images are shared and
    /// immutable; the weak pointer says when one is gone), with the channel it was read through.
    struct Thumb {
        std::weak_ptr<const DecodedImage> image;
        int                               channel = -1;
        QPixmap                           pixmap;
    };
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
    /// What a texture slot IS to the tab: its label and its tooltip (its thumbnail lives on the line
    /// of the value it scales — attachSlot — or, scaling no value, on a row of its own: addSlotRow).
    struct SlotInfo {
        QString label;
        QString tip;
    };
    /// A collapsible section and the test that says a zone USES it (opened for such a zone).
    struct Section {
        QToolButton*                                  header = nullptr;
        std::function<bool(const MaterialView& view)> inUse;
        bool                                          wasInUse = false; ///< inUse at the last refresh (a section opens as it COMES into use)
        MaterialLayer                                 layer = MaterialLayer::Count; ///< the layer its header switch turns on and off (Count: none)
        QCheckBox*                                    toggle = nullptr;             ///< that switch
        QGraphicsOpacityEffect*                       dim = nullptr;                ///< the body dimmed while the layer is off
    };

    void buildUi();
    /// The first show: the label column aligned across every section (see alignLabelColumn).
    void showEvent(QShowEvent* event) override;
    /// Gives every row label the widest label's width, so the fields line up down the whole tab.
    void alignLabelColumn();
    bool m_columnAligned = false;
    /// A collapsible section appended to @p col; @p inUse opens it for a zone that uses it (null:
    /// never by itself). Returns the body's form.
    QFormLayout* addSection(QVBoxLayout* col, const QString& title, bool expanded,
                            std::function<bool(const MaterialView&)> inUse, MaterialLayer layer = MaterialLayer::Count);
    /// The header switch: layer @p layer on or off, every value kept (one undo entry; linked zones follow).
    void setLayerEnabled(MaterialLayer layer, bool on);
    /// Adds one scrub-field row to @p form, wired to setField / restoreField. @p origin is where
    /// the field's fill starts (the rest value: 1.5 for an IOR, 100 for a scale).
    void addRow(QFormLayout* form, MaterialField field, const QString& label, const QString& tip, double min, double max,
                double step, int decimals, double origin = 0.0);
    /// Adds one color row (a swatch opening the color dialog) wired to pickColor / restoreColor.
    void addColorRow(QFormLayout* form, MaterialColor which, const QString& label, const QString& tip);
    /// Names a slot (its label for the file dialog and the menus, its tooltip for its thumbnails).
    void defineSlot(MaterialMap map, const QString& label, const QString& tip);
    /// A row for a slot that scales no value: the label, the thumbnail and the bound file's name.
    void addSlotRow(QFormLayout* form, MaterialMap map, const QString& label, const QString& tip);
    /// A thumbnail button for slot @p map, @p px wide with @p radius corners: a click chooses an
    /// image, a right-click offers remove / restore (the Textures list's large one and the line
    /// items' small ones alike).
    MaterialThumbButton* makeThumb(MaterialMap map, int px, int radius, const QString& tip);
    /// Puts a small thumbnail of slot @p map in front of a line item's field (the row's box or
    /// swatch), so the map and the value it scales sit side by side.
    void attachSlot(QWidget* field, MaterialMap map, const QString& tip);
    /// The thumbnails' context menu: Choose Image / Remove / Restore Imported Map.
    void showThumbMenu(MaterialMap map, const QPoint& globalPos);
    /// Mirrors the viewport's selected object into the heading, the picker and every row (signals
    /// blocked: nothing here edits a material); opens the sections a newly shown zone uses.
    void refresh();
    /// The slots' part of refresh: every thumbnail's image, preview and tooltip, the slot rows' names.
    void refreshSlots();
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
    /// The Thin Walled checkbox (one bracket).
    void setThinWalled(bool on);
    void setDoubleSided(bool on);
    /// The Textures group: a file dialog for slot @p map, then setTexture.
    void chooseTexture(MaterialMap map);
    /// Slot @p map made @p source ("" clears it, kImportedTexture puts the import's back, else a
    /// file) — one undo entry; a file that cannot be read is reported.
    void setTexture(MaterialMap map, const QString& source);
    /// The thumbnail of the selected zone's map in slot @p map (a null pixmap without one): a
    /// strided sample of the decoded image at four times the thumbnail's size, smooth-scaled down,
    /// so a 4K map costs tens of thousands of pixel reads, not sixteen million; cached per image.
    QPixmap slotThumbnail(MaterialMap map);
    /// The same at the hover preview's size (kPreviewPx), cached apart.
    QPixmap slotPreview(MaterialMap map);
    /// The sampler behind both: the slot's image at @p px logical pixels, through @p cache.
    QPixmap slotPixmap(MaterialMap map, int px, std::unordered_map<const DecodedImage*, Thumb>& cache);
    bool linked() const;

    ViewportWidget*   m_viewport = nullptr;
    QScrollArea*      m_scroll = nullptr;    // the tab's scroll area
    QLabel*           m_objectName = nullptr; // the selected object's file name, or "No object selected"
    QLabel*           m_hint = nullptr;       // how to get an object selected (shown while none is)
    MenuPickerButton* m_zonePicker = nullptr; // the object's zones
    QCheckBox*        m_linked = nullptr;     // "Edit matching zones together"
    QLabel*           m_linkedNote = nullptr; // how many other zones share the imported material
    std::array<Row, static_cast<std::size_t>(MaterialField::Count)>      m_rows{};
    std::array<ColorRow, static_cast<std::size_t>(MaterialColor::Count)> m_colors{};
    std::array<SlotInfo, kMaterialMapCount>                              m_slots{};     // the slots' labels and tips
    std::array<QLabel*, kMaterialMapCount>                               m_slotNames{}; // a slot row's file-name label (addSlotRow)
    std::vector<QLabel*>                                                 m_slotRowLabels; // the slot rows' labels (the shared label column)
    /// The line items' small thumbnails, by slot: a slot may show on more than one line (the
    /// Roughness slot is the roughness map AND a skin's lobe 1 map).
    std::array<std::vector<MaterialThumbButton*>, kMaterialMapCount>     m_rowThumbs{};
    QCheckBox*        m_thinWalled = nullptr;
    QToolButton*      m_thinWalledRestore = nullptr;
    QCheckBox*        m_doubleSided = nullptr;
    QToolButton*      m_doubleSidedRestore = nullptr;
    std::vector<Section> m_sections;
    OverlayStack*     m_overlays = nullptr;   // the Overlays section: the zone's overlay stack (overlaystack.h)
    QPushButton*      m_resetZone = nullptr;
    QPushButton*      m_resetAll = nullptr;
    QStringList       m_zoneNames;            // the picker's items as last built
    MaterialView      m_view;                 // what the rows show now
    std::string       m_shownZone;            // which object + zone the sections were last opened for
    QColorDialog*     m_colorDialog = nullptr;
    MaterialColor     m_colorPicking = MaterialColor::Base; // which color the open dialog edits
    QColor            m_colorAtOpen;          // that color when the dialog opened (Cancel puts it back)
    std::unordered_map<const DecodedImage*, Thumb> m_thumbs;   // the 39 px thumbnails
    std::unordered_map<const DecodedImage*, Thumb> m_previews; // the hover previews
    /// Which slot a line item carries (MaterialMap::Count: none) — its restore arrow puts the map
    /// back with the value.
    std::array<MaterialMap, static_cast<std::size_t>(MaterialField::Count)> m_rowSlot{};
    std::array<MaterialMap, static_cast<std::size_t>(MaterialColor::Count)> m_colorSlot{};
};

} // namespace pose

#endif // MATERIALPANEL_H
