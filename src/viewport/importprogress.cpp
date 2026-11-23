// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file importprogress.cpp
 * @brief An import's progress dialog and phase clock. See importprogress.h.
 */

#include "importprogress.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QProgressDialog>
#include <QWidget>

namespace pose {

ImportProgress::ImportProgress(bool show, const QString& windowTitle, const QString& readingLabel) {
    m_clock.start();
    if (!show) {
        return;
    }
    m_dialog = std::make_unique<QProgressDialog>(readingLabel, QString() /*no cancel button*/, 0, 0,
                                                 QApplication::activeWindow());
    m_dialog->setWindowTitle(windowTitle);
    m_dialog->setWindowModality(Qt::ApplicationModal);
    m_dialog->setMinimumDuration(0); // don't wait for the default ~4s estimate before showing
    m_dialog->setValue(0);           // range 0,0 => a busy indicator while the file parses
    // Force it on-screen and paint it NOW, before the parse blocks the GUI thread (see the class
    // comment for why QProgressDialog's own auto-show would come too late).
    m_dialog->show();
    QApplication::processEvents();
}

ImportProgress::~ImportProgress() = default;

void ImportProgress::parsed() {
    m_parseMs = m_clock.elapsed();
    if (m_dialog) {
        m_dialog->setRange(0, kSteps); // switch the busy indicator to a determinate bar
        m_dialog->setValue(++m_step);  // parse complete
    }
}

void ImportProgress::phase(const QString& label) {
    if (m_dialog) {
        m_dialog->setLabelText(label);
        m_dialog->setValue(++m_step);
    }
}

void ImportProgress::done() {
    if (m_dialog) {
        m_dialog->setValue(kSteps); // complete -> dialog closes
    }
}

void reportImportFailure(bool interactive, const QString& title, const QString& path, const QString& detail) {
    static const bool kScriptedRun = qEnvironmentVariableIsSet("POSESTUDIO_IK_SCRIPT");
    if (!interactive || kScriptedRun) {
        return;
    }
    // Over the window the import was asked from. (The progress dialog that has just closed may
    // have been the active window until this moment, and nothing is active before the event
    // loop turns: the application's main window then.)
    QWidget* parent = QApplication::activeWindow();
    if (parent == nullptr) {
        for (QWidget* candidate : QApplication::topLevelWidgets()) {
            if (candidate->isVisible() && candidate->inherits("QMainWindow")) {
                parent = candidate;
                break;
            }
        }
    }
    QMessageBox::warning(parent, title, QObject::tr("Could not import \"%1\".\n\n%2").arg(QDir::toNativeSeparators(path), detail));
}

} // namespace pose
