/**
 * @file figureutils.h
 * @brief Shared JSON / URI / path helpers for the native figure importer (import/figure/).
 *
 * Every parser in this folder walks the same few conventions of the scene format — array blocks
 * written as {"count":N,"values":[...]} or bare, channel objects carrying a dialed `current_value`
 * over a default `value`, channel references of the form "[Scope:][file]#id[?property]", and the
 * geometry/modifier library lookups — and each used to carry a private copy (five valuesArray()s,
 * two percent-decoders, three channel-reference splitters that agreed on real content and differed
 * on corner cases). They live here ONCE so a fix lands everywhere and the parsers read the same
 * way. Two things a newcomer must not "fix":
 *
 *  - The channel readers come in TWO deliberate flavours, documented side by side below: the
 *    "current wins" readers (channelScalar/channelDouble/channelBool/channelColor) for DIALED values
 *    — a material colour, a morph weight — and the "rest wins" reader (restVec3Channels) for BIND
 *    transforms, where a scene's current_value is the posed state and the bind must come from rest.
 *  - parseChannelRef() returns BOTH the raw and the percent-decoded id: the morph resolver compares
 *    raw-vs-raw (both sides come from the same encoded formula text), while the corrective gate
 *    path compares the decoded id against dialed morph ids (which arrive decoded). Each caller
 *    picks the field it always compared on, so consolidation changed no behaviour.
 *
 * Pure std + GLM + nlohmann — no Qt, no Vulkan.
 */

#ifndef FIGUREUTILS_H
#define FIGUREUTILS_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>

namespace pose {

/// A channel reference / channel-driver URL split into its parts. The written form is
/// "[Scope:][file]#id[?property]" — e.g. "lForeArm:/data/…/base.dsf#lForeArm?rotation/y",
/// "figure:#Some%20Character%20Body%201?value", "/data/…/x.dsf#FBMHeavy".
struct ChannelRef {
    std::string scope;      ///< The "Name:" alias prefix, without the colon (empty if none).
    std::string fileUrl;    ///< Scope-stripped "[file]#id" (everything before '?') — what the resolver loads.
    std::string rawKey;     ///< The id after '#' (the whole fileUrl if it has no '#'), URL-encoded as written.
    std::string decodedKey; ///< rawKey percent-decoded ("Some%20Body" -> "Some Body").
    std::string property;   ///< The part after '?' ("value", "rotation/y", "scale/general"), or empty.
};

/// Splits @p ref per the ChannelRef layout. The scope is stripped only when its colon precedes any
/// '/', so a colon inside a path or fragment ("/data/x.dsf#Some:Channel") is never mistaken for a
/// scope separator. Never throws; malformed input just leaves the parts it can't find empty.
ChannelRef parseChannelRef(const std::string& ref);

/// Percent-decodes a URL-encoded string ("My%20Figures" -> "My Figures"). Invalid escapes are kept
/// literally; '+' is left as-is (the figure format encodes spaces as %20, not '+').
std::string urlDecode(const std::string& s);

/// ASCII-lowercases @p s in place and returns it (byte-wise; the format's ids and file names are
/// ASCII, and this must stay locale-independent).
std::string toLowerAscii(std::string s);

/// The parent directory of @p path (std::filesystem semantics; empty for a bare file name).
std::string directoryOf(const std::string& path);

/// The element array of a format array block: {"count":N,"values":[...]} yields its "values", a
/// bare array yields itself. (A few blocks, e.g. polygon_vertex_indices, are written bare.)
const nlohmann::json& valuesArray(const nlohmann::json& node);

/// Nested-object navigation returning nullptr anywhere along a miss (a null @p node, or no @p key).
/// Note it only FINDS: the result may be any JSON type — check is_object()/is_string() before
/// calling value()/get() on it (both throw on the wrong type).
const nlohmann::json* childOf(const nlohmann::json* node, const char* key);

/// The geometry_library entry named @p fragment in @p doc, else the FIRST entry (also when
/// @p fragment is empty); nullptr if the document carries no geometry.
const nlohmann::json* findGeometryEntry(const nlohmann::json& doc, const std::string& fragment);

/// The vertex count of @p doc's first geometry_library entry — its "vertices.count" when written,
/// else the length of its values array; 0 if the document carries no geometry. Cheap: it never
/// reads the vertices themselves (used to test two cages for topology compatibility).
std::size_t geometryVertexCount(const nlohmann::json& doc);

/// Scans a preset's scene.nodes for the FIRST node carrying a geometries[] entry and returns its
/// geometries[0].url ("…dsf#geometry"), or empty. That first geometry-bearing node is taken to be
/// the figure instance — the same criterion morphresolver uses to identify the figure node; a
/// preset with several geometry-bearing nodes (a figure plus props) imports the first listed.
std::string findGeometryUri(const nlohmann::json& root);

/// The scene node findGeometryUri reads the geometry url from — the figure instance — or nullptr.
const nlohmann::json* findFigureNode(const nlohmann::json& root);

/// The modifier_library entry whose "id" equals @p id, or nullptr.
const nlohmann::json* findModifierById(const nlohmann::json& doc, const std::string& id);

/// The modifier carrying a morph: the entry whose "id" equals @p fragment AND has a "morph", else
/// the first entry that has a "morph" at all, else nullptr (a formula-only driver document).
const nlohmann::json* findMorphModifier(const nlohmann::json& doc, const std::string& fragment);

// --- Channel readers ---------------------------------------------------------------------------
// A channel object carries the channel default in "value" and the DIALED state in "current_value".
// For everything a preset dials — material colours and scalars, morph weights, enable flags — the
// dialed value must win: reading "value" alone once rendered every zone with the 0.75-grey channel
// default (textures ~25% too dark) and drew black-dialed pupils as bright dots. These readers try
// "current_value" first, then "value", then the fallback.

/// The channel's scalar as float ("current_value", else "value"), else @p fallback.
float channelScalar(const nlohmann::json& channel, float fallback);

/// The same lookup in double precision — the morph resolver accumulates channel values as doubles
/// through its formula rounds, and a float round-trip would perturb the last bits it propagates.
double channelDouble(const nlohmann::json& channel, double fallback);

/// The channel's boolean ("current_value", else "value"; numbers are tolerated as != 0), else
/// @p fallback.
bool channelBool(const nlohmann::json& channel, bool fallback);

/// Reads a float_color channel's RGB ("current_value", else "value") into @p out; false (and
/// @p out untouched) when the channel carries neither as a 3+ element array.
bool channelColor(const nlohmann::json& channel, glm::vec3& out);

/// Reads an array of three channel_float objects ([{id:x,value:..},{id:y,..},{id:z,..}]) into a
/// vec3 taking each channel's "value" (its REST value) and only falling back to "current_value".
/// This is the deliberate INVERSE of the readers above: these are rest transforms — center/end
/// points, rest orientation — where a scene's dialed current_value is the POSED state, and the
/// bind must come from the rest state. Missing/non-array input yields zeros.
glm::vec3 restVec3Channels(const nlohmann::json& arr);

} // namespace pose

#endif // FIGUREUTILS_H
