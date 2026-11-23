// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file menupickerbutton.cpp
 * @brief Implementation of MenuPickerButton. See menupickerbutton.h.
 */

#include "menupickerbutton.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>

namespace pose {

MenuPickerButton::MenuPickerButton(QWidget* parent) : QPushButton(parent) {
    m_menu = new QMenu(this);
    m_group = new QActionGroup(m_menu);
    m_group->setExclusive(true);
    setMenu(m_menu); // QPushButton drops the menu straight down from the button
}

void MenuPickerButton::setItems(const QList<Item>& items) {
    // Rebuild from scratch: the actions are owned by the menu, the group only references them.
    const QList<QAction*> old = m_menu->actions();
    for (QAction* action : old) {
        m_group->removeAction(action);
        m_menu->removeAction(action);
        action->deleteLater();
    }
    m_items = items;

    bool anyToolTip = false;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items.at(i);
        QAction* action = m_menu->addAction(item.label);
        action->setCheckable(true);
        if (!item.toolTip.isEmpty()) {
            action->setToolTip(item.toolTip);
            anyToolTip = true;
        }
        m_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, i]() {
            // A user pick: mirror it in the caption, then tell the owner. Re-picking the current
            // entry still emits — owners treat it as a no-op change (undo registers nothing).
            m_currentIndex = i;
            updateCaption();
            emit currentChanged(i);
        });
        if (item.separatorAfter) m_menu->addSeparator();
    }
    // QMenu shows action tooltips only when opted in.
    m_menu->setToolTipsVisible(anyToolTip);

    m_currentIndex = m_items.isEmpty() ? -1 : 0;
    setCurrentIndex(m_currentIndex);
}

void MenuPickerButton::setCaptionPrefix(const QString& prefix) {
    m_prefix = prefix;
    updateCaption();
}

void MenuPickerButton::setCurrentIndex(int index) {
    if (index < 0 || index >= m_items.size()) index = -1;
    m_currentIndex = index;
    // setChecked never re-fires triggered, so this can't loop back into currentChanged.
    const QList<QAction*> actions = m_group->actions();
    for (int i = 0; i < actions.size(); ++i) actions.at(i)->setChecked(i == index);
    updateCaption();
}

void MenuPickerButton::setDetachedCaption(const QString& caption) {
    m_currentIndex = -1;
    const QList<QAction*> actions = m_group->actions();
    for (QAction* action : actions) action->setChecked(false);
    setText(m_prefix + caption);
}

void MenuPickerButton::updateCaption() {
    if (m_currentIndex >= 0 && m_currentIndex < m_items.size()) {
        setText(m_prefix + m_items.at(m_currentIndex).label);
    }
}

} // namespace pose
