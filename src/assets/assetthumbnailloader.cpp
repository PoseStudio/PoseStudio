// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetthumbnailloader.cpp
 * @brief Implementation of AssetThumbnailLoader. See the header for the design.
 */

#include "assetthumbnailloader.h"
#include "constants.h"

#include <QFutureWatcher>
#include <QImageReader>
#include <QLinearGradient>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPixmap>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace {

// Canvas edge in DEVICE pixels for a given device pixel ratio.
int canvasPixels(qreal dpr) {
    return static_cast<int>(std::lround(Constants::GRID_ICON_DISPLAY_SIZE * dpr));
}

// The worker-side decode: header-only size probe, codec-scaled read, post-scale fallback for
// codecs that can't report a size up front, then the canvas. Returns a null image on failure.
QImage decodeThumbnail(const QString& imagePath, qreal dpr) {
    const int target = canvasPixels(dpr);

    // Decode at thumbnail size rather than decoding the full image and scaling down:
    // setScaledSize lets the codec do the reduction (JPEG can decode at 1/2..1/8 DCT scale),
    // which is what keeps big preview images cheap even off the GUI thread.
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    const QSize origSize = reader.size(); // header-only read for most formats
    if (origSize.isValid()) {
        reader.setScaledSize(origSize.scaled(target, target, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull()) return {};

    if (image.width() > target || image.height() > target) {
        // Codec couldn't report a size up front: fall back to scaling after decode.
        image = image.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return AssetThumbnailLoader::renderThumbnailCanvas(image, dpr);
}

// Wraps a canvas image into the icon the grid items carry (the same pixmap for Normal and
// Selected, so selection doesn't tint the thumbnail). GUI thread only (QPixmap).
QIcon iconFromCanvas(const QImage& canvas) {
    const QPixmap px = QPixmap::fromImage(canvas); // keeps the image's device pixel ratio
    QIcon icon;
    icon.addPixmap(px, QIcon::Normal);
    icon.addPixmap(px, QIcon::Selected);
    return icon;
}

} // namespace

AssetThumbnailLoader::AssetThumbnailLoader(QListWidget* grid, QObject* parent)
    : QObject(parent)
    , m_grid(grid)
    , m_generation(std::make_shared<std::atomic<int>>(0))
    , m_maxInFlight(qMax(2, QThread::idealThreadCount())) {}

AssetThumbnailLoader::~AssetThumbnailLoader() {
    // Watchers are children and go with us; running workers finish on their own and see the
    // bumped generation, so they skip the decode and their results are discarded.
    clear();
}

void AssetThumbnailLoader::enqueue(QListWidgetItem* item, const QString& imagePath) {
    if (!item || imagePath.isEmpty()) return;
    m_pending.append({item, imagePath});
    dispatch();
}

void AssetThumbnailLoader::clear() {
    m_pending.clear();
    m_generation->fetch_add(1);
}

QIcon AssetThumbnailLoader::folderIcon() {
    const qreal dpr = m_grid->devicePixelRatioF();
    if (!m_folderIcon.isNull() && qFuzzyCompare(dpr, m_folderIconDpr)) return m_folderIcon;

    const int edge = canvasPixels(dpr);
    QImage canvas(edge, edge, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        const QImage folderPx(QStringLiteral(":/resources/icons/folder.png"));
        const int iconSz = edge * 82 / 100;
        const QImage scaled = folderPx.scaled(iconSz, iconSz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&canvas);
        p.drawImage((edge - scaled.width()) / 2, (edge - scaled.height()) / 2, scaled);
    }
    canvas.setDevicePixelRatio(dpr);

    m_folderIcon = iconFromCanvas(canvas);
    m_folderIconDpr = dpr;
    return m_folderIcon;
}

QImage AssetThumbnailLoader::renderThumbnailCanvas(const QImage& image, qreal dpr) {
    const int edge = canvasPixels(dpr);
    QImage canvas(edge, edge, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    QLinearGradient grad(0, 0, 0, edge);
    grad.setColorAt(0.0, QColor(Constants::COLOR_THUMB_BG_START));
    grad.setColorAt(1.0, QColor(Constants::COLOR_THUMB_BG_END));
    p.fillRect(0, 0, edge, edge, grad);
    p.drawImage((edge - image.width()) / 2, (edge - image.height()) / 2, image);
    p.end();

    canvas.setDevicePixelRatio(dpr);
    return canvas;
}

void AssetThumbnailLoader::dispatch() {
    while (m_inFlight < m_maxInFlight && !m_pending.isEmpty()) {
        const Job job = m_pending.takeFirst();
        const int gen = m_generation->load();
        const qreal dpr = m_grid->devicePixelRatioF();
        std::shared_ptr<std::atomic<int>> generation = m_generation;
        const QString path = job.imagePath;

        QFuture<QImage> future = QtConcurrent::run([path, dpr, gen, generation]() -> QImage {
            if (generation->load() != gen) return {}; // the grid moved on; don't waste a decode
            return decodeThumbnail(path, dpr);
        });

        ++m_inFlight;
        auto* watcher = new QFutureWatcher<QImage>(this);
        QListWidgetItem* item = job.item;
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, item, gen]() {
            --m_inFlight;
            if (gen == m_generation->load()) {
                // Same generation, so the grid hasn't been cleared and `item` is still alive.
                const QImage canvas = watcher->result();
                if (!canvas.isNull()) item->setIcon(iconFromCanvas(canvas));
            }
            watcher->deleteLater();
            dispatch();
        });
        watcher->setFuture(future);
    }
}
