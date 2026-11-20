/**
 * @file assetmanagerwidget.cpp
 * @brief AssetManagerWidget, the first of its four translation units: setup, the grid (what a
 *        source shows, the tooltip, opening an asset, script links, the manual order) and the
 *        collection / favorites operations.
 *
 * The other three: assetmanagerwidget_tree.cpp (the tree: build / refresh, lazy loading,
 * search, navigation), assetmanagerwidget_menus.cpp (the context menus) and
 * assetmanagerwidget_files.cpp (an asset's file operations and packages). One class, split for
 * size; each has full access to its state.
 *
 * The widget is the glue between the tree model, the grid, and the data layer. Two invariants
 * everything here respects: the grid's items are only ever deleted through clearGrid(), which
 * first detaches everything that holds item pointers (the thumbnail loader's jobs, the
 * tooltip's owner item); and every change to a collection's contents goes through a helper
 * that also repaints that collection's tree row (its hit icon is cached by the proxy model).
 *
 * The class is instantiated once per library tab (the Asset Manager, the Materials Library:
 * assetmanagerscope.h). Where an instance's scope matters it is asked for by name — m_scope —
 * and ONLY in these functions:
 *   setupUI                  the search field's placeholder
 *   currentLibraryRoots      where the roots come from                                  (tree)
 *   refreshAssetManager      no "Add Asset Folder" hint over roots of its own; a browser of
 *                            one such root opens it                                     (tree)
 *   showEvent                roots of its own resolved again; the one root shown in the grid
 *                            the first time the tab is seen                             (tree)
 *   loadCollectionsTree      whose collections are listed                               (tree)
 *   favoritePathsInScope     which favorites are shown
 *   newCollectionName, createCollection
 *                            whose collection is named and created
 *   addManageRefreshActions  no "Manage Asset Folders" entry over roots of its own      (menus)
 * Nothing else may differ between the tabs; keep it that way, and keep this list exact.
 */

#include "assetmanagerwidget.h"
#include "importformats.h"
#include "shaderpresetfile.h" // isShaderPresetFile: a `.duf` that is a material, not a figure
#include "assetbreadcrumblabel.h"
#include "assetdb.h"
#include "assetfolderproxymodel.h"
#include "assetgriddelegate.h"
#include "assetgriddragcontroller.h"
#include "assetlibraryprompt.h"
#include "assetnodeids.h"
#include "assetscan.h"
#include "assetscriptlink.h"
#include "assetthumbnailloader.h"
#include "assettreedelegate.h"
#include "assettreeview.h"
#include "constants.h"
#include "customtooltip.h"
#include "preferencesmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

using AssetScan::AssetHit;

namespace {
// The name a tree row is shown under in the grid title. Search-result rows display their
// truncated relative path ("a / b / matched"); only for those is the last segment the name.
// A collection the user named "Sci-Fi / Props" keeps its full text.
QString rowDisplayName(const QModelIndex& index) {
    QString name = index.data(Qt::DisplayRole).toString();
    if (index.data(AssetNode::SearchResult).toBool())
        name = name.section(QStringLiteral(" / "), -1);
    return name;
}
}

// =============================================================================
// [ SETUP ]
// =============================================================================

AssetManagerWidget::AssetManagerWidget(const AssetManagerScope& scope, QWidget *parent)
    : QWidget(parent), m_scope(scope) {
    // App-level theming (Fusion + AppProxyStyle) and global tooltip effects are configured
    // once in main.cpp, before any widget is built, not here.
    setupUI();
}

AssetManagerWidget::~AssetManagerWidget() = default;

const QIcon& AssetManagerWidget::icon(const QString& name) {
    auto it = m_icons.find(name);
    if (it == m_icons.end())
        it = m_icons.insert(name, QIcon(QStringLiteral(":/resources/icons/%1.png").arg(name)));
    return it.value();
}

/**
 * @brief Initializes the main layout, QSplitters, and primary views.
 */
