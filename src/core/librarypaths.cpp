/**
 * @file librarypaths.cpp
 * @brief Implementation of LibraryPaths. See librarypaths.h.
 */

#include "librarypaths.h"

#include "assetlibraries.h"
#include "constants.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

// The stock panorama preferred as the startup default whenever it exists (wherever the user
// filed it — root or any category subfolder). Matched by basename, extension-agnostic, so either
// a .hdr or an .exr of the stock set satisfies it.
constexpr const char* kStockHdri = "Studio Small 09";

// The library root, resolved once from the database. Empty = not resolved yet (the root itself
// is never empty — see userLibraryRoot).
QString g_cachedRoot;

} // namespace

QString LibraryPaths::userLibraryRoot() {
    if (!g_cachedRoot.isEmpty()) {
        return g_cachedRoot;
    }
    // Prefer a library the user has registered (wherever on disk they keep it) so their existing
    // "My PoseStudio Library" is honoured; the Documents default is the fresh-install fallback and
    // matches the folder initializeDatabase() creates on first launch.
    QString root;
    const QList<AssetLibraries::Library> libraries = AssetLibraries::enabled();
    for (const AssetLibraries::Library& lib : libraries) {
        if (lib.builtIn) {
            continue;
        }
        if (QDir(lib.path).dirName().compare(QLatin1String(Constants::USER_LIBRARY_DIRNAME),
                                             Qt::CaseInsensitive) == 0) {
            root = lib.path;
            break;
        }
    }
    if (root.isEmpty()) {
        root = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                   .filePath(QLatin1String(Constants::USER_LIBRARY_DIRNAME));
    }
    g_cachedRoot = root;
    return root;
}

void LibraryPaths::invalidateCache() {
    g_cachedRoot.clear();
}

QString LibraryPaths::hdriDirectory() {
    return QDir(userLibraryRoot()).filePath(QStringLiteral("hdri"));
}

QString LibraryPaths::ensureHdriDirectory() {
    const QString dir = hdriDirectory();
    QDir().mkpath(dir);
    return dir;
}

QString LibraryPaths::materialsDirectory() {
    if (const QString lever = qEnvironmentVariable("POSESTUDIO_MATERIALS_DIR"); !lever.isEmpty()) {
        return QDir::cleanPath(QDir::fromNativeSeparators(lever));
    }
    return QDir(userLibraryRoot()).filePath(QStringLiteral("Materials"));
}

QString LibraryPaths::ensureMaterialsDirectory() {
    const QString dir = materialsDirectory();
    QDir().mkpath(dir);
    return dir;
}

QString LibraryPaths::libraryRootOf(const QString& folder) {
    const QString wanted = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    QStringList   roots = AssetLibraries::enabledPaths();
    roots.append(materialsDirectory());
    QString best;
    for (const QString& candidate : std::as_const(roots)) {
        const QString root = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
        if (root.isEmpty()) {
            continue;
        }
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        const bool    holds = wanted.compare(root, Qt::CaseInsensitive) == 0 || wanted.startsWith(prefix, Qt::CaseInsensitive);
        // (The OUTERMOST: a search by file name should see the whole library.)
        if (holds && (best.isEmpty() || root.size() < best.size())) {
            best = root;
        }
    }
    return best;
}

QList<LibraryPaths::HdriEntry> LibraryPaths::hdriEntries() {
    const QDir root(hdriDirectory());

    // The sort key is the file name WITH its extension (how the flat folder listed before
    // categorization existed), which the entry doesn't otherwise carry — so keep it beside the
    // entry for the sort instead of re-deriving it per comparison.
    struct Scanned {
        HdriEntry entry;
        QString   fileName;
    };
    std::vector<Scanned> scanned;

    QDirIterator it(root.path(), QStringList{QStringLiteral("*.hdr"), QStringLiteral("*.exr")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = root.relativeFilePath(path);
        Scanned s;
        s.entry.path = path;
        // The category: the folder path relative to the hdri root — "" for uncategorized root
        // files, which therefore sort ahead of every named category.
        s.entry.category = relative.section(QLatin1Char('/'), 0, -2);
        s.entry.name = QFileInfo(path).completeBaseName();
        s.fileName = relative.section(QLatin1Char('/'), -1);
        scanned.push_back(std::move(s));
    }

    // Display order: category (subfolder) first, name within — case-insensitive.
    std::sort(scanned.begin(), scanned.end(), [](const Scanned& a, const Scanned& b) {
        const int byCategory = a.entry.category.compare(b.entry.category, Qt::CaseInsensitive);
        if (byCategory != 0) {
            return byCategory < 0;
        }
        return a.fileName.compare(b.fileName, Qt::CaseInsensitive) < 0;
    });

    QList<HdriEntry> entries;
    entries.reserve(static_cast<qsizetype>(scanned.size()));
    for (Scanned& s : scanned) {
        entries.append(std::move(s.entry));
    }
    return entries;
}

QStringList LibraryPaths::hdriFiles() {
    QStringList files;
    const QList<HdriEntry> entries = hdriEntries();
    files.reserve(entries.size());
    for (const HdriEntry& entry : entries) {
        files.append(entry.path);
    }
    return files;
}

QString LibraryPaths::defaultHdri() {
    // An explicit <appDir>/environment.hdr (or .exr) override wins — checked here, in the one
    // resolution both the viewport and the Environment panel call, so the panel's caption can
    // never disagree with what the viewport loaded.
    for (const char* name : {"environment.hdr", "environment.exr"}) {
        const QString override =
            QCoreApplication::applicationDirPath() + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(override)) {
            return override;
        }
    }
    const QList<HdriEntry> entries = hdriEntries();
    for (const HdriEntry& entry : entries) {
        if (entry.name.compare(QLatin1String(kStockHdri), Qt::CaseInsensitive) == 0) {
            return entry.path;
        }
    }
    return entries.isEmpty() ? QString() : entries.first().path;
}
