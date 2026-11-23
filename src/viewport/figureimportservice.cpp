// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file figureimportservice.cpp
 * @brief Implementation of FigureImportService. See figureimportservice.h.
 */

#include "figureimportservice.h"

#include "modeldata.h"
#include "modelimportservice.h" // ImportProgress + uploadModelData (shared with the model path)
#include "import/figure/figuredata.h"
#include "import/figure/figureimporter.h"
#include "rendering/vulkanrenderer.h"

#include "preferencesmanager.h" // user-configured content roots
#include "shaderpresetfile.h"  // isShaderPresetFile: the failure message of a preset that is a material
#include "constants.h"          // PREF_FIGURE_CONTENT_ROOTS

#include <limits>
#include <glm/gtc/matrix_transform.hpp>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
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

// The content root the imported file itself lies under — the first ancestor directory that
// contains a "data" folder (the anchor every "/data/..." and "/Runtime/..." URI resolves against)
// — as a list of none or one. The walk is the importer's own (FigureImporter::contentRootOf), so
// the application and the headless check tool cannot disagree about where a library begins.
// Empty when there is none (the importer then cannot resolve the base geometry, unless a
// configured root does: see below).
std::vector<std::string> detectContentRoots(const QString& path) {
    const std::string root = FigureImporter::contentRootOf(path.toStdString());
    return root.empty() ? std::vector<std::string>{} : std::vector<std::string>{root};
}

