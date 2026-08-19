/**
 * @file figureimportservice.cpp
 * @brief Implementation of FigureImportService. See figureimportservice.h.
 */

#include "figureimportservice.h"

#include "aobaker.h"
#include "modeldata.h"
#include "tangentgen.h"
#include "modelimportservice.h" // decodeModelTextures (shared with the model path)
#include "import/figure/figuredata.h"
#include "import/figure/figureimporter.h"
#include "rendering/vulkanrenderer.h"

#include "preferencesmanager.h" // user-configured content roots
#include "constants.h"          // PREF_FIGURE_CONTENT_ROOTS

#include <glm/gtc/matrix_transform.hpp>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pose {

namespace {

// Walks up from the imported file to the content root — the first ancestor directory that
// contains a "data" folder (the anchor every "/data/..." and "/Runtime/..." URI resolves against).
// Returns an empty list if none is found (the importer then can't resolve the base geometry).
std::vector<std::string> detectContentRoots(const QString& path) {
    std::vector<std::string> roots;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path.toStdString()).parent_path();
    for (int i = 0; i < 16 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "data", ec)) {
            roots.push_back(dir.string());
            break;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            break; // reached the filesystem root
        }
        dir = parent;
    }
    return roots;
}

// Converts a FigureData into the renderer's ModelData: one mesh per zone, base color + diffuse map
// path, moving the geometry out of `fig`. Positions are scaled from the figure's native centimetres
// to world units (÷100), so a ~180 cm figure lands at a grid-friendly ~1.8 units.
ModelData toModelData(FigureData&& fig) {
    // cm -> world, times the character's overall scale (height). Applied to vertices AND bone
    // origins so the skeleton stays consistent with the mesh.
    const float kCmToWorld = 0.01f * fig.figureScale;
    ModelData model;
    model.meshes.reserve(fig.meshes.size());
    for (FigureMesh& zone : fig.meshes) {
        if (zone.indices.empty()) {
            continue;
        }
        MeshData mesh;
        mesh.vertices = std::move(zone.vertices);
        mesh.indices = std::move(zone.indices);
        mesh.baseVertex = std::move(zone.baseVertex); // source base index per vertex (for correctives)
        for (Vertex& v : mesh.vertices) {
            v.pos *= kCmToWorld;
        }
        mesh.baseColor = zone.material.baseColor;
        mesh.diffuse.path = zone.material.diffuseMapPath;
        mesh.opacityMask.path = zone.material.opacityMapPath;
        mesh.roughness = zone.material.roughness;
        mesh.specularWeight = zone.material.specularWeight;
        mesh.specularWeightWithMap = zone.material.specularWeightWithMap;
        mesh.metalness = zone.material.metallic;
        mesh.lobe1Roughness = zone.material.lobe1Roughness;
        mesh.lobe2Roughness = zone.material.lobe2Roughness;
        mesh.lobeRatio = zone.material.lobeRatio;
        mesh.topCoatWeight = zone.material.topCoatWeight;
        mesh.topCoatRoughness = zone.material.topCoatRoughness;
        mesh.translucencyWeight = zone.material.translucencyWeight;
        mesh.roughnessMap.path = zone.material.roughnessMapPath;
        mesh.specMask.path = zone.material.specMaskMapPath;
        mesh.translucencyMap.path = zone.material.translucencyMapPath;
        mesh.detailNormalMap.path = zone.material.detailNormalMapPath;
        mesh.detailWeight = zone.material.detailWeight;
        mesh.detailTiles = zone.material.detailTiles;
        mesh.opacity = zone.material.opacity;
        // Detail map: prefer a true tangent-space normal map (mode 1); else the grayscale bump/height
        // map (mode 2, applied via a texture-space central difference in the shader). The base figure
        // ships bump maps, so this is what gives its skin surface grain. Either way the AUTHORED
        // strength rides along — heavily-detailed skins dial e.g. Normal Map 2.5 / Bump 4, and
        // rendering them at 1x left the surface too coherent (broad sheen read as wet plastic).
        if (!zone.material.normalMapPath.empty()) {
            mesh.normal.path = zone.material.normalMapPath;
            mesh.normalMode = 1;
            mesh.normalStrength = zone.material.normalStrength;
        } else if (!zone.material.bumpMapPath.empty()) {
            mesh.normal.path = zone.material.bumpMapPath;
            mesh.normalMode = 2;
            mesh.normalStrength = zone.material.bumpStrength;
        }
        model.meshes.push_back(std::move(mesh));
    }

    // Skeleton: apply the same cm->world scale to bone origins as to the vertices, so skin matrices
    // operate in the figure's rendered space. The bind transform is deliberately translation-only
    // (bind pose reproduces the rest geometry regardless of orientation); the orientation and
    // rotation order travel separately, and the posing runtime applies pose rotations in the
    // joint's oriented frame (see Model::setBoneRotation).
    model.bones.reserve(fig.bones.size());
    for (const FigureBone& bone : fig.bones) {
        ModelBone modelBone;
        modelBone.name = bone.name;
        modelBone.parent = bone.parent;
        // Rest transform is translation-only (the joint orientation cancels at rest); the orientation
        // + rotation order travel separately so pose rotations act in the joint's true frame.
        modelBone.bindGlobal = glm::translate(glm::mat4(1.0f), bone.origin * kCmToWorld);
        modelBone.orientation = bone.orientation;
        modelBone.rotationOrder = bone.rotationOrder;
        // Rotation limits are angles (degrees), unit-independent — pass through unscaled.
        modelBone.rotationMin = bone.rotationMin;
        modelBone.rotationMax = bone.rotationMax;
        modelBone.rotationLimited = bone.rotationLimited;
        model.bones.push_back(modelBone);
    }

    // Pose correctives: their deltas are in the same native-centimetre base-vertex space as the
    // geometry, so scale them by the same cm->world factor. The formulas (driven by joint angles in
    // degrees) are unit-independent and pass through unchanged.
    model.correctives = std::move(fig.correctives);
    for (PoseCorrective& c : model.correctives) {
        for (auto& [index, delta] : c.deltas) {
            delta *= kCmToWorld;
        }
    }
    return model;
}

