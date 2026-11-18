/**
 * @file assetfileops.cpp
 * @brief Implementation of an asset's file operations (assetfileops.h): rename, duplicate,
 *        move and delete, each taking the icon and the textures along.
 */

#include "assetfileops.h"

#include "assetstore_detail.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>

#include <utility>

using namespace AssetStore::detail;

namespace {

/// True when @p folder is a texture store or lies inside one, anywhere up its path.
bool inAStore(const QString& folder) {
    const QStringList parts = clean(folder).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (AssetStore::isTextureStore(part)) {
            return true;
        }
    }
    return false;
}

/// Every store file the OTHER own assets of @p folder name (keys), so that a texture is removed
/// only when nobody is left to name it. @p allRead comes back false when one of them could not
/// be read: what THAT one names is unknown, and the caller must then remove no texture at all —
/// "could not be read" is no proof of "names nothing" (the library may live in a synced folder,
/// and a file there can be a placeholder, half-written or a conflicted copy at any moment).
QSet<QString> namedByOthers(const QString& folder, const QString& exceptAsset, bool& allRead) {
    QSet<QString> named;
    allRead = true;
    const QFileInfoList files = QDir(folder).entryInfoList(AssetScan::kFileFilter);
    for (const QFileInfo& f : files) {
        if (!AssetStore::isOwnAsset(f.fileName()) || samePath(f.absoluteFilePath(), exceptAsset)) {
            continue;
        }
        bool readable = false;
        for (const QString& file : AssetStore::materialOwnFiles(f.absoluteFilePath(), &readable)) {
            named.insert(key(file));
        }
        allRead = allRead && readable;
    }
    return named;
}

bool removeFile(const QString& path, bool permanently) {
    if (!QFileInfo::exists(path)) {
        return true;
    }
    makeWritable(path);
    return permanently ? QFile::remove(path) : QFile::moveToTrash(path);
}

/// Removes the folders the removal of @p file emptied, up to and including the store itself.
void pruneEmptyFolders(const QString& file, const QString& store) {
    QString dir = QFileInfo(file).absolutePath();
    while (within(dir, store)) {
        if (!QDir().rmdir(dir)) {
            break; // not empty (or not ours to remove)
        }
        if (samePath(dir, store)) {
            break;
        }
        dir = QFileInfo(dir).absolutePath();
    }
}

} // namespace

namespace AssetStore {

bool validAssetName(const QString& name, QString* why) {
    const auto no = [why](const QString& reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    if (name.trimmed().isEmpty()) {
        return no(QObject::tr("The name is empty."));
    }
    static const QString kForbidden = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : name) {
        if (kForbidden.contains(c) || c.unicode() < 32) {
            return no(QObject::tr("A name cannot contain any of these characters: \\ / : * ? \" < > |"));
        }
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return no(QObject::tr("A name cannot end with a dot or a space."));
    }
    if (name.startsWith(QLatin1Char('.'))) {
        return no(QObject::tr("A name cannot begin with a dot: the library does not show files and folders whose names do."));
    }
    return true;
}

Result renameAsset(const QString& assetPath, const QString& newName) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QString name = newName.trimmed();
    QString       why;
    if (!validAssetName(name, &why)) {
        return failed(why);
    }
    const QDir    folder = info.absoluteDir();
    const QString oldName = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    Result        r;
    if (name == oldName) {
        r.ok = true;
        r.path = clean(info.absoluteFilePath());
        return r;
    }
    // (A change of case alone names the same file on Windows and macOS: nothing is in its way.
    // Where case tells files apart it is a name like any other, and may be taken.)
    const bool    caseOnly = kFileCase == Qt::CaseInsensitive && name.compare(oldName, Qt::CaseInsensitive) == 0;
    const QString target = folder.filePath(name + suffix);
    if (!caseOnly && QFileInfo::exists(target)) {
        return failed(QObject::tr("\"%1\" is already in this folder.").arg(name + suffix));
    }
    const QStringList icons = iconsOf(assetPath);
    QStringList       iconTargets;
    for (const QString& icon : icons) {
        const QString iconTarget = folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix());
        if (!caseOnly && QFileInfo::exists(iconTarget)) {
            return failed(QObject::tr("A picture named \"%1\" is already in this folder.").arg(QFileInfo(iconTarget).fileName()));
        }
        iconTargets.append(iconTarget);
    }

    // ALL OR NOTHING, the icons first. An asset is a file WITH a picture of its name beside it:
    // renamed without its picture it would be in no browser, with nothing in the application to
    // mend it. So whichever step fails (a picture a sync client or a thumbnail decode holds open
    // is the usual one), what was renamed before it is renamed back.
    int renamed = 0;
    while (renamed < icons.size() && QFile::rename(icons[renamed], iconTargets[renamed])) {
        ++renamed;
    }
    const bool iconsRenamed = renamed == icons.size();
    if (!iconsRenamed || !QFile::rename(info.absoluteFilePath(), target)) {
        bool restored = true;
        for (int i = renamed - 1; i >= 0; --i) {
            restored = QFile::rename(iconTargets[i], icons[i]) && restored;
        }
        if (!restored) {
            return failed(QObject::tr("The file could not be renamed, and its icon could not be given its old name back:\n%1")
                              .arg(QDir::toNativeSeparators(assetPath)));
        }
        return iconsRenamed
                   ? failed(QObject::tr("The file could not be renamed:\n%1").arg(QDir::toNativeSeparators(assetPath)))
                   : failed(QObject::tr("The asset's icon could not be renamed, so nothing was renamed:\n%1")
                                .arg(QDir::toNativeSeparators(icons[renamed])));
    }
    // The name it carries inside (for people; a file this build cannot read keeps its content).
    json root;
    if (readJson(target, root) && isMaterial(root)) {
        root["name"] = name.toStdString();
        writeJson(target, root);
    }
    r.ok = true;
    r.path = clean(target);
    return r;
}

