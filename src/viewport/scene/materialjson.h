/**
 * @file materialjson.h
 * @brief A material as JSON: the ONE codec behind a project file's zones (projectfile.h) and a
 *        saved material (materialfile.h).
 *
 * A zone's material is written the same way wherever it goes: every value of MaterialParams by
 * name, the texture slots as (slot name, source) pairs, the overlays as objects. Reading is
 * tolerant where it must be (an absent value keeps the struct's default, an unknown slot or
 * blend is left out) and strict where it can be (a value of the wrong type, or a non-finite
 * number, fails the read — the caller rejects its whole file). Qt-free: nlohmann + glm.
 *
 * The json type appears here by forward declaration only, so including this header costs no
 * one the whole JSON library.
 */

#ifndef MATERIALJSON_H
#define MATERIALJSON_H

#include "materialparams.h"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace pose {

/// One texture slot as a file names it: the slot (materialMapName) and what it holds — a file
/// path, or "" for a slot with no map. (What a list of them MEANS is the file's: a project
/// lists the slots that differ from the import, a material file every slot that has a map.)
struct ProjectTexture {
    std::string slot;
    std::string source;
};

/// @p params and @p textures as one JSON object (the values by name, "textures", "overlays").
nlohmann::json materialToJson(const MaterialParams& params, const std::vector<ProjectTexture>& textures);

/// Reads such an object into @p params (over whatever it holds: an absent value keeps what is
/// there) and @p textures (appended). False on a malformed entry.
bool materialFromJson(const nlohmann::json& entry, MaterialParams& params, std::vector<ProjectTexture>& textures);

/// Reads @p j's [x, y, z] into @p out. False (leaving @p out untouched) if it isn't a
/// three-number array or any value is non-finite.
bool readJsonVec3(const nlohmann::json& j, glm::vec3& out);

/// Reads the boolean @p field of @p j into @p out: absent leaves @p out as it is (and is true);
/// present but not a JSON boolean is false.
bool readJsonBool(const nlohmann::json& j, const char* field, bool& out);

} // namespace pose

#endif // MATERIALJSON_H
