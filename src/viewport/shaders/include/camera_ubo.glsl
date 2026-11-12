// camera_ubo.glsl — the per-frame camera/lighting uniform block (descriptor set 0, binding 0)
// and the environment-rotation helper that goes with its params2.z dial.
//
// ONE declaration for every stage that binds the block. It mirrors Scene's CameraUbo (cameraubo.h,
// 1072 bytes std140) member for member; a stage that needs only the view-projection still declares
// the whole block through this include — a partial-but-misordered subset (e.g. skipping `view`)
// would put later fields at the wrong std140 offsets, and a full declaration costs nothing.
//
// Included by: mesh.vert/frag, background.vert/frag, skeleton.vert. Needs GL_GOOGLE_include_directive.

// One LIGHT OBJECT of the scene (scenelight.h; cameraubo.h's LightUbo).
struct SceneLightUbo {
    vec4 position;  // xyz = where it is (world); w = its kind (0 point, 1 spot, 2 area, 3 sun)
    vec4 direction; // xyz = the way it shines (unit); w = the least squared distance it is evaluated at
    vec4 color;     // rgb = color × intensity (the irradiance a metre away); w = a spot's cos(half cone)
    vec4 params;    // x = a spot's edge: 1 / ((1 - cos half cone) × blend)
};
const int kSceneLights = 8;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 view;
    mat4 lightViewProj; // key light's ortho view-projection (shadow-map space)
    vec4 cameraPos;
    vec4 lightDir;    // key light (direction TO the light)
    vec4 lightColor;
    vec4 fillDir;     // fill light (softer, opposite the key)
    vec4 fillColor;
    vec4 rimDir;      // rim / back light (behind the subject, pops the silhouette)
    vec4 rimColor;
    vec4 ambient;
    vec4 params;  // x = shade mode, y = exposure, z = specularIntensity, w = ambientFill
    vec4 sh[9];   // environment diffuse irradiance: 9 SH coefficients (rgb in .xyz)
    vec4 params2; // x = diffuseIntensity, y = keyIntensity, z = envRotation(rad),
                  // w = the joint highlight color's luminance (what the tint divides by)
    vec4 params3; // x = subsurface, y = rimIntensity, z = backdrop mode (1 = environment,
                  // 2 = ground-projected dome), w = backdrop blur (prefiltered mips)
    vec4 params4; // x = backdrop brightness, y = dome radius (world units),
                  // z = shadow intensity (0..1), w = the wireframe overlay's linear grey
    vec4 selection; // the SELECTED-JOINT highlight (Preferences -> Selection): rgb = its color,
                    // LINEAR; a = its opacity at a full skin weight
    vec4 lightInfo; // x = how many of `lights` are in use
    SceneLightUbo lights[kSceneLights]; // the scene's light objects (the PBR mode shades with them)
} cam;

// Rotates a direction about the vertical (Y) axis — used to spin the lighting environment (its SH
// diffuse and prefiltered specular, and the visible backdrop) around the subject without
// re-baking, from the panel's rotation dial (cam.params2.z).
vec3 rotateY(vec3 v, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}
