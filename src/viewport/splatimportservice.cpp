/**
 * @file splatimportservice.cpp
 * @brief Implements the Gaussian splat import. See splatimportservice.h.
 */

#include "splatimportservice.h"

#include "model.h"
#include "modelimportservice.h" // ImportProgress
#include "scene.h"
#include "splatimporter.h"
#include "vulkanrenderer.h"

#include <QDebug>
#include <QFileInfo>
#include <QMessageBox>
#include <QObject>

#include <exception>

namespace pose {

bool SplatImportService::importInto(VulkanRenderer& renderer, const QString& path, bool showProgress) {
    ModelImportService::ImportProgress progress(showProgress, QObject::tr("Import Gaussian Splat"),
                                                QObject::tr("Reading the splats…"));
    try {
        SplatData data = loadSplatFile(path.toStdString());
        progress.parsed();
        if (data.count() == 0) {
            throw std::runtime_error("the file holds no splats");
        }
        progress.phase(QObject::tr("Uploading to the GPU"));
        // The capture's frame is y-down (the reference implementation's camera convention):
        // turned over about x — in the DATA, so the model transform starts at the identity and
        // the Transform tab reads 0 for this orientation — the scene stands the way its
        // photographs did. The user turns, tips, moves and scales it from there.
        turnOverYDown(data);
        renderer.addSplatModel(data, glm::mat4(1.0f));
        Scene& scene = renderer.scene();
        scene.modelAt(scene.modelCount() - 1)->setSourcePath(path.toStdString());
        progress.done();
        qInfo().noquote() << QStringLiteral("[splat] %1: %2 splats (%3), spherical harmonics degree %4; read %5 ms, %6 ms in all")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(static_cast<qulonglong>(data.count()))
                                 .arg(QString::fromStdString(data.format))
                                 .arg(data.shDegree)
                                 .arg(progress.parseMs())
                                 .arg(progress.elapsedMs());
        return true;
    } catch (const std::exception& e) {
        qWarning().noquote() << QStringLiteral("[splat] import failed: %1").arg(QString::fromUtf8(e.what()));
        if (showProgress) {
            QMessageBox::warning(nullptr, QObject::tr("Import Gaussian Splat"),
                                 QObject::tr("The splat scene could not be imported.\n\n%1").arg(QString::fromUtf8(e.what())));
        }
        return false;
    }
}

} // namespace pose
