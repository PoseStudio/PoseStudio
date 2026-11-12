# Rendering

**Render → Render Image** (`F12`) makes a finished picture of the scene: the figure as you posed her, the camera where the viewport has it, the materials, the lighting and the environment — traced by **Cycles**, a path tracer, instead of drawn by the viewport. Light now really travels: a hand throws its shadow on a hip, skin lets light through an ear, an eye sits behind its clear cornea, and glass bends what is behind it.

The viewport's **PBR Shaded** mode is the quick preview of this picture: the same framing, the same colors, the same brightness. Its **[Rendered](shading.md#the-rendered-mode)** mode is the exact one — the same renderer, running in the viewport and following the camera, the lights and the scene as you work. Set the picture up there; render it here, at any size and as clean as you like.

## Making a render

1. Set up the picture in the viewport: pose, frame the camera, choose the lighting on the Environment tab and place any [lights](lights.md) of your own.
2. Choose **Render → Render Image**, press `F12`, or click the **Render** button at the left end of the viewport's control strip. The **Render window** opens and the picture appears in it as it renders, grainy at first and cleaner with every pass.
3. When it is done, **Save Image…** (`Ctrl+S`) saves it.

The render is of the scene *as it was when you pressed Render*. The window is separate from the main one: you can go on posing while a render runs, and nothing you change reaches the picture that is being made. Press **Render** again — the button reads **Render Again** while one is running — to start over with the scene as it is now.

**Render → Show Render Window** brings the window, and the last picture, back without starting a render.

## The Render window

| Control | What it does |
| --- | --- |
| **Render** | Renders the scene as the viewport shows it now. |
| **Stop** | Stops the render and keeps the picture so far (`Esc` does the same; with nothing rendering, `Esc` closes the window). |
| **Size** | The picture's size in pixels. **Viewport Size** renders exactly what the viewport frames; the other entries are common sizes — HD to 4K, square, and portrait. |
| **Samples** | How many light paths are traced for each pixel. More is cleaner and takes longer. Drag to change it, `Shift` + drag for single steps, or click and type. |
| **Device** | What does the work: a graphics card, or the processor. |
| **Denoise** | Cleans the finished picture of the grain that is left at this number of samples. |
| **Transparent** | Leaves the background out: the picture is transparent wherever the scene is not. |
| **Save Image…** | Saves the picture (`Ctrl+S`). |

The line under the picture says what the render is doing, then how long it took, at what size and with how many samples. The settings are remembered for the next time.

### Size and framing

The viewport and a render share the camera's **height**: what is at the top and the bottom of the viewport is at the top and the bottom of the picture. A size of another shape shows more to the sides, or less — a portrait size crops the sides of a wide viewport, a wider one adds to them. Frame for a portrait picture by keeping the figure in the middle of the viewport.

### Samples and denoising

With **Denoise** on, 64 to 256 samples are usually enough for a clean picture, and a close-up of skin or eyes is worth the higher end. Without it, grain shows until several hundred. Denoising runs once, at the end: the picture clears up in its last moment.

### The device

A graphics card renders many times faster than the processor. The list shows what the engine found:

- An **NVIDIA** card appears twice. **(OptiX)** uses the card's ray-tracing hardware (RTX cards) and is the fastest; **(CUDA)** works on every supported NVIDIA card. Both need a current NVIDIA driver.
- The **processor** entry always works, on any computer.

PoseStudio starts on the fastest device it finds. If a render on the graphics card fails to start, update the card's driver, or pick the processor.

## What a render takes from the scene

