# Getting Started

This page takes you from a fresh install to your first posed figure.

## System requirements

- **Windows 10 or 11** (64-bit). Builds for macOS and Linux are planned; the code is cross-platform, but only the Windows build is currently tested and shipped.
- A graphics card and driver with **Vulkan** support. Any GPU from the last several years qualifies — if PoseStudio cannot create a Vulkan instance, the application still starts but the 3D viewport is replaced by a message asking you to update your graphics driver (see [Troubleshooting](troubleshooting.md#the-viewport-says-vulkan-is-unavailable)).
- Enough memory for your content: a fully textured character figure can use several hundred megabytes of graphics memory, as its textures are decoded at up to 4096 × 4096 pixels.

## Installing

Releases come in two forms, both from the project's [GitHub releases page](https://github.com/PoseStudio/PoseStudio/releases):

- **The installer** — a per-user install that needs no administrator rights. It places a starter set of HDR environments in your Documents folder (see below), adds Start Menu entries and an uninstaller, and lets a later version upgrade in place.
- **The portable zip** — unzip anywhere and run `PoseStudio.exe`. Nothing is written outside your user profile.

Both bundle everything the application needs, including the Visual C++ runtime and the Vulkan loader. The installer is not code-signed yet, so Windows SmartScreen may warn on first launch; choose *More info → Run anyway* if you downloaded it from the releases page.

## First launch

On its first start PoseStudio:

- Creates its settings database in your user profile (`AppData\Roaming\PoseStudio` on Windows). Your asset folders, collections, favorites and preferences live there — not next to the executable.
- Creates **My PoseStudio Library** in your Documents folder and registers it as an asset library. This is the home for your own content; its `hdri` subfolder is where PoseStudio looks for HDR environments (the installer puts the starter set there; the portable build starts with none and uses a built-in studio environment until you add some).
- Shows the splash screen. Click anywhere to dismiss it — the same screen is **Help → About PoseStudio**.

A few seconds after every launch the application sends an anonymous "this install exists" ping and checks GitHub for a newer release. Both can be turned off in **Edit → Preferences → General**; see [Privacy and Updates](privacy-updates.md) for exactly what is sent.

## The window at a glance

- The **menu bar** — File, Edit, View and Help.
- The **3D viewport** fills most of the window. In its top-right corner floats a strip of controls: the shading picker, the camera-view picker, and the Home, Ground and Skeleton buttons.
- The **side panel** on the right has seven tabs on its left edge: **Asset Manager** (your content), **Transform** (the selected joint's rotation), **Materials** (the selected object's surfaces), **Materials Library** (your materials on disk), **Soft Body Physics** (how the breasts and the buttocks move), **Environment** (the scene's lighting) and **Lights** (lamps and spotlights of your own). Drag the divider between the viewport and the panel to resize them.

[The Interface](interface.md) describes each part in detail.

## Your first figure

1. **Import a figure.** Use **File → Import → .DUF (DUF File)** and pick a character preset, or add the folder that holds your figures as an asset library and double-click one in the Asset Manager. A progress dialog shows the stages (reading, decoding textures, baking ambient occlusion, uploading); a fully textured figure takes some seconds. The figure lands standing on the floor, facing you, framed by the camera. If PoseStudio asks you to locate a content folder, see [Importing Content](importing.md#when-the-figures-data-cannot-be-found).
2. **Look around.** Drag on empty space to orbit, drag with the middle button to pan, roll the wheel to zoom. Press `1`, `3` or `7` for the front, right and top views, `5` to come back to the default view.
3. **Pose.** Drag any part of the body. Grab a hand and pull it up — the arm raises and the body leans; pull further and she rises on her toes. Grab the hips and pull them down — she crouches. Take the hips back and she steps. Release, and the pose is kept; `Ctrl+Z` undoes it. [Posing](posing.md) explains everything the body does, and [Poses Step by Step](poses.md) shows how to make a kneel, a sit, a lie-down, all fours and more.
4. **Light it.** Open the **Environment** tab, pick an HDRI from the list, and adjust exposure and the key light. The picture updates live.
5. **Save your work.** **File → Save** (`Ctrl+S`) writes the whole scene — the figure, its pose, the lighting and the camera — to a `.pss` project file you can open again with **File → Open**; see [Project Files](project-files.md). **File → Save Pose…** writes just the figure's pose to a `.pose` file you can load onto any figure later; see [Pose Files](pose-files.md).

## Where things live

| What | Where |
| --- | --- |
| Your settings, asset libraries, collections, favorites | `%APPDATA%\PoseStudio\posestudio.db` |
| Your content library (assets you add, HDR environments) | `Documents\My PoseStudio Library` |
| HDR environments | `Documents\My PoseStudio Library\hdri` (subfolders become categories) |
| The built-in sample library ("Maquettes") | Next to the executable |
| Poses and projects you save | Wherever you choose; the last folder of each is remembered |

Nothing you import is copied or modified: PoseStudio reads your files where they are.
