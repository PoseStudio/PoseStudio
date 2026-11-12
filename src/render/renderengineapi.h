/**
 * @file renderengineapi.h
 * @brief THE RENDER ENGINE INTERFACE: the binary contract between PoseStudio and a production
 *        render engine (Cycles — engines/cycles/) loaded at runtime as a shared library.
 *
 * A render engine is a PLUGIN: `PoseStudio.exe` builds and runs without one (a fresh clone still
 * needs only Qt + the Vulkan SDK), and an engine's own dependencies — Cycles brings OpenImageIO,
 * TBB, Embree, OpenImageDenoise and its GPU kernels — never reach the application's build. The
 * application describes the scene to render as PLAIN DATA (this file), the engine library turns
 * it into its own scene and renders it on its own threads, and the application polls for
 * progress and pixels.
 *
 * THE RULES OF THE BOUNDARY (an engine may be built with another configuration than the
 * application — a Release engine under a Debug application — so nothing with a layout that
 * depends on the build may cross):
 *   - only the types in this file cross: PODs, raw pointers with counts, UTF-8 `const char*`;
 *     no std:: types, no exceptions (an engine catches its own and reports through Status);
 *   - EVERY POINTER IN A Scene IS BORROWED and must stay valid until the render it was started
 *     with has ended — Engine::start() returned false, or cancel() / destroy() returned, or
 *     status() reported a final state — an engine may read pixels lazily from its render
 *     threads (a figure's 4K maps are not copied a third time);
 *   - memory is freed by the side that allocated it (an engine is destroyed with destroy(),
 *     never `delete`).
 *
 * CONVENTIONS (the application's own, so a snapshot is a copy and not a conversion):
 *   - world space is right-handed, +Y up, metres; a figure faces +Z;
 *   - mesh data is in WORLD space, posed as the viewport shows it;
 *   - colors are LINEAR Rec.709 unless said; an Image says its own encoding;
 *   - an image's row 0 is its TOP row and UV (0,0) is its top-left texel (v runs DOWN);
 *   - angles are radians.
 *
 * Bump POSESTUDIO_RENDER_API_VERSION on ANY change of this file's layout: the application
 * refuses an engine built against another version.
 */

#ifndef RENDERENGINEAPI_H
#define RENDERENGINEAPI_H

#include <cstdint>

#define POSESTUDIO_RENDER_API_VERSION 1

/// The one symbol an engine library exports (resolved by name): see CreateEngineFn below.
#define POSESTUDIO_RENDER_ENGINE_ENTRY "posestudio_create_render_engine"

namespace pose {
namespace renderapi {

// ---------------------------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------------------------

enum class PixelFormat : int32_t {
    UInt8 = 0,   ///< 8 bits per channel
    Float32 = 1, ///< 32-bit float per channel
};

enum class ColorSpace : int32_t {
    SRgb = 0,   ///< display-encoded color (a base color, emission or sheen color map)
    Linear = 1, ///< linear color, or non-color data (roughness, a normal map, a mask)
};

/// One image in memory. `id` is its identity: two Image entries with the same id are the same
/// pixels (an engine loads them once).
struct Image {
    uint64_t    id = 0;
    const void* pixels = nullptr; ///< width × height × channels, tightly packed, row 0 = top
    int32_t     width = 0;
    int32_t     height = 0;
    int32_t     channels = 0; ///< 1, 3 or 4
    PixelFormat format = PixelFormat::UInt8;
    ColorSpace  colorSpace = ColorSpace::SRgb;
    const char* name = nullptr; ///< for logs
};

/// A material input's map: an image of the scene and, for a scalar input, the channel its
/// value rides in.
struct Texture {
    int32_t image = -1;   ///< index into Scene::images; -1 = no map
    int32_t channel = -1; ///< 0..3 = that channel as a scalar; -1 = the image's color
};

// ---------------------------------------------------------------------------------------------
// Materials (the canonical material, scene/materialparams.h, as a renderer reads it: the layer
// switches already applied — a layer that is off arrives with a weight of 0)
// ---------------------------------------------------------------------------------------------

enum class MapSlot : int32_t {
    BaseColor = 0,
    Opacity,           ///< the cutout: a mask's channel, or the base color image's alpha
    Roughness,
    Metallic,
    Specular,          ///< multiplies the specular weight
    Normal,            ///< tangent-space normal map
    Bump,              ///< height map
    Emission,
    Subsurface,        ///< the translucency color map: its luminance scales the subsurface weight
    DetailNormal,      ///< the tiled micro-detail normal map
    CoatWeight,
    CoatRoughness,
    SheenWeight,
    SheenColor,
    ThinFilmThickness,
    TransmissionWeight,
    Count
};
constexpr int kMapSlotCount = static_cast<int>(MapSlot::Count);

enum MaterialFlags : uint32_t {
    kMaterialThinWalled = 1u << 0,    ///< a sheet, not a solid (a pane, a leaf, a lash card)
    kMaterialShadowCatcher = 1u << 1, ///< shows only the shadows cast on it (the floor)
};

struct Material {
    const char* name = nullptr;
    uint32_t    flags = 0;

