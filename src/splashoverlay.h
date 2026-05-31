#ifndef SPLASHOVERLAY_H
#define SPLASHOVERLAY_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QEvent>
#include <QPixmap>

class SplashOverlay : public QWidget {
public:
    SplashOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet("background-color: rgba(0, 0, 0, 0);");

        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *imageLabel = new QLabel(this);
        
        int targetWidth = 740;  
        int targetHeight = 555; 
        
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

        if (parent) {
            parent->installEventFilter(this);
            resize(parent->size()); 
        }
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        Q_UNUSED(event);
        deleteLater(); 
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parent() && event->type() == QEvent::Resize) {
            resize(static_cast<QWidget*>(parent())->size());
        }
        return QWidget::eventFilter(watched, event);
    }
};

#endif // SPLASHOVERLAY_H