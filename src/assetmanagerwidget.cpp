#include "assetmanagerwidget.h"

AssetManagerWidget::AssetManagerWidget(QWidget *parent) : QWidget(parent) {
    setupUI();
}

void AssetManagerWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); 

    titleLabel = new QLabel("Asset Manager", this);
    // Give it a unique ID instead of inline CSS
    titleLabel->setObjectName("AssetManagerTitle"); 
    
    refreshButton = new QPushButton("Scan for Assets", this);
    // Give it a unique ID instead of inline CSS
    refreshButton->setObjectName("AssetManagerRefreshBtn");

    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(refreshButton);
    mainLayout->addStretch(); 
}