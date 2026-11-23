// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file importformats.h
 * @brief The one place that says which file extensions take which import route.
 *
 * Four routes exist: a static model (ModelImportService: `.obj`), a Gaussian splat scene
 * (SplatImportService: `.ply` / `.splat` / `.spz`), a native figure preset
 * (FigureImportService: `.duf` / `.dsf`), and a rigged character in an interchange format
 * (RiggedImportService: `.fbx`, `.glb` / `.gltf` / `.vrm`, `.usd` / `.usda` / `.usdc` /
 * `.usdz`), the last two sharing the "figure" entry points (ViewportWidget::importFigure, the
 * Asset Manager's importFigureRequested, the command line). The File → Import dialogs' filters,
 * the Asset Manager's double-click, the command line and the viewport's dispatch all read
 * these, so a new format is added here once. Each family's extensions are listed ONCE — the
 * `…Extensions()` functions — and both the `is…File()` tests and the dialogs' filters
 * (dialogFilter) are made from those lists.
 */

#ifndef IMPORTFORMATS_H
#define IMPORTFORMATS_H

#include <QString>
#include <QStringList>

namespace pose::ImportFormats {

/// True when @p path ends in one of @p extensions (lowercase, with the dot), whatever its case.
inline bool hasExtension(const QString& path, const QStringList& extensions) {
    for (const QString& ext : extensions) {
        if (path.endsWith(ext, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/// A file dialog's filter for @p extensions under the name @p name:
/// ("glTF Files", {".glb", ".gltf"}) -> "glTF Files (*.glb *.gltf)".
inline QString dialogFilter(const QString& name, const QStringList& extensions) {
    QStringList patterns;
    for (const QString& ext : extensions) {
        patterns << QLatin1Char('*') + ext;
    }
    return QStringLiteral("%1 (%2)").arg(name, patterns.join(QLatin1Char(' ')));
}

/// A static model file: Wavefront OBJ.
inline const QStringList& modelExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".obj")};
    return kExtensions;
}
inline bool isModelFile(const QString& path) {
    return hasExtension(path, modelExtensions());
}

/// A native figure preset or data file (`.duf` / `.dsf`).
inline const QStringList& nativeFigureExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".duf"), QStringLiteral(".dsf")};
    return kExtensions;
}
inline bool isNativeFigureFile(const QString& path) {
    return hasExtension(path, nativeFigureExtensions());
}

/// FBX (`.fbx`): among the rigged formats the one with no test of its own — the rigged import
/// service takes whatever is neither glTF nor USD for it.
inline const QStringList& fbxExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".fbx")};
    return kExtensions;
}

/// glTF, binary or JSON (`.glb` / `.gltf`) …
inline const QStringList& gltfExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".glb"), QStringLiteral(".gltf")};
    return kExtensions;
}
/// … and a VRM avatar (`.vrm`): a GLB with a humanoid map, read by the glTF reader.
inline const QStringList& vrmExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".vrm")};
    return kExtensions;
}
/// What the glTF reader reads: glTF itself, or a VRM avatar.
inline bool isGltfFile(const QString& path) {
    return hasExtension(path, gltfExtensions()) || hasExtension(path, vrmExtensions());
}

/// USD in any of its encodings (`.usd` / `.usda` / `.usdc`) or a USDZ package.
inline const QStringList& usdExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".usd"), QStringLiteral(".usda"),
                                            QStringLiteral(".usdc"), QStringLiteral(".usdz")};
    return kExtensions;
}
inline bool isUsdFile(const QString& path) {
    return hasExtension(path, usdExtensions());
}

/// The extensions of the rigged-character interchange formats, lowercase with the dot.
inline const QStringList& riggedExtensions() {
    static const QStringList kExtensions = fbxExtensions() + gltfExtensions() + vrmExtensions() + usdExtensions();
    return kExtensions;
}

/// A rigged character (or a plain mesh) in an interchange format: FBX, glTF / GLB / VRM, USD.
inline bool isRiggedFile(const QString& path) {
    return hasExtension(path, riggedExtensions());
}

/// A Gaussian splat scene: the trained `.ply`, the compact `.splat` or the compressed `.spz`
/// (SplatImportService).
inline const QStringList& splatExtensions() {
    static const QStringList kExtensions = {QStringLiteral(".ply"), QStringLiteral(".splat"), QStringLiteral(".spz")};
    return kExtensions;
}
inline bool isSplatFile(const QString& path) {
    return hasExtension(path, splatExtensions());
}

/// The extension of a saved material (scene/materialfile.h), lowercase with its dot.
inline QString materialFileExtension() {
    return QStringLiteral(".psm");
}

/// A saved material (`.psm`): not imported — APPLIED to the zone the Materials tab shows.
inline bool isMaterialFile(const QString& path) {
    return path.endsWith(materialFileExtension(), Qt::CaseInsensitive);
}

/// A MaterialX document (`.mtlx`): its materials are read (scene/materialxfile.h) and one is
/// APPLIED to the zone the Materials tab shows, as a saved material is.
inline bool isMaterialXFile(const QString& path) {
    return path.endsWith(QStringLiteral(".mtlx"), Qt::CaseInsensitive);
}

/// A file that is applied to a zone and not imported: a saved material or a MaterialX document.
inline bool isAppliedMaterialFile(const QString& path) {
    return isMaterialFile(path) || isMaterialXFile(path);
}

/// Anything the figure route imports: a native figure or a rigged interchange file.
inline bool isFigureRouteFile(const QString& path) {
    return isNativeFigureFile(path) || isRiggedFile(path);
}

} // namespace pose::ImportFormats

#endif // IMPORTFORMATS_H
