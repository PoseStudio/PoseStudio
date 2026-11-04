/**
 * @file riggedimport.h
 * @brief What the rigged-character readers share — the FBX (import/fbx/), glTF (import/gltf/) and
 *        USD (import/usd/) readers each turn one file into a ModelData with the skinned meshes,
 *        their materials and textures, and a RAW skeleton (names, hierarchy, bind positions;
 *        identity frames, no limits) that the rig standard's conformer (scene/ik/rigconformer.h)
 *        then gives the frames, rotation orders and limits of each bone's role.
 *
 * The readers differ in how they read a file; what they settle afterwards is the same, and lives
 * here so the three cannot drift: the figure FACING +z with her left at +x (the yaw read off the
 * skeleton and baked into geometry and bones), centred over the origin, standing on the floor
 * (the lowest vertex at y = 0), the bones written out with unique names (finishRiggedModel); the
 * search for a texture file a model refers to (TextureFinder); the import summary every service
 * logs (RiggedImportInfo); the small conversions (sRGB to linear, smooth normals for a mesh
 * without them). Qt-free (std + GLM).
 */

#ifndef RIGGEDIMPORT_H
#define RIGGEDIMPORT_H

#include "modeldata.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

/// What a reader found and did — for the import log. One struct for every format; a field a
/// format has no reading for stays at its default.
struct RiggedImportInfo {
    std::string              sourceAxes;          ///< The file's own axes and unit, as text.
    std::string              skinningMethod;      ///< "linear" / "dual quaternion" / "rigid" / "none".
    float                    heightMetres = 0.0f; ///< The skeleton's (or the mesh's) height after conversion.
    float                    yawDegrees = 0.0f;   ///< The turn applied so the figure faces +z.
    float                    groundedMm = 0.0f;   ///< How far the figure was moved onto the floor.
    int                      bones = 0;
    int                      prunedLeaves = 0;    ///< Skinless leaf bones left out (an exporter's end markers).
    int                      skinnedMeshes = 0;
    int                      rigidMeshes = 0;
    int                      materials = 0;
    int                      embeddedTextures = 0;
    int                      externalTextures = 0;
    int                      missingTextures = 0;
    std::vector<std::string> notes;               ///< Anything worth a line in the log.
};

/// One bone as a reader found it: its name as the file has it (made unique on output), its parent
/// in the list (parents come BEFORE their children), its bind position in the world the reader
/// settled (metres, Y up), and, when the file said so, where its tip lies (an end marker the reader
/// left out of the skeleton — the direction of a bone with no joint below it).
struct RawSkeletonBone {
    std::string name;
    int         parent = -1;
    glm::vec3   position{0.0f};
    glm::vec3   tip{0.0f};
    bool        hasTip = false;
};

/// Finds a texture's file for a model whose textures are files beside it: the paths the file
/// gives (absolute, or relative to the model's folder, or to the folder ABOVE it — exports keep
/// "textures/" beside a "formats/" folder and write the path relative to the project), then a
/// file of that NAME anywhere under the model's folder (deep) or the folder above (shallower).
class TextureFinder {
public:
    explicit TextureFinder(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    /// Tries each of @p candidates in order (empty ones skipped), then their basenames against the
    /// folder index; counts the outcome in @p info and notes the first few misses under
    /// @p shown (the name to log, usually the first non-empty candidate).
    TextureSource resolve(const std::vector<std::string>& candidates, RiggedImportInfo* info, const std::string& shown);

    /// The model's folder.
    const std::filesystem::path& directory() const { return m_dir; }

private:
    TextureSource found(const std::filesystem::path& p, RiggedImportInfo* info);
    void          index();

    std::filesystem::path                                      m_dir;
    bool                                                       m_indexed = false;
    std::unordered_map<std::string, std::filesystem::path>     m_byName;
    std::unordered_set<std::string>                            m_missingNoted;
};

/// True for the file extensions the texture finder's index takes as images.
bool isImageFile(const std::filesystem::path& p);

/// ASCII lower case.
std::string lowerCopy(std::string s);

/// sRGB -> linear (the renderer's base colour is linear; authored colours are often sRGB).
float srgbToLinear(float c);

/// Gives every vertex without a normal (a zero vector) the smooth normal of the triangles around
/// it; authored normals are left alone. A mesh's normals are read from the file when it has them.
void fillMissingNormals(MeshData& mesh);

/// The shared last step of every rigged-character reader. Turns the figure to face +z (the yaw
/// read off the bones: the lateral axis is the skeleton's widest horizontal spread, the toes give
/// the sign), centres it over the hips (the first bone with two children or more; the mesh's
/// centroid for a boneless model), puts its lowest vertex on y = 0, and writes @p model's bones
/// from @p bones — raw: translation-only binds, identity frames, "XYZ", no limits, unique names.
/// Fills @p info's bones, height, yaw and grounding, and notes an unusual height.
void finishRiggedModel(ModelData& model, std::vector<RawSkeletonBone>& bones, RiggedImportInfo* info);

} // namespace pose

#endif // RIGGEDIMPORT_H
