/**
 * @file riggedimportservice.cpp
 * @brief The Qt-facing rigged-character import (see riggedimportservice.h).
 */

#include "riggedimportservice.h"

#include "fbxfigureimporter.h"
#include "gltffigureimporter.h"
#include "importformats.h"
#include "modelimportservice.h" // ImportProgress + uploadModelData (shared with the other paths)
#include "rigconformer.h"
#include "usdfigureimporter.h"
#include "vulkanrenderer.h"

#include <QDebug>
#include <QMessageBox>
#include <QObject>

#include <cstdlib>
#include <exception>

namespace pose {

namespace {

/// The reader a path takes, by its extension; the label the dialogs and the log use.
struct Format {
    const char* tag;      ///< The log prefix: "fbx", "gltf", "usd".
    const char* kind;     ///< The upload log's kind prefix.
    QString     title;    ///< The progress dialog's title.
    QString     reading;  ///< The progress dialog's busy label.
    QString     failed;   ///< The failure box's title.
};

Format formatOf(const QString& path) {
    if (ImportFormats::isGltfFile(path)) {
        return {"gltf", "glTF ", QObject::tr("Importing glTF Character"), QObject::tr("Reading glTF…"), QObject::tr("Import glTF Character")};
    }
    if (ImportFormats::isUsdFile(path)) {
        return {"usd", "USD ", QObject::tr("Importing USD Character"), QObject::tr("Reading USD…"), QObject::tr("Import USD Character")};
    }
    return {"fbx", "FBX ", QObject::tr("Importing FBX Character"), QObject::tr("Reading FBX…"), QObject::tr("Import FBX Character")};
}

ModelData readFile(const QString& path, RiggedImportInfo& info) {
    const std::string file = path.toStdString();
    if (ImportFormats::isGltfFile(path)) {
        return GltfFigureImporter().load(file, &info);
    }
    if (ImportFormats::isUsdFile(path)) {
        return UsdFigureImporter().load(file, &info);
    }
    return FbxFigureImporter().load(file, &info);
}

} // namespace

bool RiggedImportService::importInto(VulkanRenderer& renderer, const QString& path, bool showProgress) {
    const Format format = formatOf(path);
    try {
        ModelImportService::ImportProgress progress(showProgress, format.title, format.reading);
        RiggedImportInfo                   info;
        ModelData                          data = readFile(path, info);
        progress.parsed();

        qInfo("[%s] %s: %s; %d bones (%d skinless leaves left out), %d skinned + %d rigid meshes, %d materials; skinning %s; %.2f m tall; "
              "turned %.1f deg to face +z; put %.1f mm onto the floor; textures: %d embedded, %d files, %d missing",
              format.tag, qPrintable(path), info.sourceAxes.c_str(), info.bones, info.prunedLeaves, info.skinnedMeshes, info.rigidMeshes,
              info.materials, info.skinningMethod.c_str(), static_cast<double>(info.heightMetres), static_cast<double>(info.yawDegrees),
              static_cast<double>(info.groundedMm), info.embeddedTextures, info.externalTextures, info.missingTextures);
        for (const std::string& note : info.notes) {
            qInfo("[%s]   %s", format.tag, note.c_str());
        }

        QString counts = QObject::tr(", %1 meshes, %2 bones").arg(data.meshes.size()).arg(data.bones.size());
        static const bool kNoConform = std::getenv("POSESTUDIO_RIG_NO_CONFORM") != nullptr ||
                                       std::getenv("POSESTUDIO_FBX_NO_CONFORM") != nullptr; // A/B: the raw skeleton
        if (!data.bones.empty() && !kNoConform) {
            const RigConformReport rep = conformRigToStandard(data);
            if (rep.conformed) {
                qInfo("[%s] conformed to the rig standard: %d bones of the body, spine %d link(s), neck %d, %d twist helpers, %d finger joints, "
                      "%d toe joints, %d without a role, %d helpers driven%s",
                      format.tag, rep.bodyBones, rep.spineLinks, rep.neckLinks, rep.twistHelpers, rep.fingerJoints, rep.toeJoints, rep.unplaced,
                      rep.drivenHelpers, rep.complete ? "; complete" : "");
                qInfo("[%s] %d bone(s) named by the standard (hips, spine1, upper_arm_l, thumb1_r ...); the face rig and the helpers keep their file's names",
                      format.tag, rep.renamed);
                for (const std::string& m : rep.missing) {
                    qWarning("[%s]   required role missing: %s", format.tag, m.c_str());
                }
                for (const std::string& c : rep.collisions) {
                    qInfo("[%s]   the standard's name was not applied — %s (those bones keep their file's names)", format.tag, c.c_str());
                }
                if (std::getenv("POSESTUDIO_RIG_ROLES_TRACE") != nullptr) {
                    qInfo("%s", rep.roles.c_str());
                }
                counts += rep.complete ? QObject::tr(", conformed to the rig standard")
                                       : QObject::tr(", conformed to the rig standard with %1 role(s) missing").arg(rep.missing.size());
            } else {
                qWarning("[%s] the skeleton could not be conformed to the rig standard: imported as read", format.tag);
            }
        }
        ModelImportService::uploadModelData(renderer, data, progress, format.kind, path, counts);
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[viewport]" << format.tag << "import failed:" << path << "-" << e.what();
        if (showProgress) {
            QMessageBox::warning(nullptr, format.failed, QObject::tr("Could not import \"%1\".\n\n%2").arg(path, QString::fromUtf8(e.what())));
        }
        return false;
    }
}

} // namespace pose
