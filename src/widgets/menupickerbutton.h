/**
 * @file menupickerbutton.h
 * @brief The app-standard "dropdown": a push button that opens a real QMenu of exclusive choices.
 *
 * PoseStudio deliberately does NOT use QComboBox for its pickers (the viewport's shading-mode
 * and view pickers, the Environment tab's backdrop-mode field). Setting any stylesheet on a
 * QComboBox makes its popup render item states inconsistently (the hover highlight paints as a
 * faint native tint no matter how the view/items are styled) and the popup repositions to align
 * the current row with the field. A QPushButton whose menu is a genuine QMenu looks and behaves
 * EXACTLY like the File/Edit/Help menus — colors, hover, spacing, top-down drop — for free,
 * because it inherits the global QMenu QSS (_menumanager.qss).
 *
 * This widget is that pattern packaged once: give it the choices, it builds an exclusive,
 * checkable action group, mirrors the current choice in the button caption, and emits
 * currentChanged(int) on a user pick. The field's LOOK is left to the caller (an object name
 * plus QSS or setStyleSheet), so the same widget serves the viewport strip's dark field and the
 * Environment tab's form field.
 */

#ifndef MENUPICKERBUTTON_H
#define MENUPICKERBUTTON_H

#include <QList>
#include <QPushButton>
#include <QString>

class QActionGroup;
class QMenu;

namespace pose {

/**
 * @class MenuPickerButton
 * @brief Button + exclusive QMenu, with the current choice shown as the caption.
 */
class MenuPickerButton : public QPushButton {
    Q_OBJECT
public:
    /// One menu entry.
    struct Item {
        QString label;                ///< Menu text; also the caption when chosen
        bool    separatorAfter = false; ///< Draw a separator under this entry (groups)
        QString toolTip;              ///< Optional tooltip (the menu opts into showing them)
    };

    explicit MenuPickerButton(QWidget* parent = nullptr);

    /// Replaces the menu's entries. The current index resets to 0 (no signal).
    void setItems(const QList<Item>& items);
    int itemCount() const { return m_items.size(); }

    /// Text placed before the caption — the viewport strip uses two spaces so a left-aligned
    /// caption keeps off the border (QPushButton ignores QSS padding-left for left-aligned text).
    void setCaptionPrefix(const QString& prefix);

    /// Programmatic selection: updates the check marks and the caption WITHOUT emitting
    /// currentChanged (mirrors external state — e.g. the camera view the user orbited into).
    void setCurrentIndex(int index);
    int currentIndex() const { return m_currentIndex; }

    /// Shows a caption that is not one of the items (e.g. "Perspective" once the camera leaves
    /// every named view). Every check mark is cleared; currentIndex() becomes -1.
    void setDetachedCaption(const QString& caption);

    /// The underlying menu, for per-picker styling (a stylesheet, a minimum width).
    QMenu* pickerMenu() const { return m_menu; }

signals:
    /// The user picked an entry (never emitted by setCurrentIndex).
    void currentChanged(int index);

private:
    void updateCaption();

    QList<Item>   m_items;
    QMenu*        m_menu = nullptr;
    QActionGroup* m_group = nullptr;
    QString       m_prefix;
    int           m_currentIndex = -1;
};

} // namespace pose

#endif // MENUPICKERBUTTON_H