// Copies one zone's material into the renderer's mesh: every value, and every map as the path
// the decode layer will read. Which detail map drives the mesh's normal slot is decided here.
void copyZoneMaterial(const FigureMaterial& material, MeshData& mesh) {
    mesh.baseColor = material.baseColor;
    mesh.diffuse.path = material.diffuseMapPath;
    mesh.opacityMask.path = material.opacityMapPath;
    mesh.roughness = material.roughness;
    mesh.specularWeight = material.specularWeight;
    mesh.specularWeightWithMap = material.specularWeightWithMap;
    mesh.metalness = material.metallic;
    mesh.lobe1Roughness = material.lobe1Roughness;
    mesh.lobe2Roughness = material.lobe2Roughness;
    mesh.lobeRatio = material.lobeRatio;
    mesh.topCoatWeight = material.topCoatWeight;
    mesh.topCoatEnabled = material.topCoatEnabled;
    mesh.topCoatRoughness = material.topCoatRoughness;
    mesh.translucencyWeight = material.translucencyWeight;
    mesh.roughnessMap.path = material.roughnessMapPath;
    mesh.specMask.path = material.specMaskMapPath;
    mesh.translucencyMap.path = material.translucencyMapPath;
    mesh.translucencyWeightMap.path = material.translucencyWeightMapPath;
    mesh.detailNormalMap.path = material.detailNormalMapPath;
    mesh.detailWeight = material.detailWeight;
    mesh.detailTiles = material.detailTiles;
    mesh.detailTilesV = material.detailTilesV;
    mesh.detailOffset = material.detailOffset;
    mesh.detailRoughness = material.detailRoughness;
    mesh.detailWeightMap.path = material.detailWeightMapPath;
    mesh.detailRoughnessMap.path = material.detailRoughnessMapPath;
    mesh.opacity = material.opacity;
    // The canonical material's further channels (figuredata.h).
    mesh.emissionColor = material.emissionColor;
    mesh.emission = material.emission;
    mesh.emissionMap.path = material.emissionMapPath;
    mesh.ior = material.ior;
    mesh.transmission = material.transmission;
    mesh.transmissionColor = material.transmissionColor;
    mesh.transmissionDepth = material.transmissionDepth;
    mesh.transmissionRoughness = material.transmissionRoughness;
    mesh.thinWalled = material.thinWalled;
    mesh.subsurfaceColor = material.subsurfaceColor;
    mesh.subsurfaceRadiusColor = material.subsurfaceRadiusColor;
    mesh.subsurfaceRadius = material.subsurfaceRadius;
    mesh.subsurfaceAnisotropy = material.subsurfaceAnisotropy;
    mesh.anisotropy = material.anisotropy;
    mesh.anisotropyRotation = material.anisotropyRotation;
    mesh.diffuseRoughness = material.diffuseRoughness;
    mesh.diffuseRoughnessMap.path = material.diffuseRoughnessMapPath;
    mesh.coatColor = material.coatColor;
    mesh.coatIor = material.coatIor;
    mesh.coatWeightMap.path = material.coatWeightMapPath;
    mesh.coatColorMap.path = material.coatColorMapPath;
    mesh.coatRoughnessMap.path = material.coatRoughnessMapPath;
    mesh.coatReflectivityMap.path = material.coatReflectivityMapPath;
    mesh.coatBumpMap.path = material.coatBumpMapPath;
    mesh.coatBumpStrength = material.coatBumpStrength;
    mesh.specularColorSlotMap.path = material.specularColorMapPath;
    mesh.displacementMap.path = material.displacementMapPath;
    mesh.transmissionColorMap.path = material.transmissionColorMapPath;
    mesh.transmissionWeightMap.path = material.transmissionWeightMapPath;
    mesh.coatAnisotropy = material.coatAnisotropy;
    mesh.coatAnisotropyRotation = material.coatAnisotropyRotation;
    mesh.coatAnisotropyMap.path = material.coatAnisotropyMapPath;
    mesh.coatAnisotropyRotationMap.path = material.coatAnisotropyRotationMapPath;
    mesh.sheen = material.sheen;
    mesh.sheenColor = material.sheenColor;
    mesh.sheenRoughness = material.sheenRoughness;
    mesh.sheenWeightMap.path = material.sheenWeightMapPath;
    mesh.sheenColorMap.path = material.sheenColorMapPath;
    mesh.sheenRoughnessMap.path = material.sheenRoughnessMapPath;
    mesh.thinFilm = material.thinFilm;
    mesh.thinFilmThickness = material.thinFilmThickness;
    mesh.thinFilmIor = material.thinFilmIor;
    mesh.thinFilmThicknessMap.path = material.thinFilmThicknessMapPath;
    mesh.displacementStrength = material.displacementStrength;
    mesh.displacementMidLevel = material.displacementMidLevel;
    mesh.displacementSubdiv = material.displacementSubdiv;
    mesh.anisotropyMap.path = material.anisotropyMapPath;
    mesh.anisotropyRotationMap.path = material.anisotropyRotationMapPath;
    mesh.lobe2RoughnessMap.path = material.lobe2RoughnessMapPath;
    mesh.lobeRatioMap.path = material.lobeRatioMapPath;
    mesh.uvTiling = material.uvTiling;
    mesh.uvOffset = material.uvOffset;
    // Detail map: prefer a true tangent-space normal map (mode 1); else the grayscale bump/height
    // map (mode 2, applied via a texture-space central difference in the shader). The base figure
    // ships bump maps, so this is what gives its skin surface grain. Either way the AUTHORED
    // strength rides along — heavily-detailed skins dial e.g. Normal Map 2.5 / Bump 4, and
    // rendering them at 1x left the surface too coherent (broad sheen read as wet plastic).
    if (!material.normalMapPath.empty()) {
        mesh.normal.path = material.normalMapPath;
        mesh.normalMode = 1;
        mesh.normalStrength = material.normalStrength;
        // A bump map BESIDE the normal map rides its own slot (mesh.frag applies both): until
        // 2026-11-09 a skin with both lost its bump's grain.
        mesh.bumpMap.path = material.bumpMapPath;
        mesh.bumpStrength = material.bumpStrength;
    } else if (!material.bumpMapPath.empty()) {
        mesh.normal.path = material.bumpMapPath;
        mesh.normalMode = 2;
        mesh.normalStrength = material.bumpStrength;
    }
}