void AssetManagerWidget::setupUI() {
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setHandleWidth(6);

    // --- 1. Top Panel: Search bar + Directory & Collections Tree ---
    dirModel = new QStandardItemModel(this);
    proxyModel = new AssetFolderProxyModel(dirModel, this);

    QWidget *topPanel = new QWidget(splitter);
    topPanel->setObjectName("AssetManagerTopPanel");
    QVBoxLayout *topLayout = new QVBoxLayout(topPanel);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    QWidget *searchRow = new QWidget(topPanel);
    searchRow->setObjectName("AssetManagerSearchRow");
    QHBoxLayout *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(6, 5, 6, 5);
    searchLayout->setSpacing(4);

    searchInput = new QLineEdit(searchRow);
    searchInput->setObjectName("AssetManagerSearchInput");
    searchInput->setPlaceholderText(m_scope.searchPlaceholder.isEmpty() ? tr("Search assets...")
                                                                        : m_scope.searchPlaceholder);
    // For the scripted `ui` hook (tools/ikscripts): every library tab's search field carries the
    // same id, told apart by their order — 0 the Asset Manager, 1 the Materials Library.
    searchInput->setProperty("testId", QStringLiteral("library.search"));

    clearSearchButton = new QPushButton(searchRow);
    clearSearchButton->setObjectName("AssetManagerClearButton");
    clearSearchButton->setIcon(icon(QStringLiteral("clear")));
    clearSearchButton->setIconSize(QSize(14, 14));
    clearSearchButton->setFixedSize(26, 26);

    searchButton = new QPushButton(searchRow);
    searchButton->setObjectName("AssetManagerSearchButton");
    searchButton->setIcon(icon(QStringLiteral("search")));
    searchButton->setIconSize(QSize(14, 14));
    searchButton->setFixedSize(26, 26);

    searchLayout->addWidget(searchInput);
    searchLayout->addWidget(clearSearchButton);
    searchLayout->addWidget(searchButton);

    QFrame *searchSeparator = new QFrame(topPanel);
    searchSeparator->setObjectName("AssetManagerSearchSeparator");
    searchSeparator->setFrameShape(QFrame::HLine);
    searchSeparator->setFrameShadow(QFrame::Plain);

    dirTreeView = new AssetTreeView(topPanel);
    dirTreeView->setObjectName("AssetManagerTree");
    dirTreeView->setModel(proxyModel);
    dirTreeView->setItemDelegate(new AssetTreeDelegate(this));
    dirTreeView->setHeaderHidden(true);

    dirTreeView->setEditTriggers(QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    dirTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    // Collection drag-and-drop reparenting (see AssetFolderProxyModel's flags/mimeData/
    // canDropMimeData/dropMimeData overrides for what's actually draggable/droppable).
    dirTreeView->setDragEnabled(true);
    dirTreeView->setAcceptDrops(true);
    dirTreeView->setDropIndicatorShown(false); // we paint our own drop highlight (AssetTreeView)
    dirTreeView->setDragDropMode(QAbstractItemView::InternalMove);
    dirTreeView->setDefaultDropAction(Qt::MoveAction);

    connect(dirTreeView, &QTreeView::customContextMenuRequested, this, &AssetManagerWidget::onContextMenuRequested);
    connect(dirModel, &QStandardItemModel::itemChanged, this, &AssetManagerWidget::onItemChanged);
    // Queued so the reparent (which mutates the model via takeRow/appendRow) runs after Qt's
    // drop event has fully unwound, rather than re-entering the model mid-drop.
    connect(proxyModel, &AssetFolderProxyModel::collectionReparentRequested,
            this, &AssetManagerWidget::reparentCollection, Qt::QueuedConnection);

    topLayout->addWidget(searchRow);
    topLayout->addWidget(searchSeparator);
    topLayout->addWidget(dirTreeView);

    // =====================================================================
    // EMPTY-STATE HINT: shown over the directory tree only while no asset library is configured
    // =====================================================================
    addLibraryHintLabel = new QLabel(dirTreeView->viewport());
    addLibraryHintLabel->setObjectName("AssetManagerAddLibraryHint");
    addLibraryHintLabel->setTextFormat(Qt::RichText);
    // Qt's QSS engine doesn't apply selectors to rich-text <a> links inside a QLabel, so the
    // link color is inline in the HTML. (The markup is no text to translate; the label is.)
    addLibraryHintLabel->setText(QStringLiteral("<a href=\"#\" style=\"color: #ffffff;\">%1</a>")
                                     .arg(tr("Add Asset Folder")));
    addLibraryHintLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    addLibraryHintLabel->setCursor(Qt::PointingHandCursor);
    addLibraryHintLabel->hide();
    connect(addLibraryHintLabel, &QLabel::linkActivated, this, [this](const QString&) {
        promptAddAssetLibrary();
    });

    // A layout on the viewport centers the hint and keeps it centered across resizes,
    // without interfering with the tree's own item painting (not child widgets).
    QVBoxLayout *hintLayout = new QVBoxLayout(dirTreeView->viewport());
    hintLayout->addWidget(addLibraryHintLabel, 0, Qt::AlignCenter);
    // =====================================================================

    // --- 2. Bottom Panel: Asset Thumbnail Grid ---
    QWidget *bottomPanel = new QWidget(splitter);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);

    titleLabel = new AssetBreadcrumbLabel(bottomPanel);
    connect(titleLabel, &AssetBreadcrumbLabel::navigateRequested, this, [this](const QString& path) {
        deselectTree();
        displayFolder(path);
    });
    connect(titleLabel, &AssetBreadcrumbLabel::browseRequested, this, [](const QString& path) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    assetListWidget = new QListWidget(bottomPanel);
    assetListWidget->setObjectName("AssetManagerGrid");
    assetListWidget->setViewMode(QListView::IconMode);
    assetListWidget->setIconSize(QSize(Constants::GRID_ICON_DISPLAY_SIZE, Constants::GRID_ICON_DISPLAY_SIZE));

    assetListWidget->setResizeMode(QListView::Adjust);
    assetListWidget->setMovement(QListView::Static);
    assetListWidget->setSpacing(6);
    assetListWidget->setItemDelegate(new AssetGridDelegate(assetListWidget));

    m_thumbnails = new AssetThumbnailLoader(assetListWidget, this);

    // =====================================================================
    // INITIALIZE INTERACTIVE TOOLTIPS
    // =====================================================================
    customToolTip = new CustomToolTip(this);
    activeToolTipItem = nullptr;

    // Force the grid to track mouse movements even when not clicking
    assetListWidget->setMouseTracking(true);
    // Install the filter so we can intercept the ToolTip spawn requests (and feed the drag)
    assetListWidget->viewport()->installEventFilter(this);
    // =====================================================================

    // The hand-rolled asset drag: reorder within a sortable view, or drop onto a tree node.
    m_gridDrag = new AssetGridDragController(assetListWidget, dirTreeView,
                                             [this]() { return isSortableView(); }, this);
    connect(m_gridDrag, &AssetGridDragController::dragStarted, this, [this]() { customToolTip->hide(); });
    connect(m_gridDrag, &AssetGridDragController::reorderCommitted, this, &AssetManagerWidget::persistGridOrder);
    connect(m_gridDrag, &AssetGridDragController::droppedOnTree, this, &AssetManagerWidget::dropAssetOnTreeNode);

    assetListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(assetListWidget, &QListWidget::customContextMenuRequested, this, &AssetManagerWidget::onGridContextMenuRequested);
    connect(assetListWidget, &QListWidget::itemDoubleClicked, this, &AssetManagerWidget::onGridItemDoubleClicked);

    infoBarLabel = new QLabel(bottomPanel);
    infoBarLabel->setObjectName("AssetManagerInfoBar");
    infoBarLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    infoBarLabel->hide(); // nothing displayed yet at startup

    bottomLayout->addWidget(titleLabel);
    bottomLayout->addWidget(assetListWidget);
    bottomLayout->addWidget(infoBarLabel);

    splitter->addWidget(topPanel);
    splitter->addWidget(bottomPanel);
    splitter->setSizes({300, 700});

    mainLayout->addWidget(splitter);

    connect(dirTreeView, &QTreeView::clicked, this, &AssetManagerWidget::onFolderSelected);
    // Keyboard navigation (arrow keys) moves the current row without a click; follow it too.
    connect(dirTreeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &AssetManagerWidget::onTreeCurrentChanged);
    connect(dirTreeView, &QTreeView::expanded, this, &AssetManagerWidget::onTreeExpanded);

    connect(searchButton, &QPushButton::clicked, this, [this]() {
        runSearch(searchInput->text().trimmed());
    });
    connect(searchInput, &QLineEdit::returnPressed, searchButton, &QPushButton::click);
    connect(clearSearchButton, &QPushButton::clicked, this, [this]() {
        searchInput->clear();
        runSearch(QString());
    });

    refreshAssetManager();
}

/**
 * @brief Prompts the user for a folder via the native file dialog and registers it as a
 *        new Asset Library, then refreshes the tree so it appears immediately.
 */
void AssetManagerWidget::promptAddAssetLibrary() {
    // (The dialog flow is shared with Preferences -> Assets: assetlibraryprompt.h.)
    if (AssetLibraryPrompt::addLibrary(this)) {
        refreshAssetManager();
    }
}

// =============================================================================
// [ GRID DISPLAY ]
// =============================================================================

void AssetManagerWidget::displayTreeIndex(const QModelIndex& proxyIndex) {
    const QString folderPath = proxyIndex.data(Qt::UserRole).toString();
    if (AssetNode::isStructural(folderPath)
        || folderPath == AssetNode::COLLECTIONS_ROOT || folderPath == AssetNode::SEARCH_ROOT) return;
    displayFolder(folderPath, rowDisplayName(proxyIndex));
}

/**
 * @brief Rebuilds the footer info bar ("Assets: X   Folders: X   Sortable"), omitting any
 *        segment that isn't relevant to the currently displayed source.
 */
void AssetManagerWidget::refreshInfoBar() {
    if (m_currentFolderPath.isEmpty()) {
        infoBarLabel->hide();
        return;
    }

    QStringList countSegments;
    if (m_currentAssetCount > 0)
        countSegments << tr("Assets: %1").arg(m_currentAssetCount);
    if (m_currentFolderCount > 0)
        countSegments << tr("Folders: %1").arg(m_currentFolderCount);

    const bool isSortable = isSortableView();

    if (countSegments.isEmpty() && !isSortable) {
        infoBarLabel->hide();
        return;
    }

    // An extra space sets "Sortable" apart from the Assets/Folders counts a bit more.
    QString text = countSegments.join(QStringLiteral("   "));
    if (isSortable) {
        if (!text.isEmpty()) text += QStringLiteral("    ");
        text += tr("Sortable");
    }

    infoBarLabel->setText(text);
    infoBarLabel->show();
}

void AssetManagerWidget::clearGrid() {
    m_thumbnails->clear();       // queued/in-flight decodes point at the items about to die
    activeToolTipItem = nullptr; // ditto for the tooltip's item
    m_gridDrag->cancel();        // a drag can't outlive its item either
    assetListWidget->clear();
}

void AssetManagerWidget::resetGridView() {
    clearGrid();
    titleLabel->clearTitle();
    m_currentFolderPath.clear();
    m_currentAssetCount = 0;
    m_currentFolderCount = 0;
    infoBarLabel->hide();
}

void AssetManagerWidget::refreshCurrentView() {
    if (m_currentFolderPath.isEmpty()) refreshAssetManager();
    else displayFolder(m_currentFolderPath);
}

/**
 * @brief Parses and displays assets for a given source. Called by the tree selection, by
 *        breadcrumb link clicks in the title label, and by grid folder items.
 */
void AssetManagerWidget::displayFolder(const QString& folderPath, const QString& title) {
    // "Virtual" sources (Collections, Favorites) hold a DB-backed list of asset items rather than
    // a physical directory: they get a plain title (not a breadcrumb) and no subfolder listing.
    const bool isFavorites = (folderPath == AssetNode::FAVORITES_ROOT);
    const bool isCollection = AssetNode::isCollection(folderPath);
    const bool isVirtual = isFavorites || isCollection;

    QList<AssetHit> discoveredAssets;
    if (isFavorites) {
        discoveredAssets = AssetScan::resolveAssetHits(favoritePathsInScope());
        m_favoritesRevisionShown = AssetDb::favoritesRevision();
    } else if (isCollection) {
        discoveredAssets = AssetScan::resolveAssetHits(AssetDb::collectionItemPaths(AssetNode::collectionId(folderPath)));
    } else {
        discoveredAssets = AssetScan::scanFolderAssets(folderPath);
    }

    m_currentFolderPath = folderPath;
    m_currentAssetCount = discoveredAssets.size();

    if (isVirtual) {
        QString titleText = title;
        if (titleText.isEmpty()) {
            // No name handed in (a refresh, a drop-move): read it off the tree row.
            if (isFavorites) titleText = tr("Favorites");
            else if (QStandardItem* item = findCollectionTreeItem(AssetNode::collectionId(folderPath)))
                titleText = item->text();
            else titleText = tr("Collection");
        }
        titleLabel->setPlainTitle(titleText);
    } else {
        titleLabel->setFolder(folderPath, m_libraryPaths);
    }

    // Favorites and Collections keep the user's manual drag order (the DB query returned it);
    // everything else is shown alphabetically.
    if (!isVirtual) {
        std::sort(discoveredAssets.begin(), discoveredAssets.end(), [](const AssetHit& a, const AssetHit& b) {
            return a.assetFileName.compare(b.assetFileName, Qt::CaseInsensitive) < 0;
        });
    }

    clearGrid();
    assetListWidget->setUpdatesEnabled(false);

    // --- Subfolder items (physical paths only) ---
    int folderCount = 0;
    if (!isVirtual) {
        const QFileInfoList subdirs = AssetScan::listSubdirectories(folderPath);
        if (!subdirs.isEmpty()) {
            const QIcon folderIcon = m_thumbnails->folderIcon();
            for (const QFileInfo& di : subdirs) {
                QListWidgetItem *folderItem = new QListWidgetItem();
                folderItem->setText(di.fileName());
                folderItem->setIcon(folderIcon);
                folderItem->setData(AssetGridRole::FilePath, di.absoluteFilePath());
                folderItem->setData(AssetGridRole::ItemKind, kFolderItemKind);
                assetListWidget->addItem(folderItem);
                ++folderCount;
            }
        }
    }

    m_currentFolderCount = folderCount;
    refreshInfoBar();

    for (const AssetHit& hit : std::as_const(discoveredAssets)) {
        // (The whole name without its extension: "Material.001" and "Material.002" are two names.)
        const QString cleanName = QFileInfo(hit.assetFileName).completeBaseName();
        const QString fullPath = QDir(hit.folderPath).filePath(hit.assetFileName);

        QListWidgetItem *item = new QListWidgetItem();
        item->setText(cleanName);
        item->setData(AssetGridRole::FilePath, fullPath);
        // The tooltip is built on demand from these (assetTooltipHtml), not stored per item.
        item->setData(AssetGridRole::FileSize, hit.sizeBytes);
        item->setData(AssetGridRole::LastModified, hit.lastModified);

        assetListWidget->addItem(item);

        if (!hit.bestImage.isEmpty())
            m_thumbnails->enqueue(item, QDir(hit.folderPath).filePath(hit.bestImage));
    }

    assetListWidget->setUpdatesEnabled(true);
    // Lay the new items out now: navigateToCollectionAssetItem scrolls to one of them right
    // after this returns, which needs their geometry.
    assetListWidget->doItemsLayout();
}

QString AssetManagerWidget::assetTooltipHtml(const QListWidgetItem* item) {
    if (!item || item->data(AssetGridRole::ItemKind).toString() == kFolderItemKind) return {};
    const QString fullPath = item->data(AssetGridRole::FilePath).toString();
    if (fullPath.isEmpty()) return {};

    const QFileInfo info(fullPath); // string ops only: no stat below
    const QString ext = info.suffix().toUpper();
    const double sz = static_cast<double>(item->data(AssetGridRole::FileSize).toLongLong());
    const QDateTime modified = item->data(AssetGridRole::LastModified).toDateTime();
    // (Every piece of text is one message of its own; the date is written the system's way.)
    const QString size = sz > (1024 * 1024) ? tr("%1 MB").arg(QString::number(sz / (1024.0 * 1024.0), 'f', 2))
                                            : tr("%1 KB").arg(QString::number(sz / 1024.0, 'f', 2));
    return QString(
        "<div style='white-space: pre-wrap;'>"
        "<span style='font-weight: bold; color: %1;'>%2</span><br/>"
        "%3<br/>"
        "%4<br/><br/>"
        "<span style='color: %5; font-size: 11px;'>%6</span>"
        "</div>"
    ).arg(Constants::COLOR_ACCENT,
          ext.isEmpty() ? tr("File") : tr(".%1 File").arg(ext),
          tr("Size: %1").arg(size),
          tr("Modified: %1").arg(QLocale().toString(modified, QLocale::ShortFormat)),
          Constants::COLOR_TOOLTIP_MUTED,
          QDir::toNativeSeparators(info.absolutePath()).toHtmlEscaped());
}

/**
 * @brief Handles double-clicking an item in the asset grid.
 */
void AssetManagerWidget::onGridItemDoubleClicked(QListWidgetItem *item) {
    if (!item) return;

    const QString path = item->data(AssetGridRole::FilePath).toString();
    if (path.isEmpty()) return;

    if (item->data(AssetGridRole::ItemKind).toString() == kFolderItemKind) {
        deselectTree();
        displayFolder(path);
    } else {
        openAsset(path);
    }
}

void AssetManagerWidget::openAsset(const QString& path) {
    if (!openWithinApp(path)) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool AssetManagerWidget::openWithinApp(const QString& path) {
    if (pose::ImportFormats::isModelFile(path)) {
        // Importable model: load it into the 3D viewport rather than the OS default app.
        emit importModelRequested(path);
    } else if (path.endsWith(QStringLiteral(".duf"), Qt::CaseInsensitive) && pose::isShaderPresetFile(path.toStdString())) {
        // A content library's shader preset: a material, put on the selected zone (the extension
        // is every kind of preset's, so the file itself is asked what it is).
        emit applyMaterialRequested(path);
    } else if (pose::ImportFormats::isFigureRouteFile(path)) {
        // Character figure (native, or a rigged FBX / glTF / USD): import it into the 3D viewport
        // (same path as File -> Import / open-with).
        emit importFigureRequested(path);
    } else if (pose::ImportFormats::isSplatFile(path)) {
        emit importSplatRequested(path); // a Gaussian splat scene, as a static model
    } else if (pose::ImportFormats::isAppliedMaterialFile(path)) {
        emit applyMaterialRequested(path); // a saved material, or a MaterialX document: put on the selected zone
    } else if (AssetScriptLink::isScriptFile(path)) {
        openScriptLink(path); // read for its link, never run
    } else {
        return false;
    }
    return true;
}

// =============================================================================
// [ SCRIPT LINKS ]
// =============================================================================

QStringList AssetManagerWidget::scriptLinkRoots() const {
    QStringList roots = m_libraryPaths;
    const QString configured =
        PreferencesManager::instance().getValue(Constants::PREF_FIGURE_CONTENT_ROOTS).toString();
    for (const QString& root : configured.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!root.trimmed().isEmpty()) roots << root.trimmed();
    }
    return roots;
}

void AssetManagerWidget::openScriptLink(const QString& scriptPath) {
    using AssetScriptLink::Link;
    Link link = AssetScriptLink::follow(scriptPath, scriptLinkRoots());
    qInfo().noquote() << "[script-link]" << QDir::toNativeSeparators(scriptPath) << "->"
                      << (link.kind == Link::Kind::None ? QStringLiteral("nothing") : link.target);

    switch (link.kind) {
    case Link::Kind::Folder: {
        // The folder in the tree when it lies in a library (the content library's own
        // behaviour), else in the grid alone.
        navigateToFolderInTree(link.target);
        if (AssetScan::normalizePath(m_currentFolderPath)
                .compare(AssetScan::normalizePath(link.target), AssetScan::kPathCase) != 0) {
            deselectTree();
            displayFolder(link.target);
        }
        // A folder of one character IS that character: open it, as its double-click would.
        if (const QString figure = AssetScriptLink::loneFigureIn(link.target); !figure.isEmpty()) {
            qInfo().noquote() << "[script-link] the folder's one figure:" << QDir::toNativeSeparators(figure);
            emit importFigureRequested(figure);
        }
        return;
    }
    case Link::Kind::File:
        // (Never a script: follow() has already walked the chain. And always a file PoseStudio
        // opens — but should the reader ever say otherwise, the file is NOT handed on: the
        // message below is shown instead.)
        if (openWithinApp(link.target)) return;
        link.problem = Link::Problem::Unsupported;
        link.missing = QDir::toNativeSeparators(link.target);
        break;
    case Link::Kind::Url:
        QDesktopServices::openUrl(QUrl(link.target));
        return;
    case Link::Kind::None:
        break;
    }

    QString why;
    switch (link.problem) {
    case Link::Problem::Unreadable:
        why = tr("It is encrypted or compiled, so PoseStudio cannot read where it leads.");
        break;
    case Link::Problem::Missing:
        why = tr("It leads to \"%1\", which could not be found.").arg(link.missing);
        break;
    case Link::Problem::Unsupported:
        why = tr("It leads to \"%1\", which is not a kind of file PoseStudio opens.").arg(link.missing);
        break;
    default:
        why = tr("It is a program for another application, not a link to a folder, a file or a web page.");
        break;
    }
    QMessageBox box(QMessageBox::Information, tr("Script Not Followed"),
                    tr("\"%1\" is a script. PoseStudio does not run scripts; it follows the ones "
                       "that link to a folder, a file or a web page.")
                        .arg(QFileInfo(scriptPath).fileName()),
                    QMessageBox::NoButton, this);
    box.setInformativeText(why);
    QPushButton* openElsewhere =
        box.addButton(tr("Open in Default Application"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.setDefaultButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == openElsewhere) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(scriptPath));
    }
}

