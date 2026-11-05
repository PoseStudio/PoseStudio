#version 450

// GAUSSIAN SPLATS — the fragment stage: the Gaussian's falloff off the corner's σ-unit
// coordinates (the quad spans ±3σ along the 2D covariance's eigenvectors; past 3σ the footprint
// is cut round), times the splat's opacity, blended over what is behind it in far-first order
// (standard alpha blending; the HDR target's alpha, the SSS mask, is not written — the
// pipeline masks it).
//
// Pass:    the HDR scene pass (see splat.vert).
// Inputs:  vLocal, vColor from splat.vert.
// Outputs: location 0 -> the HDR colour (attachment 1, the specular target, is masked off).

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vLocal, vLocal);
    if (r2 > 9.0) {
        discard;
    }
    float alpha = vColor.a * exp(-0.5 * r2);
    if (alpha < 1.0 / 255.0) {
        discard;
    }
    outColor = vec4(vColor.rgb, alpha);
}
