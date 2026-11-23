// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file viewportwidget_testhooks.cpp
 * @brief ViewportWidget's side of the scripted in-app test (POSESTUDIO_IK_SCRIPT): the commands
 *        that reach OUTSIDE the viewport, to the application's widgets and its main window.
 *
 * The script runner lives on VulkanWindow (vulkanwindow_script.cpp), which knows nothing of the
 * widgets around it; what a script asks of them arrives here as the window's signals: `uishot`
 * (a picture of the widgets), `ui` (one widget driven through its own slot, and read back),
 * `app` (input as it arrives at the application with the viewport not focused) and `windowsize`.
 * A translation unit of its own so that the facade's file stays free of the widget classes a
 * test drives. The hooks are connected only when a script runs (nothing else emits the signals).
 */

#include "viewportwidget.h"

#include "dragnumberbox.h"
#include "menupickerbutton.h"
#include "viewportstrip.h"
#include "vulkanwindow.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace pose {

void ViewportWidget::connectScriptHooks() {
    connect(m_window, &VulkanWindow::scriptWindowSizeRequested, this, &ViewportWidget::scriptResizeWindow);
    connect(m_window, &VulkanWindow::uiShotRequested, this, &ViewportWidget::scriptUiShot);
    connect(m_window, &VulkanWindow::uiActionRequested, this, &ViewportWidget::scriptUiAction);
    // (Direct: the runner reads the answer when its emit returns.)
    connect(m_window, &VulkanWindow::uiQueryRequested, this, &ViewportWidget::scriptUiQuery, Qt::DirectConnection);
    connect(m_window, &VulkanWindow::appInputRequested, this, &ViewportWidget::scriptAppInput);
}

/// The scripted test's `windowsize`: the main window takes that size (logical pixels), as when
/// the user resizes it — out of a maximized or full-screen state first, which would ignore it.
void ViewportWidget::scriptResizeWindow(int width, int height) {
    if (QWidget* top = window()) {
        if (top->isMaximized() || top->isFullScreen()) {
            top->showNormal();
        }
        top->resize(width, height);
    }
}

/// The scripted test's `uishot`: the main window's widgets as a picture (the panels beside the
/// viewport; the native viewport itself is not in a widget grab — `shot` reads that off the GPU),
/// and the floating strip, a window of its own, beside it as <name>_strip.png (its badge).
void ViewportWidget::scriptUiShot(const QString& path) {
    // The picture of the widgets AS LAID OUT: a tab that has just rebuilt or unfolded part of
    // itself (a section opening for the zone clicked a line earlier, an overlay's card) has
    // only POSTED its layout request — grabbed now it is drawn at its stale, squeezed
    // geometry. (Twice: a layout pass can post the next one up the hierarchy.)
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (QWidget* top = window()) {
        top->grab().save(path);
        // ... and an open DIALOG (the Preferences dialog a script opened with `prefs`), a
        // window of its own, beside it as <name>_dialog.png.
        if (QWidget* dialog = QApplication::activeModalWidget(); dialog != nullptr && dialog != top) {
            QString dialogPath = path;
            dialogPath.replace(QStringLiteral(".png"), QStringLiteral("_dialog.png"));
            dialog->grab().save(dialogPath);
        }
        // ... and the Render window, a window of its own too, as <name>_render.png.
        if (QWidget* render = top->findChild<QWidget*>(QStringLiteral("RenderWindow")); render != nullptr && render->isVisible()) {
            QString renderPath = path;
            renderPath.replace(QStringLiteral(".png"), QStringLiteral("_render.png"));
            render->grab().save(renderPath);
        }
    }
    if (m_strip) {
        QString stripPath = path;
        stripPath.replace(QStringLiteral(".png"), QStringLiteral("_strip.png"));
        m_strip->grab().save(stripPath);
    }
}

