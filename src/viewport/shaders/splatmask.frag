#version 450

// GAUSSIAN SPLATS — the SELECTION OUTLINE's coverage: splat.vert's quads again, into the
// OutlineMask (outlinemask.h) when the selected model is a splat scene, so a capture wears the
// blue outline like any model. The mask accumulates the splats' OPACITY as the photo pass does
// (the "over" blend: the mask ends as the capture's total opacity per pixel, 1 through its body
// and falling off across its fuzzy edge), and the composite outlines that opacity's 50% contour
// — ONE line around the outermost extent, like a mesh's silhouette. The first cut stamped each
// splat hard over a threshold instead, and the edge band, full of holes and islands between
// splats, wore an outline around every one of them.
//
// Pass:    the OutlineMask's coverage pass (no depth: the whole projected silhouette; the
//          pipeline blends, straight alpha).
// Inputs:  vLocal, vColor from splat.vert.
// Outputs: location 0 -> the coverage in R (1, blended by the alpha = the splat's opacity here).

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outCoverage;

void main() {
    const float r2 = dot(vLocal, vLocal);
    if (r2 > 9.0) {
        discard;
    }
    const float alpha = vColor.a * exp(-0.5 * r2);
    if (alpha < 1.0 / 255.0) {
        discard;
    }
    outCoverage = vec4(1.0, 0.0, 0.0, alpha); // out.r = alpha·1 + dst·(1 − alpha): the opacity accumulates
}
