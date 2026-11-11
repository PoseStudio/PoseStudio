#version 450

// GAUSSIAN SPLATS — the fragment stage: the Gaussian's falloff off the corner's σ-unit
// coordinates (the quad spans the splat's extent along the 2D covariance's eigenvectors — 3σ,
// or where its own Gaussian falls under 1/255; past 3σ the footprint is cut round), times the
// splat's opacity, blended far-first into the PHOTO LAYER — the photo pass's one attachment
// (hdrtarget.h), where the splats accumulate display-referred, in the trainers' own color
// space, as premultiplied color + coverage (the pipeline's "over" blend); the composite lays
// the layer over the tonemapped scene. Nothing of the scene pass is touched: a figure inside a
// capture is shaded as ever underneath and covered where the splats cover her.
//
// Pass:    the PHOTO pass (see splat.vert).
// Inputs:  vLocal, vColor from splat.vert (the color as trained, sRGB-encoded 0..1).
// Outputs: location 0 -> the photo layer (color, coverage).

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outPhoto;

void main() {
    float r2 = dot(vLocal, vLocal);
    if (r2 > 9.0) {
        discard;
    }
    float alpha = vColor.a * exp(-0.5 * r2);
    if (alpha < 1.0 / 255.0) {
        discard;
    }
    outPhoto = vec4(vColor.rgb, alpha);
}