/// The scripted test's `ui`: a widget of the application, addressed by its `testId` dynamic
/// property, driven through its own slot (a click, a scrub value, a check, a picker entry)
/// and read back — how a script checks that a panel's CONTROLS do what its commands do.
/// findTestWidget is the address: the @p index-th widget (in creation order) carrying that id.
QWidget* ViewportWidget::findTestWidget(const QString& id, int index) const {
    QWidget* top = window();
    if (top == nullptr || index < 0) {
        return nullptr;
    }
    int seen = 0;
    const QList<QWidget*> all = top->findChildren<QWidget*>();
    for (QWidget* widget : all) {
        if (widget->property("testId").toString() == id && seen++ == index) {
            return widget;
        }
    }
    return nullptr;
}

/// `ui <verb> <id> <n> ...`: the widget driven (see VulkanWindow::uiActionRequested for the verbs).
void ViewportWidget::scriptUiAction(const QString& verb, const QString& id, int index, double value, const QString& text) {
    QWidget* target = findTestWidget(id, index);
    if (target == nullptr) {
        qWarning().noquote() << "[ui] no widget" << id << index;
        return;
    }
    if (verb == QLatin1String("click")) {
        if (auto* button = qobject_cast<QAbstractButton*>(target); button != nullptr && button->isEnabled()) {
            button->click();
        }
    } else if (verb == QLatin1String("set")) {
        if (auto* box = qobject_cast<DragNumberBox*>(target); box != nullptr && box->isEnabled()) {
            box->setValue(value);
        }
    } else if (verb == QLatin1String("check")) {
        if (auto* box = qobject_cast<QCheckBox*>(target); box != nullptr && box->isEnabled()) {
            box->setChecked(value >= 0.5);
        }
    } else if (verb == QLatin1String("call") || verb == QLatin1String("post")) {
        // One of the widget's own slots / invokables taking an int — what a context menu's
        // entry runs (a menu's exec() cannot be driven from here): "<method> [int]". `post`
        // runs it from the event loop instead of here and now: for a slot that opens a MODAL
        // dialog, whose exec() would otherwise hold the script that is to look at it.
        const QStringList part = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const auto        how = verb == QLatin1String("post") ? Qt::QueuedConnection : Qt::DirectConnection;
        if (!part.isEmpty() && !QMetaObject::invokeMethod(target, part[0].toLatin1().constData(), how,
                                                          Q_ARG(int, part.size() > 1 ? part[1].toInt() : 0))) {
            qWarning().noquote() << "[ui] no slot" << part[0] << "on" << id;
        }
    } else if (verb == QLatin1String("type")) {
        if (auto* edit = qobject_cast<QLineEdit*>(target); edit != nullptr && edit->isEnabled()) {
            edit->setText(text);
        }
    } else if (verb == QLatin1String("key")) {
        // Enter or Esc to the widget itself, through its event filters: an inline editor's commit and cancel.
        const int key = text.compare(QLatin1String("esc"), Qt::CaseInsensitive) == 0 ? Qt::Key_Escape : Qt::Key_Return;
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &press);
    } else if (verb == QLatin1String("scrub")) {
        // A SCRUB of a field: a left press at its middle, mouse moves to @p value PERCENT OF ITS
        // WIDTH to the right of it (negative: left) and a release — with Shift held when the
        // text says so — sent to the widget itself, through its own mouse handlers (the snap to
        // the step, the fine scrub, the edit bracket). "<% of the width> [shift]". A share of
        // the width, not pixels: a box sweeps its range across its width, so the value a scrub
        // lands on is the same however wide the panel happens to be.
        if (target->isEnabled()) {
            const bool                  shift = text.contains(QLatin1String("shift"), Qt::CaseInsensitive);
            const Qt::KeyboardModifiers mods = shift ? Qt::ShiftModifier : Qt::NoModifier;
            const QPointF               at(target->width() * 0.5, target->height() * 0.5);
            const double                dx = value / 100.0 * std::max(1, target->width());
            const auto send = [target, mods](QEvent::Type type, const QPointF& where, Qt::MouseButton button, Qt::MouseButtons held) {
                QMouseEvent e(type, where, target->mapToGlobal(where), button, held, mods);
                QCoreApplication::sendEvent(target, &e);
            };
            send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
            const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(dx) / 4.0)));
            for (int k = 1; k <= steps; ++k) {
                send(QEvent::MouseMove, at + QPointF(dx * k / steps, 0.0), Qt::NoButton, Qt::LeftButton);
            }
            send(QEvent::MouseButtonRelease, at + QPointF(dx, 0.0), Qt::LeftButton, Qt::NoButton);
        }
    } else if (verb == QLatin1String("pick")) {
        if (auto* picker = qobject_cast<MenuPickerButton*>(target); picker != nullptr && picker->isEnabled()) {
            int entry = 0;
            for (QAction* action : picker->pickerMenu()->actions()) {
                if (!action->isSeparator() && entry++ == static_cast<int>(value)) {
                    action->trigger();
                    break;
                }
            }
        }
    }
}