Result duplicateAsset(const QString& assetPath) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QDir        folder = info.absoluteDir();
    const QString     oldName = info.completeBaseName();
    const QString     suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    const QStringList icons = iconsOf(assetPath);

    // "<name> copy", "<name> copy 2", …: the first under which neither the file nor a picture is.
    QString name;
    for (int n = 1;; ++n) {
        name = n == 1 ? QObject::tr("%1 copy").arg(oldName) : QObject::tr("%1 copy %2").arg(oldName).arg(n);
        bool taken = QFileInfo::exists(folder.filePath(name + suffix));
        for (const QString& icon : icons) {
            taken = taken || QFileInfo::exists(folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix()));
        }
        if (!taken) {
            break;
        }
    }
    const QString target = folder.filePath(name + suffix);
    if (!QFile::copy(info.absoluteFilePath(), target)) {
        return failed(QObject::tr("The copy could not be written:\n%1").arg(QDir::toNativeSeparators(target)));
    }
    makeWritable(target);
    json root;
    if (readJson(target, root) && isMaterial(root)) {
        root["name"] = name.toStdString();
        writeJson(target, root);
    }
    // (A copy without its icon would be no asset to the browser: all of it, or none.)
    QStringList made{target};
    for (const QString& icon : icons) {
        const QString iconTarget = folder.filePath(name + QLatin1Char('.') + QFileInfo(icon).suffix());
        if (!QFile::copy(icon, iconTarget)) {
            for (const QString& file : std::as_const(made)) {
                removeFile(file, /*permanently=*/true);
            }
            return failed(QObject::tr("The asset's icon could not be copied, so no copy was made:\n%1").arg(QDir::toNativeSeparators(icon)));
        }
        makeWritable(iconTarget);
        made.append(iconTarget);
    }
    Result r;
    r.ok = true;
    r.path = clean(target);
    return r;
}

