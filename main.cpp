#include "database.h"
#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRect>
#include <QScreen>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVBoxLayout>
#include <QWidget>

// Create a custom Overlay Widget
class SplashOverlay : public QWidget {
public:
    SplashOverlay(QWidget *parent) : QWidget(parent) {
        // Required to allow custom QWidgets to use CSS background colors
        setAttribute(Qt::WA_StyledBackground, true);
        
        // Make the background a semi-transparent black so the app behind it is dimmed.
        // (Change to "rgba(0,0,0,0);" if you want the background to be 100% invisible).
        setStyleSheet("background-color: rgba(0, 0, 0, 0);");

        // Use a layout to perfectly center the splash image in the middle of the app
        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *imageLabel = new QLabel(this);
        
        // --- NEW RESIZING LOGIC ---
        int targetWidth = 740;  // Change this to your preferred width
        int targetHeight = 555; // Change this to your preferred height
        
        QPixmap originalPixmap(":/resources/splash_screen.png");
        QPixmap scaledPixmap = originalPixmap.scaled(
            targetWidth, 
            targetHeight, 
            Qt::KeepAspectRatio,       // Prevents distortion
            Qt::SmoothTransformation   // Keeps the edges crisp and anti-aliased
        );
        
        imageLabel->setPixmap(scaledPixmap);
        // --------------------------

        imageLabel->setAlignment(Qt::AlignCenter);
        
        // Ensure the image label itself doesn't inherit the dark background
        imageLabel->setStyleSheet("background-color: transparent;"); 
        layout->addWidget(imageLabel);

        // Tell the overlay to listen to the parent window for resize events
        if (parent) {
            parent->installEventFilter(this);
            resize(parent->size()); // Snap to initial size
        }
    }

protected:
    // Intercept ANY click on the overlay to dismiss it
    void mousePressEvent(QMouseEvent *event) override {
        Q_UNUSED(event);
        // deleteLater() safely removes the widget from the screen and frees the memory
        deleteLater(); 
    }

    // Keep the overlay perfectly stretched if the user resizes the app window
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parent() && event->type() == QEvent::Resize) {
            resize(static_cast<QWidget*>(parent())->size());
        }
        return QWidget::eventFilter(watched, event);
    }
};

