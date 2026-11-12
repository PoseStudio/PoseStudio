/**
 * @file renderenginelibrary.cpp
 * @brief Loading a render engine's library. See renderenginelibrary.h.
 */

#include "renderenginelibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStandardPaths>

namespace pose {

namespace {

const char* const kLibraryBaseName = "PoseStudioCycles";

/// The engine library's file in @p directory under this platform's naming, or "".
QString libraryFile(const QString& directory) {
    const QDir dir(directory);
    for (const QString& name : dir.entryList(QDir::Files)) {
        if (name.contains(QLatin1String(kLibraryBaseName)) && QLibrary::isLibrary(name)) {
            return dir.absoluteFilePath(name);
        }
    }
    return QString();
}

} // namespace

QString RenderEngineLibrary::cyclesDirectory() {
    const QString override = qEnvironmentVariable("POSESTUDIO_CYCLES_DIR");
    if (!override.isEmpty()) {
        return QDir::cleanPath(override);
    }
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("engines/cycles"));
}

bool RenderEngineLibrary::present(const QString& directory) {
    return !libraryFile(directory).isEmpty();
}

std::unique_ptr<RenderEngineLibrary> RenderEngineLibrary::load(const QString& directory, QString* error) {
    const auto fail = [&](const QString& why) -> std::unique_ptr<RenderEngineLibrary> {
        if (error != nullptr) {
            *error = why;
        }
        return nullptr;
    };
    const QString file = libraryFile(directory);
    if (file.isEmpty()) {
        return fail(QCoreApplication::translate("RenderEngineLibrary", "No render engine is installed in %1.")
                        .arg(QDir::toNativeSeparators(directory)));
    }

    // The engine's own dependencies sit beside it, and the system's loader looks for a library's
    // dependencies in the process's search path — not in the folder the library came from: that
    // folder is put first on PATH for the load. (Where the loader does look beside the library —
    // an rpath of $ORIGIN — this changes nothing.)
    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", QDir::toNativeSeparators(QFileInfo(file).absolutePath()).toLocal8Bit() +
                        QByteArray(1, QDir::listSeparator().toLatin1()) + oldPath);
    QLibrary library(file);
    library.setLoadHints(QLibrary::PreventUnloadHint);
    const bool loaded = library.load();
    const QString loadError = library.errorString();
    const QFunctionPointer entry = loaded ? library.resolve(POSESTUDIO_RENDER_ENGINE_ENTRY) : nullptr;
    qputenv("PATH", oldPath);
    if (!loaded) {
        return fail(QCoreApplication::translate("RenderEngineLibrary", "The render engine could not be loaded: %1")
                        .arg(loadError));
    }
    if (entry == nullptr) {
        return fail(QCoreApplication::translate("RenderEngineLibrary",
                                                "The file %1 is not a PoseStudio render engine.")
                        .arg(QDir::toNativeSeparators(file)));
    }

    // Its caches (compiled kernels) go to a folder of the user's that it may write.
    const QString userDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("cycles"));
    QDir().mkpath(userDir);
    const auto create = reinterpret_cast<renderapi::CreateEngineFn>(entry);
    const QByteArray resourceDir = QDir::toNativeSeparators(QFileInfo(file).absolutePath()).toUtf8();
    const QByteArray userDirUtf8 = QDir::toNativeSeparators(userDir).toUtf8();
    renderapi::Engine* engine = create(POSESTUDIO_RENDER_API_VERSION, resourceDir.constData(), userDirUtf8.constData());
    if (engine == nullptr) {
        return fail(QCoreApplication::translate(
            "RenderEngineLibrary", "The render engine was built for another version of PoseStudio and cannot be used."));
    }
    std::unique_ptr<RenderEngineLibrary> result(new RenderEngineLibrary());
    result->m_engine = engine;
    result->m_create = create;
    result->m_resourceDir = resourceDir;
    result->m_userDir = userDirUtf8;
    result->m_directory = QFileInfo(file).absolutePath();
    return result;
}

std::unique_ptr<RenderEngineInstance> RenderEngineLibrary::createEngine() const {
    renderapi::Engine* engine =
        m_create != nullptr ? m_create(POSESTUDIO_RENDER_API_VERSION, m_resourceDir.constData(), m_userDir.constData()) : nullptr;
    return engine != nullptr ? std::make_unique<RenderEngineInstance>(engine) : nullptr;
}

RenderEngineLibrary::~RenderEngineLibrary() {
    if (m_engine != nullptr) {
        m_engine->destroy();
    }
}

} // namespace pose
