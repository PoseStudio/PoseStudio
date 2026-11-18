/**
 * @file propertyrows.h
 * @brief The builders every properties tab shares (Transform, Materials, Soft Body Physics,
 *        Environment, Lights and the Materials tab's overlay cards): the scroll column a tab
 *        stands in, a titled group and a collapsible one with a form inside, a scrub-field row,
 *        the small restore-default button, the picker field's look, and the pass that gives a
 *        tab's forms one label column.
 *
 * One set of builders — and one set of object names, styled once in _environment.qss
 * (#PropertyGroup, #PropertyCollapsible*, #PropertyRowRestore, #PropertyResetButton,
 * #EnvironmentHdriButton) — so the tabs cannot drift apart in margins, spacing or look. The
 * color row's parts are colorswatch.h's.
 */

#ifndef PROPERTYROWS_H
#define PROPERTYROWS_H

#include <vector>

class QFormLayout;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QString;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace pose {
class DragNumberBox;
}

namespace pose::propertyrows {

/// THE SCAFFOLD A TAB STANDS IN: a frameless scroll area filling @p panel, and the content
/// widget (named @p contentName for the style sheet) whose 12 px-margined column the tab's
/// groups are added to, @p spacing apart.
struct ScrollColumn {
    QScrollArea* scroll = nullptr;
    QWidget*     content = nullptr;
    QVBoxLayout* column = nullptr;
    /// Hands the filled content to the scroll area: the LAST step of a tab's construction.
    void finish() const;
};
ScrollColumn makeScrollColumn(QWidget* panel, const QString& contentName, int spacing);

/// Builds a titled QGroupBox (#PropertyGroup) with a QFormLayout inside, appended to @p parent;
/// returns the form.
QFormLayout* addGroup(QVBoxLayout* parent, const QString& title);

/// What addCollapsibleGroup built.
struct CollapsibleGroup {
    QFormLayout* form = nullptr;   ///< the body's form: add rows to it as to addGroup's
    QToolButton* header = nullptr; ///< the fold button: an arrow and the title (#PropertyCollapsibleHeader)
    QWidget*     body = nullptr;   ///< what the header shows and hides (#PropertyCollapsibleBody)
    QFrame*      box = nullptr;    ///< the whole group (#PropertyCollapsible)
    QVBoxLayout* column = nullptr; ///< the box's layout: the header over the body
};
/// A COLLAPSIBLE group (the Materials tab's sections): a header button with an arrow and the
/// title over a body holding a QFormLayout; clicking the header shows or hides the body.
/// Appended to @p parent, open or closed as @p expanded says.
CollapsibleGroup addCollapsibleGroup(QVBoxLayout* parent, const QString& title, bool expanded);

/// The small restore-default icon button (#PropertyRowRestore) that sits right of a row. The
/// caller wires clicked to the row's own restore behaviour.
QToolButton* makeRestoreButton();

/// A form row's field widget: the row's main control stretched, its restore button snug on the
/// right.
QWidget* rowWithRestore(QWidget* main, QToolButton* restore);

/// One SCRUB-FIELD row's widgets: the label and the DragNumberBox (both with @p tip), the box
/// given its range, step and decimals, a restore button, and the field holding box + button.
/// The row's meaning is the caller's: it sets the box's origin or scrub span where it wants
/// one, connects the box's edit bracket and valueChanged and the restore button's clicked,
/// gives the restore button its tooltip, and adds label + field to its form.
struct ScrubRow {
    QLabel*        label = nullptr;
    DragNumberBox* box = nullptr;
    QToolButton*   restore = nullptr;
    QWidget*       field = nullptr; ///< rowWithRestore(box, restore)
};
ScrubRow makeScrubRow(const QString& label, const QString& tip, double min, double max, double step, int decimals);

/// THE PICKER FIELD'S LOOK on a button that opens a menu (the HDRI selector, a MenuPickerButton):
/// the #EnvironmentHdriButton object name the style sheet styles, the pointing-hand cursor, and
/// the caption's left inset — a transparent 10 px spacer icon, because Qt's style engine ignores
/// QSS padding-left on a left-aligned button label (the text is laid out from the border box).
void stylePickerField(QPushButton* button);

/// ONE LABEL COLUMN for a tab of several forms: left to themselves the forms' label columns
/// differ by the longest label each holds, so the fields would start at a different x from group
/// to group — every label of @p labels (null entries skipped) is given the widest one's width.
/// To be called at the tab's FIRST SHOW, when the style sheet's font is on the labels: measured
/// at construction they still wear the application's larger default font, and the column comes
/// out half again too wide.
void alignLabelColumn(const std::vector<QLabel*>& labels);

} // namespace pose::propertyrows

#endif // PROPERTYROWS_H
