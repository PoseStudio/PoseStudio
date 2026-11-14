/**
 * @file check_assetstore.cpp
 * @brief The asset store on folders of its own (CTest `asset-store-check`).
 *
 * The standard for where a saved asset's files live (src/assets/assetstore.h) — the asset, its
 * icon beside it, and its textures in a `textures` folder beside both, named by relative paths
 * — is what makes a material's folder all the material needs. Checked here, in a folder made
 * in the OS temp directory and removed at the end:
 *
 * - COLLECTING: a texture beside the asset or in its folder's store is named where it is; any
 *   other is copied into `textures/<asset>/`; the same bytes are stored once (and found under
 *   ANOTHER asset's subfolder); a copy never overwrites a different file; an image without a
 *   file is written there.
 * - FINDING: relative to the asset, then to the library's root, the absolute path, and at last
 *   by file name — beside the asset, in its store, anywhere in the library (the nearest).
 * - THE FILE OPERATIONS: rename, duplicate, move and delete take the icon and the textures
 *   along, keep every field of the file they do not know, and never remove a texture another
 *   asset of the folder still names.
 * - PACKAGES: an asset or a folder zipped with what it needs, installed elsewhere and whole
 *   there; a file that is in the way is kept or replaced as asked; an archive whose entry
 *   would leave the destination is refused before anything is written.
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "assetstore.h"

#include <miniz.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

bool write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
}

QByteArray read(const QString& path) {
    QFile in(path);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

bool exists(const QString& path) {
    return QFileInfo::exists(path);
}

/// A saved material's text: the form the application writes, with one field it does not know.
QByteArray material(const QString& name, const QStringList& slotsAndSources) {
    QString textures;
    for (int i = 0; i + 1 < slotsAndSources.size(); i += 2) {
        QString source = slotsAndSources[i + 1];
        source.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        textures += QStringLiteral("%1      { \"slot\": \"%2\", \"source\": \"%3\" }").arg(i == 0 ? QString() : QStringLiteral(",\n"), slotsAndSources[i], source);
    }
    const QString text = QStringLiteral("{\n  \"format\": \"posestudio.material\",\n  \"futureField\": 7,\n  \"material\": {\n"
                                        "    \"roughness\": 0.25,\n    \"textures\": [\n%1\n    ]\n  },\n  \"name\": \"%2\",\n  \"version\": 1\n}\n")
                             .arg(textures, name);
    return text.toUtf8();
}

void checkCollecting(const QString& root) {
    const QString folder = root + QStringLiteral("/lib/Fabric");
    const QString outside = root + QStringLiteral("/elsewhere");
    write(folder + QStringLiteral("/beside.png"), "beside");
    write(folder + QStringLiteral("/textures/Canvas/weave.png"), "weave-bytes");
    write(folder + QStringLiteral("/maps/deep.png"), "deep");
    write(outside + QStringLiteral("/color.png"), "color-one");
    write(outside + QStringLiteral("/other/color.png"), "color-two");
    write(outside + QStringLiteral("/renamed_weave.png"), "weave-bytes");

    AssetStore::TextureCollector collector(folder, QStringLiteral("Denim"));
    AssetStore::Placed           how = AssetStore::Placed::Failed;

    check(collector.addFile(folder + QStringLiteral("/beside.png"), &how) == QLatin1String("beside.png") && how == AssetStore::Placed::Linked,
          "a texture beside the asset is named where it is");
    check(collector.addFile(folder + QStringLiteral("/textures/Canvas/weave.png"), &how) == QLatin1String("textures/Canvas/weave.png") &&
              how == AssetStore::Placed::Linked,
          "a texture in the folder's store is named where it is");

    const QString color = collector.addFile(outside + QStringLiteral("/color.png"), &how);
    check(color == QLatin1String("textures/Denim/color.png") && how == AssetStore::Placed::Copied &&
              read(folder + QStringLiteral("/textures/Denim/color.png")) == "color-one" && exists(outside + QStringLiteral("/color.png")),
          "a texture from elsewhere is copied into textures/<asset>/ (the original stays)");
    check(collector.addFile(outside + QStringLiteral("/color.png"), &how) == color && how == AssetStore::Placed::Copied &&
              QDir(folder + QStringLiteral("/textures/Denim")).entryList(QDir::Files).size() == 1,
          "the same file handed over twice is one copy");
    check(collector.addFile(outside + QStringLiteral("/other/color.png"), &how) == QLatin1String("textures/Denim/color_2.png") &&
              how == AssetStore::Placed::Copied && read(folder + QStringLiteral("/textures/Denim/color.png")) == "color-one" &&
              read(folder + QStringLiteral("/textures/Denim/color_2.png")) == "color-two",
          "a different file of the same name takes the next free name: nothing is overwritten");
    check(collector.addFile(outside + QStringLiteral("/renamed_weave.png"), &how) == QLatin1String("textures/Canvas/weave.png") &&
              how == AssetStore::Placed::Reused && !exists(folder + QStringLiteral("/textures/Denim/renamed_weave.png")),
          "the same bytes under another asset's subfolder are named there, not copied again");
    check(collector.addFile(folder + QStringLiteral("/maps/deep.png"), &how) == QLatin1String("textures/Denim/deep.png") &&
              how == AssetStore::Placed::Copied,
          "a texture deeper in the asset's folder (not its store) is collected into the store");
    check(collector.addFile(outside + QStringLiteral("/gone.png"), &how).isEmpty() && how == AssetStore::Placed::Failed,
          "a file that is not there is not named");

    const QString written = collector.addBytes("gray-image", QStringLiteral("Denim_roughness.png"), &how);
    check(written == QLatin1String("textures/Denim/Denim_roughness.png") && how == AssetStore::Placed::Copied &&
              read(folder + QStringLiteral("/textures/Denim/Denim_roughness.png")) == "gray-image",
          "an image without a file is written into textures/<asset>/");
    check(collector.addBytes("gray-image", QStringLiteral("Denim_roughness.png"), &how) == written && how == AssetStore::Placed::Reused,
          "… and the same image again is the file already there");
    check(collector.addBytes("gray-other", QStringLiteral("Denim_roughness.png"), &how) == QLatin1String("textures/Denim/Denim_roughness_2.png") &&
              how == AssetStore::Placed::Copied,
          "… another image under the same name takes the next free name");

    // A second save (a new collector) finds what the first one stored.
    AssetStore::TextureCollector again(folder, QStringLiteral("Denim Blue"));
    check(again.addFile(outside + QStringLiteral("/color.png"), &how) == QLatin1String("textures/Denim/color.png") &&
              how == AssetStore::Placed::Reused && !exists(folder + QStringLiteral("/textures/Denim Blue")),
          "a variant saved into the folder shares the first one's file (no folder of its own is made for nothing)");

    // A read-only source: the copy is the user's to remove.
    write(outside + QStringLiteral("/locked.png"), "locked");
    QFile::setPermissions(outside + QStringLiteral("/locked.png"), QFileDevice::ReadOwner | QFileDevice::ReadUser);
    const QString locked = again.addFile(outside + QStringLiteral("/locked.png"), &how);
    check(how == AssetStore::Placed::Copied && QFile::remove(folder + QLatin1Char('/') + locked), "the copy of a read-only file can be removed again");
    QFile::setPermissions(outside + QStringLiteral("/locked.png"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);

    check(AssetStore::isTextureStore(QStringLiteral("textures")) && AssetStore::isTextureStore(QStringLiteral("Textures")) &&
              !AssetStore::isTextureStore(QStringLiteral("texture")) && !AssetStore::isTextureStore(QStringLiteral("Denim")),
          "a folder named `textures` (any case) is a texture store, no other is");
}

void checkFinding(const QString& root) {
    const QString library = root + QStringLiteral("/find");
    const QString folder = library + QStringLiteral("/Materials/Wood");
    write(folder + QStringLiteral("/textures/Oak/grain.png"), "grain");
    write(folder + QStringLiteral("/textures/Oak/knots.png"), "knots");
    write(library + QStringLiteral("/Shared/bark.png"), "bark");
    write(library + QStringLiteral("/Far/Away/pores.png"), "far");
    write(library + QStringLiteral("/Materials/Near/pores.png"), "near");
    write(root + QStringLiteral("/abs/real.png"), "real");
    bool viaName = true;

    check(AssetStore::resolveTexture(QStringLiteral("textures/Oak/grain.png"), folder, library, &viaName) == QDir::cleanPath(folder + QStringLiteral("/textures/Oak/grain.png")) &&
              !viaName,
          "a relative path is found beside the asset");
    check(AssetStore::resolveTexture(QStringLiteral("textures\\Oak\\grain.png"), folder, library, &viaName).endsWith(QLatin1String("/textures/Oak/grain.png")) && !viaName,
          "… written with backslashes too");
    check(AssetStore::resolveTexture(QStringLiteral("Shared/bark.png"), folder, library, &viaName) == QDir::cleanPath(library + QStringLiteral("/Shared/bark.png")) && !viaName,
          "a relative path not beside the asset is tried from the library's root");
    check(AssetStore::resolveTexture(root + QStringLiteral("/abs/real.png"), folder, library, &viaName) == QDir::cleanPath(root + QStringLiteral("/abs/real.png")) && !viaName,
          "an absolute path that is there is itself");
    check(AssetStore::resolveTexture(QStringLiteral("C:/Users/SomeoneElse/Desktop/knots.png"), folder, library, &viaName).endsWith(QLatin1String("/textures/Oak/knots.png")) && viaName,
          "an absolute path from another machine is found by its name in the asset's store");
    check(AssetStore::resolveTexture(QStringLiteral("textures/Renamed/GRAIN.PNG"), folder, library, &viaName).endsWith(QLatin1String("/textures/Oak/grain.png")) && viaName,
          "a relative path whose folder was renamed is found by its name (any case)");
    check(AssetStore::resolveTexture(QStringLiteral("old/pores.png"), folder, library, &viaName) == QDir::cleanPath(library + QStringLiteral("/Materials/Near/pores.png")) && viaName,
          "a file of that name elsewhere in the library is found: the nearest to the asset");
    check(AssetStore::resolveTexture(QStringLiteral("old/nowhere.png"), folder, library, &viaName).isEmpty() && !viaName, "a file that is nowhere is not found");
    check(AssetStore::resolveTexture(QStringLiteral("old/pores.png"), folder, QString(), &viaName).isEmpty(), "outside every library only the asset's own folder is searched");
    check(AssetStore::resolveTexture(QString(), folder, library).isEmpty(), "an empty source is nothing");
}

void checkOperations(const QString& root) {
    const QString folder = root + QStringLiteral("/ops/Fabric");
    const QString other = root + QStringLiteral("/ops/Other");
    QDir().mkpath(other);
    write(folder + QStringLiteral("/textures/Denim/color.png"), "denim-color");
    write(folder + QStringLiteral("/textures/Denim/normal.png"), "shared-normal");
    write(folder + QStringLiteral("/textures/Canvas/color.png"), "canvas-color");
    write(folder + QStringLiteral("/beside.png"), "beside");
    write(root + QStringLiteral("/ops/abs.png"), "absolute");
    const QString absolute = QDir::cleanPath(root + QStringLiteral("/ops/abs.png"));
    write(folder + QStringLiteral("/Denim.psm"),
          material(QStringLiteral("Denim"), {QStringLiteral("basecolor"), QStringLiteral("textures/Denim/color.png"), QStringLiteral("normal"),
                                             QStringLiteral("textures/Denim/normal.png"), QStringLiteral("roughness"), QStringLiteral("beside.png"),
                                             QStringLiteral("emission"), absolute, QStringLiteral("bump"), QStringLiteral("textures/Denim/gone.png")}));
    write(folder + QStringLiteral("/Denim.png"), "denim-icon");
    write(folder + QStringLiteral("/Canvas.psm"),
          material(QStringLiteral("Canvas"), {QStringLiteral("basecolor"), QStringLiteral("textures/Canvas/color.png"), QStringLiteral("normal"),
                                              QStringLiteral("textures/Denim/normal.png")}));
    write(folder + QStringLiteral("/Canvas.png"), "canvas-icon");

    check(AssetStore::materialSources(folder + QStringLiteral("/Denim.psm")).size() == 5 &&
              AssetStore::materialSources(folder + QStringLiteral("/Denim.png")).isEmpty(),
          "a material's sources are read as written; another file has none");
    const QStringList own = AssetStore::materialOwnFiles(folder + QStringLiteral("/Denim.psm"));
    check(own.size() == 3 && own[0].endsWith(QLatin1String("/textures/Denim/color.png")) && own[2].endsWith(QLatin1String("/beside.png")),
          "its own files: what it names in its folder that is there (not the absolute one, not the missing one)");
    check(AssetStore::iconsOf(folder + QStringLiteral("/Denim.psm")) == QStringList{QDir::cleanPath(folder + QStringLiteral("/Denim.png"))},
          "its icon is the picture beside it under its name");
    check(AssetStore::isOwnAsset(QStringLiteral("a/b/Gold.PSM")) && !AssetStore::isOwnAsset(QStringLiteral("a/b/figure.duf")),
          "a saved material is PoseStudio's own asset; another application's file is not");

    // --- names ---
    QString why;
    check(AssetStore::validAssetName(QStringLiteral("Gold (Matte) 1.5")) && !AssetStore::validAssetName(QStringLiteral(" "), &why) && !why.isEmpty() &&
              !AssetStore::validAssetName(QStringLiteral("a/b")) && !AssetStore::validAssetName(QStringLiteral("what?")) &&
              !AssetStore::validAssetName(QStringLiteral("dot.")),
          "a name: not empty, no path characters, no trailing dot");

    // --- rename ---
    AssetStore::Result r = AssetStore::renameAsset(folder + QStringLiteral("/Denim.psm"), QStringLiteral("Canvas"));
    check(!r.ok && !r.error.isEmpty() && exists(folder + QStringLiteral("/Denim.psm")) && read(folder + QStringLiteral("/Canvas.png")) == "canvas-icon",
          "a rename onto a name that is taken is refused, and nothing changes");
    r = AssetStore::renameAsset(folder + QStringLiteral("/Denim.psm"), QStringLiteral("bad:name"));
    check(!r.ok && exists(folder + QStringLiteral("/Denim.psm")), "a rename to a name a file cannot have is refused");
    r = AssetStore::renameAsset(folder + QStringLiteral("/Denim.psm"), QStringLiteral("Indigo"));
    const QByteArray renamed = read(folder + QStringLiteral("/Indigo.psm"));
    check(r.ok && r.path.endsWith(QLatin1String("/Indigo.psm")) && !exists(folder + QStringLiteral("/Denim.psm")) && !exists(folder + QStringLiteral("/Denim.png")) &&
              read(folder + QStringLiteral("/Indigo.png")) == "denim-icon",
          "a rename moves the file and its icon to the new name");
    check(renamed.contains("\"name\": \"Indigo\"") && renamed.contains("\"futureField\": 7") && renamed.contains("\"roughness\": 0.25") &&
              renamed.contains("textures/Denim/color.png"),
          "… the name inside follows; every other field, known or not, is kept");
    check(AssetStore::materialOwnFiles(folder + QStringLiteral("/Indigo.psm")).size() == 3 && exists(folder + QStringLiteral("/textures/Denim/color.png")),
          "… and its textures stay where they are, still its own");
    r = AssetStore::renameAsset(folder + QStringLiteral("/Indigo.psm"), QStringLiteral("INDIGO"));
    check(r.ok && QDir(folder).entryList({QStringLiteral("*.psm")}, QDir::Files).contains(QStringLiteral("INDIGO.psm")) &&
              QDir(folder).entryList({QStringLiteral("*.png")}, QDir::Files).contains(QStringLiteral("INDIGO.png")),
          "a change of case alone is a rename too");
    r = AssetStore::renameAsset(folder + QStringLiteral("/INDIGO.psm"), QStringLiteral("Denim"));
    check(r.ok && exists(folder + QStringLiteral("/Denim.psm")), "(renamed back)");

    // --- duplicate ---
    r = AssetStore::duplicateAsset(folder + QStringLiteral("/Denim.psm"));
    check(r.ok && r.path.endsWith(QLatin1String("/Denim copy.psm")) && read(folder + QStringLiteral("/Denim copy.png")) == "denim-icon" &&
              read(r.path).contains("\"name\": \"Denim copy\"") && AssetStore::materialOwnFiles(r.path).size() == 3,
          "a duplicate: \"<name> copy\" with a copy of the icon, naming the same textures");
    check(QDir(folder + QStringLiteral("/textures/Denim")).entryList(QDir::Files).size() == 2 && !exists(folder + QStringLiteral("/textures/Denim copy")),
          "… nothing in the store is copied for it");
    r = AssetStore::duplicateAsset(folder + QStringLiteral("/Denim.psm"));
    check(r.ok && r.path.endsWith(QLatin1String("/Denim copy 2.psm")), "a second duplicate is \"<name> copy 2\"");
    AssetStore::deleteAsset(folder + QStringLiteral("/Denim copy 2.psm"), /*permanently=*/true);

    // --- delete: a texture goes only when nobody is left to name it ---
    r = AssetStore::deleteAsset(folder + QStringLiteral("/Denim copy.psm"), /*permanently=*/true);
    check(r.ok && r.files == 0 && !exists(folder + QStringLiteral("/Denim copy.psm")) && !exists(folder + QStringLiteral("/Denim copy.png")) &&
              exists(folder + QStringLiteral("/textures/Denim/color.png")) && exists(folder + QStringLiteral("/textures/Denim/normal.png")),
          "deleting a duplicate removes the file and its icon; the textures the original names stay");

    // --- move ---
    write(other + QStringLiteral("/textures/Linen/n.png"), "shared-normal"); // the destination has the normal map already
    write(other + QStringLiteral("/Canvas.psm"), "in the way");
    r = AssetStore::moveAsset(folder + QStringLiteral("/Canvas.psm"), other);
    check(!r.ok && exists(folder + QStringLiteral("/Canvas.psm")) && read(other + QStringLiteral("/Canvas.psm")) == "in the way",
          "a move onto a file of that name is refused, and nothing moves");
    QFile::remove(other + QStringLiteral("/Canvas.psm"));
    r = AssetStore::moveAsset(folder + QStringLiteral("/Denim.psm"), folder + QStringLiteral("/textures"));
    check(!r.ok && exists(folder + QStringLiteral("/Denim.psm")), "an asset cannot be moved into a texture store");

    r = AssetStore::moveAsset(folder + QStringLiteral("/Denim.psm"), other);
    const QByteArray moved = read(other + QStringLiteral("/Denim.psm"));
    check(r.ok && r.path.endsWith(QLatin1String("/Other/Denim.psm")) && !exists(folder + QStringLiteral("/Denim.psm")) &&
              !exists(folder + QStringLiteral("/Denim.png")) && read(other + QStringLiteral("/Denim.png")) == "denim-icon",
          "a move takes the file and its icon to the new folder");
    check(read(other + QStringLiteral("/textures/Denim/color.png")) == "denim-color" && moved.contains("\"source\": \"textures/Denim/color.png\""),
          "… its textures are collected into the new folder's store");
    check(moved.contains("\"source\": \"textures/Linen/n.png\"") && !exists(other + QStringLiteral("/textures/Denim/normal.png")) && r.reused == 1 && r.files == 3,
          "… a texture the new folder's store already holds is named there, not copied");
    check(read(other + QStringLiteral("/textures/Denim/beside.png")) == "beside" && exists(folder + QStringLiteral("/beside.png")) &&
              moved.contains("\"source\": \"textures/Denim/beside.png\""),
          "… a texture that lay beside it is copied into the store (the file beside the old place is not touched)");
    check(moved.contains(absolute.toUtf8()) && moved.contains("textures/Denim/gone.png") && moved.contains("\"futureField\": 7"),
          "… an absolute path, a missing file's name and unknown fields are kept as they were");
    check(!exists(folder + QStringLiteral("/textures/Denim/color.png")) && exists(folder + QStringLiteral("/textures/Denim/normal.png")) &&
              exists(folder + QStringLiteral("/textures/Canvas/color.png")),
          "… the old store loses what nobody there names, and keeps what another asset still names");
    check(AssetStore::materialOwnFiles(other + QStringLiteral("/Denim.psm")).size() == 3, "… and where it is now, all three are its own again");

    // --- delete: the last user of a texture takes it along, and the emptied folders ---
    r = AssetStore::deleteAsset(folder + QStringLiteral("/Canvas.psm"), /*permanently=*/true);
    check(r.ok && r.files == 2 && !exists(folder + QStringLiteral("/Canvas.psm")) && !exists(folder + QStringLiteral("/Canvas.png")) &&
              !exists(folder + QStringLiteral("/textures")) && exists(folder + QStringLiteral("/beside.png")),
          "deleting the last asset that names them removes its textures and the emptied store; a file beside it stays");
    r = AssetStore::deleteAsset(folder + QStringLiteral("/Canvas.psm"), /*permanently=*/true);
    check(!r.ok && !r.error.isEmpty(), "deleting what is no longer there says so");

    // --- an asset saved over itself with other maps: what it no longer names goes ---
    const QString tidy = root + QStringLiteral("/ops/Tidy");
    write(tidy + QStringLiteral("/textures/Wool/old.png"), "old-map");
    write(tidy + QStringLiteral("/textures/Wool/kept.png"), "kept-map");
    write(tidy + QStringLiteral("/textures/Wool/new.png"), "new-map");
    write(tidy + QStringLiteral("/beside.png"), "beside");
    write(tidy + QStringLiteral("/Wool.psm"), material(QStringLiteral("Wool"), {QStringLiteral("basecolor"), QStringLiteral("textures/Wool/old.png"),
                                                                             QStringLiteral("normal"), QStringLiteral("textures/Wool/kept.png"),
                                                                             QStringLiteral("bump"), QStringLiteral("beside.png")}));
    write(tidy + QStringLiteral("/Felt.psm"), material(QStringLiteral("Felt"), {QStringLiteral("normal"), QStringLiteral("textures/Wool/kept.png")}));
    const QStringList before = AssetStore::materialOwnFiles(tidy + QStringLiteral("/Wool.psm"));
    write(tidy + QStringLiteral("/Wool.psm"), material(QStringLiteral("Wool"), {QStringLiteral("basecolor"), QStringLiteral("textures/Wool/new.png")}));
    check(before.size() == 3 && AssetStore::removeUnnamed(tidy, before, /*permanently=*/true) == 1 && !exists(tidy + QStringLiteral("/textures/Wool/old.png")),
          "saved over itself with another map, the map nothing names any more goes");
    check(exists(tidy + QStringLiteral("/textures/Wool/kept.png")) && exists(tidy + QStringLiteral("/textures/Wool/new.png")) && exists(tidy + QStringLiteral("/beside.png")),
          "… the one another asset names stays, the new one stays, and a file outside the store is never touched");
}

