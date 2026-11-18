/**
 * @file factoryresetpreferencespanel.cpp
 * @brief Implements the inline "Factory Reset" preferences page.
 */

#include "factoryresetpreferencespanel.h"

#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>

FactoryResetPreferencesPanel::FactoryResetPreferencesPanel(QWidget* parent)
    : PreferencesPanel(tr("Factory Reset"), parent) {

    addDescription(
        tr("Permanently deletes all your asset libraries, collections, and preferences. "
           "This cannot be undone. PoseStudio will restart automatically once the reset is complete."));

    addDescription(tr("To confirm, type RESET into the box below, then click \"Factory Reset\"."));

    auto* confirmInput = new QLineEdit(this);
    confirmInput->setObjectName(QStringLiteral("FactoryResetConfirmInput"));
    confirmInput->setPlaceholderText(tr("Type RESET to confirm"));
    confirmInput->setMaximumWidth(240);
    contentLayout()->addWidget(confirmInput);

    auto* resetButton = new QPushButton(tr("Factory Reset"), this);
    resetButton->setObjectName(QStringLiteral("FactoryResetButton"));
    resetButton->setEnabled(false);
    contentLayout()->addWidget(resetButton, 0, Qt::AlignLeft);

    // Enable the reset only once the user has typed the confirmation word (case-insensitive).
    connect(confirmInput, &QLineEdit::textChanged, this, [resetButton](const QString& text) {
        resetButton->setEnabled(text.trimmed().compare(QLatin1String("reset"), Qt::CaseInsensitive) == 0);
    });

    // The page only ASKS: the reset itself — the unsaved-changes prompt first, then the wipe and
    // the restart — is the shell's (DocumentActions::factoryReset), which owns the document and
    // the one way the application relaunches itself.
    connect(resetButton, &QPushButton::clicked, this, &FactoryResetPreferencesPanel::factoryResetRequested);
}
