# Shading and Display

The **shading picker** in the viewport strip (its tooltip reads *Viewport shading mode*) chooses how the scene is drawn. Switching is instant and never affects the pose or the lighting settings — it only changes what you see. The picker's eighteen modes fall into four groups.

## Lit shading of the real materials

| Mode | What you see |
| --- | --- |
| **PBR Shaded** (the default) | The photoreal mode: physically based materials lit by the HDR environment and the key light, with skin translucency, subsurface scattering, soft shadows, bloom and filmic tone mapping. The only mode that shows the HDRI backdrop and responds to every dial on the Environment tab. |
| **Texture Shaded** | The textured materials under a simple three-point studio rig. Fast, even and forgiving. |
| **Flat Texture Shaded** | The same, with per-face shading — the surface's facets show. |
| **Cartoon Shaded** | Stepped, cel-style shading with the textures. |

## Untextured form studies

| Mode | What you see |
| --- | --- |
| **Matcap** | A neutral studio material sphere applied to every surface — a sculptor's view of form, independent of the lights. |
| **Clay Shaded** | A matte, evenly lit clay. |
| **Lighting Only** | A white material under the lights, with highlights — for judging the lighting on its own. |
| **Silhouette** | One flat fill: the pose's outline and nothing else. |

## Wireframes

| Mode | What you see |
| --- | --- |
| **Wireframe** | Every edge, with no surface. |
| **Hidden Line Wireframe** | The edges that face you, with the hidden ones removed — the surface occludes but is not drawn. |
| **Clay Shaded Wireframe** | The clay surface with its edges drawn over it. |
| **Texture Shaded Wireframe** | The textured surface with its edges drawn over it. |

Wireframe modes need a graphics driver that supports line rendering (all desktop drivers do); if yours does not, the wire modes show the surface alone.

## Data views

These show one component of the material or the lighting, for checking content rather than for looking at it:

| Mode | What you see |
| --- | --- |
| **Albedo** | The diffuse color maps, unlit. |
| **Ambient Occlusion** | The baked ambient occlusion as grey. |
| **Roughness Map** | The materials' roughness as grey: black is glossy, white is matte. |
| **Specular Only** | Only the specular reflections of the PBR mode. |
| **Normals** | Surface normals as color. |
| **UV Checker** | A checker pattern mapped through the models' texture coordinates. |

## The selection outline

The selected model wears a blue outline. It is drawn around the object's whole silhouette, so it stays visible through anything in front of it. Selecting is described in [Navigating the Viewport](navigation.md#selecting-objects).

## The skeleton overlay

**View → Show Skeleton** and the strip's **Skeleton** button draw the figure's bones as lines from each joint to its parent. It is off by default because you do not need it: joints are grabbed directly on the body, and the part of the body a selected joint drives is tinted blue so you can see what you have hold of. Turn the overlay on when you want to see exactly where the joints are — for example to find a twist bone in the middle of a limb.

## Anti-aliasing and the picture

The viewport renders with multisample anti-aliasing (4× where the graphics card supports it) and in high dynamic range; the PBR mode's picture goes through subsurface scattering for skin, bloom for the brightest highlights and the ACES filmic tone mapper before it reaches the screen. The other modes skip that chain and show their colors directly. Rendering happens only when something changes, so an idle PoseStudio uses no graphics power.
