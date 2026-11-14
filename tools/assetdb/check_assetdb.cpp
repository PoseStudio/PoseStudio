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
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "assetdb.h"
#include "assetscan.h"
#include "database.h"

#include <QCoreApplication>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

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

/// The schema file's statements, split the way initializeDatabase splits them (whole-line
/// comments dropped, then on ';').
QStringList schemaStatements(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    QStringList kept;
    for (const QString& line : lines) {
        if (!line.trimmed().startsWith(QLatin1String("--"))) kept << line;
    }
    QStringList statements;
    for (const QString& statement : kept.join(QLatin1Char('\n')).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (!statement.trimmed().isEmpty()) statements << statement.trimmed();
    }
    return statements;
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

    // The connection must be gone before the application object is.
    QSqlDatabase::database(QLatin1String(kDatabaseConnectionName)).close();
    QSqlDatabase::removeDatabase(QLatin1String(kDatabaseConnectionName));

    std::printf("%d failed\n", failures);
    return failures;
}
