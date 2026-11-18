/**
 * @file hiddennames.h
 * @brief THE ONE RULE FOR WHAT A LIBRARY BROWSER DOES NOT SHOW: a file or a folder whose name
 *        begins with a dot is hidden. QtCore only, header only.
 *
 * THE STANDARD (2026-11-14). Anything in a library — a folder, an asset, a picture — is hidden
 * from the Asset Manager, the Materials Library and the Environment tab's HDRI list by giving it
 * a name that begins with "." (`.textures`, `.drafts`, `.Old Chrome.psm`). A hidden folder hides
 * everything in it. The rule is about NAMES and nothing else: no folder name is special (the
 * `textures` folders of other applications' content are ordinary folders), and no list of
 * hidden things is kept anywhere.
 *
 * It is the convention macOS and Linux already have — there Qt's directory listings leave dot
 * names out by themselves — so the rule's whole effect is that Windows, where a dot name is an
 * ordinary name, shows the same library the other two do. What the operating system itself
 * marks hidden (the Hidden attribute on Windows) stays out of the listings as it always has.
 *
 * Hidden is for the BROWSERS. A file is still found at the path that names it: a saved
 * material's textures live in a hidden `.textures` folder beside it (assets/assetstore.h) and
 * are read, moved, packed and deleted from there like any file.
 *
 * A library's ROOT is never hidden by its own name: a folder the user registered as a library
 * is shown whatever it is called (inHiddenPath looks only below the root).
 */

#ifndef HIDDENNAMES_H
#define HIDDENNAMES_H

#include <QDir>
#include <QString>
#include <QStringList>

namespace HiddenNames {

/// True when a file or folder of that NAME is hidden from the library browsers: it begins with
/// a dot (and is not the directory entries "." and "..").
inline bool isHidden(const QString& name) {
    return name.size() > 1 && name.startsWith(QLatin1Char('.')) && name != QLatin1String("..");
}

/// True when @p path, below @p root, is hidden or lies inside a hidden folder: one of its path
/// segments below the root has a hidden name. The root's own name does not count.
inline bool inHiddenPath(const QString& path, const QString& root) {
    const QString     relative = QDir(root).relativeFilePath(path);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (isHidden(part)) {
            return true;
        }
    }
    return false;
}

} // namespace HiddenNames

#endif // HIDDENNAMES_H
