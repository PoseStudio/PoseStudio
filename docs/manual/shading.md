# Shading and Display

The **shading picker** in the viewport strip (its tooltip reads *Viewport shading mode*) chooses how the scene is drawn. Switching is instant and never affects the pose or the lighting settings — it only changes what you see. The picker's nineteen modes fall into four groups.

## Lit shading of the real materials

| Mode | What you see |
| --- | --- |
| **PBR Shaded** (the default) | The photoreal mode: physically based materials lit by the HDR environment and the key light, with skin translucency, subsurface scattering, soft shadows, bloom and filmic tone mapping. It shows the HDRI backdrop, responds to every dial on the Environment tab and is lit by your [lights](lights.md). |
| **Rendered** | The scene **path traced in the viewport**, by the render engine that makes final renders: real shadows from every light, bounced light, skin, glass. It follows the camera, the lighting and the scene as you work. See [The Rendered mode](#the-rendered-mode) below. |
| **Texture Shaded** | The textured materials under a simple three-point studio rig. Fast, even and forgiving. |
| **Flat Texture Shaded** | The same, with per-face shading — the surface's facets show. |
| **Cartoon Shaded** | Stepped, cel-style shading with the textures. |

## The Rendered mode

**Rendered** is the picture a [render](rendering.md) would make, shown in the viewport and kept up to date. Where the PBR Shaded mode draws the scene with a game engine's shortcuts, this mode traces light: every light casts its shadow — the key light, the environment's own lights, each [light](lights.md) you placed, soft or hard by the size of its source — surfaces light each other, skin lets light through, and glass and the eyes' clear surfaces bend what is behind them.

Choose **Rendered** in the shading picker. The PBR picture stays for a moment while the scene is handed to the renderer, then the traced picture takes its place: coarse and soft at first, sharper and cleaner with every pass. A line under the control strip says where it is — *Rendered: 37 / 128 samples*, then just *Rendered* when the picture is finished.

It follows what you do:

- **The camera.** Orbit, pan or zoom and the picture starts again from the new view — coarse for an instant, then sharpening.
- **The lighting.** Turn a dial on the Environment tab, choose another panorama, add a light or drag one around: the scene is relit where it stands. This is the quick way to place lights — move a lamp and watch its shadow move.
- **The scene.** While you pose — during a drag, while a dial is being turned, while a material value is being scrubbed — the viewport shows the **PBR Shaded** picture, so posing is as quick as ever and the joint you hold is tinted as usual. A moment after you let go, the traced picture of the new pose comes back. Importing or deleting a model does the same.

The selection outline, the light markers, the skeleton overlay and the pins are drawn over the traced picture as over any other. The floor grid and the blue tint of the selected joint are not: the floor shows what a render shows — the shadows that fall on it.

### What it needs

- **The render engine.** The mode is drawn by the same plugin that renders images. A copy of PoseStudio without it says so and stays in PBR Shaded (see [If this copy has no render engine](rendering.md#if-this-copy-has-no-render-engine)).
- **A graphics card, to be fluid.** The mode uses the **Device** chosen in the [Render window](rendering.md#the-device) — the fastest one found, until you choose. On a recent graphics card the picture follows the camera almost as you move; on the processor it works, and each change takes seconds to clear up. The first time a device is used in a session its programs are loaded, which takes a few seconds.
- **Memory.** The renderer keeps its own copy of the scene on the device, beside the viewport's.

### Where it steps aside

- While a **final render** is running in the Render window, the viewport's picture pauses — the line reads *Rendered: paused while an image renders* — and the PBR picture shows. It resumes when the render ends.
- In **VR Mode** the headset shows the PBR Shaded picture.
- **Gaussian splat scenes** are not path traced; the viewport lays them over the traced picture as it lays them over the PBR one, in front of the figure and behind her. A render also drops her shadow onto the capture's floor (see [Gaussian splat scenes in a render](rendering.md#gaussian-splat-scenes-in-a-render)).

Everything else on that list applies here too: the Rendered mode and a render are the same picture, so what a render leaves out of the viewport's dials — the shadow's Intensity and Reach, the Specular dial, the rim light, the backdrop's blur — this mode leaves out as well.

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