/// A zip with one entry that climbs out of where it is unpacked.
QByteArray hostileZip() {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    mz_zip_writer_init_heap(&zip, 0, 0);
    mz_zip_writer_add_mem(&zip, "ok.txt", "fine", 4, MZ_DEFAULT_LEVEL);
    mz_zip_writer_add_mem(&zip, "../evil.txt", "evil", 4, MZ_DEFAULT_LEVEL);
    void*  data = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
    const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    mz_free(data);
    mz_zip_writer_end(&zip);
    return bytes;
}

void checkPackages(const QString& root) {
    const QString folder = root + QStringLiteral("/pack/Fabric");
    write(folder + QStringLiteral("/textures/Denim/color.png"), "denim-color");
    write(folder + QStringLiteral("/textures/Canvas/color.png"), "canvas-color");
    write(folder + QStringLiteral("/Denim.psm"), material(QStringLiteral("Denim"), {QStringLiteral("basecolor"), QStringLiteral("textures/Denim/color.png")}));
    write(folder + QStringLiteral("/Denim.png"), "denim-icon");
    write(folder + QStringLiteral("/Canvas.psm"), material(QStringLiteral("Canvas"), {QStringLiteral("basecolor"), QStringLiteral("textures/Canvas/color.png")}));
    write(folder + QStringLiteral("/Canvas.png"), "canvas-icon");
    write(folder + QStringLiteral("/Thumbs.db"), "clutter");

    // One asset: the file, its icon, its textures — and nothing of its neighbour.
    const QString     one = root + QStringLiteral("/pack/denim.zip");
    AssetStore::Result r = AssetStore::exportPackage({folder + QStringLiteral("/Denim.psm")}, one);
    check(r.ok && r.files == 3 && exists(one), "one asset is packed as three files: itself, its icon, its texture");
    const QString first = root + QStringLiteral("/pack/installed-one");
    QDir().mkpath(first);
    check(AssetStore::packageConflicts(one, first) == 0, "an empty folder has nothing in a package's way");
    r = AssetStore::installPackage(one, first, /*replaceExisting=*/false);
    check(r.ok && r.files == 3 && r.kept.isEmpty() && read(first + QStringLiteral("/Denim.psm")) == read(folder + QStringLiteral("/Denim.psm")) &&
              read(first + QStringLiteral("/Denim.png")) == "denim-icon" && read(first + QStringLiteral("/textures/Denim/color.png")) == "denim-color" &&
              !exists(first + QStringLiteral("/Canvas.psm")),
          "installed elsewhere it is whole: the same files in the same layout");
    check(AssetStore::resolveTexture(QStringLiteral("textures/Denim/color.png"), first, QString()) == QDir::cleanPath(first + QStringLiteral("/textures/Denim/color.png")),
          "… and the installed material finds its texture");
    r = AssetStore::installPackage(one, first, /*replaceExisting=*/false);
    check(r.ok && r.files == 0 && r.kept.isEmpty(), "installing it again writes nothing: every file is there, the same");

    // A file in the way: kept, or replaced, as asked.
    write(first + QStringLiteral("/Denim.png"), "my own icon");
    check(AssetStore::packageConflicts(one, first) == 1, "a file with other content is one conflict");
    r = AssetStore::installPackage(one, first, /*replaceExisting=*/false);
    check(r.ok && r.files == 0 && r.kept == QStringList{QStringLiteral("Denim.png")} && read(first + QStringLiteral("/Denim.png")) == "my own icon",
          "… left as it is when not told to replace, and listed");
    r = AssetStore::installPackage(one, first, /*replaceExisting=*/true);
    check(r.ok && r.files == 1 && r.kept.isEmpty() && read(first + QStringLiteral("/Denim.png")) == "denim-icon", "… replaced when told to");

    // A whole folder: under its own name, everything in it but the clutter.
    const QString all = root + QStringLiteral("/pack/fabric.zip");
    r = AssetStore::exportPackage({folder}, all);
    check(r.ok && r.files == 6, "a folder is packed with everything in it (not Thumbs.db)");
    const QString second = root + QStringLiteral("/pack/installed-all");
    QDir().mkpath(second);
    r = AssetStore::installPackage(all, second, false);
    check(r.ok && r.files == 6 && read(second + QStringLiteral("/Fabric/Canvas.psm")) == read(folder + QStringLiteral("/Canvas.psm")) &&
              read(second + QStringLiteral("/Fabric/textures/Canvas/color.png")) == "canvas-color" && !exists(second + QStringLiteral("/Fabric/Thumbs.db")),
          "… and unpacks as that folder");
    // Two assets of one folder: relative to the folder they share.
    const QString two = root + QStringLiteral("/pack/two.zip");
    r = AssetStore::exportPackage({folder + QStringLiteral("/Denim.psm"), folder + QStringLiteral("/Canvas.psm")}, two);
    check(r.ok && r.files == 6, "two assets of one folder are packed together");

    // What is not a package, and what is a hostile one.
    write(root + QStringLiteral("/pack/not.zip"), "this is not an archive");
    r = AssetStore::installPackage(root + QStringLiteral("/pack/not.zip"), second, false);
    check(!r.ok && !r.error.isEmpty() && AssetStore::packageConflicts(root + QStringLiteral("/pack/not.zip"), second) == -1, "a file that is no archive is refused");
    const QString hostile = root + QStringLiteral("/pack/hostile.zip");
    write(hostile, hostileZip());
    const QString third = root + QStringLiteral("/pack/victim/inner");
    QDir().mkpath(third);
    r = AssetStore::installPackage(hostile, third, true);
    check(!r.ok && !exists(root + QStringLiteral("/pack/victim/evil.txt")) && !exists(third + QStringLiteral("/ok.txt")),
          "an archive with an entry that climbs out of the destination is refused before anything is written");
    r = AssetStore::exportPackage({root + QStringLiteral("/pack/nothing-here.psm")}, root + QStringLiteral("/pack/empty.zip"));
    check(!r.ok && !exists(root + QStringLiteral("/pack/empty.zip")), "nothing to pack writes no package");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir    temp;
    if (!temp.isValid()) {
        std::printf("FAIL no temporary folder\n");
        return 1;
    }
    const QString root = QDir::cleanPath(temp.path());

    checkCollecting(root);
    checkFinding(root);
    checkOperations(root);
    checkPackages(root);

    std::printf("%d failed\n", failures);
    return failures;
}
