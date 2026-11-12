# Welcome to PoseStudio

PoseStudio is a free, open-source desktop application for building 3D character poses and scenes. You bring in a rigged character figure, light it with a real photographic environment, and pose it by simply grabbing any part of the body and moving it — the rest of the body follows the way a person's would: the feet stay planted, the weight stays balanced, and the figure steps, crouches, kneels, sits and lies down as you take it there.

This manual describes everything that works in the version you are running. It opens with **Help → User Manual** or the **F1** key, and it is part of the application itself, so it always matches the build you have.

## What PoseStudio does today

- **Imports** static models (`.obj`) and fully rigged, morphable character figures from their native scene format (`.duf` presets with `.dsf` data files), complete with materials, textures, skin weights, joint limits and pose correctives.
- **Renders** them in a Vulkan-based viewport with image-based lighting from HDR panoramas, a photoreal PBR mode with skin shading, soft shadows, bloom and anti-aliasing — plus seventeen other shading modes for form studies, wireframes and diagnostic views.
- **Poses** figures with full-body inverse kinematics: drag any joint and the whole body answers, with planted feet, balance, stepping, joint limits, self-collision, joint pins and a complete undo history — by dragging, with the `B` / `S` / `T` keys, or by number on the Transform tab. Poses save to plain-text `.pose` files, and the whole scene — figures, poses, lighting and camera — to a `.pss` project file.
- **Organises** your content with the Asset Manager: browse any number of asset folders, search them, and file assets into Favorites and nested Collections.

## What it does not do yet

PoseStudio is an early-stage project. Animation, timelines and the other file formats listed in the File menu are on the roadmap but do not exist in this build — every menu entry that is greyed out is a placeholder for something planned. The project's website and release notes say what is coming; see [What's New](changelog.md) for what changed in this version.

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
| `Ctrl+Z`, `F1`, `1` | Press these keys. Keys named alone (`1`, `P`, `X`) are pressed without modifiers. |
| **File → Import** | Open the File menu, then choose Import. |
| *Numpad* | The numeric keypad. The view keys work on the number row too. |

PoseStudio's world units are metres: a standing figure is about 1.8 units tall, and the floor is the grid at height zero.
