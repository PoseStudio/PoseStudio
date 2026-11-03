/**
 * @file fbximportservice.cpp
 * @brief The Qt-facing FBX character import (see fbximportservice.h).
 */

#include "fbximportservice.h"

#include "fbxfigureimporter.h"
#include "modelimportservice.h" // ImportProgress + uploadModelData (shared with the other paths)
#include "rigconformer.h"
#include "vulkanrenderer.h"

#include <QDebug>
#include <QMessageBox>
#include <QObject>

#include <cstdlib>
#include <exception>

namespace pose {

bool FbxImportService::importInto(VulkanRenderer& renderer, const QString& path, bool showProgress) {
    try {
        ModelImportService::ImportProgress progress(showProgress, QObject::tr("Importing FBX Character"),
                                                    QObject::tr("Reading FBX…"));
        FbxFigureImporter importer;
        FbxImportInfo     info;
        ModelData         data = importer.load(path.toStdString(), &info);
        progress.parsed();

        qInfo("[fbx] %s: %s; %d bones (%d skinless leaves left out), %d skinned + %d rigid meshes, %d materials; skinning %s; %.2f m tall; turned %.1f deg to face +z; "
              "put %.1f mm onto the floor; textures: %d embedded, %d files, %d missing",
              qPrintable(path), info.sourceAxes.c_str(), info.bones, info.prunedLeaves, info.skinnedMeshes, info.rigidMeshes, info.materials,
              info.skinningMethod.c_str(), static_cast<double>(info.heightMetres), static_cast<double>(info.yawDegrees),
              static_cast<double>(info.groundedMm), info.embeddedTextures, info.externalTextures, info.missingTextures);
        for (const std::string& note : info.notes) {
            qInfo("[fbx]   %s", note.c_str());
        }

        QString counts = QObject::tr(", %1 meshes, %2 bones").arg(data.meshes.size()).arg(data.bones.size());
        static const bool kNoConform = std::getenv("POSESTUDIO_FBX_NO_CONFORM") != nullptr; // A/B: the raw skeleton
        if (!data.bones.empty() && !kNoConform) {
            const RigConformReport rep = conformRigToStandard(data);
            if (rep.conformed) {
                qInfo("[fbx] conformed to the rig standard: %d bones of the body, spine %d link(s), neck %d, %d twist helpers, %d finger joints, "
                      "%d toe joints, %d without a role, %d helpers driven%s",
                      rep.bodyBones, rep.spineLinks, rep.neckLinks, rep.twistHelpers, rep.fingerJoints, rep.toeJoints, rep.unplaced,
                      rep.drivenHelpers, rep.complete ? "; complete" : "");
                for (const std::string& m : rep.missing) {
                    qWarning("[fbx]   required role missing: %s", m.c_str());
                }
                if (std::getenv("POSESTUDIO_RIG_ROLES_TRACE") != nullptr) {
                    qInfo("%s", rep.roles.c_str());
                }
                counts += rep.complete ? QObject::tr(", conformed to the rig standard")
                                       : QObject::tr(", conformed to the rig standard with %1 role(s) missing").arg(rep.missing.size());
            } else {
                qWarning("[fbx] the skeleton could not be conformed to the rig standard: imported as read");
            }
        }
        ModelImportService::uploadModelData(renderer, data, progress, "FBX ", path, counts);
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[viewport] FBX import failed:" << path << "-" << e.what();
        if (showProgress) {
            QMessageBox::warning(nullptr, QObject::tr("Import FBX Character"),
                                 QObject::tr("Could not import \"%1\".\n\n%2").arg(path, QString::fromUtf8(e.what())));
        }
        return false;
    }
}

} // namespace pose
