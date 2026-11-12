/**
 * @file overlaystack.h
 * @brief The Materials tab's OVERLAYS section: the selected zone's overlay stack
 *        (materialoverlay.h) as a column of cards, and the button that adds one.
 *
 * An overlay is an image layered over the skin — a tattoo, freckles, a bruise, veins, a scar,
 * makeup — painted on the zone's UV layout. Each card is one overlay:
 *
 *   header   fold arrow + the overlay's NAME ("Overlay 1" as added; right-click > Rename edits it
 *            in place), the on/off switch, move up / down in the stack, remove
 *   Color   Blend (Multiply by default: ink and bruising darken the skin that is there, so the
 *            same image sits right on any skin tone), Base Color — a swatch and, before it, the
 *            thumbnail of the overlay's PICTURE (click: choose one; right-click: remove it, open
 *            its folder): the color multiplies the picture, or IS the overlay where there is
 *            none — Brightness, Contrast, Gamma, Hue, Saturation
 *   Gloss    glossier or more matte where the overlay is (lip gloss, powder)
 *   Metallic more metallic, or less, where the overlay is (metallic ink, gold leaf) — with a
 *            thumbnail for an image that limits it within the overlay
 *   Opacity  how much of the color shows, with a thumbnail for its MAP: the overlay's mask —
 *            the shape of the color, the gloss and the metal (a Base Color and an opacity map
 *            alone are a lipstick of any color, in any blend)
 *   Relief   Bump, Normal and Displacement — each its own image (a thumbnail to choose it) and
 *            a strength
 *   Place    Offset X / Y, Rotation, Scale — the whole group moved, turned and sized
 *
 * "+ Add Overlay" adds an EMPTY overlay (no file is asked for): its card is where it is made.
 *
 * The cards are listed as an image editor lists its layers: the TOP of the stack first, a new
 * overlay appearing at the top. The stack belongs to the MAP, not the one zone: every zone painted on the selected zone's color map
 * carries it (a face image reaches the lips and the ears) — the note under the cards says which.
 *
 * Live and undoable like the tab's rows: a scrub re-composites the one map its control touches
 * and is ONE undo entry (begin/endMaterialEdit bracket it), as is an add, a remove, a move, an
 * image choice, a tint pick. The widget holds no state of its own but which cards are folded: it
 * mirrors the viewport's MaterialView (setView), rebuilding its cards only when the stack's
 * STRUCTURE changes — an overlay added, removed, moved, an image chosen — so a scrub in flight
 * is never torn down under the pointer.
 */

#ifndef OVERLAYSTACK_H
#define OVERLAYSTACK_H

#include "materialparams.h"

#include <QColor>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QColorDialog;
class QFormLayout;
class QFrame;
class QGraphicsOpacityEffect;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace pose {

class DragNumberBox;
class MaterialThumbButton;
class MenuPickerButton;
class ViewportWidget;
struct OverlayImage;
enum class OverlayImageKind;

class OverlayStack : public QWidget {
    Q_OBJECT

public:
    /// @param viewport The viewport whose selected zone's overlays this shows and edits. Not owned.
    explicit OverlayStack(ViewportWidget* viewport, QWidget* parent = nullptr);

    /// Mirrors @p view's overlay stack into the cards (signals blocked: nothing here edits).
    void setView(const MaterialView& view);

public slots:
    /// Adds an EMPTY overlay at the bottom of the list (one undo entry) — the + Add Overlay
    /// button's, and the folded section header's "+".
    void addOverlay();
    /// Opens the inline NAME editor on overlay @p index's card (Enter or a click elsewhere keeps
    /// the text, Esc drops it). What the header's Rename… runs.
    void beginRename(int index);

signals:
    /// The stack was edited through this widget: the tab re-reads the viewport.
    void edited();

private:
    /// One scrub-field row of a card.
    struct FieldRow {
        QLabel*        label = nullptr;
        DragNumberBox* box = nullptr;
        QToolButton*   restore = nullptr;
    };
    /// One overlay's widgets.
    struct Card {
        QFrame*           frame = nullptr;
        QToolButton*      header = nullptr; ///< fold arrow + the overlay's name
        QCheckBox*        enabled = nullptr;
        QToolButton*      up = nullptr;
        QToolButton*      down = nullptr;
        QToolButton*      remove = nullptr;
        QWidget*          body = nullptr;
        MenuPickerButton* blend = nullptr;
        QLabel*           blendLabel = nullptr;
        QToolButton*      blendRestore = nullptr;
        QGraphicsOpacityEffect* dim = nullptr; ///< the body dimmed while the overlay is off
        QPushButton*      tint = nullptr;      ///< the Base Color's swatch
        QLabel*           tintLabel = nullptr;
        QToolButton*      tintRestore = nullptr;
        std::array<FieldRow, static_cast<std::size_t>(OverlayField::Count)>               rows{};
        std::array<MaterialThumbButton*, static_cast<std::size_t>(OverlayElement::Count)> thumbs{};
    };
    /// A thumbnail by the cropped image's identity and the size it was made for.
    struct Thumb {
        std::weak_ptr<const OverlayImage> image;
        QPixmap                           pixmap;
    };