/**
 * @brief Intercepts events on the asset grid viewport: the drag controller sees every mouse
 *        event first (a live drag consumes moves), then the custom tooltip's show/hide logic.
 */
bool AssetManagerWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == assetListWidget->viewport()) {
        // 0. The hand-rolled asset drag (reorder / drop onto a tree node).
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseMove) {
            if (m_gridDrag->handleMouseEvent(static_cast<QMouseEvent *>(event)))
                return true; // consumed during an active drag (also suppresses tooltips)
        }

        // 1. Intercept the exact moment Qt tries to spawn a native tooltip
        if (event->type() == QEvent::ToolTip) {
            QHelpEvent *helpEvent = static_cast<QHelpEvent *>(event);
            QListWidgetItem *item = assetListWidget->itemAt(helpEvent->pos());

            if (item) {
                const QString html = assetTooltipHtml(item);
                if (!html.isEmpty()) {
                    customToolTip->stopHideTimer();
                    customToolTip->setText(html);

                    // Offset the box slightly right/down so the cursor doesn't instantly block it
                    customToolTip->move(helpEvent->globalPos() + QPoint(15, 15));
                    customToolTip->show();

                    // Only update the "owner" when a new tooltip successfully spawns
                    activeToolTipItem = item;
                    return true; // fully suppress the native OS tooltip
                }
            } else {
                customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
            }
        }
        // 2. Track mouse movements to manage the closing grace period
        else if (event->type() == QEvent::MouseMove) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            QListWidgetItem *item = assetListWidget->itemAt(mouseEvent->pos());

            if (customToolTip->isVisible()) {
                if (item == activeToolTipItem) {
                    // The mouse is still moving safely inside the item that OWNS the tooltip. Keep it alive.
                    customToolTip->stopHideTimer();
                } else {
                    // The mouse left the owning item (to empty space OR a neighboring item). Start the countdown!
                    customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
                }
            }
        }
        // 3. The mouse left the grid entirely
        else if (event->type() == QEvent::Leave) {
            if (customToolTip->isVisible()) {
                customToolTip->startHideTimer(Constants::TOOLTIP_HIDE_DELAY_MS);
            }
        }
    }

    // Pass all other normal events back to the application
    return QWidget::eventFilter(watched, event);
}

