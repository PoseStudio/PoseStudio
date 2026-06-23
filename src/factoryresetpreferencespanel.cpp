/**
 * @file factoryresetpreferencespanel.cpp
 * @brief Implements the inline "Factory Reset" preferences page.
 */

#include "factoryresetpreferencespanel.h"
#include "database.h"

#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QApplication>
#include <QProcess>

FactoryResetPreferencesPanel::FactoryResetPreferencesPanel(QWidget* parent)
    : PreferencesPanel(QStringLiteral("Factory Reset"), parent) {

    addDescription(
        "Permanently deletes all your asset libraries, collections, and preferences. "
        "This cannot be undone. PoseStudio will restart automatically once the reset is complete.");

    addDescription("To confirm, type RESET into the box below, then click \"Factory Reset\".");

    auto* confirmInput = new QLineEdit(this);
    confirmInput->setObjectName(QStringLiteral("FactoryResetConfirmInput"));
    confirmInput->setPlaceholderText(QStringLiteral("Type RESET to confirm"));
    confirmInput->setMaximumWidth(240);
    contentLayout()->addWidget(confirmInput);

    auto* resetButton = new QPushButton(QStringLiteral("Factory Reset"), this);
    resetButton->setObjectName(QStringLiteral("FactoryResetButton"));
    resetButton->setEnabled(false);
    contentLayout()->addWidget(resetButton, 0, Qt::AlignLeft);

    // Enable the reset only once the user has typed the confirmation word (case-insensitive).
    connect(confirmInput, &QLineEdit::textChanged, this, [resetButton](const QString& text) {
        resetButton->setEnabled(text.trimmed().compare(QLatin1String("reset"), Qt::CaseInsensitive) == 0);
    });

    // Wipe and rebuild posestudio.db, then relaunch: many widgets (the asset tree,
    // PreferencesManager's cache) hold in-memory state derived from the old data, so rather
    // than reconciling all of it live we just restart the process after the reset.
    connect(resetButton, &QPushButton::clicked, this, []() {
        initializeDatabase(DbInitMode::FactoryReset);
        QProcess::startDetached(QApplication::applicationFilePath(), QApplication::arguments());
        QApplication::quit();
    });
}
