# Troubleshooting

## The viewport says Vulkan is unavailable

*"3D viewport unavailable. Could not create a Vulkan instance — check that your GPU supports Vulkan and that your graphics drivers are up to date."*

PoseStudio renders with Vulkan, which needs a graphics driver that provides it. Every current driver from NVIDIA, AMD and Intel does; the usual causes are an old driver, a virtual machine or remote desktop session without GPU access, or a very old GPU. Install the latest driver from your GPU maker's site and start PoseStudio again. The rest of the application (the Asset Manager, Preferences) works meanwhile, and the viewport comes back once Vulkan does.

## The application will not start

- *Missing VCRUNTIME140.dll* or *vulkan-1.dll*: the release packages bundle both next to the executable. If you copied `PoseStudio.exe` somewhere on its own, copy the whole folder.
- A console message *"PoseStudio cannot launch without a valid database connection"* means the settings database could not be opened — usually a permissions problem with `%APPDATA%\PoseStudio`. Check that the folder is writable, or delete it to start fresh (you lose your libraries, collections and preferences; your files are untouched).

## A figure will not import

- **"Content Folder Needed" / "Locate Content Folder…"** — the preset's `data` folder was not found. Point PoseStudio at the folder that directly contains `data`; see [Importing Content](importing.md#when-the-figures-data-cannot-be-found). It is remembered for next time.
- **"This file may be a pose, material, or other preset rather than a figure."** — only character presets import. Pose and material presets in that format are not supported yet.
- **The import takes long** — a fully textured character takes some seconds while its textures decode and its correctives are prepared; the progress dialog names each stage. A debug build (the window title says so) is much slower.

## The figure looks wrong

- **A character comes in with the base figure's shape, or with a plain head on her own body**: one of her morph files is not in the content folders PoseStudio knows — a character built on another needs that one's files as well. The log names each shape that was applied and warns about each that was not, with the file it looked for (see [Seeing what PoseStudio is doing](#seeing-what-posestudio-is-doing)); install the content, or locate its folder ([Importing Content](importing.md#when-the-figures-data-cannot-be-found)), and import again.
- **Her hair, eyelashes or clothes are missing**: they were saved as separate figures in the character's file, and only the character is imported from such a file so far. See [Importing Content](importing.md#a-file-that-holds-several-figures).
- **Skin looks like wet plastic** in the PBR mode: lower **Specular** under Image-Based Lighting, or try a less glossy environment. Most skin materials render as intended; the remaining cases are content whose maps were authored for another renderer.
- **The figure is dark**: raise **Exposure**, or switch the HDRI — some panoramas are much dimmer than others.
- **A black or missing texture**: the texture file the material refers to is not where the preset expects it. PoseStudio skips textures it cannot decode and uses the material's colour.
- **Skin details sit slightly off** — a seam across the scalp, a patch of mismatched skin on the back, the throat's notch out of place: the figure's maps are painted on a UV layout other than the one its materials name. PoseStudio reads each zone through the layout its material names, so this only happens when the content itself names the wrong one.
- **The figure floats or sinks**: press the **Ground** button. Figures import on the floor; loading a pose file, turning joints with `B` / `S` / `T` or the Transform tab, or lifting her with Ctrl + drag can leave her off it.

## Posing does not do what I expect

- **The joint I clicked is not the one I wanted**: the pick is the joint nearest the pointer, and on a thick limb the twist bone in its middle counts as the limb. Turn on **View → Show Skeleton** to see where the joints are, or click a little closer to the joint you mean.
- **Dragging orbits the camera instead of posing**: the press did not land on a joint. Press on the body nearer a joint.
- **The hand stops short of the cursor**: the arm is at full reach and the body has leaned as far as its balance allows. Take the hips along, or let a foot step first (drag the hips or the chest).
- **She will not go lower / will not stand up**: a kneeling body is stopped by its thighs over the planted knees — drag the hips *back* to sit onto the heels. To get up from a kneel or a sit, drag the hips or the chest *up*.
- **A pinned joint holds a drag back**: unpin it (`P`, or *Unpin All Joints* in the context menu). Orange markers show the pins.
- **Her breasts or buttocks do not move**: check the switches in the **Soft Body Physics** tab. The breasts' motion is sized by the bust read off her mesh at import — a flat chest gets none, and a small bust very little; raise **Softness** to see more. A model with no skeleton (a static scan) has no soft body. See [Soft breasts](posing.md#soft-breasts).
- **Delete or P does nothing**: the viewport needs keyboard focus — click in it first.
- **Undo is empty after deleting an object**: deleting clears the whole undo history. File → New clears it too.
- **Opening a project says a figure file is missing**: the project names each figure by the path it was imported from; if the file moved, choose it again in the dialog, or skip it to open the rest. See [Project Files](project-files.md#if-a-figure-file-has-moved).

## Assets do not appear

- **A folder shows no assets**: only files with a same-named image beside them are listed (see [The Asset Manager](asset-manager.md#what-shows-up-as-an-asset)). Add a preview image with the file's name, and Refresh.
- **A library is missing from the tree**: its folder no longer exists or is not reachable (an unplugged drive). Reconnect it and Refresh, or remove it in Preferences → Assets.
- **An HDRI has no thumbnail**: put an image with the panorama's name beside it. `.webp` previews need Qt's optional image formats, which the release packages include.
- **New HDRI files do not show**: they must be `.hdr` or `.exr`, under `Documents\My PoseStudio Library\hdri` (or its subfolders); the menu rescans every time it opens.

## Seeing what PoseStudio is doing

PoseStudio logs what it does to its console output — imports, the ping, the update check, warnings about content it skipped. To see the log on Windows, start it from a terminal with the environment variable `QT_LOGGING_TO_CONSOLE=1` set:

```
set QT_LOGGING_TO_CONSOLE=1
PoseStudio.exe
```

For an imported character the log has a line `[figure] shape: … morphs applied (…)` naming the shapes that made her, and a warning `[figure] shape: NOT applied —` for each shape that could not be applied, with its reason: its file is not under any content folder (the path it was looked for at is given), or it was made for another figure.

## Starting over

**Edit → Preferences → Factory Reset** wipes the settings database and restarts PoseStudio as if freshly installed. Your files — content, HDRIs, poses — are not touched.

## Reporting a problem

Problems and ideas go to the project's [GitHub issues](https://github.com/PoseStudio/PoseStudio/issues). Include the version (Help → About PoseStudio, or the window title), what you did, and what happened; for a posing problem, the `.pose` file and which figure generation it was on is the most useful thing you can attach.