    /// Tears the cards down and builds one per overlay of m_view (the structure changed).
    void rebuild();
    /// Builds overlay @p index's card (its widgets wired to the edit functions by that index).
    Card buildCard(int index);
    /// Adds one scrub-field row to a card's form; @p element's thumbnail (the row's own image:
    /// Opacity, Metallic, Bump, Normal, Displacement) sits before the field when given. (The Base
    /// Color row, a swatch with the picture's thumbnail, is built in buildCard.)
    void addFieldRow(Card& card, QFormLayout* form, int index, OverlayField field, const QString& label, const QString& tip, double min,
                     double max, double step, int decimals, double origin, OverlayElement element = OverlayElement::Count);
    MaterialThumbButton* makeThumb(int index, OverlayElement element);
    /// Mirrors overlay @p index's values into its card.
    void syncCard(int index);

    // --- The edits (each from the stack as the viewport has it, one undo entry) ---
    std::vector<MaterialOverlay> stack() const;
    /// Hands @p overlays to the viewport and tells the tab.
    void apply(const std::vector<MaterialOverlay>& overlays);
    void removeOverlay(int index);
    void moveOverlay(int index, int delta);
    void setField(int index, OverlayField field, double value);
    void restoreField(int index, OverlayField field);
    void chooseImage(int index, OverlayElement element);
    void setImage(int index, OverlayElement element, const QString& path);
    void showThumbMenu(int index, OverlayElement element, const QPoint& globalPos);
    void pickTint(int index);
    void setTint(int index, const QColor& color);
    void setName(int index, const QString& name);
    /// Closes the name editor: @p commit keeps its text (one undo entry), else it is dropped.
    void finishRename(bool commit);
    /// What the tab calls overlay @p index: its name, or — it has none — its place in the stack.
    QString displayName(int index) const;
    /// A file dialog for an overlay image (remembering its folder); empty when cancelled.
    QString askImage(const QString& title);

    /// An overlay image's picture at @p px logical pixels: the cropped image fitted into the
    /// square over a light backdrop (dark ink must read), a height as grey.
    QPixmap imagePixmap(const std::string& path, OverlayImageKind kind, int px, std::unordered_map<std::string, Thumb>& cache);

    ViewportWidget* m_viewport = nullptr;
    QVBoxLayout*    m_cardColumn = nullptr; // the cards, the top of the stack first
    QLabel*         m_zonesNote = nullptr;  // which zones the stack is painted on
    QPushButton*    m_add = nullptr;
    MaterialView    m_view;
    std::vector<Card>        m_cards;
    std::vector<std::string> m_structure; // per overlay, its image paths: what the cards were built for
    std::vector<bool>        m_open;      // per overlay, whether its card is unfolded
    std::string              m_shownZone; // the object + zone the fold state belongs to
    QColorDialog*   m_tintDialog = nullptr;
    int             m_tintIndex = -1;
    QColor          m_tintAtOpen;
    bool            m_tintAbandoned = false; // the dialog is closing because its overlay moved or went: no Cancel restore
    std::unordered_map<std::string, Thumb> m_thumbs;   // the cards' thumbnails
    std::unordered_map<std::string, Thumb> m_previews; // the hover previews
    QPointer<QLineEdit> m_nameEditor;      // the inline rename editor, while one is open
    int                 m_nameIndex = -1;  // … and the overlay it names

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace pose

#endif // OVERLAYSTACK_H
