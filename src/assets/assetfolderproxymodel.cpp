/**
 * @file assetfolderproxymodel.cpp
 * @brief Implementation of AssetFolderProxyModel. See the header for the design.
 */

#include "assetfolderproxymodel.h"
#include "assetdb.h"
#include "assetnodeids.h"

#include <QColor>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeData>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

namespace {
/// The MIME type a dragged Collection row carries; its payload is the row's "COLLECTION_<id>"
/// node id. Private to this model: the tree view only forwards the QMimeData back to it.
const QString kCollectionMimeType = QStringLiteral("application/x-posestudio-collection");

/// What a hit-check worker returns: the states it established, keyed by node id (a folder walk
/// yields every folder it visited; a collection check yields just that collection).
using HitStates = QHash<QString, AssetScan::HitState>;

const QColor kNoHitTextColor(110, 110, 110);
}

AssetFolderProxyModel::AssetFolderProxyModel(QAbstractItemModel* source, QObject* parent)
    : QIdentityProxyModel(parent)
    // Two workers are plenty for the handful of visible rows a tree shows; more would only
    // compete with thumbnail decodes and the IBL bake for the same global pool.
    , m_maxHitChecksInFlight(qBound(1, QThread::idealThreadCount() / 2, 2))
    , m_searchIcon(QStringLiteral(":/resources/icons/search.png"))
    , m_searchDisabledIcon(QStringLiteral(":/resources/icons/search-d.png"))
    , m_collectionsIcon(QStringLiteral(":/resources/icons/collections.png"))
    , m_favoritesIcon(QStringLiteral(":/resources/icons/favorite.png"))
    , m_subCollectionIcon(QStringLiteral(":/resources/icons/sub-collection.png"))
    , m_folderFullIcon(QStringLiteral(":/resources/icons/folder-full.png"))
    , m_folderHitIcon(QStringLiteral(":/resources/icons/folder-hit.png"))
    , m_folderEmptyIcon(QStringLiteral(":/resources/icons/folder-empty.png")) {
    setSourceModel(source);
    connect(source, &QAbstractItemModel::modelReset, this, [this]() {
        m_hitCache.clear();
        // The pending queue must reset with the model: a reset invalidates every queued
        // QPersistentModelIndex, and a queued id is only released when its job completes. A
        // survivor id would block that folder from ever being re-enqueued, freezing it on the
        // placeholder "hit" icon. In-flight jobs finish on their own and are discarded by the
        // generation check.
        m_pendingHitChecks.clear();
        m_pendingHitIds.clear();
        m_collectionItemsLoaded = false;
        ++m_generation;
    });
}

AssetFolderProxyModel::~AssetFolderProxyModel() {
    // Watchers are children and die with us; their workers finish on the pool and their
    // results are simply never read.
    ++m_generation;
}

void AssetFolderProxyModel::refreshNode(const QModelIndex& proxyIndex) {
    if (!proxyIndex.isValid()) return;
    const QString nodeId = sourceModel()->data(mapToSource(proxyIndex), Qt::UserRole).toString();
    m_hitCache.remove(nodeId);
    // A collection's items changed: the aggregate is stale for it, so reload on next need.
    if (AssetNode::isCollection(nodeId)) m_collectionItemsLoaded = false;
    emit dataChanged(proxyIndex, proxyIndex, {Qt::ForegroundRole, Qt::DecorationRole});
}

// =============================================================================
// [ HIT CLASSIFICATION ]
// =============================================================================
// The rule (AssetScan): a folder has a DIRECT hit when it contains an asset, an asset being any
// non-image file (minus sidecar extensions) that shares its basename with an image file in the
// same folder. A folder with no direct hit but a DirectHit somewhere in its subtree is an
// INDIRECT hit; otherwise NoHit. A Collection is a DirectHit when at least one of its items
// still exists on disk. The cache is per node id and only ever touched on the GUI thread; the
// workers get an implicitly-shared snapshot of it (read-only, so no detach on their side) and
// hand back the states they established.

AssetFolderProxyModel::FolderHitState AssetFolderProxyModel::folderHitState(const QString& folderPath) const {
    if (const auto it = m_hitCache.constFind(folderPath); it != m_hitCache.cend()) return it.value();
    const HitStates states = AssetScan::classifyFolder(folderPath, m_hitCache);
    for (auto it = states.cbegin(); it != states.cend(); ++it) m_hitCache.insert(it.key(), it.value());
    return m_hitCache.value(folderPath, FolderHitState::NoHit);
}

void AssetFolderProxyModel::ensureCollectionItemsLoaded() const {
    if (m_collectionItemsLoaded) return;
    m_collectionItems = AssetDb::collectionItemPathsByCollection();
    m_collectionItemsLoaded = true;
}

