#include "database.h"
#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDebug>


int main(int argc, char *argv[]) {

    // Initializes the Qt application loop
    QApplication app(argc, argv);

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

    // Creates a basic text label widget
    QLabel label("Hello World! Welcome to PoseStudio.");
    
    // Set the window size and center the text
    label.resize(400, 200);
    label.setAlignment(Qt::AlignCenter);
    
    // Display the widget on screen
    label.show();

    // Pass control to Qt's event loop
    return app.exec();
}

