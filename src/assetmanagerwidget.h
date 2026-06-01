#ifndef ASSETMANAGERWIDGET_H
#define ASSETMANAGERWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
// We will eventually need QTreeView or QListView for the actual files!

class AssetManagerWidget : public QWidget {
    Q_OBJECT // This macro is MANDATORY for custom Qt widgets so they can use signals/slots!

public:
    explicit AssetManagerWidget(QWidget *parent = nullptr);
    ~AssetManagerWidget() = default;

private:
    // Pointers to the UI elements that belong to this panel
    QVBoxLayout *mainLayout;
    QLabel *titleLabel;
    QPushButton *refreshButton;

    // A private function to keep the constructor clean
    void setupUI();
};

#endif // ASSETMANAGERWIDGET_H