void AssetFolderProxyModel::enqueueHitCheck(const QModelIndex& proxyIndex, const QString& nodeId) const {
    if (m_pendingHitIds.contains(nodeId)) return; // already queued or running
    m_pendingHitIds.insert(nodeId);
    m_pendingHitChecks.append({QPersistentModelIndex(proxyIndex), nodeId});
    dispatchHitChecks();
}

void AssetFolderProxyModel::dispatchHitChecks() const {
    auto* self = const_cast<AssetFolderProxyModel*>(this);
    while (m_hitChecksInFlight < m_maxHitChecksInFlight && !m_pendingHitChecks.isEmpty()) {
        const HitJob job = m_pendingHitChecks.takeFirst();
        if (!job.index.isValid()) { // the row vanished (search results replaced) before its turn
            m_pendingHitIds.remove(job.nodeId);
            continue;
        }

        const int gen = m_generation;
        QFuture<HitStates> future;
        if (AssetNode::isCollection(job.nodeId)) {
            ensureCollectionItemsLoaded();
            const QString id = job.nodeId;
            const QStringList paths = m_collectionItems.value(AssetNode::collectionId(id));
            future = QtConcurrent::run([id, paths]() -> HitStates {
                // A collection counts as a "hit" when it holds at least one existing asset item.
                bool found = false;
                for (const QString& p : paths) {
                    if (QFileInfo::exists(p)) { found = true; break; }
                }
                return {{id, found ? FolderHitState::DirectHit : FolderHitState::NoHit}};
            });
        } else {
            const QString path = job.nodeId;
            const HitStates known = m_hitCache; // implicitly shared snapshot; the worker only reads it
            future = QtConcurrent::run([path, known]() -> HitStates {
                return AssetScan::classifyFolder(path, known);
            });
        }

        ++m_hitChecksInFlight;
        auto* watcher = new QFutureWatcher<HitStates>(self);
        connect(watcher, &QFutureWatcher<HitStates>::finished, self, [self, watcher, job, gen]() {
            --self->m_hitChecksInFlight;
            if (gen == self->m_generation) {
                const HitStates states = watcher->result();
                for (auto it = states.cbegin(); it != states.cend(); ++it)
                    self->m_hitCache.insert(it.key(), it.value());
                self->m_pendingHitIds.remove(job.nodeId);
                if (job.index.isValid()) {
                    const QModelIndex idx(job.index);
                    emit self->dataChanged(idx, idx, {Qt::ForegroundRole, Qt::DecorationRole});
                }
            }
            watcher->deleteLater();
            self->dispatchHitChecks();
        });
        watcher->setFuture(future);
    }
}

/**
 * @brief Intercepts data requests from the view to inject dynamic icons and text colors.
 */