bool AssetManagerWidget::isSortableView() const {
    return AssetNode::isVirtual(m_currentFolderPath);
}

void AssetManagerWidget::persistGridOrder() {
    QStringList pathsInOrder;
    pathsInOrder.reserve(assetListWidget->count());
    for (int i = 0; i < assetListWidget->count(); ++i)
        pathsInOrder.append(assetListWidget->item(i)->data(AssetGridRole::FilePath).toString());

    if (m_currentFolderPath == AssetNode::FAVORITES_ROOT) {
        AssetDb::setFavoriteOrder(pathsInOrder);
        m_favoritesRevisionShown = AssetDb::favoritesRevision(); // the grid already shows it
    } else if (AssetNode::isCollection(m_currentFolderPath))
        AssetDb::setCollectionOrder(AssetNode::collectionId(m_currentFolderPath), pathsInOrder);
}

// =============================================================================
// [ COLLECTIONS & FAVORITES ]
// =============================================================================

/**
 * @brief Persists a renamed Collection back to the SQLite database. Empty/whitespace names are
 *        refused: the row reverts to its stored name.
 */
void AssetManagerWidget::onItemChanged(QStandardItem *item) {
    if (!item) return;

    // Handle Collection renaming (at any nesting depth, not just top-level collections)
    const int collId = AssetNode::collectionId(item->data(Qt::UserRole).toString());
    if (collId < 0) return;

    const QString newName = item->text().trimmed();
    const auto revertTo = [&](const QString& name) {
        // setText re-enters this slot; the block keeps it from becoming a second rename.
        const QSignalBlocker blocker(dirModel);
        item->setText(name);
    };

    if (newName.isEmpty()) {
        revertTo(AssetDb::collectionName(collId));
        return;
    }
    if (newName != item->text()) revertTo(newName); // store what we display: the trimmed name

    if (!AssetDb::renameCollection(collId, newName)) {
        revertTo(AssetDb::collectionName(collId));
        return;
    }
    QStandardItem* parent = item->parent() ? item->parent() : collectionsRootItem;
    parent->sortChildren(0, Qt::AscendingOrder);
}

