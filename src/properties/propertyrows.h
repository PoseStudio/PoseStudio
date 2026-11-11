/**
 * @file propertyrows.h
 * @brief The row builders every properties tab shares (Environment, Transform): a titled group
 *        with a form inside, the small restore-default button, and a field with that button snug
 *        on its right.
 *
 * One set of builders — and one set of object names, styled once in _environment.qss
 * (#PropertyGroup, #PropertyRowRestore, #PropertyResetButton) — so the tabs cannot drift apart
 * in margins, spacing or look.
 */

#ifndef PROPERTYROWS_H
#define PROPERTYROWS_H

class QFormLayout;
class QString;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace pose::propertyrows {

/// Builds a titled QGroupBox (#PropertyGroup) with a QFormLayout inside, appended to @p parent;
/// returns the form.
QFormLayout* addGroup(QVBoxLayout* parent, const QString& title);

/// A COLLAPSIBLE group (the Materials tab's sections, 2026-11-09): a header button with an arrow
/// and the title (#PropertyCollapsibleHeader) over a body (#PropertyCollapsibleBody) holding a
/// QFormLayout; clicking the header shows or hides the body. Appended to @p parent, open or
/// closed as @p expanded says; returns the form (the body's) — add rows to it as to addGroup's.
QFormLayout* addCollapsibleGroup(QVBoxLayout* parent, const QString& title, bool expanded);

/// The small restore-default icon button (#PropertyRowRestore) that sits right of a row. The
/// caller wires clicked to the row's own restore behaviour.
QToolButton* makeRestoreButton();

/// A form row's field widget: the row's main control stretched, its restore button snug on the
/// right.
QWidget* rowWithRestore(QWidget* main, QToolButton* restore);

} // namespace pose::propertyrows

#endif // PROPERTYROWS_H