QVariant AssetFolderProxyModel::data(const QModelIndex &proxyIndex, int role) const {
    if (proxyIndex.column() != 0) return QIdentityProxyModel::data(proxyIndex, role);

    const QModelIndex sourceIndex = mapToSource(proxyIndex);
    const QString path = sourceModel()->data(sourceIndex, Qt::UserRole).toString();

    // ---------------------------------------------------------
    // 1. Section-root overrides (Search Results, Collections, Favorites)
    // ---------------------------------------------------------
    if (path == AssetNode::SEARCH_ROOT) {
        if (role == Qt::DecorationRole || role == Qt::ForegroundRole) {
            // Real results carry their folder path; the "(No results)" / "Searching..."
            // placeholders carry none, so the first child's id says whether there are results.
            const QModelIndex firstChild = sourceModel()->index(0, 0, sourceIndex);
            const bool hasResults = firstChild.isValid()
                && !sourceModel()->data(firstChild, Qt::UserRole).toString().isEmpty();
            if (role == Qt::DecorationRole)
                return hasResults ? m_searchIcon : m_searchDisabledIcon;
            return hasResults ? QVariant() : kNoHitTextColor;
        }
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == AssetNode::COLLECTIONS_ROOT) {
        if (role == Qt::DecorationRole) return m_collectionsIcon;
        return QIdentityProxyModel::data(proxyIndex, role);
    }
    if (path == AssetNode::FAVORITES_ROOT) {
        if (role == Qt::DecorationRole) return m_favoritesIcon;
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    // ---------------------------------------------------------
    // 2. Ignore structural/dummy nodes
    // ---------------------------------------------------------
    if (AssetNode::isStructural(path)) {
        return QIdentityProxyModel::data(proxyIndex, role);
    }

    // ---------------------------------------------------------
    // 3. Folders and Collections: icon + color from the (deferred) hit state
    // ---------------------------------------------------------
    if (role == Qt::DecorationRole || role == Qt::ForegroundRole) {
        const bool isCollection = AssetNode::isCollection(path);
        const auto cached = m_hitCache.constFind(path);
        if (cached == m_hitCache.cend()) {
            // Unknown yet: queue the classification and paint the placeholder (a collection's
            // own icon, or the "hit" folder) in the meantime, un-greyed.
            enqueueHitCheck(proxyIndex, path);
            if (role == Qt::ForegroundRole) return QVariant();
            return isCollection ? m_subCollectionIcon : m_folderHitIcon;
        }
        const FolderHitState state = cached.value();

        if (role == Qt::ForegroundRole) {
            return (state == FolderHitState::NoHit) ? QVariant(kNoHitTextColor) : QVariant();
        }

        if (isCollection) return m_subCollectionIcon;

        if (state == FolderHitState::DirectHit) return m_folderFullIcon;
        if (state == FolderHitState::IndirectHit) return m_folderHitIcon;
        return m_folderEmptyIcon;
    }

    return QIdentityProxyModel::data(proxyIndex, role);
}

// =============================================================================
// [ COLLECTION DRAG-AND-DROP (REPARENTING) ]
// =============================================================================

/**
 * @brief Only Collection rows are draggable; only Collection rows and the Collections root
 *        accept drops. Every other row (physical folders/roots, Search Results, Favorites,
 *        separators) gets neither, overriding QStandardItem's drag/drop-enabled-by-default flags.
 */
Qt::ItemFlags AssetFolderProxyModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QIdentityProxyModel::flags(index) & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    if (!index.isValid() || index.column() != 0) return f;

    const QString path = sourceModel()->data(mapToSource(index), Qt::UserRole).toString();
    if (AssetNode::isCollection(path)) {
        f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    } else if (path == AssetNode::COLLECTIONS_ROOT) {
        f |= Qt::ItemIsDropEnabled;
    }
    return f;
}

QStringList AssetFolderProxyModel::mimeTypes() const {
    return { kCollectionMimeType };
}

QMimeData* AssetFolderProxyModel::mimeData(const QModelIndexList &indexes) const {
    if (indexes.isEmpty()) return nullptr;
    const QString path = sourceModel()->data(mapToSource(indexes.first()), Qt::UserRole).toString();
    if (!AssetNode::isCollection(path)) return nullptr;

    QMimeData *mime = new QMimeData();
    mime->setData(kCollectionMimeType, path.toUtf8());
    return mime;
}

bool AssetFolderProxyModel::canDropMimeData(const QMimeData *data, Qt::DropAction action,
                                             int row, int column, const QModelIndex &parent) const {
    Q_UNUSED(row);
    Q_UNUSED(column);
    if (action != Qt::MoveAction || !data->hasFormat(kCollectionMimeType)) return false;

    const QString draggedPath = QString::fromUtf8(data->data(kCollectionMimeType));
    // The tree's true invisible root (parent invalid) is never a valid target; only the
    // Collections root or another Collection are.
    if (!parent.isValid()) return false;
    const QString targetPath = sourceModel()->data(mapToSource(parent), Qt::UserRole).toString();
    if (targetPath != AssetNode::COLLECTIONS_ROOT && !AssetNode::isCollection(targetPath))
        return false;

    if (targetPath == draggedPath) return false; // dropping onto itself is a no-op

    // Reject dropping into one of its own descendants: that would create a cycle.
    for (QModelIndex walk = parent; walk.isValid(); walk = walk.parent()) {
        if (sourceModel()->data(mapToSource(walk), Qt::UserRole).toString() == draggedPath) return false;
    }
    return true;
}

bool AssetFolderProxyModel::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                          int row, int column, const QModelIndex &parent) {
    if (!canDropMimeData(data, action, row, column, parent)) return false;

    const QString draggedPath = QString::fromUtf8(data->data(kCollectionMimeType));
    const int draggedId = AssetNode::collectionId(draggedPath);

    const QString targetPath = sourceModel()->data(mapToSource(parent), Qt::UserRole).toString();
    const int newParentId = (targetPath == AssetNode::COLLECTIONS_ROOT)
        ? 0 : AssetNode::collectionId(targetPath);

    emit collectionReparentRequested(draggedId, newParentId);

    // Return false so Qt's own InternalMove machinery does NOT also remove the "source" row:
    // we move the tree item ourselves in reparentCollection (wired as a queued connection so it
    // runs after this drop event fully unwinds). Returning true here would make the view delete
    // the row we just relocated, so the collection would vanish until the next refresh/restart.
    return false;
}