void setupMenus(QMainWindow *window) {

    // FILE MENU
    QMenu *fileMenu = window->menuBar()->addMenu("File");

    // MENU ITEM: UNDO
    QIcon newIcon;
    newIcon.addPixmap(QPixmap(":/resources/new.png"), QIcon::Normal);
    newIcon.addPixmap(QPixmap(":/resources/new-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(newIcon), "New...")->setEnabled(false);

    // MENU ITEM: OPEN
    QIcon openIcon;
    openIcon.addPixmap(QPixmap(":/resources/open.png"), QIcon::Normal);
    openIcon.addPixmap(QPixmap(":/resources/open-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(openIcon), "Open...")->setEnabled(false);

    // MENU ITEM: OPEN RECENT
    fileMenu->addAction("Open Recent...")->setEnabled(false);

    // MENU ITEM: SEPERATOR
    fileMenu->addSeparator();

    // MENU ITEM: SAVE
    QIcon saveIcon;
    saveIcon.addPixmap(QPixmap(":/resources/save.png"), QIcon::Normal);
    saveIcon.addPixmap(QPixmap(":/resources/save-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(saveIcon), "Save")->setEnabled(false);

    // MENU ITEM: SAVE AS
    fileMenu->addAction("Save As...")->setEnabled(false);

    // MENU ITEM: SAVE COPY
    fileMenu->addAction("Save Copy...")->setEnabled(false);

    // MENU ITEM: SEPERATOR
    fileMenu->addSeparator();

    // MENU ITEM: IMPORT
    QIcon importIcon;
    importIcon.addPixmap(QPixmap(":/resources/import.png"), QIcon::Normal);
    importIcon.addPixmap(QPixmap(":/resources/import-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(importIcon), "Import...")->setEnabled(false);

    // MENU ITEM: IMPORT
    QIcon exportIcon;
    exportIcon.addPixmap(QPixmap(":/resources/export.png"), QIcon::Normal);
    exportIcon.addPixmap(QPixmap(":/resources/export-d.png"), QIcon::Disabled);
    fileMenu->addAction(QIcon(exportIcon), "Export...")->setEnabled(false);

    // MENU ITEM: OPEN SEPERATOR
    fileMenu->addSeparator();
    
    // MENU ITEM: QUIT
    QAction *quitAction = fileMenu->addAction("Quit");
    quitAction->setShortcuts({QKeySequence("Ctrl+Q"), QKeySequence::Quit});
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);


    // EDIT MENU
    QMenu *editMenu = window->menuBar()->addMenu("Edit");

    // MENU ITEM: UNDO
    QIcon undoIcon;
    undoIcon.addPixmap(QPixmap(":/resources/undo.png"), QIcon::Normal);
    undoIcon.addPixmap(QPixmap(":/resources/undo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(undoIcon), "Undo")->setEnabled(false);

    // MENU ITEM: REDO
    QIcon redoIcon;
    redoIcon.addPixmap(QPixmap(":/resources/redo.png"), QIcon::Normal);
    redoIcon.addPixmap(QPixmap(":/resources/redo-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(redoIcon), "Redo")->setEnabled(false);

    // MENU ITEM: UNDO HISTORY
    editMenu->addAction("Undo History...")->setEnabled(false);

    // MENU ITEM: SEPERATOR
    editMenu->addSeparator();

    // MENU ITEM: COPY
    QIcon copyIcon;
    copyIcon.addPixmap(QPixmap(":/resources/copy.png"), QIcon::Normal);
    copyIcon.addPixmap(QPixmap(":/resources/copy-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(copyIcon), "Copy")->setEnabled(false);

    // MENU ITEM: PASTE
    editMenu->addAction("Paste")->setEnabled(false);

    // MENU ITEM: SEPERATOR
    editMenu->addSeparator();

    // MENU ITEM: PREFERENCES
    QIcon preferencesIcon;
    preferencesIcon.addPixmap(QPixmap(":/resources/preferences.png"), QIcon::Normal);
    preferencesIcon.addPixmap(QPixmap(":/resources/preferences-d.png"), QIcon::Disabled);
    editMenu->addAction(QIcon(preferencesIcon), "Preferences")->setEnabled(false);


    // HELP MENU
    QMenu *helpMenu = window->menuBar()->addMenu("Help");

    // MENU ITEM: RELEASE NOTES
    helpMenu->addAction("Release Notes")->setEnabled(false);

    // MENU ITEM: TUTORIALS
    QIcon tutorialsIcon;
    tutorialsIcon.addPixmap(QPixmap(":/resources/tutorials.png"), QIcon::Normal);
    tutorialsIcon.addPixmap(QPixmap(":/resources/tutorials-d.png"), QIcon::Disabled);
    helpMenu->addAction(QIcon(tutorialsIcon), "Tutorials")->setEnabled(false);

    // MENU ITEM: RELEASE NOTES
    helpMenu->addAction("Support")->setEnabled(false);

    // MENU ITEM: SEPERATOR
    helpMenu->addSeparator();


    // MENU ITEM: ABOUT
    QIcon aboutIcon;
    aboutIcon.addPixmap(QPixmap(":/resources/about.png"), QIcon::Normal);
    aboutIcon.addPixmap(QPixmap(":/resources/about-d.png"), QIcon::Disabled);
    helpMenu->addAction(QIcon(aboutIcon), "About PoseStudio")->setEnabled(true);

    // Connect the Quit action using the global application instance
    QObject::connect(quitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);
    
}

int main(int argc, char *argv[]) {

    // Initializes the Qt application loop
    QApplication app(argc, argv);

    int initDb = 0;
    if (initDb == 1) {
        initializeDatabase();

        // Connect to database
        QSqlDatabase db = connectDatabase();

        // Attempt to open the connection
        if (!db.open()) {
            qCritical() << "Database Error: Could not connect to SQLite database.";
            qCritical() << "Reason:" << db.lastError().text();
            return false;
        }

        // Select record to test database
        QSqlQuery query(db);
        if (!query.exec("SELECT appErrorText FROM appErrors")) {
            QMessageBox::critical(nullptr, "Database Error 3", 
                                    "Failed to query database:\n" + query.lastError().text());
        }
        if (query.next()) {
            QString poseName = query.value("appErrorText").toString();
            QMessageBox::information(nullptr, "Pose Found", 
                                    "The name of the pose is: " + poseName);
        }

    }

    app.setWindowIcon(QIcon("resources/icon.png"));

    app.setStyle("Fusion");
    app.setStyleSheet("QMainWindow { background-color: #191a1b; }"
                      "QLabel { color: #ffffff; font-size: 18px; padding: 4px;}"
                      
                      /* Added border-radius and slight padding to the main dropdown menu */
                      "QMenu { background-color: #323232; color: white; border: 1px solid #555; border-radius: 4px; padding: 4px; }"

                      /* Added a slightly smaller radius to the hover highlight so it fits inside the menu */
                      "QMenu::item { padding: 6px 4px 6px 6px; }"
                      
                      /* Added a slightly smaller radius to the hover highlight so it fits inside the menu */
                      "QMenu::item:selected { background-color: #555555; border-radius: 4px; }"

                      /* Change color for disabled menu items */
                      "QMenu::item:disabled { color: #929292;}"
                      
                      "QMenuBar { background-color: #191a1b; color: white; padding: 4px; border-bottom: 1px solid #313131;}" 

                      /* ---> Padding for the text items on the top bar <--- */
                      "QMenuBar::item { padding: 6px 12px;}"
                      
                      /* Optional: Round the top menu bar highlights as well */
                      "QMenuBar::item:selected { background-color: #323232; border-radius: 4px; }");

    // Create the Main Window
    QMainWindow mainWindow;
    mainWindow.setWindowTitle("PoseStudio 0.0.015");

    // Get the primary screen the application is running on
    QScreen *screen = app.primaryScreen();
    
    // Get the available geometry (this safely excludes the Windows taskbar)
    QRect screenGeometry = screen->availableGeometry();

    int windowHeight = screenGeometry.height() * 0.80;
    int windowWidth = screenGeometry.height() * 1.4;
    mainWindow.resize(windowWidth, windowHeight); // Give your main window a starting size

    setupMenus(&mainWindow);

    // Display the Main Window on screen
    mainWindow.show();

    // Attach the overlay directly to the main window AFTER it is shown
    SplashOverlay *splash = new SplashOverlay(&mainWindow);
    splash->show();

    // Enter the main event loop and wait until exit() is called
    return app.exec();
}

