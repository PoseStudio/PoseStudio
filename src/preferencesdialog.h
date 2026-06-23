/**
 * @file preferencesdialog.h
 * @brief Declares the PreferencesDialog popup.
 */

#ifndef PREFERENCESDIALOG_H
#define PREFERENCESDIALOG_H

#include <QDialog>

class QListWidget;
class QStackedWidget;
class PreferencesPanel;

/**
 * @class PreferencesDialog
 * @brief Modal popup for editing user preferences.
 *
 * A vertical tab list down the left drives a stack of panels on the right. Each panel is a
 * PreferencesPanel subclass living in its own file; new tabs are registered through
 * addPanel(), keeping the left nav and right stack in lock-step.
 */
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

    /// Jumps to the tab whose nav label matches `tabLabel` (e.g. "Assets"). No-op if not found.
    void selectTab(const QString& tabLabel);

signals:
    /// Forwarded from AssetsPreferencesPanel so callers don't need to know which tab owns it.
    void assetLibrariesChanged();

    /// Forwarded from AssetsPreferencesPanel when the user double-clicks a library row.
    /// The dialog closes itself right after, so the caller's navigation is actually visible.
    void navigateToLibraryRequested(const QString& path);

private:
    /// Registers a panel as a tab: adds its label to the left nav and its page to the stack.
    void addPanel(const QString& tabLabel, PreferencesPanel* panel);

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
};

#endif // PREFERENCESDIALOG_H
