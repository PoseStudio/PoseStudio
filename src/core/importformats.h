/**
 * @file importformats.h
 * @brief The one place that says which file extensions take which import route.
 *
 * Three routes exist: a static model (ModelImportService: `.obj`), a native figure preset
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
        QStringLiteral(".glb"), QStringLiteral(".gltf"),
        QStringLiteral(".usd"), QStringLiteral(".usda"), QStringLiteral(".usdc"), QStringLiteral(".usdz"),
    };
    return kExtensions;
}

/// A rigged character (or a plain mesh) in an interchange format: FBX, glTF / GLB, USD.
inline bool isRiggedFile(const QString& path) {
    for (const QString& ext : riggedExtensions()) {
        if (path.endsWith(ext, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/// Anything the figure route imports: a native figure or a rigged interchange file.
inline bool isFigureRouteFile(const QString& path) {
    return isNativeFigureFile(path) || isRiggedFile(path);
}

/// FBX (`.fbx`).
inline bool isFbxFile(const QString& path) {
    return path.endsWith(QStringLiteral(".fbx"), Qt::CaseInsensitive);
}

/// glTF, binary or JSON (`.glb` / `.gltf`).
inline bool isGltfFile(const QString& path) {
    return path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".gltf"), Qt::CaseInsensitive);
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