/// The `uicount.` / `uivalue.` / `uienabled.` / `uivisible.` metrics: the widget read back.
void ViewportWidget::scriptUiQuery(const QString& what, const QString& id, int index, double* value, bool* ok) {
    if (what == QLatin1String("count")) {
        int count = 0;
        if (QWidget* top = window()) {
            const QList<QWidget*> all = top->findChildren<QWidget*>();
            for (QWidget* widget : all) {
                count += widget->property("testId").toString() == id ? 1 : 0;
            }
        }
        *value = count;
        *ok = true;
        return;
    }
    QWidget* target = findTestWidget(id, index);
    if (target == nullptr) {
        *ok = false;
        return;
    }
    *ok = true;
    if (what == QLatin1String("enabled")) {
        *value = target->isEnabled() ? 1.0 : 0.0;
    } else if (what == QLatin1String("visible")) {
        *value = target->isVisibleTo(window()) ? 1.0 : 0.0;
    } else if (auto* label = qobject_cast<QLabel*>(target)) {
        *value = label->text().toDouble(ok); // (a label that is a number: a count)
    } else if (auto* box = qobject_cast<DragNumberBox*>(target)) {
        *value = box->value();
    } else if (auto* check = qobject_cast<QCheckBox*>(target)) {
        *value = check->isChecked() ? 1.0 : 0.0;
    } else if (auto* picker = qobject_cast<MenuPickerButton*>(target)) {
        *value = picker->currentIndex();
    } else if (auto* button = qobject_cast<QAbstractButton*>(target)) {
        *value = button->isChecked() ? 1.0 : 0.0;
    } else {
        *ok = false;
    }
}

/// The scripted test's `app`: the mode's input as it reaches the application with the viewport
/// NOT focused. The keys are the Edit menu's actions (found by their shortcut, and only if
/// enabled, as the shortcut map would); Esc, Enter, the clicks and the moves are events sent
/// to the main window, through the application's event filters like any other.
void ViewportWidget::scriptAppInput(const QString& what, const QPointF& delta, int steps) {
    QWidget* top = window();
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (!handle || !m_window) {
        return;
    }
    const auto sendKey = [handle](int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &press);
    };
    const auto sendMouse = [this, handle](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent e(type, handle->mapFromGlobal(m_scriptCursor), m_scriptCursor, button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &e);
    };
    if (what == QLatin1String("b") || what == QLatin1String("s") || what == QLatin1String("t")) {
        const QKeySequence key(what == QLatin1String("b") ? Qt::Key_B : what == QLatin1String("s") ? Qt::Key_S : Qt::Key_T);
        m_scriptCursor = m_window->mapToGlobal(QPointF(m_window->width() * 0.5, m_window->height() * 0.5));
        const QList<QAction*> actions = top->findChildren<QAction*>();
        for (QAction* action : actions) {
            if (action->shortcut() == key && action->isEnabled()) {
                action->trigger();
                break;
            }
        }
    } else if (what == QLatin1String("esc")) {
        sendKey(Qt::Key_Escape);
    } else if (what == QLatin1String("enter")) {
        sendKey(Qt::Key_Return);
    } else if (what == QLatin1String("click") || what == QLatin1String("rclick")) {
        const Qt::MouseButton button = what == QLatin1String("click") ? Qt::LeftButton : Qt::RightButton;
        sendMouse(QEvent::MouseButtonPress, button, button);
        sendMouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
    } else if (what == QLatin1String("move")) {
        for (int k = 0; k < steps; ++k) {
            m_scriptCursor += delta / steps;
            sendMouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
        }
    }
}

} // namespace pose