Result moveAsset(const QString& assetPath, const QString& destinationFolder) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    if (!QFileInfo(destinationFolder).isDir()) {
        return failed(QObject::tr("The folder is not there:\n%1").arg(QDir::toNativeSeparators(destinationFolder)));
    }
    const QString folder = clean(info.absolutePath());
    const QDir    destination(clean(QDir(destinationFolder).absolutePath()));
    Result        r;
    if (samePath(folder, destination.path())) {
        r.ok = true;
        r.path = clean(info.absoluteFilePath());
        return r;
    }
    if (inAStore(destination.path())) {
        return failed(QObject::tr("A \"%1\" folder holds the pictures assets use, not assets. Choose another folder.").arg(texturesFolderName()));
    }
    const QString target = destination.filePath(info.fileName());
    if (QFileInfo::exists(target)) {
        return failed(QObject::tr("\"%1\" is already in that folder.").arg(info.fileName()));
    }
    const QStringList icons = iconsOf(assetPath);
    for (const QString& icon : icons) {
        if (QFileInfo::exists(destination.filePath(QFileInfo(icon).fileName()))) {
            return failed(QObject::tr("A picture named \"%1\" is already in that folder.").arg(QFileInfo(icon).fileName()));
        }
    }

    // ALL OR NOTHING. Everything is first MADE in the destination — the textures, the asset, a
    // copy of each icon — and only then is the old asset removed. Whatever fails before that
    // point, what this call itself wrote is taken away again (never a texture the destination's
    // store already held: that one was named, not written) and nothing has moved.
    QStringList made;
    const auto  nothingMoved = [&made, &destination](const QString& why) {
        for (auto file = made.crbegin(); file != made.crend(); ++file) {
            if (removeFile(*file, /*permanently=*/true)) {
                pruneEmptyFolders(*file, storeRoot(destination.path()));
            }
        }
        return failed(why);
    };

    // The textures first: a move that cannot take them along must not have moved anything.
    json        root;
    const bool  material = readJson(info.absoluteFilePath(), root) && isMaterial(root);
    QStringList fromStore; // what the asset named in the OLD folder's store
    if (material) {
        TextureCollector collector(destination.path(), info.completeBaseName());
        for (json& entry : *materialTextures(root)) {
            const QString written = portable(sourceOf(entry));
            if (written.isEmpty() || !QDir::isRelativePath(written)) {
                continue; // an absolute path is as good from the new folder as from the old
            }
            const QString absolute = clean(QDir(folder).absoluteFilePath(written));
            if (!QFileInfo(absolute).isFile()) {
                continue; // (missing already: the name is kept, for the search by name)
            }
            Placed        how = Placed::Failed;
            const QString placed = collector.addFile(absolute, &how);
            if (how == Placed::Failed) {
                return nothingMoved(QObject::tr("A texture could not be copied to the new folder, so nothing was moved:\n%1")
                                        .arg(QDir::toNativeSeparators(absolute)));
            }
            if (how == Placed::Copied) {
                const QString copy = clean(destination.absoluteFilePath(placed));
                if (!made.contains(copy)) {
                    made.append(copy); // (one source handed over twice is one copy)
                }
            }
            entry["source"] = placed.toStdString();
            ++r.files;
            r.reused += how == Placed::Reused ? 1 : 0;
            if (within(absolute, storeRoot(folder))) {
                fromStore.append(absolute);
            }
        }
    }
    if (material ? !writeJson(target, root) : !QFile::copy(info.absoluteFilePath(), target)) {
        return nothingMoved(QObject::tr("The file could not be written:\n%1").arg(QDir::toNativeSeparators(target)));
    }
    made.append(target);
    for (const QString& icon : icons) {
        const QString iconTarget = destination.filePath(QFileInfo(icon).fileName());
        if (!QFile::copy(icon, iconTarget)) {
            return nothingMoved(QObject::tr("The asset's icon could not be copied to the new folder, so nothing was moved:\n%1")
                                    .arg(QDir::toNativeSeparators(icon)));
        }
        made.append(iconTarget);
    }
    if (!removeFile(info.absoluteFilePath(), /*permanently=*/true)) {
        return nothingMoved(QObject::tr("The old file could not be removed, so nothing was moved:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }

    // It has moved. What is left to do is tidying the old folder, and none of it can undo the
    // move: the old icons (a picture that will not go is a picture with no asset beside it —
    // shown by no browser), and the textures.
    for (const QString& icon : icons) {
        removeFile(icon, /*permanently=*/true);
    }
    // What the asset named in the old folder's store, and no asset left there names, goes
    // (the new folder has it). Nothing else in that store is touched — and nothing at all when
    // an asset left there could not be read (namedByOthers).
    bool                allRead = true;
    const QSet<QString> named = namedByOthers(folder, QString(), allRead);
    for (const QString& file : fromStore) {
        if (!allRead) {
            r.kept.append(QDir(folder).relativeFilePath(file));
        } else if (!named.contains(key(file)) && removeFile(file, /*permanently=*/true)) {
            pruneEmptyFolders(file, storeRoot(folder));
        }
    }
    r.ok = true;
    r.path = clean(target);
    return r;
}

Result deleteAsset(const QString& assetPath, bool permanently) {
    const QFileInfo info(assetPath);
    if (!info.isFile()) {
        return failed(QObject::tr("The file is no longer there:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    const QString folder = clean(info.absolutePath());
    // Read before anything goes: what it names in the store, and what the others still name.
    QStringList inStore;
    for (const QString& file : materialOwnFiles(info.absoluteFilePath())) {
        if (within(file, storeRoot(folder))) {
            inStore.append(file);
        }
    }
    bool                allRead = true;
    const QSet<QString> named = namedByOthers(folder, info.absoluteFilePath(), allRead);
    const QStringList   icons = iconsOf(assetPath);

    if (!removeFile(info.absoluteFilePath(), permanently)) {
        return failed(permanently ? QObject::tr("The file could not be removed:\n%1").arg(QDir::toNativeSeparators(assetPath))
                                  : QObject::tr("The file could not be moved to the trash:\n%1").arg(QDir::toNativeSeparators(assetPath)));
    }
    Result r;
    for (const QString& icon : icons) {
        removeFile(icon, permanently);
    }
    for (const QString& file : inStore) {
        if (!allRead) {
            r.kept.append(QDir(folder).relativeFilePath(file)); // (another asset here may use it: namedByOthers)
            continue;
        }
        if (named.contains(key(file))) {
            continue; // another asset of the folder still uses it
        }
        if (removeFile(file, permanently)) {
            ++r.files;
            pruneEmptyFolders(file, storeRoot(folder));
        }
    }
    r.ok = true;
    return r;
}

} // namespace AssetStore