// Converts a FigureData into the renderer's ModelData, moving the geometry out of `fig`: one mesh
// per material zone with its whole material (every value, every map's path — copyZoneMaterial), the
// skeleton, and the pose correctives. Positions, bone origins and corrective deltas are scaled
// from the figure's native centimetres to world units (÷100, times the character's own scale), so
// a ~180 cm figure lands at a grid-friendly ~1.8 units; and the figure is put ON THE FLOOR, mesh
// and skeleton together (see the end of the function).
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
        mesh.name = zone.materialZone; // the Materials tab's zone name
        mesh.vertices = std::move(zone.vertices);
        mesh.indices = std::move(zone.indices);
        mesh.baseVertex = std::move(zone.baseVertex); // source base index per vertex (for correctives)
        for (Vertex& v : mesh.vertices) {
            v.pos *= kCmToWorld;
        }
        copyZoneMaterial(zone.material, mesh);
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

    // THE FIGURE STANDS ON THE FLOOR AT REST. A base figure's soles are at y = 0, and everything
    // that reasons about the floor from the bind pose — the posing rig's foot contacts (a joint's
    // standing height IS its bind height), the ball of the foot (found by its bind height against
    // the ankle's), what counts as a foot at all — takes that for granted. A custom CHARACTER
    // need not oblige: a morph that shortens the legs commonly does it by lifting the FEET, not
    // by lowering the hips, and the character then rests a few centimetres in the air (3.9 on the
    // one that showed it). She hovered over the grid; her toes, 5.8cm up against an ankle at
    // 10.3, no longer read as the ball of a foot, so her ankles were pinned rigidly — no heel
    // lift, no pivot — and she could not kneel, crouch deep, stride or walk by her hips (23 of
    // the IK harness's 114 phases failed on her skeleton); and the knees, whose floor is the
    // world's, had 4cm further to go than the feet they were kneeling beside. So the figure is
    // put on the floor here, mesh and skeleton together: the lowest point of its skin at y = 0.
    // (Only when it is more than a millimetre off: a figure that already stands there is left
    // bit for bit as it was.)
    float lowest = std::numeric_limits<float>::max();
    for (const MeshData& mesh : model.meshes) {
        for (const Vertex& v : mesh.vertices) {
            lowest = std::min(lowest, v.pos.y);
        }
    }
    if (lowest < std::numeric_limits<float>::max() && std::abs(lowest) > 0.001f) {
        for (MeshData& mesh : model.meshes) {
            for (Vertex& v : mesh.vertices) {
                v.pos.y -= lowest;
            }
        }
        for (ModelBone& bone : model.bones) {
            bone.bindGlobal[3][1] -= lowest;
        }
        qInfo("[figure] rests %.1f mm %s the floor as authored: put on it (mesh and skeleton together)",
              std::abs(lowest) * 1000.0f, lowest > 0.0f ? "above" : "below");
    }
    if (fig.zonesOnOwnUvSet > 0) {
        qInfo("[figure] %d material zones read their maps through the UV set their material names, not the geometry's default",
              fig.zonesOnOwnUvSet);
    }
    for (const std::string& skipped : fig.skipped) {
        qWarning("[figure] left out: %s", skipped.c_str());
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
    box.setWindowTitle(QObject::tr("Content Folder Needed"));
    box.setText(QObject::tr("PoseStudio couldn't find the geometry and morph data for:\n%1")
                    .arg(QFileInfo(path).fileName()));
    box.setInformativeText(
        QObject::tr("The folder you imported from contains the figure's presets but not its \"data\" folder "
                    "(where the mesh, skeleton, morphs and skin weights live).\n\n"
                    "Point PoseStudio at the figure's content folder — the folder that directly contains the "
                    "\"data\" folder — and it will be remembered for future imports."));
    QPushButton* locateBtn =
        box.addButton(QObject::tr("Locate Content Folder…"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != locateBtn) {
        return QString();
    }

    // Browse starting near the imported file (the library root is often an ancestor of the presets).
    const QString dir = QFileDialog::getExistingDirectory(
        QApplication::activeWindow(),
        QObject::tr("Select the Content Folder (the one containing \"data\")"),
        QFileInfo(path).absolutePath());
    if (dir.isEmpty()) {
        return QString();
    }
    if (!QDir(dir).exists(QStringLiteral("data"))) {
        // Not fatal — accept it anyway, but warn so a still-failing retry isn't a mystery.
        QMessageBox::information(
            QApplication::activeWindow(), QObject::tr("No \"data\" Folder"),
            QObject::tr("The selected folder doesn't directly contain a \"data\" folder, so the "
                        "figure's geometry may still not be found. It has been added anyway — "
                        "pick the folder that directly contains \"data\" if the import fails."));
    }
    return dir;
}

// Final failure message, phrased to match the actual cause.
void showImportFailureMessage(const QString& path, const QString& detail, bool notAFigure,
                              bool missingContent) {
    QString body;
    if (isShaderPresetFile(path.toStdString())) {
        body = QObject::tr("\"%1\" is a shader preset: a material, not a figure.\n\nTo use it, select an object, open the "
                           "Materials tab and choose Import - or double-click the preset in a library.")
                   .arg(QFileInfo(path).fileName());
    } else if (notAFigure) {
        body = QObject::tr("Could not import a character figure from:\n%1\n\nThis file may be a "
                           "pose, material, or other preset rather than a figure.\n\nDetails: %2")
            .arg(QFileInfo(path).fileName(), detail);
    } else if (missingContent) {
        body = QObject::tr("Could not import a character figure from:\n%1\n\nThe figure's geometry "
                           "and morph data (its \"data\" folder) couldn't be found. Import again "
                           "and choose \"Locate Content Folder…\" to point PoseStudio at the folder "
                           "that contains \"data\".\n\nDetails: %2")
            .arg(QFileInfo(path).fileName(), detail);
    } else {
        body = QObject::tr("Could not import a character figure from:\n%1\n\nDetails: %2")
            .arg(QFileInfo(path).fileName(), detail);
    }
    QMessageBox::warning(QApplication::activeWindow(), QObject::tr("Import Failed"), body);
}

// What became of the character's dialed shape, in the log: the morphs that were applied, and — a
// warning — the ones that could not be, each with its reason (its file is not under any content
// folder; its file does not define it; it was made for another geometry). A character that comes
// out as the base figure, or with a base head on her own body, then says which file is missing.
void logShape(const ShapeReport& shape) {
    constexpr int kListed = 12;
    if (!shape.applied.empty()) {
        QStringList names;
        for (const ShapeReport::Morph& morph : shape.applied) {
            if (names.size() == kListed) {
                names << QStringLiteral("and %1 more").arg(static_cast<int>(shape.applied.size()) - kListed);
                break;
            }
            names << QStringLiteral("%1 %2").arg(QString::fromStdString(morph.id)).arg(morph.weight, 0, 'f', 2);
        }
        qInfo().noquote() << QStringLiteral("[figure] shape: %1 morphs applied (%2); %3 joints follow")
                                 .arg(shape.applied.size())
                                 .arg(names.join(QStringLiteral(", ")))
                                 .arg(shape.jointsMoved);
    }
    for (const std::string& missing : shape.missing) {
        qWarning().noquote() << "[figure] shape: NOT applied —" << QString::fromStdString(missing);
    }
}

// One import attempt with a fixed set of content roots: roots -> FigureImporter::load ->
// toModelData -> the shared post-parse pipeline (ModelImportService::uploadModelData: texture
// decode, AO + tangents, GPU upload, timing log). Throws std::runtime_error on failure; drives
// the modal staged progress dialog when showProgress is set.
void runFigureImport(VulkanRenderer& renderer, const QString& path,
                     const std::vector<std::string>& roots, bool showProgress) {
    ImportProgress progress(showProgress, QObject::tr("Importing Figure"), QObject::tr("Reading figure…"));

    if (roots.empty()) {
        qWarning() << "[viewport] Figure import: no content root (a folder containing 'data') "
                      "auto-detected above"
                   << path << "and none configured";
    }

    FigureImporter importer;
    FigureData     figure = importer.load(path.toStdString(), roots);
    progress.parsed();
    logShape(figure.shape);

    ModelData data = toModelData(std::move(figure));
    // (A suffix of the import's LOG line, not interface text: never translated.)
    const QString counts = QStringLiteral(", %1 zones, %2 pose correctives")
                               .arg(data.meshes.size())
                               .arg(data.correctives.size());
    ModelImportService::uploadModelData(renderer, data, progress, "figure ", path, counts);
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
