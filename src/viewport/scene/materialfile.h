/**
 * @file materialfile.h
 * @brief The `.psm` material file: ONE material — its values and the maps it samples — saved
 *        from a zone of one object, to be put on a zone of any other.
 *
 * The Materials tab's Save button writes one; the Materials Library tab lists them (each with
 * the picture of a ball saved beside it, which is what makes the file an asset there) and a
 * double-click applies one to the zone the Materials tab shows. A small versioned JSON
 * document:
 *
 *     { "format": "posestudio.material", "version": 1, "name": "Gold",
 *       "material": { "baseColor": [...], "roughness": ..., ..., "textures": [ { "slot": "basecolor", "source": "..." } ] } }
 *
 * The "material" object is the project file's own (materialjson.h: one codec for both), with
 * two differences of MEANING, not of form:
 *   - "textures" lists EVERY slot that holds a map — a slot not listed has none — where a
 *     project lists the slots that differ from the import. A source is a file path: absolute,
 *     or relative to the folder the material file lies in (a map kept beside its material
 *     travels with it). The reader hands the sources back as written; resolving them is the
 *     caller's, who knows where the file is.
 *   - NO OVERLAYS. An overlay is painted on one object's UV layout — a tattoo on this figure's
 *     arm — and is that object's, not the surface's: a material file never carries one, and a
 *     read drops any it finds.
 *
 * This file owns the codec only, as TEXT: the caller reads and writes the bytes (the Qt layer
 * does, so a path with characters outside the system code page is no obstacle). Unknown fields
 * are ignored; another format marker, a higher version or a malformed value rejects the text
 * WHOLE with a readable error. Qt-free.
 */

#ifndef MATERIALFILE_H
#define MATERIALFILE_H

#include "materialjson.h"
#include "materialparams.h"

#include <string>
#include <vector>

namespace pose {

struct MaterialDocument {
    std::string                 name;     ///< For people: the file's name as it was saved.
    MaterialParams              params;   ///< The values (its map sources and overlays are not part of a material file).
    std::vector<ProjectTexture> textures; ///< Every slot that holds a map: slot name + file path.
};

/// @p doc as the text of a material file.
std::string materialFileText(const MaterialDocument& doc);

/// Reads the text of a material file into @p doc (left empty on failure). False with a
/// human-readable @p error when the text is not JSON, is another kind of document, is of a
/// version this build does not know, or holds a malformed value.
bool readMaterialFileText(const std::string& text, MaterialDocument& doc, std::string& error);

} // namespace pose

#endif // MATERIALFILE_H
