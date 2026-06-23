/**
 * @file preferencespanel.h
 * @brief Base class for a single page in the Preferences dialog.
 */

#ifndef PREFERENCESPANEL_H
#define PREFERENCESPANEL_H

#include <QWidget>

class QVBoxLayout;
class QLabel;

/**
 * @class PreferencesPanel
 * @brief Shared scaffolding for one page of the Preferences dialog.
 *
 * Provides the chrome every preferences page needs — a heading plus a top-aligned content
 * area — so concrete panels only append their own setting rows via contentLayout(). Each
 * tab lives in its own subclass/file (e.g. GeneralPreferencesPanel) so the areas can be
 * built out independently without touching the dialog or each other.
 */
class PreferencesPanel : public QWidget {
    Q_OBJECT

public:
    explicit PreferencesPanel(const QString& title, QWidget* parent = nullptr);

protected:
    /// Layout subclasses append their setting rows into; stays top-aligned within the panel.
    QVBoxLayout* contentLayout() const { return m_contentLayout; }

    /// Convenience for placeholder pages: drops a single muted message into the content area.
    void addPlaceholder(const QString& message);

    /// Adds standardized, word-wrapped body/explainer copy below the heading — smaller and
    /// more muted than the global QLabel default, and consistent across every panel that uses
    /// it (rather than each panel hand-rolling its own QLabel + font-size).
    QLabel* addDescription(const QString& text);

private:
    QVBoxLayout* m_contentLayout = nullptr;
};

#endif // PREFERENCESPANEL_H
