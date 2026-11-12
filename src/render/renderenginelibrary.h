/**
 * @file renderenginelibrary.h
 * @brief Finds and loads a render engine's shared library (renderengineapi.h) and owns the
 *        engine it creates.
 *
 * An engine is installed to its own folder — `<application folder>/engines/cycles` — with
 * everything it needs beside it: the library, the libraries IT depends on, its GPU kernels.
 * The folder is absent in a build made without the engine; then there is simply no engine, and
 * the application says so where a render is asked for. QtCore only.
 */

#ifndef RENDERENGINELIBRARY_H
#define RENDERENGINELIBRARY_H

#include "renderengineapi.h"

#include <QString>

#include <memory>

namespace pose {

class RenderEngineLibrary {
public:
    /// The folder the Cycles engine is looked for in: POSESTUDIO_CYCLES_DIR when set (a
    /// developer's engine build), else `<application folder>/engines/cycles`.
    static QString cyclesDirectory();
    /// Whether an engine library is present in @p directory (nothing is loaded).
    static bool present(const QString& directory);

    /// Loads the engine in @p directory and creates its engine. Null, with @p error saying why
    /// (no library there, a library that will not load, one built for another interface
    /// version), on failure. The library is never unloaded: an engine's worker threads and the
    /// libraries it brought may outlive any point a caller could name.
    static std::unique_ptr<RenderEngineLibrary> load(const QString& directory, QString* error = nullptr);

    ~RenderEngineLibrary();
    RenderEngineLibrary(const RenderEngineLibrary&) = delete;
    RenderEngineLibrary& operator=(const RenderEngineLibrary&) = delete;

    renderapi::Engine* engine() const { return m_engine; }
    const QString&     directory() const { return m_directory; }

private:
    RenderEngineLibrary() = default;

    renderapi::Engine* m_engine = nullptr;
    QString            m_directory;
};

} // namespace pose

#endif // RENDERENGINELIBRARY_H
