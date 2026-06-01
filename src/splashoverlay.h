/**
 * @file splashoverlay.h
 * @brief Defines the SplashOverlay class for application branding and "About" screens.
 * * This is a lightweight, header-only implementation of a transparent overlay. 
 * It intercepts resize events from the main window to ensure the splash graphic 
 * remains perfectly centered, and safely self-destructs when clicked.
 */

#ifndef SPLASHOVERLAY_H
#define SPLASHOVERLAY_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QEvent>
#include <QPixmap>

/**
 * @class SplashOverlay
 * @brief A full-window, click-to-dismiss image overlay.
 * * Designed to be instantiated over the QMainWindow during the initial boot sequence 
 * or when a user clicks "About PoseStudio". It blocks underlying UI interactions 
 * until the user clicks anywhere on the screen to dismiss it.
 */
class SplashOverlay : public QWidget {
public:
    /**
     * @brief Constructs the SplashOverlay and binds it to a parent window.
     * @param parent The widget (usually QMainWindow) this overlay will cover.
     */
    SplashOverlay(QWidget *parent) : QWidget(parent) {
        // Enable CSS styling on this custom QWidget and make the background completely transparent
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet("background-color: rgba(0, 0, 0, 0);");

        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *imageLabel = new QLabel(this);
        
        // Define the target dimensions for the branding graphic
        int targetWidth = 740;  
        int targetHeight = 555; 
        
        // Load and cleanly downsample the splash graphic to prevent pixelation
        QPixmap originalPixmap(":/resources/splash_screen.png");
        QPixmap scaledPixmap = originalPixmap.scaled(
            targetWidth, 
            targetHeight, 
            Qt::KeepAspectRatio,       
            Qt::SmoothTransformation   
        );
        
        imageLabel->setPixmap(scaledPixmap);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setStyleSheet("background-color: transparent;"); 
        layout->addWidget(imageLabel);

        // Bind to the parent window to track its geometry
        if (parent) {
            // Install an event filter to intercept the parent's resize events
            parent->installEventFilter(this);
            
            // Instantly snap to the parent's current size
            resize(parent->size()); 
        }
    }

protected:
    /**
     * @brief Catches user clicks to dismiss the overlay.
     * @param event The mouse event payload.
     */
    void mousePressEvent(QMouseEvent *event) override {
        Q_UNUSED(event);
        
        // CRITICAL: Use deleteLater() instead of 'delete this'. 
        // This tells the Qt Event Loop to safely destroy the widget once it finishes 
        // processing the current click event, preventing memory access violations.
        deleteLater(); 
    }

    /**
     * @brief Intercepts events from the parent window.
     * @param watched The object that generated the event (expected to be the parent).
     * @param event The event being intercepted.
     * @return true if the event was handled, false to pass it down the chain.
     */
    bool eventFilter(QObject *watched, QEvent *event) override {
        // If the main window is resized by the user, resize the overlay to match
        if (watched == parent() && event->type() == QEvent::Resize) {
            resize(static_cast<QWidget*>(parent())->size());
        }
        
        // Allow the default event processing to continue
        return QWidget::eventFilter(watched, event);
    }
};

#endif // SPLASHOVERLAY_H