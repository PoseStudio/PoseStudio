/**
 * @file assetfolderproxymodel.h
 * @brief The identity proxy between the Asset Manager's QStandardItemModel tree and its view:
 *        folder "hit" classification (icons + greyed text) and collection drag-reparenting.
 *
 * The tree's source model is a plain QStandardItemModel of physical folders and virtual rows.
 * This proxy overlays two things on it without touching the source.
 *
 * (1) Hit state. Each folder row's icon and text color reflect whether the folder contains
 * assets DIRECTLY (AssetScan's non-image-plus-same-basename-image rule), only somewhere BELOW
 * it (IndirectHit), or nowhere (NoHit, greyed); a Collection counts as a hit when it holds at
 * least one item that still exists on disk. Classifying a folder means walking its subtree,
 * and data() runs for every visible row on every repaint, so nothing is ever classified inline:
 * an unknown row is queued, painted with a placeholder icon, classified on a QtConcurrent
 * worker (AssetScan::classifyFolder is a pure function of the filesystem; a collection check
 * stats its item paths from one aggregate query), and re-painted once its state lands. Results
 * are merged into a per-path cache on the GUI thread only; the cache and queue reset with the
 * model, and a generation counter discards results from before a reset. The search needs
 * states synchronously (it walks results as it finds them): folderHitState() classifies on
 * the calling thread into the same cache.
 *
 * (2) Drag-and-drop. Only Collection rows are draggable and only Collections/the Collections
 * header accept drops; a validated drop is reported through collectionReparentRequested, and
 * AssetManagerWidget performs the DB update and the tree-node move itself (the model stays
 * read-mostly).
 */

#ifndef ASSETFOLDERPROXYMODEL_H
#define ASSETFOLDERPROXYMODEL_H

#include "assetscan.h"

#include <QHash>
#include <QIcon>
#include <QIdentityProxyModel>
#include <QList>
#include <QPersistentModelIndex>
#include <QSet>
#include <QString>
#include <QStringList>

class QMimeData;

/**
 * @class AssetFolderProxyModel
 * @brief Intercepts data requests to overlay hit-state icons/colors on folder rows, owns the
 *        deferred hit-classification cache, and validates Collection drag-drop reparenting.
 */
class AssetFolderProxyModel : public QIdentityProxyModel {
    Q_OBJECT
public:
    /// How a folder relates to assets (AssetScan::HitState): only DirectHit folders are search
    /// results and open as grids with assets; IndirectHit folders are the path to them.
    using FolderHitState = AssetScan::HitState;

    explicit AssetFolderProxyModel(QAbstractItemModel* source, QObject* parent = nullptr);
    ~AssetFolderProxyModel() override;

    QVariant data(const QModelIndex &proxyIndex, int role = Qt::DisplayRole) const override;

    /// Forgets the cached hit state of the row at `proxyIndex` (a Collection whose items
    /// changed, or the Search Results root whose result count changed) and repaints it; the
    /// state is re-established lazily on the next paint. The caller locates the row (the widget
    /// knows where its collections and section roots live), so no tree walk happens here.
    void refreshNode(const QModelIndex& proxyIndex);

    /// SYNCHRONOUS classification for the search: walks the subtree on the calling thread and
    /// caches every folder it visits. The painting path never calls this.
    FolderHitState folderHitState(const QString& folderPath) const;
    bool hasHit(const QString& folderPath)      const { return folderHitState(folderPath) != FolderHitState::NoHit; }
    bool isDirectHit(const QString& folderPath) const { return folderHitState(folderPath) == FolderHitState::DirectHit; }

    // --- Drag-and-drop: lets the user drag a Collection onto another Collection (or onto the
    // Collections root) to reparent it. Validation/gesture handling lives here; the actual DB
    // update + tree-node move is performed by AssetManagerWidget via the signal below, keeping
    // this model read-mostly like the rest of its responsibilities.
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action,
                          int row, int column, const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action,
                       int row, int column, const QModelIndex &parent) override;

signals:
    /// Emitted once a drag-drop reparent passes validation; newParentId is 0 for the Collections root.
    void collectionReparentRequested(int collectionId, int newParentId);

private:
    struct HitJob {
        QPersistentModelIndex index; ///< The row to repaint when the state lands (may die meanwhile)
        QString nodeId;              ///< Folder path or "COLLECTION_<id>"
    };

    /// Queues a classification for a row whose state is unknown (called from data(), hence const).
    void enqueueHitCheck(const QModelIndex& proxyIndex, const QString& nodeId) const;
    /// Starts queued jobs on the worker pool while fewer than m_maxInFlight are running.
    void dispatchHitChecks() const;
    /// Loads the collection -> item paths aggregate once per invalidation.
    void ensureCollectionItemsLoaded() const;

    mutable QHash<QString, FolderHitState> m_hitCache;
    mutable QList<HitJob> m_pendingHitChecks;
    mutable QSet<QString> m_pendingHitIds;   ///< Queued OR in flight: blocks duplicate jobs for one row
    mutable int m_hitChecksInFlight = 0;
    const int m_maxHitChecksInFlight;
    /// Bumped on every model reset: a worker result created under an older generation is
    /// discarded (its row and cache are gone).
    mutable int m_generation = 0;

    mutable QHash<int, QStringList> m_collectionItems; ///< collection id -> item paths (one aggregate query)
    mutable bool m_collectionItemsLoaded = false;

    // data() runs for every visible row on every repaint (hover, scroll, expand), so the icons
    // are constructed once per model and reused. They are members, not function statics: a
    // static QIcon outlives QApplication and is destroyed after the platform integration is
    // gone, which is undefined territory for pixmap data.
    const QIcon m_searchIcon;
    const QIcon m_searchDisabledIcon;
    const QIcon m_collectionsIcon;
    const QIcon m_favoritesIcon;
    const QIcon m_subCollectionIcon;
    const QIcon m_folderFullIcon;
    const QIcon m_folderHitIcon;
    const QIcon m_folderEmptyIcon;
};

#endif // ASSETFOLDERPROXYMODEL_H
