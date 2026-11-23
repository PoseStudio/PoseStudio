// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetthumbnailloader.h
 * @brief Off-thread thumbnail decoding for the asset grid, plus the shared thumbnail canvas.
 *
 * A folder can hold hundreds of assets, each with a preview image of arbitrary size. Decoding
 * them on the GUI thread, even 20 per event-loop tick as the widget once did, stalled the grid
 * and forced a full relayout per batch. This loader decodes on QtConcurrent's global pool
 * (QImageReader is reentrant, QImage is a value type), asking the codec to decode straight to
 * thumbnail size (JPEG can do 1/2..1/8 DCT scaling), paints the gradient canvas on the worker
 * as a QImage, and only turns the result into a QPixmap/QIcon on the GUI thread, where the
 * item's setIcon repaints just that cell.
 *
 * Jobs are keyed by grid ITEM POINTER plus a generation counter: clear() bumps the generation
 * (and must be called before the grid is cleared), so a result for an item that has since been
 * deleted is dropped without ever touching the pointer. Item pointers rather than row indices
 * because a drag-reorder while thumbnails are still loading shifts rows, but QListWidget's
 * takeItem/insertItem never invalidates the pointers. Workers read the generation through a
 * shared atomic so a stale job skips the decode entirely instead of wasting the pool.
 *
 * Every canvas is a THUMB-sized SQUARE rendered at the grid's device pixel ratio, with the
 * ratio set on the pixmap: the old 120x128 canvas was always drawn into a 120x120 icon rect,
 * so QIcon downscaled every thumbnail to 112x120 (a narrower gradient box, a second resample),
 * and without a device pixel ratio a 1.5x display showed 120 device pixels upscaled to 180.
 */

#ifndef ASSETTHUMBNAILLOADER_H
#define ASSETTHUMBNAILLOADER_H

#include <QIcon>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

class QListWidget;
class QListWidgetItem;

/**
 * @class AssetThumbnailLoader
 * @brief Decodes grid thumbnails on worker threads and applies them to their items.
 */
class AssetThumbnailLoader : public QObject {
    Q_OBJECT
public:
    explicit AssetThumbnailLoader(QListWidget* grid, QObject* parent = nullptr);
    ~AssetThumbnailLoader() override;

    /// Queues the decode of `imagePath` for `item` and starts it as soon as a worker slot is
    /// free. `item` must stay alive until clear() is called (i.e. until the grid is cleared).
    void enqueue(QListWidgetItem* item, const QString& imagePath);

    /// Drops every pending job and invalidates every in-flight one. MUST be called before the
    /// grid's items are deleted (AssetManagerWidget::clearGrid does), or results would land on
    /// dangling item pointers.
    void clear();

    /// The subfolder item icon (the folder glyph centred on a transparent canvas), rendered at
    /// the grid's current device pixel ratio and cached until that ratio changes. Rendering it
    /// once per displayFolder, as before, redid the same scaling for every folder shown.
    QIcon folderIcon();

    /// Paints `image` (already no larger than the canvas) centred over the thumbnail gradient.
    /// `dpr` is the device pixel ratio the canvas is rendered at (and stamped with). Pure QImage
    /// work, so it is safe on a worker thread.
    static QImage renderThumbnailCanvas(const QImage& image, qreal dpr);

private:
    struct Job {
        QListWidgetItem* item = nullptr;
        QString imagePath;
    };

    /// Starts pending jobs while fewer than m_maxInFlight are running.
    void dispatch();

    QListWidget* m_grid;
    /// Shared with the workers: a job compares the generation it was created under against
    /// this before decoding, and the GUI-side completion compares again before touching the item.
    std::shared_ptr<std::atomic<int>> m_generation;
    QList<Job> m_pending;
    int m_inFlight = 0;
    const int m_maxInFlight;

    QIcon m_folderIcon;
    qreal m_folderIconDpr = 0.0;
};

#endif // ASSETTHUMBNAILLOADER_H
