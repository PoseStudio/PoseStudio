# Rendering

**Render → Render Image** (`F12`) makes a finished picture of the scene: the figure as you posed her, the camera where the viewport has it, the materials, the lighting and the environment — traced by **Cycles**, a path tracer, instead of drawn by the viewport. Light now really travels: a hand throws its shadow on a hip, skin lets light through an ear, an eye sits behind its clear cornea, and glass bends what is behind it.

The viewport's **PBR Shaded** mode is the preview of this picture. A render is that mode's image, done properly: the same framing, the same colors, the same brightness.

## Making a render

1. Set up the picture in the viewport: pose, frame the camera, choose the lighting on the Environment tab.
2. Choose **Render → Render Image**, or press `F12`. The **Render window** opens and the picture appears in it as it renders, grainy at first and cleaner with every pass.
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
- **The backdrop**: the viewport's grey when the backdrop is off, the panorama when it is on, at the backdrop's brightness.
- **The floor**: with **Shadows** on, the floor shows the shadows that fall on it and nothing else, as in the viewport. The **Softness** dial is the key light's size: crisp shadows at 0, soft ones at 1.

## Where a render differs from the viewport

A path tracer has no shortcuts to turn up or down, so a few of the viewport's dials have nothing to act on:

- The **shade mode** does not matter: a render is always the photoreal picture.
- **Shadows** are the real ones. The key light's shadow is as dark as the rest of the lighting leaves it, and as long as the light is low; the environment's own lights throw shadows too. The shadow **Intensity** and **Reach** dials do not apply.
- The **Specular** dial does not apply: reflections of the environment are as strong as its light.
- The **Rim** light is not rendered.
- The backdrop is drawn sharp — the **Blur** dial does not apply — and **Dome** mode renders as **Environment**.
- **Glass and the eyes' clear surfaces** refract and reflect for real, where the viewport only tints what is behind them.
- **Displacement** and the **Detail** layer of a material are not rendered yet.
- **Gaussian splat scenes** are not rendered: they are a recorded picture, not surfaces a ray can be traced against. The window says so when the scene has one, and a scene of splats alone has nothing to render.
- The selection outline, the joint highlight, pins, the skeleton and the grid are the viewport's own and never appear in a render.

## Saving

**Save Image…** offers three kinds of file:

| File | What is saved |
| --- | --- |
| **PNG** | The picture as shown. Keeps the transparency of a **Transparent** render. |
| **JPEG** | The picture as shown, smaller, without transparency. |
| **OpenEXR** | The renderer's own values: linear, 32-bit, with the exposure applied and *before* the tonemap — the file to take into a compositing or grading program. Keeps transparency. |

## If this copy has no render engine

The render engine is a separate part of PoseStudio: a plugin in the `engines` folder beside the application, loaded the first time you render. A copy built from the source code has it only if it was built with the engine turned on (the repository's README says how). Without it, **Render → Render Image** tells you so and everything else works as usual.
