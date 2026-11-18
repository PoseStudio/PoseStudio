# Welcome to PoseStudio

PoseStudio is a free, open-source desktop application for building 3D character poses and scenes. You bring in a rigged character figure, light it with a real photographic environment, and pose it by simply grabbing any part of the body and moving it — the rest of the body follows the way a person's would: the feet stay planted, the weight stays balanced, and the figure steps, crouches, kneels, sits and lies down as you take it there.

This manual describes everything that works in the version you are running. It opens with **Help → User Manual** or the **F1** key, and it is part of the application itself, so it always matches the build you have.

## What PoseStudio does today

- **Imports** static models (`.obj`); fully rigged, morphable character figures from their native scene format (`.duf` presets with `.dsf` data files), complete with materials, textures, skin weights, joint limits and pose correctives; rigged characters and plain models from `.fbx`, glTF (`.glb`, `.gltf`), VRM (`.vrm`) and USD (`.usd`, `.usda`, `.usdc`, `.usdz`) files, which pose like the native figures; and Gaussian splat scenes (`.ply`, `.splat`, `.spz`) — photographed places and people a figure can stand in. See [Importing Content](importing.md).
- **Shows** them in a Vulkan-based viewport with image-based lighting from HDR panoramas, a photoreal PBR mode with skin shading, soft shadows, bloom and anti-aliasing — plus eighteen other shading modes: form studies, wireframes, diagnostic views, and a **Rendered** mode that path traces the scene in the viewport as you work. See [Shading and Display](shading.md).
- **Poses** figures with full-body inverse kinematics: drag any joint and the whole body answers, with planted feet, balance, stepping, joint limits, self-collision, joint pins and a complete undo history — by dragging, with the `B` / `S` / `T` keys, or by number on the Transform tab. Poses save to plain-text `.pose` files, and the whole scene — figures, poses, materials, lighting and camera — to a `.pss` project file.
- **Edits materials** on the Materials tab: every surface is one physically based material, set with sliders, color swatches and texture slots, with overlays for tattoos, marks and makeup, and a library for the materials you save. See [Materials](materials.md).
- **Lights** the scene with an HDR panorama and a key light, and with lights of your own — point, spot, area and sun lights placed in the scene. See [Lighting and Environment](environment.md) and [Lights](lights.md).
- **Renders** finished pictures with a path tracer (**Render → Render Image**, `F12`) — real shadows, bounced light, skin and glass, at sizes up to 4K. See [Rendering](rendering.md).
- **Shows the scene in a VR headset** (**View → VR Mode**): you walk around the figure at her true size and pose her with the controllers. See [VR Mode](vr.md).
- **Organises** your content with the Asset Manager: browse any number of asset folders, search them, and file assets into Favorites and nested Collections.

## What it does not do yet

PoseStudio is an early-stage project. Animation and timelines are on the roadmap but do not exist in this build — every menu entry that is greyed out is a placeholder for something planned. The project's website and release notes say what is coming; see [What's New](changelog.md) for what changed in this version.

## How to use this manual

- The **table of contents** on the left lists every page; expand a page to jump straight to one of its sections.
- **Search** (the field at the top right, or `Ctrl+F`) narrows the contents to the pages that mention your words and highlights them on the page; press `Enter` (or `F3`) to step through the matches.
- **Back** and **Forward** (`Alt+Left`, `Alt+Right`) retrace your steps, like a browser.
- Links between pages are shown in blue. Links to the web open in your browser.
- `Esc` closes the manual; it opens again where you left it.

If you are new, start with [Getting Started](getting-started.md), then [Posing](posing.md). If you know your way around and want to look something up, [Keyboard and Mouse Reference](shortcuts.md) is the quickest page.

## Conventions used here

| When you read… | It means… |
| --- | --- |
| **Drag** | Press and hold the left mouse button while moving the mouse. |
| **Click** | Press and release without moving the mouse. |
| `Ctrl+Z`, `F1`, `1` | Press these keys. Keys named alone (`1`, `P`, `T`) are pressed without modifiers. |
| **File → Import** | Open the File menu, then choose Import. |
| *Numpad* | The numeric keypad. The view keys work on the number row too. |

PoseStudio's world units are metres: a standing figure is about 1.8 units tall, and the floor is the grid at height zero.
