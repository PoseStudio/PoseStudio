#ifndef MENUMANAGER_H
#define MENUMANAGER_H

#include <QObject>
#include <QMainWindow>

/**
 * @class MenuManager
 * @brief Builds and owns the application's top menu bar.
 */
class MenuManager : public QObject {
    Q_OBJECT

public:
    explicit MenuManager(QMainWindow *parent = nullptr);

    /// Constructs the File/Edit/Help menus and attaches them to the main window.
    void setupMenus();

private:
    QMainWindow *mainWindow;
};

#endif // MENUMANAGER_H