- **The camera**: the viewport's own view, perspective or one of the orthographic views.
- **Every figure and model, as posed**, with the shapes the pose brings — the corrective shapes at bent joints, the Soft Body Physics tab's soft tissue.
- **The materials** as the [Materials](materials.md) tab shows them, with their texture maps and their [overlays](materials.md#overlays-tattoos-marks-and-makeup) — color, roughness, metal, the specular reflection, a top coat, sheen, thin film, glass and its tint, emission, cutouts, normal and bump maps, the texture tiling.
- **Skin** scatters light under its surface, by the zone's Translucency Weight and the Environment tab's **Subsurface** dial.
- **The lighting**: the environment's panorama at its rotation and strength, the ambient fill, and the key light with its direction and intensity. **Exposure** and the **Tonemap** switch apply to the picture as they do to the viewport.
- **Your lights**: every [light](lights.md) that is switched on — point, spot, area and sun — at its place, color and intensity, each casting shadows unless its **Casts shadows in a render** switch is off. The size of a source (a point or spot light's Radius, an area light's Width and Height, a sun's Size) is what makes its shadows soft.
- **The backdrop**: the viewport's grey when the backdrop is off, the panorama when it is on, at the backdrop's brightness.
- **The floor**: with **Shadows** on, the floor shows the shadows that fall on it and nothing else, as in the viewport. The **Softness** dial is the key light's size: crisp shadows at 0, soft ones at 1.

## Where a render differs from the viewport

A path tracer has no shortcuts to turn up or down, so a few of the viewport's dials have nothing to act on:

- The **shade mode** does not matter: a render is always the photoreal picture.
- **Shadows** are the real ones. The key light's shadow is as dark as the rest of the lighting leaves it, and as long as the light is low; the environment's own lights throw shadows too, and so does every light you placed — the PBR Shaded mode draws none for those. The shadow **Intensity** and **Reach** dials do not apply.
- The **Specular** dial does not apply: reflections of the environment are as strong as its light.
- The **Rim** light is not rendered.
- The backdrop is drawn sharp — the **Blur** dial does not apply — and **Dome** mode renders as **Environment**.
- **Glass and the eyes' clear surfaces** refract and reflect for real, where the viewport only tints what is behind them.
- **Displacement** and the **Detail** layer of a material are not rendered yet.
- **Gaussian splat scenes** are not path traced — a capture is a recorded picture, not surfaces a ray can be traced against — but they are *in* the render: see [Gaussian splat scenes in a render](#gaussian-splat-scenes-in-a-render) below.
- The selection outline, the joint highlight, pins, the skeleton, the light markers and the grid are the viewport's own and never appear in a render.

## Gaussian splat scenes in a render

A Gaussian splat scene is a photograph you can walk around: it has colors, but no surfaces for light to bounce off. So a render does with it what film does to put a computer-made character into a filmed place. The capture is drawn as the viewport draws it — at the render's own size, with its colors exactly as they were photographed — and the path-traced scene is laid *into* it:

- **The capture is the picture's plate.** Wherever nothing of your scene is in front of it, it comes out of the render as it looks in the viewport.
- **Your figure stands in it.** What of the capture is behind her is behind her, with her edges, her hair and her lashes blended against the capture itself; what of the capture stands in front of her — a chair, a doorway, a branch — is in front of her.
- **Her shadow falls on the capture's floor.** With **Shadows** on, the shadows she casts on the floor darken the capture there, as light does. For this the capture's floor has to lie on the grid's plane: whatever of the capture lies within about four centimetres of it is treated as the floor. If the shadow is hidden under the capture's ground, or floats above it, move the capture up or down with the Transform tab until its floor meets the grid.

A scene of splats alone renders too: the capture, at any size, as a picture.

What a capture does *not* do in a render, because it is not made of surfaces:

- It **casts no shadow** on your figure or on the floor, and it does not light her: she is lit by the environment and your lights, so choose a panorama and lights that agree with the place.
- It is **not reflected** in her eyes, in glass or in metal, and it does not show through glass: a pane of glass in front of a capture shows the environment behind it instead.
- The **exposure**, the tonemap and the lights leave it alone, as in the viewport.

**Transparent** keeps the capture in the picture with its own coverage. An **OpenEXR** file holds the capture's colors taken back to linear values, so that the file looks the same once the tonemap is applied.

Drawing a capture at a render's size needs room on the graphics card for a moment — about a gigabyte at 4K. If there is not enough, the render goes on without the capture and the status line says so; choose a smaller size, or close other programs that use the card.

The **Rendered** shading mode shows a capture too, laid over the traced picture as the PBR Shaded mode lays it over its own — in front of the figure and behind her. The shadow on the capture's floor, and the soft fringe of hair against the capture, are the render's.

## Saving

**Save Image…** offers three kinds of file:

| File | What is saved |
| --- | --- |
| **PNG** | The picture as shown. Keeps the transparency of a **Transparent** render. |
| **JPEG** | The picture as shown, smaller, without transparency. |
| **OpenEXR** | The renderer's own values: linear, 32-bit, with the exposure applied and *before* the tonemap — the file to take into a compositing or grading program. Keeps transparency. |

## If this copy has no render engine

The render engine is a separate part of PoseStudio: a plugin in the `engines` folder beside the application, loaded the first time you render or choose the **Rendered** shading mode. A copy built from the source code has it only if it was built with the engine turned on (the repository's README says how). Without it, **Render → Render Image** and the Rendered mode tell you so and everything else works as usual.
