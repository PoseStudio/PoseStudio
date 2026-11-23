// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
// fullscreen.glsl — the one oversized triangle that covers the screen (no vertex buffers).
//
// Shared by fullscreen.vert (every post pass) and background.vert (the HDRI backdrop). Index
// gl_VertexIndex (0..2) into it; the triangle's clip-space corners overshoot to (3,-1)/(-1,3) so
// the visible [-1,1] square is fully covered by a single primitive (no diagonal seam, and every
// pixel is shaded exactly once).
const vec2 kTri[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
