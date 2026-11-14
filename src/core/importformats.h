/**
 * @file importformats.h
 * @brief The one place that says which file extensions take which import route.
 *
 * Four routes exist: a static model (ModelImportService: `.obj`), a Gaussian splat scene
 * (SplatImportService: `.ply` / `.splat` / `.spz`), a native figure preset
 * (FigureImportService: `.duf` / `.dsf`), and a rigged character in an interchange format
 * (RiggedImportService: `.fbx`, `.glb` / `.gltf`, `.usd` / `.usda` / `.usdc` / `.usdz`), the
 * last two sharing the "figure" entry points (ViewportWidget::importFigure, the Asset Manager's
 * importFigureRequested, the command line). The menu, the Asset Manager's double-click, the
 * command line and the viewport's dispatch all read these, so a new format is added here once.
 */

#ifndef IMPORTFORMATS_H
#define IMPORTFORMATS_H

#include <QString>
#include <QStringList>

namespace pose::ImportFormats {

/// A static model file: Wavefront OBJ.
inline bool isModelFile(const QString& path) {
    return path.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive);
}

/// A native figure preset or data file (`.duf` / `.dsf`).
inline bool isNativeFigureFile(const QString& path) {
    return path.endsWith(QStringLiteral(".duf"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".dsf"), Qt::CaseInsensitive);
}

/// The extensions of the rigged-character interchange formats, lowercase with the dot.
inline const QStringList& riggedExtensions() {
    static const QStringList kExtensions = {
        QStringLiteral(".fbx"),
        QStringLiteral(".glb"), QStringLiteral(".gltf"), QStringLiteral(".vrm"),
        QStringLiteral(".usd"), QStringLiteral(".usda"), QStringLiteral(".usdc"), QStringLiteral(".usdz"),
    };
    return kExtensions;
}

/// A rigged character (or a plain mesh) in an interchange format: FBX, glTF / GLB / VRM, USD.
inline bool isRiggedFile(const QString& path) {
    for (const QString& ext : riggedExtensions()) {
        if (path.endsWith(ext, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/// A Gaussian splat scene: the trained `.ply`, the compact `.splat` or the compressed `.spz`
/// (SplatImportService).
inline bool isSplatFile(const QString& path) {
    return path.endsWith(QStringLiteral(".ply"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".splat"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".spz"), Qt::CaseInsensitive);
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

/// FBX (`.fbx`).
inline bool isFbxFile(const QString& path) {
    return path.endsWith(QStringLiteral(".fbx"), Qt::CaseInsensitive);
}

/// glTF, binary or JSON (`.glb` / `.gltf`), or a VRM avatar (`.vrm`: a GLB with a humanoid map).
inline bool isGltfFile(const QString& path) {
    return path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".gltf"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".vrm"), Qt::CaseInsensitive);
}

/// USD in any of its encodings (`.usd` / `.usda` / `.usdc`) or a USDZ package.
inline bool isUsdFile(const QString& path) {
    return path.endsWith(QStringLiteral(".usd"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".usda"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".usdc"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".usdz"), Qt::CaseInsensitive);
}

} // namespace pose::ImportFormats

#endif // IMPORTFORMATS_H
