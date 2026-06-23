/**
 * @file preferencespanel.cpp
 * @brief Implements the shared chrome for Preferences dialog pages.
 */

#include "preferencespanel.h"

#include <QVBoxLayout>
#include <QLabel>

PreferencesPanel::PreferencesPanel(const QString& title, QWidget* parent)
    : QWidget(parent) {
    // Plain QWidget subclasses don't honor stylesheet backgrounds unless flagged as styled.
    setObjectName(QStringLiteral("PreferencesPanel"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 20, 24, 20);
    // Tighter than the content rows' own spacing below — the heading should sit close to
    // whatever immediately follows it (usually a description), not float apart from it.
    outer->setSpacing(6);

    auto* heading = new QLabel(title, this);
    heading->setObjectName(QStringLiteral("PreferencesPanelHeading"));
    outer->addWidget(heading);

    m_contentLayout = new QVBoxLayout();
    m_contentLayout->setSpacing(10);
    outer->addLayout(m_contentLayout);

    // Keep the heading + content block pinned to the top, regardless of how little it holds.
    outer->addStretch(1);
}

void PreferencesPanel::addPlaceholder(const QString& message) {
    auto* label = new QLabel(message, this);
    label->setObjectName(QStringLiteral("PreferencesPlaceholder"));
    label->setWordWrap(true);
    m_contentLayout->addWidget(label);
}

QLabel* PreferencesPanel::addDescription(const QString& text) {
    auto* label = new QLabel(text, this);
    label->setObjectName(QStringLiteral("PreferencesDescription"));
    label->setWordWrap(true);
    m_contentLayout->addWidget(label);
    return label;
}
