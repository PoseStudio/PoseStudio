# Lighting and Environment

The **Environment** tab on the side panel lights the scene. In the PBR shading mode the figure is lit by an HDR panorama — the environment surrounds it, reflects in it and casts its soft light from every direction — plus one adjustable key light for the direct sun or lamp and its shadow. The simpler shading modes use just the key light and a fixed studio rig.

Every dial is a **scrub field**: drag left or right across it to change the value, or click it once to type a number. Hold **Shift** while you drag for fine control — a tenth of the step, at a tenth of the speed (see [Fine control](interface.md#value-fields-and-fine-control)). Each row has a small **restore** button that returns that one setting to its default, and **Restore All Defaults** at the bottom resets everything. Changes show immediately, and each one is an undo step (`Ctrl+Z`) like a pose edit — except choosing an HDRI, which is not undoable.

## Choosing an HDRI

The **HDRI** button shows the name of the current panorama. Click it for a menu of every `.hdr` and `.exr` file in your library's `hdri` folder, each with a thumbnail. **Subfolders are categories**: put panoramas into `Outdoor`, `Studio`, `Night` and they appear under those headings, files in the root of `hdri` first. The menu is rebuilt each time it opens, so files you add while PoseStudio runs appear at once. **Open HDRI Folder…** at the bottom of the menu opens the folder in Explorer.

A thumbnail is any image with the same file name as the panorama in the same folder (`beach.hdr` + `beach.jpg`); PoseStudio prefers webp, then png, jpg, jpeg, bmp, gif and tif. The installer's starter set comes with previews.

Switching panoramas recomputes the lighting in the background; the viewport keeps showing the previous environment for a moment, then the new one lands. When it does, the **key light turns to face the environment's sun** — its azimuth and elevation are set to where the brightest light in the panorama comes from — so the shadow and the highlights agree with the backdrop. Overcast panoramas leave the key light where it was. You can always re-aim it by hand afterwards.

With no panoramas at all, PoseStudio lights the scene with a built-in **Procedural Studio** environment.

## Environment

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **HDRI** | | the starter studio panorama | The panorama that lights the scene (see above). |
| **Rotation°** | 0 – 360 | 0 | Turns the environment about the figure. In the PBR mode the key light turns with it, so the sun stays where the backdrop shows it. |

## Backdrop

What is drawn *behind* the figure in the PBR mode. Lighting is unaffected by the backdrop mode.

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Mode** | Off / Environment / Dome | Off | *Off* keeps the plain viewport grey. *Environment* shows the panorama all around, as an infinite sphere. *Dome* projects the panorama's ground onto the floor, so the figure stands on the environment's floor and its shadow falls on it. |
| **Blur** | 0 – 4 | 1 | Defocuses the backdrop, in steps; 0 is sharp. |
| **Brightness** | 0 – 2 | 1 | The backdrop's own exposure — turn the background a stop down without darkening the figure. |
| **Dome radius** | 2 – 100 | 12 | The size of the dome in metres. Smaller domes bring the floor's features closer. |

## Exposure and tone

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Exposure** | 0 – 3 | 0.65 | Overall brightness of the lit picture. |
| **ACES filmic tonemap** | on / off | on | The filmic curve that keeps bright highlights from clipping. Off, colors are simply clamped. |

## Image-based lighting

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Diffuse** | 0 – 3 | 0.5 | How much the environment's soft light fills the figure. |
| **Specular** | 0 – 2 | 1 | How strongly the environment reflects in the surfaces. |
| **Ambient fill** | 0 – 1 | 0.15 | A flat lift of the shadows. |

## Key light

The one direct light — the sun, a window, a lamp. Its diffuse light wraps around skin the way light does, driven by the **Subsurface** dial below.

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Intensity** | 0 – 5 | 1.25 | Its brightness. |
| **Azimuth°** | −180 – 180 | 45 | Where it comes from around the figure: 0 is straight from the camera's home side; turn it either way to walk the light around. |
| **Elevation°** | 0 – 85 | 20 | Its height above the horizon. |

Both angles are re-aimed automatically when you switch HDRIs (see above); the restore buttons return them to the values in the table.

## Shadow

The key light's shadow, both on the figure and on the floor. The floor shadow is a soft area-light shadow: crisp where the figure touches the ground and widening as it trails away.

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Enable shadows** | on / off | on | Off skips the shadow entirely. |
| **Intensity** | 0 – 1 | 0.75 | How dark the shadow is. |
| **Softness** | 0 – 1 | 0.25 | How quickly it blurs with distance from the contact point. |
| **Reach** | 1 – 25 | 4 | The distance in metres over which the floor shadow fades out. |

## Skin and rim

| Setting | Range | Default | What it does |
| --- | --- | --- | --- |
| **Subsurface** | 0 – 1 | 0.5 | How far the key light wraps past the terminator on skin — the cheap translucency that makes skin read as flesh. 0 is an opaque surface. |
| **Rim light** | 0 – 1.5 | 0.25 | A photographic rim light from behind, separating the figure from the background. 0 is off. |

## Tips

- For a quick portrait: pick a studio panorama, set the backdrop to *Off* or a blurred *Environment*, and bring the key light's elevation up to 30–40°.
- For an outdoor scene: choose an outdoor panorama with a visible sun, set the backdrop to *Dome* so she stands on its ground, and let the auto-aimed key light cast the shadow.
- The lighting dials are on the same undo history as poses, so `Ctrl+Z` after a lighting change undoes the lighting change, not your last pose edit.
