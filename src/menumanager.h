#ifndef MENUMANAGER_H
#define MENUMANAGER_H

#include <QObject>
#include <QMainWindow>

class MenuManager : public QObject {
    Q_OBJECT

public:
    // The constructor takes the main window so it knows where to draw the menus
    explicit MenuManager(QMainWindow *parent = nullptr);
    
    // The function that builds everything
    void setupMenus();

private:
    QMainWindow *mainWindow;
};

#endif // MENUMANAGER_H