// --- Content-root resolution ---------------------------------------------------------------------
//
// A figure preset references its geometry/morphs/skin/UVs by root-relative URI ("/data/...#id"),
// resolved against a content root (a folder directly containing "data/"). detectContentRoots
// above finds that root by walking up from the imported file — which works for a self-contained
// library, but fails when the browsed folder holds only presets and the "data/" library lives
// elsewhere (a common content layout). To cover that, we also let the user configure/point at their
// content root(s); those are unioned with the auto-detected one and persisted for future imports.

// User-configured content roots (newline-separated in the preference), filtered to existing folders.
std::vector<std::string> configuredContentRoots() {
    std::vector<std::string> roots;
    const QString stored =
        PreferencesManager::instance().getValue(Constants::PREF_FIGURE_CONTENT_ROOTS).toString();
    const QStringList parts = stored.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString trimmed = p.trimmed();
        if (!trimmed.isEmpty() && QDir(trimmed).exists()) {
            roots.push_back(trimmed.toStdString());
        }
    }
    return roots;
}

// The ordered candidate roots for a given file: the auto-detected co-located root first (most
// authoritative for a self-contained library), then the user-configured roots. Deduplicated.
std::vector<std::string> buildContentRoots(const QString& path) {
    std::vector<std::string> roots = detectContentRoots(path);
    for (const std::string& r : configuredContentRoots()) {
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) {
            roots.push_back(r);
        }
    }
    return roots;
}

