// color.glsl — the tonemapping curve.
//
// Included by mesh.frag (the matcap mode tonemaps inline) and composite.frag (the PBR chain's
// final ACES step). The stylized modes author display-ready values and never tonemap. (The
// selection highlight's color is a preference and rides the camera block: camera_ubo.glsl.)

// ACES filmic tonemap. Applied in composite.frag for the PBR family (which renders linear HDR into
// the offscreen target, so bloom thresholds/blurs see real radiance) and inline by the matcap
// mode, whose procedural material sphere exceeds 1.0 and writes straight to a display-ready
// value. The sRGB swapchain then does its own gamma encode.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

