#version 450

// Lit mesh fragment shader: a single directional key light (from the camera UBO) plus an ambient
// fill, modulated by the per-draw base color. Two-sided: the normal is flipped to face the viewer
// so meshes shade correctly regardless of OBJ winding (the pipeline does no back-face culling).

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;
} pc;

// Diffuse texture (set 1). Untextured meshes bind a 1x1 white fallback, so sampling is always
// valid and albedo = baseColor * texture works uniformly. The view is _SRGB, so the sample is
// already linearised here.
layout(set = 1, binding = 0) uniform sampler2D uDiffuse;

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(vWorldNormal);
    if (!gl_FrontFacing) {
        n = -n; // light back faces as if they faced us
    }
    vec3 l = normalize(cam.lightDir.xyz);
    float ndl = max(dot(n, l), 0.0);

    vec3 albedo = pc.baseColor.rgb * texture(uDiffuse, vUv).rgb;
    vec3 lighting = cam.ambient.rgb + cam.lightColor.rgb * ndl;
    outColor = vec4(albedo * lighting, 1.0);
}