// Appends a content root to the persisted list (deduplicated, case-insensitive).
void persistContentRoot(const QString& dir) {
    const QString norm = QDir::cleanPath(dir);
    const QString stored =
        PreferencesManager::instance().getValue(Constants::PREF_FIGURE_CONTENT_ROOTS).toString();
    QStringList parts = stored.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        if (QDir::cleanPath(p.trimmed()).compare(norm, Qt::CaseInsensitive) == 0) {
            return; // already present
        }
    }
    parts.append(norm);
    PreferencesManager::instance().setValue(Constants::PREF_FIGURE_CONTENT_ROOTS,
                                            parts.join(QLatin1Char('\n')));
}

// Interactive recovery: explain that the figure's "data" folder wasn't found and offer to browse
// for it. Returns the chosen folder (persisted by the caller), or empty if the user declined.
QString promptLocateContentFolder(const QString& path) {
    QMessageBox box(QApplication::activeWindow());
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Content Folder Needed"));
    box.setText(QStringLiteral("PoseStudio couldn't find the geometry and morph data for:\n%1")
                    .arg(QFileInfo(path).fileName()));
    box.setInformativeText(QStringLiteral(
        "The folder you imported from contains the figure's presets but not its \"data\" folder "
        "(where the mesh, skeleton, morphs and skin weights live).\n\n"
        "Point PoseStudio at the figure's content folder — the folder that directly contains the "
        "\"data\" folder — and it will be remembered for future imports."));
    QPushButton* locateBtn =
        box.addButton(QStringLiteral("Locate Content Folder…"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != locateBtn) {
        return QString();
    }

    // Browse starting near the imported file (the library root is often an ancestor of the presets).
    const QString dir = QFileDialog::getExistingDirectory(
        QApplication::activeWindow(),
        QStringLiteral("Select the Content Folder (the one containing \"data\")"),
        QFileInfo(path).absolutePath());
    if (dir.isEmpty()) {
        return QString();
    }
    if (!QDir(dir).exists(QStringLiteral("data"))) {
        // Not fatal — accept it anyway, but warn so a still-failing retry isn't a mystery.
        QMessageBox::information(
            QApplication::activeWindow(), QStringLiteral("No \"data\" Folder"),
            QStringLiteral("The selected folder doesn't directly contain a \"data\" folder, so the "
                           "figure's geometry may still not be found. It has been added anyway — "
                           "pick the folder that directly contains \"data\" if the import fails."));
    }
    return dir;
}

// Final failure message, phrased to match the actual cause.
void showImportFailureMessage(const QString& path, const QString& detail, bool notAFigure,
                              bool missingContent) {
    QString body;
    if (notAFigure) {
        body = QStringLiteral("Could not import a character figure from:\n%1\n\nThis file may be a "
                              "pose, material, or other preset rather than a figure.\n\nDetails: %2")
                   .arg(QFileInfo(path).fileName(), detail);
    } else if (missingContent) {
        body = QStringLiteral("Could not import a character figure from:\n%1\n\nThe figure's geometry "
                              "and morph data (its \"data\" folder) couldn't be found. Import again "
                              "and choose \"Locate Content Folder…\" to point PoseStudio at the folder "
                              "that contains \"data\".\n\nDetails: %2")
                   .arg(QFileInfo(path).fileName(), detail);
    } else {
        body = QStringLiteral("Could not import a character figure from:\n%1\n\nDetails: %2")
                   .arg(QFileInfo(path).fileName(), detail);
    }
    QMessageBox::warning(QApplication::activeWindow(), QStringLiteral("Import Failed"), body);
}

// One import attempt with a fixed set of content roots. Throws std::runtime_error on failure; drives
// the modal staged progress dialog when showProgress is set.
void runFigureImport(VulkanRenderer& renderer, const QString& path,
                     const std::vector<std::string>& roots, bool showProgress) {
    std::unique_ptr<QProgressDialog> progress;
    if (showProgress) {
        progress = std::make_unique<QProgressDialog>(QStringLiteral("Reading figure…"),
                                                     QString() /*no cancel*/, 0, 0,
                                                     QApplication::activeWindow());
        progress->setWindowTitle(QStringLiteral("Importing Figure"));
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(0);
        progress->setValue(0);
        progress->show();
        QApplication::processEvents();
    }

    QElapsedTimer timer;
    timer.start();

    if (roots.empty()) {
        qWarning() << "[viewport] Figure import: no content root (a folder containing 'data') "
                      "auto-detected above"
                   << path << "and none configured";
    }

    FigureImporter importer;
    FigureData figure = importer.load(path.toStdString(), roots);
    const qint64 msParse = timer.elapsed();

    ModelData data = toModelData(std::move(figure));
    const int meshCount = static_cast<int>(data.meshes.size());
    const int correctiveCount = static_cast<int>(data.correctives.size());
    const int total = 5; // parse, decode, bake, upload, done
    int step = 0;
    if (progress) {
        progress->setRange(0, total);
        progress->setValue(++step); // parse complete
    }

    // Decode every unique texture once — figure zones share atlas files, so decodes are shared
    // across meshes and run across cores (see ModelImportService::decodeModelTextures).
    if (progress) {
        progress->setLabelText(QStringLiteral("Decoding textures…"));
        progress->setValue(++step);
    }
    ModelImportService::decodeModelTextures(data);
    const qint64 msDecode = timer.elapsed();

    // Bake per-vertex ambient occlusion + UV tangents on the final render mesh (post-morph,
    // post-subdivision, all zones together — the AO parallel across cores). See aobaker.h /
    // tangentgen.h.
    if (progress) {
        progress->setLabelText(QStringLiteral("Baking ambient occlusion…"));
        progress->setValue(++step);
    }
    bakeVertexAO(data);
    computeTangents(data);

    if (progress) {
        progress->setLabelText(QStringLiteral("Uploading to GPU…"));
        progress->setValue(++step);
    }
    renderer.addModel(data);
    const qint64 msUpload = timer.elapsed();
    if (progress) {
        progress->setValue(total);
    }

    qDebug().nospace() << "[viewport] imported figure " << path << " in " << msUpload << "ms (parse "
                       << msParse << ", decode " << (msDecode - msParse) << ", upload "
                       << (msUpload - msDecode) << "), " << meshCount << " zones, " << correctiveCount
                       << " pose correctives";
}

} // namespace