    // Base
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float metallic = 0.0f;
    float diffuseRoughness = 0.0f;
    // Specular
    float specular = 1.0f; ///< 1 = the dielectric's own reflectance at `ior`
    float specularColor[3] = {1.0f, 1.0f, 1.0f};
    float roughness = 0.5f;
    float ior = 1.5f;
    float anisotropy = 0.0f;
    float anisotropyRotation = 0.0f;
    // Transmission
    float transmission = 0.0f;
    float transmissionColor[3] = {1.0f, 1.0f, 1.0f};
    /// The depth the tint is reached at, metres. 0 = the tint is the surface's; given a depth it
    /// is the volume's (a solid absorbs toward it; a thin wall, having no depth, is clear).
    float transmissionDepth = 0.0f;
    /// How rough the surface is to what passes through it; negative = `roughness`.
    float transmissionRoughness = -1.0f;
    // Subsurface
    float subsurface = 0.0f;                           ///< weight
    /// How far light diffuses under the surface before it comes back out, per channel, metres.
    float subsurfaceRadius[3] = {0.005f, 0.001f, 0.0005f};
    float subsurfaceAnisotropy = 0.0f;
    // Sheen
    float sheen = 0.0f;
    float sheenColor[3] = {1.0f, 1.0f, 1.0f};
    float sheenRoughness = 0.5f;
    // Coat
    float coat = 0.0f;
    float coatRoughness = 0.03f;
    float coatIor = 1.5f;
    float coatColor[3] = {1.0f, 1.0f, 1.0f};
    // Thin film
    float thinFilmThickness = 0.0f; ///< nanometres; 0 = no film
    float thinFilmIor = 1.33f;
    // Emission
    float emissionColor[3] = {0.0f, 0.0f, 0.0f};
    float emissionStrength = 0.0f;
    // Geometry
    float opacity = 1.0f;
    float normalStrength = 1.0f;
    float bumpStrength = 1.0f;
    float detailWeight = 0.0f;
    float detailTiling[2] = {1.0f, 1.0f};
    float detailOffset[2] = {0.0f, 0.0f};
    // The material's UV transform: scale, then rotate about the UV origin, then translate.
    float uvTiling[2] = {1.0f, 1.0f};
    float uvOffset[2] = {0.0f, 0.0f};
    float uvRotation = 0.0f;

