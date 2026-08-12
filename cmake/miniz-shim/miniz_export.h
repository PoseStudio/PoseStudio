/*
 * Build shim for miniz.
 *
 * miniz.h #includes "miniz_export.h", which miniz's own CMake generates (via generate_export_header)
 * to carry dllexport/import decorations. We deliberately skip miniz's CMake and compile its C sources
 * straight into the PoseStudio target (see CMakeLists.txt), so that generated header never exists.
 * Because miniz lives in the same binary and is linked statically, the export decorations are no-ops
 * — so we provide this stub (on the include path) to satisfy the include with empty macros.
 */

#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#endif /* MINIZ_EXPORT_H */