bool FigureImportService::importInto(VulkanRenderer& renderer, const QString& path,
                                     bool showProgress) {
    // Candidate content roots: auto-detected from the file's location, plus any the user has
    // configured or located before. Rebuilt after a successful "locate" so the retry sees the new root.
    std::vector<std::string> roots = buildContentRoots(path);

    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            runFigureImport(renderer, path, roots, showProgress); // throws on failure
            return true;
        } catch (const std::exception& e) {
            const QString detail = QString::fromUtf8(e.what());
            qWarning() << "[viewport] Figure import failed:" << path << "-" << e.what();
            if (!showProgress) {
                return false; // startup/command-line drain: no dialogs (avoids re-entering expose)
            }

            // Classify the failure so we respond usefully. A `.duf` that isn't a figure (pose/material/
            // light presets share the extension and all appear in the asset grid) throws "no geometry
            // reference"; a real figure whose "data/" library isn't on any known root throws a resolve
            // error (or we never found a root at all) — that's the recoverable case.
            const bool notAFigure = detail.contains(QStringLiteral("no geometry reference"),
                                                    Qt::CaseInsensitive);
            const bool missingContent =
                !notAFigure &&
                (roots.empty() || detail.contains(QStringLiteral("resolve"), Qt::CaseInsensitive));

            if (missingContent && attempt == 0) {
                const QString located = promptLocateContentFolder(path);
                if (!located.isEmpty()) {
                    persistContentRoot(located);
                    roots = buildContentRoots(path); // fold in the newly located root, then retry
                    continue;
                }
                // User declined — fall through to the explanatory message below.
            }
            showImportFailureMessage(path, detail, notAFigure, missingContent);
            return false;
        }
    }
    return false;
}

} // namespace pose
