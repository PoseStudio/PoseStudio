// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file licensenotice.cpp
 * @brief Implementation of Help → License (see licensenotice.h for why its wording matters).
 */

#include "licensenotice.h"
#include "constants.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

namespace {

constexpr const char* kSourceUrl = "https://github.com/PoseStudio/PoseStudio";

/// A link in the box's rich text. Qt's style sheets do not reach an <a> in a label, so the
/// accent color is set inline (the Asset Manager's hint label does the same).
QString link(const QString& url, const QString& text) {
    return QStringLiteral("<a href=\"%1\" style=\"color:%2;\">%3</a>")
        .arg(url, QString::fromLatin1(Constants::COLOR_ACCENT), text.toHtmlEscaped());
}

/// The license to open: the copy the package ships next to the executable, or the repository's.
QUrl licenseUrl() {
    const QString shipped = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("LICENSE.txt"));
    if (QFileInfo::exists(shipped)) {
        return QUrl::fromLocalFile(shipped);
    }
    return QUrl(QString::fromLatin1(kSourceUrl) + QStringLiteral("/blob/main/LICENSE"));
}

} // namespace

void LicenseNotice::show(QWidget* parent) {
    const QString source = QString::fromLatin1(kSourceUrl);
    const QString docs = source + QStringLiteral("/blob/main/");

    auto* box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("LicenseNotice"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QObject::tr("License"));
    box->setIcon(QMessageBox::NoIcon);
    box->setTextFormat(Qt::RichText);
    box->setTextInteractionFlags(Qt::TextBrowserInteraction);
    // Wide enough for the attribution line on one line: the box sizes itself to its text, and a
    // narrower one wraps the address after "https://" (Qt's rich text has no <nobr>).
    box->setStyleSheet(QStringLiteral("QLabel { min-width: 540px; }"));

    // The copyright notice and the attribution are the lines the additional terms ask a version
    // based on PoseStudio to preserve; they are not translated (a notice is quoted, not paraphrased).
    box->setText(QStringLiteral("<p><b>PoseStudio %1</b><br>%2<br>%3</p>")
                     .arg(QString::fromLatin1(Constants::APP_VERSION),
                          QStringLiteral("Copyright &copy; 2026 Ion Hatzithomas and the PoseStudio contributors."),
                          QString::fromLatin1(LicenseNotice::kAttribution).toHtmlEscaped()));

    box->setInformativeText(
        QObject::tr("<p>PoseStudio is free software: you can redistribute it and/or modify it under the "
                    "terms of the GNU Affero General Public License, version 3, with the additional "
                    "permission and terms in LICENSE-ADDITIONAL-TERMS.txt. Both files are in "
                    "PoseStudio's folder, and the source code is at %1.</p>"
                    "<p>PoseStudio comes with ABSOLUTELY NO WARRANTY, to the extent permitted by law.</p>"
                    "<p>\"PoseStudio\" and its logo are trademarks of Ion Hatzithomas (%2). "
                    "A commercial license for uses the open-source license does not allow is "
                    "available: see %3.</p>")
            .arg(link(source, QStringLiteral("github.com/PoseStudio/PoseStudio")),
                 link(docs + QStringLiteral("TRADEMARKS.md"), QObject::tr("trademark policy")),
                 link(docs + QStringLiteral("LICENSING.md"), QStringLiteral("LICENSING.md"))));

    QPushButton* view = box->addButton(QObject::tr("View License"), QMessageBox::ActionRole);
    box->addButton(QMessageBox::Close);
    box->setDefaultButton(QMessageBox::Close);
    QObject::connect(box, &QMessageBox::buttonClicked, box, [view](QAbstractButton* clicked) {
        if (clicked == view) {
            QDesktopServices::openUrl(licenseUrl());
        }
    });
    box->open();
}