QStringList AssetManagerWidget::favoritePathsInScope() const {
    const QStringList all = AssetDb::favoritePaths();
    return m_scope.hasOwnRoots() ? AssetScan::pathsWithin(all, m_libraryPaths) : all;
}

QString AssetManagerWidget::newCollectionName(const QString& baseName, int parentCollectionId) const {
    return AssetDb::uniqueCollectionName(baseName, parentCollectionId, m_scope.collections);
}

int AssetManagerWidget::createCollection(const QString& name, int parentCollectionId) {
    const int newId = AssetDb::createCollection(name, parentCollectionId, m_scope.collections);
    if (newId > 0) insertCollectionTreeItem(newId, name, parentCollectionId);
    return newId;
}

void AssetManagerWidget::insertCollectionTreeItem(int collectionId, const QString& name, int parentCollectionId) {
    QStandardItem *parentItem = (parentCollectionId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(parentCollectionId);
    if (!parentItem) return;

    parentItem->appendRow(makeCollectionItem(collectionId, name));
    parentItem->sortChildren(0, Qt::AscendingOrder);
}

/**
 * @brief Turns a physical folder into a collection: creates a new (uniquely named) sub-collection
 *        named after the folder under parentCollectionId, then adds the folder's own assets as
 *        collection items (subfolders and their contents are ignored). Returns the new id, or -1.
 */
int AssetManagerWidget::addFolderAsCollection(const QString& folderPath, int parentCollectionId) {
    const QString folderName = QDir(folderPath).dirName();
    const int newColId = createCollection(
        newCollectionName(folderName, parentCollectionId), parentCollectionId);
    if (newColId <= 0) return -1;

    // scanFolderAssets returns only this folder's own (non-recursive) thumbnailed assets.
    const QList<AssetHit> assets = AssetScan::scanFolderAssets(folderPath);
    QStringList paths;
    paths.reserve(assets.size());
    for (const AssetHit& hit : assets)
        paths.append(QDir(hit.folderPath).filePath(hit.assetFileName));
    AssetDb::addCollectionItems(newColId, paths);

    refreshCollectionNode(newColId);
    return newColId;
}

/**
 * @brief Moves a collection (and its entire subtree, untouched) under a new parent, in response
 *        to a validated drag-drop from AssetFolderProxyModel. Only the dragged collection's own
 *        AssetCollectionParentID changes in the DB; descendants keep referencing its same ID, so
 *        they come along structurally without any writes of their own.
 */
void AssetManagerWidget::reparentCollection(int collectionId, int newParentId) {
    QStandardItem *collItem = findCollectionTreeItem(collectionId);
    if (!collItem) return;
    QStandardItem *oldParentItem = collItem->parent();
    if (!oldParentItem) return;

    QStandardItem *newParentItem = (newParentId == 0)
        ? collectionsRootItem
        : findCollectionTreeItem(newParentId);
    if (!newParentItem || oldParentItem == newParentItem) return;

    if (!AssetDb::reparentCollection(collectionId, newParentId)) return; // logged

    const QList<QStandardItem*> takenRow = oldParentItem->takeRow(collItem->row());
    newParentItem->appendRow(takenRow);
    newParentItem->sortChildren(0, Qt::AscendingOrder);

    const QModelIndex newProxyIdx = proxyModel->mapFromSource(dirModel->indexFromItem(collItem));
    if (newProxyIdx.isValid()) {
        m_selectingProgrammatically = true;
        dirTreeView->setCurrentIndex(newProxyIdx);
        dirTreeView->scrollTo(newProxyIdx);
        m_selectingProgrammatically = false;
    }
}

bool AssetManagerWidget::addAssetToCollection(const QString& filePath, int collectionId) {
    if (!AssetDb::addCollectionItem(filePath, collectionId)) return false;
    refreshCollectionNode(collectionId);
    return true;
}

bool AssetManagerWidget::removeAssetFromCollection(const QString& filePath, int collectionId) {
    if (!AssetDb::removeCollectionItem(filePath, collectionId)) return false;
    refreshCollectionNode(collectionId);
    return true;
}

void AssetManagerWidget::dropAssetOnTreeNode(QListWidgetItem* item, const QString& targetNodeId) {
    if (!item || targetNodeId.isEmpty()) return;
    const QString fullPath = item->data(AssetGridRole::FilePath).toString();
    if (fullPath.isEmpty()) return;

    // Dropping onto the very node the asset already lives in is a no-op.
    if (targetNodeId == m_currentFolderPath) return;

    // The current view decides the gesture: a Favorites/Collection source means the asset is
    // already filed somewhere, so the drag MOVES it; a library/search source just ADDS a copy
    // (`from` stays empty, and AssetDb::moveItem then only adds).
    AssetDb::Filing from;
    if (m_currentFolderPath == AssetNode::FAVORITES_ROOT) from.favorites = true;
    else if (AssetNode::isCollection(m_currentFolderPath)) from.collectionId = AssetNode::collectionId(m_currentFolderPath);
    const bool isMove = from.favorites || from.collectionId > 0;

    // Dropping on the Collections header spawns a brand-new top-level collection, same as
    // picking "New Collection" from the asset's context menu, and files the asset into it.
    AssetDb::Filing to;
    int newCollectionId = -1;
    if (targetNodeId == AssetNode::FAVORITES_ROOT) {
        to.favorites = true;
    } else if (targetNodeId == AssetNode::COLLECTIONS_ROOT) {
        newCollectionId = createCollection(newCollectionName(tr("New Collection"), 0), 0);
        if (newCollectionId <= 0) return; // logged; nothing was moved
        to.collectionId = newCollectionId;
    } else {
        to.collectionId = AssetNode::collectionId(targetNodeId);
    }

    // Add + remove in ONE transaction: a failed add can never delete the asset's only filing.
    // (Both inserts are UNIQUE/INSERT-OR-IGNORE, so re-filing is harmless.)
    if (!AssetDb::moveItem(fullPath, from, to)) return; // logged; nothing changed
    if (to.collectionId > 0) refreshCollectionNode(to.collectionId);
    if (from.collectionId > 0) refreshCollectionNode(from.collectionId);

    if (newCollectionId > 0) {
        // Jump to the freshly created collection and drop it straight into rename edit mode,
        // matching the "New Collection" context-menu action.
        navigateToCollectionNode(newCollectionId, true);
    } else if (isMove) {
        // A move leaves the current grid; refresh it so the asset visibly disappears from this view.
        refreshCurrentView();
    }
}
