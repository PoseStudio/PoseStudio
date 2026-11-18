/**
 * @file check_assetdb.cpp
 * @brief The library browsers' data layer on a database of its own (CTest `asset-db-check`).
 *
 * The Asset Manager and the Materials Library are one widget class built twice over one
 * database (assets/assetmanagerscope.h). What keeps the two apart, and what they share, is in
 * the data layer, and that is what is checked here — on an in-memory SQLite database opened
 * under the application's connection name, so the real AssetDb statements run and nothing of
 * the user's is touched:
 *
 * - THE MIGRATION: a database made before collections had a scope gets the column from
 *   ensureSchemaCurrent() (the steps initializeDatabase runs), its collections become the
 *   Asset Manager's, and running the steps again changes nothing.
 * - A FRESH INSTALL: the schema file (resources/database/initialize.sql) creates the same
 *   column, so the two ways a database comes to be cannot drift apart.
 * - COLLECTIONS BELONG TO ONE BROWSER: each lists only its own, at every depth; a new one is
 *   named against its own siblings (a "New Collection" in each, not "New Collection (2)").
 * - FAVORITES ARE SHARED: one list, of which a scoped browser shows the part under its roots
 *   (AssetScan::pathsWithin) — and reordering that part leaves every other favorite in its
 *   place, where numbering the part alone would have shuffled it in among the others.
 * - favoritesRevision() counts the changes a browser on display has to catch up with.
 * - HIDDEN NAMES (core/hiddennames.h), on folders made in the OS temp directory: a file or a
 *   folder whose name begins with a dot is in no listing the browsers are built from — not a
 *   folder of the tree, not an asset, not a thumbnail, not a reason to call its parent a hit —
 *   and a folder named `textures` is an ordinary folder.
 * - THE THUMBNAIL RULE (assets/assetscan.h): which picture an asset wears. The three ways content
 *   names a thumbnail — `Name.png`, `Name.duf.png`, and a larger `Name.tip.png` — each still
 *   pair with `Name.duf`; two assets whose names differ only after a dot (`Material.001`,
 *   `Material.002`) each wear their own picture; and one with no picture of its own is an asset
 *   still, as it was when the first dot alone decided.
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "assetdb.h"
#include "assetscan.h"
#include "database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool run(const QString& sql) {
    QSqlQuery q(appDatabase());
    if (!q.exec(sql)) {
        std::printf("     sql failed: %s\n     %s\n", qPrintable(sql), qPrintable(q.lastError().text()));
        return false;
    }
    return true;
}

/// A new, empty in-memory database under the application's connection name (replacing the one
/// before it): appDatabase() then hands it to every AssetDb call.
bool openFreshDatabase() {
    const QString name = QLatin1String(kDatabaseConnectionName);
    if (QSqlDatabase::contains(name)) {
        QSqlDatabase::database(name).close();
        QSqlDatabase::removeDatabase(name);
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(QStringLiteral(":memory:"));
    return db.open();
}

bool hasColumn(const QString& table, const QString& column) {
    QSqlQuery q(appDatabase());
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) return false;
    while (q.next()) {
        if (q.value(QStringLiteral("name")).toString() == column) return true;
    }
    return false;
}

QStringList names(const QList<AssetDb::CollectionRow>& rows) {
    QStringList out;
    for (const AssetDb::CollectionRow& row : rows) out << row.name;
    return out;
}

/// The schema file's statements, split by the application's own splitter (database.h) — so the
/// check runs exactly what a fresh install runs, never a copy of the rule.
QStringList schemaStatements(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return splitSchemaStatements(QString::fromUtf8(file.readAll()));
}

const QString kMaterials = QStringLiteral("materials");

void checkMigration() {
    std::printf("-- a database from before collections had a scope\n");
    check(openFreshDatabase(), "an in-memory database opens under the application's connection name");

    // The tables as releases through 0.4.4 created them.
    run(QStringLiteral("CREATE TABLE Favorites(FavoriteID INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "FavoritePath TEXT NOT NULL UNIQUE, FavoriteSortOrder INTEGER NOT NULL DEFAULT 0)"));
    run(QStringLiteral("CREATE TABLE AssetCollections(AssetCollectionID INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "AssetCollectionName TEXT NOT NULL, AssetCollectionParentID INTEGER NOT NULL DEFAULT 0)"));
    run(QStringLiteral("CREATE TABLE AssetCollectionItems(AssetCollectionItemID INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "AssetCollectionItemPath TEXT NOT NULL, AssetCollectionItemCol INTEGER NOT NULL DEFAULT 0, "
                       "AssetCollectionItemSortOrder INTEGER NOT NULL DEFAULT 0, "
                       "UNIQUE(AssetCollectionItemPath, AssetCollectionItemCol) ON CONFLICT IGNORE)"));
    run(QStringLiteral("CREATE TABLE AssetLibraries(AssetLibraryID INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "AssetLibraryPath TEXT NOT NULL UNIQUE, AssetLibraryEnabled INTEGER DEFAULT 1, "
                       "AssetLibraryIsBuiltIn INTEGER NOT NULL DEFAULT 0)"));
    run(QStringLiteral("INSERT INTO AssetCollections (AssetCollectionName, AssetCollectionParentID) VALUES ('Characters', 0)"));
    run(QStringLiteral("INSERT INTO AssetCollections (AssetCollectionName, AssetCollectionParentID) VALUES ('Elves', 1)"));
    run(QStringLiteral("INSERT INTO AssetCollectionItems (AssetCollectionItemPath, AssetCollectionItemCol) "
                       "VALUES ('D:/Content/People/Elena.duf', 1)"));
    check(!hasColumn(QStringLiteral("AssetCollections"), QStringLiteral("AssetCollectionScope")),
          "the old table has no scope column");

    QSqlDatabase db = appDatabase();
    ensureSchemaCurrent(db);
    check(hasColumn(QStringLiteral("AssetCollections"), QStringLiteral("AssetCollectionScope")),
          "the schema steps add the scope column");
    check(names(AssetDb::allCollections()) == QStringList({QStringLiteral("Characters"), QStringLiteral("Elves")}),
          "the collections that were there are the Asset Manager's");
    check(AssetDb::allCollections(kMaterials).isEmpty(), "and none of them is the Materials Library's");
    check(AssetDb::collectionItemPaths(1) == QStringList{QStringLiteral("D:/Content/People/Elena.duf")},
          "their items are untouched");

    ensureSchemaCurrent(db);
    check(names(AssetDb::allCollections()).size() == 2 && AssetDb::allCollections(kMaterials).isEmpty(),
          "running the schema steps again changes nothing");
}

void checkFreshSchema(const QString& schemaPath) {
    std::printf("-- a fresh install's schema (initialize.sql)\n");
    const QStringList statements = schemaStatements(schemaPath);
    check(!statements.isEmpty(), "the schema file is read");
    check(openFreshDatabase(), "a second empty database opens");
    bool allRan = true;
    for (const QString& statement : statements) allRan = run(statement) && allRan;
    check(allRan, "every statement of the schema file runs");
    check(hasColumn(QStringLiteral("AssetCollections"), QStringLiteral("AssetCollectionScope")),
          "the schema file creates the scope column itself");

    QSqlDatabase db = appDatabase();
    ensureSchemaCurrent(db);
    check(AssetDb::createCollection(QStringLiteral("Props"), 0) > 0
              && names(AssetDb::allCollections()) == QStringList{QStringLiteral("Props")},
          "a collection is created and listed on it after the schema steps");
}

void checkCollections() {
    std::printf("-- collections belong to one browser\n");
    // (On the fresh database: the Asset Manager has "Props".)
    const int skin = AssetDb::createCollection(QStringLiteral("Skin"), 0, kMaterials);
    const int metals = AssetDb::createCollection(QStringLiteral("Metals"), 0, kMaterials);
    check(skin > 0 && metals > 0, "the Materials Library creates collections of its own");
    check(names(AssetDb::allCollections(kMaterials)) == QStringList({QStringLiteral("Metals"), QStringLiteral("Skin")}),
          "it lists them, by name");
    check(names(AssetDb::allCollections()) == QStringList{QStringLiteral("Props")},
          "the Asset Manager does not see them");

    const int freckled = AssetDb::createCollection(QStringLiteral("Freckled"), skin, kMaterials);
    QList<AssetDb::CollectionRow> rows = AssetDb::allCollections(kMaterials);
    bool nested = false;
    for (const AssetDb::CollectionRow& row : rows) nested = nested || (row.id == freckled && row.parentId == skin);
    check(nested && rows.size() == 3, "a sub-collection is listed with its parent, in its browser alone");

    // Naming: each browser against its own siblings.
    check(AssetDb::createCollection(QStringLiteral("New Collection"), 0) > 0, "the Asset Manager makes a 'New Collection'");
    check(AssetDb::uniqueCollectionName(QStringLiteral("New Collection"), 0, kMaterials) == QStringLiteral("New Collection"),
          "the Materials Library's first is still plain 'New Collection'");
    check(AssetDb::uniqueCollectionName(QStringLiteral("New Collection"), 0) == QStringLiteral("New Collection (2)"),
          "the Asset Manager's second is 'New Collection (2)'");
    AssetDb::createCollection(QStringLiteral("New Collection"), 0, kMaterials);
    check(AssetDb::uniqueCollectionName(QStringLiteral("New Collection"), 0, kMaterials) == QStringLiteral("New Collection (2)"),
          "and the Materials Library's second likewise");
    check(AssetDb::uniqueCollectionName(QStringLiteral("Freckled"), skin, kMaterials) == QStringLiteral("Freckled (2)"),
          "a sub-collection is named against its siblings");

    // Items, rename, re-parent and delete go by id, whoever owns the collection.
    check(AssetDb::addCollectionItem(QStringLiteral("D:/Lib/Materials/Skin/pale.psm"), skin)
              && AssetDb::addCollectionItem(QStringLiteral("D:/Lib/Materials/Skin/tan.psm"), skin)
              && AssetDb::collectionItemPaths(skin).size() == 2,
          "items are filed into a Materials Library collection");
    check(AssetDb::setCollectionOrder(skin, {QStringLiteral("D:/Lib/Materials/Skin/tan.psm"),
                                             QStringLiteral("D:/Lib/Materials/Skin/pale.psm")})
              && AssetDb::collectionItemPaths(skin).first().endsWith(QStringLiteral("tan.psm")),
          "and keep their manual order");
    check(AssetDb::renameCollection(metals, QStringLiteral("Metal"))
              && names(AssetDb::allCollections(kMaterials)).contains(QStringLiteral("Metal")),
          "a rename stays in its browser");
    check(AssetDb::reparentCollection(freckled, metals) && AssetDb::collectionIsEmpty(freckled)
              && !AssetDb::collectionIsEmpty(metals),
          "a collection moved under another is that one's content");
    check(AssetDb::deleteCollection(freckled) && AssetDb::allCollections(kMaterials).size() == 3,
          "a delete removes it from its browser");
    check(AssetDb::allCollections().size() == 2, "and through all of it the Asset Manager's are as they were");
}

void checkFavorites() {
    std::printf("-- favorites are one list, shown in part\n");
    const QString root = QStringLiteral("D:/Lib/My PoseStudio Library/Materials");
    const QString a = QStringLiteral("D:/Content/People/Elena.duf");
    const QString m1 = root + QStringLiteral("/Skin/pale.psm");
    const QString b = QStringLiteral("D:/Content/Props/chair.obj");
    const QString m2 = root + QStringLiteral("/Metals/gold.psm");
    const QString m3 = root + QStringLiteral("/glass.psm");
    // Beside the root, with its name as a prefix: not inside it.
    const QString near = QStringLiteral("D:/Lib/My PoseStudio Library/Materials Old/rust.psm");

    const int before = AssetDb::favoritesRevision();
    bool added = true;
    for (const QString& path : {a, m1, b, m2, m3, near}) added = AssetDb::addFavorite(path) && added;
    check(added && AssetDb::favoritePaths() == QStringList({a, m1, b, m2, m3, near}),
          "favorites are listed in the order they were added");
    check(AssetDb::favoritesRevision() > before, "adding counts as a change");

    const QStringList roots{root};
    check(AssetScan::pathsWithin(AssetDb::favoritePaths(), roots) == QStringList({m1, m2, m3}),
          "the Materials Library's part: the favorites under its folder, in order");
    check(AssetScan::pathsWithin({QStringLiteral("d:\\lib\\my posestudio library\\materials\\x.psm")}, roots).size() == 1,
          "a path spelled with another case and separators is still under the folder");
    check(AssetScan::pathsWithin({near, a}, roots).isEmpty(),
          "a folder beside it with its name as a prefix is not");
    check(AssetScan::pathsWithin({m1}, {}).isEmpty() && AssetScan::pathsWithin({}, roots).isEmpty(),
          "no roots, or no paths: nothing");

    // The Materials Library reorders ITS three; the others keep their places.
    int revision = AssetDb::favoritesRevision();
    check(AssetDb::setFavoriteOrder({m3, m1, m2}), "the part is reordered");
    check(AssetDb::favoritePaths() == QStringList({a, m3, b, m1, m2, near}),
          "its members trade places among themselves; every other favorite stays where it was");
    check(AssetDb::favoritesRevision() > revision, "reordering counts as a change");
    check(AssetScan::pathsWithin(AssetDb::favoritePaths(), roots) == QStringList({m3, m1, m2}),
          "and the part reads back in the order it was given");

    // The Asset Manager reorders the whole list: exactly that order.
    check(AssetDb::setFavoriteOrder({near, m2, m1, b, m3, a})
              && AssetDb::favoritePaths() == QStringList({near, m2, m1, b, m3, a}),
          "the whole list reordered is exactly the order given");
    // A grid that left out a favorite whose file is gone (here: b) orders the rest.
    check(AssetDb::setFavoriteOrder({a, m3, m1, m2, near})
              && AssetDb::favoritePaths() == QStringList({a, m3, m1, b, m2, near}),
          "a list that leaves one favorite out keeps that one in its place");
    check(AssetDb::setFavoriteOrder({QStringLiteral("D:/not/a/favorite.obj"), m2, m2, a})
              && AssetDb::favoritePaths() == QStringList({m2, m3, m1, b, a, near}),
          "a path that is no favorite, or named twice, is ignored");

    revision = AssetDb::favoritesRevision();
    check(AssetDb::removeFavorite(m3) && AssetDb::favoritePaths() == QStringList({m2, m1, b, a, near})
              && AssetDb::favoritesRevision() > revision,
          "removing one leaves the rest in order, and counts as a change");

    // A move between a favorite and a collection is one transaction and counts too.
    revision = AssetDb::favoritesRevision();
    AssetDb::Filing from;
    from.favorites = true;
    AssetDb::Filing to;
    to.collectionId = AssetDb::createCollection(QStringLiteral("Gold"), 0, kMaterials);
    check(AssetDb::moveItem(m2, from, to) && !AssetDb::favoritePaths().contains(m2)
              && AssetDb::collectionItemPaths(to.collectionId) == QStringList{m2}
              && AssetDb::favoritesRevision() > revision,
          "a favorite moved into a collection leaves the list and lands in the collection");

    // AN ASSET RENAMED, MOVED OR DELETED ON DISK (the library's file operations, assetstore.h):
    // what is filed under its path follows it, or goes with it.
    std::printf("-- filings follow a renamed asset, and go with a deleted one\n");
    const QString renamed = root + QStringLiteral("/Skin/porcelain.psm");
    const QStringList favoritesBefore = AssetDb::favoritePaths();
    const int         place = static_cast<int>(favoritesBefore.indexOf(m1));
    AssetDb::addCollectionItem(m1, to.collectionId);
    revision = AssetDb::favoritesRevision();
    check(place >= 0 && AssetDb::repathItem(m1, renamed) && AssetDb::favoritePaths().indexOf(renamed) == place &&
              !AssetDb::favoritePaths().contains(m1) && AssetDb::favoritePaths().size() == favoritesBefore.size() &&
              AssetDb::collectionItemPaths(to.collectionId) == QStringList({m2, renamed}) && AssetDb::favoritesRevision() > revision,
          "a renamed asset keeps its place among the favorites and in its collection");
    // Onto a path that is filed already: one filing remains, not two and not none.
    check(favoritesBefore.contains(near) && AssetDb::repathItem(near, renamed) && AssetDb::favoritePaths().count(renamed) == 1 &&
              !AssetDb::favoritePaths().contains(near) && AssetDb::favoritePaths().size() == favoritesBefore.size() - 1,
          "… onto a path that is a favorite already: the two become one");
    check(AssetDb::forgetItem(renamed) && !AssetDb::favoritePaths().contains(renamed) &&
              AssetDb::collectionItemPaths(to.collectionId) == QStringList{m2} && AssetDb::favoritePaths().contains(a),
          "a deleted asset leaves the favorites and its collections; the others stay");
    check(AssetDb::forgetItem(QStringLiteral("D:/never/filed.psm")) && AssetDb::repathItem(QStringLiteral("D:/never/filed.psm"), QStringLiteral("D:/x.psm")),
          "an asset that is filed nowhere is followed and forgotten without complaint");
}

bool touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write("x") == 1;
}

QStringList namesOf(const QFileInfoList& entries) {
    QStringList names;
    for (const QFileInfo& entry : entries) names.append(entry.fileName());
    return names;
}

/// THE HIDDEN-NAME RULE in the scans every browser is built from (assetscan.h).
void checkHiddenNames() {
    std::printf("-- a name that begins with a dot is hidden from the library browsers\n");
    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, "a temporary folder for the hidden-name checks");
        return;
    }
    // (The library's own folder has a dot name: a root is shown whatever it is called.)
    const QString lib = temp.path() + QStringLiteral("/.lib");
    bool          made = true;
    for (const char* file : {"Visible/a.psm", "Visible/a.png",
                             "textures/t.psm", "textures/t.png",                 // an ordinary folder now
                             ".hidden/h.psm", ".hidden/h.png",                   // a hidden folder with an asset in it
                             "Visible/.textures/Denim/color.png",                // an asset's texture store
                             "OnlyHidden/.inner/i.psm", "OnlyHidden/.inner/i.png",
                             "Mixed/shown.psm", "Mixed/shown.png", "Mixed/.shown.jpg",
                             "Mixed/.secret.psm", "Mixed/.secret.png",           // a hidden asset with its hidden icon
                             "Mixed/.lonely.psm", "Mixed/lonely.png",
                             "Secret/.only.psm", "Secret/.only.png"}) {
        made = touch(lib + QLatin1Char('/') + QLatin1String(file)) && made;
    }
    check(made, "the test library is written");

    check(HiddenNames::isHidden(QStringLiteral(".textures")) && HiddenNames::isHidden(QStringLiteral(".a.psm")) &&
              !HiddenNames::isHidden(QStringLiteral("textures")) && !HiddenNames::isHidden(QStringLiteral("a.b.psm")) &&
              !HiddenNames::isHidden(QStringLiteral(".")) && !HiddenNames::isHidden(QStringLiteral("..")) && !HiddenNames::isHidden(QString()),
          "a name is hidden when it begins with a dot (\".\" and \"..\" are no names)");
    check(HiddenNames::inHiddenPath(lib + QStringLiteral("/.hidden/h.psm"), lib) && HiddenNames::inHiddenPath(lib + QStringLiteral("/.hidden"), lib) &&
              HiddenNames::inHiddenPath(lib + QStringLiteral("/Visible/.textures/Denim/color.png"), lib) &&
              HiddenNames::inHiddenPath(lib + QStringLiteral("/Mixed/.secret.psm"), lib),
          "a path is hidden when any of its segments below the library's root is");
    check(!HiddenNames::inHiddenPath(lib + QStringLiteral("/Visible/a.psm"), lib) && !HiddenNames::inHiddenPath(lib, lib) &&
              !HiddenNames::inHiddenPath(lib + QStringLiteral("/textures/t.psm"), lib),
          "… and the root's own dot name hides nothing in it");

    check(namesOf(AssetScan::listSubdirectories(lib)) ==
              QStringList({QStringLiteral("Mixed"), QStringLiteral("OnlyHidden"), QStringLiteral("Secret"), QStringLiteral("textures"), QStringLiteral("Visible")}),
          "the folders listed: every one but the hidden one — `textures` among them");
    check(namesOf(AssetScan::listSubdirectories(lib + QStringLiteral("/Visible"))).isEmpty() && !AssetScan::hasSubdirectories(lib + QStringLiteral("/Visible")),
          "a folder whose only subfolder is its texture store lists none, and is not expandable");
    check(!AssetScan::hasSubdirectories(lib + QStringLiteral("/OnlyHidden")) && AssetScan::hasSubdirectories(lib),
          "… nor is one whose only subfolder is hidden");

    const QList<AssetScan::AssetHit> mixed = AssetScan::scanFolderAssets(lib + QStringLiteral("/Mixed"));
    check(mixed.size() == 1 && mixed.first().assetFileName == QLatin1String("shown.psm") && mixed.first().bestImage == QLatin1String("shown.png"),
          "a folder's assets: the hidden asset is not one, and a hidden picture is nobody's thumbnail");
    using AssetScan::HitState;
    const auto holdsAssets = [](const QString& folder) { return AssetScan::classifyFolder(folder, {}).value(folder) == HitState::DirectHit; };
    check(AssetScan::scanFolderAssets(lib + QStringLiteral("/Secret")).isEmpty() && !holdsAssets(lib + QStringLiteral("/Secret")) &&
              holdsAssets(lib + QStringLiteral("/Mixed")) && holdsAssets(lib + QStringLiteral("/textures")),
          "a folder that holds only hidden assets holds none; one named `textures` holds its own");

    const QHash<QString, HitState> states = AssetScan::classifyFolder(lib, {});
    bool                            walkedHidden = false;
    for (auto it = states.cbegin(); it != states.cend(); ++it) walkedHidden = walkedHidden || HiddenNames::inHiddenPath(it.key(), lib);
    check(states.value(lib) == HitState::IndirectHit && !walkedHidden, "classifying the library never walks into a hidden folder");
    check(AssetScan::classifyFolder(lib + QStringLiteral("/OnlyHidden"), {}).value(lib + QStringLiteral("/OnlyHidden")) == HitState::NoHit &&
              AssetScan::classifyFolder(lib + QStringLiteral("/Secret"), {}).value(lib + QStringLiteral("/Secret")) == HitState::NoHit,
          "a folder whose assets are all hidden, or all in a hidden folder, is no hit (greyed in the tree)");
    check(AssetScan::classifyFolder(lib + QStringLiteral("/Visible"), {}).value(lib + QStringLiteral("/Visible")) == HitState::DirectHit &&
              AssetScan::classifyFolder(lib + QStringLiteral("/textures"), {}).value(lib + QStringLiteral("/textures")) == HitState::DirectHit,
          "a folder with an asset is a hit, its hidden texture store beside the point — and so is one named `textures`");
}

/// A file of @p bytes bytes (a thumbnail's SIZE decides between two that qualify).
bool sized(const QString& path, int bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(QByteArray(bytes, 'x')) == bytes;
}

/// "asset=picture" for every asset of @p folder, by asset name: what the grid would show.
QStringList pairsOf(const QString& folder) {
    QStringList pairs;
    for (const AssetScan::AssetHit& hit : AssetScan::scanFolderAssets(folder)) pairs << hit.assetFileName + QLatin1Char('=') + hit.bestImage;
    pairs.sort();
    return pairs;
}

/// The same pairs as Favorites and collections resolve them (resolveAssetHits), for @p assets of @p folder.
QStringList resolvedPairsOf(const QString& folder, const QStringList& assets) {
    QStringList paths;
    for (const QString& asset : assets) paths << folder + QLatin1Char('/') + asset;
    QStringList pairs;
    for (const AssetScan::AssetHit& hit : AssetScan::resolveAssetHits(paths)) pairs << hit.assetFileName + QLatin1Char('=') + hit.bestImage;
    pairs.sort();
    return pairs;
}

/// THE THUMBNAIL RULE (assetscan.h): which picture an asset wears.
void checkThumbnails() {
    std::printf("-- an asset wears the picture named after it\n");
    QTemporaryDir temp;
    if (!temp.isValid()) {
        check(false, "a temporary folder for the thumbnail checks");
        return;
    }
    const QString lib = temp.path();
    bool          made = true;
    // The three ways content names a thumbnail.
    made = sized(lib + "/Plain/Name.duf", 10) && sized(lib + "/Plain/Name.png", 40) && made;
    made = sized(lib + "/Whole/Name.duf", 10) && sized(lib + "/Whole/Name.duf.png", 40) && made;
    made = sized(lib + "/Tip/Name.duf", 10) && sized(lib + "/Tip/Name.png", 40) && sized(lib + "/Tip/Name.tip.png", 400) && made;
    made = sized(lib + "/TipOnly/Name.duf", 10) && sized(lib + "/TipOnly/Name.tip.png", 400) && made;
    made = sized(lib + "/WholeTip/Name.duf", 10) && sized(lib + "/WholeTip/Name.DUF.png", 40) && sized(lib + "/WholeTip/Name.duf.Tip.png", 400) && made;
    // Names that differ only after a dot.
    made = sized(lib + "/Dotted/Material.001.psm", 10) && sized(lib + "/Dotted/Material.001.png", 40) && made;
    made = sized(lib + "/Dotted/Material.002.psm", 10) && sized(lib + "/Dotted/Material.002.png", 400) && made;
    made = sized(lib + "/Dotted/Material.003.psm", 10) && made; // (no picture of its own)
    made = sized(lib + "/Previews/Char 8.1 A.duf", 10) && sized(lib + "/Previews/Char 8.1 A.png", 40) && sized(lib + "/Previews/Char 8.1 A.tip.png", 90) && made;
    made = sized(lib + "/Previews/Char 8.1 B.duf", 10) && sized(lib + "/Previews/Char 8.1 B.png", 40) && sized(lib + "/Previews/Char 8.1 B.tip.png", 400) && made;
    // A picture that is named after no asset of its group.
    made = sized(lib + "/Borrowed/Set.A.duf", 10) && sized(lib + "/Borrowed/Set.B.duf", 10) && sized(lib + "/Borrowed/Set.png", 40) && made;
    check(made, "the test folders are written");

    check(pairsOf(lib + "/Plain") == QStringList{"Name.duf=Name.png"}, "`Name.png` beside `Name.duf`: an asset, wearing it");
    check(pairsOf(lib + "/Whole") == QStringList{"Name.duf=Name.duf.png"}, "`Name.duf.png` (the whole file name): an asset, wearing it");
    check(pairsOf(lib + "/Tip") == QStringList{"Name.duf=Name.tip.png"},
          "`Name.png` and a larger `Name.tip.png`: ONE asset (the preview is none of its own), wearing the larger");
    check(pairsOf(lib + "/TipOnly") == QStringList{"Name.duf=Name.tip.png"}, "`Name.tip.png` alone: an asset still");
    check(pairsOf(lib + "/WholeTip") == QStringList{"Name.duf=Name.duf.Tip.png"},
          "the whole file name with `.tip`, in any case: the larger of the asset's own again");
    check(pairsOf(lib + "/Dotted") ==
              QStringList({"Material.001.psm=Material.001.png", "Material.002.psm=Material.002.png", "Material.003.psm=Material.002.png"}),
          "`Material.001` and `Material.002` each wear their own picture; `Material.003`, with none of its own, is an asset still");
    check(pairsOf(lib + "/Previews") == QStringList({"Char 8.1 A.duf=Char 8.1 A.tip.png", "Char 8.1 B.duf=Char 8.1 B.tip.png"}),
          "a dotted name keeps to its own preview, though its neighbour's is larger");
    check(pairsOf(lib + "/Borrowed") == QStringList({"Set.A.duf=Set.png", "Set.B.duf=Set.png"}),
          "a picture named after neither still makes both assets (the first-dot rule, kept)");

    check(resolvedPairsOf(lib + "/Dotted", {"Material.001.psm", "Material.002.psm", "Material.003.psm"}) == pairsOf(lib + "/Dotted") &&
              resolvedPairsOf(lib + "/Tip", {"Name.duf"}) == pairsOf(lib + "/Tip") &&
              resolvedPairsOf(lib + "/Whole", {"Name.duf"}) == pairsOf(lib + "/Whole") &&
              resolvedPairsOf(lib + "/Previews", {"Char 8.1 A.duf", "Char 8.1 B.duf"}) == pairsOf(lib + "/Previews"),
          "Favorites and collections pair an asset with the same picture its folder does");

    using AssetScan::HitState;
    bool allHits = true;
    for (const char* folder : {"Plain", "Whole", "Tip", "TipOnly", "WholeTip", "Dotted", "Previews", "Borrowed"}) {
        const QString path = lib + QLatin1Char('/') + QLatin1String(folder);
        allHits = allHits && AssetScan::classifyFolder(path, {}).value(path) == HitState::DirectHit;
    }
    check(allHits, "every one of those folders is a folder with assets");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        std::printf("FAIL the SQLite driver is not available\n");
        return 1;
    }

    checkMigration();
    checkFreshSchema(QStringLiteral(POSESTUDIO_SCHEMA_FILE));
    checkCollections();
    checkFavorites();
    checkHiddenNames();
    checkThumbnails();

    // The connection must be gone before the application object is.
    QSqlDatabase::database(QLatin1String(kDatabaseConnectionName)).close();
    QSqlDatabase::removeDatabase(QLatin1String(kDatabaseConnectionName));

    std::printf("%d failed\n", failures);
    return failures;
}
