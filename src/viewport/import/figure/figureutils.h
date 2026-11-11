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
 *  - parseChannelRef() returns BOTH the raw and the percent-decoded id, and AN ID IS COMPARED
 *    DECODED: a reference writes it percent-encoded ("#Some%20Character%20Body"), the file that
 *    defines it writes it plain ("id": "Some Character Body"). The morph resolver compared raw
 *    with raw until 2026-11-10 — and looked a modifier up in its file by the raw spelling, so a
 *    control whose id has a space in it was never found, its formulas never walked, and the
 *    character it stands for imported as the base figure (a third of one library's morphs have
 *    such ids). rawKey is for a caller that must hand the reference on as written.
 *
 * Pure std + GLM + nlohmann — no Qt, no Vulkan.
 */

#ifndef FIGUREUTILS_H
#define FIGUREUTILS_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
/// A node whose geometry is a placeholder for whatever is selected when the file is loaded
/// ("name://@selection…": the figure a wearable saved "to be added" lists before itself) is no
/// figure of the file and is passed over.
const nlohmann::json* findFigureNode(const nlohmann::json& root);

/// WHICH FIGURE of a scene something belongs to. A preset's scene can hold several figures — a
/// character, her eyelashes, her hair, a ring, a second character — and it lists every one's
/// modifiers and materials side by side. A modifier names its figure through `parent` (the
/// figure's node, "#Figure", or its geometry instance, "#geometry"), a material through
/// `geometry`. Read without that, a character took the dials of everything saved with her: the
/// eyelashes' "EyeMoisture" material replaced her own eye's, a suit's "Lips" her lips, a second
/// character's whole skin hers, and a morph dialed on a follower was baked into HER cage by vertex
/// index (found 2026-11-10 on user-saved scene subsets, which is how most custom characters are
/// distributed).
struct FigureScope {
    /// False for a scene with ONE figure: everything in it is that figure's, whatever it names.
    bool                     scoped = false;
    std::string              nodeId;      ///< The figure's scene node id.
    std::string              assetId;     ///< Its asset url's fragment, decoded — what a formula's output names the figure by.
    std::string              assetFile;   ///< Its asset url's file part as written ("/data/…/Figure.dsf"): where the figure's own files live.
    std::vector<std::string> geometryIds; ///< Its geometry instances' ids.
    /// The node ids and asset ids of the scene's OTHER figures (never one this figure shares).
    std::vector<std::string> otherFigures;

    /// True when @p reference — a modifier's `parent` or a material's `geometry`, written "#id"
    /// percent-encoded — names this figure. An empty reference names nobody in particular and is
    /// everyone's; so is every reference of a scene with one figure.
    bool owns(const std::string& reference) const;

    /// True when @p name — the "Name:" a channel reference is prefixed with, decoded — is one of
    /// the scene's other figures: a control saved on a character can drive a morph of her clothes
    /// ("Suit:/…/Suit.dsf#Expand?value"), which is the clothes' shape and none of hers.
    bool namesAnotherFigure(const std::string& name) const;
};

/// The scope of @p figureNode within @p root's scene (the first figure when @p figureNode is
/// null); an unscoped, empty scope for a document with no figure node (a bare base file).
FigureScope figureScopeOf(const nlohmann::json& root, const nlohmann::json* figureNode = nullptr);

/// True when @p root is a document the figure importer loads: a base figure file (it carries a
/// geometry_library) or a preset whose scene names a figure's geometry (findFigureNode) — the
/// test loadFigureFile makes before anything else. A pose, material or shape preset is neither.
bool isFigureRoot(const nlohmann::json& root);

/// What a figure file holds, as far as opening it goes.
enum class FigureFileKind {
    NotAFigure, ///< isFigureRoot is false: a pose, material or shape preset, or not a scene file
    Character,  ///< a figure that stands on its own with a skeleton: a character preset, a base
                ///< figure file
    Prop,       ///< a figure that stands on its own with no skeleton to speak of (a wand, a chair)
    Wearable,   ///< a figure fitted onto another one or attached to it (its figure node names a
                ///< conform_target or a parent): clothing, hair, an accessory, a prop on a bone
};

/// isFigureRoot over a file's raw bytes (plain or gzip-compressed), for a caller that has not
/// parsed it — the Asset Manager asking which file in a folder is THE character — and what kind
/// of figure it is. A preset lists its figure's bones as scene nodes (each with a parent in the
/// file); a figure with at least kCharacterMinBones of them is a character. A file that names no geometry at all skips the JSON parse (the
/// textual prefilter), so a folder of pose or material presets costs its reads; anything
/// unreadable is NotAFigure, never an exception.
FigureFileKind figureFileKind(std::vector<std::uint8_t> bytes);

/// The modifier_library entry whose "id" equals @p id, or nullptr. @p id is the id as the file
/// writes it — a reference's DECODED key, never its percent-encoded spelling.
const nlohmann::json* findModifierById(const nlohmann::json& doc, const std::string& id);

/// The modifier whose morph a reference to @p fragment applies: the entry whose "id" equals
/// @p fragment if it carries a "morph" — nullptr if that entry is a control with none — and, only
/// when @p fragment is empty or names no entry of this file, the first entry that has a "morph" at
/// all (nullptr for a formula-only driver document).
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
