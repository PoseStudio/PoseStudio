// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file jointmodalinput.cpp
 * @brief Implementation of JointModalInput. See jointmodalinput.h.
 */

#include "jointmodalinput.h"

#include "vulkanwindow.h"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>

namespace pose {

JointModalInput::JointModalInput(VulkanWindow* window, QObject* parent) : QObject(parent), m_window(window) {}

JointModalInput::~JointModalInput() {
    setActive(false);
    uninstall();
}

void JointModalInput::install() {
    if (!m_installed && QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(this);
        m_installed = true;
    }
}

void JointModalInput::uninstall() {
    if (m_installed && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    m_installed = false;
}

void JointModalInput::setActive(bool on) {
    // The mode is the mouse's wherever the pointer is: say so AT the pointer, application-wide
    // (the badge is in the viewport's corner). A four-way arrow, as a modal move has in the DCCs.
    if (on != m_active) {
        if (on) {
            QGuiApplication::setOverrideCursor(QCursor(Qt::SizeAllCursor));
        } else {
            QGuiApplication::restoreOverrideCursor();
        }
    }
    m_active = on;
    if (on) {
        m_swallow = Qt::NoButton;
        install();
    } else if (m_swallow == Qt::NoButton) {
        uninstall(); // (else: once the consumed click's release has gone by)
    }
}

bool JointModalInput::eventFilter(QObject* watched, QEvent* event) {
    // Installed on the application, this sees every event in the process: the type first.
    const QEvent::Type type = event->type();
    const bool         mouse = type == QEvent::MouseMove || type == QEvent::MouseButtonPress ||
                               type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick;
    if (!mouse && type != QEvent::KeyPress) {
        return false;
    }
    // An event as it arrives at a WINDOW, before a widget gets its copy (one physical event, one
    // reading) — and never the viewport's own: its handlers do the same there.
    if (!m_window || !watched->isWindowType() || watched == m_window) {
        return false;
    }
    // (A scripted test drives the mode itself and runs on a desktop in use: real input is not part of it.)
    if (m_window->scriptRunning() && event->spontaneous()) {
        return false;
    }
    if (!m_active) {
        // The mode is over: only the release of a click it consumed is still its own.
        if (type == QEvent::MouseButtonRelease) {
            const Qt::MouseButton button = static_cast<QMouseEvent*>(event)->button();
            if (m_swallow & button) {
                m_swallow &= ~button;
                if (m_swallow == Qt::NoButton) {
                    uninstall();
                }
                return true;
            }
        } else if (type == QEvent::MouseButtonPress) {
            m_swallow = Qt::NoButton; // (that release never came: a new press is nobody's but its widget's)
            uninstall();
        }
        return false;
    }
    switch (type) {
    case QEvent::MouseMove:
        m_window->moveJointModalTo(static_cast<QMouseEvent*>(event)->globalPosition());
        return false; // (hover effects under the pointer are harmless)
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const Qt::MouseButton button = static_cast<QMouseEvent*>(event)->button();
        m_swallow |= button; // before the mode ends below: setActive(false) keeps the filter for the release
        if (button == Qt::LeftButton) {
            m_window->confirmJointModal();
        } else if (button == Qt::RightButton) {
            m_window->cancelJointModal();
        }
        return true; // the click is the mode's: the widget under it never sees it
    }
    case QEvent::MouseButtonRelease: {
        const Qt::MouseButton button = static_cast<QMouseEvent*>(event)->button();
        if (m_swallow & button) {
            m_swallow &= ~button;
            return true;
        }
        return false;
    }
    case QEvent::KeyPress: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) {
            m_window->cancelJointModal();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            m_window->confirmJointModal();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

} // namespace pose