    Texture maps[kMapSlotCount];
};

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

/// One triangle mesh with one material — a model's material zone, posed, in world space.
struct Mesh {
    const char*     name = nullptr;
    const float*    positions = nullptr; ///< xyz per vertex
    const float*    normals = nullptr;   ///< xyz per vertex, unit (the smooth shading normals)
    const float*    uvs = nullptr;       ///< uv per vertex (v down), or null
    const float*    tangents = nullptr;  ///< xyzw per vertex: the UV tangent + bitangent sign, or null
    uint32_t        vertexCount = 0;
    const uint32_t* indices = nullptr;   ///< three per triangle
    uint32_t        indexCount = 0;
    int32_t         material = -1;       ///< index into Scene::materials
};

// ---------------------------------------------------------------------------------------------
// Camera, lights, environment
// ---------------------------------------------------------------------------------------------

struct Camera {
    /// Camera -> world, column-major (m[12..14] the eye). The camera looks down its -Z with +Y up.
    float cameraToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float fovY = 0.8726646f;  ///< vertical field of view (perspective)
    float orthoHeight = 0.0f; ///< > 0: ORTHOGRAPHIC, the view volume's full height in metres
    float nearClip = 0.05f;
    float farClip = 1000.0f;
};

/// A distant light (the viewport's key light).
struct SunLight {
    float direction[3] = {0.0f, 1.0f, 0.0f}; ///< unit vector TO the light
    float color[3] = {1.0f, 1.0f, 1.0f};
    float strength = 1.0f; ///< irradiance on a surface facing it, in the scene's radiance units
    float angle = 0.01f;   ///< angular diameter: the shadow's softness
};

/// The lighting environment and what the camera sees behind the scene.
struct Environment {
    int32_t     image = -1;      ///< an equirectangular Image of the scene (row 0 = the +Y pole) …
    const char* file = nullptr;  ///< … or a panorama file the engine reads (.hdr / .exr); neither = `color`
    float       color[3] = {0.5f, 0.5f, 0.5f};
    float       rotation = 0.0f; ///< about +Y
    /// The environment's radiance scale. (ONE scale: a path tracer cannot light diffuse and
    /// glossy bounces by different amounts and stay consistent — the viewport's Specular dial
    /// has no counterpart here.)
    float strength = 1.0f;
    float ambient = 0.0f; ///< a uniform radiance added to the lighting (the Ambient Fill dial)
    /// What camera rays see: 0 = the flat `backdropColor`, 1 = the environment × backdropStrength.
    int32_t backdrop = 0;
    float   backdropColor[3] = {0.0f, 0.0f, 0.0f};
    float   backdropStrength = 1.0f;
};

struct Scene {
    const Image*    images = nullptr;
    uint32_t        imageCount = 0;
    const Material* materials = nullptr;
    uint32_t        materialCount = 0;
    const Mesh*     meshes = nullptr;
    uint32_t        meshCount = 0;
    const SunLight* suns = nullptr;
    uint32_t        sunCount = 0;
    Camera          camera;
    Environment     environment;
    /// A shadow-catching floor at y = floorHeight (the viewport's grid plane): invisible but for
    /// the shadows on it.
    int32_t floorShadows = 0;
    float   floorHeight = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// The render
// ---------------------------------------------------------------------------------------------

enum class DeviceType : int32_t { Cpu = 0, Cuda, Optix, Hip, OneApi, Metal };

struct DeviceInfo {
    char       id[192] = {0};   ///< what Settings::device names
    char       name[256] = {0}; ///< for the user ("NVIDIA GeForce RTX 4090")
    DeviceType type = DeviceType::Cpu;
};

struct Settings {
    int32_t     width = 1280;
    int32_t     height = 720;
    int32_t     samples = 256;
    const char* device = nullptr; ///< a DeviceInfo::id; null or "" = the CPU
    int32_t     denoise = 1;
    int32_t     maxBounces = 12;
    float       exposure = 1.0f; ///< a multiplier on the image (the viewport's Exposure dial)
    int32_t     transparentBackground = 0; ///< the backdrop left out: alpha = coverage
    int32_t     threads = 0; ///< CPU threads; 0 = all
};

enum class State : int32_t { Idle = 0, Starting, Rendering, Finished, Cancelled, Failed };

struct Status {
    State    state = State::Idle;
    float    progress = 0.0f; ///< 0..1
    int32_t  sample = 0;      ///< samples rendered so far
    int32_t  samples = 0;     ///< of
    double   seconds = 0.0;   ///< render time so far
    uint64_t imageVersion = 0; ///< bumped whenever copyImage() would return other pixels
    char     text[256] = {0};  ///< what the engine is doing ("Loading images", "Sample 12/256")
    char     error[512] = {0}; ///< State::Failed: why
};

/// A render engine instance. One render at a time: start() begins it on the engine's own
/// threads and returns; poll status() and copyImage() from the calling thread.
class Engine {
public:
    /// The engine's name and version, for the interface ("Cycles", "5.3.0").
    virtual const char* name() const = 0;
    virtual const char* version() const = 0;

    /// The devices it can render on, the CPU first.
    virtual int32_t deviceCount() = 0;
    virtual bool    device(int32_t index, DeviceInfo& out) = 0;

    /// Begins rendering @p scene. False (with Status::error set) if it could not start. See the
    /// file comment for how long the scene's pointers must live.
    virtual bool start(const Scene& scene, const Settings& settings) = 0;
    /// Stops the render and waits until the engine no longer touches the scene's memory.
    virtual void cancel() = 0;
    virtual void status(Status& out) = 0;
    /// Copies the image as it stands — linear RGBA float, @p width × @p height as started, row 0
    /// = top, the exposure applied, alpha = coverage — into @p rgba. False before the first
    /// pixels exist or if the size is not the render's.
    virtual bool copyImage(float* rgba, int32_t width, int32_t height) = 0;

    /// Ends any render and frees the engine.
    virtual void destroy() = 0;

protected:
    ~Engine() = default;
};

/// The exported entry (POSESTUDIO_RENDER_ENGINE_ENTRY, C linkage). @p apiVersion is the
/// application's POSESTUDIO_RENDER_API_VERSION — an engine built against another returns null.
/// @p resourceDir is the folder the engine's own files (kernels, shaders) were installed to,
/// @p userDir a writable folder for its caches. Null on failure.
using CreateEngineFn = Engine* (*)(int32_t apiVersion, const char* resourceDir, const char* userDir);

} // namespace renderapi
} // namespace pose

#endif // RENDERENGINEAPI_H
